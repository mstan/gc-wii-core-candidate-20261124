/* SPDX-License-Identifier: MIT */
#ifndef GCNLLR_SERVICE_IDENTITY_H
#define GCNLLR_SERVICE_IDENTITY_H
/* Compiled provider identity for diagnostics, never a runtime selector. */
#ifdef __cplusplus
extern "C" {
#endif
const char* gcn_service_identity(void);
#ifdef __cplusplus
}
#endif
#endif
