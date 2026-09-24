#include "screen_reader.h"
#include "screen_reader_cli.h"

#include <furi.h>
#include <furi_hal_rtc.h>
#include <gui/gui_i.h>
#include <gui/canvas_i.h>
#include <input/input.h>
#include <desktop/desktop.h>
#include <momentum/settings.h>

#define TAG "ScreenReader"

#define SR_FLAG_FRAME      (1 << 0)
#define SR_FLAG_COMMAND    (1 << 1)
#define SR_FLAG_UNLOCKED   (1 << 2)
#define SR_FLAG_LOCKED     (1 << 3)
#define SR_FLAG_LOCKED_PIN (1 << 4)
#define SR_FLAG_LOCK_ANY   (SR_FLAG_UNLOCKED | SR_FLAG_LOCKED | SR_FLAG_LOCKED_PIN)
#define SR_FLAG_ALL        (SR_FLAG_FRAME | SR_FLAG_COMMAND | SR_FLAG_LOCK_ANY)
#define SR_SETTLE_MS       50
#define SR_MAX_LATENCY_MS  300
#define SR_KEY_RECENT_MS   500
#define SR_DESKTOP_KEY_MS  2000

// The lock screen's own words (desktop_view_locked.c), said when the desktop locks
#define SR_LOCKED_TEXT     "Locked, press Back three times to unlock"
#define SR_LOCKED_PIN_TEXT "Locked with PIN, press Up to enter it"

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

    // Chords: the state belongs to the input filter. A command waits here for the service
    // thread, which takes it with an atomic exchange: the latest one wins, none runs twice
    SrChords chords;
    uint8_t pending_command;

    // The desktop's lock state, for "Locked" and "Unlocked"; subscribed once the desktop record
    // exists. The service thread alone keeps the time of the last lock announcement
    FuriPubSubSubscription* desktop_subscription;
    bool lock_said;
    uint32_t lock_said_tick;
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
    r->note = record->note;
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
        // seen the key, so the announcement the key causes is queued after it and survives.
        // The filter drops the Press of a chord key before it gets here, so a command's own
        // speech is not stopped by the key that asked for it
        speech_stop(sr->speech);
    }
}

void screen_reader_run_command(ScreenReader* sr, SrChordCommand command) {
    furi_check(sr);
    __atomic_store_n(&sr->pending_command, (uint8_t)command, __ATOMIC_SEQ_CST);
    furi_thread_flags_set(sr->thread_id, SR_FLAG_COMMAND);
}

// Input filter: runs on the input service, timer service or a console thread, serialized by
// the input filter lock. It must not block, call input_set_filter or publish input events.
// Installed once at start and read like the text tap reads the setting: while the reader is off,
// whoever turned it off (the console, or the settings app writing the setting directly), every
// event passes and the chord state stays fresh for when it comes back on
static void sr_input_filter(const InputEvent* event, InputFilterResult* result, void* context) {
    ScreenReader* sr = context;
    if(!momentum_settings.screen_reader) {
        sr_chords_init(&sr->chords);
        return;
    }
    bool drop = false, emit_long = false;
    SrChordCommand command =
        sr_chords_feed(&sr->chords, event->key, event->type, furi_get_tick(), &drop, &emit_long);
    result->drop = drop;
    result->emit_long = emit_long;
    if(command != SrChordNone) screen_reader_run_command(sr, command);
}

// The rate, volume and voice as saved, pushed before the reader speaks: the settings app writes
// momentum_settings and its changes apply from the next announcement on (two cheap stores)
static void sr_push_voice(ScreenReader* sr) {
    speech_set_voice(
        sr->speech, (uint8_t)momentum_settings.sr_rate, (uint8_t)momentum_settings.sr_volume);
    speech_set_voice_clips(sr->speech, momentum_settings.sr_voice);
}

// Desktop thread, from desktop_lock and desktop_unlock. Only desktop_unlock publishes locked
// false, and only from the locked state, so each such message is the step from locked to
// unlocked (also when the lock happened before the reader subscribed). desktop_lock sets the RTC
// lock flag for a PIN lock before it publishes, and the lock screen reads the same flag for its
// words, so the flag is read here, when the lock happens
static void sr_desktop_status_callback(const void* message, void* context) {
    const DesktopStatus* status = message;
    ScreenReader* sr = context;
    uint32_t flag = SR_FLAG_UNLOCKED;
    if(status->locked) {
        flag = furi_hal_rtc_is_flag_set(FuriHalRtcFlagLock) ? SR_FLAG_LOCKED_PIN : SR_FLAG_LOCKED;
    }
    furi_thread_flags_set(sr->thread_id, flag);
}

// The desktop service starts before the reader but creates its record at the end of its setup,
// so the service loop retries on every frame until the record is there
static void sr_subscribe_desktop(ScreenReader* sr) {
    if(sr->desktop_subscription || !furi_record_exists(RECORD_DESKTOP)) return;
    Desktop* desktop = furi_record_open(RECORD_DESKTOP);
    sr->desktop_subscription = furi_pubsub_subscribe(
        desktop_api_get_status_pubsub(desktop), sr_desktop_status_callback, sr);
}

// The desktop locked or unlocked, on the service thread, at once: a frame still settling is
// modelled after it. Never kept quiet like the desktop's own changes: an auto lock comes without
// a key press. A lock interrupts what is being said. "Unlocked" waits for it instead (the key
// that unlocked has silenced speech already), and the "Home screen" that follows, when the lock
// screen's text goes away, is queued behind it
static void sr_say_lock(ScreenReader* sr, const char* text, bool locked) {
    if(!momentum_settings.screen_reader) return;
    sr_lock(sr);
    SrAnnouncement* a = &sr->announcements[0];
    a->kind = SrAnnLock;
    a->interrupt = locked;
    strlcpy(a->text, text, sizeof(a->text));
    sr->stats.announcements++;
    if(locked) {
        sr->lock_said = true;
        sr->lock_said_tick = furi_get_tick();
    }
    sr_push_voice(sr);
    speech_say(sr->speech, a->text, a->interrupt, false);
    if(sr->watch_queue) furi_message_queue_put(sr->watch_queue, a, 0);
    sr_unlock(sr);
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
        // changes on the same screen; when one vanishes, the desktop's text is gone and the
        // model says "Home screen" without interrupting, as it does when the lock menu or a
        // dialog of the desktop closes. Without a key press in the last two seconds nobody asked
        // for either, so they are not spoken; they still reach the console mirror, and "Home
        // screen" on arrival from another screen interrupts and stays
        bool on_desktop = sr->working.content_layer == SrLayerDesktop;
        bool quiet_desktop = on_desktop && since_press >= SR_DESKTOP_KEY_MS;
        // The lock announcement has said what the lock screen shows. Until a key is pressed, or
        // for two seconds, the desktop says nothing more: the lock screen arriving under its
        // sliding cover, the dolphin's bubbles drawn behind the cover, and a frame of the menu
        // that locked, drawn before the lock screen but modelled after the announcement (the
        // lock menu's "Lock" tile as its popup closes), whose focus would cut it off
        uint32_t since_lock = now - sr->lock_said_tick;
        bool after_lock = on_desktop && sr->lock_said && since_lock < SR_DESKTOP_KEY_MS &&
                          since_press >= since_lock;
        sr->model.verbosity = momentum_settings.sr_verbosity;
        size_t n = sr_model_process(
            &sr->model, &sr->working, now, key_recent, sr->announcements, SR_MAX_ANNOUNCEMENTS);
        sr->stats.frames++;
        for(size_t i = 0; i < n; i++) {
            const SrAnnouncement* a = &sr->announcements[i];
            bool unasked = a->kind == SrAnnChange || (a->kind == SrAnnHome && !a->interrupt);
            if((quiet_desktop && unasked) || after_lock) {
                sr->stats.suppressed++;
            } else {
                sr->stats.announcements++;
                if(momentum_settings.screen_reader) {
                    // Screen and focus announcements interrupt what is being said; a change
                    // on the same screen waits its turn and replaces a change still waiting
                    sr_push_voice(sr);
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

// A chord's command, on the service thread. Every command interrupts what is being said; the
// texts that can outgrow a speech item (the whole screen, the status bar, the focus) are said
// in parts. The screen is read in place: only this thread writes model.prev (sr_process), and a
// copy would take another 1.3 KB of heap in apps that leave little
static void sr_run_command(ScreenReader* sr, SrChordCommand command) {
    char* text = malloc(SR_DESCRIBE_TEXT_MAX);
    const SrScreen* screen = &sr->model.prev;
    sr_push_voice(sr);
    switch(command) {
    case SrChordReadAll:
        sr_screen_describe(screen, text, SR_DESCRIBE_TEXT_MAX);
        if(text[0] == '\0') {
            // The home screen draws no text of its own; the model announces it the same way
            strlcpy(
                text,
                screen->content_layer == SrLayerDesktop ? "Home screen" : "Nothing on screen",
                SR_DESCRIBE_TEXT_MAX);
        }
        speech_say_parts(sr->speech, text);
        break;
    case SrChordStatus:
        sr_status_text(screen, text, SR_DESCRIBE_TEXT_MAX);
        if(text[0] == '\0') strlcpy(text, "No status bar", SR_DESCRIBE_TEXT_MAX);
        speech_say_parts(sr->speech, text);
        break;
    case SrChordRepeat:
        sr_focus_with_position(screen, text, SR_DESCRIBE_TEXT_MAX);
        if(text[0] == '\0') strlcpy(text, "No focus", SR_DESCRIBE_TEXT_MAX);
        speech_say_parts(sr->speech, text);
        break;
    case SrChordSpell:
        // One speech item of letters (159 characters is a minute of spelling)
        sr_screen_focus_text(screen, text, SR_DESCRIBE_TEXT_MAX);
        if(text[0] == '\0') {
            speech_say(sr->speech, "No focus", true, false); // said, not spelled
        } else {
            speech_say_spelled(sr->speech, text);
        }
        break;
    case SrChordVolumeDown:
    case SrChordVolumeUp: {
        int v = (int)momentum_settings.sr_volume + (command == SrChordVolumeUp ? 10 : -10);
        if(v < 0) v = 0;
        if(v > 100) v = 100;
        momentum_settings.sr_volume = (uint32_t)v;
        momentum_settings_save();
        speech_set_voice(sr->speech, (uint8_t)momentum_settings.sr_rate, (uint8_t)v);
        snprintf(text, SR_DESCRIBE_TEXT_MAX, "Volume %d", v);
        speech_say(sr->speech, text, true, false);
        break;
    }
    default:
        break;
    }
    free(text);
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
    speech_set_voice_clips(sr->speech, momentum_settings.sr_voice);

    sr->input_events = furi_record_open(RECORD_INPUT_EVENTS);
    sr->input_subscription = furi_pubsub_subscribe(sr->input_events, sr_input_callback, sr);
    sr_chords_init(&sr->chords);
    input_set_filter(sr_input_filter, sr);

    sr->gui = furi_record_open(RECORD_GUI);
    gui_tap_set(sr->gui, &screen_reader_tap, sr);
    sr_subscribe_desktop(sr);

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
            continue;
        }
        // A lock before an unlock, should both arrive at once
        if(flags & SR_FLAG_LOCKED_PIN) sr_say_lock(sr, SR_LOCKED_PIN_TEXT, true);
        if(flags & SR_FLAG_LOCKED) sr_say_lock(sr, SR_LOCKED_TEXT, true);
        if(flags & SR_FLAG_UNLOCKED) sr_say_lock(sr, "Unlocked", false);
        if(flags & SR_FLAG_COMMAND) {
            // A command reads the screen as it is now: a frame still settling is modelled first,
            // as the timeout would have done. With the reader turned off meanwhile it is dropped
            pending = false;
            sr_process(sr);
            SrChordCommand command = (SrChordCommand)__atomic_exchange_n(
                &sr->pending_command, (uint8_t)SrChordNone, __ATOMIC_SEQ_CST);
            if(command != SrChordNone && momentum_settings.screen_reader) {
                sr_run_command(sr, command);
            }
        } else if(flags & SR_FLAG_FRAME) {
            sr_subscribe_desktop(sr);
            if(!pending) pending_since = furi_get_tick();
            pending = true;
        }
    }
    return 0;
}
