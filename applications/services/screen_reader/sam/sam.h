/**
 * @file sam.h
 * SAM (Software Automatic Mouth) speech synthesizer: plain C port of the Text to SAM app's
 * STM32SAM class. Renders text into 8 bit values handed to a callback five at a time, each
 * group with its duration in SAM time units. No hardware access, no heap. Not reentrant:
 * one caller at a time.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Longest text sam_speak accepts, in characters. */
#define SAM_MAX_INPUT 250

typedef struct {
    uint8_t pitch; /**< 64 by default; smaller values give a higher voice */
    uint8_t speed; /**< 72 by default; bigger values speak slower */
    uint8_t mouth; /**< 128 by default */
    uint8_t throat; /**< 128 by default */
} SamVoice;

#define SAM_VOICE_DEFAULT ((SamVoice){.pitch = 64, .speed = 72, .mouth = 128, .throat = 128})

/**
 * Receives five consecutive output values (0..255, 128 is the mid level) that together last
 * `delta` SAM time units. The Text to SAM app held each of the five values for delta / 8
 * microseconds. Return false to abort rendering.
 */
typedef bool (*SamOutputCallback)(const uint8_t values[5], uint16_t delta, void* context);

/**
 * Render text. Returns true when the text was rendered, up to `consumed`; false when the text was
 * empty, longer than SAM_MAX_INPUT, could not be converted to phonemes, or the callback aborted.
 * `consumed` (may be NULL) receives the number of characters of `text` that were rendered: the
 * whole length normally, less when the reciter ran out of room at a word boundary (its phoneme
 * string holds 120 characters); call again with `text + consumed` for the rest.
 */
bool sam_speak(
    const char* text,
    const SamVoice* voice,
    SamOutputCallback callback,
    void* context,
    size_t* consumed);

#ifdef __cplusplus
}
#endif
