# Rollback netcode

Status: the save/replay core, a rollback ring, input prediction, a UDP
transport with frame sync, and a lobby that connects two runners into one
session from their first frame. Menus run in lockstep and live rounds roll
back; two scripted runners have played offline Versus this way. A frame-locked clock
and SyncTest (`docs/bring-up.md`, Determinism checks) save, replay and
compare whole frames, with guest RAM in page snapshots
(`guest_snapshot.c`). The launcher has no lobby screen yet.
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

A stall does not freeze the fight. The online match scene cannot be reached
on this host (XNetStartup fails and there is no XOnline logon), so this was
traced statically. Game logic runs in task fibers resumed by
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
20-byte record is a ready input format. The transport, the stall loop and
the XOnline session and role handshakes between rounds are tied to Xbox
Live and do not carry over.

## Host state

Rollback needs a save and restore of everything a fight frame changes, taken
every frame and restored several frames at a time. Most of it now exists for
SyncTest.

- Guest RAM snapshots: on Windows the runner allocates guest RAM with
  `MEM_WRITE_WATCH`, and `guest_snapshot.c` keeps a ring of save points
  that each store only the pages written since the previous save. Write
  watch also reports `ReadFile` writes, which host read workers make
  straight into guest RAM. Other hosts compare every page with a shadow
  copy. Saves and loads must run while no host thread writes guest RAM, so
  netplay makes file reads synchronous as SyncTest does.
- Each XAPI fiber keeps its C continuation on a native host stack. Parked
  fibers are saved with their captured context and live stack, so a rewind
  restores the C frames as well as the guest registers.
- The other host modules (heap, allocations, threads, files, input, D3D8,
  DirectSound) save and restore their own state.
- `frame_time_update` (`0xfefc0`) feeds QueryPerformanceCounter time into
  `g_frame_time_6000ths` and `g_frame_fps`. `0x2506f0` spawns into a pool
  only when the frame time is below 101, so a slow frame changes guest RAM.
  The frame-locked clock makes time a function of the frame count.
- Re-simulated frames must not present, play sound or rumble. The D3D8
  adapter can skip the flip and GPU work, and the buffer adapter can drop
  audio output, while their models keep advancing. A skipped flip still
  runs Present's vblank wait, which moves guest time even though the frame
  clock never sleeps there; without it hidden frames diverged.
- Frames that write saves or present from the display mode change cannot be
  rewound. A rollback window must not reach back past one.
- Both peers must produce identical floating-point results. The same build
  on x86-64 is the safe case; macOS arm64 and host libm differences are
  unverified.

## Rollback session

`rollback.c` keeps both sides' pads by frame. A remote frame that has not
arrived runs with the last remote pad; when its pad arrives and differs from
the one that ran, the session rewinds to the earliest wrong frame. The local
side runs at most `RECOMP_ROLLBACK_WINDOW` (8) frames past the last remote
pad. The ring in `synctest.c` keeps one save per frame of SyncTest's modules
and the guest RAM pages, and refuses a load that SyncTest would skip.

The runner drives the session from the Swap boundary. At the newest frame it
adds the pads that arrived, then either loads the earliest mispredicted
frame or saves the frame about to run. The frame's pads become the input
adapter's sample source, so every XInputGetState in that frame reads the
same pad however often the game polls. Frames run again are hidden.

`RECOMP_ROLLBACK=loopback` tests this without a peer. From swap
`RECOMP_ROLLBACK_START`, a generated remote player on port 0 changes pads
nearly every frame, and its pads arrive `RECOMP_ROLLBACK_LAG` frames late
(default 4, at most 8). At session frame `RECOMP_ROLLBACK_FRAMES` (default
600) every pad arrives. After the last rollback the runner writes guest RAM,
with SyncTest's host-scheduled ranges zeroed, to `RECOMP_ROLLBACK_DUMP` and
exits 0. Lag 0 never predicts, so its dump is the reference.

From swap 3000 of the scripted Story fight, lag 4 over 600 frames rolled
back 594 times (2,376 frames run again) and lag 8 over 900 frames 890 times
(7,120 frames). Both dumps matched lag 0 byte for byte, and two lag 0 runs
match each other, so separate processes simulate the same frames. A hidden
frame costs about 3.7 ms on the development machine (26.4 s more than the
paced lag 0 run for 7,120 frames), so about three frames of rollback fit in
a 60 Hz frame; longer delays need input delay as well.

Limits of this stage:

- A save that cannot be rewound ends the run (`rollback:unrestorable`). At
  lag 8 the Story save after the fight, about 1,050 frames in, does this.
  Lobby sessions return to lockstep before the save after a match (see
  Lobby); `RECOMP_ROLLBACK` runs do not.
- Loopback has no local player, so port 1 stays empty.
- Two early hidden-frame runs ended with exit code -1 and no host report.
  Both started while the runner was being rebuilt and other runners were
  being killed. 69 later lag 4 runs of 1,000 frames, several at once, on
  this build and on a rebuild of the commit that first showed it, all exited
  normally, so the cause is unconfirmed.

## Transport and pacing

`netplay.c` sends one UDP packet per frame. It carries the sender's frame,
how many of the receiver's pads it holds, its frame advantage and every
local pad the receiver has not acknowledged, so the next packet covers a
lost one and nothing is resent. Packets from any address other than the
configured peer are dropped, and a malformed packet changes nothing.

`RECOMP_ROLLBACK=udp:<local port>:<peer host>:<peer port>` plays a session
against another runner. `RECOMP_ROLLBACK_PLAYER` (0 or 1) is the local
player's port and `RECOMP_ROLLBACK_DELAY` the input delay; both sides must
use the same delay and start swap. The local pad is the host's first
controller, and only the local player's port in shown frames drives its
rumble. Runs with `--input-script-only` play a generated stream for each
player instead. While the session is outside its window, the newest frame
waits and sends every 4 ms, for up to 120 s before first contact and 5 s
after it.

The presenter holds shown frames to 60 Hz, but the guest may run up to two
frames ahead into its queue, and a sleep there only used up that slack. So
each new frame of a UDP session starts on the runner's own 1/60 s grid, and
a late frame may catch up by at most two frames. The local pad is read and
any rewind taken when the frame's slot arrives, so pacing adds no input lag
and one replay covers every pad that came in meanwhile. Hidden frames run
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
same time within that spread. At input delay 0 the replays cost more than a
frame's budget, so both sides fall below 60 Hz together.

In every pair the two sides' guest RAM matched except for one save file's
times, which the game lists at boot and the host reports from each play
folder. Players' profiles and save metadata will differ the same way, so
the lobby must start both sides from the same profile.

A temporary hook slept player 1 for 250 ms at frame 300 on localhost with
input delay 2. Before the grid, player 0 settled a frame ahead (advantages
about 2 and 0) and waited 15 times without closing the gap, because each
sleep only drained the presenter queue. With the grid it waited 4 frames
and both advantages were back to 0 by frame 420.

Limits of the transport:

- IPv4 only. There is no close handshake: a finished side repeats its last
  packet for half a second.
- There is no desync check between peers yet.
- The grid assumes the 60 Hz progressive mode. With a display slower than
  60 Hz the guest would wait in the presenter queue instead; that is untested.

## Lobby

The Live menus need XOnline logon, match search, QoS, friends and stats
services before they reach a fight, and no System Link text appears in the
XBE. Instead a lobby on the host connects two runners, which then play the
game's offline modes together, the host on the first controller port and the
joiner on the second. That avoids all of XOnline.

`--netplay-host <port>` waits up to 10 minutes for a joiner, and
`--netplay-join <host>:<port>` says hello every 100 ms for up to 2 minutes.
The hello carries the SHA-256 of the joiner's runner executable, and the
host refuses any other build. That also refuses a Windows and Linux pair,
whose floating point is unverified. The host answers with its input delay
(`--netplay-delay`, default 2) and the options that change the simulation
(`--no-unlock-all`, `--skip-intro`), which the joiner adopts, and the size
of its profile.

The host first recovers its own save journal, then packs its
`.recomp-storage/partition1` (`UDATA` profiles and `TDATA` title saves)
into one blob of at most 1 MiB with a SHA-256 of its contents. The joiner
asks for 16 chunks of 1 KiB at a time from the first one it lacks, asks
again when a burst has arrived or after 100 ms, and says when it has them
all. The host answers hellos and requests until then; if that last message
is lost, the joiner's first session packet ends the wait. Before writing,
each side checks the hash, a limit of 512 entries and every path: relative,
below `UDATA` or `TDATA`, letters, digits, `.`, `_` and `-` only, and
no Windows device names. The host's 92 KB profile arrives in one run of
bursts on a local network.

Before it waits, the host asks the router to forward its UDP port with UPnP
(miniupnpc 2.3.3), and removes the forward when the runner exits. It sends
the search from the address its default route uses: on the development
machine, Tailscale, WSL and Hyper-V adapters took the default multicast
route, and the router never heard a search sent without it. The log names
the public address the other player joins, or says the port must be
forwarded by hand. A router whose own address is private (carrier-grade
NAT) still forwards, but players outside the provider's network cannot
reach it, and the log says so. On the development network the router
(MiniUPnPd, IGD v2) listed the forward during a session and had removed it
after the runner stopped.

Both runners then boot and play one session from their first frame. Until
it starts, and on ports 3 and 4 throughout, the guest sees no controller, so
neither machine's other pads reach the game. Saves go to
`.recomp-storage/netplay/partition1` with their own undo journal, wiped when
each session starts and filled with the host's profile, so both sides start
alike and neither player's own profile changes. The cached archives in partitions 3 to
5 stay shared; `run_game.py` prepares them the same way on every machine.
During a session the host reports no file times, which differ between
machines.

The session runs in lockstep, where a frame runs only once the remote pad
has arrived, and predicts up to 8 frames only while a round is live:

- `g_game_mode_state` (`0x8a56d0`) is 5, the match state that
  `game_mode_frontend_update` (`0xd87c0`) enters for every playable mode;
- `g_game_tasks_suspended` (`0x8783c9`) is 0. `game_tasks_suspend`
  (`0xd8d90`) sets it while the pause menu freezes the fight, and
  `game_tasks_resume` (`0xd8e10`) clears it;
- `g_round_timer_seconds` (`0x87ae08`), which `round_timer_tick`
  (`0x119320`) counts down, is above 0;
- both fighters' life (`0x87ae54`, stride `0x94`) is above 0.

Each side decides from its own frames, as the window only limits how far it
runs ahead. In the scripted Story fight the condition turned true 25 frames
into the match state, while the stage still loaded for about 15 more frames;
the display mode change came before it. A KO or time-over closes the window
seconds before the save at the end of a match. Before the timer was part of
the condition, that save ran in the frame the match state ended while the
window was still open.

Every open that may change the profile also waits, inside its frame, until
the other player's pads through that frame have arrived
(`recomp_kernel_profile_mutation_hook`). A save then never runs on a
guess, and every later rewind lands after it. If an arrived pad differs from
the one the frame ran with, the frame is already wrong, and the session
stops before the write (`rollback:save-mispredicted`). With the window
closed this never waits. Quitting from the pause menu saves about 20 frames
after the quit, long after the window closed at the pause.

Two runners on one machine, each in its own play folder, played scripted
sessions. The host's input script chose Versus and Single Battle and picked
a character, and the joiner picked one and pressed random buttons in live
rounds. In the first four runs both sides started from an empty profile,
and the host's script made one (nickname and flag); the last two started
from the host's copied profile, where the host paused at frame 4,300 and
chose Quit:

| Input delay | Frames | Rollbacks (host) | Window changes | Guest RAM at the end |
|---|---|---|---|---|
| 2 | 6,000, title screen only | 0 | none | identical |
| 2 | 7,000 | 0 | rollback from 3,744 | identical |
| 0 | 14,000 | 2,465 | two rounds; lockstep at both time-overs | identical |
| 0 | 19,000 | 3,367 | two matches of two rounds | identical |
| 0 | 6,000 | 233 | rollback from 3,748; lockstep from the pause | identical |
| 0 | 5,000, test build with the window held open from 4,000 | 238 | the Quit save in frame 4,920 waited on both sides | identical |

None of the comparisons masked a byte.

Limits of the lobby:

- The launcher has no lobby screen. Without UPnP the host must forward its
  UDP port by hand; there is no hole punching or relay.
- A runner that is killed leaves its port forward on the router until the
  next host run replaces it.
- When one runner quits, the other stops 5 s later
  (`rollback:peer-timeout`).
- A save in a frame that ran with a wrong guess still ends the session; no
  save site seen so far comes within the 8-frame window of a live round.
- The condition reads two fighters; Tag matches and rounds without a time
  limit are unverified.

