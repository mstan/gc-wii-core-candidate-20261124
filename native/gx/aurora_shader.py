# SPDX-License-Identifier: MIT
"""Scoped correctness adaptations for the pinned Aurora shader backend.

Generated translation units retain their dependency's license. Keep the donor
checkout unchanged and reject a changed source anchor instead of guessing.
"""
import argparse
from pathlib import Path


def once(text, old, new):
    if text.count(old) != 1:
        raise ValueError('Pinned Aurora shader source differs: ' + old[:80])
    return text.replace(old, new, 1)


def shader_info(text, packet_uniform=False, matrix_header=None):
    texture_input = '    CHECK(stage.texCoordId != GX_TEXCOORD_NULL, "tex coord not bound");'
    if text.count(texture_input) != 2:
        raise ValueError('Pinned Aurora texture dependency source differs')
    # A disabled TEV texture contributes a constant without a sampler or
    # texcoord dependency, even when the combiner selects that texture input.
    text = text.replace(texture_input,
        '    if (stage.texMapId == GX_TEXMAP_NULL) break;\n' + texture_input)
    # Keep the prefix size and offsets: the existing eight pad bytes now carry
    # both alpha thresholds from the BP state owned by this draw.
    text = once(text, '  buf.append_zeroes(8); // pad',
                '  buf.append(g_gxState.alphaCompare.ref0);\n'
                '  buf.append(g_gxState.alphaCompare.ref1);')
    if packet_uniform:
        text = once(text, 'namespace aurora::gx {',
                    'namespace aurora::gfx {\n'
                    'ByteBuffer& machine_uniform_packet(size_t capacity, u32& offset);\n'
                    '}\nnamespace aurora::gx {')
        text = once(text, '  static ByteBuffer buf;\n  buf.clear();\n  buf.reserve_extra(info.uniformSize);',
                    '  u32 offset;\n  auto& buf = gfx::machine_uniform_packet(info.uniformSize, offset);')
        text = once(text, '  return gfx::push_uniform(buf.data(), buf.size());',
                    '  return {offset, static_cast<u32>(buf.size() - offset)};')
    if matrix_header:
        uniform_state = Path(matrix_header).with_name('retail_uniform_state.inc').as_posix()
        text = once(text, '#include "shader_info.hpp"',
                    '#include "shader_info.hpp"\n#include <cstdio>\n#include <cstring>\n#include "' + matrix_header + '"')
        text = once(text, '  info.uniformSize += sizeof(Mat3x4<float>) * 30;\n', '')
        text = once(text, '  if (info.usesPTTexMtx.any())\n'
                          '    info.uniformSize += sizeof(Mat3x4<float>) * MaxPTTexMtx;',
                    '  const auto matrices = retail_uniform_matrices(config, info);\n'
                    '  info.uniformSize += sizeof(Mat3x4<float>) *\n'
                    '                      (matrices.position_count() + matrices.normal_count() + info.usesPTTexMtx.count());')
        text = once(text, 'gfx::Range build_uniform(const ShaderInfo& info, u32 vtxStart, const BindGroupRanges& ranges)',
                    '#include "' + uniform_state + '"\n\n'
                    'gfx::Range build_retail_uniform(const ShaderInfo& info, const RetailUniformMatrices& matrices,\n'
                    '                                u64 materialVersion, u32 vtxStart, const BindGroupRanges& ranges,\n'
                    '                                u32& firstInstance)')
        text = once(text, '      .uniformSize = 4 + 4 + 8 + 8 + 8 + 48 + 64,',
                    '      .uniformSize = 4 + 4 + 8 + 8 + 8 + (config.lineMode ? 48u : 0u) + 64,')
        text = once(text, 'ByteBuffer& machine_uniform_packet(size_t capacity, u32& offset);',
                    'ByteBuffer& machine_uniform_packet(size_t capacity, u32& offset);\n'
                    'u32 machine_geometry_packet(const u32* data, size_t words);')
        text = once(text, '  u32 offset;\n  auto& buf = gfx::machine_uniform_packet(info.uniformSize, offset);', r'''  const bool retainedState = info.lineMode == 0;
  const u64 packet = gfx::current_frame_id();
  std::array<Vec4<float>, 8> textureParameters{};
  for (u32 mask = static_cast<u32>(info.sampledTextures.to_ulong()); mask; mask &= mask - 1u) {
    const u32 index = std::countr_zero(mask);
    textureParameters[index] = texture_size_bias(get_texture(static_cast<GXTexMapID>(index)));
  }
  if (retainedState) {
    RetailDrawGeometry geometry{};
    static_assert(std::tuple_size_v<decltype(ranges.vaRanges)> + 1u == std::tuple_size_v<RetailDrawGeometry>);
    geometry[0] = vtxStart;
    for (size_t i = 0; i < ranges.vaRanges.size(); ++i)
      geometry[i + 1u] = ranges.vaRanges[i].offset;
    firstInstance = gfx::machine_geometry_packet(geometry.data(), geometry.size());
    if (packet && packet == retailUniform.packet && materialVersion == retailUniform.material &&
        !g_gxState.stateDirty &&
        std::memcmp(retailUniform.textures.data(), textureParameters.data(), sizeof(textureParameters)) == 0) {
      ++retainedUniformDraws;
      retainedUniformBytes += retailUniform.range.size;
      return retailUniform.range;
    }
  }

  u32 offset;
  auto& buf = gfx::machine_uniform_packet(info.uniformSize, offset);''')
        text = once(text, '  buf.append(vtxStart);', '  buf.append(retainedState ? 0u : vtxStart);')
        text = once(text, '''  for (const auto& vaRange : ranges.vaRanges) {
    buf.append<u32>(vaRange.offset);
  }''', '''  if (!retainedState) {
    for (const auto& vaRange : ranges.vaRanges) buf.append<u32>(vaRange.offset);
  }''')
        text = once(text, '''    const auto& tex = get_texture(static_cast<GXTexMapID>(i));
    // CHECK(tex, "unbound texture {}", i);
    buf.append(texture_size_bias(tex));''', '    buf.append(textureParameters[i]);')
        text = once(text, '  return {offset, static_cast<u32>(buf.size() - offset)};', r'''  const gfx::Range range{offset, static_cast<u32>(buf.size() - offset)};
  if (retainedState) {
    retailUniform = {range, packet, materialVersion, textureParameters};
    ++newUniformDraws;
  }
  return range;''')
        text = once(text, '''  for (int i = 0; i < MaxPnMtx; i++) {
    buf.append(g_gxState.pnMtx[i].pos);
  }

  for (int i = 0; i < MaxTexMtx; i++) {
    buf.append(g_gxState.texMtxs[i]);
  }

  for (int i = 0; i < MaxPnMtx; i++) {
    buf.append(g_gxState.pnMtx[i].nrm);
  }''', '''  if (matrices.fullPositions) {
    for (const auto& matrix : g_gxState.pnMtx) buf.append(matrix.pos);
    buf.append(g_gxState.texMtxs);
  } else {
    const auto index = g_gxState.currentPnMtx;
    if (index < MaxPnMtx) {
      buf.append(g_gxState.pnMtx[index].pos);
    } else if (index < RetailUniformMatrices::PositionBankSize) {
      buf.append(g_gxState.texMtxs[index - MaxPnMtx]);
    } else {
      buf.append(Mat3x4<float>{});
    }
    for (u32 mask = matrices.textureMatrices; mask; mask &= mask - 1u) {
      const u32 slot = std::countr_zero(mask);
      buf.append(slot < MaxPnMtx ? g_gxState.pnMtx[slot].pos : g_gxState.texMtxs[slot - MaxPnMtx]);
    }
  }
  if (matrices.perVertexPosition) {
    for (const auto& matrix : g_gxState.pnMtx) buf.append(matrix.nrm);
  } else {
    const auto index = g_gxState.currentPnMtx;
    buf.append(index < MaxPnMtx ? g_gxState.pnMtx[index].nrm : Mat3x4<float>{});
  }''')
        text = once(text, '      buf.append(g_gxState.ptTexMtxs[i]);',
                    '      if (info.usesPTTexMtx.test(i)) buf.append(g_gxState.ptTexMtxs[i]);')
    return text


def shader(text, packet_shader_cache=False, matrix_header=None):
    text = once(text, '    pad: vec2u,\n', '    alpha_ref: vec2u,\n')
    text = once(text, '''static AlphaCompareExpr alpha_compare(GXCompare comp, u8 ref) {
  const auto iref = static_cast<u32>(ref);
  switch (comp) {
    DEFAULT_FATAL("invalid alpha comp {}", underlying(comp));
  case GX_NEVER:
    return alpha_compare_const(false);
  case GX_LESS:
    if (ref == 0) {
      return alpha_compare_const(false);
    }
    return {fmt::format("(alphaCompare < {}u)", iref), -1};
  case GX_LEQUAL:
    if (ref == 255) {
      return alpha_compare_const(true);
    }
    return {fmt::format("(alphaCompare <= {}u)", iref), -1};
  case GX_EQUAL:
    return {fmt::format("(alphaCompare == {}u)", iref), -1};
  case GX_NEQUAL:
    return {fmt::format("(alphaCompare != {}u)", iref), -1};
  case GX_GEQUAL:
    if (ref == 0) {
      return alpha_compare_const(true);
    }
    return {fmt::format("(alphaCompare >= {}u)", iref), -1};
  case GX_GREATER:
    if (ref == 255) {
      return alpha_compare_const(false);
    }
    return {fmt::format("(alphaCompare > {}u)", iref), -1};
  case GX_ALWAYS:
    return alpha_compare_const(true);
  }
}''', '''static AlphaCompareExpr alpha_compare(GXCompare comp, std::string_view ref) {
  std::string_view op;
  switch (comp) {
    DEFAULT_FATAL("invalid alpha comp {}", underlying(comp));
  case GX_NEVER:
    return alpha_compare_const(false);
  case GX_LESS:
    op = "<";
    break;
  case GX_LEQUAL:
    op = "<=";
    break;
  case GX_EQUAL:
    op = "==";
    break;
  case GX_NEQUAL:
    op = "!=";
    break;
  case GX_GEQUAL:
    op = ">=";
    break;
  case GX_GREATER:
    op = ">";
    break;
  case GX_ALWAYS:
    return alpha_compare_const(true);
  }
  return {fmt::format("(alphaCompare {} {})", op, ref), -1};
}''')
    text = once(text, '''    const auto comp0 = alpha_compare(config.alphaCompare.comp0, config.alphaCompare.ref0);
    const auto comp1 = alpha_compare(config.alphaCompare.comp1, config.alphaCompare.ref1);''',
                '''    const auto comp0 = alpha_compare(config.alphaCompare.comp0, "ubuf.alpha_ref.x");
    const auto comp1 = alpha_compare(config.alphaCompare.comp1, "ubuf.alpha_ref.y");''')
    text = once(text,
        '    UNLIKELY FATAL("unmapped vtx attr {}", underlying(attr));',
        '''    // XF can read a texture source absent from the CP vertex layout.
    // Its default ST is zero; the texgen supplies the homogeneous components.
    if (attr >= GX_VA_TEX0 && attr <= GX_VA_TEX7) {
      return "vec2f(0.0, 0.0)"s;
    }
    UNLIKELY FATAL("unmapped vtx attr {}", underlying(attr));''')
    texture_check = '''    CHECK(stage.texMapId != GX_TEXMAP_NULL, "unmapped texture for stage {}", stageIdx);
    CHECK(stage.texMapId >= GX_TEXMAP0 && stage.texMapId <= GX_TEXMAP7, "invalid texture {} for stage {}",
          underlying(stage.texMapId), stageIdx);'''
    if text.count(texture_check) != 3:
        raise ValueError('Pinned Aurora texture argument checks differ')
    text = text.replace(texture_check,
        '''    CHECK(stage.texMapId == GX_TEXMAP_NULL ||
              (stage.texMapId >= GX_TEXMAP0 && stage.texMapId <= GX_TEXMAP7),
          "invalid texture {} for stage {}", underlying(stage.texMapId), stageIdx);''')
    text = once(text,
        '    CHECK(stage.texMapId != GX_TEXMAP_NULL, "unmapped texture for stage {}", i);',
        '''    // Disabled texturing supplies a constant to the TEV combiner. Keep this
    // after indirect-coordinate processing, which may feed later stages.
    if (stage.texMapId == GX_TEXMAP_NULL) {
      // Match the functional no-texgen case in Dolphin's PixelShaderGen.
      fragmentFnPre += fmt::format("\\n    var sampled{} = vec4f({});", i, config.pad2 ? "1.0" : "0.0");
      continue;
    }''')
    if matrix_header:
        text = once(text, '#include "shader_info.hpp"',
                    '#include "shader_info.hpp"\n#include "' + matrix_header + '"')
        text = once(text, '  const auto info = build_shader_info(config);',
                    '  const auto info = build_shader_info(config);\n'
                    '  const auto matrices = retail_uniform_matrices(config, info);')
        text = once(text, '  std::string uniformPre;', '''  std::string uniformPre = fmt::format(R"(
fn pn_pos(index: u32) -> mat3x4f {{ return ubuf.postex_mtx[{}]; }}
fn pn_nrm(index: u32) -> mat3x4f {{ return ubuf.nrm_mtx[{}]; }}
)", matrices.fullPositions ? "index" : "0u", matrices.perVertexPosition ? "index" : "0u");''')
        for index in ('in_pnmtxidx', 'pnmtxidx_a', 'pnmtxidx_b'):
            reference = 'ubuf.postex_mtx[' + index + ']'
            if text.count(reference) != (2 if index == 'in_pnmtxidx' else 1):
                raise ValueError('Pinned position matrix readers differ: ' + index)
            text = text.replace(reference, 'pn_pos(' + index + ')')
        reference = 'ubuf.nrm_mtx[in_pnmtxidx]'
        if text.count(reference) != 3:
            raise ValueError('Pinned normal matrix readers differ')
        text = text.replace(reference, 'pn_nrm(in_pnmtxidx)')
        text = once(text, 'MaxPnMtx + MaxTexMtx);', 'matrices.position_count());')
        text = once(text, '"\\n    nrm_mtx: array<mat3x4f, {}>,", MaxPnMtx);',
                    '"\\n    nrm_mtx: array<mat3x4f, {}>,", matrices.normal_count());')
        text = once(text, 'i, texMtxIdx);', 'i, matrices.texgen_index(texMtxIdx));')
        text = once(text, 'i, postMtxIdx);',
                    'i, RetailUniformMatrices::packed_index(info.usesPTTexMtx.to_ulong(), postMtxIdx));')
        text = once(text, '"\\n    postmtx: array<mat3x4f, {}>,", MaxPTTexMtx);',
                    '"\\n    postmtx: array<mat3x4f, {}>,", info.usesPTTexMtx.count());')
        text = text.replace('ubuf.vtx_start', 'gx_vtx_start()')
        text = text.replace('ubuf.array_start[{}]', 'gx_array_start({}u)')
        text = once(text, '  const auto shaderSource = fmt::format(R"""(', r'''  const char* arrayGeometry = "";
  if (info.lineMode == 0) {
    vtxInAttrs += ",\n    @builtin(instance_index) gx_draw_geometry: u32";
    vtxXfrAttrsPre = "\n    gx_draw_base = gx_draw_geometry;" + vtxXfrAttrsPre;
    uniformPre += R"(
var<private> gx_draw_base: u32;
fn gx_vtx_start() -> u32 { return abuf[gx_draw_base]; }
fn gx_array_start(index: u32) -> u32 { return abuf[gx_draw_base + 1u + index]; }
)";
  } else {
    arrayGeometry = "\n    array_start: array<u32, 12>,";
    uniformPre += R"(
fn gx_vtx_start() -> u32 { return ubuf.vtx_start; }
fn gx_array_start(index: u32) -> u32 { return ubuf.array_start[index]; }
)";
  }
  const auto shaderSource = fmt::format(R"""(''')
        text = once(text, '    array_start: array<u32, 12>,{0}', '{9}{0}')
        text = once(text, 'fragmentFnPre, vtxXfrAttrsPre, uniformPre);',
                    'fragmentFnPre, vtxXfrAttrsPre, uniformPre, arrayGeometry);')
    if packet_shader_cache:
        text = once(text, '#include <mutex>',
                    '#include <future>\n#include <mutex>\n#include <optional>\n#include <unordered_map>')
        text = once(text, 'wgpu::ShaderModule build_shader(const ShaderConfig& config) noexcept {',
                    'static wgpu::ShaderModule create_shader_module(const ShaderConfig& config) noexcept {')
        text = once(text, '} // namespace aurora::gx', '''
namespace {
struct ShaderConfigHash {
  size_t operator()(const ShaderConfig& config) const noexcept { return xxh3_hash(config); }
};

std::mutex shaderModuleMutex;
std::unordered_map<ShaderConfig, std::shared_future<wgpu::ShaderModule>, ShaderConfigHash> shaderModules;
size_t shaderModuleReuses = 0;
} // namespace

wgpu::ShaderModule build_shader(const ShaderConfig& config) noexcept {
  std::shared_future<wgpu::ShaderModule> module;
  std::optional<std::promise<wgpu::ShaderModule>> create;
  {
    std::lock_guard lock{shaderModuleMutex};
    auto [entry, firstUse] = shaderModules.try_emplace(config);
    if (firstUse) {
      create.emplace();
      entry->second = create->get_future().share();
    } else {
      ++shaderModuleReuses;
    }
    module = entry->second;
  }
  // The exact shader config owns translation and module creation. Distinct
  // shaders can compile concurrently, including while another PSO uses this one.
  if (create) {
    create->set_value(create_shader_module(config));
  }
  return module.get();
}

void clear_machine_shader_modules() {
  std::lock_guard lock{shaderModuleMutex};
  if (!shaderModules.empty()) {
    Log.info("Native GX shader modules created={} reused={}", shaderModules.size(), shaderModuleReuses);
  }
  shaderModules.clear();
  shaderModuleReuses = 0;
}
} // namespace aurora::gx''')
    return text


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--kind', choices=('shader', 'shader_info'), required=True)
    p.add_argument('--packet-uniform', action='store_true')
    p.add_argument('--packet-shader-cache', action='store_true')
    p.add_argument('--retail-uniform-matrices', type=Path)
    args = p.parse_args()
    source = args.source.read_text(encoding='utf-8')
    header = args.retail_uniform_matrices.as_posix() if args.retail_uniform_matrices else None
    result = (shader_info(source, args.packet_uniform, header) if args.kind == 'shader_info'
              else shader(source, args.packet_shader_cache, header))
    args.out.parent.mkdir(parents=True, exist_ok=True)
    if not args.out.exists() or args.out.read_text(encoding='utf-8') != result:
        args.out.write_text(result, encoding='utf-8', newline='\n')


if __name__ == '__main__':
    main()
