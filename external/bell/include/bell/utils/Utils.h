#pragma once

#include <sys/time.h>
#include <cstdint>

// Contains various utility functions
namespace bell::utils {
// @brief Constructs a timeval struct from milliseconds
timeval millisecondsToTimeval(uint32_t milliseconds);

// @brief Converts a timeval struct to milliseconds
uint32_t timevalToMilliseconds(const timeval& tv);

// @brief Sleeps for the specified number of milliseconds
void sleepMs(uint32_t milliseconds);
}  // namespace bell::utils
