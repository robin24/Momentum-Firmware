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

#ifdef __cplusplus
}
#endif
