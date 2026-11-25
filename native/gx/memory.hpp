// SPDX-License-Identifier: MIT
#pragma once
#include "gcnllr/native_gx.h"
#include <cstdint>

namespace gcnllr {
inline bool gx_memory(const GcnllrNativeGxConfig& config,uint32_t address,uint32_t size,
                      GcnllrNativeGxMemory& result) {
    result={};
    if(config.resolve_memory) {
        if(!config.resolve_memory(config.user,address,size,&result))return false;
        return result.bytes && size<=result.available &&
            uint64_t(result.physical_address)+result.available<=0x100000000ull;
    }
    const uint32_t offset=address&0x1fffffffu;
    if(address!=offset && address!=offset+0x80000000u && address!=offset+0xc0000000u)return false;
    if(!config.mem1 || offset>=config.mem1_size || size>config.mem1_size-offset)return false;
    result={config.mem1+offset,offset,config.mem1_size-offset};
    return true;
}
inline bool gx_address(const GcnllrNativeGxConfig& config,const void* data,uint32_t size,
                       uint32_t& address) {
    if(!data)return false;
    if(config.locate_memory)return config.locate_memory(config.user,data,size,&address)!=0;
    const auto first=reinterpret_cast<uintptr_t>(config.mem1),at=reinterpret_cast<uintptr_t>(data);
    if(!first || at<first || at-first>=config.mem1_size || size>config.mem1_size-(at-first))return false;
    address=uint32_t(at-first);
    return true;
}
}
