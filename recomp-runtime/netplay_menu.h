#ifndef DOA2U_RECOMP_NETPLAY_MENU_H
#define DOA2U_RECOMP_NETPLAY_MENU_H

#include "input_model.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

/* The Versus menu's online entries and the lobby room, drawn by the runner
   over the game's main menu. When both players are ready, each runner leaves
   a launch note and exits for run_game.py to start it again (exit code 4);
   the new runner reads the note and joins the session from its first frame. */

/* Turns the menu on for an offline run. storage is the play folder's
   .recomp-storage, digest the runner build's hex digest, and ownName reads
   the player's profile name when a room opens. */
void recomp_netplay_menu_install(const std::filesystem::path &storage, const std::string &digest,
    std::function<std::string()> ownName);
/* Sees each sampled pad first; while the menu is open the game gets an idle pad. */
void recomp_netplay_menu_filter(std::uint32_t port, RecompInputGamepad *pad);
/* Takes the launch note a room left, once: hostPort and delay for the host,
   or join as host:port for the joiner. */
bool recomp_netplay_menu_take_launch(const std::filesystem::path &storage, std::uint32_t &hostPort,
    std::uint32_t &delay, std::string &join);
/* The session's network display from NETPLAY SETTINGS: 0 off, 1 ping, 2 full. */
int recomp_netplay_menu_display(const std::filesystem::path &storage);

#endif
