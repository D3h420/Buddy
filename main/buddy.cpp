#include <algorithm>
#include <cstdint>
#include <cstdio>

#include "buddy_board.h"
#include "buddy_display.h"
#include "buddy_types.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
constexpr const char *TAG = "Buddy";
constexpr int HIGH = 1;
constexpr int LOW = 0;
uint32_t millis() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }
void delay(uint32_t ms) {
  const TickType_t ticks = pdMS_TO_TICKS(ms);
  vTaskDelay(ticks > 0 ? ticks : 1);
}
int digitalRead(uint8_t pin) { return gpio_get_level(static_cast<gpio_num_t>(pin)); }
}  // namespace

// -----------------------------------------------------------------------------
// Hardware configuration — verified on the physical Buddy prototype.
// -----------------------------------------------------------------------------

constexpr uint32_t DEBOUNCE_MS = 30;
constexpr int16_t SCREEN_W = 240;
constexpr int16_t SCREEN_H = 240;

BuddyDisplay tft;

// -----------------------------------------------------------------------------
// Buddy visual system — a compact 16-bit night palette.
// -----------------------------------------------------------------------------

constexpr uint16_t rgb565(uint8_t red, uint8_t green, uint8_t blue) {
  return static_cast<uint16_t>(((red & 0xF8) << 8) |
                               ((green & 0xFC) << 3) | (blue >> 3));
}

constexpr uint16_t COLOR_BG = rgb565(5, 6, 25);
constexpr uint16_t COLOR_INK = rgb565(13, 10, 35);
constexpr uint16_t COLOR_SURFACE = rgb565(18, 17, 51);
constexpr uint16_t COLOR_SURFACE_2 = rgb565(31, 28, 72);
constexpr uint16_t COLOR_GRID = rgb565(48, 42, 92);
constexpr uint16_t COLOR_MAGENTA_DARK = rgb565(88, 12, 66);
constexpr uint16_t COLOR_MAGENTA = rgb565(255, 66, 177);
constexpr uint16_t COLOR_PINK = rgb565(255, 174, 221);
constexpr uint16_t COLOR_CYAN = rgb565(66, 226, 255);
constexpr uint16_t COLOR_TEXT = rgb565(255, 241, 239);
constexpr uint16_t COLOR_MUTED = rgb565(164, 151, 190);

struct ButtonState {
  uint8_t pin;
  const char *name;
  const char *pinName;
  bool rawState;
  bool stableState;
  bool seen;
  uint32_t changedAt;
  uint32_t presses;
  uint32_t pressedAt;
  bool holdHandled;
};

struct MenuItem {
  const char *title;
  const char *subtitle;
  MenuIcon icon;
};

ButtonState buttons[DIRECTION_COUNT] = {
    {BTN_UP, "UP", "D0", HIGH, HIGH, false, 0, 0, 0, false},
    {BTN_DOWN, "DOWN", "D5", HIGH, HIGH, false, 0, 0, 0, false},
    {BTN_LEFT, "LEFT", "D1", HIGH, HIGH, false, 0, 0, 0, false},
    {BTN_RIGHT, "RIGHT", "D4", HIGH, HIGH, false, 0, 0, 0, false},
};

constexpr MenuItem MENU_ITEMS[] = {
    {"DISPLAY", "PIXEL + MOTION", ICON_DISPLAY},
    {"BUTTONS", "INPUT MONITOR", ICON_INPUT},
    {"SYSTEM", "DEVICE STATUS", ICON_SYSTEM},
    {"XYZ", "CUSTOM MODE", ICON_XYZ},
};
constexpr uint8_t MENU_ITEM_COUNT = sizeof(MENU_ITEMS) / sizeof(MENU_ITEMS[0]);

Page currentPage = SPLASH;
uint8_t selectedItem = 0;
uint8_t displayStage = 0;
int8_t lastDirection = -1;

bool navigationPending = false;
Page pendingPage = MENU;
uint32_t navigationAt = 0;
bool inputReleaseGate = false;

bool splashPromptBright = true;
uint32_t splashPromptUpdatedAt = 0;
uint32_t menuFocusUpdatedAt = 0;
bool menuFocusBright = true;

int16_t scannerX = 20;
int16_t previousScannerX = 20;
uint32_t scannerUpdatedAt = 0;

bool buttonTestSeen[DIRECTION_COUNT] = {false, false, false, false};
uint32_t buttonTestBaseline[DIRECTION_COUNT] = {0, 0, 0, 0};
uint8_t buttonHoldProgress[DIRECTION_COUNT] = {0, 0, 0, 0};

uint32_t systemUpdatedAt = 0;
uint32_t selfTestStartedAt = 0;
uint8_t selfTestStep = 0;
uint16_t selfTestProgressDrawn = 0;
bool selfTestResults[3] = {false, false, false};
bool selfTestHasRun = false;
bool selfTestAnyFailure = false;
bool selfTestRunAnyFailure = false;
bool selfTestRunCommitted = false;

// -----------------------------------------------------------------------------
// Drawing primitives.
// -----------------------------------------------------------------------------

void drawCenteredText(const char *text, int16_t centerX, int16_t y,
                      uint8_t size, uint16_t color) {
  int16_t x1, y1;
  uint16_t width, height;
  tft.setTextSize(size);
  tft.setTextColor(color);
  tft.getTextBounds(text, 0, y, &x1, &y1, &width, &height);
  tft.setCursor(centerX - static_cast<int16_t>(width) / 2, y);
  tft.print(text);
}

void drawRightText(const char *text, int16_t rightX, int16_t y, uint8_t size,
                   uint16_t color) {
  int16_t x1, y1;
  uint16_t width, height;
  tft.setTextSize(size);
  tft.setTextColor(color);
  tft.getTextBounds(text, 0, y, &x1, &y1, &width, &height);
  tft.setCursor(rightX - static_cast<int16_t>(width), y);
  tft.print(text);
}

void fillChamferedRect(int16_t x, int16_t y, int16_t width, int16_t height,
                       int16_t cut, uint16_t color) {
  if (width <= 0 || height <= 0) return;
  const int16_t maxCut = std::min((width - 1) / 2, (height - 1) / 2);
  cut = std::clamp<int16_t>(cut, 0, maxCut);
  if (cut == 0) {
    tft.fillRect(x, y, width, height, color);
    return;
  }

  // Three non-overlapping rectangular bands plus short edge runs avoid writing
  // the large center area twice — important on the verified 4 MHz SPI link.
  tft.fillRect(x, y + cut, width, height - cut * 2, color);
  tft.fillRect(x + cut, y, width - cut * 2, cut, color);
  tft.fillRect(x + cut, y + height - cut, width - cut * 2, cut, color);
  for (int16_t row = 1; row < cut; ++row) {
    tft.drawFastHLine(x + cut - row, y + row, row, color);
    tft.drawFastHLine(x + width - cut, y + row, row, color);
    tft.drawFastHLine(x + cut - row, y + height - 1 - row, row, color);
    tft.drawFastHLine(x + width - cut, y + height - 1 - row, row, color);
  }
}

void drawChamferedRect(int16_t x, int16_t y, int16_t width, int16_t height,
                       int16_t cut, uint16_t color) {
  const int16_t right = x + width - 1;
  const int16_t bottom = y + height - 1;
  tft.drawFastHLine(x + cut, y, width - cut * 2, color);
  tft.drawFastHLine(x + cut, bottom, width - cut * 2, color);
  tft.drawFastVLine(x, y + cut, height - cut * 2, color);
  tft.drawFastVLine(right, y + cut, height - cut * 2, color);
  tft.drawLine(x, y + cut, x + cut, y, color);
  tft.drawLine(right - cut, y, right, y + cut, color);
  tft.drawLine(x, bottom - cut, x + cut, bottom, color);
  tft.drawLine(right - cut, bottom, right, bottom - cut, color);
}

void drawPanelShadow(int16_t x, int16_t y, int16_t width, int16_t height,
                     int16_t cut) {
  // The main filled panel covers the top/left portion of this offset outline,
  // leaving a continuous two-pixel right/bottom shadow including its bevels.
  drawChamferedRect(x + 2, y + 2, width, height, cut, COLOR_INK);
}

void drawHeader(const char *title, const char *subtitle, const char *badge) {
  tft.fillRect(0, 0, SCREEN_W, 41, COLOR_BG);
  tft.fillRect(0, 0, 76, 3, COLOR_MAGENTA);
  tft.fillRect(76, 0, 29, 3, COLOR_CYAN);
  tft.fillRect(107, 0, 133, 3, COLOR_SURFACE_2);

  tft.fillRect(9, 9, 5, 20, COLOR_MAGENTA);
  tft.setTextSize(2);
  tft.setTextColor(COLOR_TEXT);
  tft.setCursor(20, 7);
  tft.print(title);
  tft.setTextSize(1);
  tft.setTextColor(COLOR_MUTED);
  tft.setCursor(20, 26);
  tft.print(subtitle);

  fillChamferedRect(188, 10, 42, 18, 4, COLOR_SURFACE_2);
  drawCenteredText(badge, 209, 16, 1, COLOR_PINK);
  tft.drawFastHLine(9, 39, 222, COLOR_GRID);
  tft.drawPixel(9, 40, COLOR_MAGENTA);
  tft.drawPixel(230, 40, COLOR_CYAN);
}

void drawFooter(const char *leftKey, const char *leftLabel,
                const char *rightKey, const char *rightLabel) {
  tft.fillRect(0, 213, SCREEN_W, 27, COLOR_BG);
  tft.drawFastHLine(8, 213, 224, COLOR_GRID);
  tft.fillRect(8, 220, 13, 13, COLOR_MAGENTA);
  drawCenteredText(leftKey, 14, 223, 1, COLOR_INK);
  tft.setTextSize(1);
  tft.setTextColor(COLOR_MUTED);
  tft.setCursor(27, 223);
  tft.print(leftLabel);

  if (rightKey == nullptr || rightKey[0] == '\0') {
    drawRightText(rightLabel, 232, 223, 1, COLOR_MUTED);
    return;
  }

  int16_t x1, y1;
  uint16_t labelWidth, labelHeight;
  tft.getTextBounds(rightLabel, 0, 223, &x1, &y1, &labelWidth, &labelHeight);
  const int16_t keyX = 232 - static_cast<int16_t>(labelWidth) - 19;
  tft.fillRect(keyX, 220, 13, 13, COLOR_MAGENTA);
  drawCenteredText(rightKey, keyX + 6, 223, 1, COLOR_INK);
  tft.setTextColor(COLOR_MUTED);
  tft.setCursor(keyX + 18, 223);
  tft.print(rightLabel);
}

uint8_t seenButtonCount() {
  uint8_t count = 0;
  for (const ButtonState &button : buttons) {
    if (button.seen) ++count;
  }
  return count;
}

uint8_t buttonTestSeenCount() {
  uint8_t count = 0;
  for (bool seen : buttonTestSeen) {
    if (seen) ++count;
  }
  return count;
}

uint16_t gray565(uint8_t level) {
  return static_cast<uint16_t>(((level & 0xF8) << 8) |
                               ((level & 0xFC) << 3) | (level >> 3));
}

// -----------------------------------------------------------------------------
// Splash / attract screen.
// -----------------------------------------------------------------------------

void drawSplashPrompt() {
  fillChamferedRect(24, 211, 192, 22, 5, COLOR_INK);
  drawChamferedRect(24, 211, 192, 22, 5,
                    splashPromptBright ? COLOR_MAGENTA : COLOR_MAGENTA_DARK);
  drawCenteredText("PRESS ANY KEY TO START", 120, 218, 1,
                   splashPromptBright ? COLOR_TEXT : COLOR_MUTED);
}

void drawSplash() {
  tft.fillScreen(COLOR_BG);
  drawChamferedRect(3, 3, 234, 234, 8, COLOR_CYAN);
  tft.drawFastHLine(12, 3, 55, COLOR_MAGENTA);
  tft.drawFastVLine(3, 12, 42, COLOR_MAGENTA);
  drawCenteredText("BUDDY", 121, 102, 3, COLOR_MAGENTA_DARK);
  drawCenteredText("BUDDY", 120, 101, 3, COLOR_TEXT);
  drawCenteredText("POCKET SYSTEM // 01", 120, 149, 1, COLOR_MUTED);
  splashPromptBright = true;
  splashPromptUpdatedAt = millis();
  drawSplashPrompt();
}

void updateSplash() {
  if (currentPage != SPLASH) return;
  const uint32_t now = millis();
  if (now - splashPromptUpdatedAt >= 560) {
    splashPromptUpdatedAt = now;
    splashPromptBright = !splashPromptBright;
    drawSplashPrompt();
  }
}

// -----------------------------------------------------------------------------
// Main menu.
// -----------------------------------------------------------------------------

void drawDisplayIcon(int16_t x, int16_t y, uint16_t color) {
  tft.drawRect(x, y, 20, 14, color);
  tft.drawRect(x + 2, y + 2, 16, 10, color);
  tft.drawFastVLine(x + 10, y + 14, 4, color);
  tft.drawFastHLine(x + 5, y + 18, 11, color);
}

void drawInputIcon(int16_t x, int16_t y, uint16_t color) {
  tft.drawFastHLine(x + 6, y, 8, color);
  tft.drawFastHLine(x + 6, y + 19, 8, color);
  tft.drawFastVLine(x, y + 6, 8, color);
  tft.drawFastVLine(x + 19, y + 6, 8, color);
  tft.drawLine(x + 10, y + 3, x + 10, y + 15, color);
  tft.drawLine(x + 4, y + 9, x + 16, y + 9, color);
  tft.fillRect(x + 8, y + 7, 5, 5, color);
}

void drawSystemIcon(int16_t x, int16_t y, uint16_t color) {
  tft.drawRect(x + 3, y + 3, 14, 14, color);
  tft.drawRect(x + 7, y + 7, 6, 6, color);
  for (uint8_t index = 0; index < 3; ++index) {
    const int16_t offset = 5 + index * 5;
    tft.drawFastVLine(x + offset, y, 3, color);
    tft.drawFastVLine(x + offset, y + 17, 3, color);
    tft.drawFastHLine(x, y + offset, 3, color);
    tft.drawFastHLine(x + 17, y + offset, 3, color);
  }
}

void drawXyzIcon(int16_t x, int16_t y, uint16_t color) {
  tft.drawRect(x, y, 20, 20, color);
  tft.drawLine(x + 5, y + 5, x + 14, y + 14, color);
  tft.drawLine(x + 14, y + 5, x + 5, y + 14, color);
}

void drawMenuIcon(MenuIcon icon, int16_t x, int16_t y, uint16_t color) {
  if (icon == ICON_DISPLAY) {
    drawDisplayIcon(x, y, color);
  } else if (icon == ICON_INPUT) {
    drawInputIcon(x, y, color);
  } else if (icon == ICON_SYSTEM) {
    drawSystemIcon(x, y, color);
  } else {
    drawXyzIcon(x, y, color);
  }
}

void drawMenuCard(uint8_t index) {
  const int16_t x = 10;
  const int16_t y = 43 + index * 41;
  const int16_t width = 220;
  const int16_t height = 39;
  const bool selected = index == selectedItem;
  const uint16_t fill = selected ? COLOR_MAGENTA : COLOR_SURFACE;
  const uint16_t border = selected ? COLOR_PINK : COLOR_GRID;
  const uint16_t primary = selected ? COLOR_INK : COLOR_TEXT;
  const uint16_t secondary = selected ? COLOR_MAGENTA_DARK : COLOR_MUTED;

  drawPanelShadow(x, y, width, height, 6);
  fillChamferedRect(x, y, width, height, 6, fill);
  drawChamferedRect(x, y, width, height, 6, border);

  char ordinal[4];
  snprintf(ordinal, sizeof(ordinal), "%02u", index + 1);
  drawCenteredText(ordinal, 25, y + 16, 1, secondary);
  drawMenuIcon(MENU_ITEMS[index].icon, 40, y + 9, primary);

  tft.setTextSize(2);
  tft.setTextColor(primary);
  tft.setCursor(68, y + 3);
  tft.print(MENU_ITEMS[index].title);
  tft.setTextSize(1);
  tft.setTextColor(secondary);
  tft.setCursor(69, y + 23);
  tft.print(MENU_ITEMS[index].subtitle);

  tft.drawLine(214, y + 14, 220, y + 18, primary);
  tft.drawLine(220, y + 18, 214, y + 22, primary);
  if (selected) tft.fillRect(224, y + 6, 2, 27, COLOR_PINK);
}

void drawMenuStatus() {
  tft.fillRect(0, 207, SCREEN_W, 6, COLOR_BG);
  for (uint8_t index = 0; index < MENU_ITEM_COUNT; ++index) {
    tft.fillRect(85 + index * 18, 209, 12, 2,
                 index == selectedItem ? COLOR_MAGENTA : COLOR_SURFACE_2);
  }
}

void drawMenu() {
  tft.fillScreen(COLOR_BG);
  drawHeader("BUDDY", "POCKET SYSTEM", "HOME");
  for (uint8_t index = 0; index < MENU_ITEM_COUNT; ++index) {
    drawMenuCard(index);
  }
  drawMenuStatus();
  drawFooter("+", "MOVE", ">", "OPEN");
  menuFocusBright = true;
  menuFocusUpdatedAt = millis();
}

void updateMenuFocus() {
  if (currentPage != MENU || millis() - menuFocusUpdatedAt < 420) return;
  menuFocusUpdatedAt = millis();
  menuFocusBright = !menuFocusBright;
  const int16_t y = 43 + selectedItem * 41;
  const uint16_t color = menuFocusBright ? COLOR_INK : COLOR_PINK;
  tft.drawLine(214, y + 14, 220, y + 18, color);
  tft.drawLine(220, y + 18, 214, y + 22, color);
}

// -----------------------------------------------------------------------------
// Display diagnostics.
// -----------------------------------------------------------------------------

void drawDisplayStageTrack() {
  for (uint8_t index = 0; index < 3; ++index) {
    const int16_t x = 18 + index * 72;
    const uint16_t color = index == displayStage
                               ? COLOR_MAGENTA
                               : (index < displayStage ? COLOR_CYAN : COLOR_GRID);
    tft.fillRect(x, 47, 60, 4, color);
    tft.fillRect(x + 62, 48, 4, 2, index == displayStage ? COLOR_PINK : COLOR_SURFACE_2);
  }
}

void drawDisplayCaption(const char *name) {
  tft.fillRect(0, 193, SCREEN_W, 20, COLOR_BG);
  drawCenteredText(name, 120, 199, 1, COLOR_TEXT);
}

void drawGeometryTest() {
  fillChamferedRect(11, 58, 218, 133, 7, COLOR_SURFACE);
  drawChamferedRect(11, 58, 218, 133, 7, COLOR_GRID);

  for (int16_t x = 24; x <= 216; x += 24) {
    tft.drawFastVLine(x, 68, 111, COLOR_SURFACE_2);
  }
  for (int16_t y = 68; y <= 176; y += 18) {
    tft.drawFastHLine(20, y, 201, COLOR_SURFACE_2);
  }

  tft.drawCircle(120, 124, 42, COLOR_MAGENTA);
  tft.drawCircle(120, 124, 26, COLOR_CYAN);
  tft.drawFastHLine(68, 124, 105, COLOR_GRID);
  tft.drawFastVLine(120, 72, 105, COLOR_GRID);
  tft.fillRect(117, 121, 7, 7, COLOR_PINK);

  tft.drawFastHLine(19, 66, 18, COLOR_MAGENTA);
  tft.drawFastVLine(19, 66, 18, COLOR_MAGENTA);
  tft.drawFastHLine(203, 66, 18, COLOR_CYAN);
  tft.drawFastVLine(220, 66, 18, COLOR_CYAN);
  tft.drawFastHLine(19, 181, 18, COLOR_CYAN);
  tft.drawFastVLine(19, 164, 18, COLOR_CYAN);
  tft.drawFastHLine(203, 181, 18, COLOR_MAGENTA);
  tft.drawFastVLine(220, 164, 18, COLOR_MAGENTA);

  fillChamferedRect(85, 114, 70, 21, 4, COLOR_INK);
  drawCenteredText("240 x 240", 120, 121, 1, COLOR_TEXT);
  drawDisplayCaption("01 // GEOMETRY + ALIGNMENT");
}

void drawGrayscaleTest() {
  fillChamferedRect(11, 58, 218, 133, 7, COLOR_SURFACE);
  drawChamferedRect(11, 58, 218, 133, 7, COLOR_GRID);
  drawCenteredText("8 LEVEL LUMINANCE RAMP", 120, 67, 1, COLOR_MUTED);

  constexpr uint8_t LEVELS[] = {0, 36, 73, 109, 146, 182, 219, 255};
  for (uint8_t index = 0; index < 8; ++index) {
    const int16_t x = 20 + index * 25;
    tft.fillRect(x, 82, 24, 72, gray565(LEVELS[index]));
    tft.drawFastVLine(x, 82, 72, COLOR_GRID);
    if (index == 0 || index == 7) {
      char value[3];
      snprintf(value, sizeof(value), "%02X", LEVELS[index]);
      drawCenteredText(value, x + 12, 163, 1, COLOR_TEXT);
    }
  }
  tft.drawRect(19, 81, 201, 74, COLOR_MUTED);
  tft.setTextSize(1);
  tft.setTextColor(COLOR_MUTED);
  tft.setCursor(20, 177);
  tft.print("BLACK");
  drawRightText("WHITE", 220, 177, 1, COLOR_MUTED);
  drawDisplayCaption("02 // CONTRAST + BANDING");
}

void restoreMotionColumn(int16_t x) {
  if (x < 20 || x > 219) return;
  tft.drawFastVLine(x, 68, 111, COLOR_SURFACE);
  if ((x - 20) % 20 == 0) {
    tft.drawFastVLine(x, 68, 111, COLOR_SURFACE_2);
  } else {
    for (int16_t y = 68; y <= 168; y += 20) {
      tft.drawPixel(x, y, COLOR_SURFACE_2);
    }
  }
}

void drawMotionTestBase() {
  fillChamferedRect(11, 58, 218, 133, 7, COLOR_SURFACE);
  drawChamferedRect(11, 58, 218, 133, 7, COLOR_GRID);
  for (int16_t x = 20; x <= 220; x += 20) {
    tft.drawFastVLine(x, 68, 111, COLOR_SURFACE_2);
  }
  for (int16_t y = 68; y <= 168; y += 20) {
    tft.drawFastHLine(20, y, 201, COLOR_SURFACE_2);
  }

  scannerX = previousScannerX = 20;
  tft.drawFastVLine(scannerX, 68, 111, COLOR_MAGENTA);
  tft.drawFastVLine(scannerX + 1, 68, 111, COLOR_PINK);
  drawCenteredText("LIVE REFRESH SCAN", 120, 181, 1, COLOR_MUTED);
  drawDisplayCaption("03 // MOTION + TEARING");
  scannerUpdatedAt = millis();
}

void drawDisplayTest() {
  tft.fillScreen(COLOR_BG);
  char badge[7];
  snprintf(badge, sizeof(badge), "%02u/03", displayStage + 1);
  drawHeader("DISPLAY", "DIAGNOSTIC DECK", badge);
  drawDisplayStageTrack();

  if (displayStage == 0) {
    drawGeometryTest();
  } else if (displayStage == 1) {
    drawGrayscaleTest();
  } else {
    drawMotionTestBase();
  }
  drawFooter("<", "BACK", ">", "NEXT");
}

void updateMotionTest() {
  if (currentPage != DISPLAY_TEST || displayStage != 2) return;
  if (millis() - scannerUpdatedAt < 32) return;

  restoreMotionColumn(previousScannerX);
  restoreMotionColumn(previousScannerX + 1);
  scannerX += 2;
  if (scannerX > 218) scannerX = 20;
  tft.drawFastVLine(scannerX, 68, 111, COLOR_MAGENTA);
  tft.drawFastVLine(scannerX + 1, 68, 111, COLOR_PINK);
  previousScannerX = scannerX;
  scannerUpdatedAt = millis();
}

// -----------------------------------------------------------------------------
// Input monitor.
// -----------------------------------------------------------------------------

void getButtonCardBounds(Direction direction, int16_t &x, int16_t &y,
                         int16_t &width, int16_t &height) {
  width = (direction == LEFT || direction == RIGHT) ? 87 : 76;
  height = 41;
  if (direction == UP) {
    x = 82;
    y = 48;
  } else if (direction == DOWN) {
    x = 82;
    y = 143;
  } else if (direction == LEFT) {
    x = 9;
    y = 96;
  } else {
    x = 144;
    y = 96;
  }
}

void drawDirectionMark(Direction direction, int16_t centerX, int16_t centerY,
                       uint16_t color) {
  if (direction == UP) {
    tft.drawLine(centerX - 4, centerY + 2, centerX, centerY - 2, color);
    tft.drawLine(centerX, centerY - 2, centerX + 4, centerY + 2, color);
  } else if (direction == DOWN) {
    tft.drawLine(centerX - 4, centerY - 2, centerX, centerY + 2, color);
    tft.drawLine(centerX, centerY + 2, centerX + 4, centerY - 2, color);
  } else if (direction == LEFT) {
    tft.drawLine(centerX + 2, centerY - 4, centerX - 2, centerY, color);
    tft.drawLine(centerX - 2, centerY, centerX + 2, centerY + 4, color);
  } else {
    tft.drawLine(centerX - 2, centerY - 4, centerX + 2, centerY, color);
    tft.drawLine(centerX + 2, centerY, centerX - 2, centerY + 4, color);
  }
}

void drawButtonCard(Direction direction) {
  int16_t x, y, width, height;
  getButtonCardBounds(direction, x, y, width, height);
  const ButtonState &button = buttons[direction];
  const bool seen = buttonTestSeen[direction];
  const bool recent = seen && button.stableState == LOW;
  const uint32_t sessionPresses = button.presses - buttonTestBaseline[direction];
  const uint16_t fill = recent ? COLOR_MAGENTA : COLOR_SURFACE;
  const uint16_t border = recent ? COLOR_PINK : (seen ? COLOR_CYAN : COLOR_GRID);
  const uint16_t primary = recent ? COLOR_INK : COLOR_TEXT;
  const uint16_t secondary = recent ? COLOR_MAGENTA_DARK : COLOR_MUTED;

  drawPanelShadow(x, y, width, height, 6);
  fillChamferedRect(x, y, width, height, 6, fill);
  drawChamferedRect(x, y, width, height, 6, border);
  drawDirectionMark(direction, x + 13, y + 13, primary);

  tft.setTextSize(1);
  tft.setTextColor(primary);
  tft.setCursor(x + 23, y + 8);
  tft.print(button.name);
  char count[18];
  if (sessionPresses > 9999) {
    snprintf(count, sizeof(count), "%s x9K+", button.pinName);
  } else {
    snprintf(count, sizeof(count), "%s x%lu", button.pinName,
             static_cast<unsigned long>(sessionPresses));
  }
  tft.setTextColor(secondary);
  tft.setCursor(x + 12, y + 25);
  tft.print(count);

  tft.fillRect(x + width - 10, y + 7, 4, 4,
               seen ? (recent ? COLOR_INK : COLOR_CYAN) : COLOR_GRID);
  buttonHoldProgress[direction] = 0;
}

void drawDpadCore() {
  fillChamferedRect(101, 96, 38, 41, 7, COLOR_SURFACE_2);
  drawChamferedRect(101, 96, 38, 41, 7, COLOR_GRID);
  tft.drawLine(112, 116, 120, 107, COLOR_MAGENTA);
  tft.drawLine(120, 107, 128, 116, COLOR_CYAN);
  tft.drawLine(128, 116, 120, 125, COLOR_MAGENTA);
  tft.drawLine(120, 125, 112, 116, COLOR_CYAN);
  tft.fillRect(118, 114, 5, 5, COLOR_TEXT);
}

void drawButtonSummary() {
  tft.fillRect(0, 189, SCREEN_W, 24, COLOR_BG);
  char summary[28];
  snprintf(summary, sizeof(summary), "SESSION INPUT  %u / 4",
           buttonTestSeenCount());
  const uint16_t color = buttonTestSeenCount() == 4 ? COLOR_CYAN : COLOR_MUTED;
  drawCenteredText(summary, 120, 197, 1, color);
  for (uint8_t index = 0; index < 4; ++index) {
    tft.fillRect(99 + index * 11, 208, 7, 2,
                 buttonTestSeen[index] ? COLOR_MAGENTA : COLOR_SURFACE_2);
  }
}

void drawButtonTest() {
  tft.fillScreen(COLOR_BG);
  drawHeader("BUTTONS", "LIVE INPUT MONITOR", "4-WAY");
  drawButtonCard(UP);
  drawButtonCard(LEFT);
  drawDpadCore();
  drawButtonCard(RIGHT);
  drawButtonCard(DOWN);
  drawButtonSummary();
  drawFooter("<", "HOLD BACK", ">", "HOLD STATUS");
}

// -----------------------------------------------------------------------------
// System status and non-blocking refresh sequence.
// -----------------------------------------------------------------------------

void drawInfoRow(int16_t y, const char *label, const char *value,
                 uint16_t valueColor) {
  fillChamferedRect(12, y, 216, 30, 5, COLOR_SURFACE);
  drawChamferedRect(12, y, 216, 30, 5, COLOR_GRID);
  tft.fillRect(18, y + 9, 4, 12, valueColor);
  tft.setTextSize(1);
  tft.setTextColor(COLOR_MUTED);
  tft.setCursor(29, y + 11);
  tft.print(label);
  drawRightText(value, 216, y + 11, 1, valueColor);
}

void drawSystemUptimeValue() {
  tft.fillRect(149, 162, 67, 12, COLOR_SURFACE);
  char value[18];
  snprintf(value, sizeof(value), "%lu SEC",
           static_cast<unsigned long>(millis() / 1000));
  drawRightText(value, 216, 164, 1, COLOR_TEXT);
}

void drawSystemInputRow() {
  char inputStatus[18];
  snprintf(inputStatus, sizeof(inputStatus), "%u / 4 SEEN", seenButtonCount());
  drawInfoRow(84, "INPUTS", inputStatus,
              seenButtonCount() == 4 ? COLOR_CYAN : COLOR_MAGENTA);
}

void drawSystemTest() {
  tft.fillScreen(COLOR_BG);
  drawHeader("SYSTEM", "BUDDY CORE STATUS",
             selfTestHasRun && selfTestAnyFailure ? "CHECK" : "LIVE");
  drawInfoRow(48, "DISPLAY", "240x240 / SET", COLOR_CYAN);

  drawSystemInputRow();
  drawInfoRow(120, "AUDIO D9", "NOT FITTED", COLOR_MUTED);
  drawInfoRow(156, "UPTIME", "", COLOR_TEXT);
  drawSystemUptimeValue();

  if (!selfTestHasRun) {
    drawCenteredText("CONFIG LOADED // REV 01", 120, 198, 1, COLOR_MUTED);
  } else {
    drawCenteredText(selfTestAnyFailure ? "LAST REFRESH // CHECK"
                                       : "LAST REFRESH // PASS",
                     120, 198, 1,
                     selfTestAnyFailure ? COLOR_MAGENTA : COLOR_CYAN);
  }
  drawFooter("<", "BACK", ">", "REFRESH");
  systemUpdatedAt = millis();
}

void drawSelfTestRow(uint8_t index, int8_t status) {
  constexpr const char *LABELS[] = {"DISPLAY CFG", "INPUT LINES", "UI FONT"};
  constexpr const char *VALUES[] = {"MODE3 SET", "ALL IDLE", "FONT READY"};
  const bool complete = status != 0;
  const bool passed = status > 0;
  const uint16_t resultColor = passed ? COLOR_CYAN : COLOR_MAGENTA;
  const int16_t y = 57 + index * 42;
  fillChamferedRect(18, y, 204, 32, 5, COLOR_SURFACE);
  drawChamferedRect(18, y, 204, 32, 5,
                    complete ? resultColor : COLOR_GRID);
  tft.fillRect(26, y + 9, 10, 10,
               complete ? resultColor : COLOR_SURFACE_2);
  if (passed) {
    tft.drawLine(28, y + 14, 31, y + 17, COLOR_INK);
    tft.drawLine(31, y + 17, 35, y + 11, COLOR_INK);
  } else if (complete) {
    tft.drawLine(28, y + 11, 34, y + 17, COLOR_INK);
    tft.drawLine(34, y + 11, 28, y + 17, COLOR_INK);
  }
  tft.setTextSize(1);
  tft.setTextColor(complete ? COLOR_TEXT : COLOR_MUTED);
  tft.setCursor(45, y + 12);
  tft.print(LABELS[index]);
  drawRightText(!complete ? "WAIT" : (passed ? VALUES[index] : "CHECK"),
                210, y + 12, 1,
                complete ? resultColor : COLOR_MUTED);
}

bool runSelfTestCheck(uint8_t index) {
  if (index == 0) {
    return TFT_SPI_HZ == 4000000 && TFT_CS == -1;
  }
  if (index == 1) {
    for (const ButtonState &button : buttons) {
      if (button.stableState == LOW) return false;
    }
    return true;
  }
  return tft.fontReady();
}

void drawSelfTest() {
  tft.fillScreen(COLOR_BG);
  drawHeader("SYSTEM", "STATUS REFRESH", "RUN");
  for (uint8_t index = 0; index < 3; ++index) {
    drawSelfTestRow(index, 0);
  }
  drawCenteredText("VALIDATING CONFIG...", 120, 186, 1, COLOR_MUTED);
  drawChamferedRect(14, 199, 212, 9, 3, COLOR_GRID);
  drawFooter("<", "CANCEL", "", "AUTO RETURN");
  selfTestStartedAt = millis();
  selfTestStep = 0;
  selfTestProgressDrawn = 0;
}

void startSelfTest() {
  currentPage = SELF_TEST;
  navigationPending = false;
  selfTestRunAnyFailure = false;
  selfTestRunCommitted = false;
  for (bool &result : selfTestResults) result = false;
  ESP_LOGI(TAG, "Status refresh started");
  drawSelfTest();
}

void updateSelfTest() {
  if (currentPage != SELF_TEST) return;
  const uint32_t elapsed = millis() - selfTestStartedAt;
  const uint8_t targetStep = elapsed >= 840 ? 3 : elapsed / 280;

  while (selfTestStep < targetStep) {
    const bool passed = runSelfTestCheck(selfTestStep);
    selfTestResults[selfTestStep] = passed;
    selfTestRunAnyFailure |= !passed;
    drawSelfTestRow(selfTestStep, passed ? 1 : -1);
    ++selfTestStep;
  }
  if (selfTestStep == 3 && !selfTestRunCommitted) {
    selfTestHasRun = true;
    selfTestAnyFailure = selfTestRunAnyFailure;
    selfTestRunCommitted = true;
  }

  const uint16_t targetWidth = elapsed >= 1100 ? 204 : elapsed * 204 / 1100;
  if (targetWidth > selfTestProgressDrawn) {
    tft.fillRect(18 + selfTestProgressDrawn, 202,
                 targetWidth - selfTestProgressDrawn, 3, COLOR_MAGENTA);
    selfTestProgressDrawn = targetWidth;
  }

  const uint32_t returnAt = selfTestRunAnyFailure ? 2600 : 1500;
  if (selfTestStep == 3 && elapsed >= returnAt) {
    ESP_LOGI(TAG, "Status refresh complete");
    currentPage = SYSTEM_TEST;
    drawSystemTest();
  }
}

void updateSystemUptime() {
  if (currentPage != SYSTEM_TEST) return;
  if (millis() - systemUpdatedAt < 1000) return;
  systemUpdatedAt = millis();
  drawSystemUptimeValue();
}

// -----------------------------------------------------------------------------
// XYZ module.
// -----------------------------------------------------------------------------

void drawXyzScreen() {
  tft.fillScreen(COLOR_BG);
  drawHeader("XYZ", "CUSTOM MODE", "LIVE");
  fillChamferedRect(20, 69, 200, 126, 8, COLOR_SURFACE);
  drawChamferedRect(20, 69, 200, 126, 8, COLOR_GRID);
  drawCenteredText("ACTIVE", 120, 110, 3, COLOR_CYAN);
  drawCenteredText("XYZ MODULE", 120, 157, 1, COLOR_MUTED);
  drawFooter("<", "BACK", "", "");
}

void runXyz() {
  if (currentPage != XYZ_SCREEN) return;
  // TODO: implement XYZ behavior here. Called on every loop while active.
}

// -----------------------------------------------------------------------------
// Navigation and input.
// -----------------------------------------------------------------------------

void startButtonTest() {
  currentPage = BUTTON_TEST;
  navigationPending = false;
  for (uint8_t index = 0; index < DIRECTION_COUNT; ++index) {
    buttonTestSeen[index] = false;
    buttonTestBaseline[index] = buttons[index].presses;
    buttonHoldProgress[index] = 0;
  }
  drawButtonTest();
}

void openSelectedPage() {
  if (selectedItem == 0) {
    currentPage = DISPLAY_TEST;
    displayStage = 0;
    drawDisplayTest();
  } else if (selectedItem == 1) {
    startButtonTest();
  } else if (selectedItem == 2) {
    currentPage = SYSTEM_TEST;
    drawSystemTest();
  } else {
    currentPage = XYZ_SCREEN;
    drawXyzScreen();
  }
}

void scheduleNavigation(Page page, uint32_t delayMs = 150) {
  pendingPage = page;
  navigationAt = millis() + delayMs;
  navigationPending = true;
}

void updatePendingNavigation() {
  if (!navigationPending) return;
  if (static_cast<int32_t>(millis() - navigationAt) < 0) return;
  navigationPending = false;
  currentPage = pendingPage;
  if (currentPage == MENU) {
    drawMenu();
  } else if (currentPage == SYSTEM_TEST) {
    drawSystemTest();
  }
}

void handlePress(Direction direction) {
  ButtonState &button = buttons[direction];
  ++button.presses;
  button.seen = true;
  button.pressedAt = millis();
  button.holdHandled = false;
  lastDirection = direction;

  ESP_LOGI(TAG, "Button %s (%s) pressed", button.name, button.pinName);

  if (currentPage == SPLASH) {
    currentPage = MENU;
    drawMenu();
    return;
  }

  if (currentPage == MENU) {
    if (direction == UP || direction == DOWN) {
      const uint8_t previousItem = selectedItem;
      if (direction == UP) {
        selectedItem = (selectedItem + MENU_ITEM_COUNT - 1) % MENU_ITEM_COUNT;
      } else {
        selectedItem = (selectedItem + 1) % MENU_ITEM_COUNT;
      }
      drawMenuCard(previousItem);
      drawMenuCard(selectedItem);
      drawMenuStatus();
    } else if (direction == RIGHT) {
      openSelectedPage();
    }
    return;
  }

  if (currentPage == DISPLAY_TEST) {
    if (direction == LEFT) {
      currentPage = MENU;
      drawMenu();
    } else if (direction == RIGHT) {
      displayStage = (displayStage + 1) % 3;
      drawDisplayTest();
    }
    return;
  }

  if (currentPage == BUTTON_TEST) {
    buttonTestSeen[direction] = true;
    drawButtonCard(direction);
    drawButtonSummary();
    return;
  }

  if (currentPage == SYSTEM_TEST) {
    if (direction == LEFT) {
      currentPage = MENU;
      drawMenu();
    } else if (direction == RIGHT) {
      startSelfTest();
    } else {
      drawSystemInputRow();
    }
    return;
  }

  if (currentPage == XYZ_SCREEN) {
    if (direction == LEFT) {
      currentPage = MENU;
      drawMenu();
    }
    return;
  }

  if (currentPage == SELF_TEST && direction == LEFT) {
    currentPage = SYSTEM_TEST;
    drawSystemTest();
  }
}

void handleRelease(Direction direction) {
  if (currentPage == BUTTON_TEST && buttonTestSeen[direction]) {
    drawButtonCard(direction);
  }
}

void updateButtonTestHolds() {
  if (currentPage != BUTTON_TEST || navigationPending) return;
  const uint32_t now = millis();

  constexpr Direction NAV_DIRECTIONS[] = {LEFT, RIGHT};
  for (Direction direction : NAV_DIRECTIONS) {
    ButtonState &button = buttons[direction];
    if (!buttonTestSeen[direction] || button.stableState != LOW ||
        button.holdHandled) continue;

    const uint32_t elapsed = now - button.pressedAt;
    const uint8_t progress = elapsed >= 650 ? 63 : elapsed * 63 / 650;
    if (progress > buttonHoldProgress[direction]) {
      int16_t x, y, width, height;
      getButtonCardBounds(direction, x, y, width, height);
      tft.fillRect(x + 12 + buttonHoldProgress[direction], y + 36,
                   progress - buttonHoldProgress[direction], 2, COLOR_INK);
      buttonHoldProgress[direction] = progress;
    }
    if (elapsed >= 650) {
      button.holdHandled = true;
      scheduleNavigation(direction == LEFT ? MENU : SYSTEM_TEST, 0);
      return;
    }
  }
}

void pollButtons() {
  const uint32_t now = millis();
  const bool gateWasActive = inputReleaseGate;
  int8_t pressedIndex = -1;
  bool released[DIRECTION_COUNT] = {false, false, false, false};

  for (uint8_t index = 0; index < DIRECTION_COUNT; ++index) {
    ButtonState &button = buttons[index];
    const bool sample = digitalRead(button.pin);

    if (sample != button.rawState) {
      button.rawState = sample;
      button.changedAt = now;
    }

    if ((now - button.changedAt >= DEBOUNCE_MS) &&
        (button.stableState != button.rawState)) {
      button.stableState = button.rawState;
      if (button.stableState == LOW) {
        if (!gateWasActive && pressedIndex < 0) {
          pressedIndex = index;
        } else {
          // Treat a same-scan chord as one action. The suppressed direction is
          // still debounced, but cannot unexpectedly navigate the new page.
          button.pressedAt = now;
          button.holdHandled = true;
        }
      } else {
        released[index] = true;
      }
    }
  }

  // Dispatch only after all electrical states are settled so a handler cannot
  // change currentPage halfway through scanning the remaining pins.
  for (uint8_t index = 0; index < DIRECTION_COUNT; ++index) {
    if (released[index]) handleRelease(static_cast<Direction>(index));
  }
  if (gateWasActive) {
    bool allReleased = true;
    for (const ButtonState &button : buttons) {
      allReleased &= button.stableState == HIGH && button.rawState == HIGH;
    }
    if (allReleased) inputReleaseGate = false;
    return;
  }
  if (pressedIndex >= 0) {
    handlePress(static_cast<Direction>(pressedIndex));
    inputReleaseGate = true;
  }
}

// -----------------------------------------------------------------------------
// Hardware initialization.
// -----------------------------------------------------------------------------

void initializeButtons() {
  for (ButtonState &button : buttons) {
    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << button.pin;
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&config));
    const bool state = digitalRead(button.pin);
    button.rawState = state;
    button.stableState = state;
    button.changedAt = millis();
  }
}

void armStartInput() {
  // The display initialization takes time. Resample at the visible start
  // screen so a button held at power-on (or pressed during initialization)
  // generates exactly one debounced start event.
  inputReleaseGate = false;
  const uint32_t now = millis();
  for (ButtonState &button : buttons) {
    button.rawState = digitalRead(button.pin);
    button.stableState = HIGH;
    button.changedAt = now;
    button.holdHandled = false;
  }
}

void initializeDisplayMode3() { tft.init(); }

void setup() {
  delay(250);
  initializeButtons();
  gpio_config_t backlight = {};
  backlight.pin_bit_mask = 1ULL << TFT_BL;
  backlight.mode = GPIO_MODE_OUTPUT;
  ESP_ERROR_CHECK(gpio_config(&backlight));
  ESP_ERROR_CHECK(gpio_set_level(static_cast<gpio_num_t>(TFT_BL), 0));
  initializeDisplayMode3();

  ESP_LOGI(TAG, "Buddy pocket UI started");
  ESP_LOGI(TAG, "TFT: rotation 180, SPI MODE3 at 4 MHz, CS tied to GND");
  ESP_LOGI(TAG, "Controls: UP/DOWN select, RIGHT enter, LEFT back");
  ESP_LOGI(TAG, "Buzzer: disabled / D9 untouched");

  currentPage = SPLASH;
  drawSplash();
  armStartInput();
  ESP_ERROR_CHECK(gpio_set_level(static_cast<gpio_num_t>(TFT_BL), 1));
}

void loop() {
  pollButtons();
  updateButtonTestHolds();
  updatePendingNavigation();
  updateSplash();
  updateMenuFocus();
  updateMotionTest();
  updateSystemUptime();
  updateSelfTest();
  runXyz();
  delay(2);
}

extern "C" void app_main(void) {
  setup();
  while (true) loop();
}
