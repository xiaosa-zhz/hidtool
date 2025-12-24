#pragma once

#include <cstddef>
#include <string>
#include <filesystem>
#include <utility>
#include <span>

namespace hidraw {

// From ioctl HIDIOCGRDESC
class descriptor
{
public:
    descriptor() = default;
    ~descriptor();
    descriptor(const descriptor& other);

    descriptor& operator=(const descriptor& other) {
        descriptor(other).swap(*this);
        return *this;
    }

    descriptor(descriptor&& other) noexcept
        : ptr(std::exchange(other.ptr, nullptr))
    {}

    descriptor& operator=(descriptor&& other) noexcept {
        descriptor(std::move(other)).swap(*this);
        return *this;
    }

    void swap(descriptor& other) noexcept {
        std::ranges::swap(ptr, other.ptr);
    }

    std::string to_hex() const;

private:
    friend class device;
    void* ptr = nullptr;
};

// From ioctl HIDIOCGRAWINFO
class info
{
public:
    info() = default;
    info(const info&) = default;
    info& operator=(const info&) = default;
    ~info() = default;

    std::string to_string() const;
    
private:
    friend class device;
    std::uint32_t bustype = 0;
    std::int16_t vendor = 0;
    std::int16_t product = 0;
};

class device
{
public:
    device() = default;

    explicit device(const std::filesystem::path& path) {
        this->open(path);
    }

    ~device() { this->close(); }

    device(const device&) = delete;
    device& operator=(const device&) = delete;

    device(device&& other) noexcept
        : fd(std::exchange(other.fd, -1))
    {}

    device& operator=(device&& other) noexcept {
        device(std::move(other)).swap(*this);
        return *this;
    }

    void swap(device& other) noexcept {
        std::ranges::swap(fd, other.fd);
    }

    int native_handle() const noexcept { return fd; }
    bool valid() const noexcept { return fd != -1; }

    void open(const std::filesystem::path& path);
    void close();
    std::size_t report_desc_size() const;
    descriptor report_desc() const;
    info raw_info() const;
    std::string raw_name() const;
    std::string addr() const;

private:
    int fd = -1;
};

} // namespace hidraw
