#include "../momentum_app.h"

enum VarItemListIndex {
    VarItemListIndexScreenReader,
    VarItemListIndexVoice,
    VarItemListIndexRate,
    VarItemListIndexVolume,
    VarItemListIndexVerbosity,
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
    bool value = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, value ? "Recorded" : "SAM");
    momentum_settings.sr_voice = value;
    app->save_settings = true;
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

    item = variable_item_list_add(
        var_item_list, "Voice", 2, momentum_app_scene_accessibility_voice_changed, app);
    value_index = momentum_settings.sr_voice;
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, value_index ? "Recorded" : "SAM");

    item = variable_item_list_add(
        var_item_list, "Rate", SR_RATE_COUNT, momentum_app_scene_accessibility_rate_changed, app);
    value_index = (momentum_settings.sr_rate - SR_RATE_MIN) / SR_RATE_STEP;
    variable_item_set_current_value_index(item, value_index);
    char rate_str[6];
    snprintf(rate_str, sizeof(rate_str), "%lu", momentum_settings.sr_rate);
    variable_item_set_current_value_text(item, rate_str);

    item = variable_item_list_add(
        var_item_list,
        "Volume",
        SR_VOLUME_COUNT,
        momentum_app_scene_accessibility_volume_changed,
        app);
    value_index = (momentum_settings.sr_volume - SR_VOLUME_MIN) / SR_VOLUME_STEP;
    variable_item_set_current_value_index(item, value_index);
    char volume_str[6];
    snprintf(volume_str, sizeof(volume_str), "%lu", momentum_settings.sr_volume);
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
