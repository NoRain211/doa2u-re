/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "live_backend.h"

/* The Xbox Live service live_xapi.c talks to. The runner installs the
   matchmaker client (live_client.c) at startup; tests install the in-memory
   backend (live_backend_local.c). NULL is Live off: the guest keeps its own
   library and the game stays offline. */
static const RecompLiveBackend *live_backend;

const RecompLiveBackend *recomp_live_backend(void)
{
    return live_backend;
}

void recomp_live_backend_set(const RecompLiveBackend *backend)
{
    live_backend = backend;
}
