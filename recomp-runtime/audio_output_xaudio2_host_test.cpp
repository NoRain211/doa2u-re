/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "audio_capture_xapo.cpp"
#include "audio_output_xaudio2.cpp"

static bool check(const char *label, bool condition)
{
    if (!condition) std::fprintf(stderr, "FAIL native audio %s\n", label);
    return condition;
}

/* Master gain precedes its effect chain, so capture then silence the signal. */
class SilentCapture : public Capture {
public:
    void STDMETHODCALLTYPE Process(UINT32 inputs,
        const XAPO_PROCESS_BUFFER_PARAMETERS *input, UINT32 outputs,
        XAPO_PROCESS_BUFFER_PARAMETERS *output, BOOL enabled) override
    {
        Capture::Process(inputs, input, outputs, output, enabled);
        if (output[0].pBuffer)
            std::memset(output[0].pBuffer, 0, size_t(output[0].ValidFrameCount) * channels * sizeof(float));
        output[0].BufferFlags = XAPO_BUFFER_SILENT;
    }
};

static bool checkReverbCapture(Capture *recording, uint32_t channels, bool wet)
{
    engine->StopEngine();
    recording->used = 0;
    recording->first_tick = 0;
    float dry_matrix[] = {0.0f, 0.0f, 0.0f, 0.0f};
    float wet_matrix[] = {0.0f, 0.0f, 0.0f, 0.0f};
    float *matrix = wet ? wet_matrix : dry_matrix;
    matrix[0] = 1.0f;
    matrix[channels == 1 ? 1 : 3] = 1.0f;
    bool passed = check("route", recomp_audio_output_set_voice_routing(
        1, channels, dry_matrix, wet_matrix) != 0);
    if (!passed || !check("start capture", SUCCEEDED(engine->StartEngine()))) return false;
    float pcm[960]{};
    pcm[0] = 0.75f;
    if (channels == 2) pcm[1] = -0.5f;
    recomp_audio_output_submit(1, reinterpret_cast<const uint8_t *>(pcm),
        480 * channels * sizeof(float), 48000, channels, 32, 0);
    if (!check("capture submission", recomp_audio_output_enabled() != 0)) return false;
    const ULONGLONG deadline = GetTickCount64() + 500;
    while (passed && GetTickCount64() < deadline) {
        Sleep(5);
        passed = check("capture device", recomp_audio_output_enabled() != 0);
    }
    engine->StopEngine();
    if (!passed) return false;
    double early_energy = 0.0, tail_energy = 0.0;
    bool finite = true;
    for (size_t i = 0; i < recording->used; ++i) {
        const float value = recording->samples[i];
        finite &= std::isfinite(value);
        const size_t frame = i / recording->channels;
        if (frame < recording->rate * 150 / 1000) early_energy += double(value) * value;
        else tail_energy += double(value) * value;
    }
    std::printf("native FX capture channels=%u wet=%u frames=%zu early_energy=%.9g tail_energy=%.9g\n",
        channels, wet ? 1u : 0u, recording->used / recording->channels,
        early_energy, tail_energy);
    passed &= check("capture format", recording->rate == 48000 && recording->channels == 2 &&
        recording->used >= recording->rate * recording->channels * 300 / 1000);
    passed &= check("finite capture", finite);
    passed &= check("impulse response", early_energy > 1e-7);
    passed &= check("reverb tail", wet ? tail_energy > 1e-7 : tail_energy < 1e-12);
    recomp_audio_output_reset_voice(1);
    return passed;
}

int main()
{
    if (!check("environment", _putenv_s("RECOMP_AUDIO_GAIN", "1") == 0 &&
            _putenv_s("RECOMP_AUDIO_CAPTURE", "") == 0 &&
            _putenv_s("RECOMP_AUDIO_SURROUND", "0") == 0)) return 1;
    recomp_audio_output_initialize();
    bool passed = check("endpoint", recomp_audio_output_enabled() && recomp_audio_output_ready());
    if (passed) passed = check("master mute", SUCCEEDED(master->SetVolume(0.0f)));
    if (passed) {
        int16_t pcm[800];
        for (uint32_t i = 0; i < 800; ++i) pcm[i] = (i % 40u < 20u) ? 1000 : -1000;
        recomp_audio_output_submit(0, reinterpret_cast<const uint8_t *>(pcm),
            sizeof pcm, 8000, 1, 16, 0);
        passed = check("submission", recomp_audio_output_enabled() && voices[0].source &&
            submitted_buffers == 1 && submitted_bytes == sizeof pcm && nonzero_buffers == 1);
        std::memset(pcm, 0, sizeof pcm);
        XAUDIO2_VOICE_STATE state{};
        const ULONGLONG deadline = GetTickCount64() + 2000;
        while (passed && GetTickCount64() < deadline) {
            recomp_audio_output_initialize();
            passed = check("device during consumption", recomp_audio_output_enabled() != 0);
            if (!passed) break;
            voices[0].source->GetState(&state, 0);
            if (state.BuffersQueued == 0) break;
            Sleep(5);
        }
        passed &= check("consumed PCM", state.BuffersQueued == 0 &&
            state.SamplesPlayed >= 800 + 8000 * kPrerollMs / 1000);
        recomp_audio_output_reset_voice(0);
        passed &= check("reset", voices[0].source == nullptr);
    }
    Capture *recording = nullptr;
    if (passed) {
        recording = new SilentCapture;
        XAUDIO2_EFFECT_DESCRIPTOR descriptor{recording, TRUE, 2};
        XAUDIO2_EFFECT_CHAIN chain{1, &descriptor};
        passed = check("capture tap", SUCCEEDED(master->SetEffectChain(&chain)));
        if (passed) passed = check("gain before silent capture", SUCCEEDED(master->SetVolume(1.0f)));
    }
    if (passed) {
        const RecompAudioReverb parameters{-1000, -100, 0.0f, 1.49f, 0.83f,
            -2602, 0.007f, 200, 0.011f, 100.0f, 100.0f, 5000.0f};
        passed = check("native reverb", recomp_audio_output_set_reverb(&parameters) != 0);
        if (passed) passed = checkReverbCapture(recording, 1, false);
        if (passed) passed = checkReverbCapture(recording, 1, true);
        if (passed) {
            reverb_voice->DestroyVoice();
            reverb_voice = nullptr;
            passed = check("fresh stereo reverb", recomp_audio_output_set_reverb(&parameters) != 0);
            if (passed) passed = checkReverbCapture(recording, 2, true);
        }
    }
    recomp_audio_output_shutdown();
    if (recording) recording->Release();
    recomp_audio_output_shutdown();
    passed &= check("shutdown", !recomp_audio_output_enabled() && !recomp_audio_output_ready() && !master && !com_initialized);
    if (passed) std::puts("PASS real XAudio2 endpoint, copied PCM, dry impulse and mono/stereo native reverb tails");
    return passed ? 0 : 1;
}
