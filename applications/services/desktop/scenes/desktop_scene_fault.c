#include <furi_hal.h>

#include "../desktop_i.h"

#define DesktopFaultEventExit 0x00FF00FF

/** The address line: a line break, "at " and eight hex digits */
#define DESKTOP_FAULT_ADDRESS_LINE_LEN (sizeof("\nat 0803ABCD") - 1)

/** The popup keeps a pointer to its text, so the text lives here rather than on the stack: the
 * message, then "at" and the crash address check.c kept, when there is one */
static char desktop_fault_text[64];

void desktop_scene_fault_callback(void* context) {
    Desktop* desktop = (Desktop*)context;
    view_dispatcher_send_custom_event(desktop->view_dispatcher, DesktopFaultEventExit);
}

void desktop_scene_fault_on_enter(void* context) {
    Desktop* desktop = (Desktop*)context;

    Popup* popup = desktop->popup;
    popup_set_context(popup, desktop);
    popup_set_header(
        popup,
        "Flipper crashed\n and was rebooted",
        64,
        14 + STATUS_BAR_Y_SHIFT,
        AlignCenter,
        AlignCenter);

    const char* message = (const char*)furi_hal_rtc_get_fault_data();
    uint32_t address = furi_hal_rtc_get_register(FuriHalRtcRegisterFaultLr);
    if(address) {
        // The message is cut, if it must be, so that the address line always fits
        int message_max = (int)(sizeof(desktop_fault_text) - DESKTOP_FAULT_ADDRESS_LINE_LEN - 1);
        snprintf(
            desktop_fault_text,
            sizeof(desktop_fault_text),
            "%.*s\nat %08lX",
            message_max,
            message,
            (unsigned long)address);
    } else {
        // Read no further than the buffer holds: a message kept across an update can be stale
        snprintf(
            desktop_fault_text,
            sizeof(desktop_fault_text),
            "%.*s",
            (int)(sizeof(desktop_fault_text) - 1),
            message);
    }
    popup_set_text(
        popup, desktop_fault_text, 64, 37 + STATUS_BAR_Y_SHIFT, AlignCenter, AlignCenter);
    popup_set_callback(popup, desktop_scene_fault_callback);

    view_dispatcher_switch_to_view(desktop->view_dispatcher, DesktopViewIdPopup);
}

bool desktop_scene_fault_on_event(void* context, SceneManagerEvent event) {
    Desktop* desktop = (Desktop*)context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case DesktopFaultEventExit:
            scene_manager_previous_scene(desktop->scene_manager);
            consumed = true;
            break;
        default:
            break;
        }
    }

    return consumed;
}

void desktop_scene_fault_on_exit(void* context) {
    Desktop* desktop = (Desktop*)context;
    furi_assert(desktop);

    Popup* popup = desktop->popup;
    popup_reset(popup);

    // The address register is left as it is: the last crash's address stays readable in
    // `sr status` until the next crash
    furi_hal_rtc_set_fault_data(0);
}
