/**
 * @file speech_voice.h
 * Recorded voice: maps the words of an expanded announcement to clip files on the card and
 * the pauses between them, and remembers which words the vocabulary lacked. Pure C, host
 * tested; the worker in speech.c does the file and sample work.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPEECH_VOICE_DIR         "/ext/sr/voice"
#define SPEECH_VOICE_WORD_MAX    32 /**< clip name with its terminator */
#define SPEECH_VOICE_PATH_MAX    64 /**< "/ext/sr/voice/x/" + name + ".raw" */
#define SPEECH_VOICE_MISSING_SET 64 /**< words remembered as missing, once per boot each */
#define SPEECH_VOICE_LOG_MAX     8 /**< missing words kept for the log per utterance */
#define SPEECH_VOICE_HZ          16000u
#define SPEECH_VOICE_GAP_MS      40
#define SPEECH_VOICE_COMMA_MS    150
#define SPEECH_VOICE_STOP_MS     300

typedef struct {
    char word[SPEECH_VOICE_WORD_MAX]; /**< lower case clip name, punctuation stripped; empty
                                            when the token was punctuation only */
    uint16_t pause_ms; /**< silence after the word */
} SpeechVoiceWord;

/** The next word of `text` from *pos; false at the end. A token is a run of non-space
 *  characters; its last character decides the pause (period, question and exclamation mark:
 *  stop; comma, semicolon, colon: comma; anything else: gap), then punctuation is stripped
 *  from both ends and the rest lower cased and cut at SPEECH_VOICE_WORD_MAX - 1 characters. */
bool speech_voice_next_word(const char* text, size_t* pos, SpeechVoiceWord* out);

/** The clip path of a word: SPEECH_VOICE_DIR "/<initial>/<word>.raw", the initial being the
 *  word's first character when it is a lower case letter or a digit, '_' otherwise. Returns
 *  the length written, 0 for an empty word or a buffer too small. */
size_t speech_voice_path(const char* word, char* out, size_t out_size);

typedef struct {
    uint32_t hashes[SPEECH_VOICE_MISSING_SET]; /**< ring of words already recorded */
    uint8_t next;
    uint8_t count;
    char log[SPEECH_VOICE_LOG_MAX][SPEECH_VOICE_WORD_MAX]; /**< waiting for the log file */
    uint8_t log_count;
    uint32_t clip_words; /**< words spoken from a clip since boot */
    uint32_t fallback_words; /**< words spoken by SAM since boot */
    uint32_t missing_words; /**< distinct words recorded as missing since boot */
} SpeechVoiceState;

void speech_voice_state_init(SpeechVoiceState* state);

/** Record a missing word. True when it is new this boot and there was room in the log buffer
 *  (the caller writes the buffer to the card after the utterance); false when it was recorded
 *  before, is empty, or the buffer is full, in which case it is not remembered either, so a
 *  later utterance records it. */
bool speech_voice_missing(SpeechVoiceState* state, const char* word);

size_t speech_voice_log_count(const SpeechVoiceState* state);
const char* speech_voice_log_word(const SpeechVoiceState* state, size_t index);
void speech_voice_log_clear(SpeechVoiceState* state);

#ifdef __cplusplus
}
#endif
