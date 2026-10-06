/* Wi-Fi Remote "Setup" scene: configure the auto-recovery dead-man's switch
 * (recovery timer + return-to-home-WiFi). Each change is persisted immediately
 * via wlan_remote_set_settings(). */

#include "../wlan_app.h"
#include <wlan_remote.h>

#define RECOVERY_COUNT 6
static const char* const recovery_text[RECOVERY_COUNT] =
    {"OFF", "1min", "2min", "5min", "10min", "15min"};
static const uint32_t recovery_value[RECOVERY_COUNT] =
    {0, 60000, 120000, 300000, 600000, 900000};

#define ONOFF_COUNT 2
static const char* const onoff_text[ONOFF_COUNT] = {"OFF", "ON"};

static uint8_t recovery_index(uint32_t ms) {
    for(uint8_t i = 0; i < RECOVERY_COUNT; i++) {
        if(recovery_value[i] == ms) return i;
    }
    return 0;
}

static void remote_setup_recovery_cb(VariableItem* item) {
    WlanApp* app = variable_item_get_context(item);
    UNUSED(app);
    uint8_t idx = variable_item_get_current_value_index(item);
    if(idx >= RECOVERY_COUNT) idx = RECOVERY_COUNT - 1;
    variable_item_set_current_value_text(item, recovery_text[idx]);
    WlanRemoteSettings s;
    wlan_remote_get_settings(&s);
    s.recovery_timeout_ms = recovery_value[idx];
    wlan_remote_set_settings(&s);
}

static void remote_setup_returnhome_cb(VariableItem* item) {
    WlanApp* app = variable_item_get_context(item);
    UNUSED(app);
    uint8_t idx = variable_item_get_current_value_index(item);
    if(idx >= ONOFF_COUNT) idx = ONOFF_COUNT - 1;
    variable_item_set_current_value_text(item, onoff_text[idx]);
    WlanRemoteSettings s;
    wlan_remote_get_settings(&s);
    s.return_home = (idx == 1);
    wlan_remote_set_settings(&s);
}

void wlan_app_scene_remote_setup_on_enter(void* context) {
    WlanApp* app = context;
    VariableItemList* list = app->variable_item_list;
    variable_item_list_reset(list);

    WlanRemoteSettings s;
    wlan_remote_get_settings(&s);

    VariableItem* it;
    uint8_t idx;

    it = variable_item_list_add(list, "Recovery Timer", RECOVERY_COUNT, remote_setup_recovery_cb, app);
    idx = recovery_index(s.recovery_timeout_ms);
    variable_item_set_current_value_index(it, idx);
    variable_item_set_current_value_text(it, recovery_text[idx]);

    it = variable_item_list_add(list, "Return Home", ONOFF_COUNT, remote_setup_returnhome_cb, app);
    idx = s.return_home ? 1 : 0;
    variable_item_set_current_value_index(it, idx);
    variable_item_set_current_value_text(it, onoff_text[idx]);

    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewVariableItemList);
}

bool wlan_app_scene_remote_setup_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void wlan_app_scene_remote_setup_on_exit(void* context) {
    WlanApp* app = context;
    variable_item_list_reset(app->variable_item_list);
}
