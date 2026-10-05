# Buddy — ESP-IDF

![Buddy logo](buddy_chrome.png)

Buddy is a small device built around a Seeed Studio XIAO ESP32-C5, a 240 × 240
ST7789 display, and four directional buttons. The firmware is a native ESP-IDF
project without Arduino. It includes a **BUDDY** splash screen, a menu, three
display tests, a button monitor, system status, and Lab Tester.

## Wiring

The display module's `SCL` and `SDA` labels refer to SPI signals, not I²C. The
display is the only device on the bus, and its `CS` pin is connected to GND.

| ST7789 | XIAO ESP32-C5 | GPIO | Function |
|---|---|---:|---|
| GND | GND | — | Ground |
| VCC | 3V3 | — | Power |
| SCL | D8 | 8 | SPI SCK |
| SDA | D10 | 10 | SPI MOSI |
| RST | D2 | 25 | Reset |
| DC | D6 | 11 | Data / command |
| CS | GND | — | Always active |
| BL | D7 | 12 | Backlight, active HIGH |

The display uses **SPI MODE3, 4 MHz, RGB565, and 180° rotation**. The firmware
draws directly over SPI without a full-screen framebuffer.

Each button connects its GPIO pin to GND. Internal pull-ups are enabled, so LOW
means the button is pressed.

| Direction | XIAO | GPIO |
|---|---|---:|
| UP | D0 | 1 |
| DOWN | D5 | 24 |
| LEFT / BACK | D1 | 0 |
| RIGHT / ENTER | D4 | 23 |

GPIO9 / D9 remains free; no buzzer is connected.

## Controls

| Screen | UP / DOWN | LEFT | RIGHT |
|---|---|---|---|
| Splash | Open menu | Open menu | Open menu |
| Menu | Change selection | — | Open selection |
| Display | — | Return to menu | Next test |
| Buttons | Test press | Test press; hold for 650 ms to return | Test press; hold for 650 ms to open status |
| System | — | Return to menu | Refresh status |
| Lab Tester | — | Hold for 650 ms to stop and return to menu | — |
| Refresh | — | Cancel | — |

Button presses are debounced for 30 ms. The `RIGHT` press used to open the button
monitor is not counted in the new session. Status refresh checks the display
configuration, button states, and font. The SPI connection has no MISO line, so
the status screen does not electrically test the display.

## Build and flash

The project builds with **ESP-IDF 5.5.3** for the `esp32c5` target:

```sh
. /path/to/esp-idf/export.sh
idf.py -DIDF_TARGET=esp32c5 build
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

Choose the port for the connected board. `sdkconfig.defaults` routes the
console to USB Serial/JTAG so UART does not occupy GPIO11 and GPIO12. The built
application is `build/buddy.bin`; the entire `build/` directory is generated
locally and ignored by Git.

The UI and button handling are in `main/buddy.cpp`, the pin map is in
`main/buddy_board.h`, and the ST7789 driver is in `main/buddy_display.cpp`. The
5 × 7 font uses Adafruit GFX data under the BSD license (`main/font.LICENSE`).
Lab Tester starts the Blackout module through its public interface. Holding
LEFT stops the module and returns to the menu.

The build has been verified locally. Display appearance and physical button
behavior still require checking on the connected device.

## Enclosure

[Enclosure files on MakerWorld](https://makerworld.com/pl/models/3354005-buddy-seeed-studio-xiao-esp32-c5-1-54-tft#profileId-3812397).
