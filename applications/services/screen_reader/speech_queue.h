/**
 * @file speech_queue.h
 * Bounded queue of utterances with interrupt and replace semantics. Pure C, host tested.
 * The caller provides locking.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPEECH_QUEUE_CAPACITY 4
#define SPEECH_ITEM_TEXT_MAX  160

typedef struct {
    uint32_t generation; /**< generation the item was queued in */
    bool replaceable; /**< a later replaceable item may overwrite it while it waits */
    char text[SPEECH_ITEM_TEXT_MAX];
} SpeechItem;

typedef struct {
    SpeechItem items[SPEECH_QUEUE_CAPACITY];
    uint8_t head;
    uint8_t count;
    /** bumped by interrupt and stop; a speaking item of an older generation aborts */
    uint32_t generation;
    uint32_t dropped; /**< items pushed out of a full queue */
} SpeechQueue;

void speech_queue_init(SpeechQueue* queue);

/** interrupt: clear everything and start a new generation before adding. replaceable: overwrite a
 *  replaceable item at the tail instead of appending. A full queue drops its oldest item. */
void speech_queue_push(SpeechQueue* queue, const char* text, bool interrupt, bool replaceable);

bool speech_queue_pop(SpeechQueue* queue, SpeechItem* out);

/** Clear everything and start a new generation. */
void speech_queue_stop(SpeechQueue* queue);

size_t speech_queue_count(const SpeechQueue* queue);

#ifdef __cplusplus
}
#endif
