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

void speech_pcm_init(SpeechPcm* pcm, uint8_t volume_percent) {
    memset(pcm, 0, sizeof(*pcm));
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

bool speech_pcm_push_raw(
    SpeechPcm* pcm,
    const uint8_t* samples,
    size_t count,
    uint32_t hold_ns,
    SpeechPcmEmit emit,
    void* context) {
    uint64_t start_ns = pcm->now_ns;
    bool accepted = true;
    for(size_t i = 0; i < count && accepted; i++) {
        pcm->subsamples++;
        uint64_t end_ns = pcm->now_ns + hold_ns;
        while(pcm->next_slot_ns < end_ns) {
            if(!emit(pcm->lut[samples[i]], context)) {
                accepted = false;
                break;
            }
            pcm->slots++;
            pcm->next_slot_ns += SPEECH_SLOT_NS;
        }
        if(accepted) pcm->now_ns = end_ns;
    }
    // The nominal time is the exact total of the completed samples: the difference of the
    // microsecond floors before and after carries a sub microsecond remainder across calls, so
    // the 62.5 us samples of a 16 kHz clip add up to a whole second and not to 992 ms
    pcm->nominal_us += (uint32_t)(pcm->now_ns / 1000u - start_ns / 1000u);
    return accepted;
}
