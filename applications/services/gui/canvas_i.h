/**
 * @file canvas_i.h
 * GUI: internal Canvas API
 */

#pragma once

#include "canvas.h"
#include <u8g2.h>
#include <toolbox/compress.h>
#include <m-array.h>
#include <m-algo.h>
#include <furi.h>

#define ICON_DECOMPRESSOR_BUFFER_SIZE (128u * 64 / 8)

#define CANVAS_TAP_TEXT_MAX      48
#define CANVAS_TAP_LAYER_UNKNOWN 255
#define CANVAS_TAP_FONT_CUSTOM   255

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*CanvasCommitCallback)(
    uint8_t* data,
    size_t size,
    CanvasOrientation orientation,
    void* context);

typedef struct {
    CanvasCommitCallback callback;
    void* context;
} CanvasCallbackPair;

ARRAY_DEF(CanvasCallbackPairArray, CanvasCallbackPair, M_POD_OPLIST); //-V658

#define M_OPL_CanvasCallbackPairArray_t() ARRAY_OPLIST(CanvasCallbackPairArray, M_POD_OPLIST)

ALGO_DEF(CanvasCallbackPairArray, CanvasCallbackPairArray_t);

/** One drawn string, reported to the screen reader tap */
typedef struct {
    int16_t x; /**< absolute framebuffer x of the string start, after alignment */
    int16_t y; /**< absolute framebuffer baseline y */
    uint8_t layer; /**< GuiLayer being drawn, or CANVAS_TAP_LAYER_UNKNOWN */
    uint8_t font; /**< Font, or CANVAS_TAP_FONT_CUSTOM */
    bool inverted; /**< logical colour was ColorWhite, i.e. text on a filled box */
    bool focus; /**< a focus hint applied to this string */
    bool note; /**< not drawn: a note's words, at x 0, y 0 from canvas_tap_hint_note, at their
                    place from canvas_tap_hint_note_at */
    uint8_t button; /**< dialog button hint: 0 none, 1 left, 2 center, 3 right */
    uint16_t index; /**< 1-based position from the focus hint, 0 if unknown */
    uint16_t count; /**< item count from the focus hint, 0 if unknown */
    char text[CANVAS_TAP_TEXT_MAX];
} CanvasTapRecord;

typedef void (*CanvasTapCallback)(const CanvasTapRecord* record, void* context);

/** Canvas structure
 */
struct Canvas {
    u8g2_t fb;
    CanvasOrientation orientation;
    size_t offset_x;
    size_t offset_y;
    size_t width;
    size_t height;
    CompressIcon* compress_icon;
    CanvasCallbackPairArray_t canvas_callback_pair;
    FuriMutex* mutex;

    // Screen reader text tap
    CanvasTapCallback tap_callback;
    void* tap_context;
    uint8_t tap_layer;
    uint8_t tap_font;
    bool tap_hint_focus;
    uint8_t tap_hint_button;
    uint16_t tap_hint_index;
    uint16_t tap_hint_count;
    bool tap_hint_full;
    bool tap_hint_verify;
    char tap_hint_text[CANVAS_TAP_TEXT_MAX];
    bool tap_run_active;
    int16_t tap_run_next_x;
    CanvasTapRecord tap_run;
};

/** Allocate memory and initialize canvas
 *
 * @return     Canvas instance
 */
Canvas* canvas_init(void);

/** Free canvas memory
 *
 * @param      canvas  Canvas instance
 */
void canvas_free(Canvas* canvas);

/** Set drawing region relative to real screen buffer
 *
 * @param      canvas    Canvas instance
 * @param      offset_x  x coordinate offset
 * @param      offset_y  y coordinate offset
 * @param      width     width
 * @param      height    height
 */
void canvas_frame_set(
    Canvas* canvas,
    int32_t offset_x,
    int32_t offset_y,
    size_t width,
    size_t height);

/** Set canvas orientation
 *
 * @param      canvas       Canvas instance
 * @param      orientation  CanvasOrientation
 */
void canvas_set_orientation(Canvas* canvas, CanvasOrientation orientation);

/** Get canvas orientation
 *
 * @param      canvas  Canvas instance
 *
 * @return     CanvasOrientation
 */
CanvasOrientation canvas_get_orientation(const Canvas* canvas);

/** Draw a u8g2 bitmap
 *
 * @param      u8g2     u8g2 instance
 * @param      x        x coordinate
 * @param      y        y coordinate
 * @param      width    width
 * @param      height   height
 * @param      bitmap   bitmap
 * @param      rotation rotation
 */
void canvas_draw_u8g2_bitmap(
    u8g2_t* u8g2,
    int32_t x,
    int32_t y,
    size_t width,
    size_t height,
    const uint8_t* bitmap,
    IconRotation rotation);

/** Add canvas commit callback.
 *
 * This callback will be called upon Canvas commit.
 * 
 * @param      canvas    Canvas instance
 * @param      callback  CanvasCommitCallback
 * @param      context   CanvasCommitCallback context
 */
void canvas_add_framebuffer_callback(Canvas* canvas, CanvasCommitCallback callback, void* context);

/** Remove canvas commit callback.
 *
 * @param      canvas    Canvas instance
 * @param      callback  CanvasCommitCallback
 * @param      context   CanvasCommitCallback context
 */
void canvas_remove_framebuffer_callback(
    Canvas* canvas,
    CanvasCommitCallback callback,
    void* context);

/** Install the screen reader tap. NULL callback disables it. GUI service only. */
void canvas_tap_set_callback(Canvas* canvas, CanvasTapCallback callback, void* context);

/** Tag the layer that is about to be drawn (GuiLayer value). */
void canvas_tap_set_layer(Canvas* canvas, uint8_t layer);

/** Report a pending glyph run. Call at frame end before the frame is handed over. */
void canvas_tap_flush(Canvas* canvas);

/** The next drawn string is the focused item. index is 1-based, 0 if unknown. */
void canvas_tap_hint_focus(Canvas* canvas, uint16_t index, uint16_t count);

/** The next drawn string is a dialog button label: 1 left, 2 center, 3 right. */
void canvas_tap_hint_button(Canvas* canvas, uint8_t side);

/** The next drawn string is a shortened or scrolled fragment of full_text.
 * With verify_prefix, the hint is used only if the drawn string (minus a trailing "...")
 * is a prefix of full_text. */
void canvas_tap_hint_full_text(Canvas* canvas, const char* full_text, bool verify_prefix);

/** Report text that is shown as an icon (for example the Save key), as if drawn at x, y. */
void canvas_tap_note(Canvas* canvas, int32_t x, int32_t y, const char* text);

#ifdef __cplusplus
}
#endif
