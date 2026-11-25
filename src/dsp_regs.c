/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Flipper ARAM-controller reset values and direct-register masks. Adapted
 * from ModernGekko/RecompCore's Core/HW/DSP.cpp at
 * e13ab348f13cd67879f6db6e9d7185410f8f62c6.
 */
#include "dsp/dsp.h"
#include "memory/memory.h"
#include "cpu/native_code.h"

#include <string.h>

static u32 idx16(u32 off) {
    return off >> 1;
}

void gcn_dsp_reset_registers(GcnDsp* dsp) {
    memset(dsp, 0, sizeof *dsp);
    /* The ARAM controller has completed initialization at console reset.
     * Nintendo's ARInit waits for AR_MODE bit zero before probing ARAM. */
    dsp->reg[idx16(GCN_DSP_AR_MODE)] = 1u;
    dsp->reg[idx16(GCN_DSP_AR_REFRESH)] = 156u;
}

u16 gcn_dsp_reg_read16(const GcnDsp* dsp, u32 off) {
    if (!dsp || (off & 1u) || off >= GCN_DSP_SIZE)
        return 0u;
    return dsp->reg[idx16(off)];
}

void gcn_dsp_reg_write16(GcnDsp* dsp, u32 off, u16 value) {
    if (!dsp || (off & 1u) || off >= GCN_DSP_SIZE)
        return;

    switch (off) {
    case GCN_DSP_AR_MODE:
        return;                         /* hardware-owned/read-only */
    case GCN_DSP_AR_INFO:
        value &= 0x007Fu;
        break;
    case GCN_DSP_AR_REFRESH:
        value &= 0x07FFu;
        break;
    case GCN_DSP_AR_MMADDR:
    case GCN_DSP_AR_ARADDR:
        value &= 0x03FFu;
        break;
    case GCN_DSP_AR_MMADDR + 2u:
    case GCN_DSP_AR_ARADDR + 2u:
    case GCN_DSP_AR_CNT_LO:
        value &= 0xFFE0u;
        break;
    case GCN_DSP_AR_CNT:
        value &= 0x83FFu;               /* direction bit plus address bits */
        break;
    default:
        break;
    }
    dsp->reg[idx16(off)] = value;
}

bool gcn_dsp_aram_advance(GcnDsp* dsp, u32 ppc_cycles) {
    if (!dsp || !dsp->dma_active)
        return false;

    if (ppc_cycles < dsp->dma_cycles_left) {
        dsp->dma_cycles_left -= ppc_cycles;
        return false;
    }

    dsp->dma_active = false;
    dsp->dma_cycles_left = 0u;
    dsp->csr |= GCN_DSP_CSR_ARINT;
    return true;
}

bool gcn_dsp_aram_transfer(GcnDsp* dsp, CPUState* cpu, u8* aram, u32 aram_size) {
    u32 main=(u32)dsp->reg[idx16(GCN_DSP_AR_MMADDR)]<<16 | dsp->reg[idx16(GCN_DSP_AR_MMADDR)+1];
    u32 aux=(u32)dsp->reg[idx16(GCN_DSP_AR_ARADDR)]<<16 | dsp->reg[idx16(GCN_DSP_AR_ARADDR)+1];
    u32 count=(u32)dsp->reg[idx16(GCN_DSP_AR_CNT)]<<16 | dsp->reg[idx16(GCN_DSP_AR_CNT)+1];
    u32 bytes=count&0x03ffffffu, available;
    u8* memory=gcn_mem_resolve(cpu,main,&available);
    if (!aram || !memory || bytes > available)
        return false;

    /* Unexpanded GameCube ARAM forwards addresses above 16 MiB to the empty
     * high-speed port. Both directions leave their destination unchanged.
     * The SDK's ARChecksize detects expansion by comparing its DMA buffers. */
    bool empty_expansion = !cpu->mem2_size && aram_size == GCN_DSP_ARAM_GAMECUBE_SIZE &&
                           aux >= aram_size;
    if (!empty_expansion) {
        if (aux > aram_size || bytes > aram_size - aux)
            return false;
        if (count & 0x80000000u) {
            if (cpu->reserve_valid && bytes) {
                u32 reservation = gcn_reservation_key(cpu->reserve_addr) & ~31u;
                if (main < reservation + 32u && reservation < (u64)main + bytes)
                    cpu->reserve_valid = false;
            }
            gcn_native_code_content_dirty(main, bytes);
            memmove(memory, aram + aux, bytes);
        } else {
            memmove(aram + aux, memory, bytes);
        }
    }
    dsp->dma_active=true;
    dsp->dma_cycles_left=(bytes/32u)*GCN_DSP_ARAM_TICKS_PER_32B;
    if(!dsp->dma_cycles_left)dsp->dma_cycles_left=1;
    return true;
}
