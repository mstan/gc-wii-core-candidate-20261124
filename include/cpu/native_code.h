#ifndef GCN_NATIVE_CODE_H
#define GCN_NATIVE_CODE_H

#include "cpu/cpu.h"

/*
 * Page-granular validity fence for statically recompiled MEM1 code.
 *
 * A generated candidate is eligible only while its backing page still holds
 * the bytes it was compiled from. Disc DMA and interpreted stores invalidate
 * pages before control can reach newly loaded code. Invalid pages fall through
 * to the interpreter/capture path until a content-identified AOT module is
 * installed for them.
 */
void gcn_native_code_reset(void);
void gcn_native_code_invalidate(u32 address, u32 size);
bool gcn_native_code_is_invalid(u32 pc);
u32 gcn_native_code_invalid_page_count(void);

/* A bounded content-verified native candidate for code installed during boot.
 * Registration does not clear the monotonic IPL invalidation fence. The
 * candidate must verify its own exact executable ranges before dispatch. */
typedef int (*GcnNativeCandidateFn)(CPUState*, u32);
typedef void (*GcnNativeCandidateInvalidateFn)(u32, u32);
void gcn_native_code_set_candidate(u32 physical_start, u32 physical_end,
    GcnNativeCandidateFn call, GcnNativeCandidateInvalidateFn invalidate);
int gcn_native_code_try_candidate(CPUState* cpu, u32 pc);

/* Content-dirtiness WITHOUT icache invalidation: the page's bytes changed,
 * but architecturally the instruction cache still holds the old line (dcbz's
 * zeroing store, gather-pipe redirect writes, GX XFB/EFB->RAM copies, any
 * device write the guest must icbi before executing). Feeds only the
 * native-miss page-CRC memo; never touches the native-dispatch fence.
 * gcn_native_code_invalidate implies this. */
void gcn_native_code_content_dirty(u32 address, u32 size);

/* Single-CPU store fence. A page can skip redundant bookkeeping only when
 * both invalidation and content-staleness are already recorded and no native
 * candidate/REL observer watches it. Watches are conservative until reset.
 * CRC consumption clears DIRTY; binding any code marks OBSERVED before use.
 * This cache never authorizes native code or suppresses a required callback. */
#define GCN_NATIVE_STORE_DIRTY 1u
#define GCN_NATIVE_STORE_OBSERVED 2u
extern u8 gcn_native_store_pages[GC_MAIN_RAM_SIZE >> 12u];
void gcn_native_code_watch(u32 address,u32 size);
static inline void gcn_native_code_note_store(u32 address,u32 size) {
    u32 offset=address-GC_RAM_BASE;
    if (offset>=GC_MAIN_RAM_SIZE) {
        if (address<GC_MAIN_RAM_SIZE) offset=address;
        else {
            offset=address-GC_RAM_UNCACHED;
            if (offset>=GC_MAIN_RAM_SIZE) {
                gcn_native_code_invalidate(address,size);
                return;
            }
        }
    }
    if (size && size<=4096u-(offset&4095u) &&
        gcn_native_store_pages[offset>>12u]==GCN_NATIVE_STORE_DIRTY)
        return;
    gcn_native_code_invalidate(address,size);
}

/* Read-and-clear content-staleness bit for pc's MEM1 page: true if any
 * gcn_native_code_invalidate or gcn_native_code_content_dirty call (guest
 * store, DMA, icbi, dcbz, device RAM write) covered the page since the last
 * take. Backs interpreter.c's native-miss page-CRC memo; pages outside MEM1
 * always report stale. */
bool gcn_native_code_page_content_stale_take(u32 pc);

#endif
