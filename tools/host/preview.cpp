#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include "../../BuddyHardwareTest.ino"

void save(const std::filesystem::path &directory, const char *name) {
  std::ofstream file(directory / (std::string(name) + ".ppm"), std::ios::binary);
  file << "P6\n240 240\n255\n";
  for (int i = 0; i < 240 * 240; ++i) {
    const uint16_t color = tft.getBuffer()[i];
    const char rgb[] = {
        static_cast<char>(((color >> 11) & 31) * 255 / 31),
        static_cast<char>(((color >> 5) & 63) * 255 / 63),
        static_cast<char>((color & 31) * 255 / 31)};
    file.write(rgb, 3);
  }
}

void settle() { pollButtons(); hostTime += 31; pollButtons(); }
void press(Direction direction) { hostPins[buttons[direction].pin] = LOW; settle(); }
void release(Direction direction) { hostPins[buttons[direction].pin] = HIGH; settle(); }
void tap(Direction direction) { press(direction); release(direction); }

int main(int argc, char **argv) {
  assert(argc == 2);
  const std::filesystem::path directory(argv[1]);
  std::filesystem::create_directories(directory);
  for (int &pin : hostPins) pin = HIGH;
  initializeButtons();
  tft.setTextWrap(false);
  drawSplash();
  save(directory, "01-start");

  // A held start button opens only the menu, never its selected module.
  press(RIGHT);
  assert(currentPage == MENU);
  hostTime += 800; pollButtons();
  assert(currentPage == MENU);
  release(RIGHT);
  tap(UP); assert(selectedItem == 2);
  tap(DOWN); assert(selectedItem == 0);
  save(directory, "02-menu");
  tap(DOWN); save(directory, "03-menu-buttons");
  press(RIGHT);
  assert(currentPage == BUTTON_TEST && buttonTestSeenCount() == 0);
  hostTime += 1000; updateButtonTestHolds(); updatePendingNavigation();
  assert(currentPage == BUTTON_TEST); // Entry press must not trigger a hold.
  release(RIGHT);
  save(directory, "04-buttons-empty");
  tap(UP); tap(DOWN); tap(LEFT);
  assert(currentPage == BUTTON_TEST && buttonTestSeenCount() == 3);
  press(RIGHT);
  assert(buttonTestSeenCount() == 4);
  hostTime += 250;
  updateButtonTestHolds();
  assert(buttonHoldProgress[RIGHT] > 0 && buttonHoldProgress[RIGHT] < 63);
  save(directory, "05-buttons-active");
  hostTime += 399; updateButtonTestHolds(); updatePendingNavigation();
  assert(currentPage == BUTTON_TEST);
  ++hostTime; updateButtonTestHolds(); updatePendingNavigation();
  assert(currentPage == SYSTEM_TEST);
  release(RIGHT);
  save(directory, "06-system");

  tap(RIGHT);
  assert(currentPage == SELF_TEST);
  hostTime += 900; updateSelfTest();
  assert(selfTestHasRun && !selfTestAnyFailure);
  save(directory, "07-refresh");
  hostTime += 700; updateSelfTest();
  assert(currentPage == SYSTEM_TEST);
  tap(RIGHT); tap(LEFT);
  assert(currentPage == SYSTEM_TEST && selfTestHasRun && !selfTestAnyFailure);
  assert(runSelfTestCheck(2));

  // Navigation/hold thresholds remain correct across the 32-bit millis wrap.
  tap(LEFT);
  selectedItem = 0; tap(RIGHT);
  assert(currentPage == DISPLAY_TEST);
  save(directory, "08-display-geometry");
  tap(RIGHT); save(directory, "09-display-grayscale");
  tap(RIGHT); hostTime += 33; updateMotionTest();
  save(directory, "10-display-motion");
  tap(RIGHT); assert(displayStage == 0);
  tap(LEFT); selectedItem = 1; tap(RIGHT);
  hostTime = UINT32_MAX - 100;
  press(LEFT); hostTime += 650;
  updateButtonTestHolds(); updatePendingNavigation();
  assert(currentPage == MENU);
  release(LEFT);

  // A chord whose second edge debounces later cannot skip the start menu.
  currentPage = SPLASH; inputReleaseGate = false;
  hostPins[BTN_UP] = LOW; pollButtons();
  hostTime += 5; hostPins[BTN_RIGHT] = LOW; pollButtons();
  hostTime += 26; pollButtons();
  assert(currentPage == MENU);
  hostTime += 5; pollButtons();
  assert(currentPage == MENU);
  release(UP); release(RIGHT);

  // Any held direction during actual setup starts once, including RIGHT.
  for (uint8_t index = 0; index < DIRECTION_COUNT; ++index) {
    for (int &pin : hostPins) pin = HIGH;
    hostPins[buttons[index].pin] = LOW;
    setup();
    assert(currentPage == SPLASH);
    hostTime += 29; pollButtons();
    assert(currentPage == SPLASH);
    hostTime += 1; pollButtons();
    assert(currentPage == MENU);
    hostTime += 1000; pollButtons();
    assert(currentPage == MENU);
    release(static_cast<Direction>(index));
  }
  std::cout << "PASS: boot (including held inputs), wrap navigation, clean input session, hold feedback, refresh, cancel, RLE, timer rollover, chords\n";
}
