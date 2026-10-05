#include "oled_display.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

static portMUX_TYPE s_display_lock = portMUX_INITIALIZER_UNLOCKED;
static oled_display_snapshot_t s_display;

static void copy_line(char destination[OLED_DISPLAY_LINE_CAPACITY],
                      const char *source)
{
    size_t index = 0;
    while (index + 1 < OLED_DISPLAY_LINE_CAPACITY && source[index] != '\0') {
        destination[index] = source[index];
        ++index;
    }
    destination[index] = '\0';
}

void oled_display_update_full(const char *line1, const char *line2,
                              const char *line3, const char *line4)
{
    const char *lines[OLED_DISPLAY_LINE_COUNT] = {line1, line2, line3, line4};
    char prepared[OLED_DISPLAY_LINE_COUNT][OLED_DISPLAY_LINE_CAPACITY] = {{0}};

    // Copy caller-owned text before entering the short critical section.
    for (size_t index = 0; index < OLED_DISPLAY_LINE_COUNT; ++index) {
        if (lines[index] != NULL) {
            copy_line(prepared[index], lines[index]);
        }
    }

    portENTER_CRITICAL(&s_display_lock);
    for (size_t index = 0; index < OLED_DISPLAY_LINE_COUNT; ++index) {
        if (lines[index] != NULL) {
            memcpy(s_display.lines[index], prepared[index],
                   OLED_DISPLAY_LINE_CAPACITY);
        }
    }
    ++s_display.generation;
    portEXIT_CRITICAL(&s_display_lock);
}

bool oled_display_copy_snapshot(oled_display_snapshot_t *out,
                                uint32_t last_generation)
{
    if (out == NULL) {
        return false;
    }

    portENTER_CRITICAL(&s_display_lock);
    const bool changed = s_display.generation != last_generation;
    if (changed) {
        memcpy(out, &s_display, sizeof(*out));
    }
    portEXIT_CRITICAL(&s_display_lock);
    return changed;
}

void oled_display_reset(void)
{
    portENTER_CRITICAL(&s_display_lock);
    memset(s_display.lines, 0, sizeof(s_display.lines));
    ++s_display.generation;
    portEXIT_CRITICAL(&s_display_lock);
}
