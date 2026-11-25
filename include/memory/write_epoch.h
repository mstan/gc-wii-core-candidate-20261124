/* SPDX-License-Identifier: MIT */
#ifndef GCN_MEMORY_WRITE_EPOCH_H
#define GCN_MEMORY_WRITE_EPOCH_H
#include "cpu/cpu.h"
/* MEM1 allocation has the same zeroed, writable, stable-pointer contract.
 * Native Windows uses OS write-watch; the portable allocation remains calloc.
 * Epoch queries are synchronous on the emulation owner thread. A false result
 * requires the consumer to read/hash actual bytes instead of reusing a cache. */
u8* gcn_mem1_alloc(u32 size);
void gcn_mem1_release(u8* ram);
bool gcn_mem_write_epoch(CPUState*,u32 address,u32 size,u64* epoch);
#endif
