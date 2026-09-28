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
 * come; a chord drops both, and Back's Repeats are dropped in both modes. Down pressed while
 * Back is held is held back in both, since only its
 * Short or its Long tells which chord it is; with the reader off a short one is lost. With the
 * reader on the chord begins at Down's Press, off only at Down's Long: a Short of Back's between
 * the two, Back let go first, is dropped with the reader on and passes with it off. Back's Long,
 * which waits for the release, is dropped when Back is let go while Down is held back, as the
 * chord may still come. A chord key held on past its Back hold still names its command at its
 * Short or Long, but a new hold of Back meanwhile is not its chord and keeps its own events.
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
#define INPUT_SEQUENCE_SOURCE_HARDWARE (0u)
#define INPUT_SEQUENCE_SOURCE_SOFTWARE (1u)
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
    bool key_orphaned; /**< the Back hold the chord key began in has ended: its command still
                            runs, but a later hold is no chord of its */
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

/** The chord state of each source of events the filter sees: the keys, through the input
 *  service, and the console's input send, which marks its events as made in software. Apart, a
 *  console press whose release never comes (a scripted run cut off mid-chord) cannot turn the
 *  keys into chords, and a console key cannot join a chord of the keys. */
typedef struct {
    SrChords keys;
    SrChords console;
} SrChordSources;

void sr_chord_sources_init(SrChordSources* s);

/** sr_chords_feed on the state of the event's source: sequence_source as in InputEvent,
 *  INPUT_SEQUENCE_SOURCE_HARDWARE for the keys, anything else for the console. */
SrChordCommand sr_chord_sources_feed(
    SrChordSources* s,
    uint8_t sequence_source,
    bool reader_on,
    InputKey key,
    InputType type,
    uint32_t now_ms,
    bool* drop,
    bool* emit_long);

#ifdef __cplusplus
}
#endif
