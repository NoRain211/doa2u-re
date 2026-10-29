Dead or Alive 2 Ultimate native PC port - 0.1.7 Alpha (experimental)
===================================================================

This is an early, experimental build. It contains no game files. You
build the game yourself from your own copy of Dead or Alive Ultimate
(Xbox, USA), Disc 2. Other regions, versions and Disc 1 are refused.

What you need (Windows 10 or 11, 64-bit):

- Python 3.12 or newer: https://www.python.org/downloads/windows/
  (tick "Add python.exe to PATH").
- CMake 3.20 or newer: https://cmake.org/download/ (add it to PATH).
- An internet connection for the first build (it downloads the llvm-mingw
  compiler, SDL3, SMAA and the Capstone and Pillow Python packages). No
  Visual Studio is needed.
- Your Disc 2 as an Xbox ISO, and about 6 GB of free space on the drive
  holding this folder.

Build (once, about 15 minutes):

1. Extract this ZIP to a folder with a short path, for example C:\DOA2U.
   On FAT32 or exFAT drives, common on USB sticks, the play folder holds a
   copy of the disc instead of hardlinks, so allow about 4 GB more space.
2. Open Launcher.exe and drag your Disc 2 ISO onto it, or choose it on the
   BUILD GAME page, then choose Build. The ISO is only read.
3. Wait for "Build complete". The build is checked against the exact
   program the developers tested; any difference stops the build.
   The launcher shows the elapsed time and the build's latest message; keep
   it open until it finishes.

Play: open Launcher.exe, pick resolution, MSAA, SMAA and widescreen on VIDEO
SETTINGS, stereo or 5.1 on AUDIO SETTINGS and the intro skip on MOVIES, then
choose START GAME. A controller, the keyboard (arrows, Enter, Esc) or the
mouse all work. It remembers your choices, and DOA2U.exe plays with the same
choices. Widescreen
uses the game's own 16:9 camera and menus. The first
launch copies about 700 MB of the game's disc archives into its hard-disk
cache, which takes a few seconds. Saves and the cache live in
private\play-disc. Start + Back + both triggers resets the game.

Every character and costume is unlocked, and the game saves the unlocks
to your profile.

Controls: the first connected controller (Xbox, PlayStation and other
controllers SDL3 supports). The keyboard drives player 1:

    Arrow keys = D-pad      Enter = START     Backspace = BACK
    A = X    S = Y    Z = A    X = B
    Q = White    W = Black    E = Left trigger    R = Right trigger

Known limits: this is bring-up software. Story, Watch and the attract demo
play in development runs on every stage, but many modes and characters
have not been tried. A draw the port cannot render yet stops the game, and the log
says why. The Xbox Live menus stop the game, as there is no online service.
A demanding moment on The Burai Zenin can still drop a single frame, and
the safari stage's heat haze does not ripple yet.

Reporting problems: open an issue at
https://github.com/NoRain211/doa2u-re/issues and say what you did and what
happened. Copy the "recomp stop" or crash lines from the end of the newest
log in private\play-logs, the error Launcher.exe shows if there is no log, or
the error from the newest private\build-logs folder if the build failed.
Remove your user name and other private paths,
and do not upload whole logs or any game files.
