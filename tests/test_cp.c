/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cp/cp.h"
#include <stdio.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);return 1;}}while(0)
static int level;
static unsigned raises;
static void irq(void* user,int value) {(void)user;if(value && !level)++raises;level=value;}
static void write16(GcnCp* cp,u32 offset,u16 value) {gcn_cp_write(cp,NULL,GCN_CP_BASE+offset,value,2);}
static void pointer(GcnCp* cp,u32 offset,u32 value) {
    write16(cp,offset+2,(u16)(value>>16));write16(cp,offset,(u16)value);
}
static u16 status(GcnCp* cp) {return (u16)gcn_cp_read(cp,NULL,GCN_CP_BASE+GCN_CP_STATUS,2);}
static int profile(bool wii,u32 base) {
    GcnCp cp;gcn_cp_init_model(&cp,wii);gcn_cp_set_irq(&cp,irq,NULL);level=0;raises=0;
    pointer(&cp,GCN_CP_FIFO_BASE_LO,base);pointer(&cp,GCN_CP_FIFO_END_LO,base+0x60);
    pointer(&cp,GCN_CP_FIFO_RPTR_LO,base);pointer(&cp,GCN_CP_FIFO_WPTR_LO,base+0x60);
    pointer(&cp,GCN_CP_FIFO_BP_LO,base+0x40);pointer(&cp,GCN_CP_FIFO_RW_DIST_LO,0x60);
    write16(&cp,GCN_CP_CTRL,GCN_CP_CTRL_GPREAD_ENABLE|GCN_CP_CTRL_BP_ENABLE|GCN_CP_CTRL_BP_INT);
    CHECK(!level && !gcn_cp_at_breakpoint(&cp));
    gcn_cp_gpu_consume_chunk(&cp);CHECK(!level && gcn_cp_fifo_read_pointer(&cp)==base+0x20);
    gcn_cp_gpu_consume_chunk(&cp);
    CHECK(level && raises==1 && gcn_cp_at_breakpoint(&cp));
    CHECK(status(&cp)&GCN_CP_STATUS_BREAKPOINT);
    CHECK(gcn_cp_fifo_rw_distance(&cp)==0x20 && gcn_cp_fifo_read_pointer(&cp)==base+0x40);
    /* Interrupt enable gates the line without changing the stopped pointer. */
    write16(&cp,GCN_CP_CTRL,GCN_CP_CTRL_GPREAD_ENABLE|GCN_CP_CTRL_BP_ENABLE);
    CHECK(!level && (status(&cp)&GCN_CP_STATUS_BREAKPOINT));
    write16(&cp,GCN_CP_CTRL,GCN_CP_CTRL_BP_ENABLE|GCN_CP_CTRL_BP_INT);
    CHECK(!level && (status(&cp)&GCN_CP_STATUS_BREAKPOINT));
    write16(&cp,GCN_CP_CTRL,GCN_CP_CTRL_GPREAD_ENABLE|GCN_CP_CTRL_BP_INT);
    CHECK(!level && !(status(&cp)&GCN_CP_STATUS_BREAKPOINT));
    gcn_cp_gpu_consume_chunk(&cp);CHECK(gcn_cp_fifo_read_pointer(&cp)==base+0x60);
    CHECK(gcn_cp_fifo_rw_distance(&cp)==0);
    /* Echoes enables at the already drained write pointer after GXFlush. */
    pointer(&cp,GCN_CP_FIFO_BP_LO,base+0x60);
    write16(&cp,GCN_CP_CTRL,GCN_CP_CTRL_GPREAD_ENABLE|GCN_CP_CTRL_BP_ENABLE|GCN_CP_CTRL_BP_INT);
    CHECK(level && raises==2 && (status(&cp)&GCN_CP_STATUS_READ_IDLE));
    pointer(&cp,GCN_CP_FIFO_BP_LO,base);CHECK(!level && !gcn_cp_at_breakpoint(&cp));
    /* A real chunk at END wraps to BASE and reaches the new breakpoint. */
    pointer(&cp,GCN_CP_FIFO_RW_DIST_LO,0x20);gcn_cp_gpu_consume_chunk(&cp);
    CHECK(level && raises==3 && gcn_cp_fifo_read_pointer(&cp)==base);
    CHECK(gcn_cp_fifo_rw_distance(&cp)==0);
    write16(&cp,GCN_CP_CTRL,0);CHECK(!level && !(status(&cp)&GCN_CP_STATUS_BREAKPOINT));
    return 0;
}
int main(void) {
    CHECK(!profile(false,0x00100000));CHECK(!profile(true,0x10100000));
    puts("CP FIFO breakpoint, drained enable, interrupt gating and wrap passed");return 0;
}
