# Buddy — ESP-IDF

Natywny projekt ESP-IDF dla Seeed Studio XIAO ESP32-C5, wyświetlacza ST7789
240 × 240 i czterech przycisków. Wersja nie używa Arduino ani bibliotek
Adafruit w firmware. Zachowuje menu, test geometrii / skali szarości / ruchu,
monitor przycisków i ekran systemowy z odświeżaniem statusu. Intro zostało
uproszczone do napisu **BUDDY**.

## Sprzęt

Wyświetlacz jest jedynym urządzeniem SPI. Oznaczenia `SCL` i `SDA` na module
oznaczają linie SPI, nie I²C. `CS` jest połączony na stałe z GND.

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

## Pliki

- `main/buddy.cpp` — interfejs, nawigacja, przyciski i status.
- `main/buddy_display.cpp` — natywny sterownik SPI/ST7789 i rysowanie.
- `main/buddy_font.h` — czcionka 5 × 7 z Adafruit GFX; licencja BSD w `main/font.LICENSE`.
- `BuddyHardwareTest.ino` — poprzednia wersja Arduino do porównania; nie jest kompilowana przez ESP-IDF.

Build został wykonany lokalnie. Wygląd ekranu i reakcja fizycznych przycisków
wymagają jeszcze sprawdzenia na urządzeniu.
