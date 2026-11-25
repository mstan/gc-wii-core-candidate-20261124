/* SPDX-License-Identifier: MIT */
#ifndef GCNLLR_FAST_TITLE_H
#define GCNLLR_FAST_TITLE_H
#include "cpu/cpu.h"
/* Optional build-selected native module. It shares the canonical machine
 * state and bus; a declined entry continues through the maintained floor. */
bool gcn_fast_title_call(CPUState* cpu,u32 address);
void gcn_fast_title_invalidate(u32 address,u32 size);
/* Read-only boundary guards for a native caller that may retain its C frame. */
bool gcn_pi_external_pending(void);
bool ppc_decrementer_pending(void);
bool gcn_debug_native_can_skip(void);
#endif
