#include <print>
#include <string>
#include <string_view>
#include <array>
#include <optional>
#include <stdexcept>
#include <algorithm>
#include <format>
#include <charconv>
#include <fstream>
#include <chrono>

#include "hidraw.h"
#include "hid_report_desc.h"
#include "hid_report_desc_dump.h"

/**
 * This program is a simple wrapper around ioctl of hidraw device.
 * Usage:
 *  dump <hidraw device path>
 *   - Dumps the HID report descriptor and device info.
 *  send <hidraw device path> <report id> <hex data file path>
 *   - Sends an output report to the device.
 *  recv <hidraw device path> <report id> [<output hex data file path>]
 *   - Receives an input report from the device.
 *   - If <output hex data file path> is a directory, saves the report as a hex file in that directory named by timestamp.
 *   - If <output hex data file path> is not provided, prints to stdout.
 *  feature-get <hidraw device path> <report id> [<output hex data file path>]
 *   - Gets a feature report from the device.
 *   - If <output hex data file path> is a directory, saves the report as a hex file in that directory named by timestamp.
 *   - If <output hex data file path> is not provided, prints to stdout.
 *  feature-set <hidraw device path> <report id> <hex data file path>
 *   - Sets a feature report to the device.
 */

static int dump(const hidraw::device& dev) {
    auto desc = dev.report_desc();
    std::println("[Name] {}", dev.raw_name());
    std::println("[Address] {}", dev.addr());
    std::println("[Info]");
    std::println("{}", dev.raw_info().to_string());
    std::println("[HID Report Descriptor]");
    std::println("{}", desc.to_hex());
    return 0;
}

static int dumphid(hidraw::device& dev, const std::optional<std::filesystem::path>& output_path = std::nullopt) {
    auto desc = dev.report_desc();
    std::string text = hid::descriptor_to_string(desc.to_bytes());

    if (!output_path) {
        std::println("{}", text);
        return 0;
    }

    const auto& out = *output_path;
    std::filesystem::path final_path = out;
    if (std::filesystem::is_directory(out)) {
        auto now = std::chrono::system_clock::now();
        auto fname = std::format("{:%Y%m%d_%H%M%S}_hid.txt", now);
        final_path = out / fname;
    }

    std::ofstream ofs(final_path, std::ios::binary);
    if (!ofs) {
        throw std::runtime_error(std::format("Failed to open output path: {}", final_path.string()));
    }
    ofs << text;
    std::println("[Saved human-readable HID descriptor] {}", final_path.string());
    return 0;
}

static std::size_t report_size_by_kind(hidraw::device& dev, uint8_t report_id, hid::report_descriptor_tree::field_kind kind) {
    hidraw::descriptor desc = dev.report_desc();
    auto tree = hid::report_descriptor_tree::parse(desc.to_bytes());
    auto fields = tree.find_by_report_id(report_id);
    std::size_t size = 0;
    for (const auto& f : fields) {
        if (f->kind == kind)
            size += (f->report_size_bits * f->report_count + 7) / 8;
    }
    return size;
}

static void write_hex_output(std::span<const std::uint8_t> data, const std::optional<std::filesystem::path>& output_path) {
    if (!output_path) {
        for (std::size_t i = 0; i < data.size(); ++i) {
            std::print("{:02X} ", data[i]);
            if ((i + 1) % 16 == 0) std::println();
        }
        if (data.size() % 16 != 0) std::println();
        return;
    }
    const auto& out = *output_path;
    std::filesystem::path final_path = out;
    if (std::filesystem::is_directory(out)) {
        auto now = std::chrono::system_clock::now();
        auto fname = std::format("{:%Y%m%d_%H%M%S}.hex", now);
        final_path = out / fname;
    }
    std::ofstream ofs(final_path, std::ios::binary);
    if (!ofs) {
        throw std::runtime_error(std::format("Failed to open output path: {}", final_path.string()));
    }
    for (std::size_t i = 0; i < data.size(); ++i) {
        ofs << std::format("{:02X} ", data[i]);
        if ((i + 1) % 16 == 0) ofs << '\n';
    }
    if (data.size() % 16 != 0) ofs << '\n';
    std::println("[Saved] {}", final_path.string());
}

static std::vector<std::uint8_t> read_hex_file(const std::filesystem::path& path) {
    std::ifstream ifs(path);
    if (!ifs) {
        throw std::runtime_error(std::format("Failed to open hex file: {}", path.string()));
    }
    std::vector<std::uint8_t> result;
    std::string line;
    while (std::getline(ifs, line)) {
        std::string_view sv(line);
        auto pos = sv.find_first_not_of(" \t");
        if (pos == std::string_view::npos) continue;
        sv = sv.substr(pos);
        if (sv.starts_with("//") || sv.starts_with("#") || sv.starts_with("size:"))
            continue;
        while (!sv.empty()) {
            pos = sv.find_first_not_of(" \t,");
            if (pos == std::string_view::npos) break;
            sv = sv.substr(pos);
            auto tok_end = sv.find_first_of(" \t,");
            auto token = sv.substr(0, tok_end);
            if (token.starts_with("0x") || token.starts_with("0X"))
                token.remove_prefix(2);
            uint8_t val = 0;
            auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), val, 16);
            if (ec != std::errc{} || ptr != token.data() + token.size())
                throw std::runtime_error(std::format("Invalid hex byte '{}' in {}", token, path.string()));
            result.push_back(val);
            sv = (tok_end == std::string_view::npos) ? std::string_view{} : sv.substr(tok_end);
        }
    }
    return result;
}

static int send(hidraw::device& dev, uint8_t report_id, const std::filesystem::path& hex_file_path) {
    auto data = read_hex_file(hex_file_path);
    if (data.empty()) {
        throw std::runtime_error("Hex file contains no data.");
    }

    std::size_t output_size = report_size_by_kind(dev, report_id, hid::report_descriptor_tree::field_kind::output);
    if (output_size == 0) {
        throw std::runtime_error(std::format("No output report with ID {} found.", report_id));
    }
    if (data.size() != output_size) {
        throw std::runtime_error(std::format(
            "Data size mismatch: file has {} bytes, but output report ID {} expects {} bytes.",
            data.size(), report_id, output_size));
    }

    std::vector<std::uint8_t> buffer(output_size + 1);
    buffer[0] = report_id;
    std::copy(data.begin(), data.end(), buffer.begin() + 1);

    dev.write(buffer);
    std::println("Output Report ID {} sent ({} bytes).", report_id, output_size);
    return 0;
}

static int recv(hidraw::device& dev, uint8_t report_id, const std::optional<std::filesystem::path>& output_path = std::nullopt) {
    std::size_t input_size = report_size_by_kind(dev, report_id, hid::report_descriptor_tree::field_kind::input);
    if (input_size == 0) {
        throw std::runtime_error(std::format("No input report with ID {} found.", report_id));
    }

    std::vector<std::uint8_t> buffer(input_size + 1);
    std::size_t nread = dev.read(buffer);

    std::println("Input Report ID {} ({} bytes read):", report_id, nread > 0 ? nread - 1 : 0);
    if (nread > 1) {
        write_hex_output(std::span(buffer).subspan(1, nread - 1), output_path);
    }
    return 0;
}

static int feature_get(hidraw::device& dev, uint8_t report_id, const std::optional<std::filesystem::path>& output_path = std::nullopt) {
    std::size_t feature_size = report_size_by_kind(dev, report_id, hid::report_descriptor_tree::field_kind::feature);
    if (feature_size == 0) {
        throw std::runtime_error(std::format("No feature report with ID {} found.", report_id));
    }
    std::vector<std::uint8_t> buffer(feature_size + 1);
    buffer[0] = report_id;
    dev.feature_get(buffer);

    std::println("Feature Report ID {} ({} bytes):", report_id, feature_size);
    write_hex_output(std::span(buffer).subspan(1), output_path);
    return 0;
}

static int feature_set(hidraw::device& dev, uint8_t report_id, const std::filesystem::path& hex_file_path) {
    auto data = read_hex_file(hex_file_path);
    if (data.empty()) {
        throw std::runtime_error("Hex file contains no data.");
    }

    std::size_t feature_size = report_size_by_kind(dev, report_id, hid::report_descriptor_tree::field_kind::feature);
    if (feature_size == 0) {
        throw std::runtime_error(std::format("No feature report with ID {} found.", report_id));
    }
    if (data.size() != feature_size) {
        throw std::runtime_error(std::format(
            "Data size mismatch: file has {} bytes, but feature report ID {} expects {} bytes.",
            data.size(), report_id, feature_size));
    }

    std::vector<std::uint8_t> buffer(feature_size + 1);
    buffer[0] = report_id;
    std::copy(data.begin(), data.end(), buffer.begin() + 1);

    dev.feature_set(buffer);
    std::println("Feature Report ID {} set ({} bytes).", report_id, feature_size);
    return 0;
}

struct interact {
    std::string_view command;
    int (*handler)(const interact& self, hidraw::device& dev, char* rest_args[]);
    std::string_view usage_message;
};

class wrong_usage_exception : public std::runtime_error
{
public:
    explicit wrong_usage_exception(const interact& cmd)
        : std::runtime_error(std::format("Wrong usage of command: {}", cmd.command)), cmd(cmd)
    {}

    wrong_usage_exception(const interact& cmd, const std::string& message)
        : std::runtime_error(message), cmd(cmd)
    {}

    std::string_view usage() const noexcept {
        return cmd.usage_message;
    }

private:
    const interact& cmd;
};

static int dump_handler(const interact& self, hidraw::device& dev, char* rest_args[]) {
    return dump(dev);
}

static int dumphid_handler(const interact& self, hidraw::device& dev, char* rest_args[]) {
    std::optional<std::filesystem::path> output_path = std::nullopt;
    if (rest_args[0]) {
        output_path = rest_args[0];
    }
    return dumphid(dev, output_path);
}

static uint8_t parse_report_id(const interact& self, std::string_view arg) {
    if (arg.empty()) {
        throw wrong_usage_exception(self, "Wrong report ID");
    }

    int base = 10;
    if (arg.starts_with("0x") || arg.starts_with("0X")) {
        arg.remove_prefix(2);
        base = 16;
    }

    uint8_t value = 0;
    const char* first = arg.data();
    const char* last = arg.data() + arg.size();
    auto [ptr, ec] = std::from_chars(first, last, value, base);
    if (ec != std::errc{} || ptr != last || value > 0xFFu) {
        throw wrong_usage_exception(self, "Wrong report ID");
    }
    return value;
}

static int send_handler(const interact& self, hidraw::device& dev, char* rest_args[]) {
    if (!rest_args[0] || !rest_args[1]) {
        throw wrong_usage_exception(self, "Missing arguments for send command.");
    }
    uint8_t report_id = parse_report_id(self, rest_args[0]);
    std::filesystem::path hex_file_path = rest_args[1];
    return send(dev, report_id, hex_file_path);
}

static int recv_handler(const interact& self, hidraw::device& dev, char* rest_args[]) {
    if (!rest_args[0]) {
        throw wrong_usage_exception(self, "Missing arguments for recv command.");
    }
    uint8_t report_id = parse_report_id(self, rest_args[0]);
    std::optional<std::filesystem::path> output_path;
    if (rest_args[1]) {
        output_path = rest_args[1];
    }
    return recv(dev, report_id, output_path);
}

static int feature_get_handler(const interact& self, hidraw::device& dev, char* rest_args[]) {
    if (!rest_args[0]) {
        throw wrong_usage_exception(self, "Missing arguments for feature-get command.");
    }
    uint8_t report_id = parse_report_id(self, rest_args[0]);
    std::optional<std::filesystem::path> output_path = std::nullopt;
    if (rest_args[1]) {
        output_path = rest_args[1];
    }
    return feature_get(dev, report_id, output_path);
}

static int feature_set_handler(const interact& self, hidraw::device& dev, char* rest_args[]) {
    if (!rest_args[0] || !rest_args[1]) {
        throw wrong_usage_exception(self, "Missing arguments for feature-set command.");
    }
    uint8_t report_id = parse_report_id(self, rest_args[0]);
    std::filesystem::path hex_file_path = rest_args[1];
    return feature_set(dev, report_id, hex_file_path);
}

static int unknown_handler(const interact& self, hidraw::device& dev, char* rest_args[]) {
    throw wrong_usage_exception(self);
}

// static constexpr std::string_view usage = R"(Usage:
//   dump <hidraw device path>
//     - Dumps the HID report descriptor and device info.
//   send <hidraw device path> <report id> <hex data file path>
//     - Sends an output report to the device.
//   recv <hidraw device path> <report id> [<output hex data file path>]
//     - Receives an input report from the device.
//     - If <output hex data file path> is a directory, saves the report as a hex file in that directory named by timestamp.
//     - If <output hex data file path> is not provided, prints to stdout.
//   feature-get <hidraw device path> <report id> [<output hex data file path>]
//     - Gets a feature report from the device.
//     - If <output hex data file path> is a directory, saves the report as a hex file in that directory named by timestamp.
//     - If <output hex data file path> is not provided, prints to stdout.
//   feature-set <hidraw device path> <report id> <hex data file path>
//     - Sets a feature report to the device.
//   help
//     - Displays this help message.
// )";

static constexpr std::string_view dump_usage = R"(  dump <hidraw device path>
    - Dumps device info the HID report descriptor.
)";

static constexpr std::string_view dumphid_usage = R"(  dumphid <hidraw device path> [<output file or dir>]
    - Prints HID report descriptor in a human-readable form only.
    - If <output path> is a directory, saves to a timestamped file inside.
)";

static constexpr std::string_view send_usage = R"(  send <hidraw device path> <report id> <hex data file path>
    - Sends an output report to the device.
)";

static constexpr std::string_view recv_usage = R"(  recv <hidraw device path> <report id> [<output hex data file path>]
    - Receives an input report from the device.
    - If <output hex data file path> is a directory, saves the report as a hex file in that directory named by timestamp.
    - If <output hex data file path> is not provided, prints to stdout.
)";

static constexpr std::string_view feature_get_usage = R"(  feature-get <hidraw device path> <report id> [<output hex data file path>]
    - Gets a feature report from the device.
    - If <output hex data file path> is a directory, saves the report as a hex file in that directory named by timestamp.
    - If <output hex data file path> is not provided, prints to stdout.
)";

static constexpr std::string_view feature_set_usage = R"(  feature-set <hidraw device path> <report id> <hex data file path>
    - Sets a feature report to the device.
)";

template<const std::string_view&... usages>
consteval auto make_raw_usage() noexcept {
    constexpr std::size_t total_size = (usages.size() + ... + 0) + sizeof...(usages) + 100;
    std::array<char, total_size> buf{};
    auto it = buf.data();
    ((it = std::ranges::copy(usages, it).out, *it++ = '\n'), ...);
    constexpr std::string_view help_line = "  help\n    - Displays this help message.\n";
    it = std::ranges::copy(help_line, it).out;
    return buf;
}

static constexpr auto raw_usage = make_raw_usage<
    dump_usage,
    dumphid_usage,
    send_usage,
    recv_usage,
    feature_get_usage,
    feature_set_usage
>();

static constexpr std::string_view usage = std::string_view(raw_usage.begin(), std::ranges::find(raw_usage, '\0'));

static constexpr auto help_command = "help";

static constexpr auto commands = [] {
    auto cmds = std::to_array<interact>({
        {"dump", &dump_handler, dump_usage},
        {"dumphid", &dumphid_handler, dumphid_usage},
        {"send", &send_handler, send_usage},
        {"recv", &recv_handler, recv_usage},
        {"feature-get", &feature_get_handler, feature_get_usage},
        {"feature-set", &feature_set_handler, feature_set_usage},
    });
    std::ranges::sort(cmds, {}, &interact::command);
    return cmds;
}();

static constexpr interact unknown_command = {"UNKNOWN-COMMAND-DO-NOT-USE-I-BEG-YOU", &unknown_handler, usage};

static void display_usage(std::string_view self, std::string_view usage) {
    std::println("HID Raw Interaction Tool (at {})", self);
    std::println("Usage:");
    std::println("{}", usage);
}

int main(int argc, char* argv[]) {
    try {
        if (argc < 2) {
            throw wrong_usage_exception(unknown_command, "Missing command.");
        }
        const std::string_view command = argv[1];
        if (command == help_command) {
            display_usage(argv[0], usage);
            return 0;
        }
        const auto eq = std::ranges::equal_range(commands, command, {}, &interact::command);
        if (eq.empty()) {
            throw wrong_usage_exception(unknown_command, std::format("Unknown command: {}", command));
        }
        const auto& interact = eq[0];
        if (argc < 3) {
            throw wrong_usage_exception(interact, "Missing hidraw device path.");
        }
        hidraw::device dev(argv[2]);
        std::println("[Opened device] {}", argv[2]);
        return interact.handler(interact, dev, &argv[3]);
    } catch (const wrong_usage_exception& e) {
        std::println("Error: {}", e.what());
        display_usage(argv[0], e.usage());
        return 1;
    } catch (const std::exception& e) {
        std::println("Error: {}", e.what());
        return 1;
    }
    return 0;
}
