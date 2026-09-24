#include <furi.h>
#include <gui/elements.h>
#include <gui/canvas_i.h>
#include <assets_icons.h>
#include <momentum/momentum.h>
#include <furi_hal_rtc.h>

#include "../desktop_i.h"
#include "desktop_view_lock_menu.h"

static const NotificationSequence sequence_note_c = {
    &message_note_c5,
    &message_delay_100,
    &message_sound_off,
    NULL,
};

typedef enum {
    DesktopLockMenuIndexLefthandedMode,
    DesktopLockMenuIndexSettings,
    DesktopLockMenuIndexDarkMode,
    DesktopLockMenuIndexLock,
    DesktopLockMenuIndexBluetooth,
    DesktopLockMenuIndexMomentum,
    DesktopLockMenuIndexBrightness,
    DesktopLockMenuIndexVolume,

    DesktopLockMenuIndexTotalCount
} DesktopLockMenuIndex;

// The tiles are icons: the screen reader hears the selected one's name, a switch with its state
static const char* const desktop_lock_menu_names[DesktopLockMenuIndexTotalCount] = {
    [DesktopLockMenuIndexLefthandedMode] = "Left handed",
    [DesktopLockMenuIndexSettings] = "Settings",
    [DesktopLockMenuIndexDarkMode] = "Dark mode",
    [DesktopLockMenuIndexLock] = "Lock",
    [DesktopLockMenuIndexBluetooth] = "Bluetooth",
    [DesktopLockMenuIndexMomentum] = "Momentum",
    [DesktopLockMenuIndexBrightness] = "Brightness",
    [DesktopLockMenuIndexVolume] = "Volume",
};

static const char* const desktop_lock_menu_popup_names[DesktopLockMenuPopupIndexMAX] = {
    [DesktopLockMenuPopupIndexKeypad] = "Keypad Lock",
    [DesktopLockMenuPopupIndexPinCode] = "PIN Code Lock",
    [DesktopLockMenuPopupIndexPinOff] = "PIN Lock + OFF",
};

void desktop_lock_menu_set_callback(
    DesktopLockMenuView* lock_menu,
    DesktopLockMenuViewCallback callback,
    void* context) {
    furi_assert(lock_menu);
    furi_assert(callback);
    lock_menu->callback = callback;
    lock_menu->context = context;
}

void desktop_lock_menu_set_pin_state(DesktopLockMenuView* lock_menu, bool pin_is_set) {
    with_view_model(
        lock_menu->view,
        DesktopLockMenuViewModel * model,
        {
            model->pin_is_set = pin_is_set;
            model->lock_popup_index = pin_is_set; // Select with PIN by default if set
        },
        true);
}

void desktop_lock_menu_set_stealth_mode_state(DesktopLockMenuView* lock_menu, bool stealth_mode) {
    with_view_model(
        lock_menu->view,
        DesktopLockMenuViewModel * model,
        { model->stealth_mode = stealth_mode; },
        true);
}

void desktop_lock_menu_set_idx(DesktopLockMenuView* lock_menu, uint8_t idx) {
    furi_assert(idx < DesktopLockMenuIndexTotalCount);
    with_view_model(
        lock_menu->view, DesktopLockMenuViewModel * model, { model->idx = idx; }, true);
}

/** The selected tile for the screen reader: its name, "on" or "off" after a switch, the level
 * after a slider ("Brightness 50"), and "muted" after the volume in stealth mode ("Volume 50,
 * muted"). */
static void
    desktop_lock_menu_tap_note(Canvas* canvas, const DesktopLockMenuViewModel* m, bool enabled) {
    if(m->idx >= DesktopLockMenuIndexTotalCount) return;
    const char* name = desktop_lock_menu_names[m->idx];
    const NotificationSettings* settings = &m->lock_menu->notification->settings;
    char note[32];
    switch(m->idx) {
    case DesktopLockMenuIndexLefthandedMode:
    case DesktopLockMenuIndexDarkMode:
    case DesktopLockMenuIndexBluetooth:
        snprintf(note, sizeof(note), "%s %s", name, enabled ? "on" : "off");
        break;
    case DesktopLockMenuIndexBrightness:
        snprintf(
            note, sizeof(note), "%s %d", name, (int)(settings->display_brightness * 100.0f + 0.5f));
        break;
    case DesktopLockMenuIndexVolume:
        // Stealth mode mutes the device's sounds: the tile shows the muted icon and the level
        snprintf(
            note,
            sizeof(note),
            "%s %d%s",
            name,
            (int)(settings->speaker_volume * 100.0f + 0.5f),
            m->stealth_mode ? ", muted" : "");
        break;
    default:
        strlcpy(note, name, sizeof(note));
        break;
    }
    canvas_tap_hint_note(canvas, note, true);
}

void desktop_lock_menu_draw_callback(Canvas* canvas, void* model) {
    DesktopLockMenuViewModel* m = model;

    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontBatteryPercent);

    int8_t x, y, w, h;
    bool selected, toggle;
    bool enabled = false;
    bool selected_enabled = false;
    uint8_t value = 0;
    int8_t total = 58;
    const Icon* icon = NULL;
    for(size_t i = 0; i < DesktopLockMenuIndexTotalCount; ++i) {
        selected = m->idx == i;
        toggle = i < 6;
        if(toggle) {
            x = 2 + 32 * (i / 2);
            y = 2 + 32 * (i % 2);
            w = 28;
            h = 28;
            enabled = false;
        } else {
            x = 98 + 16 * (i % 2);
            y = 2;
            w = 12;
            h = 60;
            value = 0;
        }

        switch(i) {
        case DesktopLockMenuIndexLefthandedMode:
            icon = &I_CC_LefthandedMode_16x16;
            enabled = furi_hal_rtc_is_flag_set(FuriHalRtcFlagHandOrient);
            break;
        case DesktopLockMenuIndexSettings:
            icon = &I_CC_Settings_16x16;
            break;
        case DesktopLockMenuIndexDarkMode:
            icon = &I_CC_DarkMode_16x16;
            enabled = momentum_settings.dark_mode;
            break;
        case DesktopLockMenuIndexLock:
            icon = &I_CC_Lock_16x16;
            break;
        case DesktopLockMenuIndexBluetooth:
            icon = &I_CC_Bluetooth_16x16;
            enabled = m->lock_menu->bt->bt_settings.enabled;
            break;
        case DesktopLockMenuIndexMomentum:
            icon = &I_CC_Momentum_16x16;
            break;
        case DesktopLockMenuIndexBrightness:
            icon = &I_Pin_star_7x7;
            value = total - m->lock_menu->notification->settings.display_brightness * total;
            break;
        case DesktopLockMenuIndexVolume:
            icon = m->stealth_mode ? &I_Muted_8x8 : &I_Volup_8x6;
            value = total - m->lock_menu->notification->settings.speaker_volume * total;
            break;
        default:
            break;
        }

        if(selected) {
            selected_enabled = enabled;
            elements_bold_rounded_frame(canvas, x - 1, y - 1, w + 1, h + 1);
        } else {
            canvas_draw_rframe(canvas, x, y, w, h, 5);
        }

        if(toggle) {
            if(enabled) {
                canvas_draw_rbox(canvas, x, y, w, h, 5);
                canvas_set_color(canvas, ColorWhite);
            }
            canvas_draw_icon(
                canvas,
                x + (w - icon_get_width(icon)) / 2,
                y + (h - icon_get_height(icon)) / 2,
                icon);
            if(enabled) {
                canvas_set_color(canvas, ColorBlack);
            }
        } else {
            canvas_draw_icon(
                canvas,
                x + (w - icon_get_width(icon)) / 2,
                y + (h - icon_get_height(icon)) / 2,
                icon);
            canvas_set_color(canvas, ColorXOR);
            canvas_draw_box(canvas, x + 1, y + 1 + value, w - 2, h - 2 - value);
            if(selected) {
                canvas_set_color(canvas, ColorBlack);
            } else {
                canvas_set_color(canvas, ColorWhite);
            }
            canvas_draw_dot(canvas, x + 1, y + 1);
            canvas_draw_dot(canvas, x + 1, y + h - 2);
            canvas_draw_dot(canvas, x + w - 2, y + 1);
            canvas_draw_dot(canvas, x + w - 2, y + h - 2);
            canvas_set_color(canvas, ColorBlack);
            canvas_draw_rframe(canvas, x, y, w, h, 5);
        }
    }

    if(m->show_lock_popup) {
        if(momentum_settings.popup_overlay) {
            canvas_draw_overlay(canvas);
        }
        canvas_set_font(canvas, FontSecondary);
        elements_bold_rounded_frame(canvas, 24, 4, 80, 56);
        for(size_t i = 0; i < DesktopLockMenuPopupIndexMAX; i++) {
            // A frame marks the choice, which the screen reader hears as the focus
            if(i == m->lock_popup_index) {
                canvas_tap_hint_focus(canvas, i + 1, DesktopLockMenuPopupIndexMAX);
            }
            canvas_draw_str_aligned(
                canvas,
                64,
                16 + 16 * i,
                AlignCenter,
                AlignCenter,
                desktop_lock_menu_popup_names[i]);
        }
        elements_frame(canvas, 28, 8 + m->lock_popup_index * 16, 72, 15);
    } else {
        desktop_lock_menu_tap_note(canvas, m, selected_enabled);
    }
}

View* desktop_lock_menu_get_view(DesktopLockMenuView* lock_menu) {
    furi_assert(lock_menu);
    return lock_menu->view;
}

bool desktop_lock_menu_input_callback(InputEvent* event, void* context) {
    furi_assert(event);
    furi_assert(context);

    DesktopLockMenuView* lock_menu = context;
    uint8_t idx = 0;
    bool show_lock_popup = false;
    DesktopLockMenuPopupIndex lock_popup_index = 0;
    bool stealth_mode = false;
    bool consumed = true;

    with_view_model(
        lock_menu->view,
        DesktopLockMenuViewModel * model,
        {
            show_lock_popup = model->show_lock_popup;
            stealth_mode = model->stealth_mode;
            if((event->type == InputTypeShort) || (event->type == InputTypeRepeat)) {
                if(model->show_lock_popup) {
                    if(event->key == InputKeyUp) {
                        if(model->lock_popup_index == 0) {
                            model->lock_popup_index = DesktopLockMenuPopupIndexMAX - 1;
                        } else {
                            model->lock_popup_index--;
                        }
                    } else if(event->key == InputKeyDown) {
                        if(model->lock_popup_index == DesktopLockMenuPopupIndexMAX - 1) {
                            model->lock_popup_index = 0;
                        } else {
                            model->lock_popup_index++;
                        }
                    } else if(event->key == InputKeyBack || event->key == InputKeyOk) {
                        model->show_lock_popup = false;
                    }
                } else {
                    if(model->idx == DesktopLockMenuIndexLock && event->key == InputKeyOk) {
                        model->show_lock_popup = true;
                    } else if(model->idx < 6) {
                        if(event->key == InputKeyUp || event->key == InputKeyDown) {
                            if(model->idx % 2) {
                                model->idx--;
                            } else {
                                model->idx++;
                            }
                        } else if(event->key == InputKeyLeft) {
                            if(model->idx < 2) {
                                model->idx = 7;
                            } else {
                                model->idx -= 2;
                            }
                        } else if(event->key == InputKeyRight) {
                            if(model->idx >= 4) {
                                model->idx = 6;
                            } else {
                                model->idx += 2;
                            }
                        }
                    } else {
                        if(event->key == InputKeyLeft) {
                            model->idx--;
                        } else if(event->key == InputKeyRight) {
                            if(model->idx >= 7) {
                                model->idx = 1;
                            } else {
                                model->idx++;
                            }
                        }
                    }
                }
            }
            idx = model->idx;
            lock_popup_index = model->lock_popup_index;
        },
        true);

    DesktopEvent desktop_event = 0;
    if(show_lock_popup) {
        if(event->key == InputKeyOk && event->type == InputTypeShort) {
            switch(lock_popup_index) {
            case DesktopLockMenuPopupIndexKeypad:
                desktop_event = DesktopLockMenuEventLockKeypad;
                break;
            case DesktopLockMenuPopupIndexPinCode:
                desktop_event = DesktopLockMenuEventLockPinCode;
                break;
            case DesktopLockMenuPopupIndexPinOff:
                desktop_event = DesktopLockMenuEventLockPinOff;
                break;
            default:
                break;
            }
        }
    } else {
        if(event->key == InputKeyBack) {
            consumed = false;
        } else if(event->key == InputKeyOk && event->type == InputTypeShort) {
            switch(idx) {
            case DesktopLockMenuIndexLefthandedMode:
                if(furi_hal_rtc_is_flag_set(FuriHalRtcFlagHandOrient)) {
                    furi_hal_rtc_reset_flag(FuriHalRtcFlagHandOrient);
                } else {
                    furi_hal_rtc_set_flag(FuriHalRtcFlagHandOrient);
                }
                break;
            case DesktopLockMenuIndexSettings:
                desktop_event = DesktopLockMenuEventSettings;
                break;
            case DesktopLockMenuIndexDarkMode:
                momentum_settings.dark_mode = !momentum_settings.dark_mode;
                lock_menu->save_momentum = true;
                break;
            case DesktopLockMenuIndexBluetooth:
                lock_menu->bt->bt_settings.enabled = !lock_menu->bt->bt_settings.enabled;
                if(lock_menu->bt->bt_settings.enabled) {
                    furi_hal_bt_start_advertising();
                } else {
                    furi_hal_bt_stop_advertising();
                }
                lock_menu->save_bt = true;
                break;
            case DesktopLockMenuIndexMomentum:
                desktop_event = DesktopLockMenuEventMomentum;
                break;
            case DesktopLockMenuIndexBrightness:
                desktop_event = DesktopLockMenuEventScreenSettings;
                break;
            case DesktopLockMenuIndexVolume:
                desktop_event = stealth_mode ? DesktopLockMenuEventStealthModeOff :
                                               DesktopLockMenuEventStealthModeOn;
                break;
            default:
                break;
            }
        } else if(idx >= 6 && (event->type == InputTypeShort || event->type == InputTypeRepeat)) {
            int8_t offset = 0;
            if(event->key == InputKeyUp) {
                offset = 1;
            } else if(event->key == InputKeyDown) {
                offset = -1;
            }
            if(offset) {
                float value;
                switch(idx) {
                case DesktopLockMenuIndexBrightness:
                    value = lock_menu->notification->settings.display_brightness + 0.05 * offset;
                    lock_menu->notification->settings.display_brightness =
                        value < 0.00f ? 0.00f : (value > 1.00f ? 1.00f : value);
                    lock_menu->save_notification = true;
                    notification_message(lock_menu->notification, &sequence_display_backlight_on);
                    break;
                case DesktopLockMenuIndexVolume:
                    value = lock_menu->notification->settings.speaker_volume + 0.05 * offset;
                    lock_menu->notification->settings.speaker_volume =
                        value < 0.00f ? 0.00f : (value > 1.00f ? 1.00f : value);
                    lock_menu->save_notification = true;
                    notification_message(lock_menu->notification, &sequence_note_c);
                    break;
                default:
                    break;
                }
            }
        }
    }
    if(desktop_event) {
        lock_menu->callback(desktop_event, lock_menu->context);
    }

    return consumed;
}

DesktopLockMenuView* desktop_lock_menu_alloc(void) {
    DesktopLockMenuView* lock_menu = malloc(sizeof(DesktopLockMenuView));
    lock_menu->bt = furi_record_open(RECORD_BT);
    lock_menu->notification = furi_record_open(RECORD_NOTIFICATION);
    lock_menu->view = view_alloc();
    view_allocate_model(lock_menu->view, ViewModelTypeLocking, sizeof(DesktopLockMenuViewModel));
    with_view_model(
        lock_menu->view,
        DesktopLockMenuViewModel * model,
        { model->lock_menu = lock_menu; },
        false);
    view_set_context(lock_menu->view, lock_menu);
    view_set_draw_callback(lock_menu->view, (ViewDrawCallback)desktop_lock_menu_draw_callback);
    view_set_input_callback(lock_menu->view, desktop_lock_menu_input_callback);

    return lock_menu;
}

void desktop_lock_menu_free(DesktopLockMenuView* lock_menu_view) {
    furi_assert(lock_menu_view);

    view_free(lock_menu_view->view);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_BT);
    free(lock_menu_view);
}
