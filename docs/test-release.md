Dead or Alive 2 Ultimate native PC port - 0.4.0 Alpha
=====================================================

This is an early build. It contains no game files. You
build the game yourself from your own copy of Dead or Alive Ultimate
(Xbox, USA), Disc 2. Other regions, versions and Disc 1 are refused.

What you need (Windows 10 or 11, 64-bit):

- An internet connection for the first build (it downloads SDL3 and SMAA).
  Nothing else needs installing: this ZIP carries Python 3.12.10 with
  Capstone, Pillow and truststore in tools\python, CMake 4.4.5 in tools\cmake
  and the llvm-mingw compiler in tools\llvm-mingw, each with its upstream
  license files.
- Your Disc 2 as an Xbox ISO, and about 6 GB of free space on the drive
  holding this folder.

Build (once, about 10 minutes):

1. Extract this ZIP to a folder with a short path, for example C:\DOA2U.
   On FAT32 or exFAT drives, common on USB sticks, the play folder holds a
   copy of the disc instead of hardlinks, so allow about 4 GB more space.
2. Open Launcher.exe and drag your Disc 2 ISO onto it, or choose it on the
   GAME page; the build starts at once. The ISO is only read.
3. Wait for "Build complete". The build is checked against the exact
   program the developers tested; any difference stops the build.
   The launcher shows the elapsed time and the build's latest message; keep
   it open until it finishes.

Play: open Launcher.exe, pick resolution, MSAA, SMAA, widescreen, the
renderer, VRR and the intro skip on VIDEO, stereo or 5.1 on AUDIO and the
unlocks and free camera on GAMEPLAY, then choose START GAME on the GAME
page. Direct3D 11 is the default renderer; Vulkan is an
alternative to try if it stutters. Turn VRR on only for a G-SYNC or FreeSync
display. A controller, the keyboard
(arrows, Q/E, Enter, Esc) or the
mouse all work. It remembers your choices, and DOA2U.exe plays with the same
choices. Widescreen
uses the game's own 16:9 camera and menus. The first
launch copies about 700 MB of the game's disc archives into its hard-disk
cache, which takes a few seconds. Saves and the cache live in
private\play-disc. Start + Back + both triggers resets the game.

With Unlocks on Everything (the default), every character and costume is
unlocked while you play, but your profile saves only what you earn. Earn
offers only what your profile has earned, so switching back gives you your
own progress. Profiles saved by earlier releases keep the unlocks those
releases saved.

Controls: the first connected controller (Xbox, PlayStation and other
controllers SDL3 supports). The keyboard drives player 1:

    Arrow keys = D-pad      Enter = START     Backspace = BACK
    A = X    S = Y    Z = A    X = B
    Q = White    W = Black    E = Left trigger    R = Right trigger

Online play (test, work in progress): DOA ONLINE on the main menu opens the
game's own Xbox Live pages, which run on the project's server. The first
start writes `live-identity.bin` to the play folder's `.recomp-storage`, next
to the saves, and the first logon makes an account from it, named after your
game profile; losing that file loses the account. Quick Match, OptiMatch and
the lobby have been played, and fights use rollback netcode; friends and
rankings have only been tested without the game. Both players need this same
release built from the same disc; players of other builds do not see each
other's sessions. Versus is the game's own again: two controllers on one
machine. On Windows, allow the game through the firewall when asked.

What the server sees: nothing is sent until you open DOA ONLINE. Logging on
sends the server your IP address and port, the account's ID, the build's
fingerprint and your profile name, which other players see as your
gamertag. Players who join you, or ask the server to connect to you, get your
IP address and port, as on the Xbox. To keep Live off, open a Command Prompt
in the release folder and run `set RECOMP_LIVE=0` and then `Launcher.exe`.

Known limits: this is bring-up software. Story, Watch and the attract demo
play in development runs on every stage, but many modes and characters
have not been tried. A draw the port cannot render yet stops the game, and the log
says why. Online play is a test: two players across the internet, and players on
different systems, have not played each other yet, and voice chat is not
supported. If the two games stop matching, the fight ends and the game says a player left.
A demanding moment on The Burai Zenin can still drop a single frame, and
the safari stage's heat haze does not ripple yet.

Reporting problems: open an issue at
https://github.com/NoRain211/doa2u-re/issues and say what you did and what
happened. Copy the "recomp stop" or crash lines from the end of the newest
log in private\play-logs, the error Launcher.exe shows if there is no log, or
the error from the newest private\build-logs folder if the build failed.
Logs show your home folder and user name as ~; remove any other private paths,
and do not upload whole logs or any game files.
