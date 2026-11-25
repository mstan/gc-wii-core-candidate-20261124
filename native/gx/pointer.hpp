// SPDX-License-Identifier: MIT
#pragma once
#include "gcnllr/native_gx.h"
#include <algorithm>
#include <cmath>

namespace gcnllr {
// SDL mouse coordinates are logical window units. Presentation dimensions are
// physical pixels and may include letterboxing or a different display scale.
inline GcnllrNativeGxPointer image_pointer(float x,float y,unsigned buttons,
    unsigned width,unsigned height,unsigned pixels_x,unsigned pixels_y,
    unsigned image_x,unsigned image_y) {
    if(!width || !height || !pixels_x || !pixels_y || !image_x || !image_y ||
       !std::isfinite(x) || !std::isfinite(y))return {};
    double view_x=pixels_x,view_y=pixels_y;
    if(uint64_t(pixels_x)*image_y>uint64_t(pixels_y)*image_x)
        view_x=std::round(double(pixels_y)*image_x/image_y);
    else view_y=std::round(double(pixels_x)*image_y/image_x);
    const double px=double(x)*pixels_x/width-(pixels_x-view_x)/2;
    const double py=double(y)*pixels_y/height-(pixels_y-view_y)/2;
    if(view_x<=0 || view_y<=0 || px<0 || py<0 || px>=view_x || py>=view_y)return {};
    return {1,buttons&7u,uint16_t(std::clamp(std::lround(px*65535/view_x),0l,65535l)),
                        uint16_t(std::clamp(std::lround(py*65535/view_y),0l,65535l))};
}
}
