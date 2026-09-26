/**
 * @file speech_voice.h
 * Recorded voice: maps the words of an expanded announcement to clip files on the card and
 * the pauses between them, names the letter and digit clips that spell a word the vocabulary
 * lacks, and remembers which words it lacked. Pure C, host tested; the worker in speech.c does
 * the file and sample work.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPEECH_VOICE_SETS_DIR "/ext/sr/voices" /**< one folder per voice set */
#define SPEECH_VOICE_SET_MAX  32 /**< set name with its terminator */
#define SPEECH_VOICE_PATH_MAX 96 /**< "/ext/sr/voices/" + set + "/hh/" + name + ".raw" */
#define SPEECH_VOICE_WORD_MAX    32 /**< clip name with its terminator */
#define SPEECH_VOICE_BUCKETS     64 /**< clip folders on the card, chosen by the word's hash */
#define SPEECH_VOICE_MISSING_SET 64 /**< words remembered as missing, once per boot each */
#define SPEECH_VOICE_LOG_MAX     8 /**< missing words kept for the log per utterance */
#define SPEECH_VOICE_HZ          16000u
#define SPEECH_VOICE_GAP_MS      40
#define SPEECH_VOICE_COMMA_MS    150
#define SPEECH_VOICE_STOP_MS     300

/** Silence between the letters of a word spelled with the letter clips. */
#define SPEECH_VOICE_LETTER_GAP_MS 60

typedef struct {
    char word[SPEECH_VOICE_WORD_MAX]; /**< lower case clip name, punctuation stripped; empty
                                            when the token was punctuation only */
    uint16_t pause_ms; /**< silence after the word */
} SpeechVoiceWord;

/** The next word of `text` from *pos; false at the end. A token is a run of non-space
 *  characters; its last character decides the pause (period, question and exclamation mark:
 *  stop; comma, semicolon, colon: comma; anything else: gap), then punctuation is stripped
 *  from both ends, apostrophes are left out ("don't" is the clip dont, which the generator
 *  makes from the spoken "don't"), and the rest is lower cased and cut at
 *  SPEECH_VOICE_WORD_MAX - 1 characters. */
bool speech_voice_next_word(const char* text, size_t* pos, SpeechVoiceWord* out);

/** FNV-1a, 32 bit, over the bytes of a word: the clip folder and the missing-word set. */
uint32_t speech_voice_hash(const char* word);

/** True for a set name the card and the settings can hold: 1 to SPEECH_VOICE_SET_MAX - 1
 *  characters, each a lower case letter, a digit, a hyphen or an underscore. */
bool speech_voice_set_valid(const char* name);

/** SPEECH_VOICE_SETS_DIR "/<set>/<file>", or the set's folder itself for an empty file name.
 *  Returns the length written, 0 for an empty set or a buffer too small. The set is not
 *  validated here: the callers pass names that came through speech_voice_set_valid or from
 *  the card's own folder listing. */
size_t speech_voice_set_file(const char* set, const char* file, char* out, size_t out_size);

/** The clip path of a word in a set: SPEECH_VOICE_SETS_DIR "/<set>/<hh>/<word>.raw", where
 *  hh is speech_voice_hash(word) modulo SPEECH_VOICE_BUCKETS as two lower case hex digits. A
 *  directory lookup on the card reads the entries one by one, so a vocabulary is spread over
 *  64 folders of a few dozen clips each. Returns the length written, 0 for an empty set or
 *  word, or a buffer too small. */
size_t speech_voice_path(const char* set, const char* word, char* out, size_t out_size);

/** The clip name that spells one character: "ay" for a, the letter itself for b to z, the
 *  digit itself for 0 to 9. False for anything else (the character is skipped). */
bool speech_voice_letter_clip(char c, char out[3]);

typedef struct {
    uint32_t hashes[SPEECH_VOICE_MISSING_SET]; /**< ring of words already recorded */
    uint8_t next;
    uint8_t count;
    char log[SPEECH_VOICE_LOG_MAX][SPEECH_VOICE_WORD_MAX]; /**< waiting for the log file */
    uint8_t log_count;
    uint32_t clip_words; /**< words and spelled letters spoken from a clip since boot */
    uint32_t fallback_words; /**< words spelled letter by letter since boot */
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
