// SPDX-License-Identifier: GPL-3.0-or-later
// A GX list that contains only triangles of one vertex layout has no state
// effects. Its primitive topology and indexed extents belong to the asset.
struct MachineSurface {
    u32 address{};
    GXVtxFmt format{};
    gcnllr::RetailVertexPlan layout;
    std::vector<u8> source, vertices;
    ByteBuffer indices;
    uint64_t uploadFrame{};
#if GCNLLR_PERSISTENT_GEOMETRY
    uint64_t source_epoch{},audit_frame{},first_frame{},arena{},identity{};
    uint32_t audit_frames{};
    bool observed{},demoted{};
#endif
    gfx::Range vertexRange{}, indexRange{};
    std::array<u32,GX_VA_MAX_ATTR> elements{};
    u32 count{};
};

static bool machine_compile_surface(MachineSurface& surface,const u8* data,u32 size) {
    for (u32 pos=0; pos<size;) {
        const u8 command=data[pos++];
        if (!command) continue;
        const auto prim=static_cast<GXPrimitive>(command & CP_OPCODE_MASK);
        if (prim!=GX_TRIANGLES && prim!=GX_QUADS && prim!=GX_TRIANGLEFAN && prim!=GX_TRIANGLESTRIP)
            return false;
        if ((command & CP_VAT_MASK)!=surface.format || size-pos<2) return false;
        const u32 count=(u32(data[pos])<<8)|data[pos+1];
        pos+=2;
        const uint64_t bytes=uint64_t(count)*surface.layout.stride();
        if (count<3 || (prim==GX_QUADS && count%4) || (prim==GX_TRIANGLES && count%3) ||
            count>UINT16_MAX-surface.count || bytes>size-pos) return false;
        surface.layout.accumulate(data+pos,bytes,count,true,[](u32){return 1u;},surface.elements);
        prepare_idx_buffer(surface.indices,prim,static_cast<u16>(surface.count),static_cast<u16>(count));
        surface.vertices.insert(surface.vertices.end(),data+pos,data+pos+bytes);
        surface.count+=count;
        pos+=static_cast<u32>(bytes);
    }
    return surface.count!=0;
}

static std::list<MachineSurface> surfaces;
static std::unordered_map<uint64_t,decltype(surfaces)::iterator> surfaceLookup;
static size_t surfaceBytes{};
static constexpr size_t surfaceCacheBytes=24u*1024u*1024u;
static MachineSurface* pendingSurface{};
#if GCNLLR_PERSISTENT_GEOMETRY
struct MachineSurfaceBatchEntry {
    std::vector<uint64_t> identities;
    uint64_t first_frame{},arena{};
    gfx::Range vertices{},indices{};
};
static std::unordered_map<uint64_t,MachineSurfaceBatchEntry> surface_batches;
static size_t surface_batch_bytes{};
static uint64_t surface_identity{};
#endif
static size_t surface_cost(const MachineSurface& surface) {
    return sizeof(surface)+surface.source.capacity()+surface.vertices.capacity()+surface.indices.size();
}
static void machine_reset_surfaces() {
    pendingSurface=nullptr;
#if GCNLLR_PERSISTENT_GEOMETRY
    machine_arrays.clear();
    machine_batch.surfaces.clear();
    machine_batch.surface_key=0;
    surface_batches.clear();
    surface_batch_bytes=0;
#endif
    surfaces.clear();
    surfaceLookup.clear();
    surfaceBytes=0;
}

static void machine_finish_surface() {
    if (!pendingSurface) return;
    auto& surface=*pendingSurface;
    pendingSurface=nullptr;
    std::array<u32,GX_VA_MAX_ATTR> arrays{};
    for (unsigned a=GX_VA_POS;a<=GX_VA_TEX7;++a)
        arrays[a]=surface.elements[a]*g_gxState.arrays[a].stride;
    const auto frame=gfx::current_frame_id();
    const bool uploaded=frame && surface.uploadFrame==frame;
#if GCNLLR_PERSISTENT_GEOMETRY
    const auto arena=gfx::persistent_geometry_generation();
    const bool retained=arena==surface.arena;
    const bool persistent=retained || (frame && frame!=surface.first_frame &&
        gfx::persistent_geometry_room(gfx::GeometryKind::vertices,surface.vertices.size()) &&
         gfx::persistent_geometry_room(gfx::GeometryKind::indices,surface.indices.size()));
#else
    constexpr bool persistent=false;
#endif
    machine_prepare_arrays(arrays,(uploaded||persistent)?0:surface.vertices.size(),
                           (uploaded||persistent)?0:surface.indices.size());
    machine_raster();
    machine_efb_state();
    gcnllr_aurora_retail_draw();
    // Array allocation can start a new frame packet. Its staging ranges
    // belong to that packet, even if the same view and geometry are reused.
#if GCNLLR_PERSISTENT_GEOMETRY
    if(persistent) {
        if(retained)++machine_geometry_stats.list_persistent_hits;
        else {
            surface.vertexRange=gfx::retain_geometry(gfx::GeometryKind::vertices,
                surface.vertices.data(),surface.vertices.size());
            surface.indexRange=gfx::retain_geometry(gfx::GeometryKind::indices,
                surface.indices.data(),surface.indices.size());
            surface.arena=arena;
        }
    } else
#endif
    if (!uploaded || gfx::current_frame_id()!=frame) {
        if (uploaded) machine_prepare_arrays(arrays,surface.vertices.size(),surface.indices.size());
        surface.vertexRange=gfx::push_verts(surface.vertices.data(),surface.vertices.size(),4);
        surface.indexRange=gfx::push_indices(surface.indices.data(),surface.indices.size(),4);
        surface.uploadFrame=gfx::current_frame_id();
    }
    push_gx_draw(GX_TRIANGLES,surface.format,static_cast<u16>(surface.count),surface.vertexRange,surface.indexRange,
                 static_cast<u32>(surface.indices.size()/sizeof(u16)),arrays);
}

static void append_surface_bytes(const MachineSurface& surface,u32 first_vertex) {
    auto& batch=machine_batch;
    batch.vertices.append(surface.vertices.data(),surface.vertices.size());
    const auto* indices=reinterpret_cast<const u16*>(surface.indices.data());
    batch.indices.reserve_extra(surface.indices.size());
    for (size_t i=0;i<surface.indices.size()/sizeof(u16);++i)
        batch.indices.append(static_cast<u16>(first_vertex+indices[i]));
}

#if GCNLLR_PERSISTENT_GEOMETRY
static bool pending_batch_contains(const MachineSurface* surface) {
    const auto& pending=machine_batch.surfaces;
    return std::find(pending.begin(),pending.end(),surface)!=pending.end();
}

static void machine_flatten_surface_batch() {
    auto& batch=machine_batch;
    u32 first_vertex=0;
    for (const auto* surface:batch.surfaces) {
        append_surface_bytes(*surface,first_vertex);
        first_vertex+=surface->count;
    }
    batch.surfaces.clear();
    batch.surface_key=0;
}

static void machine_emit_surface_batch() {
    auto& batch=machine_batch;
    const auto frame=gfx::current_frame_id();
    const auto arena=gfx::persistent_geometry_generation();
    const auto size=batch.surfaces.size();
    if (surface_batch_bytes>=surfaceCacheBytes) {
        surface_batches.clear();
        surface_batch_bytes=0;
    }
    auto& entry=surface_batches[batch.surface_key];
    const bool same=entry.identities.size()==size &&
        std::equal(entry.identities.begin(),entry.identities.end(),batch.surfaces.begin(),
            [](uint64_t id,const MachineSurface* surface){return id==surface->identity;});
    if (!same) {
        const bool new_entry=entry.identities.empty();
        const size_t old_capacity=entry.identities.capacity();
        entry.identities.resize(size);
        std::transform(batch.surfaces.begin(),batch.surfaces.end(),entry.identities.begin(),
            [](const MachineSurface* surface){return surface->identity;});
        surface_batch_bytes+=(entry.identities.capacity()-old_capacity)*sizeof(uint64_t);
        if (new_entry) surface_batch_bytes+=sizeof(MachineSurfaceBatchEntry)+sizeof(uint64_t);
        entry.first_frame=frame;
        entry.arena=0;
    }

    const bool retained=entry.arena==arena;
    bool persistent=retained;
    if (!retained) {
        machine_flatten_surface_batch();
        persistent=frame && frame!=entry.first_frame &&
            gfx::persistent_geometry_room(gfx::GeometryKind::vertices,batch.vertices.size()) &&
            gfx::persistent_geometry_room(gfx::GeometryKind::indices,batch.indices.size());
        if (persistent) {
            entry.vertices=gfx::retain_geometry(gfx::GeometryKind::vertices,batch.vertices.data(),batch.vertices.size());
            entry.indices=gfx::retain_geometry(gfx::GeometryKind::indices,batch.indices.data(),batch.indices.size());
            entry.arena=arena;
            ++machine_geometry_stats.group_uploads;
        }
    } else {
        ++machine_geometry_stats.group_hits;
        batch.surfaces.clear();
        batch.surface_key=0;
    }
    const auto arrays=batch.arrays;
    machine_prepare_arrays(arrays,persistent?0:batch.vertices.size(),persistent?0:batch.indices.size());
    machine_raster();
    machine_efb_state();
    gcnllr_aurora_retail_draw();
    const auto vertices=persistent ? entry.vertices : gfx::push_verts(batch.vertices.data(),batch.vertices.size(),4);
    const auto indices=persistent ? entry.indices : gfx::push_indices(batch.indices.data(),batch.indices.size(),4);
    push_gx_draw(GX_TRIANGLES,batch.format,static_cast<u16>(batch.count),vertices,indices,
        indices.size/sizeof(u16),arrays);
    batch.count=0;
    batch.vertices.clear();
    batch.indices.clear();
    batch.arrays.fill(0);
}
#endif

static void append_surface(const MachineSurface& surface) {
    auto& batch=machine_batch;
    batch.format=surface.format;
#if GCNLLR_PERSISTENT_GEOMETRY
    if (!batch.vertices.size()) {
        batch.surfaces.push_back(&surface);
        batch.surface_key=(batch.surface_key^surface.identity)*1099511628211ull;
    } else
#endif
    append_surface_bytes(surface,batch.count);
    batch.count+=surface.count;
    for (unsigned a=GX_VA_POS;a<=GX_VA_TEX7;++a)
        batch.arrays[a]=std::max(batch.arrays[a],u32(surface.elements[a]*g_gxState.arrays[a].stride));
}

static bool machine_surface(u32 address,const u8* data,u32 size) {
    if (size<3 || size>16384 || !(data[0]&0x80)) return false;
    const auto format=static_cast<GXVtxFmt>(data[0]&CP_VAT_MASK);
    const auto& layout=machine_vertex_plan(format);
    if (!layout.stride()) return false;
    const uint64_t key=(uint64_t(address)<<3)|format;
    const auto found=surfaceLookup.find(key);
    const bool known=found!=surfaceLookup.end();
    if (!known) {
        surfaces.emplace_front();
        surfaceLookup.emplace(key,surfaces.begin());
    } else surfaces.splice(surfaces.begin(),surfaces,found->second);
    auto& surface=surfaces.front();
    const bool same_layout=known && surface.layout==layout && surface.source.size()==size;
    const auto equal_source=[&] {return std::memcmp(surface.source.data(),data,size)==0;};
#if GCNLLR_PERSISTENT_GEOMETRY
    uint32_t physical{};
    uint64_t epoch{};
    const bool tracked=gcnllr_aurora_geometry_epoch(data,size,&physical,&epoch);
    bool hit=false;
    if(same_layout && tracked && surface.observed && !surface.demoted && surface.source_epoch==epoch) {
        hit=true;
        const auto frame=gfx::current_frame_id();
        if(surface.audit_frame!=frame) {
            surface.audit_frame=frame;
            if((++surface.audit_frames&127u)==0) {
                ++machine_geometry_stats.list_audits;
                hit=equal_source();
                if(!hit) {
                    surface.demoted=true;
                    ++machine_geometry_stats.list_demotions;
                }
            }
        }
        machine_geometry_stats.list_hits+=hit;
    } else if(same_layout)hit=equal_source();
    if(hit) {
        surface.source_epoch=epoch;
        surface.observed=tracked;
    }
#else
    const bool hit=same_layout && equal_source();
#endif
    if (!hit) {
        if (pendingSurface==&surface) machine_finish_surface();
#if GCNLLR_PERSISTENT_GEOMETRY
        if (pending_batch_contains(&surface)) machine_flush_batch();
#endif
        if (!surface.source.empty()) surfaceBytes-=surface_cost(surface);
        MachineSurface fresh;
        fresh.address=address;
        fresh.format=format;
        fresh.layout=layout;
#if GCNLLR_PERSISTENT_GEOMETRY
        fresh.source_epoch=epoch;
        fresh.first_frame=gfx::current_frame_id();
        if (surface_identity==UINT64_MAX)throw std::runtime_error("Native surface identities exhausted");
        fresh.identity=++surface_identity;
        fresh.observed=tracked;
        fresh.demoted=surface.demoted;
#endif
        fresh.source.assign(data,data+size);
        if (!machine_compile_surface(fresh,data,size)) fresh.count=0;
        surface=std::move(fresh);
        surfaceBytes+=surface_cost(surface);
        while (surfaceBytes>surfaceCacheBytes && surfaces.size()>1) {
            auto& oldest=surfaces.back();
            if (pendingSurface==&oldest) machine_finish_surface();
#if GCNLLR_PERSISTENT_GEOMETRY
            if (pending_batch_contains(&oldest)) machine_flush_batch();
#endif
            surfaceBytes-=surface_cost(oldest);
            surfaceLookup.erase((uint64_t(oldest.address)<<3)|oldest.format);
            surfaces.pop_back();
        }
    }
    if (!surface.count) return false;
    if (pendingSurface) {
        if (pendingSurface->format==format && pendingSurface->count+surface.count<=4096) {
            append_surface(*pendingSurface);
            pendingSurface=nullptr;
        } else machine_finish_surface();
    }
    if (machine_batch.count && (machine_batch.format!=format || machine_batch.count+surface.count>4096))
        machine_flush_batch();
    if (g_gxState.lastVtxFmt!=format) calculate_last_vtx_size(format);
    if (machine_batch.count) append_surface(surface);
    else pendingSurface=&surface;
    static struct Counts {
        uint64_t hits{}, builds{};
        ~Counts(){std::fprintf(stderr,"[native-surfaces] cached=%llu compiled=%llu\n",
            (unsigned long long)hits,(unsigned long long)builds);}
    } counts;
    if (hit) ++counts.hits;
    else ++counts.builds;
    return true;
}
