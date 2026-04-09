// HID Report Descriptor parser — table-driven per USB HID 1.11.
#include "hid_report_desc.h"
#include "priv/hid_report_item.h"

#include <iterator>
#include <stack>
#include <array>
#include <algorithm>

namespace hid {

namespace {

using detail::item;
using detail::parse_item;
using detail::sign_extend;

// ════════════════════════════════════════════════════════════════════════════
// Parse handler table — maps (type, tag) → state-machine action
// ════════════════════════════════════════════════════════════════════════════

struct global_state {
	uint16_t usage_page = 0;
	uint8_t  report_id = 0;
	uint32_t report_size_bits = 0;
	uint32_t report_count = 0;
	int32_t  logical_min = 0;
	int32_t  logical_max = 0;
	int32_t  physical_min = 0;
	int32_t  physical_max = 0;
	uint32_t unit = 0;
	int8_t   unit_exponent = 0;
};

struct local_state {
	std::vector<uint32_t> usages;
	bool     has_usage_minmax = false;
	uint32_t usage_min = 0;
	uint32_t usage_max = 0;
	void clear() { usages.clear(); has_usage_minmax = false; usage_min = usage_max = 0; }
};

using collection_node = report_descriptor_tree::collection_node;
using report_field    = report_descriptor_tree::report_field;
using field_kind      = report_descriptor_tree::field_kind;

struct parse_ctx {
	collection_node*              current;
	std::stack<collection_node*>& node_stack;
	global_state&                 g;
	std::stack<global_state>&     g_stack;
	local_state&                  l;
};

using parse_handler_fn = void(*)(parse_ctx&, const item&);

// --- Main item handlers ---

static void make_report_field(parse_ctx& ctx, const item& it, field_kind kind) {
	report_field f{};
	f.kind            = kind;
	f.flags.raw       = static_cast<uint8_t>(it.data);
	f.report_id       = ctx.g.report_id;
	f.usage_page      = ctx.g.usage_page;
	if (ctx.l.has_usage_minmax) {
		for (uint32_t u = ctx.l.usage_min; u <= ctx.l.usage_max; ++u)
			f.usages.push_back(u);
	} else if (!ctx.l.usages.empty()) {
		f.usages = ctx.l.usages;
	}
	f.report_size_bits = ctx.g.report_size_bits;
	f.report_count     = ctx.g.report_count;
	f.logical_min      = ctx.g.logical_min;
	f.logical_max      = ctx.g.logical_max;
	f.physical_min     = ctx.g.physical_min;
	f.physical_max     = ctx.g.physical_max;
	f.unit             = ctx.g.unit;
	f.unit_exponent    = ctx.g.unit_exponent;
	ctx.current->fields.emplace_back(std::move(f));
	ctx.l.clear();
}

static void handle_input     (parse_ctx& c, const item& it) { make_report_field(c, it, field_kind::input);   }
static void handle_output    (parse_ctx& c, const item& it) { make_report_field(c, it, field_kind::output);  }
static void handle_feature   (parse_ctx& c, const item& it) { make_report_field(c, it, field_kind::feature); }

static void handle_collection(parse_ctx& c, const item& it) {
	auto node = std::make_unique<collection_node>();
	node->type       = static_cast<uint8_t>(it.data);
	node->usage_page = c.g.usage_page;
	if (!c.l.usages.empty()) node->usage = c.l.usages.back();
	c.current->children.emplace_back(std::move(node));
	c.current = c.current->children.back().get();
	c.node_stack.push(c.current);
	c.l.clear();
}

static void handle_end_collection(parse_ctx& c, const item&) {
	if (c.node_stack.size() > 1) { c.node_stack.pop(); c.current = c.node_stack.top(); }
	c.l.clear();
}

static void handle_main_unknown(parse_ctx& c, const item&) { c.l.clear(); }

// --- Global item handlers ---

static void g_usage_page   (parse_ctx& c, const item& it) { c.g.usage_page      = static_cast<uint16_t>(it.data); }
static void g_logical_min  (parse_ctx& c, const item& it) { c.g.logical_min     = sign_extend(it.data, it.size); }
static void g_logical_max  (parse_ctx& c, const item& it) { c.g.logical_max     = sign_extend(it.data, it.size); }
static void g_physical_min (parse_ctx& c, const item& it) { c.g.physical_min    = sign_extend(it.data, it.size); }
static void g_physical_max (parse_ctx& c, const item& it) { c.g.physical_max    = sign_extend(it.data, it.size); }
static void g_unit_exponent(parse_ctx& c, const item& it) { c.g.unit_exponent   = static_cast<int8_t>(sign_extend(it.data, it.size)); }
static void g_unit         (parse_ctx& c, const item& it) { c.g.unit            = it.data; }
static void g_report_size  (parse_ctx& c, const item& it) { c.g.report_size_bits = it.data; }
static void g_report_id    (parse_ctx& c, const item& it) { c.g.report_id       = static_cast<uint8_t>(it.data); }
static void g_report_count (parse_ctx& c, const item& it) { c.g.report_count    = it.data; }
static void g_push         (parse_ctx& c, const item&)    { c.g_stack.push(c.g); }
static void g_pop          (parse_ctx& c, const item&)    { if (!c.g_stack.empty()) { c.g = c.g_stack.top(); c.g_stack.pop(); } }

// --- Local item handlers ---

static void l_usage     (parse_ctx& c, const item& it) { c.l.usages.push_back(it.data); }
static void l_usage_min (parse_ctx& c, const item& it) { c.l.has_usage_minmax = true; c.l.usage_min = it.data; }
static void l_usage_max (parse_ctx& c, const item& it) { c.l.has_usage_minmax = true; c.l.usage_max = it.data; }

// --- Unified parse dispatch table ---

struct parse_entry {
	uint8_t         type;
	uint8_t         tag;
	parse_handler_fn handler;
};

static constexpr auto parse_table = std::to_array<parse_entry>({
	// Main (type=0)
	{0, 0x08, handle_input},
	{0, 0x09, handle_output},
	{0, 0x0A, handle_collection},
	{0, 0x0B, handle_feature},
	{0, 0x0C, handle_end_collection},
	// Global (type=1)
	{1, 0x00, g_usage_page},    {1, 0x01, g_logical_min},
	{1, 0x02, g_logical_max},   {1, 0x03, g_physical_min},
	{1, 0x04, g_physical_max},  {1, 0x05, g_unit_exponent},
	{1, 0x06, g_unit},          {1, 0x07, g_report_size},
	{1, 0x08, g_report_id},     {1, 0x09, g_report_count},
	{1, 0x0A, g_push},          {1, 0x0B, g_pop},
	// Local (type=2)
	{2, 0x00, l_usage},
	{2, 0x01, l_usage_min},
	{2, 0x02, l_usage_max},
});
static constexpr auto parse_entry_proj(const parse_entry& e) noexcept {
	return std::make_tuple(e.type, e.tag);
}
static_assert(std::ranges::is_sorted(parse_table, {}, &parse_entry_proj));

static parse_handler_fn find_parse_handler(uint8_t type, uint8_t tag) noexcept {
	auto key = std::make_tuple(type, tag);
	auto it = std::ranges::lower_bound(parse_table, key, {}, &parse_entry_proj);
	if (it != std::ranges::end(parse_table) && parse_entry_proj(*it) == key) return it->handler;
	return nullptr;
}

// Build report-id index by walking the finished tree (avoids pointer invalidation)
static void build_report_index(
	const collection_node& node,
	std::unordered_map<uint8_t, std::vector<const report_field*>>& idx)
{
	for (const auto& f : node.fields)
		idx[f.report_id].push_back(&f);
	for (const auto& ch : node.children)
		build_report_index(*ch, idx);
}

} // namespace

// ════════════════════════════════════════════════════════════════════════════
// Public API
// ════════════════════════════════════════════════════════════════════════════

report_descriptor_tree report_descriptor_tree::parse(std::span<const uint8_t> bytes) {
	report_descriptor_tree tree;
	tree.root_ = std::make_unique<collection_node>();
	std::stack<collection_node*> nstack;
	nstack.push(tree.root_.get());

	global_state g{};
	std::stack<global_state> gstack;
	local_state l{};

	parse_ctx ctx{tree.root_.get(), nstack, g, gstack, l};

	const uint8_t* p   = bytes.data();
	const uint8_t* end = p + bytes.size();
	while (p < end) {
		item it = parse_item(p, end);
		auto handler = find_parse_handler(it.type, it.tag);
		if (handler) {
			handler(ctx, it);
		} else if (it.type == 0) {
			handle_main_unknown(ctx, it);
		}
	}

	build_report_index(*tree.root_, tree.index_by_report_id_);
	return tree;
}

std::vector<const report_descriptor_tree::report_field*>
report_descriptor_tree::find_by_report_id(uint8_t report_id) const {
	auto it = index_by_report_id_.find(report_id);
	if (it == index_by_report_id_.end()) return {};
	return it->second;
}

} // namespace hid

