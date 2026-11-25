// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from pinned RecompCore338a3dbc graphics/frontend/src/retail_gx_frontend.cpp.
// Retains gcnllr's qualified Z16 alias and two-bit intensity conversion fixes.
#pragma once
#include <cstdint>
namespace gcnllr {
inline uint32_t retail_copy_format(uint32_t trigger,bool depth) {
    const uint32_t target=(trigger>>3)&15u,real=target/2+(target&1)*8;
    if(depth) {
        switch(real) {
        case 0:return 0x30;
        case 1:case 8:return 0x11;
        case 3:case 0xb:return 0x13;
        case 9:return 0x39;
        case 10:return 0x3a;
        case 12:return 0x3c;
        default:return 0x16;
        }
    }
    if((trigger&(3u<<15))==(3u<<15) && real<=3)return real;
    switch(real) {
    case 0:return 0x20;
    case 1:case 8:return 0x28;
    case 2:return 0x22;
    case 3:return 0x23;
    case 4:return 4;
    case 5:return 5;
    case 7:return 0x27;
    case 9:return 0x29;
    case 10:return 0x2a;
    case 11:return 0x2b;
    case 12:return 0x2c;
    default:return 6;
    }
}
}
