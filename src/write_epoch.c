/* SPDX-License-Identifier: MIT */
#include "memory/write_epoch.h"
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32) && defined(GCNLLR_WRITE_WATCH)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define MAX_PAGES (GC_MAIN_RAM_SIZE/4096u)
static u8* watched_ram;
static u32 watched_size,page_size,owner_thread;
static u64 serial,versions[MAX_PAGES];
static void* dirty_pages[MAX_PAGES];
static bool valid_epochs;
#endif
u8* gcn_mem1_alloc(u32 size) {
#if defined(_WIN32) && defined(GCNLLR_WRITE_WATCH)
    if (!watched_ram) {
        SYSTEM_INFO info;GetSystemInfo(&info);
        if (size==GC_MAIN_RAM_SIZE && info.dwPageSize>=4096 && !(size%info.dwPageSize)) {
            u8* memory=VirtualAlloc(NULL,size,MEM_RESERVE|MEM_COMMIT|MEM_WRITE_WATCH,PAGE_READWRITE);
            if (memory) {
                watched_ram=memory;watched_size=size;page_size=info.dwPageSize;
                owner_thread=GetCurrentThreadId();serial=1;valid_epochs=true;
                memset(versions,0,sizeof(versions));return memory;
            }
        }
    }
#endif
    return calloc(1,size);
}
void gcn_mem1_release(u8* ram) {
#if defined(_WIN32) && defined(GCNLLR_WRITE_WATCH)
    if (ram && ram==watched_ram) {
        VirtualFree(ram,0,MEM_RELEASE);watched_ram=NULL;watched_size=0;valid_epochs=false;return;
    }
#endif
    free(ram);
}
bool gcn_mem_write_epoch(CPUState* cpu,u32 address,u32 size,u64* epoch) {
#if defined(_WIN32) && defined(GCNLLR_WRITE_WATCH)
    if (!cpu || !epoch || !size || !valid_epochs || cpu->ram!=watched_ram ||
        GetCurrentThreadId()!=owner_thread) return false;
    u32 offset;
    if (address<watched_size) offset=address;
    else if (address>=GC_RAM_BASE && address-GC_RAM_BASE<watched_size) offset=address-GC_RAM_BASE;
    else if (address>=GC_RAM_UNCACHED && address-GC_RAM_UNCACHED<watched_size) offset=address-GC_RAM_UNCACHED;
    else return false;
    if (size>watched_size-offset) return false;
    const u32 first=offset/page_size,last=(offset+size-1u)/page_size;
    ULONG_PTR count=MAX_PAGES;DWORD granularity=0;
    const UINT result=GetWriteWatch(WRITE_WATCH_FLAG_RESET,watched_ram+(size_t)first*page_size,
        (size_t)(last-first+1u)*page_size,dirty_pages,&count,&granularity);
    if (result || granularity!=page_size) {valid_epochs=false;return false;}
    if (count) {
        if (++serial==0) {valid_epochs=false;return false;}
        for (ULONG_PTR i=0;i<count;i++) {
            const size_t p=((u8*)dirty_pages[i]-watched_ram)/page_size;
            if (p>=MAX_PAGES) {valid_epochs=false;return false;}
            versions[p]=serial;
        }
    }
    u64 latest=1;
    for (u32 page=first;page<=last;page++) if (versions[page]>latest) latest=versions[page];
    *epoch=latest;return true;
#else
    (void)cpu;(void)address;(void)size;(void)epoch;return false;
#endif
}
