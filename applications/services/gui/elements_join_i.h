/**
 * @file elements_join_i.h
 * Whether a line break falls inside a word: the drawing helpers that break text into lines then
 * mark the next piece with the screen reader's join hint, so the reader reads the word whole
 */
#pragma once

#include <stdbool.h>

/** Whether a line break before `at`, inside `text`, falls inside a word: the characters on either
 * side are neither spaces nor line breaks, and the text goes on after it. The text box's format
 * markers, an escape and its letter, are passed over before the break, so a marker after a space
 * keeps two words apart. A break at the start of the text, or before a marker, falls inside none */
static inline bool elements_break_inside_word(const char* text, const char* at) {
    char after = at[0];
    if(after == ' ' || after == '\n' || after == '\e' || after == '\0') return false;
    const char* before = at;
    while(before - text >= 2 && before[-2] == '\e') {
        before -= 2;
    }
    if(before <= text) return false;
    return before[-1] != ' ' && before[-1] != '\n';
}
