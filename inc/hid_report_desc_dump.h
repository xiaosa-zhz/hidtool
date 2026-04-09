#pragma once

#include <cstdint>
#include <string>
#include <span>

namespace hid {

// Human-readable dump of raw HID report descriptor bytes.
std::string descriptor_to_string(std::span<const uint8_t> bytes);

} // namespace hid
