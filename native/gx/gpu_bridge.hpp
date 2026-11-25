// SPDX-License-Identifier: MIT
#pragma once

namespace gcnllr { void draw_mouse_settings(); }
#include <cstdint>
#include <memory>
#include <vector>
#if GCNLLR_ASYNC_COPIES
#include "async_readback.hpp"
#endif
namespace gxruntime::gxcore { struct EfbCopyCommand; }
namespace aurora::gfx { struct TextureRef; namespace gxcore {struct OwnedTexture;} }
struct GXTlutObj_;
namespace gcnllr::gpu_bridge {
void set_scanout_view(uint32_t x,uint32_t y,uint32_t columns,uint32_t rows,
                      uint32_t aspect_width,uint32_t aspect_height,uint32_t trim_lines);
void set_scanout_timing(uint64_t native_ns);
unsigned efb_scale(const char* setting);
void initialize();
bool settle_framebuffer();
struct CopiedImage {uint32_t width{},height{};std::vector<uint8_t> yuy2;};
struct EncodedImage;
using EncodedImageHandle=std::shared_ptr<const EncodedImage>;
struct DisplayCopyFilter {
    uint32_t low{},high{};
    bool clamp_top{},clamp_bottom{};
};
void record_display_copy(const gxruntime::gxcore::EfbCopyCommand& copy,const DisplayCopyFilter& filter,
                         bool eager,uint32_t output_rows,float yscale);
std::vector<CopiedImage> take_display_copies();
// Immutable display generations. Observation submits the source packet before
// consuming real guest bytes.
std::vector<EncodedImageHandle> retain_display_copies();
// Submit any lazily recorded generations before CPU observation/PE completion.
// Segmented providers already submitted them; no live provider switch exists.
void note_recorded_copy();
void submit_copies();
void validate_submission();
// Await host encoding/submission acknowledgement, without GPU execution,
// transfer or map completion. Propagate deferred encoding errors before PE.
void acknowledge_submission();
CopiedImage materialize_display_copy(const EncodedImageHandle&,uint32_t offset=0,uint32_t size=0);
#if GCNLLR_ASYNC_COPIES
aurora::gfx::AsyncReadback queue_display_readback(const EncodedImageHandle&);
#endif
bool present_display_copy(const EncodedImageHandle&,uint32_t width,uint32_t height);
struct DisplaySpan {EncodedImageHandle image;uint32_t source,destination,size;std::vector<uint8_t> cpu;};
bool present_display_spans(const std::vector<DisplaySpan>&,uint32_t width,uint32_t height);
std::vector<uint8_t> read_efb(uint32_t& width,uint32_t& height);
std::vector<uint32_t> read_depth(uint32_t& width,uint32_t& height);
#if !GCNLLR_RETAIL_PIPELINE && !GCNLLR_NATIVE_DRAW_SINK
struct EfbCheckpoint {
    uint32_t width{},height{};
    std::vector<uint8_t> rgba;
    std::vector<uint32_t> native_depth;
};
bool checkpoint_efb_supported(uint32_t* width=nullptr,uint32_t* height=nullptr);
EfbCheckpoint capture_efb();
bool restore_efb(const EfbCheckpoint&);
#endif
void release_depth();
void clear_efb(const gxruntime::gxcore::EfbCopyCommand& copy);
bool present(const uint8_t* yuy2,uint32_t width,uint32_t height,uint32_t stride);
bool peek_scanout(uint16_t x,uint16_t y,uint32_t* value);
void finish();
void release();
#if GCNLLR_GPU_OWNED_TEXTURES
std::shared_ptr<aurora::gfx::TextureRef> reinterpret_texture(const aurora::gfx::gxcore::OwnedTexture&,
                                                           uint32_t format,uint32_t width,uint32_t height);
std::shared_ptr<aurora::gfx::TextureRef> colorize_ci8(const std::shared_ptr<aurora::gfx::TextureRef>&,
                                                    const GXTlutObj_&);
#endif
}
