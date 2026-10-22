// main/app_wifi.c —— ESP32-C3 STA，2.4 GHz。凭证在 menuconfig 里填，
// 不落 NVS、不生成二维码：这是个人小设备，少一层配网状态机就少一类黑屏。
// 所有初始化步骤均返回错误码而非 ESP_ERROR_CHECK abort()，任何异常都不会引发无限重启屏闪。
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
static bool s_netif_inited;
static bool s_started;
static volatile bool s_connecting;
static int s_retry;
static unsigned s_last_reason;
static esp_err_t s_init_err;
static char s_ip_str[20];

static void ensure_backup_dns(void)
{
    if (!s_netif) return;

    esp_netif_dns_info_t dns = { 0 };
    if (esp_netif_get_dns_info(s_netif, ESP_NETIF_DNS_BACKUP, &dns) != ESP_OK ||
        dns.ip.u_addr.ip4.addr == 0) {
        esp_netif_dns_info_t ali = { .ip = ESP_IP4ADDR_INIT(223, 5, 5, 5) };
        (void)esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_BACKUP, &ali);
    }
    memset(&dns, 0, sizeof dns);
    if (esp_netif_get_dns_info(s_netif, ESP_NETIF_DNS_FALLBACK, &dns) != ESP_OK ||
        dns.ip.u_addr.ip4.addr == 0) {
        esp_netif_dns_info_t cf = { .ip = ESP_IP4ADDR_INIT(1, 1, 1, 1) };
        (void)esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_FALLBACK, &cf);
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        xEventGroupSetBits(s_events, WIFI_BIT_STARTED);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *disc =
            (const wifi_event_sta_disconnected_t *)data;
        s_last_reason = disc ? (unsigned)disc->reason : 0u;
        s_ip_str[0] = 0;
        xEventGroupClearBits(s_events, WIFI_BIT_CONNECTED);
        if (!s_connecting) return;
        if (s_retry < WIFI_MAX_RETRY) {
            s_retry++;
            ESP_LOGW(TAG, "连接断开(reason=%u)，重试 %d/%d",
                     s_last_reason, s_retry, WIFI_MAX_RETRY);
            esp_wifi_connect();
        } else {
            s_connecting = false;
            ESP_LOGE(TAG, "Wi-Fi 重试耗尽(reason=%u)", s_last_reason);
            xEventGroupSetBits(s_events, WIFI_BIT_FAILED);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
        snprintf(s_ip_str, sizeof s_ip_str, IPSTR, IP2STR(&ev->ip_info.ip));
        ensure_backup_dns();
        ESP_LOGI(TAG, "已获取 IP: %s", s_ip_str);
        s_retry = 0;
        s_last_reason = 0;
        s_connecting = false;
        xEventGroupClearBits(s_events, WIFI_BIT_FAILED);
        xEventGroupSetBits(s_events, WIFI_BIT_CONNECTED);
    }
}

static esp_err_t stack_start(void)
{
    if (s_started) return ESP_OK;
    if (s_init_err != ESP_OK) return s_init_err;

    s_events = xEventGroupCreate();
    if (!s_events) {
        s_init_err = ESP_ERR_NO_MEM;
        return s_init_err;
    }

    if (!s_netif_inited) {
        esp_err_t e = esp_netif_init();
        if (e != ESP_OK) { s_init_err = e; return e; }
        e = esp_event_loop_create_default();
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) { s_init_err = e; return e; }
        s_netif_inited = true;
    }

    s_netif = esp_netif_create_default_wifi_sta();
    if (!s_netif) { s_init_err = ESP_ERR_NO_MEM; return s_init_err; }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t e = esp_wifi_init(&init_cfg);
    if (e != ESP_OK) { s_init_err = e; return e; }

    e = esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL);
    if (e != ESP_OK) { s_init_err = e; return e; }

    e = esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL, NULL);
    if (e != ESP_OK) { s_init_err = e; return e; }

    wifi_config_t cfg = { 0 };
    snprintf((char *)cfg.sta.ssid, sizeof cfg.sta.ssid, "%s", CONFIG_APP_WIFI_SSID);
    snprintf((char *)cfg.sta.password, sizeof cfg.sta.password, "%s", CONFIG_APP_WIFI_PASSWORD);
    cfg.sta.threshold.authmode =
        CONFIG_APP_WIFI_PASSWORD[0] ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    // 必须先切到 RAM 存储，再设 mode 与 config；否则密码会被默认写进 NVS。
    e = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (e != ESP_OK) { s_init_err = e; return e; }
    e = esp_wifi_set_mode(WIFI_MODE_STA);
    if (e != ESP_OK) { s_init_err = e; return e; }
    e = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (e != ESP_OK) { s_init_err = e; return e; }
    e = esp_wifi_start();
    if (e != ESP_OK) { s_init_err = e; return e; }

    // 关闭 modem 省电，避免 ESP32-C3 在 DNS / TLS 握手包突发时丢包超时。
    (void)esp_wifi_set_ps(WIFI_PS_NONE);

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

void app_wifi_describe(char *out, size_t cap)
{
    if (!out || cap == 0) return;
    if (s_init_err != ESP_OK) {
        snprintf(out, cap, "Wi-Fi: 初始化错误(0x%X)", (unsigned)s_init_err);
        return;
    }
    if (app_wifi_is_connected()) {
        snprintf(out, cap, "Wi-Fi: %s (%s)",
                 CONFIG_APP_WIFI_SSID, s_ip_str[0] ? s_ip_str : "已连接");
        return;
    }
    const char *why = "未连接";
    if (s_last_reason == 201) why = "未找到热点";
    else if (s_last_reason == 202 || s_last_reason == 15 || s_last_reason == 205) {
        why = "密码或认证失败";
    } else if (s_last_reason != 0) {
        snprintf(out, cap, "Wi-Fi: %s (断开#%u)", CONFIG_APP_WIFI_SSID, s_last_reason);
        return;
    }
    snprintf(out, cap, "Wi-Fi: %s (%s)", CONFIG_APP_WIFI_SSID, why);
}

void app_wifi_stop(void)
{
    if (!s_started) return;
    s_connecting = false;
    s_ip_str[0] = 0;
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
