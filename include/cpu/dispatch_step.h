/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef GCN_CPU_DISPATCH_STEP_H
#define GCN_CPU_DISPATCH_STEP_H
#include "cpu/interpreter.h"
#include <stdio.h>

/* Shared by both dispatcher accounting paths. A constant native callback is
 * inlined by the optimized build; no runtime backend selection is introduced. */
static inline int gcn_cpu_terminal(CPUState* cpu,int* failure) {
    if (!cpu->halted) return 0;
    *failure=1;
    fprintf(stderr,"gcnllr: terminal CPU/device failure at PC=0x%08X reason=%d\n",cpu->pc,cpu->halt_reason);
    return 1;
}
static inline int gcn_cpu_dispatch_step(CPUState* cpu, bool cosim,
                                       int (*native)(CPUState*), int* strict_failure) {
    if (gcn_cpu_terminal(cpu,strict_failure)) return 0;
    if (!cosim) {
        int executed=native(cpu);
        if (gcn_cpu_terminal(cpu,strict_failure)) return 0;
        if (executed) return 1;
        gcn_interpreter_note_native_miss(cpu);
    }
#ifdef GCNLLR_STATIC_CPU
    *strict_failure = 1;
    fprintf(stderr, "gcnllr: STATIC CPU coverage miss at PC=0x%08X; no interpreter execution\n", cpu->pc);
    return 0;
#else
    (void)strict_failure;
    gcn_interpreter_set_cache_loop_batch_enabled(!cosim);
    return gcn_interpreter_step(cpu);
#endif
}
#endif
