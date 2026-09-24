/**
 * @file screen_model.h
 * Screen reader: pure C model of what is on the display. No firmware dependencies,
 * so it compiles on the host for unit tests.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SR_TEXT_MAX          48
#define SR_MAX_RECORDS       32
#define SR_MAX_ROWS          16
#define SR_ROW_TEXT_MAX      64
#define SR_ANN_TEXT_MAX      160
/** A whole screen read on request (Back plus Up, the console): speech says it in parts */
#define SR_DESCRIBE_TEXT_MAX (4 * SR_ANN_TEXT_MAX)
#define SR_MAX_ANNOUNCEMENTS 4

/** Same numbering as GuiLayer in gui.h; duplicated so this header has no gui dependency. */
enum {
    SrLayerDesktop = 0,
    SrLayerWindow = 1,
    SrLayerStatusLeft = 2,
    SrLayerStatusRight = 3,
    SrLayerFullscreen = 4,
    SrLayerUnknown = 255,
};

/** Same numbering as Font in canvas.h. */
enum {
    SrFontPrimary = 0,
    SrFontSecondary = 1,
    SrFontKeyboard = 2,
    SrFontBigNumbers = 3,
    SrFontBatteryPercent = 4,
    SrFontCustom = 255,
};

/** One drawn string as captured by the GUI text tap. */
typedef struct {
    int16_t x;
    int16_t y;
    uint8_t layer;
    uint8_t font;
    bool inverted; /**< drawn in white, i.e. on a filled box */
    bool focus; /**< a module marked this string as the focused item */
    uint8_t button; /**< dialog button hint: 0 none, 1 left, 2 center, 3 right */
    uint16_t index; /**< 1-based position from the module, 0 if unknown */
    uint16_t count; /**< item count from the module, 0 if unknown */
    char text[SR_TEXT_MAX];
} SrRecord;

/** Everything captured during one frame. */
typedef struct {
    SrRecord records[SR_MAX_RECORDS];
    uint8_t count;
    bool overflow; /**< more strings were drawn than fit */
    uint8_t content_layer; /**< layer that drew the main content, SrLayerUnknown if none */
} SrFrame;

typedef enum {
    SrRowNormal,
    SrRowFocus,
    SrRowButton,
    SrRowStatus,
} SrRowKind;

/** Strings on one baseline, joined in reading order. */
typedef struct {
    char text[SR_ROW_TEXT_MAX];
    int16_t x;
    int16_t y;
    uint8_t font;
    SrRowKind kind;
    uint8_t button; /**< button side hint of the first record, 0 if none */
    uint16_t index;
    uint16_t count;
} SrRow;

/** A frame after modelling. */
typedef struct {
    SrRow rows[SR_MAX_ROWS];
    uint8_t row_count;
    int8_t title_row; /**< index into rows, or -1 */
    uint8_t content_layer;
    bool has_content; /**< at least one record outside the status bar */
    bool has_keyboard; /**< at least one record in the keyboard font */
    bool overflow;
} SrScreen;

typedef enum {
    SrAnnFocus, /**< the focused item changed */
    SrAnnScreen, /**< a different screen appeared */
    SrAnnChange, /**< some text on the same screen changed */
    SrAnnTyped, /**< a character was typed into a text field */
    SrAnnHome, /**< the home screen with no text appeared */
} SrAnnKind;

typedef struct {
    SrAnnKind kind;
    bool interrupt; /**< speech should stop what it is saying */
    char text[SR_ANN_TEXT_MAX];
} SrAnnouncement;

typedef struct {
    SrScreen prev;
    SrScreen current;
    bool have_prev;
    uint32_t last_spontaneous_ms;
    uint8_t verbosity; /**< 0 terse, 1 normal, 2 verbose */
} SrModel;

/** Trim, collapse spaces, drop the text cursor bar and non printable bytes. */
void sr_normalize(const char* in, char* out, size_t out_size);

/** Turn a frame into rows, focus, title, buttons. */
void sr_screen_build(const SrFrame* frame, SrScreen* screen);

/** All focus rows joined with ", ". Returns the length written. */
size_t sr_screen_focus_text(const SrScreen* screen, char* out, size_t out_size);

/** Title, rows, focus with position, buttons, as one readable text. */
size_t sr_screen_describe(const SrScreen* screen, char* out, size_t out_size);

/** The status bar rows (clock, battery percentage) joined with ", ". Returns the length
 *  written, 0 when the status bar has no text. */
size_t sr_status_text(const SrScreen* screen, char* out, size_t out_size);

/** The focus text, then the position of the first focus row that knows it: "Apps, 3 of 11".
 *  Just the focus text when no position is known. Returns the length written, 0 without
 *  focus. */
size_t sr_focus_with_position(const SrScreen* screen, char* out, size_t out_size);

void sr_model_init(SrModel* model, uint8_t verbosity);

/** Compare the frame with the previous one and write announcements. Returns how many. */
size_t sr_model_process(
    SrModel* model,
    const SrFrame* frame,
    uint32_t now_ms,
    bool key_recent,
    SrAnnouncement* out,
    size_t out_max);

#ifdef __cplusplus
}
#endif
