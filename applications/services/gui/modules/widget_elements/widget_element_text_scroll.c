#include "widget_element_i.h"
#include "../text_stops.h"
#include <gui/canvas_i.h>
#include <gui/elements.h>
#include <m-array.h>
#include <string.h>

#define WIDGET_ELEMENT_TEXT_SCROLL_BAR_OFFSET (4)

typedef struct {
    Font font;
    Align horizontal;
    FuriString* text;
    uint16_t stop; /**< with the reader on, the stop this row is in, from 1; 0 for a silent line */
    uint8_t join; /**< with the reader on, CANVAS_TAP_JOIN_* onto the row before in its stop */
} TextScrollLineArray;

ARRAY_DEF(TextScrollLineArray, TextScrollLineArray, M_POD_OPLIST) //-V658

typedef struct {
    TextScrollLineArray_t line_array;
    uint8_t x;
    uint8_t y;
    uint8_t width;
    uint8_t height;
    FuriString* text;
    uint16_t scroll_pos_total;
    uint16_t scroll_pos_current;
    bool text_formatted;

    // With the screen reader listening at the first draw, the text is read by stops, for the
    // element's life: formatting uses the text up. See text_stops.h
    bool reader;
    bool opening; /**< still the opening, which reads opening_taken stops at once */
    uint16_t stop_current; /**< the stop being read after the opening, from 1 */
    uint16_t stops_total;
    uint16_t opening_taken;
} WidgetElementTextScrollModel;

static bool
    widget_element_text_scroll_process_ctrl_symbols(TextScrollLineArray* line, FuriString* text) {
    bool processed = false;

    do {
        if(furi_string_get_char(text, 0) != '\e') break;
        char ctrl_symbol = furi_string_get_char(text, 1);
        if(ctrl_symbol == 'c') {
            line->horizontal = AlignCenter;
        } else if(ctrl_symbol == 'r') {
            line->horizontal = AlignRight;
        } else if(ctrl_symbol == '#') {
            line->font = FontPrimary;
        } else if(ctrl_symbol == '*') {
            line->font = FontKeyboard;
        }
        furi_string_right(text, 2);
        processed = true;
    } while(false);

    return processed;
}

void widget_element_text_scroll_add_line(WidgetElement* element, TextScrollLineArray* line) {
    WidgetElementTextScrollModel* model = element->model;
    TextScrollLineArray new_line;
    new_line.font = line->font;
    new_line.horizontal = line->horizontal;
    new_line.text = furi_string_alloc_set(line->text);
    new_line.stop = line->stop;
    new_line.join = line->join;
    TextScrollLineArray_push_back(model->line_array, new_line);
}

static void widget_element_text_scroll_fill_lines(Canvas* canvas, WidgetElement* element) {
    WidgetElementTextScrollModel* model = element->model;
    TextScrollLineArray line_tmp;
    bool all_text_processed = false;
    line_tmp.text = furi_string_alloc();
    line_tmp.stop = 0;
    line_tmp.join = 0;
    bool reached_new_line = true;
    uint16_t total_height = 0;

    while(!all_text_processed) {
        if(reached_new_line) {
            // Set default line properties
            line_tmp.font = FontSecondary;
            line_tmp.horizontal = AlignLeft;
            furi_string_reset(line_tmp.text);
            // Process control symbols
            while(widget_element_text_scroll_process_ctrl_symbols(&line_tmp, model->text))
                ;
        }
        // Set canvas font
        canvas_set_font(canvas, line_tmp.font);
        const CanvasFontParameters* params = canvas_get_font_params(canvas, line_tmp.font);
        total_height += params->height;
        if(total_height > model->height) {
            model->scroll_pos_total++;
        }

        uint8_t line_width = 0;
        uint16_t char_i = 0;
        while(true) {
            char next_char = furi_string_get_char(model->text, char_i++);
            if(next_char == '\0') {
                furi_string_push_back(line_tmp.text, '\0');
                widget_element_text_scroll_add_line(element, &line_tmp);
                total_height += params->leading_default - params->height;
                all_text_processed = true;
                break;
            } else if(next_char == '\n') {
                furi_string_push_back(line_tmp.text, '\0');
                widget_element_text_scroll_add_line(element, &line_tmp);
                furi_string_right(model->text, char_i);
                total_height += params->leading_default - params->height;
                reached_new_line = true;
                break;
            } else {
                line_width += canvas_glyph_width(canvas, next_char);
                if(line_width > model->width) {
                    furi_string_push_back(line_tmp.text, '\0');
                    widget_element_text_scroll_add_line(element, &line_tmp);
                    furi_string_right(model->text, char_i - 1);
                    furi_string_reset(line_tmp.text);
                    total_height += params->leading_default - params->height;
                    reached_new_line = false;
                    break;
                } else {
                    furi_string_push_back(line_tmp.text, next_char);
                }
            }
        }
    }

    furi_string_free(line_tmp.text);
}

static uint8_t widget_element_text_scroll_glyph_width(char c, void* context) {
    return (uint8_t)canvas_glyph_width(context, (uint8_t)c);
}

/** With the reader on: rows wrapped at spaces, each knowing its stop, and how many stops the
 *  opening reads. */
static void widget_element_text_scroll_fill_stops(Canvas* canvas, WidgetElement* element) {
    WidgetElementTextScrollModel* model = element->model;
    const CanvasFontParameters* secondary = canvas_get_font_params(canvas, FontSecondary);
    const TextStopsLayout layout = {
        .glyph_width = widget_element_text_scroll_glyph_width,
        .context = canvas,
        .row_width = model->width,
        .rows_max = MAX(model->height / secondary->leading_default, 1),
    };
    TextScrollLineArray line;
    line.text = furi_string_alloc();
    TextStopsGroup group = {0};
    uint16_t stops = 0, taken = 0;
    size_t stop_chars = 0, used_chars = 0;
    bool taking = true;
    TextStopsRow row;

    do {
        // As the plain formatting: each printed line starts with its own font and alignment
        line.font = FontSecondary;
        line.horizontal = AlignLeft;
        while(widget_element_text_scroll_process_ctrl_symbols(&line, model->text))
            ;
        canvas_set_font(canvas, line.font);
        const char* text = furi_string_get_cstr(model->text);
        const bool silent = text_stops_line_is_silent(text, strcspn(text, "\n"));
        TextStopsBreak before = TextStopsBreakLine;
        bool line_start = true;
        size_t pos = 0;
        do {
            text_stops_row(text, pos, &layout, &row);
            pos = row.next;
            // A break just before the line's end leaves no empty row
            if(row.length == 0 && !line_start) continue;
            line.stop = 0;
            line.join = 0;
            if(!silent) {
                if(text_stops_group_row(&group, row.length, line_start, &layout)) {
                    if(stops > 0 && taking) {
                        taking = text_stops_opening_takes(taken, used_chars, stop_chars);
                        if(taking) {
                            taken++;
                            used_chars += stop_chars;
                        }
                    }
                    stops++;
                    stop_chars = row.length;
                } else {
                    line.join = before == TextStopsBreakSpace ? CANVAS_TAP_JOIN_SPACE :
                                                                CANVAS_TAP_JOIN_DIRECT;
                    stop_chars += row.length + (before == TextStopsBreakSpace ? 1 : 0);
                }
                line.stop = stops;
            }
            furi_string_set_strn(line.text, text + row.start, row.length);
            widget_element_text_scroll_add_line(element, &line);
            before = row.ends;
            line_start = false;
        } while(row.ends != TextStopsBreakLine && row.ends != TextStopsBreakEnd);
        furi_string_right(model->text, pos);
    } while(row.ends != TextStopsBreakEnd);

    if(stops > 0 && taking && text_stops_opening_takes(taken, used_chars, stop_chars)) taken++;
    furi_string_free(line.text);
    model->stops_total = stops;
    model->opening_taken = taken;
    model->opening = true;
    model->stop_current = 0;
}

/** With the reader on: the opening's stops, or the current stop, for the reader, and the display
 *  from that stop's first row. What fits in the opening but not on the display is reported without
 *  being drawn. */
static void
    widget_element_text_scroll_draw_stops(Canvas* canvas, WidgetElementTextScrollModel* model) {
    int32_t y = model->y;
    int32_t x = model->x;
    bool started = model->opening;
    bool past_display = false;
    TextScrollLineArray_it_t it;
    for(TextScrollLineArray_it(it, model->line_array); !TextScrollLineArray_end_p(it);
        TextScrollLineArray_next(it)) {
        TextScrollLineArray* line = TextScrollLineArray_ref(it);
        if(!started) {
            if(line->stop != model->stop_current) continue;
            started = true;
        }
        const CanvasFontParameters* params = canvas_get_font_params(canvas, line->font);
        past_display = past_display || y + params->descender > model->y + model->height;
        bool read = model->opening ? (line->stop >= 1 && line->stop <= model->opening_taken) :
                                     line->stop == model->stop_current;
        if(past_display && (!model->opening || line->stop > model->opening_taken)) break;
        if(past_display && !read) continue;

        canvas_set_font(canvas, line->font);
        if(line->horizontal == AlignLeft) {
            x = model->x;
        } else if(line->horizontal == AlignCenter) {
            x = (model->x + model->width) / 2;
        } else if(line->horizontal == AlignRight) {
            x = model->x + model->width;
        }
        if(read) {
            canvas_tap_hint_reading(
                canvas, model->opening ? CANVAS_TAP_READING_OPEN : CANVAS_TAP_READING_STOP);
            if(line->join != 0) {
                canvas_tap_hint_join(canvas, line->join);
            } else if(!model->opening) {
                canvas_tap_hint_focus(canvas, model->stop_current, model->stops_total);
            }
        } else {
            canvas_tap_hint_full_text(canvas, "", false);
        }
        const char* text = furi_string_get_cstr(line->text);
        if(past_display) {
            canvas_tap_note(canvas, x, y + params->height, text);
        } else {
            canvas_draw_str_aligned(canvas, x, y, line->horizontal, AlignTop, text);
        }
        y += params->leading_default;
    }

    if(model->opening && model->opening_taken < model->stops_total) {
        char position[16];
        snprintf(
            position,
            sizeof(position),
            "%u of %u",
            (unsigned)model->opening_taken,
            (unsigned)model->stops_total);
        canvas_tap_hint_reading(canvas, CANVAS_TAP_READING_POSITION);
        canvas_tap_note(canvas, model->x, y + canvas_current_font_height(canvas), position);
    }

    if(model->stops_total > 1) {
        elements_scrollbar_pos(
            canvas,
            model->x + model->width + WIDGET_ELEMENT_TEXT_SCROLL_BAR_OFFSET,
            model->y,
            model->height,
            model->opening ? 0 : model->stop_current - 1,
            model->stops_total);
    }
}

static void widget_element_text_scroll_draw(Canvas* canvas, WidgetElement* element) {
    furi_assert(canvas);
    furi_assert(element);

    furi_mutex_acquire(element->model_mutex, FuriWaitForever);

    WidgetElementTextScrollModel* model = element->model;
    if(!model->text_formatted) {
        model->reader = canvas->tap_callback != NULL;
        if(model->reader) {
            widget_element_text_scroll_fill_stops(canvas, element);
        } else {
            widget_element_text_scroll_fill_lines(canvas, element);
        }
        model->text_formatted = true;
    }
    if(model->reader) {
        widget_element_text_scroll_draw_stops(canvas, model);
        furi_mutex_release(element->model_mutex);
        return;
    }

    uint8_t y = model->y;
    uint8_t x = model->x;
    uint16_t curr_line = 0;
    if(TextScrollLineArray_size(model->line_array)) {
        TextScrollLineArray_it_t it;
        for(TextScrollLineArray_it(it, model->line_array); !TextScrollLineArray_end_p(it);
            TextScrollLineArray_next(it), curr_line++) {
            if(curr_line < model->scroll_pos_current) continue;
            TextScrollLineArray* line = TextScrollLineArray_ref(it);
            const CanvasFontParameters* params = canvas_get_font_params(canvas, line->font);
            if(y + params->descender > model->y + model->height) break;
            canvas_set_font(canvas, line->font);
            if(line->horizontal == AlignLeft) {
                x = model->x;
            } else if(line->horizontal == AlignCenter) {
                x = (model->x + model->width) / 2;
            } else if(line->horizontal == AlignRight) {
                x = model->x + model->width;
            }
            canvas_draw_str_aligned(
                canvas, x, y, line->horizontal, AlignTop, furi_string_get_cstr(line->text));
            y += params->leading_default;
        }
        // Draw scroll bar
        if(model->scroll_pos_total > 1) {
            elements_scrollbar_pos(
                canvas,
                model->x + model->width + WIDGET_ELEMENT_TEXT_SCROLL_BAR_OFFSET,
                model->y,
                model->height,
                model->scroll_pos_current,
                model->scroll_pos_total);
        }
    }

    furi_mutex_release(element->model_mutex);
}

static bool widget_element_text_scroll_input(InputEvent* event, WidgetElement* element) {
    furi_assert(event);
    furi_assert(element);

    furi_mutex_acquire(element->model_mutex, FuriWaitForever);

    WidgetElementTextScrollModel* model = element->model;
    bool consumed = false;

    if((event->type == InputTypeShort) || (event->type == InputTypeRepeat)) {
        if(model->reader) {
            if(event->key == InputKeyUp || event->key == InputKeyDown) {
                text_stops_opening_step(
                    &model->opening,
                    &model->stop_current,
                    model->opening_taken,
                    model->stops_total,
                    event->key == InputKeyUp ? -1 : 1);
                consumed = true;
            }
        } else if(event->key == InputKeyUp) {
            if(model->scroll_pos_current > 0) {
                model->scroll_pos_current--;
            }
            consumed = true;
        } else if(event->key == InputKeyDown) {
            if((model->scroll_pos_total > 1) &&
               (model->scroll_pos_current < model->scroll_pos_total - 1)) {
                model->scroll_pos_current++;
            }
            consumed = true;
        }
    }

    furi_mutex_release(element->model_mutex);

    return consumed;
}

static void widget_element_text_scroll_free(WidgetElement* text_scroll) {
    furi_assert(text_scroll);

    WidgetElementTextScrollModel* model = text_scroll->model;
    TextScrollLineArray_it_t it;
    for(TextScrollLineArray_it(it, model->line_array); !TextScrollLineArray_end_p(it);
        TextScrollLineArray_next(it)) {
        TextScrollLineArray* line = TextScrollLineArray_ref(it);
        furi_string_free(line->text);
    }
    TextScrollLineArray_clear(model->line_array);
    furi_string_free(model->text);
    free(text_scroll->model);
    furi_mutex_free(text_scroll->model_mutex);
    free(text_scroll);
}

WidgetElement* widget_element_text_scroll_create(
    uint8_t x,
    uint8_t y,
    uint8_t width,
    uint8_t height,
    const char* text) {
    furi_assert(text);

    // Allocate and init model
    WidgetElementTextScrollModel* model = malloc(sizeof(WidgetElementTextScrollModel));
    model->x = x;
    model->y = y;
    model->width = width - WIDGET_ELEMENT_TEXT_SCROLL_BAR_OFFSET;
    model->height = height;
    model->scroll_pos_current = 0;
    model->scroll_pos_total = 1;
    model->text_formatted = false;
    model->reader = false;
    model->opening = false;
    model->stop_current = 0;
    model->stops_total = 0;
    model->opening_taken = 0;
    TextScrollLineArray_init(model->line_array);
    model->text = furi_string_alloc_set(text);

    WidgetElement* text_scroll = malloc(sizeof(WidgetElement));
    text_scroll->parent = NULL;
    text_scroll->draw = widget_element_text_scroll_draw;
    text_scroll->input = widget_element_text_scroll_input;
    text_scroll->free = widget_element_text_scroll_free;
    text_scroll->model = model;
    text_scroll->model_mutex = furi_mutex_alloc(FuriMutexTypeNormal);

    return text_scroll;
} //-V773
