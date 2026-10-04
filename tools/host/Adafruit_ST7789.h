#pragma once
#include <Adafruit_GFX.h>
#include "SPI.h"
#define ST77XX_SWRESET 1
#define ST77XX_SLPOUT 2
#define ST77XX_MADCTL 3
#define ST77XX_COLMOD 4
#define ST77XX_INVON 5
#define ST77XX_NORON 6
#define ST77XX_DISPON 7
// Execute the actual installed Adafruit drawing/text implementation in a host
// canvas. Hardware transactions are stubbed; firmware coordinates are intact.
class Adafruit_ST7789 : public GFXcanvas16 {
public:
  Adafruit_ST7789(HostSPI *, int, int, int) : GFXcanvas16(240, 240) {}
  void init(int, int, int) {}
  void setSPISpeed(uint32_t) {}
  void sendCommand(uint8_t, const uint8_t * = nullptr, size_t = 0) {}
  void setRotation(uint8_t) {} // Capture the logical UI, before panel rotation.
};
