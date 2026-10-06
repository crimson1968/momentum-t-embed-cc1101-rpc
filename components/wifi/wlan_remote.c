#include "wlan_remote.h"
#include "wlan_hal.h"

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_random.h>
#include <storage/storage.h>
#include <saved_struct.h>
#include <rpc/rpc.h>
#include <loader/loader.h>

#include <esp_http_server.h>
#include <esp_wifi.h>
#include <lwip/sockets.h>
#include <string.h>

#define TAG "WlanRemote"

#define WLAN_REMOTE_PORT        (80)
#define WLAN_REMOTE_WS_PATH     "/rpc"
#define WLAN_REMOTE_TOKEN_DIR   "/ext/apps_data/wlan_remote"
#define WLAN_REMOTE_TOKEN_PATH  WLAN_REMOTE_TOKEN_DIR "/token"
/* One WS binary frame carries one or more RPC byte chunks. Flipper RPC messages
 * are small (screen frames ~1 KB); cap inbound frames well above that and reject
 * anything larger as a malformed/hostile client. */
#define WLAN_REMOTE_MAX_FRAME   (16384)

/* All server state is static: it is owned by this always-linked component, not
 * by the wlan_app that merely starts it, so it survives the app unloading. */
static httpd_handle_t s_http = NULL;
static bool s_running = false;
static char s_ip[16] = {0};
static char s_token[WLAN_REMOTE_TOKEN_LEN + 1] = {0};

/* Keep the device awake and the radio pinned while the server runs: without an
 * insomnia hold the device sleeps after ~30 s and drops the STA (and the
 * server). Pinning the user radio keeps Wi-Fi up across app switches and
 * auto-reconnects after transient drops. Both are restored on stop. */
static bool s_insomnia = false;
static bool s_prev_user_enabled = false;

/* Periodic connectivity monitor: re-binds the server after a Wi-Fi reconnect
 * (the old listen socket dies on the previous netif) and follows IP changes, so
 * the device does not end up answering with "connection refused" after a blip. */
static FuriTimer* s_monitor = NULL;
static uint32_t s_ip_addr = 0;
#define WLAN_REMOTE_MONITOR_MS (2000)

/* Auto-recovery settings + state. s_recovery_deadline is the tick at which, if
 * the connection is still down with an app running, we force the app to exit. */
#define WLAN_REMOTE_SETTINGS_PATH  WLAN_REMOTE_TOKEN_DIR "/settings"
#define WLAN_REMOTE_SETTINGS_MAGIC (0x57)
#define WLAN_REMOTE_SETTINGS_VER   (1)
static WlanRemoteSettings s_settings = {.recovery_timeout_ms = 0, .return_home = true};
static bool s_settings_loaded = false;
static uint32_t s_recovery_deadline = 0; // 0 = not armed

/* Single active RPC session, guarded by s_lock against the httpd task (open on
 * GET, feed on frame, end on close) racing the RPC worker (send/terminate). */
static FuriMutex* s_lock = NULL;
static RpcSession* s_session = NULL;
static int s_session_fd = -1;
static bool s_session_closing = false;

/* ─────────────────────── token ─────────────────────── */

/* Crockford base32: no 0/O, 1/I/L, U ambiguities when read off the display. */
static const char WLAN_REMOTE_ALPHABET[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

/* Canonicalize a user- or file-supplied token: uppercase, drop group separators,
 * and fold the characters people most often confuse into the alphabet. */
static void remote_token_normalize(const char* in, char* out, size_t out_sz) {
    size_t j = 0;
    for(size_t i = 0; in[i] && j + 1 < out_sz; i++) {
        char c = in[i];
        if(c >= 'a' && c <= 'z') c -= 32;
        if(c == '-' || c == ' ' || c == '\r' || c == '\n') continue;
        if(c == 'O') c = '0';
        else if(c == 'I' || c == 'L') c = '1';
        else if(c == 'U') c = 'V';
        out[j++] = c;
    }
    out[j] = '\0';
}

static bool remote_token_is_valid(const char* s) {
    if(strlen(s) != WLAN_REMOTE_TOKEN_LEN) return false;
    for(size_t i = 0; i < WLAN_REMOTE_TOKEN_LEN; i++) {
        if(!strchr(WLAN_REMOTE_ALPHABET, s[i])) return false;
    }
    return true;
}

static void remote_token_generate(char* out) {
    uint8_t raw[WLAN_REMOTE_TOKEN_LEN];
    furi_hal_random_fill_buf(raw, sizeof(raw));
    for(size_t i = 0; i < WLAN_REMOTE_TOKEN_LEN; i++) {
        out[i] = WLAN_REMOTE_ALPHABET[raw[i] & 31];
    }
    out[WLAN_REMOTE_TOKEN_LEN] = '\0';
}

static bool remote_token_save(const char* token) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, "/ext/apps_data");
    storage_simply_mkdir(storage, WLAN_REMOTE_TOKEN_DIR);
    File* file = storage_file_alloc(storage);
    bool ok = false;
    if(storage_file_open(file, WLAN_REMOTE_TOKEN_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        ok = storage_file_write(file, token, WLAN_REMOTE_TOKEN_LEN) == WLAN_REMOTE_TOKEN_LEN;
    }
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return ok;
}

/* Load the saved token, or generate and persist a new one. The token lives on SD
 * so it is stable across reboots; a user-set token (via the UI or by editing the
 * file) is authoritative and simply loaded here. */
static void remote_token_ensure(void) {
    if(remote_token_is_valid(s_token)) return;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    bool loaded = false;

    if(storage_file_open(file, WLAN_REMOTE_TOKEN_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        char buf[64] = {0};
        size_t n = storage_file_read(file, buf, sizeof(buf) - 1);
        buf[n] = '\0';
        char norm[64];
        remote_token_normalize(buf, norm, sizeof(norm));
        if(remote_token_is_valid(norm)) {
            strcpy(s_token, norm);
            loaded = true;
        }
    }
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);

    if(!loaded) {
        remote_token_generate(s_token);
        remote_token_save(s_token);
        FURI_LOG_I(TAG, "Generated new remote token");
    }
}

static bool remote_request_token_ok(httpd_req_t* req) {
    size_t qlen = httpd_req_get_url_query_len(req) + 1;
    if(qlen <= 1 || qlen > 256) return false;
    char* query = malloc(qlen);
    bool ok = false;
    if(httpd_req_get_url_query_str(req, query, qlen) == ESP_OK) {
        char value[64] = {0};
        if(httpd_query_key_value(query, "token", value, sizeof(value)) == ESP_OK) {
            char norm[64];
            remote_token_normalize(value, norm, sizeof(norm));
            ok = remote_token_is_valid(norm) && (strcmp(norm, s_token) == 0);
        }
    }
    free(query);
    return ok;
}

/* ─────────────────────── settings ─────────────────────── */

static void remote_settings_ensure_loaded(void) {
    if(s_settings_loaded) return;
    s_settings_loaded = true;
    if(!saved_struct_load(
           WLAN_REMOTE_SETTINGS_PATH,
           &s_settings,
           sizeof(s_settings),
           WLAN_REMOTE_SETTINGS_MAGIC,
           WLAN_REMOTE_SETTINGS_VER)) {
        s_settings.recovery_timeout_ms = 0;
        s_settings.return_home = true;
    }
}

void wlan_remote_get_settings(WlanRemoteSettings* out) {
    if(!out) return;
    remote_settings_ensure_loaded();
    *out = s_settings;
}

void wlan_remote_set_settings(const WlanRemoteSettings* s) {
    if(!s) return;
    s_settings = *s;
    s_settings_loaded = true;
    // Re-arm fresh against the new timeout.
    s_recovery_deadline = 0;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, "/ext/apps_data");
    storage_simply_mkdir(storage, WLAN_REMOTE_TOKEN_DIR);
    furi_record_close(RECORD_STORAGE);
    saved_struct_save(
        WLAN_REMOTE_SETTINGS_PATH,
        &s_settings,
        sizeof(s_settings),
        WLAN_REMOTE_SETTINGS_MAGIC,
        WLAN_REMOTE_SETTINGS_VER);
}

/* ─────────────────────── outbound: RPC -> WebSocket ─────────────────────── */

typedef struct {
    httpd_handle_t hd;
    int fd;
    size_t len;
    uint8_t data[];
} RemoteAsyncSend;

/* Runs on the httpd task (via httpd_queue_work), the only safe context to push
 * an unsolicited frame to a client socket. */
static void remote_ws_async_send(void* arg) {
    RemoteAsyncSend* a = arg;
    httpd_ws_frame_t frame = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_BINARY,
        .payload = a->data,
        .len = a->len,
    };
    httpd_ws_send_frame_async(a->hd, a->fd, &frame);
    free(a);
}

/* RPC worker thread -> marshal the bytes onto the httpd task. It is forbidden to
 * call RPC API here; we only copy and queue. */
static void remote_rpc_send_bytes(void* context, uint8_t* bytes, size_t len) {
    UNUSED(context);
    if(!s_http || len == 0) return;

    furi_mutex_acquire(s_lock, FuriWaitForever);
    httpd_handle_t hd = s_http;
    int fd = s_session_fd;
    bool have = (s_session != NULL) && (fd >= 0) && !s_session_closing;
    furi_mutex_release(s_lock);
    if(!have) return;

    RemoteAsyncSend* a = malloc(sizeof(RemoteAsyncSend) + len);
    a->hd = hd;
    a->fd = fd;
    a->len = len;
    memcpy(a->data, bytes, len);
    if(httpd_queue_work(hd, remote_ws_async_send, a) != ESP_OK) {
        free(a);
    }
}

/* ─────────────────────── session lifecycle ─────────────────────── */

static void remote_session_terminated(void* context) {
    furi_mutex_acquire(s_lock, FuriWaitForever);
    // Only clear if the session that terminated is the current one -- a stale
    // (previous) session's teardown must not wipe a newly opened session.
    bool current = (context == (void*)s_session);
    if(current) {
        s_session = NULL;
        s_session_fd = -1;
        s_session_closing = false;
    }
    furi_mutex_release(s_lock);
    FURI_LOG_I(TAG, "Remote RPC session terminated (current=%d)", (int)current);
}

/* The client asked to stop the session (stop_session command). Trigger the
 * socket close; the httpd close hook then tears the RPC session down. Must not
 * call RPC API from within this callback. */
static void remote_session_close_requested(void* context) {
    UNUSED(context);
    httpd_handle_t hd;
    int fd;
    furi_mutex_acquire(s_lock, FuriWaitForever);
    hd = s_http;
    fd = s_session_fd;
    furi_mutex_release(s_lock);
    if(hd && fd >= 0) httpd_sess_trigger_close(hd, fd);
}

/* Open a fresh RPC session bound to this client fd. Caller holds no lock. */
static bool remote_session_begin(httpd_handle_t hd, int fd) {
    Rpc* rpc = furi_record_open(RECORD_RPC);
    RpcSession* session = rpc_session_open(rpc, RpcOwnerWifi);
    furi_record_close(RECORD_RPC);
    if(!session) {
        FURI_LOG_E(TAG, "rpc_session_open failed");
        return false;
    }

    // Pass the session as the callback context so terminated/close callbacks
    // can tell whether they belong to the current session.
    rpc_session_set_context(session, session);
    rpc_session_set_send_bytes_callback(session, remote_rpc_send_bytes);
    rpc_session_set_close_callback(session, remote_session_close_requested);
    rpc_session_set_terminated_callback(session, remote_session_terminated);

    furi_mutex_acquire(s_lock, FuriWaitForever);
    s_session = session;
    s_session_fd = fd;
    s_session_closing = false;
    furi_mutex_release(s_lock);

    UNUSED(hd);
    FURI_LOG_I(TAG, "Remote RPC session opened (fd=%d)", fd);
    return true;
}

/* Tear the session down (socket closed or server stopping). Safe to call from
 * the httpd task; not from inside an RPC callback. */
static void remote_session_end(void) {
    RpcSession* session = NULL;
    furi_mutex_acquire(s_lock, FuriWaitForever);
    if(s_session && !s_session_closing) {
        s_session_closing = true;
        session = s_session;
    }
    furi_mutex_release(s_lock);
    if(session) {
        // terminated callback clears s_session once the worker has finished.
        rpc_session_close(session);
    }
}

/* ─────────────────────── WebSocket handler ─────────────────────── */

static esp_err_t remote_ws_handler(httpd_req_t* req) {
    const int fd = httpd_req_to_sockfd(req);

    // esp_http_server reuses the handshake request object for subsequent WS
    // frames, so req->method stays HTTP_GET for data frames too. Discriminate
    // the handshake from a frame by whether this fd already has a session, not
    // by method alone -- otherwise every frame re-runs the (token-less) token
    // check and the connection is closed on the first command.
    bool established;
    furi_mutex_acquire(s_lock, FuriWaitForever);
    established = (s_session != NULL && s_session_fd == fd);
    furi_mutex_release(s_lock);

    if(req->method == HTTP_GET && !established) {
        // Upgrade handshake: authenticate and bind the single session.
        if(!remote_request_token_ok(req)) {
            FURI_LOG_W(TAG, "Rejected remote client: bad token");
            return ESP_FAIL;
        }
        furi_mutex_acquire(s_lock, FuriWaitForever);
        bool busy = (s_session != NULL);
        furi_mutex_release(s_lock);
        if(busy) {
            FURI_LOG_W(TAG, "Rejected remote client: session already active");
            return ESP_FAIL;
        }
        if(!remote_session_begin(req->handle, fd)) return ESP_FAIL;
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {0};
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if(err != ESP_OK) return err;
    if(frame.len > WLAN_REMOTE_MAX_FRAME) return ESP_FAIL;

    uint8_t* payload = NULL;
    if(frame.len) {
        payload = malloc(frame.len);
        if(!payload) return ESP_ERR_NO_MEM;
        frame.payload = payload;
        err = httpd_ws_recv_frame(req, &frame, frame.len);
        if(err != ESP_OK) {
            free(payload);
            return err;
        }
    }

    switch(frame.type) {
    case HTTPD_WS_TYPE_BINARY: {
        RpcSession* session;
        furi_mutex_acquire(s_lock, FuriWaitForever);
        session = (s_session && s_session_fd == fd && !s_session_closing) ? s_session : NULL;
        furi_mutex_release(s_lock);
        if(session && frame.len) {
            rpc_session_feed(session, payload, frame.len, 3000);
        }
        break;
    }
    case HTTPD_WS_TYPE_PING: {
        httpd_ws_frame_t pong = {
            .final = true,
            .type = HTTPD_WS_TYPE_PONG,
            .payload = payload,
            .len = frame.len,
        };
        httpd_ws_send_frame(req, &pong);
        break;
    }
    case HTTPD_WS_TYPE_CLOSE:
        if(payload) free(payload);
        return ESP_FAIL; // triggers close hook
    default:
        break;
    }

    if(payload) free(payload);
    return ESP_OK;
}

/* Called by httpd when a socket is torn down (client gone, CLOSE, or error). */
static void remote_http_close(httpd_handle_t hd, int fd) {
    bool ours;
    furi_mutex_acquire(s_lock, FuriWaitForever);
    ours = (s_session_fd == fd);
    furi_mutex_release(s_lock);
    if(ours) remote_session_end();
    UNUSED(hd);
    close(fd);
}

/* ─────────────────────── server bring-up (wlan_hal worker) ─────────────────────── */

static bool remote_start_http(void) {
    if(s_http) return true;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WLAN_REMOTE_PORT;
    // Distinct UDP control-socket port so this never clashes with another httpd
    // instance (e.g. WebFS) that uses the default 32768.
    config.ctrl_port = 32769;
    config.stack_size = 6144;
    config.max_uri_handlers = 4;
    config.max_open_sockets = 3;
    config.lru_purge_enable = true;
    config.close_fn = remote_http_close;

    esp_err_t err = httpd_start(&s_http, &config);
    if(err != ESP_OK) {
        FURI_LOG_E(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        s_http = NULL;
        return false;
    }

    static const httpd_uri_t ws = {
        .uri = WLAN_REMOTE_WS_PATH,
        .method = HTTP_GET,
        .handler = remote_ws_handler,
        .is_websocket = true,
        .handle_ws_control_frames = true,
    };
    if(httpd_register_uri_handler(s_http, &ws) != ESP_OK) {
        httpd_stop(s_http);
        s_http = NULL;
        return false;
    }
    return true;
}

typedef struct {
    bool result;
} RemoteWorkerArgs;

static void remote_start_worker(void* arg) {
    RemoteWorkerArgs* a = arg;
    a->result = remote_start_http();
    if(a->result) {
        // Disable Wi-Fi modem sleep: with power-save on, the radio sleeps
        // between beacons and idle TCP/WebSocket connections are dropped within
        // seconds and the device is intermittently unreachable. The shipped
        // hotspot_arcade WebSocket server does the same.
        esp_wifi_set_ps(WIFI_PS_NONE);
    }
}

static void remote_stop_worker(void* arg) {
    UNUSED(arg);
    if(s_http) {
        httpd_stop(s_http);
        s_http = NULL;
    }
    // Restore the default modem power-save to save battery when not serving.
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
}

/* Stop only the httpd (no power-save change) -- used when the STA is temporarily
 * down; the monitor re-binds once it reconnects. */
static void remote_httpd_stop_worker(void* arg) {
    UNUSED(arg);
    if(s_http) {
        httpd_stop(s_http);
        s_http = NULL;
    }
}

/* Tear down any dead listener and start a fresh one bound to the current netif. */
static void remote_rebind_worker(void* arg) {
    RemoteWorkerArgs* a = arg;
    if(s_http) {
        httpd_stop(s_http);
        s_http = NULL;
    }
    a->result = remote_start_http();
    if(a->result) esp_wifi_set_ps(WIFI_PS_NONE);
}

static void remote_set_ip(uint32_t ip) {
    s_ip_addr = ip;
    snprintf(
        s_ip,
        sizeof(s_ip),
        "%u.%u.%u.%u",
        (unsigned)(ip & 0xff),
        (unsigned)((ip >> 8) & 0xff),
        (unsigned)((ip >> 16) & 0xff),
        (unsigned)((ip >> 24) & 0xff));
}

/* Auto-recovery: a remotely launched app has seized the radio and the link has
 * stayed down past the configured timeout -- signal the foreground app to exit
 * so the radio frees, the home network reconnects and the server re-binds. */
static void remote_recovery_trigger(void) {
    Loader* loader = furi_record_open(RECORD_LOADER);
    bool signalled = loader_signal(loader, FuriSignalExit, NULL);
    furi_record_close(RECORD_LOADER);
    FURI_LOG_I(TAG, "Wi-Fi Remote recovery: exit app signalled=%d", (int)signalled);
    // Reconnect + re-bind happen via wlan_hal's background STA (pinned on in
    // wlan_remote_start) and this monitor once the radio is free again.
}

/* Periodic: keep the listener bound to the live STA connection, and run the
 * recovery dead-man's switch while the link is down. */
static void remote_monitor_tick(void* ctx) {
    UNUSED(ctx);
    if(!s_running) return;

    if(wlan_hal_is_connected()) {
        s_recovery_deadline = 0; // link is healthy; disarm recovery
        uint32_t ip = wlan_hal_get_own_ip();
        if(s_http == NULL || ip != s_ip_addr) {
            RemoteWorkerArgs a = {.result = false};
            if(wlan_hal_run_in_worker(remote_rebind_worker, &a) && a.result) {
                remote_set_ip(ip);
                FURI_LOG_I(TAG, "Wi-Fi Remote (re)bound at %s", s_ip);
            }
        }
        return;
    }

    // STA is down.
    if(s_http) {
        // Drop the dead listener; the next reconnect re-binds automatically.
        wlan_hal_run_in_worker(remote_httpd_stop_worker, NULL);
        FURI_LOG_I(TAG, "Wi-Fi Remote: STA down, server paused");
    }

    // Recovery only applies while an app is running (something seized the radio);
    // a plain background drop just waits for the auto-reconnect.
    remote_settings_ensure_loaded();
    bool armed = s_settings.recovery_timeout_ms > 0 && s_settings.return_home;
    Loader* loader = furi_record_open(RECORD_LOADER);
    bool app_running = loader_is_locked(loader);
    furi_record_close(RECORD_LOADER);

    if(armed && app_running) {
        uint32_t now = furi_get_tick();
        if(s_recovery_deadline == 0) {
            s_recovery_deadline = now + furi_ms_to_ticks(s_settings.recovery_timeout_ms);
        } else if((int32_t)(now - s_recovery_deadline) >= 0) {
            s_recovery_deadline = 0;
            remote_recovery_trigger();
        }
    } else {
        s_recovery_deadline = 0;
    }
}

/* ─────────────────────── public API ─────────────────────── */

bool wlan_remote_start(void) {
    if(s_running) return true;
    if(!wlan_hal_is_connected()) {
        FURI_LOG_W(TAG, "Cannot start: no STA connection");
        return false;
    }
    if(!s_lock) s_lock = furi_mutex_alloc(FuriMutexTypeNormal);

    remote_token_ensure();

    RemoteWorkerArgs a = {.result = false};
    if(!wlan_hal_run_in_worker(remote_start_worker, &a) || !a.result) {
        FURI_LOG_E(TAG, "Failed to start remote server");
        return false;
    }

    remote_set_ip(wlan_hal_get_own_ip());

    s_running = true;
    s_recovery_deadline = 0;
    remote_settings_ensure_loaded();

    // Watch the STA connection, re-bind after reconnects, run recovery.
    if(!s_monitor) {
        s_monitor = furi_timer_alloc(remote_monitor_tick, FuriTimerTypePeriodic, NULL);
    }
    furi_timer_start(s_monitor, furi_ms_to_ticks(WLAN_REMOTE_MONITOR_MS));

    // Stay awake so the STA (and this server) are not dropped by sleep.
    if(!s_insomnia) {
        furi_hal_power_insomnia_enter();
        s_insomnia = true;
    }
    // Keep Wi-Fi on in the background so it survives leaving the app and
    // reconnects automatically after transient drops.
    s_prev_user_enabled = wlan_hal_is_user_enabled();
    if(!s_prev_user_enabled) wlan_hal_set_user_enabled(true);

    FURI_LOG_I(TAG, "Wi-Fi Remote active at ws://%s:%d%s", s_ip, WLAN_REMOTE_PORT, WLAN_REMOTE_WS_PATH);
    return true;
}

void wlan_remote_stop(void) {
    if(!s_running) return;
    if(s_monitor) furi_timer_stop(s_monitor);
    remote_session_end();
    wlan_hal_run_in_worker(remote_stop_worker, NULL);
    s_running = false;
    s_ip[0] = '\0';
    s_ip_addr = 0;

    if(s_insomnia) {
        furi_hal_power_insomnia_exit();
        s_insomnia = false;
    }
    // Restore the user's prior Wi-Fi preference only if we were the ones who
    // turned it on.
    if(!s_prev_user_enabled) wlan_hal_set_user_enabled(false);

    FURI_LOG_I(TAG, "Wi-Fi Remote stopped");
}

bool wlan_remote_is_running(void) {
    return s_running;
}

bool wlan_remote_get_ip(char* out, size_t len) {
    if(!s_running || !out || len < 8) return false;
    strncpy(out, s_ip, len - 1);
    out[len - 1] = '\0';
    return true;
}

bool wlan_remote_get_token(char* out, size_t len) {
    if(!out || len <= WLAN_REMOTE_TOKEN_LEN) return false;
    remote_token_ensure();
    if(!remote_token_is_valid(s_token)) return false;
    strncpy(out, s_token, len - 1);
    out[len - 1] = '\0';
    return true;
}

bool wlan_remote_set_token(const char* token) {
    char norm[64];
    remote_token_normalize(token ? token : "", norm, sizeof(norm));
    if(!remote_token_is_valid(norm)) return false;
    strcpy(s_token, norm);
    return remote_token_save(s_token);
}

uint16_t wlan_remote_get_port(void) {
    return WLAN_REMOTE_PORT;
}

uint8_t wlan_remote_get_client_count(void) {
    uint8_t n;
    furi_mutex_acquire(s_lock ? s_lock : (s_lock = furi_mutex_alloc(FuriMutexTypeNormal)), FuriWaitForever);
    n = (s_session != NULL) ? 1 : 0;
    furi_mutex_release(s_lock);
    return n;
}
