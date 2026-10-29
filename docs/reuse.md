# Sister-project reuse

Widescreen and antialiasing follow committed DOA3 source at
`26672b6` and DOAXBV at `0e5b1ea`. DOA2U already had their MSAA/SMAA presenter
implementation; this pass connects the build to the same upstream SMAA
revision `71c806a838bdd7d517df19192a20f0c61b3ca29d`. CMake fetches a
hash-verified archive rather than adding another submodule. Upstream MIT
notices remain with that source. DOA2U's verified native widescreen branches
use XC_VIDEO's widescreen bit without DOA3's extra letterbox flag. Shared-depth
full-frame effects now retain depth under scaling/MSAA instead of inheriting
the sister presenter's depth-disabled fallback.
The Windows launcher follows committed DOA3 `tools/launcher.cs` at
`6c782d5`: one C# file, built by the in-box .NET Framework 4 compiler into
`Launcher.exe` (build, settings, play) and `DOA2U.exe` (play only). It keeps
DOA3's Horizon Bar menu, controller, keyboard and mouse input, and its
AI-generated beach backdrop (`tools/launcher-background.jpg`). The export
checker's hash-pinned `binary_files` come from DOA3's
`tools/public_export.py` at the same commit; path, extension and size rules
still apply to pinned files.
Choices stay in `private/launcher.json`. DOA3's volume, free camera and
swimsuit pages are omitted; DOA2U adds surround and intro-movie pages, extends
MSAA through 32x, and runs its authenticated Python tools for the build and
play. Resolution presets keep the same height/480 scaling contract.

The macOS and Linux ports take the host layer of committed DOA3 at `48d05e9`
(its PR #7), which came from DOAXBV's macOS and Linux ports: `win32_compat`,
the POSIX fiber switch, the SDL3 audio output, the scheduler and vblank waits,
the save flushes, the path handling in `kernel_file.c` and the macOS and Linux
launchers. DOA2U's own additions are kept on top of them: the HLSL draw shader
and its constants move from the Direct3D 11 presenter into
`d3d_draw_shader.cpp` so the SDL_GPU presenter compiles the same source, the
SDL_GPU presenter follows DOA2U's Direct3D 11 backend rather than DOA3's
(volume textures, bump and water combiners, scaled offscreen targets, depth
bias, coverage probes and visibility counts), and the SDL audio output adds
DOA2U's voice routing and I3DL2 reverb. The reverb network is altered from
FAudio 26.10 under its zlib license (see NOTICE).

The frame-buffer correction follows committed DOA3 fixes `bb479cf` and
`c10423f`: constant-alpha blend decoding and the shared host-frame flag with
a separate previous-frame selector. DOA2U keeps its verified 5849 surface
layout; no sister-game addresses or uncommitted source are copied.

The starter takes committed public source from DOA3 at
`69cb910a13a737534fe2272cffd6211b1edf8d8e`. DOAXBV was inspected at
`1cdbd62167d4a0825f73895604311a2c761164b2`. Local uncommitted changes,
private inputs, generated code and caches were excluded.

## Already brought over

| Area | Source and coverage |
| --- | --- |
| Shared models | APU, OHCI, fibers, directory enumeration, symbolic links, CRI service lifecycle and D3D frame commands; existing synthetic suites |
| Audio helpers | Xbox ADPCM decoder and DirectSound HRTF; existing standalone tests |
| Movie helper | Planar color conversion; existing standalone tests |
| Input | Controller model, SDL host, scripted pulse source and persistent settings; synthetic API and virtual-pad tests |
| Saves | Save transaction journal, recovery and validation; existing tests |
| Timing | Guest clock and Windows vblank pacing, including the existing unpaced test |
| Tooling | Verified ISO extraction, synthetic importer tests, XBE metadata inspection and public export checker |

`runtime.h` and the input/presenter model headers are retained as dependencies
of these tests. Their presence alone does not supply a complete runtime or renderer.
The initial snapshot kept `program_manual.c` empty. Bring-up now includes one
verified XAPI kernel-compatibility entry; the native host owns kernel lifetime
instead of patching a retail kernel PE or registering its INIT/LDT segment.

The next bring-up pass also reused the core register runtime, kernel dispatch,
files, memory, crypto, thread scheduler, stop reports and read-only XBE parser.
The new loader and allocator reserve bootstrap state beyond the selected
image. The runner has no sister-game startup hooks or copied library address
bindings. Additional MMX helper coverage is required by this lift rather
than by a copied game-specific hook.

The initial D3D8 adapter also reuses committed DOA3's semantic D3D11 backend,
presenter worker/capture, vertex-program translation, draw/texture helpers and
frame models. Its 5849 device, resource, viewport, tile, gamma and lighting
bindings are separate. No DOA3/DOAXBV static device, surface address, push
buffer or game-state hook is enabled. Capture and real presenter lifecycle
tests cover the reused host; synthetic API tests cover the new guest layout.
The audio pass brings over the committed DOA3 XAudio2 PCM/capture host and
the independent buffer clock. It excludes the sister manager/APU layouts,
opaque unmapped handles and effects-download success stub. New 5849 bindings
own device/buffer views and headroom/data/reference contracts. Host status
distinguishes explicit mute from device failure.

The input pass ports the controller model and SDL host from committed DOA3
`9fce2e5958dc0322a7474aada684445e1954ffc4`. The 5849 adapter uses separately
verified entry points and packed structures. Feedback completes synchronously
through the native kernel's event service. No sister-game input addresses or
game-state hooks are enabled.

The effects decoder and guest view are independent new code. Native FX uses
the installed XAudio2 I3DL2 implementation rather than retail DSP programs.
The host tests compare dry and wet impulse tails and exercise native routing
and teardown failures. Original SDK decoder comparisons remain private.

## Good next candidates, with work required

The frame-state pass reuses DOA3's fixed-function declaration builder, and
the fiber pass ports DOA3's native fiber adapter from committed source at
`5d350856dff80141491bb5569cf6730c4466fef4`. Both keep only the DOA2U 5849
addresses and TLS layout; DOA3's binding tables and DOAXBV probes stay out.

| Candidate | Reusable part | DOA2U work |
| --- | --- | --- |
| Kernel and startup | Ordinal dispatch, files, memory, crypto, threads and cooperative scheduler | Verify exports, launch behavior, TLS and memory allocation against the actual image |
| D3D8 | Creation/draw/state/texture/shader models, semantic presenter and D3D11 backend | Find 5849 functions, verify device/resource layouts and ABI, remove title-specific presentation assumptions |
| Input and fibers | SDL host, controller model and native fibers | Rebind XAPI/XPP entry points and verify polling, vibration and stack transfer |
| Audio | PCM/XAudio2 output, ADPCM and service models | Rebind DirectSound 5849 objects and verify stream timing, loops, 3D state and CRI worker behavior |
| Movies | CRI lifecycle and color conversion | Check this revision's media format, decoder and worker contract before enabling playback |
| Build pipeline | Extraction, hashing, authenticated manifests and lifter orchestration | Supply DOA2U function discovery, ABI and manual-binding recipe; select the game rather than its launcher |

DOAXBV is the source for broader host/platform support. DOA3 supplies the
closer fighting-game investigation and some newer rendering work. Neither is
a drop-in runner for this title.

## Boundaries that prevent blind copying

- SDK builds differ: DOA2U records 5849, DOAXBV 4928, and DOA3
  XAPI 3911 / D3D8 3925 / DirectSound 3936.
- In DOA3, omitting `RECOMP_DOAXBV_BINDINGS` selects DOA3 bindings. It does
  not disable interception. DOAXBV's lookup also enables its own addresses.
- Both runners contain title-specific globals, startup shortcuts and
  presentation behavior. Their launchers, recipes and generated forwarders
  need replacement rather than a title string change.
- The selected DOA2U image exceeds DOA3's reserved kernel-data and fixed
  heap-start boundaries. The initial allocator was excluded; the bring-up
  allocator now derives its start from the loaded image and reserves
  bootstrap state in the final MiB of guest RAM.
- Some tests named `*_model_test` also execute address adapters. Only the
  independent suites were retained.
- DOA3 collision logic and DOAXBV vacation, animation and idle-mode hooks
  remain in their respective projects. Similar engine ancestry does not
  establish identical game-owned structures.

Shared decisions stay in DOAXBV's [custody](https://github.com/NoRain211/doaxbv-re/issues/2),
[presenter](https://github.com/NoRain211/doaxbv-re/issues/6), and
[audio/movie](https://github.com/NoRain211/doaxbv-re/issues/8) issues.

The resource/draw pass connects those helpers to verified 5849 resource,
stream, pixel-shader and draw entry points. It also carries over the committed
DOA3 render-state model and exact four-tap filter recognition. Native capture
now owns non-linear texture payloads. The bindings retain explicit limits for
unrecognized combiners, programmable vertices and external resource
allocator ownership; no sister-game state hooks are enabled.

The lighting snapshot ports committed DOA3's directional/point presenter
contract from `9fce2e5958dc0322a7474aada684445e1954ffc4`. DOA2U uses its verified
5849 active-slot array, material/source shadows and transforms. Synthetic
snapshots and native pixel readback cover that connection. Unsupported lighting
configurations stop rather than falling through to an unlit approximation.
The same committed presenter supplies the reflection material; its 5849 binding
verifies texture1 transforms, generated coordinates and matching samplers.
Reflection diffuse now selects stream color for unlit colored vertices, while
the evaluated lighting path retains material alpha and post-stage specular.
The premultiplied texture-alpha variant extends this same semantic material
with a per-draw flag. Its Xbox operation ordering is verified separately from
the PC enum; native pixels check intermediate saturation and alpha behavior.
The 5849 screen-space offset setter feeds a per-draw pixel translation through
the same WVP path; SDK raster bias stays in the guest shadow. Native tests
cover perspective, blended transforms, subviewports and pretransformed draws.
One allowed scissor rectangle follows the same target scaling; native pixels
check its edges and restoration without changing geometry or viewport clipping.
The four-tap filter now reads its factors from the live constants and accepts
a second, identity-scale combiner variant. The bright pass and MODULATE2X bloom
composite extend the same filter block in the shared constant buffer; native
pixels distinguish the Gaussian kernel from linear and box sampling.
