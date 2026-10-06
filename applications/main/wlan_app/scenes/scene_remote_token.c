/* Wi-Fi Remote "Set Token" scene: lets the user type a permanent access token.
 * Input is normalized and validated by wlan_remote_set_token() (8 chars from the
 * unambiguous alphabet); an invalid entry is reported and the field reopened. */

#include "../wlan_app.h"
#include <wlan_remote.h>

static void remote_token_input_cb(void* context) {
    WlanApp* app = context;
    view_dispatcher_send_custom_event(
        app->view_dispatcher, WlanAppCustomEventRemoteTokenEntered);
}

void wlan_app_scene_remote_token_on_enter(void* context) {
    WlanApp* app = context;

    // Prefill with the current token so the user can tweak rather than retype.
    char current[WLAN_REMOTE_TOKEN_LEN + 1] = {0};
    if(wlan_remote_get_token(current, sizeof(current))) {
        strncpy(app->remote_token_input, current, sizeof(app->remote_token_input) - 1);
        app->remote_token_input[sizeof(app->remote_token_input) - 1] = '\0';
    } else {
        app->remote_token_input[0] = '\0';
    }

    text_input_set_header_text(app->text_input, "Token (8 chars):");
    text_input_set_result_callback(
        app->text_input,
        remote_token_input_cb,
        app,
        app->remote_token_input,
        sizeof(app->remote_token_input),
        false);

    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewTextInput);
}

bool wlan_app_scene_remote_token_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == WlanAppCustomEventRemoteTokenEntered) {
            if(wlan_remote_set_token(app->remote_token_input)) {
                scene_manager_previous_scene(app->scene_manager);
            } else {
                // Invalid: show a brief notice and let the user try again.
                widget_reset(app->widget);
                widget_add_string_element(
                    app->widget, 64, 24, AlignCenter, AlignCenter, FontPrimary, "Invalid token");
                widget_add_string_element(
                    app->widget,
                    64,
                    40,
                    AlignCenter,
                    AlignCenter,
                    FontSecondary,
                    "Need 8 valid chars");
                view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewWidget);
            }
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        scene_manager_previous_scene(app->scene_manager);
        consumed = true;
    }

    return consumed;
}

void wlan_app_scene_remote_token_on_exit(void* context) {
    WlanApp* app = context;
    text_input_reset(app->text_input);
}
