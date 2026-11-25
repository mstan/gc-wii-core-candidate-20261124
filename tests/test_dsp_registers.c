/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "dsp/dsp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #expr); \
        return 1; \
    } \
} while (0)

int main(void) {
    GcnDsp dsp;
    gcn_dsp_reset_registers(&dsp);

    CHECK(gcn_dsp_reg_read16(&dsp, GCN_DSP_AR_MODE) == 1u);
    CHECK(gcn_dsp_reg_read16(&dsp, GCN_DSP_AR_REFRESH) == 156u);

    gcn_dsp_reg_write16(&dsp, GCN_DSP_AR_MODE, 0u);
    CHECK(gcn_dsp_reg_read16(&dsp, GCN_DSP_AR_MODE) == 1u);

    gcn_dsp_reg_write16(&dsp, GCN_DSP_AR_INFO, 0xFFFFu);
    gcn_dsp_reg_write16(&dsp, GCN_DSP_AR_REFRESH, 0xFFFFu);
    CHECK(gcn_dsp_reg_read16(&dsp, GCN_DSP_AR_INFO) == 0x007Fu);
    CHECK(gcn_dsp_reg_read16(&dsp, GCN_DSP_AR_REFRESH) == 0x07FFu);

    gcn_dsp_reg_write16(&dsp, GCN_DSP_AR_MMADDR, 0xFFFFu);
    gcn_dsp_reg_write16(&dsp, GCN_DSP_AR_MMADDR + 2u, 0xFFFFu);
    gcn_dsp_reg_write16(&dsp, GCN_DSP_AR_CNT, 0xFFFFu);
    gcn_dsp_reg_write16(&dsp, GCN_DSP_AR_CNT_LO, 0xFFFFu);
    CHECK(gcn_dsp_reg_read16(&dsp, GCN_DSP_AR_MMADDR) == 0x03FFu);
    CHECK(gcn_dsp_reg_read16(&dsp, GCN_DSP_AR_MMADDR + 2u) == 0xFFE0u);
    CHECK(gcn_dsp_reg_read16(&dsp, GCN_DSP_AR_CNT) == 0x83FFu);
    CHECK(gcn_dsp_reg_read16(&dsp, GCN_DSP_AR_CNT_LO) == 0xFFE0u);

    dsp.dma_active = true;
    dsp.dma_cycles_left = 2u * GCN_DSP_ARAM_TICKS_PER_32B;
    dsp.csr = GCN_DSP_CSR_ARMASK;
    CHECK(!gcn_dsp_aram_advance(&dsp, dsp.dma_cycles_left - 1u));
    CHECK(dsp.dma_active);
    CHECK(dsp.dma_cycles_left == 1u);
    CHECK(!(dsp.csr & GCN_DSP_CSR_ARINT));
    CHECK(gcn_dsp_aram_advance(&dsp, 1u));
    CHECK(!dsp.dma_active);
    CHECK(dsp.dma_cycles_left == 0u);
    CHECK(dsp.csr & GCN_DSP_CSR_ARINT);
    CHECK(!gcn_dsp_aram_advance(&dsp, 1u));

    CPUState cpu;
    CHECK(cpu_init(&cpu));
    u8 aram[64]={0};
    memset(cpu.ram+0x1000,0xa5,32);
    gcn_dsp_reset_registers(&dsp);
    gcn_dsp_reg_write16(&dsp,GCN_DSP_AR_MMADDR+2,0x1000);
    gcn_dsp_reg_write16(&dsp,GCN_DSP_AR_CNT_LO,32);
    CHECK(gcn_dsp_aram_transfer(&dsp,&cpu,aram,sizeof aram));
    CHECK(!memcmp(aram,cpu.ram+0x1000,32));
    CHECK(dsp.dma_active && dsp.dma_cycles_left==246);
    CHECK(!gcn_dsp_aram_advance(&dsp,245));
    CHECK(gcn_dsp_aram_advance(&dsp,1));
    memset(aram,0x3c,32);
    cpu.reserve_valid=true;cpu.reserve_addr=0xc0001000;
    gcn_dsp_reg_write16(&dsp,GCN_DSP_AR_CNT,0x8000);
    CHECK(gcn_dsp_aram_transfer(&dsp,&cpu,aram,sizeof aram));
    CHECK(cpu.ram[0x1000]==0x3c && !cpu.reserve_valid);
    dsp.dma_active=false;
    gcn_dsp_reg_write16(&dsp,GCN_DSP_AR_ARADDR+2,64);
    CHECK(!gcn_dsp_aram_transfer(&dsp,&cpu,aram,sizeof aram));
    CHECK(!dsp.dma_active && aram[63]==0);

    /* Retail ARChecksize uses real DMA in both directions above 16 MiB. */
    u8* gamecube_aram = calloc(1, GCN_DSP_ARAM_GAMECUBE_SIZE);
    CHECK(gamecube_aram);
    gcn_dsp_reset_registers(&dsp);
    gcn_dsp_reg_write16(&dsp, GCN_DSP_AR_MMADDR + 2u, 0x1000u);
    gcn_dsp_reg_write16(&dsp, GCN_DSP_AR_ARADDR, 0x0100u);
    gcn_dsp_reg_write16(&dsp, GCN_DSP_AR_CNT_LO, 32u);
    CHECK(gcn_dsp_aram_transfer(&dsp, &cpu, gamecube_aram, GCN_DSP_ARAM_GAMECUBE_SIZE));
    CHECK(gamecube_aram[0] == 0u && gamecube_aram[GCN_DSP_ARAM_GAMECUBE_SIZE - 1u] == 0u);
    CHECK(!gcn_dsp_aram_advance(&dsp, 245u));
    CHECK(gcn_dsp_aram_advance(&dsp, 1u) && (dsp.csr & GCN_DSP_CSR_ARINT));
    dsp.csr = 0u;
    cpu.reserve_valid = true;
    gcn_dsp_reg_write16(&dsp, GCN_DSP_AR_CNT, 0x8000u);
    CHECK(gcn_dsp_aram_transfer(&dsp, &cpu, gamecube_aram, GCN_DSP_ARAM_GAMECUBE_SIZE));
    CHECK(cpu.ram[0x1000] == 0x3cu && cpu.reserve_valid);
    CHECK(!gcn_dsp_aram_advance(&dsp, 245u));
    CHECK(gcn_dsp_aram_advance(&dsp, 1u) && (dsp.csr & GCN_DSP_CSR_ARINT));
    free(gamecube_aram);
    cpu_free(&cpu);

    puts("dsp registers: PASS");
    return 0;
}
