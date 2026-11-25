include("${CMAKE_CURRENT_LIST_DIR}/source_replace.cmake")
# SPDX-License-Identifier: MIT
# Adapt a generated copy; preserve the pinned dependency checkout verbatim.
file(READ "${GXRUNTIME_DIR}/graphics/aurora/lib/window.cpp" _gcnllr_window)
set(_gcnllr_hidden "if ((flags & SDL_WINDOW_HIDDEN) != 0u) {")
string(FIND "${_gcnllr_window}" "${_gcnllr_hidden}" _gcnllr_offset)
if(_gcnllr_offset EQUAL -1)
  message(FATAL_ERROR "Pinned native Aurora hidden-window adaptation differs")
endif()
string(REPLACE "${_gcnllr_hidden}"
  "if ((flags & SDL_WINDOW_HIDDEN) != 0u && !SDL_GetHintBoolean(\"GCNLLR_RENDER_HIDDEN\", false)) {"
  _gcnllr_window "${_gcnllr_window}")
string(REPLACE "void show_window() {"
  "void show_window() {\n  if (SDL_GetHintBoolean(\"GCNLLR_RENDER_HIDDEN\", false)) return;"
  _gcnllr_window "${_gcnllr_window}")
string(REPLACE "SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, \"1\")"
  "(SDL_GetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN) != nullptr || SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, \"1\"))"
  _gcnllr_window "${_gcnllr_window}")
string(REPLACE "if (SDL_WaitEvent(&event)) {" "if (SDL_WaitEventTimeout(&event, 16)) {"
  _gcnllr_window "${_gcnllr_window}")
set(_gcnllr_window_path "${CMAKE_BINARY_DIR}/gcnllr-services/adapted/window.cpp")
file(CONFIGURE OUTPUT "${_gcnllr_window_path}" CONTENT "${_gcnllr_window}" @ONLY)
get_target_property(_gcnllr_core_sources aurora_core SOURCES)
list(REMOVE_ITEM _gcnllr_core_sources lib/window.cpp)
list(APPEND _gcnllr_core_sources "${_gcnllr_window_path}")
set_property(TARGET aurora_core PROPERTY SOURCES "${_gcnllr_core_sources}")
target_include_directories(aurora_core PRIVATE "${GXRUNTIME_DIR}/graphics/aurora/lib")

# A profile can retain compiled device blobs when it changes portable seeds.
# The caller still owns exclusive use of the writable profile cache.
set(_gcnllr_device_cache_source "${GXRUNTIME_DIR}/graphics/aurora/lib/webgpu/gpu_cache.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_gcnllr_device_cache_source}")
file(READ "${_gcnllr_device_cache_source}" _gcnllr_device_cache)
set(_gcnllr_device_cache_path "reinterpret_cast<const char8_t*>(g_config.cachePath)")
string(FIND "${_gcnllr_device_cache}" "${_gcnllr_device_cache_path}" _gcnllr_device_cache_offset)
if(_gcnllr_device_cache_offset EQUAL -1)
  message(FATAL_ERROR "Pinned Aurora device cache path differs")
endif()
string(PREPEND _gcnllr_device_cache
  "#include <SDL3/SDL_stdinc.h>\n#include \"${CMAKE_CURRENT_LIST_DIR}/sqlite_cache.hpp\"\n")
string(REPLACE "static std::filesystem::path cache_path() {"
  "static std::filesystem::path cache_path() {\n  const auto* directory = SDL_getenv(\"GCNLLR_DEVICE_CACHE_DIRECTORY\");"
  _gcnllr_device_cache "${_gcnllr_device_cache}")
string(REPLACE "${_gcnllr_device_cache_path}"
  "reinterpret_cast<const char8_t*>(directory && *directory ? directory : g_config.cachePath)"
  _gcnllr_device_cache "${_gcnllr_device_cache}")
string(REPLACE "sqlite3_open(file.c_str(), &db)" "gcnllr::open_shader_cache(file.c_str(), &db)"
  _gcnllr_device_cache "${_gcnllr_device_cache}")
set(_gcnllr_device_cache_adapted "${CMAKE_BINARY_DIR}/gcnllr-services/adapted/gpu_cache.cpp")
file(CONFIGURE OUTPUT "${_gcnllr_device_cache_adapted}" CONTENT "${_gcnllr_device_cache}" @ONLY)
get_target_property(_gcnllr_core_sources aurora_core SOURCES)
list(REMOVE_ITEM _gcnllr_core_sources lib/webgpu/gpu_cache.cpp)
list(APPEND _gcnllr_core_sources "${_gcnllr_device_cache_adapted}")
set_property(TARGET aurora_core PROPERTY SOURCES "${_gcnllr_core_sources}")
set_source_files_properties("${_gcnllr_device_cache_adapted}" TARGET_DIRECTORY aurora_core
  PROPERTIES INCLUDE_DIRECTORIES "${GXRUNTIME_DIR}/graphics/aurora/lib/webgpu")

# Dawn's Windows loader requires absolute runtime search directories. A DLL
# service cannot assume that the caller's executable sits beside Dawn/DXC.
file(READ "${GXRUNTIME_DIR}/graphics/aurora/lib/webgpu/gpu.cpp" _gcnllr_gpu)
string(PREPEND _gcnllr_gpu "#include <atomic>\n#include <SDL3/SDL_hints.h>\n")
set(_gcnllr_power ".powerPreference = wgpu::PowerPreference::HighPerformance,")
string(FIND "${_gcnllr_gpu}" "${_gcnllr_power}" _gcnllr_power_offset)
if(_gcnllr_power_offset EQUAL -1)
  message(FATAL_ERROR "Pinned native GPU adapter preference differs")
endif()
# Host adapter selection is independent of HLE/LLE selection. Keep the normal
# preference unless an explicit per-process diagnostic asks for a low-power
# adapter; this never changes another application's GPU configuration.
string(REPLACE "${_gcnllr_power}"
  ".powerPreference = SDL_GetHintBoolean(\"GCNLLR_GPU_LOW_POWER\", false) ? wgpu::PowerPreference::LowPower : wgpu::PowerPreference::HighPerformance,"
  _gcnllr_gpu "${_gcnllr_gpu}")
# Only the final scanout pass follows the surface aspect. The EFB and the
# selected VI copy retain their original dimensions and guest coordinates.
set(_gcnllr_present "  uint32_t viewport_width = surface_width;")
string(FIND "${_gcnllr_gpu}" "${_gcnllr_present}" _gcnllr_present_offset)
if(_gcnllr_present_offset EQUAL -1)
  message(FATAL_ERROR "Pinned native GPU presentation viewport differs")
endif()
string(REPLACE "${_gcnllr_present}"
  [=[  if (const uint32_t aspect = g_fixed_present_aspect.load(std::memory_order_relaxed)) {
    content_width = aspect >> 16;
    content_height = aspect & 0xffffu;
  } else if (SDL_GetHintBoolean("GCNLLR_GX_FILL_WINDOW", false)) {
    return {0.f, 0.f, static_cast<float>(surface_width), static_cast<float>(surface_height), 0.f, 1.f};
  }

  uint32_t viewport_width = surface_width;]=]
  _gcnllr_gpu "${_gcnllr_gpu}")
set(_gcnllr_viewport "Viewport calculate_present_viewport(")
string(REPLACE "${_gcnllr_viewport}"
  "static std::atomic<uint32_t> g_fixed_present_aspect;\n\nvoid set_fixed_present_aspect(uint32_t width, uint32_t height) noexcept {\n  g_fixed_present_aspect.store(width && height ? (width << 16) | height : 0, std::memory_order_relaxed);\n}\n\n${_gcnllr_viewport}"
  _gcnllr_gpu "${_gcnllr_gpu}")
set(_gcnllr_resize "void resize_swapchain(uint32_t width, uint32_t height, uint32_t nativeWidth, uint32_t nativeHeight, bool force) {\n  const gfx::render_worker::Pause pause{gfx::flush_deferred_presents};")
string(FIND "${_gcnllr_gpu}" "${_gcnllr_resize}" _gcnllr_resize_offset)
if(_gcnllr_resize_offset EQUAL -1)
  message(FATAL_ERROR "Pinned native GPU surface resize differs")
endif()
# Hand a window-only resize to the same worker that acquires and presents
# surface textures. Matching EFB dimensions keep the current render targets.
string(REPLACE "${_gcnllr_resize}"
  "void resize_swapchain(uint32_t width, uint32_t height, uint32_t nativeWidth, uint32_t nativeHeight, bool force) {\n  if (!force && width == g_frameBuffer.size.width && height == g_frameBuffer.size.height) {\n    set_follow_size(nativeWidth, nativeHeight);\n    return;\n  }\n  const gfx::render_worker::Pause pause{gfx::flush_deferred_presents};"
  _gcnllr_gpu "${_gcnllr_gpu}")
set(_gcnllr_instance "dawn::native::DawnInstanceDescriptor dawnInstanceDescriptor;")
string(REPLACE "${_gcnllr_instance}"
  "${_gcnllr_instance}\n    const char* nativeRuntimePaths[] = {SDL_GetHint(\"GCNLLR_NATIVE_RUNTIME_DIR\"), SDL_GetHint(\"GCNLLR_SYSTEM_RUNTIME_DIR\")};\n    Log.info(\"Native GPU runtime paths: {} | {}\", nativeRuntimePaths[0] ? nativeRuntimePaths[0] : \"unset\", nativeRuntimePaths[1] ? nativeRuntimePaths[1] : \"unset\");\n    if (nativeRuntimePaths[0] && nativeRuntimePaths[1]) {\n      dawnInstanceDescriptor.additionalRuntimeSearchPathsCount = 2;\n      dawnInstanceDescriptor.additionalRuntimeSearchPaths = nativeRuntimePaths;\n    }"
  _gcnllr_gpu "${_gcnllr_gpu}")
set(_gcnllr_gpu_path "${CMAKE_BINARY_DIR}/gcnllr-services/adapted/gpu.cpp")
string(PREPEND _gcnllr_gpu "#include <algorithm>\n#include <cstring>\n")
set(_gcnllr_surface_usage ".usage = wgpu::TextureUsage::RenderAttachment,\n              .width = size.native_fb_width,")
string(FIND "${_gcnllr_gpu}" "${_gcnllr_surface_usage}" _gcnllr_surface_usage_offset)
if(_gcnllr_surface_usage_offset EQUAL -1)
  message(FATAL_ERROR "Pinned Aurora surface usage differs")
endif()
string(REPLACE "${_gcnllr_surface_usage}" [=[
.usage = wgpu::TextureUsage::RenderAttachment |
                  (SDL_GetHintBoolean("GCNLLR_TEST_CAPTURE_PRESENT", false) &&
                           bool(g_surfaceCapabilities.usages & wgpu::TextureUsage::CopySrc)
                       ? wgpu::TextureUsage::CopySrc : wgpu::TextureUsage::None),
              .width = size.native_fb_width,]=] _gcnllr_gpu "${_gcnllr_gpu}")
string(REPLACE "constexpr std::array enableToggles{" "std::vector<const char*> enableToggles{"
  _gcnllr_gpu "${_gcnllr_gpu}")
string(REPLACE "wgpu::DawnTogglesDescriptor togglesDescriptor(" [=[if (SDL_GetHintBoolean("GCNLLR_VALIDATE_GPU", false)) {
      enableToggles.erase(std::remove_if(enableToggles.begin(), enableToggles.end(), [](const char* toggle) {
        return std::strcmp(toggle, "skip_validation") == 0 || std::strcmp(toggle, "disable_robustness") == 0;
      }), enableToggles.end());
    }
    wgpu::DawnTogglesDescriptor togglesDescriptor(]=]
  _gcnllr_gpu "${_gcnllr_gpu}")
file(CONFIGURE OUTPUT "${_gcnllr_gpu_path}" CONTENT "${_gcnllr_gpu}" @ONLY)
get_target_property(_gcnllr_core_sources aurora_core SOURCES)
list(FIND _gcnllr_core_sources lib/webgpu/gpu.cpp _gcnllr_gpu_source_index)
if(_gcnllr_gpu_source_index EQUAL -1)
  message(FATAL_ERROR "Pinned Aurora GPU source target differs")
endif()
list(REMOVE_ITEM _gcnllr_core_sources lib/webgpu/gpu.cpp)
list(APPEND _gcnllr_core_sources "${_gcnllr_gpu_path}")
set_property(TARGET aurora_core PROPERTY SOURCES "${_gcnllr_core_sources}")
target_include_directories(aurora_core PRIVATE "${GXRUNTIME_DIR}/graphics/aurora/lib/webgpu")

# Reuse Aurora's readback queue for an opt-in capture of the actual composed
# backbuffer. The ordinary debugger screenshot still reads the guest XFB.
file(READ "${GXRUNTIME_DIR}/graphics/aurora/lib/gfx/efb_readback.cpp" _gcnllr_readback)
string(REPLACE "std::atomic_bool g_requested{false};"
  "std::atomic_bool g_requested{false};\nstd::atomic_bool g_surface_requested{false};"
  _gcnllr_readback "${_gcnllr_readback}")
set(_gcnllr_readback_after "void after_submit() noexcept {\n  if (!g_requested.load(std::memory_order_acquire)) {")
string(FIND "${_gcnllr_readback}" "${_gcnllr_readback_after}" _gcnllr_readback_offset)
if(_gcnllr_readback_offset EQUAL -1)
  message(FATAL_ERROR "Pinned Aurora framebuffer readback differs")
endif()
string(REPLACE "${_gcnllr_readback_after}" [=[
static void read_after_submit(const wgpu::Texture& texture, std::atomic_bool& requested) noexcept {
  if (!requested.load(std::memory_order_acquire)) {]=] _gcnllr_readback "${_gcnllr_readback}")
string(REPLACE "    const auto& src = webgpu::present_source();\n    if (!src.texture) {"
  "    if (!texture) {" _gcnllr_readback "${_gcnllr_readback}")
string(REPLACE "src.format" "texture.GetFormat()" _gcnllr_readback "${_gcnllr_readback}")
string(REPLACE "src.size.width" "texture.GetWidth()" _gcnllr_readback "${_gcnllr_readback}")
string(REPLACE "src.size.height" "texture.GetHeight()" _gcnllr_readback "${_gcnllr_readback}")
string(REPLACE ".texture = src.texture," ".texture = texture," _gcnllr_readback "${_gcnllr_readback}")
string(REPLACE "g_requested.store(false," "requested.store(false," _gcnllr_readback "${_gcnllr_readback}")
string(REPLACE "bool take(const std::uint8_t** rgba," [=[
void after_submit() noexcept {
  read_after_submit(webgpu::present_source().texture, g_requested);
}

bool request_surface() noexcept {
  if (!bool(webgpu::g_graphicsConfig.surfaceConfiguration.usage & wgpu::TextureUsage::CopySrc))
    return false;
  g_surface_requested.store(true, std::memory_order_release);
  return true;
}

void after_surface_submit(const wgpu::Texture& texture) noexcept {
  read_after_submit(texture, g_surface_requested);
}

bool take(const std::uint8_t** rgba,]=] _gcnllr_readback "${_gcnllr_readback}")
set(_gcnllr_readback_path "${CMAKE_BINARY_DIR}/gcnllr-services/adapted/efb_readback.cpp")
file(CONFIGURE OUTPUT "${_gcnllr_readback_path}" CONTENT "${_gcnllr_readback}" @ONLY)

file(READ "${GXRUNTIME_DIR}/graphics/aurora/lib/aurora.cpp" _gcnllr_surface)
function(gcnllr_trace_boundary variable before after)
  string(FIND "${${variable}}" "${before}" offset)
  if(offset LESS 0)
    message(FATAL_ERROR "Pinned Aurora timing boundary differs: ${before}")
  endif()
  string(REPLACE "${before}" "${after}" adapted "${${variable}}")
  set(${variable} "${adapted}" PARENT_SCOPE)
endfunction()
gcnllr_trace_boundary(_gcnllr_surface "g_surface.GetCurrentTexture(&surfaceTexture);"
  "hitch::mark(hitch::Lane::render, hitch::Event::acquire_begin);\n        g_surface.GetCurrentTexture(&surfaceTexture);\n        hitch::mark(hitch::Lane::render, hitch::Event::acquire_end);")
gcnllr_trace_boundary(_gcnllr_surface "const auto buffer = encoder.Finish(&cmdBufDescriptor);"
  "hitch::mark(hitch::Lane::render, hitch::Event::finish_begin);\n    const auto buffer = encoder.Finish(&cmdBufDescriptor);")
gcnllr_trace_boundary(_gcnllr_surface "g_queue.Submit(1, &buffer);"
  "hitch::mark(hitch::Lane::render, hitch::Event::submit_begin);\n      g_queue.Submit(1, &buffer);\n      hitch::mark(hitch::Lane::render, hitch::Event::submit_end);")
gcnllr_trace_boundary(_gcnllr_surface "status = g_surface.Present();"
  "hitch::mark(hitch::Lane::render, hitch::Event::swap_begin);\n          status = g_surface.Present();\n          hitch::mark(hitch::Lane::render, hitch::Event::swap_end);")
set(_gcnllr_surface_submit "    if (canPresent && g_surface) {\n      ZoneScopedN(\"Present\");")
string(FIND "${_gcnllr_surface}" "${_gcnllr_surface_submit}" _gcnllr_surface_offset)
if(_gcnllr_surface_offset EQUAL -1)
  message(FATAL_ERROR "Pinned Aurora composed surface submission differs")
endif()
string(REPLACE "${_gcnllr_surface_submit}"
  "    if (canPresent && g_surface) {\n      gfx::efb_readback::after_surface_submit(currentTexture);\n      ZoneScopedN(\"Present\");"
  _gcnllr_surface "${_gcnllr_surface}")
string(PREPEND _gcnllr_surface
  "#include \"${CMAKE_CURRENT_LIST_DIR}/present_trace.hpp\"\n#include <webgpu/webgpu_cpp.h>\nnamespace hitch = gcnllr::present_trace;\nnamespace aurora::gfx::efb_readback { void after_surface_submit(const wgpu::Texture&) noexcept; }\n")
set(_gcnllr_surface_path "${CMAKE_BINARY_DIR}/gcnllr-services/adapted/aurora.cpp")
file(CONFIGURE OUTPUT "${_gcnllr_surface_path}" CONTENT "${_gcnllr_surface}" @ONLY)
get_target_property(_gcnllr_core_sources aurora_core SOURCES)
list(REMOVE_ITEM _gcnllr_core_sources lib/aurora.cpp)
list(APPEND _gcnllr_core_sources "${_gcnllr_surface_path}")

set_property(TARGET aurora_core PROPERTY SOURCES "${_gcnllr_core_sources}")
get_target_property(_gcnllr_readback_sources aurora_gx SOURCES)
list(REMOVE_ITEM _gcnllr_readback_sources lib/gfx/efb_readback.cpp)
list(APPEND _gcnllr_readback_sources "${_gcnllr_readback_path}")
set_property(TARGET aurora_gx PROPERTY SOURCES "${_gcnllr_readback_sources}")
target_include_directories(aurora_gx PRIVATE "${GXRUNTIME_DIR}/graphics/aurora/lib/gfx")

# Release live-device owners during shutdown, before Windows' loader lock is
# held by FreeLibrary. The unmodified donor executable never tested DLL unload.
if(GCNLLR_RENDER_PIPELINE STREQUAL "retail" AND GCNLLR_GPU_FRAME_PACKETS)
  include("${CMAKE_CURRENT_LIST_DIR}/retail_draw_packet.cmake")
endif()
file(READ "${GXRUNTIME_DIR}/graphics/aurora/lib/gx/gx.cpp" _gcnllr_gx)
if(_gcnllr_draw_packet_header)
  gcnllr_draw_packet_replace(_gcnllr_gx "#include \"pipeline.hpp\""
    "#include \"${_gcnllr_draw_packet_header}\"")
endif()
set(_gcnllr_shutdown "void shutdown() noexcept {\n  // TODO we should probably store this all in g_state.gx instead")
string(FIND "${_gcnllr_gx}" "${_gcnllr_shutdown}" _gcnllr_shutdown_offset)
if(_gcnllr_shutdown_offset EQUAL -1)
  message(FATAL_ERROR "Pinned native GX shutdown adaptation differs")
endif()
string(REPLACE "${_gcnllr_shutdown}"
  "void shutdown() noexcept {\n  sPipelineLayout = {};\n  g_emptyTextureBindGroup = {};\n  sEmptyTextureView = {};\n  sEmptyTexture = {};\n  sEmptySampler = {};"
  _gcnllr_gx "${_gcnllr_gx}")
if(_gcnllr_draw_packet_header)
  string(PREPEND _gcnllr_gx "namespace aurora::gx { void reset_retail_uniforms() noexcept; }\n")
  gcnllr_draw_packet_replace(_gcnllr_gx "void shutdown() noexcept {\n  sPipelineLayout"
    "void shutdown() noexcept {\n  reset_retail_uniforms();\n  sPipelineLayout")
endif()
set(_gcnllr_gx_path "${CMAKE_BINARY_DIR}/gcnllr-services/adapted/gx.cpp")
file(CONFIGURE OUTPUT "${_gcnllr_gx_path}" CONTENT "${_gcnllr_gx}" @ONLY)
get_target_property(_gcnllr_gx_sources aurora_gx SOURCES)
list(REMOVE_ITEM _gcnllr_gx_sources lib/gx/gx.cpp)
list(APPEND _gcnllr_gx_sources "${_gcnllr_gx_path}")
set_property(TARGET aurora_gx PROPERTY SOURCES "${_gcnllr_gx_sources}")
set_source_files_properties("${_gcnllr_gx_path}" TARGET_DIRECTORY aurora_gx
  PROPERTIES INCLUDE_DIRECTORIES "${GXRUNTIME_DIR}/graphics/aurora/lib/gx")

# Specialized shaders may compile asynchronously only when a ready fallback
# can draw the frame. The donor also queued that fallback asynchronously,
# losing first-use draws. Wait for the fallback (or the specialized shader on
# a backend without fallback support) instead of omitting guest geometry.
set(_gcnllr_pipeline_original "${GXRUNTIME_DIR}/graphics/aurora/lib/gfx/pipeline_cache.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_gcnllr_pipeline_original}")
file(READ "${_gcnllr_pipeline_original}" _gcnllr_pipeline)
string(PREPEND _gcnllr_pipeline
  "#include \"${CMAKE_CURRENT_LIST_DIR}/sqlite_cache.hpp\"\n")
gcnllr_draw_packet_replace(_gcnllr_pipeline
  "std::filesystem::path{g_config.cachePath} / \"pipeline_cache.db\""
  "std::filesystem::path{reinterpret_cast<const char8_t*>(g_config.cachePath)} / \"pipeline_cache.db\"")
gcnllr_draw_packet_replace(_gcnllr_pipeline "sqlite3_open(path.c_str(), &g_pipelineCacheDb)"
  "gcnllr::open_shader_cache(path.c_str(), &g_pipelineCacheDb)")
if(_gcnllr_draw_packet_header)
  gcnllr_draw_packet_replace(_gcnllr_pipeline "#include \"../gx/pipeline.hpp\""
    "#include \"${_gcnllr_draw_packet_header}\"")
endif()
if(GCNLLR_RENDER_PIPELINE STREQUAL "retail")
  # Retail keys include captured texgen state. Use their version for both
  # loading and pruning, so persisted keys can be compiled before first use.
  set(_gcnllr_retail_key_version 3)
  if(_gcnllr_draw_packet_header)
    set(_gcnllr_retail_key_version 5)
  endif()
  target_compile_definitions(aurora_gx PRIVATE GCNLLR_RETAIL_GX_CONFIG_VERSION_OFFSET=${_gcnllr_retail_key_version})
  set(_gcnllr_gx_version "gx::GXPipelineConfigVersion")
  string(REGEX MATCHALL "gx::GXPipelineConfigVersion" _gcnllr_gx_versions "${_gcnllr_pipeline}")
  list(LENGTH _gcnllr_gx_versions _gcnllr_gx_version_count)
  if(NOT _gcnllr_gx_version_count EQUAL 2)
    message(FATAL_ERROR "Pinned native GX pipeline cache version checks differ")
  endif()
  string(REPLACE "${_gcnllr_gx_version}"
    "(${_gcnllr_gx_version} + GCNLLR_RETAIL_GX_CONFIG_VERSION_OFFSET)"
    _gcnllr_pipeline "${_gcnllr_pipeline}")
endif()
set(_gcnllr_hash "const PipelineRef hash = xxh3_hash(config, static_cast<HashType>(type));")
string(FIND "${_gcnllr_pipeline}" "${_gcnllr_hash}" _gcnllr_pipeline_offset)
if(_gcnllr_pipeline_offset EQUAL -1)
  message(FATAL_ERROR "Pinned native GX pipeline adaptation differs")
endif()
string(REPLACE "${_gcnllr_hash}"
  "if constexpr (std::is_same_v<PipelineConfig, gx::PipelineConfig>) {\n    if (priority == PipelinePriority::Normal) priority = PipelinePriority::Blocking;\n  }\n#ifdef AURORA_ENABLE_GXCORE\n  if constexpr (std::is_same_v<PipelineConfig, gxcore::PipelineConfig>) {\n    if (priority == PipelinePriority::Normal && (config.depthOnly == 2u || ubershader_mode() == 0))\n      priority = PipelinePriority::Blocking;\n  }\n#endif\n  ${_gcnllr_hash}"
  _gcnllr_pipeline "${_gcnllr_pipeline}")
string(PREPEND _gcnllr_pipeline "#include <cstring>\n#include <type_traits>\n")
if(GCNLLR_GPU_FRAME_PACKETS)
  string(REPLACE "${_gcnllr_hash}" [=[// The machine owns creation and ordered draw recording.
  // Reuse only an exact key that the blocking path has already made ready.
  struct ReadyPipeline {
    PipelineConfig config{};
    PipelineRef ref{};
    ShaderType type{};
    uint64_t generation{};
    bool valid{};
  };
  static thread_local ReadyPipeline ready;
  const bool canReuse = std::is_same_v<PipelineConfig, gx::PipelineConfig> &&
                        priority == PipelinePriority::Blocking && !firstFrameUsedOverride && !queuedNew;
  const auto cacheGeneration = pipeline_cache_generation();
  if (canReuse && ready.valid && ready.generation == cacheGeneration && ready.type == type &&
      std::memcmp(&ready.config, &config, sizeof(config)) == 0)
    return ready.ref;
  const PipelineRef hash = xxh3_hash(config, static_cast<HashType>(type));]=]
    _gcnllr_pipeline "${_gcnllr_pipeline}")
  set(_gcnllr_pipeline_cv "    g_pipelineReadyCv.wait(lock, [=] { return g_pipelines.contains(hash) || g_pipelineThreadEnd; });")
  string(FIND "${_gcnllr_pipeline}" "${_gcnllr_pipeline_cv}" _gcnllr_pipeline_cv_at)
  if(_gcnllr_pipeline_cv_at EQUAL -1)
    message(FATAL_ERROR "Pinned blocking pipeline wait differs")
  endif()
  string(REPLACE "${_gcnllr_pipeline_cv}" [=[    static const bool profileWait = std::getenv("GCNLLR_GX_PROFILE") != nullptr;
    const auto waitBegin = profileWait ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    g_pipelineReadyCv.wait(lock, [=] { return g_pipelines.contains(hash) || g_pipelineThreadEnd; });
    if (profileWait) {
      const auto waitEnd = std::chrono::steady_clock::now();
      const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(waitEnd - waitBegin).count();
      if (elapsed >= 2000)
        Log.info("Blocking pipeline frame={} key={:x} time_us={} stamp_ns={}", current_frame(), hash, elapsed,
                 std::chrono::duration_cast<std::chrono::nanoseconds>(waitEnd.time_since_epoch()).count());
    }]=] _gcnllr_pipeline "${_gcnllr_pipeline}")
  set(_gcnllr_pipeline_wait [=[    auto pipelineIt = g_pipelines.find(hash);
    if (pipelineIt != g_pipelines.end() && persist && firstFrameUsed < pipelineIt->second.firstFrameUsed) {]=])
  string(FIND "${_gcnllr_pipeline}" "${_gcnllr_pipeline_wait}" _gcnllr_pipeline_wait_at)
  if(_gcnllr_pipeline_wait_at EQUAL -1)
    message(FATAL_ERROR "Pinned native pipeline readiness boundary differs")
  endif()
  string(REPLACE "${_gcnllr_pipeline_wait}" [=[    auto pipelineIt = g_pipelines.find(hash);
    pipelineReady = pipelineIt != g_pipelines.end();
    if (pipelineReady && persist && firstFrameUsed < pipelineIt->second.firstFrameUsed) {]=]
    _gcnllr_pipeline "${_gcnllr_pipeline}")
  set(_gcnllr_pipeline_return [=[  return hash;
}

#ifdef AURORA_ENABLE_GXCORE]=])
  string(FIND "${_gcnllr_pipeline}" "${_gcnllr_pipeline_return}" _gcnllr_pipeline_return_at)
  if(_gcnllr_pipeline_return_at EQUAL -1)
    message(FATAL_ERROR "Pinned native pipeline return boundary differs")
  endif()
  string(REPLACE "${_gcnllr_pipeline_return}" [=[  if (canReuse && pipelineReady)
    ready = {config, hash, type, cacheGeneration, true};
  return hash;
}

#ifdef AURORA_ENABLE_GXCORE]=] _gcnllr_pipeline "${_gcnllr_pipeline}")
  if(GCNLLR_RENDER_PIPELINE STREQUAL "retail")
    # Retail draws use their exact pipelines. The host SDK's initial GXCore
    # work can use a specialized pipeline instead of starting the generic
    # compiler before the first frame or prewarming its unused cache entries.
    set(_gcnllr_uber_default "    return webgpu::g_backendType == wgpu::BackendType::D3D12 ? 1 : 0;")
    set(_gcnllr_uber_cached "      if (const int8_t uber = ubershader_of(config); uber >= 0) {")
    foreach(_gcnllr_uber_anchor IN ITEMS "${_gcnllr_uber_default}" "${_gcnllr_uber_cached}")
      string(FIND "${_gcnllr_pipeline}" "${_gcnllr_uber_anchor}" _gcnllr_uber_offset)
      if(_gcnllr_uber_offset EQUAL -1)
        message(FATAL_ERROR "Pinned retail ubershader boundary differs: ${_gcnllr_uber_anchor}")
      endif()
    endforeach()
    string(REPLACE "${_gcnllr_uber_default}" "    return 0;" _gcnllr_pipeline "${_gcnllr_pipeline}")
    string(REPLACE "${_gcnllr_uber_cached}"
      "      if (ubershader_mode() == 0 && ubershader_of(config) >= 0)\n        continue;\n${_gcnllr_uber_cached}"
      _gcnllr_pipeline "${_gcnllr_pipeline}")
    set(_gcnllr_pipeline_clear "  g_pipelines.clear();\n  g_pipelineQueue.clear();")
    string(FIND "${_gcnllr_pipeline}" "${_gcnllr_pipeline_clear}" _gcnllr_pipeline_clear_at)
    if(_gcnllr_pipeline_clear_at EQUAL -1)
      message(FATAL_ERROR "Pinned native shader module shutdown boundary differs")
    endif()
    string(PREPEND _gcnllr_pipeline "namespace aurora::gx { void clear_machine_shader_modules(); }\n")
    string(REPLACE "${_gcnllr_pipeline_clear}"
      "  g_pipelines.clear();\n  gx::clear_machine_shader_modules();\n  g_pipelineQueue.clear();"
      _gcnllr_pipeline "${_gcnllr_pipeline}")
  endif()
endif()
set(_gcnllr_pipeline_path "${CMAKE_BINARY_DIR}/gcnllr-services/adapted/pipeline_cache.cpp")
file(CONFIGURE OUTPUT "${_gcnllr_pipeline_path}" CONTENT "${_gcnllr_pipeline}" @ONLY)
get_target_property(_gcnllr_gx_sources aurora_gx SOURCES)
list(REMOVE_ITEM _gcnllr_gx_sources lib/gfx/pipeline_cache.cpp)
list(APPEND _gcnllr_gx_sources "${_gcnllr_pipeline_path}")
set_property(TARGET aurora_gx PROPERTY SOURCES "${_gcnllr_gx_sources}")
set_source_files_properties("${_gcnllr_pipeline_path}" TARGET_DIRECTORY aurora_gx
  PROPERTIES INCLUDE_DIRECTORIES "${GXRUNTIME_DIR}/graphics/aurora/lib/gfx")

# Dropping an unparsed display list loses required geometry. Preserve the
# diagnostic and reject the command instead of clearing its failure state.
file(READ "${GXRUNTIME_DIR}/graphics/frontend/src/retail_gx_frontend.cpp" _gcnllr_frontend)
set(_gcnllr_drop "last_error_ = nullptr;\n        ++display_lists_skipped_;")
string(FIND "${_gcnllr_frontend}" "${_gcnllr_drop}" _gcnllr_frontend_offset)
if(_gcnllr_frontend_offset EQUAL -1)
  message(FATAL_ERROR "Pinned display-list failure adaptation differs")
endif()
string(REPLACE "${_gcnllr_drop}"
  "return fail_parse(\"Display list did not parse completely\", cmd, pos, address, size, static_cast<uint32_t>(dl_consumed));"
  _gcnllr_frontend "${_gcnllr_frontend}")
# Retail GXSetTexCopyDst encodes Z16 as real copy format 0xB. Treating it
# as the default Z24X8 doubles the MEM1 write footprint and corrupts callers.
set(_gcnllr_z16 "case 0x3:\n      return 0x13; // GX_TF_Z16")
string(FIND "${_gcnllr_frontend}" "${_gcnllr_z16}" _gcnllr_z16_offset)
if(_gcnllr_z16_offset EQUAL -1)
  message(FATAL_ERROR "Pinned GX Z16 copy decoder adaptation differs")
endif()
string(REPLACE "${_gcnllr_z16}"
  "case 0x3:\n    case 0xB: // GXSetTexCopyDst(GX_TF_Z16)\n      return 0x13; // GX_TF_Z16"
  _gcnllr_frontend "${_gcnllr_frontend}")
# Color conversion is requested only when both trigger bits are set. Without
# auto-conversion the same tiled destination carries raw selected channels.
set(_gcnllr_intensity_decode "const bool intensity = bp_get(value, 1u, 15u) != 0u;")
string(FIND "${_gcnllr_frontend}" "${_gcnllr_intensity_decode}" _gcnllr_intensity_offset)
if(_gcnllr_intensity_offset EQUAL -1)
  message(FATAL_ERROR "Pinned copy intensity decoder differs")
endif()
string(REPLACE "${_gcnllr_intensity_decode}"
  "const bool intensity = bp_get(value, 1u, 15u) != 0u && bp_get(value, 1u, 16u) != 0u;"
  _gcnllr_frontend "${_gcnllr_frontend}")
set(_gcnllr_frontend_path "${CMAKE_BINARY_DIR}/gcnllr-services/adapted/retail_gx_frontend.cpp")
file(CONFIGURE OUTPUT "${_gcnllr_frontend_path}" CONTENT "${_gcnllr_frontend}" @ONLY)
get_target_property(_gcnllr_frontend_sources gxruntime_retail_gx_frontend SOURCES)
list(REMOVE_ITEM _gcnllr_frontend_sources src/retail_gx_frontend.cpp)
list(APPEND _gcnllr_frontend_sources "${_gcnllr_frontend_path}")
set_property(TARGET gxruntime_retail_gx_frontend PROPERTY SOURCES "${_gcnllr_frontend_sources}")

# Materialize supported native EFB copies into the machine's tiled MEM1 ABI.
# The donor only encoded I8/RGB565 because its own CPU path bypassed other
# consumers. Our actual retail CPU can observe every copied buffer.
file(READ "${GXRUNTIME_DIR}/graphics/aurora/lib/gfx/gxcore_draw.cpp" _gcnllr_draw)
set(_gcnllr_copy_guard "it == g_efbCopyTextures.end() || !it->second.handle ||\n      (cmd.format != 1u && cmd.format != 4u)")
string(FIND "${_gcnllr_draw}" "${_gcnllr_copy_guard}" _gcnllr_copy_offset)
if(_gcnllr_copy_offset EQUAL -1)
  message(FATAL_ERROR "Pinned native texture-copy readback adaptation differs")
endif()
string(REPLACE "${_gcnllr_copy_guard}" "it == g_efbCopyTextures.end() || !it->second.handle"
  _gcnllr_draw "${_gcnllr_draw}")
string(REPLACE "gxc::encode_efb_copy(cmd.format" "gcnllr::encode_native_copy(cmd.format"
  _gcnllr_draw "${_gcnllr_draw}")
# An unavailable dirty epoch cannot qualify an unchanged EFB shadow. Our
# machine materializes the copy in MEM1; decode/hash those actual bytes.
set(_gcnllr_shadow_guard "!efbIt->second.memory_epoch_valid ||")
string(FIND "${_gcnllr_draw}" "${_gcnllr_shadow_guard}" _gcnllr_shadow_offset)
if(_gcnllr_shadow_offset EQUAL -1)
  message(FATAL_ERROR "Pinned EFB shadow coherence guard differs")
endif()
string(REPLACE "${_gcnllr_shadow_guard}" "efbIt->second.memory_epoch_valid &&"
  _gcnllr_draw "${_gcnllr_draw}")
# Retain the donor's GPU copy after actual CPU materialization. Hash the bytes
# produced by the copy, not a later RAM snapshot: native CPU stores may resume
# as soon as the machine acknowledges publication. Verify actual RAM and the
# sampled layout before reuse; unknown/strided views still decode real MEM1.
set(_gcnllr_binding "bool memory_epoch_valid = false;\n};\nabsl::flat_hash_map<uint32_t, EfbCopyBinding> g_efbCopyTextures;")
string(FIND "${_gcnllr_draw}" "${_gcnllr_binding}" _gcnllr_binding_offset)
if(_gcnllr_binding_offset EQUAL -1)
  message(FATAL_ERROR "Pinned EFB copy binding differs")
endif()
string(REPLACE "${_gcnllr_binding}" [=[bool memory_epoch_valid = false;
  uint64_t materialized_hash = 0;
  uint32_t materialized_size = 0;
  uint32_t width = 0, height = 0, format = 0;
};
absl::flat_hash_map<uint32_t, EfbCopyBinding> g_efbCopyTextures;
uint64_t g_verifiedCopyHits = 0, g_verifiedCopyMisses = 0;]=]
  _gcnllr_draw "${_gcnllr_draw}")
set(_gcnllr_epoch_test [=[const bool memoryUnchanged =
          efbIt->second.memory_epoch_valid &&
          (efb_copy_epoch(address, efbIt->second.byte_size, &memoryEpoch) &&
           memoryEpoch == efbIt->second.memory_epoch);]=])
string(FIND "${_gcnllr_draw}" "${_gcnllr_epoch_test}" _gcnllr_epoch_test_offset)
if(_gcnllr_epoch_test_offset EQUAL -1)
  message(FATAL_ERROR "Pinned EFB copy sampling guard differs")
endif()
string(REPLACE "${_gcnllr_epoch_test}" [=[const auto& copy = efbIt->second;
      bool memoryUnchanged;
      if (copy.materialized_size != 0u) {
        // A bound pointer can outlive another GPU producer at this address.
        // Observe ownership before reading the expected copy's actual bytes.
        if (g_textureDirtyEpochObserver != nullptr && data != nullptr) {
          uint64_t observedEpoch;
          g_textureDirtyEpochObserver(address, std::min(available,copy.materialized_size), &observedEpoch);
        }
        memoryUnchanged = data != nullptr && available >= copy.materialized_size &&
            width == copy.width && height == copy.height && format == copy.format &&
            XXH3_64bits(data, copy.materialized_size) == copy.materialized_hash;
        if (memoryUnchanged) ++g_verifiedCopyHits;
        else ++g_verifiedCopyMisses;
      } else {
        memoryUnchanged = copy.memory_epoch_valid &&
            (efb_copy_epoch(address, copy.byte_size, &memoryEpoch) && memoryEpoch == copy.memory_epoch);
      }]=] _gcnllr_draw "${_gcnllr_draw}")
set(_gcnllr_copy_publish [=[
void publish_efb_copy_memory(uint32_t address, const void* bytes, uint32_t size,
                             uint32_t width, uint32_t height, uint32_t format) {
  auto it = g_efbCopyTextures.find(address);
  if (it == g_efbCopyTextures.end() || bytes == nullptr || size == 0u) return;
  auto& copy = it->second;
  copy.materialized_hash = XXH3_64bits(bytes, size);
  copy.materialized_size = size;
  copy.width = width; copy.height = height; copy.format = format;
}
void efb_copy_memory_stats(uint64_t* hits, uint64_t* misses) {
  *hits = g_verifiedCopyHits; *misses = g_verifiedCopyMisses;
}
]=])
string(REPLACE "void copy_efb_to_texture(const gxc::EfbCopyCommand& cmd) {"
  "${_gcnllr_copy_publish}\nvoid copy_efb_to_texture(const gxc::EfbCopyCommand& cmd) {"
  _gcnllr_draw "${_gcnllr_draw}")
string(REPLACE "void reset_texture_cache() {"
  "void reset_texture_cache() {\n  g_verifiedCopyHits = g_verifiedCopyMisses = 0;"
  _gcnllr_draw "${_gcnllr_draw}")
# A deferred producer owns a fresh render target. The donor's address/shape
# cache normally reuses mutable targets, which cannot represent older tokens.
set(_gcnllr_owned_allocation "auto it = g_efbCopyCache.find(key);\n  if (it == g_efbCopyCache.end() || !it->second) {")
set(_gcnllr_owned_epoch "cmd.byte_size != 0u && efb_copy_epoch(cmd.dest_address, cmd.byte_size, &memoryEpoch);")
set(_gcnllr_owned_lookup "    auto efbIt = g_efbCopyTextures.find(address);")
set(_gcnllr_owned_tail "// Uniform de-duplication across consecutive draws.")
foreach(_gcnllr_owned_anchor IN ITEMS _gcnllr_owned_allocation _gcnllr_owned_epoch _gcnllr_owned_lookup _gcnllr_owned_tail)
  string(FIND "${_gcnllr_draw}" "${${_gcnllr_owned_anchor}}" _gcnllr_owned_offset)
  if(_gcnllr_owned_offset EQUAL -1)
    message(FATAL_ERROR "Pinned owned texture adaptation differs: ${_gcnllr_owned_anchor}")
  endif()
endforeach()
string(REPLACE "${_gcnllr_owned_allocation}"
  "auto it = g_efbCopyCache.find(key);\n  if (g_capturingOwnedTexture || it == g_efbCopyCache.end() || !it->second) {"
  _gcnllr_draw "${_gcnllr_draw}")
# Complete replacement need not download the previous destination. The
# machine's row ownership manager handles partial overlap/eviction at publish.
string(REPLACE "${_gcnllr_owned_epoch}"
  "!g_capturingOwnedTexture && cmd.byte_size != 0u && efb_copy_epoch(cmd.dest_address, cmd.byte_size, &memoryEpoch);"
  _gcnllr_draw "${_gcnllr_draw}")
string(REPLACE "void copy_efb_to_texture(const gxc::EfbCopyCommand& cmd) {"
  "bool g_capturingOwnedTexture = false;\nOwnedTextureLookup g_ownedTextureLookup = nullptr;\nvoid copy_efb_to_texture(const gxc::EfbCopyCommand& cmd) {"
  _gcnllr_draw "${_gcnllr_draw}")
string(REPLACE "${_gcnllr_owned_lookup}" [=[    // A live exact producer is the proof for a GPU view. On a miss every
    // existing observer/hash/decode below still sees actual current MEM1.
    if (g_ownedTextureLookup != nullptr) {
      if (auto owned = g_ownedTextureLookup(address, tsize, format, width, height))
        return owned;
    }
    auto efbIt = g_efbCopyTextures.find(address);]=]
  _gcnllr_draw "${_gcnllr_draw}")
string(REPLACE "${_gcnllr_owned_tail}"
  "#include \"${CMAKE_CURRENT_LIST_DIR}/owned_texture.inc\"\n\n${_gcnllr_owned_tail}"
  _gcnllr_draw "${_gcnllr_draw}")
string(PREPEND _gcnllr_draw "#include \"${CMAKE_CURRENT_LIST_DIR}/owned_texture.hpp\"\n#include <stdexcept>\n")
string(PREPEND _gcnllr_draw "#include \"${CMAKE_CURRENT_LIST_DIR}/texture_encode.hpp\"\n")
set(_gcnllr_draw_path "${CMAKE_BINARY_DIR}/gcnllr-services/adapted/gxcore_draw.cpp")
file(CONFIGURE OUTPUT "${_gcnllr_draw_path}" CONTENT "${_gcnllr_draw}" @ONLY)
get_target_property(_gcnllr_gx_sources aurora_gx SOURCES)
list(REMOVE_ITEM _gcnllr_gx_sources lib/gfx/gxcore_draw.cpp)
list(APPEND _gcnllr_gx_sources "${_gcnllr_draw_path}")
set_property(TARGET aurora_gx PROPERTY SOURCES "${_gcnllr_gx_sources}")
set_source_files_properties("${_gcnllr_draw_path}" TARGET_DIRECTORY aurora_gx
  PROPERTIES INCLUDE_DIRECTORIES "${GXRUNTIME_DIR}/graphics/aurora/lib/gfx")

# Replace the diagnostic allocation/poll loop with the ordered native-copy
# provider. It preserves actual RGBA bytes and uses worker-owned pooled staging
# and Dawn completion events; the donor checkout stays immutable.
file(READ "${GXRUNTIME_DIR}/graphics/aurora/lib/gfx/texture.cpp" _gcnllr_texture)
string(FIND "${_gcnllr_texture}" "std::vector<uint8_t> read_texture_rgba8(const TextureHandle& texture) {" _gcnllr_readback_first)
string(FIND "${_gcnllr_texture}" "void write_texture(TextureRef& ref, ArrayRef<uint8_t> data) noexcept {" _gcnllr_readback_last)
if(_gcnllr_readback_first LESS 0 OR _gcnllr_readback_last LESS_EQUAL _gcnllr_readback_first)
  message(FATAL_ERROR "Pinned texture readback provider boundary differs")
endif()
string(SUBSTRING "${_gcnllr_texture}" 0 ${_gcnllr_readback_first} _gcnllr_texture_before)
string(SUBSTRING "${_gcnllr_texture}" ${_gcnllr_readback_last} -1 _gcnllr_texture_after)
set(_gcnllr_texture "${_gcnllr_texture_before}${_gcnllr_texture_after}")
set(_gcnllr_texture_path "${CMAKE_BINARY_DIR}/gcnllr-services/adapted/texture.cpp")
file(CONFIGURE OUTPUT "${_gcnllr_texture_path}" CONTENT "${_gcnllr_texture}" @ONLY)
get_target_property(_gcnllr_gx_sources aurora_gx SOURCES)
list(REMOVE_ITEM _gcnllr_gx_sources lib/gfx/texture.cpp)
list(APPEND _gcnllr_gx_sources "${_gcnllr_texture_path}")
list(APPEND _gcnllr_gx_sources "${CMAKE_CURRENT_LIST_DIR}/texture_readback.cpp")
set_property(TARGET aurora_gx PROPERTY SOURCES "${_gcnllr_gx_sources}")
set_source_files_properties("${_gcnllr_texture_path}" TARGET_DIRECTORY aurora_gx
  PROPERTIES INCLUDE_DIRECTORIES "${GXRUNTIME_DIR}/graphics/aurora/lib/gfx")
set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/texture_readback.cpp" TARGET_DIRECTORY aurora_gx
  PROPERTIES INCLUDE_DIRECTORIES "${GXRUNTIME_DIR}/graphics/aurora/lib")

# A GPU-resident RGB565 copy must sample the same quantized channels as its
# tiled MEM1 representation. An unquantized RGBA blit only agrees on primary
# colors and changes after a CPU observation forces tile decode.
file(READ "${GXRUNTIME_DIR}/graphics/aurora/lib/gfx/tex_copy_conv.cpp" _gcnllr_conversion)
set(_gcnllr_rgb565 [=[static constexpr std::string_view FragRGB565 = R"(
@fragment fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let c = textureSample(src, src_samp, in.uv);
    return vec4f(c.rgb, 1.0);
}
)"sv;]=])
string(FIND "${_gcnllr_conversion}" "${_gcnllr_rgb565}" _gcnllr_rgb565_offset)
if(_gcnllr_rgb565_offset EQUAL -1)
  message(FATAL_ERROR "Pinned RGB565 copy conversion differs")
endif()
string(REPLACE "${_gcnllr_rgb565}" [=[static constexpr std::string_view FragRGB565 = R"(
@fragment fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let c = textureSample(src, src_samp, in.uv);
    let raw = vec3u(clamp(floor(c.rgb * 255.0 + vec3f(0.5)), vec3f(0.0), vec3f(255.0)));
    let packed = raw >> vec3u(3u, 2u, 3u);
    let expanded = (packed << vec3u(3u, 2u, 3u)) | (packed >> vec3u(2u, 4u, 2u));
    return vec4f(vec3f(expanded) / 255.0, 1.0);
}
)"sv;]=] _gcnllr_conversion "${_gcnllr_conversion}")
# Use the hardware-tested integer luma matrix before format quantization.
# Approximate floating coefficients produce 81 instead of 82 for full red.
# Dolphin TextureConverterShaderGen.cpp supplies the same conversion in the
# maintained scalar floor (GPL-2.0-or-later; surrounding Aurora is GPL).
set(_gcnllr_luma [=[fn intensity(rgb: vec3f) -> f32 {
    // ITU-R BT.601 luma coefficients
    return dot(rgb, vec3f(0.257, 0.504, 0.098)) + 16.0 / 255.0;
}]=])
string(FIND "${_gcnllr_conversion}" "${_gcnllr_luma}" _gcnllr_luma_offset)
if(_gcnllr_luma_offset EQUAL -1)
  message(FATAL_ERROR "Pinned copy luma conversion differs")
endif()
string(REPLACE "${_gcnllr_luma}" [=[fn intensity(rgb: vec3f) -> f32 {
    let raw = vec3u(clamp(floor(rgb * 255.0 + vec3f(0.5)), vec3f(0.0), vec3f(255.0)));
    let y = (66u * raw.r + 129u * raw.g + 25u * raw.b + 4096u + 128u) >> 8u;
    return f32(y) / 255.0;
}]=] _gcnllr_conversion "${_gcnllr_conversion}")
set(_gcnllr_conversion_path "${CMAKE_BINARY_DIR}/gcnllr-services/adapted/tex_copy_conv.cpp")
file(CONFIGURE OUTPUT "${_gcnllr_conversion_path}" CONTENT "${_gcnllr_conversion}" @ONLY)
get_target_property(_gcnllr_gx_sources aurora_gx SOURCES)
list(REMOVE_ITEM _gcnllr_gx_sources lib/gfx/tex_copy_conv.cpp)
list(APPEND _gcnllr_gx_sources "${_gcnllr_conversion_path}")
set_property(TARGET aurora_gx PROPERTY SOURCES "${_gcnllr_gx_sources}")
set_source_files_properties("${_gcnllr_conversion_path}" TARGET_DIRECTORY aurora_gx
  PROPERTIES INCLUDE_DIRECTORIES "${GXRUNTIME_DIR}/graphics/aurora/lib/gfx")

# GX_MTX2x4 initializes only two rows. Requiring an unused third row discarded
# valid IPL environment-map matrices and substituted raw-normal coordinates.
file(READ "${GXRUNTIME_DIR}/graphics/gxcore/src/gxcore.cpp" _gcnllr_core)
set(_gcnllr_tex_rows "for (std::uint32_t k = 0; k < 3u; ++k)\n        resolved = load_matrix_row(draw, row + k, rows[k]) && resolved;")
string(FIND "${_gcnllr_core}" "${_gcnllr_tex_rows}" _gcnllr_tex_rows_offset)
if(_gcnllr_tex_rows_offset EQUAL -1)
  message(FATAL_ERROR "Pinned GX texture matrix adaptation differs")
endif()
string(REPLACE "${_gcnllr_tex_rows}"
  "identity_rows(rows);\n      const std::uint32_t required_rows = key.tex_gens[i].projection != 0u ? 3u : 2u;\n      for (std::uint32_t k = 0; k < required_rows; ++k)\n        resolved = load_matrix_row(draw, row + k, rows[k]) && resolved;"
  _gcnllr_core "${_gcnllr_core}")
# Direct-vertex BIOS quads may batch just like indexed triangles. The inner
# consumer still requires identical vertex layout and XF version, preserves
# every primitive's topology, and stops at resources, state and copy fences.
# Donor interpolation identity was the reason for the indexed-only restriction;
# this service presents actual VI frames with interpolation disabled.
set(_gcnllr_direct_fusion "!pending_state_.bp_valid(GxCoreState::kDrawTagRegister) && pending_state_.position_indexed())")
string(FIND "${_gcnllr_core}" "${_gcnllr_direct_fusion}" _gcnllr_direct_fusion_offset)
if(_gcnllr_direct_fusion_offset EQUAL -1)
  message(FATAL_ERROR "Pinned GX direct-primitive fusion adaptation differs")
endif()
string(REPLACE "${_gcnllr_direct_fusion}" "!pending_state_.bp_valid(GxCoreState::kDrawTagRegister))"
  _gcnllr_core "${_gcnllr_core}")
if(GCNLLR_RENDER_PIPELINE STREQUAL "packet")
  # Save the consumer's guest resource bindings along with the planner state.
  # Keep the original packet path and its validation for reopening bindings.
  set(_checkpoint_include "${CMAKE_BINARY_DIR}/gcnllr-services/adapted/include")
  set(_sink_header "${GXRUNTIME_DIR}/graphics/frontend/include/gxruntime/aurora_recomp/render_sink.hpp")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_sink_header}")
  file(READ "${_sink_header}" _sink_source)
  set(_sink_anchor "  const ConsumedTexture& bound_texture() const { return bound_texture_; }")
  string(FIND "${_sink_source}" "${_sink_anchor}" _sink_offset)
  if(_sink_offset EQUAL -1)
    message(FATAL_ERROR "Pinned GX consumer checkpoint adaptation differs")
  endif()
  string(REPLACE "${_sink_anchor}" "${_sink_anchor}\n  const ConsumedTexture& bound_texture_at(std::uint32_t slot) const { return bound_textures_[slot]; }\n  void start_packet_stream() { has_last_sequence_ = false; }"
    _sink_source "${_sink_source}")
  file(CONFIGURE OUTPUT "${_checkpoint_include}/gxruntime/aurora_recomp/render_sink.hpp"
    CONTENT "${_sink_source}" @ONLY)
  foreach(_target gxruntime_aurora_render_sink gxruntime_retail_gx_frontend gxruntime_gxcore)
    target_include_directories(${_target} BEFORE PUBLIC "$<BUILD_INTERFACE:${_checkpoint_include}>")
  endforeach()
  string(FIND "${_gcnllr_core}" "namespace {\nconstexpr std::uint32_t kSinkStateMagic" _checkpoint_begin)
  string(FIND "${_gcnllr_core}" "} // namespace gxruntime::gxcore" _checkpoint_end REVERSE)
  if(_checkpoint_begin EQUAL -1 OR _checkpoint_end LESS _checkpoint_begin)
    message(FATAL_ERROR "Pinned GX planner checkpoint adaptation differs")
  endif()
  string(SUBSTRING "${_gcnllr_core}" 0 ${_checkpoint_begin} _core_prefix)
  string(SUBSTRING "${_gcnllr_core}" ${_checkpoint_end} -1 _core_suffix)
  set(_gcnllr_core "${_core_prefix}#include \"${CMAKE_CURRENT_LIST_DIR}/gxcore_checkpoint.inc\"\n\n${_core_suffix}")
endif()
set(_gcnllr_core_path "${CMAKE_BINARY_DIR}/gcnllr-services/adapted/gxcore.cpp")
file(CONFIGURE OUTPUT "${_gcnllr_core_path}" CONTENT "${_gcnllr_core}" @ONLY)
get_target_property(_gcnllr_core_sources gxruntime_gxcore SOURCES)
list(REMOVE_ITEM _gcnllr_core_sources src/gxcore.cpp)
list(APPEND _gcnllr_core_sources "${_gcnllr_core_path}")
set_property(TARGET gxruntime_gxcore PROPERTY SOURCES "${_gcnllr_core_sources}")

# Keep partial copy clears inside the recorded GPU pass sequence. A display
# fence can then submit all BIOS bands together, rather than map/unmap the
# large staging arena and wait for the GPU at every individual band.
file(READ "${GXRUNTIME_DIR}/graphics/aurora/lib/gfx/common.cpp" _gcnllr_common)
if(_gcnllr_draw_packet_header)
  gcnllr_draw_packet_replace(_gcnllr_common "#include \"../gx/pipeline.hpp\""
    "#include \"${_gcnllr_draw_packet_header}\"")
  set(_gcnllr_storage_push "Range push_storage(const uint8_t* data, size_t length) {")
  set(_gcnllr_geometry_packet [=[u32 machine_geometry_packet(const u32* data, size_t words) {
  auto& storage = current_frame_packet().storage;
  const size_t begin = storage.size();
  const size_t aligned = AURORA_ALIGN(begin, sizeof(u32));
  if (aligned > begin) storage.append_zeroes(aligned - begin);
  storage.append(reinterpret_cast<const u8*>(data), words * sizeof(u32));
  return static_cast<u32>(aligned / sizeof(u32));
}

]=])
  gcnllr_draw_packet_replace(_gcnllr_common "${_gcnllr_storage_push}"
     "${_gcnllr_geometry_packet}${_gcnllr_storage_push}")
endif()
string(REPLACE "bool captureDepthSnapshot = false;"
  "bool captureDepthSnapshot = false;\n  wgpu::RenderPipeline machineClearPipeline;\n  ClipRect machineClearRect{};\n  std::vector<std::function<void(wgpu::CommandEncoder&)>> machineBeforeDraw;\n  wgpu::Color machineClearColor{};\n  float machineClearZ = 0;"
  _gcnllr_common "${_gcnllr_common}")
set(_gcnllr_pass_anchor "auto pass = cmd.BeginRenderPass(&renderPassDescriptor);\n  render_pass(pass, frame, passInfo, 0, split);")
string(FIND "${_gcnllr_common}" "${_gcnllr_pass_anchor}" _gcnllr_pass_offset)
if(_gcnllr_pass_offset EQUAL -1)
  message(FATAL_ERROR "Pinned recorded clear adaptation differs")
endif()
string(REPLACE "${_gcnllr_pass_anchor}" [=[auto pass = cmd.BeginRenderPass(&renderPassDescriptor);
  if (passInfo.machineClearPipeline) {
    const auto& r = passInfo.machineClearRect;
    pass.SetPipeline(passInfo.machineClearPipeline);
    pass.SetBlendConstant(&passInfo.machineClearColor);
    pass.SetViewport(0, 0, float(passInfo.targetSize.width), float(passInfo.targetSize.height),
                     passInfo.machineClearZ, passInfo.machineClearZ);
    if (r.width > 0 && r.height > 0) {
      pass.SetScissorRect(r.x, r.y, r.width, r.height);
      pass.Draw(3);
    }
  }
  render_pass(pass, frame, passInfo, 0, split);]=]
  _gcnllr_common "${_gcnllr_common}")
string(REPLACE "void queue_palette_conv(tex_palette_conv::ConvRequest req) {" [=[void machine_copy_clear(const wgpu::RenderPipeline& pipeline, ClipRect rect,
                        wgpu::Color color, float depth) {
  auto& pass = current_render_passes()[g_currentRenderPass];
  pass.machineClearPipeline = pipeline;
  pass.machineClearRect = rect;
  pass.machineClearColor = color;
  pass.machineClearZ = depth;
  pass.observable = true;
}
void queue_palette_conv(tex_palette_conv::ConvRequest req) {]=]
  _gcnllr_common "${_gcnllr_common}")
set(_gcnllr_texture_view_anchor "  for (const auto& conv : passInfo.paletteConvs) {")
string(FIND "${_gcnllr_common}" "${_gcnllr_texture_view_anchor}" _gcnllr_texture_view_offset)
if(_gcnllr_texture_view_offset EQUAL -1)
  message(FATAL_ERROR "Pinned pre-draw texture view boundary differs")
endif()
string(REPLACE "${_gcnllr_texture_view_anchor}"
  "  for (const auto& encode : passInfo.machineBeforeDraw) encode(cmd);\n${_gcnllr_texture_view_anchor}"
  _gcnllr_common "${_gcnllr_common}")
if(GCNLLR_GPU_FRAME_PACKETS)
  set(_gcnllr_uniform_push "Range push_uniform(const uint8_t* data, size_t length) {")
  string(FIND "${_gcnllr_common}" "${_gcnllr_uniform_push}" _gcnllr_uniform_at)
  if(_gcnllr_uniform_at EQUAL -1)
    message(FATAL_ERROR "Pinned uniform packet allocator differs")
  endif()
  string(REPLACE "${_gcnllr_uniform_push}" [=[ByteBuffer& machine_uniform_packet(size_t capacity, u32& offset) {
  auto& uniforms = current_frame_packet().uniforms;
  const size_t begin = uniforms.size();
  const size_t aligned = AURORA_ALIGN(begin, g_cachedLimits.minUniformBufferOffsetAlignment);
  if (aligned > begin) uniforms.append_zeroes(aligned - begin);
  uniforms.reserve_extra(capacity);
  offset = static_cast<u32>(aligned);
  return uniforms;
}
Range push_uniform(const uint8_t* data, size_t length) {]=]
    _gcnllr_common "${_gcnllr_common}")
  set(_gcnllr_bind_generation "static std::mutex g_bindGroupCacheMutex;")
  set(_gcnllr_bind_clear "g_cachedBindGroups.clear();")
  set(_gcnllr_bind_begin "BindGroupRef bind_group_ref(const WGPUBindGroupDescriptor& descriptor) {")
  set(_gcnllr_bind_end "  return id;\n}\n\nwgpu::BindGroup find_bind_group")
  foreach(_boundary IN ITEMS _gcnllr_bind_generation _gcnllr_bind_clear _gcnllr_bind_begin _gcnllr_bind_end)
    string(FIND "${_gcnllr_common}" "${${_boundary}}" _offset)
    if(_offset EQUAL -1)
      message(FATAL_ERROR "Pinned bind-group publication boundary differs: ${_boundary}")
    endif()
  endforeach()
  string(REPLACE "${_gcnllr_bind_generation}"
    "${_gcnllr_bind_generation}\nstatic std::atomic<uint64_t> g_machineBindGroupGeneration{1};"
    _gcnllr_common "${_gcnllr_common}")
  string(REPLACE "${_gcnllr_bind_clear}"
    "${_gcnllr_bind_clear}\n  g_machineBindGroupGeneration.fetch_add(1, std::memory_order_release);"
    _gcnllr_common "${_gcnllr_common}")
  string(REPLACE "${_gcnllr_bind_begin}" [=[BindGroupRef bind_group_ref(const WGPUBindGroupDescriptor& descriptor) {
  // The frame owner has already published this exact texture group in the
  // same recording packet. The shared cache retains its resource handles.
  struct ReadyTextureGroup {
    std::array<WGPUBindGroupEntry, gx::MaxTextures * 2> entries{};
    WGPUBindGroupLayout layout{};
    BindGroupRef id{};
    uint64_t packet{}, generation{};
  };
  static thread_local ReadyTextureGroup lastGroup;
  const auto packet = current_frame_id();
  const auto generation = g_machineBindGroupGeneration.load(std::memory_order_acquire);
  const bool comparable = packet && !descriptor.nextInChain && descriptor.entryCount == lastGroup.entries.size();
  const size_t bytes = sizeof(WGPUBindGroupEntry) * descriptor.entryCount;
  if (comparable && lastGroup.packet == packet && lastGroup.generation == generation &&
      lastGroup.layout == descriptor.layout && std::memcmp(lastGroup.entries.data(), descriptor.entries, bytes) == 0)
    return lastGroup.id;]=] _gcnllr_common "${_gcnllr_common}")
  string(REPLACE "${_gcnllr_bind_end}" [=[  if (comparable) {
    std::memcpy(lastGroup.entries.data(), descriptor.entries, bytes);
    lastGroup.layout = descriptor.layout;
    lastGroup.id = id;
    lastGroup.packet = packet;
    lastGroup.generation = generation;
  }
  return id;
}

wgpu::BindGroup find_bind_group]=] _gcnllr_common "${_gcnllr_common}")
endif()
set(_gcnllr_common_path "${CMAKE_BINARY_DIR}/gcnllr-services/adapted/common.cpp")
set(_gcnllr_encode_anchor "void queue_palette_conv(tex_palette_conv::ConvRequest req) {")
string(FIND "${_gcnllr_common}" "${_gcnllr_encode_anchor}" _gcnllr_encode_offset)
if(_gcnllr_encode_offset LESS 0)
  message(FATAL_ERROR "Pinned ordered native encoding boundary differs")
endif()
string(REPLACE "${_gcnllr_encode_anchor}"
  "#include \"${CMAKE_CURRENT_LIST_DIR}/ordered_encode.inc\"\n#if GCNLLR_PERSISTENT_GEOMETRY\n#include \"${CMAKE_CURRENT_LIST_DIR}/persistent_geometry.inc\"\n#endif\n${_gcnllr_encode_anchor}"
  _gcnllr_common "${_gcnllr_common}")
if(GCNLLR_PERSISTENT_GEOMETRY)
  string(PREPEND _gcnllr_common "#include \"${CMAKE_CURRENT_LIST_DIR}/persistent_geometry.hpp\"\n#include <stdexcept>\n")
  foreach(_geometry IN ITEMS Vertex Index Storage)
    string(REGEX MATCH "${_geometry}BufferSize,[ \t\r\n]+\"Shared ${_geometry} Buffer\""
      _gcnllr_buffer_anchor "${_gcnllr_common}")
    if(NOT _gcnllr_buffer_anchor)
      message(FATAL_ERROR "Pinned shared geometry buffer boundary differs")
    endif()
    if(_geometry STREQUAL "Vertex")
      set(_gcnllr_geometry_kind vertices)
    elseif(_geometry STREQUAL "Index")
      set(_gcnllr_geometry_kind indices)
    else()
      set(_gcnllr_geometry_kind arrays)
    endif()
    string(REPLACE "${_gcnllr_buffer_anchor}"
      "geometry_buffer_size(GeometryKind::${_gcnllr_geometry_kind}, ${_geometry}BufferSize), \"Shared ${_geometry} Buffer\""
      _gcnllr_common "${_gcnllr_common}")
  endforeach()
  string(REPLACE "bool begin_frame(bool preserveEfb) {"
    "bool begin_frame(bool preserveEfb) {\n  begin_geometry_frame(preserveEfb);"
    _gcnllr_common "${_gcnllr_common}")
  string(REPLACE "  g_vertexBuffer = {};"
    "  release_geometry();\n  g_vertexBuffer = {};"
    _gcnllr_common "${_gcnllr_common}")
endif()
set(_gcnllr_midpoint_anchor "  bool interpolate = false;\n  int interpSteps = 1;")
string(FIND "${_gcnllr_common}" "${_gcnllr_midpoint_anchor}" _gcnllr_midpoint_offset)
if(_gcnllr_midpoint_offset LESS 0)
  message(FATAL_ERROR "Pinned Aurora presentation frame packet differs")
endif()
string(REPLACE "${_gcnllr_midpoint_anchor}"
  "${_gcnllr_midpoint_anchor}\n  std::shared_ptr<const webgpu::TextureWithSampler> scanoutSource;\n  uint32_t scanoutTarget = 0;\n  uint32_t scanoutAspectWidth = 0;\n  uint32_t scanoutAspectHeight = 0;\n  uint64_t scanoutNativeNs = 0;"
  _gcnllr_common "${_gcnllr_common}")
foreach(_gcnllr_midpoint_boundary IN ITEMS
    "static bool note_game_frame_end();"
    "    const auto stats = packet.stats;"
    "      interpolate = packet.interpolate && callback && periodKnown;"
    "      hold = !interpolate && callback && periodKnown && frame_interp::enabled();"
    "    if (!clocked)\n      run_deferred_present();"
    "    } else if (callback) {\n      if (presents)\n        g_heldShown = false;\n      callback(encoder);"
    "  g_deferredPresents.clear();"
    "  int step = 0;           // the in-between frame's step, or -1 for the real frame"
    "  log_present(kind, present.gameFrame, present.step, present.due);"
    "void after_submit() noexcept {")
  string(FIND "${_gcnllr_common}" "${_gcnllr_midpoint_boundary}" _gcnllr_midpoint_offset)
  if(_gcnllr_midpoint_offset LESS 0)
    message(FATAL_ERROR "Pinned Aurora presentation boundary differs: ${_gcnllr_midpoint_boundary}")
  endif()
endforeach()
string(REPLACE "static bool note_game_frame_end();"
  "static bool note_game_frame_end();\nstatic bool present_interpolated_scanout(wgpu::CommandEncoder&, const EndFrameCallback&, const webgpu::TextureWithSampler&, uint64_t, uint32_t, uint32_t, uint32_t, uint64_t);\nstatic void release_machine_interpolation();"
  _gcnllr_common "${_gcnllr_common}")
string(REPLACE "    const auto stats = packet.stats;"
  "    const auto stats = packet.stats;\n    const auto scanout = std::move(packet.scanoutSource);\n    const auto scanoutTarget = packet.presents && scanout ? packet.scanoutTarget : 0u;\n    const auto scanoutAspectWidth = packet.scanoutAspectWidth;\n    const auto scanoutAspectHeight = packet.scanoutAspectHeight;\n    const auto scanoutNativeNs = packet.scanoutNativeNs;\n    const auto nativeFrameId = packet.frameId;"
  _gcnllr_common "${_gcnllr_common}")
string(REPLACE "      interpolate = packet.interpolate && callback && periodKnown;"
  "      interpolate = !scanout && packet.interpolate && callback && periodKnown;"
  _gcnllr_common "${_gcnllr_common}")
string(REPLACE "      hold = !interpolate && callback && periodKnown && frame_interp::enabled();"
  "      hold = !scanout && !interpolate && callback && periodKnown && frame_interp::enabled();"
  _gcnllr_common "${_gcnllr_common}")
string(REPLACE "    if (!clocked)\n      run_deferred_present();"
  "    const bool presentedScanout = scanoutTarget && callback &&\n        present_interpolated_scanout(encoder, callback, *scanout, nativeFrameId, scanoutTarget,\n                                     scanoutAspectWidth, scanoutAspectHeight, scanoutNativeNs);\n    // A render segment submitted for a copy or readback carries no new scanout.\n    if (presents && !clocked && !presentedScanout)\n      run_deferred_present();"
  _gcnllr_common "${_gcnllr_common}")
string(REPLACE "    } else if (callback) {\n      if (presents)\n        g_heldShown = false;\n      callback(encoder);"
  [=[    } else if (callback && !presentedScanout) {
      if (presents)
        g_heldShown = false;
      if (presents && scanout) {
        webgpu::set_fixed_present_aspect(scanoutAspectWidth, scanoutAspectHeight);
        webgpu::set_present_source_override(scanout.get());
      }
      callback(encoder);
      if (presents && scanout)
        webgpu::set_present_source_override(nullptr);]=] _gcnllr_common "${_gcnllr_common}")
string(REPLACE "  g_deferredPresents.clear();" "  g_deferredPresents.clear();\n  release_machine_interpolation();"
  _gcnllr_common "${_gcnllr_common}")
string(REPLACE "  int step = 0;           // the in-between frame's step, or -1 for the real frame"
  "  int step = 0;           // the in-between frame's step, or -1 for the real frame\n  PresentClock::time_point discardAfter{};\n  uint32_t scanoutAspectWidth = 0;\n  uint32_t scanoutAspectHeight = 0;\n  bool machineScanout = false;"
  _gcnllr_common "${_gcnllr_common}")
string(REPLACE "  log_present(kind, present.gameFrame, present.step, present.due);"
  [=[  if (present.discardAfter.time_since_epoch().count() != 0 && PresentClock::now() > present.discardAfter) {
    log_present("dropped-generated", present.gameFrame, present.step, present.due);
    return;
  }
  if (present.machineScanout)
    webgpu::set_fixed_present_aspect(present.scanoutAspectWidth, present.scanoutAspectHeight);
  log_present(kind, present.gameFrame, present.step, present.due);]=]
  _gcnllr_common "${_gcnllr_common}")
string(REPLACE "void after_submit() noexcept {"
  "#include \"${CMAKE_CURRENT_LIST_DIR}/scanout_interpolation.inc\"\n\nvoid after_submit() noexcept {"
  _gcnllr_common "${_gcnllr_common}")
gcnllr_trace_boundary(_gcnllr_common "  log_present(kind, present.gameFrame, present.step, present.due);"
  "  hitch::mark(hitch::Lane::render, hitch::Event::present_selected, present.gameFrame, present.step);\n  log_present(kind, present.gameFrame, present.step, present.due);")
gcnllr_trace_boundary(_gcnllr_common "  auto encoder = g_device.CreateCommandEncoder(&EncoderDescriptor);\n  const auto start = PresentClock::now();\n  present.callback(encoder);"
  "  hitch::mark(hitch::Lane::render, hitch::Event::encoder_begin, present.gameFrame);\n  auto encoder = g_device.CreateCommandEncoder(&EncoderDescriptor);\n  hitch::mark(hitch::Lane::render, hitch::Event::callback_begin, present.gameFrame);\n  const auto start = PresentClock::now();\n  present.callback(encoder);\n  hitch::mark(hitch::Lane::render, hitch::Event::callback_end, present.gameFrame);")
gcnllr_trace_boundary(_gcnllr_common "  const uint64_t present = g_shownFrames.fetch_add(1, std::memory_order_relaxed) + 1;"
  "  const uint64_t present = g_shownFrames.fetch_add(1, std::memory_order_relaxed) + 1;\n  hitch::mark(hitch::Lane::render, hitch::Event::present_complete, present);")
foreach(_gcnllr_present_message IN ITEMS present present-took encode-took display-timing)
  gcnllr_trace_boundary(_gcnllr_common "std::fprintf(stderr, \"[${_gcnllr_present_message}]"
    "hitch::record_text(\"[${_gcnllr_present_message}]")
endforeach()
# Drawing statistics already have their own explicit interpolation trace.
# Keep them in the optional presentation capture without synchronous logging.
gcnllr_trace_boundary(_gcnllr_common "  if (frame_interp::tracing() || g_presentLog ||"
  [=[  if (g_presentLog)
    hitch::record_text("[native-gx:aurora::gfx] In-between data for game frame %llu: %zu bytes of constants and %zu of vertices, %zu blocks did not fit; frame uniforms %zu bytes, vertices %zu, indices %zu, draws %zu (%zu merged)\n",
        static_cast<unsigned long long>(frame_interp::game_frame_number()), size_t(interpSlot.uniformUsed),
        size_t(interpSlot.vertexUsed), size_t(g_interpUniformOverflows), frame.uniforms.size(),
        frame.verts.size(), frame.indices.size(), size_t(g_drawCallCount), size_t(g_mergedDrawCallCount));
  if (frame_interp::tracing() ||]=])
string(PREPEND _gcnllr_common "#include \"${CMAKE_CURRENT_LIST_DIR}/present_trace.hpp\"\n#include <cstdint>\n#include <memory>\nnamespace hitch = gcnllr::present_trace;\nnamespace aurora::webgpu {\nvoid set_fixed_present_aspect(std::uint32_t width, std::uint32_t height) noexcept;\n}\n")
file(CONFIGURE OUTPUT "${_gcnllr_common_path}" CONTENT "${_gcnllr_common}" @ONLY)
get_target_property(_gcnllr_gx_sources aurora_gx SOURCES)
list(REMOVE_ITEM _gcnllr_gx_sources lib/gfx/common.cpp)
list(APPEND _gcnllr_gx_sources "${_gcnllr_common_path}")
set_property(TARGET aurora_gx PROPERTY SOURCES "${_gcnllr_gx_sources}")
set_source_files_properties("${_gcnllr_common_path}" TARGET_DIRECTORY aurora_gx
  PROPERTIES INCLUDE_DIRECTORIES "${GXRUNTIME_DIR}/graphics/aurora/lib/gfx")
