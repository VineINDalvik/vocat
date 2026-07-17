// SPDX-FileCopyrightText: 2025 Mayfair Inc.
// SPDX-License-Identifier: Apache-2.0

#include "wifi_init.h"
#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "sdkconfig.h"

static const char *TAG = "wifi_init";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define MAX_RETRY          5

#define NVS_NAMESPACE  "wifi_creds"
#define NVS_KEY_SSID    "ssid"
#define NVS_KEY_PASS    "password"
#define PREFERRED_WIFI_SSID "Vine’s iPhone"

static EventGroupHandle_t s_wifi_event_group = NULL;
static int                s_retry_count      = 0;
static int                s_max_retry        = MAX_RETRY;
static bool               s_handlers_registered = false;
static bool               s_connecting       = false;  // false = ignore STA_START

// Callback for WiFi status changes (connection success/failure)
static wifi_status_cb_t s_status_cb = NULL;

static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "STA_START → connecting...");
        if (s_connecting) esp_wifi_connect();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGE(TAG, "DISCONNECTED reason=%d ssid=\"%s\"", d->reason, (char *)d->ssid);
        if (s_retry_count < s_max_retry) {
            esp_wifi_connect();
            s_retry_count++;
            ESP_LOGW(TAG, "Retry %d/%d", s_retry_count, s_max_retry);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            if (s_status_cb) s_status_cb(WIFI_STATUS_FAILED);
        }
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&e->ip_info.ip));
        // The iPhone hotspot DNS intermittently puts an unreachable Cloudflare
        // edge first. Use a stable resolver whose current answer orders the
        // reachable edge first; ESP-TLS only attempts the first DNS address.
        esp_netif_dns_info_t dns = {0};
        dns.ip.type = IPADDR_TYPE_V4;
        dns.ip.u_addr.ip4.addr = ipaddr_addr("1.1.1.1");
        esp_err_t dns_err = esp_netif_set_dns_info(e->esp_netif, ESP_NETIF_DNS_MAIN, &dns);
        ESP_LOGI(TAG, "DNS set to 1.1.1.1: %s", esp_err_to_name(dns_err));
        s_retry_count = 0;
        esp_netif_set_default_netif(e->esp_netif);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        if (s_status_cb) s_status_cb(WIFI_STATUS_CONNECTED);
    }
}

static void register_handlers(void)
{
    if (s_handlers_registered) return;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));
    s_handlers_registered = true;
}

void wifi_register_status_cb(wifi_status_cb_t cb)
{
    s_status_cb = cb;
}

void wifi_stop_connecting(void)
{
    s_connecting = false;  // Prevent STA_START handler from auto-connecting
    s_retry_count = MAX_RETRY;  // Prevent DISCONNECT handler from retrying
}

esp_err_t wifi_load_credentials(char *ssid, size_t ssid_len,
                                 char *password, size_t pass_len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    err = nvs_get_str(h, NVS_KEY_SSID, ssid, &ssid_len);
    if (err != ESP_OK) { nvs_close(h); return err; }

    err = nvs_get_str(h, NVS_KEY_PASS, password, &pass_len);
    nvs_close(h);
    return err;
}

esp_err_t wifi_save_credentials(const char *ssid, const char *password)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_str(h, NVS_KEY_SSID, ssid);
    if (err != ESP_OK) { nvs_close(h); return err; }

    err = nvs_set_str(h, NVS_KEY_PASS, password);
    if (err != ESP_OK) { nvs_close(h); return err; }

    err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "saved WiFi creds: SSID=\"%s\"", ssid);
    return err;
}

// Hardcoded fallback networks (tried if primary fails)
typedef struct { const char *ssid; const char *pass; } wifi_fallback_t;
static const wifi_fallback_t s_fallbacks[] = {
    // No fallbacks currently — add WPA2-only networks here if needed
};
#define NUM_FALLBACKS (sizeof(s_fallbacks) / sizeof(s_fallbacks[0]))
#define MAX_RETRY_FAST  3   // Fewer retries for fallback networks

esp_err_t wifi_init_sta(void)
{
    char ssid[33] = {0};
    char password[65] = {0};

    // Vine's iPhone is the product's preferred boot network.  An older saved
    // lab SSID must not prevent the device from using the configured hotspot.
    if (wifi_load_credentials(ssid, sizeof(ssid), password, sizeof(password)) == ESP_OK) {
        if (strcmp(ssid, PREFERRED_WIFI_SSID) != 0 ||
            strcmp(password, CONFIG_MEETING_WIFI_PASSWORD) != 0) {
            ESP_LOGW(TAG, "restoring configured credentials for preferred WiFi \"%s\"",
                     CONFIG_MEETING_WIFI_SSID);
            strlcpy(ssid, CONFIG_MEETING_WIFI_SSID, sizeof(ssid));
            strlcpy(password, CONFIG_MEETING_WIFI_PASSWORD, sizeof(password));
            ESP_ERROR_CHECK(wifi_save_credentials(ssid, password));
        }
        ESP_LOGI(TAG, "auto-connecting saved WiFi: \"%s\" (preferred)", ssid);
    } else {
        // Fall back to Kconfig defaults
        ESP_LOGI(TAG, "no saved WiFi, trying Kconfig default \"%s\"", CONFIG_MEETING_WIFI_SSID);
        strlcpy(ssid, CONFIG_MEETING_WIFI_SSID, sizeof(ssid));
        strlcpy(password, CONFIG_MEETING_WIFI_PASSWORD, sizeof(password));
    }

    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register persistent handlers (never unregistered — needed for reconnect)
    register_handlers();

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    // Modem sleep can stall the first TLS handshake on phone hotspots. This
    // must be configured at runtime; CONFIG_ESP_WIFI_PS_NONE is obsolete.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    // Try the primary credentials, then fallback networks
    // Primary: full retries; fallbacks: fewer retries for faster boot
    for (int attempt = 0; ; attempt++) {
        int max_retry = (attempt == 0) ? MAX_RETRY : MAX_RETRY_FAST;
        wifi_config_t wifi_cfg = {};
        strlcpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid));
        strlcpy((char *)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password));
        wifi_cfg.sta.pmf_cfg.capable = true;
        wifi_cfg.sta.pmf_cfg.required = false;
        wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
        s_retry_count = 0;
        s_max_retry = max_retry;  // dynamic limit for event handler
        s_connecting = true;
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

        ESP_ERROR_CHECK(esp_wifi_start());
        ESP_LOGI(TAG, "Connecting to \"%s\" (attempt %d)...", ssid, attempt + 1);

        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                               WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                               pdFALSE, pdFALSE,
                                               pdMS_TO_TICKS(30000));

        if (bits & WIFI_CONNECTED_BIT) {
            // Keep the successful association alive. Stopping and immediately
            // restarting here produced a needless disconnect and made the
            // meeting preflight race the DHCP/TLS reconnect on phone hotspots.
            ESP_LOGI(TAG, "WiFi ready; keeping current connection active");
            return ESP_OK;
        }

        esp_wifi_stop();

        ESP_LOGW(TAG, "\"%s\" failed, trying next network...", ssid);

        // Pick next fallback (only if we haven't tried them all)
        if (attempt < (int)NUM_FALLBACKS) {
            strlcpy(ssid, s_fallbacks[attempt].ssid, sizeof(ssid));
            strlcpy(password, s_fallbacks[attempt].pass, sizeof(password));
        } else {
            // All attempts exhausted — scan and log APs for debugging
            s_connecting = false;
            s_retry_count = MAX_RETRY;
            esp_wifi_disconnect();
            esp_wifi_stop();
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_wifi_start();
            vTaskDelay(pdMS_TO_TICKS(1000));
            wifi_scan_config_t scan_cfg = { .ssid = NULL, .bssid = NULL, .channel = 0 };
            esp_err_t scan_err = esp_wifi_scan_start(&scan_cfg, true);
            ESP_LOGI(TAG, "diagnostic scan result: %s", esp_err_to_name(scan_err));
            if (scan_err == ESP_OK) {
                uint16_t num = 0;
                esp_wifi_scan_get_ap_num(&num);
                wifi_ap_record_t aps[10];
                if (num > 10) num = 10;
                esp_wifi_scan_get_ap_records(&num, aps);
                ESP_LOGI(TAG, "found %d APs:", num);
                for (int i = 0; i < num; i++) {
                    ESP_LOGI(TAG, "  AP[%d]: \"%s\" rssi=%d auth=%d ch=%d",
                             i, aps[i].ssid, aps[i].rssi, aps[i].authmode, aps[i].primary);
                }
            }
            esp_wifi_start();
            ESP_LOGE(TAG, "WiFi connection failed for all networks");
            return ESP_FAIL;
        }
    }
}

// Background task for WiFi reconnect (offloaded from LVGL task)
static void reconnect_task(void *arg)
{
    char *ssid = (char *)((void **)arg)[0];
    char *pass = (char *)((void **)arg)[1];
    free(arg);

    wifi_config_t cfg = {};
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password));
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_LOGI(TAG, "reconnecting to \"%s\"...", ssid);

    // Stop WiFi first to prevent DISCONNECTED event handler from
    // calling esp_wifi_connect() with OLD credentials before set_config
    esp_wifi_stop();

    s_connecting = true;
    s_retry_count = 0;
    if (s_wifi_event_group) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    } else {
        s_wifi_event_group = xEventGroupCreate();
    }

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_set_config failed: %s", esp_err_to_name(err));
        if (s_status_cb) s_status_cb(WIFI_STATUS_FAILED);
        free(ssid); free(pass);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "wifi_set_config OK, starting WiFi...");

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_start failed: %s", esp_err_to_name(err));
        if (s_status_cb) s_status_cb(WIFI_STATUS_FAILED);
        free(ssid); free(pass);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "wifi started, waiting for connection...");

    // Wait for result (handlers will update event group)
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(30000));

    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGE(TAG, "reconnect failed (bits=0x%x)", (unsigned)bits);
        if (s_status_cb) s_status_cb(WIFI_STATUS_FAILED);
    } else {
        ESP_LOGI(TAG, "reconnect succeeded!");
    }

    free(ssid);
    free(pass);
    vTaskDelete(NULL);
}

esp_err_t wifi_reconnect(const char *ssid, const char *password)
{
    // Deep-copy strings for the background task
    char *ssid_copy = strdup(ssid);
    char *pass_copy = strdup(password ? password : "");
    if (!ssid_copy || !pass_copy) {
        free(ssid_copy); free(pass_copy);
        return ESP_ERR_NO_MEM;
    }

    void **args = malloc(2 * sizeof(void *));
    if (!args) { free(ssid_copy); free(pass_copy); return ESP_ERR_NO_MEM; }
    args[0] = ssid_copy;
    args[1] = pass_copy;

    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
        reconnect_task, "wifi_reconn", 6 * 1024, args, 5,
        NULL, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ret != pdPASS) {
        free(args); free(ssid_copy); free(pass_copy);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
