#include "sr_chords.h"

void sr_chords_init(SrChords* s) {
    s->back_down = false;
    s->chord_used = false;
    s->long_pending = false;
    s->back_down_ms = 0;
    s->chord_key = -1;
    s->ok_long_seen = false;
}

static SrChordCommand command_for(InputKey key) {
    switch(key) {
    case InputKeyUp:
        return SrChordReadAll;
    case InputKeyDown:
        return SrChordStatus;
    case InputKeyLeft:
        return SrChordVolumeDown;
    case InputKeyRight:
        return SrChordVolumeUp;
    default:
        return SrChordNone;
    }
}

SrChordCommand sr_chords_feed(
    SrChords* s,
    InputKey key,
    InputType type,
    uint32_t now_ms,
    bool* drop,
    bool* emit_long) {
    *drop = false;
    *emit_long = false;

    // A chord key is swallowed until it is released, whatever Back does meanwhile
    if(s->chord_key == (int8_t)key) {
        *drop = true;
        if(key == InputKeyOk) {
            if(type == InputTypeShort && !s->ok_long_seen) return SrChordRepeat;
            if(type == InputTypeLong) {
                s->ok_long_seen = true;
                return SrChordSpell;
            }
        }
        if(type == InputTypeRelease) {
            s->chord_key = -1;
            s->ok_long_seen = false;
        }
        return SrChordNone;
    }

    if(key == InputKeyBack) {
        switch(type) {
        case InputTypePress:
            s->back_down = true;
            s->chord_used = false;
            s->long_pending = false;
            s->back_down_ms = now_ms;
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

    // Another key while Back is held and no chord key is active: a chord starts on its Press
    if(s->back_down && s->chord_key < 0 && type == InputTypePress) {
        s->chord_key = (int8_t)key;
        s->chord_used = true;
        s->long_pending = false;
        s->ok_long_seen = false;
        *drop = true;
        return command_for(key); // OK decides on Short or Long, so None here
    }
    return SrChordNone;
}
