# Frame generation

The game runs at 60 frames per second and its logic is tied to that rate.
**Frame rate: Display** fills a faster display with generated frames between
the game's own frames, so motion looks smoother at 120 Hz or more. It is
experimental and off by default.

It costs input delay: each of the game's frames reaches the screen about 8 to
15 ms later than without it (8.3 ms at 120 Hz, 14.6 ms at 480 Hz). See [Latency](#latency) before you turn it on
for a fighting game.

In this page a *real frame* is a frame the game drew, a *generated frame* is
a picture made between two real frames, and *k* is the number of pictures
the window shows per real frame.

## What it is and is not

It is image-only interpolation, written for this project. The presenter
keeps the last two real frames, estimates how the picture moved between
them, and draws the pictures in between.

It is not DLSS Frame Generation or FSR 3 Frame Generation. Those take motion
vectors and depth from the game engine through their SDKs. DLSS Frame
Generation also needs an RTX 40 series or newer GPU, and FSR 3 needs
Direct3D 12 or Vulkan. The lifted game gives the presenter neither motion
vectors nor a usable depth for this, and one renderer is Direct3D 11. So the
motion is estimated from the finished pictures, as AMD Fluid Motion Frames
and Lossless Scaling do.

The passes are fragment shaders within Shader Model 4. Both renderers
compile the same HLSL from `recomp-runtime/d3d_framegen_shaders.h`:
Direct3D 11 with FXC, at feature level 10 or higher, and Vulkan through
shaderc.

## When it runs

Turn it on with one of these. It takes effect at the next launch.

- **Frame rate: Display** on the Windows launcher's (`Launcher.exe`) VIDEO
  SETTINGS page or in the game's PC SETTINGS. Both save the choice, and when
  the game starts through the launcher or `run_game.py`, the saved choice
  takes priority over `RECOMP_D3D_FRAMEGEN` set by hand.
- `--framegen` on `tools/run_game.py` (`--no-framegen` turns it off).
- `RECOMP_D3D_FRAMEGEN=1` in the runner's environment.

The display's refresh rate is read once at startup and sets k:

- **A whole multiple of 60 Hz** from 120 Hz (120, 180, 240, 300, 360, 480 Hz…, within 0.5%):
  k = refresh / 60. Each picture takes one refresh on VSYNC.
- **Any other rate, with VRR: On** (Direct3D 11 only):
  k = floor((refresh - 3) / 60). 144 Hz and 165 Hz give 2, 200 Hz gives 3,
  280 Hz gives 4. The pictures are presented with tearing allowed,
  1 / (60 k) seconds apart on the game's 1/60 s grid. That is at least 3 Hz
  below the panel's maximum, so VRR stays in its range.
- **Anything else** stays off and logs `framegen unsupported` with the
  reason: a refresh below 120 Hz, a rate that is not a whole multiple of
  60 Hz without VRR, or a VRR rate with no two slots below its maximum.

A window moved to another display keeps the k it started with.

It is off, and the game presents its real frames as usual, in these cases:

- Under the frame clock: netplay and synctest. Online play never uses it.
- While fast-forward is held.
- Under immediate present.
- While the window is hidden or minimized.
- In an unpaced run (`RECOMP_UNPACED=1`).
- On macOS and Linux. Only the Windows presenter worker presents pictures
  between real frames; the other platforms log `framegen unsupported`.

When it comes back on, the next real frame starts the history again.

## The pipeline

All sizes below follow from the render resolution (the main target). The
*finest level* is the picture scaled down by a whole factor so it is at most
720 high: factor 1 at 480p and 720p, 2 at 1080p and 1440p, 3 at 2160p.

### For each real frame N

1. **History.** The picture after MSAA resolve and SMAA, but before the
   gamma ramp, is copied to `fg_cur`; the frame before it is `fg_prev`.
   Blending happens before gamma because the game's own pixels and alpha
   blends live there.
2. **HUD mask.** The frame's 2D layer draws are drawn again into an 8-bit
   mask the size of the picture. See [The HUD mask](#the-hud-mask).
3. **Luminance pyramids.** `kFlowLuma` makes Rec. 601 luminance at the
   finest level, then three more levels, each half the size (4 levels,
   `kFlowLevels`). The pyramid of N-1 is kept from the frame before.
4. **Block matching.** One vector per 8x8 block of a level, by the sum of
   absolute differences against the other frame. The coarsest level
   searches ±4 pixels around zero (`kFlowCoarseRadius`). Each finer level
   searches ±2 pixels (`kFlowRadius`) around twice the coarser level's
   vector. An equiangular fit of the neighbouring sums refines each vector
   to a fraction of a pixel. The search can follow up to 53.5 finest-level
   pixels per axis per real frame.
5. **Both directions.** The search runs forward (N-1 to N) and backward
   (N to N-1), so each frame has a field of its own.
6. **Confidence.** `kFlowConfidence` gives each finest block a confidence
   from 0 to 1, the product of four terms:
   - forward and backward vectors agree (none at 2 pixels apart);
   - the match is close (none at 8% mean difference);
   - the search window told candidates apart (none in a flat window);
   - the share of the block the HUD mask leaves uncovered.
7. **Cut test.** See [Scene cuts](#scene-cuts).

### For each generated frame at time t

t is (slot + 1) / k of the way from N-1 to N.

1. **Select** (`kFlowSelect`). Each finest-level pixel picks one motion:
   zero, or the forward or reversed backward vector of one of the 2x2 blocks
   nearest to it. The pick is the one whose 3x3 luminance patches of N-1
   (moved back by t) and N (moved on by 1 - t) differ least. A doubtful
   vector and a vector far from the confident ones around it cost extra, and
   zero wins ties. The pick gets a trust from 0 to 1: the confidence near
   the pixel (none below 0.2), times how well the patches agree (none at 15%
   mean difference), times how close the pick is to the confident vectors
   around it (none 3 finest-level pixels away).
2. **Warp** (`kFlowWarp`). N-1 is sampled back by t along the motion and N
   on by 1 - t. The two are blended by temporal distance, 1 - t and t. The
   result fades toward a plain crossfade of the two frames as trust drops.
   Zero motion is the crossfade itself. Motion and trust are sampled
   bilinearly so they change smoothly between finest-level pixels.
3. **Present.** The picture goes through the gamma ramp, the aspect fit and
   the overlay, as a real frame does, and is presented.

### The HUD mask

A moving scene must not drag the life bars, timer or text with it, and a
still HUD must not hold the scene behind it at zero motion.

The presenter queues each draw of the game's 2D layer a second time: the
HUD, names, menus and logos. Fades and backdrops that span the screen's
width are left out, and so is a screen effect laid over the whole height
from one of the game's render targets. At present, these draws go into the
mask with a shader that outputs how much of the scene each one hides: all
of it when unblended, its alpha when blended over the scene, its brightness
when added.

The mask is used in three places:

- A block's confidence falls with the share of it the mask covers, so the
  still HUD does not pull the scene's vectors toward zero.
- A warp sample that reads any 2D layer in its own frame gives way to the
  other frame's sample, or with neither, to the crossfade.
- A pixel the 2D layer covers in either real frame shows the nearer real
  frame: N-1 before t = 0.5, N after. A quarter of the scene hidden counts
  as covered. A timer or counter that changes stays one sharp number.

If the mask cannot be created, the presenter logs it and warps without one.

### Scene cuts

Interpolating across a camera cut would smear two unrelated pictures. Each
pair of real frames is tested on the GPU (`kFlowCutBlocks`, `kFlowCut`):

- Per 8x8 block of the coarsest luminance level, the best normalized
  correlation of N-1's block with N within 4 pixels. A block matches at 0.7
  or more. Correlation ignores a change of brightness, so fades and flashes
  still match.
- A block votes as much as neither HUD mask covers it, and not at all where
  either frame is flat there (deviation under 1%).
- The pair is a cut when at least a quarter of the blocks voted, under 30%
  of the votes matched, and the pair before was steady (at least half of
  its votes matched).

At a cut, the generated frames show N-1 with its HUD, so the cut lands with
real frame N, as at 60 Hz.

### Fallbacks

- If the flow passes cannot be created, the generated frames are a plain
  crossfade of N-1 and N (`blendShader`).
- If the crossfade cannot be created either, they show the nearer real
  frame.
- If the history textures cannot be created, frame generation turns off.

Each failure is logged once.

## Scheduling and presentation

When real frame N arrives, its k *slots* start. Slot j (0 to k - 1) is due
j / (60 k) seconds after N arrived and shows the picture (j + 1) / k of the
way from N-1 to N. So slots 0 to k - 2 are generated frames and the last
slot is N itself. `recomp-runtime/d3d_framegen_schedule.c` holds this rule.

- **Who presents.** The game's present only records N and returns. The
  presenter worker calls `d3d_backend_idle`, which presents the slot that is
  due and returns the time until the next one. The worker calls it when it
  is idle and also between the draws of the next real frame, so a slot does
  not wait for that frame to finish. The worker sleeps on a high-resolution
  timer between slots.
- **Late slots.** A generated frame more than half a slot late is skipped,
  never shown late. The real frame's slot stays due until the next real
  frame arrives.
- **No free image.** The presenter never waits for a swap chain image for a
  generated frame; it skips the slot. For the real frame it tries again
  every 0.5 ms. If N+1 arrives first, N is not shown.
- **Frames in flight.** Vulkan allows three frames in flight, which also
  asks for three swap chain images: one shown, one queued, one free.
  Direct3D 11 uses a flip chain of three buffers with a maximum frame
  latency of 2, and presents with `DXGI_PRESENT_DO_NOT_WAIT`.
- **VRR slots** (Direct3D 11). A timed slot presents at sync interval 0
  with tearing allowed, at its time on the game's 1/60 s grid, so the
  jitter of the presenter's own work does not reach the panel. Its
  generated frame is made before its time comes. Two presents never start
  closer than one refresh apart; a slot that fell that far behind waits, up
  to one refresh.
- **Stall reset.** A real frame that arrives long after the one before it
  (a load) starts the history again, so no generated frame mixes it with a
  stale picture: more than 25 ms (1.5 game frames) on Vulkan, 40 ms on
  Direct3D 11, whose gap also holds the presenter's own work. A change of
  aspect or of quality settings does the same. The slots of that frame all
  show the frame itself.

## Latency

Frame generation adds input delay. This is inherent in interpolation: a
picture between N-1 and N cannot be made before N exists. So when N
arrives, the generated frames are shown first and N is shown at its last
slot, (k - 1) / (60 k) seconds later than it would be without frame
generation.

| Display | k | Added delay of each real frame |
| --- | --- | --- |
| 120 Hz, or 144 Hz and 165 Hz with VRR | 2 | about 8.3 ms |
| 180 Hz, or 200 Hz with VRR | 3 | about 11.1 ms |
| 240 Hz, or 280 Hz with VRR | 4 | about 12.5 ms |
| 300 Hz | 5 | about 13.3 ms |
| 360 Hz | 6 | about 13.9 ms |
| 480 Hz | 8 | about 14.6 ms |

The delay grows toward one game frame (16.7 ms) as k grows. On top of it
comes the GPU time of the flow and warp passes, and with VRR a late slot
can wait up to one more refresh.

A real frame is never late for its own slot because of a generated frame:
late generated frames are skipped. That is a statement about the schedule,
not about latency. The slot itself is later than a present without frame
generation.

Half a game frame or more matters in a fighting game, where moves are timed
to single frames. That is why frame generation is opt-in, and why it turns
itself off under the frame clock: netplay and synctest always present the
real frames directly.

## Known limits and artifacts

- **Doubtful motion ghosts.** In flat or noisy areas such as water, in
  repeating patterns, and at the edges of moving objects, where one frame
  shows what the other hides, trust is low and the picture blends toward
  the crossfade. Moving objects show a double image there.
- **Fast motion.** Motion beyond the search's reach (53.5 finest-level
  pixels per axis per real frame) finds no match and also falls back to the
  crossfade. A very fast camera move can be taken for one cut.
- **The HUD and what is behind it update at 60 Hz.** Where the 2D layer
  covers a pixel, the generated frame shows the nearer real frame, scene
  and all. Under a see-through HUD element the scene does not move between
  real frames.
- **Width-spanning 2D draws are not masked.** Fades and backdrops as wide
  as the screen are treated as scene and can be warped.
- **Skipped slots.** A slow GPU or a busy swap chain skips generated
  frames, which shows as uneven motion rather than as extra delay.
- **One display.** k is fixed at startup; moving the window to a display
  with another refresh rate needs a restart.
- **Windows only**, and VRR slots are Direct3D 11 only. VRR is not yet
  verified on a VRR monitor.

## Debugging and evaluation

- **Flow view.** `RECOMP_D3D_FRAMEGEN=flow` shows the forward field in
  place of the generated frames, as color per search block: hue is the
  direction, brightness the speed, dark blocks have low confidence, the 2D
  layer is whitened, and the whole picture is tinted red at a cut.
- **Counters.** With `RECOMP_PERF_COUNTER=1` the presenter logs one
  `recomp framegen:` line a second:

  | Field | Meaning |
  | --- | --- |
  | `k` | pictures per real frame |
  | `real` | real frames shown |
  | `generated` | generated frames shown |
  | `late` | generated frames skipped because their slot had passed |
  | `not_ready` | generated frames skipped because no swap chain image was free |
  | `real_unshown` | real frames replaced by the next one before they were shown |
  | `interval_ms p1 p50 p99 max` | time between shown pictures |
  | `refreshes 1= 2= other=` | intervals within a quarter slot of one slot, of two slots, and the rest |
  | `window_max_ms` | the longest single slot present, from its start to its return, without a VRR slot's wait for its time |

  A steady run shows `generated` near (k - 1) x 60, `real` near 60, and
  almost all intervals under `refreshes 1=`.
- **Capture and replay** (Vulkan only). `RECOMP_D3D_FRAMEGEN_CAPTURE=<folder>`
  writes what frame generation gets for each real frame: the picture before
  gamma, the HUD mask, the gamma ramp, and whether the frame started the
  history again. `RECOMP_D3D_FRAMEGEN_CAPTURE_AT` (first present, default 1)
  and `RECOMP_D3D_FRAMEGEN_CAPTURE_COUNT` (default all) select the frames.
  Frame generation then also runs with `RECOMP_HEADLESS=1`. The game runs
  slowly while it writes, and a 2160 frame takes 41 MB; keep the quality
  fixed during a capture. The frames are game output: keep captures under
  `private/`. If the folder or its `framegen.txt` cannot be written, the
  presenter logs `framegen capture off` with the reason and captures
  nothing more; a frame it cannot download or write in full, its HUD mask
  included, logs `framegen capture failed` and is left out of the index.
  Direct3D 11 logs that capture is unsupported.

  Built with the runtime tests,
  `recomp_d3d_framegen_replay <capture> <output>` renders every picture the
  window would show at k times 60 Hz (`--k 4` by default) through the
  presenter's own frame generation, after gamma, as `<frame>-<slot>.bmp`
  files. `--crossfade` replaces the generated frames with the plain
  crossfade, and `--original` repeats each real frame k times, as the game
  looks at 60 Hz. `--ffmpeg` and the options after it pipe the frames to
  `ffmpeg` on `PATH` instead, with `<output>` as its output file, for
  example `--ffmpeg -c:v ffv1`. Those options and `<output>` go through
  `cmd.exe`, so the tool refuses any that hold `"`, `%`, `^`, `&`, `|`, `<`,
  `>` or a line break. The first real frame starts the history.
- **Tests.** `recomp-runtime-d3d-framegen-schedule` checks the slot rule
  and the VRR plan. The presenter tests (`recomp-runtime-d3d-presenter` for
  Direct3D 11 and `recomp-runtime-d3d-presenter-vulkan`) check that real
  frames are unchanged, the flow of known motion, the warp and its edges,
  the HUD mask, cuts and restarts, and that a capture replays bit for bit.
  They skip the schedule on a display where frame generation is
  unsupported.

## Code map

All files are in `recomp-runtime/`.

- `d3d_framegen_shaders.h`: the shared HLSL of every pass, the search radii
  and level count, and the builder that declares each pass's textures.
- `d3d_framegen_schedule.h`, `d3d_framegen_schedule.c`: when each slot is
  due, which slots are late, and `recomp_framegen_plan` (k and the slot
  mode for a refresh rate, with or without VRR).
- `d3d_framegen_schedule_test.c`: tests of the schedule and the plan.
- `d3d_presenter_sdl_gpu.cpp`: the Vulkan side. Search for `framegen`:
  `framegenRecord` (history and slot start), `framegenFlow`, `framegenCut`,
  `framegenCover` and `queueCover` (HUD mask), `framegenGenerate`,
  `framegenPresent`, `framegenIdle`, `framegenCount` and `framegenCapture`.
- `d3d_framegen_d3d11.h`: the Direct3D 11 side, included by
  `d3d_presenter_d3d11.cpp`: its resources, the HUD mask replay, the window
  presents and the VRR slots.
- `d3d_presenter_thread.cpp`: the presenter worker that calls
  `d3d_backend_idle`.
- `widescreen.h`: `recomp_widescreen_2d_layer`, the test for a 2D layer
  draw.
- `d3d_framegen_capture.h`, `d3d_framegen_replay.cpp`: the capture reader
  and the offline replay tool.
- `d3d_presenter_thread_test.cpp`: the presenter tests (`testFramegen*`).
