// SPDX-License-Identifier: GPL-3.0-or-later
// Native streaming replacement for RecompCore 338a3dbc's GxCoreSink and
// ConsumingAuroraRenderSink (graphics/gxcore and graphics/frontend).
// Keep one live register model and retire a span-complete draw before changing
// it. This removes historical draw retention and the live/pending state replay.
#pragma once
#include <gxruntime/gxcore/gxcore.hpp>
#include <array>
#include <algorithm>
#include <cstring>

namespace gcnllr {
class NativeDrawSink final : public gxruntime::aurora_recomp::AuroraRenderSink {
    using Packet=gxruntime::aurora_recomp::RenderPacket;
    using Draw=gxruntime::aurora_recomp::ConsumedDraw;
    using Texture=gxruntime::aurora_recomp::ConsumedTexture;
    using Binding=gxruntime::aurora_recomp::ConsumedArrayBinding;
    using Core=gxruntime::gxcore::GxCoreState;
public:
    using PlanObserver=gxruntime::gxcore::GxCoreSink::PlanObserver;
    using CopyObserver=gxruntime::gxcore::GxCoreSink::CopyObserver;
    void set_guest_resolver(const DolGuestAddressResolver* r){resolver_=r;}
    void set_plan_observer(PlanObserver f,void* u){plan_=f;plan_user_=u;}
    void set_copy_observer(CopyObserver f,void* u){copy_=f;copy_user_=u;}
    const char* failure_reason()const{return error_;}
    void flush_frame() {
        if(!pending_)return;
        pending_=false;
        state_.build_draw_plan_into(draw_,counters_,&attrs_,scratch_);
        if(state_.bp_valid(Core::kDrawScopeRegister)) {
            const auto count=state_.bp_valid(Core::kDrawScopeCountRegister)?state_.bp(Core::kDrawScopeCountRegister):0;
            scope_=state_.bp(Core::kDrawScopeRegister);scope_left_=count&0x7fffffu;
            scope_indexed_=(count&0x800000u)!=0;scope_part_=1;
        }
        if(scope_left_) {
            if(scratch_.ok && (scratch_.match_direct_position || scope_indexed_)) {
                scratch_.draw_scope=scope_;scratch_.draw_scope_part=scope_part_++;--scope_left_;
            } else scope_left_=0;
        }
        if(plan_)plan_(scratch_,plan_user_);
        state_.forget_draw_tag();
    }
    bool submit_packet(const Packet& packet)override {
        using namespace gxruntime::aurora_recomp;
        if(error_)return false;
        if(seen_ && packet.sequence<=sequence_)return fail("non-monotonic native packet sequence");
        seen_=true;sequence_=packet.sequence;
        if(packet.kind==RenderPacketKind::Stream)return packet.stream.size || fail("empty native stream");
        if(packet.kind==RenderPacketKind::State) {
            flush_frame();state_.apply(packet.state);
            const auto& p=packet.state;
            if(p.kind==RenderStateKind::CpArrayBase && p.index<bindings_.size()) {
                bindings_[p.index].base_valid=true;bindings_[p.index].base=p.value;
            } else if(p.kind==RenderStateKind::CpArrayStride && p.index<bindings_.size()) {
                bindings_[p.index].stride_valid=true;bindings_[p.index].stride=p.value;
            } else if(p.kind==RenderStateKind::CullAll)cull_=p.value!=0;
            return true;
        }
        if(packet.kind==RenderPacketKind::Draw) {
            const auto& p=packet.draw;
            if(!p.vertex_count || !p.vertex_size || !p.vertex_payload ||
               uint64_t(p.vertex_count)*p.vertex_size!=p.vertex_payload_size)return fail("invalid native draw payload");
            const auto triangle=[](uint32_t v){v&=0xf8;return v==0x80 || v==0x90 || v==0x98 || v==0xa0;};
            // This machine has no interpolation identity consumer. Retain the
            // existing adapter's direct-vertex fusion as well as indexed draws.
            if(pending_ && triangle(draw_.primitive) && triangle(p.primitive) &&
               !scope_left_ && !state_.bp_valid(Core::kDrawScopeRegister) && !state_.bp_valid(Core::kDrawTagRegister) &&
               draw_.vtx_fmt==p.vtx_fmt && draw_.vertex_size==p.vertex_size && p.xf_version &&
               p.xf_version==draw_.xf_version && draw_.vertex_count+p.vertex_count<=1024) {
                if(draw_.segments.empty())draw_.segments.push_back((draw_.primitive<<16)|draw_.vertex_count);
                draw_.segments.push_back((p.primitive<<16)|p.vertex_count);
                draw_.vertex_count+=p.vertex_count;draw_.payload_pn_matrix_mask|=p.payload_pn_matrix_mask;
                draw_.vertex_payload.insert(draw_.vertex_payload.end(),p.vertex_payload,p.vertex_payload+p.vertex_payload_size);
                return true;
            }
            flush_frame();
            draw_.sequence=packet.sequence;draw_.primitive=p.primitive;draw_.vtx_fmt=p.vtx_fmt;
            draw_.vertex_count=p.vertex_count;draw_.vertex_size=p.vertex_size;draw_.cull_all=cull_;
            draw_.active_array_mask=0;draw_.array_input_count=0;draw_.segments.clear();
            for(unsigned i=0;i<bindings_.size();i++)if(bindings_[i].base_valid && bindings_[i].stride_valid)draw_.active_array_mask|=1u<<i;
            draw_.texture=texture_;std::copy(textures_.begin(),textures_.end(),draw_.textures);
            if(!p.xf_version || p.xf_version!=draw_.xf_version) {
#define COPY_TRANSFORM(member) std::memcpy(&draw_.member,&p.member,sizeof(p.member))
                COPY_TRANSFORM(viewport);COPY_TRANSFORM(projection);COPY_TRANSFORM(position_matrices);
                COPY_TRANSFORM(normal_matrices);COPY_TRANSFORM(normal_matrix_word_mask);
                COPY_TRANSFORM(light_words);COPY_TRANSFORM(light_word_mask);COPY_TRANSFORM(chan_regs);
                COPY_TRANSFORM(tex_matrices);COPY_TRANSFORM(tex_matrix_word_mask);COPY_TRANSFORM(xf_regs);
                COPY_TRANSFORM(post_tex_rows);
#undef COPY_TRANSFORM
            }
            draw_.xf_version=p.xf_version;draw_.transform_flags=p.transform_flags;
            draw_.current_pn_matrix=p.current_pn_matrix;draw_.payload_pn_matrix_mask=p.payload_pn_matrix_mask;
            draw_.position_matrix_valid_mask=p.position_matrix_valid_mask;draw_.projection_type=p.projection_type;
            draw_.chan_reg_mask=p.chan_reg_mask;draw_.xf_reg_mask=p.xf_reg_mask;
            draw_.post_tex_mask=p.post_tex_mask;draw_.post_tex_normalize=p.post_tex_normalize;
            draw_.vertex_payload.assign(p.vertex_payload,p.vertex_payload+p.vertex_payload_size);
            pending_=true;return true;
        }
        if(packet.kind!=RenderPacketKind::Resource)return fail("unknown native packet kind");
        const auto& r=packet.resource;
        if(r.kind==RenderResourceKind::IndexedArraySpan) {
            if(!r.size || !r.count || r.index>=bindings_.size())return fail("invalid native array span");
            if(!pending_)return true;
            auto* target=draw_.arrays+draw_.array_input_count;
            for(unsigned i=0;i<draw_.array_input_count;i++)if(draw_.arrays[i].attr==r.index){target=draw_.arrays+i;break;}
            if(target==draw_.arrays+draw_.array_input_count) {
                if(draw_.array_input_count==Draw::kMaxArrays)return fail("too many native arrays");
                ++draw_.array_input_count;
            } else if(target->span_size>=r.size)return true;
            DolGuestResolvedRange range{};
            const bool resolved=resolver_ && dol_guest_address_resolver_resolve(resolver_,bindings_[r.index].base,r.size,
                DOL_GUEST_ADDRESS_AUTO,DOL_GUEST_RESOURCE_VERTEX_ARRAY,&range);
            *target={.attr=r.index,.base=bindings_[r.index].base,.stride=bindings_[r.index].stride,.span_size=r.size,
                .indexed=true,.resolved=resolved,.host_data=resolved?range.data:nullptr,.host_available=resolved?range.available:0,
                .vertex_offset=r.vertex_offset,.index_size=r.index_size,.element_size=r.element_size};
            return true;
        }
        flush_frame();
        if(r.kind==RenderResourceKind::Texture) {
            if(!r.size || r.index>=textures_.size())return fail("invalid native texture");
            texture_={.valid=true,.slot=r.index,.address=r.address,.size=r.size,.format=r.format,.width=r.width,.height=r.height};
            DolGuestResolvedRange range{};
            if(!resolver_ || !dol_guest_address_resolver_resolve(resolver_,r.address,r.size,
                DOL_GUEST_ADDRESS_AUTO,DOL_GUEST_RESOURCE_TEXTURE,&range))return fail("native texture address did not resolve");
            texture_.resolved=true;texture_.host_data=range.data;texture_.host_available=range.available;
            if(r.tlut_address && r.tlut_entries && dol_guest_address_resolver_resolve(resolver_,r.tlut_address,r.tlut_entries*2,
                DOL_GUEST_ADDRESS_AUTO,DOL_GUEST_RESOURCE_TLUT,&range)) {
                texture_.has_tlut=true;texture_.tlut_address=r.tlut_address;texture_.tlut_format=r.tlut_format;
                texture_.tlut_entries=r.tlut_entries;texture_.tlut_host_data=range.data;texture_.tlut_host_available=range.available;
            }
            textures_[r.index]=texture_;return true;
        }
        if(r.kind==RenderResourceKind::Tlut)return r.size || fail("empty native palette");
        if(r.kind!=RenderResourceKind::CopyDestination)return fail("unknown native resource");
        if(!r.size && r.format!=0xf)return fail("empty native copy");
        if(copy_) {
            const auto c0=state_.bp(0x4f),c1=state_.bp(0x50),cz=state_.bp(0x51);
            const gxruntime::gxcore::EfbCopyCommand command{
                .dest_address=r.address,.byte_size=r.size,.format=r.format,.src_x=r.copy_src_x,.src_y=r.copy_src_y,
                .width=r.copy_src_width?r.copy_src_width:r.width,.height=r.copy_src_height?r.copy_src_height:r.height,
                .destination_width=r.width,.destination_height=r.height,.clear=r.copy_clear!=0,
                .clear_r=c0&255,.clear_g=(c1>>8)&255,.clear_b=c1&255,.clear_a=(c0>>8)&255,.clear_z=cz&0xffffff,
                .color_update=!state_.bp_valid(0x41) || (state_.bp(0x41)&8)!=0,
                .alpha_update=!state_.bp_valid(0x41) || (state_.bp(0x41)&16)!=0,
                .depth_update=!state_.bp_valid(0x40) || (state_.bp(0x40)&16)!=0,
                .efb_has_alpha=!state_.bp_valid(0x43) || (state_.bp(0x43)&7)==1};
            copy_(command,copy_user_);
        }
        return true;
    }
private:
    bool fail(const char* message){error_=message;return false;}
    const DolGuestAddressResolver* resolver_{};
    Core state_;
    Draw draw_;
    gxruntime::gxcore::GapCounters counters_{};
    gxruntime::gxcore::CachedVertexAttrs attrs_{};
    gxruntime::gxcore::DrawPlan scratch_{};
    std::array<Binding,16> bindings_{};
    std::array<Texture,8> textures_{};Texture texture_{};
    PlanObserver plan_{};void* plan_user_{};CopyObserver copy_{};void* copy_user_{};
    uint64_t sequence_{};bool seen_{},pending_{},cull_{};const char* error_{};
    uint32_t scope_{},scope_left_{},scope_part_{};bool scope_indexed_{};
};
}
