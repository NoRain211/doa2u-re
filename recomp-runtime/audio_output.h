#ifndef DOAXBV_RECOMP_AUDIO_OUTPUT_H
#define DOAXBV_RECOMP_AUDIO_OUTPUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 6 when RECOMP_AUDIO_SURROUND is set and not "0", otherwise 2. Reads the
   environment only, so it is valid before initialize and without a device.
   Keep this in step with the kernel's XC_AUDIO speaker setting. */
uint32_t recomp_audio_output_speakers(void);
/* Call on the runtime thread. Initialization is attempted once per process;
   an unavailable device or shutdown leaves guest audio timing unchanged.
   RECOMP_AUDIO_GAIN is the host master gain (0..1, default 1); zero stays muted.
   RECOMP_AUDIO_CAPTURE optionally records the mixed float WAV (first 60 s).
   Callers serialize all calls; the DirectSound adapter holds its clock lock. */
void recomp_audio_output_initialize(void);
void recomp_audio_output_shutdown(void);
void recomp_audio_output_reset_voice(uint32_t slot);
/* Nonzero while a device is open and healthy. */
int recomp_audio_output_enabled(void);
/* Nonzero for a healthy device or explicit RECOMP_AUDIO_GAIN=0 mute.
   Initialization/device failure, invalid gain and shutdown return zero. */
int recomp_audio_output_ready(void);
/* Atomic owner check; does not inspect device state. */
int recomp_audio_output_on_owner_thread(void);

/* I3DL2 listener parameters, in hundredths of dB, seconds and percent.
   Native reverb is a semantic replacement; Xbox DSP bit parity is unproven. */
typedef struct RecompAudioReverb {
    int32_t room, room_hf;
    float room_rolloff, decay_time, decay_hf_ratio;
    int32_t reflections;
    float reflections_delay;
    int32_t reverb;
    float reverb_delay, diffusion, density, hf_reference;
} RecompAudioReverb;

/* Owner-thread calls. Nonzero on success or explicit mute; invalid parameters,
   shutdown and backend failures return zero. A wet send requires set_reverb.
   Matrices use [source + destination * channels]. dry has channels * speakers
   entries; with 6 speakers the destinations are WAVE order FL, FR, C, LFE, BL,
   BR. wet stays stereo (channels * 2) and reaches the reverb submix. An
   unconfigured slot maps its source to FL/FR (the default dry send).
   Routing survives source format recreation; reset_voice clears it. */
int recomp_audio_output_set_reverb(const RecompAudioReverb *parameters);
int recomp_audio_output_set_voice_routing(uint32_t slot, uint32_t channels,
    const float *dry, const float *wet);

/* Copies PCM before returning; never retains a guest-memory pointer.
   Slots: 0..255. PCM: 1000..200000 Hz, 1-3 channels, unsigned 8 bit, signed
   16 bit, or 32-bit IEEE float (1.0 = full scale; values beyond +/-1 allowed).
   Each submission is frame-aligned and at most 160000 bytes. A full queue
   drops the incoming chunk without waiting. A new or starved voice first
   queues 50 ms of silence as a jitter cushion, then trims its pitch by at most
   1% to hold that cushion against the device's actual consumption rate. */
void recomp_audio_output_submit(
    uint32_t slot, const uint8_t *pcm, uint32_t bytes,
    uint32_t sample_rate, uint32_t channels, uint32_t bits_per_sample,
    int32_t volume_hundredth_db);

#ifdef __cplusplus
}
#endif

#endif
