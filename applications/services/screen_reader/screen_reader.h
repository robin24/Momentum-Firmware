/**
 * @file screen_reader.h
 * Screen reader service: captures drawn text, models it, and announces changes.
 */
#pragma once

#include <furi.h>
#include "screen_model.h"
#include "speech.h"
#include "sr_chords.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RECORD_SCREEN_READER "screen_reader"

typedef struct ScreenReader ScreenReader;

typedef struct {
    uint32_t frames; /**< frames processed */
    uint32_t dropped_frames; /**< frames the GUI could not hand over in time */
    uint32_t announcements; /**< announcements handed to speech */
    uint32_t suppressed; /**< desktop announcements not spoken: its changes, and its text going
                              away, with no key press within 2 s; and for 2 s after a lock
                              announcement, or until a key press, every desktop announcement */
} ScreenReaderStats;

/** Copy the last captured frame (raw records). */
void screen_reader_get_frame(ScreenReader* sr, SrFrame* out);

/** Copy the screen description of the last processed frame. */
void screen_reader_get_screen(ScreenReader* sr, SrScreen* out);

/** Attach a queue of SrAnnouncement that receives every announcement. NULL detaches. */
void screen_reader_set_watch_queue(ScreenReader* sr, FuriMessageQueue* queue);

/** Turn the reader on or off and save the setting. */
void screen_reader_set_enabled(ScreenReader* sr, bool enabled);

bool screen_reader_is_enabled(ScreenReader* sr);

void screen_reader_get_stats(ScreenReader* sr, ScreenReaderStats* out);

/** The speech engine, for the console command. */
Speech* screen_reader_get_speech(ScreenReader* sr);

/** Run a chord's command on the reader's thread, as the Back chords do; for sr chord. The latest
 *  command wins when two arrive before the thread runs. */
void screen_reader_run_command(ScreenReader* sr, SrChordCommand command);

#ifdef __cplusplus
}
#endif
