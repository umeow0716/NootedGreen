// Tahoe AppleGVA front for the TGL VA driver.
//
// AppleGVA on Tahoe expects two things the TGL VA driver predates:
//  - the accelerator description returned in metadata+8 uses 0x438-byte codec
//    entries, with an extra 8-byte field at 0x20 (a copy of the field at 0x18);
//    the driver builds 0x428-byte entries;
//  - GetDriverParameter answers tag 0xe with that description;
//  - the H.264 SPS passed with each frame has two more dwords before
//    level_idc (constraint_set4/5), shifting every later field by 8.
// The driver's own media kernels (TNR, checksum, statistics, scaler...) are
// Gen11 ISA; Gen12 rebuilds placed in Resources replace them when present.
// Load the driver, convert its description and SPS, and answer tag 0xe here.
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <IOSurface/IOSurface.h>
#include <malloc/malloc.h>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sys/mman.h>
#include <unistd.h>
#include "vdenc_gen12.hpp"
#include <cstdio>
#include <map>
#include <syslog.h>
#include <utility>

extern "C" int AVD_CreateAVDAccelerator(void **, const void *, void *);

namespace {

using Create = int (*)(void **accelerator, const void *input, void *metadata);
using Destroy = int (*)(void *accelerator);
using GetParameter = int (*)(void *self, void *parameters);
using Decode = int (*)(void *self, void *frames, void *out);

Create driverCreate;
Destroy driverDestroy;
GetParameter driverGetParameter;
Decode driverDecode;
uintptr_t driverBase;
std::mutex lock;
std::map<void *, void *> descriptions; // accelerator -> converted description
std::map<void *, uint8_t *> spsCopies; // Tahoe SPS -> driver-layout copy

constexpr size_t slots = 0x23, oldEntry = 0x428, newEntry = 0x438;

struct Description {
	uint8_t head[0x10];
	uint32_t count;
	uint32_t pad;
	uint8_t **slot;
};

// Old: entry[0x428] linked through entry+0x428. New: entry[0x438] linked
// through entry+0x430, field 0x18 duplicated at 0x20, rest shifted by 8.
uint8_t *convert(const Description *old, const uint8_t *extra = nullptr) {
	if (!old || old->count != slots || !old->slot) return nullptr;
	size_t entries = 0;
	for (size_t i = 0; i < slots; i++)
		for (auto *e = old->slot[i]; e; e = *reinterpret_cast<uint8_t **>(e + oldEntry))
			entries++;
	const size_t total = slots + (extra ? 1 : 0);
	auto *out = static_cast<uint8_t *>(calloc(1, 0x20 + total * 8 + (entries + (extra ? 1 : 0)) * newEntry));
	if (!out) return nullptr;
	memcpy(out, old, 0x20);
	auto *slot = reinterpret_cast<uint8_t **>(out + 0x20);
	reinterpret_cast<Description *>(out)->slot = slot;
	reinterpret_cast<Description *>(out)->count = static_cast<uint32_t>(total);
	auto *next = out + 0x20 + total * 8;
	if (extra) {
		// Already in the new layout (ICL driver), alone in the last slot.
		slot[slots] = next + entries * newEntry;
		memcpy(slot[slots], extra, newEntry);
		*reinterpret_cast<uint8_t **>(slot[slots] + 0x430) = nullptr;
	}
	for (size_t i = 0; i < slots; i++) {
		uint8_t **link = &slot[i];
		for (auto *e = old->slot[i]; e; e = *reinterpret_cast<uint8_t **>(e + oldEntry)) {
			*link = next;
			memcpy(next, e, 0x20);
			memcpy(next + 0x20, e + 0x18, 8);
			memcpy(next + 0x28, e + 0x20, oldEntry - 0x20);
			link = reinterpret_cast<uint8_t **>(next + 0x430);
			next += newEntry;
		}
		*link = nullptr;
	}
	return out;
}

struct Entry { uint32_t tag; uint32_t pad; void *value; };
struct Parameters { uint32_t count; uint32_t pad; Entry *entry; };

int getParameter(void *self, void *raw) {
	auto *parameters = static_cast<Parameters *>(raw);
	void *description = nullptr;
	{
		std::lock_guard<std::mutex> guard(lock);
		auto it = descriptions.find(*static_cast<void **>(self));
		if (it != descriptions.end()) description = it->second;
	}
	uint32_t forwarded = 0;
	for (uint32_t i = 0; i < parameters->count; i++) {
		if (parameters->entry[i].tag == 0xe) {
			if (!description) return 0xb;
			parameters->entry[i].value = description;
		} else {
			forwarded++;
		}
	}
	if (!forwarded) return 0;
	if (forwarded == parameters->count) return driverGetParameter(self, raw);
	Entry rest[forwarded];
	uint32_t n = 0;
	for (uint32_t i = 0; i < parameters->count; i++)
		if (parameters->entry[i].tag != 0xe) rest[n++] = parameters->entry[i];
	Parameters sub {forwarded, 0, rest};
	int result = driverGetParameter(self, &sub);
	n = 0;
	for (uint32_t i = 0; i < parameters->count; i++)
		if (parameters->entry[i].tag != 0xe) parameters->entry[i] = rest[n++];
	return result;
}

// Driver SPS: Tahoe's layout without the dwords at 0x1c and 0x20. Each frame
// points at its picture (+0x20), which points at the SPS (+0x420).
constexpr size_t picturePointer = 0x20, spsPointer = 0x420, spsGap = 0x1c, spsShift = 8;

uint8_t *driverSps(uint8_t *sps) {
	const size_t size = malloc_size(sps);
	if (size < 0x880) return nullptr;
	std::lock_guard<std::mutex> guard(lock);
	auto &copy = spsCopies[sps];
	if (!copy) copy = static_cast<uint8_t *>(calloc(1, size));
	if (!copy) return nullptr;
	memcpy(copy, sps, spsGap);
	memcpy(copy + spsGap, sps + spsGap + spsShift, size - spsGap - spsShift);
	return copy;
}

using SetParameters = int (*)(void *self, void *parameters);
SetParameters driverSetParameters;

// AppleGVA sets parameter kind 1 (stream out) on the H.264 encoder, which
// the driver's encoder does not implement; accept it without forwarding.
int setParameters(void *self, void *raw) {
	auto *parameters = static_cast<Parameters *>(raw);
	if (parameters && parameters->count == 1 && parameters->entry &&
	    *reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(parameters->entry) + 0x20) == 1)
		return 0;
	return driverSetParameters(self, raw);
}

struct Frames { uint32_t count; uint32_t pad; uint8_t **frame; };

// Tahoe's VDEnc H.264 frame (job type 0x20, value 0x4000) and the layout the
// driver's VDEnc encoder reads: same SPS/PPS/slice objects, moved fields.
template <typename T> T &at(uint8_t *p, size_t o) { return *reinterpret_cast<T *>(p + o); }
uint8_t *driverEncodeFrame(uint8_t *t) {
	auto *d = static_cast<uint8_t *>(calloc(1, 0x4b0));
	if (!d) return nullptr;
	memcpy(d, t, 0x10);
	at<uint64_t>(d, 0x8) = at<uint64_t>(t, 0x30);    // input surface
	at<uint64_t>(d, 0x478) = at<uint64_t>(t, 0x40);  // bitstream
	at<uint64_t>(d, 0x410) = at<uint64_t>(t, 0x448); // SPS
	at<uint64_t>(d, 0x418) = at<uint64_t>(t, 0x450); // PPS
	at<uint32_t>(d, 0x420) = at<uint32_t>(t, 0x458); // slice count
	at<uint64_t>(d, 0x428) = at<uint64_t>(t, 0x460);
	at<uint64_t>(d, 0x430) = at<uint64_t>(t, 0x468); // slices
	at<uint64_t>(d, 0x440) = at<uint64_t>(t, 0x480);
	at<uint32_t>(d, 0x454) = at<uint32_t>(t, 0x478); // reconstructed picture index
	at<uint64_t>(d, 0x480) = at<uint64_t>(t, 0x470);
	at<uint64_t>(d, 0x488) = at<uint64_t>(t, 0x4a0); // coded size out
	return d;
}

// AppleGVA's type-2 0x800 (scaler) job copies or scales app-owned frames
// (encoder input, VTPixelTransferSession). The driver's video processor needs
// VEBOX, which this device does not have. Let the driver run the job as a
// same-surface no-op (it still hands out the completion stamp AppleGVA waits
// on) and do the work on the CPU for 4:2:0 biplanar surfaces.
struct Rect { double x, y, width, height; };
struct ScaleJob { IOSurfaceRef source; Rect from; IOSurfaceRef target; Rect to; };
struct ScaleWork { ScaleJob *job; IOSurfaceRef target; Rect to; };

bool biplanar420(IOSurfaceRef s) {
	const OSType f = IOSurfaceGetPixelFormat(s);
	return IOSurfaceGetPlaneCount(s) == 2 && (f == '420v' || f == '420f');
}

// Bilinear resample of one plane with `channels` interleaved bytes per pixel.
void resample(const uint8_t *src, size_t srcPitch, double sx, double sy, double sw, double sh,
              uint8_t *dst, size_t dstPitch, size_t dx, size_t dy, size_t dw, size_t dh, size_t channels,
              size_t srcW, size_t srcH) {
	if (!dw || !dh || !srcW || !srcH) return;
	const double fx = sw / dw, fy = sh / dh;
	for (size_t y = 0; y < dh; y++) {
		const double v = std::max(0.0, sy + (y + 0.5) * fy - 0.5);
		const size_t y0 = std::min(static_cast<size_t>(v), srcH - 1), y1 = std::min(y0 + 1, srcH - 1);
		const uint32_t wy = static_cast<uint32_t>((v - y0) * 256);
		const uint8_t *r0 = src + y0 * srcPitch, *r1 = src + y1 * srcPitch;
		uint8_t *out = dst + (dy + y) * dstPitch + dx * channels;
		for (size_t x = 0; x < dw; x++) {
			const double u = std::max(0.0, sx + (x + 0.5) * fx - 0.5);
			const size_t x0 = std::min(static_cast<size_t>(u), srcW - 1), x1 = std::min(x0 + 1, srcW - 1);
			const uint32_t wx = static_cast<uint32_t>((u - x0) * 256);
			for (size_t c = 0; c < channels; c++) {
				const uint32_t a = r0[x0 * channels + c] * (256 - wx) + r0[x1 * channels + c] * wx;
				const uint32_t b = r1[x0 * channels + c] * (256 - wx) + r1[x1 * channels + c] * wx;
				out[x * channels + c] = static_cast<uint8_t>((a * (256 - wy) + b * wy + 32768) >> 16);
			}
		}
	}
}

void scaleSurface(IOSurfaceRef from, const Rect &a, IOSurfaceRef to, const Rect &b) {
	IOSurfaceLock(from, kIOSurfaceLockReadOnly, nullptr);
	IOSurfaceLock(to, 0, nullptr);
	for (size_t p = 0; p < 2; p++) {
		auto *src = static_cast<const uint8_t *>(IOSurfaceGetBaseAddressOfPlane(from, p));
		auto *dst = static_cast<uint8_t *>(IOSurfaceGetBaseAddressOfPlane(to, p));
		if (!src || !dst) continue;
		const size_t dstPitch = IOSurfaceGetBytesPerRowOfPlane(to, p), dstH = IOSurfaceGetHeightOfPlane(to, p);
		const double k = p ? 0.5 : 1.0; // chroma plane: half resolution, 2 bytes per sample
		const size_t dx = static_cast<size_t>(b.x * k), dy = static_cast<size_t>(b.y * k);
		const size_t dw = std::min(static_cast<size_t>(b.width * k), IOSurfaceGetWidthOfPlane(to, p) - std::min(dx, IOSurfaceGetWidthOfPlane(to, p)));
		const size_t dh = std::min(static_cast<size_t>(b.height * k), dstH - std::min(dy, dstH));
		resample(src, IOSurfaceGetBytesPerRowOfPlane(from, p), a.x * k, a.y * k, a.width * k, a.height * k,
		         dst, dstPitch, dx, dy, dw, dh, p ? 2 : 1, IOSurfaceGetWidthOfPlane(from, p), IOSurfaceGetHeightOfPlane(from, p));
		// The GPU reads this surface without snooping the CPU caches.
		for (size_t o = 0; o < dstH * dstPitch; o += 64)
			__builtin_ia32_clflush(dst + o);
	}
	__builtin_ia32_mfence();
	IOSurfaceUnlock(to, 0, nullptr);
	IOSurfaceUnlock(from, kIOSurfaceLockReadOnly, nullptr);
}

int decode(void *self, void *raw, void *out) {
	auto *frames = static_cast<Frames *>(raw);
	const uint32_t count = frames && frames->frame ? frames->count : 0;
	uint8_t **slot[count ? count : 1], *saved[count ? count : 1];
	for (uint32_t i = 0; i < count; i++) {
		if (at<uint32_t>(frames->frame[i], 0) != 1) { slot[i] = nullptr; saved[i] = nullptr; continue; }
		auto *picture = *reinterpret_cast<uint8_t **>(frames->frame[i] + picturePointer);
		slot[i] = picture ? reinterpret_cast<uint8_t **>(picture + spsPointer) : nullptr;
		saved[i] = slot[i] ? *slot[i] : nullptr;
		if (auto *copy = saved[i] ? driverSps(saved[i]) : nullptr) *slot[i] = copy;
	}
	uint8_t *encodeFrame[count ? count : 1], *encodeSaved[count ? count : 1];
	for (uint32_t i = 0; i < count; i++) {
		auto *frame = frames->frame[i];
		auto *&picture = at<uint8_t *>(frame, picturePointer);
		encodeSaved[i] = picture;
		encodeFrame[i] = at<uint32_t>(frame, 0) == 0x20 && at<uint64_t>(frame, 8) == 0x4000 && picture ?
			driverEncodeFrame(picture) : nullptr;
		if (encodeFrame[i]) picture = encodeFrame[i];
	}
	ScaleWork scales[count ? count : 1];
	for (uint32_t i = 0; i < count; i++) {
		auto *job = frames->frame[i];
		auto *scale = at<uint32_t>(job, 0) == 2 && at<uint64_t>(job, 8) == 0x800 ? at<ScaleJob *>(job, 0x20) : nullptr;
		scales[i] = {};
		if (!scale || !scale->source || !scale->target || !biplanar420(scale->source) || !biplanar420(scale->target)) continue;
		scales[i] = {scale, scale->target, scale->to};
		scale->target = scale->source;
		scale->to = scale->from;
	}
	int result = driverDecode(self, raw, out);
	for (uint32_t i = 0; i < count; i++) {
		auto &w = scales[i];
		if (!w.job) continue;
		w.job->target = w.target;
		w.job->to = w.to;
		if (!result) scaleSurface(w.job->source, w.job->from, w.target, w.to);
		else syslog(LOG_ERR, "ngva: scaler no-op failed %d (%gx%g -> %gx%g)", result, w.job->from.width, w.job->from.height,
		            w.to.width, w.to.height);
	}
	static const bool trace = access("/tmp/ngva-trace", F_OK) == 0;
	for (uint32_t i = 0; trace && i < count; i++) {
		auto *job = frames->frame[i];
		const auto *scale = at<uint32_t>(job, 0) == 2 && at<uint64_t>(job, 8) == 0x800 ? at<ScaleJob *>(job, 0x20) : nullptr;
		const auto *size = encodeFrame[i] ? at<uint32_t *>(encodeSaved[i], 0x4a0) : nullptr;
		char from[5] = "----", to[5] = "----";
		for (int k = 0; scale && k < 4; k++) {
			from[k] = static_cast<char>(IOSurfaceGetPixelFormat(scale->source) >> (24 - 8 * k));
			to[k] = static_cast<char>(IOSurfaceGetPixelFormat(scale->target) >> (24 - 8 * k));
		}
		syslog(LOG_NOTICE, "ngva: job %u/%u type %x value %llx result %d coded %u scale %s -> %s", i, count, at<uint32_t>(job, 0),
		       static_cast<unsigned long long>(at<uint64_t>(job, 8)), result, size ? *size : 0, from, to);
	}
	for (uint32_t i = 0; i < count; i++) {
		if (slot[i]) *slot[i] = saved[i];
		if (encodeFrame[i]) {
			at<uint8_t *>(frames->frame[i], picturePointer) = encodeSaved[i];
			free(encodeFrame[i]);
		}
	}
	return result;
}

// AVD_* media contexts: vtable slot holding the kernel getter (which tail
// calls the heap loader at +0x2c566 with the Gen11 kernel) -> Gen12 file.
struct Kernel { uintptr_t slot; const char *name; void *data; uint32_t size; };
Kernel kernels[] = {
	{0x7543b8, "tnr"}, {0x754470, "checksum"}, {0x7545b8, "chromadeblock"},
	{0x754a18, "framestatistics"}, {0x754b60, "noisestatistics"}, {0x7550d8, "rvra1scaler"},
};
using LoadKernel = int (*)(void *self, const void *data, uint32_t size);

template <int I> int getKernel(void *self) {
	return reinterpret_cast<LoadKernel>(driverBase + 0x2c566)(self, kernels[I].data, kernels[I].size);
}
using GetKernel = int (*)(void *);
GetKernel getKernels[] = {getKernel<0>, getKernel<1>, getKernel<2>, getKernel<3>, getKernel<4>, getKernel<5>};

// H.264 VDEnc status: the encode frame call (vtable 0x754a68 slot +0x28) waits on
// the frame fence (IOAccelVideoContextFinishFenceEvent, ignoring its result) and
// reads the MFC byte count from a 0x38-byte entry of a cacheable CPU buffer
// (*(enc+0xb60) + index*0x38, index enc+0x6bb4) that the GPU writes without
// snooping. Write the CPU's zeroing back before the frame, and drop the lines
// again between the fence wait and the read, so the read sees the GPU's count.
using EncodeFrame = long (*)(void *, void *, void *, void *, void *, void *);
using FinishFence = int (*)(void *, void *, void *, void *, void *, void *);
EncodeFrame driverEncodeFrameCall;
FinishFence driverFinishFence;
thread_local uint8_t *encodingStatus;

void flushStatus(uint8_t *entry) {
	for (uintptr_t line = reinterpret_cast<uintptr_t>(entry) & ~63UL; line < reinterpret_cast<uintptr_t>(entry) + 0x38; line += 64)
		__builtin_ia32_clflush(reinterpret_cast<void *>(line));
	__builtin_ia32_mfence();
}

long encodeFrame(void *enc, void *a1, void *a2, void *a3, void *a4, void *a5) {
	auto *base = at<uint8_t *>(static_cast<uint8_t *>(enc), 0xb60);
	encodingStatus = base ? base + at<uint32_t>(static_cast<uint8_t *>(enc), 0x6bb4) * 0x38 : nullptr;
	if (encodingStatus) flushStatus(encodingStatus);
	static bool once;
	if (!once) { once = true; syslog(LOG_NOTICE, "ngva: encoder status hook active, entry %p", encodingStatus); }
	const long result = driverEncodeFrameCall(enc, a1, a2, a3, a4, a5);
	encodingStatus = nullptr;
	return result;
}

int finishFence(void *a, void *b, void *c, void *d, void *e, void *f) {
	const int result = driverFinishFence(a, b, c, d, e, f);
	if (encodingStatus) {
		if (result) syslog(LOG_ERR, "ngva: encode fence wait returned %#x", result);
		flushStatus(encodingStatus);
		// A zero byte count after the fence: is the GPU still writing the status?
		auto &count = *reinterpret_cast<volatile uint32_t *>(encodingStatus + 0x18);
		auto &marker = *reinterpret_cast<volatile uint32_t *>(encodingStatus + 0x28);
		if (!count) {
			const uint32_t before = marker;
			int waited = 0;
			for (; !count && waited < 200; waited++) {
				usleep(1000);
				flushStatus(encodingStatus);
			}
			syslog(LOG_ERR, "ngva: zero byte count after fence (result %#x, marker %#x -> %#x): %u after %d ms", result, before,
			       marker, count, waited);
		}
	}
	return result;
}

// AVC VDEnc: convert Apple's Gen9 batch to Gen12 before submission and
// replace its 0x8d00 relocation token with a generic 0x82 record
// (vdenc_gen12.hpp). The sizing slot (+0x30: encoder, frame, &batch dwords,
// &sideband dwords) reserves room for the larger batch; the builder (+0x60:
// encoder, frame, command buffer) writes the batch at command buffer +0x70
// and the token at +0x78, advancing both as cursors (+0x20: reserved end).
// /Users/Shared/ngva-kext-vdenc leaves the conversion to the kext (V300).
constexpr uint32_t kVdencHeadroom = 256, kTokenScratch = 0x1e8 / 4, kRecordWords = 320;
using SizeFrame = long (*)(void *, void *, uint32_t *, uint32_t *, void *, void *);
using BuildBatch = long (*)(void *, void *, void *, void *, void *, void *);
SizeFrame driverSizeFrame;
BuildBatch driverBuildBatch;
bool userVdenc;

long sizeFrame(void *enc, void *frame, uint32_t *batch, uint32_t *side, void *a4, void *a5) {
	const long result = driverSizeFrame(enc, frame, batch, side, a4, a5);
	if (userVdenc && batch) {
		*batch += kVdencHeadroom;
		at<uint32_t>(static_cast<uint8_t *>(enc), 0x6c1c) = *batch;
		// Later code places the fence record by the reserved size, so the
		// 0x82 record must fill exactly this much.
		if (side) *side += kRecordWords;
	}
	return result;
}

long buildBatch(void *enc, void *frame, void *cmd, void *a3, void *a4, void *a5) {
	auto *c = static_cast<uint8_t *>(cmd);
	uint32_t *batch = c ? at<uint32_t *>(c, 0x70) : nullptr, *side = c ? at<uint32_t *>(c, 0x78) : nullptr;
	const long result = driverBuildBatch(enc, frame, cmd, a3, a4, a5);
	if (!userVdenc || !batch || !side) return result;
	const long words = at<uint32_t *>(c, 0x70) - batch, room = at<uint32_t *>(c, 0x20) - batch;
	const long sideWords = at<uint32_t *>(c, 0x78) - side, sideRoom = at<uint32_t *>(c, 0x60) - side, tokenWords = side[0] >> 16;
	// Encoders run on several threads; the conversion buffers are shared.
	static std::mutex lock;
	static NgvaVdenc::Output out;
	static int logs;
	std::lock_guard<std::mutex> guard(lock);
	if (words <= 0 || words > 1024 || (side[0] & 0xffff) != 0x8d00 || tokenWords != sideWords ||
	    !NgvaVdenc::convert(batch, static_cast<size_t>(words), side, static_cast<size_t>(tokenWords), out) ||
	    static_cast<long>(out.words) + 1 + kTokenScratch > room || 4 + static_cast<long>(out.count) * 5 > kRecordWords ||
	    tokenWords + kRecordWords > sideRoom) {
		if (logs++ < 8)
			syslog(LOG_ERR, "ngva: VDEnc batch (%ld dwords, room %ld, token %lx) left to the kext: %s", words, room,
			       static_cast<unsigned long>(side[0]), out.reason);
		return result;
	}
	const long end = static_cast<long>(out.words), entries = static_cast<long>(out.count);
	memcpy(batch, out.batch, end * 4);
	batch[end] = 0x05000000;
	memset(batch + end + 1, 0, kTokenScratch * 4);
	at<uint32_t *>(c, 0x70) = batch + end + 1 + kTokenScratch;
	// Keep the 0x8d00 token for what only it does (resource binding with
	// updateResourceMappingCacheType); its fixed-offset patches land in the
	// scratch after MI_BATCH_BUFFER_END, and its status section is off.
	uint32_t *P = side + 2;
	P[0xc18 / 4] = P[0xc1c / 4] = static_cast<uint32_t>(end + 1);
	P[0xc10 / 4] = 0;
	P[0xc2c / 4] = 0;
	// Then the real relocations as a generic 0x82 record.
	uint32_t *r = side + tokenWords;
	r[0] = 0x8200 | kRecordWords << 16;
	r[1] = side[1];
	r[2] = 0; // mode: patch the batch at startDw
	memcpy(r + 3, out.relocs, entries * sizeof(out.relocs[0]));
	memset(r + 3 + entries * 5, 0, (kRecordWords - 3 - entries * 5) * 4);
	at<uint32_t *>(c, 0x78) = r + kRecordWords;
	if (logs++ < 4)
		syslog(LOG_NOTICE, "ngva: VDEnc batch %ld -> %ld dwords, %ld relocations", words, end, entries);
	return result;
}

// H.264 decode (codechal) submit, 0x64df0 (osInterface, command buffer, ...):
// patch-vadriver.py routes it through the pointer at 0x75f000; 0x2c13f runs
// the original. The decode MFX_SURFACE_STATE is relocated by this submission's
// 0x82 record: DW3 = ((pitch - 1) << 3) + delta, where Apple's delta carries
// Gen9's surface format and leaves the pitch one short (0x080ffffb: 1280 ->
// 0x081027f3); Gen12 needs PLANAR_420_8 (0x48000000) and DW5 (Cr Y offset)
// equal to DW4, which no entry writes. Edit the record before submission.
thread_local uint32_t *lastSlice; // newest HEVC slice state (hcpSlice below)
using Submit = long (*)(void *, void *, void *, void *, void *, void *);
Submit driverSubmit;

// A submission's batch (command buffer +0x148..+0x150) and its generic 0x82
// relocation record, which ends at the context's sideband cursor (os: *os +8 +
// index (os +8) * 0x15c0, sideband +0x20). Same layout in both VA drivers.
struct Submission { const uint32_t *batch, *batchEnd; uint32_t words; uint32_t *record; uint32_t length; };
bool findSubmission(void *os, void *cmd, Submission &s) {
	auto *o = static_cast<uint8_t *>(os);
	auto *ctx = o ? at<uint8_t *>(o, 0) : nullptr;
	if (!ctx || !cmd) return false;
	ctx += 8 + at<uint32_t>(o, 8) * 0x15c0;
	auto **sb = at<uint32_t **>(ctx, 0x20);
	s.batch = at<uint32_t *>(static_cast<uint8_t *>(cmd), 0x148);
	s.batchEnd = at<uint32_t *>(static_cast<uint8_t *>(cmd), 0x150);
	s.record = nullptr;
	if (sb && sb[0] && s.batch && s.batchEnd > s.batch)
		for (uint32_t *w = sb[0] - 1, n = 0; n < 4096; n++, w--)
			if ((*w & 0xffff) == 0x8200 && w + (*w >> 16) == sb[0]) { s.record = w; break; }
	s.words = s.batch && s.batchEnd > s.batch ? static_cast<uint32_t>(s.batchEnd - s.batch) : 0;
	s.length = s.record ? s.record[0] >> 16 : 0;
	return true;
}

void logWords(const char *what, int n, const uint32_t *w, uint32_t count) {
	for (uint32_t i = 0; i < count; i += 8)
		syslog(LOG_NOTICE, "ngva: %s %d +%03x: %08x %08x %08x %08x %08x %08x %08x %08x", what, n, i * 4, w[i], i + 1 < count ? w[i + 1] : 0,
		       i + 2 < count ? w[i + 2] : 0, i + 3 < count ? w[i + 3] : 0, i + 4 < count ? w[i + 4] : 0, i + 5 < count ? w[i + 5] : 0,
		       i + 6 < count ? w[i + 6] : 0, i + 7 < count ? w[i + 7] : 0);
}

long submitDecode(void *os, void *cmd, void *a2, void *a3, void *a4, void *a5) {
	Submission sub {};
	if (!userVdenc || !findSubmission(os, cmd, sub)) return driverSubmit(os, cmd, a2, a3, a4, a5);
	const uint32_t *batch = sub.batch, *batchEnd = sub.batchEnd;
	uint32_t *record = sub.record;
	const uint32_t words = sub.words, length = sub.length;
	// HEVC batches (HCP_PIPE_MODE_SELECT, or HUC_PIPE_MODE_SELECT for HuC
	// slice parsing). The kext runs only those marked here (MI_NOOP with ID
	// 0x4e47 in the prologue's NOOP at dword 6) and ends the rest at their
	// first dword. /Users/Shared/ngva-hevc-dump logs the batch and its
	// relocation record and leaves it unmarked; only 4:2:0 decode (PLANAR_420_8
	// and P010 surfaces) runs.
	if (words && std::find_if(batch, batchEnd, [](uint32_t w) { return (w & 0xffff0000) == 0x73800000 || (w & 0xffff0000) == 0x75800000; }) != batchEnd) {
		static const bool hevcDump = access("/Users/Shared/ngva-hevc-dump", F_OK) == 0;
		static int dumps, logs;
		if (hevcDump && dumps++ < 2) {
			logWords("hevc batch", dumps, batch, words);
			if (record) logWords("hevc record", dumps, record, length);
		}
		// The slice commands sit in a second-level batch at a PPGTT address,
		// but the driver's MI_BATCH_BUFFER_START leaves the address space bit
		// (8) at GGTT; the relocation entry for its address finds it.
		uint32_t surfaces = 0, planar420 = 0, starts = 0;
		for (uint32_t k = 3; k + 5 <= length && (record[k] & 1); k += 5) {
			const uint32_t type = record[k] >> 1 & 7, at = record[k] >> 5 & 0xfffff;
			if (type == 2 && at >= 2 && at < words && batch[at - 2] == 0x73810003) {
				surfaces++;
				const uint32_t format = record[k + 2] & 0xf8000000;
				planar420 += format == 0x20000000 || format == 0x68000000; // PLANAR_420_8, P010
			}
			if (type == 0 && at >= 1 && at < words && batch[at - 1] == 0x18c00001) {
				const_cast<uint32_t *>(batch)[at - 1] |= 0x100;
				starts++;
			}
		}
		const bool lastSet = lastSlice != nullptr;
		if (lastSlice) {
			lastSlice[2] &= ~0x07ff03ffU;
			lastSlice[3] |= 4;
			lastSlice = nullptr;
		}
		const bool run = !hevcDump && lastSet && surfaces && surfaces == planar420 && words > 6 && !batch[6];
		if (run) const_cast<uint32_t *>(batch)[6] = 0x00404e47;
		if (logs++ < 4)
			syslog(LOG_NOTICE, "ngva: HEVC batch %u dwords, %u/%u 4:2:0 surfaces, %u PPGTT batch starts, last slice %s: %s", words, planar420,
			       surfaces, starts, lastSet ? "set" : "missing", run ? "run" : "dropped");
		return driverSubmit(os, cmd, a2, a3, a4, a5);
	}
	if (!record) return driverSubmit(os, cmd, a2, a3, a4, a5);
	uint32_t *chroma = nullptr, fixed = 0, k = 3;
	bool hasCr = false;
	for (; k + 5 < length && (record[k] & 1); k += 5) {
		const uint32_t type = record[k] >> 1 & 7, at = record[k] >> 5 & 0xfffff;
		if (type == 1 && at >= 3 && at < words && batch[at - 3] == 0x70010004 && (record[k + 2] & 0xf8000000) == 0x08000000) {
			record[k + 2] = 0x48000000 | (record[k + 2] & 7);
			fixed++;
		}
		if (type == 2 && at >= 4 && at + 1 < words && batch[at - 4] == 0x70010004) chroma = record + k;
		if (type == 2 && at >= 5 && at < words && batch[at - 5] == 0x70010004) hasCr = true;
	}
	if (chroma && !hasCr && k + 6 <= length) {
		memcpy(record + k, chroma, 5 * 4);
		record[k] += 1 << 5; // next dword: DW5
		record[k + 5] = 0;
	}
	static int logs;
	if (fixed && logs++ < 2)
		syslog(LOG_NOTICE, "ngva: decode surface relocations: %u DW3 to Gen12, DW5 %s", fixed, chroma && !hasCr ? "added" : "kept");
	return driverSubmit(os, cmd, a2, a3, a4, a5);
}

// HEVC decode surfaces: the driver's Gen12 AddHcpDecodeSurfaceStateCmd
// (0x192d20, two vtables) builds HCP_SURFACE_STATE on its stack and adds the
// DW1 (pitch) and DW2 (Cb row offset) relocations at the command buffer's
// write position, but never writes the command: the relocations then land on
// the next command, HCP_PIPE_BUF_ADDR_STATE's decoded picture address. Write
// the command there; the kernel fills DW1 and DW2 (surface ID and format come
// with the relocations), DW4 (compression) stays 0.
using HcpSurface = long (*)(void *, uint8_t *, uint8_t *);
using AddCommand = int (*)(uint8_t *, const void *, uint32_t);
HcpSurface driverHcpSurface;
long hcpSurface(void *self, uint8_t *buffer, uint8_t *params) {
	const long result = driverHcpSurface(self, buffer, params);
	// Without relocations (osInterface +0x70 bit 0 clear) DW1/DW2 would need
	// the driver's own values; that mode is not used with AppleGVA.
	auto *os = at<uint8_t *>(static_cast<uint8_t *>(self), 8);
	if (result || !buffer || !os || !(os[0x70] & 1)) return result;
	const uint32_t command[5] = {0x73810003, 0, 0, 0xffff, 0}; // DW3: default alpha, as media-driver
	const int added = reinterpret_cast<AddCommand>(driverBase + 0x41300)(buffer, command, sizeof(command));
	static int logs;
	if (logs++ < 2) syslog(LOG_NOTICE, "ngva: HEVC surface state %u written at dword %u (%d)", params ? params[0x12] & 0xf : 0,
	                       at<uint32_t>(buffer, 0x158) / 4 - 5, added);
	return added;
}

// HEVC decode slices (Gen12 AddHcpDecodeSliceStateCmd 0x198f90, two
// vtables): the driver never sets bLastSlice, and derives the next slice's
// start from a CTU count it leaves 0. HCP then waits for a slice that never
// comes and the frame's closing MI_FLUSH_DW never completes. Each slice gives
// its start (DW1) to the previous slice's next-slice start (DW2); submit sets
// LastSliceOfPic (DW3 bit 2) on the picture's final slice.
using HcpSlice = long (*)(void *, uint8_t *, uint8_t *);
HcpSlice driverHcpSlice;
long hcpSlice(void *self, uint8_t *buffer, uint8_t *state) {
	auto *slice = buffer ? at<uint32_t *>(buffer, 0x150) : nullptr;
	const long result = driverHcpSlice(self, buffer, state);
	if (result || !slice || slice[0] != 0x7394000b) return result;
	// A picture's first slice starts at CTB 0 (DW1 0).
	if (lastSlice && slice[1]) lastSlice[2] = (lastSlice[2] & ~0x07ff03ffU) | (slice[1] & 0x03ff03ffU);
	lastSlice = slice;
	return result;
}

template <typename T> bool replacePointer(uintptr_t offset, T replacement, T &original, uintptr_t base = 0) {
	const long page = getpagesize();
	auto *slot = reinterpret_cast<T *>((base ? base : driverBase) + offset);
	auto start = reinterpret_cast<uintptr_t>(slot) & ~(page - 1);
	if (mprotect(reinterpret_cast<void *>(start), page, PROT_READ | PROT_WRITE)) return false;
	original = *slot;
	*slot = replacement;
	return true;
}

void replaceKernels(const char *bundle) {
	const long page = getpagesize();
	for (size_t i = 0; i < sizeof(kernels) / sizeof(*kernels); i++) {
		char path[1024];
		snprintf(path, sizeof(path), "%s/Contents/Resources/%s.gen12", bundle, kernels[i].name);
		FILE *f = fopen(path, "rb");
		if (!f) continue;
		fseek(f, 0, SEEK_END);
		long size = ftell(f);
		fseek(f, 0, SEEK_SET);
		void *data = size > 0 ? malloc(size) : nullptr;
		if (!data || fread(data, 1, size, f) != static_cast<size_t>(size)) {
			fclose(f);
			free(data);
			continue;
		}
		fclose(f);
		kernels[i].data = data;
		kernels[i].size = static_cast<uint32_t>(size);
		auto *slot = reinterpret_cast<GetKernel *>(driverBase + kernels[i].slot);
		auto start = reinterpret_cast<uintptr_t>(slot) & ~(page - 1);
		if (mprotect(reinterpret_cast<void *>(start), page, PROT_READ | PROT_WRITE)) continue;
		*slot = getKernels[i];
		mprotect(reinterpret_cast<void *>(start), page, PROT_READ);
		syslog(LOG_NOTICE, "ngva: Gen12 %s kernel (%ld bytes)", kernels[i].name, size);
	}
}

// The description, SPS and encode job layouts converted here are AppleGVA's.
// Minor macOS updates rebuild AppleGVA from the same source version, so check
// that rather than the image itself; another version makes VideoToolbox fall
// back to software instead of handing the driver mismatched structures.
constexpr uint64_t testedAppleGva = 113ULL << 40 | 56ULL << 30; // 113.56.0.0.0

bool appleGvaTested() {
	for (uint32_t i = 0; i < _dyld_image_count(); i++) {
		const char *name = _dyld_get_image_name(i);
		const char *suffix = "/AppleGVA.framework/Versions/A/AppleGVA";
		if (!name || strlen(name) < strlen(suffix) || strcmp(name + strlen(name) - strlen(suffix), suffix)) continue;
		auto *header = reinterpret_cast<const mach_header_64 *>(_dyld_get_image_header(i));
		auto *command = reinterpret_cast<const load_command *>(header + 1);
		for (uint32_t c = 0; c < header->ncmds; c++) {
			if (command->cmd == LC_SOURCE_VERSION) {
				uint64_t version = reinterpret_cast<const source_version_command *>(command)->version;
				if (version == testedAppleGva) return true;
				syslog(LOG_ERR, "ngva: AppleGVA %llu.%llu not tested, hardware video off", version >> 40, version >> 30 & 0x3ff);
				return false;
			}
			command = reinterpret_cast<const load_command *>(reinterpret_cast<const uint8_t *>(command) + command->cmdsize);
		}
	}
	syslog(LOG_ERR, "ngva: AppleGVA version not found, hardware video off");
	return false;
}

// VP9 decode: Tahoe's ICL VA driver (shipped with AppleGVA, same structures)
// serves the sessions that ask for VP9 (request type 1, value 0x1000); the
// TGL driver has no VP9. Its Gen11 HCP commands become Gen12 here:
// HCP_PIPE_MODE_SELECT gains DW6 and HCP_SURFACE_STATE DW4 (both 0), appended
// after the driver writes them, which keeps their relocation offsets. Offsets
// are of the 25G332 ICL driver; another build leaves VP9 off.
constexpr const char *iclPath = "/System/Library/Extensions/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/AppleIntelICLGraphicsVADriver";
constexpr uintptr_t iclHcpVtable = 0x792250, iclAddCommand = 0x3dbb0, iclStructMethodSlot = 0x78a0c0;
Create iclCreate;
Destroy iclDestroy;
uintptr_t iclBase;
int iclAdd(uint8_t *buffer, const void *words, uint32_t bytes) {
	return reinterpret_cast<int (*)(uint8_t *, const void *, uint32_t)>(iclBase + iclAddCommand)(buffer, words, bytes);
}
using HcpCommand = long (*)(void *, uint8_t *, void *);
HcpCommand iclPipeMode, iclSurface, iclBsd, iclVp9Bsd;
Submit iclSubmitOriginal;
using Any = long (*)(void *, void *, void *, void *, void *, void *);
std::map<void *, void *> twins; // TGL accelerator table -> ICL twin
// Sessions (fn0 creates one, fn1 destroys it, the rest take it as self) get an
// ICL twin too; after a VP9 fn2 the session's calls go to the twin.
// Only fn2 tells a VP9 session (AppleGVA has already allocated its bitstream
// buffer pool on the TGL session with fn5: input {u32 kind, ..., u32 count
// +0x14, address array +0x18}). fn2 allocates the same pool on the twin, and
// each VP9 submission copies its bitstream from the TGL buffer AppleGVA wrote.
struct Session {
	void *icl;
	bool vp9;
	uint8_t pool[0x20];
	void *tglBuffer, *iclBuffer;
};
thread_local Session *vp9Session; // the routed session whose ICL call is running
std::map<void *, Session> sessions;
Any tglFn[16], iclFn[16];

// The ICL (Gen11) VP9 batch on Gen12: the prologue becomes the TGL driver's
// (MI_FORCE_WAKEUP, the status MI_STORE_DATA_IMM moved up to free dword 6 for
// the kext's marker), MI stores get the GGTT bit, PIPE_BUF_ADDR loses the
// Gen11 attribute bit 10 and takes the Gen12 deblocking row-store offset.
// Returns whether the batch is safe to run.
void flushCache(const void *p, size_t bytes) {
	for (auto a = reinterpret_cast<uintptr_t>(p) & ~uintptr_t(63); a < reinterpret_cast<uintptr_t>(p) + bytes; a += 64)
		__builtin_ia32_clflush(reinterpret_cast<const void *>(a));
	__builtin_ia32_mfence();
}

bool iclToGen12(const Submission &sub) {
	auto *b = const_cast<uint32_t *>(sub.batch);
	const uint32_t n = sub.words;
	if (n < 16 || b[0] != 0x13000082 || b[4] || b[5] != 0x10000002 || !sub.record) return false;
	uint32_t *status = nullptr;
	for (uint32_t i = 3; i + 5 <= sub.length && sub.record[i] & 1; i += 5)
		if ((sub.record[i] >> 5 & 0xfffff) == 6) status = &sub.record[i];
	if (!status) return false;
	*status = (*status & ~(0xfffffu << 5)) | 3 << 5;
	const uint32_t prologue[10] = {0x0e800000, 0x03000100, 0x10400002, b[6], b[7], b[8], 0, 0, 0, 0};
	memcpy(b, prologue, sizeof(prologue));
	bool pipeMode = false, control = false, surface = false, pba = false, bitstream = false;
	uint32_t width = 0;
	for (uint32_t i = 10; i < n;) {
		const uint32_t h = b[i];
		uint32_t length = 1;
		if (h >> 29 == 0) {
			const uint32_t op = h >> 23 & 0x3f;
			if (op == 0x0a) break;
			if (op == 0x20 || op == 0x24) b[i] = h | 0x00400000;
			if (op == 0x20) length = (h & 0x3ff) + 2;
			else if (op == 0x24 || op == 0x31 || op == 0x1d) length = (h & 0xff) + 2;
			else if (op == 0x26) length = (h & 0x3f) + 2;
		} else if (h >> 29 == 3) {
			length = h >> 16 == 0x6800 ? 1 : (h & 0xfff) + 2;
			if (h == 0x738a0001) control = true;
			if (h == 0x73800005) pipeMode = true;
			if (h == 0x73810003 && i + 2 < n) surface = (b[i + 2] & 0xf8000000) == 0x20000000;
			if (h == 0x73b0000a && i + 1 < n) width = (b[i + 1] & 0x3fff) + 1;
			if (h == 0x73a00001 && i + 2 < n && vp9Session && vp9Session->tglBuffer && vp9Session->iclBuffer &&
				static_cast<uint64_t>(b[i + 1]) + b[i + 2] <= at<uint32_t>(vp9Session->pool, 8)) {
				// The twin's buffer is CPU-cached; the video engine reads memory.
				memcpy(vp9Session->iclBuffer, vp9Session->tglBuffer, static_cast<size_t>(b[i + 1]) + b[i + 2]);
				flushCache(vp9Session->iclBuffer, static_cast<size_t>(b[i + 1]) + b[i + 2]);
				bitstream = true;
				static int copies;
				if (copies++ < 2) {
					logWords("vp9 tgl bitstream", copies, static_cast<uint32_t *>(vp9Session->tglBuffer), 8);
					logWords("vp9 icl bitstream", copies, static_cast<uint32_t *>(vp9Session->iclBuffer), 8);
				}
			}
			if (h == 0x73820077 && i + 121 <= n) {
				// Memory attributes (MOCS) as the Linux media driver sets them on
				// this MOCS table; the row-store cache select bit stays.
				static const uint8_t attributes[] = {3, 6, 9, 12, 15, 18, 21, 24, 27, 30, 33, 36, 53, 56, 59, 62, 65, 82, 85, 88, 91,
					94, 97, 100, 103, 106, 109, 112, 115, 118};
				for (const uint32_t j : attributes)
					b[i + j] = (b[i + j] & 0x1000) | (j == 3 || j == 18 || j == 21 || j == 33 || j == 82 ? 6 : 0xa);
				if (b[i + 6] & 0x1000) b[i + 4] = 0x6000;
				pba = true;
			}
			if (h == 0x7383001b && i + 4 < n) b[i + 3] = 6;
		} else return false;
		i += length;
	}
	return pipeMode && control && surface && pba && bitstream && width && width <= 2048;
}

long iclSubmit(void *os, void *cmd, void *a2, void *a3, void *a4, void *a5) {
	Submission sub {};
	static const bool dump = access("/Users/Shared/ngva-vp9-dump", F_OK) == 0;
	static const bool run = access("/Users/Shared/ngva-vp9-run", F_OK) == 0;
	static int dumps, logs;
	if (findSubmission(os, cmd, sub) && sub.words) {
		const bool safe = iclToGen12(sub);
		if (dump && sub.words > 16 && dumps++ < 3) {
			logWords("vp9 batch", dumps, sub.batch, sub.words);
			if (sub.record) logWords("vp9 record", dumps, sub.record, sub.length);
		}
		// Unmarked HCP batches end at their first dword in the kext.
		const bool mark = run && !dump && safe;
		if (mark) const_cast<uint32_t *>(sub.batch)[6] = 0x00404e47;
		if (logs++ < 4) syslog(LOG_NOTICE, "ngva: VP9 batch %u dwords, %s: %s", sub.words, safe ? "converted" : "not converted", mark ? "run" : "dropped");
	}
	const long result = iclSubmitOriginal(os, cmd, a2, a3, a4, a5);
	static int after;
	if (sub.words > 16 && after++ < 2) logWords("vp9 submitted", after, sub.batch, std::min(sub.words, 0xc0u));
	return result;
}

// Append the Gen12 dword to the command the driver just wrote (DW0 length +1),
// before the MFX_WAIT that may follow it.
long appendGen12Dword(void *self, uint8_t *buffer, uint32_t header) {
	if (!buffer) return 0;
	auto *os = at<uint8_t *>(static_cast<uint8_t *>(self), 8);
	if (os && at<Submit>(os, 0x2d0) != iclSubmit) {
		if (!iclSubmitOriginal) iclSubmitOriginal = at<Submit>(os, 0x2d0);
		if (at<Submit>(os, 0x2d0) == iclSubmitOriginal) at<Submit>(os, 0x2d0) = iclSubmit;
	}
	const uint32_t length = (header & 0xfff) + 2;
	auto *end = at<uint32_t *>(buffer, 0x150);
	const bool wait = end[-1] == 0x68000100;
	uint32_t *command = end - wait - length;
	if (command[0] != header) return 0;
	const uint32_t trailer = wait ? 0x68000100 : 0;
	const int result = iclAdd(buffer, &trailer, 4);
	if (result) return result;
	command[0] = header + 1;
	command[length] = 0;
	return 0;
}
// Gen12 wraps the HCP pipeline in VD_CONTROL_STATE: initialization before
// PIPE_MODE_SELECT, memory implicit flush after the BSD object.
long hcpPipeModeGen12(void *self, uint8_t *buffer, void *params) {
	const uint32_t init[3] = {0x738a0001, 1, 0};
	if (buffer && iclAdd(buffer, init, sizeof(init))) return 1;
	const long result = iclPipeMode(self, buffer, params);
	return result ? result : appendGen12Dword(self, buffer, 0x73800004);
}
long bsdFlush(long result, uint8_t *buffer) {
	const uint32_t flush[3] = {0x738a0001, 0, 4};
	return result || !buffer ? result : iclAdd(buffer, flush, sizeof(flush));
}
long hcpBsdGen12(void *self, uint8_t *buffer, void *params) { return bsdFlush(iclBsd(self, buffer, params), buffer); }
long hcpVp9BsdGen12(void *self, uint8_t *buffer, void *params) { return bsdFlush(iclVp9Bsd(self, buffer, params), buffer); }
long hcpSurfaceGen12(void *self, uint8_t *buffer, void *params) {
	const long result = iclSurface(self, buffer, params);
	return result ? result : appendGen12Dword(self, buffer, 0x73810002);
}

// The ICL driver accepts only ICL device ids from its hw-caps query
// (IOConnectCallStructMethod selector 0xb, id at dword 2); its import slot
// for that call goes through here.
using StructMethod = int (*)(unsigned, uint32_t, const void *, size_t, void *, size_t *);
StructMethod iclStructMethod;
int iclHwCaps(unsigned connection, uint32_t selector, const void *in, size_t inSize, void *out, size_t *outSize) {
	const int result = iclStructMethod(connection, selector, in, inSize, out, outSize);
	if (!result && selector == 0xb && out && outSize && *outSize >= 12 && static_cast<uint32_t *>(out)[2] == 0x9a408086)
		static_cast<uint32_t *>(out)[2] = 0x8a528086;
	return result;
}

bool loadIcl() {
	void *icl = dlopen(iclPath, RTLD_NOW | RTLD_LOCAL);
	iclCreate = icl ? reinterpret_cast<Create>(dlsym(icl, "AVD_CreateAVDAccelerator")) : nullptr;
	iclDestroy = icl ? reinterpret_cast<Destroy>(dlsym(icl, "AVD_DestroyAVDAccelerator")) : nullptr;
	Dl_info info {};
	if (!iclCreate || !iclDestroy || !dladdr(reinterpret_cast<void *>(iclCreate), &info)) return false;
	iclBase = reinterpret_cast<uintptr_t>(info.dli_fbase);
	auto *vtable = reinterpret_cast<uintptr_t *>(iclBase + iclHcpVtable);
	if (vtable[0x78 / 8] != iclBase + 0x19bd50 || vtable[0x80 / 8] != iclBase + 0x19c050 || vtable[0xe0 / 8] != iclBase + 0x1a1210 ||
		vtable[0xe8 / 8] != iclBase + 0x169fc0 ||
		*reinterpret_cast<void **>(iclBase + iclStructMethodSlot) != dlsym(RTLD_DEFAULT, "IOConnectCallStructMethod")) {
		syslog(LOG_ERR, "ngva: ICL VA driver build not tested, VP9 off");
		return false;
	}
	return replacePointer<StructMethod>(iclStructMethodSlot, iclHwCaps, iclStructMethod, iclBase) &&
		replacePointer<HcpCommand>(iclHcpVtable + 0x78, hcpPipeModeGen12, iclPipeMode, iclBase) &&
		replacePointer<HcpCommand>(iclHcpVtable + 0xe0, hcpBsdGen12, iclBsd, iclBase) &&
		replacePointer<HcpCommand>(iclHcpVtable + 0xe8, hcpVp9BsdGen12, iclVp9Bsd, iclBase) &&
		replacePointer<HcpCommand>(iclHcpVtable + 0x80, hcpSurfaceGen12, iclSurface, iclBase);
}

template <int I> long forward(void *self, void *a, void *b, void *c, void *d, void *e) {
	static const bool trace = access("/Users/Shared/ngva-vp9-dump", F_OK) == 0;
	void *icl = nullptr;
	bool twin = false;
	{
		std::lock_guard<std::mutex> guard(lock);
		auto it = sessions.find(self);
		twin = it != sessions.end();
		if (twin && it->second.vp9) {
			icl = it->second.icl;
			vp9Session = &it->second;
		}
	}
	const long result = icl ? iclFn[I](icl, a, b, c, d, e) : tglFn[I](self, a, b, c, d, e);
	vp9Session = nullptr;
	if (trace && twin) syslog(LOG_NOTICE, "ngva: %s fn%d(%p, %p, %p) = %#lx", icl ? "ICL" : "TGL", I, a, b, c, result);
	else if (icl && result) syslog(LOG_ERR, "ngva: ICL fn%d returned %#lx", I, result);
	return result;
}
template <int... I> void forwardAll(uint8_t *table, std::integer_sequence<int, I...>) {
	((*reinterpret_cast<void **>(table + 8 + I * 8) = reinterpret_cast<void *>(forward<I>)), ...);
}

// fn0's input is {u32 index, accelerator table}; the session keeps the table
// and reaches the accelerator description through it.
long createSession(void *out, void *input, void *b, void *c, void *d, void *e) {
	const long result = tglFn[0](out, input, b, c, d, e);
	if (result || !*static_cast<void **>(out) || !input) return result;
	struct Input { uint32_t index; void *accelerator; } twinInput = *static_cast<Input *>(input);
	{
		std::lock_guard<std::mutex> guard(lock);
		auto it = twins.find(twinInput.accelerator);
		if (it == twins.end()) return result;
		twinInput.accelerator = it->second;
	}
	void *icl = nullptr;
	const long iclResult = iclFn[0](&icl, &twinInput, b, c, d, e);
	if (iclResult || !icl) {
		syslog(LOG_ERR, "ngva: ICL session failed %ld", iclResult);
		return result;
	}
	std::lock_guard<std::mutex> guard(lock);
	sessions[*static_cast<void **>(out)] = {icl};
	return result;
}

long allocatePool(void *self, void *input, void *b, void *c, void *d, void *e) {
	const long result = tglFn[5](self, input, b, c, d, e);
	auto *in = static_cast<uint8_t *>(input);
	if (result || !in || at<uint32_t>(in, 0) || at<uint32_t>(in, 0x14) != 1 || !at<void **>(in, 0x18)) return result;
	std::lock_guard<std::mutex> guard(lock);
	auto it = sessions.find(self);
	if (it != sessions.end()) {
		memcpy(it->second.pool, in, sizeof(it->second.pool));
		it->second.tglBuffer = at<void **>(in, 0x18)[0];
	}
	return result;
}

long destroySession(void *self, void *a, void *b, void *c, void *d, void *e) {
	Session session {};
	{
		std::lock_guard<std::mutex> guard(lock);
		auto it = sessions.find(self);
		if (it != sessions.end()) {
			session = it->second;
			sessions.erase(it);
		}
	}
	long result = tglFn[1](self, a, b, c, d, e);
	if (session.icl) {
		const long iclResult = iclFn[1](session.icl, a, b, c, d, e);
		if (session.vp9) result = iclResult;
	}
	return result;
}

// fn2 creates the session's accelerators from AppleGVA's requests ({count,
// list}; request: type +0, value +8, next +0x38).
long createAccelerators(void *self, void *requests, void *b, void *c, void *d, void *e) {
	bool vp9 = false;
	auto *list = requests ? *static_cast<uint8_t **>(requests) : nullptr;
	for (uint32_t i = 0; list && i < at<uint32_t>(list, 0); i++)
		for (auto *r = at<uint8_t **>(list, 8)[i]; r; r = at<uint8_t *>(r, 0x38))
			vp9 |= at<uint32_t>(r, 0) == 1 && at<uint64_t>(r, 8) == 0x1000;
	Session *session = nullptr;
	{
		std::lock_guard<std::mutex> guard(lock);
		auto it = sessions.find(self);
		if (vp9 && it != sessions.end() && it->second.tglBuffer) session = &it->second;
	}
	if (!session) return tglFn[2](self, requests, b, c, d, e);
	uint8_t pool[0x20];
	void *buffer = nullptr;
	memcpy(pool, session->pool, sizeof(pool));
	at<void **>(pool, 0x18) = &buffer;
	const long result = iclFn[5](session->icl, pool, nullptr, nullptr, nullptr, nullptr);
	if (result || !buffer) {
		syslog(LOG_ERR, "ngva: ICL bitstream pool failed %#lx", result);
		return result ? result : 1;
	}
	{
		std::lock_guard<std::mutex> guard(lock);
		session->iclBuffer = buffer;
		session->vp9 = true;
	}
	syslog(LOG_NOTICE, "ngva: VP9 session on the ICL VA driver");
	return iclFn[2](session->icl, requests, b, c, d, e);
}

bool load() {
	static std::once_flag once;
	std::call_once(once, [] {
		if (!appleGvaTested()) return;
		// The patched driver sits next to this image.
		Dl_info self {};
		char path[1024];
		if (!dladdr(reinterpret_cast<void *>(&AVD_CreateAVDAccelerator), &self) || !self.dli_fname) return;
		strlcpy(path, self.dli_fname, sizeof(path));
		if (auto *slash = strrchr(path, '/')) slash[1] = 0;
		strlcat(path, "AppleIntelTGLGraphicsVADriver", sizeof(path));
		void *driver = dlopen(path, RTLD_NOW | RTLD_LOCAL);
		if (!driver) {
			syslog(LOG_ERR, "ngva: %s", dlerror());
			return;
		}
		driverCreate = reinterpret_cast<Create>(dlsym(driver, "AVD_CreateAVDAccelerator"));
		driverDestroy = reinterpret_cast<Destroy>(dlsym(driver, "AVD_DestroyAVDAccelerator"));
		Dl_info info {};
		if (driverCreate && dladdr(reinterpret_cast<void *>(driverCreate), &info))
			driverBase = reinterpret_cast<uintptr_t>(info.dli_fbase);
		if (driverBase) {
			if (auto *contents = strstr(path, "/Contents/MacOS/")) *contents = 0;
			replaceKernels(path);
			userVdenc = access("/Users/Shared/ngva-kext-vdenc", F_OK) != 0;
			driverSubmit = reinterpret_cast<Submit>(driverBase + 0x2c13f);
			Submit unused = nullptr;
			if (!replacePointer<Submit>(0x75f000, submitDecode, unused))
				syslog(LOG_ERR, "ngva: cannot hook the codechal submit");
			HcpSurface second = nullptr;
			if (!replacePointer<HcpSurface>(0x759898, hcpSurface, driverHcpSurface) ||
			    !replacePointer<HcpSurface>(0x75a3e0, hcpSurface, second))
				syslog(LOG_ERR, "ngva: cannot hook the HEVC surface state");
			HcpSlice secondSlice = nullptr;
			if (!replacePointer<HcpSlice>(0x7598f0, hcpSlice, driverHcpSlice) ||
			    !replacePointer<HcpSlice>(0x75a438, hcpSlice, secondSlice))
				syslog(LOG_ERR, "ngva: cannot hook the HEVC slice state");
			if (!replacePointer<EncodeFrame>(0x754a90, encodeFrame, driverEncodeFrameCall) ||
			    !replacePointer<FinishFence>(0x7600d0, finishFence, driverFinishFence) ||
			    !replacePointer<BuildBatch>(0x754ac8, buildBatch, driverBuildBatch) ||
			    !replacePointer<SizeFrame>(0x754a98, sizeFrame, driverSizeFrame))
				syslog(LOG_ERR, "ngva: cannot hook the encoder status read");
		}
	});
	return driverCreate && driverDestroy;
}
#ifdef NGVA_TRACE
using Any = long (*)(long, long, long, long, long, long);
Any traced[16];
template <int I> long trace(long a, long b, long c, long d, long e, long f) {
	long r = traced[I](a, b, c, d, e, f);
	syslog(LOG_NOTICE, "ngva: fn%d(%lx, %lx) -> %ld", I, a, b, r);
	return r;
}
template <int... I> void traceAll(uint8_t *table, std::integer_sequence<int, I...>) {
	((traced[I] = *reinterpret_cast<Any *>(table + 8 + I * 8),
	  *reinterpret_cast<Any *>(table + 8 + I * 8) = trace<I>), ...);
}
#endif
} // namespace

extern "C" __attribute__((visibility("default")))
int AVD_CreateAVDAccelerator(void **accelerator, const void *input, void *metadata) {
	if (!load()) return 7;
	int result = driverCreate(accelerator, input, metadata);
	if (result || !*accelerator) return result;
	auto *table = static_cast<uint8_t *>(*accelerator);
	auto *context = *reinterpret_cast<uint8_t **>(table);
	auto *original = *reinterpret_cast<Description **>(context + 0x30);
	// VP9: an ICL twin and its VP9 decode entry.
	void *twin = nullptr;
	const uint8_t *vp9Entry = nullptr;
	static const bool icl = loadIcl();
	uint8_t iclMetadata[0x40] {};
	if (icl && !iclCreate(&twin, input, iclMetadata) && twin) {
		auto *d = *reinterpret_cast<Description **>(iclMetadata + 8);
		for (uint32_t i = 0; d && i < d->count && !vp9Entry; i++)
			for (auto *e = d->slot[i]; e && !vp9Entry; e = *reinterpret_cast<uint8_t **>(e + 0x430))
				if (at<uint32_t>(e, 0) == 1 && at<uint64_t>(e, 8) == 0x1000) vp9Entry = e;
		if (!vp9Entry) {
			iclDestroy(twin);
			twin = nullptr;
		}
	}
	auto *description = convert(original, vp9Entry);
	if (!description) {
		syslog(LOG_ERR, "ngva: unexpected accelerator description");
		driverDestroy(*accelerator);
		*accelerator = nullptr;
		return 7;
	}
	{
		std::lock_guard<std::mutex> guard(lock);
		descriptions[*accelerator] = description;
	}
	auto *slot = reinterpret_cast<GetParameter *>(table + 0x80);
	if (!driverGetParameter) driverGetParameter = *slot;
	*slot = getParameter;
	auto *set = reinterpret_cast<SetParameters *>(table + 0x68);
	if (!driverSetParameters) driverSetParameters = *set;
	*set = setParameters;
	// The H.264 frame decode entry (driver build d265f203...).
	for (int i = 0; i < 16; i++) {
		auto *entry = reinterpret_cast<Decode *>(table + 8 + i * 8);
		if (driverBase && reinterpret_cast<uintptr_t>(*entry) == driverBase + 0x159a4) {
			driverDecode = *entry;
			*entry = decode;
		}
	}
#ifdef NGVA_TRACE
	traceAll(table, std::make_integer_sequence<int, 16>{});
#endif
	if (twin) {
		static std::once_flag once;
		std::call_once(once, [&] {
			memcpy(tglFn, table + 8, sizeof(tglFn));
			memcpy(iclFn, static_cast<uint8_t *>(twin) + 8, sizeof(iclFn));
		});
		forwardAll(table, std::make_integer_sequence<int, 16>{});
		*reinterpret_cast<void **>(table + 8) = reinterpret_cast<void *>(createSession);
		*reinterpret_cast<void **>(table + 0x10) = reinterpret_cast<void *>(destroySession);
		*reinterpret_cast<void **>(table + 0x18) = reinterpret_cast<void *>(createAccelerators);
		*reinterpret_cast<void **>(table + 0x30) = reinterpret_cast<void *>(allocatePool);
		std::lock_guard<std::mutex> guard(lock);
		twins[*accelerator] = twin;
	}
	*reinterpret_cast<void **>(static_cast<uint8_t *>(metadata) + 8) = description;
	return 0;
}

extern "C" __attribute__((visibility("default")))
int AVD_DestroyAVDAccelerator(void *accelerator) {
	if (!load()) return 5;
	void *description = nullptr, *twin = nullptr;
	{
		std::lock_guard<std::mutex> guard(lock);
		auto it = descriptions.find(accelerator);
		if (it != descriptions.end()) {
			description = it->second;
			descriptions.erase(it);
		}
		auto t = twins.find(accelerator);
		if (t != twins.end()) {
			twin = t->second;
			twins.erase(t);
		}
	}
	const int result = driverDestroy(accelerator);
	if (twin) iclDestroy(twin);
	free(description);
	return result;
}
