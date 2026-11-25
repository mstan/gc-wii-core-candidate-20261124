/* SPDX-License-Identifier: MIT */
#ifndef GCN_IPL_WAIT_H
#define GCN_IPL_WAIT_H
#include "cpu/cpu.h"
/* Native replacement for the verified IPL event wait, selected at build time.
 * The slow implementation is the original compiled three-instruction loop.
 * Returns zero without changing CPU state outside the supported idle case. */
int gcn_ipl_wait(CPUState* cpu);
u64 gcn_ipl_wait_count(void);
#endif
