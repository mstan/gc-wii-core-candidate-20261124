# SPDX-License-Identifier: MIT
"""Compose a private, GPL-preserving native retail Aurora command owner.

The donor checkout stays immutable. Its existing renderer/decoder supplies the
operation; our adaptation adds partial-input framing and machine-owned effects.
"""
from pathlib import Path
import argparse

def once(text,old,new):
    if text.count(old)!=1:raise ValueError('Pinned Aurora retail adapter differs: '+old[:90])
    return text.replace(old,new,1)

def compose(text,root,packet_draw_state=False,draw_packet_header=None):
    # Materialize the hardware's full 1..1024 range. The donor's raw fallback
    # getters mask after adding one, which turns a 1024-pixel axis into zero.
    text=once(text,'        slot.mWidth = 0;\n        slot.mHeight = 0;',
        '        slot.mWidth = (value & 0x3ffu) + 1u;\n        slot.mHeight = ((value >> 10) & 0x3ffu) + 1u;')
    text=once(text,'return (reg & 0x001FFFFFu) << 5;',
        'return (reg & (GC_WII_GX_WII ? 0x00FFFFFFu : 0x001FFFFFu)) << 5;')
    text=once(text,'#include "command_processor.hpp"',
        '#include "command_processor.hpp"\n#include "'+(root/'retail_vertex_plan.hpp').as_posix()+'"')
    text=once(text,'static u32 s_display_list_depth = 0;',
        '#include "'+(root/'retail_raster.inc').as_posix()+'"\n#include "'+
        (root/'retail_overlay.inc').as_posix()+'"\nstatic std::array<bool,256> machine_bp_valid{};\nstatic void machine_flush_batch();\nstatic void machine_invalidate_vertex_plans();\nstatic u32 s_display_list_depth = 0;')
    # A second entry shares the entire donor command loop and live GX state.
    # Recursive display lists use the same adapted complete-input entry.
    start=text.index('void process(const u8* data, u32 size, bool bigEndian) {')
    end=text.index('\n// Helper to extract bit fields',start)
    body=text[start:end].rstrip()
    if not body.endswith('}'):raise ValueError('Unknown command-loop boundary')
    native=body.replace('void process(const u8* data, u32 size, bool bigEndian)',
        'extern "C" u32 gcnllr_aurora_retail_process(const u8* data, u32 size)',1)
    native=once(native,'  u32 pos = 0;','  constexpr bool bigEndian=true;\n  u32 pos = 0;')
    native=once(native,'  while (pos < size) {',r'''  while (pos < size) {
    const u32 framed=gcnllr::retail_command_size(data+pos,size-pos,[](u8 fmt) {
      return g_gxState.lastVtxFmt==fmt?g_gxState.lastVtxSize:calculate_last_vtx_size(static_cast<GXVtxFmt>(fmt));
    });
    if(!framed)break;''')
    native=native[:-1]+'  return pos;\n}\n'
    # Hooks see the masked register value, including repeated PE notifications.
    old='    if (g_gxState.bpRegCache[regId] == value && regId != 0x52)\n      return;\n    g_gxState.bpRegCache[regId] = value;'
    new='''    const bool handled=gcnllr_aurora_retail_bp(regId,value&0xffffffu);
    // LOAD_TLUT1 executes a transfer even when its control word repeats.
    // LOAD_TLUT0 or the source RAM may have changed since the last transfer.
    if (machine_bp_valid[regId] && g_gxState.bpRegCache[regId] == value && regId != 0x52 && regId != 0x65 && !handled)
      return;
    machine_bp_valid[regId]=true;
    g_gxState.bpRegCache[regId] = value;
    if(handled)return;'''
    new=new.replace('    g_gxState.bpRegCache[regId] = value;', '''    if(!((regId>=0x8c && regId<=0x93) || (regId>=0xac && regId<=0xb3)))machine_flush_batch();
    g_gxState.bpRegCache[regId] = value;''')
    text=once(text,old,new)
    text=once(text,'static void handle_cp(u8 addr, u32 value, bool bigEndian) {',r'''static std::array<u32,256> machine_cp{};
static std::array<bool,256> machine_cp_valid{};
extern "C" void gcnllr_aurora_retail_reset(){machine_cp_valid.fill(false);machine_bp_valid.fill(false);machine_invalidate_vertex_plans();machine_viewport_valid=machine_projection_valid=false;machine_raster_dirty=true;}
static void handle_cp(u8 addr, u32 value, bool bigEndian) {
  // These fields have one writer in this selected pipeline. Matrix indices
  // can also be written by XF, so deliberately do not cache those here.
  if(addr==0x50 || addr==0x60 || (addr>=0x70 && addr<=0x97) || addr>=0xa0) {
    if(machine_cp_valid[addr] && machine_cp[addr]==value)return;
    machine_cp[addr]=value;machine_cp_valid[addr]=true;
  }
  machine_flush_batch();
  if(addr==0x50 || addr==0x60 || (addr>=0x70 && addr<=0x97))machine_invalidate_vertex_plans();''')
    text=once(text,'static void calculate_indexed_array_sizes(GXVtxFmt fmt,',
        'static void calculate_indexed_array_sizes_reference(GXVtxFmt fmt,')
    marker='static void handle_draw_unmerged(GXPrimitive prim, GXVtxFmt fmt, u16 vtxCount, gfx::Range vertRange,'
    # One forward declaration and a later body; insert before the declaration.
    position=text.index(marker)
    text=text[:position]+'#include "'+(root/'retail_vertex_plan.inc').as_posix()+'"\n'+text[position:]
    text=once(text,'''      case TexBpRegMapping::Kind::Image2:
        // GXTexRegion regs
        break;''','''      case TexBpRegMapping::Kind::Image2:
        // Native resources have no hardware TMEM placement dependency.
        return;''')
    text=once(text,'  u32 totalVtxBytes = vtxCount * vtxSize;',
        '  machine_raster();machine_efb_state();\n  gcnllr_aurora_retail_draw();\n  u32 totalVtxBytes = vtxCount * vtxSize;')
    signature='static void draw_prim(GXPrimitive prim, GXVtxFmt fmt, u16 vtxCount, const u8* data, u32& pos, u32 size, bool bigEndian) {'
    arrays='#if GCNLLR_PERSISTENT_GEOMETRY\n#include "'+(root/'persistent_arrays.inc').as_posix()+'"\n#endif\n'
    text=once(text,signature,arrays+r'''static void machine_prepare_arrays(const std::array<u32,GX_VA_MAX_ATTR>& sizes,
                                   size_t vertices,size_t indices) {
  static std::array<uint64_t,GX_VA_MAX_ATTR> versions{};
#if GCNLLR_PERSISTENT_GEOMETRY
  static std::array<bool,GX_VA_MAX_ATTR> epoch_versions{};
#endif
  size_t needed=0, all=0;
  for (u32 a=0;a<GX_VA_MAX_ATTR;++a) {
    auto& array=g_gxState.arrays[a];
#if GCNLLR_PERSISTENT_GEOMETRY
    const u32 bytes=std::min(sizes[a],array.size);
#else
    const u32 bytes=sizes[a];
#endif
    if (!bytes) continue;
#if GCNLLR_PERSISTENT_GEOMETRY
    const auto persistent=machine_array_binding(array.data,bytes);
    const bool epoch_version=persistent.version!=0;
    const auto version=epoch_version ? persistent.version : gcnllr_aurora_retail_array(array.data,bytes);
    const bool representation_changed=epoch_versions[a]!=epoch_version;
    epoch_versions[a]=epoch_version;
#else
    const auto version=gcnllr_aurora_retail_array(array.data,bytes);
    constexpr bool representation_changed=false;
#endif
    if (representation_changed || (version && version!=versions[a])) {
      array.cachedRange={};
      g_gxState.stateDirty=true;
      versions[a]=version;
    }
#if GCNLLR_PERSISTENT_GEOMETRY
    if (persistent.range.size) array.cachedRange=persistent.range;
#endif
    const size_t upload=size_t(bytes)+256u;
    all+=upload;
    if (array.cachedRange.size<bytes) needed+=upload;
  }
  const auto room=[&](size_t storage) {
    return gfx::staging_has_capacity(vertices,indices,MaxUniformSize*2u,0,storage);
  };
  if (room(needed)) return;
  if (!gfx::segment_frame()) throw std::runtime_error("Native retail staging segment failed");
  for (auto& array:g_gxState.arrays) array.cachedRange={};
  g_gxState.stateDirty=true;
  if (!room(all)) throw std::runtime_error("Native retail draw exceeds an empty staging segment");
}
'''+signature.replace('draw_prim(','draw_prim_reference('))
    marker='static void handle_draw(u8 cmd, const u8* data, u32& pos, u32 size, bool bigEndian) {'
    text=once(text,marker,'static void draw_prim(GXPrimitive,GXVtxFmt,u16,const u8*,u32&,u32,bool);\n'+marker)
    text=once(text,'static void machine_flush_batch();',
        '#include "'+(root/'retail_pe.inc').as_posix()+'"\nstatic void machine_flush_batch();')
    text=once(text,'std::string read_string(const u8* data, u32& pos, u32 size, bool bigEndian) {',
        '#include "'+(root/'retail_batch.inc').as_posix()+'"\nstd::string read_string(const u8* data, u32& pos, u32 size, bool bigEndian) {')
    xf_data='  const u8* xfData = data + pos;'
    text=once(text,xf_data,xf_data+r'''
  // The donor's scalar XF register cache already treats these writes as
  // no-ops. Don't split the pending draw batch before the same comparison.
  if (addr >= 0x1000 && addr < 0x101a && count <= 0x101a - addr) {
    bool unchanged = true;
    for (u32 i = 0; i < count && unchanged; ++i)
      unchanged = read_u32(xfData + i * 4, bigEndian) == g_gxState.xfRegCache[addr - 0x1000 + i];
    if (unchanged) {
      pos += dataBytes;
      return;
    }
  }
  machine_flush_batch();''')
    if packet_draw_state:
        if draw_packet_header is None:
            raise ValueError('The retained draw state requires the retail packet header')
        text=once(text, '#include "pipeline.hpp"', '#include "'+draw_packet_header.as_posix()+'"')
        text=once(text,'#include "command_processor.hpp"', '#include "command_processor.hpp"\n#include "'+
            (root/'retail_uniform_matrices.hpp').as_posix()+'"')
        text=once(text,'MaxUniformSize*2u,0,storage);',
            'MaxUniformSize*2u,0,storage+sizeof(RetailDrawGeometry)+sizeof(u32)-1u);')
        text=once(text,'  const auto info = build_shader_info(config.shaderConfig);',r'''  struct MaterialInfo {
    ShaderConfig key{};
    ShaderInfo info{};
    RetailUniformMatrices matrices{};
    uint64_t version{};
    bool valid{};
  };
  static thread_local MaterialInfo lastMaterial;
  if (!lastMaterial.valid ||
      std::memcmp(&lastMaterial.key, &config.shaderConfig, sizeof(config.shaderConfig)) != 0) {
    const auto info = build_shader_info(config.shaderConfig);
    lastMaterial = {config.shaderConfig, info, retail_uniform_matrices(config.shaderConfig, info),
                    lastMaterial.version + 1u, true};
  }
  const auto& info = lastMaterial.info;''')
        text=once(text, '  gfx::push_draw_command(DrawData{',
            '  u32 firstInstance{};\n'
            '  const auto uniforms = build_retail_uniform(info, lastMaterial.matrices, lastMaterial.version,\n'
            '                                            vertRange.offset, ranges, firstInstance);\n'
            '  gfx::push_draw_command(DrawData{')
        text=once(text, '.uniformRange = build_uniform(info, vertRange.offset, ranges),',
            '.uniformRange = uniforms,\n      .firstInstance = firstInstance,')
    text=once(text,'      ZoneScopedN("LOAD_INDX");','      ZoneScopedN("LOAD_INDX");\n      machine_flush_batch();')
    memory_boundary='  calculate_indexed_array_sizes(fmt, vtxCount, data + pos, bigEndian, arraySizes);'
    if text.count(memory_boundary)!=2:raise ValueError('Pinned native resource boundaries differ')
    text=text.replace(memory_boundary,r'''  calculate_indexed_array_sizes(fmt, vtxCount, data + pos, bigEndian, arraySizes);
  machine_prepare_arrays(arraySizes,size_t(vtxCount)*vtxSize,size_t(vtxCount)*6u*sizeof(u16));
  // Texture observation occurs at the actual resolver, after a live GPU-owned
  // view has been considered. It must not force a download before that lookup.
''')
    # Retail source performs the same staging check in its instanced extension,
    # whose local names differ. Our real FIFO scope never uses that host opcode.
    instanced=text.index('void handle_aurora(const u8* data, u32& pos, u32 size, bool bigEndian) {')
    tail=text[instanced:]
    tail=tail.replace('size_t(vtxCount)*vtxSize','size_t(vtxCount)*calculate_last_vtx_size(fmt)')
    text=text[:instanced]+tail
    first=text.index('  // Scissor registers (0x20, 0x21)')
    last=text.index('  // Line/point size (0x22)',first)
    text=text[:first]+'''  case 0x20:case 0x21:case 0x59:
    machine_raster_dirty=true;break;

'''+text[last:]
    text=once(text,'          f32 width = sx * 2.0f;',r'''          const float captured[]={sx,sy,sz,ox,oy,oz};
          std::memcpy(machine_viewport,captured,sizeof(captured));
          machine_viewport_valid=true;machine_raster_dirty=true;
          f32 width = sx * 2.0f;''')
    text=once(text,'.znear = (oz - sz) / 1.6777215e7f,',
        '.znear = gcnllr::normalized_viewport_depth(oz - sz),')
    text=once(text,'.zfar = oz / 1.6777215e7f,',
        '.zfar = gcnllr::normalized_viewport_depth(oz),')
    text=once(text,'''          g_gxState.stateDirty = true;
        }
        break;
      }
      case 0x3F:''','''          machine_projection=g_gxState.proj;machine_projection_valid=true;machine_raster_dirty=true;
          g_gxState.stateDirty = true;
        }
        break;
      }
      case 0x3F:''')
    # The pinned raw decoder still used the older raster-origin constant.
    text=text.replace('ox - 340.0f','ox - 342.0f').replace('oy - 340.0f','oy - 342.0f')
    # A failed nested resource must fail this provider, never silently omit it.
    text=once(text,'        Log.warn("Unable to resolve GX_CMD_CALL_DL address=0x{:08X} size={}", address, dlSize);\n        break;',
        '        throw std::runtime_error("Native retail display-list address did not resolve");')
    text=once(text,'        Log.warn("Skipping GX_CMD_CALL_DL recursion at depth {} (address=0x{:08X}, size={})", s_display_list_depth,\n                 address, dlSize);\n        break;',
        '        throw std::runtime_error("Native retail display-list recursion exceeded");')
    # Re-extract the now-adapted complete loop so recursive lists receive the
    # same failure/effect handling as the incremental top-level stream.
    text=once(text,'  s_display_list_depth++;', '''#if GCNLLR_COMPILED_SURFACES
      if (bigEndian && machine_surface(address,static_cast<const u8*>(dlData),dlSize)) break;
#endif
      s_display_list_depth++;''')
    entry='void process(const u8* data, u32 size, bool bigEndian) {'
    text=once(text,entry,'#if GCNLLR_COMPILED_SURFACES\nstatic bool machine_surface(u32,const u8*,u32);\nstatic void machine_reset_surfaces();\n#endif\n'+entry)
    reset_uniforms='\n  reset_retail_uniforms();' if packet_draw_state else ''
    text=once(text,'gcnllr_aurora_retail_reset(){','gcnllr_aurora_retail_reset(){'+reset_uniforms+
        '\n#if GCNLLR_COMPILED_SURFACES\n  machine_reset_surfaces();\n#endif\n')
    marker='std::string read_string(const u8* data, u32& pos, u32 size, bool bigEndian) {'
    text=once(text,marker,'#if GCNLLR_COMPILED_SURFACES\n#include "'+(root/'retail_surface.inc').as_posix()+'"\n#endif\n'+marker)
    start=text.index(entry)
    adapted_end=text.index('\n// Helper to extract bit fields',start)
    adapted=text[start:adapted_end].rstrip()
    native=adapted.replace('void process(const u8* data, u32 size, bool bigEndian)',
        'extern "C" u32 gcnllr_aurora_retail_process(const u8* data, u32 size)',1)
    native=once(native,'  u32 pos = 0;','  constexpr bool bigEndian=true;\n  u32 pos = 0;')
    native=once(native,'  while (pos < size) {',r'''  while (pos < size) {
    const u32 framed=gcnllr::retail_command_size(data+pos,size-pos,[](u8 fmt) {
      return g_gxState.lastVtxFmt==fmt?g_gxState.lastVtxSize:calculate_last_vtx_size(static_cast<GXVtxFmt>(fmt));
    });
    if(!framed)break;''')
    native=native[:-1]+'  return pos;\n}\n'
    framing=r'''
static u32 calculate_last_vtx_size(GXVtxFmt);

extern "C" u32 gcnllr_aurora_retail_required(const u8* data,u32 size) {
  return gcnllr::retail_command_required(data,size,[](u8 fmt) {
    return g_gxState.lastVtxFmt==fmt?g_gxState.lastVtxSize:calculate_last_vtx_size(static_cast<GXVtxFmt>(fmt));
  });
}
'''
    text=text[:adapted_end]+framing+native+text[adapted_end:]
    # Native host SDK initialization uses this same command processor before a
    # machine binding exists; hooks are inert until its real FIFO takes over.
    prefix='''// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from pinned RecompCore338a3dbc Aurora gx/command_processor.cpp.
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <list>
#include <unordered_map>
#include "'''+(root/'raster_viewport.hpp').as_posix()+'''"
#include "'''+(root/'retail_command.hpp').as_posix()+'''"
extern "C" bool gcnllr_aurora_retail_bp(uint32_t,uint32_t);
extern "C" void gcnllr_aurora_retail_draw();
extern "C" void gcnllr_aurora_retail_observe(const void*,uint32_t);
extern "C" uint64_t gcnllr_aurora_retail_array(const void*,uint32_t);
#if GCNLLR_PERSISTENT_GEOMETRY
#include "'''+(root/'persistent_geometry.hpp').as_posix()+'''"
extern "C" bool gcnllr_aurora_geometry_epoch(const void*,uint32_t,uint32_t*,uint64_t*);
#endif
'''
    # Reuse the original scoreboard draw and its resources for each view.
    marker='  gfx::push_draw_command(DrawData{'
    if packet_draw_state:
        marker='  u32 firstInstance{};\n  const auto uniforms = build_retail_uniform('
    begin=text.index(marker)
    end=text.index('  });\n}',begin)+len('  });')
    body=text[begin:end]
    text=text[:begin]+'  machine_draw_shared_overlay([&] {\n'+body+'\n  });'+text[end:]
    text=text.replace('MaxUniformSize*2u,0,storage',
                      'MaxUniformSize*(machine_overlay_shift_x!=0.f ? 9u : machine_overlay.tiles ? 5u : 2u),0,storage')
    return prefix+text

def compose_texture(text):
    text=once(text, '  for (u8 i = 0; i < g_gxState.numTexGens; ++i) {',
        '''  // Capture texgen count in a reserved shader-key byte. Disabled
  // texturing differs without texgens; worker compilation must use captured
  // state rather than the mutable machine state.
  config.shaderConfig.pad2 = g_gxState.numTexGens;
  for (u8 i = 0; i < g_gxState.numTexGens; ++i) {''')
    text=once(text, '    config.shaderConfig.alphaCompare = g_gxState.alphaCompare;',
        '''    // The per-draw uniform owns the alpha thresholds, including fades.
    // Keep the two compare functions and their boolean operator in the key.
    config.shaderConfig.alphaCompare = {
        .comp0 = g_gxState.alphaCompare.comp0,
        .ref0 = 0,
        .op = g_gxState.alphaCompare.op,
        .comp1 = g_gxState.alphaCompare.comp1,
        .ref1 = 0,
    };''')
    text=once(text,'      .msaaSamples = gfx::get_sample_count(),',
        '      .version = GXPipelineConfigVersion + GCNLLR_RETAIL_GX_CONFIG_VERSION_OFFSET,\n'
        '      .msaaSamples = gfx::get_sample_count(),')
    for name,args in [('resolve_static_texture','const GXTexObj_& obj'),
                      ('resolve_static_palette_texture','const GXTexObj_& obj, const GXTlutObj_& tlut')]:
        marker='gfx::TextureHandle '+name+'('+args+') {'
        replacement=marker.replace('const GXTexObj_& obj','const GXTexObj_& input')
        if name=='resolve_static_texture':
            replacement+=r'''
  if(auto live=gcnllr_aurora_retail_owned(input.data,recomp::texture_byte_size(input),
      input.width(),input.height(),input.format(),input.mip_count()))return live;
'''
        else:
            replacement+=r'''
  if (input.format() == GX_TF_C8) {
    if (auto live = gcnllr_aurora_retail_owned_ci8(input, tlut)) return live;
  }
'''
        replacement+=r'''
  gcnllr_aurora_retail_observe(input.data,recomp::texture_byte_size(input));
  GXTexObj_ owned=input;
  if(gcnllr_aurora_retail_texture(owned.data,recomp::texture_byte_size(owned),owned.width(),owned.height(),
      owned.format(),owned.mip_count(),&owned.texObjId,&owned.texDataVersion))owned.set_no_cache(false);
  const GXTexObj_& obj=owned;'''
        text=once(text,marker,replacement)
    return '''// SPDX-License-Identifier: GPL-3.0-or-later
// Pinned Aurora raw-renderer texture cache, bound to actual machine writers.
#include "recomp.hpp"
#include <memory>
namespace aurora::gfx {struct TextureRef;}
std::shared_ptr<aurora::gfx::TextureRef> gcnllr_aurora_retail_owned(const void*,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
std::shared_ptr<aurora::gfx::TextureRef> gcnllr_aurora_retail_owned_ci8(const GXTexObj_&, const GXTlutObj_&);
extern "C" void gcnllr_aurora_retail_observe(const void*,uint32_t);
extern "C" bool gcnllr_aurora_retail_texture(const void*,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t*,uint32_t*);
'''+text

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--source',type=Path,required=True);p.add_argument('--out',type=Path,required=True);p.add_argument('--root',type=Path,required=True);p.add_argument('--texture',action='store_true');p.add_argument('--packet-draw-state',action='store_true');p.add_argument('--draw-packet-header',type=Path);a=p.parse_args()
    a.out.parent.mkdir(parents=True,exist_ok=True)
    original=a.source.read_text(encoding='utf-8')
    result=compose_texture(original) if a.texture else compose(original,a.root,a.packet_draw_state,a.draw_packet_header)
    if not a.out.exists() or a.out.read_text(encoding='utf-8') != result:
        a.out.write_text(result,encoding='utf-8',newline='\n')
