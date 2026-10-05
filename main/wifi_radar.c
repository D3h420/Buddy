#include "wifi_radar.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "blackout_shared.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"

#define RADAR_TAG "WiFiRadar"
#define SCAN_INTERVAL_US 10000000LL
#define STOP_EVENT_WAIT_US 250000LL

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static wifi_radar_snapshot_t s_snapshot;
static esp_event_handler_instance_t s_scan_handler;
static bool s_handler_registered;
static bool s_active;
static bool s_scan_in_progress;
static bool s_scan_done;
static bool s_stop_waiting;
static bool s_rescan_requested;
static uint8_t s_scan_status;
static int64_t s_last_scan_at_us;

static void radar_scan_done(void *arg, esp_event_base_t base,
                            int32_t event_id, void *event_data)
{
    (void)arg;
    if (base != WIFI_EVENT || event_id != WIFI_EVENT_SCAN_DONE) {
        return;
    }

    const wifi_event_sta_scan_done_t *event =
        (const wifi_event_sta_scan_done_t *)event_data;
    portENTER_CRITICAL(&s_lock);
    if (s_stop_waiting) {
        s_stop_waiting = false;
    } else if (s_active && s_scan_in_progress) {
        s_scan_status = event == NULL ? 1 : event->status;
        s_scan_done = true;
    }
    portEXIT_CRITICAL(&s_lock);
}

static void set_scan_error(esp_err_t err)
{
    ESP_LOGW(RADAR_TAG, "Wi-Fi scan failed: %s", esp_err_to_name(err));
    portENTER_CRITICAL(&s_lock);
    s_scan_in_progress = false;
    s_scan_done = false;
    s_snapshot.count = 0;
    s_snapshot.scanning = false;
    s_snapshot.error = true;
    ++s_snapshot.generation;
    portEXIT_CRITICAL(&s_lock);
}

static void begin_scan(int64_t now_us)
{
    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = {
            .min = 0,
            .max = 120,
        },
    };

    portENTER_CRITICAL(&s_lock);
    s_last_scan_at_us = now_us;
    s_rescan_requested = false;
    s_scan_done = false;
    s_scan_in_progress = true;
    s_snapshot.scanning = true;
    s_snapshot.error = false;
    ++s_snapshot.generation;
    portEXIT_CRITICAL(&s_lock);

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, false);
    if (err != ESP_OK) {
        set_scan_error(err);
    }
}

static void collect_scan_results(uint8_t status)
{
    wifi_radar_snapshot_t next = {0};
    esp_err_t err = ESP_OK;

    if (status != 0) {
        esp_wifi_clear_ap_list();
        err = ESP_FAIL;
    } else {
        uint16_t ap_count = 0;
        err = esp_wifi_scan_get_ap_num(&ap_count);
        if (err == ESP_OK && ap_count != 0) {
            wifi_ap_record_t *records = calloc(ap_count, sizeof(*records));
            if (records == NULL) {
                err = ESP_ERR_NO_MEM;
            } else {
                uint16_t returned = ap_count;
                err = esp_wifi_scan_get_ap_records(&returned, records);
                if (err == ESP_OK) {
                    // ESP-IDF returns strongest RSSI first. Retain the first
                    // six 2.4 GHz APs, as in the original ESP8266 scanner.
                    for (uint16_t i = 0;
                         i < returned && next.count < WIFI_RADAR_MAX_ENTRIES;
                         ++i) {
                        if (records[i].primary < 1 || records[i].primary > 14) {
                            continue;
                        }
                        wifi_radar_entry_t *entry = &next.entries[next.count++];
                        memcpy(entry->ssid, records[i].ssid, 32);
                        entry->ssid[32] = '\0';
                        entry->rssi = records[i].rssi;
                        entry->distance_m = (float)pow(
                            10.0,
                            (27.55 - 20.0 * log10(2400.0)
                             + abs((int)entry->rssi)) / 20.0);
                    }

                    for (uint8_t i = 1; i < next.count; ++i) {
                        wifi_radar_entry_t entry = next.entries[i];
                        uint8_t j = i;
                        while (j > 0 &&
                               entry.distance_m < next.entries[j - 1].distance_m) {
                            next.entries[j] = next.entries[j - 1];
                            --j;
                        }
                        next.entries[j] = entry;
                    }
                }
                free(records);
            }
        }
        if (err != ESP_OK || ap_count == 0) {
            // get_ap_records() normally releases the driver's scan list;
            // clear it here when allocation or record retrieval failed.
            esp_wifi_clear_ap_list();
        }
    }

    if (err != ESP_OK) {
        ESP_LOGW(RADAR_TAG, "Reading scan results failed: %s",
                 esp_err_to_name(err));
    }

    portENTER_CRITICAL(&s_lock);
    memcpy(s_snapshot.entries, next.entries, sizeof(next.entries));
    s_snapshot.count = next.count;
    s_snapshot.scanning = false;
    s_snapshot.error = err != ESP_OK;
    ++s_snapshot.generation;
    portEXIT_CRITICAL(&s_lock);
}

bool wifi_radar_start(void)
{
    if (s_active) {
        return true;
    }
    if (!ensure_wifi_mode()) {
        return false;
    }

    esp_err_t err = esp_event_handler_instance_register(
        WIFI_EVENT, WIFI_EVENT_SCAN_DONE, radar_scan_done, NULL,
        &s_scan_handler);
    if (err != ESP_OK) {
        ESP_LOGE(RADAR_TAG, "Registering scan event failed: %s",
                 esp_err_to_name(err));
        return false;
    }
    s_handler_registered = true;

    portENTER_CRITICAL(&s_lock);
    s_active = true;
    s_scan_in_progress = false;
    s_scan_done = false;
    s_stop_waiting = false;
    s_rescan_requested = true;
    s_last_scan_at_us = 0;
    memset(s_snapshot.entries, 0, sizeof(s_snapshot.entries));
    s_snapshot.count = 0;
    s_snapshot.scanning = false;
    s_snapshot.error = false;
    ++s_snapshot.generation;
    portEXIT_CRITICAL(&s_lock);

    wifi_radar_update();
    return true;
}

void wifi_radar_stop(void)
{
    bool scan_was_active;
    bool scan_done;
    portENTER_CRITICAL(&s_lock);
    scan_was_active = s_scan_in_progress;
    scan_done = s_scan_done;
    const bool should_stop = scan_was_active && !scan_done;
    s_stop_waiting = should_stop;
    s_active = false;
    s_scan_in_progress = false;
    s_scan_done = false;
    s_rescan_requested = false;
    s_snapshot.scanning = false;
    ++s_snapshot.generation;
    portEXIT_CRITICAL(&s_lock);

    if (should_stop) {
        esp_wifi_scan_stop();
        const int64_t deadline_us = esp_timer_get_time() + STOP_EVENT_WAIT_US;
        while (esp_timer_get_time() < deadline_us) {
            portENTER_CRITICAL(&s_lock);
            bool waiting = s_stop_waiting;
            portEXIT_CRITICAL(&s_lock);
            if (!waiting) {
                break;
            }
            vTaskDelay(1);
        }
        portENTER_CRITICAL(&s_lock);
        bool timed_out = s_stop_waiting;
        s_stop_waiting = false;
        portEXIT_CRITICAL(&s_lock);
        if (timed_out) {
            ESP_LOGW(RADAR_TAG, "Timed out waiting for scan stop event");
        }
    }
    if (scan_was_active || scan_done) {
        esp_wifi_clear_ap_list();
    }
    if (s_handler_registered) {
        esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_SCAN_DONE,
                                              s_scan_handler);
        s_handler_registered = false;
    }
}

void wifi_radar_update(void)
{
    bool active;
    bool scanning;
    bool done;
    bool force;
    uint8_t status;
    int64_t last_scan_at_us;

    portENTER_CRITICAL(&s_lock);
    active = s_active;
    scanning = s_scan_in_progress;
    done = s_scan_done;
    status = s_scan_status;
    force = s_rescan_requested;
    last_scan_at_us = s_last_scan_at_us;
    if (done) {
        s_scan_done = false;
        s_scan_in_progress = false;
    }
    portEXIT_CRITICAL(&s_lock);

    if (!active) {
        return;
    }
    if (done) {
        collect_scan_results(status);
        scanning = false;
    }

    int64_t now_us = esp_timer_get_time();
    if (scanning) {
        return;
    }

    if (force || now_us - last_scan_at_us >= SCAN_INTERVAL_US) {
        begin_scan(now_us);
    }
}

bool wifi_radar_copy_snapshot(wifi_radar_snapshot_t *out,
                              uint32_t after_generation)
{
    if (out == NULL) {
        return false;
    }

    bool changed;
    portENTER_CRITICAL(&s_lock);
    changed = s_snapshot.generation != after_generation;
    if (changed) {
        *out = s_snapshot;
    }
    portEXIT_CRITICAL(&s_lock);
    return changed;
}

void wifi_radar_rescan(void)
{
    portENTER_CRITICAL(&s_lock);
    if (s_active) {
        s_rescan_requested = true;
    }
    portEXIT_CRITICAL(&s_lock);
}
