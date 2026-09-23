#include "speech_queue.h"

#include <string.h>

void speech_queue_init(SpeechQueue* queue) {
    memset(queue, 0, sizeof(*queue));
}

static SpeechItem* tail(SpeechQueue* queue) {
    return &queue->items[(queue->head + queue->count - 1) % SPEECH_QUEUE_CAPACITY];
}

static void set_text(SpeechItem* item, const char* text) {
    size_t n = strlen(text);
    if(n > SPEECH_ITEM_TEXT_MAX - 1) n = SPEECH_ITEM_TEXT_MAX - 1;
    memcpy(item->text, text, n);
    item->text[n] = '\0';
}

static void push_item(
    SpeechQueue* queue,
    const char* text,
    bool interrupt,
    bool replaceable,
    bool file,
    uint32_t rate) {
    if(interrupt) {
        queue->generation++;
        queue->count = 0;
        queue->head = 0;
    }
    SpeechItem* item;
    if(replaceable && queue->count > 0 && tail(queue)->replaceable) {
        item = tail(queue);
    } else {
        if(queue->count == SPEECH_QUEUE_CAPACITY) {
            queue->head = (queue->head + 1) % SPEECH_QUEUE_CAPACITY;
            queue->count--;
            queue->dropped++;
        }
        queue->count++;
        item = tail(queue);
    }
    item->generation = queue->generation;
    item->replaceable = replaceable;
    item->file = file;
    item->rate = rate;
    set_text(item, text);
}

void speech_queue_push(SpeechQueue* queue, const char* text, bool interrupt, bool replaceable) {
    push_item(queue, text, interrupt, replaceable, false, 0);
}

void speech_queue_push_file(SpeechQueue* queue, const char* path, uint32_t rate) {
    push_item(queue, path, true, false, true, rate);
}

bool speech_queue_pop(SpeechQueue* queue, SpeechItem* out) {
    if(queue->count == 0) return false;
    *out = queue->items[queue->head];
    queue->head = (queue->head + 1) % SPEECH_QUEUE_CAPACITY;
    queue->count--;
    return true;
}

void speech_queue_stop(SpeechQueue* queue) {
    queue->generation++;
    queue->count = 0;
    queue->head = 0;
}

size_t speech_queue_count(const SpeechQueue* queue) {
    return queue->count;
}
