#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/hidraw.h>
#include <linux/input.h>

#include "hidraw.h"

#include <new>
#include <system_error>
#include <type_traits>
#include <format>
#include <iterator>
#include <array>
#include <string_view>
#include <algorithm>
#include <utility>

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

template<typename... Args>
[[noreturn]] static inline void throw_system_error(
    std::type_identity_t<std::format_string<Args...>> fmt, Args&&... args) noexcept(false) {
    throw std::system_error(errno, std::system_category(), std::format(fmt, std::forward<Args>(args)...));
}

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

// Format:
//  size: ...
//  XX XX XX XX XX XX XX XX XX XX XX XX XX XX XX XX
//  ...
std::string descriptor::to_hex() const {
    if (!ptr) return {};

    report_descriptor_head* head = to_report_descriptor_head(ptr);
    const std::size_t size = head->size;
    const unsigned char* data = std::launder(reinterpret_cast<const unsigned char*>(head + 1));
    std::string result;
    auto it = std::back_inserter(result);
    it = std::format_to(it, "size: {}\n", size);
    for (std::size_t i = 0; i < size; ++i) {
        it = std::format_to(it, "{:02X} ", data[i]);
        if ((i + 1) % 16 == 0) {
            *it = '\n';
            ++it;
        }
    }

    return result;
}

void device::open(const std::filesystem::path& path) {
    if (valid()) {
        throw std::runtime_error("Device already opened");
    }

    fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        throw_system_error("Failed to open device at '{}'", path.string());
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
        throw_system_error("Failed to get report descriptor size");
    }
    return static_cast<std::size_t>(size);
}

descriptor device::report_desc() const {
    ASSERT_FD_OPENED();
    const std::size_t size = report_desc_size();
    descriptor desc;
    desc.ptr = create_report_descriptor(size);
    if (::ioctl(fd, HIDIOCGRDESC, reinterpret_cast<::hidraw_report_descriptor*>(desc.ptr)) < 0) {
        throw_system_error("Failed to get report descriptor");
    }
    return desc;
}

info device::raw_info() const {
    ASSERT_FD_OPENED();
    ::hidraw_devinfo raw_info;
    if (::ioctl(fd, HIDIOCGRAWINFO, &raw_info) < 0) {
        throw_system_error("Failed to get raw info");
    }
    info info;
    info.bustype = raw_info.bustype;
    info.vendor = raw_info.vendor;
    info.product = raw_info.product;
    return info;
}

static constexpr std::array bus_type_list = {
    BUS_USB,
    BUS_HIL,
    BUS_BLUETOOTH,
    BUS_VIRTUAL,
};

static constexpr std::size_t bus_type_table_size = std::ranges::max(bus_type_list) + 1;

static constexpr std::array<std::string_view, bus_type_table_size> bus_type_lookup = [] consteval {
    std::array<std::string_view, bus_type_table_size> arr{};
    arr.fill("UNKNOWN");
    arr[BUS_USB] = "USB";
    arr[BUS_HIL] = "HIL";
    arr[BUS_BLUETOOTH] = "BLUETOOTH";
    arr[BUS_VIRTUAL] = "VIRTUAL";
    return arr;
}();

std::string info::to_string() const {
    return std::format("Bus Type: {} (0x{:04X}), Vendor ID: {} (0x{:04X}), Product ID: {} (0x{:04X})",
        (bustype < bus_type_table_size ? bus_type_lookup[bustype] : "UNKNOWN"), bustype,
        vendor, vendor,
        product, product);
}

std::string device::raw_name() const {
    ASSERT_FD_OPENED();
    constexpr std::size_t max_len = 256;
    std::string name;
    name.resize(max_len);
    if (::ioctl(fd, HIDIOCGRAWNAME(max_len), name.data()) < 0) {
        throw_system_error("Failed to get raw name");
    }
    name.resize(std::strlen(name.c_str()));
    return name;
}

std::string device::addr() const {
    ASSERT_FD_OPENED();
    constexpr std::size_t max_len = 256;
    std::string addr;
    addr.resize(max_len);
    if (::ioctl(fd, HIDIOCGRAWPHYS(max_len), addr.data()) < 0) {
        throw_system_error("Failed to get raw address");
    }
    addr.resize(std::strlen(addr.c_str()));
    return addr;
}

} // namespace hidraw
