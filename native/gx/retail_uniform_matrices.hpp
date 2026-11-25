// SPDX-License-Identifier: MIT
#pragma once

#include "gx.hpp"

#include <bit>

namespace aurora::gx {

struct RetailUniformMatrices {
  static constexpr u32 PositionBankSize = MaxPnMtx + MaxTexMtx;
  static_assert(PositionBankSize < 32 && MaxPTTexMtx < 32);

  u32 textureMatrices = 0;
  bool fullPositions = false;
  bool perVertexPosition = false;

  u32 position_count() const {
    return fullPositions ? PositionBankSize : 1u + std::popcount(textureMatrices);
  }

  u32 normal_count() const { return perVertexPosition ? MaxPnMtx : 1u; }

  u32 texgen_index(u32 index) const {
    return fullPositions ? index : 1u + packed_index(textureMatrices, index);
  }

  static u32 packed_index(u32 mask, u32 index) {
    return std::popcount(mask & ((1u << index) - 1u));
  }
};

inline RetailUniformMatrices retail_uniform_matrices(const ShaderConfig& config, const ShaderInfo& info) {
  RetailUniformMatrices layout;
  layout.perVertexPosition = config.attrs[GX_VA_PNMTXIDX].attrType != GX_NONE;
  layout.fullPositions = layout.perVertexPosition;
  for (u32 i = 0; !layout.fullPositions && i < info.sampledTexCoords.size(); ++i) {
    const auto& generator = config.tcgs[i];
    if (!info.sampledTexCoords.test(i) || (generator.type != GX_TG_MTX2x4 && generator.type != GX_TG_MTX3x4))
      continue;

    if (info.indexAttr.test(GX_VA_TEX0MTXIDX + i)) {
      layout.fullPositions = true;
    } else if (generator.mtx != GX_IDENTITY) {
      const u32 index = generator.mtx / 3;
      if (index < RetailUniformMatrices::PositionBankSize)
        layout.textureMatrices |= 1u << index;
      else
        layout.fullPositions = true;
    }
  }
  return layout;
}

using RetailDrawGeometry = std::array<u32, 13>;
gfx::Range build_retail_uniform(const ShaderInfo& info, const RetailUniformMatrices& matrices,
                                u64 materialVersion, u32 vtxStart, const BindGroupRanges& ranges,
                                u32& firstInstance) noexcept;
void reset_retail_uniforms() noexcept;

} // namespace aurora::gx
