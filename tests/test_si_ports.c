/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "si/si.h"
#include <stdio.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static void direct(GcnSi* si, u32 port) {
    gcn_si_write(si, NULL, GCN_SI_BASE + GCN_SI_IOBUF, 0x40000000, 4);
    gcn_si_write(si, NULL, GCN_SI_BASE + GCN_SI_COMCSR, (port << 1) | GCN_SI_CSR_TSTART, 4);
}

int main(void) {
    GcnSi si;
    gcn_si_init(&si);
    const GcnSiPadInput first = {0x100, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    const GcnSiPadInput second = {0x200, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    CHECK(si.ch[0].connected && !si.ch[1].connected);
    CHECK(!gcn_si_set_port(&si, GCN_SI_CHANNELS, &second));
    gcn_si_set_input(&si, &first);
    CHECK(gcn_si_debug_set_port(1, &second));
    gcn_si_beam_poll(&si, GCN_SI_NUM_HALF_LINES_FOR_POLL, 0);
    const u32 hi[] = {0x01801122, 0x0280aabb};
    const u32 lo[] = {0x33445566, 0xccddeeff};
    for (u32 port = 0; port < 2; port++) {
        CHECK(si.ch[port].rdst);
        CHECK(gcn_si_read(&si, NULL, GCN_SI_BASE + port * GCN_SI_CH_STRIDE + 4, 4) == hi[port]);
        CHECK(gcn_si_read(&si, NULL, GCN_SI_BASE + port * GCN_SI_CH_STRIDE + 8, 4) == lo[port]);
        direct(&si, port);
        CHECK(!(si.comcsr & GCN_SI_CSR_COMERR));
        CHECK(gcn_si_read(&si, NULL, GCN_SI_BASE + GCN_SI_IOBUF, 4) == hi[port]);
        CHECK(gcn_si_read(&si, NULL, GCN_SI_BASE + GCN_SI_IOBUF + 4, 4) == lo[port]);
    }
    CHECK(gcn_si_debug_set_port(1, NULL));
    direct(&si, 1);
    CHECK((si.comcsr & GCN_SI_CSR_COMERR) && si.ch[1].norep);
    direct(&si, 0);
    CHECK(!(si.comcsr & GCN_SI_CSR_COMERR));
    CHECK(gcn_si_read(&si, NULL, GCN_SI_BASE + GCN_SI_IOBUF, 4) == hi[0]);
    return 0;
}
