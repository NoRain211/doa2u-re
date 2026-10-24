// SPDX-License-Identifier: GPL-3.0-or-later
/* SDL3 host audio backend (macOS/Linux). Mirrors audio_output_xaudio2.cpp:
   one SDL_AudioStream per guest voice, a small silence cushion on start or
   starvation, and an inaudible pitch trim that holds that cushion against the
   device's real consumption rate.

   XAudio2 sends each voice through an output matrix to the master and, when
   wet, to an I3DL2 reverb submix. Here a routed voice is converted to float
   for the device's speaker count with its dry matrix on the way in, and its
   wet half goes to a stereo twin stream on a second logical device whose
   post-mix callback runs the reverb (audio_reverb.h) over the summed wet
   signal, as the submix does. */
#include "audio_output.h"
#include "audio_reverb.h"
#include "dsound_hrtf.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace {

constexpr uint32_t kVoiceCount = 256;
constexpr uint32_t kMaxBufferBytes = 160000;
constexpr uint32_t kMinSampleRate = 1000;
constexpr uint32_t kMaxSampleRate = 200000;
/* XAudio2's per-matrix-entry limit (XAUDIO2_MAX_VOLUME_LEVEL). */
constexpr float kMaxVolumeLevel = 16777216.0f;
/* Silence queued ahead of a new or starved voice so producer jitter (pump
   wakeups, game-thread hitches) is absorbed instead of heard as gaps. */
constexpr uint32_t kPrerollMs = 50;
/* The producer follows the host monotonic clock; the device follows its own
   crystal. Each voice trims its pitch by at most this much to stay level. */
constexpr float kRateTrim = 0.01f;
constexpr int kOutputRate = 48000;

struct VoiceRouting {
    uint32_t channels;
    float dry[3 * 6], wet[3 * 2];
};

struct SdlVoice {
    SDL_AudioStream *stream;
    SDL_AudioStream *wet_stream;  // routed voices with a wet send only
    uint32_t sample_rate, channels, bits_per_sample;
    bool routed;
    float gain, ratio, level_ms;
    uint64_t bytes_submitted;
};

std::mutex audio_mutex;
SDL_AudioDeviceID audio_device, reverb_device;
SdlVoice voices[kVoiceCount];
VoiceRouting routing[kVoiceCount];
bool reported_nonzero[kVoiceCount];
bool attempted, summary_printed, muted, reverb_ready;
std::atomic<std::thread::id> owner_thread{};
std::vector<uint8_t> silence_buffer, wet_silence_buffer;
std::vector<float> dry_frames, wet_frames;

std::mutex reverb_mutex;
RecompAudioReverbState *reverb;
RecompXtalk xtalk;

unsigned long long submitted_buffers, submitted_bytes, nonzero_buffers;
unsigned long long dropped_buffers, underruns, voice_resets;
unsigned long long reverb_updates, routing_updates;

/* RECOMP_AUDIO_CAPTURE=<file.wav> (or the older RECOMP_AUDIO_DUMP) records the
   dry device mix for diagnosis. A capture never overwrites an existing file. */
FILE *dump_file;
uint64_t dump_bytes;
int dump_channels, dump_rate;

void writeWavHeader(FILE *file, uint32_t data_bytes, int channels, int rate)
{
    auto u32 = [file](uint32_t v) { std::fwrite(&v, 4, 1, file); };
    auto u16 = [file](uint16_t v) { std::fwrite(&v, 2, 1, file); };
    std::fseek(file, 0, SEEK_SET);
    std::fwrite("RIFF", 1, 4, file); u32(36 + data_bytes);
    std::fwrite("WAVEfmt ", 1, 8, file); u32(16);
    u16(3); /* IEEE float */
    u16(static_cast<uint16_t>(channels)); u32(static_cast<uint32_t>(rate));
    u32(static_cast<uint32_t>(rate * channels * 4));
    u16(static_cast<uint16_t>(channels * 4)); u16(32);
    std::fwrite("data", 1, 4, file); u32(data_bytes);
}

void SDLCALL dumpPostmix(void *, const SDL_AudioSpec *spec, float *buffer, int buflen)
{
    if (!dump_file || spec->channels != dump_channels || spec->freq != dump_rate) return;
    std::fwrite(buffer, 1, static_cast<size_t>(buflen), dump_file);
    dump_bytes += static_cast<uint64_t>(buflen);
}

/* The wet device's mix is the reverb input; the callback replaces it with
   the reverb output, which SDL then adds to the dry device's mix. */
void SDLCALL reverbPostmix(void *, const SDL_AudioSpec *spec, float *buffer, int buflen)
{
    const int frames = buflen / static_cast<int>(sizeof(float)) / spec->channels;
    std::lock_guard<std::mutex> lock(reverb_mutex);
    if (reverb == nullptr || spec->channels != 2) {
        std::memset(buffer, 0, static_cast<size_t>(buflen));
        return;
    }
    recomp_audio_reverb_process(reverb, buffer, frames);
    /* shortcut: the 11-sample delay and pole assume 48 kHz; resample for other device rates. */
    recomp_xtalk_process_block(&xtalk, buffer, static_cast<uint32_t>(frames));
}

void dropBuffer(uint32_t slot, const char *reason)
{
    ++dropped_buffers;
    if (dropped_buffers <= 4 || (dropped_buffers & (dropped_buffers - 1)) == 0) {
        std::fprintf(stderr, "[audio-output] dropped=%llu slot=%u reason=%s\n",
            dropped_buffers, slot, reason);
    }
}

void destroyVoice(SdlVoice &voice)
{
    if (voice.stream) SDL_DestroyAudioStream(voice.stream);
    if (voice.wet_stream) SDL_DestroyAudioStream(voice.wet_stream);
    voice = {};
}

bool inRange(float value, float low, float high)
{
    return std::isfinite(value) && value >= low && value <= high;
}

float sampleAt(const uint8_t *pcm, uint32_t bits, uint32_t index)
{
    if (bits == 8) return (static_cast<int>(pcm[index]) - 128) / 128.0f;
    if (bits == 16) {
        int16_t value;
        std::memcpy(&value, pcm + index * 2u, sizeof value);
        return value / 32768.0f;
    }
    float value;
    std::memcpy(&value, pcm + index * 4u, sizeof value);
    return value;
}

/* Applies one output matrix ([source + destination * channels], as XAudio2)
   to whole frames, giving interleaved float for the given speaker count. */
void mixOutput(const uint8_t *pcm, uint32_t frames, uint32_t channels, uint32_t bits,
    const float *matrix, uint32_t speakers, std::vector<float> &out)
{
    out.assign(static_cast<size_t>(frames) * speakers, 0.0f);
    for (uint32_t frame = 0u; frame < frames; ++frame) {
        float *mixed = out.data() + static_cast<size_t>(frame) * speakers;
        for (uint32_t source = 0u; source < channels; ++source) {
            const float sample = sampleAt(pcm, bits, frame * channels + source);
            for (uint32_t destination = 0u; destination < speakers; ++destination)
                mixed[destination] += sample * matrix[source + destination * channels];
        }
    }
}

SDL_AudioStream *openStream(SDL_AudioDeviceID device, SDL_AudioFormat format, uint32_t channels,
    uint32_t sample_rate)
{
    SDL_AudioSpec source{};
    source.format = format;
    source.channels = static_cast<int>(channels);
    source.freq = static_cast<int>(sample_rate);
    SDL_AudioStream *stream = SDL_CreateAudioStream(&source, nullptr);
    if (stream != nullptr && !SDL_BindAudioStream(device, stream)) {
        SDL_DestroyAudioStream(stream);
        stream = nullptr;
    }
    return stream;
}

bool putData(SdlVoice &voice, uint32_t slot, const void *dry, uint32_t dry_bytes,
    const void *wet, uint32_t wet_bytes)
{
    if (!SDL_PutAudioStreamData(voice.stream, dry, static_cast<int>(dry_bytes)) ||
        (voice.wet_stream != nullptr &&
         !SDL_PutAudioStreamData(voice.wet_stream, wet, static_cast<int>(wet_bytes)))) {
        dropBuffer(slot, "put-data");
        return false;
    }
    voice.bytes_submitted += dry_bytes;
    return true;
}

bool openReverbDevice()
{
    if (reverb_device != 0) return true;
    const SDL_AudioSpec want{SDL_AUDIO_F32, 2, kOutputRate};
    reverb_device = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &want);
    if (reverb_device == 0) {
        std::fprintf(stderr, "[audio-output] reverb device failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_AudioSpec spec{};
    SDL_GetAudioDeviceFormat(reverb_device, &spec, nullptr);
    {
        std::lock_guard<std::mutex> lock(reverb_mutex);
        reverb = recomp_audio_reverb_create(spec.freq > 0 ? spec.freq : kOutputRate);
        xtalk = {};
    }
    if (reverb == nullptr || !SDL_SetAudioPostmixCallback(reverb_device, reverbPostmix, nullptr)) {
        std::fprintf(stderr, "[audio-output] reverb unavailable: %s\n", SDL_GetError());
        SDL_CloseAudioDevice(reverb_device);
        reverb_device = 0;
        return false;
    }
    SDL_ResumeAudioDevice(reverb_device);
    std::fprintf(stderr, "[audio-output] reverb ready freq=%d\n", spec.freq);
    return true;
}

} // namespace

extern "C" uint32_t recomp_audio_output_speakers(void)
{
    const char *surround = std::getenv("RECOMP_AUDIO_SURROUND");
    return surround && std::strcmp(surround, "0") != 0 ? 6u : 2u;
}

extern "C" int recomp_audio_output_on_owner_thread(void)
{
    const std::thread::id owner = owner_thread.load(std::memory_order_relaxed);
    return owner == std::thread::id{} || owner == std::this_thread::get_id();
}

extern "C" int recomp_audio_output_enabled(void)
{
    return audio_device != 0 ? 1 : 0;
}

extern "C" int recomp_audio_output_ready(void)
{
    return recomp_audio_output_enabled() || muted;
}

extern "C" void recomp_audio_output_initialize(void)
{
    std::lock_guard<std::mutex> lock(audio_mutex);
    if (attempted) return;
    attempted = true;
    muted = false;
    owner_thread = std::this_thread::get_id();

    double gain = 1.0;
    const char *setting = std::getenv("RECOMP_AUDIO_GAIN");
    if (setting) {
        char *end;
        gain = std::strtod(setting, &end);
        if (end == setting || *end != '\0' || !std::isfinite(gain) ||
            gain < 0.0 || gain > 1.0) {
            std::fprintf(stderr,
                "[audio-output] muted invalid RECOMP_AUDIO_GAIN (expected 0..1)\n");
            return;
        }
    }
    if (gain == 0.0) {
        muted = true;
        std::fprintf(stderr, "[audio-output] muted RECOMP_AUDIO_GAIN=0\n");
        return;
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "[audio-output] SDL audio init failed: %s\n", SDL_GetError());
        return;
    }
    /* Float output keeps headroom while SDL sums voices; it is CoreAudio's
       native format, so no extra integer conversion stage is added. */
    const SDL_AudioSpec want{SDL_AUDIO_F32,
        static_cast<int>(recomp_audio_output_speakers()), kOutputRate};
    audio_device = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &want);
    if (audio_device == 0) {
        std::fprintf(stderr, "[audio-output] SDL open audio device failed: %s\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return;
    }
    SDL_SetAudioDeviceGain(audio_device, static_cast<float>(gain));

    SDL_AudioSpec spec{};
    int frames = 0;
    SDL_GetAudioDeviceFormat(audio_device, &spec, &frames);
    const char *capture_path = std::getenv("RECOMP_AUDIO_CAPTURE");
    const char *dump_path = capture_path ? capture_path : std::getenv("RECOMP_AUDIO_DUMP");
    if (dump_path && *dump_path) {
        dump_file = std::fopen(dump_path, capture_path ? "wbx" : "wb");
        if (dump_file) {
            dump_channels = spec.channels;
            dump_rate = spec.freq;
            dump_bytes = 0;
            writeWavHeader(dump_file, 0, dump_channels, dump_rate);
            SDL_SetAudioPostmixCallback(audio_device, dumpPostmix, nullptr);
        }
    }
    SDL_ResumeAudioDevice(audio_device);
    std::fprintf(stderr,
        "[audio-output] initialized backend=sdl3 master_gain=%.6f driver=%s "
        "freq=%d channels=%d format=0x%04x frames=%d\n",
        gain, SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "unknown",
        spec.freq, spec.channels, static_cast<unsigned>(spec.format), frames);
}

extern "C" void recomp_audio_output_shutdown(void)
{
    if (!recomp_audio_output_on_owner_thread()) {
        std::fprintf(stderr, "[audio-output] shutdown deferred to owner thread\n");
        return;
    }
    std::lock_guard<std::mutex> lock(audio_mutex);
    if (summary_printed) return;
    for (auto &voice : voices) destroyVoice(voice);
    std::memset(routing, 0, sizeof routing);
    if (reverb_device != 0) {
        SDL_CloseAudioDevice(reverb_device);
        reverb_device = 0;
    }
    {
        std::lock_guard<std::mutex> reverb_lock(reverb_mutex);
        recomp_audio_reverb_destroy(reverb);
        reverb = nullptr;
        xtalk = {};
    }
    reverb_ready = false;
    if (audio_device != 0) {
        SDL_CloseAudioDevice(audio_device);
        audio_device = 0;
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    if (dump_file) {
        writeWavHeader(dump_file, static_cast<uint32_t>(dump_bytes), dump_channels, dump_rate);
        std::fclose(dump_file);
        dump_file = nullptr;
    }
    attempted = true;
    muted = false;
    summary_printed = true;
    std::fprintf(stderr,
        "[audio-output] summary submitted_buffers=%llu submitted_bytes=%llu "
        "nonzero_buffers=%llu dropped_buffers=%llu underruns=%llu voice_resets=%llu "
        "reverb_updates=%llu routing_updates=%llu\n",
        submitted_buffers, submitted_bytes, nonzero_buffers, dropped_buffers,
        underruns, voice_resets, reverb_updates, routing_updates);
}

extern "C" void recomp_audio_output_test_reset(void)
{
    std::lock_guard<std::mutex> lock(audio_mutex);
    attempted = false;
    summary_printed = false;
    owner_thread = std::thread::id{};
}

extern "C" void recomp_audio_output_reset_voice(uint32_t slot)
{
    if (!recomp_audio_output_on_owner_thread()) return;
    std::lock_guard<std::mutex> lock(audio_mutex);
    if (slot < kVoiceCount) {
        if (voices[slot].stream) ++voice_resets;
        destroyVoice(voices[slot]);
        routing[slot] = {};
    }
}

extern "C" int recomp_audio_output_set_reverb(const RecompAudioReverb *parameters)
{
    if (!recomp_audio_output_on_owner_thread() || !parameters) return 0;
    const RecompAudioReverb &p = *parameters;
    if (p.room < -10000 || p.room > 0 || p.room_hf < -10000 || p.room_hf > 0 ||
        !inRange(p.room_rolloff, 0.0f, 10.0f) ||
        !inRange(p.decay_time, 0.1f, 20.0f) ||
        !inRange(p.decay_hf_ratio, 0.1f, 2.0f) ||
        p.reflections < -10000 || p.reflections > 1000 ||
        !inRange(p.reflections_delay, 0.0f, 0.3f) ||
        p.reverb < -10000 || p.reverb > 2000 ||
        !inRange(p.reverb_delay, 0.0f, 0.1f) ||
        !inRange(p.diffusion, 0.0f, 100.0f) ||
        !inRange(p.density, 0.0f, 100.0f) ||
        !inRange(p.hf_reference, 20.0f, 20000.0f)) return 0;
    recomp_audio_output_initialize();
    std::lock_guard<std::mutex> lock(audio_mutex);
    if (muted) return 1;
    if (audio_device == 0 || !openReverbDevice()) return 0;
    {
        std::lock_guard<std::mutex> reverb_lock(reverb_mutex);
        recomp_audio_reverb_set_i3dl2(reverb, &p);
    }
    reverb_ready = true;
    ++reverb_updates;
    return 1;
}

extern "C" int recomp_audio_output_set_voice_routing(uint32_t slot, uint32_t channels,
    const float *dry, const float *wet)
{
    if (!recomp_audio_output_on_owner_thread() || slot >= kVoiceCount ||
        (channels < 1 || channels > 3) || !dry || !wet) return 0;
    VoiceRouting route{};
    route.channels = channels;
    const uint32_t speakers = recomp_audio_output_speakers();
    bool wet_send = false;
    for (uint32_t i = 0; i < channels * speakers; ++i) {
        if (!inRange(dry[i], -kMaxVolumeLevel, kMaxVolumeLevel)) return 0;
        route.dry[i] = dry[i];
    }
    for (uint32_t i = 0; i < channels * 2; ++i) {
        if (!inRange(wet[i], -kMaxVolumeLevel, kMaxVolumeLevel)) return 0;
        route.wet[i] = wet[i];
        wet_send |= wet[i] != 0.0f;
    }
    recomp_audio_output_initialize();
    std::lock_guard<std::mutex> lock(audio_mutex);
    if (muted) return 1;
    if (audio_device == 0 || (wet_send && !reverb_ready)) return 0;
    SdlVoice &voice = voices[slot];
    if (voice.stream && voice.channels != channels) return 0;
    if (std::memcmp(&routing[slot], &route, sizeof route) == 0) return 1;
    // A voice switching between direct and routed output restarts its stream.
    if (voice.stream && !voice.routed) destroyVoice(voice);
    routing[slot] = route;
    ++routing_updates;
    return 1;
}

extern "C" void recomp_audio_output_submit(
    uint32_t slot, const uint8_t *pcm, uint32_t bytes,
    uint32_t sample_rate, uint32_t channels, uint32_t bits_per_sample,
    int32_t volume_hundredth_db)
{
    recomp_audio_output_initialize();
    if (audio_device == 0 || bytes == 0) return;
    if (slot >= kVoiceCount || !pcm || bytes > kMaxBufferBytes ||
        sample_rate < kMinSampleRate || sample_rate > kMaxSampleRate ||
        (channels < 1 || channels > 3) ||
        (bits_per_sample != 8 && bits_per_sample != 16 && bits_per_sample != 32)) {
        dropBuffer(slot, "invalid-pcm");
        return;
    }
    const uint32_t block_align = channels * (bits_per_sample / 8);
    if (bytes % block_align != 0) {
        dropBuffer(slot, "partial-frame");
        return;
    }

    std::lock_guard<std::mutex> lock(audio_mutex);
    SdlVoice &voice = voices[slot];
    const VoiceRouting &route = routing[slot];
    if (route.channels && route.channels != channels) {
        dropBuffer(slot, "routing-channel-mismatch");
        return;
    }
    const bool routed = route.channels != 0u;
    const uint32_t speakers = recomp_audio_output_speakers();
    bool wet_send = false;
    for (uint32_t i = 0; routed && i < channels * 2u; ++i) wet_send |= route.wet[i] != 0.0f;
    if (voice.stream && (voice.sample_rate != sample_rate || voice.channels != channels ||
        voice.bits_per_sample != bits_per_sample || voice.routed != routed ||
        (voice.wet_stream != nullptr) != (wet_send && reverb_device != 0))) {
        destroyVoice(voice);
    }
    if (!voice.stream) {
        /* 32-bit voices are IEEE float, 1.0 full scale (the HRTF mix). A
           routed voice arrives as the float its dry matrix produces; the wet
           stream stays stereo because the reverb device is. */
        const SDL_AudioFormat format = routed ? SDL_AUDIO_F32
            : bits_per_sample == 8 ? SDL_AUDIO_U8 : bits_per_sample == 16 ? SDL_AUDIO_S16LE : SDL_AUDIO_F32LE;
        const uint32_t stream_channels = routed ? speakers : channels;
        voice.stream = openStream(audio_device, format, stream_channels, sample_rate);
        if (!voice.stream) {
            dropBuffer(slot, "create-stream");
            return;
        }
        if (wet_send && reverb_device != 0) {
            voice.wet_stream = openStream(reverb_device, format, 2u, sample_rate);
            if (!voice.wet_stream) {
                dropBuffer(slot, "create-wet-stream");
                destroyVoice(voice);
                return;
            }
        }
        voice.sample_rate = sample_rate;
        voice.channels = channels;
        voice.bits_per_sample = bits_per_sample;
        voice.routed = routed;
        voice.gain = 1.0f;
        voice.ratio = 1.0f;
        voice.level_ms = kPrerollMs;
    }

    if (volume_hundredth_db < -10000) volume_hundredth_db = -10000;
    if (volume_hundredth_db > 0) volume_hundredth_db = 0;
    const float gain = volume_hundredth_db == -10000 ? 0.0f :
        std::pow(10.0f, volume_hundredth_db / 2000.0f);
    if (gain != voice.gain) {
        voice.gain = gain;
        SDL_SetAudioStreamGain(voice.stream, gain);
        if (voice.wet_stream) SDL_SetAudioStreamGain(voice.wet_stream, gain);
    }

    const uint32_t frames = bytes / block_align;
    const uint32_t stream_align = routed ? speakers * 4u : block_align;
    const uint32_t wet_align = 2u * 4u;
    const int queued = SDL_GetAudioStreamQueued(voice.stream);
    if (queued > 0 && static_cast<uint32_t>(queued) >= kMaxBufferBytes * stream_align / block_align) {
        dropBuffer(slot, "queue-full");
        return;
    }
    const bool starved = queued <= 0;
    if (starved && voice.bytes_submitted > 0) ++underruns;
    const uint8_t silence = bits_per_sample == 8 && !routed ? 0x80 : 0;
    float queued_ms = queued > 0 ?
        static_cast<float>(queued / stream_align) * 1000.0f / sample_rate : 0.0f;
    if (starved) {
        const uint32_t preroll = sample_rate * kPrerollMs / 1000 * stream_align;
        if (silence_buffer.size() < preroll) silence_buffer.resize(preroll);
        std::memset(silence_buffer.data(), silence, preroll);
        const uint32_t wet_preroll = sample_rate * kPrerollMs / 1000 * wet_align;
        const uint8_t *wet_silence = nullptr;
        if (voice.wet_stream) {
            if (wet_silence_buffer.size() < wet_preroll) wet_silence_buffer.resize(wet_preroll);
            std::memset(wet_silence_buffer.data(), 0, wet_preroll);
            wet_silence = wet_silence_buffer.data();
        }
        if (!putData(voice, slot, silence_buffer.data(), preroll, wet_silence, wet_preroll)) return;
        queued_ms = static_cast<float>(kPrerollMs);
        voice.level_ms = queued_ms;
    }
    voice.level_ms += (queued_ms - voice.level_ms) / 16.0f; /* smooths pump jitter */
    float ratio = 1.0f + (voice.level_ms - kPrerollMs) / 10000.0f;
    if (ratio < 1.0f - kRateTrim) ratio = 1.0f - kRateTrim;
    if (ratio > 1.0f + kRateTrim) ratio = 1.0f + kRateTrim;
    if (std::fabs(ratio - voice.ratio) >= 0.0005f) {
        voice.ratio = ratio;
        SDL_SetAudioStreamFrequencyRatio(voice.stream, ratio);
        if (voice.wet_stream) SDL_SetAudioStreamFrequencyRatio(voice.wet_stream, ratio);
    }
    if (routed) {
        mixOutput(pcm, frames, channels, bits_per_sample, route.dry, speakers, dry_frames);
        if (voice.wet_stream)
            mixOutput(pcm, frames, channels, bits_per_sample, route.wet, 2u, wet_frames);
        const uint32_t dry_bytes = frames * speakers * 4u;
        const uint32_t wet_bytes = frames * 2u * 4u;
        if (!putData(voice, slot, dry_frames.data(), dry_bytes, wet_frames.data(), wet_bytes)) return;
    } else if (!putData(voice, slot, pcm, bytes, nullptr, 0u)) {
        return;
    }

    ++submitted_buffers;
    submitted_bytes += bytes;
    bool nonzero = false;
    if (bits_per_sample == 32) {
        for (uint32_t i = 0; i < bytes && !nonzero; i += sizeof(float)) {
            float sample;
            std::memcpy(&sample, pcm + i, sizeof sample);
            nonzero = sample != 0.0f;
        }
    } else {
        const uint8_t pcm_silence = bits_per_sample == 8 ? 0x80 : 0;
        for (uint32_t i = 0; i < bytes && !nonzero; ++i) nonzero = pcm[i] != pcm_silence;
    }
    if (nonzero) ++nonzero_buffers;
    if (submitted_buffers == 1 || (nonzero && !reported_nonzero[slot])) {
        std::fprintf(stderr,
            "[audio-output] submitted slot=%u bytes=%u rate=%u channels=%u "
            "bits=%u nonzero=%u gain=%.6f\n",
            slot, bytes, sample_rate, channels, bits_per_sample,
            nonzero ? 1u : 0u, static_cast<double>(gain));
    }
    if (nonzero) reported_nonzero[slot] = true;
}
