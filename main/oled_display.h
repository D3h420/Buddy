#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Blackout's four text lines are passed to Buddy's existing display loop.
#define OLED_DISPLAY_LINE_COUNT 4
#define OLED_DISPLAY_LINE_CAPACITY 64

typedef struct {
    char lines[OLED_DISPLAY_LINE_COUNT][OLED_DISPLAY_LINE_CAPACITY];
    uint32_t generation;
} oled_display_snapshot_t;

// NULL preserves the previous content of that line, as in the source module.
void oled_display_update_full(const char *line1, const char *line2,
                              const char *line3, const char *line4);

// Returns true and copies all four lines when the content changed since
// last_generation. Call from the Buddy UI task to draw on its ST7789 screen.
bool oled_display_copy_snapshot(oled_display_snapshot_t *out,
                                uint32_t last_generation);

// Clear the text when leaving Lab Tester or before starting it again.
void oled_display_reset(void);

#ifdef __cplusplus
}
#endif
