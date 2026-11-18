/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_NETPLAY_SESSION_H
#define DOA2U_RECOMP_NETPLAY_SESSION_H

#include "input_model.h"
#include "input_pulse_source.h"

#include <cstdint>
#include <filesystem>
#include <string>

/* Rollback sessions over the running game (docs/rollback-netcode.md): Live's
   fights, or the RECOMP_ROLLBACK loopback test. The runner boots the game;
   this module drives every frame of a session. */

// What a session borrows from the runner.
struct RecompNetplaySessionHost {
    std::filesystem::path disc;          // the play folder
    std::string digest;                  // RECOMP_NETPLAY_BUILD, so both sides simulate alike
    RecompInputPulseSource *pulses;      // this player's pad or input script
    bool scriptedInput;                  // --input-script-only
    std::uint8_t *guestRam;              // for RECOMP_ROLLBACK_DUMP
};
void recomp_netplay_session_init(const RecompNetplaySessionHost &host);

/* Live (docs/xbox-live.md): the game's own Xbox Live pages start each fight,
   and live_fight.c asks for a rollback session at every match's round start
   instead of connecting from the command line. The runner calls start_live at
   startup, unless RECOMP_LIVE=0. live_fight.c is C, so both have C linkage
   (live_fight.h declares the second one for it). */
extern "C" {
void recomp_netplay_session_set_live(bool on);
/* Starts the Live client and sets Live mode, unless RECOMP_LIVE=0 or a
   Loopback test session runs; call once, after init. */
void recomp_netplay_session_start_live();
bool recomp_netplay_session_live();
}

/* Per boot. read_env takes RECOMP_ROLLBACK when no session is set (false
   when it is malformed); enabled says a session runs this boot. */
bool recomp_netplay_session_read_env();
bool recomp_netplay_session_enabled();
/* Installs this boot's frame and save hooks for a session. The runner clears
   them first. */
void recomp_netplay_session_install(bool synctest);

/* Input: true when the session decides the guest port's pad, with present set. */
bool recomp_netplay_session_pad(std::uint32_t port, RecompInputGamepad *pad, bool *present);
void recomp_netplay_session_set_vibration(std::uint32_t port, std::uint16_t left, std::uint16_t right);
// Each presented frame: the Live transport's pump.
void recomp_netplay_session_on_swap();
// At exit: tells the other player.
void recomp_netplay_session_exit();

#endif
