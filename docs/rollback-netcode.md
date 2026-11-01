# Rollback netcode

Status: design and the save/replay core. Two runners will connect through a
host-owned lobby and play the offline Versus mode, with rollback over the
fight. A frame-locked clock and SyncTest (`docs/bring-up.md`, Determinism
checks) save, replay and compare whole frames, with guest RAM in page
snapshots (`guest_snapshot.c`). SyncTest replays one frame at a time from a
single save point; rollback needs a ring of module states beside the RAM
ring, input injection and a transport. Game addresses below come from
static analysis of `/DOA2.xbe` in Ghidra unless a section says otherwise.

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
  audio output, while their models keep advancing.
- Frames that write saves or present from the display mode change cannot be
  rewound. A rollback window must not reach back past one.
- Both peers must produce identical floating-point results. The same build
  on x86-64 is the safe case; macOS arm64 and host libm differences are
  unverified.

## Entry point

The Live menus need XOnline logon, match search, QoS, friends and stats
services before they reach a fight, and no System Link text appears in the
XBE. A host-owned lobby connects two runners and then drives the offline
Versus mode, with the remote player on the second controller port, which
avoids all of XOnline. Menus stay in delay-based lockstep and rollback covers
only the fight.

