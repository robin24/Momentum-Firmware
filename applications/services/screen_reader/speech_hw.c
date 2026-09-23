#include "speech_hw.h"

#include <furi.h>
#include <furi_hal.h>
#include <stm32wbxx_ll_dma.h>
#include <stm32wbxx_ll_tim.h>

#define SPEECH_TIMER       TIM16
#define SPEECH_DMA         DMA2
#define SPEECH_DMA_CHANNEL LL_DMA_CHANNEL_4

static SpeechHwEvent speech_hw_callback;
static void* speech_hw_context;

static void speech_hw_dma_isr(void* context) {
    UNUSED(context);
    if(LL_DMA_IsActiveFlag_HT4(SPEECH_DMA)) {
        LL_DMA_ClearFlag_HT4(SPEECH_DMA);
        speech_hw_callback(false, speech_hw_context);
    }
    if(LL_DMA_IsActiveFlag_TC4(SPEECH_DMA)) {
        LL_DMA_ClearFlag_TC4(SPEECH_DMA);
        speech_hw_callback(true, speech_hw_context);
    }
}

void speech_hw_start(uint8_t* buffer, size_t size, SpeechHwEvent callback, void* context) {
    speech_hw_callback = callback;
    speech_hw_context = context;

    // 64 MHz / (4 + 1) / (255 + 1) = 50 kHz carrier, as in the Text to SAM app
    LL_TIM_InitTypeDef tim;
    memset(&tim, 0, sizeof(tim));
    tim.Prescaler = 4;
    tim.Autoreload = 255;
    tim.CounterMode = LL_TIM_COUNTERMODE_UP;
    LL_TIM_Init(SPEECH_TIMER, &tim);

    LL_TIM_OC_InitTypeDef oc;
    memset(&oc, 0, sizeof(oc));
    oc.OCMode = LL_TIM_OCMODE_PWM1;
    oc.OCState = LL_TIM_OCSTATE_ENABLE;
    oc.CompareValue = buffer[0];
    LL_TIM_OC_Init(SPEECH_TIMER, LL_TIM_CHANNEL_CH1, &oc);
    LL_TIM_OC_EnablePreload(SPEECH_TIMER, LL_TIM_CHANNEL_CH1);

    // LL_DMA_ConfigAddresses only writes the two address registers; the direction bit has to
    // be set separately or the channel would copy CCR1 into the buffer instead
    LL_DMA_SetDataTransferDirection(
        SPEECH_DMA, SPEECH_DMA_CHANNEL, LL_DMA_DIRECTION_MEMORY_TO_PERIPH);
    LL_DMA_ConfigAddresses(
        SPEECH_DMA,
        SPEECH_DMA_CHANNEL,
        (uint32_t)buffer,
        (uint32_t) & (SPEECH_TIMER->CCR1),
        LL_DMA_DIRECTION_MEMORY_TO_PERIPH);
    LL_DMA_SetDataLength(SPEECH_DMA, SPEECH_DMA_CHANNEL, size);
    LL_DMA_SetPeriphRequest(SPEECH_DMA, SPEECH_DMA_CHANNEL, LL_DMAMUX_REQ_TIM16_UP);
    LL_DMA_SetChannelPriorityLevel(SPEECH_DMA, SPEECH_DMA_CHANNEL, LL_DMA_PRIORITY_VERYHIGH);
    LL_DMA_SetMode(SPEECH_DMA, SPEECH_DMA_CHANNEL, LL_DMA_MODE_CIRCULAR);
    LL_DMA_SetPeriphIncMode(SPEECH_DMA, SPEECH_DMA_CHANNEL, LL_DMA_PERIPH_NOINCREMENT);
    LL_DMA_SetMemoryIncMode(SPEECH_DMA, SPEECH_DMA_CHANNEL, LL_DMA_MEMORY_INCREMENT);
    LL_DMA_SetPeriphSize(SPEECH_DMA, SPEECH_DMA_CHANNEL, LL_DMA_PDATAALIGN_HALFWORD);
    LL_DMA_SetMemorySize(SPEECH_DMA, SPEECH_DMA_CHANNEL, LL_DMA_MDATAALIGN_BYTE);
    LL_DMA_ClearFlag_HT4(SPEECH_DMA);
    LL_DMA_ClearFlag_TC4(SPEECH_DMA);
    LL_DMA_EnableIT_HT(SPEECH_DMA, SPEECH_DMA_CHANNEL);
    LL_DMA_EnableIT_TC(SPEECH_DMA, SPEECH_DMA_CHANNEL);
    furi_hal_interrupt_set_isr(FuriHalInterruptIdDma2Ch4, speech_hw_dma_isr, NULL);
    LL_DMA_EnableChannel(SPEECH_DMA, SPEECH_DMA_CHANNEL);

    LL_TIM_EnableDMAReq_UPDATE(SPEECH_TIMER);
    LL_TIM_EnableAllOutputs(SPEECH_TIMER);
    LL_TIM_EnableCounter(SPEECH_TIMER);
}

void speech_hw_stop(void) {
    LL_TIM_DisableDMAReq_UPDATE(SPEECH_TIMER);
    LL_DMA_DisableChannel(SPEECH_DMA, SPEECH_DMA_CHANNEL);
    LL_DMA_DisableIT_HT(SPEECH_DMA, SPEECH_DMA_CHANNEL);
    LL_DMA_DisableIT_TC(SPEECH_DMA, SPEECH_DMA_CHANNEL);
    furi_hal_interrupt_set_isr(FuriHalInterruptIdDma2Ch4, NULL, NULL);
    LL_TIM_DisableAllOutputs(SPEECH_TIMER);
    LL_TIM_DisableCounter(SPEECH_TIMER);
}
