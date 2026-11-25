// SPDX-License-Identifier: GPL-3.0-or-later
// Composed with Aurora's native CP/VAT owner; no guest-specific values.
struct MachineVertexPlan {
    bool valid{};
    gcnllr::RetailVertexPlan layout;
};
static std::array<MachineVertexPlan,8> machine_vertex_plans;
static void machine_invalidate_vertex_plans() {
    for(auto& plan:machine_vertex_plans)plan.valid=false;
}
static const gcnllr::RetailVertexPlan& machine_vertex_plan(GXVtxFmt fmt) {
    const auto slot=static_cast<unsigned>(fmt);
    if(slot>=machine_vertex_plans.size())throw std::runtime_error("Native VAT format outside hardware range");
    auto& cached=machine_vertex_plans[slot];
    if(!cached.valid) {
        gcnllr::RetailVertexPlan layout;
        const auto& format=g_gxState.vtxFmts[fmt];
        for(int i=GX_VA_PNMTXIDX;i<=GX_VA_TEX7;++i) {
            const auto attr=static_cast<GXAttr>(i);
            switch(g_gxState.vtxDesc[i]) {
            case GX_NONE:break;
            case GX_DIRECT:
                layout.direct(comp_type_size(attr,format.attrs[i].type)*comp_cnt_count(attr,format.attrs[i].cnt));
                break;
            case GX_INDEX8:case GX_INDEX16:
                layout.index(i,g_gxState.vtxDesc[i]==GX_INDEX8?1:2,
                    i==GX_VA_NRM && format.attrs[i].cnt==GX_NRM_NBT3?3:1,
                    i>=GX_VA_POS && i<=GX_VA_TEX7);
                break;
            }
        }
        cached.layout=layout;cached.valid=true;
    }
    return cached.layout;
}
static void machine_accumulate_array_sizes(GXVtxFmt fmt,u16 count,const u8* vertices,bool bigEndian,
                                           std::array<u32,GX_VA_MAX_ATTR>& sizes) {
#if GCNLLR_RETAIL_VERTEX_PLAN
    const auto& plan=machine_vertex_plan(fmt);
    plan.accumulate(vertices,size_t(plan.stride())*count,count,bigEndian,
        [](uint32_t attribute){return g_gxState.arrays[attribute].stride;},sizes);
#else
    std::array<u32,GX_VA_MAX_ATTR> required;
    calculate_indexed_array_sizes_reference(fmt,count,vertices,bigEndian,required);
    for(unsigned i=0;i<required.size();++i)sizes[i]=std::max(sizes[i],required[i]);
#endif
}
static void calculate_indexed_array_sizes(GXVtxFmt fmt,u16 count,const u8* vertices,bool bigEndian,
                                         std::array<u32,GX_VA_MAX_ATTR>& sizes) {
    sizes.fill(0);
    machine_accumulate_array_sizes(fmt,count,vertices,bigEndian,sizes);
}
