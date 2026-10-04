#pragma once

#include <cstdint>

enum Direction : uint8_t { UP, DOWN, LEFT, RIGHT, DIRECTION_COUNT };
enum Page : uint8_t { SPLASH, MENU, DISPLAY_TEST, BUTTON_TEST, SYSTEM_TEST, SELF_TEST };
enum MenuIcon : uint8_t { ICON_DISPLAY, ICON_INPUT, ICON_SYSTEM };
