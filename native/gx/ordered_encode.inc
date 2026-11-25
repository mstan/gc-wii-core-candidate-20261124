// SPDX-License-Identifier: MIT
// Included in Aurora's gfx namespace. The caller has just resolved/sealed the
// preceding pass. Record into the same packet before any following pass: no
// submission, staging unmap or frame-slot turnover is needed for this copy.
// Actual CPU/PE observations still submit the packet before waiting for bytes.
void machine_encode_after_resolve(std::function<void(wgpu::CommandEncoder&)> encode) {
  const size_t slot = g_recordingFrameSlot;
  render_worker::enqueue_encode_pass(current_frame_packet().frameId, 0,
      [slot, encode = std::move(encode)] { encode(g_framePackets[slot].encoder); });
}
// The following compute reads the EFB itself. Mark the preceding pass as
// observable before sealing it, so the renderer retains it without a crop
// target or an additional depth snapshot.
void machine_seal_display_source(ClipRect rect) {
  current_render_passes()[g_currentRenderPass].observable = true;
  resolve_pass({}, rect, false, false, false, {}, 0, GX_TF_RGBA8);
}
// A retained texture view reads an earlier resolve. Encode it in the same
// packet immediately before the pass that first samples the resulting view.
void machine_encode_before_draw(std::function<void(wgpu::CommandEncoder&)> encode) {
  auto& pass = current_render_passes()[g_currentRenderPass];
  ASSERT(!pass.sealed, "Attempted to append a texture view to sealed pass {}", g_currentRenderPass);
  pass.machineBeforeDraw.push_back(std::move(encode));
}
// Seal preceding EFB work, then append scanout composition and conversion to
// that packet. Frame end remains responsible for unmapping staging and submit.
void machine_encode_tail(std::function<void(wgpu::CommandEncoder&)> encode) {
  finish();
  machine_encode_after_resolve(std::move(encode));
}

void machine_present_next_scanout(std::shared_ptr<const webgpu::TextureWithSampler> source,
                                  uint32_t targetFps, uint32_t aspectWidth, uint32_t aspectHeight,
                                  uint64_t nativeNs) {
  auto& packet = current_frame_packet();
  packet.scanoutSource = std::move(source);
  packet.scanoutTarget = targetFps;
  packet.scanoutAspectWidth = aspectWidth;
  packet.scanoutAspectHeight = aspectHeight;
  packet.scanoutNativeNs = nativeNs;
}
