#include "screen_reader_cli.h"
#include "speech_queue.h"
#include "speech_voice.h"

#include <furi.h>
#include <furi_hal_rtc.h>
#include <cli/cli.h>
#include <toolbox/cli/cli_command.h>
#include <toolbox/cli/cli_ansi.h>
#include <toolbox/args.h>
#include <toolbox/pipe.h>
#include <input/input.h>
#include <momentum/settings.h>
#include <storage/storage.h>

// The longest timed mute, sr voice off with seconds: an hour
#define SR_VOICE_OFF_MAX_S 3600

// sr voice use saves a name the engine lists (speech_voice_set_listable): the setting holds any
// such whole
_Static_assert(
    SR_VOICE_SET_LEN == SPEECH_VOICE_SET_MAX,
    "the voice set setting must fit a set name");

static const char* sr_layer_name(uint8_t layer) {
    switch(layer) {
    case SrLayerDesktop:
        return "desktop";
    case SrLayerWindow:
        return "window";
    case SrLayerStatusLeft:
        return "status-left";
    case SrLayerStatusRight:
        return "status-right";
    case SrLayerFullscreen:
        return "fullscreen";
    default:
        return "none";
    }
}

static const char* sr_font_name(uint8_t font) {
    switch(font) {
    case SrFontPrimary:
        return "primary";
    case SrFontSecondary:
        return "secondary";
    case SrFontKeyboard:
        return "keyboard";
    case SrFontBigNumbers:
        return "bignumbers";
    case SrFontBatteryPercent:
        return "battery";
    default:
        return "custom";
    }
}

static const char* sr_button_name(uint8_t side) {
    switch(side) {
    case 1:
        return "left";
    case 2:
        return "center";
    case 3:
        return "right";
    case 4:
        return "up";
    case 5:
        return "down";
    default:
        return "?";
    }
}

static const char* sr_kind_name(SrAnnKind kind) {
    switch(kind) {
    case SrAnnFocus:
        return "focus";
    case SrAnnScreen:
        return "screen";
    case SrAnnChange:
        return "change";
    case SrAnnTyped:
        return "typed";
    case SrAnnHome:
        return "home";
    case SrAnnLock:
        return "lock";
    case SrAnnLevel:
        return "level";
    default:
        return "?";
    }
}

static void sr_cli_usage(void) {
    printf("Usage: sr <command>\r\n");
    printf("  screen   print the captured screen and how it reads\r\n");
    printf("  watch    stream announcements; arrows, Enter, Backspace drive the device\r\n");
    printf("  on|off   enable or disable the screen reader (saved)\r\n");
    printf("  status   show state and counters\r\n");
    printf("  say <text>   speak text now; sr status shows its timing when done\r\n");
    printf("  stop         stop speaking\r\n");
    printf("  volume <n>   0..100 (saved)\r\n");
    printf(
        "  delay <ms>   change delay %d..%d: a same-screen change is said at most once per\r\n"
        "               delay, the latest (saved)\r\n",
        SR_CHANGE_MS_MIN,
        SR_CHANGE_MS_MAX);
    printf("  play <path> [rate]  raw 8 bit mono clip from the card, 8000..32000 Hz (16000)\r\n");
    printf(
        "  voice on|off [s]|status|list|use <set>  recorded word clips from the card; off mutes speech until on or a reboot, or for s seconds; use picks the set (saved)\r\n");
    printf(
        "  chord up|down|ok|okhold|downhold|left|right  what Back plus that key does: screen,\r\n"
        "               status bar, focus, focus spelled, reader off or on (also while it is off),\r\n"
        "               volume down, volume up\r\n");
}

static void sr_cli_screen(ScreenReader* sr) {
    SrFrame* frame = malloc(sizeof(SrFrame));
    screen_reader_get_frame(sr, frame);
    printf(
        "records: %u%s, content layer: %s\r\n",
        (unsigned)frame->count,
        frame->overflow ? " (overflow)" : "",
        sr_layer_name(frame->content_layer));
    for(size_t i = 0; i < frame->count; i++) {
        const SrRecord* r = &frame->records[i];
        printf(
            "%-12s x=%3d y=%3d %-10s%s%s%s",
            sr_layer_name(r->layer),
            (int)r->x,
            (int)r->y,
            sr_font_name(r->font),
            r->inverted ? " inverted" : "",
            r->focus ? " FOCUS" : "",
            r->note ? " NOTE" : "");
        if(r->count) printf(" %u/%u", (unsigned)r->index, (unsigned)r->count);
        if(r->button) printf(" button=%s", sr_button_name(r->button));
        printf(" \"%s\"\r\n", r->text);
    }
    free(frame);

    SrScreen* screen = malloc(sizeof(SrScreen));
    screen_reader_get_screen(sr, screen);
    char* text = malloc(SR_DESCRIBE_TEXT_MAX);
    sr_screen_describe(screen, text, SR_DESCRIBE_TEXT_MAX);
    printf("reads as: %s\r\n", text);
    free(text);
    free(screen);
}

static void sr_cli_print_voice(ScreenReader* sr) {
    SpeechVoiceStats v;
    speech_get_voice_stats(screen_reader_get_speech(sr), &v);
    // The set asked for is named too when it is not the one in use: its folder is not on the
    // card (the engine took the first set found), or there is no set or no card at all
    bool differs = v.wanted[0] != '\0' && strcmp(v.wanted, v.set) != 0;
    // A timed mute says how long it has left, rounded up
    char state[32] = "on";
    if(!v.enabled && v.on_in_ms) {
        snprintf(
            state, sizeof(state), "off for %lu s more", (unsigned long)((v.on_in_ms + 999) / 1000));
    } else if(!v.enabled) {
        snprintf(state, sizeof(state), "off");
    }
    printf(
        "voice: %s, vocabulary %s set %s%s%s%s%s, clips %lu, fallback %lu, missing %lu, muted %lu, low memory %lu, open max %lu ms, last %lu ms\r\n",
        state,
        v.vocabulary ? "yes" : "no",
        v.set[0] ? v.set : "none",
        differs ? " wanted " : "",
        differs ? v.wanted : "",
        v.settings[0] ? " " : "",
        v.settings,
        (unsigned long)v.clip_words,
        (unsigned long)v.fallback_words,
        (unsigned long)v.missing_words,
        (unsigned long)v.muted,
        (unsigned long)v.low_memory,
        (unsigned long)v.open_max_ms,
        (unsigned long)v.open_last_ms);
}

// A line of sr voice list: the set, and whether the engine speaks in it
static void sr_cli_voice_list_one(const char* name, void* context) {
    const char* in_use = context;
    printf("%s%s\r\n", name, strcmp(name, in_use) == 0 ? " (in use)" : "");
}

// The sets on the card as the card lists them. The one marked in use is the one the engine
// resolved at the first utterance after a card mount or a change of set; until that utterance
// the one before it stays marked, or none
static void sr_cli_voice_list(ScreenReader* sr) {
    Speech* speech = screen_reader_get_speech(sr);
    SpeechVoiceStats v;
    speech_get_voice_stats(speech, &v);
    if(speech_voice_sets(speech, sr_cli_voice_list_one, v.set) == 0) printf("none found\r\n");
}

// Whether the card holds the folder of that set, through a storage record of the call's own
static bool sr_cli_voice_set_exists(const char* set) {
    char path[SPEECH_VOICE_PATH_MAX];
    if(!speech_voice_set_file(set, "", path, sizeof(path))) return false;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    bool exists = storage_dir_exists(storage, path);
    furi_record_close(RECORD_STORAGE);
    return exists;
}

// sr voice use <set>: a set on the card, any name that sr voice list shows and the settings app
// offers, in quotes when it has a space; or auto for the first found (the setting emptied).
// Saved, and passed to the engine at once, which resolves the set afresh at the next utterance,
// also for the name already in use: a set synced again, or back after a fallback, is taken
static void sr_cli_voice_use(ScreenReader* sr, FuriString* args) {
    FuriString* name = furi_string_alloc();
    do {
        if(!args_read_probably_quoted_string_and_trim(args, name)) {
            printf("sr voice use <set>|auto\r\n");
            break;
        }
        const char* set = furi_string_get_cstr(name);
        if(strcmp(set, "auto") == 0) {
            set = "";
        } else if(!speech_voice_set_listable(set) || !sr_cli_voice_set_exists(set)) {
            printf("no such voice set: %s\r\n", set);
            break;
        }
        strlcpy(momentum_settings.sr_voice_set, set, sizeof(momentum_settings.sr_voice_set));
        momentum_settings_save();
        speech_set_voice_set(screen_reader_get_speech(sr), set, true);
        if(set[0] != '\0') {
            printf("voice set: %s\r\n", set);
        } else {
            printf("voice set: auto, the first found\r\n");
        }
    } while(false);
    furi_string_free(name);
}

// The item being spoken when the voice goes off stops at its next sample; the command answers
// once it has ended, at most a second later, so that no clip is opened after the answer
static void sr_cli_wait_idle(Speech* speech) {
    for(int i = 0; i < 100 && speech_is_busy(speech); i++)
        furi_delay_ms(10);
}

static void sr_cli_voice(ScreenReader* sr, FuriString* args) {
    FuriString* sub = furi_string_alloc();
    bool has = args_read_string_and_trim(args, sub);
    if(has && furi_string_cmp_str(sub, "on") == 0) {
        speech_set_voice_clips(screen_reader_get_speech(sr), true);
        printf("voice on\r\n");
    } else if(has && furi_string_cmp_str(sub, "off") == 0) {
        // With seconds, the guard the generator sets around a transfer and renews as it goes:
        // the voice comes back by itself if the tool dies. A voice already off without an end
        // stays so
        Speech* speech = screen_reader_get_speech(sr);
        int seconds = 0;
        if(furi_string_empty(args)) {
            speech_set_voice_clips(speech, false);
            sr_cli_wait_idle(speech);
            printf("voice off: muted until sr voice on or a reboot\r\n");
        } else if(
            !args_read_int_and_trim(args, &seconds) || seconds < 1 ||
            seconds > SR_VOICE_OFF_MAX_S) {
            printf("sr voice off [1..%d seconds]\r\n", SR_VOICE_OFF_MAX_S);
        } else if(speech_mute_voice_for(speech, (uint32_t)seconds * 1000)) {
            sr_cli_wait_idle(speech);
            printf("voice off for %d s, then on again by itself\r\n", seconds);
        } else {
            printf("voice off: muted until sr voice on or a reboot\r\n");
        }
    } else if(!has || furi_string_cmp_str(sub, "status") == 0) {
        sr_cli_print_voice(sr);
    } else if(furi_string_cmp_str(sub, "list") == 0) {
        sr_cli_voice_list(sr);
    } else if(furi_string_cmp_str(sub, "use") == 0) {
        sr_cli_voice_use(sr, args);
    } else {
        printf("sr voice on|off [seconds]|status|list|use <set>\r\n");
    }
    furi_string_free(sub);
}

static void sr_cli_status(ScreenReader* sr) {
    ScreenReaderStats stats;
    screen_reader_get_stats(sr, &stats);
    printf("enabled: %s\r\n", screen_reader_is_enabled(sr) ? "yes" : "no");
    // The last crash's address, the instruction that faulted or the return address of the failed
    // check, and the build that stored it, kept across reboots and updates by check.c; a build
    // from before the build register stored none
    uint32_t crash_address = furi_hal_rtc_get_register(FuriHalRtcRegisterFaultLr);
    uint32_t crash_build = furi_hal_rtc_get_register(FuriHalRtcRegisterFaultBuild);
    if(!crash_address) {
        printf("last crash: none\r\n");
    } else if(crash_build) {
        printf(
            "last crash: at %08lX in build %08lx\r\n",
            (unsigned long)crash_address,
            (unsigned long)crash_build);
    } else {
        printf("last crash: at %08lX in build unknown\r\n", (unsigned long)crash_address);
    }
    printf(
        "volume: %lu, verbosity: %lu, change delay: %lu ms\r\n",
        (unsigned long)momentum_settings.sr_volume,
        (unsigned long)momentum_settings.sr_verbosity,
        (unsigned long)momentum_settings.sr_change_ms);
    printf(
        "frames: %lu, dropped: %lu, announcements: %lu, suppressed: %lu\r\n",
        (unsigned long)stats.frames,
        (unsigned long)stats.dropped_frames,
        (unsigned long)stats.announcements,
        (unsigned long)stats.suppressed);
    SpeechStats speech;
    speech_get_stats(screen_reader_get_speech(sr), &speech);
    printf(
        "speech: %s, queued %lu, speaker held: %s, utterances %lu, aborted %lu, dropped %lu, underruns %lu, queue dropped %lu\r\n",
        speech.speaking ? "speaking" : "idle",
        (unsigned long)speech.queued,
        speech.speaker_held ? "yes" : "no",
        (unsigned long)speech.utterances,
        (unsigned long)speech.aborted,
        (unsigned long)speech.dropped,
        (unsigned long)speech.underruns,
        (unsigned long)speech.queue_dropped);
    printf(
        "last utterance: %lu ms played, %lu subsamples, %lu ms nominal, worker stack free %lu\r\n",
        (unsigned long)speech.last_played_ms,
        (unsigned long)speech.last_subsamples,
        (unsigned long)speech.last_nominal_ms,
        (unsigned long)speech.stack_free);
    sr_cli_print_voice(sr);
    printf("free heap: %zu\r\n", memmgr_get_free_heap());
}

static void sr_cli_watch(PipeSide* pipe, ScreenReader* sr);

static void sr_cli_say(ScreenReader* sr, FuriString* args) {
    const char* text = furi_string_get_cstr(args);
    size_t length = strlen(text);
    if(length == 0) {
        printf("say what?\r\n");
        return;
    }
    speech_say(screen_reader_get_speech(sr), text, true, false);
    // A queue item holds SPEECH_ITEM_TEXT_MAX - 1 characters; longer text is cut there
    size_t queued = length < SPEECH_ITEM_TEXT_MAX ? length : SPEECH_ITEM_TEXT_MAX - 1;
    if(queued < length) {
        printf(
            "queued %u of %u characters, cut; sr status shows the timing when it is done\r\n",
            (unsigned)queued,
            (unsigned)length);
    } else {
        printf(
            "queued %u characters; sr status shows the timing when it is done\r\n",
            (unsigned)queued);
    }
}

static void sr_cli_play(ScreenReader* sr, FuriString* args) {
    FuriString* path = furi_string_alloc();
    do {
        if(!args_read_probably_quoted_string_and_trim(args, path)) {
            printf("play what? sr play <path> [rate]\r\n");
            break;
        }
        if(furi_string_size(path) >= SPEECH_ITEM_TEXT_MAX) {
            printf("the path is too long: at most %d characters\r\n", SPEECH_ITEM_TEXT_MAX - 1);
            break;
        }
        int rate = 16000;
        if(furi_string_size(args) > 0 &&
           (!args_read_int_and_trim(args, &rate) || rate < 8000 || rate > 32000)) {
            printf("expected a sample rate from 8000 to 32000\r\n");
            break;
        }
        Storage* storage = furi_record_open(RECORD_STORAGE);
        FileInfo info;
        FS_Error error = storage_common_stat(storage, furi_string_get_cstr(path), &info);
        furi_record_close(RECORD_STORAGE);
        if(error != FSE_OK || file_info_is_dir(&info)) {
            printf(
                "cannot play %s: %s\r\n",
                furi_string_get_cstr(path),
                error == FSE_OK ? "it is a folder" : filesystem_api_error_get_desc(error));
            break;
        }
        speech_play(screen_reader_get_speech(sr), furi_string_get_cstr(path), (uint32_t)rate);
        printf(
            "playing %s at %d Hz; sr status shows the timing when it is done\r\n",
            furi_string_get_cstr(path),
            rate);
    } while(false);
    furi_string_free(path);
}

static void sr_cli_chord(ScreenReader* sr, FuriString* args) {
    static const struct {
        const char* key;
        SrChordCommand command;
        const char* does;
    } chords[] = {
        {"up", SrChordReadAll, "read the screen"},
        {"down", SrChordStatus, "read the status bar"},
        {"ok", SrChordRepeat, "repeat the focus"},
        {"okhold", SrChordSpell, "spell the focus"},
        {"downhold", SrChordToggleReader, "turn the reader off or on"},
        {"left", SrChordVolumeDown, "volume down"},
        {"right", SrChordVolumeUp, "volume up"},
    };
    FuriString* key = furi_string_alloc();
    bool has = args_read_string_and_trim(args, key);
    size_t i = 0;
    while(i < COUNT_OF(chords) && !(has && furi_string_cmp_str(key, chords[i].key) == 0))
        i++;
    if(i == COUNT_OF(chords)) {
        printf("sr chord up|down|ok|okhold|downhold|left|right\r\n");
    } else if(chords[i].command != SrChordToggleReader && !screen_reader_is_enabled(sr)) {
        // As on the device: with the reader off the other chords are plain keys
        printf("screen reader is off\r\n");
    } else {
        screen_reader_run_command(sr, chords[i].command);
        printf("back + %s: %s\r\n", chords[i].key, chords[i].does);
    }
    furi_string_free(key);
}

static void sr_cli_set_volume(ScreenReader* sr, FuriString* args) {
    int value = 0;
    if(!args_read_int_and_trim(args, &value) || value < 0 || value > 100) {
        printf("expected a number from 0 to 100\r\n");
        return;
    }
    momentum_settings.sr_volume = (uint32_t)value;
    momentum_settings_save();
    speech_set_volume(screen_reader_get_speech(sr), (uint8_t)value);
    printf("volume set to %d\r\n", value);
}

// The reader reads the setting at every change, so the new delay applies from the next one on
static void sr_cli_delay(FuriString* args) {
    int value = 0;
    if(!args_read_int_and_trim(args, &value) || value < SR_CHANGE_MS_MIN ||
       value > SR_CHANGE_MS_MAX) {
        printf(
            "expected a number of milliseconds from %d to %d; the change delay is %lu ms\r\n",
            SR_CHANGE_MS_MIN,
            SR_CHANGE_MS_MAX,
            (unsigned long)momentum_settings.sr_change_ms);
        return;
    }
    momentum_settings.sr_change_ms = (uint32_t)value;
    momentum_settings_save();
    printf("change delay set to %d ms\r\n", value);
}

static void sr_cli_execute(PipeSide* pipe, FuriString* args, void* context) {
    ScreenReader* sr = context;
    FuriString* cmd = furi_string_alloc();
    do {
        if(!args_read_string_and_trim(args, cmd)) {
            sr_cli_usage();
            break;
        }
        if(furi_string_cmp_str(cmd, "screen") == 0) {
            sr_cli_screen(sr);
        } else if(furi_string_cmp_str(cmd, "watch") == 0) {
            sr_cli_watch(pipe, sr);
        } else if(furi_string_cmp_str(cmd, "on") == 0) {
            screen_reader_set_enabled(sr, true);
            printf("screen reader on\r\n");
        } else if(furi_string_cmp_str(cmd, "off") == 0) {
            screen_reader_set_enabled(sr, false);
            printf("screen reader off\r\n");
        } else if(furi_string_cmp_str(cmd, "status") == 0) {
            sr_cli_status(sr);
        } else if(furi_string_cmp_str(cmd, "say") == 0) {
            sr_cli_say(sr, args);
        } else if(furi_string_cmp_str(cmd, "stop") == 0) {
            speech_stop(screen_reader_get_speech(sr));
            printf("stopped\r\n");
        } else if(furi_string_cmp_str(cmd, "volume") == 0) {
            sr_cli_set_volume(sr, args);
        } else if(furi_string_cmp_str(cmd, "delay") == 0) {
            sr_cli_delay(args);
        } else if(furi_string_cmp_str(cmd, "play") == 0) {
            sr_cli_play(sr, args);
        } else if(furi_string_cmp_str(cmd, "voice") == 0) {
            sr_cli_voice(sr, args);
        } else if(furi_string_cmp_str(cmd, "chord") == 0) {
            sr_cli_chord(sr, args);
        } else {
            sr_cli_usage();
        }
    } while(false);
    furi_string_free(cmd);
}

// Each run of the command gets a thread with this stack, for the whole of an sr watch session.
// The deepest path by GCC's call graph, a number printed through the console pipe, needs about
// 1.3 KB, and about 1.6 KB with an interrupt's frame; the default would be 4 KB.
#define SR_CLI_STACK_SIZE 2048

void screen_reader_cli_register(ScreenReader* sr) {
    CliRegistry* registry = furi_record_open(RECORD_CLI);
    cli_registry_add_command_ex(
        registry, "sr", CliCommandFlagParallelSafe, sr_cli_execute, sr, SR_CLI_STACK_SIZE);
    furi_record_close(RECORD_CLI);
}

static void sr_cli_press(FuriPubSub* input, InputKey key) {
    InputEvent event;
    memset(&event, 0, sizeof(event));
    event.key = key;
    event.type = InputTypePress;
    furi_pubsub_publish(input, &event);
    event.type = InputTypeShort;
    furi_pubsub_publish(input, &event);
    event.type = InputTypeRelease;
    furi_pubsub_publish(input, &event);
}

static void sr_cli_print_screen(ScreenReader* sr) {
    SrScreen* screen = malloc(sizeof(SrScreen));
    screen_reader_get_screen(sr, screen);
    char* text = malloc(SR_DESCRIBE_TEXT_MAX);
    sr_screen_describe(screen, text, SR_DESCRIBE_TEXT_MAX);
    printf("screen: %s\r\n", text);
    free(text);
    free(screen);
}

static void sr_cli_watch(PipeSide* pipe, ScreenReader* sr) {
    FuriMessageQueue* queue = furi_message_queue_alloc(8, sizeof(SrAnnouncement));
    SrAnnouncement* announcement = malloc(sizeof(SrAnnouncement));
    FuriPubSub* input = furi_record_open(RECORD_INPUT_EVENTS);
    FuriPubSub* ascii = furi_record_open(RECORD_ASCII_EVENTS);

    printf("Watching. Arrows move, Enter is OK, Backspace is Back, Tab reads the screen,\r\n");
    printf("letters type into text fields. Ctrl+C stops.\r\n");
    screen_reader_set_watch_queue(sr, queue);
    sr_cli_print_screen(sr);

    bool run = true;
    while(run && pipe_state(pipe) == PipeStateOpen) {
        if(furi_message_queue_get(queue, announcement, 20) == FuriStatusOk) {
            printf("%s: %s\r\n", sr_kind_name(announcement->kind), announcement->text);
        }
        while(run && pipe_bytes_available(pipe)) {
            char c = getchar();
            if(c == CliKeyETX) {
                run = false;
            } else if(c == CliKeyEsc) {
                // Arrow keys arrive as ESC [ A..D within a few milliseconds. Read the rest
                // of a control sequence only while bytes keep arriving, so a bare Escape
                // press or a truncated sequence never blocks the loop. Sequences with
                // parameters (ESC [ 3 ~, ESC [ 1 ; 5 C) are consumed and ignored.
                char seq[8];
                size_t got = 0;
                bool complete = false;
                uint32_t started = furi_get_tick();
                while(got < sizeof(seq) && (furi_get_tick() - started) < 50) {
                    if(!pipe_bytes_available(pipe)) {
                        furi_delay_ms(2);
                        continue;
                    }
                    char b = getchar();
                    if(got == 0) {
                        if(b != '[') break; // not a control sequence, drop it
                        seq[got++] = b;
                        continue;
                    }
                    seq[got++] = b;
                    if(b >= 0x40 && b <= 0x7E) { // final byte of the sequence
                        complete = true;
                        break;
                    }
                }
                if(complete && got == 2) {
                    if(seq[1] == 'A') sr_cli_press(input, InputKeyUp);
                    if(seq[1] == 'B') sr_cli_press(input, InputKeyDown);
                    if(seq[1] == 'C') sr_cli_press(input, InputKeyRight);
                    if(seq[1] == 'D') sr_cli_press(input, InputKeyLeft);
                }
            } else if(c == CliKeyCR || c == CliKeyLF) {
                sr_cli_press(input, InputKeyOk);
            } else if(c == CliKeyBackspace || c == CliKeyDEL) {
                sr_cli_press(input, InputKeyBack);
            } else if(c == CliKeyTab) {
                sr_cli_print_screen(sr);
            } else if(c >= 0x20 && c < 0x7F) {
                AsciiEvent event = {.value = (uint8_t)c};
                furi_pubsub_publish(ascii, &event);
            }
        }
    }

    screen_reader_set_watch_queue(sr, NULL);
    furi_record_close(RECORD_ASCII_EVENTS);
    furi_record_close(RECORD_INPUT_EVENTS);
    furi_message_queue_free(queue);
    free(announcement);
    printf("stopped\r\n");
}
