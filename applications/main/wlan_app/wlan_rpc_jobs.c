#include "wlan_rpc_jobs.h"

#include <furi_hal_subghz.h>
#include <boards/board.h>
#include <lib/subghz/devices/devices.h>
#include <lib/subghz/devices/cc1101_int/cc1101_int_interconnect.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <string.h>

/* Fixed memory, one job at a time; only the latest result is retained.
 * The WiFi application owns the loader lock for the entire WebFS lifetime.
 * That excludes other GUI apps and non-parallel-safe CLI commands (subghz).
 * Do not expose this service outside that lifecycle without a shared RF lease.
 * Use the built-in device directly: no global device-registry init/deinit. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static WlanRpcJob s_job;
static uint32_t s_next_id;
static bool s_stopping;
static bool s_cancel_requested;
static bool s_capturing;
static QueueHandle_t s_requests;
static SemaphoreHandle_t s_exited;

bool wlan_rpc_jobs_supported(void) {
    return BOARD_HAS_SUBGHZ && strcmp(BOARD_ID, "t-embed") == 0;
}

bool wlan_rpc_jobs_frequency_valid(uint32_t frequency_hz) {
    return wlan_rpc_jobs_supported() && furi_hal_subghz_is_frequency_valid(frequency_hz);
}

static void capture(bool level, uint32_t duration, void* context) {
    (void)duration;
    (void)context;
    /* Called in GPIO ISR context. Never allocate, log or call the HTTP stack. */
    portENTER_CRITICAL_ISR(&s_lock);
    if(s_capturing && s_job.pulses != UINT32_MAX) {
        s_job.pulses++;
        if(level) s_job.high_pulses++;
    }
    portEXIT_CRITICAL_ISR(&s_lock);
}

static void worker(void* context) {
    (void)context;
    uint8_t request;
    const SubGhzDevice* device = &subghz_device_cc1101_int;
    while(xQueueReceive(s_requests, &request, portMAX_DELAY) == pdTRUE) {
        if(request == 0) break;
        WlanRpcJob job = wlan_rpc_jobs_snapshot();
        /* Same preset and device functions as subghz_cli_command_rx().
         * cc1101_int has no begin hook; its is_connect hook always returns true,
         * so neither is used as an invented hardware-presence check. */
        subghz_devices_reset(device);
        subghz_devices_load_preset(device, FuriHalSubGhzPresetOok650Async, NULL);
        uint32_t actual = subghz_devices_set_frequency(device, job.frequency_hz);
        portENTER_CRITICAL(&s_lock);
        s_job.actual_frequency_hz = actual;
        s_capturing = true;
        portEXIT_CRITICAL(&s_lock);
        subghz_devices_start_async_rx(device, capture, NULL);
        int64_t started = esp_timer_get_time();
        for(;;) {
            uint32_t elapsed = (uint32_t)((esp_timer_get_time() - started) / 1000);
            portENTER_CRITICAL(&s_lock);
            bool stop = s_stopping || s_cancel_requested;
            s_job.elapsed_ms = elapsed;
            portEXIT_CRITICAL(&s_lock);
            if(stop || elapsed >= job.duration_ms) break;
            float rssi = subghz_devices_get_rssi(device);
            portENTER_CRITICAL(&s_lock);
            if(s_job.rssi_samples == 0 || rssi > s_job.peak_rssi_dbm) {
                s_job.peak_rssi_dbm = rssi;
            }
            s_job.rssi_samples++;
            portEXIT_CRITICAL(&s_lock);
            vTaskDelay(pdMS_TO_TICKS(20) ? pdMS_TO_TICKS(20) : 1);
        }
        portENTER_CRITICAL(&s_lock);
        s_capturing = false;
        portEXIT_CRITICAL(&s_lock);
        subghz_devices_stop_async_rx(device);
        subghz_devices_sleep(device);
        portENTER_CRITICAL(&s_lock);
        s_job.cancelled = s_stopping || s_cancel_requested;
        s_job.busy = false; /* Hardware is idle before another request can start. */
        s_cancel_requested = false;
        portEXIT_CRITICAL(&s_lock);
    }
    xSemaphoreGive(s_exited);
    vTaskDelete(NULL);
}

bool wlan_rpc_jobs_init(void) {
    if(s_requests) return false;
    portENTER_CRITICAL(&s_lock);
    memset(&s_job, 0, sizeof(s_job));
    s_stopping = false;
    s_cancel_requested = false;
    s_capturing = false;
    portEXIT_CRITICAL(&s_lock);
    s_requests = xQueueCreate(1, sizeof(uint8_t));
    s_exited = xSemaphoreCreateBinary();
    if(s_requests && s_exited &&
       xTaskCreate(worker, "wlan_rpc_rx", 4096, NULL, 5, NULL) == pdPASS) return true;
    if(s_requests) vQueueDelete(s_requests);
    if(s_exited) vSemaphoreDelete(s_exited);
    s_requests = NULL;
    s_exited = NULL;
    return false;
}

void wlan_rpc_jobs_stop(void) {
    if(!s_requests) return;
    portENTER_CRITICAL(&s_lock);
    s_stopping = true;
    portEXIT_CRITICAL(&s_lock);
    uint8_t request = 0;
    xQueueSend(s_requests, &request, portMAX_DELAY);
    xSemaphoreTake(s_exited, portMAX_DELAY);
    vQueueDelete(s_requests);
    vSemaphoreDelete(s_exited);
    s_requests = NULL;
    s_exited = NULL;
}

uint32_t wlan_rpc_jobs_start(uint32_t frequency_hz, uint32_t duration_ms) {
    if(!s_requests || !wlan_rpc_jobs_frequency_valid(frequency_hz) ||
       duration_ms < 100 || duration_ms > WLAN_RPC_MAX_DURATION_MS) return 0;
    portENTER_CRITICAL(&s_lock);
    if(s_job.busy || s_stopping || s_next_id == UINT32_MAX) {
        portEXIT_CRITICAL(&s_lock);
        return 0;
    }
    s_job = (WlanRpcJob){
        .id = ++s_next_id,
        .frequency_hz = frequency_hz,
        .duration_ms = duration_ms,
        .busy = true,
    };
    s_cancel_requested = false;
    uint32_t id = s_job.id;
    portEXIT_CRITICAL(&s_lock);
    uint8_t request = 1;
    /* Idle implies the previous request has been consumed. */
    if(xQueueSend(s_requests, &request, 0) != pdTRUE) {
        portENTER_CRITICAL(&s_lock);
        s_job.busy = false;
        s_job.cancelled = true;
        portEXIT_CRITICAL(&s_lock);
        return 0;
    }
    return id;
}

bool wlan_rpc_jobs_cancel(uint32_t id) {
    if(!s_requests || id == 0) return false;
    portENTER_CRITICAL(&s_lock);
    bool accepted = s_job.busy && s_job.id == id && !s_stopping;
    if(accepted) s_cancel_requested = true;
    portEXIT_CRITICAL(&s_lock);
    return accepted;
}

WlanRpcJob wlan_rpc_jobs_snapshot(void) {
    portENTER_CRITICAL(&s_lock);
    WlanRpcJob job = s_job;
    portEXIT_CRITICAL(&s_lock);
    return job;
}
