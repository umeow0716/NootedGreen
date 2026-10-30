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
#include <IOSurface/IOSurface.h>
#include <malloc/malloc.h>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sys/mman.h>
#include <unistd.h>
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
uint8_t *convert(const Description *old) {
	if (!old || old->count != slots || !old->slot) return nullptr;
	size_t entries = 0;
	for (size_t i = 0; i < slots; i++)
		for (auto *e = old->slot[i]; e; e = *reinterpret_cast<uint8_t **>(e + oldEntry))
			entries++;
	auto *out = static_cast<uint8_t *>(calloc(1, 0x20 + slots * 8 + entries * newEntry));
	if (!out) return nullptr;
	memcpy(out, old, 0x20);
	auto *slot = reinterpret_cast<uint8_t **>(out + 0x20);
	reinterpret_cast<Description *>(out)->slot = slot;
	auto *next = out + 0x20 + slots * 8;
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
using EncodeFrame = int (*)(void *, void *);
using FinishFence = int (*)(void *, void *, void *, void *, void *, void *);
EncodeFrame driverEncodeFrameCall;
FinishFence driverFinishFence;
thread_local uint8_t *encodingStatus;

void flushStatus(uint8_t *entry) {
	for (uintptr_t line = reinterpret_cast<uintptr_t>(entry) & ~63UL; line < reinterpret_cast<uintptr_t>(entry) + 0x38; line += 64)
		__builtin_ia32_clflush(reinterpret_cast<void *>(line));
	__builtin_ia32_mfence();
}

int encodeFrame(void *enc, void *frame) {
	auto *base = at<uint8_t *>(static_cast<uint8_t *>(enc), 0xb60);
	encodingStatus = base ? base + at<uint32_t>(static_cast<uint8_t *>(enc), 0x6bb4) * 0x38 : nullptr;
	if (encodingStatus) flushStatus(encodingStatus);
	static bool once;
	if (!once) { once = true; syslog(LOG_NOTICE, "ngva: encoder status hook active, entry %p", encodingStatus); }
	const int result = driverEncodeFrameCall(enc, frame);
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

template <typename T> bool replacePointer(uintptr_t offset, T replacement, T &original) {
	const long page = getpagesize();
	auto *slot = reinterpret_cast<T *>(driverBase + offset);
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

bool load() {
	static std::once_flag once;
	std::call_once(once, [] {
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
			if (!replacePointer<EncodeFrame>(0x754a90, encodeFrame, driverEncodeFrameCall) ||
			    !replacePointer<FinishFence>(0x7600d0, finishFence, driverFinishFence))
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
	auto *description = convert(original);
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
	*reinterpret_cast<void **>(static_cast<uint8_t *>(metadata) + 8) = description;
	return 0;
}

extern "C" __attribute__((visibility("default")))
int AVD_DestroyAVDAccelerator(void *accelerator) {
	if (!load()) return 5;
	void *description = nullptr;
	{
		std::lock_guard<std::mutex> guard(lock);
		auto it = descriptions.find(accelerator);
		if (it != descriptions.end()) {
			description = it->second;
			descriptions.erase(it);
		}
	}
	int result = driverDestroy(accelerator);
	free(description);
	return result;
}
