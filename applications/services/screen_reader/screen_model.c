#include "screen_model.h"
#include <string.h>
#include <stdio.h>

#define SR_ROW_Y_TOLERANCE 2
// The dialog button helpers draw their label at y=61; four row lists still draw their last
// baseline at y=60, so only y=61 and below is a button by geometry.
#define SR_BUTTON_ROW_Y    61
#define SR_TITLE_MAX_Y     26
#define SR_SPONTANEOUS_MS  1500
#define SR_SPONTANEOUS_MAX 80

static void sr_copy(char* out, size_t out_size, const char* in) {
    if(out_size == 0) return;
    size_t i = 0;
    while(i + 1 < out_size && in[i] != '\0') {
        out[i] = in[i];
        i++;
    }
    out[i] = '\0';
}

static void sr_append(char* out, size_t out_size, const char* in) {
    size_t len = strlen(out);
    if(len + 1 >= out_size) return;
    sr_copy(out + len, out_size - len, in);
}

void sr_normalize(const char* in, char* out, size_t out_size) {
    if(out_size == 0) return;
    size_t o = 0;
    bool pending_space = false;
    for(const char* p = in; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if(c == '|') continue;
        if(c <= ' ' || c > 0x7E) {
            if(o > 0) pending_space = true;
            continue;
        }
        if(pending_space) {
            if(o + 1 < out_size) out[o++] = ' ';
            pending_space = false;
        }
        if(o + 1 < out_size) out[o++] = (char)c;
    }
    out[o] = '\0';
}

static bool sr_record_is_status(const SrRecord* r) {
    return r->layer == SrLayerStatusLeft || r->layer == SrLayerStatusRight;
}

static bool sr_record_is_button(const SrRecord* r) {
    if(sr_record_is_status(r)) return false;
    if(r->button != 0) return true;
    // No hint: guess from the geometry, for third party apps.
    return r->inverted && !r->focus && r->y >= SR_BUTTON_ROW_Y && r->font == SrFontSecondary;
}

static SrRowKind sr_record_row_kind(bool status, bool button, bool focus) {
    if(status) return SrRowStatus;
    if(button) return SrRowButton;
    return focus ? SrRowFocus : SrRowNormal;
}

static bool sr_is_arrow_token(const char* text) {
    return strcmp(text, "<") == 0 || strcmp(text, ">") == 0;
}

/** A file browser row that is still loading is drawn as a run of dashes. */
static bool sr_is_dashes(const char* text) {
    if(*text == '\0') return false;
    for(const char* p = text; *p; p++) {
        if(*p != '-') return false;
    }
    return true;
}

void sr_screen_build(const SrFrame* frame, SrScreen* screen) {
    memset(screen, 0, sizeof(*screen));
    screen->title_row = -1;
    screen->content_layer = frame->content_layer;
    screen->overflow = frame->overflow;

    // Reading order: notes first (they have no position, and drawn text may pass above the top
    // edge, as the lock screen's sliding cover does), then by baseline, then by x. Insertion sort
    // on indices.
    uint8_t order[SR_MAX_RECORDS];
    uint8_t n = frame->count > SR_MAX_RECORDS ? SR_MAX_RECORDS : frame->count;
    for(uint8_t i = 0; i < n; i++) {
        uint8_t j = i;
        while(j > 0) {
            const SrRecord* a = &frame->records[order[j - 1]];
            const SrRecord* b = &frame->records[i];
            bool after = a->note != b->note ? b->note :
                                              (a->y > b->y) || (a->y == b->y && a->x > b->x);
            if(!after) break;
            order[j] = order[j - 1];
            j--;
        }
        order[j] = i;
    }

    bool any_focus_hint = false;
    for(uint8_t i = 0; i < n; i++) {
        const SrRecord* r = &frame->records[i];
        if(r->focus) any_focus_hint = true;
        if(!sr_record_is_status(r)) screen->has_content = true;
        if(r->font == SrFontKeyboard && !sr_record_is_status(r)) screen->has_keyboard = true;
    }

    SrRow* row = NULL;
    for(uint8_t k = 0; k < n; k++) {
        const SrRecord* r = &frame->records[order[k]];
        char text[SR_TEXT_MAX];
        sr_normalize(r->text, text, sizeof(text));
        if(strcmp(text, ". .") == 0) sr_copy(text, sizeof(text), "Parent folder");
        if(text[0] == '\0' || sr_is_arrow_token(text) || sr_is_dashes(text)) continue;

        bool status = sr_record_is_status(r);
        bool button = sr_record_is_button(r);
        bool focus = r->focus || (!any_focus_hint && r->inverted && !button && !status);
        // A record only joins the row above it when it reads the same way. On the on screen
        // keyboard every key of a row shares one baseline, so the selected key is its own row.
        // Buttons and status items never join: the clock and the battery percentage sit a
        // pixel apart on the status bar and are read as two items. Notes never join either:
        // they sit at y 0, where drawn text only passes by (the lock screen's sliding cover).
        SrRowKind kind = sr_record_row_kind(status, button, focus);
        bool same_row = row && !r->note && !row->note && kind != SrRowButton &&
                        kind != SrRowStatus && row->kind == kind &&
                        (r->y - row->y) <= SR_ROW_Y_TOLERANCE &&
                        (row->y - r->y) <= SR_ROW_Y_TOLERANCE;
        if(!same_row) {
            if(screen->row_count >= SR_MAX_ROWS) {
                screen->overflow = true;
                break;
            }
            row = &screen->rows[screen->row_count++];
            memset(row, 0, sizeof(*row));
            row->x = r->x;
            row->y = r->y;
            row->font = r->font;
            row->note = r->note;
            row->kind = kind;
            row->button = r->button;
        }
        if(row->text[0] != '\0') sr_append(row->text, sizeof(row->text), " ");
        sr_append(row->text, sizeof(row->text), text);

        if(r->count && !row->count) {
            row->index = r->index;
            row->count = r->count;
        }
    }

    for(uint8_t i = 0; i < screen->row_count; i++) {
        const SrRow* c = &screen->rows[i];
        if(c->kind == SrRowNormal && !c->note && c->font == SrFontPrimary &&
           c->y <= SR_TITLE_MAX_Y) {
            screen->title_row = (int8_t)i;
            break;
        }
    }
}

size_t sr_screen_focus_text(const SrScreen* screen, char* out, size_t out_size) {
    if(out_size == 0) return 0;
    out[0] = '\0';
    for(uint8_t i = 0; i < screen->row_count; i++) {
        const SrRow* row = &screen->rows[i];
        if(row->kind != SrRowFocus) continue;
        if(out[0] != '\0') sr_append(out, out_size, ", ");
        sr_append(out, out_size, row->text);
    }
    return strlen(out);
}

static void sr_append_position(char* out, size_t out_size, const SrRow* row) {
    if(!row->count) return;
    char pos[32];
    snprintf(pos, sizeof(pos), ", %u of %u", (unsigned)row->index, (unsigned)row->count);
    sr_append(out, out_size, pos);
}

/** The position of the first focus row that knows it, as ", 3 of 11"; nothing when none does. */
static void sr_append_focus_position(const SrScreen* screen, char* out, size_t out_size) {
    for(uint8_t i = 0; i < screen->row_count; i++) {
        if(screen->rows[i].kind == SrRowFocus && screen->rows[i].count) {
            sr_append_position(out, out_size, &screen->rows[i]);
            return;
        }
    }
}

size_t sr_focus_with_position(const SrScreen* screen, char* out, size_t out_size) {
    if(sr_screen_focus_text(screen, out, out_size) == 0) return 0;
    sr_append_focus_position(screen, out, out_size);
    return strlen(out);
}

static bool sr_row_is_battery(const SrRow* row) {
    return row->kind == SrRowStatus && row->font == SrFontBatteryPercent;
}

size_t sr_status_text(const SrScreen* screen, char* out, size_t out_size) {
    if(out_size == 0) return 0;
    out[0] = '\0';
    // The battery font draws only the charge's digits; the bar style draws them in two colours
    // as two strings, so all of them make one number, said where the first one sits
    char battery[16] = "";
    for(uint8_t i = 0; i < screen->row_count; i++) {
        if(sr_row_is_battery(&screen->rows[i])) {
            sr_append(battery, sizeof(battery), screen->rows[i].text);
        }
    }
    bool battery_said = false;
    for(uint8_t i = 0; i < screen->row_count; i++) {
        const SrRow* row = &screen->rows[i];
        if(row->kind != SrRowStatus) continue;
        bool is_battery = sr_row_is_battery(row);
        if(is_battery && battery_said) continue;
        if(out[0] != '\0') sr_append(out, out_size, ", ");
        if(is_battery) {
            battery_said = true;
            sr_append(out, out_size, "battery ");
            sr_append(out, out_size, battery);
            sr_append(out, out_size, " percent");
        } else {
            sr_append(out, out_size, row->text);
        }
    }
    return strlen(out);
}

static const char* sr_button_side(const SrRow* row) {
    if(row->button == 1) return "left";
    if(row->button == 2) return "center";
    if(row->button == 3) return "right";
    // No hint: guess from where the label sits, for third party apps.
    if(row->x < 40) return "left";
    if(row->x > 88) return "right";
    return "center";
}

static void sr_append_buttons(const SrScreen* screen, char* out, size_t out_size) {
    bool first = true;
    for(uint8_t i = 0; i < screen->row_count; i++) {
        const SrRow* row = &screen->rows[i];
        if(row->kind != SrRowButton) continue;
        if(first) {
            if(out[0] != '\0') sr_append(out, out_size, ". ");
            sr_append(out, out_size, "buttons: ");
            first = false;
        } else {
            sr_append(out, out_size, ", ");
        }
        sr_append(out, out_size, sr_button_side(row));
        sr_append(out, out_size, " ");
        sr_append(out, out_size, row->text);
    }
}

size_t sr_screen_describe(const SrScreen* screen, char* out, size_t out_size) {
    if(out_size == 0) return 0;
    out[0] = '\0';
    if(screen->title_row >= 0) {
        sr_append(out, out_size, screen->rows[screen->title_row].text);
    }
    for(uint8_t i = 0; i < screen->row_count; i++) {
        const SrRow* row = &screen->rows[i];
        if(row->kind != SrRowNormal || (int8_t)i == screen->title_row) continue;
        if(out[0] != '\0') sr_append(out, out_size, ". ");
        sr_append(out, out_size, row->text);
    }
    for(uint8_t i = 0; i < screen->row_count; i++) {
        const SrRow* row = &screen->rows[i];
        if(row->kind != SrRowFocus) continue;
        if(out[0] != '\0') sr_append(out, out_size, ". ");
        sr_append(out, out_size, row->text);
        sr_append_position(out, out_size, row);
    }
    sr_append_buttons(screen, out, out_size);
    if(screen->overflow) sr_append(out, out_size, ", and more");
    return strlen(out);
}

void sr_model_init(SrModel* model, uint8_t verbosity) {
    memset(model, 0, sizeof(*model));
    model->verbosity = verbosity;
}

static void sr_emit(
    SrAnnouncement* out,
    size_t out_max,
    size_t* n,
    SrAnnKind kind,
    bool interrupt,
    const char* text) {
    if(*n >= out_max) return;
    SrAnnouncement* a = &out[*n];
    a->kind = kind;
    a->interrupt = interrupt;
    sr_copy(a->text, sizeof(a->text), text);
    (*n)++;
}

static bool sr_screen_has_text(const SrScreen* screen, const char* text) {
    for(uint8_t i = 0; i < screen->row_count; i++) {
        if(screen->rows[i].kind == SrRowStatus) continue;
        if(strcmp(screen->rows[i].text, text) == 0) return true;
    }
    return false;
}

/** Any row outside the status bar: text drawn by the screen, or a note. */
static bool sr_screen_has_rows(const SrScreen* screen) {
    for(uint8_t i = 0; i < screen->row_count; i++) {
        if(screen->rows[i].kind != SrRowStatus) return true;
    }
    return false;
}

static const char* sr_title_text(const SrScreen* screen) {
    return screen->title_row >= 0 ? screen->rows[screen->title_row].text : "";
}

static bool sr_screen_changed(const SrScreen* prev, const SrScreen* cur) {
    if(prev->content_layer != cur->content_layer) return true;
    if(prev->has_content != cur->has_content) return true;
    if(strcmp(sr_title_text(prev), sr_title_text(cur)) != 0) return true;
    // Fewer than half of the previous rows still present, ignoring their roles: a list
    // that scrolled by one keeps most rows, a new screen keeps almost none.
    unsigned total = 0, kept = 0;
    for(uint8_t i = 0; i < prev->row_count; i++) {
        const SrRow* row = &prev->rows[i];
        if(row->kind == SrRowStatus) continue;
        total++;
        if(sr_screen_has_text(cur, row->text)) kept++;
    }
    return total > 0 && kept * 2 < total;
}

static void
    sr_screen_announcement(const SrModel* model, const SrScreen* s, char* out, size_t out_size) {
    out[0] = '\0';
    char focus[SR_ANN_TEXT_MAX];
    sr_screen_focus_text(s, focus, sizeof(focus));
    if(s->title_row >= 0) sr_append(out, out_size, sr_title_text(s));
    if(focus[0] != '\0') {
        if(out[0] != '\0') sr_append(out, out_size, ". ");
        sr_append(out, out_size, focus);
        if(model->verbosity >= 2) sr_append_focus_position(s, out, out_size);
    } else {
        for(uint8_t i = 0; i < s->row_count; i++) {
            const SrRow* row = &s->rows[i];
            if(row->kind != SrRowNormal || (int8_t)i == s->title_row) continue;
            if(out[0] != '\0') sr_append(out, out_size, ". ");
            sr_append(out, out_size, row->text);
        }
    }
    if(model->verbosity >= 1) sr_append_buttons(s, out, out_size);
    if(s->overflow) sr_append(out, out_size, ", and more");
}

static bool
    sr_typed_character(const SrScreen* prev, const SrScreen* cur, char* out, size_t out_size) {
    if(!cur->has_keyboard || !prev->has_keyboard) return false;
    for(uint8_t i = 0; i < cur->row_count; i++) {
        const SrRow* now = &cur->rows[i];
        if(now->kind != SrRowNormal) continue;
        for(uint8_t j = 0; j < prev->row_count; j++) {
            const SrRow* was = &prev->rows[j];
            if(was->kind != SrRowNormal) continue;
            if((now->y - was->y) > SR_ROW_Y_TOLERANCE || (was->y - now->y) > SR_ROW_Y_TOLERANCE)
                continue;
            size_t ln = strlen(now->text), lw = strlen(was->text);
            if(ln == lw + 1 && strncmp(now->text, was->text, lw) == 0) {
                char c = now->text[lw];
                if(c == ' ') {
                    sr_copy(out, out_size, "space");
                } else {
                    out[0] = c;
                    out[1] = '\0';
                }
                return true;
            }
            if(lw == ln + 1 && strncmp(now->text, was->text, ln) == 0) {
                sr_copy(out, out_size, "deleted");
                return true;
            }
        }
    }
    return false;
}

size_t sr_model_process(
    SrModel* model,
    const SrFrame* frame,
    uint32_t now_ms,
    bool key_recent,
    SrAnnouncement* out,
    size_t out_max) {
    size_t n = 0;
    SrScreen* cur = &model->current;
    sr_screen_build(frame, cur);

    char text[SR_ANN_TEXT_MAX];
    bool first = !model->have_prev;
    // The desktop keeps redrawing its dolphin and its speech bubbles. Staying on it is never
    // a new screen, so bubble text arrives as a rate limited change. The desktop's own screens
    // (the lock menu, the lock screen, the PIN entry, the power off dialog) stay on it too.
    bool stay_on_desktop = !first && model->prev.content_layer == SrLayerDesktop &&
                           cur->content_layer == SrLayerDesktop;
    bool changed = !stay_on_desktop && (first || sr_screen_changed(&model->prev, cur));

    if(changed) {
        if(cur->has_content) {
            sr_screen_announcement(model, cur, text, sizeof(text));
            sr_emit(out, out_max, &n, SrAnnScreen, true, text);
        } else if(cur->content_layer == SrLayerDesktop) {
            sr_emit(out, out_max, &n, SrAnnHome, true, "Home screen");
        }
    } else if(stay_on_desktop && sr_screen_has_rows(&model->prev) && !sr_screen_has_rows(cur)) {
        // Still on the desktop and its own text went away: one of its screens closed, or a
        // bubble vanished. Home again, said once and after what is being said (an "Unlocked");
        // the service keeps it silent without a recent key press, as it keeps the bubbles
        sr_emit(out, out_max, &n, SrAnnHome, false, "Home screen");
    } else {
        char focus_now[SR_ANN_TEXT_MAX];
        char focus_was[SR_ANN_TEXT_MAX];
        sr_screen_focus_text(cur, focus_now, sizeof(focus_now));
        sr_screen_focus_text(&model->prev, focus_was, sizeof(focus_was));
        if(focus_now[0] != '\0' && strcmp(focus_now, focus_was) != 0) {
            sr_copy(text, sizeof(text), focus_now);
            if(model->verbosity >= 2) sr_append_focus_position(cur, text, sizeof(text));
            sr_emit(out, out_max, &n, SrAnnFocus, true, text);
        } else if(sr_typed_character(&model->prev, cur, text, sizeof(text))) {
            sr_emit(out, out_max, &n, SrAnnTyped, true, text);
        } else {
            text[0] = '\0';
            for(uint8_t i = 0; i < cur->row_count; i++) {
                const SrRow* row = &cur->rows[i];
                if(row->kind != SrRowNormal) continue;
                if(sr_screen_has_text(&model->prev, row->text)) continue;
                if(text[0] != '\0') sr_append(text, sizeof(text), ". ");
                sr_append(text, sizeof(text), row->text);
            }
            if(text[0] != '\0') {
                bool allowed = key_recent ||
                               (now_ms - model->last_spontaneous_ms) >= SR_SPONTANEOUS_MS;
                if(allowed) {
                    if(!key_recent) {
                        model->last_spontaneous_ms = now_ms;
                        if(strlen(text) > SR_SPONTANEOUS_MAX) text[SR_SPONTANEOUS_MAX] = '\0';
                    }
                    sr_emit(out, out_max, &n, SrAnnChange, false, text);
                }
            }
        }
    }

    memcpy(&model->prev, cur, sizeof(SrScreen));
    model->have_prev = true;
    return n;
}
