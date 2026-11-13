/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_BUTTON_CONFIG_H
#define DOA2U_RECOMP_BUTTON_CONFIG_H

#include "pc_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Button Config: the Controller page's Rebind row, and the game's Button
   Config screen (scene 0x19) while it rebinds. pc_settings.c owns PC SETTINGS
   and calls these from the Options menu hooks both pages share. */
void recomp_button_config_install(const RecompPcSettingsHost *host, unsigned text_base);
RecompFunction recomp_button_config_lookup_manual(uint32_t guest_address);

/* True when it handled the call, so the caller leaves it to the game. */
bool recomp_button_config_input(void);          /* Options update: the Rebind row */
bool recomp_button_config_rows(uint16_t page, uint32_t *rows);
bool recomp_button_config_text(void);           /* 0x910a0: the Rebind row's label */
/* The help line's text id, or 0 for the game's; may rewrite args. */
unsigned recomp_button_config_help(uint16_t page, uint16_t row, uint32_t args[3]);
/* The page's layout, or 0 for the game's. */
uint32_t recomp_button_config_layout(uint16_t page);
/* The rebinding layout where the game's page 4 one is passed, or 0. */
uint32_t recomp_button_config_cursor(uint32_t template_address);

#ifdef __cplusplus
}
#endif

#endif
