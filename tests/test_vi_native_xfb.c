/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cpu/cpu.h"
#include "vi/vi.h"

#include <stdio.h>
#include <stdlib.h>

static unsigned presents;
static u64 publications = 1;

void gcn_ring_event(u16 kind, u32 detail, u32 aux, u32 pc) {
    (void)kind; (void)detail; (void)aux; (void)pc;
}
int gcn_host_window_enabled(void) { return 1; }
int gcn_present_stats_enabled(void) { return 0; }
void gcn_gx_xfb_read_begin(void) {
    fputs("Native presenter must be able to complete an XFB write\n", stderr);
    exit(1);
}
void gcn_gx_xfb_read_end(void) {}
u64 gcn_gx_xfb_generation(void) { return 1; }
u64 gcn_gx_xfb_pub_count(void) { return publications; }
void gcn_host_window_present(const u8* xfb, u32 width, u32 height, u32 stride) {
    (void)xfb; (void)width; (void)height; (void)stride;
    ++presents;
    ++publications; /* Pending copy completed during the present. */
}

int main(void) {
    u8 ram[4096] = {0};
    CPUState cpu = {.ram = ram, .ram_size = sizeof ram};
    GcnVi vi;
    gcn_vi_init(&vi);
    gcn_vi_set_cpu(&vi, &cpu);
    vi.reg[GCN_VI_VTR >> 1] = 0x0016u;
    vi.reg[GCN_VI_HTR0_LO >> 1] = 1u;
    vi.reg[0x48u >> 1] = 0x0101u;
    vi.reg[(GCN_VI_FB_LEFT_TOP_HI + 2u) >> 1] = 0x20u;
    gcn_vi_tick(100000u);
    if (presents != 1) {
        fprintf(stderr, "Expected one completed XFB generation, saw %u\n", presents);
        return 1;
    }
    ++publications;
    gcn_vi_tick(200000u);
    if (presents != 2) {
        fprintf(stderr, "Expected a new present after the next XFB copy, saw %u\n", presents);
        return 1;
    }
    return 0;
}
