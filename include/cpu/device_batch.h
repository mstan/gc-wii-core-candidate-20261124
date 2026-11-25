/* SPDX-License-Identifier: MIT */
#ifndef GCN_DEVICE_BATCH_H
#define GCN_DEVICE_BATCH_H
#include "cpu/cpu.h"
/* 8.43 us at the 486 MHz core clock. Dependent MMIO flushes sooner; the
 * maintained per-block LLE clock provider does not use this batching. */
#define GCN_NATIVE_DEVICE_QUANTUM 4096u
typedef void (*GcnDeviceBatchFn)(void*,u32 dsp_cycles,u32 core_cycles,u64 absolute_cycles);
typedef struct {
    void* user;GcnDeviceBatchFn execute;
    u32 dsp,core;u64 absolute,queued,flushed,calls;
} GcnDeviceBatch;
void gcn_device_batch_init(GcnDeviceBatch*,void*,GcnDeviceBatchFn);
void gcn_device_batch_add(GcnDeviceBatch*,u32 dsp,u32 core,u64 absolute,int force);
void gcn_device_batch_flush(GcnDeviceBatch*);
#endif
