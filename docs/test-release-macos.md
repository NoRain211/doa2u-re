Dead or Alive 2 Ultimate native macOS port - 0.2.1 Alpha (experimental)
=======================================================================

This is an early, experimental build. It contains no game files. You
build the game yourself from your own copy of Dead or Alive Ultimate
(Xbox, USA), Disc 2. Other regions, versions and Disc 1 are refused.

What you need (macOS on Apple Silicon or Intel):

- Homebrew (https://brew.sh). In Terminal, install the build tools:

      brew install cmake ninja sdl3 shaderc spirv-cross python@3.13

- The Xcode command line tools, for the settings window and extract-xiso:

      xcode-select --install

- extract-xiso, which Homebrew does not have. Build it once:

      git clone https://github.com/XboxDev/extract-xiso.git
      cmake -S extract-xiso -B extract-xiso/build
      cmake --build extract-xiso/build

- An internet connection for the first build, your Disc 2 as an Xbox ISO,
  and about 6 GB of free space.

Build (once; lifting and compiling take several minutes):

1. Extract this ZIP to a new folder and open Terminal in that folder.
2. Give the build its own Python packages:

      python3.13 -m venv private/venv
      private/venv/bin/pip install capstone==5.0.7

3. Build from your ISO. The ISO is only read:

      ./build_game.sh /path/to/disc2.iso --extractor /path/to/extract-xiso/build/extract-xiso

4. Wait for "Build complete". The build is checked against the exact
   program the developers tested; any difference stops the build.

Play: ./launcher.sh opens a menu: pick resolution, MSAA, SMAA and widescreen
on VIDEO SETTINGS, stereo or 5.1 on AUDIO SETTINGS and the intro skip on
MOVIES, then choose START GAME. Arrow keys move, Return or Space selects and
Esc goes back; the mouse works too. It saves the choices and starts the game.
./run_game.sh plays again with the saved choices. The
first launch copies about 700 MB of the game's disc archives into its
hard-disk cache, which takes a few seconds. Saves and the cache live in
private/play-disc. Start + Back + both triggers resets the game.

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
Type an address from the keyboard, or with the controller (up/down changes
a character, left/right moves). Once the other player is in the room, Enter
or Tab opens a chat line.
On a Steam Deck in Game Mode, the on-screen keyboard shows only while an
address or a chat line is being typed. The clipboard works only on Windows
for now.

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
match saves a replay in `private/play-disc/.recomp-storage/replays`, unless
the log says it cannot write one. Keep it
when you report a desync or a cheating player; it holds both players'
profiles, so share it only privately, when a developer asks for it.
MSAA stops at 4x on Apple GPUs; a higher choice falls back to 4x.

This macOS build comes from @tmcleroy's port (pull request #1), which was
built and played on an Apple M4 Mac. The developers have not built this ZIP
on a Mac.

Reporting problems: open an issue at
https://github.com/NoRain211/doa2u-re/issues and say what you did and what
happened. Copy the "recomp stop" or crash lines from the end of the newest
log in private/play-logs, or the error from the newest private/build-logs
folder if the build failed. Logs show your home folder and user name as ~;
remove any other private paths, and do not upload whole logs or any game files.

