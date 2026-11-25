// SPDX-License-Identifier: GPL-3.0-or-later
// Native draw batch composed with pinned Aurora command_processor.cpp.
// The FIFO entry owns all RAM inputs until finish; state mutations, copies,
// PE effects and return to the machine retire its pending batch first.
#if GCNLLR_PERSISTENT_GEOMETRY
struct MachineSurface;
#endif
struct MachineDrawBatch {
    ByteBuffer vertices,indices;
    std::array<u32,GX_VA_MAX_ATTR> arrays{};
    GXVtxFmt format=GX_VTXFMT0;
    u32 count{};
#if GCNLLR_PERSISTENT_GEOMETRY
    std::vector<const MachineSurface*> surfaces;
    uint64_t surface_key{};
#endif
};
static MachineDrawBatch machine_batch;
#if GCNLLR_COMPILED_SURFACES
static void machine_finish_surface();
#endif
#if GCNLLR_PERSISTENT_GEOMETRY
static void machine_flatten_surface_batch();
static void machine_emit_surface_batch();
#endif
static void machine_flush_batch() {
#if GCNLLR_COMPILED_SURFACES
    machine_finish_surface();
#endif
    auto& batch=machine_batch;
#if GCNLLR_PERSISTENT_GEOMETRY
    if (!batch.surfaces.empty()) {
        machine_emit_surface_batch();
        return;
    }
#endif
    if(!batch.count)return;
    if(!batch.indices.size()) {
        batch.count=0;batch.vertices.clear();batch.arrays.fill(0);return;
    }
    const auto arrays=batch.arrays;
    machine_prepare_arrays(arrays,batch.vertices.size(),batch.indices.size());
    machine_raster();machine_efb_state();
    gcnllr_aurora_retail_draw();
    const auto vertices=gfx::push_verts(batch.vertices.data(),batch.vertices.size(),4);
    const auto indices=gfx::push_indices(batch.indices.data(),batch.indices.size(),4);
    push_gx_draw(GX_TRIANGLES,batch.format,static_cast<u16>(batch.count),vertices,indices,
        static_cast<u32>(batch.indices.size()/sizeof(u16)),arrays);
    batch.count=0;batch.vertices.clear();batch.indices.clear();batch.arrays.fill(0);
}
extern "C" void gcnllr_aurora_retail_finish(){machine_flush_batch();}
static void draw_prim(GXPrimitive primitive,GXVtxFmt format,u16 count,const u8* data,u32& pos,u32 size,bool bigEndian) {
#if GCNLLR_COMPILED_SURFACES
    machine_finish_surface();
#endif
    const bool triangles=primitive==GX_QUADS || primitive==GX_TRIANGLES ||
        primitive==GX_TRIANGLEFAN || primitive==GX_TRIANGLESTRIP;
    if(!triangles) {
        machine_flush_batch();draw_prim_reference(primitive,format,count,data,pos,size,bigEndian);return;
    }
    if(!count)return;
    const u32 stride=g_gxState.lastVtxFmt==format?g_gxState.lastVtxSize:calculate_last_vtx_size(format);
    const uint64_t bytes=uint64_t(count)*stride;
    if(!stride || bytes>size-pos)throw std::runtime_error("Native retail batch vertex input truncated");
    auto& batch=machine_batch;
    // One batch uses one real live format and native state. Flush before a
    // format change or bounded vertex-index rollover, retaining every primitive.
    if(batch.count && (batch.format!=format || batch.count+count>4096))machine_flush_batch();
#if GCNLLR_PERSISTENT_GEOMETRY
    machine_flatten_surface_batch();
#endif
    batch.format=format;
    machine_accumulate_array_sizes(format,count,data+pos,bigEndian,batch.arrays);
    const u16 complete=primitive==GX_QUADS?u16(count-count%4):
        primitive==GX_TRIANGLES?u16(count-count%3):count;
    if(complete>=3)prepare_idx_buffer(batch.indices,primitive,static_cast<u16>(batch.count),complete);
    batch.vertices.append(data+pos,static_cast<size_t>(bytes));pos+=static_cast<u32>(bytes);
    batch.count+=count;
}
