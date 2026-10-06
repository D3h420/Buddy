#ifndef BUDDY_BLE_SCAN_H
#define BUDDY_BLE_SCAN_H

#include <stdbool.h>
#include <stdint.h>

#define BUDDY_BLE_MAX_DISPLAY_DEVICES 4
#define BUDDY_BLE_DEVICE_NAME_LENGTH 24

typedef enum {
    BUDDY_BLE_PHONE = 0,
    BUDDY_BLE_AUDIO,
    BUDDY_BLE_WEARABLE,
    BUDDY_BLE_COMPUTER,
    BUDDY_BLE_OTHER,
} buddy_ble_category_t;

typedef enum {
    BUDDY_BLE_STOPPED = 0,
    BUDDY_BLE_STARTING,
    BUDDY_BLE_SCANNING,
    BUDDY_BLE_ERROR,
} buddy_ble_state_t;

typedef struct {
    char name[BUDDY_BLE_DEVICE_NAME_LENGTH];
    buddy_ble_category_t category;
    int8_t rssi;
} buddy_ble_device_t;

typedef struct {
    uint32_t generation;
    buddy_ble_state_t state;
    int error_code;
    uint8_t seconds_remaining;
    uint16_t total;
    uint16_t phones;
    uint16_t audio;
    uint16_t wearables;
    uint16_t computers;
    uint16_t other;
    bool truncated;
    uint8_t device_count;
    buddy_ble_device_t devices[BUDDY_BLE_MAX_DISPLAY_DEVICES];
} buddy_ble_snapshot_t;

#ifdef __cplusplus
extern "C" {
#endif

// The UI task owns this API. NimBLE scans asynchronously; update() never waits
// for a scan. Counts refer to distinct advertiser addresses in the current
// 10-second cycle, not verified physical devices.
bool buddy_ble_start(void);
void buddy_ble_stop(void);
void buddy_ble_update(void);
bool buddy_ble_copy_snapshot(buddy_ble_snapshot_t *out,
                             uint32_t after_generation);

#ifdef __cplusplus
}
#endif

#endif // BUDDY_BLE_SCAN_H
