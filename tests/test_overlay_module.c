/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cpu/overlay_module.h"
#include "cpu/aot_module.h"
#include <stdio.h>
static int failures;
#define CHECK(x) do { if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);failures++;}}while(0)
static void body(CPUState* cpu){cpu->lr=cpu->pc+4;cpu->pc+=4;}
int main(void){
    CPUState cpu; if(!cpu_init(&cpu))return 1;
    const GcnOverlayPage pages[]={{0x1000,0x2000,0,1},{0x80001000,0x80002000,0,1},{0xc0001000,0xc0002000,0,1}};
    GcnOverlayVariant variant={gcn_aot_fnv1a64(cpu.ram+0x1000,4096),body};
    u32 resident[]={GCN_OVERLAY_UNKNOWN,GCN_OVERLAY_UNKNOWN,GCN_OVERLAY_UNKNOWN};
    GcnOverlayModule module={pages,&variant,resident,3,1};
    for(unsigned i=0;i<3;i++){
        cpu.pc=pages[i].start+8;
        CHECK(gcn_overlay_module_call(&module,&cpu,cpu.pc));
        CHECK(cpu.pc==pages[i].start+12 && cpu.lr==pages[i].start+12);
    }
    CHECK(module.pages_resident==3 && module.hashes==3);
    cpu.ram[0x1000]=1;
    gcn_overlay_module_invalidate(&module,0xc0001000,32);
    CHECK(module.pages_resident==0);
    for(unsigned i=0;i<3;i++){
        cpu.pc=pages[i].start+8;
        CHECK(!gcn_overlay_module_call(&module,&cpu,cpu.pc));
        CHECK(cpu.pc==pages[i].start+8);
    }
    /* A cached-only table cannot substitute its body for a physical PC. */
    GcnOverlayModule cached={pages+1,&variant,resident+1,1,1};
    resident[1]=GCN_OVERLAY_UNKNOWN;cpu.ram[0x1000]=0;cpu.pc=0x1008;
    CHECK(!gcn_overlay_module_call(&cached,&cpu,cpu.pc));
    CHECK(cpu.pc==0x1008);
    cpu_free(&cpu);return failures?1:0;
}
