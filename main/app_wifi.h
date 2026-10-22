// main/app_wifi.h —— 只做 STA 连接，凭证来自 Kconfig（menuconfig → 005 Signal Log）。
#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

esp_err_t app_wifi_start(void);                       // 幂等
esp_err_t app_wifi_connect(uint32_t timeout_ms);      // 阻塞等待拿到 IP
bool app_wifi_is_connected(void);
void app_wifi_stop(void);
