#pragma once
// Userspace half of the AVC VDEnc Gen9 -> Gen12 conversion (formerly kext V300).
// Apple's VA driver builds a Gen9 (KBL) VDEnc batch and describes its resources
// with a 0x8d00 sideband token; the kext's process_token_VDENC patches fixed
// Gen9 offsets from it. Here the batch is converted before submission
// (NgreenAvcGen12::produceBeforeRelocation) and the token is replaced, in
// place, by a generic 0x82xx relocation record whose entries target the
// converted batch. Layouts: build/claude/notes/vdenc-token-relocs.md.
#include <stddef.h>
#include <stdint.h>
#include "kern_avc_gen12_commands.hpp"

namespace NgvaVdenc {

struct Reloc { uint32_t dw0, handles, delta, x, y; };

enum : uint32_t { kAddress = 0, kPitch = 1, kChromaOffset = 2, kBind = 3 };

struct Output {
	uint32_t batch[1536];
	size_t words = 0;
	Reloc relocs[96];
	size_t count = 0;
	const char *reason = "input";
};

namespace detail {
inline uint32_t field(const uint32_t *p, uint32_t offset) { return p[offset / 4]; }
} // namespace detail

// in: Gen9 batch as built (no MI_BATCH_BUFFER_END yet), words long.
// token: the 0x8d00 record (header included), tokenWords long.
inline bool convert(const uint32_t *in, size_t words, const uint32_t *token, size_t tokenWords, Output &o) {
	using detail::field;
	o.count = 0;
	if ((token[0] & 0xffff) != 0x8d00 || tokenWords < 2 + 0xce0 / 4 || token[1] != 0) { o.reason = "token"; return false; }
	const uint32_t *P = token + 2;
	const uint32_t entries = P[0];
	if (!entries || entries > 0x80 || 2 + entries * 6 > 0xc08 / 4) { o.reason = "entries"; return false; }
	auto handle = [&](uint32_t idx) { return P[2 + idx * 6]; };
	auto writes = [&](uint32_t idx) { return P[2 + idx * 6 + 5] & 1; };

	uint32_t width = 0, height = 0;
	for (size_t i = 0; i + 2 < words; i++)
		if (in[i] == 0x70810004) { width = ((in[i + 2] >> 4) & 0x3fff) + 1; height = (in[i + 2] >> 18) + 1; break; }
	static uint16_t map[1024]; // callers serialize
	const auto r = NgreenAvcGen12::produceBeforeRelocation(in, words, width, height, o.batch, 1536, map);
	if (!r.ok) { o.reason = r.reason; return false; }
	o.words = r.words;

	bool bad = false;
	bool used[0x80] {};
	auto add = [&](uint32_t type, size_t at, uint32_t h1, uint32_t h2, uint32_t delta, uint32_t shift, uint32_t write) {
		if (at >= words || map[at] == 0xffff || o.count >= sizeof(o.relocs) / sizeof(o.relocs[0]) || h1 > 0xffff || h2 > 0xffff) { bad = true; return; }
		// Every entry binds its handle(s) with this write flag (residency).
		o.relocs[o.count++] = {1U | type << 1 | write << 4 | uint32_t(map[at]) << 5 | shift << 25, h1 | h2 << 16, delta, 0, 0};
	};
	auto idx = [&](uint32_t offset) {
		const uint32_t i = field(P, offset);
		if (i >= entries) bad = true;
		else used[i] = true;
		return i < entries ? i : 0;
	};
	// Address: full 64-bit write of A + delta; delta keeps the low bits the
	// kext preserved (RMW48) or the byte offset it added.
	auto address = [&](size_t at, uint32_t i, uint32_t delta) { add(kAddress, at, handle(i), 0, delta, 0, writes(i)); };
	// Pitch dword: ((pitch - 1) << 3) + the kext's template; VDENC surfaces
	// also get produce()'s Gen12 format (bits 27..31 = 4, bits 20..26 clear).
	auto pitch = [&](size_t at, uint32_t i, uint32_t tmpl, bool vdenc) {
		add(kPitch, at, handle(i), 0, vdenc ? (tmpl & 0x000fffffU) | 4U << 27 : tmpl, 3, writes(i));
	};
	auto chroma = [&](size_t at, uint32_t y, uint32_t uv) {
		add(kChromaOffset, at, handle(y), handle(uv), 0, 0, writes(y) | writes(uv));
		add(kChromaOffset, at + 1, handle(y), handle(uv), 0, 0, writes(y) | writes(uv));
	};

	const uint32_t srcY = idx(0xc08), srcUv = idx(0xc0c), reconY = idx(0xc58), dsY = idx(0xc60), dsUv = idx(0xc64);
	const size_t b0 = field(P, 0xc18), b1 = field(P, 0xc1c);
	if (b0 + 0x1e8 / 4 > words || b1 + 0xdc / 4 > words) { o.reason = "bases"; return false; }
	// MFX group (MFX_SURFACE_STATE x3, PIPE_BUF_ADDR at +0x48, IND_OBJ at +0x158, BSP at +0x1c0).
	pitch(b0 + 3, srcY, field(P, 0xc30), false), chroma(b0 + 4, srcY, srcUv);
	pitch(b0 + 9, srcY, field(P, 0xc30), false), chroma(b0 + 10, srcY, srcUv);
	pitch(b0 + 15, dsY, field(P, 0xc34), false), chroma(b0 + 16, dsY, dsUv);
	address(b0 + 19, reconY, in[b0 + 19] & 0x3f);
	address(b0 + 22, reconY, in[b0 + 22] & 0x3f);
	address(b0 + 25, srcY, in[b0 + 25] & 0x3f);
	address(b0 + 80, dsY, in[b0 + 80] & 0x3f);
	for (uint32_t k = 0; k < 16; k++)
		if (const uint32_t i = field(P, 0xc78 + 4 * k) & 0xfff) { if (i >= entries) bad = true; else { used[i] = true; address(b0 + 37 + 2 * k, i, in[b0 + 37 + 2 * k] & 0x3f); } }
	if (field(P, 0xc10)) if (const uint32_t i = idx(0xc68)) address(b0 + 28, i, in[b0 + 28] & 0x3f);
	if (const uint32_t i = idx(0xc6c)) address(b0 + 31, i, in[b0 + 31] & 0x3f);
	if (const uint32_t i = idx(0xc74)) address(b0 + 34, i, in[b0 + 34] & 0x3f);
	if (const uint32_t i = idx(0xccc)) address(b0 + 113, i, 0);
	if (const uint32_t i = idx(0xcd8)) {
		address(b0 + 107, i, 0);
		address(b0 + 110, i, field(P, 0xcdc) & ~0xfffU);
	}
	// VDENC group (Gen9 layout in the input; map moves it).
	pitch(b1 + 3, srcY, field(P, 0xc38), true), chroma(b1 + 4, srcY, srcUv);
	pitch(b1 + 9, srcY, field(P, 0xc38), true), chroma(b1 + 10, srcY, srcUv);
	pitch(b1 + 15, dsY, field(P, 0xc3c), true), chroma(b1 + 16, dsY, dsUv);
	const struct { uint32_t at, offset; } vdenc[] {{19, 0xcc4}, {22, 0xcc8}, {31, 0xc28}, {34, 0xc70}, {40, 0xcb8}, {43, 0xcbc}, {46, 0xcc0}};
	for (const auto &v : vdenc)
		if (const uint32_t i = idx(v.offset)) address(b1 + v.at, i, 0);
	address(b1 + 28, srcY, 0);
	// Status stores.
	if (const uint32_t s = idx(0xc2c)) {
		const size_t d = field(P, 0xc20) + field(P, 0xc24);
		address(6, s, field(P, 0xc40) & ~3U);
		address(d + 1, s, field(P, 0xc44) & ~3U);
		for (uint32_t k = 0; k < 4; k++) address(d + 12 + 4 * k, s, field(P, 0xc48 + 4 * k) & ~3U);
	}
	// MFX stream-out (DW10), MB status (DW52) and slice size stream-out (DW65):
	// Apple leaves them 0 but Gen12 VDEnc writes them. Use the unused VDEnc
	// stream-in buffer (stream-in is disabled) as their scratch.
	if (const uint32_t streamIn = field(P, 0xc28); streamIn && streamIn < entries) {
		static const uint8_t slots[] {10, 52, 65};
		static const uint32_t offsets[] {0x10000, 0x20000, 0};
		for (uint32_t s = 0; s < 3; s++) {
			const size_t at = b0 + 18 + slots[s];
			if (!in[at] && !in[at + 1]) address(at, streamIn, offsets[s]);
		}
	}
	// Bind the rest (residency and write flags) as the VDENC token did.
	for (uint32_t i = 1; i < entries; i++)
		if (!used[i] && handle(i) && o.count < sizeof(o.relocs) / sizeof(o.relocs[0]))
			o.relocs[o.count++] = {1U | kBind << 1 | writes(i) << 4, handle(i), 0, 0, 0};
	if (bad) { o.reason = "relocation"; return false; }

	// Gen12 VDENC_IMG_STATE takes the frame QP in DW14 (KBL had no such field):
	// use the first MFX_AVC_SLICE_STATE's slice QP like media-driver.
	for (size_t k = 0, image = 0; k + 14 < o.words; k++) {
		if (o.batch[k] == 0x70850021) image = k;
		if (o.batch[k] == 0x71030009 && image) { o.batch[image + 14] = (o.batch[image + 14] & ~0xffU) | ((o.batch[k + 3] >> 16) & 0x3f); break; }
	}
	// MFX_AVC_IMG_STATE: frame-only 4:2:0 (DW4), InterMbMaxSz 0xfff (DW6) as media-driver.
	for (size_t k = 0; k + 6 < o.words; k++)
		if (o.batch[k] == 0x71000013) {
			o.batch[k + 4] = (o.batch[k + 4] & ~0xc02U) | 0x400U;
			o.batch[k + 6] = (o.batch[k + 6] & 0xffffU) | 0x0fff0000U;
			break;
		}
	o.reason = "converted";
	return true;
}

} // namespace NgvaVdenc
