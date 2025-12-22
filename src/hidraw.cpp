#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/hidraw.h>

#include "hidraw.h"

#include <new>
#include <system_error>
#include <format>

#define ASSERT_FD_OPENED() \
    do { \
        if (fd < 0) { \
            throw std::runtime_error("Device not opened"); \
        } \
    } while (0)

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

descriptor::descriptor(const descriptor& other)
    : ptr(other.ptr ? dump_report_descriptor(other.ptr) : nullptr)
{}

descriptor::~descriptor() {
    delete_report_descriptor(ptr);
}

std::string descriptor::to_hex() const {
    if (!ptr) return {};

    report_descriptor_head* head = to_report_descriptor_head(ptr);
    const std::size_t size = head->size;
    const unsigned char* data = std::launder(reinterpret_cast<const unsigned char*>(head + 1));
    std::string result;
    result.resize(size * 2);

    for (std::size_t i = 0; i < size; ++i) {
        std::format_to(result.data() + i * 2, "{:02X}", data[i]);
    }

    return result;
}

void device::open(const std::filesystem::path& path) {
    if (valid()) {
        throw std::runtime_error("Device already opened");
    }

    fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd == -1) {
        throw std::system_error(errno, std::generic_category(),
            std::format("Failed to open device at '{}'", path.string()));
    }
}

void device::close() {
    if (valid()) {
        ::close(fd);
        fd = -1;
    }
}

std::size_t device::report_desc_size() const {
    ASSERT_FD_OPENED();
    int size;
    if (::ioctl(fd, HIDIOCGRDESCSIZE, &size) < 0) {
        throw std::system_error(errno, std::generic_category(),
            "Failed to get report descriptor size");
    }
    return static_cast<std::size_t>(size);
}

descriptor device::report_desc() const {
    ASSERT_FD_OPENED();
    const std::size_t size = report_desc_size();
    report_descriptor_head* head = create_report_descriptor(size);
    if (::ioctl(fd, HIDIOCGRDESC, reinterpret_cast<::hidraw_report_descriptor*>(head)) < 0) {
        delete_report_descriptor(head);
        throw std::system_error(errno, std::generic_category(),
            "Failed to get report descriptor");
    }
    descriptor desc;
    desc.ptr = head;
    return desc;
}

info device::raw_info() const {
    ASSERT_FD_OPENED();
    // TODO
    return info{};
}

} // namespace hidraw
