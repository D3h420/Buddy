#pragma once

#include <cstdint>

// Seeed Studio XIAO ESP32-C5 pin mapping verified on the Buddy prototype.
constexpr int8_t TFT_SCLK = 8;   // D8
constexpr int8_t TFT_MOSI = 10;  // D10
constexpr int8_t TFT_CS = -1;    // Physically tied to GND
constexpr int8_t TFT_DC = 11;    // D6
constexpr int8_t TFT_RST = 25;   // D2
constexpr int8_t TFT_BL = 12;    // D7
constexpr uint32_t TFT_SPI_HZ = 4000000;

constexpr uint8_t BTN_UP = 1;     // D0
constexpr uint8_t BTN_DOWN = 24;  // D5
constexpr uint8_t BTN_LEFT = 0;   // D1
constexpr uint8_t BTN_RIGHT = 23; // D4
