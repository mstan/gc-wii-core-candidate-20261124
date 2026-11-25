/* SPDX-License-Identifier: MIT */
#ifndef GCN_IPL_GX_H
#define GCN_IPL_GX_H
#include "cpu/cpu.h"
int gcn_ipl_gx_tev_order(CPUState* cpu);
int gcn_ipl_gx_tex_load(CPUState* cpu);
int gcn_ipl_gx_su(CPUState* cpu);
int gcn_ipl_gx(CPUState* cpu);
#endif
