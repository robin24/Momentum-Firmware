/**
 * @file sr_chords.h
 * Back-chord state machine for the screen reader's input filter. Pure C, host tested: it sees
 * every input event before it is published and decides which are swallowed, which Back Long
 * is delivered on release, and which command a chord names. No timing of its own: the input
 * service applies the 300 ms Long threshold before the Long event arrives.
 *
 * It runs with the reader on and off. Off, the one chord is Back and Down held long, which turns
 * the reader on again, and every other key passes as an ordinary key. Back is handled alike in
 * both: its Short passes at once and its Long waits for the release, since a chord may still
 * come; a chord drops both. Down pressed while Back is held is held back in both, since only its
 * Short or its Long tells which chord it is; with the reader off a short one is lost.
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

typedef enum {
    SrChordNone,
    SrChordReadAll, /**< Back + Up: the whole screen */
    SrChordStatus, /**< Back + Down short: the status rows */
    SrChordRepeat, /**< Back + OK short: the focus with its position */
    SrChordSpell, /**< Back + OK long: the focus spelled */
    SrChordVolumeDown, /**< Back + Left */
    SrChordVolumeUp, /**< Back + Right */
    SrChordToggleReader, /**< Back + Down long: the reader off, or on; also while it is off */
} SrChordCommand;

typedef struct {
    bool back_down;
    bool chord_used; /**< a chord ran during this Back hold */
    bool long_pending; /**< Back's Long arrived and waits for the release */
    int8_t chord_key; /**< key whose events are swallowed until its release, -1 for none */
    bool key_long_seen; /**< the chord key's Long ran its command (OK: Spell, Down: ToggleReader),
                             so its Short must not run the short one (Repeat, Status) */
} SrChords;

void sr_chords_init(SrChords* s);

/** Feed one event. reader_on tells whether the reader is on: while it is off, Back and Down held
 *  long is the only chord (see above). Returns the command to run (SrChordNone for none) and
 *  writes whether the event is dropped and whether a Long must be emitted for it. now_ms, the
 *  event's time, is reserved: the machine keeps no time of its own and ignores it. */
SrChordCommand sr_chords_feed(
    SrChords* s,
    bool reader_on,
    InputKey key,
    InputType type,
    uint32_t now_ms,
    bool* drop,
    bool* emit_long);

#ifdef __cplusplus
}
#endif
