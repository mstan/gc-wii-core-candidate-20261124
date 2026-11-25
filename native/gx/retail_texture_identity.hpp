// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>
namespace gcnllr {
class RetailTextureIdentity {
public:
    using Key=std::array<uint32_t,6>; // physical address, bytes, width, height, format, mips
    template<class Generation>
    std::pair<uint32_t,uint32_t> identify(const Key& key,const uint8_t* bytes,Generation generation) {
        if(!bytes || !key[1])throw std::runtime_error("Empty native texture identity");
        ++queries;
        auto& entry=entries_[key];
        uint64_t epoch{};const bool valid=generation(&epoch);
        const bool same=entry.id && ((valid && entry.valid && epoch==entry.epoch) ||
            (!valid && !entry.valid && entry.snapshot.size()==key[1] &&
             !std::memcmp(entry.snapshot.data(),bytes,key[1])));
        if(same)++reused;
        if(!same) {
            if(!entry.id || entry.version==UINT32_MAX) {
                if(next_id_==UINT32_MAX)throw std::runtime_error("Native texture identities exhausted");
                entry.id=++next_id_;entry.version=0;
            }
            ++entry.version;
            entry.valid=valid;entry.epoch=epoch;
            if(valid)entry.snapshot.clear();else entry.snapshot.assign(bytes,bytes+key[1]);
        }
        return {entry.id,entry.version};
    }
    // Indexed draws reference prefixes of the same array. Extending a verified
    // prefix preserves its identity. Aurora checks the GPU upload length before
    // using it. A mutation in previously seen bytes invalidates old uploads.
    std::pair<uint32_t,uint32_t> array_prefix(uint32_t address,const uint8_t* data,uint32_t size,uint64_t scope) {
        if (!scope || !data || !size) throw std::runtime_error("Invalid native vertex array span");
        ++queries;
        auto& entry=arrays_[address];
        if (entry.scope!=scope) {
            entry.scope=scope;
            entry.verified=0;
        }
        if (size<=entry.verified) {
            ++scope_hits;
            ++reused;
            return {entry.id,entry.version};
        }
        const auto known=std::min<size_t>(entry.snapshot.size(),size);
        const auto first=entry.verified;
        const bool changed=first<known && std::memcmp(entry.snapshot.data()+first,data+first,known-first);
        if (changed) std::memcpy(entry.snapshot.data()+first,data+first,known-first);
        if (known<size) entry.snapshot.insert(entry.snapshot.end(),data+known,data+size);
        if (!entry.id || (changed && entry.version==UINT32_MAX)) {
            if (next_id_==UINT32_MAX) throw std::runtime_error("Native vertex array identities exhausted");
            entry.id=++next_id_;
            entry.version=1;
        } else if (changed) {
            ++entry.version;
        } else {
            ++reused;
        }
        entry.verified=size;
        return {entry.id,entry.version};
    }
    void clear(){entries_.clear();arrays_.clear();} // ids are never reused within this host lifetime
    uint64_t queries{},reused{},scope_hits{};
private:
    struct Entry {uint32_t id{},version{};uint64_t epoch{};bool valid{};std::vector<uint8_t> snapshot;};
    struct Array {uint32_t id{},version{},verified{};uint64_t scope{};std::vector<uint8_t> snapshot;};
    std::map<Key,Entry> entries_;
    std::map<uint32_t,Array> arrays_;
    uint32_t next_id_=0x40000000u;
};
}
