# Buddy
<img src="https://github.com/user-attachments/assets/a18e5d0c-eec3-4ff3-9fd1-9f487c830203"
     alt="Buddy"
     width="400">
     
Buddy is a compact handheld device built around a Seeed Studio XIAO ESP32-C5,
a 1.54-inch 240 x 240 ST7789 TFT, and four tactile direction buttons. The
current revision is powered over USB-C. A protected 1-cell Li-Po battery and an
on/off switch may be added later.

## Verified display wiring

The labels `SCL` and `SDA` on this TFT module are SPI signals, not I2C.

| ST7789 module | XIAO ESP32-C5 | ESP32-C5 GPIO | Purpose |
|---|---|---:|---|
| `GND` | `GND` | — | Common ground |
| `VCC` | `3V3` | — | Display power |
| `SCL` | `D8` | GPIO8 | SPI clock (`SCK`) |
| `SDA` | `D10` | GPIO10 | SPI data (`MOSI`) |
| `RST` | `D2` | GPIO25 | Display reset, active LOW |
| `DC` | `D6` | GPIO11 | Data/command selection |
| `CS` | `GND` | — | Display permanently selected |
| `BL` | `D7` | GPIO12 | Backlight, active HIGH |

With the header at the top, the module markings read:

```text
Front/viewing side: GND VCC SCL SDA RST DC CS BL
Rear/PCB side:     BL CS DC RST SDA SCL VCC GND
```

`CS` is intentionally connected directly to ground because the display is the
only device on the SPI bus. In code, use `TFT_CS = -1`. If another SPI device is
added later, `CS` must be disconnected from ground and controlled by a GPIO.

## Display settings confirmed on the real hardware

This module works with the following configuration:

```cpp
SPI.begin(D8, -1, D10, -1);       // SCK, MISO, MOSI, SS
tft.init(240, 240, SPI_MODE3);
tft.setSPISpeed(4000000);         // 4 MHz
tft.setRotation(2);               // user interface rotated by 180 degrees
```

## Button wiring

Each tactile switch is connected between its GPIO and `GND`. The firmware uses
`INPUT_PULLUP`, so a released button reads `HIGH` and a pressed button reads
`LOW`. No external resistors are required.

| Logical button | XIAO pin | ESP32-C5 GPIO | Other switch terminal |
|---|---|---:|---|
| `UP` | `D0` | GPIO1 | `GND` |
| `DOWN` | `D5` | GPIO24 | `GND` |
| `LEFT / BACK` | `D1` | GPIO0 | `GND` |
| `RIGHT / ENTER` | `D4` | GPIO23 | `GND` |

On a standard four-leg tact switch, the two legs on each side are internally
connected. Connect the GPIO and ground wires to opposite sides of the switch.

## User interface controls

| Screen | `UP` / `DOWN` | `LEFT` | `RIGHT` |
|---|---|---|---|
| Start | Any direction opens the menu | Same | Same |
| Menu | Move selection; wraps at both ends | No action | Open module |
| Display | No action | Back to menu | Next of three tests; wraps |
| Buttons | Register and highlight | Tap to test; hold 650 ms for menu | Tap to test; hold 650 ms for status |
| System | No action | Back to menu | Run status refresh |
| Refresh | No action | Cancel | No action |

The display module includes geometry/alignment, eight grayscale levels, and an
animated refresh scan. The button monitor starts a clean four-button session
each time it opens, so the `RIGHT` press used to enter is not counted. The
active `LEFT`/`RIGHT` card fills a small progress bar during its 650 ms hold.
Holding any direction while Buddy powers on also starts the menu once the
opening screen is ready. The system status separately retains the lifetime
`seen` state. All diagnostic
button events are printed over USB serial at 115200 baud when the board's
default **USB CDC On Boot** option is enabled. If USB CDC is disabled, firmware
omits serial logging so hardware UART cannot claim GPIO11/GPIO12, which are
already used by TFT `DC` and backlight.

The UI deliberately avoids a full framebuffer. Static pages are rendered once,
while the blinking start prompt, signal meter, motion scanner, uptime, button
highlights, and status progress use small dirty regions. This keeps RAM usage
low and avoids unnecessary full-screen transfers over the verified 4 MHz SPI
link.

The system refresh is a non-blocking status sequence for the configured display
link, input matrix, and embedded UI asset. It does not electrically probe the
display because this write-only SPI wiring has no MISO connection.

## Pin allocation

| XIAO pin | Current use |
|---|---|
| `D0` | UP button |
| `D1` | LEFT/BACK button |
| `D2` | TFT reset |
| `D3` | Reserved; review boot/JTAG use before assigning it |
| `D4` | RIGHT/ENTER button |
| `D5` | DOWN button |
| `D6` | TFT data/command |
| `D7` | TFT backlight |
| `D8` | TFT SPI clock |
| `D9` | Free; reserved for an optional passive piezo buzzer |
| `D10` | TFT SPI MOSI |

The buzzer is **not connected in the current prototype**, and the working
firmware leaves `D9` untouched. If a passive piezo is added later, connect its
positive terminal to `D9/GPIO9` and its negative terminal to `GND`, then drive
it with PWM rather than a constant DC level.

## Firmware build and upload

Required Arduino libraries:

- Adafruit GFX Library
- Adafruit ST7735 and ST7789 Library

Compile:

```bash
arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32C5 BuddyHardwareTest
```

Find the USB port and upload:

```bash
arduino-cli board list
arduino-cli compile --upload \
  --fqbn esp32:esp32:XIAO_ESP32C5 \
  -p /dev/cu.usbmodemXXXX \
  BuddyHardwareTest
```

## Future battery connection

Use only a protected 1-cell Li-Po/Li-ion battery: 3.7 V nominal and 4.2 V fully
charged. Connect it only to the XIAO battery pads, observing polarity.

```text
Battery + ---- power switch ---- BAT+
Battery - ---------------------- BAT-
```

Do not connect the battery to `3V3` or `5V/VBUS`. Before connecting USB or a
battery, check for a short between `3V3` and `GND` with a multimeter.

## Enclosure files

<img width="401" alt="Buddy enclosure" src="https://github.com/user-attachments/assets/70e00686-5db9-47f7-99d9-fc7b45777f95">

<br>

[📦 **Download enclosure files on MakerWorld**](https://makerworld.com/pl/models/3354005-buddy-seeed-studio-xiao-esp32-c5-1-54-tft#profileId-3812397)
