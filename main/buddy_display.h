#pragma once

#include <cstddef>
#include <cstdint>
#include "driver/spi_master.h"

// Minimal native ESP-IDF ST7789 renderer with the drawing calls used by Buddy.
// The panel has CS permanently tied to ground, so only one SPI device is used.
class BuddyDisplay {
 public:
  void init();
  void sendCommand(uint8_t command, const uint8_t *data = nullptr, size_t length = 0);
  void fillScreen(uint16_t color);
  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
  void drawPixel(int16_t x, int16_t y, uint16_t color);
  void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color);
  void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color);
  void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
  void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color);
  void drawCircle(int16_t x0, int16_t y0, int16_t r, uint16_t color);

  void setTextSize(uint8_t size) { text_size_ = size; }
  void setTextColor(uint16_t color) { text_color_ = color; }
  void setCursor(int16_t x, int16_t y) { cursor_x_ = x; cursor_y_ = y; }
  void print(const char *text);
  void getTextBounds(const char *text, int16_t x, int16_t y, int16_t *x1,
                     int16_t *y1, uint16_t *w, uint16_t *h) const;
  bool fontReady() const;

 private:
  void writeCommand(uint8_t command);
  void writeData(const void *data, size_t length);
  void setWindow(int16_t x, int16_t y, int16_t w, int16_t h);
  void drawChar(int16_t x, int16_t y, char c);

  spi_device_handle_t spi_ = nullptr;
  uint8_t text_size_ = 1;
  uint16_t text_color_ = 0xffff;
  int16_t cursor_x_ = 0;
  int16_t cursor_y_ = 0;
  alignas(4) uint8_t fill_buffer_[240 * 2 * 4] = {};
};
