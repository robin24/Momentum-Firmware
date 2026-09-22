#include "screen_model.h"
#include <string.h>
#include <stdio.h>

#define SR_ROW_Y_TOLERANCE 2
#define SR_BUTTON_ROW_Y    54
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
    return r->inverted && !r->focus && r->y >= SR_BUTTON_ROW_Y &&
           r->font == SrFontSecondary && !sr_record_is_status(r);
}

static bool sr_is_arrow_token(const char* text) {
    return strcmp(text, "<") == 0 || strcmp(text, ">") == 0;
}

void sr_screen_build(const SrFrame* frame, SrScreen* screen) {
    memset(screen, 0, sizeof(*screen));
    screen->title_row = -1;
    screen->content_layer = frame->content_layer;
    screen->overflow = frame->overflow;

    // Reading order: by baseline, then by x. Insertion sort on indices.
    uint8_t order[SR_MAX_RECORDS];
    uint8_t n = frame->count > SR_MAX_RECORDS ? SR_MAX_RECORDS : frame->count;
    for(uint8_t i = 0; i < n; i++) {
        uint8_t j = i;
        while(j > 0) {
            const SrRecord* a = &frame->records[order[j - 1]];
            const SrRecord* b = &frame->records[i];
            bool after = (a->y > b->y) || (a->y == b->y && a->x > b->x);
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
        if(text[0] == '\0' || sr_is_arrow_token(text)) continue;

        bool status = sr_record_is_status(r);
        bool button = sr_record_is_button(r);
        bool same_row = row && !button && row->kind != SrRowButton &&
                        (status ? row->kind == SrRowStatus : row->kind != SrRowStatus) &&
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
            row->kind = status ? SrRowStatus : (button ? SrRowButton : SrRowNormal);
        }
        if(row->text[0] != '\0') sr_append(row->text, sizeof(row->text), " ");
        sr_append(row->text, sizeof(row->text), text);

        bool focus = r->focus || (!any_focus_hint && r->inverted && !button && !status);
        if(focus && row->kind == SrRowNormal) row->kind = SrRowFocus;
        if(r->count && !row->count) {
            row->index = r->index;
            row->count = r->count;
        }
    }

    for(uint8_t i = 0; i < screen->row_count; i++) {
        const SrRow* c = &screen->rows[i];
        if(c->kind == SrRowNormal && c->font == SrFontPrimary && c->y <= SR_TITLE_MAX_Y) {
            screen->title_row = (int8_t)i;
            break;
        }
    }
}

size_t sr_screen_focus_text(const SrScreen* screen, char* out, size_t out_size) {
    (void)screen;
    if(out_size) out[0] = '\0';
    return 0;
}

size_t sr_screen_describe(const SrScreen* screen, char* out, size_t out_size) {
    (void)screen;
    if(out_size) out[0] = '\0';
    return 0;
}

void sr_model_init(SrModel* model, uint8_t verbosity) {
    memset(model, 0, sizeof(*model));
    model->verbosity = verbosity;
}

size_t sr_model_process(
    SrModel* model,
    const SrFrame* frame,
    uint32_t now_ms,
    bool key_recent,
    SrAnnouncement* out,
    size_t out_max) {
    (void)model;
    (void)frame;
    (void)now_ms;
    (void)key_recent;
    (void)out;
    (void)out_max;
    return 0;
}
