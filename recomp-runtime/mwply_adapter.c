/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mwply_adapter.h"

#include "audio_output.h"
#include "kernel_abi.h"
#include "sfd_format.h"
#include "sfd_movie.h"
#include "stop_report.h"
#include "xapi_time_adapter.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* The CRI MWSFD player calls DOA2U makes, from movie_open (0xf9e10) and its
   per-frame helpers (0xf98f0, 0xf99b0, 0xf9810), all cdecl. The guest player
   ran its MPEG decoder on CRI's file server thread, where one burst held every
   other guest thread for up to 250 ms (issue #69); decoding now runs on a host
   thread. The handle is the start of the work buffer the game passes in; its
   first word points at the player's function table (0x7c6430), so the game's
   table calls reach these bindings. mwPlyInitSfdFx and the work-size query
   still run as guest code: they also set up the ADX library music uses. */
enum { FUNCTION_TABLE = 0x7c6430u, FRAME_WORDS = 19u };

typedef struct Player {
    uint32_t handle, pitch, rows, channel;
    RecompSfdMovie *movie;
    bool silent, failed;
    int32_t volume; /* hundredths of dB */
    const uint8_t *pixels;
    uint32_t width, height;
} Player;
static Player player;

static uint32_t argument(uint32_t entry, uint32_t index)
{
    return *recomp_memory_u32(entry + 4u + index * 4u);
}

static void finish(uint32_t value)
{
    recomp_runtime.registers.eax = value;
    recomp_runtime.registers.esp += 4u;
}

static uint32_t handle_argument(void)
{
    const uint32_t handle = argument(recomp_runtime.registers.esp, 0u);
    if (handle == 0u || handle != player.handle) recomp_stop(2, "mwply:handle:%08" PRIx32, handle);
    return handle;
}

static double now(void)
{
    return (double)recomp_xapi_performance_counter() / (double)recomp_xapi_performance_frequency();
}

static void close_movie(void)
{
    recomp_sfd_movie_close(player.movie);
    player.movie = NULL;
    player.pixels = NULL;
    player.failed = false;
}

/* mwPlyCreateSofdec (0x30f370, creation parameters): the work buffer is +0x18, its size +0x1c. */
static void create(void)
{
    const uint32_t parameters = argument(recomp_runtime.registers.esp, 0u);
    const uint32_t work = *recomp_memory_u32(parameters + 0x18u), size = *recomp_memory_u32(parameters + 0x1cu);
    if (player.handle != 0u) recomp_stop(2, "mwply:second-player");
    if (work == 0u || size < 4u) recomp_stop(2, "mwply:work-buffer:%08" PRIx32, work);
    *recomp_memory_u32(work) = FUNCTION_TABLE;
    player = (Player){.handle = work};
    finish(work);
}

/* Table +0x14 (0x30f290): destroy. */
static void destroy(void)
{
    handle_argument();
    close_movie();
    player = (Player){0};
    finish(0u);
}

/* Table +0x18 (0x31bbb0, handle, file name): mwPlyStartFname. A file the
   disc lacks reports status 4, as the guest player's open error did. */
static void start_file(void)
{
    handle_argument();
    const uint32_t name = argument(recomp_runtime.registers.esp, 1u);
    char guest_path[128] = "", host_path[1024];
    for (uint32_t i = 0u; name != 0u && i + 1u < sizeof guest_path; ++i)
        if ((guest_path[i] = (char)*recomp_memory(name + i, 1u)) == '\0') break;
    close_movie();
    if (recomp_kernel_disc_host_path(guest_path, host_path, sizeof host_path))
        player.movie = recomp_sfd_movie_open(host_path, RECOMP_AUDIO_OUTPUT_MOVIE_SLOT, !player.silent);
    player.failed = player.movie == NULL;
    if (player.failed) fprintf(stderr, "recomp mwply: cannot open %s\n", guest_path);
    else recomp_sfd_movie_set_volume(player.movie, player.volume);
    finish(0u);
}

/* Table +0x1c (0x31b7d0): mwPlyStop. */
static void stop(void)
{
    handle_argument();
    close_movie();
    finish(0u);
}

/* Table +0x20 (0x30dff0): 0 stopped, 1 preparing, 2 playing, 3 ended, 4 error. */
static void get_status(void)
{
    handle_argument();
    uint32_t status = player.failed ? 4u : 0u;
    if (player.movie != NULL) {
        static const uint32_t statuses[] = {1u, 2u, 3u, 4u};
        status = statuses[recomp_sfd_movie_poll(player.movie, now())];
    }
    finish(status);
}

/* Table +0x28 (0x31b820, handle, switch): the game only releases pause. */
static void set_pause(void)
{
    handle_argument();
    /* shortcut: no caller pauses a movie; bind a clock pause if one does. */
    if (argument(recomp_runtime.registers.esp, 1u) != 0u) recomp_stop(2, "mwply:pause-unbound");
    finish(0u);
}

/* Table +0x2c (0x30e0e0, handle, volume in tenths of dB). */
static void set_volume(void)
{
    handle_argument();
    player.volume = (int32_t)argument(recomp_runtime.registers.esp, 1u) * 10;
    if (player.movie != NULL) recomp_sfd_movie_set_volume(player.movie, player.volume);
    finish(0u);
}

/* mwPlyGetAdxtHn (0x30e210): no ADX handle; adx_set_mix_bins (0x2fc970) skips 0. */
static void get_adx_handle(void)
{
    handle_argument();
    finish(0u);
}

/* mwPlySetAudioSw (0x30e1a0, handle, switch): off before the surround path
   plays its own AIX soundtrack. */
static void set_audio(void)
{
    handle_argument();
    if (player.movie != NULL) recomp_stop(2, "mwply:audio-switch-while-playing");
    player.silent = argument(recomp_runtime.registers.esp, 1u) == 0u;
    finish(0u);
}

/* mwPlyGetCurFrm (0x30de60, handle, frame): word 0 is nonzero for a new
   frame; the game reads its width (+8) and height (+0xc). */
static void get_frame(void)
{
    handle_argument();
    const uint32_t frame = argument(recomp_runtime.registers.esp, 1u);
    uint32_t *words = (uint32_t *)(void *)recomp_memory(frame, FRAME_WORDS * 4u);
    memset(words, 0, FRAME_WORDS * 4u);
    if (player.movie != NULL) {
        const double time = now();
        (void)recomp_sfd_movie_poll(player.movie, time);
        if (recomp_sfd_movie_take_frame(player.movie, time, &player.pixels, &player.width, &player.height)) {
            words[0] = player.handle;
            words[2] = player.width;
            words[3] = player.height;
        }
    }
    finish(0u);
}

/* mwPlyRelCurFrm (0x30d830): the host keeps the frame until the next one. */
static void release_frame(void)
{
    handle_argument();
    finish(0u);
}

/* 0x30d380 (handle, pitch, rows): MWSFSFX_SetOutBufSize for the locked texture. */
static void set_output_size(void)
{
    handle_argument();
    player.pitch = argument(recomp_runtime.registers.esp, 1u);
    player.rows = argument(recomp_runtime.registers.esp, 2u);
    finish(0u);
}

/* mwPlyFxCnvFrmARGB8888 (0x30cd50, handle, frame, bits): A8R8G8B8 rows at the pitch set above. */
static void convert_frame(void)
{
    handle_argument();
    const uint32_t bits = argument(recomp_runtime.registers.esp, 2u);
    const uint32_t rows = player.height < player.rows ? player.height : player.rows;
    if (player.pixels != NULL && player.width * 4u <= player.pitch) {
        for (uint32_t y = 0u; y < rows; ++y)
            memcpy(recomp_memory(bits + y * player.pitch, player.width * 4u),
                player.pixels + (size_t)y * player.width * 4u, player.width * 4u);
    }
    finish(0u);
}

/* 0x30c680 (handle, channel): the subtitle channel, kept at +0x2ac by the guest player. */
static void set_subtitle_channel(void)
{
    handle_argument();
    player.channel = argument(recomp_runtime.registers.esp, 1u);
    finish(0u);
}

/* 0x30ccb0 (handle, text, size, info): copies the SUBTITLE cue shown now and
   returns 1, or clears info and returns 0. info[1] is the copied length. */
static void get_subtitle(void)
{
    handle_argument();
    const uint32_t entry = recomp_runtime.registers.esp;
    const uint32_t text = argument(entry, 1u), size = argument(entry, 2u), info = argument(entry, 3u);
    char cue[RECOMP_SFD_CUE_TEXT];
    const uint32_t length = player.movie == NULL ? 0u : recomp_sfd_movie_subtitle(
        player.movie, player.channel, now(), cue, size < sizeof cue ? size : sizeof cue);
    memset(recomp_memory(info, 20u), 0, 20u);
    if (length != 0u) {
        memcpy(recomp_memory(text, length), cue, length);
        recomp_memory_u32(info)[1] = length;
    }
    finish(length != 0u);
}

RecompFunction recomp_mwply_lookup_manual(uint32_t address)
{
    switch (address) {
    case 0x0030f370u: return create;
    case 0x0030f290u: return destroy;
    case 0x0031bbb0u: return start_file;
    case 0x0031b7d0u: return stop;
    case 0x0030dff0u: return get_status;
    case 0x0031b820u: return set_pause;
    case 0x0030e0e0u: return set_volume;
    case 0x0030e210u: return get_adx_handle;
    case 0x0030e1a0u: return set_audio;
    case 0x0030de60u: return get_frame;
    case 0x0030d830u: return release_frame;
    case 0x0030d380u: return set_output_size;
    case 0x0030cd50u: return convert_frame;
    case 0x0030c680u: return set_subtitle_channel;
    case 0x0030ccb0u: return get_subtitle;
    default: return NULL;
    }
}

