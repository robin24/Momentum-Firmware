#include "screen_reader.h"
#include "screen_reader_cli.h"

#include <furi.h>
#include <gui/gui_i.h>
#include <gui/canvas_i.h>
#include <input/input.h>
#include <momentum/settings.h>

#define TAG "ScreenReader"

#define SR_FLAG_FRAME     (1 << 0)
#define SR_FLAG_ALL       (SR_FLAG_FRAME)
#define SR_SETTLE_MS      50
#define SR_MAX_LATENCY_MS 300
#define SR_KEY_RECENT_MS  500
#define SR_DESKTOP_KEY_MS 2000

struct ScreenReader {
    FuriThreadId thread_id;
    FuriMutex* mutex;
    Gui* gui;
    Speech* speech;

    // Written on the GUI thread between frame_begin and frame_end
    bool capturing;
    SrFrame filling;

    // Shared, guarded by mutex
    SrFrame ready;
    bool ready_valid;
    FuriMessageQueue* watch_queue;
    ScreenReaderStats stats;
    SrFrame working;
    SrModel model;
    SrAnnouncement announcements[SR_MAX_ANNOUNCEMENTS];

    // Input
    FuriPubSub* input_events;
    FuriPubSubSubscription* input_subscription;
    volatile uint32_t last_press_tick;
};

static void sr_lock(ScreenReader* sr) {
    furi_check(furi_mutex_acquire(sr->mutex, FuriWaitForever) == FuriStatusOk);
}

static void sr_unlock(ScreenReader* sr) {
    furi_check(furi_mutex_release(sr->mutex) == FuriStatusOk);
}

// GUI thread. Must be fast, must not call gui_* functions.
static void sr_frame_begin(void* context) {
    ScreenReader* sr = context;
    sr->capturing = momentum_settings.screen_reader;
    sr->filling.count = 0;
    sr->filling.overflow = false;
    sr->filling.content_layer = SrLayerUnknown;
}

static void sr_text(const CanvasTapRecord* record, void* context) {
    ScreenReader* sr = context;
    if(!sr->capturing) return;
    if(sr->filling.count >= SR_MAX_RECORDS) {
        sr->filling.overflow = true;
        return;
    }
    SrRecord* r = &sr->filling.records[sr->filling.count++];
    r->x = record->x;
    r->y = record->y;
    r->layer = record->layer;
    r->font = record->font;
    r->inverted = record->inverted;
    r->focus = record->focus;
    r->button = record->button;
    r->index = record->index;
    r->count = record->count;
    strlcpy(r->text, record->text, sizeof(r->text));
}

static void sr_frame_end(void* context, uint8_t content_layer) {
    ScreenReader* sr = context;
    if(!sr->capturing) return;
    sr->filling.content_layer = content_layer;
    // Short wait: the service only holds the mutex for a memcpy
    if(furi_mutex_acquire(sr->mutex, 2) == FuriStatusOk) {
        memcpy(&sr->ready, &sr->filling, sizeof(SrFrame));
        sr->ready_valid = true;
        furi_mutex_release(sr->mutex);
        furi_thread_flags_set(sr->thread_id, SR_FLAG_FRAME);
    } else {
        sr->stats.dropped_frames++;
    }
}

static void sr_input_callback(const void* value, void* context) {
    const InputEvent* event = value;
    ScreenReader* sr = context;
    if(event->type == InputTypePress) {
        sr->last_press_tick = furi_get_tick();
        // Any key silences speech. speech_stop retires the queue under the speech mutex (its
        // users hold it for microseconds) and wakes the worker; this callback runs on the
        // input service thread, not in an interrupt, and the stop lands before the app has
        // seen the key, so the announcement the key causes is queued after it and survives
        speech_stop(sr->speech);
    }
}

// Runs the model under the mutex (well under a millisecond) so that the console
// commands never read a half written screen description.
static void sr_process(ScreenReader* sr) {
    sr_lock(sr);
    if(sr->ready_valid) {
        memcpy(&sr->working, &sr->ready, sizeof(SrFrame));
        sr->ready_valid = false;
        uint32_t now = furi_get_tick();
        uint32_t since_press = now - sr->last_press_tick;
        bool key_recent = since_press < SR_KEY_RECENT_MS;
        // The home screen keeps redrawing its dolphin speech bubbles, and they arrive as
        // changes on the same screen. Without a key press in the last two seconds nobody asked
        // for them, so they are not spoken; they still reach the console mirror, and "Home
        // screen" on arrival is a different kind and stays
        bool quiet_desktop = sr->working.content_layer == SrLayerDesktop &&
                             since_press >= SR_DESKTOP_KEY_MS;
        sr->model.verbosity = momentum_settings.sr_verbosity;
        size_t n = sr_model_process(
            &sr->model, &sr->working, now, key_recent, sr->announcements, SR_MAX_ANNOUNCEMENTS);
        sr->stats.frames++;
        for(size_t i = 0; i < n; i++) {
            const SrAnnouncement* a = &sr->announcements[i];
            if(quiet_desktop && a->kind == SrAnnChange) {
                sr->stats.suppressed++;
            } else {
                sr->stats.announcements++;
                if(momentum_settings.screen_reader) {
                    // Screen and focus announcements interrupt what is being said; a change
                    // on the same screen waits its turn and replaces a change still waiting
                    speech_say(sr->speech, a->text, a->interrupt, a->kind == SrAnnChange);
                }
            }
            if(sr->watch_queue) {
                furi_message_queue_put(sr->watch_queue, a, 0);
            }
        }
    }
    sr_unlock(sr);
}

void screen_reader_get_frame(ScreenReader* sr, SrFrame* out) {
    furi_check(sr && out);
    sr_lock(sr);
    memcpy(out, sr->ready_valid ? &sr->ready : &sr->working, sizeof(SrFrame));
    sr_unlock(sr);
}

void screen_reader_get_screen(ScreenReader* sr, SrScreen* out) {
    furi_check(sr && out);
    sr_lock(sr);
    memcpy(out, &sr->model.prev, sizeof(SrScreen));
    sr_unlock(sr);
}

void screen_reader_set_watch_queue(ScreenReader* sr, FuriMessageQueue* queue) {
    furi_check(sr);
    sr_lock(sr);
    sr->watch_queue = queue;
    sr_unlock(sr);
}

void screen_reader_set_enabled(ScreenReader* sr, bool enabled) {
    furi_check(sr);
    momentum_settings.screen_reader = enabled;
    momentum_settings_save();
    if(enabled) {
        // Announce the current screen on the next frame
        sr_lock(sr);
        sr->model.have_prev = false;
        sr_unlock(sr);
        gui_update(sr->gui);
    } else {
        speech_stop(sr->speech);
    }
}

bool screen_reader_is_enabled(ScreenReader* sr) {
    UNUSED(sr);
    return momentum_settings.screen_reader;
}

void screen_reader_get_stats(ScreenReader* sr, ScreenReaderStats* out) {
    furi_check(sr && out);
    sr_lock(sr);
    *out = sr->stats;
    sr_unlock(sr);
}

Speech* screen_reader_get_speech(ScreenReader* sr) {
    furi_check(sr);
    return sr->speech;
}

static const GuiTap screen_reader_tap = {
    .frame_begin = sr_frame_begin,
    .text = sr_text,
    .frame_end = sr_frame_end,
};

int32_t screen_reader_srv(void* p) {
    UNUSED(p);
    ScreenReader* sr = malloc(sizeof(ScreenReader));
    memset(sr, 0, sizeof(ScreenReader));
    sr->thread_id = furi_thread_get_current_id();
    sr->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    sr_model_init(&sr->model, momentum_settings.sr_verbosity);
    sr->speech = speech_alloc();
    speech_set_voice(
        sr->speech, (uint8_t)momentum_settings.sr_rate, (uint8_t)momentum_settings.sr_volume);

    sr->input_events = furi_record_open(RECORD_INPUT_EVENTS);
    sr->input_subscription = furi_pubsub_subscribe(sr->input_events, sr_input_callback, sr);

    sr->gui = furi_record_open(RECORD_GUI);
    gui_tap_set(sr->gui, &screen_reader_tap, sr);

    furi_record_create(RECORD_SCREEN_READER, sr);
    screen_reader_cli_register(sr);
    FURI_LOG_I(TAG, "Started, enabled=%d", momentum_settings.screen_reader);

    bool pending = false;
    uint32_t pending_since = 0;
    while(true) {
        uint32_t timeout = FuriWaitForever;
        if(pending) {
            uint32_t waited = furi_get_tick() - pending_since;
            timeout = waited >= SR_MAX_LATENCY_MS ? 0 : SR_SETTLE_MS;
        }
        uint32_t flags = furi_thread_flags_wait(SR_FLAG_ALL, FuriFlagWaitAny, timeout);
        if(flags & FuriFlagError) {
            // Timeout: the screen has settled, or waited long enough
            pending = false;
            sr_process(sr);
        } else if(flags & SR_FLAG_FRAME) {
            if(!pending) pending_since = furi_get_tick();
            pending = true;
        }
    }
    return 0;
}
