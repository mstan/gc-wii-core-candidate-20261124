/* SPDX-License-Identifier: MIT */
#include "cpu/rel_module.h"
#include "cpu/native_code.h"
#include <stdio.h>
#include <string.h>

#define MAX_SECTIONS 1024u
#define PAGE_SHIFT 12u
#define PAGE_COUNT (GC_MAIN_RAM_SIZE >> PAGE_SHIFT)
#define OWNER_WORDS (MAX_SECTIONS / 32u)

typedef struct { u32 header, start; bool bound, dirty; } Binding;
static const GcnRelSection* catalog;
static u32 section_count;
static Binding bindings[MAX_SECTIONS];
/* A page may contain several independently bound sections. Keeping only the
 * last owner made alternating between adjacent RELs rescan the guest module
 * list and rehash every instruction on each call. The two-level bitmap keeps
 * every owner and visits only set bits, including on invalidation. */
typedef struct { u32 sections[OWNER_WORDS]; } PageOwners;
static PageOwners owners[PAGE_COUNT];
/* Ordinary data stores consult this compact table. Interleaving summaries
 * with the larger bitmaps made that hot lookup span ~800 KiB instead of
 * 24 KiB, despite nearly all such pages having no executable owner. */
static u32 owner_words[PAGE_COUNT];
static GcnRelStats stats;

static bool range(CPUState* cpu, u32 start, u32 length) {
    return start >= GC_RAM_BASE && start - GC_RAM_BASE <= cpu->ram_size &&
        length <= cpu->ram_size - (start - GC_RAM_BASE);
}
static u32 word(CPUState* cpu, u32 address) {
    return gcn_rel_instruction_word(cpu, address);
}
void gcn_rel_reset(void) {
    memset(bindings, 0, sizeof(bindings));
    memset(owners, 0, sizeof(owners));
    memset(owner_words,0,sizeof owner_words);
    memset(&stats, 0, sizeof(stats));
}
void gcn_rel_set_catalog(const GcnRelSection* sections, u32 count) {
    catalog = count <= MAX_SECTIONS ? sections : NULL;
    section_count = catalog ? count : 0;
    gcn_rel_reset();
}
void gcn_rel_stats(GcnRelStats* out) { *out = stats; }

static bool verify(CPUState* cpu, u32 index) {
    const GcnRelSection* section = &catalog[index];
    Binding* binding = &bindings[index];
    stats.verifications++;
    if (!range(cpu, binding->start, section->size)) return false;
    for (u32 i = 0; i < section->size / 4u; ++i) {
        if (((word(cpu, binding->start + i * 4u) ^ section->words[i]) & section->masks[i]) != 0) {
            stats.mismatches++;
            return false;
        }
    }
    binding->dirty = false;
    return true;
}
static bool owns(u32 index, u32 address) {
    return index < section_count && bindings[index].bound &&
        address >= bindings[index].start && address - bindings[index].start < catalog[index].size;
}
static u32 first_bit(u32 bits) {
#if defined(__GNUC__) || defined(__clang__)
    return (u32)__builtin_ctz(bits); /* caller always supplies a nonzero word */
#else
    u32 bit=0;
    while (!(bits & 1u)) { bits >>= 1; bit++; }
    return bit;
#endif
}
static void index_pages(u32 index, bool add) {
    if (!bindings[index].bound) return;
    if (add) gcn_native_code_watch(bindings[index].start,catalog[index].size);
    u32 start = (bindings[index].start - GC_RAM_BASE) >> PAGE_SHIFT;
    u32 end = (bindings[index].start - GC_RAM_BASE + catalog[index].size - 1u) >> PAGE_SHIFT;
    u32 slot=index/32u, mask=1u<<(index%32u);
    for (u32 p = start; p <= end; ++p) {
        PageOwners* page=&owners[p];
        if (add) {
            page->sections[slot] |= mask;
            owner_words[p] |= 1u<<slot;
        } else {
            page->sections[slot] &= ~mask;
            if (!page->sections[slot]) owner_words[p] &= ~(1u<<slot);
        }
    }
}
static int cached_binding(u32 address) {
    u32 number=(address-GC_RAM_BASE)>>PAGE_SHIFT;
    const PageOwners* page=&owners[number];
    for (u32 slots=owner_words[number]; slots; slots&=slots-1u) {
        u32 slot=first_bit(slots);
        for (u32 entries=page->sections[slot]; entries; entries&=entries-1u) {
            u32 index=slot*32u+first_bit(entries);
            if (owns(index,address)) return (int)index;
        }
    }
    return -1;
}
/* Read the actual SDK's doubly linked OSModuleInfo list. The guest loader
 * owns placement/linking; this layer neither loads nor relocates guest data. */
static int find_binding(CPUState* cpu, u32 address) {
    if (!range(cpu, GC_RAM_BASE + 0x30C8u, 4)) return -1;
    u32 header = word(cpu, GC_RAM_BASE + 0x30C8u);
    for (u32 visited = 0; header && visited < MAX_SECTIONS; ++visited) {
        if ((header & 3u) || !range(cpu, header, 0x40)) return -1;
        u32 id = word(cpu, header);
        u32 next = word(cpu, header + 4u);
        u32 count = word(cpu, header + 12u);
        u32 table = word(cpu, header + 16u);
        u32 version = word(cpu, header + 28u);
        if (!count || count > 64 || version < 1 || version > 3 ||
            (table & 3u) || !range(cpu, table, count * 8u)) return -1;
        for (u32 i = 0; i < section_count; ++i) {
            const GcnRelSection* section = &catalog[i];
            if (section->module_id != id || section->section >= count) continue;
            u32 entry = table + section->section * 8u;
            u32 start = word(cpu, entry);
            u32 size = word(cpu, entry + 4u);
            if (!(start & 1u) || (start & 2u) || size != section->size) continue;
            start &= ~3u;
            if (!range(cpu, start, size) || address < start || address - start >= size) continue;
            index_pages(i,false);
            bindings[i] = (Binding){header, start, true, true};
            if (!verify(cpu, i)) { bindings[i].bound = false; return -1; }
            index_pages(i,true);
            stats.bindings++;
            fprintf(stderr, "[native-rel] bound %s id=%u section=%u at=0x%08X bytes=%u\n",
                section->name, id, section->section, start, size);
            return (int)i;
        }
        if (next == header) return -1;
        header = next;
    }
    return -1;
}
bool gcn_rel_call(CPUState* cpu, u32 address) {
    if (!catalog || !cpu || !cpu->ram || (address & 3u) || !range(cpu, address, 4)) return false;
    int index = cached_binding(address);
    if (index < 0) index=find_binding(cpu,address);
    if (index < 0) return false;
    Binding* binding = &bindings[index];
    if (binding->dirty && !verify(cpu, (u32)index)) {
        index_pages((u32)index,false);
        binding->bound = false;
        index = find_binding(cpu, address);
        if (index < 0) return false;
        binding = &bindings[index];
    }
    const GcnRelSection* section = &catalog[index];
    u32 offset = address - binding->start;
    u32 lo = 0, hi = section->chunk_count;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2u;
        const GcnRelChunk* chunk = &section->chunks[mid];
        if (offset < chunk->start) hi = mid;
        else if (offset >= chunk->end) lo = mid + 1u;
        else {
            u32 previous_bias = cpu->native_pc_bias;
            cpu->native_pc_bias = binding->start - section->virtual_base;
            chunk->function(cpu);
            cpu->native_pc_bias = previous_bias;
            stats.dispatches++;
            return true;
        }
    }
    return false;
}
void gcn_rel_invalidate(u32 address, u32 size) {
    if (!catalog || !size) return;
    u32 offset;
    if (address < GC_MAIN_RAM_SIZE) offset = address;
    else if (address >= GC_RAM_BASE && address - GC_RAM_BASE < GC_MAIN_RAM_SIZE) offset = address - GC_RAM_BASE;
    else if (address >= GC_RAM_UNCACHED && address - GC_RAM_UNCACHED < GC_MAIN_RAM_SIZE) offset = address - GC_RAM_UNCACHED;
    else return;
    u64 end = (u64)offset + size;
    if (end > GC_MAIN_RAM_SIZE) end = GC_MAIN_RAM_SIZE;
    const u64 first_byte=GC_RAM_BASE+(u64)offset, last_byte=GC_RAM_BASE+end;
    for (u32 p = offset >> PAGE_SHIFT; p <= (end - 1u) >> PAGE_SHIFT; ++p) {
        const PageOwners* page=&owners[p];
        if (!owner_words[p]) continue;
        for (u32 slots=owner_words[p]; slots; slots&=slots-1u) {
            u32 slot=first_bit(slots);
            for (u32 entries=page->sections[slot]; entries; entries&=entries-1u) {
                u32 i=slot*32u+first_bit(entries);
                if (bindings[i].bound && bindings[i].start < last_byte &&
                    (u64)bindings[i].start + catalog[i].size > first_byte) bindings[i].dirty = true;
            }
        }
        stats.invalidations++;
    }
}
