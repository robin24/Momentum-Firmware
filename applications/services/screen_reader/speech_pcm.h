/**
 * @file speech_pcm.h
 * Turns the recorded voice's 8 bit samples into PWM duty bytes, one per 20 microsecond carrier
 * period, each sample held for its time at the clip's rate. Pure C, host tested.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** TIM16 update period: 64 MHz / 5 / 256 = 50 kHz */
#define SPEECH_SLOT_NS       20000u
#define SPEECH_SILENCE_VALUE 128u

typedef bool (*SpeechPcmEmit)(uint8_t duty, void* context);

typedef struct {
    uint8_t lut[256]; /**< loudness curve with the volume folded in */
    uint64_t now_ns;
    uint64_t next_slot_ns;
    uint32_t subsamples; /**< samples entered since reset */
    uint32_t nominal_us; /**< time of the samples completed since reset, in microseconds */
    uint32_t slots; /**< duty bytes emitted since reset */
} SpeechPcm;

void speech_pcm_init(SpeechPcm* pcm, uint8_t volume_percent);
void speech_pcm_set_volume(SpeechPcm* pcm, uint8_t volume_percent);

/** Start of an utterance: timers and counters back to zero. */
void speech_pcm_reset(SpeechPcm* pcm);

/** The duty byte that keeps the speaker quiet between utterances. */
uint8_t speech_pcm_silence(const SpeechPcm* pcm);

/** Feed `count` recorded samples, each held for exactly hold_ns, through the slot accumulator
 *  and the loudness table: one duty byte per slot boundary crossed. subsamples grows by every
 *  sample entered and nominal_us by the exact time of the samples completed, with a sub
 *  microsecond remainder carried from call to call. Returns false when emit asked to stop; the
 *  stage then sits inside the refused sample, so call speech_pcm_reset before pushing again. */
bool speech_pcm_push_raw(
    SpeechPcm* pcm,
    const uint8_t* samples,
    size_t count,
    uint32_t hold_ns,
    SpeechPcmEmit emit,
    void* context);

#ifdef __cplusplus
}
#endif
