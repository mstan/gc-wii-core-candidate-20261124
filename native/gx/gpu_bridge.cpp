// SPDX-License-Identifier: MIT
#include "gpu_bridge.hpp"
#include "present_trace.hpp"
#if GCNLLR_GPU_OWNED_TEXTURES
#include "owned_texture.hpp"
#include "texture_encode.hpp"
#include "gfx/tex_palette_conv.hpp"
#include "gfx/texture_convert.hpp"
#endif
#include <gxruntime/gxcore/shader.hpp>
#include <aurora/aurora.h>
#include "gfx/common.hpp"
#include "gfx/texture.hpp"
#include "gfx/clear.hpp"
#include "gfx/gxcore_draw.hpp"
#include "gfx/pipeline_cache.hpp"
#include "gfx/render_worker.hpp"
#include "gfx/efb_readback.hpp"
#include "gfx/depth_peek.hpp"
#include "gfx/post_fx.hpp"
#include "gx/gx.hpp"
#include "imgui.hpp"
#include "window.hpp"
#include "webgpu/gpu.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <avrt.h>
#endif

namespace aurora::gfx {
void machine_copy_clear(const wgpu::RenderPipeline&,ClipRect,wgpu::Color,float);
void machine_encode_after_resolve(std::function<void(wgpu::CommandEncoder&)>);
void machine_seal_display_source(ClipRect);
void machine_encode_tail(std::function<void(wgpu::CommandEncoder&)>);
void machine_present_next_scanout(std::shared_ptr<const webgpu::TextureWithSampler>,
                                  uint32_t targetFps, uint32_t aspectWidth, uint32_t aspectHeight,
                                  uint64_t native_ns);
void machine_encode_before_draw(std::function<void(wgpu::CommandEncoder&)>);
}

namespace gcnllr::gpu_bridge {
namespace gfx=aurora::gfx;
namespace gpu=aurora::webgpu;
namespace worker=aurora::gfx::render_worker;
unsigned efb_scale(const char* setting) {
    if (!setting || !*setting) return 1;
    unsigned scale{};
    const char* end=setting+std::strlen(setting);
    const auto parsed=std::from_chars(setting,end,scale);
    wgpu::Limits limits{};
    if (!gpu::g_device.GetLimits(&limits))
        throw std::runtime_error("Unable to query GPU limits for the requested EFB size");
    if (parsed.ec!=std::errc{} || parsed.ptr!=end || !scale ||
        scale>limits.maxTextureDimension2D/640u)
        throw std::runtime_error("EFB scale must be a positive integer within the GPU texture size limit");
    return scale;
}
struct DisplayBand;
struct EncodedImage {
    uint32_t width{},height{};
    wgpu::Buffer buffer;
#if GCNLLR_GPU_OWNED_DISPLAY
    std::unique_ptr<const DisplayBand> color;
#endif
};
static gpu::TextureWithSampler scanout;
#if GCNLLR_QUEUED_PRESENT
// Aurora also borrows the override on the caller while preparing a viewport.
// Keep one stable object per geometry until a resize/shutdown drain, rather
// than letting a worker clear destroy it concurrently with that caller read.
static std::shared_ptr<gpu::TextureWithSampler> queued_scanout;
static std::shared_ptr<gpu::TextureWithSampler> local_scanout;
struct ScanoutView {
    uint32_t x{},y{},columns{1},rows{1},aspect_width{},aspect_height{};
    uint32_t trim_lines{};
};
static ScanoutView scanout_view;
static uint64_t scanout_native_ns{};
#endif
static std::array<wgpu::RenderPipeline,8> clear_pipelines;
static std::vector<uint8_t> pixels;
static wgpu::Buffer readback_buffer;
static uint64_t readback_capacity{};
static wgpu::Buffer display_encoded;
static uint64_t display_encoded_capacity{};
static wgpu::ComputePipeline display_encoder;
static wgpu::Buffer scanout_input;
static uint64_t scanout_input_capacity{};
static wgpu::ComputePipeline scanout_decoder;
#if GCNLLR_GPU_OWNED_DISPLAY
static wgpu::ComputePipeline scanout_color;
static std::vector<gfx::TextureHandle> display_surfaces;
static uint64_t color_copies{},color_scanouts{},color_observations{};
#endif
struct DisplayBand {
    gfx::TextureHandle texture;
    uint32_t width{},height{},first_row{},capture_rows{},scale{},source_x{},source_y{};
    uint32_t source_rows{};
    float yscale{1.f};
    std::array<uint32_t,3> weights{};
    EncodedImageHandle encoded;
    uint64_t offset{};
    bool direct{};
};
static uint64_t band_bytes(const DisplayBand& band) {
    return uint64_t(band.width) * band.height * 2;
}
static std::vector<DisplayBand> display_bands;
static bool recorded_copies{};
static uint64_t recorded_encodes{},observation_submissions{},publication_joins{};
static uint64_t scanout_frames{},scanout_joins{};
#if GCNLLR_DIRECT_DISPLAY_COPY
static uint64_t direct_display_bands{}, direct_display_batches{};
#endif
static std::mutex submission_mutex;
static std::exception_ptr submission_error;
void validate_submission() {
    std::lock_guard lock(submission_mutex);
    if(submission_error)std::rethrow_exception(submission_error);
}
void acknowledge_submission() {
    worker::synchronize();
    validate_submission();
}
void note_recorded_copy() {recorded_copies=true;}
void submit_copies() {
#if GCNLLR_BATCHED_SUBMISSION
    validate_submission();
    if(recorded_copies) {
        if(!gfx::segment_frame())throw std::runtime_error("Observed GPU packet submission failed");
        recorded_copies=false;++observation_submissions;
    }
#endif
}
static constexpr size_t display_param_words=GCNLLR_GPU_OWNED_DISPLAY ? 12 : 8;

static wgpu::Buffer display_parameters(const DisplayBand& band,
                                       [[maybe_unused]] std::array<uint32_t,2> range={}) {
    const std::array<uint32_t,display_param_words> values{band.weights[0],band.weights[1],band.weights[2],
        band.first_row | (band.capture_rows << 16),
        band.width,band.height,band.scale,band.source_x | (band.source_y << 16)
#if GCNLLR_GPU_OWNED_DISPLAY
        ,band.source_rows,std::bit_cast<uint32_t>(band.yscale),range[0],range[1]
#endif
    };
    const wgpu::BufferDescriptor descriptor{.label="Immutable GameCube copy filter",
        .usage=wgpu::BufferUsage::Uniform|wgpu::BufferUsage::CopyDst,.size=sizeof(values)};
    auto buffer=gpu::g_device.CreateBuffer(&descriptor);
    gpu::g_queue.WriteBuffer(buffer,0,values.data(),sizeof(values));
    return buffer;
}
static void reserve_display_storage(uint64_t bytes) {
    if (display_encoded && display_encoded_capacity >= bytes) return;
    const wgpu::BufferDescriptor descriptor{.label="GameCube encoded XFB bands",
        .usage=wgpu::BufferUsage::Storage|wgpu::BufferUsage::CopySrc,.size=bytes};
    display_encoded=gpu::g_device.CreateBuffer(&descriptor);
    display_encoded_capacity=bytes;
}
static void reserve_display_readback(uint64_t bytes) {
    if (readback_buffer && readback_capacity >= bytes) return;
    const wgpu::BufferDescriptor descriptor{.label="GameCube completed display copies",
        .usage=wgpu::BufferUsage::MapRead|wgpu::BufferUsage::CopyDst,.size=bytes};
    readback_buffer=gpu::g_device.CreateBuffer(&descriptor);
    readback_capacity=bytes;
}
static void encode_display_band(const wgpu::ComputePassEncoder& pass, const DisplayBand& band,
                                const wgpu::TextureView& source, const wgpu::Buffer& output,
                                uint64_t offset,uint32_t first_pair=0,uint32_t pair_count=0) {
    const auto parameters=display_parameters(band,{first_pair,pair_count});
    const std::array entries{
        wgpu::BindGroupEntry{.binding=0,.textureView=source},
        wgpu::BindGroupEntry{.binding=1,.buffer=output,.offset=offset,.size=pair_count?uint64_t(pair_count)*4:band_bytes(band)},
        wgpu::BindGroupEntry{.binding=2,.buffer=parameters,.size=display_param_words*sizeof(uint32_t)}};
    const wgpu::BindGroupDescriptor descriptor{.layout=display_encoder.GetBindGroupLayout(0),
        .entryCount=entries.size(),.entries=entries.data()};
    pass.SetBindGroup(0,gpu::g_device.CreateBindGroup(&descriptor));
    if(pair_count)pass.DispatchWorkgroups((pair_count+127)/128,1);
    else pass.DispatchWorkgroups((band.width/2+15)/16,(band.height+7)/8);
}
static size_t pending_displays{};
static constexpr char display_sampling_shader[]=R"(
@group(0) @binding(0) var src: texture_2d<f32>;
)"
#if GCNLLR_GPU_OWNED_DISPLAY
R"(
struct CopyParameters { weights: vec4<u32>, geometry: vec4<u32>, stretch: vec4<u32> };
)"
#else
R"(
struct CopyParameters { weights: vec4<u32>, geometry: vec4<u32> };
)"
#endif
R"(
@group(0) @binding(2) var<uniform> parameters: CopyParameters;
fn rgb(x: i32,y: i32) -> vec3<i32> {
    let scale=i32(parameters.geometry.z);
    let source=parameters.geometry.w;
    let height=i32(parameters.weights.w>>16u);
    let origin=(vec2<i32>(x,clamp(y,0,height-1))+
                vec2<i32>(i32(source&65535u),i32(source>>16u)))*scale;
    if (scale==1) {
        return vec3<i32>(textureLoad(src,origin,0).rgb*255.0+0.5);
    }
    var sum=vec3<f32>(0.0);
    for (var dy=0;dy<scale;dy++) {
        for (var dx=0;dx<scale;dx++) {
            sum+=textureLoad(src,origin+vec2<i32>(dx,dy),0).rgb;
        }
    }
    return vec3<i32>(sum*(255.0/f32(scale*scale))+0.5);
}
fn filtered(x: i32,y: i32) -> vec3<i32> {
)"
#if GCNLLR_GPU_OWNED_DISPLAY
R"(
    let source_y=min(i32(parameters.stretch.x)-1,i32(f32(y)/bitcast<f32>(parameters.stretch.y)+0.5));
)"
#else
R"(
    let source_y=y;
)"
#endif
R"(
    let row=source_y+i32(parameters.weights.w&65535u);
    let middle=rgb(x,row);
    let weights=parameters.weights.xyz;
    // The SDK's middle-only taps sum to64. Preserve the fast identity path;
    // every other set, including all-zero taps, follows integer arithmetic.
    if (all(weights==vec3<u32>(0u,64u,0u))) { return middle; }
    let sum=rgb(x,row-1)*i32(weights.x)+middle*i32(weights.y)+rgb(x,row+1)*i32(weights.z);
    return min(sum>>vec3<u32>(6u),vec3<i32>(255));
}
)";
// Same integer BT.601 conversion as the portable CPU copy. The storage word
// represents the four actual guest bytes for an observation of MEM1.
static constexpr char display_encode_shader[]=R"(
@group(0) @binding(1) var<storage, read_write> dst: array<u32>;
fn yuv(rgb: vec3<i32>) -> vec3<i32> {
    let y=((66*rgb.r+129*rgb.g+25*rgb.b+128)>>8u)+16;
    let u=((-38*rgb.r-74*rgb.g+112*rgb.b+128)>>8u)+128;
    let v=((112*rgb.r-94*rgb.g-18*rgb.b+128)>>8u)+128;
    return vec3<i32>(y,u,v);
}
@compute @workgroup_size(16,8) fn main(@builtin(global_invocation_id) id: vec3<u32>) {
    let size=parameters.geometry.xy;
    let cols=size.x/2u;
    var position=id.xy;
    var pair_index=id.y*cols+id.x;
)"
#if GCNLLR_GPU_OWNED_DISPLAY
R"(
    if (parameters.stretch.w!=0u) {
        pair_index=id.x*8u+id.y;
        if (pair_index>=parameters.stretch.w) { return; }
        let pair=parameters.stretch.z+pair_index;
        position=vec2<u32>(pair%cols,pair/cols);
    } else
)"
#endif
R"(
    if (id.x>=cols || id.y>=size.y) { return; }
    let a=yuv(filtered(i32(position.x*2u),i32(position.y)));
    let b=yuv(filtered(i32(position.x*2u+1u),i32(position.y)));
    let y0=u32(clamp(a.x,0,255));let y1=u32(clamp(b.x,0,255));
    let u=u32(clamp((a.y+b.y)/2,0,255));let v=u32(clamp((a.z+b.z)/2,0,255));
    dst[pair_index]=y0|(u<<8u)|(y1<<16u)|(v<<24u);
}
)";
#if GCNLLR_GPU_OWNED_DISPLAY
static constexpr char display_color_shader[]=R"(
@group(0) @binding(1) var dst: texture_storage_2d<rgba8unorm, write>;
fn host_rgb(x: i32, y: i32, capture_height: i32) -> vec3<i32> {
    return vec3<i32>(textureLoad(src, vec2<i32>(x,clamp(y,0,capture_height-1)), 0).rgb*255.0+0.5);
}
fn host_color(position: vec2<u32>) -> vec3<i32> {
    let scale=parameters.geometry.z;
    if (scale==1u) { return filtered(i32(position.x),i32(position.y)); }
    // Keep individual EFB samples and apply the display filter at the
    // finer texel spacing. The packed XFB encoder uses the guest spacing.
    let capture_height=i32((parameters.weights.w>>16u)*scale);
    let first_row=i32((parameters.weights.w&65535u)*scale);
    let last_source_row=i32(parameters.stretch.x*scale)-1;
    let source_row=min(last_source_row,i32((f32(position.y)+0.5)/bitcast<f32>(parameters.stretch.y)));
    let row=first_row+source_row;
    let x=i32(position.x);
    let middle=host_rgb(x,row,capture_height);
    let weights=parameters.weights.xyz;
    if (all(weights==vec3<u32>(0u,64u,0u))) { return middle; }
    let sum=host_rgb(x,row-1,capture_height)*i32(weights.x)+middle*i32(weights.y)+
            host_rgb(x,row+1,capture_height)*i32(weights.z);
    return min(sum>>vec3<u32>(6u),vec3<i32>(255));
}
@compute @workgroup_size(16,8) fn main(@builtin(global_invocation_id) id: vec3<u32>) {
    if (any(id.xy>=textureDimensions(dst))) { return; }
    let color=host_color(id.xy+parameters.stretch.zw);
    textureStore(dst,vec2<i32>(id.xy),vec4<f32>(vec3<f32>(color)/255.0,1.0));
}
)";
#endif
static constexpr char scanout_decode_shader[]=R"(
@group(0) @binding(0) var<storage, read> src: array<u32>;
@group(0) @binding(1) var dst: texture_storage_2d<rgba8unorm, write>;
@compute @workgroup_size(16,8) fn main(@builtin(global_invocation_id) id: vec3<u32>) {
    let size=textureDimensions(dst);
    if (id.x>=size.x/2u || id.y>=size.y) { return; }
    let pair=src[id.y*(size.x/2u)+id.x];
    let d=i32((pair>>8u)&255u)-128;let e=i32(pair>>24u)-128;
    let chroma=vec3<i32>(409*e+128,-100*d-208*e+128,516*d+128);
    for (var n=0u;n<2u;n++) {
        let c=298*(i32((pair>>(n*16u))&255u)-16);
        let rgb=clamp((vec3<i32>(c)+chroma)>>vec3<u32>(8u),vec3<i32>(0),vec3<i32>(255));
        textureStore(dst,vec2<i32>(i32(id.x*2u+n),i32(id.y)),vec4<f32>(vec3<f32>(rgb)/255.0,1.0));
    }
}
)";
#ifdef _WIN32
static HANDLE main_schedule{},render_schedule{};
#endif

#if GCNLLR_GPU_OWNED_TEXTURES
#include "texture_copy_view.inc"

gfx::TextureHandle colorize_ci8(const gfx::TextureHandle& source, const GXTlutObj_& tlut) {
    // CI8 addresses at most 256 entries. The software decoder emits
    // transparent black for indices outside a short TLUT.
    const auto entries = std::min<uint32_t>(tlut.numEntries, 256);
    auto palette = gfx::convert_tlut(gfx::tlut_texture_format(tlut.format), entries,
        {static_cast<const uint8_t*>(tlut.data), size_t(entries) * sizeof(uint16_t)});
    if (palette.data.size() != size_t(entries) * 4) {
        throw std::runtime_error("Retained CI8 palette decode failed");
    }
    palette.data.append_zeroes(size_t(256 - entries) * 4);
    auto table = gfx::new_static_texture_2d(256, 1, 1, GX_TF_RGBA8_PC,
        {palette.data.data(), palette.data.size()}, false, "Retained CI8 TLUT");
    auto result = gfx::new_conv_texture(source->size.width, source->size.height, GX_TF_RGBA8,
        "Retained CI8 texture");
    gfx::machine_encode_before_draw([
        request = gfx::tex_palette_conv::ConvRequest{
            gfx::tex_palette_conv::Variant::FromFloat8, source, result, std::move(table)}
    ](wgpu::CommandEncoder& encoder) {
        gfx::tex_palette_conv::run(encoder, request);
    });
    return result;
}
#endif
void initialize() {
#if GCNLLR_GPU_OWNED_TEXTURES
    texture_view_count=0;
    prepare_texture_view_pipeline();
#endif
    recorded_copies=false;recorded_encodes=observation_submissions=publication_joins=0;
    scanout_frames=scanout_joins=0;
#if GCNLLR_DIRECT_DISPLAY_COPY
    direct_display_bands=direct_display_batches=0;
#endif
    {std::lock_guard lock(submission_mutex);submission_error={};}
    const auto encode_source=std::string(display_sampling_shader)+display_encode_shader;
    wgpu::ShaderSourceWGSL source{};source.code=encode_source.c_str();
    const wgpu::ShaderModuleDescriptor module_descriptor{.nextInChain=&source,.label="GameCube exact YUY2 encode"};
    const auto module=gpu::g_device.CreateShaderModule(&module_descriptor);
    const wgpu::ComputePipelineDescriptor pipeline_descriptor{.label="GameCube exact YUY2 encode",
        .compute={.module=module,.entryPoint="main"}};
    display_encoder=gpu::g_device.CreateComputePipeline(&pipeline_descriptor);
    source.code=scanout_decode_shader;
    const auto decode_module=gpu::g_device.CreateShaderModule(&module_descriptor);
    const wgpu::ComputePipelineDescriptor decode_descriptor{.label="GameCube exact VI decode",
        .compute={.module=decode_module,.entryPoint="main"}};
    scanout_decoder=gpu::g_device.CreateComputePipeline(&decode_descriptor);
#if GCNLLR_GPU_OWNED_DISPLAY
    color_copies=color_scanouts=color_observations=0;
    const auto color_source=std::string(display_sampling_shader)+display_color_shader;
    source.code=color_source.c_str();
    const auto color_module=gpu::g_device.CreateShaderModule(&module_descriptor);
    const wgpu::ComputePipelineDescriptor color_descriptor{.label="GameCube retained display color",
        .compute={.module=color_module,.entryPoint="main"}};
    scanout_color=gpu::g_device.CreateComputePipeline(&color_descriptor);
#endif
#ifdef _WIN32
    // Real-time media work must not compete as a background batch job.
    // MMCSS scopes the scheduling request to the two deadline-bearing threads
    // and reverts it on those same threads during normal service shutdown.
    DWORD index{};main_schedule=AvSetMmThreadCharacteristicsW(L"Games",&index);
    worker::enqueue_work([] {DWORD index{};render_schedule=AvSetMmThreadCharacteristicsW(L"Games",&index);});
    worker::synchronize();
    std::fprintf(stderr,"[native-gx-scheduler] main=%s render=%s\n",
        main_schedule ? "MMCSS Games" : "normal",render_schedule ? "MMCSS Games" : "normal");
#endif
    // Compile the general fallback before any firmware time or audio starts.
    // Other fixed-function variants share these shaders and remain small;
    // first-use guest geometry must neither disappear nor pause boot for DXC.
    if (gfx::ubershader_mode()!=0) {
        gfx::gxcore::PipelineConfig config{};
        config.msaaSamples=gpu::g_graphicsConfig.msaaSamples;config.depthOnly=2;
        config.key.color_update=1;config.key.blend_enable=1;config.key.depth_func=3;
        config.key.src_factor=config.key.src_factor_alpha=4;
        config.key.dst_factor=config.key.dst_factor_alpha=5;
        const auto ref=gfx::pipeline_ref(config);
        wgpu::RenderPipeline pipeline;
        if (!gfx::get_pipeline(ref,pipeline) || !pipeline)
            throw std::runtime_error("Native GPU fallback pipeline did not compile");
    }
}
bool settle_framebuffer() {
    // Aurora's first begin binds the window-sized EFB before VI's pending
    // native framebuffer resize. Start a continuation on the settled target
    // before any retail copy or draw can reference the obsolete attachment.
    finish();
    if (!gfx::begin_frame(false)) return false;
    const auto size=gpu::g_frameBuffer.size;
    std::fprintf(stderr,"[native-gx] EFB render target %ux%u\n",size.width,size.height);
    return true;
}
#if GCNLLR_GPU_OWNED_DISPLAY
static gfx::TextureHandle acquire_display_surface(uint32_t width,uint32_t height) {
    for(const auto& surface:display_surfaces)
        if(surface.use_count()==1 && surface->size.width==width && surface->size.height==height)return surface;
    std::erase_if(display_surfaces,[](const gfx::TextureHandle& s){return s.use_count()==1;});
    auto surface=gfx::new_render_texture(width,height,GX_TF_RGBA8,"Immutable GameCube display source");
    display_surfaces.push_back(surface);
    return surface;
}
#endif
void record_display_copy(const gxruntime::gxcore::EfbCopyCommand& copy,const DisplayCopyFilter& filter,
                         [[maybe_unused]] bool eager,[[maybe_unused]] uint32_t output_rows,
                         [[maybe_unused]] float yscale) {
    if (!copy.width || !copy.height || copy.src_x+copy.width>640 || copy.src_y+copy.height>480)
        throw std::runtime_error("Display copy outside supported EFB");
    const auto tap=[](uint32_t word,unsigned index){return (word>>(index*6u))&63u;};
    const std::array<uint32_t,3> weights{tap(filter.low,0)+tap(filter.low,1),
        tap(filter.low,2)+tap(filter.low,3)+tap(filter.high,0),tap(filter.high,1)+tap(filter.high,2)};
    // Capture required neighbors now, before copy-clear or subsequent draws.
    // Cropped boundaries clamp to the crop only when the corresponding BP
    // bit requests it; physical EFB boundaries always clamp to the last row.
    const uint32_t above=weights[0] && !filter.clamp_top && copy.src_y ? 1u:0u;
    const uint32_t below=weights[2] && !filter.clamp_bottom && copy.src_y+copy.height<480 ? 1u:0u;
    const uint32_t capture_y=copy.src_y-above,capture_height=copy.height+above+below;
    if (pending_displays==display_bands.size()) display_bands.emplace_back();
    uint64_t offset=0;
    if (pending_displays) {
        const auto& previous=display_bands[pending_displays-1];
        offset=(previous.offset+band_bytes(previous)+255u)&~uint64_t(255u);
    }
    auto& band=display_bands[pending_displays++];
    band.width=copy.width;band.height=copy.height;band.first_row=above;band.weights=weights;
    band.source_rows=copy.height;band.yscale=1.f;
    const auto& fb=gpu::g_frameBuffer;
    const gfx::ClipRect capture_rect{int(copy.src_x*fb.size.width/640),int(capture_y*fb.size.height/480),
        int(copy.width*fb.size.width/640),int(capture_height*fb.size.height/480)};
    if (fb.size.width%640 || fb.size.height%480 || fb.size.width/640!=fb.size.height/480)
        throw std::runtime_error("Display copy requires an integer-scaled GameCube EFB");
    band.capture_rows=capture_height;band.scale=fb.size.width/640;
    band.source_x=band.source_y=0;band.offset=offset;band.direct=false;
#if GCNLLR_DIRECT_DISPLAY_COPY
    const auto& resolved=gpu::g_graphicsConfig.msaaSamples>1 ? gpu::g_frameBufferResolved : fb;
    const auto format=resolved.texture.GetFormat();
    // Shared with the machine's flush limit. Retain a stable slice for every
    // pending copy until the real guest-memory observation consumes the batch.
    constexpr uint64_t maximum_batch_bytes=16*640*480*2;
    band.direct=eager && !gfx::is_offscreen() && !gfx::post_fx::any_enabled() &&
        (format==wgpu::TextureFormat::RGBA8Unorm || format==wgpu::TextureFormat::BGRA8Unorm) &&
        offset+band_bytes(band)<=maximum_batch_bytes;
    if (band.direct) {
        ++direct_display_bands;
        reserve_display_storage(maximum_batch_bytes);
        band.source_x=copy.src_x;band.source_y=capture_y;
        // Seal and resolve the preceding EFB pass without allocating a crop
        // texture. This compute observes the source before any following clear.
        gfx::machine_seal_display_source(capture_rect);
        gfx::machine_encode_after_resolve([band,source=resolved.view,output=display_encoded](wgpu::CommandEncoder& encoder) {
            auto pass=encoder.BeginComputePass();
            pass.SetPipeline(display_encoder);
            encode_display_band(pass,band,source,output,band.offset);
            pass.End();
        });
    } else
#endif
    {
        const auto width=uint32_t(capture_rect.width),height=uint32_t(capture_rect.height);
#if GCNLLR_GPU_OWNED_DISPLAY
        if(!eager)band.texture=acquire_display_surface(width,height);
        else
#endif
        if (!band.texture || band.texture->size.width!=width || band.texture->size.height!=height)
            band.texture=gfx::new_render_texture(width,height,GX_TF_RGBA8,"GameCube display band and filter halo");
        gfx::resolve_pass(band.texture,capture_rect,false,false,false,{},0,GX_TF_RGBA8);
    }
    if (copy.clear && (copy.color_update || copy.alpha_update || copy.depth_update)) {
        const gfx::ClipRect clear_rect{int(copy.src_x*fb.size.width/640),int(copy.src_y*fb.size.height/480),
            int(copy.width*fb.size.width/640),int(copy.height*fb.size.height/480)};
        const unsigned key=unsigned(copy.color_update)|unsigned(copy.alpha_update)*2|unsigned(copy.depth_update)*4;
        auto& pipeline=clear_pipelines[key];
        if (!pipeline) pipeline=gfx::clear::create_pipeline({.msaaSamples=gpu::g_graphicsConfig.msaaSamples,
            .clearColor=copy.color_update,.clearAlpha=copy.alpha_update,.clearDepth=copy.depth_update});
        float z=copy.clear_z/16777215.f;if (aurora::gx::UseReversedZ) z=1-z;
        gfx::machine_copy_clear(pipeline,clear_rect,{copy.clear_r/255.,copy.clear_g/255.,copy.clear_b/255.,copy.clear_a/255.},z);
    }
    // The GPU snapshot and filter metadata are immutable before publication.
    // Encoding may happen later; it never samples the mutable EFB again.
#if GCNLLR_GPU_OWNED_DISPLAY
    if(!eager) {
        band.height=output_rows;band.yscale=yscale;
        auto image=std::make_shared<EncodedImage>();
        image->width=band.width;image->height=band.height;
        band.encoded.reset();
        image->color=std::make_unique<DisplayBand>(band);
        band.encoded=std::move(image);band.texture.reset();
        recorded_copies=true;++color_copies;
        return;
    }
#endif
#if GCNLLR_BATCHED_SUBMISSION
    auto image=std::make_shared<EncodedImage>();
    image->width=band.width;image->height=band.height;
    const uint64_t bytes=uint64_t(image->width)*image->height*2;
    const wgpu::BufferDescriptor descriptor{.label="Immutable GameCube XFB generation",
        .usage=wgpu::BufferUsage::Storage|wgpu::BufferUsage::CopySrc,.size=bytes};
    image->buffer=gpu::g_device.CreateBuffer(&descriptor);
    band.encoded=image;
    gfx::machine_encode_after_resolve([band,image,bytes](wgpu::CommandEncoder& encoder) {
      try {
        auto pass=encoder.BeginComputePass();pass.SetPipeline(display_encoder);
        encode_display_band(pass,band,band.texture->texture.CreateView(),image->buffer,0);
        pass.End();
      }catch(...) {std::lock_guard lock(submission_mutex);if(!submission_error)submission_error=std::current_exception();}
    });
    recorded_copies=true;++recorded_encodes;
#else
    if (!band.direct && !gfx::segment_frame()) throw std::runtime_error("Display band submission failed");
#endif
}

std::vector<CopiedImage> take_display_copies() {
    if (!pending_displays) return {};
#if GCNLLR_BATCHED_SUBMISSION
    auto encoded=retain_display_copies();std::vector<CopiedImage> images;
    images.reserve(encoded.size());
    for(const auto& image:encoded)images.push_back(materialize_display_copy(image));
    return images;
#endif
    std::vector<DisplayBand> bands(display_bands.begin(),display_bands.begin()+pending_displays);
    const uint64_t total=bands.back().offset+band_bytes(bands.back());
    const bool direct=std::all_of(bands.begin(),bands.end(),[](const DisplayBand& band){return band.direct;});
    if (direct) {
#if GCNLLR_DIRECT_DISPLAY_COPY
        ++direct_display_batches;
#endif
        reserve_display_readback(total);
        // The encodes have already been ordered at the corresponding EFB
        // boundaries. Copy their packed bytes in the same submission.
        gfx::machine_encode_after_resolve([source=display_encoded,target=readback_buffer,total](wgpu::CommandEncoder& encoder) {
            encoder.CopyBufferToBuffer(source,0,target,0,total);
        });
    }
    if (!gfx::segment_frame()) throw std::runtime_error("Display fence submission failed");
    auto result=std::make_shared<std::vector<CopiedImage>>();
    worker::enqueue_work([bands=std::move(bands),result,total,direct] {
        if (!direct) {
            reserve_display_readback(total);
            reserve_display_storage(total);
            auto encoder=gpu::g_device.CreateCommandEncoder();
            auto pass=encoder.BeginComputePass();pass.SetPipeline(display_encoder);
            for (const auto& band:bands) {
                if (!band.direct) encode_display_band(pass,band,band.texture->texture.CreateView(),display_encoded,band.offset);
            }
            pass.End();encoder.CopyBufferToBuffer(display_encoded,0,readback_buffer,0,total);
            const auto commands=encoder.Finish();gpu::g_queue.Submit(1,&commands);
        }
        auto buffer=readback_buffer;
        const auto future=buffer.MapAsync(wgpu::MapMode::Read,0,total,wgpu::CallbackMode::WaitAnyOnly,
            [buffer,total,bands,result](wgpu::MapAsyncStatus status,wgpu::StringView) {
                if (status!=wgpu::MapAsyncStatus::Success) return;
                const auto* mapped=static_cast<const uint8_t*>(buffer.GetConstMappedRange(0,total));
                if (mapped) for (const auto& band:bands) {
                    CopiedImage image{band.width,band.height,std::vector<uint8_t>(band_bytes(band))};
                    std::memcpy(image.yuy2.data(),mapped+band.offset,image.yuy2.size());
                    result->push_back(std::move(image));
                }
                buffer.Unmap();
            });
        gpu::g_instance.WaitAny(future,5000000000ull);
    });
    worker::synchronize();
    if (result->size()!=pending_displays) throw std::runtime_error("Display fence readback failed");
    pending_displays=0;
    return std::move(*result);
}

std::vector<EncodedImageHandle> retain_display_copies() {
    if(!pending_displays)return {};
#if GCNLLR_BATCHED_SUBMISSION
    validate_submission();std::vector<EncodedImageHandle> images;images.reserve(pending_displays);
    for(size_t i=0;i<pending_displays;i++)images.push_back(display_bands[i].encoded);
    pending_displays=0;return images;
#endif
    if(!gfx::segment_frame())throw std::runtime_error("Resident display submission failed");
    struct Result {std::vector<EncodedImageHandle> images;std::exception_ptr error;};
    auto result=std::make_shared<Result>();
    std::vector<DisplayBand> bands(display_bands.begin(),display_bands.begin()+pending_displays);
    worker::enqueue_work([bands=std::move(bands),result] {
      try {
        auto encoder=gpu::g_device.CreateCommandEncoder();
        auto pass=encoder.BeginComputePass();pass.SetPipeline(display_encoder);
        for(const auto& band:bands) {
            auto image=std::make_shared<EncodedImage>();
            image->width=band.width;image->height=band.height;
            const uint64_t bytes=uint64_t(image->width)*image->height*2;
            const wgpu::BufferDescriptor descriptor{.label="Immutable GameCube XFB generation",
                .usage=wgpu::BufferUsage::Storage|wgpu::BufferUsage::CopySrc,.size=bytes};
            image->buffer=gpu::g_device.CreateBuffer(&descriptor);
            encode_display_band(pass,band,band.texture->texture.CreateView(),image->buffer,0);
            result->images.push_back(std::move(image));
        }
        pass.End();const auto commands=encoder.Finish();gpu::g_queue.Submit(1,&commands);
      }catch(...) {result->error=std::current_exception();}
    });
    // This waits for command submission, not GPU completion or mapping. Each
    // buffer is distinct: a later copy cannot overwrite a retained generation.
    worker::synchronize();
    ++publication_joins;
    if(result->error)std::rethrow_exception(result->error);
    if(result->images.size()!=pending_displays)throw std::runtime_error("Resident display encoding failed");
    pending_displays=0;return std::move(result->images);
}

#if GCNLLR_ASYNC_COPIES
gfx::AsyncReadback queue_display_readback(const EncodedImageHandle& image) {
    submit_copies();
    if(!image || !image->buffer)throw std::runtime_error("Unknown streamed XFB generation");
    return gfx::queue_buffer_readback(image->buffer,uint64_t(image->width)*image->height*2u);
}
#endif
CopiedImage materialize_display_copy(const EncodedImageHandle& image,uint32_t offset,uint32_t size) {
    submit_copies();
    if(!image || (!image->buffer
#if GCNLLR_GPU_OWNED_DISPLAY
                 && !image->color
#endif
                 ))throw std::runtime_error("Unknown resident XFB generation");
#if GCNLLR_GPU_OWNED_DISPLAY
    if(image->color)++color_observations;
#endif
    const uint64_t total=uint64_t(image->width)*image->height*2;
    const uint64_t bytes=size?size:total;
    if(((offset|bytes)&3u) || offset>total || bytes>total-offset)
        throw std::runtime_error("Resident XFB byte range outside generation");
    struct Result {CopiedImage image;std::exception_ptr error;bool done{};};
    auto result=std::make_shared<Result>();
    worker::enqueue_work([image,result,offset,bytes,size] {
      try {
        if(!readback_buffer || readback_capacity<bytes) {
            const wgpu::BufferDescriptor descriptor{.label="Observed GameCube XFB bytes",
                .usage=wgpu::BufferUsage::MapRead|wgpu::BufferUsage::CopyDst,.size=bytes};
            readback_buffer=gpu::g_device.CreateBuffer(&descriptor);readback_capacity=bytes;
        }
        auto encoder=gpu::g_device.CreateCommandEncoder();
        auto source=image->buffer;
        uint32_t source_offset=offset;
#if GCNLLR_GPU_OWNED_DISPLAY
        if(image->color) {
            reserve_display_storage(bytes);
            source=display_encoded;
            auto pass=encoder.BeginComputePass();
            pass.SetPipeline(display_encoder);
            encode_display_band(pass,*image->color,image->color->texture->sampleTextureView,source,0,
                                offset/4,size/4);
            pass.End();source_offset=0;
        }
#endif
        encoder.CopyBufferToBuffer(source,source_offset,readback_buffer,0,bytes);
        const auto commands=encoder.Finish();gpu::g_queue.Submit(1,&commands);
        auto buffer=readback_buffer;
        const auto future=buffer.MapAsync(wgpu::MapMode::Read,0,bytes,wgpu::CallbackMode::WaitAnyOnly,
            [image,result,buffer,bytes](wgpu::MapAsyncStatus status,wgpu::StringView) {
                if(status==wgpu::MapAsyncStatus::Success) {
                    try {
                        const auto* mapped=static_cast<const uint8_t*>(buffer.GetConstMappedRange(0,bytes));
                        if(mapped) {
                            result->image.width=image->width;result->image.height=image->height;
                            result->image.yuy2.assign(mapped,mapped+bytes);
                        }
                    }catch(...) {result->error=std::current_exception();}
                    buffer.Unmap();
                }
                result->done=true;
            });
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        gpu::g_instance.WaitAny(future,5000000000ull);
        while(!result->done && std::chrono::steady_clock::now()<deadline) {
            gpu::g_instance.WaitAny(future,0);
            if(!result->done)std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        if(!result->done) {readback_buffer={};readback_capacity=0;}
      }catch(...) {result->error=std::current_exception();readback_buffer={};readback_capacity=0;}
    });
    worker::synchronize();
    validate_submission();
    if(result->error)std::rethrow_exception(result->error);
    if(!result->done || result->image.yuy2.size()!=bytes)
        throw std::runtime_error("Resident XFB materialization failed");
    return std::move(result->image);
}

std::vector<uint8_t> read_efb(uint32_t& width,uint32_t& height) {
    // End a recording segment without presenting or clearing the EFB.
    if (!gfx::segment_frame()) throw std::runtime_error("EFB submission failed");
    const auto& fb=gpu::g_graphicsConfig.msaaSamples>1 ? gpu::g_frameBufferResolved : gpu::g_frameBuffer;
    width=fb.size.width;height=fb.size.height;
    struct Result {std::vector<uint8_t> rgba;};
    auto result=std::make_shared<Result>();
    // This is a synchronous caller-visible copy, not an occasional screenshot.
    // Wait on the GPU completion event itself, without a timer/polling floor or
    // a new large staging allocation for every band of every BIOS frame.
    const auto texture=fb.texture;
    const auto size=fb.size;
    const bool bgra=fb.format==wgpu::TextureFormat::BGRA8Unorm;
    worker::enqueue_work([texture,size,bgra,result] {
        const uint32_t stride=(size.width*4+255)&~255u;
        const uint64_t bytes=uint64_t(stride)*size.height;
        if (!readback_buffer || readback_capacity<bytes) {
            const wgpu::BufferDescriptor descriptor{.label="GameCube EFB CPU copy",
                .usage=wgpu::BufferUsage::MapRead|wgpu::BufferUsage::CopyDst,.size=bytes};
            readback_buffer=gpu::g_device.CreateBuffer(&descriptor);readback_capacity=bytes;
        }
        auto encoder=gpu::g_device.CreateCommandEncoder();
        const wgpu::TexelCopyTextureInfo source{.texture=texture};
        const wgpu::TexelCopyBufferInfo destination{
            .layout={.bytesPerRow=stride,.rowsPerImage=size.height},.buffer=readback_buffer};
        encoder.CopyTextureToBuffer(&source,&destination,&size);
        const auto commands=encoder.Finish();gpu::g_queue.Submit(1,&commands);
        auto buffer=readback_buffer;
        const auto future=buffer.MapAsync(wgpu::MapMode::Read,0,bytes,wgpu::CallbackMode::WaitAnyOnly,
            [buffer,bytes,stride,size,bgra,result](wgpu::MapAsyncStatus status,wgpu::StringView) {
                if (status!=wgpu::MapAsyncStatus::Success) return;
                const auto* mapped=static_cast<const uint8_t*>(buffer.GetConstMappedRange(0,bytes));
                if (mapped) {
                    result->rgba.resize(size_t(size.width)*size.height*4);
                    for (uint32_t y=0;y<size.height;y++) {
                        auto* row=result->rgba.data()+size_t(y)*size.width*4;
                        std::memcpy(row,mapped+size_t(y)*stride,size.width*4);
                        if (bgra) for (uint32_t x=0;x<size.width;x++) std::swap(row[x*4],row[x*4+2]);
                    }
                }
                buffer.Unmap();
            });
        gpu::g_instance.WaitAny(future,5000000000ull);
    });
    worker::synchronize();
    auto rgba=std::move(result->rgba);
    if (!width || !height || rgba.size()!=size_t(width)*height*4)
        throw std::runtime_error("Actual GPU EFB readback failed");
    return rgba;
}
void clear_efb(const gxruntime::gxcore::EfbCopyCommand& copy) {
    if (!copy.clear || (!copy.color_update && !copy.alpha_update && !copy.depth_update)) return;
    worker::enqueue_work([copy] {
        const unsigned key=unsigned(copy.color_update)|unsigned(copy.alpha_update)*2|unsigned(copy.depth_update)*4;
        auto& pipeline=clear_pipelines[key];
        if (!pipeline) pipeline=gfx::clear::create_pipeline({.msaaSamples=gpu::g_graphicsConfig.msaaSamples,
            .clearColor=copy.color_update,.clearAlpha=copy.alpha_update,.clearDepth=copy.depth_update});
        const auto& fb=gpu::g_frameBuffer;
        wgpu::RenderPassColorAttachment color{};
        color.view=fb.view;color.loadOp=wgpu::LoadOp::Load;color.storeOp=wgpu::StoreOp::Store;
        if (gpu::g_graphicsConfig.msaaSamples>1) color.resolveTarget=gpu::g_frameBufferResolved.view;
        wgpu::RenderPassDepthStencilAttachment depth{};
        depth.view=gpu::g_depthBuffer.view;depth.depthLoadOp=wgpu::LoadOp::Load;depth.depthStoreOp=wgpu::StoreOp::Store;
        const wgpu::RenderPassDescriptor descriptor{.label="GameCube EFB copy clear",
            .colorAttachmentCount=1,.colorAttachments=&color,.depthStencilAttachment=&depth};
        auto encoder=gpu::g_device.CreateCommandEncoder();
        auto pass=encoder.BeginRenderPass(&descriptor);
        pass.SetPipeline(pipeline);
        const wgpu::Color clear{copy.clear_r/255.,copy.clear_g/255.,copy.clear_b/255.,copy.clear_a/255.};
        pass.SetBlendConstant(&clear);
        float z=copy.clear_z/16777215.f;if (aurora::gx::UseReversedZ) z=1-z;
        pass.SetViewport(0,0,float(fb.size.width),float(fb.size.height),z,z);
        const uint32_t left=std::min(fb.size.width,uint32_t(uint64_t(copy.src_x)*fb.size.width/640));
        const uint32_t top=std::min(fb.size.height,uint32_t(uint64_t(copy.src_y)*fb.size.height/480));
        const uint32_t right=std::min(fb.size.width,uint32_t(uint64_t(copy.src_x+copy.width)*fb.size.width/640));
        const uint32_t bottom=std::min(fb.size.height,uint32_t(uint64_t(copy.src_y+copy.height)*fb.size.height/480));
        if (right>left && bottom>top) {pass.SetScissorRect(left,top,right-left,bottom-top);pass.Draw(3);}
        pass.End();const auto commands=encoder.Finish();gpu::g_queue.Submit(1,&commands);
    });
}
static void prepare_scanout(uint32_t width,uint32_t height,uint32_t scale=1) {
    validate_submission();
#if !GCNLLR_QUEUED_PRESENT
    submit_copies();
#endif
    if (!width || !height || width>640 || height>576 || (width&1))
        throw std::runtime_error("Invalid VI scanout");
    width*=scale;
    height*=scale;
    // The previous present must be submitted before its texture is updated.
    // Queue ordering then protects the previous frame without a GPU idle wait.
#if !GCNLLR_QUEUED_PRESENT
    worker::synchronize();++scanout_joins;
#endif
    if (!scanout.texture || scanout.size.width!=width || scanout.size.height!=height) {
#if GCNLLR_QUEUED_PRESENT
        if(scanout.texture){worker::synchronize();++scanout_joins;}
#endif
        scanout.size={width,height,1};scanout.format=wgpu::TextureFormat::RGBA8Unorm;
        const wgpu::TextureDescriptor descriptor{.label="GameCube VI scanout",.usage=wgpu::TextureUsage::StorageBinding|
            wgpu::TextureUsage::TextureBinding|wgpu::TextureUsage::CopySrc,.size=scanout.size,.format=scanout.format};
        scanout.texture=gpu::g_device.CreateTexture(&descriptor);scanout.view=scanout.texture.CreateView();
        const wgpu::SamplerDescriptor sampler{.magFilter=wgpu::FilterMode::Linear,.minFilter=wgpu::FilterMode::Linear};
        scanout.sampler=gpu::g_device.CreateSampler(&sampler);
#if GCNLLR_QUEUED_PRESENT
        queued_scanout=std::make_shared<gpu::TextureWithSampler>(scanout);
#endif
    }
}
static void decode_scanout(wgpu::CommandEncoder& encoder,const wgpu::Buffer& buffer,
        const gpu::TextureWithSampler& target,uint32_t width,uint32_t height) {
    const uint64_t bytes=uint64_t(width)*height*2;
    const std::array entries{wgpu::BindGroupEntry{.binding=0,.buffer=buffer,.size=bytes},
        wgpu::BindGroupEntry{.binding=1,.textureView=target.view}};
    const wgpu::BindGroupDescriptor descriptor{.layout=scanout_decoder.GetBindGroupLayout(0),
        .entryCount=entries.size(),.entries=entries.data()};
    auto pass=encoder.BeginComputePass();
    pass.SetPipeline(scanout_decoder);pass.SetBindGroup(0,gpu::g_device.CreateBindGroup(&descriptor));
    pass.DispatchWorkgroups((width/2+15)/16,(height+7)/8);pass.End();
}
#if GCNLLR_QUEUED_PRESENT
static std::shared_ptr<gpu::TextureWithSampler> local_scanout_target(const wgpu::Extent3D& extent) {
    if(local_scanout && local_scanout->size.width==extent.width &&
       local_scanout->size.height==extent.height)return local_scanout;
    auto target=std::make_shared<gpu::TextureWithSampler>();
    target->size=extent;
    target->format=scanout.format;
    target->sampler=scanout.sampler;
    const wgpu::TextureDescriptor descriptor{.label="Local multiplayer scanout",
        .usage=wgpu::TextureUsage::CopySrc|wgpu::TextureUsage::CopyDst|
               wgpu::TextureUsage::TextureBinding|wgpu::TextureUsage::StorageBinding,
        .size=extent,.format=target->format};
    target->texture=gpu::g_device.CreateTexture(&descriptor);
    target->view=target->texture.CreateView();
    local_scanout=target;
    return target;
}

static bool queue_scanout_packet(
        std::function<void(wgpu::CommandEncoder&,const gpu::TextureWithSampler&,wgpu::Origin3D)> encode,
        uint32_t color_scale=0) {
    static const uint32_t interpolationTarget = [] {
        const char* setting = std::getenv("GCNLLR_FRAME_INTERPOLATION");
        if (!setting) return 0u;
        if (std::strcmp(setting,"1")==0) return 60u;
        uint32_t target{};
        const char* end=setting+std::strlen(setting);
        const auto parsed=std::from_chars(setting,end,target);
        return parsed.ec==std::errc{} && parsed.ptr==end && target>=31 &&
            target<=360 ? target : 0u;
    }();
    // Keep the exact texture generation with its render packet, even if the
    // caller changes geometry before the render worker reaches this frame.
    const auto target=queued_scanout;++scanout_frames;
    auto display=target;
    wgpu::Origin3D offset{};
    const auto [column,row,columns,rows,aspect_width,aspect_height,trim_lines]=scanout_view;
    const auto width=target->size.width,height=target->size.height;
    const auto scaled_trim=trim_lines*std::max(1u,color_scale);
    const auto trim=scaled_trim*2<height ? scaled_trim : 0u;
    if(columns>1 || rows>1 || trim) {
        const auto content_height=height-trim*2;
        offset={width*column/columns,trim+content_height*row/rows,0};
        display=local_scanout_target({width*(column+1)/columns-offset.x,
                                      trim+content_height*(row+1)/rows-offset.y,1});
    }
    gfx::machine_encode_tail([target,display,offset,color_scale,encode=std::move(encode)](wgpu::CommandEncoder& encoder) {
        try {
            if(color_scale) {
                encode(encoder,*display,offset);
                return;
            }
            encode(encoder,*target,{});
            if(display!=target) {
                const wgpu::TexelCopyTextureInfo from{.texture=target->texture,.origin=offset};
                const wgpu::TexelCopyTextureInfo to{.texture=display->texture};
                encoder.CopyTextureToTexture(&from,&to,&display->size);
            }
        }catch(...) {
            std::lock_guard lock(submission_mutex);
            if(!submission_error)submission_error=std::current_exception();
        }
    });
    const bool adapted_view = display != target;
    gfx::machine_present_next_scanout(std::move(display),interpolationTarget,
                                     adapted_view ? aspect_width : 0,adapted_view ? aspect_height : 0,
                                     scanout_native_ns);
    draw_mouse_settings();
    aurora_end_frame();
    present_trace::mark(present_trace::Lane::source,present_trace::Event::scanout_queued,scanout_frames);
    recorded_copies=false;
    aurora::imgui::new_frame(aurora::window::get_window_size());
    // Aurora's bounded frame packets provide backpressure. Real CPU/PE reads,
    // screenshot observations and shutdown still wait for their required data.
    const bool ready=gfx::begin_frame(true);
    present_trace::mark(present_trace::Lane::source,present_trace::Event::frame_slot_ready,scanout_frames);
    return ready;
}
static bool queue_scanout(const wgpu::Buffer& buffer,uint32_t width,uint32_t height,
        std::vector<DisplaySpan> spans={},std::vector<uint8_t> cpu={}) {
    return queue_scanout_packet([buffer,width,height,spans=std::move(spans),cpu=std::move(cpu)]
            (wgpu::CommandEncoder& encoder,const gpu::TextureWithSampler& target,wgpu::Origin3D) {
        // One worker orders uploads and submitted draws that share this input.
        if(!cpu.empty())gpu::g_queue.WriteBuffer(buffer,0,cpu.data(),cpu.size());
        for(const auto& span:spans)if(span.image)
            encoder.CopyBufferToBuffer(span.image->buffer,span.source,buffer,span.destination,span.size);
        decode_scanout(encoder,buffer,target,width,height);
    });
}
#endif

void set_scanout_view(uint32_t x,uint32_t y,uint32_t columns,uint32_t rows,
                      uint32_t aspect_width,uint32_t aspect_height,uint32_t trim_lines) {
#if GCNLLR_QUEUED_PRESENT
    if(columns && rows && x<columns && y<rows)
        scanout_view={x,y,columns,rows,aspect_width,aspect_height,trim_lines};
#else
    (void)x;(void)y;(void)columns;(void)rows;
    (void)aspect_width;(void)aspect_height;(void)trim_lines;
#endif
}
void set_scanout_timing(uint64_t native_ns) {
#if GCNLLR_QUEUED_PRESENT
    scanout_native_ns=native_ns;
#else
    (void)native_ns;
#endif
}
static bool show_encoded_buffer(const wgpu::Buffer& buffer,uint32_t width,uint32_t height) {
#if GCNLLR_QUEUED_PRESENT
    return queue_scanout(buffer,width,height);
#else
    ++scanout_frames;
    auto encoder=gpu::g_device.CreateCommandEncoder();
    decode_scanout(encoder,buffer,scanout,width,height);
    const auto commands=encoder.Finish();gpu::g_queue.Submit(1,&commands);
    worker::enqueue_work([] {gpu::set_present_source_override(&scanout);});
    draw_mouse_settings();
    aurora_end_frame();
    worker::enqueue_work([] {gpu::set_present_source_override(nullptr);});
    aurora::imgui::new_frame(aurora::window::get_window_size());
    const bool ok=gfx::begin_frame(true);
    worker::synchronize();++scanout_joins;
    return ok;
#endif
}
bool present_display_copy(const EncodedImageHandle& image,uint32_t width,uint32_t height) {
    if(!image || image->width!=width || image->height!=height)return false;
#if GCNLLR_GPU_OWNED_DISPLAY
    if(image->color) {
        prepare_scanout(width,height,image->color->scale);
        ++color_scanouts;
        return queue_scanout_packet([image](wgpu::CommandEncoder& encoder,const gpu::TextureWithSampler& target,
                                            wgpu::Origin3D offset) {
            const auto& band=*image->color;
            const auto parameters=display_parameters(band,{offset.x,offset.y});
            const std::array entries{
                wgpu::BindGroupEntry{.binding=0,.textureView=band.texture->sampleTextureView},
                wgpu::BindGroupEntry{.binding=1,.textureView=target.view},
                wgpu::BindGroupEntry{.binding=2,.buffer=parameters,.size=display_param_words*sizeof(uint32_t)}};
            const wgpu::BindGroupDescriptor descriptor{.layout=scanout_color.GetBindGroupLayout(0),
                .entryCount=entries.size(),.entries=entries.data()};
            auto pass=encoder.BeginComputePass();
            pass.SetPipeline(scanout_color);
            pass.SetBindGroup(0,gpu::g_device.CreateBindGroup(&descriptor));
            pass.DispatchWorkgroups((target.size.width+15)/16,(target.size.height+7)/8);
            pass.End();
        },image->color->scale);
    }
#endif
    prepare_scanout(width,height);
    return show_encoded_buffer(image->buffer,width,height);
}
bool present_display_spans(const std::vector<DisplaySpan>& spans,uint32_t width,uint32_t height) {
    const uint64_t bytes=uint64_t(width)*height*2;
    uint64_t end=0;
    for(const auto& span:spans) {
        if(!span.size ||
           ((span.source|span.destination|span.size)&3u) || span.destination!=end ||
           uint64_t(span.destination)+span.size>bytes)return false;
        if(span.image) {
            if(!span.image->buffer || uint64_t(span.source)+span.size>
                uint64_t(span.image->width)*span.image->height*2)return false;
        } else if(span.cpu.size()!=span.size)return false;
        end+=span.size;
    }
    if(end!=bytes)return false;
    prepare_scanout(width,height);
    if(!scanout_input || scanout_input_capacity<bytes) {
        const wgpu::BufferDescriptor descriptor{.label="GameCube native VI composition",
            .usage=wgpu::BufferUsage::Storage|wgpu::BufferUsage::CopyDst,.size=bytes};
        scanout_input=gpu::g_device.CreateBuffer(&descriptor);scanout_input_capacity=bytes;
    }
    // Immutable encoded generations stay on the GPU. Queue order preserves
    // their earlier rendering and the previous scanout without a RAM round trip.
    std::vector<uint8_t> cpu;
    if(std::any_of(spans.begin(),spans.end(),[](const DisplaySpan& s){return !s.image;})) {
        cpu.resize(bytes);
        for(const auto& span:spans)if(!span.image)
            std::memcpy(cpu.data()+span.destination,span.cpu.data(),span.size);
        // One upload for all CPU-owned regions; GPU regions overwrite the
        // zero-filled holes below. Never inspect GPU-owned guest RAM.
#if !GCNLLR_QUEUED_PRESENT
        gpu::g_queue.WriteBuffer(scanout_input,0,cpu.data(),bytes);
#endif
    }
#if GCNLLR_QUEUED_PRESENT
    return queue_scanout(scanout_input,width,height,spans,std::move(cpu));
#else
    auto encoder=gpu::g_device.CreateCommandEncoder();
    for(const auto& span:spans)if(span.image)
        encoder.CopyBufferToBuffer(span.image->buffer,span.source,scanout_input,span.destination,span.size);
    const auto commands=encoder.Finish();gpu::g_queue.Submit(1,&commands);
    return show_encoded_buffer(scanout_input,width,height);
#endif
}
bool present(const uint8_t* yuy2,uint32_t width,uint32_t height,uint32_t stride) {
    if(!yuy2 || stride<width*2)throw std::runtime_error("Invalid VI scanout bytes");
    prepare_scanout(width,height);
    const uint64_t bytes=uint64_t(width)*height*2;
    if (!scanout_input || scanout_input_capacity<bytes) {
        const wgpu::BufferDescriptor descriptor{.label="GameCube VI YUY2 bytes",
            .usage=wgpu::BufferUsage::Storage|wgpu::BufferUsage::CopyDst,.size=bytes};
        scanout_input=gpu::g_device.CreateBuffer(&descriptor);scanout_input_capacity=bytes;
    }
    if (stride!=width*2) {
        pixels.resize(bytes);
        for (uint32_t y=0;y<height;y++) std::memcpy(pixels.data()+size_t(y)*width*2,yuy2+size_t(y)*stride,width*2);
        yuy2=pixels.data();
    }
#if GCNLLR_QUEUED_PRESENT
    return queue_scanout(scanout_input,width,height,{},std::vector<uint8_t>(yuy2,yuy2+bytes));
#else
    gpu::g_queue.WriteBuffer(scanout_input,0,yuy2,bytes);
    return show_encoded_buffer(scanout_input,width,height);
#endif
}
void finish() {
    gfx::finish();
    gfx::end_frame([](wgpu::CommandEncoder& encoder) {
        const auto commands=encoder.Finish();gpu::g_queue.Submit(1,&commands);gfx::after_submit();
    });
    worker::synchronize();
    recorded_copies=false;
    validate_submission();
}
bool peek_scanout(uint16_t x,uint16_t y,uint32_t* value) {
    if (!value || !scanout.texture || x>=scanout.size.width || y>=scanout.size.height) return false;
    worker::synchronize();
    validate_submission();
    const auto rgba=gfx::read_texture_rgba8(scanout.texture,scanout.size,scanout.format);
    if (rgba.size()!=size_t(scanout.size.width)*scanout.size.height*4) return false;
    const auto* p=rgba.data()+(size_t(y)*scanout.size.width+x)*4;
    *value=(uint32_t(p[3])<<24)|(uint32_t(p[0])<<16)|(uint32_t(p[1])<<8)|p[2];
    return true;
}
void release() {
#if GCNLLR_GPU_OWNED_TEXTURES
    std::fprintf(stderr,"[native-gpu-texture-views] reinterpreted=%llu\n",(unsigned long long)texture_view_count);
    texture_view_pipeline={};
#endif
#if GCNLLR_GPU_OWNED_DISPLAY
    std::fprintf(stderr,"[native-gpu-display] copies=%llu scanouts=%llu byte-observations=%llu pooled=%zu\n",
        (unsigned long long)color_copies,(unsigned long long)color_scanouts,
        (unsigned long long)color_observations,display_surfaces.size());
#endif
#if GCNLLR_DIRECT_DISPLAY_COPY
    std::fprintf(stderr,"[native-direct-display] encoded=%llu combined_submissions=%llu\n",
        (unsigned long long)direct_display_bands,(unsigned long long)direct_display_batches);
#endif
    std::fprintf(stderr,"[native-vi-submission] queued=%u frames=%llu worker_joins=%llu\n",
        unsigned(GCNLLR_QUEUED_PRESENT),(unsigned long long)scanout_frames,(unsigned long long)scanout_joins);
    std::fprintf(stderr,"[native-gx-submission] batched=%u encodes=%llu observation_submissions=%llu publication_joins=%llu\n",
        unsigned(GCNLLR_BATCHED_SUBMISSION),(unsigned long long)recorded_encodes,
        (unsigned long long)observation_submissions,(unsigned long long)publication_joins);
    worker::synchronize();release_depth();
    worker::synchronize();scanout={};clear_pipelines={};pixels.clear();readback_buffer={};readback_capacity=0;
#if GCNLLR_QUEUED_PRESENT
    queued_scanout.reset();
    local_scanout.reset();
    scanout_view={};
    scanout_native_ns=0;
#endif
    display_encoded={};display_encoded_capacity=0;display_encoder={};
    scanout_input={};scanout_input_capacity=0;scanout_decoder={};
    display_bands.clear();pending_displays=0;
#if GCNLLR_GPU_OWNED_DISPLAY
    scanout_color={};display_surfaces.clear();
#endif
#ifdef _WIN32
    worker::enqueue_work([] {if (render_schedule) {AvRevertMmThreadCharacteristics(render_schedule);render_schedule=nullptr;}});
    worker::synchronize();
    if (main_schedule) {AvRevertMmThreadCharacteristics(main_schedule);main_schedule=nullptr;}
#endif
}
}
