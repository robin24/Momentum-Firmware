/**
 * @file speech.h
 * Speech engine: a worker thread that plays queued utterances from the recorded voice's clips on
 * the card, through the speaker with DMA. Announcements come from the screen reader service; the
 * console command "sr say" uses it too.
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
    uint32_t aborted; /**< ended early by a newer utterance, a stop or a key press, also while
                           waiting for the speaker or playing out the end */
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

/** Say a text of any length, as a whole screen read on request: what is being said stops, what
 *  waits is dropped, and the text is queued at once in parts of up to 159 characters cut after
 *  a ". " or ", ", up to four parts (about 600 characters); see speech_queue_push_parts. */
void speech_say_parts(Speech* speech, const char* text);

/** Queue text to be spelled: what is being said stops and what waits is dropped, as with an
 *  interrupting speech_say. Every word is spelled with the letter and digit clips, 60 ms between
 *  letters and 150 ms between words. With the voice off, or without a vocabulary on the card,
 *  nothing is played. A text without letters or digits is said instead (an underscore key). */
void speech_say_spelled(Speech* speech, const char* text);

/** Play a file of raw unsigned 8 bit mono samples (128 is silence) from the card in place of
 *  speech: what is being said stops, what waits is dropped, and the file plays through the
 *  same ring, speaker and stop rules. rate is in samples per second, 8000 to 32000 (values
 *  outside are clamped); each sample is held for 1000000000 / rate nanoseconds. */
void speech_play(Speech* speech, const char* path, uint32_t rate);

/** Stop speaking and drop what is queued so far; anything pushed afterwards is kept. Takes the
 *  speech mutex briefly, so callers are threads (input callbacks run on the input service
 *  thread), never interrupts. */
void speech_stop(Speech* speech);

/** volume 0..100, applied from the next utterance on. */
void speech_set_volume(Speech* speech, uint8_t volume);

typedef struct {
    bool enabled; /**< recorded clips are used when the vocabulary is present;
                       false means muted (sr voice off) */
    bool vocabulary; /**< /ext/sr/voice was found on the card */
    uint32_t clip_words; /**< words and spelled letters spoken from clips since boot */
    uint32_t fallback_words; /**< words the vocabulary lacked, spelled, since boot */
    uint32_t missing_words; /**< distinct words recorded in /ext/sr/missing.txt since boot */
    uint32_t muted; /**< items completed silently since boot: the voice off, or no vocabulary */
    uint32_t open_max_ms; /**< longest clip open since boot, found or not */
    uint32_t open_last_ms; /**< the latest clip open */
    char settings[64]; /**< first line of /ext/sr/voice/voice.txt, empty when absent */
} SpeechVoiceStats;

/** Recorded voice on or off, from the next utterance on. Off mutes: a text item completes at
 *  once, no file is opened, nothing is played, until on again or a reboot; it is the guard the
 *  generator sets around a sync. */
void speech_set_voice_clips(Speech* speech, bool enabled);

void speech_get_voice_stats(Speech* speech, SpeechVoiceStats* out);

/** True while something is queued or being spoken. */
bool speech_is_busy(Speech* speech);

void speech_get_stats(Speech* speech, SpeechStats* out);

#ifdef __cplusplus
}
#endif
