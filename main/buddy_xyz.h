#pragma once

#include <cstdint>

// Migrated from projectZero/ESP32C5/main/main.c: cmd_start_blackout,
// blackout_attack_task and wsl_bypasser_send_deauth_frame_multiple_aps.
//
// runXyz() is called every loop iteration while the XYZ screen is open and
// starts the attack once, then only serves as its supervisor/stop channel.
// Going back to the menu (LEFT) stops the attack.

// Called from buddy.cpp when the XYZ screen becomes visible.
void xyzBegin();

// Called from buddy.cpp when leaving the XYZ screen. Stops the attack.
void xyzEnd();

// Called from buddy.cpp every loop while currentPage == XYZ_SCREEN.
// Returns true when an attack is running so the UI can show it.
bool xyzTick();

// True while the blackout task is alive.
bool xyzAttackActive();

// Number of deauth frames sent since the attack started (UI/statistics).
uint32_t xyzFramesSent();

// True once the attack has failed to start (e.g. WiFi could not be brought up).
bool xyzStartFailed();

// Last error string from a failed start (empty when xyzStartFailed() is false).
const char *xyzStartError();