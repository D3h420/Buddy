#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#define ARDUINO 100
#define PROGMEM
#define HIGH 1
#define LOW 0
#define INPUT_PULLUP 2
#define OUTPUT 3
#define D0 1
#define D1 0
#define D2 25
#define D4 23
#define D5 24
#define D6 11
#define D7 12
#define D8 8
#define D10 10
#define pgm_read_byte(p) (*(const uint8_t *)(p))
#define pgm_read_word(p) (*(const uint16_t *)(p))
using std::min;
using std::max;
using String = std::string;
class __FlashStringHelper;
template <class T, class U, class V> T constrain(T value, U low, V high) {
  return std::max(static_cast<T>(low), std::min(static_cast<T>(high), value));
}
inline float radians(float degrees) { return degrees * 0.017453292519943295f; }
inline uint32_t hostTime = 0;
inline int hostPins[40] = {};
inline uint32_t millis() { return hostTime; }
inline void delay(uint32_t duration) { hostTime += duration; }
inline void pinMode(int, int) {}
inline void digitalWrite(int pin, int value) { hostPins[pin] = value; }
inline int digitalRead(int pin) { return hostPins[pin]; }
