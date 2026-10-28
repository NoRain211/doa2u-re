# DOA2 Ultimate bring-up

- [Established baseline](#established-baseline)
- [Memory and startup](#memory-and-startup)
- [Building and running](#building-and-running)
- [Graphics API contract](#graphics-api-contract)
- [Code layout](#code-layout)
- [Game tasks](#game-tasks)
- [Saves](#saves)
- [Input API contract](#input-api-contract)
- [Audio API contract](#audio-api-contract)
- [Determinism checks](#determinism-checks)
- [Remaining work](#remaining-work)

## Established baseline

The project has public source, synthetic tests, custody checks, a pinned lifter
and a native kernel/graphics/audio runner. The actual game has been lifted; its
generated source and receipt remain private. Library API replacements and game
compatibility remain bring-up work.

Ultimate's launcher and main game are separate executables. Read the ignored
`private/target.json` to obtain the selected game path and its hashes; do not
assume the disc's default executable is the game.

The ISO hash establishes import consistency, not independent authentication of a
retail disc revision. Keep the selected revision provisional until its identity
has been checked against a trusted independent reference.

Private notes hold actual graphics/audio startup and boot observations.

## Memory and startup

### Guest memory

The allocation arena starts at the loaded image's page-aligned end and stops at
`0x03F00000`. The remaining guest RAM holds the bootstrap stack, kernel exports,
PCR and TLS. PCR occupies `0x03FF0800` through `0x03FF0BFF`; generated FS reads
and native thread snapshots use that dedicated block. Physical page zero remains
available to the SDK startup pool and cannot overwrite PCR. Heap and contiguous
allocations grow toward each other and reject overlap. The loader checks
sections, source digests, stack/TLS bounds and reserved ranges before running
code.

Memory statistics report the arena's available pages and reusable allocation
capacity, with image, pool, stack and virtual reservations accounted for. Raw
runtime allocations and alignment gaps stay consumed. Cache pages are zero
because this host has no separate guest cache-page allocator.

### Bootstrap thread

Bootstrap TLS data is aligned to 16 bytes, with its pointer slot included in the
stack-base calculation used by XAPI's negative TLS index. The selected entry
creates the startup worker and returns; the host then schedules that worker.
Closing its handle invalidates external use while retaining the worker until it
exits.

### XAPI kernel entry

The XAPI kernel-compatibility entry is a temporary whole-library replacement:
the native host owns its kernel's executable lifetime, so console kernel
patching and INIT/LDT registration are unnecessary. Its no-argument calling
convention is tested; it consumes the return slot and leaves game globals
untouched.

### Seeded indirect targets

SDK initializer-table callbacks are supplied through the public seed recipe
rather than replaced by success stubs.

- Game handlers reached only through data tables (the game-mode table at
  0x6959f0 and the four-entry handler groups at 0x7c0040) are seeded the same
  way when the lifter misses them; an unresolved indirect call names the next
  one.
- Tail-jump switch cases past a null table slot (0x2a82bc, cases 0x2a8247 and
  0x2a8286) are seeded too.
- A lone in-function arm ended by a null slot (0x2929f4) resolves in the lifter
  as a local switch.

### CRT remainder intrinsic

The CRT remainder intrinsic at `0x336dfe` is replaced as a whole with native
`fmod`. It consumes the x87 divisor at ST(0) and dividend at ST(1), returns one
x87 result, and pops only the return address. This avoids unseeded internal
special-value dispatch branches such as `0x338e10`. Numeric results include NaN
for domain errors; guest CRT errno, math-error callbacks and unmasked
floating-point exceptions are not implemented by this seam.

### Timing and vblank seams

The 5849 GetTickCount boundary reads shared monotonic time and offers bounded
cooperative scheduler polling. D3D BlockUntilVerticalBlank and its two CRI tail
aliases use the native shared-vblank service. These platform seams let the
existing worker modules run; they do not force middleware load status.

## Building and running

### Build authentication

`tools/build_game.py` authenticates the selected XBE, lifter revision, generated
source manifest and final runner. CMake rejects partial or wrong authentication,
chunk gaps and uninitialized generated EBP prologues. Missing function bodies,
unsupported imports and untranslated instructions stop explicitly.

### Bounded probe

Use `tools/run_game.py` for a bounded probe. It hardlinks the disc files
(copying them on drives without hardlinks), preserves saves and logs the runner
identity. Private notes hold the observed frontier; a timeout by itself does not
establish gameplay progress.

### Normal play

For normal play, open `play.cmd` or run `python tools/run_game.py --play`. It
uses the player's existing saves and physical controls with no time limit. Close
the game window to exit. Session logs, including the runner identity and any
runtime stop, are kept under `private/play-logs/`.

### Intro movies

The startup legal presentation is skipped automatically at its game-owned
wrapper (`0xd92c0`), before its menu and asset setup. The intro movies play
unless `--skip-intro` (the launcher's **Skip intro movies**) is set. Then the
logo movie (`0xd9300`) keeps its setup and teardown but omits its playback wait,
and the boot opening movie (scene 5, ticked by `0x69180`) ends on its first
tick; attract replays and story movies still play.

### Soft reset and dashboard requests

The game's soft reset relaunches its own XBE through XLaunchNewImage
(`0x2bab9d`); the runner exits with code 4 and `run_game.py` starts a fresh
runner, so the relaunch boots like a cold start without its launch data.

The game's own dashboard requests, such as **Yes** on the Xbox Live "No accounts
found" dialog, take the same path: the game tears down its tasks before the
call, and the PC port has no dashboard, so the title restarts. Answering **No**
returns to Mode Select. XapiBootToDash still stops the run.

### Dedication card

The optional dedication card (`RECOMP_DEDICATION_CARD`, set by `run_game.py`
from `private/dedication.bmp`) starts on the first guest read of the staff roll
track. Music table entry 17 (`0x6a0c3c`) points at the track's file name at +8,
and the runner finds that entry through each AFS archive's own directory. Track
loops seek past the entry's start, so they do not repeat the card. At the next
Swap the runner presents the card through the D3D8 adapter for 10 seconds and
then subtracts the held time from the guest clock. In Kasumi's Story the track
is first read after the final KO's white flash, so the card plays just before
the ending, which carries the staff credits over it.

### Story finale shortcut

`--story-finale` (runner and `run_game.py`) holds every live Story CPU at 1 life
and sets the stage to 5 at the first CPU KO, so the game's own advance loads the
final fight. Addresses were measured from Kasumi's Story:

| Value | Address | Notes |
|---|---|---|
| Mode | `0x87cfa0` | 3 in Story |
| Stage | `0x8783ec` | 0..6, Tengu last |
| Human flag | `0x87ae30` | Per side, stride `0x94` |
| Life | `0x87ae54` | 16-bit, per side, stride `0x94` |

Setting the stage during fight setup crashed in `0x120e0`. A scripted Kasumi run
went through one fight, the Tengu scenes and fight, the dedication card, the
ending with credits, the epilogue and the CG Gallery unlock without a stop.
Other characters' Story lengths are unverified.

### First-boot HDD install

On first boot the game installs `voice.afs`, `datahdd.afs` and `datahdd2.afs`
(about 700 MB) from `D:\doa2` to the `Z:` cache. `check_hdd_install` (`0xf6cc0`)
accepts a cached archive whose size matches the disc's (the host reports no file
times outside save profiles). Otherwise `hdd_install_thread` (`0xf70c0`) streams
it 32 KB at a time behind `g_hdd_install_gate` (`0x8a9920`: 0 runs, 1 asks to
pause, -1 is paused, 2 stops), and ADX init holds the boot on a black screen
until `voice.afs` is in; measured on this runtime that alone took six minutes.

ADX init busy-waits on the load while the CRI file server
(`cri_file_server_thread`, `0x2fc240`) runs at base priority -2
(`cri_set_server_priorities`, `0x2fc4a0`), so it gets only the moment after each
vblank. The legal screen (`0xd92c0`, shown only while an install is pending)
waits a frame at a time and lets it run; skipping that screen since `2d474b5`
took the copy from about 1 MB/s to 60 KB/s. With the screen restored and no
cache, `voice.afs` installed in 23 s and the opening movie began about 70 s
after launch, the rest installing behind it.

`run_game.py` therefore copies the three archives into
`private/play-disc/.recomp-storage/partition5` before launch, replacing any of
the wrong size. They are copies, not hardlinks: the installer reopens a stale
archive with `CREATE_ALWAYS`.

## Graphics API contract

### Widescreen

`RECOMP_D3D_WIDESCREEN=1` supplies XC_VIDEO 0x004f0000 (widescreen, no
letterbox) instead of the standard 0x004e0000. The selected image's
XGetVideoFlags (0x2bb87e) returns bit 0 from this setting; game initialization
(0x282570) retains it and requests presentation flag 0x10. The device model
accepts that flag at creation and Reset. The game's existing camera and UI
branches select their 16:9 behavior, and the presenter sizes the window to
match. No game matrices or guest code are patched.

### Anti-aliasing and render scale

The Windows build includes pinned upstream SMAA shader and lookup tables.
`RECOMP_D3D_MSAA=1/2/4/8/16/32` selects main-target samples, with GPU capability
fallback; `RECOMP_D3D_SMAA=1` applies SMAA 1x Ultra to the resolved image before
gamma. Both can run together. Native GPU tests compare each mode against the
same AA-disabled image and cover widescreen window/target size, scaled
shared-depth offscreen rendering, resolve, gamma and resource release.

The launcher exposes 480p through 2160p, mapped to `RECOMP_D3D_SCALE` as
height/480. The presenter's existing display-size and downsampling behavior
applies. A 1080p GPU test requests 32 samples and checks the resolved output and
shared depth after capability fallback.

### Frame-buffer textures and blending

Textures aliasing either embedded frame-buffer surface use host-rendered
storage. The front-buffer selector chooses the previous presented frame; both
selectors retain the presenter's shared frame-buffer flag. Constant-alpha blend
factors (`0x8003`/`0x8004`) retain their native blend semantics. Adapter and GPU
pixel regressions cover the final-stage style frame blend that would otherwise
cover the scene with blank guest memory.

### Polygon offset

Polygon offset now survives the simple-state model, captured draw and native
rasterizer. The 5849 `ZBias(1)` receiver state is slope `-0.25`, D24 units `-1`,
with fill offset enabled; resetting it restores zero bias. The reflection and
projected-shadow receivers both use this state to separate their polygons from
the floor. Active nonfinite parameters or fractional/out-of-range constant units
stop explicitly. Disabled offset does not consume those parameters.

The native rasterizer uses the
[D3D11 depth-bias equation](https://learn.microsoft.com/en-us/windows/win32/direct3d11/d3d10-graphics-programming-guide-output-merger-stage-depth-bias)
with the existing D24 depth surfaces. Scaled main targets reconstruct slope per
guest pixel in the receiver pixel shader, independently for X and Y, after
clipping; constant bias remains in the native rasterizer. With multisampling,
sample positions restore each sample depth while color and alpha retain their
pixel-center evaluation. This scaled MSAA path requires feature level 10.1.
Adapter, queued-capture and native pixel tests cover parameter transfer, sign,
slope, scaling, 4x sample visibility and restoration.

### Device and frame (`d3d8_adapter.c`)

`d3d8_adapter.c` owns the whole 5849 CreateDevice boundary and uses the
committed sister-project D3D11 presenter. The plain device, frame and tile
models remain callable independently of interception. Creation accepts the
verified 720x480 linear BGRA8/D24S8 mode and two caller-provided color headers
plus depth. It validates their storage before creating the host device and
publishes the static guest device only after host creation succeeds.

The device has 5849's embedded 24-byte surface headers and owner/binding
references. Getters add references, Release preserves retained surfaces, and
default Swap rotates Data/Lock fields while keeping header addresses fixed.
Viewport extents are clamped; invalid origins stop. Tiles retain their six-word
API descriptors without exposing hardware. Clear and default Swap submit
semantic commands, with the shared guest-vblank pacing model.

### State (`d3d8_state_adapter.c`)

`d3d8_state_adapter.c` binds gamma, ten transforms, point/directional light
records and active slots, and fog color. Gamma uses two identity-initialized SDK
shadow banks; Swap consumes the bank selected by preincrement frame parity.
Light-array growth repairs active pointers. HRESULT and void calling conventions
have separate synthetic ABI checks. CreateDevice replays the SDK render-state
defaults from the game's own tables into the guest shadow and the simple-state
model, so state the game saves and restores (such as the color write mask) keeps
its SDK value. Complex-state defaults reach only the shadow.

### Frame state

The frame-state adapter records semantic depth, stencil, blend and alpha-test
state through the shared render-state model. Simple states use the SDK's
fastcall convention, with separate guest shadows maintained by the caller.

SetVertexShader rebuilds the fixed-function declaration and applies the 5849
dirty bits. Program handles load their CreateVertexShader upload blocks into the
device program shadow and select the object as LoadVertexShader and
SelectVertexShader do; SetVertexShaderConstant1/4 keep the 192-entry constant
shadow. Declaration-only handles and declaration constants stop explicitly.
VertexBlend preserves the Xbox mode encoding: 0 disables blending; 1, 3 and 5
select one, two and three explicit weights plus the remaining weight. Draw
submission checks that encoded mode against the FVF and passes a weight count to
the presenter. Other mode encodings remain guarded. SetMaterial retains its
17-word device shadow. Spot lights, nondefault raster modes and runtime
multisampling remain explicit limits.

### Resources (`d3d8_resource_adapter.c`)

`d3d8_resource_adapter.c` binds registered textures, palettes, vertex/index
buffers and surface views. Binding references retain storage independently of
visible references. Native-created vertex buffers and mip surface wrappers have
owned lifetimes; final destruction of external headers is guarded until their
allocator is verified. Locks validate mip/rectangle spans. Write locks retire
native textures and rendered targets in their span. Registration drops only
cached texture uploads: the attract demo re-registers a render-target texture
between rendering and sampling it. Queued draws own all submitted vertex, index,
palette and texture bytes, including non-linear formats. Cube/volume views and
displaced swizzled locks remain unsupported.

The XPR loader (FUN_00289f80) registers volume textures (a 64x64x8 A8R8G8B8 map
appears after a Story fight), so Register invalidates a swizzled, uncompressed
volume's full span: width x height x depth per level, each axis halving to 1.
Stage-0 fixed-function FVF 0x142 draws can sample a single-level A8R8G8B8 volume
through a native D3D11 Texture3D. The upload unswizzles all three axes; UV0
supplies S/T, with R zero unless COUNT3's texture matrix supplies it. U/V/W
addressing and linear filtering apply. Other volume formats, mip chains,
coordinate generators, locks and views remain guarded.

### Draw adapter (`d3d8_draw_adapter.c`)

#### Fixed-function snapshot and texture filtering

`d3d8_draw_adapter.c` binds DrawVertices, DrawVerticesUP and DrawIndexedVertices
to the existing D3D11 presenter. It snapshots fixed-function layouts,
transforms, viewport, depth/blend state, stage-0 texture and palette. Supported
combinations include texture/diffuse modulation, white diffuse alpha selection,
texture alpha selection and texture factors.

Linear min/mag filters support point or linear mip selection with LOD bias
encoded as the lazy texture-state upload (0x34f400) does: 1/256 steps, truncated
after +0.5 and clamped to the 13-bit field, so NaN and out-of-range biases clamp
to -16 or 15.99 (D3D11's maximum).

#### Native pixel shaders

Pixel-shader creation, deletion, binding and constants retain the 5849 layout;
native custom shading supports the sister presenter's four-tap filter in its
doubled and identity-scale variants, with live C0/C1 factors, and the
post-process bright pass.

The bright pass matches its exact combiner words and Gaussian-cubic stage-0
filtering, then computes saturate(2 * (gauss3x3(t0) - c0) - 1) with the 1-2-1
kernel at level 0.

The bloom composite binds stage-0 MODULATE2X or MODULATE4X of texture and
texture factor over white vertices. The native color result clamps after
scaling; alpha retains its independent stage operation. Adapter and GPU pixel
checks cover both scales, clamping and alpha preservation.

The screen tint (definition 0xa74f0, drawn over the back buffer by 0xa75d0)
matches its pushed combiner words and computes saturate(c1 * dot(t0.rgb,
c0.rgb)) for RGB and alpha, with stage-0 C0 holding the luminance weights and
stage-1 C0 the tint; fog must be off so 0x34f5f0's default final combiner
outputs r0.

#### Primitives and vertex records

Quad strips draw as triangle strips. Quad lists (type 8, including
FUN_0023d2a0's four-vertex draws) expand each group of four indices to two
triangles, preserving vertex order and ignoring an incomplete final quad. UP,
stream and indexed draws share the conversion. The split uses diagonal 0-2;
hardware interpolation parity is unverified.

The fight's trail strips (setup 0x23a42d, FVF 0x102, draw 0x23ca7e) use 1x
MODULATE(TEXTURE, TFACTOR) with a grey factor, stage-0 alpha DISABLE and stages
1-3 off, so they draw texture times factor with the white diffuse alpha.

Vertex records may overlap: SetStateUP (0x3506f0) and DrawVerticesUP copy the
full declaration and advance by the caller's stride, even when it is smaller
than that declaration. The adapter checks the final record's complete extent and
repacks these records before queuing the draw. Regression cases cover the
four-UV layout with a 24-byte stride, reordered indices, and a final record
exactly at the guest-RAM boundary; extending it beyond RAM still stops.

#### Stage distortion pass

The stage distortion pass (FUN_0027b070, definition 0x7af5d8) draws FVF 0x140404
quads, 56 bytes each, with t0 PROJECT2D, t1 DOTPRODUCT and t2 DOT_ST using the
MINUS1_TO_1_D3D dot mapping. One combiner sets r0 = t2, and the final combiner
words are zero, so with FogEnable (0x3553d0) off, 0x34f5f0 outputs r0 plus v1
when specular (0x3553fc) is enabled, with r0.a as alpha. SetVertexShader
(0x3493de) supplies zero for this FVF's absent specular attribute; the
pretransformed passthrough programs copy it to v1. The result is therefore r0
regardless of SpecularEnable, including state left by water rendering. The
adapter matches those words, keeps the 48 bytes the shader reads and swaps
textures: the stage-2 scene copy becomes the primary texture and the stage-0
normal map the second. The presenter decodes n = (b - 128) / 127 from the normal
texel and samples the scene at (dot(tc1.str, n), dot(tc2.str, n)). Fog, other
layouts or different words keep the vertex-layout stop.

#### Stage overlays

The same effect's overlays bind with fixed-function stages. FUN_0027be10 draws
scene objects (FVF 0x142, unlit). Stage 0 has color SELECTARG1(TEXTURE) and
alpha MODULATE(TEXTURE, CURRENT), where stage-0 CURRENT is diffuse. Its
texcoords come from TCI_CAMERASPACEPOSITION with COUNT3|PROJECTED. For that case
the TEXTURE0 upload (0x34f7d0, case 0x331) emits S, T and Q as matrix columns 0,
1 and 2. The presenter interpolates (s, t) and q and divides per pixel. Stage 1
keeps the color and applies alpha MODULATE(TFACTOR, CURRENT) with ALPHAKILL. Per
object, FUN_0027be10 binds a stage-1 texture (0x27c370, table 0xeaad20) before
its fan (DrawVerticesUP, 0x27c388) or model (FUN_002878b0 without the material
binder). Stage 1's ops never read it, so it only discards pixels whose texel
alpha at UV0 (TCI 0) is zero; alpha is not multiplied.

FUN_0027c400 then draws a pretransformed quad (FVF 0x244). Its stage-0 alpha
takes TFACTOR times diffuse. Stage 1 applies alpha MODULATE(TEXTURE, CURRENT)
from a mask sampled at UV1, with ALPHAKILL, which discards zero mask alpha. An
unbound mask passes through, as 0x352e46 compiles it. Unlit stage-0 selection
reuses the lit path's per-channel choice of texture and diffuse. A bound stage-1
texture under the TFACTOR variant on another coordinate set, a mask on another
coordinate set or a linear or paletted mask still stop.

#### Camera-space position texgen

Unprojected camera-space position with COUNT2 also binds for rigid world draws.
The SDK upload (0x34f7d0, case 0x320) keeps the full S/T columns and sets Q to
one; the adapter supplies that constant column to the same presenter path, so
the texture matrix's third column cannot cause a divide. Native pixels check
world-view position and S/T translation independently of mesh UVs.

Normal inverses are captured only for generated reflections with a normal
stream. Camera-position texgen, mesh UVs and the no-normal eye-vector path
accept finite singular world-view matrices; a required invalid inverse stops
with the layout, generating matrix, influence and caller.

#### Lighting, materials and stage-0 combiners

The fixed-function draw snapshot binds up to eight directional/point light
slots, front material and ambient, normalization, blended world/normal
transforms and infinite-view specular. Missing vertex colors resolve material
sources as the SDK does.

Light snapshots validate only RGB coefficients and the fields consumed by the
selected light type. Directional lights use the normalized record direction;
point lights use position, range and attenuation. Unused descriptor alphas, spot
parameters and disabled specular RGB do not affect the draw. Non-finite consumed
fields still stop with the light slot, type and field index.

On stage 0 a CURRENT argument reads diffuse: the combiner compiler flags the
first stage (0x352aba) and its arg mapper (0x3529c0) then maps CURRENT to V0, so
a fight's lit MODULATE(TEXTURE, CURRENT) binds as texture times lit diffuse. Lit
stage 0 also accepts MODULATE(TEXTURE, TFACTOR) in either argument order,
independently for RGB and alpha. The SDK arg mapper (`0x3529c0`) maps TFACTOR to
combiner constant C0. The presenter selects that factor instead of lit diffuse
for the chosen channel and adds lit specular after the texture stages. Unlit
colored vertices use the same selection for MODULATE(TEXTURE, TFACTOR): the
factor replaces vertex diffuse in RGB, while alpha independently uses factor or
diffuse. This includes the FVF 0x142 mesh draw at 0x287120. Adapter tests check
both channels and argument orders; native pixels check tint, alpha and specular
ordering. Unlit streams without vertex color also admit texture times factor in
both RGB and alpha (FVF 0x102, draw return 0x1a1edd), through the same
per-channel selection. The RGB-only factor flag is cleared so it cannot apply
twice; adapter tests cover world/screen layouts and native pixels check alpha.

Stage-0 COLOROP DISABLE ends the fixed-function combiner chain with diffuse RGB
and alpha (`0x352b1e`), ignoring ALPHAOP and later combiners. Bound texture
sampling remains active (`0x34f080`), including stage-0 ALPHAKILL. Adapter and
native pixel checks cover diffuse/material alpha passthrough and that rejection.

Material checks cover only what the SDK uploads: diffuse, ambient and specular
RGB (0x34fb90), emissive RGB and diffuse alpha (0x34fc80), and specular RGB and
power (0x34fe40) only while specular is enabled. Non-finite unused terms draw; a
used one stops with its index and bits. Diffuse alpha is the exception: 0x34fc80
sends it unchanged as the lit alpha, so a NaN there (a play session wrote
7fc08080) saturates by sign to 1 or 0 as a passthrough vertex color does on
hardware. That rule is unverified for material alpha. Native pixel tests cover
diffuse/ambient, point range/attenuation, specular, normal transforms and alpha.

#### Reflection material

The reflection material binds the sister presenter's two-texture straight or
premultiplied texture-alpha blend and final diffuse modulation, with generated
reflection coordinates or mesh UV0. Each stage keeps its own ALPHAKILL: a
zero-alpha sampled texel discards the fragment before the blend, independently
for stages 0 and 1. Native pixels check each discard and an opaque surviving
fragment. Each stage also keeps its own mip filter, LOD bias and WRAP or CLAMP
addressing. The fight's chrome (Zack, Stage 01) clamps its environment map while
the base texture wraps, and FUN_002807f0 and FUN_00280f80 write a stage-0 LOD
bias without touching stage 1's. MAXMIPLEVEL, BORDER and MIRROR addressing stay
guarded.

The material binder (FUN_00286d30) takes each stage's ops from table 0x7b0078,
indexed through 0x7affc8. Stage 2 MODULATEs by diffuse in mode 5 and by its own
texture in modes 9 and 11. With no stage-2 texture bound, 0x352e46 compiles the
texture variant to a CURRENT pass-through, so the blend skips the final diffuse.
A bound stage-2 texture would need a third sampler and still stops with
`d3d8:reflection-stage2-texture`.

The binder also reaches modes 3, 4, 4, 5: a base, two successive
BLENDTEXTUREALPHA stages, then diffuse. The second blend binds a third native
sampler, with stage 2's own mip filter, bias and WRAP/CLAMP addressing. Stages 1
and 2 generate reflection coordinates through TEXTURE1 and TEXTURE2
respectively; both use the same eye and normal inputs. The queued draw owns the
third texture's bytes. Native pixels cover its alpha endpoints and midpoint, its
independent matrix and CLAMP addressing, and the final diffuse RGB/alpha. Other
third-stage combiners and coordinate sources remain guarded.

Other fight chrome (draw 0x28708b, FVF 0x112) swaps the coordinate sources:
stage 0 samples the environment through camera-space reflection texgen (TEXTURE0
matrix) and stage 1 blends the mesh-UV0 base by its own alpha. The combiner is
the same, so the presenter runs stage-0 texgen alongside the mesh-UV blend.

Texgen on both stages (material modes 3, 4 and 5 with coordinate mode 1 on
stages 0 and 1, byte +0xf through 0x7b018c) generates one camera-space
reflection vector and transforms it by each stage's own matrix: TEXTURE0 for
stage 0, which FUN_00289680 sets to the 0.5/-0.5 sphere-map scale and offset,
and TEXTURE1 for stage 1, which the game leaves at Reset's identity
(FUN_0034bc80) apart from the FUN_00268de0/FUN_00269010 and
FUN_0027b700/FUN_0027b900 save and restore pairs. Stage 2's MODULATE(DIFFUSE,
CURRENT) is the blend's final diffuse multiply.

Generated coordinates on a draw without a normal stream (fight FVF 0x142) take
the zero inline normal: no SDK path writes v2 inline data (0x34aac0 sets only v0
and v9 through SetVertexData2f), so the reflection vector is the eye direction.
Unlit vertex diffuse and lit material diffuse retain their respective alpha
sources.

Skinned reflection draws (including FVF 0x116) blend camera-space positions and
normal-matrix results with the position weights and final remainder. The SDK
upload at 0x3502d0 supplies each world-view matrix; with NORMALIZENORMALS,
0x34b530 supplies signed cofactors before the final normalize. Native pixel
tests cover both endpoint weights and an equal blend. Exact hardware rounding
remains unverified. Vertex color lighting sources, local-view specular,
two-sided/spot lighting, other combiners and multiple streams stop explicitly.
The premultiplied blend clamps its intermediate result before diffuse modulation
and post-stage specular.

#### Screen-space offsets and scissors

Screen-space offsets retain the SDK's biased shadows and snapshot API-relative
pixel translations. The presenter applies them after projection, including
blended and XYZRHW draws, without moving viewport clipping. Nonzero offsets on
programmable vertex draws remain guarded. SetScissors snapshots one allowed
rectangle in target pixels; the presenter scales its edges independently of the
draw's screen-space offset. Zero rectangles restores viewport clipping,
including pretransformed draws. Viewport, target and reset calls clear the
explicit rectangle as the SDK does. Multiple/exclusive rectangles and negative
edges remain guarded.

#### Programmable vertex draws

Programmable vertex draws snapshot the bound program, the constant shadow and
the viewport scale/offset constants c58/c59 that GetViewportOffsetAndScale
computes. They admit the position/normal/UV declaration and the sister
presenter's stage-0 texture select with an optional stage-1 alpha mask. As the
5849 combiner compiler does, an op that reads an unbound texture passes CURRENT
through. Stage-0 ALPHAKILL uses the same sampled-zero-alpha rejection as fixed
draws, before vertex or texture-factor alpha multiplication. Stage-1 ALPHAKILL
remains guarded. Other declarations, combiners and pixel shaders stop
explicitly.

#### Projected character shadow

The projected character shadow uses program 0x7af4d0 (created by FUN_00279b30)
with a position/normal declaration and no texcoord stream, drawn as FVF 0x012.
FUN_0027a430 renders the shadow into a linear 512x256 A8R8G8B8 target.
FUN_0027a760 then projects that target onto the stage. The program writes oT0 =
(s, t, r, max(q, 0)) through MAC MAX and SLT, and the presenter divides s and t
by q per pixel in texel units. Stage 0 has color SELECTARG1(TEXTURE) and alpha
MODULATE(TEXTURE, TFACTOR), with the factor's alpha. FUN_0027aa70 binds the same
program but then resets the device through FUN_00289680, which binds FVF 0x112,
so it never draws with it.

#### Other vertex programs

Program 0x7ae238 (created by FUN_0026f850 and drawn by FUN_002718e0) declares
position, D3DCOLOR v3 and UV, drawn as FVF 0x142. It writes oPos from c96-c99,
oD0 = c112 and oT0 = v9. Program draws now select stage 0 per channel, as fixed
function does. This draw uses MODULATE(TEXTURE, oD0) for color,
MODULATE(TEXTURE, TFACTOR) for alpha, and its mipped texture's own mip filter
and LOD bias.

The other program on that declaration (0x7add80, FUN_0026bd20) and the stage
effect's 0x7afb48 (FUN_0027ffc0) use LIT, SGE, oD1, oFog and oT2/oT3. The
translator follows D3D8 vs.1.1 LIT: y = max(x, 0), and z = pow(y, w) with w
clamped to +-127.9961 only when x and y are positive. oD1 is passed to the pixel
shader for the water combiner; oFog and oT2/oT3 also reach it as separate
fields. Program draws through the fixed-function stage path still ignore fog and
specular.

#### Stage effect (FUN_002807f0)

The stage effect's declaration 0x7afd0c (position, D3DCOLOR at 12, v9-v12 as
float2 at 16-40) binds as FVF 0x442, a 48-byte stride.

FUN_002807f0 draws the stage effect with the pixel shader that FUN_0027ffc0
builds: t0 PROJECT2D, t1-t3 BUMPENVMAP of t0 (texture modes 0x318c1, input
texture 0). Combiners 0-1 square T3.a twice, combiner 2 sets r0 = t2 * (1 -
T3.a), and combiner 3 multiplies by v0. The shader's own final combiner, which
0x34f5f0 keeps because its words are nonzero, outputs lerp(fog.rgb, r0, fog.a)
with v0.a. T1 is never read, and stage 1 has no texture. The adapter matches
those words exactly and passes stage 0 (one of 17 animated 128x128 V8U8 maps,
format 0x28 with COLORSIGN 0xe0000000), the stage-2 scene, the stage-3 mask, and
the stage-2/3 BUMPENVMAT00-11. The presenter reads the map as R8G8_SNORM with U
first and samples t2 and t3 at oT2/oT3 plus M n. Fog follows 0x34f5f0's
parameters: LINEAR stores bias end/(end - start) + 1 and scale -1/(end - start),
and mode 0 stores 1 and 1, so f = bias - 1 + scale * oFog. With FogEnable off,
FOG.a is taken as 1. Exponential fog, other maps or signs, and other shader
words stop. One session reached this draw on a stage transition with stage state
that matches FUN_002807f0, but its bound program declared 0x112. Its
30-instruction pipeline fits 0x7af0f0 (FUN_002786e0), but why that handle was
bound there is unverified. The host draws the bound program with this shader,
and oT2/oT3 read as zero when it does not write them.

#### Water (0x26c980)

Water draws in an unanalyzed function at 0x26c980 with program 0x7add80 (FVF
0x142; it writes oPos, oD0, oD1, oFog, oT0, oT1 and oT3.xy) and the pixel shader
that FUN_0026bd20 builds: t0 PROJECT2D, t1 and t3 BUMPENVMAP of t0, no t2
(texture modes 0x300c1). Its seven combiners compute s = max(0, dot(expand v0,
expand t3))^8, r0 = v1 * t1.a, r0 = (s - 0.5) + (r0 - 0.5), r0 += t3.a * t1 and
r0.a = t3.a * v0.a, with the stage effect's final combiner. The draw binds one
of the 17 V8U8 maps on stage 0, the water reflection target (FUN_0026d610) on
stage 1 and a filtered environment map (0xe5b0a8) on stage 3, with matrices on
stages 1 and 3, fog off and SPECULARENABLE on. The adapter matches those words
and reuses the bump path with stage 1 in place of stage 2; the presenter
computes the water combiner. Register clamps are applied after each step. Water
with SPECULARENABLE off stops, since V1's source without it is unverified.

### Offscreen render targets

BGRA8 offscreen color targets and D24S8 custom depth targets bind through the
shared presenter; clears and draws snapshot their storage descriptors. Binding
counts retain parent textures, and viewport extents follow the target.
Front-buffer targets, offscreen sharing of a supersampled main depth, and
partial clears remain guarded. Synthetic draw tests establish these contracts,
not title acceptance.

SetRenderTarget (0x3447d0) programs the depth offset and pitch from the depth
header alone, so an offscreen color can test against the main depth. The fight
bloom source pass (0x2756f0) binds a 720x480 texture with the main depth, clears
color only and draws its glow objects behind the scene's occluders. The adapter
admits that pairing when the guest back buffer is not supersampled. The host
gives full-frame offscreen colors the main target's scaled size and MSAA sample
count, preserving shared depth and stencil. Multisampled colors resolve to
single-sample textures before effects sample them. Smaller offscreen targets
keep one sample. Those with their own depth, such as water reflections, scale by
the render scale; depthless filter chains such as bloom and heat haze keep their
guest size, because their taps sit one guest texel apart. Custom depth follows
the matching color size. The 64 MiB offscreen budget counts guest storage,
preserving scene capacity across resolution and AA settings. Host color storage
and resolve textures grow with those settings; D3D11 allocation failures remain
explicit.

### Fences and display state

KickPushBuffer publishes queued native commands. WaitForIdle drains them.
InsertFence returns an even sequence and BlockOnFence returns at once: queued
commands own copies of their bytes, so every fence is already complete. They do
not supply render-target readback or hardware timing parity.

PersistDisplay retains the native displayed image, and same-mode Reset replaces
the guest surface headers, viewport and scissors while preserving other state
and frame count. Reset clears depth/stencil only. Resizing and other display
modes need further bindings. The lifter handles WBINVD over coherent host
memory; there is no separate guest CPU cache or direct GPU consumer of guest
bytes.

### Supersampled display mode

Reset also accepts the 2x horizontal supersample type 0x2021 used by character
select. The game supplies 1440-wide back and depth headers and a 720-wide front
header; the guest device keeps the SDK's scale, physical size and swap-filter
fields, while the host target and vertex-shader viewport constants stay in
logical 720x480 units. PersistDisplay publishes an SDK-shaped saved surface
without host pixels.

### Line drawing

Line lists draw natively; line strips expand adjacent index pairs through the
same path without closing the strip. The line-history effects at `0x19d920`,
`0x19de50` and `0x19e9a0` use these strips with XYZ|DIFFUSE vertices. UP, stream
and indexed draws retain their vertex order, line width and antialias state.
Pretransformed widths above one pixel become screen-space quads.

Untextured, unlit XYZ|DIFFUSE lines (FVF 0x42) also support transformed widths
and EdgeAntiAlias. The effect at `0x254dd0` draws those lines with widths 0.3,
0.6, 0.9 and 1.2; the adapter quantizes them to the SDK's 1/8-pixel steps. A
geometry shader clips endpoints against near/far/W planes and widens after
projection in viewport pixels. Fractional alpha coverage uses feathered strips,
with culling disabled and the ordinary pipeline restored after drawing. The
trapezoidal filter and square caps approximate hardware; exact line-filter
calibration remains unverified. Other transformed wide-line layouts and polygon
edge smoothing remain guarded. Native pixel tests cover fractional coverage,
clipping, both line directions and pipeline restoration.

### Stage-3 texture binding

The reached SDK pair writer (0x3443f0, fastcall method/data/format) binds a
stage-3 texture directly for method 0x81bc0. The adapter snapshots that binding
without changing the API resource shadow or its references. SetTexture on stage
3 restores the API binding; the pair cannot enable a disabled sampler. Swizzled
A8R8G8B8, P8, A8 and DXT1/3/5 are bound; other methods and formats, including
linear textures without a size, remain guarded. Point and multi-source draws
consume the same snapshot.

### Point sprites

Point lists draw as Xbox point sprites. With POINTSPRITEENABLE the 5849 combiner
compiler (0x352a60) compiles stage 3 only, and that stage's texture receives the
sprite coordinates. The fight's particles (0x2345c0) use POINTSCALEENABLE. The
adapter expands each point to an eye-space quad that is square in pixels and
draws it with the projection alone. Its size is Vh * Si / sqrt(A + B*De +
C*De^2), clamped to [MIN, MAX], which is the D3D8 formula the SDK's point
parameters (0x34ef10) encode.

Stage 3 admits SELECTARG1 of texture or diffuse, or MODULATE of texture with
diffuse or TFACTOR per channel. The factor uses the existing per-channel
selection and does not read material alpha when it replaces diffuse alpha.
Native pixels check sampled alpha times factor alpha without vertex or material
alpha.

Lit points admit texture-only RGB, optionally scaled by TFACTOR: the effect at
`0x1f5030` keeps LIGHTING enabled, but consumes only lit alpha. The SDK
(`0x34fc80`) takes that alpha from material diffuse alpha even with vertex-color
sources. The adapter supplies it once through the existing texture-alpha
multiplier, and ignores unused lighting RGB inputs. Texture-only alpha does not
read material alpha. Lit vertex RGB, two-sided lit points, unscaled sizes,
shaders and projections that mix eye x/y into w stay guarded. Hardware size
rounding and frustum culling of whole points are not modelled: a quad that
crosses the screen edge is clipped instead.

### Leaving character select

Leaving character select, the game (0x282770) widens the old headers, persists
the display, and Resets back to type 0x11. That Reset writes the same SDK fields
for the new mode: scales 1.0, flag 0x4000 clear, MultiSampleMode 0 and swap
filter 2. The game then restores the saved display into the 720-wide back buffer
with CopyRects; the supersampled direction uses a CPU loop instead. The host's
single main target still holds that frame, so CopyRects binds only this restore.
It stops if a color clear or draw has written the target since the Swap before
PersistDisplay.

### Shattering stage mirror

A shattering stage mirror (0x277670) copies the whole back buffer into a
render-target texture of the same size and format, then draws the shards with
it. The adapter sends that copy as a presenter command, which fills the
texture's host target from the main target, so the shards sample it like any
rendered target. Partial or scaled copies still stop. The shards (0x277950) are
pretransformed triangles drawn with EdgeAntiAlias; the host draws them without
edge alpha coverage and relies on MSAA instead.

### Display mode switch and scratch arena

Reset's mode set (0x34d58d) raises device+0x1de0. The next Swap (0x34af40) then
programs the mode and frees the persisted copy (0x34a600). Both mode switches
(0x282770, 0x282880) free their 0x175000-byte scratch arena, persist, Reset,
Swap, and only then allocate the arena again (0x282390). That arena holds a
matrix stack growing down from its end. If the persisted copy were kept, the
arena allocation would fail with guest memory full. The stack would then start
at 0x175000 and overwrite the switch table at 0x174530, a crash seen in fights.

### Stage-0 alpha kill and addressing

Fixed-function stage 0 also admits D3DTSS_ALPHAKILL ENABLE (4), which discards
fragments whose sampled texel alpha is zero. Programmable stage 0 keeps
independent WRAP, MIRROR or CLAMP U/V addressing, using the same native sampler
as fixed-function draws; a native pixel check covers mixed CLAMP/WRAP.

### Stage-0 camera-space texgen

Fixed-function stage 0 also admits camera-space reflection texgen (TCI 0x30000)
with COUNT2. That texgen reuses the reflection inputs with the TEXTURE0 matrix
(0xce0) and writes UV0; without a normal stream it uses a zero normal, as the
stage-1 path does. COUNT2 over mesh UV0, which a fight stage uses to scroll a
texture, follows the texture-matrix upload (0x34f7d0, case 0x220): s and t take
the first two columns with the third row as translation and q is 1, so UV0
becomes (u, v, 1, 0) times TEXTURE0. Projected and other counts stop.

When a lit world matrix is singular, the SDK inverse (0x34b530) fails and
uploads an unset matrix. The adapter then uses zero normals, so only ambient and
emissive light remain. A reflection draw cannot use such a matrix. It is skipped
only when its single world has 3x3 rank at most one, so every triangle has zero
area, or when native raster coverage is empty; otherwise it stops.

A stage projector (0x1df200) divides by the square of its size minus 50, so for
one frame TEXTURE0 holds infinities and NaNs. With camera-space position (w = 1)
a non-finite translation makes every S and T non-finite, and CLAMP sends them to
the texture edge. When every edge texel of a single-level DXT5 texture has one
exact value, the adapter samples that corner; other non-finite texture matrices
stop. Only columns the SDK upload (0x34f7d0) sends for the stage's COUNT are
checked: COUNT2 sends columns 0-1, so a non-finite _43 under camera-space
reflection texgen is never read.

### Multisample state

MultiSampleMode and MultiSampleRenderTargetMode (0x3477a0/0x3477d0) store their
guest shadow only. The game's 4X stencil pass on a one-sample back buffer shades
fewer pixels, but it covers every sample, as the host's full-rate rendering
does.

### Unreplaced SDK APIs

Any SDK API that is still unreplaced reserves console push-buffer space and
would then wait forever for GPU hardware. The reservation routine stops instead
and reports the calling API's return address.

### Render and texture state shadows

SetRenderState_ShadowFunc (`0x346580`, void/stdcall value) preserves state 0x9c
at `0x3554d0` without reserving console push-buffer space. Both direct game
callers (`0x27aa70` and `0x280f80`) set the SDK default `0x200`; the effect
state restore (`0x27b900`) can restore its saved shadow. Only that default is
bound. Other values stop, and depth-texture sampling remains guarded by the draw
adapter.

SetTextureState_BumpEnv (`0x346bc0`, stdcall stage/state/value) stores
BUMPENVMAT00 through BUMPENVLOFFSET (states 22-27) in the deferred stage array
`0x355060`, which the draw adapter reads; its NV2A method has no host use. Draws
do not consume those states yet. SetTextureState_BorderColor (`0x346c30`,
stdcall stage/color) likewise only shadows D3DTSS_BORDERCOLOR (state 29) there;
BORDER addressing itself still stops in the draw sampler checks. A stage-1
combiner the draw path cannot express stops with `d3d8:draw-texture-stages` and
reports the color/alpha ops and arguments of stages 0-2, the stage-3 op, the
stage-2 binding, and the stage-0/1 addressing, kill and coordinate sources.

### Visibility tests and callbacks

The first Story fight (Stage 01) plays through a 180-second probe. Its stage
fades a light glare by occlusion. Game code `0x276d40` brackets a draw with
BeginVisibilityTest (`0x3457c0`, no arguments) and EndVisibilityTest(index)
(`0x345860`, stdcall), then inserts a WRITE callback (`0x276b50`) that marks the
index ready. Next frame it polls GetVisibilityTestResult(index, &result, NULL)
(`0x344450`, stdcall) while it returns D3DERR_TESTINCOMPLETE. The visible/total
ratio of two tests feeds the fade, so only relative counts matter. The presenter
wraps the draws in a D3D11 occlusion query per END and resolves it on its worker
without waiting. Counts are normalized to guest target pixels and scaled by the
back buffer's supersample factor, as Xbox reports count samples. A pending poll
publishes queued work, as the SDK's fence kick does. No guest report slot is
written. Indices beyond the SDK's 64 report pages, an End without Begin, and a
requested report timestamp stop explicitly.

InsertCallback (`0x349e10`, stdcall type/callback/context) calls its cdecl
callback immediately: captured commands own their bytes and the host GPU never
writes guest memory, so READ and WRITE conditions already hold.

## Code layout

Library code in `.text` sits in contiguous linker ranges after the game's
objects. The split follows call direction (library code never calls game code
directly), 16-byte game function alignment, and each range's imports.

| Range | Contents |
|---|---|
| `0x11000`-`0x2b7d2b` | Game |
| `0x2b7d2c`-`0x2c00c7` | XAPI, CRT startup, XCrypt |
| `0x2c00c8`-`0x2f9e3c` | XVOICE/VOICMAIL |
| `0x2f9e3d`-`0x2fabff` | XNet/PHY helpers, kernel import stubs |
| `0x2fac00`-`0x3350ff` | CRI middleware |
| `0x335100`-`0x33f3ef` | LIBCMT/LIBCPMT, XAPI |
| `0x33f3f0`-`0x340f6f` | CRI middleware |
| `0x340f70`-`0x344163` | Game global initializers and EH stubs |

The game range also holds linker-folded inline library bodies that library code
calls: empty and constant returns (`0x4ba60`, `0x4bfd0`, `0x136dc0`), the vector
constructor iterator (`0x238d0`), a D3DX vector divide (`0x293570`) and C++
standard string and exception code (`0x26230`, `0x262b0`, `0x262f0`).

## Game tasks

### Fibers

Game logic runs as up to 24 XAPI fibers, resumed each frame by a task scheduler.
`fiber_adapter.c` replaces XAPI 5849 CreateFiber, DeleteFiber, SwitchToFiber and
ConvertThreadToFiber with native Windows fibers, ported from DOA3. 5849 keeps
DOA3's 16-byte fiber object but moves the TLS slots: the current fiber is TLS
block +0xC and the converted main fiber +0x10. Guest FS refers to the PCR block.
The guest-thread scheduler saves and restores each thread's current fiber.
Without this binding, the tasks never run and the game only clears frames.

### Unlock all

Every costume and character is offered. The game data pointer at `0x87cfa8`
(observed `0x7feae0`) holds one costume byte per character at +0x2ed8, in the
15-character order from `0x6959dc` mapped through `0x6959c0`. New saves start at
2; the select screen (`0x11e410`) shows `min(byte, count)` with counts from
`0x6e0f08`. Characters 0x11, 0x18 and 0x15 need the flags at +0x2ed2, +0x2ed3
and +0x2ed4 (`0x11e5f0`).

After each Swap, the runner repeats the game's award loops (`0xdc640`,
`0xdc100`): each costume byte becomes its count and the three flags become 1.
Loaded profiles get the same values, and the next save stores them. Unlocks at
+0x2ee7 to +0x2eeb (`0xdc4c0`) cover other content.

`--no-unlock-all` (runner and `run_game.py`) skips this step, so unlocks come
only from the game's own award paths. Acceptance runs use it with a profile that
has never run with the unlock on; earlier saves keep their unlocks.

## Saves

The game writes every save through `save_game_write` (`0x219c0`, cdecl, 11
arguments, 0 on success). XCreateSaveGame writes SaveMeta.xbx and SaveImage.xbx,
then the payload follows: `ups.dat` for the profile, `br.dat` for a Battle
Record replay. `save_adapter.c` brackets that call with the save journal, so one
save's files commit together, and a failed write or an interrupted process
leaves the previous save. The profile rewrite (`0x21eb0`) deletes the old save
first and writes a fallback when its first write fails; both stay outside the
journal, so they behave as on the console. XCreateSaveGame opens SaveImage.xbx
for write to learn whether it exists, so a missing file on that open does not
fail the save.

Save storage is host disk, outside every snapshot. SyncTest and run-ahead do not
rewind across a frame that opened a save file for writing, creating or deleting,
or that ends inside a journal transaction: the replay would find the files the
first pass wrote and take the overwrite branch.

## Input API contract

### Controller model (`input_adapter.c`)

`input_adapter.c` binds 5849 XInitDevices, device enumeration/changes,
XInputOpen/Close, capabilities, state and feedback. The shared controller model
retains packets, connection changes and per-port motor state. Packed guest
states have a four-byte packet followed by the 18-byte Xbox gamepad payload.
Feedback writes its final status and signals an optional native event handle
before returning. Rumble is synchronous; console USB allocation is host-owned.
An explicit XInputPoll binding remains unresolved.

### Keyboard

The shared SDL host maps four gamepads and keeps a keyboard controller on port
zero. Keyboard input requires window focus.

| Key | Button |
|---|---|
| Enter | Start |
| Arrow keys | D-pad |
| Z / X | A / B |
| A / S | X / Y |
| Q / W | White / Black |
| E / R | Triggers |

### Scripted input

Input scripts do not alter game-owned state.

- `--input-start-pulse-at <poll>` sends a one-sample Start edge through ordinary
  controller sampling; it can be repeated up to four times. The bounded probe
  accepts the same option.
- The runner and bounded probe also accept
  `--input-analog-pulse-at <poll> <index>` for one full-strength sample of A, B,
  X, Y, Black, White, left trigger or right trigger (indices 0 through 7). Polls
  must be positive uint64 values; up to 1024 analog edges may be scheduled, with
  one analog button per poll.
- The runner also accepts up to eight
  `--input-analog-pulse-every <first> <period> <last> <index>` rules, each
  pressing one button at poll first and every period polls after it through poll
  last, so long automated fights fit on the command line and can stop before an
  ending scene.
- `--isolated` keeps probe storage separate and disables physical input and
  rumble.

## Audio API contract

### Device (`dsound_api_adapter.c`)

`dsound_api_adapter.c` owns the 5849 DirectSound device singleton and its public
interior handle. Native caller/buffer ownership replaces the SDK's internal
hardware voices while preserving visible reference results. The shared XAudio2
backend owns copied PCM storage. Explicit mute keeps timing available;
host/device failures cannot report native readiness. Shutdown requires the COM
initialization owner thread.

### Buffers

The buffer adapter binds the 5849 descriptor and public handle layout to the
independent cursor model, PCM/Xbox ADPCM decoding and native output. Owned and
borrowed sample storage have separate lifetimes. Headroom changes adjust
effective volume; API argument cleanup is verified per entry. Buffers pump
synchronously from API polling and DoWork. The inherited clock drops output
spans after a stall over 100 ms while advancing its cursor. Native endpoint
consumption has an opt-in host test; synthetic API and backend tests remain
suitable for public CI.

### Effects image (`dsound_effects_image.c`)

`dsound_effects_image.c` decodes the encrypted DSP programs; CPU parameter
states are plaintext. The parser owns its payload/maps, validates all spans and
rejects nonempty Y storage. Its 256-map budget bounds allocation and overlap
checks. Fixed decoder initialization comes from the authenticated executable at
runtime. Synthetic vectors use independent initialization; no retail programs,
keys or state are included in public source.

### Effects adapter (`dsound_effects_adapter.c`)

`dsound_effects_adapter.c` publishes owned guest-readable descriptor, state and
scratch allocations. Failed parsing/allocation leaves the old image intact.
Immediate state writes update the published view; deferred writes stay in shadow
storage and flush with a dirty listener commit. Direct CPU state edits survive
unless explicitly overwritten by a deferred update. This initial adapter
requires an assigned reverb role. Unassigned roles are rejected before
publication, preserving the current native graph and image.

### I3DL2 reverb and DSP limits

Native I3DL2 reverb runs on a stereo XAudio2 submix, using Microsoft's
[parameter converter](https://learn.microsoft.com/en-us/windows/win32/api/xaudio2fx/nf-xaudio2fx-reverbconverti3dl2tonative).
The source keeps separate dry/wet matrices, per-route volumes and per-bin
headroom. Front stereo, wet bin 10 and muted other speaker routes are bound.
Recorded impulse tests establish actual wet processing, not Xbox bit parity; the
built-in converter ignores room rolloff. DSP programs are not executed.

Recording sends contain decimation, phase and scratch/DMA transfer semantics;
ordinary SourceVoice resampling is insufficient. Those sends remain guarded, as
do packet streams, scheduled StopEx, additional play flags and unsupported DSP
processing. This is an initial native FX subset.

### 3D voices

Listener SetPosition/SetVelocity store their vectors and dirty bits in the
settings block. A commit with a dirty position or velocity recomputes every 3D
voice.

Story fights play positioned effects through mono CTRL3D ADPCM buffers (flags
`0x40010`, created by game code at `0xfc516`). The six buffer setters DOA2 calls
are bound at their `IDirectSoundBuffer` thunks:

| Setter | Thunk |
|---|---|
| SetMaxDistance | `0x365159` |
| SetMinDistance | `0x36517d` |
| SetPosition | `0x3651a1` |
| SetVelocity | `0x3651d6` |
| SetDistanceFactor | `0x36520b` |
| SetRolloffFactor | `0x36522f` |

Each takes the public handle (+0x1C) and stores into the 5849 3D block.

The adapter follows the 3D calculator (`0x365553`, `0x364236`) with the
full-HRTF routines DirectSoundUseFullHRTF installs (`0x366d1a`):
`-2000·log10(1 + rolloff·(d/min − 1))` distance volume held at max distance,
Doppler pitch `4096·log2(1 − v/342)` added to the base pitch and clamped, and
the game's own HRTF bank (`0x36de70`) and map (`0x37f430`).

It renders through DOA3's `dsound_hrtf.c`, with 5849 truncating quantization.
DOA2's 3D voices are 11-22 kHz ADPCM; the APU resamples them in hardware,
outside the game image, with 8-point Lagrange interpolation (retail measurements
in xemu PR 3105). Linear interpolation lost up to 7 dB in their top octave
against the XAudio2-resampled 2D voices; the runtime now uses the same 8-point
Lagrange (4 source samples of latency), and like the APU steps a new HRTF filter
in over one 32-sample frame.

### HRTF taps and routing

Tap decoding below matches retail single-tap measurements (xemu PR 3105, nxdk-ex
PR 36). DSOUND writes the bank's bytes unchanged to the APU (`0x369cae`). Taps
decode as sign, 3-bit exponent and 4-bit mantissa, unnormalized: of the readings
tried, only this one gives DOA2's bank a louder near ear at every azimuth.
Linear int8 low-passed every 3D voice.

Routes 0-3 of a 3D voice carry the ears and later routes the unfiltered voice;
the center split (`0x366ae3`) pans a frontal source -3 dB into center and -6 dB
into the ears. Per-bin terms follow `0x369565`. On stereo output the front/back
split is off, so XTLK front bins 6/7 carry the HRTF ears and back bins 8/9 are
silent. DOA2's 3D bins `{6,8,7,9,2,10,3,22}` place center in slot 4, which
enables the center term (`0x366ae3`). Bin 10 uses the existing I3DL2 send.

Deferred 3D values apply at once. MUTE3DATMAXDISTANCE, stereo 3D voices, cones,
modes, orientation, listener factors and I3DL2 sources remain unbound; DOA2 sets
none of them.

### Surround 5.1

The launcher's Surround 5.1 option (`--surround`, `RECOMP_AUDIO_SURROUND=1`)
selects 5.1. The kernel answers XC_AUDIO (setting 9, read by `XGetAudioFlags`
`0x2bb8c7`) and GetSpeakerConfig (`0x364721`) with surround plus AC3
(`0x00010002`), so the game and DSOUND take their own surround branches. The
host opens a six-channel master (FL FR C LFE BL BR, the same order as bins 0-5)
and routes those bins straight through. XTLK front 6/7 go to FL/FR and rear 8/9
to BL/BR, doubled. For 3D voices, `0x364b53` turns on the front/back split
(`0x366a14`): with p = clamp(((|az|/90 − 1)(1 − |el|/90) + 1) − 0.5, 0, 1),
front bins and center take 2000·log10(1 − p) and rear bins 2000·log10(p). Within
half a meter (distance × distance factor) p eases toward 0.5. The HRTF lookup
folds directions behind the listener onto the front response (`0x366bfe` with
its fold flag set by `0x369cae`). The reverb return stays on the front pair.
Stereo remains the default.

### Crosstalk canceller and stereo downmix

DSOUND's GP kernel (`0x380670`, loaded at P:0) DMAs bins 0, 2, 1, 4, 5 and 3 to
the encode processor; its stereo program (`0x01067d58`) mixes L = FL + 0.707·C +
0.707·SL and R = FR + 0.707·C + 0.707·SR, dropping LFE.

The game's `xtalkamp.bin` effects 5-8 double XTLK bins 6-9 and effect 14 cancels
crosstalk: each ear adds a one-pole low-passed (pole 0x7645a1, feed 0xf88313)
copy of the other ear's output from 11 samples earlier, front pair into bins 0/1
and rear pair into 4/5. Its outputs address bins through a second X window
(0xc00 + bin·0x20); the effects accumulate, so the window must be the per-frame
mix bins. The runtime doubles XTLK routes and runs the canceller per 3D voice.
The game's SetEffectData calls only touch effects 1-4 and 15-18 (FX bins 23-30).

The I3DL2 reverb return now passes through the same canceller as the front XTLK
pair, left into bin 6 and right into bin 7, at unity gain.

## Determinism checks

### Frame clock (`RECOMP_FRAME_CLOCK`)

`RECOMP_FRAME_CLOCK=1` replaces the host clock with a frame-locked guest clock:
the counter is constant inside a frame apart from a read tick, and each Swap
advances exactly one 1/60 s. SyncTest and run-ahead imply it unless it is set to
`0`.

The read tick is 1 us by default (`RECOMP_FRAME_CLOCK_TICK_NS`): guest loops
that poll the counter, such as a vblank wait, reach their deadline in a few
thousand reads instead of burning a frame's worth of host time. The value only
affects how fast such a loop converges; it is deterministic for a given read
sequence. A skipped wait (a vblank wait, or the scheduler idling until the next
timer) sets the counter straight to its deadline: reaching it by ticks cost
thousands of idle scheduler passes per frame, about 4 ms of each run-ahead fight
frame.

Under the frame clock every read inside a frame returns the frame's end, which
lies exactly on the vblank grid. The vblank wait used to treat a refresh at that
instant as passed and wait for the next one, so a frame that waited for vblank
covered two frames of guest time. A refresh at the current time now counts as
reached (`d3d_vblank_test` checks ten frames span exactly ten intervals).

When every guest thread waits, the scheduler moves the frame clock to the
earliest deadline. Reads return the frame's end, so a thread that sleeps one
frame per loop is due at the next frame's end; that jump used to stretch the
frame to two. The movie player's audio streamer is such a thread: under the
frame clock each movie Swap covered 33 ms, the streamer wrote 5,120 bytes per
6,400 played, never kept a lead, and replayed stale ring data about 6 ms in
every 33 (heard as heavy clipping in movies with run-ahead). A wait ending by
the next frame's end now moves time inside the current frame only; longer waits
still stretch it. The movie stream under run-ahead now matches a plain run
sample for sample, and the rerun frame in a fight fell from about 6 ms to 3 ms.

### SyncTest (`RECOMP_SYNCTEST`)

`RECOMP_SYNCTEST=1` saves state at `RECOMP_SYNCTEST_START`, replays
`RECOMP_SYNCTEST_FRAMES` frames (default 1) and compares; `loop` keeps checking.
It exits 0 on a match, 2 on a mismatch and 3 when the saved state cannot be
rewound; `loop` waits 60 frames after such a skip before checking again.

### Snapshots

Fibers are snapshotted as plain data. XAPI fibers keep their guest model
(registers, FPU, exception list) plus the host continuation of every parked
fiber: the captured CONTEXT and the live part of its native stack. Guest threads
park the same way, so their native continuations are saved beside their guest
records. Three kinds of fiber are deliberately not rewound: the fiber running at
the boundary (its stack is the caller's own live stack, and a snapshot only
restores while that same fiber runs), a converted thread's fiber (its stack
holds the runner's frames), and a fiber whose stack image would not fit the 64
KiB capture. Native fiber objects are restarted in place rather than freed, so
an image that still names one stays valid, and each host slot's guest stack
bookkeeping is rewound so a create after a rewind allocates what the original
frame allocated.

Guest RAM is write-watched (GetWriteWatch) and every image records the
generation at which it was copied, so a save, a rewind and a comparison touch
only the pages written since the image they work from. On the development
machine a 64 MB frame snapshot drops from 8.3 ms to about 0.7 ms and the
comparison from 9.4 ms to about 5 ms.

Host state the guest can observe is snapshotted with it: file positions and
directory cursors, the file handle table (handles opened after the image are
closed and the handle counter is rewound, or every later open gets a different
number), and the presenter's vblank marks. Missing vblank marks were what made
run-ahead wait on a refresh from the abandoned timeline: it fell from 15 fps to
6 fps until they were restored.

### Run-ahead (`RECOMP_RUNAHEAD`)

`RECOMP_RUNAHEAD=N` runs each frame hidden with present and audio suppressed,
displays the last of N, then rewinds and runs the same N frames again. A state
that cannot be rewound suspends it for 60 frames. On the development machine it
now costs 8.2 ms per simulated frame, the same as with the frame clock off, so
N=1 holds the display rate.

Audio comes only from the frames the game keeps: plain frames and the rerun
after each rewind, one frame behind the picture. Input is read live, so a
speculative frame can see a newer button than its rerun and start other sounds;
when it fed the host voices, sounds started only by the rerun never reset their
voice and stacked on stale audio, which clipped. Speculative frames also skip
the voice reset that DirectSound Play, Stop, seek, loop and data changes
perform; resetting there discarded queued audio and opened every new sound with
a 50 ms gap (867 underruns in the first 6,000 frames of a scripted fight,
against 1 without run-ahead).

Hidden repeats skip draws, clears, copies and visibility tests as well: the
visible run already left those results in the presenter, and at 4K with 32x MSAA
drawing every frame twice could not hold 60 fps.

Run-ahead also suspends when the Present that ended the visible frame came from
the other routine, as SyncTest re-syncs below; rewinding across a display mode
change stopped on a null D3D resource.

Heavy frames such as character select and match transitions cost about 11 ms per
simulated frame, too much to run twice. When 8 consecutive cycles (save,
display, rewind) average over 20 ms, run-ahead suspends for 60 frames, so those
stretches play without it at full rate. The scripted fight fell from 89
underruns in 14,000 frames to 11; the rest come where a long heavy stretch
resumes run-ahead and suspends it again. Each suspension in a row doubles the
pause, up to 1,920 frames, and a window that keeps up resets it: resuming into
the same heavy fight every 69 frames repeated 8 slow cycles each time, heard as
a hitch about once a second. Each suspension logs the window's average shown,
kept, paced, save and load times.

The frame clock never sleeps, so the runner holds each kept frame to host time
itself: it sleeps until the frame clock's time has passed on the host clock, and
starts over after a 100 ms stall instead of rushing to catch up. Without this,
kept frames ran at whatever rate the presenter allowed and a long guest wait
advanced guest time instantly, so audio, which follows guest time, ran ahead (93
`too-far-ahead` drops in one session's boot) or behind.

Snapshots also no longer wait for the GPU when a hidden repeat dropped its
visibility test: the pending result is the visible frame's query, which the
guest's own poll resolves. That wait serialised CPU and GPU every cycle. With
all three, the scripted fight at 4K, 8x MSAA and SMAA costs about 11 ms for the
visible frame and 6 ms for the rerun, and run-ahead suspends only while the
fight loads.

### Divergences found and fixed

A frame boundary is a Present, and the game presents from two routines: the
frame loop (0x282947) and the display mode change (0x28284f). A rewind restores
guest state but leaves the host running the caller's own native frames, so it
cannot resume a frame that presented from the other routine; the replay then
plays the following frame instead. SyncTest records the Present return address
in each image and re-syncs when it moves, instead of reporting the mismatch.
That was frame 1035.

The scheduler slice offered on a tick read (recomp_kernel_poll) is gated on
guest time rather than host time, so a replayed frame preempts at the same
points; with the frame clock on that is about one slice per frame. Host time
made the interleaving depend on how fast the host ran, which also moved the
frame at which the next divergence appears.

The D3D8 image now includes the host render-state model that feeds draw
commands. Leaving it live let later state leak into an earlier replayed draw.

Frame 1422 was a stale occlusion result. The guest reads the previous visibility
test's count before ending the next test. That count lived only in the
presenter's host query map, so a rewind left the abandoned timeline's 144 in
place instead of the saved 140. The completed count is now part of `D3d8State`,
snapshot save waits for outstanding host queries, and the adapter returns the
saved count until the guest ends a new test. The loop check now runs from frame
1419 to 38,036 without a further mismatch. The two display mode changes in that
span, at frames 3036 and 6155, re-sync as designed; loops started at 3038 and
6157 verify the frames each re-sync would otherwise skip. The streaming read
that used to diverge at frames 636 and 807 is fixed; that one was the file
position and directory cursor surviving a rewind.

### Flows checked

Menu, save and story flows are checked the same way, each from its own input
schedule:

- a walk through the mode select, the settings list and the audio submenu
- the ranking and battle record screens
- a settings change that commits a 74 KB profile write at a fixed frame
- the in-fight pause menu
- story mode from character select through its cutscene into the fight

Both ways a story battle ends are covered:

- a loss reaches GAME OVER and returns to the title
- a win plays the round back under a REPLAY banner, runs the next chapter's
  cutscene and starts the following fight

The battle record path, RECORD and then SINGLE BATTLE, runs through character
and stage select, a recorded round and its playback; SAVE REPLAY in the
playback's pause menu writes the replay (`br.dat`, 578 KB) and a second save
overwrites it.

The round's end calls XAPI WideCharToMultiByte, which needs kernel ordinal 308,
RtlUnicodeStringToAnsiString; the runner stopped there before `kernel_rtl.c`
bridged it. They run clean under the loop.

A frame that saves is now skipped, as the Saves section describes, and a loop
started on the frame after a save checks what follows it. Before that rule,
profile writes replayed with identical bytes because `ups.dat` already existed,
but the first replay save diverged: its replay found the new SaveImage.xbx and
took the overwrite branch.

The profile header carries a 28-byte value that is fixed for the life of the
process and differs between sessions; nothing the guest reads back depends on
it.

## Remaining work

The [roadmap](https://github.com/NoRain211/doa2u-re/issues/3) tracks the order
of work to feature complete, and the
[acceptance matrix](https://github.com/NoRain211/doa2u-re/issues/9) lists the
flows each platform must pass.

Online libraries and dashboard/update binaries are present on the disc. The
initial scope is offline DOA2; any online behavior needs its own design. The
verified XNetStartup boundary returns the native offline policy error
WSASYSNOTREADY without creating a console network stack. The game handles its
helper's failure; this seam does not report successful network startup. PHY
initialization reports hardware unavailable, and link queries return
disconnected flags, including after the game's failed network startup.

Record each observed frontier privately. A public model-test pass or a completed
lift alone is not a gameplay result.
