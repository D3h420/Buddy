# Buddy handheld

Buddy is a compact handheld device built around a Seeed Studio XIAO ESP32-C5,
a 1.54-inch 240 x 240 ST7789 TFT, and four tactile direction buttons. The
current revision is powered over USB-C. A protected 1-cell Li-Po battery and an
on/off switch may be added later.

The current Arduino firmware is located at
[`BuddyHardwareTest/BuddyHardwareTest.ino`](./BuddyHardwareTest/BuddyHardwareTest.ino).
It combines the verified hardware diagnostics with a complete retro-anime
interface: an interactive attract screen, a magenta-focused main menu, display
tests, a live four-button monitor, system status, and a non-blocking status
refresh.
Printable enclosure files are stored in [`STL/`](./STL/).
Current screen captures are in
[`BuddyHardwareTest/previews/`](./BuddyHardwareTest/previews/). The original
test sketch is preserved in `BuddyHardwareTest/legacy/` and is not compiled.

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

Render all screens and run the host navigation checks on macOS:

```bash
sh BuddyHardwareTest/tools/preview.sh
```

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

## Boot artwork pipeline

The original generated artwork and the exact 240 x 240 preview are in
[`BuddyHardwareTest/assets/`](./BuddyHardwareTest/assets/). The firmware embeds
the image as a 59-entry RGB565 palette plus run-length encoded pixel data in
`BuddyBootAsset.h`; the converter starts from a 64-color quantization target,
then merges colors that are identical on the display. Decoding uses only a
single 240-pixel scanline buffer.

The converter uses only the Python standard library. On macOS, rebuild the
header with:

```bash
sips -z 240 240 BuddyHardwareTest/assets/buddy_boot_source.png \
  --out /tmp/buddy_boot_240.png
sips -s format bmp /tmp/buddy_boot_240.png \
  --out /tmp/buddy_boot_240.bmp
python3 BuddyHardwareTest/tools/image_to_palette.py \
  /tmp/buddy_boot_240.bmp BuddyHardwareTest/BuddyBootAsset.h \
  --preview /tmp/buddy_boot_quantized.ppm --colors 64
sips -s format png /tmp/buddy_boot_quantized.ppm \
  --out BuddyHardwareTest/assets/buddy_boot_240.png
```

The generation brief and asset provenance are documented in
[`BuddyHardwareTest/assets/README.md`](./BuddyHardwareTest/assets/README.md).

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

The final print-ready revision 2.19 meshes exported directly from the current
manually rounded Fusion model are in `STL/`: `Base.stl`, `Lid_fixed.stl`,
`Buttons_Standard.stl`, and `Lanyard_Handle.stl`. Each file contains exactly
one watertight solid. The rejected low button variant has been removed.
`Buttons_Standard.stl` now has a thin raised direction chevron on every cap:
up, down, left, and right. Each symbol uses only two rounded lines rather than
a filled arrow. The enlarged lines are approximately 1.10 mm wide, rise 0.40 mm
above the restored flat cap surface and cover almost the entire button face. In this revision, the USB-C opening is vertically aligned
with the on/off switch opening: both centre lines are at `Z = 10.20 mm`. The
USB-C opening remains `12.00 x 5.00 mm` with R1.00 corners, and the corrected
on/off opening is `12.50 x 6.00 mm`. The locally rebuilt wall remains flush
with the surrounding face and preserves the original R3.50 lower-edge fillet,
without a raised rim or rectangular step. `Lid_fixed.stl` keeps the accepted
2.50 mm of additional internal clearance while retaining the original
openings, pockets and base-compatible snap geometry.

`Lanyard_Handle.stl` is a separate `20 x 11 x 5 mm` glue-on or integration
part with a 20 mm lower base, 7.5 mm upper edge, diagonal right shoulder and a
manually rounded through-hole.

The `cad/` directory contains the editable Fusion archive, STEP export, and
four separate STL files: the base, full-perimeter snap-fit front lid,
one-piece four-button insert with directional chevrons, and lanyard handle. It also contains the current
Fusion viewport and the
preceding revision's photorealistic and blueprint concept previews. The source
generator is
[`FusionCaseGenerator/FusionCaseGenerator.py`](./FusionCaseGenerator/FusionCaseGenerator.py).
