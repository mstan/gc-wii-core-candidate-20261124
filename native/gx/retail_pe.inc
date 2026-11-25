// SPDX-License-Identifier: MIT
// RGB8/Z24 has no observable destination alpha. Keep fragment alpha intact
// for ordinary source-alpha blending; model absent destination alpha as one.
static void machine_efb_state() {
    const auto mode=g_gxState.bpRegCache[0x41],dst=g_gxState.bpRegCache[0x42];
    const bool alpha=(g_gxState.bpRegCache[0x43]&7u)==1u;
    const auto factor=[alpha](u32 raw) {
        const auto value=static_cast<GXBlendFactor>(raw);
        if(!alpha && value==GX_BL_DSTALPHA)return GX_BL_ONE;
        if(!alpha && value==GX_BL_INVDSTALPHA)return GX_BL_ZERO;
        return value;
    };
    g_gxState.blendFacSrc=factor((mode>>8)&7u);
    g_gxState.blendFacDst=factor((mode>>5)&7u);
    g_gxState.dstAlpha=alpha && (dst&256u)?dst&255u:UINT32_MAX;
    g_gxState.alphaUpdate=alpha && (mode&16u);
}
