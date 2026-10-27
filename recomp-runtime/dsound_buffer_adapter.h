/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_DSOUND_BUFFER_ADAPTER_H
#define DOA2U_RECOMP_DSOUND_BUFFER_ADAPTER_H

#include "runtime.h"

#ifdef __cplusplus
extern "C" {
#endif
RecompFunction recomp_dsound_buffer_lookup_manual(uint32_t address);
void recomp_dsound_buffers_pump(void);
void recomp_dsound_buffers_refresh_routing(void);
/* Reset buffers before releasing/resetting their device owner. */
void recomp_dsound_buffers_reset(void);
/* Shared private AddRef dispatch: root, not the public root+0x1C handle. */
int recomp_dsound_buffer_addref_root(uint32_t root, uint32_t *references);
size_t recomp_dsound_buffer_state_size(void);
void recomp_dsound_buffer_state_save(void *state);
void recomp_dsound_buffer_state_restore(const void *state);
/* Hidden run-ahead frames advance the buffers without output. */
void recomp_dsound_buffer_set_output_suppressed(bool suppressed);
#ifdef __cplusplus
}
#endif
#endif
