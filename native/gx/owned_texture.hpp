// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <memory>
#include <vector>

namespace gxruntime::gxcore {struct EfbCopyCommand;}
namespace aurora::gfx {
struct TextureRef;
namespace gxcore {
// Provider-private immutable producer. Guest layout is independent of the
// rendered target size, and remains available without touching live GX state.
struct OwnedTexture {
    std::shared_ptr<TextureRef> texture;
    uint32_t width{},height{},format{};
};
using OwnedTextureLookup=std::shared_ptr<TextureRef>(*)(uint32_t address,uint32_t size,
    uint32_t format,uint32_t width,uint32_t height);
void set_owned_texture_lookup(OwnedTextureLookup lookup);
OwnedTexture capture_owned_efb_copy(const gxruntime::gxcore::EfbCopyCommand&,bool submit);
std::vector<uint8_t> materialize_owned_texture(const OwnedTexture& copy);
void publish_owned_copy_memory(const OwnedTexture&,uint32_t address,const std::vector<uint8_t>& bytes);
}
}
