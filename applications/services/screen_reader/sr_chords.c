#include "sr_chords.h"

void sr_chords_init(SrChords* s) {
    s->back_down = false;
    s->chord_used = false;
    s->long_pending = false;
    s->chord_key = -1;
    s->key_long_seen = false;
}

static SrChordCommand command_for(InputKey key) {
    switch(key) {
    case InputKeyUp:
        return SrChordReadAll;
    case InputKeyLeft:
        return SrChordVolumeDown;
    case InputKeyRight:
        return SrChordVolumeUp;
    default:
        return SrChordNone; // OK and Down decide on their Short or Long
    }
}

// The Short or Long of OK or Down as the chord key: the short command or the long one, the Long
// once and no short one after it. With the reader off only Down's Long is a command: the moment
// Down, held back since its Press, becomes a chord
static SrChordCommand split_command(SrChords* s, bool reader_on, InputKey key, InputType type) {
    SrChordCommand command = SrChordNone;
    if(type == InputTypeShort && !s->key_long_seen) {
        command = key == InputKeyOk ? SrChordRepeat : SrChordStatus;
    } else if(type == InputTypeLong) {
        s->key_long_seen = true;
        command = key == InputKeyOk ? SrChordSpell : SrChordToggleReader;
    }
    if(!reader_on && command != SrChordToggleReader) return SrChordNone;
    if(command != SrChordNone && s->back_down) {
        // A chord ran during this Back hold: Back's own Short and held-back Long go with it
        s->chord_used = true;
        s->long_pending = false;
    }
    return command;
}

SrChordCommand sr_chords_feed(
    SrChords* s,
    bool reader_on,
    InputKey key,
    InputType type,
    uint32_t now_ms,
    bool* drop,
    bool* emit_long) {
    (void)now_ms; // reserved, see the header
    *drop = false;
    *emit_long = false;

    // A chord key is swallowed until it is released, whatever Back does meanwhile and whether
    // the reader is on or off
    if(s->chord_key == (int8_t)key) {
        *drop = true;
        SrChordCommand command = SrChordNone;
        if(key == InputKeyOk || key == InputKeyDown) {
            command = split_command(s, reader_on, key, type);
        }
        if(type == InputTypeRelease) {
            s->chord_key = -1;
            s->key_long_seen = false;
        }
        return command;
    }

    if(key == InputKeyBack) {
        switch(type) {
        case InputTypePress:
            s->back_down = true;
            s->chord_used = false;
            s->long_pending = false;
            return SrChordNone;
        case InputTypeLong:
            *drop = true; // delivered on release instead
            if(!s->chord_used) s->long_pending = true;
            return SrChordNone;
        case InputTypeRepeat:
            *drop = true;
            return SrChordNone;
        case InputTypeShort:
            *drop = s->chord_used;
            return SrChordNone;
        case InputTypeRelease:
            *emit_long = s->long_pending && !s->chord_used;
            s->back_down = false;
            s->chord_used = false;
            s->long_pending = false;
            return SrChordNone;
        default:
            return SrChordNone;
        }
    }

    // Another key while Back is held and no chord key is active: a chord starts on its Press.
    // With the reader off only Down starts one, held back until its Long makes it a chord; till
    // then no chord has run, and Back's own Short and Long stay Back's
    if(s->back_down && s->chord_key < 0 && type == InputTypePress &&
       (reader_on || key == InputKeyDown)) {
        s->chord_key = (int8_t)key;
        s->key_long_seen = false;
        *drop = true;
        if(!reader_on) return SrChordNone;
        s->chord_used = true;
        s->long_pending = false;
        return command_for(key);
    }
    return SrChordNone;
}

void sr_chord_sources_init(SrChordSources* s) {
    sr_chords_init(&s->keys);
    sr_chords_init(&s->console);
}

SrChordCommand sr_chord_sources_feed(
    SrChordSources* s,
    uint8_t sequence_source,
    bool reader_on,
    InputKey key,
    InputType type,
    uint32_t now_ms,
    bool* drop,
    bool* emit_long) {
    SrChords* state = sequence_source == INPUT_SEQUENCE_SOURCE_HARDWARE ? &s->keys : &s->console;
    return sr_chords_feed(state, reader_on, key, type, now_ms, drop, emit_long);
}
