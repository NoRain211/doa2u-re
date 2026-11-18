# Xbox Live

Status: the game's own Xbox Live pages run on the PC port against the
project's matchmaker, and Live fights run on the rollback netcode. Two
scripted runners on one machine have logged on, found each other with Quick
Match, met in the game's lobby and played three Team Battle matches without
a desync. Friends, invites, rankings and two machines across the internet
have only run in unit tests. Voice chat is not supported.

## Turning it on

Live is on unless the runner starts with `RECOMP_LIVE=0`. Off, the Live pages
report no account, as before. A Loopback test session (`RECOMP_ROLLBACK=loopback`)
keeps Live off.

- **Matchmaker.** Live uses the built-in matchmaker.
  `RECOMP_LIVE_SERVER=host:port` (or just `host`) overrides it.
- **Identity.** The first start writes a random 32-byte secret to
  `.recomp-storage/live-identity.bin`. The account's XUID, the proof that
  registers it and the key that answers the matchmaker's challenge at every
  logon all come from it ([Accounts](matchmaking.md#accounts)), so copying
  the file moves the account and losing it loses the account. Anyone who
  reads the file can log on as the account. `RECOMP_LIVE_IDENTITY=<file>` uses
  another file, which is how two runners on one machine get two accounts.
  Keep the identity file, and any `RECOMP_LIVE_IDENTITY` file, out of shared or synced
  folders and bug reports. The runner creates a new one readable by the
  player alone (0600) on Linux and macOS; on Windows the file takes its
  folder's permissions. It never changes the permissions of a file that is
  already there.
- **Known servers.** After the first logon on a matchmaker, the runner adds
  its `host:port` as a line to `live-identity.bin.servers.txt` beside the
  identity file (the identity file's name and `.servers.txt`), and never
  sends that server the proof again. Keep the list with the identity file
  when you move the account. If a matchmaker lost its accounts, the logon
  fails and stderr names the line to remove from that file to register
  again; that registration sends the proof once more. A list that is there
  but cannot be read counts as naming every server, so a matchmaker that
  says "no account" is refused the same way until the file can be read.
- **What the server sees.** Nothing goes out until the game's first Live
  request. A logon sends the player's IP address and port, XUID, build
  digest and profile name (the gamertag). Any player who joins a host, or
  CONNECTs to it, gets the host's IP address and port, as on the Xbox.

## Online and offline

The run switches itself at the player's logon. Booting starts offline: the
game's offline pages play with the player's mods, a free-running clock and
ordinary disc reads. The account page's `XOnlineLogon` (bound as
`live_logon`) switches to online mode at the next presented frame. Leaving
DOA ONLINE ends `online_logoff` (`0x2a4010`) with
`XOnlineTaskClose(g_online_logon_task)`, whose binding `live_task_close`
switches back; a failed logon the game then closes counts as leaving.

Online, what the rollback fights need is on: guest time is locked to the
frame, disc reads are synchronous and DirectSound buffers follow the same
frame clock. The free camera, random CPU costumes and the Dynamic KO replay
stay off, since they change one game's memory or timing. The fight HUD and
the hit voices are not forced off: each player's PC SETTINGS row decides,
online too, because neither writes guest memory the desync check hashes and
the hit voices are host audio only. Leaving puts the rest back, honouring the
player's settings.

A Loopback (`RECOMP_ROLLBACK=loopback`) or synctest run stays online for the
whole run.

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
side. It also leaves out what each player's own widescreen and 5.1 choices
shape inside the fight block (the camera's framing, field of view and
matrices, which the 16:9 camera sets differently, and the music track), so
players with different settings can fight each other. A desync, a timeout or a failed rewind ends the session without
stopping the runner; the other player gets a goodbye and the game announces
that a player left. A fight waits 15 s for a silent opponent before it ends.
A save the game writes mid-fight waits for the other player's input; if
that input proves a frame before the save mispredicted, the runner stops
(`rollback:save-mispredicted`), since the save cannot be taken back. See [rollback netcode](rollback-netcode.md) for how the
game's own lockstep works.

## Limits

- No voice: the voice engine's stream creation fails and the game carries on.
- QoS reports a fixed 50 ms round trip, which gives the game's shipped
  lockstep delay; a measured one needs a backend call.
- Stat writes for an opponent are not tied to a played match: the client
  CONNECTs back to anyone the matchmaker introduces, so whoever CONNECTs to a
  host may write that host's stats for six hours. Each player writes their own
  stats anyway, so the rankings trust the clients.

## Testing

The Live unit tests are the ctest entries `recomp-runtime-live-xapi`,
`-live-transport`, `-live-identity`, `-live-client` and `-live-fight`, and
`tools/test_matchmaker.py`. The client test and the
Python test share the byte transcript in `tools/live_vectors.json`.

Two runners on one machine need a local matchmaker and two play folders,
each with its own saves:

    python tools/matchmaker.py --address 127.0.0.1 --port 47790 --store live-store.json

Start each runner with `RECOMP_LIVE_SERVER=127.0.0.1:47790` and its own
`RECOMP_LIVE_IDENTITY`; Live is on by default.
