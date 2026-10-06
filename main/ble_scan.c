#include "ble_scan.h"

#include <ctype.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"

#define BLE_SCAN_TAG "BuddyBLE"
#define BLE_SCAN_CYCLE_MS 10000
#define BLE_SCAN_RESTART_US 600000LL
#define BLE_SCAN_RETRY_US 1000000LL
#define BLE_SCAN_PUBLISH_US 250000LL
#define BLE_SCAN_MAX_TRACKED 128

typedef struct {
    ble_addr_t address;
    char name[BUDDY_BLE_DEVICE_NAME_LENGTH];
    buddy_ble_category_t category;
    uint8_t evidence;
    int8_t rssi;
} tracked_advertiser_t;

typedef struct {
    char name[BUDDY_BLE_DEVICE_NAME_LENGTH];
    buddy_ble_category_t category;
    uint8_t evidence;
} advertisement_info_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static tracked_advertiser_t s_advertisers[BLE_SCAN_MAX_TRACKED];
static uint16_t s_advertiser_count;
static buddy_ble_snapshot_t s_snapshot;
static bool s_dirty;
static bool s_active;
static bool s_host_initialized;
static bool s_host_synced;
static bool s_scan_in_progress;
static bool s_waiting_for_cancel;
static uint8_t s_own_address_type;
static int64_t s_cycle_started_us;
static int64_t s_next_scan_due_us;
static int64_t s_last_publish_us;

static void set_error(int code)
{
    portENTER_CRITICAL(&s_lock);
    s_snapshot.state = BUDDY_BLE_ERROR;
    s_snapshot.error_code = code;
    s_snapshot.seconds_remaining = 0;
    ++s_snapshot.generation;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGW(BLE_SCAN_TAG, "BLE scan error: %d", code);
}

static bool has_text(const char *text, const char *needle)
{
    for (const char *start = text; *start != '\0'; ++start) {
        const char *a = start;
        const char *b = needle;
        while (*a != '\0' && *b != '\0' &&
               tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
            ++a;
            ++b;
        }
        if (*b == '\0') {
            return true;
        }
    }
    return false;
}

static bool has_numbered_model(const char *text, const char *prefix)
{
    for (const char *start = text; *start != '\0'; ++start) {
        const char *a = start;
        const char *b = prefix;
        while (*a != '\0' && *b != '\0' &&
               tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
            ++a;
            ++b;
        }
        if (*b == '\0' && isdigit((unsigned char)*a)) {
            return true;
        }
    }
    return false;
}

static void copy_advertised_name(char *out, const uint8_t *name, uint8_t length)
{
    size_t count = length;
    if (count >= BUDDY_BLE_DEVICE_NAME_LENGTH) {
        count = BUDDY_BLE_DEVICE_NAME_LENGTH - 1;
    }
    for (size_t i = 0; i < count; ++i) {
        // The display font is ASCII. Keep names safe and legible on screen.
        out[i] = name[i] >= 32 && name[i] <= 126 ? (char)name[i] : '?';
    }
    out[count] = '\0';
}

static buddy_ble_category_t category_from_name(const char *name)
{
    if (has_text(name, "airpods") || has_text(name, " buds") ||
        has_text(name, "earbuds") || has_text(name, "headphones") ||
        has_text(name, "headset") || has_text(name, "speaker") ||
        has_text(name, "soundbar") || has_text(name, "beats fit") ||
        has_text(name, "sony wf-") || has_text(name, "sony wh-")) {
        return BUDDY_BLE_AUDIO;
    }
    if (has_text(name, "watch") || has_text(name, "fitbit") ||
        has_text(name, "mi band") || has_text(name, "smart band") ||
        has_text(name, "oura ring")) {
        return BUDDY_BLE_WEARABLE;
    }
    if (has_text(name, "macbook") || has_text(name, "thinkpad") ||
        has_text(name, "chromebook") || has_text(name, "laptop") ||
        has_text(name, "surface pro") || has_text(name, "ipad") ||
        has_text(name, "pixel tablet") || has_text(name, "redmi pad")) {
        return BUDDY_BLE_COMPUTER;
    }
    if (has_text(name, "iphone") ||
        has_numbered_model(name, "pixel ") ||
        has_numbered_model(name, "galaxy s") ||
        has_numbered_model(name, "galaxy a") ||
        has_text(name, "galaxy note") ||
        has_numbered_model(name, "oneplus ") ||
        has_numbered_model(name, "redmi ") ||
        has_text(name, "redmi note")) {
        return BUDDY_BLE_PHONE;
    }
    return BUDDY_BLE_OTHER;
}

static buddy_ble_category_t category_from_appearance(uint16_t appearance)
{
    // Bluetooth SIG Appearance values encode a general device class in the
    // upper ten bits. Headsets are grouped with AUDIO, including wearable audio.
    if (appearance == 0x0086) { // Wearable computer (watch size)
        return BUDDY_BLE_WEARABLE;
    }
    switch (appearance & 0xFFC0u) {
    case 0x0040:
        return BUDDY_BLE_PHONE;
    case 0x0080:
        return BUDDY_BLE_COMPUTER;
    case 0x00C0:
    case 0x0340:
        return BUDDY_BLE_WEARABLE;
    case 0x0840:
    case 0x0940:
        return BUDDY_BLE_AUDIO;
    default:
        return BUDDY_BLE_OTHER;
    }
}

static bool has_service16(const struct ble_hs_adv_fields *fields,
                          uint16_t uuid)
{
    for (uint8_t i = 0; i < fields->num_uuids16; ++i) {
        if (fields->uuids16[i].value == uuid) {
            return true;
        }
    }
    if (fields->svc_data_uuid16_len >= 2 &&
        (uint16_t)(fields->svc_data_uuid16[0] |
                   ((uint16_t)fields->svc_data_uuid16[1] << 8)) == uuid) {
        return true;
    }
    return false;
}

static advertisement_info_t read_advertisement(const struct ble_gap_disc_desc *disc)
{
    advertisement_info_t info = {
        .category = BUDDY_BLE_OTHER,
    };
    struct ble_hs_adv_fields fields;
    if (ble_hs_adv_parse_fields(&fields, disc->data,
                                disc->length_data) != 0) {
        return info;
    }
    if (fields.name != NULL && fields.name_len > 0) {
        copy_advertised_name(info.name, fields.name, fields.name_len);
        info.category = category_from_name(info.name);
        if (info.category != BUDDY_BLE_OTHER) {
            info.evidence = 1;
        }
    }
    if (has_service16(&fields, 0x180D)) { // Heart Rate
        info.category = BUDDY_BLE_WEARABLE;
        info.evidence = 2;
    } else if (has_service16(&fields, 0x184E) || // Audio Stream Control
               has_service16(&fields, 0x1850)) { // Published Audio Capabilities
        info.category = BUDDY_BLE_AUDIO;
        info.evidence = 2;
    }
    if (fields.appearance_is_present) {
        buddy_ble_category_t category =
            category_from_appearance(fields.appearance);
        if (category != BUDDY_BLE_OTHER) {
            info.category = category;
            info.evidence = 3;
        }
    }
    // Manufacturer company IDs alone identify vendors, not device types.
    return info;
}

static void observe_advertiser(const struct ble_gap_disc_desc *disc)
{
    advertisement_info_t info = read_advertisement(disc);
    int8_t rssi = disc->rssi == 127 ? -127 : disc->rssi;

    portENTER_CRITICAL(&s_lock);
    if (!s_active || !s_scan_in_progress || s_waiting_for_cancel) {
        portEXIT_CRITICAL(&s_lock);
        return;
    }

    tracked_advertiser_t *entry = NULL;
    for (uint16_t i = 0; i < s_advertiser_count; ++i) {
        if (s_advertisers[i].address.type == disc->addr.type &&
            memcmp(s_advertisers[i].address.val, disc->addr.val,
                   sizeof(disc->addr.val)) == 0) {
            entry = &s_advertisers[i];
            break;
        }
    }
    if (entry == NULL) {
        if (s_advertiser_count == BLE_SCAN_MAX_TRACKED) {
            s_snapshot.truncated = true;
            s_dirty = true;
            portEXIT_CRITICAL(&s_lock);
            return;
        }
        entry = &s_advertisers[s_advertiser_count++];
        memset(entry, 0, sizeof(*entry));
        entry->address = disc->addr;
        entry->category = BUDDY_BLE_OTHER;
        entry->rssi = rssi;
        s_dirty = true;
    }
    if (info.name[0] != '\0' &&
        (entry->name[0] == '\0' || strlen(info.name) > strlen(entry->name))) {
        memcpy(entry->name, info.name, sizeof(entry->name));
        s_dirty = true;
    }
    if (info.evidence > entry->evidence) {
        entry->category = info.category;
        entry->evidence = info.evidence;
        s_dirty = true;
    }
    if (rssi > entry->rssi) {
        entry->rssi = rssi;
    }
    portEXIT_CRITICAL(&s_lock);
}

static int scan_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    if (event->type == BLE_GAP_EVENT_DISC) {
        observe_advertiser(&event->disc);
    } else if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        portENTER_CRITICAL(&s_lock);
        s_scan_in_progress = false;
        s_waiting_for_cancel = false;
        if (s_active) {
            s_next_scan_due_us = esp_timer_get_time() + BLE_SCAN_RESTART_US;
            s_snapshot.seconds_remaining = 0;
            s_dirty = true;
        }
        portEXIT_CRITICAL(&s_lock);
    }
    return 0;
}

static void on_host_reset(int reason)
{
    portENTER_CRITICAL(&s_lock);
    s_host_synced = false;
    s_scan_in_progress = false;
    s_waiting_for_cancel = false;
    s_next_scan_due_us = 0;
    if (s_active) {
        s_snapshot.state = BUDDY_BLE_STARTING;
        s_snapshot.error_code = reason;
        ++s_snapshot.generation;
    }
    portEXIT_CRITICAL(&s_lock);
}

static void on_host_sync(void)
{
    uint8_t own_address_type = 0;
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) {
        rc = ble_hs_id_infer_auto(0, &own_address_type);
    }
    if (rc != 0) {
        set_error(rc);
        return;
    }
    portENTER_CRITICAL(&s_lock);
    s_own_address_type = own_address_type;
    s_host_synced = true;
    s_next_scan_due_us = 0;
    portEXIT_CRITICAL(&s_lock);
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void publish_snapshot(int64_t now_us)
{
    portENTER_CRITICAL(&s_lock);
    if (!s_active || !s_host_synced) {
        portEXIT_CRITICAL(&s_lock);
        return;
    }

    uint8_t remaining = 0;
    if (s_scan_in_progress) {
        int64_t elapsed_us = now_us - s_cycle_started_us;
        if (elapsed_us < BLE_SCAN_CYCLE_MS * 1000LL) {
            remaining = (uint8_t)((BLE_SCAN_CYCLE_MS * 1000LL - elapsed_us +
                                   999999LL) / 1000000LL);
        }
    }
    if (!s_dirty && remaining == s_snapshot.seconds_remaining) {
        portEXIT_CRITICAL(&s_lock);
        return;
    }

    s_snapshot.total = s_advertiser_count;
    s_snapshot.phones = 0;
    s_snapshot.audio = 0;
    s_snapshot.wearables = 0;
    s_snapshot.computers = 0;
    s_snapshot.other = 0;
    s_snapshot.device_count = 0;
    memset(s_snapshot.devices, 0, sizeof(s_snapshot.devices));

    for (uint16_t i = 0; i < s_advertiser_count; ++i) {
        const tracked_advertiser_t *entry = &s_advertisers[i];
        switch (entry->category) {
        case BUDDY_BLE_PHONE:
            ++s_snapshot.phones;
            break;
        case BUDDY_BLE_AUDIO:
            ++s_snapshot.audio;
            break;
        case BUDDY_BLE_WEARABLE:
            ++s_snapshot.wearables;
            break;
        case BUDDY_BLE_COMPUTER:
            ++s_snapshot.computers;
            break;
        default:
            ++s_snapshot.other;
            break;
        }

        if (entry->name[0] == '\0') {
            continue;
        }
        uint8_t insert_at = 0;
        while (insert_at < s_snapshot.device_count &&
               s_snapshot.devices[insert_at].rssi >= entry->rssi) {
            ++insert_at;
        }
        if (insert_at >= BUDDY_BLE_MAX_DISPLAY_DEVICES) {
            continue;
        }
        if (s_snapshot.device_count < BUDDY_BLE_MAX_DISPLAY_DEVICES) {
            ++s_snapshot.device_count;
        }
        for (uint8_t j = s_snapshot.device_count - 1; j > insert_at; --j) {
            s_snapshot.devices[j] = s_snapshot.devices[j - 1];
        }
        buddy_ble_device_t *shown = &s_snapshot.devices[insert_at];
        memcpy(shown->name, entry->name, sizeof(shown->name));
        shown->category = entry->category;
        shown->rssi = entry->rssi;
    }
    s_snapshot.seconds_remaining = remaining;
    ++s_snapshot.generation;
    s_dirty = false;
    portEXIT_CRITICAL(&s_lock);
}

static void begin_scan(int64_t now_us)
{
    struct ble_gap_disc_params params = {0};
    params.passive = 0; // Scan requests reveal optional names in scan responses.
    params.filter_duplicates = 0; // Merge advertising and scan responses here.
    params.itvl = 0x0060;   // 60 ms
    params.window = 0x0030; // 30 ms

    uint8_t own_address_type;
    portENTER_CRITICAL(&s_lock);
    if (!s_active || !s_host_synced || s_scan_in_progress) {
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    own_address_type = s_own_address_type;
    s_scan_in_progress = true;
    s_waiting_for_cancel = false;
    s_cycle_started_us = now_us;
    s_next_scan_due_us = now_us + BLE_SCAN_CYCLE_MS * 1000LL;
    s_advertiser_count = 0;
    s_dirty = false;
    s_snapshot.total = 0;
    s_snapshot.phones = 0;
    s_snapshot.audio = 0;
    s_snapshot.wearables = 0;
    s_snapshot.computers = 0;
    s_snapshot.other = 0;
    s_snapshot.truncated = false;
    s_snapshot.device_count = 0;
    memset(s_snapshot.devices, 0, sizeof(s_snapshot.devices));
    s_snapshot.seconds_remaining = BLE_SCAN_CYCLE_MS / 1000;
    s_snapshot.state = BUDDY_BLE_SCANNING;
    s_snapshot.error_code = 0;
    ++s_snapshot.generation;
    portEXIT_CRITICAL(&s_lock);

    int rc = ble_gap_disc(own_address_type, BLE_SCAN_CYCLE_MS,
                          &params, scan_event, NULL);
    if (rc != 0) {
        portENTER_CRITICAL(&s_lock);
        s_scan_in_progress = false;
        s_next_scan_due_us = esp_timer_get_time() + BLE_SCAN_RETRY_US;
        portEXIT_CRITICAL(&s_lock);
        set_error(rc);
    }
}

bool buddy_ble_start(void)
{
    portENTER_CRITICAL(&s_lock);
    if (s_active) {
        portEXIT_CRITICAL(&s_lock);
        return true;
    }
    s_active = true;
    s_advertiser_count = 0;
    s_dirty = false;
    s_next_scan_due_us = 0;
    const uint32_t next_generation = s_snapshot.generation + 1;
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_snapshot.state = BUDDY_BLE_STARTING;
    s_snapshot.generation = next_generation;
    const bool initialized = s_host_initialized;
    portEXIT_CRITICAL(&s_lock);

    if (!initialized) {
        esp_err_t err = nvs_flash_init();
        if (err != ESP_OK) {
            portENTER_CRITICAL(&s_lock);
            s_active = false;
            portEXIT_CRITICAL(&s_lock);
            set_error(err);
            return false;
        }
        err = nimble_port_init();
        if (err != ESP_OK) {
            portENTER_CRITICAL(&s_lock);
            s_active = false;
            portEXIT_CRITICAL(&s_lock);
            set_error(err);
            return false;
        }
        ble_hs_cfg.reset_cb = on_host_reset;
        ble_hs_cfg.sync_cb = on_host_sync;
        portENTER_CRITICAL(&s_lock);
        s_host_initialized = true;
        portEXIT_CRITICAL(&s_lock);
        nimble_port_freertos_init(host_task);
    }
    buddy_ble_update();
    return true;
}

void buddy_ble_stop(void)
{
    bool cancel;
    portENTER_CRITICAL(&s_lock);
    s_active = false;
    cancel = s_scan_in_progress;
    if (cancel) {
        s_waiting_for_cancel = true;
    }
    s_snapshot.state = BUDDY_BLE_STOPPED;
    s_snapshot.seconds_remaining = 0;
    ++s_snapshot.generation;
    portEXIT_CRITICAL(&s_lock);

    if (cancel) {
        int rc = ble_gap_disc_cancel();
        if (rc == 0 || rc == BLE_HS_EALREADY) {
            // NimBLE resets discovery state synchronously on cancellation and
            // does not send DISC_COMPLETE. EALREADY means it just completed.
            portENTER_CRITICAL(&s_lock);
            s_scan_in_progress = false;
            s_waiting_for_cancel = false;
            portEXIT_CRITICAL(&s_lock);
        } else {
            // Keep the pending state until DISC_COMPLETE if the controller
            // could not cancel. A quick re-entry must not start a second scan.
            ESP_LOGW(BLE_SCAN_TAG, "Canceling BLE scan failed: %d", rc);
        }
    }
    // Keep the NimBLE host initialized for the next visit. The radio scan is
    // stopped above; tearing down the host would stall the UI on every exit.
}

void buddy_ble_update(void)
{
    bool active;
    bool synced;
    bool scanning;
    int64_t due_us;
    int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    active = s_active;
    synced = s_host_synced;
    scanning = s_scan_in_progress;
    due_us = s_next_scan_due_us;
    portEXIT_CRITICAL(&s_lock);

    if (!active || !synced) {
        return;
    }
    if (now_us - s_last_publish_us >= BLE_SCAN_PUBLISH_US) {
        publish_snapshot(now_us);
        s_last_publish_us = now_us;
    }
    if (!scanning && now_us >= due_us) {
        begin_scan(now_us);
    }
}

bool buddy_ble_copy_snapshot(buddy_ble_snapshot_t *out,
                             uint32_t after_generation)
{
    if (out == NULL) {
        return false;
    }
    portENTER_CRITICAL(&s_lock);
    bool changed = s_snapshot.generation != after_generation;
    *out = s_snapshot;
    portEXIT_CRITICAL(&s_lock);
    return changed;
}
