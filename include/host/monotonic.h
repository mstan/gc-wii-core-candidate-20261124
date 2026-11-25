/* SPDX-License-Identifier: MIT */
#ifndef GCN_HOST_MONOTONIC_H
#define GCN_HOST_MONOTONIC_H
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static inline double gcn_host_seconds(void) {
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    return (double)now.QuadPart / (double)frequency.QuadPart;
}
#else
#include <time.h>
static inline double gcn_host_seconds(void) {
    struct timespec now;
#ifdef CLOCK_MONOTONIC
    clock_gettime(CLOCK_MONOTONIC, &now);
#else
    timespec_get(&now, TIME_UTC);
#endif
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}
#endif
#endif
