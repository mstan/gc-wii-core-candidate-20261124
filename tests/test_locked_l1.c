/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cpu/cpu.h"
#include "cpu/native_code.h"
#include "memory/memory.h"

#include <stdio.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #expr); \
        return 1; \
    } \
} while (0)

int main(void) {
    CPUState cpu;
    CHECK(cpu_init(&cpu));
    u32 l1_size = 0;
    u8* l1 = gcn_mem_locked_l1(&cpu, &l1_size);
    CHECK(l1 != NULL);
    CHECK(l1_size == GCN_L1_CACHE_SIZE);

    u32 avail = 0;
    u8* first = gcn_mem_resolve(&cpu, GCN_L1_CACHE_BASE, &avail);
    CHECK(first == l1);
    CHECK(avail == GCN_L1_CACHE_SIZE);
    CHECK(gcn_mem_resolve(
              &cpu, GCN_L1_CACHE_BASE + GCN_L1_CACHE_SIZE - 1u, &avail) ==
          l1 + GCN_L1_CACHE_SIZE - 1u);
    CHECK(avail == 1u);
    CHECK(gcn_mem_resolve(
              &cpu, GCN_L1_CACHE_BASE + GCN_L1_CACHE_SIZE, &avail) == NULL);

    mem_write8(&cpu, GCN_L1_CACHE_BASE + 3u, 0xA5u);
    mem_write16(&cpu, GCN_L1_CACHE_BASE + 4u, 0x1234u);
    mem_write32(&cpu, GCN_L1_CACHE_BASE + 8u, 0x89ABCDEFu);
    mem_write64(&cpu, GCN_L1_CACHE_BASE + 16u, 0x0123456789ABCDEFull);
    CHECK(mem_read8(&cpu, GCN_L1_CACHE_BASE + 3u) == 0xA5u);
    CHECK(mem_read16(&cpu, GCN_L1_CACHE_BASE + 4u) == 0x1234u);
    CHECK(mem_read32(&cpu, GCN_L1_CACHE_BASE + 8u) == 0x89ABCDEFu);
    CHECK(mem_read64(&cpu, GCN_L1_CACHE_BASE + 16u) ==
          0x0123456789ABCDEFull);
    CHECK(cpu.ram[3] == 0u);

    /* The SDK's THP decoder publishes locked-cache blocks through DMAU/DMAL.
     * Exercise both halves of the split length, zero=128 blocks, both banks,
     * the cache wrap and writes to watched MEM1 native code. */
    CHECK(cpu_alloc_mem2(&cpu, 8192));
    const u32 blocks[] = {1, 3, 4, 5, 127, 128};
    for (unsigned bank = 0; bank < 2; ++bank) {
        const u32 address = bank ? 0x10000000u : 0x2000u;
        for (unsigned n = 0; n < sizeof blocks / sizeof *blocks; ++n) {
            const u32 count = blocks[n], cache = GCN_L1_CACHE_SIZE - 32;
            for (u32 i = 0; i < count * 32; i += 4) {
                mem_write32(&cpu, address + i, 0x12340000u + i + bank, 0x1000);
                mem_write32(&cpu, GCN_L1_CACHE_BASE + ((cache + i) & 0x3ffffu), 0, 0x1000);
            }
            ppc_mtspr(&cpu, 922, address | ((count & 127) >> 2), 0x1000);
            u32 lower = cache | ((count & 3) << 2) | 0x10;
            ppc_mtspr(&cpu, 923, lower, 0x1000);
            CHECK(mem_read32(&cpu, GCN_L1_CACHE_BASE + cache, 0x1000) == 0); /* not triggered */
            ppc_mtspr(&cpu, 923, lower | 2, 0x1000);
            CHECK(ppc_mfspr(&cpu, 923, 0x1000) == lower);
            for (u32 i = 0; i < count * 32; i += 4) {
                CHECK(mem_read32(&cpu, GCN_L1_CACHE_BASE + ((cache + i) & 0x3ffffu), 0x1000) ==
                      0x12340000u + i + bank);
                mem_write32(&cpu, address + i, 0, 0x1000);
            }
            gcn_native_code_reset();
            if (!bank) gcn_native_code_watch(address, count * 32);
            cpu.reserve_valid = true; cpu.reserve_addr = address;
            ppc_mtspr(&cpu, 923, (lower & ~0x10u) | 2, 0x1000);
            CHECK(!cpu.reserve_valid);
            if (!bank) CHECK(gcn_native_code_is_invalid(address));
            for (u32 i = 0; i < count * 32; i += 4)
                CHECK(mem_read32(&cpu, address + i, 0x1000) == 0x12340000u + i + bank);
        }
    }

    cpu_reset(&cpu);
    CHECK(gcn_mem_locked_l1(&cpu, &l1_size) == l1);
    CHECK(l1_size == GCN_L1_CACHE_SIZE);
    CHECK(mem_read32(&cpu, GCN_L1_CACHE_BASE + 8u) == 0u);

    cpu_free(&cpu);
    CHECK(gcn_mem_locked_l1(&cpu, &l1_size) == NULL);
    CHECK(l1_size == 0u);
    puts("locked L1 memory: PASS");
    return 0;
}
