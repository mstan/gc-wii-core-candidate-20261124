/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Gather Pipe (GP) model (impl). See include/gp/gp.h for scope and the exact
 * Dolphin GPFifo/CommandProcessor functions/lines every step is transcribed
 * from. Reads are meaningless (the gather pipe is write-only); writes accumulate
 * and burst.
 */
#include "gp/gp.h"
#ifndef GCNLLR_GPU_DEVICE_ONLY
#include "gx/gx.h"         /* gcn_gx_pipeline_drain — fifo-reset join (G3) */
#endif
#include "memory/memory.h"
#include "cpu/native_code.h"
#include "cpu/fifo_submit.h"
#include "debug/rings.h"

#include <stdio.h>
#include <string.h>

/* One GP like the one VI/DI; registered at init so the PI_FIFO_RESET hook (a
 * void(void) function pointer set from boot.c) can reach it without pi.c taking
 * a dependency on gp.h. */
static GcnGp* s_gp = NULL;

void gcn_gp_init(GcnGp* gp, GcnPi* pi, GcnCp* cp) {
    memset(gp, 0, sizeof *gp);
    gp->pi = pi;
    gp->cp = cp;
    s_gp = gp;
}
void gcn_gp_set_drain_hook(GcnGp* gp,void (*drain)(void*),void* user) {gp->drain=drain;gp->drain_user=user;}

void gcn_gp_reset(void) {
    /* ProcessorInterface.cpp:118-119 / GPFifo::ResetGatherPipe:80-83 — drop any
     * partial staged bytes. (The CP-register ResetFifo Dolphin also runs on
     * PI_FIFO_RESET is deferred: the IPL boot path does not depend on it, and
     * coupling it here would diverge loudly rather than silently — GX_PLAN.)
     *
     * G3: a fifo reset with pipelined-but-undecoded bytes queued would let
     * pre-reset commands execute after the reset the synchronous design
     * decoded them before — join first (no-op with the pipeline off). */
    if(s_gp && s_gp->drain)s_gp->drain(s_gp->drain_user);
#ifndef GCNLLR_GPU_DEVICE_ONLY
    else gcn_gx_pipeline_drain();
#endif
    if (s_gp)
        s_gp->count = 0;
}

/* Flush one full 32-byte block: copy to guest RAM at the PI write pointer,
 * record it, advance the PI write pointer (wrap END->BASE), then run the CP
 * gather-pipe-burst handler. */
static void gp_flush_burst(GcnGp* gp, CPUState* cpu) {
    GcnPi* pi = gp->pi;
    u32 wptr = pi->reg[GCN_PI_FIFO_WPTR >> 2];
    u32 base = pi->reg[GCN_PI_FIFO_BASE >> 2];
    u32 end  = pi->reg[GCN_PI_FIFO_END  >> 2];

    /* Copy 32 bytes into guest RAM at the physical write pointer (GPFifo.cpp:96
     * memory.CopyToEmu). Bounds-checked; the pointer is guest-physical. */
    u32 phys = wptr & 0x1FFFFFFFu;
    u32 available=0;
    u8* destination=cpu ? gcn_mem_resolve(cpu,phys,&available):NULL;
    if (destination && available>=GCN_GP_GATHER_SIZE) {
        gcn_ring_watch_check_span(phys, GCN_GP_GATHER_SIZE, 0x6A6A6A00u); /* [gcn-watch] */
        memcpy(destination, gp->stage, GCN_GP_GATHER_SIZE);
        /* Device write to RAM: dirty the miss-CRC identity only (the guest
         * must icbi before executing gather-written bytes). */
        gcn_native_code_content_dirty(phys, GCN_GP_GATHER_SIZE);
    } else {
        static int warned = 0;
        if (!warned) {
            fprintf(stderr, "gcn gp: FIFO write pointer 0x%08X out of MEM1 range — "
                            "burst not copied (FIFO not set up?)\n", wptr);
            warned = 1;
        }
    }

    /* Always-on FIFO recorder ring (ROADMAP M2 packet inventory). */
    gcn_ring_fifo(cpu ? cpu->pc : 0u, wptr, gp->stage);

    /* Advance the PI write pointer: wrap to base when it equals end, else += 32
     * (GPFifo.cpp:101-104). */
    if (wptr == end)
        wptr = base;
    else
        wptr += GCN_GP_GATHER_SIZE;
    pi->reg[GCN_PI_FIFO_WPTR >> 2] = wptr;

    /* CP side: status/interrupt eval + (when linked) mirror wptr + distance. */
    gcn_cp_gather_pipe_bursted(gp->cp, wptr);
}

u32 gcn_gp_read(void* user, CPUState* cpu, u32 addr, u8 size) {
    /* The gather pipe is write-only; a read has no defined value. */
    (void)user; (void)cpu; (void)addr; (void)size;
    return 0;
}

void gcn_gp_write(void* user, CPUState* cpu, u32 addr, u32 value, u8 size) {
    GcnGp* gp = (GcnGp*)user;
    (void)addr;   /* any offset in the window is "append to the gather pipe" */

    /* Append `size` bytes big-endian (GPFifo FastWrite* store swapped values).
     * NOTE: the MMIO layer delivers `value` as u32, so a 64-bit store's high 32
     * bits are already truncated upstream — clamp to 4 bytes accordingly (a
     * u32 shifted by >=32 would be UB). The recompiled IPL uses 32-bit
     * gather-pipe stores (offsets +0/+4), for which this is exact; a future
     * 64-bit store would land as its low word and diverge loudly in the diff. */
    if (size > 4) size = 4;
    /* A complete word fits in the current gather most of the time. Publish
     * the same four big-endian bytes together, retaining the exact 32-byte
     * burst boundary and every flush effect below. Byte/halfword and crossing
     * writes retain the general path. No host-endian or alignment assumption. */
    if(size==4 && gp->count<=GCN_GP_GATHER_SIZE-4) {
        const u8 bytes[4]={(u8)(value>>24),(u8)(value>>16),(u8)(value>>8),(u8)value};
        memcpy(gp->stage+gp->count,bytes,4);gp->count+=4;
        if(gp->count==GCN_GP_GATHER_SIZE){gp_flush_burst(gp,cpu);gp->count=0;}
        return;
    }
    for (u8 i = 0; i < size; i++) {
        u8 b = (u8)(value >> (8u * (size - 1u - i)));
        gp->stage[gp->count++] = b;
        if (gp->count == GCN_GP_GATHER_SIZE) {
            gp_flush_burst(gp, cpu);
            gp->count = 0;
        }
    }
}

int gcn_gp_write_stream(void* user,CPUState* cpu,const u8* bytes,u32 size) {
    GcnGp* gp=user;
    if(!gp || !cpu || (!bytes && size) || gp->count>=GCN_GP_GATHER_SIZE) return 0;
    while(size) {
        const u32 room=GCN_GP_GATHER_SIZE-gp->count,n=size<room ? size:room;
        memcpy(gp->stage+gp->count,bytes,n);gp->count+=n;bytes+=n;size-=n;
        if(gp->count==GCN_GP_GATHER_SIZE) {gp_flush_burst(gp,cpu);gp->count=0;}
    }
    return 1;
}
