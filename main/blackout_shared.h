#ifndef BLACKOUT_SHARED_H
#define BLACKOUT_SHARED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_wifi.h"

// Dependencies of the standalone ProjectZero Blackout task. The task itself
// remains in blackout.c; these declarations only expose its original helpers.
#define TAG "BuddyBlackout"
#define MY_LOG_INFO(tag, format, ...) ESP_LOGI(tag, format, ##__VA_ARGS__)
#define MAX_AP_CNT 64
#define MAX_TARGET_BSSIDS 50
#define FAST_SCAN_MIN_TIME 100
#define FAST_SCAN_MAX_TIME 300

typedef struct {
    uint8_t bssid[6];
    char ssid[33];
    uint8_t channel;
    wifi_auth_mode_t authmode;
    uint32_t last_seen;
    bool active;
} target_bssid_t;

#ifdef __cplusplus
extern "C" {
#endif

extern wifi_ap_record_t g_scan_results[MAX_AP_CNT];
extern uint16_t g_scan_count;
extern volatile bool g_scan_in_progress;
extern volatile bool g_scan_done;
extern int g_selected_indices[MAX_AP_CNT];
extern int g_selected_count;
extern target_bssid_t target_bssids[MAX_TARGET_BSSIDS];
extern int target_bssid_count;
extern volatile bool operation_stop_requested;

int get_scan_timeout_iterations(void);
esp_err_t start_background_scan(uint32_t min_time, uint32_t max_time);
void blackout_shared_release(void);
void save_target_bssids(void);
bool ensure_wifi_mode(void);
void log_memory_info(const char *context);
esp_err_t led_set_color(uint8_t red, uint8_t green, uint8_t blue);
esp_err_t led_clear(void);
esp_err_t led_set_idle(void);
void wsl_bypasser_send_deauth_frame_multiple_aps(wifi_ap_record_t *ap_records,
                                                  size_t count);

#ifdef __cplusplus
}
#endif

#endif // BLACKOUT_SHARED_H
