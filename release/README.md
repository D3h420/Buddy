# Buddy firmware build

Built for `esp32:esp32:XIAO_ESP32C5`, with the default USB CDC setting, on
2026-10-03. Flash usage: 399,786 bytes (11%). Global RAM: 18,908 bytes (5%).

Dependencies: ESP32 Arduino core 3.3.10, Adafruit GFX 1.12.6,
Adafruit ST7735/ST7789 1.11.0, Adafruit BusIO 1.17.4.

The compile succeeds with `--warnings all`; remaining warnings originate in
ESP32 core headers. Host navigation checks pass, including all four directions
held at power-on and visible hold progress. All ten screen captures
have been visually inspected. Physical LCD appearance and button response
still require checking on Buddy; no matching USB device was available at build
time, so these binaries have not been uploaded.

Upload the exported build after finding the actual port:

```bash
arduino-cli board list
arduino-cli upload --fqbn esp32:esp32:XIAO_ESP32C5 \
  -p /dev/cu.usbmodemXXXX \
  --input-dir BuddyHardwareTest/release BuddyHardwareTest
```

`BuddyHardwareTest.ino.bin` is the application; the adjacent bootloader and
partition images accompany the Arduino CLI upload. The merged image contains
the complete flash layout. ELF/map files are retained for debugging.
