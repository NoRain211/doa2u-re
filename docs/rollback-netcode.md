# Rollback netcode

Status: the save/replay core, a rollback ring, input prediction, a frame-synced
transport, and the game's own Xbox Live fights on top of them. Live fights roll
back; the game's pages and its menus run in lockstep. `RECOMP_ROLLBACK=loopback`
plays the same session against a generated peer for a determinism test. A
frame-locked clock and SyncTest (`docs/bring-up.md`, Determinism checks) save,
replay and compare whole frames, with guest RAM in page snapshots
(`guest_snapshot.c`). Spectating is not built.
Game addresses below come from static analysis of `/DOA2.xbe` in Ghidra
unless a section says otherwise.

## The game's Xbox Live lockstep

Online fights use delay-based lockstep over XNet UDP. The host picks a batch
size of up to four frames (`g_lockstep_batch_frames`, `0xf192a4`) and a
delay in batches (`g_lockstep_delay_batches`, `0xf19af8`) when it starts the
exchange (`online_lockstep_host_start`, `0x29c7b0`); the guest copies both
values from the start packet. Each frame, `online_lockstep_frame`
(`0x298930`) packs the local pad into a 20-byte record
(`online_lockstep_pack_pad`, `0x2937b0`) in a ring of 2 sides x 16
batches x 4 frames (`g_lockstep_pad_ring`, `0xf1a1f8`), scheduled
`delay` batches ahead. A full batch goes out through
`online_send_lockstep_packet` (`0x29fc00`). When the peer's batch for the
current slot has not arrived (`g_lockstep_batch_received`, `0xf09a00`),
the frame sets `g_lockstep_stalled` (`0xf0a540`), skips the pad update and
resends every four frames. Sequence numbers with the high bit set are resend
requests; `0xfffffffc` to `0xffffffff` are stop, stop acknowledgement,
start acknowledgement and start (`online_lockstep_receive`, `0x29c1b0`).

Received input reaches the fight through two virtual pads.
`online_lockstep_apply_pads` (`0x293870`) unpacks both sides into
`g_pad_state` slots 5 and 6 and recomputes their pressed and released edges.
`fight_copy_player_pads` (`0xda080`) copies `g_pad_state[g_player_pad_index[p]]`
(`0x8a56dc`) into the four per-player input blocks at `0x87d260`, which the
fight and the frontend read. The frontend's online match scene
(`frontend_online_match_update`, `0x1318e0`) calls the lockstep once per
frame, so character select also runs in lockstep. The match RNG is
MT19937, seeded from the session key and two session counters
(`online_seed_random_from_session`, `0x127a00`).

A stall does not freeze the fight. This was traced statically before the Live
adapter could reach the online match scene (XNetStartup used to fail and
there was no XOnline logon). Game logic runs in task fibers resumed by
`task_fiber_scheduler` (`0xfa440`); the only pause is a per-slot suspend
bit set by `task_slot_set_flag_2` (`0xfa390`), and its callers are round
start and end, movies and fight setup. `game_mode_task_main` (`0xdaef0`)
copies the pads and runs the mode update from `g_game_mode_table`
(`0x6959f0`) every frame, and `fight_task_main` (`0x5c3e0`) and the other
fight tasks from `0x69cc0` run as well. None of their call trees, nor any
mode's init, update or exit, reads the lockstep state. During a stall the
fight keeps running while pads 5 and 6 hold their last applied state,
including its press edges. The stall flag only gates the frontend's own pad
update (task loop at `0x13b800`), the waiting indicator
(`online_stall_indicator_update`, `0x131040`) and the round-end waits
(`0xd1000`, `0xd1280`, `0xd1570`, `0xd1f10`, `0xd2070`). Rollback
therefore has to pace frames itself: the host blocks at the frame boundary
when the remote side falls outside the prediction window.

## What rollback can reuse

The lockstep proves the fight simulation is deterministic given both pad
streams and the seed, at least on the Xbox. The virtual pad slots and
`g_player_pad_index` give one clean place to inject remote input, and the
20-byte record is a ready input format. The lockstep transport, its stall
loop and the XOnline role handshakes do not carry over: Live's rollback
replaces the lockstep for a fight, and the game's own pages provide logon and
matchmaking.

## Host state

Rollback needs a save and restore of everything a fight frame changes, taken
every frame and restored several frames at a time. Most of it now exists for
SyncTest.

- Guest RAM snapshots: on Windows the runner allocates guest RAM with
  `MEM_WRITE_WATCH`, and `guest_snapshot.c` keeps a ring of save points
  that each store only the pages changed since the previous save. Write
  watch also reports `ReadFile` writes, which host read workers make
  straight into guest RAM. A save reads the list of written pages without
  clearing it and compares each with a shadow copy. After a clear the
  first write to every page faults, about 0.7 us each or 0.17 ms for a
  fight frame's 260 pages, so the list is cleared only once it holds more
  than twice the changed pages plus 64. Other hosts compare every page. Saves and loads must run while no host thread writes guest RAM, so
  netplay makes file reads synchronous as SyncTest does.
- Each XAPI fiber keeps its C continuation on a native host stack. On
  Windows, parked fibers are saved with their captured context and live
  stack (up to 64 KiB), so a rewind restores the C frames as well as the
  guest registers. POSIX hosts save the stack pointer and the live stack
  the same way, since their switch keeps the registers on the stack. The
  fiber test rewinds a parked task on x86-64 Linux; a whole session's
  rollback on Linux and macOS is unverified.
- The other host modules (heap, allocations, threads, files, input, D3D8
  with its table of owned resources, DirectSound) save and restore their
  own state.
- `frame_time_update` (`0xfefc0`) feeds QueryPerformanceCounter time into
  `g_frame_time_6000ths` and `g_frame_fps`. `0x2506f0` spawns into a pool
  only when the frame time is below 101, so a slow frame changes guest RAM.
  The frame-locked clock makes time a function of the frame count.
- Re-simulated frames must not present, play sound or rumble. The D3D8
  adapter can skip the flip and GPU work, and the buffer adapter can drop
  audio output, while their models keep advancing. Host output keeps its
  own real-time place in each buffer outside the saved state, so a rewind
  neither replays nor loses what was heard. A skipped flip still
  runs Present's vblank wait, which moves guest time even though the frame
  clock never sleeps there; without it hidden frames diverged.
- Frames that write saves or present from the display mode change cannot be
  rewound. A rollback window must not reach back past one.
- Both peers must produce identical floating-point results. Basic float
  operations are IEEE-exact on every host once GCC and Clang stop fusing
  multiply-adds (`-ffp-contract=off`). The x87 transcendentals are not: each
  host's libm differs in the last bit for a few percent of inputs. The runner
  defines `RECOMP_PORTABLE_MATH`, so generated fsin, fcos, fptan, fpatan,
  fyl2x, fyl2xp1 and f2xm1 call one copy of musl's code (`musl_math/`).
  `portable_math_test.c` pins a hash of 160,000 results; MSVC, GCC and Clang
  on x86-64 and arm64 Linux all match it.
  `RCPSS` and `RSQRTSS` are table approximations whose bits differ between
  Intel, AMD and the Xbox's CPU, so the runner computes the exact reciprocal
  on every host, within Intel's documented 1.5 * 2^-12 error.
- The game reads its widescreen (XC_VIDEO) and surround (XC_AUDIO) settings
  in its own logic. A Live fight's desync check ignores everything outside the
  fight (below), so each player keeps their own settings. The presenter fits
  the game's 4:3 or 16:9 picture into the window with black bars, and a player
  on stereo speakers hears the 5.1 mix folded to stereo. A 16:10 or ultrawide
  player keeps their own shape too: it changes projection bytes the fight
  check ignores.

## Rollback session

`rollback.c` keeps both sides' pads by frame. A remote frame that has not
arrived runs with the last remote pad; when its pad arrives and differs from
the one that ran, the session rewinds to the earliest wrong frame. A local
pad is stored with the values DOA2's pad update (`0xfa760`) reads as zero
set to zero: sticks within 0x2000 of center, face, Black and White under
0x20, and triggers under 0x80. A resting stick's noise is then no
misprediction. Past those limits the fight reads exact stick values (a
0x5000-wide tracker in `0x1f4c0`), so a held stick's noise still rewinds.
The local side runs at most `RECOMP_ROLLBACK_WINDOW` (8) frames past the last remote
pad. The ring in `synctest.c` keeps saves of SyncTest's modules and the
guest RAM pages, each tagged with its frame, and refuses a load that SyncTest
would skip.

The runner drives the session from the Swap boundary. At the newest frame it
adds the pads that arrived, then either loads the earliest mispredicted
frame or saves the frame about to run. A rewind only ever goes back to a
frame that started before the other player's pad for it arrived, so only
those frames save, plus every desync check frame, whose hash reads the
newest save. Lockstep frames and frames whose pad came in time skip the
save, and their RAM changes fold into the next one. In a localhost fight of
2,400 frames the two sides made 79 and 106 saves. The frame's pads become the input
adapter's sample source, so every XInputGetState in that frame reads the
same pad however often the game polls. Frames run again are hidden.

`RECOMP_ROLLBACK=loopback` tests this without a peer. From swap
`RECOMP_ROLLBACK_START`, a generated remote player on port 0 changes pads
nearly every frame, and its pads arrive `RECOMP_ROLLBACK_LAG` frames late
(default 4, at most 8). At session frame `RECOMP_ROLLBACK_FRAMES` (default
600) every pad arrives. After the last rollback the runner exits 0, first
writing guest RAM, with SyncTest's host-scheduled ranges zeroed, to
`RECOMP_ROLLBACK_DUMP` when that is set. Lag 0 never predicts, so its dump
is the reference.

From swap 3000 of the scripted Story fight, lag 4 over 600 frames rolled
back 594 times (2,376 frames run again) and lag 8 over 900 frames 890 times
(7,120 frames). Both dumps matched lag 0 byte for byte, and two lag 0 runs
match each other, so separate processes simulate the same frames. Only
a session hides file times: a loopback run reads the save folder's host
write time into guest RAM (8 bytes at `0x49c304`), so two dumps compare
only while that folder is unchanged between their runs. A hidden
frame costs about 3.7 ms on the development machine (26.4 s more than the
paced lag 0 run for 7,120 frames), so about three frames of rollback fit in
a 60 Hz frame; longer delays need input delay as well.

Limits of this stage:

- A save that cannot be rewound ends the run (`rollback:unrestorable`). At
  lag 8 the Story save after the fight, about 1,050 frames in, does this.
  Live sessions return to lockstep before the save after a match (see
  "Live fights"); `RECOMP_ROLLBACK` runs do not.
- Loopback has no local player, so port 1 stays empty.
- Two early hidden-frame runs ended with exit code -1 and no host report.
  Both started while the runner was being rebuilt and other runners were
  being killed. 69 later lag 4 runs of 1,000 frames, several at once, on
  this build and on a rebuild of the commit that first showed it, all exited
  normally, so the cause is unconfirmed.

## Live fights

`live_fight.c` hooks the game's Live fight. The game's first-round start
(`battle_online_round_start`) runs as on the Xbox, including its ready
handshake. The rollback session starts on the first frame after it returns,
as `RollbackMode::Live` in `netplay_session.cpp`, and replaces the game's
delay-based lockstep for the rest of the match. The player index comes from
the two fighters' virtual addresses, which both sides read the same way.
Either side's request to stop its lockstep rides a pad bit, so both sides end
the session on the same frame; what follows (results, win poses, the lobby)
runs on the game's own Live messages, unsynced. A desync, a timeout or a
failed rewind hands the fight back to the game's Live code without stopping
the runner. See [Xbox Live](xbox-live.md).

## Transport and pacing

`netplay.c` sends one UDP packet per frame. It carries the sender's frame,
how many of the receiver's pads it holds, its frame advantage and every
local pad the receiver has not acknowledged, so the next packet covers a
lost one and nothing is resent. Packets from any address other than the
configured peer are dropped, and a malformed packet changes nothing.

A Live fight starts its session from the game's own round handshake
([Xbox Live](xbox-live.md)); the transport replaces the game's delay-based
lockstep and the local pad is this player's controller. Only the local
player's port in shown frames drives its rumble. Runs with
`--input-script-only` play a generated stream for each player instead.

While the session is outside its window, the newest frame
waits and sends every 4 ms, for up to 120 s before first contact and 15 s
after it. After 1 s of waiting, the runner presents its last frame again
every 100 ms with an opaque WAITING FOR OPPONENT panel and a countdown
(CONNECTING before first contact), which also keeps the window answering.
If pads arrive in time the session goes on where it stopped; the log names
the frame where the other player went quiet and how long they were gone.
A save waiting for the other player's pads shows the same panel.
Shorter hitches in a live round are logged once per second when the second
had one: the time this side waited for the other player's pads after its
frame was due, its own slowest frame, how many frames it ran again after
rewinds and how long they took, and frames frame sync held back so the
other side could catch up. A slow second with long waits points at the other
machine; a slow frame here points at this one, and it includes any frames run
again after a rewind, since the player sees them as one late frame.
Two runners on one machine, through a relay holding each packet 60 ms
(+-20%) and with the joiner pressing a new pad every live frame, ran a hidden
frame in about 5 ms: 2.9 ms of game code, 0.7 ms building draws the hidden
frame drops, and 1 ms saving and loading state. At input delay 0 the host
ran about 80 frames a second again and nearly every live second had a frame
of 33 ms or more; at delay 2 one second did, and at delay 4 none. A hidden
frame now builds a draw only while it could still clear the back buffer
flags the saved state carries: the first on-screen draw after a Swap. In
the same 3,600-frame run at delay 0, frames run again in slow seconds took
3.7 ms instead of 5.6 ms, and the host's seconds with a slow frame fell from
28 to 3 (the joiner's from 41 to 30); guest RAM matched at every check.
A save then took about 0.6 ms: 0.27 ms of guest RAM pages and 0.25 ms
clearing the fiber states' 64 KB stack copies. Saves now clear only their
headers, as a restore reads just the used stack bytes, so a save takes about
0.4 ms. A rewind writes each page once, from the oldest save it undoes, instead
of once per save: on the joiner, whose rewinds undid about four saves, it fell
from 1.0 to 0.64 ms; on the host, about 1.6 saves, from 0.71 to 0.64 ms.
Repeated runs of this test vary by about a fifth, more than this changes.
Three later changes were measured with three runs of each build, taken in
turn. Packed SSE loads and stores of guest RAM compile inline like the
scalar ones (`recomp_types.h`); the calls were about a tenth of a hidden
frame. Saves leave write-watch tracking running (see Host state). Page
hashes are brought up to date at a check, not at every save and rewind. A
frame run again fell from 3.9 to 2.8 ms on the host (4.5, 3.6 and 3.6 ms
against 2.8 three times) and from 4.1 to 2.8 ms on the joiner, a host save
from 0.35 to 0.26 ms and a rewind's load from 0.39 to 0.29 ms. The host's
seconds with a slow frame went from 27, 3 and 0 to 1, 0 and 1, the
joiner's from 37, 28 and 28 to 8, 6 and 9. The host then keeps its pace, so
its rewinds reach further back (3.9 frames each, from 3.1) and hidden
frames still take a fifth of the run. In loopback, lag 4 over 600 frames
ran its 2,376 hidden frames in 7.0 s instead of 9.1 s over four runs of
each build. A session's saves also no longer wait for a pending visibility
test: the wait held 15 of 5,620 host saves for 2 to 3 ms each on this
stage, which has a lens flare. Loopback and SyncTest saves still wait,
since their compares include the counts. A hidden frame now drops a draw
before resolving its render target once the back buffer flags are clear,
and the audio pump reserves its 80 KB output buffer only when it plays. In
a loopback profile the host's draw work fell from 6.4% to 2.0% of a hidden
frame, which took 2.46 ms instead of 2.62 ms; over eight runs of each build
the frames run again took 3% less, less than the netsim varies between
runs. Generated game code is then 82% of a hidden frame. Both
renderers log each draw shader they build with how long it took: D3D11 only
outside its boot precompile, SDL_GPU at boot too.
Freezing the joiner's process for 6 s in the menus held the host on the
panel, and the session then played on into a live round with identical
guest RAM at frame 4,400; killing the joiner ended the host's session 15 s
later. A runner that quits, by closing its window or stopping on an error,
sends the other a goodbye first. The other runner then shows OPPONENT LEFT
for 2 s and ends the session (`rollback:peer-left`); closing the joiner's
window ended the host's session 2.1 s later. Scripted runs that reach their
last frame send none.

The presenter holds shown frames to 60 Hz, but the guest may run up to two
frames ahead into its queue, and a sleep there only used up that slack. So
each new frame of a Live session starts on the runner's own 1/60 s grid, and
a late frame may catch up by at most two frames. The local pad is read and
any rewind taken when the frame's slot arrives, so pacing adds no input lag
and one rewind covers every pad that came in meanwhile. Hidden frames run
unpaced.

Each side also reports its advantage: its frame minus the peer's last
reported frame, measured once per new frame and repeated unchanged while it
stalls. The receiver keeps one sample per new peer frame. Both advantages
include the same round trip, so half the difference of the two 32-frame
averages is how far ahead one side really is. The side a frame or more
ahead moves its grid back that many frames, then waits 32 frames before it
checks again. On an asymmetric link the advantages settle equal, so both
sides predict the same number of frames.

Two runners on one machine, each in its own hardlinked play folder, started
at swap 3000 and stopped at frame 900 with every pad confirmed. The lossy
link went through a relay that delayed each packet ±20% and dropped some:

| Link | Input delay | Rollbacks per side | Frames run again | Time |
|---|---|---|---|---|
| localhost | 0 | about 450 | about 455 | 15.0 to 15.2 s |
| localhost | 2 | 0 | 0 | 15.0 s |
| 50 ms each way, 5% loss | 0 | 350 to 390 | 1,210 to 1,270 | 18.9 to 19.1 s |
| 10 ms out, 90 ms back, 2% loss | 0 | 380 to 460 | 1,240 to 1,460 | 17.5 to 19.4 s |
| 10 ms out, 90 ms back, 2% loss | 2 | about 500 | about 950 | 15.4 to 15.5 s |

Other work on the machine moved times by up to 2 s between identical runs.
Alternating runs with and without the grid on the asymmetric link took the
same time within that spread. At input delay 0 the reruns cost more than a
frame's budget, so both sides fall below 60 Hz together.

In every pair the two sides' guest RAM matched except for one save file's
times, which the game lists at boot and the host reports from each play
folder. Players' profiles and save metadata differ the same way; a Live
fight's check ignores them (see "Desync checks").

A temporary hook slept player 1 for 250 ms at frame 300 on localhost with
input delay 2. Before the grid, player 0 settled a frame ahead (advantages
about 2 and 0) and waited 15 times without closing the gap, because each
sleep only drained the presenter queue. With the grid it waited 4 frames
and both advantages were back to 0 by frame 420.

Limits of the transport:

- IPv4 only. There is no close handshake: a finished side repeats its last
  packet for half a second.
- The grid assumes the 60 Hz progressive mode. With a display slower than
  60 Hz the guest would wait in the presenter queue instead; that is untested.

## Desync checks

Every 30th frame of a Live session, each runner hashes guest RAM as the
frame starts, reading SyncTest's host-scheduled ranges as zero. The hash
belongs to the rollback ring's newest save or load
(`recomp_snapshots_hash`, `guest_snapshot.c`): each page has its own hash,
seeded with its index, and the ring hashes every page it saves or restores
while the page is still in cache, so a check only adds up the changes. Once
the other player's pads before that frame have all arrived, no rewind
reaches it again and the hash is final. Each packet carries the sender's
newest final frame and hash, 12 bytes, so the largest packet is 1,185
bytes, under the matchmaker's 1,200-byte relay limit. Each side keeps its
last eight and compares the other side's newest with its own for that frame.

A mismatch logs the frame and both hashes, writes guest RAM to
`RECOMP_ROLLBACK_DUMP` when that is set (as of the frame that found it, a
few frames after the checked one), sends this side's hash again so the other
side finds the desync too, and ends the rollback session, which hands the
fight back to the game's own Live code (`rollback:desync`). Loopback runs
make no desync checks. A side stalled on the other still compares. Every 20 matching checks
the log says so, with the slowest check.

A Live fight hashes only the fight: the match and fighter block, the battle
state and the game's Mersenne Twister. Everything else, including each side's
Live layer, profile, HUD, 2D draw state, cloth and stage effects keyed to
frames since boot, is ignored (`live_fight_rules.c`). With the whole of guest
RAM hashed, the first live round failed its first check after the HUD
appeared, at frame 1,500, and both sides' guest RAM at that frame differed in
15 bytes, all of them HUD colours. SyncTest and the dumps still compare them.

Two scripted runners on one machine at input delay 0 played six live rounds
in 15,600 frames, about 10,400 of them with the rollback window open, and
every check matched. After the first, a check took 0.01 to 0.06 ms, and
saves spent about 0.06 ms a frame hashing the pages they copy. Saves and
rewinds now only mark those pages and the check hashes the marked ones,
0.1 to 0.6 ms every 30 frames. The first check hashes all 64 MB, 4 to 6 ms
at frame 30. A thread or fiber stack that comes or goes moves the ranges, and
the pages it uncovers are hashed again; a page wholly inside the ranges counts
as 0 without a pass.

`RECOMP_ROLLBACK_FLIP=<frame>` is a test hook that flips the low bit of
fighter 1's life (`0x87ae54`) on that side alone at that frame. With it on
the joiner at frame 2,000 of a live round, both sides found the desync at
the next check, frame 2,010, and ended the rollback session.

Limits of the checks:

- Only guest RAM is hashed; the host modules' state (D3D8, DirectSound,
  fibers, CPU) is not, and a difference only in the ignored ranges goes
  unseen.
- A desync is found at the next check frame plus about a round trip: up to
  half a second plus a round trip after it began.

