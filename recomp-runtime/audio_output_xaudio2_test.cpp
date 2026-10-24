#include "audio_output.h"
#include "audio_capture_xapo.h"
#include "audio_xtalk_xapo.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <xaudio2.h>
#include <xapo.h>
#include "xaudio2_reverb.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <thread>
#include <vector>

/* Substitute only the API calls used by the backend; this test cannot open an
   audio device. Keep checks active in release builds too. */
static void check_at(bool condition, int line)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL audio output backend line %d\n", line);
        std::abort();
    }
}

struct FakeSendDescriptor { UINT32 Flags; void *pOutputVoice; };
#define check(condition) check_at((condition), __LINE__)
struct FakeVoiceSends { UINT32 SendCount; FakeSendDescriptor *pSends; };
static bool reverb_unavailable, submix_unavailable, parameters_unavailable;
static bool sends_unavailable, matrix_unavailable;
static unsigned effect_create_calls, effect_balance, effect_parameter_calls;

struct FakeEffect : IUnknown {
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **) override { return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++effect_balance; }
    ULONG STDMETHODCALLTYPE Release() override { check(effect_balance != 0); return --effect_balance; }
};
static FakeEffect fake_effect;

static HRESULT fakeCreateReverb(IUnknown **out)
{
    ++effect_create_calls;
    if (reverb_unavailable) return E_OUTOFMEMORY;
    check(effect_balance == 0);
    *out = &fake_effect;
    fake_effect.AddRef();
    return S_OK;
}

struct FakeSource {
    WAVEFORMATEX format{};
    bool destroyed = false;
    bool started = false;
    float volume = 1.0f;
    float ratio = 1.0f;
    UINT64 played = 0;
    std::vector<XAUDIO2_BUFFER> queued;
    std::vector<void *> sends;
    std::map<void *, std::vector<float>> matrices;
    unsigned send_calls = 0, matrix_calls = 0;

    void DestroyVoice() { check(!destroyed); destroyed = true; queued.clear(); }
    HRESULT Start() { started = true; return S_OK; }
    void GetState(XAUDIO2_VOICE_STATE *state, UINT32 flags);
    HRESULT SetVolume(float gain) { volume = gain; return S_OK; }
    HRESULT SetFrequencyRatio(float value) { ratio = value; return S_OK; }
    HRESULT SetOutputVoices(const FakeVoiceSends *values)
    {
        ++send_calls;
        if (sends_unavailable) return XAUDIO2_E_INVALID_CALL;
        sends.clear();
        matrices.clear();
        for (UINT32 i = 0; i < values->SendCount; ++i) {
            check(values->pSends[i].Flags == 0 && values->pSends[i].pOutputVoice);
            sends.push_back(values->pSends[i].pOutputVoice);
        }
        return S_OK;
    }
    HRESULT SetOutputMatrix(void *destination, UINT32 channels, UINT32 outputs,
        const float *values)
    {
        ++matrix_calls;
        check(channels == format.nChannels && (outputs == 2 || outputs == 6) && destination);
        if (matrix_unavailable) return XAUDIO2_E_INVALID_CALL;
        matrices[destination] = std::vector<float>(values, values + channels * outputs);
        return S_OK;
    }
    HRESULT SubmitSourceBuffer(const XAUDIO2_BUFFER *buffer)
    {
        check(started && !destroyed);
        queued.push_back(*buffer);
        return S_OK;
    }
};

static std::vector<FakeSource *> fake_sources;
static FakeSource *current_source;
static std::map<void *, FakeSource *> allocations;
static unsigned com_balance, create_calls, release_calls;
static bool create_unavailable, master_unavailable;
static const char *fake_gain, *fake_surround;
static unsigned gain_reads;

void FakeSource::GetState(XAUDIO2_VOICE_STATE *state, UINT32 flags)
{
    check(!destroyed && flags == 0);
    state->BuffersQueued = static_cast<UINT32>(queued.size());
    state->SamplesPlayed = played;
    current_source = this;
}

struct FakeMaster {
    float volume = 1.0f;
    UINT32 channels = 0;
    HRESULT SetVolume(float gain) { volume = gain; return S_OK; }
    void DestroyVoice()
    {
        for (const auto *source : fake_sources) check(source->destroyed);
        check(effect_balance == 0);
    }
};

struct FakeSubmix {
    bool destroyed = true;
    std::map<void *, std::vector<float>> matrices;
    XAUDIO2FX_REVERB_PARAMETERS parameters{};
    HRESULT SetOutputMatrix(void *destination, UINT32 channels, UINT32 outputs,
        const float *values)
    {
        check(!destroyed && channels == 2 && destination);
        matrices[destination] = std::vector<float>(values, values + channels * outputs);
        return S_OK;
    }
    HRESULT SetEffectParameters(UINT32 effect, const void *data, UINT32 bytes)
    {
        check(!destroyed && effect == 0 && bytes == sizeof parameters);
        ++effect_parameter_calls;
        if (parameters_unavailable) return XAUDIO2_E_INVALID_CALL;
        std::memcpy(&parameters, data, bytes);
        check(parameters.WetDryMix == 100.0f && parameters.RearDelay == 5);
        return S_OK;
    }
    void DestroyVoice()
    {
        check(!destroyed);
        for (const auto *source : fake_sources) check(source->destroyed);
        destroyed = true;
        fake_effect.Release();
    }
};

struct FakeEngine {
    IXAudio2EngineCallback *callback = nullptr;
    FakeMaster master;
    FakeSubmix submix;
    HRESULT RegisterForCallbacks(IXAudio2EngineCallback *value)
    {
        callback = value;
        return S_OK;
    }
    void UnregisterForCallbacks(IXAudio2EngineCallback *value)
    {
        check(value == callback);
        callback = nullptr;
    }
    HRESULT CreateMasteringVoice(FakeMaster **out, UINT32 channels, UINT32 rate)
    {
        check((channels == 2 || channels == 6) && rate == 48000);
        if (master_unavailable) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        master.channels = channels;
        *out = &master;
        return S_OK;
    }
    HRESULT CreateSubmixVoice(FakeSubmix **out, UINT32 channels, UINT32 rate,
        UINT32 flags, UINT32 stage, const FakeVoiceSends *sends,
        const XAUDIO2_EFFECT_CHAIN *chain)
    {
        check(channels == 2 && rate == 48000 && flags == 0 && stage == 0 && !sends &&
            chain && chain->EffectCount == 2 &&
            chain->pEffectDescriptors->pEffect == &fake_effect &&
            chain->pEffectDescriptors->InitialState == TRUE &&
            chain->pEffectDescriptors->OutputChannels == 2 &&
            chain->pEffectDescriptors[1].pEffect != nullptr &&
            chain->pEffectDescriptors[1].pEffect != &fake_effect &&
            chain->pEffectDescriptors[1].InitialState == TRUE &&
            chain->pEffectDescriptors[1].OutputChannels == 2 && submix.destroyed);
        if (submix_unavailable) return E_OUTOFMEMORY;
        fake_effect.AddRef();
        submix.destroyed = false;
        *out = &submix;
        return S_OK;
    }
    HRESULT CreateSourceVoice(FakeSource **out, const WAVEFORMATEX *format,
        UINT32 flags, float ratio)
    {
        check(format->wFormatTag == (format->wBitsPerSample == 32 ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM) &&
            format->nBlockAlign == format->nChannels * (format->wBitsPerSample / 8) &&
            format->nAvgBytesPerSec == format->nSamplesPerSec * format->nBlockAlign &&
            format->cbSize == 0 &&
            flags == 0 && ratio == 1.01f);
        *out = new FakeSource;
        (*out)->format = *format;
        fake_sources.push_back(*out);
        return S_OK;
    }
    void Release() { check(callback == nullptr); ++release_calls; }
    void GetPerformanceData(XAUDIO2_PERFORMANCE_DATA *data) { *data = {}; }
};

static FakeEngine fake_engine;
static void fakeCaptureAttach(FakeMaster *) {}
static void fakeCaptureSave() {}

static HRESULT fakeCreate(FakeEngine **out, UINT32 flags, XAUDIO2_PROCESSOR cpu)
{
    check(flags == 0 && cpu == XAUDIO2_DEFAULT_PROCESSOR);
    ++create_calls;
    if (create_unavailable) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    *out = &fake_engine;
    return S_OK;
}

static HRESULT fakeComInitialize(void *, DWORD flags)
{
    check(flags == COINIT_MULTITHREADED);
    ++com_balance;
    return S_OK;
}

static void fakeComUninitialize() { check(com_balance == 1); --com_balance; }

/* Track frees separately from XAudio2 so a premature release fails even when
   the allocator leaves the old PCM bytes intact. */
namespace test_std {
using std::atomic;
using std::fprintf;
using std::isfinite;
using std::memcpy;
using std::memcmp;
using std::memset;
using std::memory_order_relaxed;
using std::pow;
using std::strcmp;
using std::strtod;

const char *getenv(const char *name)
{
    if (std::strcmp(name, "RECOMP_AUDIO_SURROUND") == 0) return fake_surround;
    check(std::strcmp(name, "RECOMP_AUDIO_GAIN") == 0);
    ++gain_reads;
    return fake_gain;
}

void *realloc(void *old, size_t size)
{
    check(current_source && !current_source->destroyed);
    for (const auto &buffer : current_source->queued)
        check(buffer.pAudioData != old);
    void *data = std::realloc(old, size);
    if (data) {
        allocations.erase(old);
        allocations[data] = current_source;
    }
    return data;
}

void free(void *data)
{
    if (!data) return;
    auto allocation = allocations.find(data);
    check(allocation != allocations.end() && allocation->second->destroyed);
    allocations.erase(allocation);
    std::free(data);
}
}

#define IXAudio2 FakeEngine
#define IXAudio2MasteringVoice FakeMaster
#define IXAudio2SourceVoice FakeSource
#define IXAudio2SubmixVoice FakeSubmix
#define XAUDIO2_SEND_DESCRIPTOR FakeSendDescriptor
#define XAUDIO2_VOICE_SENDS FakeVoiceSends
#define XAudio2Create fakeCreate
#define XAudio2CreateReverb fakeCreateReverb
#define CoInitializeEx fakeComInitialize
#define CoUninitialize fakeComUninitialize
#define std test_std
#define recomp_audio_capture_attach fakeCaptureAttach
#define recomp_audio_capture_save fakeCaptureSave
#include "audio_output_xaudio2.cpp"
#undef recomp_audio_capture_save
#undef recomp_audio_capture_attach
#undef std
#undef CoUninitialize
#undef CoInitializeEx
#undef XAudio2Create
#undef XAudio2CreateReverb
#undef XAUDIO2_VOICE_SENDS
#undef XAUDIO2_SEND_DESCRIPTOR
#undef IXAudio2SubmixVoice
#undef IXAudio2SourceVoice
#undef IXAudio2MasteringVoice
#undef IXAudio2

int main()
{
    check(sizeof(RecompAudioReverb) == 48);
    uint8_t pcm[8] = {1, 0, 2, 0, 3, 0, 4, 0};
    const RecompAudioReverb reverb_parameters{
        -1000, -100, 0.0f, 1.49f, 0.83f, -2602, 0.007f, 200, 0.011f,
        100.0f, 100.0f, 5000.0f};
    const float mono_dry[] = {1.0f, 0.0f}, mono_wet[] = {0.25f, 0.5f};
    const float silent[] = {0.0f, 0.0f, 0.0f, 0.0f};
    const char *muted_values[] = {
        "0", "", "garbage", "nan", "inf", "-0.1", "1.01", "1x"};
    for (const char *value : muted_values) {
        attempted = false;
        fake_gain = value;
        const unsigned reads = gain_reads;
        recomp_audio_output_submit(0, pcm, sizeof pcm, 8000, 1, 16, 0);
        recomp_audio_output_submit(0, pcm, sizeof pcm, 8000, 1, 16, 0);
        check(!engine && create_calls == 0 && com_balance == 0 &&
            gain_reads == reads + 1);
        check(recomp_audio_output_ready() == (std::strcmp(value, "0") == 0) &&
            !recomp_audio_output_enabled());
        check(recomp_audio_output_set_reverb(&reverb_parameters) ==
            (std::strcmp(value, "0") == 0));
        check(recomp_audio_output_set_voice_routing(0, 1, mono_dry, mono_wet) ==
            (std::strcmp(value, "0") == 0));
        check(effect_create_calls == 0 && effect_balance == 0);
        recomp_audio_output_shutdown();
        check(!recomp_audio_output_ready());
        summary_printed = false;
    }
    attempted = false;
    fake_gain = "1";
    create_unavailable = true;
    recomp_audio_output_initialize();
    recomp_audio_output_submit(0, pcm, sizeof pcm, 8000, 1, 16, 0);
    check(!engine && create_calls == 1 && com_balance == 0 && !recomp_audio_output_ready());
    recomp_audio_output_shutdown();
    recomp_audio_output_shutdown();
    check(release_calls == 0);

    /* Begin a separate process lifecycle inside this test translation unit. */
    attempted = summary_printed = false;
    create_unavailable = false;
    master_unavailable = true;
    recomp_audio_output_initialize();
    recomp_audio_output_submit(0, pcm, sizeof pcm, 8000, 1, 16, 0);
    check(!engine && !master && !fake_engine.callback && !callback_registered &&
        create_calls == 2 && release_calls == 1 && com_balance == 0 &&
        fake_sources.empty() && allocations.empty() && submitted_buffers == 0 &&
        !recomp_audio_output_ready());
    recomp_audio_output_shutdown();
    recomp_audio_output_shutdown();
    check(release_calls == 1);

    attempted = summary_printed = false;
    master_unavailable = false;
    fake_gain = nullptr;
    recomp_audio_output_initialize();
    check(fake_engine.master.volume == 1.0f && recomp_audio_output_ready() &&
        recomp_audio_output_enabled());
    std::thread([] {
        check(!recomp_audio_output_on_owner_thread());
        recomp_audio_output_shutdown();
    }).join();
    check(recomp_audio_output_on_owner_thread() && engine && com_balance == 1 &&
        release_calls == 1 && !summary_printed);
    recomp_audio_output_shutdown();
    check(!engine && !com_initialized && com_balance == 0 && release_calls == 2);
    attempted = summary_printed = false;
    fake_gain = "0.02";
    recomp_audio_output_submit(0, pcm, sizeof pcm, 8000, 1, 16, 1000);
    FakeSource *source = voices[0].source;
    /* A new voice starts behind 50 ms (400 frames) of silence. */
    check(source && source->queued.size() == 2 && source->volume == 1.0f);
    check(source->queued[0].AudioBytes == 800);
    for (uint32_t i = 0; i < 800; ++i) check(source->queued[0].pAudioData[i] == 0);
    check(std::fabs(fake_engine.master.volume - 0.02f) < 0.00001f);
    const uint8_t *copy = source->queued[1].pAudioData;
    check(copy != pcm && std::memcmp(copy, pcm, sizeof pcm) == 0);
    std::memset(pcm, 0, sizeof pcm);
    check(copy[0] == 1 && nonzero_buffers == 1);

    recomp_audio_output_submit(0, pcm, sizeof pcm, 8000, 1, 16, -2000);
    check(std::fabs(source->volume - 0.1f) < 0.00001f);
    recomp_audio_output_submit(0, pcm, sizeof pcm, 8000, 1, 16, -20000);
    check(source->volume == 0.0f);
    recomp_audio_output_submit(0, pcm, sizeof pcm, 8000, 1, 16, 0);
    check(source->volume == 1.0f);
    recomp_audio_output_submit(0, pcm, sizeof pcm, 8000, 1, 16, -10000);
    check(submitted_buffers == 5 && dropped_buffers == 0 &&
        source->queued.size() == 6 && source->volume == 0.0f);
    while (voices[0].queued < kQueueSize)
        recomp_audio_output_submit(0, pcm, sizeof pcm, 8000, 1, 16, 0);
    recomp_audio_output_submit(0, pcm, sizeof pcm, 8000, 1, 16, 0);
    check(dropped_buffers == 1 && source->queued.size() == kQueueSize);

    /* Growing a slot must only reallocate retired buffers. */
    const uint8_t *active = source->queued[2].pAudioData;
    source->queued.erase(source->queued.begin(), source->queued.begin() + 2);
    uint8_t larger[1000] = {};
    recomp_audio_output_submit(0, larger, sizeof larger, 8000, 1, 16, 0);
    check(source->queued.size() == kQueueSize - 1 && source->queued[0].pAudioData == active &&
        voices[0].front == 2 && voices[0].queued == kQueueSize - 1 && underruns == 0);

    /* A drained voice counts an underrun and rebuilds its cushion. */
    source->queued.clear();
    recomp_audio_output_submit(0, pcm, sizeof pcm, 8000, 1, 16, 0);
    check(underruns == 1 && source->queued.size() == 2 &&
        source->queued[0].AudioBytes == 800 && source->queued[1].AudioBytes == 8);
    recomp_audio_output_reset_voice(0);
    check(source->destroyed && !voices[0].source && allocations.empty());

    /* The pitch trim slows a short queue and speeds up a long one. */
    recomp_audio_output_submit(2, pcm, sizeof pcm, 8000, 1, 16, 0);
    source = voices[2].source;
    check(source->ratio == 1.0f);
    source->played = 404;
    recomp_audio_output_submit(2, pcm, sizeof pcm, 8000, 1, 16, 0);
    check(source->ratio < 1.0f);
    for (int i = 0; i < 4; ++i)
        recomp_audio_output_submit(2, larger, sizeof larger, 8000, 1, 16, 0);
    check(source->ratio > 1.0f && source->ratio <= 1.0f + kRateTrim);

    /* A voice further ahead than kMaxQueueMs drops instead of growing latency. */
    const auto ahead_dropped = dropped_buffers;
    uint8_t long_pcm[4000] = {};
    recomp_audio_output_submit(3, long_pcm, sizeof long_pcm, 8000, 1, 16, 0);
    recomp_audio_output_submit(3, long_pcm, sizeof long_pcm, 8000, 1, 16, 0);
    check(dropped_buffers == ahead_dropped + 1 && voices[3].source->queued.size() == 2);

    std::memset(pcm, 0x80, sizeof pcm);
    recomp_audio_output_submit(1, pcm, sizeof pcm, 8000, 1, 8, 0);
    check(nonzero_buffers == 1);
    check(voices[1].source->queued[0].AudioBytes == 400);
    for (uint32_t i = 0; i < 400; ++i)
        check(voices[1].source->queued[0].pAudioData[i] == 0x80);
    pcm[0] = 0;
    recomp_audio_output_submit(1, pcm, sizeof pcm, 8000, 1, 8, 0);
    check(nonzero_buffers == 2 && reported_nonzero[1]);
    source = voices[1].source;
    recomp_audio_output_submit(1, pcm, sizeof pcm, 16000, 2, 16, 0);
    check(source->destroyed && voices[1].sample_rate == 16000 &&
        voices[1].channels == 2 && voices[1].bits_per_sample == 16);

    auto submitted = submitted_buffers;
    const auto invalid_before = dropped_buffers;
    recomp_audio_output_submit(256, pcm, sizeof pcm, 8000, 1, 16, 0);
    recomp_audio_output_submit(0, nullptr, sizeof pcm, 8000, 1, 16, 0);
    recomp_audio_output_submit(0, pcm, 160001, 8000, 1, 16, 0);
    recomp_audio_output_submit(0, pcm, sizeof pcm, 999, 1, 16, 0);
    recomp_audio_output_submit(0, pcm, sizeof pcm, 200001, 1, 16, 0);
    recomp_audio_output_submit(0, pcm, sizeof pcm, 8000, 3, 16, 0);
    recomp_audio_output_submit(0, pcm, sizeof pcm, 8000, 1, 24, 0);
    recomp_audio_output_submit(0, pcm, 7, 8000, 2, 16, 0);
    check(submitted_buffers == submitted && dropped_buffers == invalid_before + 8);

    /* Float keeps headroom, signed-zero silence, and byte/frame accounting. */
    float float_pcm[] = {0.0f, -0.0f, 1.5f, -1.5f};
    const uint8_t *float_bytes = reinterpret_cast<const uint8_t *>(float_pcm);
    const auto nonzero_before = nonzero_buffers;
    const auto bytes_before = submitted_bytes;
    source = voices[1].source;
    recomp_audio_output_submit(1, float_bytes, sizeof float_pcm, 48000, 2, 32, -2000);
    check(source->destroyed);
    source = voices[1].source;
    check(source && source->format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT &&
        source->format.wBitsPerSample == 32 && source->format.nBlockAlign == 8 &&
        source->format.nSamplesPerSec == 48000 && source->format.nChannels == 2 &&
        source->queued.size() == 2 && source->queued[0].AudioBytes == 19200 &&
        source->queued[1].AudioBytes == sizeof float_pcm && source->ratio == 1.0f);
    for (uint32_t i = 0; i < 19200; ++i) check(source->queued[0].pAudioData[i] == 0);
    check(source->queued[1].pAudioData != float_bytes &&
        std::memcmp(source->queued[1].pAudioData, float_bytes, sizeof float_pcm) == 0 &&
        std::fabs(source->volume - 0.1f) < 0.00001f &&
        nonzero_buffers == nonzero_before + 1 &&
        submitted_bytes == bytes_before + sizeof float_pcm);
    float_pcm[2] = float_pcm[3] = -0.0f;
    source->played = 2402;
    recomp_audio_output_submit(1, float_bytes, sizeof float_pcm, 48000, 2, 32, 0);
    check(source->ratio < 1.0f && nonzero_buffers == nonzero_before + 1);
    const auto underruns_before = underruns;
    source->played = voices[1].bytes_submitted / 8;
    source->queued.clear();
    recomp_audio_output_submit(1, float_bytes, sizeof float_pcm, 48000, 2, 32, 0);
    check(underruns == underruns_before + 1 && source->queued.size() == 2 &&
        source->queued[0].AudioBytes == 19200 && source->queued[1].AudioBytes == sizeof float_pcm);
    while (voices[1].queued < kQueueSize)
        recomp_audio_output_submit(1, float_bytes, sizeof float_pcm, 48000, 2, 32, 0);
    submitted = submitted_buffers;
    const auto dropped = dropped_buffers;
    recomp_audio_output_submit(1, float_bytes, sizeof float_pcm, 48000, 2, 32, 0);
    recomp_audio_output_submit(1, float_bytes, sizeof float_pcm - 4, 48000, 2, 32, 0);
    check(submitted_buffers == submitted && dropped_buffers == dropped + 2 &&
        source->queued.size() == kQueueSize && nonzero_buffers == nonzero_before + 1);

    fake_engine.callback->OnCriticalError(XAUDIO2_E_DEVICE_INVALIDATED);
    check(engine && !allocations.empty() && !recomp_audio_output_enabled() &&
        !recomp_audio_output_ready());
    /* Another thread stops submitting but leaves COM teardown to the owner. */
    std::thread([&] { recomp_audio_output_submit(1, pcm, sizeof pcm, 8000, 1, 8, 0); }).join();
    check(engine && release_calls == 2 && com_balance == 1 &&
        submitted_buffers == submitted && !recomp_audio_output_enabled() &&
        !recomp_audio_output_ready());
    recomp_audio_output_submit(1, pcm, sizeof pcm, 8000, 1, 8, 0);
    check(!engine && allocations.empty() && submitted_buffers == submitted &&
        release_calls == 3 && com_balance == 0);
    recomp_audio_output_initialize();
    check(create_calls == 4);
    recomp_audio_output_shutdown();
    recomp_audio_output_shutdown();
    check(release_calls == 3 && !recomp_audio_output_ready());
    check(!recomp_audio_output_set_reverb(&reverb_parameters) &&
        !recomp_audio_output_set_voice_routing(0, 1, mono_dry, mono_wet));

    /* Separate process lifecycle for native FX and routing contracts. */
    attempted = summary_printed = false;
    critical_error = S_OK;
    fake_gain = "1";
    check(recomp_audio_output_set_voice_routing(10, 1, mono_dry, silent));
    recomp_audio_output_submit(10, pcm, sizeof pcm, 8000, 1, 16, 0);
    source = voices[10].source;
    check(source && source->send_calls == 0 && source->matrix_calls == 1 &&
        source->matrices[master] == std::vector<float>({1.0f, 0.0f}));
    check(!recomp_audio_output_set_voice_routing(10, 1, mono_dry, mono_wet) &&
        recomp_audio_output_ready());
    check(recomp_audio_output_set_reverb(&reverb_parameters));
    check(reverb_voice && effect_balance == 1 && effect_create_calls == 1 &&
        effect_parameter_calls == 1 && reverb_updates == 1 &&
        fake_engine.submix.parameters.RoomFilterMain == -10.0f &&
        fake_engine.submix.parameters.RoomFilterHF == -1.0f &&
        fake_engine.submix.parameters.RoomFilterFreq == 5000.0f &&
        fake_engine.submix.parameters.DecayTime == 1.49f &&
        fake_engine.submix.parameters.ReflectionsDelay == 7 &&
        fake_engine.submix.parameters.ReverbDelay == 11);
    check(recomp_audio_output_set_voice_routing(10, 1, mono_dry, mono_wet));
    check(source->send_calls == 1 && source->matrix_calls == 3 &&
        source->sends == std::vector<void *>({master, reverb_voice}) &&
        source->matrices[reverb_voice] == std::vector<float>({0.25f, 0.5f}));
    check(recomp_audio_output_set_voice_routing(10, 1, mono_dry, mono_wet));
    check(source->send_calls == 1 && source->matrix_calls == 3);
    check(recomp_audio_output_set_voice_routing(10, 1, silent, mono_wet));
    check(source->send_calls == 1 && source->matrix_calls == 5 &&
        source->matrices[master] == std::vector<float>({0.0f, 0.0f}));
    std::thread([&] {
        check(!recomp_audio_output_set_reverb(&reverb_parameters) &&
            !recomp_audio_output_set_voice_routing(10, 1, mono_dry, mono_wet));
        recomp_audio_output_reset_voice(10);
    }).join();
    check(source->matrix_calls == 5 && effect_parameter_calls == 1 && !source->destroyed);
    RecompAudioReverb invalid = reverb_parameters;
    invalid.decay_hf_ratio = NAN;
    const float invalid_matrix[] = {NAN, 0.0f};
    check(!recomp_audio_output_set_reverb(nullptr) &&
        !recomp_audio_output_set_reverb(&invalid) &&
        !recomp_audio_output_set_voice_routing(10, 1, invalid_matrix, mono_wet) &&
        !recomp_audio_output_set_voice_routing(10, 2, silent, silent) &&
        !recomp_audio_output_set_voice_routing(256, 1, mono_dry, silent) &&
        recomp_audio_output_ready() && effect_parameter_calls == 1);
    invalid = reverb_parameters;
    invalid.reflections = 1001;
    check(!recomp_audio_output_set_reverb(&invalid) && recomp_audio_output_ready());

    const float stereo_dry[] = {1.0f, 0.0f, 0.0f, 1.0f};
    const float stereo_wet[] = {0.3f, 0.0f, 0.0f, 0.7f};
    check(recomp_audio_output_set_voice_routing(11, 2, stereo_dry, stereo_wet));
    recomp_audio_output_submit(11, pcm, sizeof pcm, 48000, 2, 16, 0);
    source = voices[11].source;
    check(source && source->matrices[master] == std::vector<float>(stereo_dry, stereo_dry + 4) &&
        source->matrices[reverb_voice] == std::vector<float>(stereo_wet, stereo_wet + 4));
    recomp_audio_output_submit(11, pcm, sizeof pcm, 16000, 2, 16, 0);
    check(source->destroyed && voices[11].source && routing[11].channels == 2 &&
        voices[11].source->matrices[reverb_voice] == std::vector<float>(stereo_wet, stereo_wet + 4));
    recomp_audio_output_reset_voice(11);
    check(!routing[11].channels);
    const auto routed_submissions = submitted_buffers;
    recomp_audio_output_submit(10, pcm, sizeof pcm, 48000, 2, 16, 0);
    check(submitted_buffers == routed_submissions && recomp_audio_output_ready());
    check(recomp_audio_output_set_reverb(&reverb_parameters) && effect_create_calls == 1 &&
        effect_parameter_calls == 2);
    recomp_audio_output_shutdown();
    check(!reverb_voice && effect_balance == 0 && !engine && allocations.empty());

    /* Creation, parameter and routing failures never advertise a healthy host. */
    bool *failures[] = {&reverb_unavailable, &submix_unavailable,
        &parameters_unavailable, &sends_unavailable, &matrix_unavailable};
    for (bool *failure : failures) {
        attempted = summary_printed = false;
        critical_error = S_OK;
        *failure = true;
        if (failure == &sends_unavailable || failure == &matrix_unavailable) {
            check(recomp_audio_output_set_reverb(&reverb_parameters));
            recomp_audio_output_submit(20, pcm, sizeof pcm, 8000, 1, 16, 0);
            check(!recomp_audio_output_set_voice_routing(20, 1, mono_dry, mono_wet));
        } else {
            check(!recomp_audio_output_set_reverb(&reverb_parameters));
        }
        check(!recomp_audio_output_ready() && !engine && !reverb_voice &&
            effect_balance == 0 && allocations.empty() && com_balance == 0);
        recomp_audio_output_shutdown();
        *failure = false;
    }

    /* Five point one: the master grows to six channels, source dry matrices
       and the stereo reverb return are explicit, and unrouted voices stay on
       front left/right. */
    attempted = summary_printed = false;
    critical_error = S_OK;
    fake_surround = "1";
    check(recomp_audio_output_speakers() == 6);
    const float surround_dry[2 * 6] = {0.5f, 0.0f, 0.0f, 0.5f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    const float surround_wet[2 * 2] = {0.25f, 0.0f, 0.0f, 0.25f};
    check(recomp_audio_output_set_reverb(&reverb_parameters));
    check(fake_engine.master.channels == 6 && recomp_audio_output_enabled());
    check(fake_engine.submix.matrices[master] == std::vector<float>(
        {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}));
    check(recomp_audio_output_set_voice_routing(30, 2, surround_dry, surround_wet));
    recomp_audio_output_submit(30, pcm, sizeof pcm, 8000, 2, 16, 0);
    source = voices[30].source;
    check(source && source->matrices[master] ==
        std::vector<float>(surround_dry, surround_dry + 12));
    recomp_audio_output_submit(31, pcm, sizeof pcm, 8000, 2, 16, 0);
    check(voices[31].source->matrices[master] == std::vector<float>(
        {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}));
    recomp_audio_output_shutdown();
    check(!engine && !reverb_voice && effect_balance == 0 && allocations.empty());
    fake_surround = "0";
    check(recomp_audio_output_speakers() == 2);
    fake_surround = nullptr;
    check(recomp_audio_output_speakers() == 2);

    /* The reverb return's canceller XAPO: an impulse in the left ear reaches
       the right ear 11 frames later, then decays by the pole. */
    IXAPO *xtalk = static_cast<IXAPO *>(recomp_audio_xtalk_xapo_create());
    check(xtalk != nullptr);
    WAVEFORMATEX stereo_float{};
    stereo_float.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    stereo_float.nChannels = 2;
    stereo_float.nSamplesPerSec = 48000;
    stereo_float.wBitsPerSample = 32;
    stereo_float.nBlockAlign = 8;
    stereo_float.nAvgBytesPerSec = 384000;
    WAVEFORMATEX mono_16 = stereo_float;
    mono_16.nChannels = 1;
    mono_16.wBitsPerSample = 16;
    mono_16.nBlockAlign = 2;
    mono_16.nAvgBytesPerSec = 96000;
    check(xtalk->IsInputFormatSupported(&stereo_float, &stereo_float, nullptr) == S_OK);
    check(FAILED(xtalk->IsOutputFormatSupported(&stereo_float, &mono_16, nullptr)));
    XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS locked{&stereo_float, 32};
    check(SUCCEEDED(xtalk->LockForProcess(1, &locked, 1, &locked)));
    float impulse[64] = {};
    impulse[0] = 1.0f;
    XAPO_PROCESS_BUFFER_PARAMETERS input{impulse, XAPO_BUFFER_VALID, 32};
    XAPO_PROCESS_BUFFER_PARAMETERS output{impulse, XAPO_BUFFER_VALID, 32};
    xtalk->Process(1, &input, 1, &output, TRUE);
    const float pole = (float)0x7645a1 / 0x800000;
    const float feed = (float)(0xf88313 - 0x1000000) / 0x800000;
    check(output.ValidFrameCount == 32 && impulse[0] == 1.0f && impulse[1] == 0.0f &&
        std::fabs(impulse[2 * 11 + 1] - feed) < 0.000001f &&
        std::fabs(impulse[2 * 12 + 1] - pole * feed) < 0.000001f);
    /* Silent input still advances the canceller tail. */
    float tail[2] = {};
    XAPO_PROCESS_BUFFER_PARAMETERS silent_input{nullptr, XAPO_BUFFER_SILENT, 1};
    XAPO_PROCESS_BUFFER_PARAMETERS silent_output{tail, XAPO_BUFFER_SILENT, 1};
    xtalk->Process(1, &silent_input, 1, &silent_output, TRUE);
    check(silent_output.BufferFlags == XAPO_BUFFER_VALID &&
        std::fabs(tail[1] - feed * std::pow(pole, 21.0f)) < 0.000001f);
    xtalk->UnlockForProcess();
    xtalk->Release();

    for (auto *voice : fake_sources) delete voice;
    std::puts("PASS audio output ownership, FIFO, overflow, volume, native FX routing and failures");
    return 0;
}
