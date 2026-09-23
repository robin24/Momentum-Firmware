#include "speech_voice.h"

#include <string.h>

static bool sv_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static bool sv_is_punct(char c) {
    return c == '.' || c == ',' || c == '?' || c == '!' || c == ';' || c == ':';
}

bool speech_voice_next_word(const char* text, size_t* pos, SpeechVoiceWord* out) {
    size_t i = *pos;
    while(text[i] != '\0' && sv_is_space(text[i]))
        i++;
    if(text[i] == '\0') {
        *pos = i;
        return false;
    }
    size_t start = i;
    while(text[i] != '\0' && !sv_is_space(text[i]))
        i++;
    size_t end = i;
    *pos = i;

    char last = text[end - 1];
    if(last == '.' || last == '?' || last == '!') {
        out->pause_ms = SPEECH_VOICE_STOP_MS;
    } else if(last == ',' || last == ';' || last == ':') {
        out->pause_ms = SPEECH_VOICE_COMMA_MS;
    } else {
        out->pause_ms = SPEECH_VOICE_GAP_MS;
    }

    while(start < end && sv_is_punct(text[start]))
        start++;
    while(end > start && sv_is_punct(text[end - 1]))
        end--;
    size_t n = end - start;
    if(n > SPEECH_VOICE_WORD_MAX - 1) n = SPEECH_VOICE_WORD_MAX - 1;
    for(size_t k = 0; k < n; k++) {
        char c = text[start + k];
        out->word[k] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    out->word[n] = '\0';
    return true;
}

size_t speech_voice_path(const char* word, char* out, size_t out_size) {
    size_t n = strlen(word);
    if(n == 0) return 0;
    char c = word[0];
    char initial = ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) ? c : '_';
    size_t dir = strlen(SPEECH_VOICE_DIR);
    size_t need = dir + 3 + n + 4 + 1; // "/x/" name ".raw" NUL
    if(need > out_size) return 0;
    char* p = out;
    memcpy(p, SPEECH_VOICE_DIR, dir);
    p += dir;
    *p++ = '/';
    *p++ = initial;
    *p++ = '/';
    memcpy(p, word, n);
    p += n;
    memcpy(p, ".raw", 4);
    p += 4;
    *p = '\0';
    return (size_t)(p - out);
}

static uint32_t sv_hash(const char* s) {
    uint32_t h = 2166136261u; // FNV-1a
    while(*s != '\0') {
        h ^= (uint8_t)*s++;
        h *= 16777619u;
    }
    return h;
}

void speech_voice_state_init(SpeechVoiceState* state) {
    memset(state, 0, sizeof(*state));
}

bool speech_voice_missing(SpeechVoiceState* state, const char* word) {
    if(word[0] == '\0') return false;
    uint32_t h = sv_hash(word);
    for(size_t i = 0; i < state->count; i++) {
        if(state->hashes[i] == h) return false;
    }
    if(state->log_count >= SPEECH_VOICE_LOG_MAX) return false;
    state->hashes[state->next] = h;
    state->next = (uint8_t)((state->next + 1) % SPEECH_VOICE_MISSING_SET);
    if(state->count < SPEECH_VOICE_MISSING_SET) state->count++;
    strncpy(state->log[state->log_count], word, SPEECH_VOICE_WORD_MAX - 1);
    state->log[state->log_count][SPEECH_VOICE_WORD_MAX - 1] = '\0';
    state->log_count++;
    state->missing_words++;
    return true;
}

size_t speech_voice_log_count(const SpeechVoiceState* state) {
    return state->log_count;
}

const char* speech_voice_log_word(const SpeechVoiceState* state, size_t index) {
    return index < state->log_count ? state->log[index] : "";
}

void speech_voice_log_clear(SpeechVoiceState* state) {
    state->log_count = 0;
}
