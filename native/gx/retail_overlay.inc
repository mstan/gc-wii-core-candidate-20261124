// SPDX-License-Identifier: MIT
struct MachineOverlayLayout {
    u32 columns{},rows{},tiles{},width{},height{};
    float aspect{};
};
static MachineOverlayLayout machine_overlay;
static float machine_overlay_split_x, machine_overlay_shift_x;

extern "C" void gcnllr_aurora_retail_overlay(u32 columns,u32 rows,u32 tiles,u32 width,u32 height,float aspect) {
    machine_overlay={columns,rows,tiles,width,height,aspect};
    machine_overlay_shift_x=0.f;
}

extern "C" void gcnllr_aurora_retail_overlay_split(float split,float shift) {
    machine_overlay_split_x=split;
    machine_overlay_shift_x=shift;
}

template<class Draw>
static void machine_draw_shared_overlay(const Draw& draw) {
    if(!machine_overlay.tiles) {
        draw();
        return;
    }
    const auto [columns,rows,tiles,width,height,aspect]=machine_overlay;
    const int tile_width=static_cast<int>(width/columns);
    const int tile_height=static_cast<int>(height/rows);
    const float size=.72f;
    const float scale_x=size*float(tile_width)/640.f*(float(width)/height)/aspect;
    const float scale_y=size*float(tile_height)/height;
    const float source_center_y=1.f-float(height)/480.f;
    float original[4][4],projection[4][4];
    std::memcpy(original,&g_gxState.proj,sizeof(original));
    std::memcpy(projection,original,sizeof(projection));
    const auto scissor=g_gxState.logicalScissor;
    for(u32 tile=0;tile<columns*rows;++tile) {
        if(!(tiles&(1u<<tile)))continue;
        const int left=static_cast<int>(tile%columns)*tile_width;
        const int top=static_cast<int>(tile/columns)*tile_height;
        const float x=2.f*(float(left)+float(tile_width)*.5f)/640.f-1.f;
        const float y=1.f-2.f*(float(top)+float(tile_height)*.18f)/480.f-source_center_y*scale_y;
        for(unsigned i=0;i<4;++i)
            projection[1][i]=scale_y*original[1][i]+y*original[3][i];
        auto draw_segment=[&](float shift,int begin,int end) {
            if(begin>=end)return;
            for(unsigned i=0;i<4;++i)
                projection[0][i]=scale_x*original[0][i]+(x+scale_x*shift)*original[3][i];
            std::memcpy(&g_gxState.proj,projection,sizeof(projection));
            g_gxState.stateDirty=true;
            set_logical_scissor({left+begin,top,end-begin,tile_height});
            draw();
        };
        if(machine_overlay_shift_x!=0.f) {
            // Convert each segment's boundary to unscaled EFB coordinates.
            const auto seam=[&](float shift) {
                const float px=tile_width*.5f+320.f*scale_x*(machine_overlay_split_x+shift);
                return std::clamp(static_cast<int>(px+.5f),0,tile_width);
            };
            draw_segment(0.f,0,seam(0.f));
            draw_segment(machine_overlay_shift_x,seam(machine_overlay_shift_x),tile_width);
        } else {
            draw_segment(0.f,0,tile_width);
        }
    }
    std::memcpy(&g_gxState.proj,original,sizeof(original));
    g_gxState.stateDirty=true;
    set_logical_scissor(scissor);
}
