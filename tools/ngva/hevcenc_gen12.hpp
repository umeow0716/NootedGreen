#pragma once
// HEVC VDEnc encode: the ICL VA driver's (CodechalVdencHevcStateG11) Gen11
// batch rebuilt as Gen12 before submission, with its generic 0x82 relocation
// record remapped to the moved dwords. Command by command against the Linux
// media driver's Gen12 batch for the same stream (build/claude/hevcenc/ref):
// the order is the same and Gen12 only appends dwords, except
// VDENC_WEIGHTSOFFSETS_STATE (5 -> 3) and VDENC_PIPE_BUF_ADDR_STATE, whose two
// new buffers (tile row store, cumulative CU count) come from the unused
// stream-in buffer. Row stores live in the internal cache at Gen12 offsets.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace NgvaHevcEnc {

struct Output {
	uint32_t batch[2048];
	size_t words = 0;
	uint32_t relocs[1024];
	size_t relocWords = 0;
	const char *reason = "input";
};

namespace detail {
inline uint32_t commandLength(uint32_t h) {
	switch (h >> 29) {
	case 0: {
		const uint32_t op = h >> 23 & 0x3f;
		if (op == 0x20) return (h & 0x3ff) + 2;
		if (op == 0x26) return (h & 0x3f) + 2;
		if (op == 0x22 || op == 0x24 || op == 0x29 || op == 0x2a || op == 0x2e || op == 0x31 || op == 0x1d || op == 0x36) return (h & 0xff) + 2;
		return 1;
	}
	case 3: return h >> 16 == 0x6800 ? 1 : (h & 0xfff) + 2;
	default: return 0;
	}
}
// Memory attributes as the Linux media driver sets them on Gen12; the row-store
// cache select bit stays.
inline uint32_t mocs(uint32_t v) { return v & 0x1000 ? 0x1000 : 6; }
} // namespace detail

// in: the batch (MI_BATCH_BUFFER_END included); record: the 0x82 record,
// length dwords (header included). Entries (5 dwords from record[3] while bit 0
// is set) carry their batch dword offset in bits 5..24.
inline bool convert(const uint32_t *in, size_t words, const uint32_t *record, size_t length, Output &o) {
	using namespace detail;
	o.words = o.relocWords = 0;
	if (words < 16 || words > 1800 || in[0] != 0x13000082 || in[4] || in[5] != 0x10000002 || length < 8 || length > 1024) {
		o.reason = "prologue";
		return false;
	}
	// Frame size and slice QP for the row-store layout, the borrowed buffers and
	// the walker's QP tables.
	uint32_t width = 0, height = 0, qp = 0;
	bool intra = false;
	for (size_t i = 10; i < words;) {
		const uint32_t h = in[i], n = commandLength(h);
		if (!n || i + n > words) { o.reason = "parse"; return false; }
		if (h == 0x70890023) width = (in[i + 1] & 0xffff) + 1, height = (in[i + 1] >> 16) + 1;
		if (h == 0x73940009) qp = in[i + 3] >> 6 & 0x3f, intra = (in[i + 3] & 3) == 2;
		if (h >> 23 == 0x0a) break;
		i += n;
	}
	if (!width || width > 4096 || height < 128 || height > 4096 || !qp) { o.reason = "geometry"; return false; }
	// RowStoreCacheAddrHEVC index 1 (4:2:0, LCU 32/64, width <= 4096), in
	// 64-byte units: metadata line, deblocking line, SAO line, VDEnc, HSAO.
	constexpr uint32_t dat = 0, df = 256, sao = 1280, vdenc = 1824, hsao = 1792;
	const uint32_t tileRowStore = (width + 31) / 32 * 128, cuCount = (width + 63) / 64 * ((height + 63) / 64) * 4,
		cuOffset = (tileRowStore + 63) & ~63U, streamIn = (width + 63) / 64 * 2 * ((height + 63) / 64 * 2) * 64;
	if (cuOffset + cuCount > streamIn) { o.reason = "stream-in size"; return false; }

	static uint32_t map[2048]; // callers serialize
	struct Extra { uint32_t from, to, tmpl; bool replace; };
	Extra extra[16];
	uint32_t extras = 0, drop[4], drops = 0, streamInAt = 0, tileAt = 0;
	uint32_t *out = o.batch;
	size_t w = 0;
	constexpr size_t cap = sizeof(o.batch) / 4;
	auto put = [&](uint32_t v) { if (w < cap) out[w] = v; w++; };

	// Prologue as the ICL VP9 path: MI_FORCE_WAKEUP (HEVC and MFX wells, as the
	// media driver's encode), the status MI_STORE_DATA_IMM moved to dword 2,
	// dword 6 free for the kext's marker. MI stores stay in the batch's PPGTT
	// (the status buffer the driver reads the byte count from).
	for (size_t i = 0; i < 10; i++) map[i] = ~0U;
	const uint32_t prologue[10] = {0x0e800000, 0x03000300, 0x10000002, in[6], in[7], in[8], 0, 0, 0, 0};
	for (uint32_t v : prologue) put(v);
	map[6] = 3, map[7] = 4, map[8] = 5;

	bool pipeMode = false, hcpBuffers = false, walker = false, end = false;
	for (size_t i = 10; i < words;) {
		const uint32_t h = in[i], n = commandLength(h);
		const size_t at = w;
		for (uint32_t k = 0; k < n; k++) map[i + k] = static_cast<uint32_t>(at + k);
		auto copy = [&]() { for (uint32_t k = 0; k < n; k++) put(in[i + k]); };
		auto zeros = [&](uint32_t count) { for (uint32_t k = 0; k < count; k++) put(0); };
		if (h >> 29 == 0) {
			const uint32_t op = h >> 23 & 0x3f;
			copy();
			if (op == 0x0a) { end = true; break; }
		} else switch (h) {
		case 0x738a0001:
			if (in[i + 1] == 1) { // VDENC_CONTROL_STATE initialization first
				put(0x708b0000), put(2);
				for (uint32_t k = 0; k < n; k++) map[i + k] = static_cast<uint32_t>(w + k);
				copy();
			} else if (in[i + 1] == 0 && in[i + 2] == 4) { for (uint32_t k = 0; k < n; k++) map[i + k] = ~0U; } // not in Gen12 encode
			else copy();
			break;
		case 0x73810002: { // HCP_SURFACE_STATE + DW4 (no compression)
			put(0x73810003);
			for (uint32_t k = 1; k < n; k++) put(in[i + k]);
			put(0);
			// Gen12 also takes the reference surfaces' format (ID 5), as the
			// reconstructed picture's.
			if (in[i + 1] >> 28 == 0 && extras + 2 <= 16) {
				const size_t ref = w;
				put(0x73810003), put((in[i + 1] & 0x0fffffff) | 5U << 28), put(in[i + 2]), put(in[i + 3]), put(0);
				extra[extras++] = {static_cast<uint32_t>(i + 1), static_cast<uint32_t>(ref + 1), 5U << 28, true};
				extra[extras++] = {static_cast<uint32_t>(i + 2), static_cast<uint32_t>(ref + 2), 0, false};
			}
			break;
		}
		case 0x73820077: { // HCP_PIPE_BUF_ADDR_STATE
			copy();
			static const uint8_t attributes[] = {3, 6, 9, 12, 15, 18, 21, 24, 27, 30, 33, 36, 53, 56, 59, 62, 65, 82, 85, 88, 91, 94, 97, 100,
				103, 106, 109, 112, 115, 118};
			for (uint32_t k : attributes) out[at + k] = mocs(out[at + k]);
			out[at + 4] = df << 6, out[at + 5] = 0, out[at + 6] = 0x1000;
			out[at + 13] = dat << 6, out[at + 14] = 0, out[at + 15] = 0x1000;
			out[at + 22] = sao << 6, out[at + 23] = 0, out[at + 24] = 0x1000;
			out[at + 95] = hsao << 6, out[at + 96] = 0, out[at + 97] = 0x1000;
			drop[drops++] = static_cast<uint32_t>(i + 95);
			hcpBuffers = true;
			break;
		}
		case 0x7383001b: { // HCP_IND_OBJ_BASE_ADDR_STATE
			copy();
			static const uint8_t attributes[] = {3, 8, 11, 16, 19, 22, 25, 28};
			for (uint32_t k : attributes) out[at + k] = mocs(out[at + k]);
			break;
		}
		case 0x70800000: // VDENC_PIPE_MODE_SELECT 2 -> 6: 4:2:0, prefetch as the media driver
			put(0x70800004), put(in[i + 1] | 1U << 15 | 1U << 21 | 2U << 23), put(0x002b030a), put(0x04010303), put(0), put(0);
			pipeMode = true;
			break;
		case 0x70810004:
		case 0x70820004:
		case 0x70830008: // surfaces: the Cr row offset (DW5, DW9) as the Cb one
			copy();
			if (extras + 2 <= 16) {
				extra[extras++] = {static_cast<uint32_t>(i + 4), static_cast<uint32_t>(at + 5), 0, false};
				if (h == 0x70830008) extra[extras++] = {static_cast<uint32_t>(i + 8), static_cast<uint32_t>(at + 9), 0, false};
			}
			break;
		case 0x7084003c: // VDENC_PIPE_BUF_ADDR_STATE 62 -> 71
			copy();
			out[at] = 0x70840045;
			for (uint32_t k = 3; k <= 60; k += 3) out[at + k] = mocs(out[at + k]);
			out[at + 16] = vdenc << 6, out[at + 17] = 0, out[at + 18] = 0x1000;
			drop[drops++] = static_cast<uint32_t>(i + 16);
			streamInAt = static_cast<uint32_t>(i + 13);
			tileAt = static_cast<uint32_t>(w);
			put(0), put(0), put(6); // tile row store
			put(0), put(0), put(6); // cumulative CU count
			zeros(3);               // palette mode stream-out
			break;
		case 0x708a001c: // VDENC_CMD1 + DW30
			copy();
			out[at] = 0x708a001d;
			put(intra ? 0x00101010 : 0x00232323);
			break;
		case 0x73900024: // HCP_PIC_STATE 38 -> 41
			copy();
			out[at] = 0x73900027;
			zeros(3);
			break;
		case 0x70890023: // VDENC_CMD2 37 -> 51, stream-in off (its buffer is borrowed)
			copy();
			out[at] = 0x70890031;
			out[at + 7] &= ~(1U << 9);
			zeros(14);
			break;
		case 0x73880080: // HCP_RDOQ_STATE 130 -> 154
			copy();
			out[at] = 0x73880098;
			zeros(24);
			break;
		case 0x73940009: // HCP_SLICE_STATE 11 -> 13
			copy();
			out[at] = 0x7394000b;
			zeros(2);
			break;
		case 0x70880003: // VDENC_WEIGHTSOFFSETS_STATE: the HEVC dwords (G11 DW3-4)
			put(0x70880001), put(in[i + 3]), put(in[i + 4]);
			for (uint32_t k = 0; k < n; k++) map[i + k] = ~0U;
			break;
		case 0x70870008: { // VDENC_WALKER_STATE 10 -> 27, DW12-16 by slice QP
			copy();
			out[at] = 0x70870019;
			static const uint32_t t1[10] = {0x50001, 0x50001, 0x50001, 0x50002, 0x90002, 0x90002, 0x90002, 0xd0002, 0x190002, 0x210003},
				t2[10] = {0x2000a, 0x2000a, 0x2000a, 0x4000a, 0x8000a, 0xc0010, 0xc0018, 0xc0018, 0x100020, 0x180030},
				t3[10] = {0x101004, 0x101004, 0x101004, 0xc1008, 0x42004, 0x42006, 0x13f06, 0x13f06, 0x13f0c, 0x13006},
				t4[10] = {0x100004, 0x100004, 0x100004, 0x100004, 0x200004, 0x300004, 0x400004, 0x600004, 0x800004, 0x1000004};
			const uint32_t x = qp <= 12 ? 0 : qp <= 47 ? (qp - 8) / 5 : qp <= 49 ? 8 : 9;
			put(0), put(0);
			put(0x1000000 | t1[x]), put(t2[x]), put(0xc8400000 | (t3[x] & 0x9fffff)), put(t4[x]), put(0xa63f3f3f);
			zeros(10);
			walker = true;
			break;
		}
		default:
			if (h >> 29 != 3) { o.reason = "command"; return false; }
			copy();
		}
		i += n;
	}
	if (!end || !pipeMode || !hcpBuffers || !walker || !streamInAt || w > cap) { o.reason = "frame"; return false; }
	o.words = w;

	// Relocations at their new offsets, the extra surface ones, the borrowed
	// stream-in buffer twice.
	uint32_t *r = o.relocs;
	size_t entries = 0;
	constexpr size_t maxEntries = (sizeof(o.relocs) / 4 - 3) / 5;
	const uint32_t *streamInEntry = nullptr;
	auto emit = [&](const uint32_t *e, uint32_t to, uint32_t tmpl, bool replace) {
		if (3 + (entries + 1) * 5 > length || entries >= maxEntries) return false;
		uint32_t *d = r + 3 + entries++ * 5;
		memcpy(d, e, 20);
		d[0] = (d[0] & ~(0xfffffU << 5)) | to << 5;
		if (replace) d[2] = (d[2] & 0x0fffffff) | tmpl;
		return true;
	};
	for (size_t k = 3; k + 5 <= length && record[k] & 1; k += 5) {
		const uint32_t *e = record + k, from = e[0] >> 5 & 0xfffff;
		if (from >= words) { o.reason = "relocation offset"; return false; }
		if (from == streamInAt) streamInEntry = e;
		bool dropped = false;
		for (uint32_t d = 0; d < drops; d++) dropped |= drop[d] == from;
		if (dropped) continue;
		if (map[from] == ~0U || !emit(e, map[from], 0, false)) { o.reason = "relocation"; return false; }
		for (uint32_t x = 0; x < extras; x++)
			if (extra[x].from == from && !emit(e, extra[x].to, extra[x].tmpl, extra[x].replace)) { o.reason = "relocation"; return false; }
	}
	if (!streamInEntry || (streamInEntry[0] >> 1 & 3)) { o.reason = "stream-in"; return false; }
	uint32_t borrowed[5] = {streamInEntry[0] | 1U << 4, streamInEntry[1], streamInEntry[2], 0, 0};
	if (!emit(borrowed, tileAt, 0, false)) { o.reason = "relocation"; return false; }
	borrowed[2] += cuOffset;
	if (!emit(borrowed, tileAt + 3, 0, false)) { o.reason = "relocation"; return false; }
	r[0] = record[0], r[1] = record[1], r[2] = record[2];
	memset(r + 3 + entries * 5, 0, (length - 3 - entries * 5) * 4);
	o.relocWords = length;
	o.reason = "converted";
	return true;
}

} // namespace NgvaHevcEnc
