#include "buddy_display.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "buddy_board.h"
#include "buddy_font.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
constexpr int kWidth = 240;
constexpr int kHeight = 240;
constexpr gpio_num_t kClock = static_cast<gpio_num_t>(TFT_SCLK);
constexpr gpio_num_t kMosi = static_cast<gpio_num_t>(TFT_MOSI);
constexpr gpio_num_t kDc = static_cast<gpio_num_t>(TFT_DC);
constexpr gpio_num_t kReset = static_cast<gpio_num_t>(TFT_RST);

void pauseMs(uint32_t ms) {
  vTaskDelay(pdMS_TO_TICKS(ms));
}
}  // namespace

void BuddyDisplay::writeCommand(uint8_t command) {
  ESP_ERROR_CHECK(gpio_set_level(kDc, 0));
  spi_transaction_t tx = {};
  tx.flags = SPI_TRANS_USE_TXDATA;
  tx.length = 8;
  tx.tx_data[0] = command;
  ESP_ERROR_CHECK(spi_device_polling_transmit(spi_, &tx));
}

void BuddyDisplay::writeData(const void *data, size_t length) {
  if (length == 0) return;
  ESP_ERROR_CHECK(gpio_set_level(kDc, 1));
  spi_transaction_t tx = {};
  tx.length = length * 8;
  if (length <= sizeof(tx.tx_data)) {
    tx.flags = SPI_TRANS_USE_TXDATA;
    memcpy(tx.tx_data, data, length);
  } else {
    tx.tx_buffer = data;
  }
  ESP_ERROR_CHECK(spi_device_polling_transmit(spi_, &tx));
}

void BuddyDisplay::sendCommand(uint8_t command, const uint8_t *data, size_t length) {
  writeCommand(command);
  if (data && length) writeData(data, length);
}

void BuddyDisplay::init() {
  gpio_config_t control = {};
  control.pin_bit_mask = (1ULL << kDc) | (1ULL << kReset);
  control.mode = GPIO_MODE_OUTPUT;
  ESP_ERROR_CHECK(gpio_config(&control));
  ESP_ERROR_CHECK(gpio_set_level(kDc, 1));
  ESP_ERROR_CHECK(gpio_set_level(kReset, 1));

  spi_bus_config_t bus = {};
  bus.sclk_io_num = kClock;
  bus.mosi_io_num = kMosi;
  bus.miso_io_num = -1;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = kWidth * 2 * 4;
  ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

  spi_device_interface_config_t device = {};
  device.clock_speed_hz = TFT_SPI_HZ;
  device.mode = 3;
  device.spics_io_num = -1;  // CS is physically connected to ground.
  device.queue_size = 1;
  ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &device, &spi_));

  // Preserve the command sequence and delays from the working Arduino sketch.
  pauseMs(100);
  ESP_ERROR_CHECK(gpio_set_level(kReset, 0));
  pauseMs(100);
  ESP_ERROR_CHECK(gpio_set_level(kReset, 1));
  pauseMs(200);
  sendCommand(0x01);  // SWRESET
  pauseMs(150);
  sendCommand(0x11);  // SLPOUT
  pauseMs(120);

  const uint8_t madctl[] = {0x00};  // Adafruit rotation 2, 0 x/y offset.
  const uint8_t ramctrl[] = {0x00, 0xE0};
  const uint8_t colmod[] = {0x55};  // RGB565
  sendCommand(0x36, madctl, sizeof(madctl));
  sendCommand(0xB0, ramctrl, sizeof(ramctrl));
  sendCommand(0x3A, colmod, sizeof(colmod));
  pauseMs(10);
  const uint8_t porctrl[] = {0x0C, 0x0C, 0x00, 0x33, 0x33};
  const uint8_t gctrl[] = {0x35};
  const uint8_t vcoms[] = {0x28};
  const uint8_t lcmctrl[] = {0x2C};
  const uint8_t vdvvrhen[] = {0x01};
  const uint8_t vrhs[] = {0x10};
  const uint8_t vdvs[] = {0x20};
  const uint8_t frctrl2[] = {0x0F};
  const uint8_t pwctrl1[] = {0xA4, 0xA1};
  const uint8_t positiveGamma[] = {0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x32,
                                   0x44, 0x42, 0x06, 0x0E, 0x12, 0x14, 0x17};
  const uint8_t negativeGamma[] = {0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x31,
                                   0x54, 0x47, 0x0E, 0x1C, 0x17, 0x1B, 0x1E};
  sendCommand(0xB2, porctrl, sizeof(porctrl));
  sendCommand(0xB7, gctrl, sizeof(gctrl));
  sendCommand(0xBB, vcoms, sizeof(vcoms));
  sendCommand(0xC0, lcmctrl, sizeof(lcmctrl));
  sendCommand(0xC2, vdvvrhen, sizeof(vdvvrhen));
  sendCommand(0xC3, vrhs, sizeof(vrhs));
  sendCommand(0xC4, vdvs, sizeof(vdvs));
  sendCommand(0xC6, frctrl2, sizeof(frctrl2));
  sendCommand(0xD0, pwctrl1, sizeof(pwctrl1));
  sendCommand(0xE0, positiveGamma, sizeof(positiveGamma));
  sendCommand(0xE1, negativeGamma, sizeof(negativeGamma));
  sendCommand(0x21);  // INVON
  sendCommand(0x13);  // NORON
  pauseMs(10);
  sendCommand(0x29);  // DISPON
  pauseMs(120);
}

void BuddyDisplay::setWindow(int16_t x, int16_t y, int16_t w, int16_t h) {
  const uint8_t column[] = {static_cast<uint8_t>(x >> 8), static_cast<uint8_t>(x),
                            static_cast<uint8_t>((x + w - 1) >> 8), static_cast<uint8_t>(x + w - 1)};
  const uint8_t row[] = {static_cast<uint8_t>(y >> 8), static_cast<uint8_t>(y),
                         static_cast<uint8_t>((y + h - 1) >> 8), static_cast<uint8_t>(y + h - 1)};
  sendCommand(0x2A, column, sizeof(column));
  sendCommand(0x2B, row, sizeof(row));
  writeCommand(0x2C);
}

void BuddyDisplay::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  if (w <= 0 || h <= 0) return;
  const int16_t x0 = std::max<int16_t>(0, x);
  const int16_t y0 = std::max<int16_t>(0, y);
  const int16_t x1 = std::min<int16_t>(kWidth, x + w);
  const int16_t y1 = std::min<int16_t>(kHeight, y + h);
  if (x1 <= x0 || y1 <= y0) return;
  const int16_t clipped_w = x1 - x0;
  const int16_t clipped_h = y1 - y0;
  setWindow(x0, y0, clipped_w, clipped_h);
  for (int i = 0; i < clipped_w * 4; ++i) {
    fill_buffer_[2 * i] = color >> 8;
    fill_buffer_[2 * i + 1] = color;
  }
  for (int remaining = clipped_h; remaining > 0;) {
    const int rows = std::min(4, remaining);
    writeData(fill_buffer_, clipped_w * rows * 2);
    remaining -= rows;
  }
}

void BuddyDisplay::fillScreen(uint16_t color) { fillRect(0, 0, kWidth, kHeight, color); }
void BuddyDisplay::drawPixel(int16_t x, int16_t y, uint16_t color) { fillRect(x, y, 1, 1, color); }
void BuddyDisplay::drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) { fillRect(x, y, w, 1, color); }
void BuddyDisplay::drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) { fillRect(x, y, 1, h, color); }

void BuddyDisplay::drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  if (w <= 0 || h <= 0) return;
  drawFastHLine(x, y, w, color);
  drawFastHLine(x, y + h - 1, w, color);
  drawFastVLine(x, y, h, color);
  drawFastVLine(x + w - 1, y, h, color);
}

void BuddyDisplay::drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) {
  if (y0 == y1) { drawFastHLine(std::min(x0, x1), y0, std::abs(x1 - x0) + 1, color); return; }
  if (x0 == x1) { drawFastVLine(x0, std::min(y0, y1), std::abs(y1 - y0) + 1, color); return; }
  const int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  const int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int error = dx + dy;
  while (true) {
    drawPixel(x0, y0, color);
    if (x0 == x1 && y0 == y1) break;
    const int twice = 2 * error;
    if (twice >= dy) { error += dy; x0 += sx; }
    if (twice <= dx) { error += dx; y0 += sy; }
  }
}

void BuddyDisplay::drawCircle(int16_t x0, int16_t y0, int16_t r, uint16_t color) {
  if (r < 0) return;
  int16_t f = 1 - r, ddx = 1, ddy = -2 * r, x = 0, y = r;
  drawPixel(x0, y0 + r, color);
  drawPixel(x0, y0 - r, color);
  drawPixel(x0 + r, y0, color);
  drawPixel(x0 - r, y0, color);
  while (x < y) {
    if (f >= 0) { --y; ddy += 2; f += ddy; }
    ++x; ddx += 2; f += ddx;
    drawPixel(x0 + x, y0 + y, color); drawPixel(x0 - x, y0 + y, color);
    drawPixel(x0 + x, y0 - y, color); drawPixel(x0 - x, y0 - y, color);
    drawPixel(x0 + y, y0 + x, color); drawPixel(x0 - y, y0 + x, color);
    drawPixel(x0 + y, y0 - x, color); drawPixel(x0 - y, y0 - x, color);
  }
}

void BuddyDisplay::drawChar(int16_t x, int16_t y, char c) {
  if (c < 32 || c > 126) return;
  const uint8_t *glyph = BUDDY_FONT_5X7 + (c - 32) * 5;
  for (int row = 0; row < 8; ++row) {
    int column = 0;
    while (column < 5) {
      while (column < 5 && !(glyph[column] & (1U << row))) ++column;
      const int start = column;
      while (column < 5 && (glyph[column] & (1U << row))) ++column;
      if (column > start) {
        fillRect(x + start * text_size_, y + row * text_size_,
                 (column - start) * text_size_, text_size_, text_color_);
      }
    }
  }
}

void BuddyDisplay::print(const char *text) {
  if (!text) return;
  for (const char *p = text; *p; ++p) {
    if (*p == '\n') { cursor_x_ = 0; cursor_y_ += 8 * text_size_; }
    else if (*p != '\r') { drawChar(cursor_x_, cursor_y_, *p); cursor_x_ += 6 * text_size_; }
  }
}

void BuddyDisplay::getTextBounds(const char *text, int16_t x, int16_t y,
                                 int16_t *x1, int16_t *y1, uint16_t *w, uint16_t *h) const {
  *x1 = x; *y1 = y; *w = 0; *h = 0;
  if (!text) return;
  uint16_t line_length = 0;
  uint16_t max_length = 0;
  uint16_t lines = 1;
  for (const char *p = text; *p; ++p) {
    if (*p == '\n') { max_length = std::max(max_length, line_length); line_length = 0; ++lines; }
    else if (*p != '\r') ++line_length;
  }
  max_length = std::max(max_length, line_length);
  if (max_length) { *w = max_length * 6 * text_size_; *h = lines * 8 * text_size_; }
}

bool BuddyDisplay::fontReady() const {
  return sizeof(BUDDY_FONT_5X7) == 95 * 5 && BUDDY_FONT_5X7[('B' - 32) * 5] != 0;
}
