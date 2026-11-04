# Project Agent Rules

## Goal and current seam

Build a native Windows port of Dead or Alive 2 Ultimate (Xbox), then replace
generated game-owned logic with readable source. Read `CONTEXT.md` for scope
and `docs/bring-up.md` before starting a game lift or runner.

`recomp-runtime/` contains the shared core, kernel host, runner and synthetic
tests. `recomp_lookup_manual()` binds XAPI kernel lifetime and the initial
D3D8 5849 device, frame and state APIs to the shared D3D11 presenter.
DirectSound 5849 owns native device/buffer lifetime and PCM output.
Effects-image download owns decoded maps/state and binds native I3DL2 reverb.
XAPI fibers run on native Windows fibers through `fiber_adapter.c`.
3D voices render through the game's HRTF tables with crosstalk bins folded to
stereo. Recording sends and additional DSP processing remain guarded.
Read the audio limits in `docs/bring-up.md` before extending this adapter.
Read the graphics limits in `docs/bring-up.md` before extending this adapter.
Before enabling another adapter,
verify the selected DOA2U executable identity, function address, calling
convention and object layout. DOA3 and DOAXBV bindings are references only.

Keep D3D8 device models callable as plain functions. Replace statically linked
D3D8 at the API boundary and send semantic commands to a host presenter.
Follow the sister projects' D3D11 presentation seam; do not build an NV2A
emulator. Replace library code wholesale; decompile only game-owned logic.
Document temporary seams beside their manual lookup.

## Ghidra

Pass `program_name="/DOA2.xbe"` on every program-scoped GhidrAssistMCP
call for the main game. Before analysis, call `list_binaries` and verify
the target's Project Path and executable identity. If renamed or
reimported, resolve its new path; never rely on the active tab.
`program_name="/DOA2U-default.xbe"` is for launcher analysis only.
Name functions and globals in Ghidra as you identify them, in snake_case
for what they do (e.g. `hdd_install_thread`, `g_hdd_install_gate`); leave
nothing you have confirmed as `FUN_`/`DAT_`. GhidrAssistMCP cannot save, so
ask the user to save the program (Ctrl+S) after naming.

## Custody and disk use

1. Never hand-edit generated C. Fix the pinned lifter and regenerate locally.
2. Never commit an XBE, generated game C, retail bytes, extracted assets or
   filenames, saves, BIOS data, private paths, or local run evidence.
3. Keep all private inputs and output under `private/`; only its README is
   tracked. New public files must be included in `public-export.json`.
4. Report observed behavior; forced state remains a hypothesis.
5. Keep the ISO in its original location and extract it only once. Use
   hardlinks for play folders; do not copy disc trees or storage caches.
6. Preserve imported discs, ISOs, play folders with saves, releases, and the
   executable folder open in Ghidra. Remove task-owned scratch dumps when done.

## Lifter and shared source

`tools/xboxrecomp` is pinned to `653782777dee86b70030622a64efb53d6d06cc0b`
on `NoRain211/xboxrecomp`'s `codex/doa2u-recipe` branch. This is an initial
tooling baseline, not a validated DOA2U recipe. Use its upstream interfaces
(`--manual-functions`, `--coalesce-functions`). Update its gitlink and the
export manifest together. Do not vendor lifter source or generated output.

Read `docs/reuse.md` before copying more sister-project code; take committed
public source and preserve its notices. Neither sister runner's guest memory
layout can be assumed safe for DOA2U.

Host decisions remain in DOAXBV issues
[#2](https://github.com/NoRain211/doaxbv-re/issues/2) (custody),
[#6](https://github.com/NoRain211/doaxbv-re/issues/6) (semantic presenter), and
[#8](https://github.com/NoRain211/doaxbv-re/issues/8) (audio and movie seams).
Read the relevant decision before changing one of those boundaries.

## Checks and Git

Run the public checks in `README.md` before committing or pushing. Keep the
diff scoped, preserve existing source style, and use LF for new text files.
Use `type(scope): message` commit subjects under 72 characters.
Publishing and merging require an explicit user request. The public remote
and issue tracker are `NoRain211/doa2u-re`; package releases with
`tools/package_test.py` as described in `README.md`.
