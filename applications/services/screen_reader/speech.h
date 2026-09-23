/**
 * @file speech.h
 * Speech engine: a worker thread that renders queued utterances with SAM and plays them
 * through the speaker with DMA. Announcements come from the screen reader service; the console
 * command "sr say" uses it too.
 */
#pragma once

#include <furi.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Speech Speech;

typedef struct {
    uint32_t utterances; /**< started */
    uint32_t dropped; /**< could not get the speaker */
    uint32_t aborted; /**< interrupted by a newer utterance, a stop, or a key press */
    uint32_t underruns; /**< DMA reached a half before it was filled */
    uint32_t queue_dropped; /**< pushed out of a full queue */
    uint32_t queued; /**< items waiting */
    bool speaking; /**< true from dequeue until the utterance has been played out */
    bool speaker_held;
    uint32_t last_subsamples;
    uint32_t last_nominal_ms;
    uint32_t last_played_ms;
    uint32_t stack_free; /**< worker thread stack watermark, bytes */
} SpeechStats;

/** Allocates everything once and starts the worker. */
Speech* speech_alloc(void);

/** Queue text. interrupt: stop what is being said and drop what waits. replaceable: a pending
 *  replaceable item is overwritten instead of queued behind it. */
void speech_say(Speech* speech, const char* text, bool interrupt, bool replaceable);

/** Stop speaking and drop the queue. Safe from any thread, including input callbacks. */
void speech_stop(Speech* speech);

/** rate is the SAM speed (40..120), volume 0..100. Applied from the next utterance on. */
void speech_set_voice(Speech* speech, uint8_t rate, uint8_t volume);

/** True while something is queued or being spoken. */
bool speech_is_busy(Speech* speech);

void speech_get_stats(Speech* speech, SpeechStats* out);

#ifdef __cplusplus
}
#endif
