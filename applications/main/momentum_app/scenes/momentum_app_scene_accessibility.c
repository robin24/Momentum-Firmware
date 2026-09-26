#include "../momentum_app.h"

enum VarItemListIndex {
    VarItemListIndexScreenReader,
    VarItemListIndexVoice,
    VarItemListIndexVolume,
    VarItemListIndexVerbosity,
    VarItemListIndexChangeDelay,
    VarItemListIndexCount,
};

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
    if(CharList_size(app->voice_set_names) == 0) return;
    uint8_t index = variable_item_get_current_value_index(item);
    const char* name = *CharList_get(app->voice_set_names, index);
    variable_item_set_current_value_text(item, name);
    strlcpy(momentum_settings.sr_voice_set, name, SR_VOICE_SET_LEN);
    app->voice_set_index = index;
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
// Nearest table entry to a saved value that may be off-table (from an older build): scan for
// the smallest absolute difference; a tie goes to the lower value.
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

void momentum_app_scene_accessibility_on_enter(void* context) {
    MomentumApp* app = context;
    VariableItemList* var_item_list = app->var_item_list;
    VariableItem* item;
    uint8_t value_index;

    item = variable_item_list_add(
        var_item_list,
        "Screen Reader",
        2,
        momentum_app_scene_accessibility_screen_reader_changed,
        app);
    value_index = momentum_settings.screen_reader;
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, value_index ? "ON" : "OFF");

    size_t voice_sets = CharList_size(app->voice_set_names);
    item = variable_item_list_add(
        var_item_list,
        "Voice",
        voice_sets ? voice_sets : 1,
        momentum_app_scene_accessibility_voice_changed,
        app);
    variable_item_set_current_value_index(item, voice_sets ? app->voice_set_index : 0);
    variable_item_set_current_value_text(
        item, voice_sets ? *CharList_get(app->voice_set_names, app->voice_set_index) : "None");

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
    value_index = momentum_app_scene_accessibility_change_delay_nearest_index(
        momentum_settings.sr_change_ms);
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, sr_change_ms_labels[value_index]);

    variable_item_list_set_enter_callback(
        var_item_list, momentum_app_scene_accessibility_var_item_list_callback, app);

    variable_item_list_set_selected_item(
        var_item_list,
        scene_manager_get_scene_state(app->scene_manager, MomentumAppSceneAccessibility));

    view_dispatcher_switch_to_view(app->view_dispatcher, MomentumAppViewVarItemList);
}

bool momentum_app_scene_accessibility_on_event(void* context, SceneManagerEvent event) {
    MomentumApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        scene_manager_set_scene_state(
            app->scene_manager, MomentumAppSceneAccessibility, event.event);
        consumed = true;
    }

    return consumed;
}

void momentum_app_scene_accessibility_on_exit(void* context) {
    MomentumApp* app = context;
    variable_item_list_reset(app->var_item_list);
}
