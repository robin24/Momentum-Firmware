#include "input.h"

#include "input_settings.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <furi.h>
#include <furi_hal_gpio.h>
#include <furi_hal_vibro.h>
#include <toolbox/cli/cli_command.h>
#include <cli/cli_main_commands.h>
#include <toolbox/pipe.h>

#define INPUT_DEBOUNCE_TICKS_HALF (INPUT_DEBOUNCE_TICKS / 2)
#define INPUT_PRESS_TICKS         150
#define INPUT_LONG_PRESS_COUNTS   2
#define INPUT_THREAD_FLAG_ISR     0x00000001

#define TAG "Input"

/** Input pin state */
typedef struct {
    const InputPin* pin;
    // State
    volatile bool state;
    volatile uint8_t debounce;
    FuriTimer* press_timer;
    FuriPubSub* event_pubsub;
    volatile uint8_t press_counter;
    volatile uint32_t counter;
} InputPinState;

// #define INPUT_DEBUG

#define GPIO_Read(input_pin) (furi_hal_gpio_read(input_pin.pin->gpio) ^ (input_pin.pin->inverted))

// The single input filter and its context, guarded by input_filter_mutex. The service allocates
// the mutex before it creates RECORD_INPUT_EVENTS, so whoever holds that record may use it.
static FuriMutex* input_filter_mutex = NULL;
static InputFilterCallback input_filter = NULL;
static void* input_filter_context = NULL;

void input_set_filter(InputFilterCallback callback, void* context) {
    // Opening the record waits for the service, which allocates the mutex first
    furi_record_open(RECORD_INPUT_EVENTS);
    furi_check(furi_mutex_acquire(input_filter_mutex, FuriWaitForever) == FuriStatusOk);
    input_filter = callback;
    input_filter_context = context;
    furi_check(furi_mutex_release(input_filter_mutex) == FuriStatusOk);
    furi_record_close(RECORD_INPUT_EVENTS);
}

// Every input event leaves through here so that the filter sees all of them: Press, Release and
// Short from the service loop, Long and Repeat from the press timers on the timer service thread,
// and the console's events. The timer service thread runs at a lower priority than the input
// service, so a filter call there can be preempted by one from the service loop: the mutex keeps
// the calls apart, and input_set_filter cannot swap the filter while it runs.
void input_publish_event(FuriPubSub* pubsub, InputEvent* event) {
    furi_check(pubsub);
    furi_check(event);

    InputFilterResult result = {false, false};
    furi_check(furi_mutex_acquire(input_filter_mutex, FuriWaitForever) == FuriStatusOk);
    if(input_filter) input_filter(event, &result, input_filter_context);
    furi_check(furi_mutex_release(input_filter_mutex) == FuriStatusOk);

    InputEvent long_event = *event;
    long_event.type = InputTypeLong;
    // gui and view_dispatcher discard a Long that follows its key's Release, so before a Release
    // the extra Long goes first
    bool long_first = result.emit_long && event->type == InputTypeRelease;
    if(long_first) furi_pubsub_publish(pubsub, &long_event);
    if(!result.drop) furi_pubsub_publish(pubsub, event);
    if(result.emit_long && !long_first) furi_pubsub_publish(pubsub, &long_event);
}

void input_press_timer_callback(void* arg) {
    InputPinState* input_pin = arg;
    InputEvent event;
    event.sequence_source = INPUT_SEQUENCE_SOURCE_HARDWARE;
    event.sequence_counter = input_pin->counter;
    event.key = input_pin->pin->key;
    input_pin->press_counter++;
    if(input_pin->press_counter == INPUT_LONG_PRESS_COUNTS) {
        event.type = InputTypeLong;
        input_publish_event(input_pin->event_pubsub, &event);
    } else if(input_pin->press_counter > INPUT_LONG_PRESS_COUNTS) {
        input_pin->press_counter--;
        event.type = InputTypeRepeat;
        input_publish_event(input_pin->event_pubsub, &event);
    }
}

void input_isr(void* _ctx) {
    FuriThreadId thread_id = (FuriThreadId)_ctx;
    furi_thread_flags_set(thread_id, INPUT_THREAD_FLAG_ISR);
}

const char* input_get_key_name(InputKey key) {
    for(size_t i = 0; i < input_pins_count; i++) {
        if(input_pins[i].key == key) {
            return input_pins[i].name;
        }
    }
    return "Unknown";
}

const char* input_get_type_name(InputType type) {
    switch(type) {
    case InputTypePress:
        return "Press";
    case InputTypeRelease:
        return "Release";
    case InputTypeShort:
        return "Short";
    case InputTypeLong:
        return "Long";
    case InputTypeRepeat:
        return "Repeat";
    default:
        return "Unknown";
    }
}

int32_t input_srv(void* p) {
    UNUSED(p);

    const FuriThreadId thread_id = furi_thread_get_current_id();
    // Before the record exists: whoever opens it may install a filter right away
    input_filter_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    FuriPubSub* event_pubsub = furi_pubsub_alloc();
    FuriPubSub* ascii_pubsub = furi_pubsub_alloc();
    uint32_t counter = 1;
    furi_record_create(RECORD_INPUT_EVENTS, event_pubsub);
    furi_record_create(RECORD_ASCII_EVENTS, ascii_pubsub);

    //define object input_settings, take memory load (or init) settings and create record for access to settings structure from outside
    InputSettings* settings = malloc(sizeof(InputSettings));
    input_settings_load(settings);
    furi_record_create(RECORD_INPUT_SETTINGS, settings);

#ifdef INPUT_DEBUG
    furi_hal_gpio_init_simple(&gpio_ext_pa4, GpioModeOutputPushPull);
#endif

    InputPinState pin_states[input_pins_count];

    for(size_t i = 0; i < input_pins_count; i++) {
        furi_hal_gpio_add_int_callback(input_pins[i].gpio, input_isr, thread_id);
        pin_states[i].pin = &input_pins[i];
        pin_states[i].state = GPIO_Read(pin_states[i]);
        pin_states[i].debounce = INPUT_DEBOUNCE_TICKS_HALF;
        pin_states[i].press_timer =
            furi_timer_alloc(input_press_timer_callback, FuriTimerTypePeriodic, &pin_states[i]);
        pin_states[i].event_pubsub = event_pubsub;
        pin_states[i].press_counter = 0;
    }

    while(1) {
        bool is_changing = false;
        for(size_t i = 0; i < input_pins_count; i++) {
            bool state = GPIO_Read(pin_states[i]);
            if(state) {
                if(pin_states[i].debounce < INPUT_DEBOUNCE_TICKS) pin_states[i].debounce += 1;
            } else {
                if(pin_states[i].debounce > 0) pin_states[i].debounce -= 1;
            }

            if(pin_states[i].debounce > 0 && pin_states[i].debounce < INPUT_DEBOUNCE_TICKS) {
                is_changing = true;
            } else if(pin_states[i].state != state) {
                pin_states[i].state = state;

                // Common state info
                InputEvent event;
                event.sequence_source = INPUT_SEQUENCE_SOURCE_HARDWARE;
                event.key = pin_states[i].pin->key;

                // Short / Long / Repeat timer routine
                if(state) {
                    pin_states[i].counter = counter++;
                    event.sequence_counter = pin_states[i].counter;
                    furi_timer_start(pin_states[i].press_timer, INPUT_PRESS_TICKS);
                } else {
                    event.sequence_counter = pin_states[i].counter;
                    furi_timer_stop(pin_states[i].press_timer);
                    while(furi_timer_is_running(pin_states[i].press_timer))
                        furi_delay_tick(1);
                    if(pin_states[i].press_counter < INPUT_LONG_PRESS_COUNTS) {
                        event.type = InputTypeShort;
                        input_publish_event(event_pubsub, &event);
                    }
                    pin_states[i].press_counter = 0;
                }

                // Send Press/Release event
                event.type = pin_states[i].state ? InputTypePress : InputTypeRelease;
                input_publish_event(event_pubsub, &event);
                // vibro signal if user setup vibro touch level in Settings-Input.
                if(settings->vibro_touch_level &&
                   ((1 << event.type) & settings->vibro_touch_trigger_mask)) {
                    //delay 1 ticks for compatibility with rgb_backlight_mod
                    furi_delay_tick(1);
                    furi_hal_vibro_on(true);
                    furi_delay_tick(settings->vibro_touch_level);
                    furi_hal_vibro_on(false);
                }
            }
        }

        if(is_changing) {
#ifdef INPUT_DEBUG
            furi_hal_gpio_write(&gpio_ext_pa4, 1);
#endif
            furi_delay_tick(1);
        } else {
#ifdef INPUT_DEBUG
            furi_hal_gpio_write(&gpio_ext_pa4, 0);
#endif
            furi_thread_flags_wait(INPUT_THREAD_FLAG_ISR, FuriFlagWaitAny, FuriWaitForever);
        }
    }

    return 0;
}
