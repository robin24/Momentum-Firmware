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
