/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_LIVE_XAPI_H
#define DOA2U_RECOMP_LIVE_XAPI_H

/* The guest's statically linked XOnline/XNet services, replaced at the API
   boundary and carried by the RecompLiveBackend (live_backend.h, see
   docs/xbox-live.md). With no backend the lookup returns NULL and the game
   keeps its own library and stays offline. */
#include "runtime.h"

#include <stdint.h>

/* guest_address is an XOnline/XNet export thunk; NULL for every other address
   and while Live is off. Never blocks: a task starts a backend request that
   XOnlineTaskContinue polls. Install the backend (recomp_live_backend_set)
   before the guest starts, as the runner does; the indirect-call cache keeps
   the lookup it resolved first, so a later change also needs
   recomp_program_lookup_reset(). */
RecompFunction recomp_live_lookup(uint32_t guest_address);

/* The guest winsock/XNet transport group (socket, bind, ioctlsocket,
   closesocket, shutdown, recvfrom, sendto, WSAGetLastError,
   XNetXnAddrToInAddr, XNetInAddrToXnAddr). live_transport.c defines it; the
   guest API forwards those addresses to it. */
RecompFunction recomp_live_transport_lookup(uint32_t guest_address);

/* Once per frame while Live is on: resend, time out, read pushed invites and
   presence, and finish calls the game does not poll (friend operations). */
void recomp_live_pump(uint64_t now_ms);

#endif
