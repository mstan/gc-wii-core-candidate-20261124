// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <array>
#include <cmath>

namespace gcnllr {
struct RasterOrigin { int x=342,y=342; };

// The SDK can specify 2^24 at the end of a GX depth range. EFB depth
// saturates at 0xFFFFFF, so that endpoint must map to exactly 1.0 before
// Aurora inverts the depth range. A sky pass may use it at both endpoints.
inline constexpr float normalized_viewport_depth(float depth) {
    constexpr float max_depth=16777215.f;
    return std::clamp(depth,0.f,max_depth)/max_depth;
}

// XF positions use the raster origin selected by the wrapped BP scissor,
// which need not be GXSetViewport's customary 342. The IPL renders two
// vertical bands with different origins into its smaller EFB region.
inline RasterOrigin raster_origin(const float* vp,unsigned tl,unsigned br,unsigned offset) {
    const int x0=(tl>>12)&2047,y0=tl&2047,x1=(br>>12)&2047,y1=br&2047;
    if (x0>x1 || y0>y1) return {};
    const int base_x=int(offset&511)*2,base_y=int((offset>>10)&511)*2;
    const float vx0=vp[3]-std::abs(vp[0]),vx1=vp[3]+std::abs(vp[0]);
    const float vy0=vp[4]-std::abs(vp[1]),vy1=vp[4]+std::abs(vp[1]);
    RasterOrigin chosen;float best_overlap=-1;int best_area=-1;
    for (int kx=-4;kx<=4;kx++) for (int ky=-4;ky<=4;ky++) {
        const int ox=base_x+kx*1024,oy=base_y+ky*1024;
        const int left=std::clamp(x0-ox,0,640),right=std::clamp(x1-ox+1,0,640);
        const int top=std::clamp(y0-oy,0,528),bottom=std::clamp(y1-oy+1,0,528);
        if (left>=right || top>=bottom) continue;
        const float overlap=std::max(0.f,std::min(float(right+ox),vx1)-std::max(float(left+ox),vx0))*
                            std::max(0.f,std::min(float(bottom+oy),vy1)-std::max(float(top+oy),vy0));
        const int area=(right-left)*(bottom-top);
        if (overlap>best_overlap || (overlap==best_overlap && area>best_area)) {
            chosen={ox,oy};best_overlap=overlap;best_area=area;
        }
    }
    return chosen;
}
inline void remap_raster_viewport(float (&projection)[4][4],float (&vp)[6],RasterOrigin origin,
                                 float width,float height) {
    const float sx=2*vp[0]/width,sy=-2*vp[1]/height;
    const float tx=2*(vp[3]-origin.x)/width-1,ty=1-2*(vp[4]-origin.y)/height;
    for (unsigned col=0;col<4;col++) {
        projection[0][col]=sx*projection[0][col]+tx*projection[3][col];
        projection[1][col]=sy*projection[1][col]+ty*projection[3][col];
    }
    vp[0]=width/2;vp[1]=-height/2;vp[3]=342+width/2;vp[4]=342+height/2;
}
}
