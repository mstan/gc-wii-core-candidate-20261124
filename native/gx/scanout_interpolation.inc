// SPDX-License-Identifier: MIT
// Included in Aurora's gfx namespace after the deferred presentation helpers.
// These inputs are completed scanout images. The guest EFB and its copies are
// never used as scratch targets for an interpolated image.
namespace {
constexpr uint32_t MotionTile = 8;
constexpr uint32_t MaxGenerated = 11; // 30 real frames/sec to 360 presents/sec
constexpr char ScanoutInterpolationShader[] = R"(
@group(0) @binding(0) var prior: texture_2d<f32>;
@group(0) @binding(1) var next: texture_2d<f32>;
@group(0) @binding(2) var flowOut: texture_storage_2d<rgba16float, write>;
@group(0) @binding(3) var flow: texture_2d<f32>;
@group(0) @binding(4) var output: texture_storage_2d_array<rgba8unorm, write>;
@group(0) @binding(5) var cutOut: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(6) var cut: texture_2d<f32>;
@group(0) @binding(7) var sampleFilter: sampler;
@group(0) @binding(8) var<uniform> phases: vec4u;

fn bounded(p: vec2i, dims: vec2i) -> vec2i {
    return clamp(p, vec2i(0), dims - vec2i(1));
}

fn luma(rgb: vec3f) -> f32 {
    return dot(rgb, vec3f(0.2126, 0.7152, 0.0722));
}

fn error_at(p: vec2i, h: vec2i, dims: vec2i) -> f32 {
    var error = 0.0;
    for (var y = -3; y <= 3; y += 3) {
        for (var x = -3; x <= 3; x += 3) {
            let q = p + vec2i(x, y);
            let a = textureLoad(prior, bounded(q - h, dims), 0).rgb;
            let b = textureLoad(next, bounded(q + h, dims), 0).rgb;
            let difference = abs(a - b);
            error += luma(difference) + 0.25 * max(max(difference.r, difference.g), difference.b);
        }
    }
    return error / 9.0;
}

@compute @workgroup_size(8, 8)
fn estimate(@builtin(global_invocation_id) id: vec3u) {
    if (any(id.xy >= textureDimensions(flowOut))) { return; }
    let dims = vec2i(textureDimensions(next));
    let p = bounded(vec2i(id.xy * 8u + 4u), dims);
    var best = vec2i(0);
    let stationary = error_at(p, best, dims);
    var bestScore = stationary;
    // Smooth a transport that moves both sources toward the output pixel.
    // Bias ties toward no motion to keep still elements such as the HUD sharp.
    if (stationary > 0.015) {
        for (var y = -8; y <= 8; y += 4) {
            for (var x = -8; x <= 8; x += 4) {
                let h = vec2i(x, y);
                let score = error_at(p, h, dims) + 0.0025 * f32(abs(x) + abs(y));
                if (score < bestScore) { best = h; bestScore = score; }
            }
        }
        for (var stride = 2; stride >= 1; stride /= 2) {
            let center = best;
            for (var y = -1; y <= 1; y += 1) {
                for (var x = -1; x <= 1; x += 1) {
                    let h = clamp(center + vec2i(x, y) * stride, vec2i(-10), vec2i(10));
                    let score = error_at(p, h, dims) + 0.0025 * f32(abs(h.x) + abs(h.y));
                    if (score < bestScore) { best = h; bestScore = score; }
                }
            }
        }
    }
    textureStore(flowOut, vec2i(id.xy), vec4f(vec2f(best), stationary, bestScore));
}

@compute @workgroup_size(1)
fn scene() {
    let dims = textureDimensions(flow);
    var changed = 0u;
    var unmatched = 0u;
    for (var y = 0u; y < 8u; y += 1u) {
        for (var x = 0u; x < 8u; x += 1u) {
            let p = vec2i((vec2u(x, y) * 2u + 1u) * dims / 16u);
            let m = textureLoad(flow, p, 0);
            changed += select(0u, 1u, m.z > 0.14);
            unmatched += select(0u, 1u, m.w > 0.10);
        }
    }
    let reject = changed >= 36u && unmatched >= 25u;
    textureStore(cutOut, vec2i(0), vec4f(select(0.0, 1.0, reject), 0.0, 0.0, 1.0));
}

@compute @workgroup_size(8, 8)
fn synthesize(@builtin(global_invocation_id) id: vec3u) {
    let dims = vec2i(textureDimensions(output));
    let p = vec2i(id.xy);
    if (any(p >= dims)) { return; }
    let before = textureLoad(prior, p, 0);
    let after = textureLoad(next, p, 0);
    let time = (f32(id.z + phases.y) + 1.0) / (f32(phases.x) + 1.0);
    let nearest = select(before, after, time > 0.5);
    if (textureLoad(cut, vec2i(0), 0).r != 0.0) {
        textureStore(output, p, i32(id.z), nearest);
        return;
    }
    let center = vec2f(p) + 0.5;
    let m = textureSampleLevel(flow, sampleFilter, center / (8.0 * vec2f(textureDimensions(flow))), 0.0);
    let uvScale = 1.0 / vec2f(dims);
    let movedBefore = textureSampleLevel(prior, sampleFilter, (center - 2.0 * time * m.xy) * uvScale, 0.0);
    let movedAfter = textureSampleLevel(next, sampleFilter, (center + 2.0 * (1.0 - time) * m.xy) * uvScale, 0.0);
    let stationary = max(max(abs(before.r - after.r), abs(before.g - after.g)), abs(before.b - after.b));
    let mismatch = luma(abs(movedBefore.rgb - movedAfter.rgb));
    // Fade unreliable motion per pixel. A hard cutoff at the edge of a
    // motion tile makes the synthesized image look blocky on camera turns.
    let confidence = smoothstep(0.018, 0.055, stationary) *
        (1.0 - smoothstep(0.05, 0.13, m.w)) *
        (1.0 - smoothstep(0.055, 0.20, mismatch));
    textureStore(output, p, i32(id.z), mix(nearest, mix(movedBefore, movedAfter, time), confidence));
}
)";

struct ScanoutInterpolation {
  wgpu::ComputePipeline estimate, scene, synthesize;
  wgpu::Sampler linear;
  wgpu::TextureView flow, cut, midpoint;
  wgpu::Texture midpointTexture;
  wgpu::Buffer phaseUniform;
  uint32_t width = 0, height = 0, layers = 0;
  std::array<std::array<webgpu::TextureWithSampler, MaxGenerated+1>, 3> held;
  unsigned heldSet = 0;
  bool hasPrevious = false;
  uint32_t targetFps = 0;
  double generatedCredit = 0.0;
  uint64_t lastNativeNs = 0;
  PresentClock::time_point lastFrame{};
  PresentClock::time_point lastSlot{};

  void prepare(const webgpu::TextureWithSampler& source, uint32_t count) {
    if (!estimate) {
      wgpu::ShaderSourceWGSL wgsl;
      wgsl.code = ScanoutInterpolationShader;
      const wgpu::ShaderModuleDescriptor shader{.nextInChain = &wgsl};
      const auto module = g_device.CreateShaderModule(&shader);
      const auto pipeline = [&module](const char* name) {
        const wgpu::ComputePipelineDescriptor desc{.label = name, .compute = {.module = module, .entryPoint = name}};
        return g_device.CreateComputePipeline(&desc);
      };
      estimate = pipeline("estimate");
      scene = pipeline("scene");
      synthesize = pipeline("synthesize");
      const wgpu::SamplerDescriptor sampler{.magFilter = wgpu::FilterMode::Linear,
          .minFilter = wgpu::FilterMode::Linear};
      linear = g_device.CreateSampler(&sampler);
      const wgpu::BufferDescriptor uniform{.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
          .size = sizeof(uint32_t) * 4};
      phaseUniform = g_device.CreateBuffer(&uniform);
    }
    const auto texture = [](uint32_t w, uint32_t h, uint32_t depth, wgpu::TextureFormat format) {
      const wgpu::TextureDescriptor desc{
          .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::StorageBinding | wgpu::TextureUsage::CopySrc,
          .size = {w, h, depth}, .format = format};
      return g_device.CreateTexture(&desc);
    };
    if (width != source.size.width || height != source.size.height) {
      width = source.size.width;
      height = source.size.height;
      flow = texture((width + MotionTile - 1) / MotionTile, (height + MotionTile - 1) / MotionTile,
                     1, wgpu::TextureFormat::RGBA16Float).CreateView();
      cut = texture(1, 1, 1, wgpu::TextureFormat::RGBA8Unorm).CreateView();
      layers = 0;
    }
    if (layers < count) {
      layers = count;
      midpointTexture = texture(width, height, layers, wgpu::TextureFormat::RGBA8Unorm);
      const wgpu::TextureViewDescriptor arrayView{.dimension = wgpu::TextureViewDimension::e2DArray,
          .arrayLayerCount = layers};
      midpoint = midpointTexture.CreateView(&arrayView);
    }
  }

  void encode(wgpu::CommandEncoder& encoder, const webgpu::TextureWithSampler& prior,
              const webgpu::TextureWithSampler& next,
              const std::array<webgpu::TextureWithSampler, MaxGenerated+1>& targets,
              uint32_t count, uint32_t firstPhase, uint32_t totalPhases) {
    const auto dispatch = [&encoder](const wgpu::ComputePipeline& pipeline,
                                    const auto& entries, uint32_t x, uint32_t y, uint32_t z,
                                    const char* name) {
      const wgpu::BindGroupDescriptor desc{.layout = pipeline.GetBindGroupLayout(0),
          .entryCount = entries.size(), .entries = entries.data()};
      const wgpu::ComputePassDescriptor passDesc{.label = name,
          .timestampWrites = webgpu::gpu_prof::pass_writes(name)};
      auto pass = encoder.BeginComputePass(&passDesc);
      pass.SetPipeline(pipeline);
      pass.SetBindGroup(0, g_device.CreateBindGroup(&desc));
      pass.DispatchWorkgroups(x, y, z);
      pass.End();
    };
    const std::array<uint32_t, 4> uniform{totalPhases,firstPhase,0,0};
    g_queue.WriteBuffer(phaseUniform, 0, uniform.data(), sizeof(uniform));
    const uint32_t groupsX = (width + 7) / 8, groupsY = (height + 7) / 8;
    dispatch(estimate, std::array{wgpu::BindGroupEntry{.binding = 0, .textureView = prior.view},
        wgpu::BindGroupEntry{.binding = 1, .textureView = next.view},
        wgpu::BindGroupEntry{.binding = 2, .textureView = flow}}, (groupsX + 7) / 8, (groupsY + 7) / 8,
        1, "Scanout motion");
    dispatch(scene, std::array{wgpu::BindGroupEntry{.binding = 3, .textureView = flow},
        wgpu::BindGroupEntry{.binding = 5, .textureView = cut}}, 1, 1, 1, "Scanout scene");
    dispatch(synthesize, std::array{wgpu::BindGroupEntry{.binding = 0, .textureView = prior.view},
        wgpu::BindGroupEntry{.binding = 1, .textureView = next.view},
        wgpu::BindGroupEntry{.binding = 3, .textureView = flow},
        wgpu::BindGroupEntry{.binding = 4, .textureView = midpoint},
        wgpu::BindGroupEntry{.binding = 6, .textureView = cut},
        wgpu::BindGroupEntry{.binding = 7, .sampler = linear},
        wgpu::BindGroupEntry{.binding = 8, .buffer = phaseUniform, .size = sizeof(uniform)}},
        groupsX, groupsY, count, "Scanout synthesis");
    const wgpu::Extent3D extent{width, height, 1};
    for (uint32_t i = 0; i < count; ++i) {
      const wgpu::TexelCopyTextureInfo src{.texture = midpointTexture, .origin = {0,0,i}};
      const wgpu::TexelCopyTextureInfo dst{.texture = targets[i].texture};
      encoder.CopyTextureToTexture(&src, &dst, &extent);
    }
  }
} g_scanoutInterpolation;
} // namespace

static void release_machine_interpolation() { g_scanoutInterpolation = {}; }

static void defer_scanout_phase(PresentClock::time_point due, PresentClock::duration spacing,
                                const webgpu::TextureWithSampler* source,
                                const EndFrameCallback& callback, uint64_t frameId, int step,
                                uint32_t aspectWidth, uint32_t aspectHeight) {
  g_deferredPresents.push_back(
      DeferredPresent{.due = due, .source = source, .callback = callback,
                      .gameFrame = frameId, .step = step,
                      .discardAfter = step < 0 ? PresentClock::time_point{} : due + spacing,
                      .scanoutAspectWidth = aspectWidth, .scanoutAspectHeight = aspectHeight,
                      .machineScanout = true});
}

static bool present_interpolated_scanout(wgpu::CommandEncoder& encoder,
                                        const EndFrameCallback& callback, const webgpu::TextureWithSampler& source,
                                        uint64_t frameId, uint32_t targetFps,
                                        uint32_t aspectWidth, uint32_t aspectHeight,
                                        uint64_t nativeNs) {
  auto& interpolation = g_scanoutInterpolation;
  if (!source.texture || source.format != wgpu::TextureFormat::RGBA8Unorm) {
    interpolation.hasPrevious = false;
    return false;
  }
  const auto now = PresentClock::now();
  gcnllr::present_trace::mark(gcnllr::present_trace::Lane::render,
                            gcnllr::present_trace::Event::scanout_ready,frameId,nativeNs);
  const auto interval = now - interpolation.lastFrame;
  interpolation.lastFrame = now;
  const auto nativeInterval = nativeNs > interpolation.lastNativeNs ? nativeNs - interpolation.lastNativeNs : 0;
  const bool nativeCadence = interpolation.lastNativeNs != 0 && nativeInterval > 0 && nativeInterval < 70'000'000;
  interpolation.lastNativeNs = nativeNs;
  const auto spacing = std::chrono::nanoseconds(1'000'000'000 / targetFps);
  const auto& previous = interpolation.held[interpolation.heldSet][MaxGenerated];
  const bool continuous = interpolation.hasPrevious && interval < std::chrono::milliseconds(70) &&
      interpolation.targetFps == targetFps;
  const bool canBlend = continuous && previous.size.width == source.size.width &&
      previous.size.height == source.size.height;
  if (!continuous)
    run_deferred_present();
  const auto sourcePeriod = std::chrono::nanoseconds(std::clamp<int64_t>(g_gameFramePeriodNs, 0, 39'500'000));
  // GameCube VI can publish up to 60 native scanouts per second. Above that
  // rate, retain the established interpolation budget per source interval.
  // The configured target selects the policy for the session.
  const bool oversampled = targetFps > 60;
  auto realDue = now;
  auto firstDue = now;
  uint32_t totalPhases = 0;
  if (oversampled) {
    // Hardware field timing survives render-worker scheduling jitter. Count
    // authored XFB holds as elapsed VI periods when assigning interpolation
    // phases. Keep the measured interval for presenters without VI timing.
    const auto sourceInterval = nativeCadence
        ? std::chrono::nanoseconds(nativeInterval)
        : std::chrono::duration_cast<std::chrono::nanoseconds>(interval);
    interpolation.generatedCredit = continuous ? std::clamp(interpolation.generatedCredit +
        std::chrono::duration<double>(sourceInterval).count() * targetFps - 1.0, -2.0, MaxGenerated + 1.0) : 0.0;
    // Round native fractional credit and carry its signed residue so a stable
    // 60 Hz stream does not lose a phase between two new framebuffers.
    const double rounding = nativeCadence ? 0.5 : 0.0;
    const auto phases = static_cast<uint32_t>(
        std::clamp(interpolation.generatedCredit + rounding, 0.0, static_cast<double>(MaxGenerated)));
    if (canBlend && g_gameFramePeriodNs >= 11'000'000 && g_gameFramePeriodNs <= 39'500'000 &&
        phases != 0) {
      totalPhases = phases;
      interpolation.generatedCredit -= totalPhases;
    }
  } else {
    // The native-rate grid retains its phase through source cadence changes.
    // Look ahead far enough for a slower source to supply the next image.
    const auto lookahead = std::max(spacing * 5 / 4, sourcePeriod - spacing * 3 / 4);
    realDue = now + lookahead;
    firstDue = realDue;
    if (continuous) {
      const auto lastSlot = interpolation.lastSlot;
      const auto slots = (realDue - lastSlot + spacing / 2) / spacing;
      if (slots <= 0 && !g_deferredPresents.empty() &&
          g_deferredPresents.back().due == lastSlot && g_deferredPresents.back().step < 0) {
        realDue = lastSlot;
        g_deferredPresents.pop_back();
      } else {
        const auto advance = std::max<decltype(spacing.count())>(slots, 1);
        realDue = lastSlot + advance * spacing;
        // A geometry change keeps native presents on the existing clock.
        // Interpolated images require equal source dimensions.
        if (canBlend)
          totalPhases = static_cast<uint32_t>(std::min<decltype(advance)>(MaxGenerated, advance - 1));
        firstDue = realDue - totalPhases * spacing;
      }
    }
  }
  // Do not encode phases whose deadline has already passed by a full slot.
  // The surviving phases keep their position between the original sources.
  const auto expired = (now - firstDue) / spacing;
  const uint32_t firstPhase = static_cast<uint32_t>(
      std::clamp<decltype(expired)>(expired, 0, totalPhases));
  const uint32_t count = totalPhases - firstPhase;
  interpolation.hasPrevious = true;
  interpolation.targetFps = targetFps;
  interpolation.heldSet = (interpolation.heldSet + 1) % interpolation.held.size();
  auto& held = interpolation.held[interpolation.heldSet];
  flush_presents_from(&held.front(), &held.back());
  ensure_held_texture(held.back(), source);
  const wgpu::TexelCopyTextureInfo src{.texture = source.texture}, dst{.texture = held.back().texture};
  const wgpu::Extent3D extent{source.size.width, source.size.height, 1};
  encoder.CopyTextureToTexture(&src, &dst, &extent);

  if (oversampled && !count) {
    interpolation.lastSlot = {};
    return false;
  }

  if (count) {
    interpolation.prepare(source, count);
    for (uint32_t i = 0; i < count; ++i)
      ensure_held_texture(held[i], source);
    interpolation.encode(encoder, previous, source, held, count, firstPhase, totalPhases);
  }
  webgpu::gpu_prof::frame_end(encoder);
  const auto commands = encoder.Finish();
  g_queue.Submit(1, &commands);
  webgpu::gpu_prof::after_submit();
  after_submit();
  if (oversampled) {
    const auto start = PresentClock::now();
    const auto lastSlot = interpolation.lastSlot;
    firstDue = lastSlot.time_since_epoch().count() != 0
        ? std::clamp(lastSlot + spacing, start, start + spacing) : start;
    realDue = firstDue + spacing * count;
  }
  interpolation.lastSlot = realDue;
  if (g_presentLog)
    gcnllr::present_trace::record_text("[scanout-timing] frame=%llu ready_us=%lld due_us=%lld reset=%d size=%ux%u\n",
                 static_cast<unsigned long long>(frameId),
                 static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(
                     now.time_since_epoch()).count()),
                 static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(
                     realDue.time_since_epoch()).count()), !continuous, source.size.width, source.size.height);
  if (g_presentLog && count)
    gcnllr::present_trace::record_text("[scanout-interpolation] frame=%llu target=%u phases=%u encode_us=%lld\n",
                 static_cast<unsigned long long>(frameId), targetFps, count,
                 static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(
                     PresentClock::now() - now).count()));
  if (count && frame_interp::dump_frame(frameId)) {
    dump_texture(previous, "prior", frameId);
    dump_texture(held[0], "between", frameId);
    dump_texture(held.back(), "real", frameId);
  }
  for (uint32_t i = 0; i < count; ++i)
    defer_scanout_phase(firstDue + (firstPhase + i)*spacing, spacing, &held[i], callback, frameId,
                        static_cast<int>(firstPhase + i), aspectWidth, aspectHeight);
  defer_scanout_phase(realDue, spacing, &held.back(), callback, frameId, -1, aspectWidth, aspectHeight);
  deferred_present_tick();
  return true;
}
