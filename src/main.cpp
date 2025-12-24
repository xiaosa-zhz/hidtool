#include <print>

#include "hidraw.h"

/**
 * This program is a simple wrapper around ioctl of hidraw device.
 */

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::println("Usage: {} <hidraw device path>", argv[0]);
        return 1;
    }
    try {
        hidraw::device dev(argv[1]);
        auto desc = dev.report_desc();
        std::println("[Opened device] {}", argv[1]);
        std::println("[Name] {}", dev.raw_name());
        std::println("[Address] {}", dev.addr());
        std::println("[Info]\n    {}", dev.raw_info().to_string());
        std::println("[HID Report Descriptor]\n{}", desc.to_hex());
    } catch (const std::exception& e) {
        std::println("Error: {}", e.what());
        return 1;
    }
}
