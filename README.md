# Buddy — ESP-IDF
<img src="https://github.com/user-attachments/assets/a18e5d0c-eec3-4ff3-9fd1-9f487c830203"
     alt="Buddy"
     width="400">

Buddy is a compact handheld device built around a Seeed Studio XIAO ESP32-C5,
a 1.54-inch 240 x 240 ST7789 TFT, and four tactile direction buttons. The
current revision is powered over USB-C. A protected 1-cell Li-Po battery and an
on/off switch may be added later.

Natywny projekt ESP-IDF dla Seeed Studio XIAO ESP32-C5, wyświetlacza ST7789
240 × 240 i czterech przycisków. Wersja nie używa Arduino ani bibliotek
Adafruit w firmware. Zachowuje menu, test geometrii / skali szarości / ruchu,
monitor przycisków i ekran systemowy z odświeżaniem statusu. Intro zostało
uproszczone do napisu **BUDDY**.

## Sprzęt

Wyświetlacz jest jedynym urządzeniem SPI. Oznaczenia `SCL` i `SDA` na module
oznaczają linie SPI, nie I²C. `CS` jest połączony na stałe z GND.

## Verified display wiring

| ST7789 | XIAO ESP32-C5 | GPIO | Funkcja |
|---|---|---:|---|

| ST7789 | XIAO ESP32-C5 | GPIO | Funkcja |
|---|---|---:|---|
| GND | GND | — | Masa |
| VCC | 3V3 | — | Zasilanie |
| SCL | D8 | 8 | SCK |
| SDA | D10 | 10 | MOSI |
| RST | D2 | 25 | Reset |
| DC | D6 | 11 | Dane / komenda |
| CS | GND | — | Stale aktywny |
| BL | D7 | 12 | Podświetlenie, aktywne HIGH |

Ustawienia wyświetlacza: **SPI MODE3, 4 MHz, RGB565, 240 × 240, obrót 180°**.
Sterownik wysyła tę samą konfigurację ST7789 co poprzedni szkic. Nie używa
bufora całego ekranu; rysuje fragmenty bezpośrednio przez SPI.

Każdy przycisk łączy odpowiednie GPIO z GND. Wewnętrzne podciąganie jest
włączone; stan LOW oznacza naciśnięcie.

| Kierunek | XIAO | GPIO |
|---|---|---:|
| UP | D0 | 1 |
| DOWN | D5 | 24 |
| LEFT / BACK | D1 | 0 |
| RIGHT / ENTER | D4 | 23 |

GPIO9 / D9 pozostaje wolny. Brzęczyk nie jest podłączony.

## Sterowanie

| Ekran | UP / DOWN | LEFT | RIGHT |
|---|---|---|---|
| Start | Dowolny kierunek otwiera menu | To samo | To samo |
| Menu | Wybór pozycji z zawijaniem | — | Otwórz pozycję |
| Display | — | Menu | Następny z 3 testów |
| Buttons | Zarejestruj naciśnięcie | Test; przytrzymaj 650 ms, aby wrócić do menu | Test; przytrzymaj 650 ms, aby przejść do statusu |
| System | — | Menu | Odśwież status |
| Refresh | — | Anuluj | — |

Program filtruje drgania styków przez 30 ms i nie liczy przycisku `RIGHT`,
którym otwarto monitor przycisków. Kierunek trzymany podczas uruchamiania
otworzy menu po wyświetleniu ekranu startowego. Odświeżanie statusu sprawdza
konfigurację wyświetlacza, spoczynkowe stany przycisków i wbudowaną czcionkę.
Nie jest to pomiar elektryczny wyświetlacza, ponieważ SPI nie ma linii MISO.

## Kompilacja i wgrywanie

Projekt sprawdzono z **ESP-IDF 5.5.3** i targetem `esp32c5`.

Projekt sprawdzono z **ESP-IDF 5.5.3** i targetem `esp32c5`.

```sh
. /ścieżka/do/esp-idf/export.sh
idf.py -DIDF_TARGET=esp32c5 build
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

Na innych systemach wpisz właściwy port szeregowy. `sdkconfig.defaults`
ustawia konsolę na USB Serial/JTAG, aby UART nie przejmował GPIO11 i GPIO12
używanych przez ekran. Logi przycisków są wysyłane przez USB z prędkością
115200 bit/s. Pliki wynikowe znajdują się w `build/`, a aplikacja to
`build/buddy.bin`.

## Pin allocation

## Pliki

- `main/buddy.cpp` — interfejs, nawigacja, przyciski i status.
- `main/buddy_display.cpp` — natywny sterownik SPI/ST7789 i rysowanie.
- `main/buddy_font.h` — czcionka 5 × 7 z Adafruit GFX; licencja BSD w `main/font.LICENSE`.
- `BuddyHardwareTest.ino` — poprzednia wersja Arduino do porównania; nie jest kompilowana przez ESP-IDF.

- `main/buddy_font.h` — czcionka 5 × 7 z Adafruit GFX; licencja BSD w `main/font.LICENSE`.
- `BuddyHardwareTest.ino` — poprzednia wersja Arduino do porównania; nie jest kompilowana przez ESP-IDF.

Build został wykonany lokalnie. Wygląd ekranu i reakcja fizycznych przycisków
wymagają jeszcze sprawdzenia na urządzeniu.

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
