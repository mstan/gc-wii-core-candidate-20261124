/* SPDX-License-Identifier: MIT */
#ifndef GCN_IPL_MATH_H
#define GCN_IPL_MATH_H
#include "cpu/cpu.h"
/* Native SDK caller contract: aligned ordinary MEM1 float 3x4 matrices;
 * copy r3 -> r4; concatenate r3,r4 -> r5; return to LR. Nonvolatile EABI
 * registers are preserved. Caller-saved registers and dead stack bytes are
 * unspecified. Unsupported FP/layout/identity cases leave CPU state alone. */
int gcn_ipl_math(CPUState*);
/* Operation body shared with the ROM-free caller-contract check. */
int gcn_ipl_matrix_service(CPUState*,int concatenate);
/* IPL Euler rotation/translation: angles r3/r4/r5, translation f1/f2/f3,
 * output matrix r6. Reads the actual guest sine/cosine tables. */
int gcn_ipl_transform_service(CPUState*);
#endif
