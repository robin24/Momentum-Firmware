#include "../momentum_app.h"

// Row order when the voice is SAM (Rate is shown).
enum VarItemListIndexSam {
    VarItemListIndexSamScreenReader,
    VarItemListIndexSamVoice,
    VarItemListIndexSamRate,
    VarItemListIndexSamVolume,
    VarItemListIndexSamVerbosity,
    VarItemListIndexSamChangeDelay,
    VarItemListIndexSamCount,
};

// Row order when the voice is Recorded (Rate is hidden). Screen Reader and Voice
// keep the same index as above; Volume/Verbosity/Change delay shift up by one.
enum VarItemListIndexRecorded {
    VarItemListIndexRecordedScreenReader,
    VarItemListIndexRecordedVoice,
    VarItemListIndexRecordedVolume,
    VarItemListIndexRecordedVerbosity,
    VarItemListIndexRecordedChangeDelay,
    VarItemListIndexRecordedCount,
};

// Custom event the Voice callback sends to ask for a list rebuild. The value is
// chosen to fall outside the valid row-index range of either layout above, so it
// can never collide with an index the enter callback passes through.
enum VarItemListEvent {
    VarItemListEventVoiceChanged = VarItemListIndexSamCount,
};

#define SR_RATE_MIN   40
#define SR_RATE_MAX   120
#define SR_RATE_STEP  4
#define SR_RATE_COUNT (((SR_RATE_MAX - SR_RATE_MIN) / SR_RATE_STEP) + 1)

#define SR_VOLUME_MIN   0
#define SR_VOLUME_MAX   100
#define SR_VOLUME_STEP  10
#define SR_VOLUME_COUNT (((SR_VOLUME_MAX - SR_VOLUME_MIN) / SR_VOLUME_STEP) + 1)

void momentum_app_scene_accessibility_var_item_list_callback(void* context, uint32_t index) {
    MomentumApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

static void momentum_app_scene_accessibility_screen_reader_changed(VariableItem* item) {
    MomentumApp* app = variable_item_get_context(item);
    bool value = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, value ? "ON" : "OFF");
    momentum_settings.screen_reader = value;
    app->save_settings = true;
}

static void momentum_app_scene_accessibility_voice_changed(VariableItem* item) {
    MomentumApp* app = variable_item_get_context(item);
    // Write the setting here, not in on_event: variable_item_list_process_left/right
    // is still holding the very VariableItem this callback belongs to, and rebuilding
    // (variable_item_list_reset then re-add) would free it out from under that
    // caller. Defer only the rebuild to on_event, which runs after process_left/right
    // has returned all the way up to the view dispatcher's event loop.
    bool value = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, value ? "Recorded" : "SAM");
    momentum_settings.sr_voice = value;
    app->save_settings = true;
    view_dispatcher_send_custom_event(app->view_dispatcher, VarItemListEventVoiceChanged);
}

static void momentum_app_scene_accessibility_rate_changed(VariableItem* item) {
    MomentumApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    uint32_t value = SR_RATE_MIN + (index * SR_RATE_STEP);
    char str[6];
    snprintf(str, sizeof(str), "%lu", value);
    variable_item_set_current_value_text(item, str);
    momentum_settings.sr_rate = value;
    app->save_settings = true;
}

static void momentum_app_scene_accessibility_volume_changed(VariableItem* item) {
    MomentumApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    uint32_t value = SR_VOLUME_MIN + (index * SR_VOLUME_STEP);
    char str[6];
    snprintf(str, sizeof(str), "%lu", value);
    variable_item_set_current_value_text(item, str);
    momentum_settings.sr_volume = value;
    app->save_settings = true;
}

static const char* const sr_verbosity_names[] = {
    "Terse",
    "Normal",
    "Verbose",
};
static void momentum_app_scene_accessibility_verbosity_changed(VariableItem* item) {
    MomentumApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, sr_verbosity_names[index]);
    momentum_settings.sr_verbosity = index;
    app->save_settings = true;
}

// A table, not uniform steps.
static const uint32_t sr_change_ms_values[] = {
    SR_CHANGE_MS_MIN, // 500
    1000,
    1500,
    2000,
    SR_CHANGE_MS_DEFAULT, // 3000
    5000,
    SR_CHANGE_MS_MAX, // 10000
};
static const char* const sr_change_ms_labels[] = {
    "0.5 s",
    "1 s",
    "1.5 s",
    "2 s",
    "3 s",
    "5 s",
    "10 s",
};
// Nearest table entry to a saved value that may be off-table (e.g. from an older
// build), same idea as Rate/Volume's nearest-step rounding but for a non-uniform
// table: scan for the smallest absolute difference instead of dividing by a step.
static uint8_t momentum_app_scene_accessibility_change_delay_nearest_index(uint32_t ms) {
    uint8_t nearest = 0;
    uint32_t nearest_diff = UINT32_MAX;
    for(uint8_t i = 0; i < COUNT_OF(sr_change_ms_values); i++) {
        uint32_t diff = (ms > sr_change_ms_values[i]) ? (ms - sr_change_ms_values[i]) :
                                                        (sr_change_ms_values[i] - ms);
        if(diff < nearest_diff) {
            nearest_diff = diff;
            nearest = i;
        }
    }
    return nearest;
}
static void momentum_app_scene_accessibility_change_delay_changed(VariableItem* item) {
    MomentumApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, sr_change_ms_labels[index]);
    momentum_settings.sr_change_ms = sr_change_ms_values[index];
    app->save_settings = true;
}

// Resets and rebuilds the whole list (Rate is skipped when the voice is Recorded),
// then restores the selection to selected_index for the resulting layout.
static void momentum_app_scene_accessibility_build_list(MomentumApp* app, uint8_t selected_index) {
    VariableItemList* var_item_list = app->var_item_list;
    VariableItem* item;
    uint8_t value_index;

    variable_item_list_reset(var_item_list);

    item = variable_item_list_add(
        var_item_list,
        "Screen Reader",
        2,
        momentum_app_scene_accessibility_screen_reader_changed,
        app);
    value_index = momentum_settings.screen_reader;
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, value_index ? "ON" : "OFF");

    item = variable_item_list_add(
        var_item_list, "Voice", 2, momentum_app_scene_accessibility_voice_changed, app);
    value_index = momentum_settings.sr_voice;
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, value_index ? "Recorded" : "SAM");

    if(!momentum_settings.sr_voice) {
        item = variable_item_list_add(
            var_item_list,
            "Rate",
            SR_RATE_COUNT,
            momentum_app_scene_accessibility_rate_changed,
            app);
        // Round to the nearest step so the label matches the index; sr_rate can be off-grid.
        value_index =
            (momentum_settings.sr_rate - SR_RATE_MIN + (SR_RATE_STEP / 2)) / SR_RATE_STEP;
        if(value_index > SR_RATE_COUNT - 1) value_index = SR_RATE_COUNT - 1;
        variable_item_set_current_value_index(item, value_index);
        uint32_t rate_value = SR_RATE_MIN + (value_index * SR_RATE_STEP);
        char rate_str[6];
        snprintf(rate_str, sizeof(rate_str), "%lu", rate_value);
        variable_item_set_current_value_text(item, rate_str);
    }

    item = variable_item_list_add(
        var_item_list,
        "Volume",
        SR_VOLUME_COUNT,
        momentum_app_scene_accessibility_volume_changed,
        app);
    // Round to the nearest step so the label matches the index; sr_volume can be off-grid.
    value_index =
        (momentum_settings.sr_volume - SR_VOLUME_MIN + (SR_VOLUME_STEP / 2)) / SR_VOLUME_STEP;
    if(value_index > SR_VOLUME_COUNT - 1) value_index = SR_VOLUME_COUNT - 1;
    variable_item_set_current_value_index(item, value_index);
    uint32_t volume_value = SR_VOLUME_MIN + (value_index * SR_VOLUME_STEP);
    char volume_str[6];
    snprintf(volume_str, sizeof(volume_str), "%lu", volume_value);
    variable_item_set_current_value_text(item, volume_str);

    item = variable_item_list_add(
        var_item_list,
        "Verbosity",
        COUNT_OF(sr_verbosity_names),
        momentum_app_scene_accessibility_verbosity_changed,
        app);
    value_index = momentum_settings.sr_verbosity;
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, sr_verbosity_names[value_index]);

    item = variable_item_list_add(
        var_item_list,
        "Change delay",
        COUNT_OF(sr_change_ms_values),
        momentum_app_scene_accessibility_change_delay_changed,
        app);
    // Nearest table entry so the label matches the index; sr_change_ms can be off-table.
    value_index = momentum_app_scene_accessibility_change_delay_nearest_index(
        momentum_settings.sr_change_ms);
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, sr_change_ms_labels[value_index]);

    variable_item_list_set_enter_callback(
        var_item_list, momentum_app_scene_accessibility_var_item_list_callback, app);

    variable_item_list_set_selected_item(var_item_list, selected_index);
}

void momentum_app_scene_accessibility_on_enter(void* context) {
    MomentumApp* app = context;
    momentum_app_scene_accessibility_build_list(
        app, scene_manager_get_scene_state(app->scene_manager, MomentumAppSceneAccessibility));
    view_dispatcher_switch_to_view(app->view_dispatcher, MomentumAppViewVarItemList);
}

bool momentum_app_scene_accessibility_on_event(void* context, SceneManagerEvent event) {
    MomentumApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == VarItemListEventVoiceChanged) {
            // The change callback already wrote momentum_settings.sr_voice (and
            // set save_settings); just rebuild for the new layout. Voice sits at
            // the same index in both (VarItemListIndexSamVoice ==
            // VarItemListIndexRecordedVoice), so this only reads the setting back.
            bool recorded = momentum_settings.sr_voice;
            uint8_t voice_index = recorded ? VarItemListIndexRecordedVoice :
                                             VarItemListIndexSamVoice;
            momentum_app_scene_accessibility_build_list(app, voice_index);
            scene_manager_set_scene_state(
                app->scene_manager, MomentumAppSceneAccessibility, voice_index);
        } else {
            scene_manager_set_scene_state(
                app->scene_manager, MomentumAppSceneAccessibility, event.event);
        }
        consumed = true;
    }

    return consumed;
}

void momentum_app_scene_accessibility_on_exit(void* context) {
    MomentumApp* app = context;
    variable_item_list_reset(app->var_item_list);
}
