#include "screen_reader_cli.h"

#include <furi.h>
#include <cli/cli.h>
#include <toolbox/cli/cli_command.h>
#include <toolbox/cli/cli_ansi.h>
#include <toolbox/args.h>
#include <toolbox/pipe.h>
#include <input/input.h>
#include <momentum/settings.h>

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

// Unused until Task 7's "sr watch" command reports announcement kinds.
__attribute__((unused)) static const char* sr_kind_name(SrAnnKind kind) {
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
        "frames: %lu, dropped: %lu, announcements: %lu\r\n",
        (unsigned long)stats.frames,
        (unsigned long)stats.dropped_frames,
        (unsigned long)stats.announcements);
    printf("free heap: %zu\r\n", memmgr_get_free_heap());
}

static void sr_cli_watch(PipeSide* pipe, ScreenReader* sr);

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

// Task 7 fills this in. Until then, watch just explains itself.
static void sr_cli_watch(PipeSide* pipe, ScreenReader* sr) {
    UNUSED(pipe);
    UNUSED(sr);
    printf("watch is not implemented yet\r\n");
}
