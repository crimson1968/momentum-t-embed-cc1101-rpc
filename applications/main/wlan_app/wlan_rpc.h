#pragma once

#include <esp_http_server.h>

/* Register four exact routes. On failure, stop httpd then call wlan_rpc_stop(). */
esp_err_t wlan_rpc_register(httpd_handle_t server);
/* Call after httpd_stop(), before WiFi app exit / radio teardown. */
void wlan_rpc_stop(void);
