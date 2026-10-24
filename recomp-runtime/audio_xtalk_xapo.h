#ifndef RECOMP_AUDIO_XTALK_XAPO_H
#define RECOMP_AUDIO_XTALK_XAPO_H

struct IUnknown;

#ifdef __cplusplus
extern "C" {
#endif

/* Reverb-return crosstalk canceller (DOA2 xtalkamp.bin effect 14) as an
   in-place XAudio2 effect: stereo float32 at 48 kHz only. The caller owns the
   reference. Returns null when allocation fails. */
IUnknown *recomp_audio_xtalk_xapo_create(void);

#ifdef __cplusplus
}
#endif

#endif
