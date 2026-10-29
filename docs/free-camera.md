# Free camera

The free camera takes over the game's camera and flies it anywhere in the
scene, for screenshots or a closer look at the fighters and stages. It is off
by default. Turn on **Free camera** in the launcher (the **FREE CAMERA** page
of `Launcher.exe`), pass `--free-camera` to `tools/run_game.py`, or set
`RECOMP_FREE_CAMERA=1` before the runner starts. Then, in a fight or a
cutscene, press **F1** or **`** (backquote), or click the right stick, to take
the camera; press again to give it back. The controls are the DOA3 port's,
without its swimsuit controls: no control uses **Back**, which the game acts
on itself.

While it flies, the pad and keyboard are the camera's, and the game reads a
still controller, so it ignores what moves the camera. The game itself keeps
running until you freeze it.

| Action | Keyboard | Controller |
|---|---|---|
| Take / give back the camera | **F1** or **`** | Click the right stick |
| Move | **W A S D** | Left stick |
| Down / up | **Q** / **E** | Left / right trigger |
| Look | Arrow keys, or drag with the right mouse button | Right stick |
| Zoom in / out | **Z** / **X** | (move closer with the left stick) |
| Faster / slower | **Shift** / **Ctrl** (held) | **A** / **B** (held) |
| Next camera the game built | **C** | **X** |
| Back to the view it started from | **R** | **Y** |
| Freeze / resume the game (P works anywhere, camera on or off) | **P** | **Start** (while flying) |
| Clean view: hide the panel and the game's 2D overlay | **H** | Left shoulder |
| Advance one frame while frozen; hold to keep stepping | **.** (period) | Right shoulder |
| Turn to the next fighter | **\\** (backslash) | D-pad left |
| Screenshot of the game's frame, without the panel | **F12** (works with the camera off too) | D-pad right (while flying) |

While the camera flies, a panel at the top left names the camera and whether
the game is frozen; a line along the bottom lists the controls: the
keyboard's after a key was last used, the controller's after a controller was
(and from the start on a Steam Deck). The camera starts where the game's
camera was. Its speed scales with how far the game's camera stood from what
it looked at, and it steps by the time since its last step, so it flies at
the same speed at any frame rate. Giving the camera back resumes a frozen
game.

The launchers save the choice in `private/launcher.json` as `freeCamera`, and
`tools/run_game.py` turns it into the variable.

## Screenshots

**F12**, or the d-pad's right while flying, saves the game's frame as a PNG
without the panel or anything else the runtime draws over it. Screenshots go
to `DOA2U` in the user's Pictures folder: on Windows the Pictures known
folder, which follows a relocated or OneDrive-backed Pictures; elsewhere
`~/Pictures/DOA2U`. `RECOMP_SCREENSHOT_DIR` chooses another folder.
`screenshot.c` writes the PNG without an image library. The Direct3D 11
presenter saves the window's back buffer, the SDL_GPU presenter the game's
frame at its rendering size.

## How it works

DOA2U keeps five camera objects of 0xCE0 bytes from `0x00805F70`. The game's
camera logic sets each one's eye (+0x00), target (+0x10), vertical field of
view (+0x30) and roll (+0x34), both in radians, and keeps a working copy of
them at +0xA8, +0xB8, +0xD8 and +0xDC. The camera task (`0x0003E490`, task
slot 11) builds the frame's view and projection matrices (+0x150, +0x190)
from that copy with `0x000399D0`, which takes the object in ESI. This is the
layout and build of DOA3's cameras (`0x0002A540` there), compiled with SSE
instead of x87; the build's first call (`0x00034510`, mode 1) derives the
angles from the working copy's own eye and target, as DOA3's does.

Every direct call in the lifted program asks `recomp_lookup_manual` first, so
`recomp-runtime/free_camera_adapter.c` wraps that build without changing the
generated program or its hash. With the camera allowed, the wrapper puts the
free camera's eye, target and field of view in both places and a level roll
in the working copy, then runs the lifted build (`sub_000399D0`). The game
then builds its matrices, and culls, for the free camera. When the camera is
given back, the game's camera logic takes the object over again. The flying
itself is `recomp-runtime/free_camera.c`, shared with the DOA3 and DOAXBV
ports and tested by `free_camera_test.c`.

The free camera flies the first camera object built after it is taken up; in
fights that is the main view. It takes the next one built whenever the one it
flies stops being built, and **Next camera** moves it to the next object the
game built in the previous frame.

DOA2U's fight cameras stand far back with a narrow lens (about 17 units from
the target at 19 degrees, a fighter being under 2 units tall), so the camera
flies at a quarter of the DOA3 port's speed.

**Turn to the next fighter** faces each fighter's hips in turn. Each fighter
keeps a bank of 32 bone matrices (64 bytes, row-vector, world space) from
`0x00875C20`, 0x800 bytes apart, hips first; slots 2 and 3 hold tag partners.
In fights, 22 of the 32 hold matrices: bones 0-15, 17, 21, 24, 25, 28 and 29.
DOA3 keeps 23 bones in banks 0x5C0 apart.

## Freezing the scene

DOA2U runs its systems as cooperative tasks, one turn each per frame on its
own fiber. The task table at `0x00A94570` holds 24 records of 0x1C bytes:
`+0` the state (bit 0 in use, bit 2 running, bit 3 finished), `+4` frames to
wait, `+8` the fiber, `+0xC` the function. The scheduler (`0x000FA440`) keeps
the running task's index at `0x00A94814`; a task yields through `0x000FA3D0`,
which clears bit 2, stores its wait and switches back to the scheduler's
fiber at `0x00A94810`. `recomp-runtime/scene_freeze_adapter.c` hooks the
fiber adapter's `SwitchToFiber` and, while frozen, declines the turns of
tasks that have not drawn lately (`scene_freeze.c`, shared with the DOA3
port and tested by `scene_freeze_test.c`). The drawing tasks keep their
turns, so the last state is drawn again, and presented, every frame from
wherever the camera is. Two tasks that draw nothing are kept too:
`0x00116B20` (slot 12) keeps what the drawing tasks draw current, and the
camera task builds the cameras while the free camera flies. Without
`0x00116B20` a frozen fight stops within about 35 frames on a stale pointer in
the drawing task `0x0005C3E0`. The game polls the controller from a task the
freeze skips, so while frozen the camera samples it once a frame itself. A
frame step lets every task take its turns until the next frame is presented,
then holds the scene again.

The fighters hold still, but a stage's own drawing task keeps animating what
it draws: falling leaves and swaying branches move while the scene is frozen.

The hooks are installed only with `RECOMP_FREE_CAMERA=1`; without it, input
samples, task switches and the camera build go through unhooked.

The clean view leaves out the game's fixed-function draws that are already
in screen space (the life bars, timer, names, the attract logo and lens
flares) in `d3d8_draw_adapter.c`. Screen-space effect passes run a pixel
shader and stay.

## Keys and the mouse

The input host (`input_host_win32.c`) reads the keys the camera uses for
itself through the keyboard model the presenters feed: window key messages on
Windows (with `GetAsyncKeyState` while the game window has the keyboard), SDL
key events elsewhere. A right-mouse drag turns the camera by the pixels
moved. On Windows it honors swapped mouse buttons and starts only over the
game's window.

For the game, the input host presses controller buttons for some keys: A, S,
Z, X, Q, W, E and R press X, Y, A, B, White, Black and the triggers, the
arrows press the d-pad, and Return and Backspace press Start and Back. The
free camera leaves those buttons out of what it reads while their keys are
down, since it reads the keys themselves. With the camera allowed, the right
stick's click goes to the camera and never to the game.

## Options

| Variable | Effect |
|---|---|
| `RECOMP_FREE_CAMERA=1` | Allows the free camera (the launchers' **Free camera**). |
| `RECOMP_SCREENSHOT_DIR` | Folder for screenshots. |
| `RECOMP_FREE_CAMERA_AT=<n>` | Test: takes the camera up at the n-th camera build. |
| `RECOMP_FREE_CAMERA_DRIFT=right,up,forward,turn,pitch` | Test: holds those controls (each -1 to 1) while flying. |
| `RECOMP_FREE_CAMERA_FREEZE_AT=<frame>` | Test: freezes the scene at that presented frame (`RECOMP_FREE_CAMERA_HIDE=1` also hides the overlay). |
| `RECOMP_FREE_CAMERA_TEST_AT=<frame>` | Test: requests a screenshot and turns the camera to the next fighter. |

## Recorded runs

On 2026-10-29, on macOS (Apple Silicon, SDL_GPU on Metal), in the attract
demo's Kasumi fight (`--isolated --skip-intro`, frame dumps at 480p):

- Taken at build 4800 with no drift, the camera held the game's view. At
  frame 2800 `RECOMP_FREE_CAMERA_TEST_AT` turned it to fighter 0, whose hips
  then projected to the centre of the game's own view and projection
  (normalized 0, 0), and saved a 720x480 PNG of the game's frame.
- Frozen at frame 3000 with the camera still, the fight stayed frozen to the
  end of the run (frame 7722, about 79 seconds) without a stop, and the
  fighters stood still in frames three seconds apart; the stage's leaves and
  branches moved. Keeping any other non-drawing task instead of `0x00116B20`
  stopped the runner within about 35 frames.
- Frozen at frame 3300 with `RECOMP_FREE_CAMERA_HIDE=1` and a drift, the
  camera moved through the frozen stage, and the clean view left out the
  attract logo and the lens flare.

The Direct3D 11 presenter's overlay and screenshot follow the DOA3 port's
and were not run on Windows for this port. Keys, the mouse and the pad
buttons were not pressed in these runs; the test variables stand in for them.
