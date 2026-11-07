/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "overlay_text.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void publish(RecompOverlaySource source, const char *title)
{
    RecompOverlayText text;
    recomp_overlay_clear(&text);
    recomp_overlay_set_title(&text, title);
    recomp_overlay_publish(source, &text);
}

static void expect_shown(const char *what, const char *title)
{
    RecompOverlayText shown;
    recomp_overlay_take(&shown);
    if (strcmp(shown.title, title) != 0) {
        fprintf(stderr, "FAIL %s: \"%s\"\n", what, shown.title);
        ++failures;
    }
}

int main(void)
{
    expect_shown("nothing published draws nothing", "");
    publish(RECOMP_OVERLAY_FREE_CAMERA, "Free camera");
    expect_shown("the free camera panel shows", "Free camera");
    publish(RECOMP_OVERLAY_QUIT_PROMPT, "Quit?");
    expect_shown("the quit prompt covers it", "Quit?");
    publish(RECOMP_OVERLAY_FREE_CAMERA, "Free camera");
    expect_shown("a free camera republish keeps the prompt", "Quit?");
    publish(RECOMP_OVERLAY_QUIT_PROMPT, "");
    expect_shown("closing the prompt brings the panel back", "Free camera");
    publish(RECOMP_OVERLAY_FREE_CAMERA, "");
    expect_shown("both empty draws nothing", "");
    return failures == 0 ? 0 : 1;
}
