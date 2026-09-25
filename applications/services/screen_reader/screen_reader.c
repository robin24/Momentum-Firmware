#include "screen_reader.h"
#include "screen_reader_cli.h"
#include "sr_throttle.h"

#include <furi.h>
#include <furi_hal_rtc.h>
#include <gui/gui_i.h>
#include <gui/canvas_i.h>
#include <input/input.h>
#include <desktop/desktop.h>
#include <dolphin/dolphin.h>
#include <storage/storage.h>
#include <momentum/settings.h>

#define TAG "ScreenReader"

#define SR_FLAG_FRAME        (1 << 0)
#define SR_FLAG_COMMAND      (1 << 1)
#define SR_FLAG_UNLOCKED     (1 << 2)
#define SR_FLAG_LOCKED       (1 << 3)
#define SR_FLAG_LOCKED_PIN   (1 << 4)
#define SR_FLAG_DOLPHIN      (1 << 5)
#define SR_FLAG_STORAGE      (1 << 6)
#define SR_FLAG_LOCK_ANY     (SR_FLAG_UNLOCKED | SR_FLAG_LOCKED | SR_FLAG_LOCKED_PIN)
#define SR_FLAG_SERVICES     (SR_FLAG_LOCK_ANY | SR_FLAG_DOLPHIN | SR_FLAG_STORAGE)
#define SR_FLAG_ALL          (SR_FLAG_FRAME | SR_FLAG_COMMAND | SR_FLAG_SERVICES)
#define SR_SETTLE_MS         50
#define SR_MAX_LATENCY_MS    300
#define SR_KEY_RECENT_MS     500
#define SR_DESKTOP_KEY_MS    2000
// A level up waits this long before it is said, and this long again after every interrupting
// announcement while it waits: the deed that brings it usually changes the screen, sometimes
// twice, and those screens are read first. Two seconds, because the core apps' popups, a Save's
// "Saved!" among them, close by themselves after up to 1.5 s, and the screen after the popup must
// come while the level up still waits. It never waits longer than SR_LEVEL_HOLD_MAX_MS
#define SR_LEVEL_HOLD_MS     2000
#define SR_LEVEL_HOLD_MAX_MS 5000
// A card mount reaches the reader before the dolphin, which then queues a reload of its state:
// this long a pause lets that reload go ahead of the reader's question for the level
#define SR_MOUNT_SETTLE_MS   50

// The lock screen's own words (desktop_view_locked.c, whose note writes the 3 as a digit to fit a
// tap record), said when the desktop locks. Quickly: after 600 ms without a key it counts afresh
#define SR_LOCKED_TEXT     "Locked, press Back three times quickly to unlock"
#define SR_LOCKED_PIN_TEXT "Locked with PIN, press Up to enter it"

struct ScreenReader {
    FuriThreadId thread_id;
    FuriMutex* mutex;
    Gui* gui;
    Speech* speech;

    // Written on the GUI thread between frame_begin and frame_end
    bool capturing;
    bool turned_on; // the setting went from off to on; cleared when a frame is handed over
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
    // Set by the input callback on every key press, taken by the service thread: a key press
    // drops a held change, whose text the key is about to change
    bool key_pressed;

    // Chords: the state belongs to the input filter. A command waits here for the service
    // thread, which takes it with an atomic exchange: the latest one wins, none runs twice
    SrChords chords;
    uint8_t pending_command;

    // The desktop's lock state, for "Locked" and "Unlocked"; subscribed once the desktop record
    // exists. The service thread alone keeps the time of the last lock announcement
    FuriPubSubSubscription* desktop_subscription;
    bool lock_said;
    uint32_t lock_said_tick;

    // The dolphin, for its level and mood; null until its record exists (sr_subscribe_dolphin).
    // Its pubsub callback only sets a flag; the service thread alone asks for the stats and keeps
    // the level last seen, and a level up held from held_level_since until held_level_due (0:
    // none). Card mounts, after which the dolphin may have loaded its state afresh, set a flag
    Dolphin* dolphin;
    FuriPubSubSubscription* dolphin_subscription;
    FuriPubSubSubscription* storage_subscription;
    uint8_t last_level;
    uint8_t held_level;
    uint32_t held_level_since;
    uint32_t held_level_due;

    // Same-screen changes said at most once per change delay, the latest held until its time
    // comes; the service thread alone uses it
    SrThrottle throttle;
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
    bool was_capturing = sr->capturing;
    sr->capturing = momentum_settings.screen_reader;
    // Turned back on, by the settings app writing the setting or by sr on: the screen is read
    // again, also when it is the one the reader last saw before it went off
    if(sr->capturing && !was_capturing) sr->turned_on = true;
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
        if(sr->turned_on) {
            // As screen_reader_set_enabled does: without a previous screen the model announces
            // this frame as a new screen
            sr->model.have_prev = false;
            sr->turned_on = false;
        }
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
        __atomic_store_n(&sr->key_pressed, true, __ATOMIC_SEQ_CST);
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

// The volume as saved, pushed before the reader speaks: the settings app writes
// momentum_settings and its change applies from the next announcement on (one cheap store)
static void sr_push_volume(ScreenReader* sr) {
    speech_set_volume(sr->speech, (uint8_t)momentum_settings.sr_volume);
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

// Dolphin thread, after a deed or a level up. Only a flag, as for the desktop: dolphin_stats waits
// for the dolphin's own event loop, the thread running this callback, so the service thread reads
// the stats instead
static void sr_dolphin_callback(const void* message, void* context) {
    UNUSED(message);
    ScreenReader* sr = context;
    furi_thread_flags_set(sr->thread_id, SR_FLAG_DOLPHIN);
}

// The dolphin service creates its record first thing when it starts, but in a special boot (an
// update) it never starts. So, as for the desktop, the service loop tries again on every frame
// until the record is there, and the reader keeps reading the update's screens meanwhile. Then it
// subscribes and reads the level once: subscribed first, so every level up after the read is seen
static void sr_subscribe_dolphin(ScreenReader* sr) {
    if(sr->dolphin || !furi_record_exists(RECORD_DOLPHIN)) return;
    sr->dolphin = furi_record_open(RECORD_DOLPHIN);
    sr->dolphin_subscription =
        furi_pubsub_subscribe(dolphin_get_pubsub(sr->dolphin), sr_dolphin_callback, sr);
    sr->last_level = dolphin_stats(sr->dolphin).level;
}

// A storage thread, on card events (a mount is published from a helper thread the storage
// service starts). Only a flag, and only for a mount: when the card was not
// ready as the dolphin started, it loads its state on the mount and publishes nothing, so the
// level the reader noted may be stale
static void sr_storage_callback(const void* message, void* context) {
    const StorageEvent* event = message;
    ScreenReader* sr = context;
    if(event->type == StorageEventTypeCardMount) {
        furi_thread_flags_set(sr->thread_id, SR_FLAG_STORAGE);
    }
}

// After a card mount, on the service thread: the level is noted afresh, silently. Subscribers
// hear of a mount newest first, the reader before the dolphin, which queues its reload then; its
// queue is first in, first out, so after a pause the reader's question comes after that reload
static void sr_reread_level(ScreenReader* sr) {
    if(!sr->dolphin) return;
    furi_delay_ms(SR_MOUNT_SETTLE_MS);
    sr->last_level = dolphin_stats(sr->dolphin).level;
}

// An interrupting announcement while a level up is held: the screen that came with the deed, or
// the one after it (a Save's popup closes by itself after 1.5 s). The level up waits
// SR_LEVEL_HOLD_MS more from now, so it is queued behind that screen, but never past
// SR_LEVEL_HOLD_MAX_MS after the level rose. The service thread alone uses these fields
static void sr_hold_level_longer(ScreenReader* sr) {
    if(!sr->held_level) return;
    uint32_t due = furi_get_tick() + SR_LEVEL_HOLD_MS;
    uint32_t last = sr->held_level_since + SR_LEVEL_HOLD_MAX_MS;
    sr->held_level_due = (int32_t)(due - last) > 0 ? last : due;
}

// Every announcement that is said goes through here, with the mutex held: counted, said while
// the reader is on (at the volume as saved), and mirrored to the console. It interrupts what is
// being said when it asks to, and then holds a waiting level up longer; a change on the same
// screen waits its turn instead and replaces a change still waiting
static void sr_announce(ScreenReader* sr, const SrAnnouncement* a) {
    sr->stats.announcements++;
    if(momentum_settings.screen_reader) {
        sr_push_volume(sr);
        speech_say(sr->speech, a->text, a->interrupt, a->kind == SrAnnChange);
    }
    if(sr->watch_queue) furi_message_queue_put(sr->watch_queue, a, 0);
    if(a->interrupt) sr_hold_level_longer(sr);
}

// The desktop's quiet rules, for an announcement on a screen of this layer at now, since_press
// after the last key press; unasked is a change or the desktop's text going away. The home
// screen keeps redrawing its dolphin speech bubbles, and they arrive as changes on the same
// screen; when one vanishes, the desktop's text is gone and the model says "Home screen" without
// interrupting, as it does when the lock menu or a dialog of the desktop closes. Without a key
// press in the last two seconds nobody asked for either, so they are not spoken; "Home screen"
// on arrival from another screen interrupts and stays. And the lock announcement has said what
// the lock screen shows: until a key is pressed, or for two seconds, the desktop says nothing
// more: the lock screen arriving under its sliding cover, the dolphin's bubbles drawn behind the
// cover, and a frame of the menu that locked, drawn before the lock screen but modelled after
// the announcement (the lock menu's "Lock" tile as its popup closes), whose focus would cut it
// off
static bool sr_quiet(
    const ScreenReader* sr,
    uint32_t now,
    uint32_t since_press,
    uint8_t layer,
    bool unasked) {
    if(layer != SrLayerDesktop) return false;
    uint32_t since_lock = now - sr->lock_said_tick;
    bool after_lock = sr->lock_said && since_lock < SR_DESKTOP_KEY_MS && since_press >= since_lock;
    return (unasked && since_press >= SR_DESKTOP_KEY_MS) || after_lock;
}

// The service's own announcements, on the service thread: the desktop locking or unlocking, said
// at once (a frame still settling is modelled after it), and the dolphin's level up, once its
// time comes (sr_say_held_level). Never kept quiet like the desktop's own changes: an auto lock
// and a level up come without a key press. A lock interrupts what is being said and starts the
// desktop's quiet window (sr_quiet). "Unlocked" and a level up wait for what is being said
// instead (the key that unlocked has silenced speech already); the "Home screen" that follows an
// unlock, when the lock screen's text goes away, is queued behind it. A lock or an unlock drops a
// held change, whose screen is gone; a level up keeps it, and the change is said in its turn
static void sr_say_event(ScreenReader* sr, SrAnnKind kind, const char* text, bool interrupt) {
    if(!momentum_settings.screen_reader) return;
    sr_lock(sr);
    SrAnnouncement* a = &sr->announcements[0];
    a->kind = kind;
    a->interrupt = interrupt;
    strlcpy(a->text, text, sizeof(a->text));
    if(kind == SrAnnLock && interrupt) {
        sr->lock_said = true;
        sr->lock_said_tick = furi_get_tick();
    }
    if(kind == SrAnnLock) sr_throttle_clear(&sr->throttle);
    sr_announce(sr, a);
    sr_unlock(sr);
}

// After a deed or a level up, on the service thread, which may wait briefly for the dolphin. A
// level above the last one seen is a level up: held for SR_LEVEL_HOLD_MS, longer while screens
// change (sr_hold_level_longer), then said as "Level up, level 4"; a second rise meanwhile only
// updates the level to be said. The level set by hand in Momentum, Misc, Dolphin shows at the
// next deed: a lower one is only noted, a higher one is said like a level up. Noted with the
// reader off too, so turning it on says nothing stale
static void sr_check_level(ScreenReader* sr) {
    DolphinStats stats = dolphin_stats(sr->dolphin);
    if(stats.level > sr->last_level) {
        if(!sr->held_level) {
            sr->held_level_since = furi_get_tick();
            sr->held_level_due = sr->held_level_since + SR_LEVEL_HOLD_MS;
        }
        sr->held_level = stats.level;
    }
    sr->last_level = stats.level;
}

// Milliseconds until the held level up is due, 0 when it is due already
static uint32_t sr_held_level_wait_ms(const ScreenReader* sr, uint32_t now) {
    int32_t left = (int32_t)(sr->held_level_due - now);
    return left > 0 ? (uint32_t)left : 0;
}

// A held level up whose time has come, on the service thread, once no frame is settling, so it
// is queued after the announcement of the screen the deed brought, without interrupting. With
// the reader off by then it is dropped; the level stays noted
static void sr_say_held_level(ScreenReader* sr) {
    if(!sr->held_level || sr_held_level_wait_ms(sr, furi_get_tick()) > 0) return;
    char text[24];
    snprintf(text, sizeof(text), "Level up, level %u", (unsigned)sr->held_level);
    sr->held_level = 0;
    sr_say_event(sr, SrAnnLevel, text, false);
}

// The Passport's words for the dolphin's mood, from its thresholds
// (applications/settings/dolphin_passport/passport.c:41-49). The Passport is an app of its own on
// the card, so the thresholds live in both places on purpose
static const char* sr_mood(uint32_t butthurt) {
    if(butthurt <= 4) return "happy";
    if(butthurt <= 9) return "okay";
    return "angry";
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
        sr->model.verbosity = momentum_settings.sr_verbosity;
        size_t n = sr_model_process(
            &sr->model, &sr->working, now, key_recent, sr->announcements, SR_MAX_ANNOUNCEMENTS);
        sr->stats.frames++;
        for(size_t i = 0; i < n; i++) {
            const SrAnnouncement* a = &sr->announcements[i];
            bool unasked = a->kind == SrAnnChange || (a->kind == SrAnnHome && !a->interrupt);
            if(sr_quiet(sr, now, since_press, sr->working.content_layer, unasked)) {
                // Not said, and neither is a change held from before: the screen it came from
                // has moved on. The console mirror still gets it
                sr_throttle_clear(&sr->throttle);
                sr->stats.suppressed++;
                if(sr->watch_queue) furi_message_queue_put(sr->watch_queue, a, 0);
                continue;
            }
            if(a->kind != SrAnnChange) {
                // A new screen, focus, home or typed character supersedes a held change
                sr_throttle_clear(&sr->throttle);
            } else if(key_recent) {
                // A key's own change is said at once; the next change is spaced from it
                sr_throttle_force(&sr->throttle, now);
            } else if(!sr_throttle_offer(
                          &sr->throttle, a->text, now, momentum_settings.sr_change_ms)) {
                // Held: it came within the change delay of the last change said. The loop says
                // the latest when its time comes (sr_say_held_change), unless another
                // announcement or a key press drops it first; the mirror gets it when it is said
                continue;
            }
            sr_announce(sr, a);
        }
    }
    sr_unlock(sr);
}

// A held change whose time has come, on the service thread, once no frame is settling (a newer
// frame's text or its own announcement would cut the change off). The desktop's quiet rules are
// applied again now: a change is held when the last key is between 0.5 and 2 s old and comes
// due after the desktop window (a change within 500 ms of a key is forced); it is then dropped
// and counted as suppressed, unsaid and unmirrored. Otherwise it is said as a change is, after
// what is being said and replacing a change still waiting
static void sr_say_held_change(ScreenReader* sr) {
    if(!sr->throttle.pending) return;
    uint32_t now = furi_get_tick();
    if(sr_throttle_wait_ms(&sr->throttle, now, momentum_settings.sr_change_ms) > 0) return;
    sr_lock(sr);
    SrAnnouncement* a = &sr->announcements[0];
    if(sr_quiet(sr, now, now - sr->last_press_tick, sr->model.prev.content_layer, true)) {
        sr_throttle_clear(&sr->throttle);
        sr->stats.suppressed++;
    } else if(sr_throttle_due(
                  &sr->throttle, now, momentum_settings.sr_change_ms, a->text, sizeof(a->text))) {
        a->kind = SrAnnChange;
        a->interrupt = false;
        sr_announce(sr, a);
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
    sr_push_volume(sr);
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
    case SrChordStatus: {
        // The status bar, then the dolphin: "battery 87 percent, level 3, mood happy"; on a
        // screen without a status bar just the dolphin. The dolphin may keep this thread briefly.
        // Without a dolphin (a special boot) the status bar alone, or "No status bar"
        size_t n = sr_status_text(screen, text, SR_DESCRIBE_TEXT_MAX);
        if(sr->dolphin) {
            DolphinStats stats = dolphin_stats(sr->dolphin);
            snprintf(
                text + n,
                SR_DESCRIBE_TEXT_MAX - n,
                "%slevel %u, mood %s",
                n ? ", " : "",
                (unsigned)stats.level,
                sr_mood(stats.butthurt));
        } else if(n == 0) {
            strlcpy(text, "No status bar", SR_DESCRIBE_TEXT_MAX);
        }
        speech_say_parts(sr->speech, text);
        break;
    }
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
        speech_set_volume(sr->speech, (uint8_t)v);
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
    if(enabled) {
        // Announce the current screen on the next frame. Forgotten before the setting changes:
        // a frame drawn while the setting is saved is then announced once, not again after it
        sr_lock(sr);
        sr->model.have_prev = false;
        sr_unlock(sr);
    }
    momentum_settings.screen_reader = enabled;
    momentum_settings_save();
    if(enabled) {
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
    sr_throttle_init(&sr->throttle);
    sr->speech = speech_alloc();
    speech_set_volume(sr->speech, (uint8_t)momentum_settings.sr_volume);

    sr->input_events = furi_record_open(RECORD_INPUT_EVENTS);
    sr->input_subscription = furi_pubsub_subscribe(sr->input_events, sr_input_callback, sr);
    sr_chords_init(&sr->chords);
    input_set_filter(sr_input_filter, sr);

    sr->gui = furi_record_open(RECORD_GUI);
    gui_tap_set(sr->gui, &screen_reader_tap, sr);
    sr_subscribe_desktop(sr);
    Storage* storage = furi_record_open(RECORD_STORAGE);
    sr->storage_subscription =
        furi_pubsub_subscribe(storage_get_pubsub(storage), sr_storage_callback, sr);
    furi_record_close(RECORD_STORAGE);

    furi_record_create(RECORD_SCREEN_READER, sr);
    screen_reader_cli_register(sr);
    // After the record and the console, as dolphin_stats may wait for the dolphin; a frame tries
    // again if the dolphin's record is not there yet
    sr_subscribe_dolphin(sr);
    FURI_LOG_I(TAG, "Started, enabled=%d", momentum_settings.screen_reader);

    bool pending = false;
    uint32_t pending_since = 0;
    while(true) {
        uint32_t timeout = FuriWaitForever;
        if(pending) {
            uint32_t waited = furi_get_tick() - pending_since;
            timeout = waited >= SR_MAX_LATENCY_MS ? 0 : SR_SETTLE_MS;
        } else {
            // A held change and a held level up wake the loop when their time comes, at once
            // when it is due already; while a frame settles, they wait for it to be modelled
            uint32_t now = furi_get_tick();
            if(sr->throttle.pending) {
                timeout = sr_throttle_wait_ms(&sr->throttle, now, momentum_settings.sr_change_ms);
            }
            if(sr->held_level) {
                uint32_t wait = sr_held_level_wait_ms(sr, now);
                if(wait < timeout) timeout = wait;
            }
        }
        uint32_t flags = furi_thread_flags_wait(SR_FLAG_ALL, FuriFlagWaitAny, timeout);
        // A key press drops a held change before the frame the key causes is modelled
        if(__atomic_exchange_n(&sr->key_pressed, false, __ATOMIC_SEQ_CST)) {
            sr_throttle_clear(&sr->throttle);
        }
        if(flags & FuriFlagError) {
            // Timeout: the screen has settled, or waited long enough; or a held change or level
            // up is due
            if(pending) {
                pending = false;
                sr_process(sr);
            }
            sr_say_held_change(sr);
            sr_say_held_level(sr);
            continue;
        }
        // A lock before an unlock, should both arrive at once
        if(flags & SR_FLAG_LOCKED_PIN) sr_say_event(sr, SrAnnLock, SR_LOCKED_PIN_TEXT, true);
        if(flags & SR_FLAG_LOCKED) sr_say_event(sr, SrAnnLock, SR_LOCKED_TEXT, true);
        if(flags & SR_FLAG_UNLOCKED) sr_say_event(sr, SrAnnLock, "Unlocked", false);
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
            sr_subscribe_dolphin(sr);
            if(!pending) pending_since = furi_get_tick();
            pending = true;
        }
        // A mount first, so a level up is measured against a level noted afresh; a level up is
        // held here and said when its time comes
        if(flags & SR_FLAG_STORAGE) sr_reread_level(sr);
        if(flags & SR_FLAG_DOLPHIN) sr_check_level(sr);
        if(!pending) {
            sr_say_held_change(sr);
            sr_say_held_level(sr);
        }
    }
    return 0;
}
