# Buddy — ESP-IDF

![Logo Buddy](buddy_chrome.png)

Buddy to niewielkie urządzenie z Seeed Studio XIAO ESP32-C5, ekranem ST7789
240 × 240 i czterema przyciskami kierunkowymi. Firmware jest natywnym projektem
ESP-IDF, bez Arduino. Zawiera ekran startowy z napisem **BUDDY**, menu, trzy
testy wyświetlacza, monitor przycisków i status systemu.

## Połączenia

Oznaczenia `SCL` i `SDA` na module wyświetlacza to linie SPI, nie I²C. Ekran
jest jedynym urządzeniem na magistrali, a jego `CS` jest połączony z GND.

| ST7789 | XIAO ESP32-C5 | GPIO | Funkcja |
|---|---|---:|---|
| GND | GND | — | Masa |
| VCC | 3V3 | — | Zasilanie |
| SCL | D8 | 8 | SPI SCK |
| SDA | D10 | 10 | SPI MOSI |
| RST | D2 | 25 | Reset |
| DC | D6 | 11 | Dane / komenda |
| CS | GND | — | Stale aktywny |
| BL | D7 | 12 | Podświetlenie, aktywne HIGH |

Ekran pracuje w trybie **SPI MODE3, 4 MHz, RGB565, obrót 180°**. Firmware
rysuje bezpośrednio przez SPI, bez bufora całego ekranu.

Każdy przycisk łączy GPIO z GND. Włączone jest wewnętrzne podciąganie, więc
stan LOW oznacza naciśnięcie.

| Kierunek | XIAO | GPIO |
|---|---|---:|
| UP | D0 | 1 |
| DOWN | D5 | 24 |
| LEFT / BACK | D1 | 0 |
| RIGHT / ENTER | D4 | 23 |

GPIO9 / D9 pozostaje wolny; brzęczyk nie jest podłączony.

## Sterowanie

| Ekran | UP / DOWN | LEFT | RIGHT |
|---|---|---|---|
| Start | Otwórz menu | Otwórz menu | Otwórz menu |
| Menu | Zmień pozycję | — | Otwórz pozycję |
| Display | — | Wróć do menu | Następny test |
| Buttons | Test naciśnięcia | Test; przytrzymaj 650 ms, by wrócić | Test; przytrzymaj 650 ms, by otworzyć status |
| System | — | Wróć do menu | Odśwież status |
| Refresh | — | Anuluj | — |

Drgania styków są filtrowane przez 30 ms. Naciśnięcie `RIGHT`, którym otwarto
monitor przycisków, nie jest liczone do nowej sesji. Odświeżanie statusu
sprawdza konfigurację ekranu, stany przycisków i czcionkę. Połączenie SPI nie
ma linii MISO, więc status nie jest elektrycznym testem wyświetlacza.

## Kompilacja i wgrywanie

Projekt kompiluje się z **ESP-IDF 5.5.3** dla targetu `esp32c5`:

```sh
. /ścieżka/do/esp-idf/export.sh
idf.py -DIDF_TARGET=esp32c5 build
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

Wybierz port właściwy dla podłączonej płytki. `sdkconfig.defaults` ustawia
konsolę na USB Serial/JTAG, aby UART nie zajmował GPIO11 i GPIO12. Aplikacja
po buildzie znajduje się w `build/buddy.bin`; cały katalog `build/` jest
generowany lokalnie i pomijany przez Git.

Kod interfejsu i obsługi przycisków jest w `main/buddy.cpp`, mapa pinów w
`main/buddy_board.h`, a sterownik ST7789 w `main/buddy_display.cpp`. Czcionka
5 × 7 używa danych z Adafruit GFX na licencji BSD (`main/font.LICENSE`).

Build został zweryfikowany lokalnie. Wygląd ekranu i reakcja fizycznych
przycisków wymagają sprawdzenia na podłączonym urządzeniu.

## Obudowa

[Pliki obudowy na MakerWorld](https://makerworld.com/pl/models/3354005-buddy-seeed-studio-xiao-esp32-c5-1-54-tft#profileId-3812397).
