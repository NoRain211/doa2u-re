Dead or Alive 2 Ultimate native PC port - 0.2.0 Alpha (experimental)
====================================================================

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

Online play (test, work in progress): choose Versus on the main menu and
press A. LOCAL plays two controllers on one machine as before. HOST ONLINE
opens a room: a PUBLIC room shows on FIND MATCH, a PRIVATE room only takes
players who have your address. JOIN ONLINE takes the host's address, such
as 203.0.113.7:47701. FIND MATCH lists open rooms with their ping; A joins
one and Y quick-matches the nearest. In the room A readies up; when both
players are ready the match starts at Versus character select. Fights
follow one another there; backing out of Versus to the main menu takes
both players back to the room. Both players need this same
release built from the same disc, or the room shows OTHER BUILD. The other
player sees your game profile name.

The host's game asks the router to open its UDP port, 47701 unless changed
in NETPLAY SETTINGS (UPnP). If the room says to forward it by hand, forward
the UDP port the room shows to the host's computer, or use FIND MATCH, which
tries a direct connection and falls back to a relay.
NETPLAY SETTINGS sets the input delay, the port, the network display in a
match, blocked players and the matchmaker. In a room Y mutes the other
player and X twice blocks them. The menu hides players' IP addresses, so they
stay off streams; while your room is empty, Y shows your address.
On Windows, allow the game through the firewall when asked. Once the other
player is in the room, Enter or Tab opens a chat line. X copies the host's
address, and Ctrl+V pastes one.

Known limits: this is bring-up software. Story, Watch and the attract demo
play in development runs on every stage, but many modes and characters
have not been tried. A draw the port cannot render yet stops the game, and the log
says why. The Xbox Live menus stop the game, as there is no online service.
Online play is a test: Windows, Linux and macOS players have not played
each other yet. If FIND MATCH cannot reach the matchmaker, JOIN ONLINE by
address still works.
Online matches are unranked: nothing goes to a leaderboard, and each match
plays on copies of both profiles, so your own profile never changes. If the
two games stop matching, the match ends with DESYNC DETECTED. Every online
match saves a replay in `private\play-disc\.recomp-storage\replays`, unless
the log says it cannot write one. Keep it
when you report a desync or a cheating player; it holds both players'
profiles, so share it only privately, when a developer asks for it.
A demanding moment on The Burai Zenin can still drop a single frame, and
the safari stage's heat haze does not ripple yet.

Reporting problems: open an issue at
https://github.com/NoRain211/doa2u-re/issues and say what you did and what
happened. Copy the "recomp stop" or crash lines from the end of the newest
log in private\play-logs, the error Launcher.exe shows if there is no log, or
the error from the newest private\build-logs folder if the build failed.
Logs show your home folder and user name as ~; remove any other private paths,
and do not upload whole logs or any game files.
