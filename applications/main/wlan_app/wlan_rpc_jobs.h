#pragma once

#include <stdbool.h>
#include <stdint.h>

#define WLAN_RPC_MAX_DURATION_MS 60000u

typedef struct {
    uint32_t id;
    uint32_t frequency_hz;
    uint32_t actual_frequency_hz;
    uint32_t duration_ms;
    uint32_t elapsed_ms;
    uint32_t pulses;
    uint32_t high_pulses;
    uint32_t rssi_samples;
    float peak_rssi_dbm;
    bool busy;
    bool cancelled;
} WlanRpcJob;

bool wlan_rpc_jobs_init(void);
/* Call only after httpd_stop(): cancels RX and waits for hardware cleanup. */
void wlan_rpc_jobs_stop(void);
bool wlan_rpc_jobs_supported(void);
bool wlan_rpc_jobs_frequency_valid(uint32_t frequency_hz);
/* HTTP handlers are serialized by esp_http_server. Returns 0 when busy/stopped. */
uint32_t wlan_rpc_jobs_start(uint32_t frequency_hz, uint32_t duration_ms);
WlanRpcJob wlan_rpc_jobs_snapshot(void);
