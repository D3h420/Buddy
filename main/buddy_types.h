#pragma once

#include <cstdint>

enum Direction : uint8_t { UP, DOWN, LEFT, RIGHT, DIRECTION_COUNT };
enum Page : uint8_t { SPLASH, MENU, DISPLAY_TEST, RADAR, SYSTEM_TEST, SELF_TEST, LAB_TESTER };
enum MenuIcon : uint8_t { ICON_DISPLAY, ICON_RADAR, ICON_SYSTEM, ICON_LAB };
