Dead or Alive 2 Ultimate native Linux port - 0.5.0
==================================================

This is an early build. It contains no game files. You
build the game yourself from your own copy of Dead or Alive Ultimate
(Xbox, USA), Disc 2. Other regions, versions and Disc 1 are refused.

What you need (x86-64 Linux; on the Steam Deck, see below). On apt, dnf or
pacman systems, ./install_deps.sh installs everything below except the
Vulkan driver for an NVIDIA GPU:

- A C++17 compiler, CMake 3.20 or newer, Ninja, Git, Python 3.12 or newer
  with its venv module, the shaderc library and a Vulkan driver. On
  Ubuntu 24.04 or Debian 13:

      sudo apt install build-essential clang cmake ninja-build git python3-venv \
          libshaderc1 libvulkan1 mesa-vulkan-drivers

  Clang is optional; it compiles the game in half the memory GCC takes,
  so more compile jobs fit.

  The build compiles SDL3 when your distribution has none, which also
  needs its development packages:

      sudo apt install libx11-dev libxext-dev libxrandr-dev libxcursor-dev \
          libxi-dev libxss-dev libxkbcommon-dev libwayland-dev \
          libegl-dev libasound2-dev libpulse-dev libpipewire-0.3-dev \
          libudev-dev libdbus-1-dev libxtst-dev

- An internet connection for the first build, your Disc 2 as an Xbox ISO,
  and about 6 GB of free space.

The build downloads a pinned, checksummed extract-xiso to read the ISO. It
needs glibc 2.34 or newer; elsewhere, build extract-xiso from
github.com/XboxDev/extract-xiso and add --extractor /path/to/extract-xiso
to step 2.

Build (once, about 15 to 25 minutes):

1. Extract this ZIP to a new folder and open a terminal in that folder.
2. Build from your ISO. The ISO is only read; quote the path if it has
   spaces. The script makes its own Python environment in private/venv and
   names any missing tool before it starts:

      ./build_game.sh "/path/to/disc2.iso"

3. Wait for "Build complete". The build is checked against the exact
   program the developers tested; any difference stops the build.

Or build from the launcher: ./launcher.sh opens on its GAME page.
This ZIP ships a prebuilt launcher, so it needs nothing
installed. Choose your Disc 2 ISO with DISC 2 ISO, or drop it on the window,
then choose BUILD. It shows the elapsed time and the latest line, keeps the
full output in private/build.log, and asks twice before it stops a build.

Steam Deck: SteamOS has no compiler, so the build runs in an Ubuntu 24.04
distrobox that shares your home folder, then you play on SteamOS itself.
Extract this ZIP under your home folder and, in Desktop Mode, either run
./launcher.sh and use BUILD on its GAME page, or run in Konsole:

      ./build_steamdeck.sh "/path/to/disc2.iso"

To play in Game Mode, add launcher.sh with "Add a Non-Steam Game" in Desktop
Mode. If one controller drives two players, turn Steam Input off for it.

Play: ./launcher.sh opens a menu: pick resolution, MSAA, SMAA, sharpening,
ambient occlusion, widescreen and the intro skip on VIDEO, stereo or 5.1 on
AUDIO and the unlocks and free camera on GAMEPLAY, then choose START GAME on
the GAME page.
A controller, the mouse or touch work; on the keyboard, arrows move and
change values, Q and E switch pages, Enter selects and Esc goes back. It saves the choices
and starts the game.
./run_game.sh plays again with the saved choices. The
first launch copies about 700 MB of the game's disc archives into its
hard-disk cache, which takes a few seconds. Saves and the cache live in
private/play-disc. Start + Back + both triggers resets the game.

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
machine, or one controller against the CPU. On Windows, allow the game through the firewall when asked.

What the server sees: nothing is sent until you open DOA ONLINE. Logging on
sends the server your IP address and port, the account's ID, the build's
fingerprint and your profile name, which other players see as your
gamertag. Players who join you, or ask the server to connect to you, get your
IP address and port, as on the Xbox. To keep Live off, start the game with
`RECOMP_LIVE=0 ./launcher.sh` (or `./run_game.sh`).

Known limits: this is bring-up software. Story, Watch and the attract demo
play in development runs on every stage, but many modes and characters
have not been tried. A draw the port cannot render yet stops the game, and the log
says why. Online play is a test: two players across the internet, and players on
different systems, have not played each other yet, and voice chat is not
supported. If the two games stop matching, the fight ends and the game says a player left.
MSAA stops at 8x on Linux; a higher choice falls back to 8x.

This Linux build comes from @tmcleroy's port (pull request #1), which was
played on a Steam Deck.

Reporting problems: open an issue at
https://github.com/NoRain211/doa2u-re/issues and say what you did and what
happened. Copy the "recomp stop" or crash lines from the end of the newest
log in private/play-logs, or the error from the newest private/build-logs
folder if the build failed. Logs show your home folder and user name as ~;
remove any other private paths, and do not upload whole logs or any game files.

