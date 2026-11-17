# Xbox Live

Status: with the Live fight wiring in place, the game's own Xbox Live pages
run on the PC port against the project's matchmaker, and Live fights run on
the rollback netcode. Two scripted runners on one machine have logged on,
found each other with Quick Match, met in the game's lobby and played three
Team Battle matches without a desync. Friends, invites, rankings and two
machines across the internet have only run in unit tests. Voice chat is not
supported.

## Turning it on

Live is off unless the runner starts with `RECOMP_LIVE=1`. It is not offered
when the runner hosts, joins or replays a room session from the command
line. When Live is off, the Live pages report no account, as before. The
start-up that reads `RECOMP_LIVE` and installs the backend comes with the Live
fight wiring, so the switch does nothing yet.

- **Matchmaker.** Live uses the matchmaker from NETPLAY SETTINGS, or the
  built-in one when that is empty. `RECOMP_LIVE_SERVER=host:port` (or just
  `host`) overrides it.
- **Identity.** The first logon writes a random 32-byte secret to
  `.recomp-storage/live-identity.bin`. The account's XUID and the proof the
  matchmaker checks both come from it, so copying the file moves the account
  and losing it loses the account. `RECOMP_LIVE_IDENTITY=<file>` uses
  another file, which is how two runners on one machine get two accounts.

## How it fits

The game links Microsoft's XOnline and XNet libraries statically. The port
replaces them at the API boundary, like the D3D8 and DirectSound adapters:

- `recomp-runtime/live_xapi.c` binds the guest's XOnline and XNet entry
  points (`recomp_live_lookup`, reached from `recomp_lookup_manual`). It
  writes the records the game reads (logon users, search results, friends,
  stats, QoS) and runs the game's asynchronous tasks.
- `recomp-runtime/live_backend.h` is the service those calls use. The
  matchmaker client (`live_client.c`) implements it for play;
  `live_backend_local.c` implements it in memory for tests.
- `recomp-runtime/live_client.c` speaks the matchmaker's Live packets,
  kinds 12 and up ([matchmaking](matchmaking.md#xbox-live)). CONNECT repeats
  with the same request id about once a second, for up to a minute, until a
  checked packet from the other player arrives. Each repeat has the
  matchmaker send both LPEERs again, so a lost packet on either side does not
  hang a join. A host learns the joiner from that LPEER and answers with one
  CONNECT of its own, as it does for a new pairing with a known player (a
  rematch after the old pairing lapsed); the matchmaker grants each side the
  other's stats only after both asked.
- `recomp-runtime/live_transport.c` owns one UDP socket for the matchmaker,
  the game's own datagrams and the rollback session. Each pairing's LPEER
  carries a random pair token, which both players' punches repeat, so a past
  partner cannot move a path or end a session. A punch is its own datagram
  kind and never reaches the game, whatever its token. Two players punch through to
  each other and fall back to the matchmaker's relay after 5 s, which carries
  the largest rollback packet (1,185 bytes); a larger game datagram on the
  relay fails with `WSAEMSGSIZE`. When the game has sent to a player and
  heard nothing checked back for 10 s, the transport asks the matchmaker for
  that player again, so a host whose game restarted elsewhere is found anew;
  a quiet player the game is not sending to is left alone. An introduction
  the game did not ask for never takes the slot of a player heard in the
  last 10 s.

## Fights

`recomp-runtime/live_fight.c` hooks the game's Live fight. The game's
first-round start (`battle_online_round_start`) runs as on the Xbox,
including its ready handshake. The rollback session starts on the first
frame after it returns, in `RollbackMode::Live`
(`recomp-runtime/netplay_session.cpp`), and replaces the game's delay-based
lockstep for the rest of the match. The game's lockstep stop rides a pad
bit so both sides end the session on the same frame. The desync check
hashes fight state only, since the Live pages around the fight differ per
side. A desync, a timeout or a failed rewind ends the session without
stopping the runner; the other player gets a goodbye and the game announces
that a player left. See [rollback netcode](rollback-netcode.md) for how the
game's own lockstep works.

## Limits

- No voice: the voice engine's stream creation fails and the game carries on.
- QoS reports a fixed 50 ms round trip, which gives the game's shipped
  lockstep delay; a measured one needs a backend call.
- The Versus menu's room still exists next to Live and is due to go.
- Stat writes for an opponent are not tied to a played match: the client
  CONNECTs back to anyone the matchmaker introduces, so whoever CONNECTs to a
  host may write that host's stats for six hours. Each player writes their own
  stats anyway, so the rankings trust the clients.

## Testing

The Live unit tests are the ctest entries `recomp-runtime-live-xapi`,
`-live-transport`, `-live-client` and `-live-fight`, and
`tools/test_matchmaker.py`. The client test and the
Python test share the byte transcript in `tools/live_vectors.json`.

Once the Live fight wiring lands, two runners on one machine need a local
matchmaker and two play folders, each with its own saves:

    python tools/matchmaker.py --address 127.0.0.1 --port 47790 --store live-store.json

Start each runner with `RECOMP_LIVE=1`,
`RECOMP_LIVE_SERVER=127.0.0.1:47790` and its own `RECOMP_LIVE_IDENTITY`.
