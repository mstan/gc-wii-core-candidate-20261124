/* SPDX-License-Identifier: MIT */
#ifndef GCNLLR_REL_MODULE_H
#define GCNLLR_REL_MODULE_H
#include "cpu/cpu.h"

typedef void (*GcnRelFunction)(CPUState*);
typedef struct {
    u32 start, end; /* section-relative bytes */
    GcnRelFunction function;
} GcnRelChunk;
typedef struct {
    u32 module_id, section, size, virtual_base;
    const char* name;
    const u32* words;
    const u32* masks; /* immutable bits; relocated operand bits are executed live */
    const GcnRelChunk* chunks;
    u32 chunk_count;
} GcnRelSection;
typedef struct {
    u64 dispatches, bindings, verifications, mismatches, invalidations;
} GcnRelStats;
bool gcn_rel_call(CPUState* cpu, u32 address);
void gcn_rel_invalidate(u32 address, u32 size);
void gcn_rel_reset(void);
void gcn_rel_set_catalog(const GcnRelSection* sections, u32 count);
void gcn_rel_stats(GcnRelStats* out);

/* Direct fetch of an already validated relocation operand. This is neither
 * opcode dispatch nor interpreter execution; the native body fixes semantics. */
static inline u32 gcn_rel_instruction_word(CPUState* cpu, u32 address) {
    const u8* p = cpu->ram + (address - GC_RAM_BASE);
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}
#endif
