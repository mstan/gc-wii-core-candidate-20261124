// SPDX-License-Identifier: MIT
// Legacy comparison provider, excluded from the native Aurora build.
// The trace observer runs before packets execute. Deliver hardware effects
// through the packet sink instead, after all preceding draws and copies.
class MachineSink final : public gxruntime::aurora_recomp::AuroraRenderSink {
public:
    GxCoreSink core;
    std::array<uint32_t,256> bp{};
    std::array<bool,256> bp_seen{};
    uint64_t duplicate_draw_states{};
    gxruntime::aurora_recomp::RenderResourcePacket texture{};
    bool texture_seen{};
    uint64_t duplicate_textures{};
    uint64_t cache_placement_states{};
    // CPU/DMA writers cannot run during one FIFO service entry. A repeated
    // binding within that entry therefore sees the same actual texture bytes.
    // Never carry this assumption across an entry or a GPU-to-MEM1 write.
    void memory_boundary() {texture_seen=false;}
    bool submit_packet(const gxruntime::aurora_recomp::RenderPacket& packet) override {
        using namespace gxruntime::aurora_recomp;
        if (packet.kind==RenderPacketKind::Resource) {
            const auto& r=packet.resource;
            if (r.kind==RenderResourceKind::Texture) {
                const auto& previous=texture;
                // Only the synchronous entry excludes concurrent native CPU
                // stores. A worker handoff may combine several such entries;
                // repeated bindings must compare their actual bytes again.
                const bool same=!fifo_worker && texture_seen && previous.index==r.index && previous.address==r.address &&
                    previous.size==r.size && previous.format==r.format &&
                    previous.width==r.width && previous.height==r.height &&
                    previous.tlut_address==r.tlut_address && previous.tlut_format==r.tlut_format &&
                    previous.tlut_entries==r.tlut_entries;
                if (same) {++duplicate_textures;return !failed.load();}
                texture=r;texture_seen=true;
            } else if (r.kind==RenderResourceKind::Tlut || r.kind==RenderResourceKind::CopyDestination) {
                memory_boundary();
            }
        }
        const bool is_bp=packet.kind==RenderPacketKind::State && packet.state.kind==RenderStateKind::BpReg;
        // The retail frontend has already resolved BP masks. These registers
        // are plain draw state: an identical write need not dirty
        // the core's replay list or prevent draw fusion. Keep first writes,
        // all other registers and the machine's BP notification. TEV's
        // aliased color/konst banks are NOT eligible.
        const auto reg=packet.state.index;
        const bool texture_state=(reg>=0x80 && reg<=0x97) || (reg>=0xA0 && reg<=0xB7);
        const bool draw_state=texture_state || reg==0 || (reg>=0x28 && reg<=0x44) ||
            (reg>=0xC0 && reg<=0xDF) || reg==0xF3;
        const bool duplicate=is_bp && draw_state && bp_seen[reg] && bp[reg]==packet.state.value;
        // TX_SETIMAGE1/2 choose hardware TMEM cache placement. The native
        // renderer resolves actual image bytes independently and reads none
        // of these registers. Retain the frontend/machine writes, but do not
        // treat cache-region rotation as a shader/geometry state change.
        const bool cache_placement=is_bp && ((reg>=0x8C && reg<=0x93) || (reg>=0xAC && reg<=0xB3));
        if (is_bp && (packet.state.index==0x20 || packet.state.index==0x21 || packet.state.index==0x59) &&
            packet.state.index<bp.size() && bp[packet.state.index]!=packet.state.value)
            core.flush_frame();
        if (is_bp && packet.state.index<bp.size()) {
            bp[packet.state.index]=packet.state.value;bp_seen[packet.state.index]=true;
        }
        if (is_bp && (packet.state.index==0x45 || packet.state.index==0x47 || packet.state.index==0x48)) {
            core.flush_frame();flush_display_copies();
        }
        if (cache_placement) ++cache_placement_states;
        else if (duplicate) ++duplicate_draw_states;
        else if (!core.submit_packet(packet)) {
#if !GCNLLR_NATIVE_DRAW_SINK
            if (const auto* reason=core.failure_reason()) throw std::runtime_error(reason);
#endif
            return false;
        }
        if (is_bp && config.bp_write && (reg==0x45 || reg==0x47 || reg==0x48))
            ordered_bp_effect(reg,packet.state.value);
        return !failed.load();
    }
};
