#pragma once

/* Wi-Fi Remote: a WebSocket endpoint that bridges to a full Flipper RPC session
 * (RpcOwnerWifi), carrying the same protobuf protocol the USB CLI uses -- screen
 * stream, input, storage and app control. It lives in the always-linked wifi
 * component (not the unloadable wlan_app) so the server keeps running while the
 * user launches and drives other apps remotely.
 *
 * Transport only: it owns no device logic, it just moves bytes between the
 * WebSocket client and an RPC session, gated by a per-device access token.
 *
 * Requires an active wlan_hal STA connection (wlan_hal_is_connected()). */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Length of the access token, excluding the NUL. 8 characters from an
 * unambiguous alphabet (no 0/O, 1/I/L, U) -- short and legible on the small
 * low-res display, ~40 bits of entropy, enough for a LAN-only device. */
#define WLAN_REMOTE_TOKEN_LEN 8

/* Start the Wi-Fi Remote server on the current STA connection. Idempotent:
 * returns true if it is (now) running. Loads or generates the access token. */
bool wlan_remote_start(void);

/* Stop the server and close any active RPC session. */
void wlan_remote_stop(void);

bool wlan_remote_is_running(void);

/* Current STA IPv4 as a string ("a.b.c.d"). false if not running. */
bool wlan_remote_get_ip(char* out, size_t len);

/* The access token (WLAN_REMOTE_TOKEN_LEN chars + NUL). false if unavailable. */
bool wlan_remote_get_token(char* out, size_t len);

/* Set a permanent, user-chosen token. Input is normalized (uppercased, group
 * separators dropped, common look-alikes folded); must be WLAN_REMOTE_TOKEN_LEN
 * valid characters after that. Persists to SD. Returns false if invalid. */
bool wlan_remote_set_token(const char* token);

/* TCP port the WebSocket endpoint listens on. */
uint16_t wlan_remote_get_port(void);

/* Number of currently connected remote clients (0 or 1 in this version). */
uint8_t wlan_remote_get_client_count(void);

/* Auto-recovery ("dead-man's switch"): when a remotely launched app seizes the
 * Wi-Fi radio, the STA link (and this server) go down. If the connection stays
 * lost while an app is running for longer than recovery_timeout_ms, and
 * return_home is set, the foreground app is signalled to exit so the radio is
 * freed, the home network reconnects, and the server re-binds -- bringing the
 * device back under remote control. */
typedef struct {
    uint32_t recovery_timeout_ms; // 0 = off
    bool return_home;
} WlanRemoteSettings;

void wlan_remote_get_settings(WlanRemoteSettings* out);
void wlan_remote_set_settings(const WlanRemoteSettings* s);

#ifdef __cplusplus
}
#endif
