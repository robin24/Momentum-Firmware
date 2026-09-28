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
// Every clip opened takes about 0.7 KB of the heap for a moment (a file object with its own
// sector buffer on the storage thread, paths and a node): below this largest free block an item
// is not spoken, so an app near the heap's floor is not pushed over it by speech
#define SPEECH_HEAP_MIN           4096
#define SPEECH_FILE_RATE_MIN      8000
#define SPEECH_FILE_RATE_MAX      32000

#define SPEECH_FLAG_WORK  (1 << 0)
#define SPEECH_FLAG_STOP  (1 << 1)
#define SPEECH_FLAG_HALF0 (1 << 2) /* DMA finished playing the first half */
#define SPEECH_FLAG_HALF1 (1 << 3) /* DMA finished playing the second half */

#define SPEECH_VOICE_HOLD_NS     (1000000000u / SPEECH_VOICE_HZ)
#define SPEECH_SPELL_WORD_GAP_MS 150 /* at least this between the words of a spelled item */

// A spelled run must reach the clip player whole
_Static_assert(SPEECH_TEXT_SPELL_RUN_MAX <= SPEECH_VOICE_WORD_MAX - 1, "spelled runs too long");

// The voice status holds a set name whole; speech.h writes the size as a number
_Static_assert(
    sizeof(((SpeechVoiceStats*)0)->set) == SPEECH_VOICE_SET_MAX &&
        sizeof(((SpeechVoiceStats*)0)->wanted) == SPEECH_VOICE_SET_MAX,
    "the set names of SpeechVoiceStats must be SPEECH_VOICE_SET_MAX bytes");

struct Speech {
    FuriThread* thread;
    volatile FuriThreadId thread_id;
    FuriMutex* mutex;
    Storage* storage;
    uint8_t* file_buffer; /* SPEECH_FILE_CHUNK bytes, worker thread only */

    // Guarded by mutex
    SpeechQueue queue;
    SpeechStats stats;
    char wanted_set[SPEECH_VOICE_SET_MAX]; /* the set asked for, empty for the first found */
    volatile bool set_changed; /* wanted_set changed since the worker last took it */

    // Set by any thread, read by the worker
    volatile uint8_t volume;
    volatile bool voice_enabled;
    // A timed mute, sr voice off with seconds: guarded by mutex, with voice_enabled
    bool voice_timed;
    uint32_t voice_on_at; /* tick at which a timed mute ends */
    volatile bool card_changed; /* a card mounted or removed since the worker last looked */

    // Worker thread only
    SpeechPcm pcm;
    uint8_t ring[SPEECH_RING_SIZE];
    uint8_t ring_silence; /* the ring's silence at this item's volume; set before the DMA runs */
    uint8_t fill_half;
    uint16_t fill_index;
    bool running;
    bool aborted;
    uint32_t generation;
    uint32_t started; /* tick at which the rendering of the current item began */
    uint8_t lut_volume;
    char expanded[SPEECH_TEXT_MAX];

    // Recorded voice: written by the worker only, speech_get_voice_stats reads a snapshot
    bool vocabulary; /* a set was resolved on the mounted card; false: resolve at the next item */
    char wanted[SPEECH_VOICE_SET_MAX]; /* wanted_set as the worker last took it */
    char set[SPEECH_VOICE_SET_MAX]; /* the set in use, empty while vocabulary is false */
    char last_set[SPEECH_VOICE_SET_MAX]; /* the set resolved before; stays while none is in use */
    char voice_settings[64]; /* first line of the set's voice.txt */
    File* voice_file; /* reused for every clip, the missing log and the look for a set */
    bool clip_tried; /* the current item tried to open a clip */
    bool clip_opened; /* and opened at least one */
    bool card_lost; /* and an open failed with the card not ready or failing */
    bool missing_failed; /* the set's missing.txt could not be written: not tried again till the
                            set is resolved again */
    SpeechVoiceState voice;
    char path[SPEECH_VOICE_PATH_MAX];
    uint32_t open_max_ms; /* longest clip open since boot, found or not */
    uint32_t open_last_ms;
    uint32_t muted; /* items completed silently: voice off, or no vocabulary on the card */
    uint32_t low_memory; /* items completed silently: too little free memory for the clips */
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
    // The half just played is silenced before the worker hears of it: a worker late to refill
    // it (a slow open, a busy card) then leaves silence where the DMA would play the old audio
    // again, and a blocked one loops silence. About 20 us every 82 ms
    memset(&speech->ring[second_half ? SPEECH_HALF : 0], speech->ring_silence, SPEECH_HALF);
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
        // A time-out comes early after a burst of wake-ups by other flags, since the wait takes
        // the whole time since its start off what is left at every one (furi_thread_flags_wait):
        // this loop's own clock decides instead, and a mid-word cut is not taken for the end
        if(flags & FuriFlagError) continue;
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
    speech->ring_silence = speech_pcm_silence(&speech->pcm);
    memset(speech->ring, speech->ring_silence, SPEECH_RING_SIZE);

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
 * The voice sets on the card, through fn: the folders under SPEECH_VOICE_SETS_DIR that
 * speech_voice_set_listable takes, in the order the card lists them (a name is read one byte
 * longer than a set name, so that a longer one is seen as such and left out). dir is a closed
 * handle, closed again at the end, so the worker passes its own and speech_voice_sets one of
 * its own. Returns the count; 0 without a card or without the folder.
 */
static size_t
    speech_voice_walk(File* dir, void (*fn)(const char* name, void* context), void* context) {
    char name[SPEECH_VOICE_SET_MAX + 1];
    FileInfo info;
    size_t count = 0;
    if(storage_dir_open(dir, SPEECH_VOICE_SETS_DIR)) {
        while(storage_dir_read(dir, &info, name, sizeof(name))) {
            if(!(info.flags & FSF_DIRECTORY) || !speech_voice_set_listable(name)) continue;
            fn(name, context);
            count++;
        }
    }
    storage_dir_close(dir);
    return count;
}

// speech_voice_walk's fn for the fallback: keeps the smallest name (speech_voice_set_before)
static void speech_voice_keep_first(const char* name, void* context) {
    char* first = context;
    if(speech_voice_set_before(name, first)) strlcpy(first, name, SPEECH_VOICE_SET_MAX);
}

/**
 * No set in use, so that the next text item resolves one: without a card, when a set is asked
 * for, and after an item that opened no clip at all. The set's name and settings line go too.
 */
static void speech_voice_forget(Speech* speech) {
    speech->vocabulary = false;
    speech->set[0] = '\0';
    speech->voice_settings[0] = '\0';
    speech->missing_failed = false;
}

// Whether the card has the folder of the set asked for; false when none was asked for
static bool speech_voice_wanted_there(Speech* speech) {
    return speech->wanted[0] != '\0' &&
           speech_voice_set_file(speech->wanted, "", speech->path, sizeof(speech->path)) > 0 &&
           storage_dir_exists(speech->storage, speech->path);
}

/**
 * At the start of a text item: a set asked for since the last item is taken over, and the set
 * is resolved again at speech_voice_ready; until then none is in use, also while the voice is
 * off. speech_set_voice_set writes wanted_set and the flag under the mutex, on any thread; only
 * the worker reads them, here.
 */
static void speech_take_voice_set(Speech* speech) {
    speech_lock(speech);
    if(speech->set_changed) {
        strlcpy(speech->wanted, speech->wanted_set, sizeof(speech->wanted));
        speech->set_changed = false;
        speech_voice_forget(speech);
    }
    speech_unlock(speech);
    // A card mounted or removed since the last item, perhaps another card: the set is resolved
    // again, as storage_sd_status alone does not tell a card swapped between two items
    if(speech->card_changed) {
        speech->card_changed = false;
        speech_voice_forget(speech);
    }
}

/**
 * The card mounts after the services start, so the voice set is resolved when it is first
 * needed and then kept while the card stays mounted. It is resolved again after a card mount
 * (a removal is noticed at the next utterance), when a set is asked for (sr voice use asks even
 * for the one in use), after an item that tried clips and opened none (the set's folder has
 * gone), and on a fallback as soon as the folder of the set asked for is on the card: one look
 * at the card per item, and only while on a fallback. The set asked for is used when its folder
 * is there; otherwise, and when none was asked for, the first set in name order, silently: sr
 * voice status shows both names. With no set at all the item is silent and the next one looks
 * again. Reads the set's settings line for sr voice status.
 */
static bool speech_voice_ready(Speech* speech) {
    if(storage_sd_status(speech->storage) != FSE_OK) {
        speech_voice_forget(speech);
        return false;
    }
    bool wanted_there;
    if(speech->vocabulary) {
        bool fallback = speech->wanted[0] != '\0' && strcmp(speech->wanted, speech->set) != 0;
        if(!fallback || !speech_voice_wanted_there(speech)) return true;
        wanted_there = true;
    } else {
        wanted_there = speech_voice_wanted_there(speech);
    }

    speech_voice_forget(speech);
    if(wanted_there) {
        strlcpy(speech->set, speech->wanted, sizeof(speech->set));
    } else {
        char first[SPEECH_VOICE_SET_MAX] = "";
        speech_voice_walk(speech->voice_file, speech_voice_keep_first, first);
        if(first[0] == '\0') return false;
        strlcpy(speech->set, first, sizeof(speech->set));
    }
    speech->vocabulary = true;
    // Another set than before (at the first resolution there is nothing to forget yet): its
    // missing list has none of the words recorded for the old one, so they are recorded afresh.
    // The same set again (the card back, or a wanted name that fell back to it) keeps them
    if(strcmp(speech->set, speech->last_set) != 0) {
        speech_voice_missing_reset(&speech->voice);
        strlcpy(speech->last_set, speech->set, sizeof(speech->last_set));
    }

    File* file = speech->voice_file;
    if(speech_voice_set_file(speech->set, "voice.txt", speech->path, sizeof(speech->path)) > 0) {
        if(storage_file_open(file, speech->path, FSAM_READ, FSOM_OPEN_EXISTING)) {
            size_t n = storage_file_read(
                file, speech->voice_settings, sizeof(speech->voice_settings) - 1);
            speech->voice_settings[n] = '\0';
            char* nl = strpbrk(speech->voice_settings, "\r\n");
            if(nl) *nl = '\0';
        }
        storage_file_close(file);
    }
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
    speech->clip_tried = true;
    if(!opened) {
        FS_Error error = storage_file_get_error(file);
        storage_file_close(file);
        // Not the word but the card: gone or failing. The item ends, and its words are not missing
        if(error == FSE_NOT_READY || error == FSE_INTERNAL) {
            speech->card_lost = true;
            speech->aborted = true;
        }
        return false;
    }
    speech->clip_opened = true;
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
    bool played = false; // the letter before played: a gap separates the two
    for(const char* p = word; *p != '\0' && !speech->aborted; p++) {
        if(!speech_voice_letter_clip(*p, name)) continue;
        // After a letter that did not play, as after every letter of a set whose folder has
        // gone, a gap would only add silence
        if(played && !speech_push_silence(speech, SPEECH_VOICE_LETTER_GAP_MS)) return;
        played = speech_voice_path(speech->set, name, speech->path, sizeof(speech->path)) > 0 &&
                 speech_play_clip(speech, speech->path);
        if(played) {
            speech->voice.clip_words++;
        } else if(!speech->card_lost) {
            speech_voice_missing(&speech->voice, name);
        }
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
            speech_voice_path(speech->set, word.word, speech->path, sizeof(speech->path)) > 0 &&
            speech_play_clip(speech, speech->path)) {
            speech->voice.clip_words++;
        } else if(!speech->card_lost) {
            speech->voice.fallback_words++;
            speech_voice_missing(&speech->voice, word.word);
            speech_spell_word(speech, word.word);
        }
    }
}

/**
 * Append the words the last utterance lacked to the missing list of the set in use, its
 * missing.txt: the lines built in the file buffer, idle between items, and written at once. The
 * log is cleared only when all of it reached the card; otherwise (a full or failing card) the
 * words stay in it, and no item tries again until the set is resolved again.
 */
static void speech_write_missing(Speech* speech) {
    size_t n = speech_voice_log_count(&speech->voice);
    if(n == 0 || speech->missing_failed) return;
    if(!speech_voice_set_file(speech->set, "missing.txt", speech->path, sizeof(speech->path))) {
        return;
    }
    // At most SPEECH_VOICE_LOG_MAX words of SPEECH_VOICE_WORD_MAX - 1 characters, each with its
    // newline: under half of the buffer
    size_t len = 0;
    for(size_t i = 0; i < n; i++) {
        const char* w = speech_voice_log_word(&speech->voice, i);
        size_t wl = strlen(w);
        if(len + wl + 1 > SPEECH_FILE_CHUNK) break;
        memcpy(&speech->file_buffer[len], w, wl);
        len += wl;
        speech->file_buffer[len++] = '\n';
    }
    File* file = speech->voice_file;
    bool written = storage_file_open(file, speech->path, FSAM_WRITE, FSOM_OPEN_APPEND) &&
                   storage_file_write(file, speech->file_buffer, len) == len;
    storage_file_close(file);
    if(written) {
        speech_voice_log_clear(&speech->voice);
    } else {
        speech->missing_failed = true;
    }
}

// A timed mute that is over turns the voice on again. Called with the mutex held
static void speech_voice_expire(Speech* speech, uint32_t now) {
    if(speech_voice_mute_over(
           speech->voice_enabled, speech->voice_timed, speech->voice_on_at, now)) {
        speech->voice_enabled = true;
        speech->voice_timed = false;
    }
}

static bool speech_voice_on(Speech* speech) {
    speech_lock(speech);
    speech_voice_expire(speech, furi_get_tick());
    bool on = speech->voice_enabled;
    speech_unlock(speech);
    return on;
}

static void speech_speak_item(Speech* speech, const SpeechItem* item) {
    speech_take_voice_set(speech);
    // Muted (sr voice off), or no voice set on the card: the item completes at once, without
    // the speaker, a file or the missing list; the voice status counts it
    if(!speech_voice_on(speech) || !speech_voice_ready(speech)) {
        speech->muted++;
        speech_lock(speech);
        speech->stats.speaking = false;
        speech_unlock(speech);
        return;
    }
    if(memmgr_heap_get_max_free_block() < SPEECH_HEAP_MIN) {
        speech->low_memory++;
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
    speech->clip_tried = false;
    speech->clip_opened = false;
    speech->card_lost = false;
    speech_speak_words(speech, spell);
    speech_item_end(speech);
    // The card went, or clips were tried and not one opened, not even a letter: the set's
    // folder has most likely gone. The next item resolves the set again, and the words this one
    // could not find are no news: neither written nor remembered as missing. With the folder
    // there, an item that tries clips opens one, but for a word with no letter or digit clip to
    // spell it with. Nothing on the normal path
    if(speech->card_lost || (speech->clip_tried && !speech->clip_opened)) {
        speech_voice_forget(speech);
        speech_voice_missing_reset(&speech->voice);
    } else if(speech_voice_on(speech)) {
        // Muted meanwhile (sr voice off, around a transfer): the words wait in the log for an
        // item with the voice on, so that no file is opened once the command has answered
        speech_write_missing(speech);
    }
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

    uint32_t idle_since = furi_get_tick(); // the end of the last item
    while(true) {
        // Quiet for a while since the last item: hand the speaker back so beeps and other apps
        // can use it. Counted from that end, not from the latest wake-up: every key press stops
        // speech and so wakes this loop, and quick keys would keep the speaker held
        uint32_t timeout = FuriWaitForever;
        if(furi_hal_speaker_is_mine()) {
            uint32_t idle = furi_get_tick() - idle_since;
            if(idle >= SPEECH_IDLE_RELEASE_MS) {
                speech_drop_speaker(speech);
                continue;
            }
            timeout = SPEECH_IDLE_RELEASE_MS - idle;
        }
        uint32_t flags =
            furi_thread_flags_wait(SPEECH_FLAG_WORK | SPEECH_FLAG_STOP, FuriFlagWaitAny, timeout);
        if(flags & FuriFlagError) continue;
        // A stop request has already retired the queue on the requesting thread; its flag
        // only wakes this loop, which then finds nothing or what was pushed after the request
        while(speech_pop(speech, item)) {
            if(item->file) {
                speech_play_item(speech, item);
            } else {
                speech_speak_item(speech, item);
            }
            idle_since = furi_get_tick();
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
    // No set asked for and none in use (the memset): until the reader passes the saved one, the
    // first set found is resolved at the first utterance

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
    // An interrupting push also wakes a DMA wait, as a stop does, so what it supersedes ends at
    // once instead of at the next played half, up to 82 ms later
    furi_thread_flags_set(
        speech->thread_id, interrupt ? SPEECH_FLAG_WORK | SPEECH_FLAG_STOP : SPEECH_FLAG_WORK);
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
    speech_lock(speech);
    speech->voice_enabled = enabled;
    speech->voice_timed = false;
    speech_unlock(speech);
    if(!enabled) speech_stop(speech);
}

bool speech_mute_voice_for(Speech* speech, uint32_t ms) {
    furi_check(speech);
    speech_lock(speech);
    bool timed = speech->voice_enabled || speech->voice_timed;
    if(timed) {
        speech->voice_enabled = false;
        speech->voice_timed = true;
        speech->voice_on_at = furi_get_tick() + ms;
    }
    speech_unlock(speech);
    if(timed) speech_stop(speech);
    return timed;
}

void speech_voice_card_changed(Speech* speech) {
    furi_check(speech);
    speech->card_changed = true;
}

void speech_get_voice_stats(Speech* speech, SpeechVoiceStats* out) {
    furi_check(speech && out);
    speech_lock(speech);
    uint32_t now = furi_get_tick();
    speech_voice_expire(speech, now);
    out->enabled = speech->voice_enabled;
    out->on_in_ms = !speech->voice_enabled && speech->voice_timed ? speech->voice_on_at - now : 0;
    speech_unlock(speech);
    // The worker owns these counters and writes them one word at a time; 32 bit reads are
    // atomic on this core, so the snapshot is consistent enough for a status line. It writes the
    // names one byte at a time, so a read while a set is resolved shows at worst a mixed name
    out->vocabulary = speech->vocabulary;
    out->clip_words = *(volatile uint32_t*)&speech->voice.clip_words;
    out->fallback_words = *(volatile uint32_t*)&speech->voice.fallback_words;
    out->missing_words = *(volatile uint32_t*)&speech->voice.missing_words;
    out->muted = *(volatile uint32_t*)&speech->muted;
    out->low_memory = *(volatile uint32_t*)&speech->low_memory;
    out->open_max_ms = *(volatile uint32_t*)&speech->open_max_ms;
    out->open_last_ms = *(volatile uint32_t*)&speech->open_last_ms;
    strncpy(out->settings, speech->voice_settings, sizeof(out->settings) - 1);
    out->settings[sizeof(out->settings) - 1] = '\0';
    strncpy(out->set, speech->set, sizeof(out->set) - 1);
    out->set[sizeof(out->set) - 1] = '\0';
    strncpy(out->wanted, speech->wanted, sizeof(out->wanted) - 1);
    out->wanted[sizeof(out->wanted) - 1] = '\0';
}

void speech_set_voice_set(Speech* speech, const char* set, bool force) {
    furi_check(speech && set);
    // A name the engine would not take as a folder, as from a settings file edited by hand,
    // means the first set found, as an empty one does. It is checked on a copy of its own, one
    // byte longer than a set name, as the saved name may be written meanwhile on another thread
    char name[SPEECH_VOICE_SET_MAX + 1];
    strlcpy(name, set, sizeof(name));
    if(!speech_voice_set_listable(name)) name[0] = '\0';
    // The reader passes the saved name before every announcement, so an unchanged one costs a
    // compare and nothing else; sr voice use forces a fresh resolution even for the same name
    speech_lock(speech);
    if(force || strcmp(speech->wanted_set, name) != 0) {
        strlcpy(speech->wanted_set, name, sizeof(speech->wanted_set));
        speech->set_changed = true;
    }
    speech_unlock(speech);
}

size_t
    speech_voice_sets(Speech* speech, void (*fn)(const char* name, void* context), void* context) {
    furi_check(speech && fn);
    // A handle of its own: the worker's is busy with clips while it speaks
    File* dir = storage_file_alloc(speech->storage);
    size_t count = speech_voice_walk(dir, fn, context);
    storage_file_free(dir);
    return count;
}
