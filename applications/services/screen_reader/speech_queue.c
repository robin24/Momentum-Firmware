#include "speech_queue.h"

#include <stdbool.h>
#include <string.h>

void speech_queue_init(SpeechQueue* queue) {
    memset(queue, 0, sizeof(*queue));
}

static SpeechItem* tail(SpeechQueue* queue) {
    return &queue->items[(queue->head + queue->count - 1) % SPEECH_QUEUE_CAPACITY];
}

static bool is_word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '\'';
}

/** The first n characters of text, cut to what an item holds. A text is cut after its last whole
 *  word that fits, as a word cut in two would be spelled and logged as a missing word; one word
 *  longer than an item, and a file's path, keep the plain cut. */
static void set_text(SpeechItem* item, const char* text, size_t n, bool file) {
    const size_t max = SPEECH_ITEM_TEXT_MAX - 1;
    if(n > max) {
        n = max;
        if(!file && is_word_char(text[max]) && is_word_char(text[max - 1])) {
            size_t j = max;
            while(j > 0 && is_word_char(text[j - 1]))
                j--;
            while(j > 0 && text[j - 1] == ' ')
                j--;
            if(j > 0) n = j;
        }
    }
    memcpy(item->text, text, n);
    item->text[n] = '\0';
}

static void push_item(
    SpeechQueue* queue,
    const char* text,
    size_t len,
    bool interrupt,
    bool replaceable,
    bool file,
    bool spell,
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
    item->spell = spell;
    item->rate = rate;
    set_text(item, text, len, file);
}

void speech_queue_push(SpeechQueue* queue, const char* text, bool interrupt, bool replaceable) {
    push_item(queue, text, strlen(text), interrupt, replaceable, false, false, 0);
}

void speech_queue_push_file(SpeechQueue* queue, const char* path, uint32_t rate) {
    push_item(queue, path, strlen(path), true, false, true, false, rate);
}

void speech_queue_push_spelled(SpeechQueue* queue, const char* text) {
    push_item(queue, text, strlen(text), true, false, false, true, 0);
}

/** The length of the next part of text: all of it when it fits in an item, else up to and with
 *  the last ". " or ", " that fits, else up to the last space that fits, else the limit. */
static size_t part_length(const char* text) {
    const size_t max = SPEECH_ITEM_TEXT_MAX - 1;
    size_t n = 0;
    while(n <= max && text[n] != '\0')
        n++;
    if(n <= max) return n;
    // text[max] is inside the text: a separator or a space there still ends a part of max
    size_t cut = 0;
    for(size_t i = 1; i <= max; i++) {
        if(text[i] == ' ' && (text[i - 1] == '.' || text[i - 1] == ',')) cut = i;
    }
    if(cut) return cut;
    for(size_t i = 1; i <= max; i++) {
        if(text[i] == ' ') cut = i;
    }
    return cut ? cut : max;
}

size_t speech_queue_push_parts(SpeechQueue* queue, const char* text) {
    size_t queued = 0;
    while(true) {
        while(*text == ' ')
            text++;
        if(*text == '\0') break;
        size_t len = part_length(text);
        if(queued < SPEECH_QUEUE_CAPACITY) {
            // The first part clears the queue, so every part after it has a free slot
            push_item(queue, text, len, queued == 0, false, false, false, 0);
            queued++;
        } else {
            queue->dropped++;
        }
        text += len;
    }
    return queued;
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
