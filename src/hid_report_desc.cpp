// HID Report Descriptor parser implementation
#include "hid_report_desc.h"

#include <sstream>
#include <stack>
#include <format>
#include <algorithm>

namespace hid {

namespace {

struct global_state {
	uint16_t usage_page = 0;
	uint8_t report_id = 0; // 0 = none
	uint32_t report_size_bits = 0;
	uint32_t report_count = 0;
	int32_t logical_min = 0;
	int32_t logical_max = 0;
	int32_t physical_min = 0;
	int32_t physical_max = 0;
	uint32_t unit = 0;
	int8_t unit_exponent = 0;
};

struct local_state {
	std::vector<uint32_t> usages;
	bool has_usage_minmax = false;
	uint32_t usage_min = 0;
	uint32_t usage_max = 0;
	void clear() {
		usages.clear();
		has_usage_minmax = false;
		usage_min = usage_max = 0;
	}
};

struct item {
	uint8_t size = 0; // 0,1,2,4 or 255 for long
	uint8_t type = 0; // 0=Main,1=Global,2=Local,3=Reserved
	uint8_t tag = 0;  // 4-bit for short, full for long (we won't use long here)
	uint32_t data = 0; // zero-extended, caller may interpret signed
};

static inline bool is_long_item(uint8_t prefix) { return prefix == 0xFE; }

static inline item parse_item(const uint8_t* &p, const uint8_t* end) {
	if (p >= end) { return {}; }
	uint8_t prefix = *p++;
	item it{};
	if (is_long_item(prefix)) {
		if (p + 2 > end) { return {}; }
		uint8_t data_size = *p++;
		(void)*p++; // long item tag, ignore
		it.size = 0xFF;
		it.type = 3;
		it.tag = 0xFF;
		// skip long data
		p = std::min(p + data_size, end);
		return it;
	}
	uint8_t bSizeCode = prefix & 0x03; // 0,1,2,3(=4 bytes)
	it.size = (bSizeCode == 3) ? 4 : bSizeCode;
	it.type = (prefix >> 2) & 0x03;
	it.tag  = (prefix >> 4) & 0x0F;
	// read data bytes little-endian
	uint32_t v = 0;
	for (uint8_t i = 0; i < it.size; ++i) {
		if (p >= end) break;
		v |= (uint32_t)(*p++) << (8 * i);
	}
	it.data = v;
	return it;
}

static inline int32_t sign_extend(uint32_t v, uint8_t size) {
	switch (size) {
		case 1: return (int8_t)v;
		case 2: return (int16_t)v;
		case 4: return (int32_t)v;
		default: return (int32_t)v;
	}
}

static inline const char* collection_type_name(uint8_t t) {
	switch (t) {
		case 0x00: return "Physical";
		case 0x01: return "Application";
		case 0x02: return "Logical";
		case 0x03: return "Report";
		case 0x04: return "Named Array";
		case 0x05: return "Usage Switch";
		case 0x06: return "Usage Modifier";
		default: return "Reserved";
	}
}

static inline const char* field_kind_name(report_descriptor_tree::field_kind k) {
	switch (k) {
		case report_descriptor_tree::field_kind::input: return "Input";
		case report_descriptor_tree::field_kind::output: return "Output";
		case report_descriptor_tree::field_kind::feature: return "Feature";
	}
	return "Unknown";
}

// --- Standard HID annotated helpers ---

static inline std::string usage_page_name(uint16_t page) {
	switch (page) {
		case 0x01: return "Generic Desktop Ctrls";
		case 0x07: return "Kbrd/Keypad";
		case 0x08: return "LEDs";
		case 0x09: return "Button";
		case 0x0C: return "Consumer";
		case 0x0D: return "Digitizer";
		case 0x0E: return "Reserved 0x0E";
		case 0x0A: return "Ordinal";
		default:
			if (page >= 0xFF00 && page <= 0xFFFF) {
				return std::format("Vendor Defined 0x{:04X}", page);
			}
			return std::format("0x{:02X}", page);
	}
}

static inline std::string usage_name(uint16_t page, uint32_t usage) {
	if (page == 0x01) { // Generic Desktop
		switch (usage) {
			case 0x01: return "Pointer";
			case 0x02: return "Mouse";
			case 0x30: return "X";
			case 0x31: return "Y";
			case 0x38: return "Wheel";
			default: break;
		}
	} else if (page == 0x0D) { // Digitizer
		switch (usage) {
			case 0x20: return "Stylus";
			default: break;
		}
	} else if (page == 0x0E) { // Reserved/Haptics-related in the doc
		switch (usage) {
			case 0x01: return "Simple Haptic Controller";
			case 0x24: return "Repeat Count";
			case 0x23: return "Intensity";
			case 0x20: return "Auto Trigger";
			case 0x21: return "Manual Trigger";
			case 0x28: return "Waveform Cutoff Time";
			case 0x25: return "Retrigger Period";
			case 0x22: return "Auto Trigger Associated Control";
			case 0x11: return "Duration List";
			case 0x10: return "Waveform List";
			default: break;
		}
	} else if (page == 0x0C) { // Consumer
		if (usage == 0xE0) return "Volume";
	}
	// Fallback
	return std::format("0x{:X}", usage);
}

static inline std::string input_output_feature_flags_text(uint8_t raw, report_descriptor_tree::field_kind kind) {
	bool is_const = raw & 0x01;
	bool is_var   = raw & 0x02;
	bool is_rel   = raw & 0x04;
	bool is_wrap  = raw & 0x08;
	bool is_nlin  = raw & 0x10;
	bool no_pref  = raw & 0x20;
	bool null_st  = raw & 0x40;
	bool bit7     = raw & 0x80;

	std::string s;
	auto add = [&](std::string v){ if (!s.empty()) s.append(","); s.append(v); };
	add(is_const ? "Const" : "Data");
	add(is_var ? "Var" : "Array");
	add(is_rel ? "Rel" : "Abs");
	add(is_wrap ? "Wrap" : "No Wrap");
	add(is_nlin ? "Non-linear" : "Linear");
	add(no_pref ? "No Preferred State" : "Preferred State");
	add(null_st ? "Null Position" : "No Null Position");
	if (kind == report_descriptor_tree::field_kind::input) {
		add(bit7 ? "Buffered Bytes" : "Bitfield");
	} else {
		add(bit7 ? "Non-volatile" : "Volatile");
	}
	return s;
}

static inline void append_item_bytes(std::ostringstream& os, const uint8_t* start, const uint8_t* end) {
	const uint8_t* p = start;
	bool first = true;
	while (p < end) {
		if (!first) os << ", ";
		first = false;
		os << "0x" << std::format("{:02X}", *p++);
	}
	size_t len = (end - start);
	size_t pads = (len * 6 >= 24) ? 1 : (24 - len * 6);
	os << std::string(pads, ' ');
}

} // namespace

report_descriptor_tree report_descriptor_tree::parse(std::span<const uint8_t> bytes) {
	report_descriptor_tree tree;
	tree.root_ = std::make_unique<collection_node>();
	tree.source_bytes_ = bytes;
	collection_node* current = tree.root_.get();
	std::stack<collection_node*> stack;
	stack.push(current);

	global_state g{};
	std::stack<global_state> gstack;
	local_state l{};

	const uint8_t* p = bytes.data();
	const uint8_t* end = bytes.data() + bytes.size();
	while (p < end) {
		item it = parse_item(p, end);
		switch (it.type) {
			case 0: { // Main
				switch (it.tag) {
					case 0x0A: { // Collection
						auto node = std::make_unique<collection_node>();
						node->type = (uint8_t)it.data;
						node->usage_page = g.usage_page;
						if (!l.usages.empty()) node->usage = l.usages.back();
						current->children.emplace_back(std::move(node));
						current = current->children.back().get();
						stack.push(current);
						l.clear();
						break;
					}
					case 0x0C: { // End Collection
						if (stack.size() > 1) {
							stack.pop();
							current = stack.top();
						}
						l.clear();
						break;
					}
					case 0x08: // Input
					case 0x09: // Output
					case 0x0B: { // Feature
						report_field f{};
						f.kind = (it.tag == 0x08) ? field_kind::input : (it.tag == 0x09) ? field_kind::output : field_kind::feature;
						f.flags.raw = (uint8_t)it.data;
						f.report_id = g.report_id;
						f.usage_page = g.usage_page;
						if (l.has_usage_minmax) {
							for (uint32_t u = l.usage_min; u <= l.usage_max; ++u) {
								f.usages.push_back(u);
							}
						} else if (!l.usages.empty()) {
							f.usages = l.usages;
						}
						f.report_size_bits = g.report_size_bits;
						f.report_count = g.report_count;
						f.logical_min = g.logical_min;
						f.logical_max = g.logical_max;
						f.physical_min = g.physical_min;
						f.physical_max = g.physical_max;
						f.unit = g.unit;
						f.unit_exponent = g.unit_exponent;
						current->fields.emplace_back(std::move(f));
						const report_field* pf = &current->fields.back();
						tree.index_by_report_id_[pf->report_id].push_back(pf);
						l.clear();
						break;
					}
					default:
						// Unknown main item, ignore
						l.clear();
						break;
				}
				break;
			}
			case 1: { // Global
				switch (it.tag) {
					case 0x00: g.usage_page = (uint16_t)it.data; break; // Usage Page
					case 0x01: g.logical_min = sign_extend(it.data, it.size); break;
					case 0x02: g.logical_max = sign_extend(it.data, it.size); break;
					case 0x03: g.physical_min = sign_extend(it.data, it.size); break;
					case 0x04: g.physical_max = sign_extend(it.data, it.size); break;
					case 0x05: g.unit_exponent = (int8_t)sign_extend(it.data, it.size); break;
					case 0x06: g.unit = it.data; break;
					case 0x07: g.report_size_bits = it.data; break;
					case 0x08: g.report_id = (uint8_t)it.data; break;
					case 0x09: g.report_count = it.data; break;
					case 0x0A: gstack.push(g); break; // Push
					case 0x0B: if (!gstack.empty()) { g = gstack.top(); gstack.pop(); } break; // Pop
					default: break;
				}
				break;
			}
			case 2: { // Local
				switch (it.tag) {
					case 0x00: l.usages.push_back(it.data); break; // Usage
					case 0x01: l.has_usage_minmax = true; l.usage_min = it.data; break; // Usage Min
					case 0x02: l.has_usage_minmax = true; l.usage_max = it.data; break; // Usage Max
					default: break;
				}
				break;
			}
			default:
				// Reserved / long: ignore
				break;
		}
	}

	return tree;
}

std::vector<const report_descriptor_tree::report_field*> report_descriptor_tree::find_by_report_id(uint8_t report_id) const {
	auto it = index_by_report_id_.find(report_id);
	if (it == index_by_report_id_.end()) return {};
	return it->second;
}

static void dump_collection(std::ostringstream& os, const report_descriptor_tree::collection_node& node, int indent) {
	auto ind = std::string(indent * 2, ' ');
	os << ind << "Collection(" << collection_type_name(node.type) << ")";
	if (node.usage_page || node.usage) {
		os << " UsagePage=0x" << std::format("{:04X}", node.usage_page);
		if (node.usage) os << " Usage=0x" << std::format("{:X}", node.usage);
	}
	os << "\n";
	for (const auto& f : node.fields) {
		os << ind << "  " << field_kind_name(f.kind)
		   << "(ReportID=" << (unsigned)f.report_id
		   << ", SizeBits=" << f.report_size_bits
		   << ", Count=" << f.report_count
		   << ", Flags=0x" << std::format("{:02X}", f.flags.raw) << ")";
		if (!f.usages.empty()) {
			os << " Usages=[";
			for (size_t i = 0; i < f.usages.size(); ++i) {
				if (i) os << ",";
				os << "0x" << std::format("{:X}", f.usages[i]);
			}
			os << "]";
		}
		os << "\n";
	}
	for (const auto& ch : node.children) {
		dump_collection(os, *ch, indent + 1);
	}
}

std::string report_descriptor_tree::to_string() const {
	std::ostringstream os;
	// Render in HID-standard annotated form from original bytes
	const uint8_t* p = source_bytes_.data();
	const uint8_t* end = source_bytes_.data() + source_bytes_.size();
	uint16_t usage_page = 0;
	int depth = 0;
	while (p && p < end) {
		const uint8_t* start = p;
		item it = parse_item(p, end);
		const uint8_t* ib_end = p;
		append_item_bytes(os, start, ib_end);
		os << "// " << std::string(depth * 2, ' ');
		if (it.type == 0) {
			switch (it.tag) {
				case 0x0A: os << "Collection (" << collection_type_name((uint8_t)it.data) << ")"; ++depth; break;
				case 0x0C: os << "End Collection"; if (depth > 0) --depth; break;
				case 0x08: os << "Input (" << input_output_feature_flags_text((uint8_t)it.data, report_descriptor_tree::field_kind::input) << ")"; break;
				case 0x09: os << "Output (" << input_output_feature_flags_text((uint8_t)it.data, report_descriptor_tree::field_kind::output) << ")"; break;
				case 0x0B: os << "Feature (" << input_output_feature_flags_text((uint8_t)it.data, report_descriptor_tree::field_kind::feature) << ")"; break;
				default: os << "Main (tag=0x" << std::format("{:X}", it.tag) << ")"; break;
			}
		} else if (it.type == 1) {
			switch (it.tag) {
				case 0x00: usage_page = (uint16_t)it.data; os << "Usage Page (" << usage_page_name(usage_page) << ")"; break;
				case 0x01: os << "Logical Minimum (" << sign_extend(it.data, it.size) << ")"; break;
				case 0x02: os << "Logical Maximum (" << sign_extend(it.data, it.size) << ")"; break;
				case 0x03: os << "Physical Minimum (" << sign_extend(it.data, it.size) << ")"; break;
				case 0x04: os << "Physical Maximum (" << sign_extend(it.data, it.size) << ")"; break;
				case 0x05: os << "Unit Exponent"; break;
				case 0x06: os << "Unit (System: SI Linear, Time: Seconds)"; break;
				case 0x07: os << "Report Size (" << it.data << ")"; break;
				case 0x08: os << "Report ID (" << (unsigned)(uint8_t)it.data << ")"; break;
				case 0x09: os << "Report Count (" << it.data << ")"; break;
				default: os << "Global (tag=0x" << std::format("{:X}", it.tag) << ")"; break;
			}
		} else if (it.type == 2) {
			switch (it.tag) {
				case 0x00: os << "Usage (" << usage_name(usage_page, it.data) << ")"; break;
				case 0x01: os << "Usage Minimum (0x" << std::format("{:02X}", (unsigned)it.data) << ")"; break;
				case 0x02: os << "Usage Maximum (0x" << std::format("{:02X}", (unsigned)it.data) << ")"; break;
				default: os << "Local (tag=0x" << std::format("{:X}", it.tag) << ")"; break;
			}
		} else {
			os << "Reserved";
		}
		os << "\n";
	}
	os << "\n// " << source_bytes_.size() << " bytes\n";
	return os.str();
}

} // namespace hid

