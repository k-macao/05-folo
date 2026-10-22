// main/app_wifi.c —— ESP32-C3 STA，2.4 GHz。凭证在 menuconfig 里填，
// 不落 NVS、不生成二维码：这是个人小设备，少一层配网状态机就少一类黑屏。
#include "app_wifi.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "sdkconfig.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "app_wifi";

#define WIFI_BIT_CONNECTED BIT0
#define WIFI_BIT_FAILED    BIT1
#define WIFI_BIT_STARTED   BIT2
#define WIFI_MAX_RETRY     5

static EventGroupHandle_t s_events;
static esp_netif_t *s_netif;
static bool s_started;
static volatile bool s_connecting;
static int s_retry;

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        xEventGroupSetBits(s_events, WIFI_BIT_STARTED);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *disc =
            (const wifi_event_sta_disconnected_t *)data;
        xEventGroupClearBits(s_events, WIFI_BIT_CONNECTED);
        if (!s_connecting) return;
        if (s_retry < WIFI_MAX_RETRY) {
            s_retry++;
            ESP_LOGW(TAG, "连接断开(reason=%u)，重试 %d/%d",
                     disc ? (unsigned)disc->reason : 0u, s_retry, WIFI_MAX_RETRY);
            esp_wifi_connect();
        } else {
            s_connecting = false;
            ESP_LOGE(TAG, "Wi-Fi 重试耗尽(reason=%u)",
                     disc ? (unsigned)disc->reason : 0u);
            xEventGroupSetBits(s_events, WIFI_BIT_FAILED);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "已获取 IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        s_retry = 0;
        s_connecting = false;
        xEventGroupClearBits(s_events, WIFI_BIT_FAILED);
        xEventGroupSetBits(s_events, WIFI_BIT_CONNECTED);
    }
}

static esp_err_t stack_start(void)
{
    if (s_started) return ESP_OK;

    s_events = xEventGroupCreate();
    if (!s_events) return ESP_ERR_NO_MEM;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();
    if (!s_netif) return ESP_ERR_NO_MEM;

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL, NULL));

    wifi_config_t cfg = { 0 };
    snprintf((char *)cfg.sta.ssid, sizeof cfg.sta.ssid, "%s", CONFIG_APP_WIFI_SSID);
    snprintf((char *)cfg.sta.password, sizeof cfg.sta.password, "%s", CONFIG_APP_WIFI_PASSWORD);
    cfg.sta.threshold.authmode =
        CONFIG_APP_WIFI_PASSWORD[0] ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    // 必须先切到 RAM 存储，再设 mode 与 config；否则 config 写入 NVS 后切 RAM 会把配置清空。
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_started = true;
    return ESP_OK;
}

esp_err_t app_wifi_start(void)
{
    return stack_start();
}

esp_err_t app_wifi_connect(uint32_t timeout_ms)
{
    esp_err_t err = stack_start();
    if (err != ESP_OK) return err;
    if (app_wifi_is_connected()) return ESP_OK;

    const EventBits_t started = xEventGroupWaitBits(
        s_events, WIFI_BIT_STARTED, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (!(started & WIFI_BIT_STARTED)) return ESP_ERR_TIMEOUT;

    xEventGroupClearBits(s_events, WIFI_BIT_FAILED);
    s_retry = 0;
    s_connecting = true;
    err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        s_connecting = false;
        ESP_LOGE(TAG, "esp_wifi_connect 失败: %s", esp_err_to_name(err));
        return err;
    }

    const EventBits_t bits = xEventGroupWaitBits(
        s_events, WIFI_BIT_CONNECTED | WIFI_BIT_FAILED, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(timeout_ms));

    if (bits & WIFI_BIT_CONNECTED) return ESP_OK;
    s_connecting = false;
    if (bits & WIFI_BIT_FAILED) return ESP_FAIL;
    ESP_LOGW(TAG, "连接 %s 超时", CONFIG_APP_WIFI_SSID);
    return ESP_ERR_TIMEOUT;
}

bool app_wifi_is_connected(void)
{
    if (!s_events) return false;
    return (xEventGroupGetBits(s_events) & WIFI_BIT_CONNECTED) != 0;
}

void app_wifi_stop(void)
{
    if (!s_started) return;
    s_connecting = false;
    esp_wifi_stop();
    esp_wifi_deinit();
    if (s_netif) {
        esp_netif_destroy(s_netif);
        s_netif = NULL;
    }
    if (s_events) {
        vEventGroupDelete(s_events);
        s_events = NULL;
    }
    s_started = false;
}
