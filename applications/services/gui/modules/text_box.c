#include "text_box.h"
#include "text_stops.h"
#include <gui/canvas.h>
#include <gui/canvas_i.h>
#include <gui/elements.h>
#include <furi.h>
#include <stdint.h>
#include <string.h>

#define TEXT_BOX_TEXT_WIDTH           (120)
#define TEXT_BOX_TEXT_HEIGHT          (56)
#define TEXT_BOX_MAX_LINES_PER_SCREEN (10)

#define TEXT_BOX_LINES_SCROLL_SPEED_MEDIUM     (3)
#define TEXT_BOX_LINES_SCROLL_SPEED_FAST       (5)
#define TEXT_BOX_LINES_SCROLL_SPEED_SATURATION (9)

#define TEXT_BOX_EMPTY_DELAY_MS (1000)

_Static_assert(TEXT_STOPS_ROW_CHARS < CANVAS_TAP_TEXT_MAX, "a row must fit one tapped string");

struct TextBox {
    View* view;

    uint16_t button_held_for_ticks;
    FuriTimer* empty_timer;
};

typedef struct {
    TextBoxFont font;
    TextBoxFocus focus;
    const char* text;

    int32_t scroll_pos;
    int32_t scroll_num;
    int32_t lines_on_screen;

    int32_t line_offset;
    int32_t text_offset;
    FuriString* text_on_screen;
    FuriString* text_line;

    bool formatted;

    // With the screen reader listening, the text is read by stops: see text_stops.h
    bool reading; /**< the reader was listening at the last draw */
    bool opened; /**< the cursor knows this text; off until the first draw of a new one */
    int16_t pending; /**< Up and Down since the last draw, which alone can measure text */
    TextStopsCursor cursor;
    FuriTimer* empty_timer; /**< the box has stayed empty long enough to say so */
    bool empty_due;
} TextBoxModel;

static void text_box_process_down(TextBox* text_box, uint8_t lines) {
    with_view_model(
        text_box->view,
        TextBoxModel * model,
        {
            if(model->reading) {
                model->pending++;
            } else if(model->scroll_pos + lines < model->scroll_num) {
                model->scroll_pos += lines;
            } else {
                if(model->scroll_num > 0) {
                    model->scroll_pos = model->scroll_num - 1;
                }
            }
        },
        true);
}

static void text_box_process_up(TextBox* text_box, uint8_t lines) {
    with_view_model(
        text_box->view,
        TextBoxModel * model,
        {
            if(model->reading) {
                model->pending--;
            } else if(model->scroll_pos - lines > 0) {
                model->scroll_pos -= lines;
            } else {
                model->scroll_pos = 0;
            }
        },
        true);
}

static bool text_box_view_input_callback(InputEvent* event, void* context) {
    furi_assert(context);

    TextBox* text_box = context;
    bool consumed = false;

    if(event->type == InputTypeShort || event->type == InputTypeRepeat) {
        int32_t scroll_speed = 1;
        if(text_box->button_held_for_ticks > TEXT_BOX_LINES_SCROLL_SPEED_FAST) {
            if(text_box->button_held_for_ticks % 2) {
                scroll_speed = 0;
            } else {
                scroll_speed =
                    (text_box->button_held_for_ticks > TEXT_BOX_LINES_SCROLL_SPEED_SATURATION) ?
                        TEXT_BOX_LINES_SCROLL_SPEED_FAST :
                        TEXT_BOX_LINES_SCROLL_SPEED_MEDIUM;
            }
        }

        if(event->key == InputKeyDown) {
            text_box_process_down(text_box, scroll_speed);
            consumed = true;
        } else if(event->key == InputKeyUp) {
            text_box_process_up(text_box, scroll_speed);
            consumed = true;
        }

        text_box->button_held_for_ticks++;
    } else if(event->type == InputTypeRelease) {
        text_box->button_held_for_ticks = 0;
        consumed = true;
    }

    return consumed;
}

static bool text_box_end_of_text_reached(TextBoxModel* model) {
    return model->text[model->text_offset] == '\0';
}

static bool text_box_start_of_text_reached(TextBoxModel* model) {
    return model->text_offset == 0;
}

static void text_box_seek_next_line(Canvas* canvas, TextBoxModel* model) {
    size_t line_width = 0;

    while(!text_box_end_of_text_reached(model)) {
        char symb = model->text[model->text_offset];
        if(symb == '\n') {
            model->text_offset++;
            break;
        } else {
            size_t glyph_width = canvas_glyph_width(canvas, symb);
            if(line_width + glyph_width > TEXT_BOX_TEXT_WIDTH) {
                break;
            }
            line_width += glyph_width;
            model->text_offset++;
        }
    }
}

static void text_box_seek_end_of_prev_line(TextBoxModel* model) {
    do {
        if(text_box_start_of_text_reached(model)) break;
        model->text_offset--;
        if(text_box_start_of_text_reached(model)) break;
        if(model->text[model->text_offset] == '\n') {
            model->text_offset--;
        }
    } while(false);
}

static void text_box_seek_prev_paragraph(TextBoxModel* model) {
    while(!text_box_start_of_text_reached(model)) {
        if(model->text[model->text_offset] == '\n') {
            model->text_offset++;
            break;
        }
        model->text_offset--;
    }
}

static void text_box_seek_prev_line(Canvas* canvas, TextBoxModel* model) {
    int32_t start_text_offset = model->text_offset;

    text_box_seek_end_of_prev_line(model);
    text_box_seek_prev_paragraph(model);

    int32_t current_text_offset = model->text_offset;
    while(true) {
        text_box_seek_next_line(canvas, model);
        if(model->text_offset == start_text_offset) {
            break;
        }
        current_text_offset = model->text_offset;
    }
    model->text_offset = current_text_offset;
}

static void text_box_move_line_offset(Canvas* canvas, TextBoxModel* model, int32_t line_offset) {
    if(line_offset >= 0) {
        for(int32_t i = 0; i < line_offset; i++) {
            text_box_seek_next_line(canvas, model);
        }
    } else {
        for(int32_t i = 0; i < (-line_offset); i++) {
            text_box_seek_prev_line(canvas, model);
        }
    }
}

static void text_box_update_screen_text(Canvas* canvas, TextBoxModel* model) {
    furi_string_reset(model->text_on_screen);
    furi_string_reset(model->text_line);

    int32_t start_text_offset = model->text_offset;

    for(int32_t i = 0; i < model->lines_on_screen; i++) {
        int32_t current_line_text_offset = model->text_offset;
        text_box_seek_next_line(canvas, model);
        int32_t next_line_text_offset = model->text_offset;
        furi_string_set_strn(
            model->text_line,
            &model->text[current_line_text_offset],
            next_line_text_offset - current_line_text_offset);
        size_t str_len = furi_string_size(model->text_line);
        if(furi_string_get_char(model->text_line, str_len - 1) != '\n') {
            furi_string_push_back(model->text_line, '\n');
        }
        furi_string_cat(model->text_on_screen, model->text_line);

        if(text_box_end_of_text_reached(model)) break;
        current_line_text_offset = next_line_text_offset;
    }

    model->text_offset = start_text_offset;
}

static void text_box_update_text_on_screen(Canvas* canvas, TextBoxModel* model) {
    int32_t line_offset = model->scroll_pos - model->line_offset;
    text_box_move_line_offset(canvas, model, line_offset);
    text_box_update_screen_text(canvas, model);
    model->line_offset = model->scroll_pos;
}

static void text_box_prepare_model(Canvas* canvas, TextBoxModel* model) {
    int32_t lines_num = 0;
    model->text_offset = 0;
    model->scroll_num = 0;
    model->scroll_pos = 0;
    model->line_offset = 0;
    model->lines_on_screen = TEXT_BOX_TEXT_HEIGHT / canvas_current_font_height(canvas);

    // Cache text offset to quick final text offset update if TextBoxFocusEnd is set
    int32_t window_offset[TEXT_BOX_MAX_LINES_PER_SCREEN] = {};
    do {
        window_offset[lines_num % model->lines_on_screen] = model->text_offset;
        text_box_seek_next_line(canvas, model);
        lines_num++;
    } while(!text_box_end_of_text_reached(model));
    model->text_offset = 0;
    lines_num++;

    if(model->focus == TextBoxFocusEnd) {
        if(lines_num > model->lines_on_screen) {
            model->text_offset = window_offset[(lines_num - 1) % model->lines_on_screen];
        }
    }

    if(lines_num > model->lines_on_screen) {
        model->scroll_num = lines_num - model->lines_on_screen;
        model->scroll_pos = (model->focus == TextBoxFocusEnd) ? model->scroll_num - 1 : 0;
    }

    text_box_update_screen_text(canvas, model);
    model->line_offset = model->scroll_pos;
}

static uint8_t text_box_glyph_width(char c, void* context) {
    return (uint8_t)canvas_glyph_width(context, (uint8_t)c);
}

/** With the screen reader listening: the text from the current stop's first row, the rows wrapped
 *  at spaces, and only that stop for the reader to read. */
static void text_box_draw_reading(Canvas* canvas, TextBoxModel* model) {
    const size_t font_height = canvas_current_font_height(canvas);
    const TextStopsLayout layout = {
        .glyph_width = text_box_glyph_width,
        .context = canvas,
        .row_width = TEXT_BOX_TEXT_WIDTH,
        .rows_max = MAX(TEXT_BOX_TEXT_HEIGHT / font_height, 1u),
    };
    const bool at_end = model->focus == TextBoxFocusEnd;
    TextStopsCursor* cursor = &model->cursor;

    // An app may have cut its text before it tells us
    if(cursor->offset > strlen(model->text)) model->formatted = false;
    if(!model->formatted) {
        if(model->opened) {
            text_stops_cursor_refresh(cursor, model->text, &layout, at_end);
        } else {
            text_stops_cursor_open(cursor, model->text, &layout, at_end);
        }
        model->opened = true;
        model->formatted = true;
    }
    if(model->pending) {
        text_stops_cursor_step(cursor, model->text, &layout, at_end, model->pending);
        model->pending = 0;
    }

    elements_slightly_rounded_frame(canvas, 0, 0, 124, 64);
    if(cursor->count > 1) elements_scrollbar(canvas, cursor->index - 1, cursor->count);

    if(cursor->index == 0) {
        // A console with no reply is not silent. Not at once: nearly every command's first line
        // comes within a moment, and "Empty" would be said before each of them
        if(model->empty_due) {
            canvas_tap_hint_reading(canvas, CANVAS_TAP_READING_STOP);
            canvas_tap_note(canvas, 3, 11, "Empty");
        } else if(!furi_timer_is_running(model->empty_timer)) {
            furi_timer_start(model->empty_timer, TEXT_BOX_EMPTY_DELAY_MS);
        }
        return;
    }
    model->empty_due = false;

    TextStop stop = {0};
    text_stops_next(model->text, cursor->offset, &layout, &stop);
    const uint8_t reading = cursor->live ? CANVAS_TAP_READING_LIVE : CANVAS_TAP_READING_STOP;
    size_t pos = cursor->offset;
    uint8_t row_number = 0;
    TextStopsBreak before = TextStopsBreakLine;
    for(size_t y = 11; y < 64 && model->text[pos] != '\0'; y += font_height) {
        TextStopsRow row;
        text_stops_row(model->text, pos, &layout, &row);
        if(row_number == 0) {
            canvas_tap_hint_focus(canvas, cursor->index, cursor->count);
            canvas_tap_hint_reading(canvas, reading);
        } else if(row_number < stop.rows) {
            canvas_tap_hint_join(
                canvas,
                before == TextStopsBreakSpace ? CANVAS_TAP_JOIN_SPACE : CANVAS_TAP_JOIN_DIRECT);
            canvas_tap_hint_reading(canvas, reading);
        } else {
            canvas_tap_hint_full_text(canvas, "", false);
        }
        furi_string_set_strn(model->text_line, model->text + row.start, row.length);
        canvas_draw_str(canvas, 3, y, furi_string_get_cstr(model->text_line));
        before = row.ends;
        pos = row.next;
        row_number++;
    }
}

static void text_box_view_draw_callback(Canvas* canvas, void* _model) {
    TextBoxModel* model = _model;

    if(!model->text) {
        return;
    }

    canvas_clear(canvas);
    if(model->font == TextBoxFontText) {
        canvas_set_font(canvas, FontSecondary);
    } else if(model->font == TextBoxFontHex) {
        canvas_set_font(canvas, FontKeyboard);
    }

    const bool reader = canvas->tap_callback != NULL;
    if(reader != model->reading) {
        // The reader went on or off with the text open: start over in the other way of reading
        model->reading = reader;
        model->opened = false;
        model->pending = 0;
        model->formatted = false;
    }
    if(reader) {
        text_box_draw_reading(canvas, model);
        return;
    }

    if(!model->formatted) {
        text_box_prepare_model(canvas, model);
        model->formatted = true;
    }

    elements_slightly_rounded_frame(canvas, 0, 0, 124, 64);
    elements_scrollbar(canvas, model->scroll_pos, model->scroll_num);

    if(model->line_offset != model->scroll_pos) {
        text_box_update_text_on_screen(canvas, model);
    }
    elements_multiline_text(canvas, 3, 11, furi_string_get_cstr(model->text_on_screen));
}

static void text_box_empty_timer_callback(void* context) {
    TextBox* text_box = context;
    with_view_model(
        text_box->view,
        TextBoxModel * model,
        { model->empty_due = model->opened && model->cursor.index == 0; },
        true);
}

TextBox* text_box_alloc(void) {
    TextBox* text_box = malloc(sizeof(TextBox));
    text_box->view = view_alloc();
    text_box->empty_timer =
        furi_timer_alloc(text_box_empty_timer_callback, FuriTimerTypeOnce, text_box);
    view_set_context(text_box->view, text_box);
    view_allocate_model(text_box->view, ViewModelTypeLocking, sizeof(TextBoxModel));
    view_set_draw_callback(text_box->view, text_box_view_draw_callback);
    view_set_input_callback(text_box->view, text_box_view_input_callback);

    with_view_model(
        text_box->view,
        TextBoxModel * model,
        {
            model->text = NULL;
            model->text_on_screen = furi_string_alloc();
            model->text_line = furi_string_alloc();
            model->formatted = false;
            model->font = TextBoxFontText;
            model->reading = false;
            model->opened = false;
            model->pending = 0;
            model->empty_timer = text_box->empty_timer;
            model->empty_due = false;
        },
        true);

    return text_box;
}

void text_box_free(TextBox* text_box) {
    furi_check(text_box);

    furi_timer_free(text_box->empty_timer);
    with_view_model(
        text_box->view,
        TextBoxModel * model,
        {
            furi_string_free(model->text_on_screen);
            furi_string_free(model->text_line);
        },
        true);
    view_free(text_box->view);
    free(text_box);
}

View* text_box_get_view(TextBox* text_box) {
    furi_check(text_box);
    return text_box->view;
}

void text_box_reset(TextBox* text_box) {
    furi_check(text_box);

    with_view_model(
        text_box->view,
        TextBoxModel * model,
        {
            model->text = NULL;
            model->font = TextBoxFontText;
            model->focus = TextBoxFocusStart;
            furi_string_reset(model->text_line);
            furi_string_reset(model->text_on_screen);
            model->line_offset = 0;
            model->text_offset = 0;
            model->lines_on_screen = 0;
            model->scroll_num = 0;
            model->scroll_pos = 0;
            model->formatted = false;
            model->reading = false;
            model->opened = false;
            model->pending = 0;
            model->empty_due = false;
        },
        true);
}

void text_box_set_text(TextBox* text_box, const char* text) {
    furi_check(text_box);
    furi_check(text);

    with_view_model(
        text_box->view,
        TextBoxModel * model,
        {
            model->text = text;
            model->formatted = false;
        },
        true);
}

void text_box_set_font(TextBox* text_box, TextBoxFont font) {
    furi_check(text_box);

    with_view_model(text_box->view, TextBoxModel * model, { model->font = font; }, true);
}

void text_box_set_focus(TextBox* text_box, TextBoxFocus focus) {
    furi_check(text_box);

    with_view_model(text_box->view, TextBoxModel * model, { model->focus = focus; }, true);
}
