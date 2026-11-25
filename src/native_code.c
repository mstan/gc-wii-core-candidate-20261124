/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cpu/native_code.h"
#include "cpu/rel_module.h"

#include <string.h>

#define GCN_CODE_PAGE_SHIFT 12u
#define GCN_CODE_PAGE_SIZE (1u << GCN_CODE_PAGE_SHIFT)
#define GCN_MEM1_PAGE_COUNT (GC_MAIN_RAM_SIZE / GCN_CODE_PAGE_SIZE)

static u8 s_invalid[(GCN_MEM1_PAGE_COUNT + 7u) / 8u];
static u32 s_invalid_count;
static u32 s_candidate_start, s_candidate_end;
static GcnNativeCandidateFn s_candidate;
static GcnNativeCandidateInvalidateFn s_candidate_invalidate;

/* Content-staleness companion to s_invalid, consumed by interpreter.c's
 * native-miss page-CRC memo. A page's CRC identity can only change through
 * the same funnel that invalidates native code (guest stores via memory.c,
 * device DMA, icbi via cpu_glue.c / the interpreter batch path), so setting
 * a bit here in the same loop lets the miss path reuse its cached CRC until
 * the page could actually have changed -- identical identity semantics to
 * recomputing the CRC on every miss, without hashing 4 KiB per duplicate.
 * Unlike s_invalid (monotonic until reset), these bits are read-and-clear. */
static u8 s_content_stale[(GCN_MEM1_PAGE_COUNT + 7u) / 8u];
u8 gcn_native_store_pages[GCN_MEM1_PAGE_COUNT];

static bool mem1_offset(u32 address, u32* offset) {
    if (address < GC_MAIN_RAM_SIZE) {
        *offset = address;
        return true;
    }
    if (address >= GC_RAM_BASE &&
        address < GC_RAM_BASE + GC_MAIN_RAM_SIZE) {
        *offset = address - GC_RAM_BASE;
        return true;
    }
    if (address >= GC_RAM_UNCACHED &&
        address < GC_RAM_UNCACHED + GC_MAIN_RAM_SIZE) {
        *offset = address - GC_RAM_UNCACHED;
        return true;
    }
    return false;
}

void gcn_native_code_reset(void) {
    gcn_rel_reset();
    memset(s_invalid, 0, sizeof s_invalid);
    memset(gcn_native_store_pages,0,sizeof gcn_native_store_pages);
    s_invalid_count = 0;
    /* All-stale, not all-clean: interpreter.c may hold CRCs cached from
     * before the reset, and reset means the backing bytes are about to be
     * repopulated. */
    memset(s_content_stale, 0xFF, sizeof s_content_stale);
    if (s_candidate_invalidate) {
        gcn_native_code_watch(s_candidate_start,s_candidate_end-s_candidate_start);
        s_candidate_invalidate(s_candidate_start, s_candidate_end - s_candidate_start);
    }
}

void gcn_native_code_set_candidate(u32 start, u32 end,
    GcnNativeCandidateFn call, GcnNativeCandidateInvalidateFn invalidate) {
    s_candidate_start = start;
    s_candidate_end = end;
    s_candidate = start < end && end <= GC_MAIN_RAM_SIZE ? call : NULL;
    s_candidate_invalidate = s_candidate ? invalidate : NULL;
    if (s_candidate_invalidate) {
        gcn_native_code_watch(start,end-start);
        s_candidate_invalidate(start, end - start);
    }
}

void gcn_native_code_watch(u32 address,u32 size) {
    u32 offset;
    if (!size || !mem1_offset(address,&offset)) return;
    u64 end=(u64)offset+size;
    if (end>GC_MAIN_RAM_SIZE) end=GC_MAIN_RAM_SIZE;
    for (u32 page=offset>>GCN_CODE_PAGE_SHIFT; page<=(end-1u)>>GCN_CODE_PAGE_SHIFT; page++)
        gcn_native_store_pages[page] |= GCN_NATIVE_STORE_OBSERVED;
}

int gcn_native_code_try_candidate(CPUState* cpu, u32 pc) {
    return s_candidate && pc >= s_candidate_start && pc < s_candidate_end
        ? s_candidate(cpu, pc) : 0;
}

void gcn_native_code_invalidate(u32 address, u32 size) {
    gcn_rel_invalidate(address, size);
    u32 offset;
    if (!size || !mem1_offset(address, &offset))
        return;

    u64 end = (u64)offset + size;
    if (end > GC_MAIN_RAM_SIZE)
        end = GC_MAIN_RAM_SIZE;
    if (s_candidate_invalidate && offset < s_candidate_end && end > s_candidate_start)
        s_candidate_invalidate(address, size);
    u32 first = offset >> GCN_CODE_PAGE_SHIFT;
    u32 last = (u32)(end - 1u) >> GCN_CODE_PAGE_SHIFT;
    for (u32 page = first; page <= last; page++) {
        u8 mask = (u8)(1u << (page & 7u));
        u8* byte = &s_invalid[page >> 3];
        if (!(*byte & mask)) {
            *byte |= mask;
            s_invalid_count++;
        }
        s_content_stale[page >> 3] |= mask;
        gcn_native_store_pages[page] |= GCN_NATIVE_STORE_DIRTY;
    }
}

void gcn_native_code_content_dirty(u32 address, u32 size) {
    gcn_rel_invalidate(address, size);
    u32 offset;
    if (!size || !mem1_offset(address, &offset))
        return;
    u64 end = (u64)offset + size;
    if (end > GC_MAIN_RAM_SIZE)
        end = GC_MAIN_RAM_SIZE;
    u32 first = offset >> GCN_CODE_PAGE_SHIFT;
    u32 last = (u32)(end - 1u) >> GCN_CODE_PAGE_SHIFT;
    for (u32 page = first; page <= last; page++) {
        const u8 mask=(u8)(1u << (page & 7u));
        s_content_stale[page >> 3] |= mask;
        if (s_invalid[page>>3]&mask) gcn_native_store_pages[page] |= GCN_NATIVE_STORE_DIRTY;
    }
}

bool gcn_native_code_page_content_stale_take(u32 pc) {
    u32 offset;
    if (!mem1_offset(pc, &offset))
        return true;   /* outside MEM1: no staleness tracking, always stale */
    u32 page = offset >> GCN_CODE_PAGE_SHIFT;
    u8 mask = (u8)(1u << (page & 7u));
    u8* byte = &s_content_stale[page >> 3];
    if (*byte & mask) {
        *byte &= (u8)~mask;
        gcn_native_store_pages[page] &= (u8)~GCN_NATIVE_STORE_DIRTY;
        return true;
    }
    return false;
}

bool gcn_native_code_is_invalid(u32 pc) {
    u32 offset;
    if (!mem1_offset(pc, &offset))
        return false;
    u32 page = offset >> GCN_CODE_PAGE_SHIFT;
    return (s_invalid[page >> 3] & (u8)(1u << (page & 7u))) != 0;
}

u32 gcn_native_code_invalid_page_count(void) {
    return s_invalid_count;
}
