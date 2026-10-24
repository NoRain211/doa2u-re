#include "audio_output.h"
#include "audio_capture_xapo.h"
#include "audio_xtalk_xapo.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <xaudio2.h>
#include "xaudio2_reverb.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr uint32_t kVoiceCount = 256;
/* The ring bounds memory; the time budget bounds latency, so voices that
   submit millisecond pieces are not dropped after a few milliseconds. */
constexpr uint32_t kQueueSize = 128;
constexpr float kMaxQueueMs = 250.0f;
constexpr uint32_t kMaxBufferBytes = 160000;
/* Silence queued ahead of a new or starved voice, so producer jitter (15.6 ms
   clock steps, thread wakeups) is absorbed instead of heard as gaps. */
constexpr uint32_t kPrerollMs = 50;
/* XAudio2 does not consume exactly the nominal rate: a 22050 Hz source runs
   0.23% fast without resampling (whole frames per 10 ms pass) and 0.05% slow
   with it. A wall-clock producer would drain or flood the queue, so each voice
   trims its pitch (inaudibly, at most this much) to hold the cushion. */
constexpr float kRateTrim = 0.01f;

struct PcmBuffer {
    uint8_t *data;
    uint32_t capacity;
};

struct OutputVoice {
    IXAudio2SourceVoice *source;
    bool sends_reverb;
    PcmBuffer buffers[kQueueSize];
    uint32_t sample_rate, channels, bits_per_sample;
    uint32_t front, queued;
    uint64_t bytes_submitted;
    float level_ms;
};

struct VoiceRouting {
    uint32_t channels;
    float dry[3 * 6], wet[3 * 2];
};

std::atomic<HRESULT> critical_error{S_OK};

struct EngineCallback : IXAudio2EngineCallback {
    void STDMETHODCALLTYPE OnProcessingPassStart() override {}
    void STDMETHODCALLTYPE OnProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnCriticalError(HRESULT error) override
    {
        critical_error.store(error, std::memory_order_relaxed);
    }
};

EngineCallback callback;
IXAudio2 *engine;
IXAudio2MasteringVoice *master;
IXAudio2SubmixVoice *reverb_voice;
OutputVoice voices[kVoiceCount];
VoiceRouting routing[kVoiceCount];
bool reported_nonzero[kVoiceCount];
bool routing_reported[kVoiceCount];
bool attempted, com_initialized, callback_registered, summary_printed, muted;
/* Initialized COM; only this thread may release it. */
std::atomic<DWORD> owner_thread{0};
unsigned long long submitted_buffers, submitted_bytes, nonzero_buffers;
unsigned long long dropped_buffers, underruns;
unsigned long long reverb_updates, routing_updates;
unsigned engine_glitches;

void destroyVoice(OutputVoice &voice)
{
    /* DestroyVoice waits until XAudio2 no longer reads these buffers. */
    if (voice.source) voice.source->DestroyVoice();
    for (auto &buffer : voice.buffers) std::free(buffer.data);
    voice = {};
}

void releaseOutput()
{
    for (auto &voice : voices) destroyVoice(voice);
    if (reverb_voice) reverb_voice->DestroyVoice();
    reverb_voice = nullptr;
    std::memset(routing, 0, sizeof routing);
    std::memset(routing_reported, 0, sizeof routing_reported);
    if (master) master->DestroyVoice();
    recomp_audio_capture_save();
    master = nullptr;
    if (engine) {
        XAUDIO2_PERFORMANCE_DATA performance{};
        engine->GetPerformanceData(&performance);
        engine_glitches = performance.GlitchesSinceEngineStarted;
        if (callback_registered) engine->UnregisterForCallbacks(&callback);
        engine->Release();
    }
    engine = nullptr;
    callback_registered = false;
    if (com_initialized) CoUninitialize();
    com_initialized = false;
}

void disableOutput(const char *operation, HRESULT error)
{
    if (!recomp_audio_output_on_owner_thread()) {
        /* CoUninitialize is thread-affine: stop output here and let the owner
           thread's next call release it. */
        HRESULT none = S_OK;
        critical_error.compare_exchange_strong(none, error);
        return;
    }
    std::fprintf(stderr,
        "[audio-output] disabled operation=%s error=0x%08lx\n",
        operation, static_cast<unsigned long>(error));
    releaseOutput();
}

void checkCriticalError()
{
    const HRESULT error = critical_error.load(std::memory_order_relaxed);
    if (engine && FAILED(error)) disableOutput("device", error);
}

void dropBuffer(uint32_t slot, const char *reason, float queued_ms = -1.0f)
{
    ++dropped_buffers;
    if (dropped_buffers <= 4 ||
        (dropped_buffers & (dropped_buffers - 1)) == 0) {
        if (queued_ms >= 0.0f)
            std::fprintf(stderr,
                "[audio-output] dropped=%llu slot=%u reason=%s queued_ms=%.0f\n",
                dropped_buffers, slot, reason, static_cast<double>(queued_ms));
        else
            std::fprintf(stderr,
                "[audio-output] dropped=%llu slot=%u reason=%s\n",
                dropped_buffers, slot, reason);
    }
}

bool applyRouting(OutputVoice &voice, const VoiceRouting &route)
{
    XAUDIO2_SEND_DESCRIPTOR descriptors[] = {{0, master}, {0, reverb_voice}};
    XAUDIO2_VOICE_SENDS sends{reverb_voice ? 2u : 1u, descriptors};
    HRESULT error = S_OK;
    if (voice.sends_reverb != (reverb_voice != nullptr)) {
        error = voice.source->SetOutputVoices(&sends);
        if (FAILED(error)) {
            disableOutput("SetOutputVoices", error);
            return false;
        }
        voice.sends_reverb = reverb_voice != nullptr;
    }
    error = voice.source->SetOutputMatrix(master, route.channels,
        recomp_audio_output_speakers(), route.dry);
    if (SUCCEEDED(error) && reverb_voice)
        error = voice.source->SetOutputMatrix(reverb_voice, route.channels, 2, route.wet);
    if (FAILED(error)) {
        disableOutput("SetOutputMatrix", error);
        return false;
    }
    return true;
}

bool applyDefaultRouting(OutputVoice &voice)
{
    const uint32_t speakers = recomp_audio_output_speakers();
    if (speakers != 6) return true;
    /* An unrouted voice sits in front left/right; the other speakers stay silent. */
    float dry[3 * 6] = {};
    for (uint32_t source = 0; source < voice.channels && source < 2; ++source)
        dry[source + source * voice.channels] = 1.0f;
    const HRESULT error = voice.source->SetOutputMatrix(
        master, voice.channels, speakers, dry);
    if (FAILED(error)) {
        disableOutput("SetOutputMatrix", error);
        return false;
    }
    return true;
}

bool inRange(float value, float low, float high)
{
    return std::isfinite(value) && value >= low && value <= high;
}

bool queueBuffer(OutputVoice &voice, uint32_t slot, const uint8_t *pcm,
    uint32_t bytes, uint8_t silence)
{
    if (voice.queued == kQueueSize) {
        dropBuffer(slot, "queue-full");
        return false;
    }
    PcmBuffer &owned = voice.buffers[(voice.front + voice.queued) % kQueueSize];
    if (owned.capacity < bytes) {
        auto *data = static_cast<uint8_t *>(std::realloc(owned.data, bytes));
        if (!data) {
            dropBuffer(slot, "allocation");
            return false;
        }
        owned.data = data;
        owned.capacity = bytes;
    }
    if (pcm) std::memcpy(owned.data, pcm, bytes);
    else std::memset(owned.data, silence, bytes);
    XAUDIO2_BUFFER buffer{};
    buffer.AudioBytes = bytes;
    buffer.pAudioData = owned.data;
    const HRESULT error = voice.source->SubmitSourceBuffer(&buffer);
    if (FAILED(error)) {
        dropBuffer(slot, "submit");
        disableOutput("SubmitSourceBuffer", error);
        return false;
    }
    ++voice.queued;
    voice.bytes_submitted += bytes;
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
    const DWORD owner = owner_thread.load(std::memory_order_relaxed);
    return owner == 0 || owner == GetCurrentThreadId();
}

extern "C" int recomp_audio_output_enabled(void)
{
    return engine != nullptr &&
        !FAILED(critical_error.load(std::memory_order_relaxed));
}

extern "C" int recomp_audio_output_ready(void)
{
    return recomp_audio_output_enabled() || muted;
}

extern "C" void recomp_audio_output_initialize(void)
{
    checkCriticalError();
    if (attempted) return;
    attempted = true;
    muted = false;
    owner_thread = GetCurrentThreadId();
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
    HRESULT error = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(error)) {
        disableOutput("CoInitializeEx", error);
        return;
    }
    com_initialized = true;
    error = XAudio2Create(&engine, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(error)) {
        disableOutput("XAudio2Create", error);
        return;
    }
    error = engine->RegisterForCallbacks(&callback);
    if (FAILED(error)) {
        disableOutput("RegisterForCallbacks", error);
        return;
    }
    callback_registered = true;
    /* XAudio2 2.9 derives the mastering channel mask from this count:
       6 selects KSAUDIO_SPEAKER_5POINT1 (FL, FR, C, LFE, BL, BR). */
    error = engine->CreateMasteringVoice(
        &master, recomp_audio_output_speakers(), 48000);
    if (FAILED(error)) {
        disableOutput("CreateMasteringVoice", error);
        return;
    }
    error = master->SetVolume(static_cast<float>(gain));
    if (FAILED(error)) {
        disableOutput("MasterSetVolume", error);
        return;
    }
    recomp_audio_capture_attach(master);
    std::fprintf(stderr,
        "[audio-output] initialized backend=xaudio2_9 master_gain=%.6f\n", gain);
}

extern "C" void recomp_audio_output_shutdown(void)
{
    if (!recomp_audio_output_on_owner_thread()) {
        std::fprintf(stderr, "[audio-output] shutdown deferred to owner thread\n");
        return;
    }
    if (summary_printed) return;
    checkCriticalError();
    releaseOutput();
    muted = false;
    attempted = true;
    summary_printed = true;
    std::fprintf(stderr,
        "[audio-output] summary submitted_buffers=%llu submitted_bytes=%llu "
        "nonzero_buffers=%llu dropped_buffers=%llu underruns=%llu "
        "engine_glitches=%u reverb_updates=%llu routing_updates=%llu\n",
        submitted_buffers, submitted_bytes, nonzero_buffers, dropped_buffers,
        underruns, engine_glitches, reverb_updates, routing_updates);
}

extern "C" void recomp_audio_output_reset_voice(uint32_t slot)
{
    if (!recomp_audio_output_on_owner_thread()) return;
    checkCriticalError();
    if (slot < kVoiceCount) {
        destroyVoice(voices[slot]);
        routing[slot] = {};
        routing_reported[slot] = false;
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
    if (muted) return 1;
    if (!recomp_audio_output_enabled()) return 0;
    if (!reverb_voice) {
        IUnknown *reverb = nullptr;
        HRESULT error = XAudio2CreateReverb(&reverb);
        if (FAILED(error)) {
            disableOutput("XAudio2CreateReverb", error);
            return 0;
        }
        IUnknown *canceller = recomp_audio_xtalk_xapo_create();
        if (!canceller) {
            reverb->Release();
            disableOutput("XtalkXapoCreate", E_OUTOFMEMORY);
            return 0;
        }
        /* The canceller runs on the wet return, after the reverb, at unity. */
        XAUDIO2_EFFECT_DESCRIPTOR descriptors[2] = {{reverb, TRUE, 2}, {canceller, TRUE, 2}};
        XAUDIO2_EFFECT_CHAIN chain{2, descriptors};
        error = engine->CreateSubmixVoice(&reverb_voice, 2, 48000, 0, 0, nullptr, &chain);
        reverb->Release();
        canceller->Release();
        if (FAILED(error)) {
            disableOutput("CreateSubmixVoice", error);
            return 0;
        }
        if (recomp_audio_output_speakers() == 6) {
            /* The reverb return is stereo and belongs at front left/right. */
            const float wet_to_master[2 * 6] = {1.0f, 0.0f, 0.0f, 1.0f,
                0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
            error = reverb_voice->SetOutputMatrix(master, 2, 6, wet_to_master);
            if (FAILED(error)) {
                disableOutput("ReverbSetOutputMatrix", error);
                return 0;
            }
        }
    }
    XAUDIO2FX_REVERB_I3DL2_PARAMETERS i3dl2{100.0f, p.room, p.room_hf,
        p.room_rolloff, p.decay_time, p.decay_hf_ratio, p.reflections,
        p.reflections_delay, p.reverb, p.reverb_delay, p.diffusion,
        p.density, p.hf_reference};
    XAUDIO2FX_REVERB_PARAMETERS native{};
    /* shortcut: native conversion ignores rolloff; add distance behavior with 3D buffers. */
    ReverbConvertI3DL2ToNative(&i3dl2, &native, FALSE);
    const HRESULT error = reverb_voice->SetEffectParameters(0, &native, sizeof native);
    if (FAILED(error)) {
        disableOutput("ReverbSetEffectParameters", error);
        return 0;
    }
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
        if (!inRange(dry[i], -XAUDIO2_MAX_VOLUME_LEVEL, XAUDIO2_MAX_VOLUME_LEVEL)) return 0;
        route.dry[i] = dry[i];
    }
    for (uint32_t i = 0; i < channels * 2; ++i) {
        if (!inRange(wet[i], -XAUDIO2_MAX_VOLUME_LEVEL, XAUDIO2_MAX_VOLUME_LEVEL)) return 0;
        route.wet[i] = wet[i];
        wet_send |= wet[i] != 0.0f;
    }
    recomp_audio_output_initialize();
    if (muted) return 1;
    if (!recomp_audio_output_enabled() || (wet_send && !reverb_voice)) return 0;
    OutputVoice &voice = voices[slot];
    if (voice.source && voice.channels != channels) return 0;
    if (std::memcmp(&routing[slot], &route, sizeof route) == 0) return 1;
    if (voice.source && !applyRouting(voice, route)) return 0;
    routing[slot] = route;
    ++routing_updates;
    if (wet_send && !routing_reported[slot]) {
        routing_reported[slot] = true;
        std::fprintf(stderr,
            "[audio-output] routing slot=%u channels=%u "
            "dry=%.3f,%.3f,%.3f,%.3f,%.3f,%.3f wet=%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n", slot, channels,
            static_cast<double>(route.dry[0]), static_cast<double>(route.dry[1]),
            static_cast<double>(route.dry[2]), static_cast<double>(route.dry[3]),
            static_cast<double>(route.dry[4]), static_cast<double>(route.dry[5]),
            static_cast<double>(route.wet[0]), static_cast<double>(route.wet[1]),
            static_cast<double>(route.wet[2]), static_cast<double>(route.wet[3]),
            static_cast<double>(route.wet[4]), static_cast<double>(route.wet[5]));
    }
    return 1;
}

extern "C" void recomp_audio_output_submit(
    uint32_t slot, const uint8_t *pcm, uint32_t bytes,
    uint32_t sample_rate, uint32_t channels, uint32_t bits_per_sample,
    int32_t volume_hundredth_db)
{
    recomp_audio_output_initialize();
    if (!engine || bytes == 0 ||
        FAILED(critical_error.load(std::memory_order_relaxed))) return;
    if (slot >= kVoiceCount || !pcm || bytes > kMaxBufferBytes ||
        sample_rate < XAUDIO2_MIN_SAMPLE_RATE ||
        sample_rate > XAUDIO2_MAX_SAMPLE_RATE ||
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

    OutputVoice &voice = voices[slot];
    if (routing[slot].channels && routing[slot].channels != channels) {
        dropBuffer(slot, "routing-channel-mismatch");
        return;
    }
    if (voice.source && (voice.sample_rate != sample_rate ||
        voice.channels != channels || voice.bits_per_sample != bits_per_sample)) {
        destroyVoice(voice);
    }
    if (!voice.source) {
        WAVEFORMATEX format{};
        format.wFormatTag = bits_per_sample == 32 ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
        format.nChannels = static_cast<WORD>(channels);
        format.nSamplesPerSec = sample_rate;
        format.nAvgBytesPerSec = sample_rate * block_align;
        format.nBlockAlign = static_cast<WORD>(block_align);
        format.wBitsPerSample = static_cast<WORD>(bits_per_sample);
        HRESULT error = engine->CreateSourceVoice(
            &voice.source, &format, 0, 1.0f + kRateTrim);
        if (FAILED(error)) {
            dropBuffer(slot, "create-voice");
            disableOutput("CreateSourceVoice", error);
            return;
        }
        voice.sample_rate = sample_rate;
        voice.channels = channels;
        voice.bits_per_sample = bits_per_sample;
        voice.level_ms = kPrerollMs;
        if (routing[slot].channels && !applyRouting(voice, routing[slot])) return;
        if (!routing[slot].channels && !applyDefaultRouting(voice)) return;
        error = voice.source->Start();
        if (FAILED(error)) {
            dropBuffer(slot, "start-voice");
            disableOutput("Start", error);
            return;
        }
    }

    if (volume_hundredth_db < -10000) volume_hundredth_db = -10000;
    if (volume_hundredth_db > 0) volume_hundredth_db = 0;
    const float gain = volume_hundredth_db == -10000 ? 0.0f :
        std::pow(10.0f, volume_hundredth_db / 2000.0f);
    HRESULT error = voice.source->SetVolume(gain);
    if (FAILED(error)) {
        dropBuffer(slot, "volume");
        disableOutput("SetVolume", error);
        return;
    }
    XAUDIO2_VOICE_STATE state{};
    voice.source->GetState(&state, 0);
    const bool starved = state.BuffersQueued == 0;
    if (starved && voice.queued > 0) ++underruns;
    /* BuffersQueued includes the active buffer; only retire the FIFO prefix. */
    while (voice.queued > state.BuffersQueued) {
        voice.front = (voice.front + 1) % kQueueSize;
        --voice.queued;
    }
    const uint8_t silence = bits_per_sample == 8 ? 0x80 : 0;
    if (starved && !queueBuffer(voice, slot, nullptr,
            sample_rate * kPrerollMs / 1000 * block_align, silence)) {
        return;
    }
    const float queued_ms = static_cast<float>(
        voice.bytes_submitted / block_align - state.SamplesPlayed) * 1000.0f / sample_rate;
    if (voice.queued == kQueueSize) {
        dropBuffer(slot, "queue-full", queued_ms);
        return;
    }
    if (queued_ms > kMaxQueueMs) {
        dropBuffer(slot, "too-far-ahead", queued_ms);
        return;
    }
    voice.level_ms += (queued_ms - voice.level_ms) / 16.0f; /* smooths pump jitter */
    float ratio = 1.0f + (voice.level_ms - kPrerollMs) / 10000.0f;
    if (ratio < 1.0f - kRateTrim) ratio = 1.0f - kRateTrim;
    if (ratio > 1.0f + kRateTrim) ratio = 1.0f + kRateTrim;
    error = voice.source->SetFrequencyRatio(ratio);
    if (FAILED(error)) {
        dropBuffer(slot, "rate");
        disableOutput("SetFrequencyRatio", error);
        return;
    }
    if (!queueBuffer(voice, slot, pcm, bytes, silence)) return;
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
        for (uint32_t i = 0; i < bytes && !nonzero; ++i)
            nonzero = pcm[i] != silence;
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
