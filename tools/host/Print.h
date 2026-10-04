#pragma once
#include "Arduino.h"
class Print {
public:
  virtual ~Print() = default;
  virtual size_t write(uint8_t) = 0;
  size_t print(const char *text) {
    size_t count = 0;
    while (*text) count += write(static_cast<uint8_t>(*text++));
    return count;
  }
  size_t print(char value) { return write(static_cast<uint8_t>(value)); }
};

