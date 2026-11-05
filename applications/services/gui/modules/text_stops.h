/**
 * @file text_stops.h
 * What the screen reader reads a long text by. A row is what fits on one line of the display,
 * broken at a space where one fits. A stop is what Up and Down move by and the reader speaks: the
 * rows of one printed line, up to TEXT_STOPS_STOP_CHARS characters and as many rows as the display
 * holds; a longer line is several stops. A blank line, or one of only spaces, dashes or characters
 * the reader doesn't say, is no stop.
 *
 * Plain C without the canvas, so the host tests can run it: the caller passes the glyph widths.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TEXT_STOPS_ROW_CHARS  47 /**< the most one tapped string holds */
#define TEXT_STOPS_STOP_CHARS 60 /**< the reader's row holds 63 */
#define TEXT_STOPS_OPENING_CHARS \
    100 /**< a scrolling text's opening, with room for title and buttons */
#define TEXT_STOPS_ANCHOR_BYTES 16 /**< how much of a stop is kept to find it again */

/** The width of a glyph in pixels, in the font being laid out. */
typedef uint8_t (*TextStopsGlyphWidth)(char c, void* context);

typedef struct {
    TextStopsGlyphWidth glyph_width;
    void* context;
    uint16_t row_width; /**< pixels */
    uint8_t rows_max; /**< rows the display holds, at least 1 */
} TextStopsLayout;

/** How a row ended. */
typedef enum {
    TextStopsBreakLine, /**< a newline ended the row */
    TextStopsBreakSpace, /**< broken at a space, which isn't drawn */
    TextStopsBreakWord, /**< broken inside a word */
    TextStopsBreakEnd, /**< the text ended */
} TextStopsBreak;

typedef struct {
    size_t start; /**< the row's first byte */
    size_t
        length; /**< bytes to draw: no newline, no carriage return before it, no space at a break */
    size_t next; /**< where the next row starts */
    TextStopsBreak ends;
} TextStopsRow;

typedef struct {
    size_t start; /**< the first row's first byte */
    size_t end; /**< just after the last drawn byte */
    size_t next; /**< where to look for the next stop */
    uint8_t rows;
    bool line_start; /**< begins its printed line */
    bool complete; /**< its printed line ends in a newline */
} TextStop;

/** The stop being built as rows go by; start it zeroed. */
typedef struct {
    size_t chars;
    uint8_t rows;
} TextStopsGroup;

/** The row of text that starts at byte start. It takes at least one character, whatever the
 *  glyph widths are, so walking a text always ends. */
void text_stops_row(
    const char* text,
    size_t start,
    const TextStopsLayout* layout,
    TextStopsRow* row);

/** Whether a line has nothing to say: only spaces, dashes, and what the reader drops. */
bool text_stops_line_is_silent(const char* line, size_t length);

/** Add a row of chars bytes. True when it begins a new stop; a row at a line's start always does. */
bool text_stops_group_row(
    TextStopsGroup* group,
    size_t chars,
    bool line_start,
    const TextStopsLayout* layout);

/** The first stop at or after from, which is a row's start. False when there's none. */
bool text_stops_next(const char* text, size_t from, const TextStopsLayout* layout, TextStop* stop);

/** The last stop starting before the byte before. False when there's none. */
bool text_stops_prev(
    const char* text,
    size_t before,
    const TextStopsLayout* layout,
    TextStop* stop);

/** Where the reader is in a text that may grow, be trimmed at its start or be replaced. */
typedef struct {
    size_t offset; /**< the current stop's first byte */
    uint16_t index; /**< 1-based, 0 when the text has no stops */
    uint16_t count;
    bool following; /**< on or after the newest complete line, or no line is complete */
    bool live; /**< a change of this stop without a key is news */
    uint32_t newest_hash; /**< of the newest complete line's first stop: when a line is dropped for
                               each one added, its index alone doesn't show a new one came */
    uint16_t newest_index; /**< 0 when no line is complete */
    char anchor[TEXT_STOPS_ANCHOR_BYTES]; /**< the start of the current stop, to find it again */
    uint8_t anchor_length;
    bool anchor_complete; /**< the stop's line was complete; an unfinished one may still grow */
} TextStopsCursor;

/** Start on a text. With at_end, the text box's TextBoxFocusEnd, on the newest complete line's
 *  first stop and following it; otherwise on the first stop. */
void text_stops_cursor_open(
    TextStopsCursor* cursor,
    const char* text,
    const TextStopsLayout* layout,
    bool at_end);

/** The text changed. Following, a new complete line takes the reader along. Otherwise the reader
 *  stays on its stop, the nearest at or before the old place that matches, because trimming only
 *  moves text towards the start; with no match, on the first stop, the oldest line still kept. */
void text_stops_cursor_refresh(
    TextStopsCursor* cursor,
    const char* text,
    const TextStopsLayout* layout,
    bool at_end);

/** Move by steps stops, down when positive, stopping at the first and the last. The offset must be
 *  a stop's start in this text, so call it after text_stops_cursor_refresh. */
void text_stops_cursor_step(
    TextStopsCursor* cursor,
    const char* text,
    const TextStopsLayout* layout,
    bool at_end,
    int16_t steps);

/** Whether a scrolling text's opening reads one more stop of stop_chars, with taken stops of
 *  used_chars read so far. The first stop is always read. */
bool text_stops_opening_takes(uint16_t taken, size_t used_chars, size_t stop_chars);

/** Up (step < 0) or Down in a scrolling text that opened with taken of total stops read. From the
 *  opening, Down goes to the next stop not heard and Up to the last stop heard, read alone; after
 *  that, one stop at a time within 1 to total. *current is 0 during the opening. */
void text_stops_opening_step(
    bool* opening,
    uint16_t* current,
    uint16_t taken,
    uint16_t total,
    int8_t step);

#ifdef __cplusplus
}
#endif
