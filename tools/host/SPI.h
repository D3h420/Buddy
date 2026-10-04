#pragma once
#define SPI_MODE3 3
struct HostSPI { void begin(int, int, int, int) {} };
inline HostSPI SPI;
