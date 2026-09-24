#include "sr_throttle.h"

#include <string.h>

void sr_throttle_init(SrThrottle* t) {
    memset(t, 0, sizeof(*t));
}

// The interval has passed since the last change was said; unsigned subtraction counts the time
// correctly across the tick's wrap
static bool sr_throttle_elapsed(const SrThrottle* t, uint32_t now_ms, uint32_t interval_ms) {
    return !t->said || (uint32_t)(now_ms - t->last_said_ms) >= interval_ms;
}

static void sr_throttle_said(SrThrottle* t, uint32_t now_ms) {
    t->pending = false;
    t->text[0] = '\0';
    t->said = true;
    t->last_said_ms = now_ms;
}

bool sr_throttle_offer(SrThrottle* t, const char* text, uint32_t now_ms, uint32_t interval_ms) {
    if(sr_throttle_elapsed(t, now_ms, interval_ms)) {
        sr_throttle_said(t, now_ms);
        return true;
    }
    strncpy(t->text, text, sizeof(t->text) - 1);
    t->text[sizeof(t->text) - 1] = '\0';
    t->pending = true;
    return false;
}

bool sr_throttle_due(
    SrThrottle* t,
    uint32_t now_ms,
    uint32_t interval_ms,
    char* out,
    size_t out_size) {
    if(!t->pending || !sr_throttle_elapsed(t, now_ms, interval_ms)) return false;
    if(out_size > 0) {
        strncpy(out, t->text, out_size - 1);
        out[out_size - 1] = '\0';
    }
    sr_throttle_said(t, now_ms);
    return true;
}

uint32_t sr_throttle_wait_ms(const SrThrottle* t, uint32_t now_ms, uint32_t interval_ms) {
    if(!t->pending || !t->said) return 0;
    uint32_t elapsed = (uint32_t)(now_ms - t->last_said_ms);
    return elapsed >= interval_ms ? 0 : interval_ms - elapsed;
}

void sr_throttle_clear(SrThrottle* t) {
    t->pending = false;
    t->text[0] = '\0';
}
