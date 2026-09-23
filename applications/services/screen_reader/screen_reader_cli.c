#include "screen_reader_cli.h"
#include "speech_queue.h"

#include <furi.h>
#include <cli/cli.h>
#include <toolbox/cli/cli_command.h>
#include <toolbox/cli/cli_ansi.h>
#include <toolbox/args.h>
#include <toolbox/pipe.h>
#include <input/input.h>
#include <momentum/settings.h>
#include <storage/storage.h>

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
    printf("  rate <n>     SAM speed 40..120, bigger is slower (saved)\r\n");
    printf("  volume <n>   0..100 (saved)\r\n");
    printf("  play <path> [rate]  raw 8 bit mono clip from the card, 8000..32000 Hz (16000)\r\n");
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
            "%-12s x=%3d y=%3d %-10s%s%s",
            sr_layer_name(r->layer),
            (int)r->x,
            (int)r->y,
            sr_font_name(r->font),
            r->inverted ? " inverted" : "",
            r->focus ? " FOCUS" : "");
        if(r->count) printf(" %u/%u", (unsigned)r->index, (unsigned)r->count);
        if(r->button) printf(" button=%s", sr_button_name(r->button));
        printf(" \"%s\"\r\n", r->text);
    }
    free(frame);

    SrScreen* screen = malloc(sizeof(SrScreen));
    screen_reader_get_screen(sr, screen);
    char* text = malloc(SR_ANN_TEXT_MAX);
    sr_screen_describe(screen, text, SR_ANN_TEXT_MAX);
    printf("reads as: %s\r\n", text);
    free(text);
    free(screen);
}

static void sr_cli_status(ScreenReader* sr) {
    ScreenReaderStats stats;
    screen_reader_get_stats(sr, &stats);
    printf("enabled: %s\r\n", screen_reader_is_enabled(sr) ? "yes" : "no");
    printf(
        "rate: %lu, volume: %lu, verbosity: %lu\r\n",
        (unsigned long)momentum_settings.sr_rate,
        (unsigned long)momentum_settings.sr_volume,
        (unsigned long)momentum_settings.sr_verbosity);
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

static void sr_cli_set_number(ScreenReader* sr, FuriString* args, bool rate) {
    int value = 0;
    int lo = rate ? 40 : 0;
    int hi = rate ? 120 : 100;
    if(!args_read_int_and_trim(args, &value) || value < lo || value > hi) {
        printf("expected a number from %d to %d\r\n", lo, hi);
        return;
    }
    if(rate) {
        momentum_settings.sr_rate = (uint32_t)value;
    } else {
        momentum_settings.sr_volume = (uint32_t)value;
    }
    momentum_settings_save();
    speech_set_voice(
        screen_reader_get_speech(sr),
        (uint8_t)momentum_settings.sr_rate,
        (uint8_t)momentum_settings.sr_volume);
    printf("%s set to %d\r\n", rate ? "rate" : "volume", value);
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
        } else if(furi_string_cmp_str(cmd, "rate") == 0) {
            sr_cli_set_number(sr, args, true);
        } else if(furi_string_cmp_str(cmd, "volume") == 0) {
            sr_cli_set_number(sr, args, false);
        } else if(furi_string_cmp_str(cmd, "play") == 0) {
            sr_cli_play(sr, args);
        } else {
            sr_cli_usage();
        }
    } while(false);
    furi_string_free(cmd);
}

void screen_reader_cli_register(ScreenReader* sr) {
    CliRegistry* registry = furi_record_open(RECORD_CLI);
    cli_registry_add_command(registry, "sr", CliCommandFlagParallelSafe, sr_cli_execute, sr);
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
    char* text = malloc(SR_ANN_TEXT_MAX);
    sr_screen_describe(screen, text, SR_ANN_TEXT_MAX);
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
