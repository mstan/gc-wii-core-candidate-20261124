// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <stdexcept>

namespace gcnllr {
// Find the exact bytes needed for either the next header or a fully
// specified command. Draw stride comes from the live CP state.
template<class VertexSize>
uint32_t retail_command_required(const uint8_t* bytes,uint32_t available,VertexSize vertex_size) {
    if(!available)return 1;
    const uint8_t op=bytes[0];
    uint64_t size=0;
    if(op==0 || op==0x48)size=1;
    else if(op==8)size=6;
    else if(op==0x10) {
        if(available<5)return 5;
        size=5+4u*(uint32_t(bytes[1])*256u+bytes[2]+1u);
    } else if(op==0x20 || op==0x28 || op==0x30 || op==0x38 || op==0x61)size=5;
    else if(op==0x40)size=9;
    else if(op>=0x80 && op<=0xbf && (op&0xf8)!=0x88) {
        if(available<3)return 3;
        const uint32_t count=uint32_t(bytes[1])*256u+bytes[2];
        const uint32_t stride=count?vertex_size(op&7u):0;
        if(count && !stride)throw std::runtime_error("Retail draw has no live vertex layout");
        size=3+uint64_t(count)*stride;
    } else throw std::runtime_error("Unsupported retail FIFO opcode");
    if(size>16u*1024u*1024u)throw std::runtime_error("Retail FIFO command exceeds bounded storage");
    return uint32_t(size);
}
template<class VertexSize>
uint32_t retail_command_size(const uint8_t* bytes,uint32_t available,VertexSize vertex_size) {
    const uint32_t required=retail_command_required(bytes,available,vertex_size);
    return required<=available?required:0;
}
}
