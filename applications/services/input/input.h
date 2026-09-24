/**
 * @file input.h
 * Input: main API
 */

#pragma once

#include <furi_hal_resources.h>
#include <storage/storage.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RECORD_INPUT_EVENTS            "input_events"
#define RECORD_ASCII_EVENTS            "ascii_events"
#define INPUT_SEQUENCE_SOURCE_HARDWARE (0u)
#define INPUT_SEQUENCE_SOURCE_SOFTWARE (1u)

/** Input Types
 * Some of them are physical events and some logical
 */
typedef enum {
    InputTypePress, /**< Press event, emitted after debounce */
    InputTypeRelease, /**< Release event, emitted after debounce */
    InputTypeShort, /**< Short event, emitted after InputTypeRelease done within INPUT_LONG_PRESS interval */
    InputTypeLong, /**< Long event, emitted after INPUT_LONG_PRESS_COUNTS interval, asynchronous to InputTypeRelease  */
    InputTypeRepeat, /**< Repeat event, emitted with INPUT_LONG_PRESS_COUNTS period after InputTypeLong event */
    InputTypeMAX, /**< Special value for exceptional */
} InputType;

/** Input Event, dispatches with FuriPubSub */
typedef struct {
    union {
        uint32_t sequence;
        struct {
            uint8_t sequence_source   : 2;
            uint32_t sequence_counter : 30;
        };
    };
    InputKey key;
    InputType type;
} InputEvent;

/** Result of an input filter: drop suppresses the event; emit_long publishes an extra Long event
 *  for the same key while the key still counts as held, that is right before the event when the
 *  event is the key's Release (gui and view_dispatcher discard a Long that follows its key's
 *  Release) and right after it otherwise; the Long alone when drop is set too. The extra Long
 *  does not pass the filter. */
typedef struct {
    bool drop;
    bool emit_long;
} InputFilterResult;

/** Called before every input event is published, on the publishing thread: the input service
 *  (Press, Release, Short), the timer service (Long, Repeat) or a console thread. The input
 *  service's filter lock keeps the calls apart from each other and from input_set_filter: the
 *  filter must not block, must not call input_set_filter and must not publish input events. */
typedef void (
    *InputFilterCallback)(const InputEvent* event, InputFilterResult* result, void* context);

typedef enum {
    AsciiValueNUL = 0x00, // NULL
    _AsciiValueSOH = 0x01, // Start of Heading
    _AsciiValueSTX = 0x02, // Start of Text
    _AsciiValueETX = 0x03, // End of Text
    _AsciiValueEOT = 0x04, // End of Transmission
    _AsciiValueENQ = 0x05, // Enquiry
    _AsciiValueACK = 0x06, // Acknowledgement
    _AsciiValueBEL = 0x07, // Bell
    AsciiValueBS = 0x08, // Backspace
    _AsciiValueTAB = 0x09, // Horizontal Tab
    _AsciiValueLF = 0x0A, // Line Feed
    _AsciiValueVT = 0x0B, // Vertical Tab
    _AsciiValueFF = 0x0C, // Form Feed
    AsciiValueCR = 0x0D, // Carriage Return
    _AsciiValueSO = 0x0E, // Shift Out
    _AsciiValueSI = 0x0F, // Shift In
    _AsciiValueDLE = 0x10, // Data Link Escape
    AsciiValueDC1 = 0x11, // Device Control 1
    AsciiValueDC2 = 0x12, // Device Control 2
    AsciiValueDC3 = 0x13, // Device Control 3
    AsciiValueDC4 = 0x14, // Device Control 4
    _AsciiValueNAK = 0x15, // Negative Acknowledgement
    _AsciiValueSYN = 0x16, // Synchronous Idle
    _AsciiValueETB = 0x17, // End of Transmission Block
    _AsciiValueCAN = 0x18, // Cancel
    _AsciiValueEM = 0x19, // End of Medium
    _AsciiValueSUB = 0x1A, // Substitute
    AsciiValueESC = 0x1B, // Escape
    _AsciiValueSF = 0x1C, // File Separator
    _AsciiValueGS = 0x1D, // Group Separator
    _AsciiValueRS = 0x1E, // Record Separator
    _AsciiValueUS = 0x1F, // Unit Separator
    // Printable Ascii 0x20-0x7E
    _AsciiValueDEL = 0x7F, // Delete
} AsciiValue;

/** Ascii Event, dispatches with FuriPubSub */
typedef struct {
    uint8_t value;
} AsciiEvent;

/** Get human readable input key name
 * @param key - InputKey
 * @return string
 */
const char* input_get_key_name(InputKey key);

/** Get human readable input type name
 * @param type - InputType
 * @return string
 */
const char* input_get_type_name(InputType type);

/** Install or remove (NULL) the single input filter. Used by the screen reader. Waits for the
 *  input service to start; once it returns, the previous filter is not running and is not called
 *  again, so its context may be freed.
 * @param callback - the filter, or NULL for none
 * @param context - passed to the filter
 */
void input_set_filter(InputFilterCallback callback, void* context);

/** Publish an input event through the filter, the way the input service publishes the events of
 *  the keys. For events made in software, such as the console's.
 * @param pubsub - the RECORD_INPUT_EVENTS pubsub
 * @param event - the event
 */
void input_publish_event(FuriPubSub* pubsub, InputEvent* event);

#ifdef __cplusplus
}
#endif
