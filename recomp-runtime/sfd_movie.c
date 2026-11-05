#include "sfd_movie.h"

#include "adx_decoder.h"
#include "audio_output.h"
#include "dsound_buffer_adapter.h"
#include "sfd_format.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"

#ifdef _WIN32
#include <windows.h>
typedef SRWLOCK Lock;
static void lock_init(Lock *l) { InitializeSRWLock(l); }
static void lock(Lock *l) { AcquireSRWLockExclusive(l); }
static void unlock(Lock *l) { ReleaseSRWLockExclusive(l); }
static void pause_ms(unsigned ms) { Sleep(ms); }
static uint64_t host_ms(void) { return GetTickCount64(); }
#else
#include <pthread.h>
#include <time.h>
typedef pthread_mutex_t Lock;
static void lock_init(Lock *l) { pthread_mutex_init(l, NULL); }
static void lock(Lock *l) { pthread_mutex_lock(l); }
static void unlock(Lock *l) { pthread_mutex_unlock(l); }
static void pause_ms(unsigned ms) { const struct timespec t = {0, (long)ms * 1000000L}; nanosleep(&t, NULL); }
static uint64_t host_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}
#endif

/* Eight decoded frames hold about a quarter second; audio keeps up to two seconds
   decoded and is submitted 100 ms ahead of the clock, the output's own cushion. */
enum { FRAMES = 8, CUES = 64, RECORDS = 8192, AUDIO_LEAD_MS = 100, PCM_SECONDS = 2, CHUNK_FRAMES = 4096 };

typedef struct Reader {
    FILE *file;
    uint8_t bytes[1u << 17];
    uint32_t start, end;
    bool failed;
} Reader;

typedef struct Frame {
    uint8_t *pixels;
    uint32_t index;
    bool full;
} Frame;

struct RecompSfdMovie {
    /* Worker-owned. */
    Reader video, audio;
    plm_buffer_t *stream;
    plm_video_t *decoder;
    RecompAdxDecoder adx;
    uint8_t adx_bytes[1u << 16];
    uint32_t adx_have;
    int16_t *pcm;
    uint32_t pcm_frames, pcm_read, pcm_count;
    uint64_t submitted;
    uint8_t records[RECORDS];
    uint32_t records_size;
    double fps;
    uint32_t slot;
#ifdef _WIN32
    HANDLE thread;
#else
    pthread_t thread;
#endif
    bool running;
    /* Guarded by guard. */
    Lock guard;
    Frame frames[FRAMES];
    uint32_t width, height, decoded;
    bool sound, header, audio_done, video_done, failed, ready, started, stopping;
    uint64_t start_ms;
    int32_t volume;
    RecompSfdCue cues[CUES];
    uint32_t cue_count;
    /* Caller-owned. */
    double start;
    uint8_t *current;
    uint32_t current_width, current_height;
    bool has_current;
    uint32_t taken;
};

/* Next packet of stream id from reader; false at the end or on a damaged file. */
static bool next_packet(RecompSfdMovie *movie, Reader *reader, uint32_t id, RecompSfdPacket *packet)
{
    while (!reader->failed) {
        const int32_t used = recomp_sfd_packet(reader->bytes + reader->start, reader->end - reader->start, packet);
        if (used < 0) { reader->failed = true; break; }
        if (used == 0) {
            memmove(reader->bytes, reader->bytes + reader->start, reader->end - reader->start);
            reader->end -= reader->start;
            reader->start = 0u;
            const size_t got = fread(reader->bytes + reader->end, 1u, sizeof reader->bytes - reader->end, reader->file);
            if (got == 0u) { reader->failed = reader->end != 0u; return false; }
            reader->end += (uint32_t)got;
            continue;
        }
        reader->start += (uint32_t)used;
        if (packet->id == RECOMP_SFD_END) return false;
        if (packet->id == RECOMP_SFD_PRIVATE && reader == &movie->video && movie->records_size + packet->size <= RECORDS) {
            memcpy(movie->records + movie->records_size, packet->payload, packet->size);
            movie->records_size += packet->size;
        }
        if (packet->id == id) return true;
    }
    return false;
}

static void load_video(plm_buffer_t *buffer, void *user)
{
    RecompSfdMovie *movie = user;
    RecompSfdPacket packet;
    if (!next_packet(movie, &movie->video, RECOMP_SFD_VIDEO, &packet)) {
        plm_buffer_signal_end(buffer);
        return;
    }
    /* TMPGEnc tags its streams in user data and codes 11-bit intra DC; FFmpeg keys on the same tag. */
    static const char tmpgenc[9] = "\0TMPGEXS\0";
    for (uint32_t i = 0u; movie->decoder->intra_dc_precision == 0 && i + sizeof tmpgenc <= packet.size; i++)
        if (memcmp((const uint8_t *)packet.payload + i, tmpgenc, sizeof tmpgenc) == 0) movie->decoder->intra_dc_precision = 3;
    plm_buffer_write(buffer, (uint8_t *)packet.payload, packet.size);
}

static void decode_video(RecompSfdMovie *movie, Frame *slot)
{
    plm_frame_t *frame = plm_video_decode(movie->decoder);
    if (frame == NULL) {
        lock(&movie->guard);
        movie->video_done = true;
        movie->failed |= movie->video.failed || movie->decoded == 0u;
        unlock(&movie->guard);
        return;
    }
    if (slot->pixels == NULL) slot->pixels = malloc((size_t)frame->width * frame->height * 4u);
    if (slot->pixels == NULL) {
        lock(&movie->guard);
        movie->failed = movie->video_done = true;
        unlock(&movie->guard);
        return;
    }
    plm_frame_to_bgra(frame, slot->pixels, (int)frame->width * 4);
    for (size_t i = 3u; i < (size_t)frame->width * frame->height * 4u; i += 4u) slot->pixels[i] = 0xffu;
    lock(&movie->guard);
    if (movie->decoded == 0u) {
        movie->fps = plm_video_get_framerate(movie->decoder);
        movie->cue_count = recomp_sfd_subtitles(movie->records, movie->records_size, movie->cues, CUES);
    }
    movie->width = frame->width;
    movie->height = frame->height;
    slot->index = movie->decoded++;
    slot->full = true;
    unlock(&movie->guard);
}

/* Decodes one audio packet into the PCM ring; false once the stream is done. */
static bool decode_audio(RecompSfdMovie *movie)
{
    RecompSfdPacket packet;
    /* ADX a full ring left undecoded goes first. */
    if (movie->pcm == NULL || movie->adx_have < RECOMP_ADX_FRAME_BYTES * movie->adx.channels) {
        if (!next_packet(movie, &movie->audio, RECOMP_SFD_AUDIO, &packet) ||
            movie->adx_have + packet.size > sizeof movie->adx_bytes) return false;
        memcpy(movie->adx_bytes + movie->adx_have, packet.payload, packet.size);
        movie->adx_have += packet.size;
    }
    uint32_t used = 0u;
    if (movie->pcm == NULL) {
        used = recomp_adx_open(&movie->adx, movie->adx_bytes, movie->adx_have);
        if (used == 0u) return movie->adx_have < 0x1000u; /* the header spans one packet */
        movie->pcm_frames = movie->adx.sample_rate * PCM_SECONDS;
        movie->pcm = malloc((size_t)movie->pcm_frames * movie->adx.channels * sizeof(int16_t));
        if (movie->pcm == NULL) return false;
        lock(&movie->guard);
        movie->header = true;
        unlock(&movie->guard);
    }
    const uint32_t group = RECOMP_ADX_FRAME_BYTES * movie->adx.channels;
    int16_t samples[RECOMP_ADX_FRAME_SAMPLES * RECOMP_ADX_MAX_CHANNELS];
    for (; used + group <= movie->adx_have &&
            movie->pcm_count + RECOMP_ADX_FRAME_SAMPLES <= movie->pcm_frames; used += group) {
        const uint32_t count = recomp_adx_decode(&movie->adx, movie->adx_bytes + used, samples);
        if (count == 0u) return false;
        for (uint32_t i = 0u; i < count; ++i) {
            const uint32_t at = (movie->pcm_read + movie->pcm_count + i) % movie->pcm_frames;
            memcpy(movie->pcm + (size_t)at * movie->adx.channels, samples + i * movie->adx.channels,
                movie->adx.channels * sizeof(int16_t));
        }
        movie->pcm_count += count;
    }
    memmove(movie->adx_bytes, movie->adx_bytes + used, movie->adx_have - used);
    movie->adx_have -= used;
    return true;
}

/* Submits decoded audio up to the clock plus the lead, from this host thread,
   so a stalled game thread does not starve it. */
static void submit_audio(RecompSfdMovie *movie, uint64_t start_ms, int32_t volume)
{
    const uint64_t due = (host_ms() - start_ms + AUDIO_LEAD_MS) * movie->adx.sample_rate / 1000u;
    while (movie->submitted < due && movie->pcm_count != 0u) {
        uint32_t count = movie->pcm_frames - movie->pcm_read;
        if (count > movie->pcm_count) count = movie->pcm_count;
        if (count > due - movie->submitted) count = (uint32_t)(due - movie->submitted);
        if (count > CHUNK_FRAMES) count = CHUNK_FRAMES;
        const uint32_t bytes = count * movie->adx.channels * (uint32_t)sizeof(int16_t);
        recomp_dsound_output_lock();
        recomp_audio_output_submit(movie->slot, (const uint8_t *)(movie->pcm + (size_t)movie->pcm_read * movie->adx.channels),
            bytes, movie->adx.sample_rate, movie->adx.channels, 16u, volume);
        recomp_dsound_output_unlock();
        movie->pcm_read = (movie->pcm_read + count) % movie->pcm_frames;
        movie->pcm_count -= count;
        movie->submitted += count;
    }
}

#ifdef _WIN32
static DWORD WINAPI run(LPVOID user)
#else
static void *run(void *user)
#endif
{
    RecompSfdMovie *movie = user;
    for (;;) {
        lock(&movie->guard);
        Frame *slot = NULL;
        for (uint32_t i = 0u; i < FRAMES && !movie->video_done; ++i)
            if (!movie->frames[i].full) slot = &movie->frames[i];
        const bool stopping = movie->stopping, audio = movie->sound && !movie->audio_done;
        const bool started = movie->started;
        const uint64_t start_ms = movie->start_ms;
        const int32_t volume = movie->volume;
        bool done = movie->failed;
        unlock(&movie->guard);
        if (stopping) break;
        bool busy = false;
        if (slot != NULL && !done) {
            decode_video(movie, slot);
            busy = true;
        }
        if (audio && !done && (movie->pcm == NULL ||
                movie->pcm_frames - movie->pcm_count >= movie->adx.sample_rate / 10u)) {
            if (!decode_audio(movie)) {
                lock(&movie->guard);
                movie->audio_done = true;
                movie->header = true; /* a broken or missing track leaves the movie silent */
                unlock(&movie->guard);
            }
            busy = true;
        }
        if (started && movie->pcm != NULL) submit_audio(movie, start_ms, volume);
        lock(&movie->guard);
        movie->ready |= movie->decoded != 0u && (!movie->sound || movie->header);
        unlock(&movie->guard);
        if (!busy) pause_ms(2u);
    }
#ifdef _WIN32
    return 0u;
#else
    return NULL;
#endif
}

RecompSfdMovie *recomp_sfd_movie_open(const char *host_path, uint32_t audio_slot, bool audio)
{
    RecompSfdMovie *movie = calloc(1u, sizeof *movie);
    if (movie == NULL) return NULL;
    lock_init(&movie->guard);
    movie->slot = audio_slot;
    movie->sound = audio;
    if (audio) {
        /* The first call owns the output; the worker must not claim it. */
        recomp_dsound_output_lock();
        recomp_audio_output_initialize();
        recomp_dsound_output_unlock();
    }
    movie->video.file = fopen(host_path, "rb");
    movie->audio.file = audio ? fopen(host_path, "rb") : NULL;
    movie->stream = plm_buffer_create_with_capacity(1u << 18);
    movie->decoder = movie->stream != NULL ? plm_video_create_with_buffer(movie->stream, 0) : NULL;
    if (movie->video.file == NULL || (audio && movie->audio.file == NULL) || movie->decoder == NULL) {
        if (movie->video.file != NULL) fclose(movie->video.file);
        if (movie->audio.file != NULL) fclose(movie->audio.file);
        if (movie->decoder != NULL) plm_video_destroy(movie->decoder);
        if (movie->stream != NULL) plm_buffer_destroy(movie->stream);
        free(movie);
        return NULL;
    }
    plm_buffer_set_load_callback(movie->stream, load_video, movie);
#ifdef _WIN32
    movie->thread = CreateThread(NULL, 0u, run, movie, 0u, NULL);
    movie->running = movie->thread != NULL;
#else
    movie->running = pthread_create(&movie->thread, NULL, run, movie) == 0;
#endif
    if (!movie->running) movie->failed = true;
    return movie;
}

void recomp_sfd_movie_close(RecompSfdMovie *movie)
{
    if (movie == NULL) return;
    lock(&movie->guard);
    movie->stopping = true;
    const bool started = movie->started;
    unlock(&movie->guard);
    if (movie->running) {
#ifdef _WIN32
        WaitForSingleObject(movie->thread, INFINITE);
        CloseHandle(movie->thread);
#else
        pthread_join(movie->thread, NULL);
#endif
    }
    if (started && movie->sound) {
        recomp_dsound_output_lock();
        recomp_audio_output_reset_voice(movie->slot);
        recomp_dsound_output_unlock();
    }
    plm_video_destroy(movie->decoder);
    plm_buffer_destroy(movie->stream);
    fclose(movie->video.file);
    if (movie->audio.file != NULL) fclose(movie->audio.file);
    for (uint32_t i = 0u; i < FRAMES; ++i) free(movie->frames[i].pixels);
    free(movie->pcm);
    free(movie->current);
    free(movie);
}

RecompSfdState recomp_sfd_movie_poll(RecompSfdMovie *movie, double now)
{
    lock(&movie->guard);
    RecompSfdState state = RECOMP_SFD_PREPARING;
    if (movie->failed) state = RECOMP_SFD_FAILED;
    else if (movie->started) {
        bool queued = false;
        for (uint32_t i = 0u; i < FRAMES; ++i) queued |= movie->frames[i].full;
        state = movie->video_done && !queued && (now - movie->start) * movie->fps >= movie->decoded
            ? RECOMP_SFD_ENDED : RECOMP_SFD_PLAYING;
    } else if (movie->ready) {
        movie->started = true;
        movie->start = now;
        movie->start_ms = host_ms();
        state = RECOMP_SFD_PLAYING;
    }
    unlock(&movie->guard);
    return state;
}

bool recomp_sfd_movie_take_frame(RecompSfdMovie *movie, double now,
    const uint8_t **pixels, uint32_t *width, uint32_t *height)
{
    lock(&movie->guard);
    Frame *chosen = NULL;
    if (movie->started) {
        const double due = floor((now - movie->start) * movie->fps);
        for (uint32_t i = 0u; i < FRAMES; ++i) {
            Frame *frame = &movie->frames[i];
            if (frame->full && frame->index <= due && (chosen == NULL || frame->index > chosen->index)) chosen = frame;
        }
    }
    bool taken = false;
    if (chosen != NULL) {
        const size_t bytes = (size_t)movie->width * movie->height * 4u;
        if (movie->current == NULL) movie->current = malloc(bytes);
        if (movie->current != NULL) {
            memcpy(movie->current, chosen->pixels, bytes);
            movie->current_width = movie->width;
            movie->current_height = movie->height;
            taken = true;
        }
        /* Frames the clock has passed are dropped, as Sofdec drops late frames. */
        const uint32_t index = chosen->index;
        for (uint32_t i = 0u; i < FRAMES; ++i)
            if (movie->frames[i].full && movie->frames[i].index <= index) movie->frames[i].full = false;
    }
    unlock(&movie->guard);
    if (taken) {
        *pixels = movie->current;
        *width = movie->current_width;
        *height = movie->current_height;
    }
    return taken;
}

void recomp_sfd_movie_set_volume(RecompSfdMovie *movie, int32_t hundredth_db)
{
    lock(&movie->guard);
    movie->volume = hundredth_db;
    unlock(&movie->guard);
}

uint32_t recomp_sfd_movie_subtitle(RecompSfdMovie *movie, uint32_t channel, double now,
    char *text, uint32_t size)
{
    uint32_t length = 0u;
    lock(&movie->guard);
    for (uint32_t i = 0u; movie->started && size != 0u && i < movie->cue_count; ++i) {
        const RecompSfdCue *cue = &movie->cues[i];
        const double at = (now - movie->start) * cue->unit;
        if (cue->channel != channel || at < cue->start || at >= (double)cue->start + cue->duration) continue;
        length = (uint32_t)strlen(cue->content) + 1u;
        if (length > size) length = size;
        memcpy(text, cue->content, length);
        break;
    }
    unlock(&movie->guard);
    return length;
}

