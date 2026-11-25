/* SPDX-License-Identifier: MIT */
#ifndef GCNLLR_NATIVE_BOUNDARY_H
#define GCNLLR_NATIVE_BOUNDARY_H
#include "cpu/cpu.h"

/* Read-only bindings to the owning machine's live latches. Bind after machine
 * initialization, and bind again if their owner is replaced. No flags here
 * acknowledge interrupts, suppress observations or manufacture a quiet CPU. */
typedef struct {
    const u32 *pi_cause,*pi_mask;
    const bool* decrementer_pending;
    const int *cosim,*checkpoint_armed,*checkpoint_field,*checkpoint_gpr,*checkpoint_lr;
    bool bound;
} GcnNativeBoundary;
bool gcn_native_boundary_bind(GcnNativeBoundary*);
bool gcn_pi_native_boundary(GcnNativeBoundary*);
const bool* ppc_native_decrementer_pending(void);

static inline bool gcn_native_boundary_quiet(const GcnNativeBoundary* b,const CPUState* cpu) {
    if (!b->bound || cpu->exception || cpu->halted || *b->cosim) return false;
    if (*b->checkpoint_armed &&
        (!*b->checkpoint_field || *b->checkpoint_gpr || *b->checkpoint_lr)) return false;
    return !(cpu->msr&0x8000u) || (!*b->decrementer_pending &&
        (__atomic_load_n(b->pi_cause,__ATOMIC_ACQUIRE)&*b->pi_mask)==0);
}
#endif
