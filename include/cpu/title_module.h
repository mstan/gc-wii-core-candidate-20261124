/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef GCN_TITLE_MODULE_H
#define GCN_TITLE_MODULE_H

#include "cpu/cpu.h"

/* Optional, content-validated title AOT module. The LLE boot path remains the
 * sole authority for loading bytes into guest RAM; this layer only accelerates
 * a PC after those live bytes match the module's immutable input. */
bool gcn_title_module_call(CPUState* cpu, u32 address);
#if defined(GCN_TITLE_DIRECT_DISPATCH) && GCN_TITLE_DIRECT_DISPATCH
bool gcn_title_module_direct(CPUState* cpu, u32 address);
#endif
void gcn_title_module_icbi(u32 address, u32 size);

#endif
