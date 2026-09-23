/**
 * @file speech_pcm.h
 * Turns SAM's five value groups into PWM duty bytes, one per 20 microsecond carrier period,
 * holding each value as long as the Text to SAM demo did. Pure C, host tested.
 */
#pragma once

#include <stdbool.h>
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
    uint32_t overhead_ns; /**< added to every value's hold time; calibrated against the demo */
    uint64_t now_ns;
    uint64_t next_slot_ns;
    uint32_t subsamples; /**< values received since reset */
    uint32_t nominal_us; /**< sum of delta / 8 microseconds since reset, without overhead */
    uint32_t slots; /**< duty bytes emitted since reset */
} SpeechPcm;

void speech_pcm_init(SpeechPcm* pcm, uint8_t volume_percent, uint32_t overhead_ns);
void speech_pcm_set_volume(SpeechPcm* pcm, uint8_t volume_percent);

/** Start of an utterance: timers and counters back to zero. */
void speech_pcm_reset(SpeechPcm* pcm);

/** The duty byte that keeps the speaker quiet between utterances. */
uint8_t speech_pcm_silence(const SpeechPcm* pcm);

/** Feed five values held for delta / 8 microseconds each plus the overhead. Emits one duty byte
 *  per slot boundary crossed. Returns false when emit asked to stop. */
bool speech_pcm_push(
    SpeechPcm* pcm,
    const uint8_t values[5],
    uint16_t delta,
    SpeechPcmEmit emit,
    void* context);

#ifdef __cplusplus
}
#endif
