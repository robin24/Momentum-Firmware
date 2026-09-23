#include "speech.h"

#include "sam/sam.h"
#include "speech_hw.h"
#include "speech_pcm.h"
#include "speech_queue.h"
#include "speech_text.h"

#include <furi_hal.h>

#define TAG "Speech"

#define SPEECH_RING_SIZE          4096
#define SPEECH_HALF               (SPEECH_RING_SIZE / 2)
#define SPEECH_OVERHEAD_NS        7400 /* calibrated against the Text to SAM demo, Task 5 */
#define SPEECH_IDLE_RELEASE_MS    200
#define SPEECH_ACQUIRE_TIMEOUT_MS 500
#define SPEECH_EVENT_TIMEOUT_MS   200
#define SPEECH_BUS_POLL_MS        5

#define SPEECH_FLAG_WORK  (1 << 0)
#define SPEECH_FLAG_STOP  (1 << 1)
#define SPEECH_FLAG_HALF0 (1 << 2) /* DMA finished playing the first half */
#define SPEECH_FLAG_HALF1 (1 << 3) /* DMA finished playing the second half */

struct Speech {
    FuriThread* thread;
    volatile FuriThreadId thread_id;
    FuriMutex* mutex;

    // Guarded by mutex
    SpeechQueue queue;
    SpeechStats stats;

    // Set by any thread, read by the worker
    volatile uint8_t rate;
    volatile uint8_t volume;

    // Worker thread only
    SpeechPcm pcm;
    uint8_t ring[SPEECH_RING_SIZE];
    uint8_t fill_half;
    uint16_t fill_index;
    bool running;
    bool aborted;
    uint32_t generation;
    uint8_t lut_volume;
    char expanded[SPEECH_TEXT_MAX];
    char chunk[SPEECH_CHUNK_MAX + 2]; /* room for an appended period */
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

static bool speech_sam_output(const uint8_t values[5], uint16_t delta, void* context) {
    Speech* speech = context;
    return speech_pcm_push(&speech->pcm, values, delta, speech_emit, speech);
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

static void speech_speak_item(Speech* speech, const SpeechItem* item) {
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
        return;
    }

    uint8_t volume = speech->volume;
    if(volume != speech->lut_volume) {
        speech_pcm_set_volume(&speech->pcm, volume);
        speech->lut_volume = volume;
    }
    speech_pcm_reset(&speech->pcm);
    memset(speech->ring, speech_pcm_silence(&speech->pcm), SPEECH_RING_SIZE);

    speech_text_expand(item->text, speech->expanded, sizeof(speech->expanded));
    SamVoice voice = SAM_VOICE_DEFAULT;
    voice.speed = speech->rate;

    speech_lock(speech);
    speech->stats.utterances++;
    speech_unlock(speech);
    uint32_t started = furi_get_tick();

    size_t pos = 0;
    while(!speech->aborted &&
          speech_text_next_chunk(speech->expanded, &pos, speech->chunk, sizeof(speech->chunk))) {
        for(char* p = speech->chunk; *p; p++) {
            if(*p >= 'a' && *p <= 'z') *p = (char)(*p - 'a' + 'A');
        }
        // A closing period gives the synthesizer a pause to shape the end of the chunk on
        size_t len = strlen(speech->chunk);
        if(len > 0 && len < sizeof(speech->chunk) - 1 &&
           strchr(".?!,", speech->chunk[len - 1]) == NULL) {
            speech->chunk[len] = '.';
            speech->chunk[len + 1] = '\0';
            len++;
        }
        // The reciter inside SAM stops at a word boundary once its phoneme string is full and
        // reports how much text it used; speak the rest with further calls
        size_t offset = 0;
        while(offset < len && !speech->aborted) {
            size_t consumed = 0;
            sam_speak(speech->chunk + offset, &voice, speech_sam_output, speech, &consumed);
            if(consumed == 0) break;
            offset += consumed;
        }
    }

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
    speech->stats.last_played_ms = furi_get_tick() - started;
    speech_unlock(speech);
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
            speech_speak_item(speech, item);
        }
    }
    return 0;
}

Speech* speech_alloc(void) {
    Speech* speech = malloc(sizeof(Speech));
    memset(speech, 0, sizeof(Speech));
    speech->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    speech_queue_init(&speech->queue);
    speech->rate = 72;
    speech->volume = 100;
    speech->lut_volume = 100;
    speech_pcm_init(&speech->pcm, 100, SPEECH_OVERHEAD_NS);

    speech->thread = furi_thread_alloc_ex("SpeechWorker", 3 * 1024, speech_worker, speech);
    furi_thread_set_priority(speech->thread, FuriThreadPriorityHigh);
    furi_thread_start(speech->thread);
    while(!speech->thread_id)
        furi_delay_ms(1);
    return speech;
}

void speech_say(Speech* speech, const char* text, bool interrupt, bool replaceable) {
    furi_check(speech && text);
    speech_lock(speech);
    speech_queue_push(&speech->queue, text, interrupt, replaceable);
    speech->stats.queue_dropped = speech->queue.dropped;
    speech_unlock(speech);
    furi_thread_flags_set(speech->thread_id, SPEECH_FLAG_WORK);
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

void speech_set_voice(Speech* speech, uint8_t rate, uint8_t volume) {
    furi_check(speech);
    speech->rate = rate;
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
