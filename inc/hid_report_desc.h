#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <span>
#include <memory>
#include <unordered_map>

namespace hid {

// Parsed HID Report Descriptor as a tree structure.
class report_descriptor_tree {
public:
    struct report_field_flags {
        // Flags from Input/Output/Feature data byte
        // Bit meanings per HID spec:
        // 0: Data(0)/Constant(1)
        // 1: Array(0)/Variable(1)
        // 2: Absolute(0)/Relative(1)
        // 3: No Wrap(0)/Wrap(1)
        // 4: Linear(0)/Non-linear(1)
        // 5: Preferred(0)/No Preferred(1)
        // 6: No Null position(0)/Null state(1)
        // 7: Bitfield(0)/Buffered bytes(1)
        uint8_t raw = 0;
        bool is_constant() const { return raw & 0x01; }
        bool is_variable() const { return raw & 0x02; }
        bool is_relative() const { return raw & 0x04; }
        bool is_wrap() const { return raw & 0x08; }
        bool is_nonlinear() const { return raw & 0x10; }
        bool is_no_preferred() const { return raw & 0x20; }
        bool is_null_state() const { return raw & 0x40; }
        bool is_buffered_bytes() const { return raw & 0x80; }
    };

    enum class field_kind : uint8_t { input, output, feature };

    struct report_field {
        field_kind kind;
        uint8_t report_id = 0; // 0 if none
        uint16_t usage_page = 0; // current usage page
        std::vector<uint32_t> usages; // collected usages for this field
        uint32_t report_size_bits = 0; // per item report size (bits)
        uint32_t report_count = 0;     // per item report count
        int32_t logical_min = 0;
        int32_t logical_max = 0;
        int32_t physical_min = 0;
        int32_t physical_max = 0;
        uint32_t unit = 0;
        int8_t unit_exponent = 0;
        report_field_flags flags{};
    };

    struct collection_node {
        // Collection types per HID spec (Application=1, Physical=0, etc.)
        uint8_t type = 0;
        uint16_t usage_page = 0;
        uint32_t usage = 0; // main usage of the collection if set
        std::vector<report_field> fields; // input/output/feature defined under this collection
        std::vector<std::unique_ptr<collection_node>> children;
    };

    // Parse from raw descriptor bytes
    static report_descriptor_tree parse(std::span<const uint8_t> bytes);

    // Find all fields bound to a specific report ID
    std::vector<const report_field*> find_by_report_id(uint8_t report_id) const;

    // Human-readable dump
    std::string to_string() const;

    const collection_node& root() const { return *root_; }

private:
    std::unique_ptr<collection_node> root_;
    std::unordered_map<uint8_t, std::vector<const report_field*>> index_by_report_id_;
    // Keep a view of the original descriptor bytes for standardized rendering
    std::span<const std::uint8_t> source_bytes_{};
};

} // namespace hid
#pragma once

namespace hid {


    
} // namespace hid
