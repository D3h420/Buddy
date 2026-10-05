#ifndef WIFI_RADAR_H
#define WIFI_RADAR_H

#include <stdbool.h>
#include <stdint.h>

#define WIFI_RADAR_MAX_ENTRIES 6

typedef struct {
    char ssid[33];
    int8_t rssi;
    float distance_m;
} wifi_radar_entry_t;

typedef struct {
    wifi_radar_entry_t entries[WIFI_RADAR_MAX_ENTRIES];
    uint8_t count;
    bool scanning;
    bool error;
    uint32_t generation;
} wifi_radar_snapshot_t;

#ifdef __cplusplus
extern "C" {
#endif

// Call these functions from the UI task. Scans run asynchronously in the Wi-Fi
// driver; update() collects completed results without waiting for a scan.
bool wifi_radar_start(void);
void wifi_radar_stop(void);
void wifi_radar_update(void);
bool wifi_radar_copy_snapshot(wifi_radar_snapshot_t *out,
                              uint32_t after_generation);
void wifi_radar_rescan(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_RADAR_H
