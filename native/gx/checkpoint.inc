// SPDX-License-Identifier: MIT
constexpr uint32_t gx_checkpoint_magic=0x31584647u;
struct GxCheckpointHeader {
    uint32_t magic, width, height, frontend_size, core_size;
};
using BpSeenBytes=std::array<uint8_t,256>;

bool checkpoint_supported() {
    return initialized && !fifo_worker && !config.own_copy && !failed.load() &&
           gcnllr::gpu_bridge::checkpoint_efb_supported();
}

int capture_checkpoint(void** data,size_t* size) {
    if (!data || !size) return 0;
    *data=nullptr;
    *size=0;
    if (!checkpoint_supported() || !drain()) return 0;
    try {
        const auto fifo_state=frontend->save_state();
        const auto core_state=sink->core.save_state();
        const auto image=gcnllr::gpu_bridge::capture_efb();
        if (fifo_state.empty() || core_state.empty() || fifo_state.size()>UINT32_MAX ||
            core_state.size()>UINT32_MAX) return 0;
        const GxCheckpointHeader header{gx_checkpoint_magic,image.width,image.height,
            uint32_t(fifo_state.size()),uint32_t(core_state.size())};
        BpSeenBytes seen;
        std::copy(sink->bp_seen.begin(),sink->bp_seen.end(),seen.begin());

        const size_t depth_bytes=image.native_depth.size()*sizeof(uint32_t);
        const size_t bytes=sizeof(header)+sizeof(sink->bp)+seen.size()+fifo_state.size()+
                           core_state.size()+image.rgba.size()+depth_bytes;
        auto* output=static_cast<uint8_t*>(std::malloc(bytes));
        if (!output) return 0;
        auto* at=output;
        const auto append=[&at](const void* from,size_t count) {
            std::memcpy(at,from,count);
            at+=count;
        };
        append(&header,sizeof(header));
        append(sink->bp.data(),sizeof(sink->bp));
        append(seen.data(),seen.size());
        append(fifo_state.data(),fifo_state.size());
        append(core_state.data(),core_state.size());
        append(image.rgba.data(),image.rgba.size());
        append(image.native_depth.data(),depth_bytes);
        *data=output;
        *size=bytes;
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr,"[native-gx] checkpoint capture failed: %s\n",e.what());
        return 0;
    }
}

int restore_checkpoint(const void* data,size_t size) {
    constexpr size_t prefix=sizeof(GxCheckpointHeader)+sizeof(MachineSink::bp)+sizeof(BpSeenBytes);
    if (!data || size<prefix || !checkpoint_supported()) return 0;
    const auto* bytes=static_cast<const uint8_t*>(data);
    GxCheckpointHeader header;
    std::memcpy(&header,bytes,sizeof(header));
    uint32_t width,height;
    if (header.magic!=gx_checkpoint_magic || !header.frontend_size || !header.core_size ||
        !gcnllr::gpu_bridge::checkpoint_efb_supported(&width,&height) ||
        header.width!=width || header.height!=height) return 0;
    const size_t pixel_count=size_t(width)*height;
    const size_t plane_size=pixel_count*sizeof(uint32_t);
    if (uint64_t(prefix)+header.frontend_size+header.core_size+uint64_t(plane_size)*2!=size) return 0;

    try {
        const auto* at=bytes+sizeof(header);
        auto restored_sink=std::make_unique<MachineSink>();
        std::memcpy(restored_sink->bp.data(),at,sizeof(restored_sink->bp));
        at+=sizeof(restored_sink->bp);
        for (auto& present:restored_sink->bp_seen) {
            if (*at>1u) return 0;
            present=*at++!=0;
        }
        auto restored_frontend=std::make_unique<RetailGxFrontend>(resolver);
        restored_frontend->set_packet_drain_enabled(true);
        restored_sink->core.set_guest_resolver(&resolver);
        restored_sink->core.set_plan_observer(plan,nullptr);
        restored_sink->core.set_copy_observer(copy,nullptr);
        if (!restored_frontend->load_state(at,header.frontend_size)) return 0;
        at+=header.frontend_size;
        if (!restored_sink->core.load_state(at,header.core_size)) return 0;
        at+=header.core_size;
        gcnllr::gpu_bridge::EfbCheckpoint image;
        image.width=width;
        image.height=height;
        image.rgba.assign(at,at+plane_size);
        at+=plane_size;
        image.native_depth.resize(pixel_count);
        std::memcpy(image.native_depth.data(),at,plane_size);
        if (!drain() || !gcnllr::gpu_bridge::restore_efb(image)) return 0;
        sink=std::move(restored_sink);
        frontend=std::move(restored_frontend);
        aurora::gfx::gxcore::reset_texture_cache();
        invalidate_memory_observations();
        ++efb_generation;
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr,"[native-gx] checkpoint restore failed: %s\n",e.what());
        return 0;
    }
}

const GcnllrNativeGxCheckpointApi checkpoint_api{capture_checkpoint,restore_checkpoint,std::free};
