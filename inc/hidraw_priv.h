#pragma once

#include <linux/hidraw.h>

#include "hidraw.h" // IWYU pragma: export

namespace hidraw {

struct report_descriptor_head {
    ::__u32 size;
};

static inline report_descriptor_head* to_report_descriptor_head(void* ptr) noexcept {
    return static_cast<report_descriptor_head*>(ptr);
}

static inline report_descriptor_head* dump_report_descriptor(void* ptr) {
    [[assume(ptr != nullptr)]];
    report_descriptor_head* head = to_report_descriptor_head(ptr);
    const std::size_t total = sizeof(report_descriptor_head) + head->size;
    unsigned char* buf = new (std::align_val_t(alignof(report_descriptor_head))) unsigned char[total];
    void* new_desc = std::memcpy(buf, ptr, total);
    return static_cast<report_descriptor_head*>(new_desc);
}

static inline report_descriptor_head* create_report_descriptor(std::size_t size) {
    const std::size_t total = sizeof(report_descriptor_head) + size;
    unsigned char* buf = new (std::align_val_t(alignof(report_descriptor_head))) unsigned char[total];
    report_descriptor_head* head = reinterpret_cast<report_descriptor_head*>(buf);
    head->size = static_cast<::__u32>(size);
    return head;
}

static inline void delete_report_descriptor(void* ptr) {
    delete[] static_cast<unsigned char*>(ptr);
}

} // namespace hidraw
