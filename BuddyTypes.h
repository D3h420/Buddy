#pragma once

#include <Arduino.h>

// Kept in a header so Arduino's generated function prototypes can see these
// types before the preprocessed sketch body.
enum Direction : uint8_t { UP, DOWN, LEFT, RIGHT, DIRECTION_COUNT };

enum Page : uint8_t {
  SPLASH,
  MENU,
  DISPLAY_TEST,
  BUTTON_TEST,
  SYSTEM_TEST,
  SELF_TEST
};

enum MenuIcon : uint8_t { ICON_DISPLAY, ICON_INPUT, ICON_SYSTEM };
