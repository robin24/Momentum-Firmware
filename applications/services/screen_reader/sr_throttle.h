/**
 * @file sr_throttle.h
 * Screen reader: the spacing of same-screen changes, such as a progress bar's percentages. A
 * change is said at once when the change delay has passed since the last change was said;
 * otherwise it is held, a newer change replacing it, and said when its time comes, so what is
 * said is always the latest text. Pure C, host tested; the service thread owns it. Times are
 * milliseconds of a 32 bit tick that wraps: every difference is taken modulo 2^32.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "screen_model.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char text[SR_ANN_TEXT_MAX]; /**< the held change while pending, else empty */
    bool pending; /**< a change is held */
    bool said; /**< a change was said since init; until then a change is said at once */
    uint32_t last_said_ms; /**< when the last change was said, at once or when due */
} SrThrottle;

/** Nothing held, no change said yet. */
void sr_throttle_init(SrThrottle* t);

/** A change on the same screen. True: say it now; now_ms is recorded as the time the last change
 *  was said, and a change still held from before is dropped. False: it is held, replacing any
 *  change held before, until interval_ms have passed since the last change was said. The text
 *  is cut at SR_ANN_TEXT_MAX - 1 characters. An interval of 0 says every change at once. */
bool sr_throttle_offer(SrThrottle* t, const char* text, uint32_t now_ms, uint32_t interval_ms);

/** True when a held change's time has come: its text is copied to out (cut to fit and
 *  terminated), it is no longer held, and now_ms is recorded as the time the last change was
 *  said. False when nothing is held or its time has not come; out is then left alone. */
bool sr_throttle_due(
    SrThrottle* t,
    uint32_t now_ms,
    uint32_t interval_ms,
    char* out,
    size_t out_size);

/** The time until a held change is due: 0 when nothing is held or it is due now, else the
 *  remaining milliseconds. */
uint32_t sr_throttle_wait_ms(const SrThrottle* t, uint32_t now_ms, uint32_t interval_ms);

/** Drop the held change: a new screen, a focus, the home screen, a lock or a typed character
 *  supersedes it. The time the last change was said is kept. */
void sr_throttle_clear(SrThrottle* t);

#ifdef __cplusplus
}
#endif
