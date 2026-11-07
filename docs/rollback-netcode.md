# Rollback netcode

Status: the save/replay core, a rollback ring, input prediction, a UDP
transport with frame sync, and a lobby that connects two runners into one
session from their first frame. Menus run in lockstep and live rounds roll
back; two scripted runners have played offline Versus this way. A frame-locked clock
and SyncTest (`docs/bring-up.md`, Determinism checks) save, replay and
compare whole frames, with guest RAM in page snapshots
(`guest_snapshot.c`). Players meet in a lobby room from the game's
Versus menu. Each session records a replay that plays back to the same
guest RAM; spectating is designed but not built.
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
- Each XAPI fiber keeps its C continuation on a native host stack. On
  Windows, parked fibers are saved with their captured context and live
  stack (up to 64 KiB), so a rewind restores the C frames as well as the
  guest registers. POSIX hosts do not capture them yet: a save with a
  parked fiber that owns its stack reports itself unrestorable there, and
  rollback on Linux and macOS is unverified.
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
  in its own logic: a joiner with either setting different from the host's
  desynced on frame 1. The lobby sends the host's, and the kernel gives
  them to the game for the session. Each player keeps their own window and
  speakers. The presenter fits the game's 4:3 or 16:9 picture into the
  window with black bars, and a player on stereo speakers hears the 5.1
  mix folded to stereo. A 16:10 or ultrawide player gets the host's picture
  too, fitted to the window: the session forces 16:9 projections, since the
  shape would change them in hashed guest RAM. Fights to frame
  1,800 with the two settings crossed both ways, and with 4x MSAA on one
  side, kept guest RAM equal.

## Rollback session

`rollback.c` keeps both sides' pads by frame. A remote frame that has not
arrived runs with the last remote pad; when its pad arrives and differs from
the one that ran, the session rewinds to the earliest wrong frame. The local
side runs at most `RECOMP_ROLLBACK_WINDOW` (8) frames past the last remote
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
waits and sends every 4 ms, for up to 120 s before first contact and 15 s
after it. After 1 s of waiting, the runner presents its last frame again
every 100 ms with an opaque WAITING FOR OPPONENT panel and a countdown
(CONNECTING before first contact), which also keeps the window answering.
If pads arrive in time the session goes on where it stopped; the log names
the frame where the other player went quiet and how long they were gone.
A save waiting for the other player's pads shows the same panel.
Freezing the joiner's process for 6 s in the menus held the host on the
panel, and the session then played on into a live round with identical
guest RAM at frame 4,400; killing the joiner ended the host's session 15 s
later. A runner that quits, by closing its window or stopping on an error,
sends the other a goodbye first. The other runner then shows OPPONENT LEFT
for 2 s and ends the session (`rollback:peer-left`); closing the joiner's
window ended the host's session 2.1 s later. Scripted runs that reach their
last frame send none.

NETPLAY SETTINGS chooses a network display for sessions, kept per player:
off, ping, or full, which adds the input delay, the deepest rewind of the
last second and how many frames this side runs ahead of the other. It is
drawn in the bottom right corner of shown frames, after the game's own
draws, so it never reaches guest state. The ping runs from a local pad's
first send until the other player's packet says it holds that pad,
averaged over about eight samples, so it includes up to a frame on each
side.

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
the lobby must start both sides from the same combined profile and save tree.

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

Every 30th frame of a UDP session, each runner hashes guest RAM as the
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
side finds the desync too, shows DESYNC DETECTED for 2 s and ends the session
(`rollback:desync`). A side stalled on the other still compares. Every 20
matching checks the log says so, with the slowest check.

The fight HUD draws the local player's name in yellow and the other's in
white (`draw_fight_name`, `netplay_guest.c`), so the two sides' 2D draw
state differs in those glyphs' colors. They sit in the sprite queue
`g_ui_sprite_queue` (`0x88eed8`), which `ui_draw_sprite` (`0xb1060`)
fills below `0x89bed9` and `ui_draw_lists_reset` (`0xae300`) empties each
frame, and in the text color `g_ui_text_color` (`0x8a01c8`). The check
reads both as zero. With them hashed, the first live round failed its first
check after the HUD appeared, at frame 1,500, and both sides' guest RAM at
that frame differed in 15 bytes, all of them those colors. SyncTest and the
dumps still compare them.

Two scripted runners on one machine at input delay 0 played six live rounds
in 15,600 frames, about 10,400 of them with the rollback window open, and
every check matched. After the first, a check took 0.01 to 0.06 ms, and
saves spent about 0.06 ms a frame hashing the pages they copy. The first
check hashes all 64 MB, 4 to 6 ms at frame 30, while the walk to Versus is
hidden. A thread or fiber stack that comes or goes moves the ranges, and the
pages it uncovers are hashed again; a page wholly inside the ranges counts
as 0 without a pass.

`RECOMP_ROLLBACK_FLIP=<frame>` is a test hook that flips the low bit of
fighter 1's life (`0x87ae54`) on that side alone at that frame. With it on
the joiner at frame 2,000 of a live round, both sides found the desync at
the next check, frame 2,010, showed DESYNC DETECTED and went back to their
rooms.

Limits of the checks:

- Only guest RAM is hashed; the host modules' state (D3D8, DirectSound,
  fibers, CPU) is not, and a difference only in the ignored ranges goes
  unseen.
- A desync is found at the next check frame plus about a round trip: up to
  half a second plus a round trip after it began.

## Lobby

The Live menus need XOnline logon, match search, QoS, friends and stats
services before they reach a fight, and no System Link text appears in the
XBE. Instead a lobby on the host connects two runners, which then play the
game's offline modes together, the host on the first controller port and the
joiner on the second. That avoids all of XOnline.

Choosing Versus on the main menu opens the runner's own menu: LOCAL, HOST
ONLINE, JOIN ONLINE, FIND MATCH ([matchmaking](matchmaking.md)) and NETPLAY
SETTINGS (`netplay_menu.cpp`). The runner
draws it over the game as untextured quads with a built-in 5 x 7 font
(`overlay.c`), sent just before each Present, and the game sees idle pads
until it closes. The menu opens on an A press while frontend scene
`g_frontend_scene_current` (`0xdf5dd8`) is 0, the main menu, with
`g_frontend_selected_mode` (`0xdf57b4`) at 5, Versus, and
`g_game_mode_state` at 4. LOCAL hands the game an A press once every
button is up; B closes the menu and holds the game's pads idle until then,
so the game never sees it. The input delay, the host's port and the last
host joined are kept in `.recomp-storage/netplay.txt`, with the
[matchmaker](matchmaking.md) set under NETPLAY SETTINGS.

HOST ONLINE binds the port, asks the router for a forward on a background
thread and keeps the address to send for X to copy; a PRIVATE room is never listed on
FIND MATCH. JOIN ONLINE takes an address from
the d-pad; on Windows it also accepts the keyboard and clipboard. The menu never draws a
player's IP address, so streams do not show one: the editor masks all but the character under the caret, the
joiner's room says how it joined, and the host's address shows only while
Y is toggled on and nobody has joined. NETPLAY SETTINGS shows the matchmaker
address as it was typed. In the room each side says its build, profile
name and readiness ten times a second; both plates show the names, ready
states and the round-trip time. Names are checked as in the session lobby,
and a different build shows on the other player's plate.
B leaves the room, and the runner first tells the other player with its own
leave message, separate from the session's goodbye, which can arrive late
into a room. A host that hears it says who left and lets the next player in;
a joiner forgets the host's address, stops calling it and shows THE HOST
CLOSED THE ROOM, so a room opened again on the same port starts empty.
Once the host's room has a round trip, INPUT DELAY suggests one frame per
33 ms of it, rounded up, none up to 20 ms, at most the rollback window;
once the other player is in, the host's left/right steps the delay. The
joiner still takes whatever the host chose.

Once the other player is in, Enter or Tab opens a chat line from the keyboard;
Enter sends it and Esc drops it. While a line is open, the room ignores the
pad buttons that letters also press. A line holds up to 40 printable ASCII
characters. Each player can send one line a second, and a line that fails
the name check is refused. The newest line rides in each room message for
2 s under an id taken from the sender's clock, so a lost packet costs
nothing and the other side shows it once. The receiver checks it again,
drops anything else and takes at most one line a second, so a line held
back shows from a later repeat. The last four lines replace the address
lines. Two scripted runners typed into each other's rooms, and real
keystrokes for X and Z went into the line without leaving the room or
toggling ready. A session never shows chat, since its room is closed.

Each install keeps a random 64-bit player ID in `netplay.txt`, made on the
first run, and every room message carries it after the chat line. Y mutes
the other player: their lines leave the chat, new ones are dropped, and
their plate says CHAT MUTED until Y again or until they leave. X asks to
block the other player and a second X within 3 s blocks them: their ID and
name go into `netplay.txt` as a `block` line, a host drops them and keeps
its room open, and a joiner leaves. From then on the room drops every
message from a blocked ID before taking its sender as the peer, so a host
never answers a blocked joiner and stops punching to one the matchmaker
paired, and a joiner that reaches a blocked host leaves with a message.
FIND MATCH hides a room whose host this runner blocked or was refused by,
for the rest of the run; the matchmaker never learns IDs or blocks. UNBLOCK
ALL under NETPLAY SETTINGS empties the list.

When both are
ready the host says start for 600 ms. Each runner closes the room, shows
STARTING MATCH over its last frame, and runs the session lobby below as if
started with `--netplay-host` or `--netplay-join`, with the intro skipped.
It then reboots the game in the same process and window: on a fresh host
fiber it puts the runtime modules back to power-on state
(`recomp_reset_runtime_modules`, which frees the old guest stacks and
keeps the D3D window and audio output), reloads the XBE image and enters
it again. Its lobby
flag tells the joiner to do the same walk to Versus character select
(frontend scene 3): both sides replace the frame's pads with Start until
`g_game_mode_state` is 4, Right until the main menu has Versus, then A,
and A again for Single Battle, the first choice of the battle type menu
(scene 2). Presses last one frame in every 16 and wait while a scene change
is pending (`g_frontend_scene_target`, `0xdf5dda`). Both sides decide from
the same guest state in the same frame, so they stay in step. Two scripted
runners on one machine went from the Versus menu through the room to
character select at session frame 334, with identical guest RAM at frame
1,200. With the reboot, both players' guest RAM at frame 1,500 matched each
other and a session started by restarting the runners byte for byte.
The walk runs out of sight and earshot: each runner shows STARTING MATCH
with LOADING VERSUS over its last frame, and turns off the game's presents
and sound output until character select. The battle type menu's fade-out
(frames 305 to 333) stays hidden, and the first frame shown is character
select fading in. Game state is unchanged, so both sides still match.

A room session that ends (`rollback:peer-left`, `rollback:peer-timeout`,
`rollback:save-mispredicted`, `rollback:unrestorable`, a lobby that
fails to connect, or `netplay:left-versus` when a player backs out of
Versus to the main menu, which both sides see at the start of the same
lockstep frame) goes back to its room. Two scripted runners matched
through a local matchmaker; the joiner backed out of character select,
and both left at frame 4,037 with equal replays and went back to their
rooms. The runner says goodbye, drops the
session's state, reboots the game offline the same way and reopens the
room: the host listens again and the joiner calls the host. Each runner
keeps one UDP socket from its room through the session and back: the room
hands it to the session lobby at launch, and the session hands it back, so
a path its routers opened stays open (see [matchmaking](matchmaking.md)).
A joiner kept one local port through two cancelled lobbies, a session and
each return to its room. Behind the room, the port 0 pad walks to the main
menu with Versus chosen, so leaving the room lands where the Versus menu was
opened. Both
players press READY again for a rematch. Until the menu has Versus chosen,
the room's backdrop is opaque and the game's sound is off, so the title and
menus do not show through. A host kept as one process
through a session, its joiner quitting, the return to the room and a
second session with a new joiner matched that joiner's guest RAM at frame
3,000, with one window throughout. Sessions started with `--netplay-host`
or `--netplay-join` still stop with exit code 4.

B, Back or Esc on STARTING MATCH cancels it, and the runner goes back to
its room. The side that cancels says goodbye to the other once it knows it,
which ends that side's wait too, and a host back in its room answers a
joiner still calling the old match with a goodbye. With two scripted
runners, the host cancelled while its joiner was suspended, and that joiner
was sent back when it resumed. The joiner then cancelled while the host was
suspended, and the host went back when it resumed. A rematch after both
matched guest RAM at frame 3,000. A host that closed its window mid-session
sent its joiner back to the room, and a new host process then played that
joiner to frame 3,000 with matching guest RAM.

`--netplay-host <port>` waits up to 10 minutes for a joiner, and
`--netplay-join <host>:<port>` says hello every 100 ms for up to 2 minutes.
The hello carries the joiner's netplay build, and the host refuses any
other and keeps waiting. It is a SHA-256 of the release in `VERSION` (the Git commit in a
checkout), the generated program's manifest and the game XBE's SHA-256
(`RECOMP_XBE_SHA256`), so runners of one release on Windows, macOS and Linux may pair; the
runners' own bytes differ by platform and compiler. A Windows runner and a
Linux runner built with GCC from one commit compute the same ID, but no
fight between two different hosts has run yet. The host answers with
its input delay
(`--netplay-delay`, default 2) and the options that change the simulation
(`--no-unlock-all`, `--skip-intro`), which the joiner adopts. Both
messages carry the size of the sender's profile.

Each side first recovers its own save journal, then packs the profile it
saved last (`UDATA/<title>/<save>`) into one blob of at most 1 MiB with a
SHA-256 of its contents. The host's blob also holds its `TDATA` title saves
and the files beside the profiles. Each side asks the other for 16 chunks
of 1 KiB at a time from the first one it lacks, asks again when a burst has
arrived or after 100 ms, and says when it has them all. Each answers hellos
and requests until the other is done; a request past the last chunk or a
session packet also counts as done. Before writing,
each side checks the hash, a limit of 512 entries and every path: relative,
below `UDATA` or `TDATA`, letters, digits, `.`, `_` and `-` only, and
no Windows device names. The host's 92 KB blob arrives in one run of
bursts on a local network. The log names both players from the `Name=`
line of each profile's `SaveMeta.xbx`.

Each runner also checks both profile names against a list of names that
are not allowed online and refuses the session on a match.

Before it waits, the host asks the router to forward its UDP port with UPnP
(miniupnpc 2.3.3), and removes the forward when the runner exits. It sends
the search from the address its default route uses: on the development
machine, Tailscale, WSL and Hyper-V adapters took the default multicast
route, and the router never heard a search sent without it. The log says
the port is forwarded, or that it must be forwarded by hand. It leaves out
the public address, since players paste this log into public issues; the
joiner uses the host's public address, as any "what is my IP" page shows
it, with that port. A router whose own address is private (carrier-grade
NAT) still forwards, but players outside the provider's network cannot
reach it, and the log says so. On the development network the router
(MiniUPnPd, IGD v2) listed the forward during a session and had removed it
after the runner stopped. A session started from a room keeps the forward
HOST ONLINE asked for and asks for none of its own.

Both runners then boot and play one session from their first frame. Until
it starts, and on ports 3 and 4 throughout, the guest sees no controller, so
neither machine's other pads reach the game. Saves go to
`.recomp-storage/netplay/partition1` with their own undo journal, wiped when
each session starts and filled with the host's blob and then the joiner's,
so both sides start alike and neither player's own profile changes. Two
profiles with the same save folder collide, and the joiner's wins. The
session loads no profile and plays the game's default settings, but each
player keeps their own controller: when the game sets up those defaults
(`profile_main_clear`), both sides read each player's profile from the
session folder and give its controller 1 settings (button type or free
buttons, vibration and analog command) to that player's port. With the
joiner's profile on Type C, two scripted runners gave port 2 Type C and
port 1 the host's Type A, and matched guest RAM through frame 1,800. The
cached archives in partitions 3 to
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
chose Quit. A later run swapped both players' profiles and played a live
round to frame 4,200 at delay 0 with 199 rollbacks on the host; the two
session save trees and guest RAM matched:

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

- Without UPnP or a [matchmaker](matchmaking.md) for hole punching and
  relay, the host must forward its UDP port by hand. JOIN ONLINE takes an
  IPv4 address.
- The hidden walks still take their time: about 6 s on STARTING MATCH
  before character select, and about 3 s behind the room after a session.
- A joiner that cancels before the host has heard its hello, in practice
  during the host's 600 ms of saying start, goes unheard; that host waits
  on STARTING MATCH until it cancels too.
- Chat typing and the clipboard work only on Windows; the SDL presenter
  has no text input yet. A pad cannot type.
- The game's own menus still choose which profile plays each side, and two
  copies of one save folder keep only the joiner's.
- A runner that is killed leaves its port forward on the router until the
  next host run replaces it.
- A runner that is killed or loses its network sends no goodbye; the other
  then counts down for 15 s and stops (`rollback:peer-timeout`).
- A save in a frame that ran with a wrong guess still ends the session; no
  save site seen so far comes within the 8-frame window of a live round.
- The condition reads two fighters; Tag matches and rounds without a time
  limit are unverified.

## Replays

Each lobby session, from a room or `--netplay-host` and `--netplay-join`,
records a replay into the player's own play folder as
`.recomp-storage/replays/<local time>.dr2r` (`replay.c`). The file stays
on that computer and is never sent: it holds the other player's profile.
It carries what the session needs to run again on the same build: the
netplay build, the local player, the input delay and the host's
options, the swap the session started at, both profile blobs as the lobby
swapped them, and both guest ports' pads for every frame. A frame goes in
once it has run and both its pads have arrived, so the file holds only
confirmed input and never a prediction. Repeated pads are stored as a count.
The runner flushes the file about once a second and when the session ends,
so a crash loses at most the last second; a frame cut short is dropped on
playback.

`--replay <file>` plays one back. It refuses a file from another build,
unpacks the two blobs into `.recomp-storage/netplay` as a session does,
adopts the recorded options and boots. From the same first frame it feeds
the recorded pads with rollback off, including the walk to Versus, and
plays as the recording player, so the fight HUD names the same side in
yellow (`draw_fight_name`). It stops with exit code 0 at the end of the
file (`replay:end`), or with 4 when the session does not start at the
recorded swap (`replay:start-mismatch`). `RECOMP_ROLLBACK_FRAMES` and
`RECOMP_ROLLBACK_DUMP` stop it and write guest RAM as for a session.

Two scripted runners played room sessions through a local matchmaker at
input delay 0 and stopped at a set frame with guest RAM dumps. Each side's
replay then ran in that side's play folder to the same frame:

| Frames | Live rounds | Rollbacks (host) | Replay against its live side |
|---|---|---|---|
| 5,000 | rollback from 1,401 to 3,932 and from 4,667 | 319 (359 frames run again) | both sides identical |
| 7,000 | rollback from 1,455 to 3,495, 4,137 to 5,475 and from 6,253 | 1,138 (1,147 frames run again) | both sides identical |

None of the comparisons masked a byte beyond SyncTest's host-scheduled
ranges. The two live sides differ from each other in two pages, the
per-player HUD bytes. The 5,000-frame file is 278 KB: 93 and 78 KB of
profiles and 107 KB of frames, where the scripted joiner pressed a new pad
every frame of a live round.

Limits of replays:

- There is no menu entry; replays play from the command line.
- Files are never pruned.
- Playback replaces the netplay save folder, as each session does.
- A replay needs the same runner build and the same cached archives.
- There is no pause, seek or speed control.

## Spectating (design)

Not built. A spectator would watch a running match as a streamed replay:
the host already writes the bytes a spectator needs, and the replay reader
already plays them. What it would take:

- Transport: the session socket accepts only its peer. The host would also
  accept a spectator's address there, with its own messages kept out of the
  session decode: a hello with the spectator's build, then requests for the
  replay's bytes from an offset, answered in 1 KiB chunks and asked again
  after a burst or 100 ms, as the profile swap does. A spectator sends no
  pads, so frame sync and acknowledgements ignore it. The stream costs the
  two profiles once (at most 2 MiB) and then 1 to 37 bytes a frame.
- Host: keep the replay's bytes in memory beside the file, answer requests
  from the frame boundary and the stall loop, and cap the spectators and
  the bytes sent per frame so watching never slows the match. Relayed
  sessions would refuse spectators to spare the matchmaker's bandwidth.
- Matchmaker: a matched room leaves the list. Running rooms would need to
  stay listed as watchable, and the server would pair a spectator with the
  host for hole punching. That is a server change and a redeploy.
- Menu and runner: a WATCH entry for running rooms on FIND MATCH; the
  spectator reboots into a replay session whose reader grows as chunks
  arrive, starts once about two seconds of frames are buffered, shows
  WAITING FOR THE MATCH when the stream runs dry, and goes back to the room
  list when the host says goodbye or the stream ends.
- Joining late: a replay can only start from session frame 0, so a late
  spectator would run every frame so far hidden and unpaced first, about
  40 s for a three-minute match at the measured 3.7 ms per hidden frame.
  Letting spectators join only between matches avoids that.
- Testing needs a third play folder.

