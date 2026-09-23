/**
 * @file speech_text.h
 * Rewrites announcement text into words SAM pronounces well, and splits it into chunks
 * short enough for one synthesizer call. Pure C, host tested.
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Five times an announcement: a screen of two digit pairs expands about 4.7 times. */
#define SPEECH_TEXT_MAX  800
#define SPEECH_CHUNK_MAX 80

/**
 * Rewrite `in` into `out`: numbers and acronyms become words or spelled letters, technical terms
 * their pronunciation, punctuation between words a pause or a word, and everything else a space.
 * The result is printable ASCII. Returns the length written; the text is cut when it does not
 * fit, and `out` is always terminated. A null `in` gives an empty string. `in` and `out` must
 * not overlap.
 */
size_t speech_text_expand(const char* in, char* out, size_t out_size);

/**
 * Copy the next chunk of `text` starting at *pos into `chunk` and advance *pos.
 * Chunks are at most SPEECH_CHUNK_MAX characters (less when `chunk_size` is smaller than
 * SPEECH_CHUNK_MAX + 1) and end at a sentence, a comma or a word boundary when possible.
 * Returns the chunk length, 0 when the text is exhausted or `chunk_size` is below 2.
 */
size_t speech_text_next_chunk(const char* text, size_t* pos, char* chunk, size_t chunk_size);

#ifdef __cplusplus
}
#endif
