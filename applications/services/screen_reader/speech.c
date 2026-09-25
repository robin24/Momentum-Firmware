#include "speech.h"

#include "speech_hw.h"
#include "speech_pcm.h"
#include "speech_queue.h"
#include "speech_text.h"
#include "speech_voice.h"

#include <furi_hal.h>
#include <storage/storage.h>

#define TAG "Speech"

#define SPEECH_RING_SIZE          8192 /* two halves of 82 ms at 20 us per slot */
#define SPEECH_HALF               (SPEECH_RING_SIZE / 2)
#define SPEECH_IDLE_RELEASE_MS    200
#define SPEECH_ACQUIRE_TIMEOUT_MS 500
#define SPEECH_EVENT_TIMEOUT_MS   200
#define SPEECH_BUS_POLL_MS        5
#define SPEECH_FILE_CHUNK         512 /* bytes read from the card at a time */
#define SPEECH_FILE_RATE_MIN      8000
#define SPEECH_FILE_RATE_MAX      32000

#define SPEECH_FLAG_WORK  (1 << 0)
#define SPEECH_FLAG_STOP  (1 << 1)
#define SPEECH_FLAG_HALF0 (1 << 2) /* DMA finished playing the first half */
#define SPEECH_FLAG_HALF1 (1 << 3) /* DMA finished playing the second half */

#define SPEECH_VOICE_HOLD_NS       (1000000000u / SPEECH_VOICE_HZ)
#define SPEECH_SPELL_WORD_GAP_MS   150 /* at least this between the words of a spelled item */
#define SPEECH_VOICE_MISSING_PATH  "/ext/sr/missing.txt"
#define SPEECH_VOICE_SETTINGS_PATH SPEECH_VOICE_DIR "/voice.txt"

// A spelled run must reach the clip player whole
_Static_assert(SPEECH_TEXT_SPELL_RUN_MAX <= SPEECH_VOICE_WORD_MAX - 1, "spelled runs too long");

struct Speech {
    FuriThread* thread;
    volatile FuriThreadId thread_id;
    FuriMutex* mutex;
    Storage* storage;
    uint8_t* file_buffer; /* SPEECH_FILE_CHUNK bytes, worker thread only */

    // Guarded by mutex
    SpeechQueue queue;
    SpeechStats stats;

    // Set by any thread, read by the worker
    volatile uint8_t volume;
    volatile bool voice_enabled;

    // Worker thread only
    SpeechPcm pcm;
    uint8_t ring[SPEECH_RING_SIZE];
    uint8_t fill_half;
    uint16_t fill_index;
    bool running;
    bool aborted;
    uint32_t generation;
    uint32_t started; /* tick at which the rendering of the current item began */
    uint8_t lut_volume;
    char expanded[SPEECH_TEXT_MAX];

    // Recorded voice: written by the worker only, speech_get_voice_stats reads a snapshot
    bool vocabulary; /* SPEECH_VOICE_DIR was found on the card */
    char voice_settings[64]; /* first line of voice.txt */
    File* voice_file; /* reused for every clip and the missing log */
    SpeechVoiceState voice;
    char path[SPEECH_VOICE_PATH_MAX];
    uint32_t open_max_ms; /* longest clip open since boot, found or not */
    uint32_t open_last_ms;
    uint32_t muted; /* items completed silently: voice off, or no vocabulary on the card */
};

static void speech_lock(Speech* speech) {
    furi_check(furi_mutex_acquire(speech->mutex, FuriWaitForever) == FuriStatusOk);
}

static void speech_unlock(Speech* speech) {
    furi_check(furi_mutex_release(speech->mutex) == FuriStatusOk);
}

// Interrupt context
static void speech_dma_event(bool second_half, void* context) {
    Speech* speech = context;
    furi_thread_flags_set(speech->thread_id, second_half ? SPEECH_FLAG_HALF1 : SPEECH_FLAG_HALF0);
}

static uint32_t half_flag(uint8_t half) {
    return half == 0 ? SPEECH_FLAG_HALF0 : SPEECH_FLAG_HALF1;
}

/**
 * A newer utterance or a stop request makes the current one worthless. Both bump the queue
 * generation under the mutex, on the requesting thread, and every item carries the generation
 * it was pushed in, so this comparison is the only thing that ends an utterance; the stop flag
 * merely wakes a DMA wait so that the comparison runs at once.
 */
static bool speech_superseded(Speech* speech) {
    return *(volatile uint32_t*)&speech->queue.generation != speech->generation;
}

/**
 * Wait until the DMA has finished playing `half`. False when the utterance was superseded
 * meanwhile or when no event came in time. A stop flag that wakes the wait is judged by the
 * generation: a flag left over from a stop that already ended an earlier utterance makes this
 * one look and keep waiting.
 */
static bool speech_wait_half_played(Speech* speech, uint8_t half) {
    uint32_t wanted = half_flag(half);
    uint32_t started = furi_get_tick();
    while(true) {
        uint32_t waited = furi_get_tick() - started;
        if(waited >= SPEECH_EVENT_TIMEOUT_MS) return false;
        uint32_t flags = furi_thread_flags_wait(
            wanted | SPEECH_FLAG_STOP, FuriFlagWaitAny, SPEECH_EVENT_TIMEOUT_MS - waited);
        if(flags & FuriFlagError) return false;
        if(speech_superseded(speech)) return false;
        if(flags & wanted) return true;
    }
}

// Called by the PCM stage for every duty byte, on the worker thread
static bool speech_emit(uint8_t duty, void* context) {
    Speech* speech = context;
    if(speech_superseded(speech)) {
        speech->aborted = true;
        return false;
    }
    speech->ring[speech->fill_half * SPEECH_HALF + speech->fill_index++] = duty;
    if(speech->fill_index < SPEECH_HALF) return true;
    speech->fill_index = 0;

    if(!speech->running) {
        // First half complete: start playing it; the second half holds silence until filled
        furi_thread_flags_clear(SPEECH_FLAG_HALF0 | SPEECH_FLAG_HALF1);
        speech_hw_start(speech->ring, SPEECH_RING_SIZE, speech_dma_event, speech);
        speech->running = true;
        speech->fill_half = 1;
        return true;
    }

    // Continue in the other half once the DMA has finished with it. If it has already
    // finished, the DMA moved into the half just written while it was still being filled.
    uint8_t next = speech->fill_half ^ 1;
    if(furi_thread_flags_get() & half_flag(next)) {
        speech_lock(speech);
        speech->stats.underruns++;
        speech_unlock(speech);
    }
    if(!speech_wait_half_played(speech, next)) {
        speech->aborted = true;
        return false;
    }
    speech->fill_half = next;
    return true;
}

/**
 * Play out what was rendered and stop the hardware cleanly. A stop request or a newer
 * utterance ends the play out at the wait it lands in and the hardware stops at once: what
 * would follow is padding, and a key press must silence the speaker without delay.
 */
static void speech_flush(Speech* speech) {
    uint8_t silence = speech_pcm_silence(&speech->pcm);
    if(!speech->running) {
        if(speech->fill_index == 0) return; // nothing was rendered
        memset(&speech->ring[speech->fill_index], silence, SPEECH_HALF - speech->fill_index);
        memset(&speech->ring[SPEECH_HALF], silence, SPEECH_HALF);
        furi_thread_flags_clear(SPEECH_FLAG_HALF0 | SPEECH_FLAG_HALF1);
        speech_hw_start(speech->ring, SPEECH_RING_SIZE, speech_dma_event, speech);
        speech->running = true;
        speech_wait_half_played(speech, 0);
    } else {
        // The half being filled ends in silence. The DMA is still in the other half; once
        // that has played it moves into this one, the other half is silenced too, and the
        // hardware stops when the DMA has played this one
        uint8_t h = speech->fill_half;
        memset(
            &speech->ring[h * SPEECH_HALF + speech->fill_index],
            silence,
            SPEECH_HALF - speech->fill_index);
        if(speech_wait_half_played(speech, h ^ 1)) {
            memset(&speech->ring[(h ^ 1) * SPEECH_HALF], silence, SPEECH_HALF);
            speech_wait_half_played(speech, h);
        }
    }
    if(speech_superseded(speech)) speech->aborted = true;
    speech_hw_stop();
    speech->running = false;
}

static bool speech_take_speaker(Speech* speech) {
    if(furi_hal_speaker_is_mine()) return true;
    // Someone else drives TIM16: the notification service holds the speaker for a whole beep
    // sequence (four 50 ms notes on a successful read, fired back to back with the result
    // screen), the NFC-V listener for a whole emulation. Wait for the bus to go quiet instead
    // of dropping the announcement, then take the mutex with what is left of the timeout; an
    // emulation outlasts it and the utterance is dropped as before. A stop request or a newer
    // utterance ends the wait at once
    uint32_t started = furi_get_tick();
    while(furi_hal_bus_is_enabled(FuriHalBusTIM16)) {
        if(speech_superseded(speech)) {
            speech->aborted = true;
            return false;
        }
        if(furi_get_tick() - started >= SPEECH_ACQUIRE_TIMEOUT_MS) return false;
        furi_delay_ms(SPEECH_BUS_POLL_MS);
    }
    uint32_t waited = furi_get_tick() - started;
    uint32_t remaining = waited < SPEECH_ACQUIRE_TIMEOUT_MS ? SPEECH_ACQUIRE_TIMEOUT_MS - waited :
                                                              0;
    if(!furi_hal_speaker_acquire(remaining)) return false;
    speech_lock(speech);
    speech->stats.speaker_held = true;
    speech_unlock(speech);
    return true;
}

static void speech_drop_speaker(Speech* speech) {
    if(!furi_hal_speaker_is_mine()) return;
    furi_hal_speaker_release();
    speech_lock(speech);
    speech->stats.speaker_held = false;
    speech_unlock(speech);
}

/**
 * Common start of an item, spoken or played: take the speaker, refresh the loudness table,
 * reset the PCM stage and the ring, count the utterance. False when the speaker could not be
 * taken; the item is then counted as aborted or dropped and nothing plays.
 */
static bool speech_item_begin(Speech* speech, const SpeechItem* item) {
    speech->generation = item->generation;
    speech->aborted = false;
    speech->fill_half = 0;
    speech->fill_index = 0;
    speech->running = false;

    if(!speech_take_speaker(speech)) {
        speech_lock(speech);
        if(speech->aborted) {
            speech->stats.aborted++; // a stop request or a newer utterance ended the wait
        } else {
            speech->stats.dropped++; // the timer stayed busy, as during an NFC-V emulation
        }
        speech->stats.speaking = false;
        speech_unlock(speech);
        return false;
    }

    uint8_t volume = speech->volume;
    if(volume != speech->lut_volume) {
        speech_pcm_set_volume(&speech->pcm, volume);
        speech->lut_volume = volume;
    }
    speech_pcm_reset(&speech->pcm);
    memset(speech->ring, speech_pcm_silence(&speech->pcm), SPEECH_RING_SIZE);

    speech_lock(speech);
    speech->stats.utterances++;
    speech_unlock(speech);
    return true;
}

/** Common end of an item: play out what was rendered or stop at once, then record the timing. */
static void speech_item_end(Speech* speech) {
    if(speech->aborted) {
        if(speech->running) {
            speech_hw_stop();
            speech->running = false;
        }
    } else {
        speech_flush(speech);
    }

    speech_lock(speech);
    speech->stats.speaking = false;
    if(speech->aborted) speech->stats.aborted++;
    speech->stats.last_subsamples = speech->pcm.subsamples;
    speech->stats.last_nominal_ms = speech->pcm.nominal_us / 1000;
    speech->stats.last_played_ms = furi_get_tick() - speech->started;
    speech_unlock(speech);
}

/**
 * The card mounts after the services start, so the vocabulary is looked for when it is
 * first needed and then remembered while the card stays mounted. A removed card is noticed at
 * the next utterance and the vocabulary is looked for again once a card is back. Reads the
 * generator's settings line for sr voice status.
 */
static bool speech_voice_ready(Speech* speech) {
    if(storage_sd_status(speech->storage) != FSE_OK) {
        speech->vocabulary = false;
        speech->voice_settings[0] = '\0';
        return false;
    }
    if(speech->vocabulary) return true;
    if(!storage_dir_exists(speech->storage, SPEECH_VOICE_DIR)) return false;
    speech->vocabulary = true;
    File* file = speech->voice_file;
    if(storage_file_open(file, SPEECH_VOICE_SETTINGS_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        size_t n =
            storage_file_read(file, speech->voice_settings, sizeof(speech->voice_settings) - 1);
        speech->voice_settings[n] = '\0';
        char* nl = strpbrk(speech->voice_settings, "\r\n");
        if(nl) *nl = '\0';
    }
    storage_file_close(file);
    return true;
}

/** Silence of `ms` at the clip sample rate, through the same stage as the clips. */
static bool speech_push_silence(Speech* speech, uint32_t ms) {
    memset(speech->file_buffer, SPEECH_SILENCE_VALUE, SPEECH_FILE_CHUNK);
    uint32_t left = ms * (SPEECH_VOICE_HZ / 1000);
    while(left > 0) {
        size_t n = left < SPEECH_FILE_CHUNK ? left : SPEECH_FILE_CHUNK;
        if(!speech_pcm_push_raw(
               &speech->pcm, speech->file_buffer, n, SPEECH_VOICE_HOLD_NS, speech_emit, speech)) {
            return false;
        }
        left -= n;
    }
    return true;
}

/**
 * One clip from the card. False when the file could not be opened: the word is missing. A
 * superseded utterance ends the clip through speech_emit, which sets aborted, and returns true.
 */
static bool speech_play_clip(Speech* speech, const char* path) {
    File* file = speech->voice_file;
    uint32_t start = furi_get_tick();
    bool opened = storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING);
    uint32_t took = furi_get_tick() - start;
    speech->open_last_ms = took;
    if(took > speech->open_max_ms) speech->open_max_ms = took;
    if(!opened) {
        storage_file_close(file);
        return false;
    }
    while(!speech->aborted) {
        size_t n = storage_file_read(file, speech->file_buffer, SPEECH_FILE_CHUNK);
        if(n == 0) break;
        if(!speech_pcm_push_raw(
               &speech->pcm, speech->file_buffer, n, SPEECH_VOICE_HOLD_NS, speech_emit, speech)) {
            break;
        }
    }
    storage_file_close(file);
    return true;
}

/**
 * A word the vocabulary lacks is spelled with the letter and digit clips, a short gap between
 * them. A character whose clip is missing too is skipped after the gap and its clip name is
 * recorded as missing, once per boot, like a word. Each letter played from a clip counts as a
 * clip in the voice status.
 */
static void speech_spell_word(Speech* speech, const char* word) {
    char name[3];
    for(const char* p = word; *p != '\0' && !speech->aborted; p++) {
        if(!speech_voice_letter_clip(*p, name)) continue;
        if(p != word && !speech_push_silence(speech, SPEECH_VOICE_LETTER_GAP_MS)) return;
        if(speech_voice_path(name, speech->path, sizeof(speech->path)) > 0 &&
           speech_play_clip(speech, speech->path)) {
            speech->voice.clip_words++;
            continue;
        }
        speech_voice_missing(&speech->voice, name);
    }
}

/**
 * The expanded text word by word: a clip when the card has it, spelled with the letter clips
 * when not. A token's pause is pushed before the next token, so it separates the two and none
 * follows the last one: the play out starts right after the last word. A token of punctuation
 * only speaks nothing, but its pause still goes between its neighbours. A spelled item spells
 * every word, with at least SPEECH_SPELL_WORD_GAP_MS between words.
 */
static void speech_speak_words(Speech* speech, bool spell) {
    size_t pos = 0;
    SpeechVoiceWord word;
    uint32_t pending_pause_ms = 0;
    while(!speech->aborted && speech_voice_next_word(speech->expanded, &pos, &word)) {
        if(pending_pause_ms > 0 && !speech_push_silence(speech, pending_pause_ms)) break;
        pending_pause_ms = word.pause_ms;
        if(spell && pending_pause_ms < SPEECH_SPELL_WORD_GAP_MS) {
            pending_pause_ms = SPEECH_SPELL_WORD_GAP_MS;
        }
        if(word.word[0] == '\0') continue;
        if(spell) {
            // Asked for: its letters count as clips, the word is neither a fallback nor missing
            speech_spell_word(speech, word.word);
        } else if(
            speech_voice_path(word.word, speech->path, sizeof(speech->path)) > 0 &&
            speech_play_clip(speech, speech->path)) {
            speech->voice.clip_words++;
        } else {
            speech->voice.fallback_words++;
            speech_voice_missing(&speech->voice, word.word);
            speech_spell_word(speech, word.word);
        }
    }
}

/**
 * Append the words the last utterance lacked to the missing list on the card. When the file
 * cannot be opened the words stay in the log and a later utterance writes them.
 */
static void speech_write_missing(Speech* speech) {
    size_t n = speech_voice_log_count(&speech->voice);
    if(n == 0) return;
    File* file = speech->voice_file;
    bool opened = storage_file_open(file, SPEECH_VOICE_MISSING_PATH, FSAM_WRITE, FSOM_OPEN_APPEND);
    if(opened) {
        for(size_t i = 0; i < n; i++) {
            const char* w = speech_voice_log_word(&speech->voice, i);
            storage_file_write(file, w, strlen(w));
            storage_file_write(file, "\n", 1);
        }
    }
    storage_file_close(file);
    if(opened) speech_voice_log_clear(&speech->voice);
}

static void speech_speak_item(Speech* speech, const SpeechItem* item) {
    // Muted (sr voice off), or no vocabulary on the card: the item completes at once, without
    // the speaker, a file or the missing list; the voice status counts it
    if(!speech->voice_enabled || !speech_voice_ready(speech)) {
        speech->muted++;
        speech_lock(speech);
        speech->stats.speaking = false;
        speech_unlock(speech);
        return;
    }
    if(!speech_item_begin(speech, item)) return;

    // A spelled item keeps its own letters and digits; one without any is said instead
    bool spell = false;
    if(item->spell) {
        spell = speech_text_spell_copy(item->text, speech->expanded, sizeof(speech->expanded)) > 0;
    }
    if(!spell) speech_text_expand(item->text, speech->expanded, sizeof(speech->expanded));
    speech->started = furi_get_tick();
    speech_speak_words(speech, spell);
    speech_item_end(speech);
    speech_write_missing(speech);
}

static uint32_t speech_clamp_rate(uint32_t rate) {
    if(rate < SPEECH_FILE_RATE_MIN) return SPEECH_FILE_RATE_MIN;
    if(rate > SPEECH_FILE_RATE_MAX) return SPEECH_FILE_RATE_MAX;
    return rate;
}

/**
 * A file item: raw unsigned 8 bit mono samples from the card, read 512 bytes at a time and
 * each held for 1000000000 / rate nanoseconds, through the same ring, speaker and stop rules
 * as speech (speech_emit refuses the next sample once the item is superseded). A file that
 * cannot be opened is dropped.
 */
static void speech_play_item(Speech* speech, const SpeechItem* item) {
    File* file = storage_file_alloc(speech->storage);
    if(!storage_file_open(file, item->text, FSAM_READ, FSOM_OPEN_EXISTING)) {
        FURI_LOG_W(TAG, "Cannot open %s", item->text);
        storage_file_close(file);
        storage_file_free(file);
        speech_lock(speech);
        speech->stats.dropped++;
        speech->stats.speaking = false;
        speech_unlock(speech);
        return;
    }

    if(speech_item_begin(speech, item)) {
        uint32_t hold_ns = 1000000000u / speech_clamp_rate(item->rate);
        speech->started = furi_get_tick();
        while(!speech->aborted) {
            size_t n = storage_file_read(file, speech->file_buffer, SPEECH_FILE_CHUNK);
            if(n == 0) break; // end of file, or a read error
            if(!speech_pcm_push_raw(
                   &speech->pcm, speech->file_buffer, n, hold_ns, speech_emit, speech)) {
                break; // superseded, stopped, or no DMA event: speech_emit set aborted
            }
        }
        speech_item_end(speech);
    }
    storage_file_close(file);
    storage_file_free(file);
}

static bool speech_pop(Speech* speech, SpeechItem* item) {
    speech_lock(speech);
    bool got = speech_queue_pop(&speech->queue, item);
    if(got) speech->stats.speaking = true; // from here until played out or dropped
    speech_unlock(speech);
    return got;
}

static int32_t speech_worker(void* context) {
    Speech* speech = context;
    speech->thread_id = furi_thread_get_current_id();
    SpeechItem* item = malloc(sizeof(SpeechItem));

    while(true) {
        uint32_t timeout = furi_hal_speaker_is_mine() ? SPEECH_IDLE_RELEASE_MS : FuriWaitForever;
        uint32_t flags =
            furi_thread_flags_wait(SPEECH_FLAG_WORK | SPEECH_FLAG_STOP, FuriFlagWaitAny, timeout);
        if(flags & FuriFlagError) {
            // Quiet for a while: hand the speaker back so beeps and other apps can use it
            speech_drop_speaker(speech);
            continue;
        }
        // A stop request has already retired the queue on the requesting thread; its flag
        // only wakes this loop, which then finds nothing or what was pushed after the request
        while(speech_pop(speech, item)) {
            if(item->file) {
                speech_play_item(speech, item);
            } else {
                speech_speak_item(speech, item);
            }
        }
    }
    return 0;
}

Speech* speech_alloc(void) {
    Speech* speech = malloc(sizeof(Speech));
    memset(speech, 0, sizeof(Speech));
    speech->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    speech_queue_init(&speech->queue);
    speech->volume = 100;
    speech->lut_volume = 100;
    speech_pcm_init(&speech->pcm, 100);
    speech->storage = furi_record_open(RECORD_STORAGE);
    speech->file_buffer = malloc(SPEECH_FILE_CHUNK);
    speech->voice_file = storage_file_alloc(speech->storage);
    speech_voice_state_init(&speech->voice);
    speech->voice_enabled = true;

    speech->thread = furi_thread_alloc_ex("SpeechWorker", 3 * 1024, speech_worker, speech);
    furi_thread_set_priority(speech->thread, FuriThreadPriorityHigh);
    furi_thread_start(speech->thread);
    while(!speech->thread_id)
        furi_delay_ms(1);
    return speech;
}

typedef enum {
    SpeechPushWords, /* speech_queue_push with interrupt and replaceable */
    SpeechPushParts,
    SpeechPushSpelled,
    SpeechPushFile, /* the text is the path, played at rate */
} SpeechPushKind;

/** Queue under the mutex, all parts of a text at once, keep the drop count for the status, then
 *  wake the worker. */
static void speech_push(
    Speech* speech,
    SpeechPushKind kind,
    const char* text,
    bool interrupt,
    bool replaceable,
    uint32_t rate) {
    speech_lock(speech);
    switch(kind) {
    case SpeechPushWords:
        speech_queue_push(&speech->queue, text, interrupt, replaceable);
        break;
    case SpeechPushParts:
        speech_queue_push_parts(&speech->queue, text);
        break;
    case SpeechPushSpelled:
        speech_queue_push_spelled(&speech->queue, text);
        break;
    case SpeechPushFile:
        speech_queue_push_file(&speech->queue, text, rate);
        break;
    }
    speech->stats.queue_dropped = speech->queue.dropped;
    speech_unlock(speech);
    furi_thread_flags_set(speech->thread_id, SPEECH_FLAG_WORK);
}

void speech_say(Speech* speech, const char* text, bool interrupt, bool replaceable) {
    furi_check(speech && text);
    speech_push(speech, SpeechPushWords, text, interrupt, replaceable, 0);
}

void speech_say_parts(Speech* speech, const char* text) {
    furi_check(speech && text);
    speech_push(speech, SpeechPushParts, text, true, false, 0);
}

void speech_say_spelled(Speech* speech, const char* text) {
    furi_check(speech && text);
    speech_push(speech, SpeechPushSpelled, text, true, false, 0);
}

void speech_play(Speech* speech, const char* path, uint32_t rate) {
    furi_check(speech && path);
    speech_push(speech, SpeechPushFile, path, true, false, speech_clamp_rate(rate));
}

void speech_stop(Speech* speech) {
    furi_check(speech);
    // Retire the queue here, on the requesting thread, so that the stop takes effect at the
    // request: what waits is dropped and the generation moves on, which supersedes what is
    // being spoken at its next sample or wake up, while anything pushed from now on carries
    // the new generation and survives however late the worker looks. Applied on the worker
    // instead, the stop of a key press wiped the announcement that same press caused, pushed
    // 50 ms later while the worker was still playing out. The flag then wakes the worker out
    // of a DMA wait. The mutex is held for microseconds by every user, and the callers are
    // threads (input service, console, reader), never interrupts
    speech_lock(speech);
    speech_queue_stop(&speech->queue);
    speech_unlock(speech);
    furi_thread_flags_set(speech->thread_id, SPEECH_FLAG_STOP);
}

void speech_set_volume(Speech* speech, uint8_t volume) {
    furi_check(speech);
    speech->volume = volume;
}

bool speech_is_busy(Speech* speech) {
    furi_check(speech);
    speech_lock(speech);
    bool busy = speech->stats.speaking || speech_queue_count(&speech->queue) > 0;
    speech_unlock(speech);
    return busy;
}

void speech_get_stats(Speech* speech, SpeechStats* out) {
    furi_check(speech && out);
    speech_lock(speech);
    *out = speech->stats;
    out->queued = speech_queue_count(&speech->queue);
    speech_unlock(speech);
    out->stack_free = furi_thread_get_stack_space(speech->thread_id);
}

void speech_set_voice_clips(Speech* speech, bool enabled) {
    furi_check(speech);
    speech->voice_enabled = enabled;
}

void speech_get_voice_stats(Speech* speech, SpeechVoiceStats* out) {
    furi_check(speech && out);
    // The worker owns these counters and writes them one word at a time; 32 bit reads are
    // atomic on this core, so the snapshot is consistent enough for a status line
    out->enabled = speech->voice_enabled;
    out->vocabulary = speech->vocabulary;
    out->clip_words = *(volatile uint32_t*)&speech->voice.clip_words;
    out->fallback_words = *(volatile uint32_t*)&speech->voice.fallback_words;
    out->missing_words = *(volatile uint32_t*)&speech->voice.missing_words;
    out->muted = *(volatile uint32_t*)&speech->muted;
    out->open_max_ms = *(volatile uint32_t*)&speech->open_max_ms;
    out->open_last_ms = *(volatile uint32_t*)&speech->open_last_ms;
    strncpy(out->settings, speech->voice_settings, sizeof(out->settings) - 1);
    out->settings[sizeof(out->settings) - 1] = '\0';
}
