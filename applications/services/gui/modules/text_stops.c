#include "text_stops.h"

#include <string.h>

void text_stops_row(
    const char* text,
    size_t start,
    const TextStopsLayout* layout,
    TextStopsRow* row) {
    size_t pos = start;
    size_t width = 0;
    size_t space = 0; // the last space with something before it on this row, 0 if none
    bool word_seen = false;

    row->start = start;
    while(true) {
        char c = text[pos];
        char after = c ? text[pos + 1] : '\0';
        if(c == '\0' || (c == '\r' && after == '\0')) {
            row->length = pos - start;
            row->next = c ? pos + 1 : pos;
            row->ends = TextStopsBreakEnd;
            return;
        }
        if(c == '\n' || (c == '\r' && after == '\n')) {
            row->length = pos - start;
            row->next = pos + (c == '\r' ? 2 : 1);
            row->ends = TextStopsBreakLine;
            return;
        }
        size_t chars = pos - start;
        uint8_t glyph = layout->glyph_width(c, layout->context);
        if(chars > 0 && (chars >= TEXT_STOPS_ROW_CHARS || width + glyph > layout->row_width)) {
            if(c == ' ') {
                row->length = chars;
                row->next = pos + 1;
                row->ends = TextStopsBreakSpace;
            } else if(space > 0) {
                row->length = space - start;
                row->next = space + 1;
                row->ends = TextStopsBreakSpace;
            } else {
                row->length = chars;
                row->next = pos;
                row->ends = TextStopsBreakWord;
            }
            return;
        }
        if(c == ' ') {
            if(word_seen) space = pos;
        } else {
            word_seen = true;
        }
        width += glyph;
        pos++;
    }
}

bool text_stops_line_is_silent(const char* line, size_t length) {
    for(size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)line[i];
        // As the reader's sr_normalize and sr_is_dashes: it drops these, so a line of them is silence
        if(c > ' ' && c <= 0x7E && c != '|' && c != '-') return false;
    }
    return true;
}

bool text_stops_group_row(
    TextStopsGroup* group,
    size_t chars,
    bool line_start,
    const TextStopsLayout* layout) {
    size_t total = group->chars + 1 + chars; // the space between the rows
    if(line_start || group->rows == 0 || total > TEXT_STOPS_STOP_CHARS ||
       group->rows >= layout->rows_max) {
        group->chars = chars;
        group->rows = 1;
        return true;
    }
    group->chars = total;
    group->rows++;
    return false;
}

/** The rest of the line from pos has nothing to say, and how long it is. */
static bool rest_is_silent(const char* text, size_t pos, size_t* length) {
    *length = strcspn(text + pos, "\n");
    return text_stops_line_is_silent(text + pos, *length);
}

bool text_stops_next(const char* text, size_t from, const TextStopsLayout* layout, TextStop* stop) {
    size_t pos = from;
    size_t rest;
    while(rest_is_silent(text, pos, &rest)) {
        pos += rest;
        if(text[pos] == '\0') return false;
        pos++;
    }

    stop->start = pos;
    stop->line_start = pos == 0 || text[pos - 1] == '\n';
    stop->complete = text[pos + rest] == '\n';

    TextStopsGroup group = {0};
    size_t at = pos;
    bool first = true;
    while(true) {
        TextStopsRow row;
        text_stops_row(text, at, layout, &row);
        if(text_stops_group_row(&group, row.length, first, layout) && !first) {
            stop->next = at;
            return true;
        }
        first = false;
        stop->rows = group.rows;
        stop->end = row.start + row.length;
        stop->next = row.next;
        if(row.ends == TextStopsBreakLine || row.ends == TextStopsBreakEnd) return true;
        at = row.next;
        // A break just before the line's end leaves no empty row in the stop
        if(rest_is_silent(text, at, &rest)) return true;
    }
}

bool text_stops_prev(
    const char* text,
    size_t before,
    const TextStopsLayout* layout,
    TextStop* stop) {
    size_t search = before;
    while(search > 0) {
        // The start of the line that holds the byte before search; measuring starts only there
        size_t line = search - 1;
        while(line > 0 && text[line - 1] != '\n')
            line--;
        bool found = false;
        TextStop walk;
        size_t from = line;
        while(text_stops_next(text, from, layout, &walk) && walk.start < before) {
            *stop = walk;
            found = true;
            from = walk.next;
        }
        if(found) return true;
        search = line;
    }
    return false;
}

static void cursor_remember(TextStopsCursor* cursor, const char* text, const TextStop* stop) {
    size_t length = stop->end - stop->start;
    if(length > TEXT_STOPS_ANCHOR_BYTES) length = TEXT_STOPS_ANCHOR_BYTES;
    memcpy(cursor->anchor, text + stop->start, length);
    cursor->anchor_length = (uint8_t)length;
    cursor->anchor_complete = stop->complete;
}

static bool cursor_matches(const TextStopsCursor* cursor, const char* text, const TextStop* stop) {
    size_t length = stop->end - stop->start;
    if(length < cursor->anchor_length) return false;
    // A complete line shorter than the anchor is all there: "a" is not found in "abc"
    if(cursor->anchor_complete && cursor->anchor_length < TEXT_STOPS_ANCHOR_BYTES &&
       length != cursor->anchor_length) {
        return false;
    }
    return memcmp(text + stop->start, cursor->anchor, cursor->anchor_length) == 0;
}

/** FNV-1a of a stop's bytes. */
static uint32_t stop_hash(const char* text, const TextStop* stop) {
    uint32_t hash = 2166136261u;
    for(size_t i = stop->start; i < stop->end; i++) {
        hash = (hash ^ (uint8_t)text[i]) * 16777619u;
    }
    return hash;
}

/** Stand on this stop, with what follows from it. */
static void cursor_arrive(
    TextStopsCursor* cursor,
    const char* text,
    const TextStop* stop,
    uint16_t index,
    bool at_end) {
    cursor->offset = stop->start;
    cursor->index = index;
    cursor->following = at_end && (cursor->newest_index == 0 || index >= cursor->newest_index);
    cursor->live = !at_end || (cursor->following && stop->complete);
    cursor_remember(cursor, text, stop);
}

void text_stops_cursor_refresh(
    TextStopsCursor* cursor,
    const char* text,
    const TextStopsLayout* layout,
    bool at_end) {
    TextStop stop, first = {0}, last = {0}, newest = {0}, found = {0};
    uint16_t index = 0, last_index = 0, newest_index = 0, found_index = 0;
    size_t from = 0;

    while(text_stops_next(text, from, layout, &stop)) {
        index++;
        if(index == 1) first = stop;
        last = stop;
        last_index = index;
        if(stop.complete && stop.line_start) {
            newest = stop;
            newest_index = index;
        }
        if(stop.start <= cursor->offset && cursor->anchor_length > 0 &&
           cursor_matches(cursor, text, &stop)) {
            found = stop;
            found_index = index;
        }
        from = stop.next;
    }

    // Following, only a new complete line moves the reader: Down may have gone past the newest
    // one's first stop, to the rest of a long line or to a line still unfinished
    bool past_newest = cursor->index > cursor->newest_index;
    uint32_t newest_hash = newest_index ? stop_hash(text, &newest) : 0;
    bool same_newest = newest_index == cursor->newest_index && newest_hash == cursor->newest_hash;
    bool stays = at_end && cursor->following && past_newest && found_index > 0 && same_newest;
    bool along = at_end && cursor->following && !stays;

    cursor->count = index;
    cursor->newest_hash = newest_hash;
    cursor->newest_index = newest_index;

    if(index == 0) {
        cursor->offset = 0;
        cursor->index = 0;
        cursor->following = at_end;
        cursor->live = !at_end;
        cursor->anchor_length = 0;
    } else if(along) {
        if(newest_index) {
            cursor_arrive(cursor, text, &newest, newest_index, at_end);
        } else {
            cursor_arrive(cursor, text, &last, last_index, at_end);
        }
    } else if(found_index) {
        cursor_arrive(cursor, text, &found, found_index, at_end);
    } else {
        cursor_arrive(cursor, text, &first, 1, at_end);
    }
}

void text_stops_cursor_open(
    TextStopsCursor* cursor,
    const char* text,
    const TextStopsLayout* layout,
    bool at_end) {
    memset(cursor, 0, sizeof(*cursor));
    cursor->following = at_end;
    text_stops_cursor_refresh(cursor, text, layout, at_end);
}

void text_stops_cursor_step(
    TextStopsCursor* cursor,
    const char* text,
    const TextStopsLayout* layout,
    bool at_end,
    int16_t steps) {
    if(cursor->index == 0 || steps == 0) return;
    TextStop stop, other;
    if(!text_stops_next(text, cursor->offset, layout, &stop)) return;
    uint16_t index = cursor->index;
    while(steps > 0 && text_stops_next(text, stop.next, layout, &other)) {
        stop = other;
        index++;
        steps--;
    }
    while(steps < 0 && text_stops_prev(text, stop.start, layout, &other)) {
        stop = other;
        index--;
        steps++;
    }
    cursor_arrive(cursor, text, &stop, index, at_end);
}

bool text_stops_opening_takes(uint16_t taken, size_t used_chars, size_t stop_chars) {
    return taken == 0 || used_chars + stop_chars <= TEXT_STOPS_OPENING_CHARS;
}

void text_stops_opening_step(
    bool* opening,
    uint16_t* current,
    uint16_t taken,
    uint16_t total,
    int8_t step) {
    if(total == 0) return;
    if(*opening) {
        if(step < 0) {
            *current = taken;
            *opening = false;
        } else if(taken < total) {
            *current = taken + 1;
            *opening = false;
        }
    } else if(step < 0) {
        if(*current > 1) (*current)--;
    } else if(*current < total) {
        (*current)++;
    }
}
