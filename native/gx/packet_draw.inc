// SPDX-License-Identifier: MIT
// Legacy packet-to-draw adapter; native Aurora never compiles this body.
void plan(const gxruntime::gxcore::DrawPlan& draw,void*) {
    Measure measurement(draw_cost);
    // The core distinguishes valid zero-fragment primitives from unsupported
    // draws in its reason/counters, but represents both with ok=false. Accept
    // only these explicit topology no-ops, never an arbitrary failed plan.
    if (!draw.ok && draw.indices.empty() && draw.skip_reason) {
        constexpr const char* noops[]={"incomplete quad no-op","incomplete triangle no-op",
            "incomplete triangle strip no-op","incomplete triangle fan no-op",
            "incomplete line no-op","incomplete line strip no-op","incomplete point no-op"};
        for (const char* reason:noops)
            if (std::strcmp(draw.skip_reason,reason)==0) return;
    }
    if (state.draws<2 && draw.vertices.size()>=8) {
        const auto& k=draw.pipeline.shader;const float* v=draw.vertices.data();
        std::fprintf(stderr,"[native-gx-plan] position=%.2f,%.2f,%.2f color=%.2f,%.2f,%.2f,%.2f chans=%u captured=%u mat=%u light=%u ras=%u cc_d=%u ac_d=%u\n",
            v[0],v[1],v[2],v[4],v[5],v[6],v[7],k.num_color_chans,k.chan_captured_mask,
            k.litchan[0].matsource,k.litchan[0].enablelighting,k.tev_stages[0].tevorders_colorchan,
            k.tev_stages[0].cc_d,k.tev_stages[0].ac_d);
    }
    const auto origin=draw.viewport_valid && draw.scissor_valid
        ? gcnllr::raster_origin(draw.viewport,sink->bp[0x20],sink->bp[0x21],sink->bp[0x59]) : gcnllr::RasterOrigin{};
    const gxruntime::gxcore::DrawPlan* submitted=&draw;
    if (draw.viewport_valid && (origin.x!=342 || origin.y!=342)) {
        static thread_local gxruntime::gxcore::DrawPlan adjusted;
        adjusted=draw;
        gcnllr::remap_raster_viewport(adjusted.constants.projection,adjusted.viewport,origin,640,480);
        adjusted.constants_id=0;
        submitted=&adjusted;
    }
    if (!draw.ok || !aurora::gfx::gxcore::submit_draw_plan(*submitted)) {
        fail(draw.skip_reason ? draw.skip_reason : "native draw rejected");
        return;
    }
    ++state.draws;
    ++efb_generation;
}
