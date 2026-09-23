/**
 * @file speech_hw.h
 * TIM16 as a 50 kHz PWM carrier on the speaker pin, fed by DMA2 channel 4 from a circular
 * byte buffer. The speaker must already be acquired (furi_hal_speaker_acquire) by the caller.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Called from the DMA interrupt when the first (false) or second (true) half was played. */
typedef void (*SpeechHwEvent)(bool second_half, void* context);

void speech_hw_start(uint8_t* buffer, size_t size, SpeechHwEvent callback, void* context);
void speech_hw_stop(void);
