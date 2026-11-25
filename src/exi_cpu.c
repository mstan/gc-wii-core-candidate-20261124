/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "exi/exi.h"
#include "memory/memory.h"
#include "cpu/native_code.h"
#include "debug/rings.h"

static u8* resolve(void* user, u32 address, u32* available) {
    return gcn_mem_resolve((CPUState*)user, address, available);
}

static void before_write(void* user, u32 address, u32 size) {
    (void)user;
    gcn_ring_watch_check_span(address, size, 0xEC1EC100u);
}

static void written(void* user, u32 address, u32 size) {
    (void)user;
    gcn_native_code_content_dirty(address, size);
}

static void trace_irq(void* user, int level) {
    (void)user;
    gcn_ring_event(level ? GCN_EV_IRQ_RAISE : GCN_EV_IRQ_CLEAR, 4u, 0u, 0u);
}

static void trace_card(void* user, u32 pc, u32 channel, u32 command, u32 rw,
                       bool dma, u32 address, u32 length, u32 data) {
    (void)user;
    gcn_ring_memcard(pc, (u8)channel, 1u, (u8)command, (u8)rw,
                     dma ? 1u : 0u, address, length, data);
}

static void install_trace(GcnExi* exi) {
    exi->trace_irq = trace_irq;
    exi->trace_card = trace_card;
    exi->trace_user = NULL;
}

u32 gcn_exi_read(void* user, CPUState* cpu, u32 addr, u8 size) {
    (void)cpu;
    install_trace((GcnExi*)user);
    return gcn_exi_bus_read((GcnExi*)user, addr, size);
}

void gcn_exi_write(void* user, CPUState* cpu, u32 addr, u32 value, u8 size) {
    install_trace((GcnExi*)user);
    const GcnExiAccess access = {
        cpu, resolve, before_write, written,
        cpu ? cpu->cycles : 0, cpu ? cpu->pc : 0
    };
    gcn_exi_bus_write((GcnExi*)user, &access, addr, value, size);
}
