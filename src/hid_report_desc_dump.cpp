// Human-readable HID report descriptor dump
#include "hid_report_desc_dump.h"
#include "priv/hid_report_item.h"

#include <type_traits>
#include <iterator>
#include <array>
#include <format>
#include <algorithm>
#include <string_view>

namespace hid {

namespace {

using detail::item;
using detail::parse_item;
using detail::sign_extend;

// ════════════════════════════════════════════════════════════════════════════
// Name lookup tables
// ════════════════════════════════════════════════════════════════════════════

struct named_u8  { uint8_t  value; std::string_view name; };
struct named_u16 { uint16_t value; std::string_view name; };
struct usage_entry { uint16_t page; uint32_t usage; std::string_view name; };

// Collection types — HID 1.11 §6.2.2.6
static constexpr auto collection_types = std::to_array<named_u8>({
	{0x00, "Physical"},      {0x01, "Application"},   {0x02, "Logical"},
	{0x03, "Report"},        {0x04, "Named Array"},
	{0x05, "Usage Switch"},  {0x06, "Usage Modifier"},
});
static_assert(std::ranges::is_sorted(collection_types, {}, &named_u8::value));

static std::string_view collection_type_name(uint8_t t) noexcept {
	auto it = std::ranges::lower_bound(collection_types, t, {}, &named_u8::value);
	if (it != std::ranges::end(collection_types) && it->value == t) return it->name;
	return (t >= 0x80) ? "Vendor Defined" : "Reserved";
}

// Usage Pages — HID Usage Tables 1.12
static constexpr auto usage_pages = std::to_array<named_u16>({
	{0x01, "Generic Desktop Ctrls"}, {0x02, "Simulation Ctrls"},
	{0x03, "VR Ctrls"},             {0x04, "Sport Ctrls"},
	{0x05, "Game Ctrls"},           {0x06, "Generic Device Ctrls"},
	{0x07, "Kbrd/Keypad"},          {0x08, "LEDs"},
	{0x09, "Button"},               {0x0A, "Ordinal"},
	{0x0B, "Telephony"},            {0x0C, "Consumer"},
	{0x0D, "Digitizer"},            {0x0E, "Haptics"},
	{0x0F, "PID"},                  {0x10, "Unicode"},
	{0x14, "Alphanumeric Display"},
});
static_assert(std::ranges::is_sorted(usage_pages, {}, &named_u16::value));

static std::string usage_page_name(uint16_t page) {
	auto it = std::ranges::lower_bound(usage_pages, page, {}, &named_u16::value);
	if (it != std::ranges::end(usage_pages) && it->value == page) return std::string(it->name);
	if (page >= 0xFF00)
		return std::format("Vendor Defined 0x{:04X}", page);
	return std::format("0x{:04X}", page);
}

// Known usages organised by page
static constexpr auto known_usages = std::to_array<usage_entry>({
	// Generic Desktop (0x01)
	{0x01, 0x01, "Pointer"},  {0x01, 0x02, "Mouse"},
	{0x01, 0x04, "Joystick"}, {0x01, 0x05, "Game Pad"},
	{0x01, 0x06, "Keyboard"}, {0x01, 0x07, "Keypad"},
	{0x01, 0x08, "Multi-axis Controller"},
	{0x01, 0x30, "X"},  {0x01, 0x31, "Y"},  {0x01, 0x32, "Z"},
	{0x01, 0x33, "Rx"}, {0x01, 0x34, "Ry"}, {0x01, 0x35, "Rz"},
	{0x01, 0x36, "Slider"}, {0x01, 0x37, "Dial"}, {0x01, 0x38, "Wheel"},
	{0x01, 0x39, "Hat switch"},
	// Consumer (0x0C)
	{0x0C, 0xB5, "Scan Next Track"},     {0x0C, 0xB6, "Scan Previous Track"},
	{0x0C, 0xCD, "Play/Pause"},
	{0x0C, 0xE0, "Volume"},
	{0x0C, 0xE9, "Volume Increment"},    {0x0C, 0xEA, "Volume Decrement"},
	// Digitizer (0x0D)
	{0x0D, 0x01, "Digitizer"},    {0x0D, 0x02, "Pen"},
	{0x0D, 0x04, "Touch Screen"}, {0x0D, 0x20, "Stylus"},
	{0x0D, 0x22, "Finger"},       {0x0D, 0x30, "Tip Pressure"},
	{0x0D, 0x32, "In Range"},     {0x0D, 0x42, "Tip Switch"},
	{0x0D, 0x44, "Barrel Switch"},{0x0D, 0x47, "Confidence"},
	{0x0D, 0x48, "Width"},        {0x0D, 0x49, "Height"},
	{0x0D, 0x51, "Contact Identifier"},
	{0x0D, 0x54, "Contact Count"},
	{0x0D, 0x55, "Contact Count Maximum"},
	// Haptics (0x0E)
	{0x0E, 0x01, "Simple Haptic Controller"},
	{0x0E, 0x10, "Waveform List"},       {0x0E, 0x11, "Duration List"},
	{0x0E, 0x20, "Auto Trigger"},        {0x0E, 0x21, "Manual Trigger"},
	{0x0E, 0x22, "Auto Trigger Associated Control"},
	{0x0E, 0x23, "Intensity"},           {0x0E, 0x24, "Repeat Count"},
	{0x0E, 0x25, "Retrigger Period"},    {0x0E, 0x28, "Waveform Cutoff Time"},
});
static constexpr auto usage_entry_proj(const usage_entry& e) noexcept {
	return std::make_tuple(e.page, e.usage);
}
static_assert(std::ranges::is_sorted(known_usages, {}, &usage_entry_proj));

static std::string usage_name(uint16_t page, uint32_t usage) {
	auto key = std::make_tuple(page, usage);
	auto it = std::ranges::lower_bound(known_usages, key, {}, &usage_entry_proj);
	if (it != std::ranges::end(known_usages) && usage_entry_proj(*it) == key) return std::string(it->name);
	if (page == 0x09 && usage > 0)
		return std::format("Button {}", usage);
	return std::format("0x{:X}", usage);
}

// ════════════════════════════════════════════════════════════════════════════
// Input/Output/Feature flag bits — HID 1.11 §6.2.2.5
// ════════════════════════════════════════════════════════════════════════════

enum class iof_kind : uint8_t { input, output, feature };

struct flag_pair { std::string_view clear; std::string_view set; };

static constexpr auto iof_flags = std::to_array<flag_pair>({
	{"Data",            "Const"},
	{"Array",           "Var"},
	{"Abs",             "Rel"},
	{"No Wrap",         "Wrap"},
	{"Linear",          "Non-linear"},
	{"Preferred State", "No Preferred State"},
	{"No Null Position","Null Position"},
});

static std::string iof_flags_text(uint8_t raw, iof_kind kind) {
	std::string s;
	auto append = [&](std::string_view v) {
		if (!s.empty()) s += ',';
		s += v;
	};
	for (int i = 0; i < 7; ++i)
		append((raw & (1 << i)) ? iof_flags[i].set : iof_flags[i].clear);
	// Bit 7: Input → Bitfield/Buffered Bytes; Output/Feature → Non Volatile/Volatile
	if (kind == iof_kind::input)
		append((raw & 0x80) ? "Buffered Bytes" : "Bitfield");
	else
		append((raw & 0x80) ? "Volatile" : "Non-volatile");
	return s;
}

// ════════════════════════════════════════════════════════════════════════════
// Item metadata table — maps (type, tag) → human-readable template
// ════════════════════════════════════════════════════════════════════════════

enum class value_kind : uint8_t {
	none,            // no parenthesised value
	u32,             // unsigned decimal
	i32,             // sign-extended decimal
	hex,             // hexadecimal
	usage_page_fmt,  // usage page name
	usage_fmt,       // usage name (needs current usage page context)
	collection_fmt,  // collection type name
	iof_input,       // Input flags
	iof_output,      // Output flags
	iof_feature,     // Feature flags
};

struct item_meta {
	uint8_t          type;
	uint8_t          tag;
	std::string_view name;
	value_kind       vk;
};

// Complete item table per HID 1.11 §6.2.2
static constexpr auto item_table = std::to_array<item_meta>({
	// Main items (type=0) — §6.2.2.4
	{0, 0x08, "Input",          value_kind::iof_input},
	{0, 0x09, "Output",         value_kind::iof_output},
	{0, 0x0A, "Collection",     value_kind::collection_fmt},
	{0, 0x0B, "Feature",        value_kind::iof_feature},
	{0, 0x0C, "End Collection", value_kind::none},
	// Global items (type=1) — §6.2.2.7
	{1, 0x00, "Usage Page",       value_kind::usage_page_fmt},
	{1, 0x01, "Logical Minimum",  value_kind::i32},
	{1, 0x02, "Logical Maximum",  value_kind::i32},
	{1, 0x03, "Physical Minimum", value_kind::i32},
	{1, 0x04, "Physical Maximum", value_kind::i32},
	{1, 0x05, "Unit Exponent",    value_kind::i32},
	{1, 0x06, "Unit",             value_kind::hex},
	{1, 0x07, "Report Size",      value_kind::u32},
	{1, 0x08, "Report ID",        value_kind::u32},
	{1, 0x09, "Report Count",     value_kind::u32},
	{1, 0x0A, "Push",             value_kind::none},
	{1, 0x0B, "Pop",              value_kind::none},
	// Local items (type=2) — §6.2.2.8
	{2, 0x00, "Usage",              value_kind::usage_fmt},
	{2, 0x01, "Usage Minimum",      value_kind::hex},
	{2, 0x02, "Usage Maximum",      value_kind::hex},
	{2, 0x03, "Designator Index",   value_kind::u32},
	{2, 0x04, "Designator Minimum", value_kind::u32},
	{2, 0x05, "Designator Maximum", value_kind::u32},
	{2, 0x07, "String Index",       value_kind::u32},
	{2, 0x08, "String Minimum",     value_kind::u32},
	{2, 0x09, "String Maximum",     value_kind::u32},
	{2, 0x0A, "Delimiter",          value_kind::u32},
});
static constexpr auto item_meta_proj(const item_meta& e) noexcept {
	return std::make_tuple(e.type, e.tag);
}
static_assert(std::ranges::is_sorted(item_table, {}, &item_meta_proj));

static const item_meta* find_meta(uint8_t type, uint8_t tag) noexcept {
	auto key = std::make_tuple(type, tag);
	auto it = std::ranges::lower_bound(item_table, key, {}, &item_meta_proj);
	if (it != std::ranges::end(item_table) && item_meta_proj(*it) == key) return &*it;
	return nullptr;
}

static std::string format_value(const item_meta& meta, const item& it, uint16_t ctx_usage_page) {
	switch (meta.vk) {
		case value_kind::none:           return {};
		case value_kind::u32:            return std::format("{}", it.data);
		case value_kind::i32:            return std::format("{}", sign_extend(it.data, it.size));
		case value_kind::hex:            return std::format("0x{:02X}", it.data);
		case value_kind::usage_page_fmt: return usage_page_name(static_cast<uint16_t>(it.data));
		case value_kind::usage_fmt:      return usage_name(ctx_usage_page, it.data);
		case value_kind::collection_fmt: return std::string(collection_type_name(static_cast<uint8_t>(it.data)));
		case value_kind::iof_input:      return iof_flags_text(static_cast<uint8_t>(it.data), iof_kind::input);
		case value_kind::iof_output:     return iof_flags_text(static_cast<uint8_t>(it.data), iof_kind::output);
		case value_kind::iof_feature:    return iof_flags_text(static_cast<uint8_t>(it.data), iof_kind::feature);
	}
	return {};
}

// ════════════════════════════════════════════════════════════════════════════
// Formatting helpers
// ════════════════════════════════════════════════════════════════════════════

template<typename... Args>
static void append_format(std::string& out, std::type_identity_t<std::format_string<Args...>> fmt, Args&&... args) {
	std::format_to(std::back_inserter(out), fmt, std::forward<Args>(args)...);
}

static void append_item_bytes(std::string& out, const uint8_t* start, const uint8_t* end) {
	for (const uint8_t* p = start; p < end; ++p) {
		if (p != start) out += ", ";
		append_format(out, "0x{:02X}", *p);
	}
	std::size_t len = static_cast<std::size_t>(end - start);
	std::size_t pad = (len * 6 >= 24) ? 1 : (24 - len * 6);
	out.append(pad, ' ');
}

static constexpr auto type_fallback_names = std::to_array<std::string_view>({"Main", "Global", "Local", "Reserved"});

} // namespace

// ════════════════════════════════════════════════════════════════════════════
// Public API
// ════════════════════════════════════════════════════════════════════════════

std::string descriptor_to_string(std::span<const uint8_t> bytes) {
	std::string result;
	const uint8_t* p   = bytes.data();
	const uint8_t* end = p + bytes.size();
	uint16_t usage_page = 0;
	int depth = 0;

	while (p && p < end) {
		const uint8_t* start = p;
		item it = parse_item(p, end);

		// Raw byte column
		append_item_bytes(result, start, p);

		// End Collection reduces depth before the annotation line
		if (it.type == 0 && it.tag == 0x0C && depth > 0) --depth;

		// Indent
		result.append("// ");
		result.append(depth * 2, ' ');

		if (const item_meta* meta = find_meta(it.type, it.tag)) {
			if (it.type == 1 && it.tag == 0x00)
				usage_page = static_cast<uint16_t>(it.data);

			std::string value = format_value(*meta, it, usage_page);
			if (value.empty())
				append_format(result, "{}\n", meta->name);
			else
				append_format(result, "{} ({})\n", meta->name, value);
		} else {
			append_format(result, "{} (tag=0x{:X})\n", type_fallback_names[it.type & 3], it.tag);
		}

		// Collection increases depth after the annotation line
		if (it.type == 0 && it.tag == 0x0A) ++depth;
	}

	append_format(result, "\n// {} bytes\n", bytes.size());
	return result;
}

} // namespace hid
