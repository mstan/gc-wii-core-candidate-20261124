// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>
#include <stdexcept>

extern "C" uint32_t gcnllr_aurora_retail_process(const uint8_t*,uint32_t);
extern "C" void gcnllr_aurora_retail_finish();
extern "C" uint32_t gcnllr_aurora_retail_required(const uint8_t*,uint32_t);
namespace gcnllr {
class NativeRetailFrontend {
public:
    bool write_fifo(std::span<const uint8_t> bytes) {
        if(bytes.empty())return false;
        if(bytes.size()>16u*1024u*1024u-pending_.size())throw std::runtime_error("Retail FIFO carry exceeds bounded storage");
#if GCNLLR_GX_ONE_COPY
        borrowed_=bytes;
#else
        pending_.insert(pending_.end(),bytes.begin(),bytes.end());
#endif
        return true;
    }
    bool flush() {
#if GCNLLR_GX_ONE_COPY
        auto input=borrowed_;
        borrowed_={};
        while(!pending_.empty()) {
            const auto required=gcnllr_aurora_retail_required(pending_.data(),uint32_t(pending_.size()));
            if(required==pending_.size()) {
                if(gcnllr_aurora_retail_process(pending_.data(),required)!=required)
                    throw std::runtime_error("Retail decoder rejected a framed split command");
                pending_.clear();
                break;
            }
            if(required<pending_.size())
                throw std::runtime_error("Retail FIFO carry exceeded its command");
            if(input.empty())break;
            const auto needed=std::min(size_t(required)-pending_.size(),input.size());
            pending_.insert(pending_.end(),input.begin(),input.begin()+needed);
            copied_bytes_+=needed;
            input=input.subspan(needed);
        }
        if(!input.empty()) {
            const uint32_t used=gcnllr_aurora_retail_process(input.data(),uint32_t(input.size()));
            if(used>input.size())throw std::runtime_error("Retail decoder consumed beyond shared input");
            borrowed_bytes_+=used;
            copied_bytes_+=input.size()-used;
            pending_.assign(input.begin()+used,input.end());
        }
        gcnllr_aurora_retail_finish();
        return true;
#else
        if(pending_.empty())return true;
        const uint32_t used=gcnllr_aurora_retail_process(pending_.data(),uint32_t(pending_.size()));
        gcnllr_aurora_retail_finish();
        if(used>pending_.size())throw std::runtime_error("Retail decoder consumed beyond its input");
        pending_.erase(pending_.begin(),pending_.begin()+used);return true;
#endif
    }
    size_t pending_fifo_size()const{return pending_.size();}
    const char* last_error()const{return nullptr;}
#if GCNLLR_GX_ONE_COPY
    uint64_t borrowed_bytes()const{return borrowed_bytes_;}
    uint64_t copied_bytes()const{return copied_bytes_;}
#endif
private:
    std::vector<uint8_t> pending_;
#if GCNLLR_GX_ONE_COPY
    std::span<const uint8_t> borrowed_;
    uint64_t borrowed_bytes_{},copied_bytes_{};
#endif
};
}
