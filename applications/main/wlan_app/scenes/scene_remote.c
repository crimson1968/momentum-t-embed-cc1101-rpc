/* Wi-Fi Remote scene: starts the background WebSocket RPC server on the current
 * STA connection and shows the address + access token to hand to the gateway.
 *
 * Leaving via Back keeps the server running in the background (so other apps can
 * be driven remotely). The on-screen "Stop" button is the explicit way to shut
 * it down; it also stops on reboot. */

#include "../wlan_app.h"
#include <wlan_remote.h>

static bool s_ok = false;
static int s_last_clients = -1;

static void remote_button_cb(GuiButtonType result, InputType type, void* context) {
    WlanApp* app = context;
    if(type != InputTypeShort) return;
    if(result == GuiButtonTypeCenter) {
        view_dispatcher_send_custom_event(app->view_dispatcher, WlanAppCustomEventRemoteStop);
    } else if(result == GuiButtonTypeLeft) {
        view_dispatcher_send_custom_event(app->view_dispatcher, WlanAppCustomEventRemoteSetToken);
    } else if(result == GuiButtonTypeRight) {
        view_dispatcher_send_custom_event(app->view_dispatcher, WlanAppCustomEventRemoteSetup);
    }
}

static void remote_render(WlanApp* app) {
    Widget* w = app->widget;
    widget_reset(w);
    widget_add_string_element(
        w, 64, 2, AlignCenter, AlignTop, FontPrimary, "Wi-Fi Remote");

    if(!s_ok) {
        widget_add_string_element(
            w, 64, 30, AlignCenter, AlignCenter, FontSecondary, "Start failed (no WiFi?)");
        widget_add_button_element(w, GuiButtonTypeCenter, "Exit", remote_button_cb, app);
        return;
    }

    char ip[16] = {0};
    char token[WLAN_REMOTE_TOKEN_LEN + 1] = {0};
    wlan_remote_get_ip(ip, sizeof(ip));
    wlan_remote_get_token(token, sizeof(token));

    // Show the 8-char token grouped as XXXX-XXXX for easy reading on the display.
    char token_grouped[12] = {0};
    if(strlen(token) == 8) {
        snprintf(token_grouped, sizeof(token_grouped), "%.4s-%.4s", token, token + 4);
    } else {
        strncpy(token_grouped, token, sizeof(token_grouped) - 1);
    }

    char body[128];
    snprintf(
        body,
        sizeof(body),
        "ws://%s:%u/rpc\nToken: %s\nClients: %u",
        ip,
        (unsigned)wlan_remote_get_port(),
        token_grouped,
        (unsigned)wlan_remote_get_client_count());
    widget_add_text_box_element(w, 0, 14, 128, 46, AlignLeft, AlignTop, body, false);
    widget_add_button_element(w, GuiButtonTypeLeft, "Token", remote_button_cb, app);
    widget_add_button_element(w, GuiButtonTypeCenter, "Stop", remote_button_cb, app);
    widget_add_button_element(w, GuiButtonTypeRight, "Setup", remote_button_cb, app);
}

void wlan_app_scene_remote_on_enter(void* context) {
    WlanApp* app = context;

    s_ok = wlan_remote_is_running() || wlan_remote_start();
    s_last_clients = -1;
    remote_render(app);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewWidget);
}

bool wlan_app_scene_remote_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == WlanAppCustomEventRemoteStop) {
            // Explicit stop: tear the server down and return to the menu.
            wlan_remote_stop();
            if(!scene_manager_search_and_switch_to_previous_scene(
                   app->scene_manager, WlanAppSceneMain)) {
                scene_manager_stop(app->scene_manager);
                view_dispatcher_stop(app->view_dispatcher);
            }
            consumed = true;
        } else if(event.event == WlanAppCustomEventRemoteSetToken) {
            // A new token takes effect immediately for new connections; no
            // restart needed. Returning here re-renders with the new token.
            scene_manager_next_scene(app->scene_manager, WlanAppSceneRemoteToken);
            consumed = true;
        } else if(event.event == WlanAppCustomEventRemoteSetup) {
            scene_manager_next_scene(app->scene_manager, WlanAppSceneRemoteSetup);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        if(s_ok && wlan_remote_is_running()) {
            int now = wlan_remote_get_client_count();
            if(now != s_last_clients) {
                s_last_clients = now;
                remote_render(app);
            }
        }
        consumed = true;
    }
    // Back is intentionally not consumed: it pops the scene and leaves the
    // server running in the background.
    return consumed;
}

void wlan_app_scene_remote_on_exit(void* context) {
    WlanApp* app = context;
    widget_reset(app->widget);
    // Deliberately does NOT stop the server -- that is what keeps it alive while
    // the user navigates away to drive other apps remotely.
}
