/**
 * @file speech_text.h
 * Rewrites announcement text into the words the recorded voice has clips for, and copies the
 * characters of a text to be spelled. Pure C, host tested.
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Five times an announcement: a screen of two digit pairs expands about 4.7 times. */
#define SPEECH_TEXT_MAX 800

/** The longest run of letters and digits speech_text_spell_copy keeps together: the clip
 *  player takes words of up to SPEECH_VOICE_WORD_MAX - 1 characters, 31. */
#define SPEECH_TEXT_SPELL_RUN_MAX 31

/**
 * Rewrite `in` into `out`: numbers and acronyms become words or spelled letters, technical terms
 * their pronunciation, punctuation between words a pause or a word, and everything else a space.
 * The result is printable ASCII. Returns the length written; the text is cut when it does not
 * fit, after its last whole word, and `out` is always terminated. A null `in` gives an empty string. `in` and `out` must
 * not overlap.
 */
size_t speech_text_expand(const char* in, char* out, size_t out_size);

/**
 * The text of an item to be spelled, its own characters rather than the expansion, which would
 * turn digits into number words and terms into their pronunciation. ASCII letters and digits are
 * kept as they are, in runs; every other character separates runs and is left out. Runs are
 * joined by single spaces, and a run longer than SPEECH_TEXT_SPELL_RUN_MAX gets a space after
 * every SPEECH_TEXT_SPELL_RUN_MAX characters. No space at either end: "Apps, 3 of 11" is
 * "Apps 3 of 11", "Sub-GHz" is "Sub GHz", and a text without letters or digits is empty.
 * Returns the length written; the text is cut when it does not fit, never after a space, and
 * `out` is always terminated when `out_size` is not 0. A null `in` gives an empty string.
 */
size_t speech_text_spell_copy(const char* in, char* out, size_t out_size);

#ifdef __cplusplus
}
#endif
