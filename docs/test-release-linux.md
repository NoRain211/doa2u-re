Dead or Alive 2 Ultimate native Linux port - 0.1.15 Alpha (experimental)
========================================================================

This is an early, experimental build. It contains no game files. You
build the game yourself from your own copy of Dead or Alive Ultimate
(Xbox, USA), Disc 2. Other regions, versions and Disc 1 are refused.

What you need (x86-64 Linux; on the Steam Deck, see below):

- A C++17 compiler, CMake 3.20 or newer, Ninja, Git, Python 3.12 or newer
  with its venv module, the shaderc library and a Vulkan driver. On
  Ubuntu 24.04 or Debian 13:

      sudo apt install build-essential cmake ninja-build git python3-venv \
          libshaderc1 libvulkan1 mesa-vulkan-drivers

  The build compiles SDL3 when your distribution has none, which also
  needs its development packages:

      sudo apt install libx11-dev libxext-dev libxrandr-dev libxcursor-dev \
          libxi-dev libxss-dev libxkbcommon-dev libwayland-dev \
          libegl-dev libasound2-dev libpulse-dev libpipewire-0.3-dev \
          libudev-dev libdbus-1-dev

- An internet connection for the first build, your Disc 2 as an Xbox ISO,
  and about 6 GB of free space.

The build downloads a pinned, checksummed extract-xiso to read the ISO. It
needs glibc 2.34 or newer; elsewhere, build extract-xiso from
github.com/XboxDev/extract-xiso and add --extractor /path/to/extract-xiso
to step 3.

Build (once, about 15 to 25 minutes):

1. Extract this ZIP to a new folder and open a terminal in that folder.
2. Give the build its own Python packages:

      python3 -m venv private/venv
      private/venv/bin/pip install capstone==5.0.7

3. Build from your ISO. The ISO is only read:

      ./build_game.sh /path/to/disc2.iso

4. Wait for "Build complete". The build is checked against the exact
   program the developers tested; any difference stops the build.

Steam Deck: SteamOS has no compiler, so build inside an Ubuntu 24.04
distrobox that shares your home folder, then play on SteamOS itself. To
play in Game Mode, add launcher.sh with "Add a Non-Steam Game" in Desktop
Mode. If one controller drives two players, turn Steam Input off for it.

Play: ./launcher.sh opens a menu: pick resolution, MSAA, SMAA and widescreen
on VIDEO SETTINGS, stereo or 5.1 on AUDIO SETTINGS and the intro skip on
MOVIES, then choose START GAME. A controller, the mouse or touch work; on the
keyboard, arrows move, Enter selects and Esc goes back. It saves the choices
and starts the game.
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
players are ready the match starts at Versus character select, and leaving
the match takes both players back to the room. Both players need this same
release built from the same disc, or the room shows OTHER BUILD. The other
player sees your game profile name.

The host's game asks the router to open UDP port 47701 (UPnP). If the room
says to forward it by hand, forward UDP 47701 to the host's computer, or use
FIND MATCH, which gets through most routers and falls back to a relay.
NETPLAY SETTINGS sets the input delay, the port, the network display in a
match, blocked players and the matchmaker. In a room Y mutes the other
player and X twice blocks them. The menu hides IP addresses, so they stay off
streams; while your room is empty, Y shows your address.
Typing, chat and the clipboard work only on Windows for now: enter an
address with the controller (up/down changes a character, left/right moves).

Known limits: this is bring-up software. Story, Watch and the attract demo
play in development runs on every stage, but many modes and characters
have not been tried. A draw the port cannot render yet stops the game, and the log
says why. The Xbox Live menus stop the game, as there is no online service.
Online play is a test: Windows, Linux and macOS players have not played
each other yet. If FIND MATCH cannot reach the matchmaker, JOIN ONLINE by
address still works.
MSAA stops at 8x on Linux; a higher choice falls back to 8x.

This Linux build comes from @tmcleroy's port (pull request #1), which was
played on a Steam Deck.

Reporting problems: open an issue at
https://github.com/NoRain211/doa2u-re/issues and say what you did and what
happened. Copy the "recomp stop" or crash lines from the end of the newest
log in private/play-logs, or the error from the newest private/build-logs
folder if the build failed. Remove your user name and other private paths,
and do not upload whole logs or any game files.

