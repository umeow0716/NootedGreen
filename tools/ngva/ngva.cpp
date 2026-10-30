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
#include <malloc/malloc.h>
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

// Tahoe's AppleGVA drives the 0x4000 encoder entry with a newer protocol than
// this driver implements; offer it as the 0x40/0x80 pair the driver's H.264
// encoder is requested by.
void renameEncoders(Description *table) {
	if (!table || !table->slot) return;
	for (uint32_t i = 0; i < table->count; i++) {
		auto *e = table->slot[i];
		if (!e || *reinterpret_cast<uint32_t *>(e) != 0x20) continue;
		auto &value = *reinterpret_cast<uint64_t *>(e + 8);
		if (value == 0x4000) value = 0x40;
		else if (value == 0x2000) value = 0x80;
	}
}

using CreateAccelerators = int (*)(void *self, void *requests);
CreateAccelerators driverCreateAccelerators;

// The driver matches accelerator requests against its own copy (+0x68).
int createAccelerators(void *self, void *requests) {
	renameEncoders(*reinterpret_cast<Description **>(static_cast<uint8_t *>(self) + 0x68));
	return driverCreateAccelerators(self, requests);
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

int decode(void *self, void *raw, void *out) {
	auto *frames = static_cast<Frames *>(raw);
	const uint32_t count = frames && frames->frame ? frames->count : 0;
	uint8_t **slot[count ? count : 1], *saved[count ? count : 1];
	for (uint32_t i = 0; i < count; i++) {
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
	int result = driverDecode(self, raw, out);
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
	renameEncoders(original);
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
	auto *create = reinterpret_cast<CreateAccelerators *>(table + 0x18);
	if (!driverCreateAccelerators) driverCreateAccelerators = *create;
	*create = createAccelerators;
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
