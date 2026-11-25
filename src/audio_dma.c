/* SPDX-License-Identifier: GPL-3.0-or-later
 * Shared audio-DMA state machine extracted from src/dsp.c. Register/transfer
 * ordering follows Dolphin DSP.cpp RegisterMMIO and UpdateAudioDMA. */
#include "dsp/dsp.h"
#include "memory/memory.h"
u16 gcn_dsp_aid_read16(const GcnDsp* dsp,u32 off) {
    switch(off) {
    case GCN_DSP_AID_START_HI:return (u16)(dsp->aid_source>>16);
    case GCN_DSP_AID_START_LO:return (u16)dsp->aid_source;
    case GCN_DSP_AID_CTRL:return dsp->aid_ctrl;
    case GCN_DSP_AID_BLOCKS_LEFT:return dsp->aid_blocks_left ? dsp->aid_blocks_left-1u:0;
    default:return dsp->reg[off>>1];
    }
}
void gcn_dsp_aid_write16(GcnDsp* dsp,u32 off,u16 value,bool wii) {
    switch(off) {
    case GCN_DSP_AID_START_HI:
        dsp->aid_source=(dsp->aid_source&0xffffu)|((u32)(value&(wii?0x1fffu:0x3ffu))<<16);return;
    case GCN_DSP_AID_START_LO:
        dsp->aid_source=(dsp->aid_source&0xffff0000u)|(value&0xffe0u);return;
    case GCN_DSP_AID_CTRL: {
        const bool enabled=(dsp->aid_ctrl&GCN_DSP_AID_ENABLE)!=0;
        dsp->aid_ctrl=value;
        if(!enabled && (value&GCN_DSP_AID_ENABLE)) {
            dsp->aid_cur_addr=dsp->aid_source;dsp->aid_blocks_left=value&GCN_DSP_AID_NUMBLOCKS;
            dsp->aid_int_pending=1;dsp->aid_start_cycles=wii?200:0;
        }
        return;
    }
    case GCN_DSP_AID_BLOCKS_LEFT:return;
    default:dsp->reg[off>>1]=value;return;
    }
}
bool gcn_dsp_aid_advance(GcnDsp* dsp,CPUState* cpu,u32 cycles,u32 period) {
    if(!period)return false;
    if(dsp->aid_int_pending) {
        if(cycles>=dsp->aid_start_cycles) {dsp->aid_int_pending=0;dsp->aid_start_cycles=0;dsp->csr|=GCN_DSP_CSR_AIDINT;}
        else dsp->aid_start_cycles-=cycles;
    }
    u64 accumulated=(u64)dsp->aid_accum+cycles;
    while(accumulated>=period) {
        accumulated-=period;
        if(!(dsp->aid_ctrl&GCN_DSP_AID_ENABLE))continue;
        /* Publish this actual block before reloading a buffer programmed by
         * the interrupt handler while the current buffer was still playing. */
        const u8* samples=NULL;u32 available=0;
        if(cpu)samples=gcn_mem_resolve(cpu,dsp->aid_cur_addr,&available);
        else if(dsp->mem1 && dsp->aid_cur_addr<=dsp->mem1_size) {
            available=dsp->mem1_size-dsp->aid_cur_addr;samples=dsp->mem1+dsp->aid_cur_addr;
        }
        if((cpu || dsp->audio_sink) && (!samples || available<32))return false;
        if(dsp->audio_sink && samples && available>=32)dsp->audio_sink(dsp->audio_user,samples,32);
        if(dsp->aid_blocks_left) {--dsp->aid_blocks_left;dsp->aid_cur_addr+=32;}
        if(!dsp->aid_blocks_left) {
            dsp->aid_cur_addr=dsp->aid_source;dsp->aid_blocks_left=dsp->aid_ctrl&GCN_DSP_AID_NUMBLOCKS;
            dsp->csr|=GCN_DSP_CSR_AIDINT;
        }
    }
    dsp->aid_accum=(u32)accumulated;return true;
}
