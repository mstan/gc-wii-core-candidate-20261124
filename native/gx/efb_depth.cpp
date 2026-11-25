// SPDX-License-Identifier: GPL-3.0-or-later
// Ordered depth readback, adapted from pinned RecompCore/Aurora depth_peek.cpp.
// Aurora portions: Copyright (c) 2022 Luke Street, MIT. The original notice
// and permission text are retained in third_party/aurora/LICENSE.
// This uses the same GX Z24 conversion but waits for the requested GPU work,
// rather than consuming an optional, wall-clock-throttled previous snapshot.
#include "gpu_bridge.hpp"
#include "gfx/common.hpp"
#include "gfx/render_worker.hpp"
#include "gx/gx.hpp"
#include "webgpu/gpu.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

namespace gcnllr::gpu_bridge {
namespace gpu=aurora::webgpu;
namespace worker=aurora::gfx::render_worker;
namespace {
std::array<wgpu::ComputePipeline,2> pipelines;
wgpu::Buffer storage,readback;
uint64_t capacity{};
#if !GCNLLR_RETAIL_PIPELINE && !GCNLLR_NATIVE_DRAW_SINK
wgpu::RenderPipeline restore_pipeline;
#endif
}
static std::vector<uint32_t> read_depth_words(uint32_t& width,uint32_t& height,bool native_words) {
    if (!aurora::gfx::segment_frame()) throw std::runtime_error("Depth EFB submission failed");
    const auto size=gpu::g_depthBuffer.size;
    const auto view=gpu::g_depthBuffer.view;
    width=size.width;height=size.height;
    if (!view || !width || !height) throw std::runtime_error("Depth EFB is unavailable");
    struct Result {std::vector<uint32_t> pixels;};
    auto result=std::make_shared<Result>();
    worker::enqueue_work([size,view,result,native_words] {
        auto& pipeline=pipelines[native_words];
        if (!pipeline) {
            std::string code="@group(0) @binding(0) var src: ";
            code+=gpu::g_graphicsConfig.msaaSamples>1 ? "texture_depth_multisampled_2d;" : "texture_depth_2d;";
            code+=R"(
@group(0) @binding(1) var<storage,read_write> dst: array<u32>;
@compute @workgroup_size(8,8) fn main(@builtin(global_invocation_id) p: vec3u) {
  let size=textureDimensions(src);
  if (p.x>=size.x || p.y>=size.y) { return; }
  let native=textureLoad(src,vec2i(p.xy),0);
)";
            if (native_words) {
                code+="dst[p.y*size.x+p.x]=bitcast<u32>(native);}";
            } else {
                code+=aurora::gx::UseReversedZ ? "let depth=1.0-native;" : "let depth=native;";
                code+="dst[p.y*size.x+p.x]=min(u32(clamp(depth,0.0,1.0)*16777215.0+0.5),16777215u);}";
            }
            wgpu::ShaderSourceWGSL source{};source.code=code.c_str();
            const wgpu::ShaderModuleDescriptor module_desc{.nextInChain=&source,.label="Ordered GameCube depth read"};
            const auto module=gpu::g_device.CreateShaderModule(&module_desc);
            const wgpu::ComputePipelineDescriptor desc{.label="Ordered GameCube depth read",
                .compute={.module=module,.entryPoint="main"}};
            pipeline=gpu::g_device.CreateComputePipeline(&desc);
        }
        const uint64_t bytes=uint64_t(size.width)*size.height*4;
        if (capacity<bytes) {
            wgpu::BufferDescriptor desc{.label="GameCube depth words",
                .usage=wgpu::BufferUsage::Storage|wgpu::BufferUsage::CopySrc,.size=bytes};
            storage=gpu::g_device.CreateBuffer(&desc);
            desc.usage=wgpu::BufferUsage::MapRead|wgpu::BufferUsage::CopyDst;
            readback=gpu::g_device.CreateBuffer(&desc);capacity=bytes;
        }
        const std::array<wgpu::BindGroupEntry,2> entries{{
            {.binding=0,.textureView=view},{.binding=1,.buffer=storage,.size=bytes}}};
        const wgpu::BindGroupDescriptor desc{.layout=pipeline.GetBindGroupLayout(0),
            .entryCount=entries.size(),.entries=entries.data()};
        const auto bind=gpu::g_device.CreateBindGroup(&desc);
        auto encoder=gpu::g_device.CreateCommandEncoder();
        auto pass=encoder.BeginComputePass();pass.SetPipeline(pipeline);pass.SetBindGroup(0,bind);
        pass.DispatchWorkgroups((size.width+7)/8,(size.height+7)/8);pass.End();
        encoder.CopyBufferToBuffer(storage,0,readback,0,bytes);
        const auto commands=encoder.Finish();gpu::g_queue.Submit(1,&commands);
        const auto buffer=readback;
        const auto future=buffer.MapAsync(wgpu::MapMode::Read,0,bytes,wgpu::CallbackMode::WaitAnyOnly,
            [buffer,bytes,result](wgpu::MapAsyncStatus status,wgpu::StringView) {
                if (status!=wgpu::MapAsyncStatus::Success) return;
                const auto* mapped=buffer.GetConstMappedRange(0,bytes);
                if (mapped) {result->pixels.resize(bytes/4);std::memcpy(result->pixels.data(),mapped,bytes);}
                buffer.Unmap();
            });
        gpu::g_instance.WaitAny(future,5000000000ull);
    });
    worker::synchronize();
    if (result->pixels.size()!=size_t(width)*height) throw std::runtime_error("Ordered depth EFB read failed");
    return std::move(result->pixels);
}
std::vector<uint32_t> read_depth(uint32_t& width,uint32_t& height) {
    return read_depth_words(width,height,false);
}

#if !GCNLLR_RETAIL_PIPELINE && !GCNLLR_NATIVE_DRAW_SINK
bool checkpoint_efb_supported(uint32_t* width,uint32_t* height) {
    const auto& color=gpu::g_frameBuffer;
    const auto& depth=gpu::g_depthBuffer;
    if (width) *width=color.size.width;
    if (height) *height=color.size.height;
    return gpu::g_graphicsConfig.msaaSamples==1 && color.texture && depth.view && color.size.width && color.size.height &&
        color.size.width==depth.size.width && color.size.height==depth.size.height &&
        (color.format==wgpu::TextureFormat::RGBA8Unorm || color.format==wgpu::TextureFormat::BGRA8Unorm) &&
        depth.format==wgpu::TextureFormat::Depth32Float;
}

EfbCheckpoint capture_efb() {
    if (!checkpoint_efb_supported()) throw std::runtime_error("Unsupported EFB checkpoint format");
    EfbCheckpoint result;
    result.rgba=read_efb(result.width,result.height);
    uint32_t depth_width,depth_height;
    result.native_depth=read_depth_words(depth_width,depth_height,true);
    if (result.width!=depth_width || result.height!=depth_height)
        throw std::runtime_error("EFB checkpoint attachments changed size");
    return result;
}

static wgpu::RenderPipeline make_restore_pipeline(wgpu::TextureFormat depth_format) {
    constexpr auto shader=R"(
struct DepthWords { width: u32, data: array<u32>, }
@group(0) @binding(0) var<storage, read> words: DepthWords;
@vertex fn vertex_main(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
    let xy = array<vec2f,3>(vec2f(-1.0,-1.0), vec2f(3.0,-1.0), vec2f(-1.0,3.0));
    return vec4f(xy[i],0.0,1.0);
}
@fragment fn fragment_main(@builtin(position) p: vec4f) -> @builtin(frag_depth) f32 {
    return bitcast<f32>(words.data[u32(p.y)*words.width+u32(p.x)]);
}
)";
    wgpu::ShaderSourceWGSL source{};
    source.code=shader;
    const wgpu::ShaderModuleDescriptor shader_desc{.nextInChain=&source,.label="GameCube checkpoint restore"};
    const auto module=gpu::g_device.CreateShaderModule(&shader_desc);
    const wgpu::DepthStencilState depth{.format=depth_format,.depthWriteEnabled=true,
                                       .depthCompare=wgpu::CompareFunction::Always};
    const wgpu::FragmentState fragment{.module=module,.entryPoint="fragment_main"};
    const wgpu::RenderPipelineDescriptor desc{.label="GameCube checkpoint restore",
        .vertex={.module=module,.entryPoint="vertex_main"},.depthStencil=&depth,.fragment=&fragment};
    return gpu::g_device.CreateRenderPipeline(&desc);
}

bool restore_efb(const EfbCheckpoint& image) {
    const auto color=gpu::g_frameBuffer;
    const auto depth=gpu::g_depthBuffer;
    const size_t pixels=size_t(color.size.width)*color.size.height;
    if (!checkpoint_efb_supported() || !pixels || image.width!=color.size.width || image.height!=color.size.height ||
        image.rgba.size()!=pixels*4 || image.native_depth.size()!=pixels ||
        !std::all_of(image.native_depth.begin(),image.native_depth.end(),[](uint32_t z) {
            return z<=0x3f800000u || z==0x80000000u;
        }) || !aurora::gfx::segment_frame()) return false;

    worker::enqueue_work([color,depth,&image] {
        if (!restore_pipeline) restore_pipeline=make_restore_pipeline(depth.format);
        const uint64_t depth_size=image.native_depth.size()*sizeof(uint32_t);
        const wgpu::BufferDescriptor upload{.label="GameCube checkpoint depth",
            .usage=wgpu::BufferUsage::Storage|wgpu::BufferUsage::CopyDst,.size=depth_size+4u};
        const auto buffer=gpu::g_device.CreateBuffer(&upload);
        gpu::g_queue.WriteBuffer(buffer,0,&image.width,sizeof(image.width));
        gpu::g_queue.WriteBuffer(buffer,4,image.native_depth.data(),depth_size);
        const wgpu::BindGroupEntry entry{.binding=0,.buffer=buffer,.size=depth_size+4u};
        const wgpu::BindGroupDescriptor bind_desc{.layout=restore_pipeline.GetBindGroupLayout(0),
                                                 .entryCount=1,.entries=&entry};
        const auto binding=gpu::g_device.CreateBindGroup(&bind_desc);
        const wgpu::RenderPassDepthStencilAttachment depth_attachment{.view=depth.view,
            .depthLoadOp=wgpu::LoadOp::Clear,.depthStoreOp=wgpu::StoreOp::Store,.depthClearValue=0.f};
        const wgpu::RenderPassDescriptor pass_desc{.label="GameCube checkpoint depth restore",
                                                   .depthStencilAttachment=&depth_attachment};
        auto encoder=gpu::g_device.CreateCommandEncoder();
        auto pass=encoder.BeginRenderPass(&pass_desc);
        pass.SetPipeline(restore_pipeline);
        pass.SetBindGroup(0,binding);
        pass.Draw(3);
        pass.End();
        const auto commands=encoder.Finish();

        const uint8_t* data=image.rgba.data();
        std::vector<uint8_t> bgra;
        if (color.format==wgpu::TextureFormat::BGRA8Unorm) {
            bgra=image.rgba;
            for (size_t p=0;p<bgra.size();p+=4) std::swap(bgra[p],bgra[p+2]);
            data=bgra.data();
        }
        const wgpu::TexelCopyTextureInfo target{.texture=color.texture};
        const wgpu::TexelCopyBufferLayout layout{.bytesPerRow=image.width*4,.rowsPerImage=image.height};
        gpu::g_queue.WriteTexture(&target,data,image.rgba.size(),&layout,&color.size);
        gpu::g_queue.Submit(1,&commands);
    });
    worker::synchronize();
    validate_submission();
    return true;
}
#endif

void release_depth() {
    pipelines={};storage={};readback={};capacity=0;
#if !GCNLLR_RETAIL_PIPELINE && !GCNLLR_NATIVE_DRAW_SINK
    restore_pipeline={};
#endif
}
}
