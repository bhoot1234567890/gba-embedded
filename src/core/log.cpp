#include "gba/core/log.hpp"

#ifndef GBA_PLATFORM_ESP32
#include <iostream>
#endif

namespace gba {

void NullLogger::log(std::string_view channel, std::string_view message) {
    (void)channel;
    (void)message;
}

void StdoutLogger::log(std::string_view channel, std::string_view message) {
#ifndef GBA_PLATFORM_ESP32
    std::cout << "[" << channel << "] " << message << '\n';
#else
    (void)channel;
    (void)message;
#endif
}

}  // namespace gba
