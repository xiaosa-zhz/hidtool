// Internal header: low-level HID item representation shared by the parser and
// the descriptor dumper.  Not part of the public API.
#pragma once

#include <cstdint>
#include <algorithm>

namespace hid {
namespace detail {

// ════════════════════════════════════════════════════════════════════════════
// Low-level item representation (HID 1.11 §6.2.2.2)
// ════════════════════════════════════════════════════════════════════════════

struct item {
	uint8_t  size = 0; // 0,1,2,4 — or 0xFF for long items
	uint8_t  type = 0; // 0=Main, 1=Global, 2=Local, 3=Reserved
	uint8_t  tag  = 0;
	uint32_t data = 0;
};

static inline item parse_item(const uint8_t*& p, const uint8_t* end) noexcept {
	if (p >= end) return {};
	uint8_t prefix = *p++;
	item it{};
	if (prefix == 0xFE) { // long item
		if (p + 2 > end) return {};
		uint8_t data_size = *p++;
		(void)*p++;
		it.size = 0xFF; it.type = 3; it.tag = 0xFF;
		p = std::min(p + data_size, end);
		return it;
	}
	uint8_t bSizeCode = prefix & 0x03;
	it.size = (bSizeCode == 3) ? 4 : bSizeCode;
	it.type = (prefix >> 2) & 0x03;
	it.tag  = (prefix >> 4) & 0x0F;
	uint32_t v = 0;
	for (uint8_t i = 0; i < it.size; ++i) {
		if (p >= end) break;
		v |= static_cast<uint32_t>(*p++) << (8 * i);
	}
	it.data = v;
	return it;
}

static constexpr int32_t sign_extend(uint32_t v, uint8_t size) noexcept {
	switch (size) {
		case 1: return static_cast<int8_t>(static_cast<uint8_t>(v));
		case 2: return static_cast<int16_t>(static_cast<uint16_t>(v));
		default: return static_cast<int32_t>(v);
	}
}

} // namespace detail
} // namespace hid
