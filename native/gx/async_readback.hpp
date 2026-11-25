// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <memory>
#include <vector>

namespace wgpu { class Buffer; }
namespace aurora::gfx {
struct TextureRef;
struct AsyncReadbackResult;
using AsyncReadback = std::shared_ptr<AsyncReadbackResult>;

// Enqueue immutable sources in render-worker order. These functions neither
// publish guest memory nor signal a guest completion. The caller retains the
// ticket until an ordered owner-thread publication has consumed its bytes.
AsyncReadback queue_texture_readback(const std::shared_ptr<TextureRef>& texture);
AsyncReadback queue_buffer_readback(const wgpu::Buffer& source, uint64_t bytes);
// Completion of already-submitted GPU work, with no staging allocation,
// transfer or mapping. Consecutive requests share a queue completion event.
// This ticket contains no CPU bytes; validate it but never consume it.
AsyncReadback queue_gpu_completion();
// All WebGPU calls and WaitAnyOnly callbacks stay on the render worker.
// A nonblocking poll submits a batch and processes only completed mappings.
void poll_async_readbacks(bool wait);
bool async_readback_ready(const AsyncReadback& ticket);
void validate_async_readback(const AsyncReadback& ticket);
std::vector<uint8_t> take_async_readback(const AsyncReadback& ticket);
}
