/**
 * @file sr_chords.h
 * Back-chord state machine for the screen reader's input filter. Pure C, host tested: it sees
 * every input event before it is published and decides which are swallowed, which Back Long
 * is delivered on release, and which command a chord names. No timing of its own beyond the
 * 300 ms Long threshold, which the input service applies before the Long event arrives.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __has_include
#if __has_include(<input/input.h>)
#include <input/input.h>
#define SR_CHORDS_HAVE_INPUT 1
#endif
#endif
#ifndef SR_CHORDS_HAVE_INPUT
typedef enum {
    InputKeyUp,
    InputKeyDown,
    InputKeyRight,
    InputKeyLeft,
    InputKeyOk,
    InputKeyBack,
    InputKeyMAX
} InputKey;
typedef enum {
    InputTypePress,
    InputTypeRelease,
    InputTypeShort,
    InputTypeLong,
    InputTypeRepeat,
    InputTypeMAX
} InputType;
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SR_CHORD_LONG_MS 300

typedef enum {
    SrChordNone,
    SrChordReadAll, /**< Back + Up: the whole screen */
    SrChordStatus, /**< Back + Down: the status rows */
    SrChordRepeat, /**< Back + OK short: the focus with its position */
    SrChordSpell, /**< Back + OK long: the focus spelled */
    SrChordVolumeDown, /**< Back + Left */
    SrChordVolumeUp, /**< Back + Right */
} SrChordCommand;

typedef struct {
    bool back_down;
    bool chord_used; /**< a chord ran during this Back hold */
    bool long_pending; /**< Back's Long arrived and waits for the release */
    uint32_t back_down_ms;
    int8_t chord_key; /**< key whose events are swallowed until its release, -1 for none */
    bool ok_long_seen; /**< OK Long ran Spell, so its Short must not run Repeat */
} SrChords;

void sr_chords_init(SrChords* s);

/** Feed one event. Returns the command to run (SrChordNone for none) and writes whether the
 *  event is dropped and whether a Long must be emitted for it. */
SrChordCommand sr_chords_feed(
    SrChords* s,
    InputKey key,
    InputType type,
    uint32_t now_ms,
    bool* drop,
    bool* emit_long);

#ifdef __cplusplus
}
#endif
