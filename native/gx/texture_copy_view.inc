// SPDX-License-Identifier: MIT
// A guest texture view addresses the same tiled bytes as its copy generation.
// Recover only the bytes this sample needs, from the immutable GPU texture.
// No intermediate guest byte array is produced or transferred to the host.
static constexpr char texture_view_shader[]=R"(
struct CopyView {
    source: vec4<u32>,
    destination: vec4<u32>,
    tile: vec4<u32>,
    scale: vec4<u32>,
}
@group(0) @binding(0) var input_texture: texture_2d<f32>;
@group(0) @binding(1) var output_texture: texture_storage_2d<rgba8unorm,write>;
@group(0) @binding(2) var<uniform> view: CopyView;

fn source_color(block:u32,pixel:u32,subpixel:vec2<u32>) -> vec4<u32> {
    let block_coordinate=vec2<u32>(block%view.source.z,block/view.source.z)*view.tile.xy;
    let within_block=vec2<u32>(pixel%view.tile.x,pixel/view.tile.x);
    let point=min(block_coordinate+within_block,view.source.xy-vec2<u32>(1u));
    return vec4<u32>(textureLoad(input_texture,vec2<i32>(point*view.scale.xy+subpixel),0)*255.0+0.5);
}

fn copy_byte(address:u32,subpixel:vec2<u32>) -> u32 {
    let block=address/view.tile.z;
    let byte=address%view.tile.z;
    if (view.tile.y==8u) {
        let hi=source_color(block,byte*2u,subpixel).r & 240u;
        let lo=source_color(block,byte*2u+1u,subpixel).r >> 4u;
        return hi|lo;
    }
    if (view.tile.x==8u) {
        let color=source_color(block,byte,subpixel);
        if (view.source.w==2u || view.source.w==34u) { return (color.a&240u)|(color.r>>4u); }
        return color.r;
    }
    let color=source_color(block,(byte%32u)/2u,subpixel);
    if (view.tile.z==64u) {
        if (byte<32u) { return select(color.a,color.r,(byte&1u)!=0u); }
        return select(color.g,color.b,(byte&1u)!=0u);
    }
    if (view.source.w==4u) {
        let word=((color.r>>3u)<<11u)|((color.g>>2u)<<5u)|(color.b>>3u);
        return select(word>>8u,word&255u,(byte&1u)!=0u);
    }
    if (view.source.w==5u) {
        let rgb4=((color.a>>5u)<<12u)|((color.r>>4u)<<8u)|((color.g>>4u)<<4u)|(color.b>>4u);
        let rgb5=32768u|((color.r>>3u)<<10u)|((color.g>>3u)<<5u)|(color.b>>3u);
        let word=select(rgb4,rgb5,color.a>=224u);
        return select(word>>8u,word&255u,(byte&1u)!=0u);
    }
    return select(color.a,color.r,(byte&1u)!=0u);
}

@compute @workgroup_size(8,8) fn main(@builtin(global_invocation_id) id:vec3<u32>) {
    if (view.destination.w==1u) {
        if (any(id.xy>=view.destination.xy)) { return; }
        // Match encode_native_copy's representative source pixel before
        // normal GX filtering on the logical-sized I8 image.
        let center=((2u*id.xy+vec2<u32>(1u))*textureDimensions(input_texture))/(2u*view.source.xy);
        let intensity=textureLoad(input_texture,vec2<i32>(center),0).r;
        textureStore(output_texture,vec2<i32>(id.xy),vec4<f32>(intensity));
        return;
    }
    if (view.destination.w==4u) {
        if (any(id.xy>=view.destination.xy)) { return; }
        // The width exposed by the CPU decoder includes the padded column.
        // Match its native-sized RGB565 image and its exact center sample.
        let point=min(id.xy,view.source.xy-vec2<u32>(1u));
        let center=point*view.scale.xy+view.scale.xy/2u;
        let color=vec3<u32>(textureLoad(input_texture,vec2<i32>(center),0).rgb*255.0+0.5);
        let quantized=color>>vec3<u32>(3u,2u,3u);
        let expanded=(quantized<<vec3<u32>(3u,2u,3u))|(quantized>>vec3<u32>(2u,4u,2u));
        textureStore(output_texture,vec2<i32>(id.xy),vec4<f32>(vec3<f32>(expanded)/255.0,1.0));
        return;
    }
    if (any(id.xy>=view.destination.xy*view.scale.xy)) { return; }
    let texel=id.xy/view.scale.xy;
    let subpixel=id.xy%view.scale.xy;
    var intensity:u32;
    var alpha:u32;
    if (view.destination.w==0u) {
        let block=(texel.y/8u)*view.destination.z+texel.x/8u;
        let pixel=(texel.y%8u)*8u+texel.x%8u;
        let packed=copy_byte(block*32u+pixel/2u,subpixel);
        intensity=17u*select(packed>>4u,packed&15u,(pixel&1u)!=0u);
        alpha=intensity;
    } else {
        let block=(texel.y/4u)*view.destination.z+texel.x/4u;
        let pixel=(texel.y%4u)*4u+texel.x%4u;
        let address=block*32u+pixel*2u;
        alpha=copy_byte(address,subpixel);
        intensity=copy_byte(address+1u,subpixel);
    }
    textureStore(output_texture,vec2<i32>(id.xy),vec4<f32>(vec4<u32>(intensity,intensity,intensity,alpha))/255.0);
}
)";
static wgpu::ComputePipeline texture_view_pipeline;
static uint64_t texture_view_count;

static void prepare_texture_view_pipeline() {
    wgpu::ShaderSourceWGSL wgsl{};
    wgsl.code=texture_view_shader;
    const wgpu::ShaderModuleDescriptor shader{.nextInChain=&wgsl,.label="GX tiled texture sampling view"};
    const auto module=gpu::g_device.CreateShaderModule(&shader);
    const wgpu::ComputePipelineDescriptor pipeline{.label="GX tiled texture sampling view",
        .compute={.module=module,.entryPoint="main"}};
    texture_view_pipeline=gpu::g_device.CreateComputePipeline(&pipeline);
}

gfx::TextureHandle reinterpret_texture(const gfx::gxcore::OwnedTexture& copy,
                                       uint32_t format,uint32_t width,uint32_t height) {
    const auto& source=copy.texture;
    const bool padded_rgb565=format==4u && copy.format==format && (copy.width&1u) &&
        width==copy.width+1u && height==copy.height;
    const bool logical_i8=format==1u && width==copy.width && height==copy.height;
    const bool native_size=padded_rgb565 || logical_i8;
    if(!source || !copy.width || !copy.height || (!native_size && format!=0u && format!=3u))return {};
    if(!logical_i8 && (source->size.width%copy.width || source->size.height%copy.height))return {};
    const auto src_tile=native_copy_layout(copy.format),dst_tile=native_copy_layout(format);
    if(!src_tile.bytes)return {};
    const auto tiles=[](uint32_t n,uint32_t tile){return (n+tile-1u)/tile;};
    const uint32_t scale_x=source->size.width/copy.width,scale_y=source->size.height/copy.height;
    const uint32_t view_scale_x=native_size?1u:scale_x,view_scale_y=native_size?1u:scale_y;
    const std::array<uint32_t,16> values{copy.width,copy.height,tiles(copy.width,src_tile.width),copy.format,
        width,height,tiles(width,dst_tile.width),format,
        src_tile.width,src_tile.height,src_tile.bytes,0,
        scale_x,scale_y,0,0};
    wgpu::Limits limits{};
    if((!logical_i8 && (!scale_x || !scale_y)) || !gpu::g_device.GetLimits(&limits) ||
       width>limits.maxTextureDimension2D/view_scale_x || height>limits.maxTextureDimension2D/view_scale_y)return {};
    const wgpu::Extent3D size{width*view_scale_x,height*view_scale_y,1};
    constexpr auto storage_format=wgpu::TextureFormat::RGBA8Unorm;
    const wgpu::TextureDescriptor storage{.label="Retained GX texture sampling view",
        .usage=wgpu::TextureUsage::StorageBinding|wgpu::TextureUsage::TextureBinding,
        .dimension=wgpu::TextureDimension::e2D,.size=size,.format=storage_format,
        .mipLevelCount=1,.sampleCount=1};
    auto texture=gpu::g_device.CreateTexture(&storage);
    auto view=texture.CreateView();
    const wgpu::BufferDescriptor uniform{.label="GX tiled texture view layout",
        .usage=wgpu::BufferUsage::Uniform|wgpu::BufferUsage::CopyDst,.size=sizeof(values)};
    auto buffer=gpu::g_device.CreateBuffer(&uniform);
    gpu::g_queue.WriteBuffer(buffer,0,values.data(),sizeof(values));
    const std::array entries{wgpu::BindGroupEntry{.binding=0,.textureView=source->sampleTextureView},
        wgpu::BindGroupEntry{.binding=1,.textureView=view},
        wgpu::BindGroupEntry{.binding=2,.buffer=buffer,.size=sizeof(values)}};
    const wgpu::BindGroupDescriptor group{.layout=texture_view_pipeline.GetBindGroupLayout(0),
        .entryCount=entries.size(),.entries=entries.data()};
    auto bind=gpu::g_device.CreateBindGroup(&group);
    gfx::machine_encode_before_draw([pipeline=texture_view_pipeline,bind=std::move(bind),size](wgpu::CommandEncoder& encoder) {
        auto pass=encoder.BeginComputePass();
        pass.SetPipeline(pipeline);pass.SetBindGroup(0,bind);
        pass.DispatchWorkgroups((size.width+7u)/8u,(size.height+7u)/8u);
        pass.End();
    });
    ++texture_view_count;
    return std::make_shared<gfx::TextureRef>(std::move(texture),view,view,size,storage_format,1,format);
}
