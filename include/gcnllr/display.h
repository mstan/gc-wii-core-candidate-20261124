/* SPDX-License-Identifier: MIT */
#ifndef GCNLLR_DISPLAY_H
#define GCNLLR_DISPLAY_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#define GCNLLR_DISPLAY_ABI 2u
typedef struct {
    uint32_t abi;
    const char* title;
    const char* data_directory;
    const char* cache_directory;
    uint32_t hidden;
} GcnllrDisplayConfig;
typedef struct {
    uint32_t buttons;
    uint8_t stick_x, stick_y, substick_x, substick_y, trigger_l, trigger_r;
} GcnllrPad;
typedef struct {
    uint64_t published, presented, coalesced;
    uint32_t failed, quit;
    GcnllrPad pad;
    uint32_t readback_ready;
    uint32_t sample_argb[3];
} GcnllrDisplayStatus;
/* Immutable C ABI across the MinGW CPU and MSVC-ABI Aurora builds. No host
 * allocations, exceptions, C++ types or device internals cross this boundary.
 * publish synchronously takes ownership of a copy, never of the caller's RAM.
 * status reports actual submissions; repeated host draws are not game frames. */
typedef struct {
    uint32_t abi;
    int (*start)(const GcnllrDisplayConfig*);
    int (*publish)(const uint8_t* yuy2, uint32_t width, uint32_t height, uint32_t stride);
    void (*status)(GcnllrDisplayStatus*);
    void (*stop)(void);
} GcnllrDisplayApi;
typedef const GcnllrDisplayApi* (*GcnllrDisplayGetApi)(uint32_t abi);
#ifdef __cplusplus
}
#endif
#endif
