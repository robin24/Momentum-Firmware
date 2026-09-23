#include "speech_pcm.h"

#include <math.h>
#include <string.h>

void speech_pcm_set_volume(SpeechPcm* pcm, uint8_t volume_percent) {
    if(volume_percent > 100) volume_percent = 100;
    float volume = (float)volume_percent / 100.0f;
    for(int x = 0; x < 256; x++) {
        // The Text to SAM app's curve: tanh soft clipping around the mid level, then volume
        float data = (float)x;
        data /= 255.0f;
        data -= 0.5f;
        data *= 4.0f;
        data = tanhf(data);
        data *= volume;
        data += 0.5f;
        data *= 255.0f;
        if(data < 0) {
            data = 0;
        } else if(data > 255) {
            data = 255;
        }
        pcm->lut[x] = (uint8_t)data;
    }
}

void speech_pcm_init(SpeechPcm* pcm, uint8_t volume_percent, uint32_t overhead_ns) {
    memset(pcm, 0, sizeof(*pcm));
    pcm->overhead_ns = overhead_ns;
    speech_pcm_set_volume(pcm, volume_percent);
}

void speech_pcm_reset(SpeechPcm* pcm) {
    pcm->now_ns = 0;
    pcm->next_slot_ns = 0;
    pcm->subsamples = 0;
    pcm->nominal_us = 0;
    pcm->slots = 0;
}

uint8_t speech_pcm_silence(const SpeechPcm* pcm) {
    return pcm->lut[SPEECH_SILENCE_VALUE];
}

bool speech_pcm_push(
    SpeechPcm* pcm,
    const uint8_t values[5],
    uint16_t delta,
    SpeechPcmEmit emit,
    void* context) {
    uint32_t hold_ns = (uint32_t)(delta / 8) * 1000u + pcm->overhead_ns;
    pcm->subsamples += 5;
    pcm->nominal_us += 5u * (delta / 8);
    for(int k = 0; k < 5; k++) {
        uint64_t end_ns = pcm->now_ns + hold_ns;
        while(pcm->next_slot_ns < end_ns) {
            if(!emit(pcm->lut[values[k]], context)) return false;
            pcm->slots++;
            pcm->next_slot_ns += SPEECH_SLOT_NS;
        }
        pcm->now_ns = end_ns;
    }
    return true;
}
