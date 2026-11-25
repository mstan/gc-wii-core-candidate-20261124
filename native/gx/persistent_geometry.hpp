// SPDX-License-Identifier: MIT
#pragma once
#include <cstddef>
#include <cstdint>

namespace aurora::gfx {
struct Range;
enum class GeometryKind { vertices, indices, arrays };
uint64_t persistent_geometry_generation();
bool persistent_geometry_room(GeometryKind kind,std::size_t length);
Range retain_geometry(GeometryKind kind,const uint8_t* bytes,std::size_t length);
}
