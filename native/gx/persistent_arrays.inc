// SPDX-License-Identifier: MIT
// Included in the native retail command owner. The source array evolves
// independently of any display list that indexes it.
struct MachineArrayBinding {
    gfx::Range range{};
    uint64_t version{};
};
struct MachineArrayEntry {
    uint32_t bytes{};
    uint64_t epoch{},version{},first_frame{},last_audit_frame{},arena{},checksum{};
    uint32_t audit_frames{};
    bool demoted{};
    gfx::Range range{};
};
static std::unordered_map<uint32_t,MachineArrayEntry> machine_arrays;
static uint64_t machine_array_version;
static struct MachineGeometryStatistics {
    uint64_t array_hits{},array_uploads{},array_audits{},array_demotions{};
    uint64_t list_hits{},list_audits{},list_demotions{},list_persistent_hits{};
    uint64_t group_hits{},group_uploads{};
    ~MachineGeometryStatistics() {
        std::fprintf(stderr,"[native-geometry-cache] array-hits=%llu array-uploads=%llu array-audits=%llu array-demotions=%llu list-epochs=%llu list-audits=%llu list-demotions=%llu list-persistent=%llu group-hits=%llu group-uploads=%llu\n",
            (unsigned long long)array_hits,(unsigned long long)array_uploads,
            (unsigned long long)array_audits,(unsigned long long)array_demotions,
            (unsigned long long)list_hits,(unsigned long long)list_audits,
            (unsigned long long)list_demotions,(unsigned long long)list_persistent_hits,
            (unsigned long long)group_hits,(unsigned long long)group_uploads);
    }
} machine_geometry_stats;

static uint64_t geometry_checksum(const uint8_t* bytes,uint32_t size) {
    uint64_t checksum=14695981039346656037ull;
    for(uint32_t i=0;i<size;++i)checksum=(checksum^bytes[i])*1099511628211ull;
    return checksum;
}

static MachineArrayBinding machine_array_binding(const void* data,uint32_t requested) {
    uint32_t address{};
    uint64_t epoch{};
    if(!gcnllr_aurora_geometry_epoch(data,requested,&address,&epoch))return {};
    auto& entry=machine_arrays[address];
    if(entry.demoted)return {};
    const uint64_t frame=gfx::current_frame_id();
    if(entry.bytes>requested && !gcnllr_aurora_geometry_epoch(data,entry.bytes,&address,&epoch))return {};
    if(!entry.version || epoch!=entry.epoch || requested>entry.bytes) {
        if(machine_array_version==UINT64_MAX)throw std::runtime_error("Native geometry versions exhausted");
        entry={.bytes=std::max(entry.bytes,requested),.epoch=epoch,
               .version=++machine_array_version,.first_frame=frame};
        return {{},entry.version};
    }

    const auto generation=gfx::persistent_geometry_generation();
    const auto* bytes=static_cast<const uint8_t*>(data);
    if(entry.range.size && entry.arena==generation) {
        if(entry.last_audit_frame!=frame) {
            entry.last_audit_frame=frame;
            if((++entry.audit_frames&127u)==0) {
                ++machine_geometry_stats.array_audits;
                if(geometry_checksum(bytes,entry.bytes)!=entry.checksum) {
                    entry.demoted=true;++machine_geometry_stats.array_demotions;
                    return {};
                }
            }
        }
        ++machine_geometry_stats.array_hits;
        return {entry.range,entry.version};
    }
    if(frame && frame!=entry.first_frame) {
        const auto range=gfx::retain_geometry(gfx::GeometryKind::arrays,bytes,entry.bytes);
        if(range.size) {
            entry.range=range;entry.arena=generation;
            entry.checksum=geometry_checksum(bytes,entry.bytes);
            ++machine_geometry_stats.array_uploads;
            return {range,entry.version};
        }
    }
    return {{},entry.version};
}
