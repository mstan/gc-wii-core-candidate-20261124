// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace gcnllr {
struct CopyLayout { uint32_t width{},height{},bytes{}; };
// Aurora's copy conversion has already selected intensity/color/depth channels.
// This stage only encodes the resulting texels into guest tiled memory.
inline CopyLayout native_copy_layout(uint32_t format) {
    switch (format) {
    case 0: case 0x20: case 0x30: return {8,8,32};
    case 1: case 2: case 0x11: case 0x22: case 0x27: case 0x28:
    case 0x29: case 0x2A: case 0x39: case 0x3A: return {8,4,32};
    case 3: case 4: case 5: case 0x13: case 0x23: case 0x2B: case 0x2C:
    case 0x3C: return {4,4,32};
    case 6: case 0x16: return {4,4,64};
    default: return {};
    }
}
inline std::vector<uint8_t> encode_native_copy(uint32_t format,uint32_t width,uint32_t height,
    std::span<const uint8_t> rgba,uint32_t render_width,uint32_t render_height) {
    const auto tile=native_copy_layout(format);
    if (!tile.bytes || !width || !height || !render_width || !render_height ||
        uint64_t(render_width)*render_height>rgba.size()/4) return {};
    const uint64_t nx=(uint64_t(width)+tile.width-1)/tile.width;
    const uint64_t ny=(uint64_t(height)+tile.height-1)/tile.height;
    if (nx*ny>0x02000000u/tile.bytes) return {};
    std::vector<uint8_t> out(size_t(nx*ny*tile.bytes));
    for (uint64_t ty=0;ty<ny;ty++) for (uint64_t tx=0;tx<nx;tx++) {
        auto* dst=out.data()+(ty*nx+tx)*tile.bytes;
        for (uint32_t y=0;y<tile.height;y++) for (uint32_t x=0;x<tile.width;x++) {
            const uint64_t sx=(2*std::min(tx*tile.width+x,uint64_t(width-1))+1)*render_width/(2*uint64_t(width));
            const uint64_t sy=(2*std::min(ty*tile.height+y,uint64_t(height-1))+1)*render_height/(2*uint64_t(height));
            const auto* p=rgba.data()+(sy*render_width+sx)*4;
            const uint32_t i=y*tile.width+x;
            if (tile.height==8) {
                if (!(i&1)) dst[i/2]=uint8_t(p[0]&0xF0);
                else dst[i/2]|=uint8_t(p[0]>>4);
            } else if (tile.width==8) {
                dst[i]=(format==2 || format==0x22) ? uint8_t((p[3]&0xF0)|(p[0]>>4)) : p[0];
            } else if (tile.bytes==64) {
                dst[i*2]=p[3];dst[i*2+1]=p[0];
                dst[32+i*2]=p[1];dst[33+i*2]=p[2];
            } else if (format==4 || format==5) {
                const uint16_t value=format==4
                    ? uint16_t((p[0]/8)*2048+(p[1]/4)*32+p[2]/8)
                    : p[3]>=224 ? uint16_t(32768+(p[0]/8)*1024+(p[1]/8)*32+p[2]/8)
                    : uint16_t((p[3]/32)*4096+(p[0]/16)*256+(p[1]/16)*16+p[2]/16);
                dst[i*2]=uint8_t(value>>8);dst[i*2+1]=uint8_t(value);
            } else {
                dst[i*2]=p[3];dst[i*2+1]=p[0];
            }
        }
    }
    return out;
}
}
