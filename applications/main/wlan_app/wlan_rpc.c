#include "wlan_rpc.h"
#include "wlan_rpc_jobs.h"
#include "wlan_webfs.h"

#include <boards/board.h>
#include <cJSON.h>
#include <esp_app_desc.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static esp_err_t send_json(httpd_req_t* req, const char* status, cJSON* json) {
    char* body = json ? cJSON_PrintUnformatted(json) : NULL;
    cJSON_Delete(json);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_status(req, body ? status : "500 Internal Server Error");
    esp_err_t err = httpd_resp_sendstr(req, body ? body : "{\"error\":\"out_of_memory\"}");
    cJSON_free(body);
    return err;
}

static esp_err_t error(httpd_req_t* req, const char* status, const char* message) {
    cJSON* json = cJSON_CreateObject();
    if(json && !cJSON_AddStringToObject(json, "error", message)) {
        cJSON_Delete(json);
        json = NULL;
    }
    return send_json(req, status, json);
}

static esp_err_t status_handler(httpd_req_t* req) {
    WlanRpcJob job = wlan_rpc_jobs_snapshot();
    const esp_app_desc_t* app = esp_app_get_description();
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    bool ap = mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA;
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey(ap ? "WIFI_AP_DEF" : "WIFI_STA_DEF");
    esp_netif_ip_info_t info = {0};
    bool up = netif && esp_netif_is_netif_up(netif) &&
              esp_netif_get_ip_info(netif, &info) == ESP_OK;
    char ip[16];
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
    cJSON* json = cJSON_CreateObject();
    if(!json) return send_json(req, "200 OK", NULL);
    bool ok = cJSON_AddStringToObject(json, "device", BOARD_NAME) &&
              cJSON_AddStringToObject(json, "board_id", BOARD_ID) &&
              cJSON_AddStringToObject(json, "firmware", app->version) &&
              cJSON_AddStringToObject(json, "idf_version", app->idf_ver) &&
              cJSON_AddStringToObject(json, "api_version", "1.0") &&
              cJSON_AddNumberToObject(json, "uptime_ms", esp_timer_get_time() / 1000) &&
              cJSON_AddStringToObject(json, "network_mode", ap ? "ap" : "sta") &&
              cJSON_AddBoolToObject(json, "network_up", up && info.ip.addr != 0) &&
              cJSON_AddStringToObject(json, "ip", ip) &&
              cJSON_AddBoolToObject(json, "webfs_running", wlan_webfs_is_running()) &&
              cJSON_AddBoolToObject(json, "busy", job.busy) &&
              cJSON_AddNumberToObject(json, "active_job_id", job.busy ? job.id : 0);
    if(!ok) { cJSON_Delete(json); json = NULL; }
    return send_json(req, "200 OK", json);
}

static esp_err_t capabilities_handler(httpd_req_t* req) {
    cJSON* json = cJSON_CreateObject();
    if(!json) return send_json(req, "200 OK", NULL);
    bool ok = cJSON_AddStringToObject(json, "api_version", "1.0") &&
              cJSON_AddBoolToObject(json, "read_only", true) &&
              cJSON_AddBoolToObject(json, "subghz_rx", wlan_rpc_jobs_supported()) &&
              cJSON_AddBoolToObject(json, "subghz_tx", false) &&
              cJSON_AddBoolToObject(json, "protocol_decode", false) &&
              cJSON_AddBoolToObject(json, "raw_file_capture", false) &&
              cJSON_AddBoolToObject(json, "ir_rx", false) &&
              cJSON_AddBoolToObject(json, "nfc_read", false) &&
              cJSON_AddStringToObject(json, "preset", "OOK650Async") &&
              cJSON_AddRawToObject(json, "frequency_ranges_hz",
                  "[[281000000,361000000],[378000000,481000000],[749000000,962000000]]") &&
              cJSON_AddStringToObject(json, "result", "pulse_statistics_and_rssi") &&
              cJSON_AddStringToObject(json, "jobs_route", "/api/jobs?id=<id>") &&
              cJSON_AddNumberToObject(json, "min_duration_ms", 100) &&
              cJSON_AddNumberToObject(json, "max_duration_ms", WLAN_RPC_MAX_DURATION_MS) &&
              cJSON_AddNumberToObject(json, "retained_jobs", 1);
    if(!ok) { cJSON_Delete(json); json = NULL; }
    return send_json(req, "200 OK", json);
}

static bool integer(const cJSON* value, double min, double max) {
    return cJSON_IsNumber(value) && isfinite(value->valuedouble) &&
           value->valuedouble >= min && value->valuedouble <= max &&
           floor(value->valuedouble) == value->valuedouble;
}

static esp_err_t rx_handler(httpd_req_t* req) {
    if(!wlan_rpc_jobs_supported()) {
        error(req, "501 Not Implemented", "unsupported_board");
        return ESP_FAIL;
    }
    char type[64];
    if(httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type)) != ESP_OK ||
       (strcmp(type, "application/json") != 0 &&
        strcmp(type, "application/json; charset=utf-8") != 0)) {
        error(req, "415 Unsupported Media Type", "use_application_json");
        return ESP_FAIL;
    }
    char body[257];
    if(req->content_len == 0 || req->content_len >= sizeof(body)) {
        error(req, "413 Payload Too Large", "body_must_be_1_to_256_bytes");
        return ESP_FAIL;
    }
    size_t received = 0;
    while(received < req->content_len) {
        int n = httpd_req_recv(req, body + received, req->content_len - received);
        if(n <= 0) {
            error(req, "408 Request Timeout", "incomplete_body");
            return ESP_FAIL; /* Close the socket; do not reuse unread request bytes. */
        }
        received += (size_t)n;
    }
    body[received] = 0;
    if(memchr(body, 0, received)) return error(req, "400 Bad Request", "invalid_json");
    cJSON* json = cJSON_ParseWithOpts(body, NULL, true);
    if(!cJSON_IsObject(json)) {
        cJSON_Delete(json);
        return error(req, "400 Bad Request", "expected_json_object");
    }
    uint32_t frequency = 433920000, duration = 5000;
    bool seen_frequency = false, seen_duration = false, valid = true;
    const cJSON* item;
    cJSON_ArrayForEach(item, json) {
        if(strcmp(item->string, "frequency_hz") == 0 && !seen_frequency &&
           integer(item, 1, UINT32_MAX)) {
            frequency = (uint32_t)item->valuedouble;
            seen_frequency = true;
        } else if(strcmp(item->string, "duration_ms") == 0 && !seen_duration &&
                  integer(item, 100, WLAN_RPC_MAX_DURATION_MS)) {
            duration = (uint32_t)item->valuedouble;
            seen_duration = true;
        } else if(strcmp(item->string, "seconds") == 0 && !seen_duration &&
                  integer(item, 1, WLAN_RPC_MAX_DURATION_MS / 1000)) {
            duration = (uint32_t)item->valuedouble * 1000;
            seen_duration = true;
        } else {
            valid = false; /* Reject unknown keys, duplicate keys and both durations. */
        }
    }
    cJSON_Delete(json);
    if(!valid || !wlan_rpc_jobs_frequency_valid(frequency)) {
        return error(req, "400 Bad Request", "invalid_frequency_duration_or_key");
    }
    uint32_t id = wlan_rpc_jobs_start(frequency, duration);
    if(!id) return error(req, "409 Conflict", "busy_or_stopping");
    /* No heap allocation after starting the job. */
    char response[32];
    snprintf(response, sizeof(response), "{\"id\":%lu}", (unsigned long)id);
    char location[48];
    snprintf(location, sizeof(location), "/api/jobs?id=%lu", (unsigned long)id);
    httpd_resp_set_hdr(req, "Location", location);
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, response);
}

static esp_err_t job_handler(httpd_req_t* req) {
    char query[48];
    if(httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
       strncmp(query, "id=", 3) != 0 || query[3] == 0) {
        return error(req, "400 Bad Request", "use_jobs_id_positive_integer");
    }
    uint32_t id = 0;
    for(const char* p = query + 3; *p; p++) {
        if(*p < '0' || *p > '9' || id > (UINT32_MAX - (uint32_t)(*p - '0')) / 10) {
            return error(req, "400 Bad Request", "invalid_id");
        }
        id = id * 10 + (uint32_t)(*p - '0');
    }
    WlanRpcJob job = wlan_rpc_jobs_snapshot();
    if(id == 0 || id != job.id) return error(req, "404 Not Found", "job_not_retained");
    cJSON* json = cJSON_CreateObject();
    if(!json) return send_json(req, "200 OK", NULL);
    bool ok = cJSON_AddNumberToObject(json, "id", job.id) &&
              cJSON_AddStringToObject(json, "state", job.busy ? "running" :
                                      (job.cancelled ? "cancelled" : "done")) &&
              cJSON_AddNumberToObject(json, "frequency_hz", job.frequency_hz) &&
              cJSON_AddNumberToObject(json, "actual_frequency_hz", job.actual_frequency_hz) &&
              cJSON_AddNumberToObject(json, "duration_ms", job.duration_ms) &&
              cJSON_AddNumberToObject(json, "elapsed_ms", job.elapsed_ms) &&
              cJSON_AddNumberToObject(json, "pulses", job.pulses) &&
              cJSON_AddNumberToObject(json, "high_pulses", job.high_pulses) &&
              cJSON_AddNumberToObject(json, "rssi_samples", job.rssi_samples) &&
              (job.rssi_samples ? cJSON_AddNumberToObject(json, "peak_rssi_dbm", job.peak_rssi_dbm) :
                                  cJSON_AddNullToObject(json, "peak_rssi_dbm"));
    if(!ok) { cJSON_Delete(json); json = NULL; }
    return send_json(req, "200 OK", json);
}

esp_err_t wlan_rpc_register(httpd_handle_t server) {
    static const httpd_uri_t routes[] = {
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_handler},
        {.uri = "/api/capabilities", .method = HTTP_GET, .handler = capabilities_handler},
        {.uri = "/api/subghz/rx", .method = HTTP_POST, .handler = rx_handler},
        {.uri = "/api/jobs", .method = HTTP_GET, .handler = job_handler},
    };
    if(!wlan_rpc_jobs_init()) return ESP_ERR_NO_MEM;
    for(size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &routes[i]);
        if(err != ESP_OK) {
            for(size_t j = 0; j < i; j++) {
                httpd_unregister_uri_handler(server, routes[j].uri, routes[j].method);
            }
            return err;
        }
    }
    return ESP_OK;
}

void wlan_rpc_stop(void) {
    wlan_rpc_jobs_stop();
}
