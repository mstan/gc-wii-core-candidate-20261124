/* SPDX-License-Identifier: MIT */
#ifndef GCN_FIFO_SUBMIT_H
#define GCN_FIFO_SUBMIT_H
#include "cpu/cpu.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef int (*GcnFifoWriter)(void*,CPUState*,const u8*,u32);
/* One machine/CPU thread owns the registered gather pipe. A changed bus,
 * another CPU, or enabled detailed tracing retains the caller's ordinary
 * MMIO writes. Bytes are stable host-owned storage, not an alias of the
 * gather stage or guest FIFO RAM. A declined operation has no effects. */
void gcn_fifo_set_writer(CPUState* cpu,GcnFifoWriter writer,void* user);
int gcn_fifo_try_submit(CPUState* cpu,const u8* bytes,u32 size);
int gcn_gp_write_stream(void* user,CPUState* cpu,const u8* bytes,u32 size);
#ifdef __cplusplus
}
#endif
#endif
