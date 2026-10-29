// Tahoe AppleGVA front for the TGL VA driver.
//
// AppleGVA on Tahoe expects two things the TGL VA driver predates:
//  - the accelerator description returned in metadata+8 uses 0x438-byte codec
//    entries, with an extra 8-byte field at 0x20 (a copy of the field at 0x18);
//    the driver builds 0x428-byte entries;
//  - GetDriverParameter answers tag 0xe with that description.
// Load the driver, convert its description and answer tag 0xe here.
#include <dlfcn.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <map>
#include <syslog.h>
#include <utility>

extern "C" int AVD_CreateAVDAccelerator(void **, const void *, void *);

namespace {

using Create = int (*)(void **accelerator, const void *input, void *metadata);
using Destroy = int (*)(void *accelerator);
using GetParameter = int (*)(void *self, void *parameters);

Create driverCreate;
Destroy driverDestroy;
GetParameter driverGetParameter;
std::mutex lock;
std::map<void *, void *> descriptions; // accelerator -> converted description

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
	// AppleGVA rejects the description with the {0x20, 0x4000, 0x3e9} codec in it.
	uint32_t kept = 0;
	for (size_t i = 0; i < slots; i++) {
		auto *e = slot[i];
		if (e && *reinterpret_cast<uint32_t *>(e) == 0x20 && *reinterpret_cast<uint64_t *>(e + 8) == 0x4000 &&
		    *reinterpret_cast<uint32_t *>(e + 0x10) == 0x3e9)
			continue;
		slot[kept++] = e;
	}
	reinterpret_cast<Description *>(out)->count = kept;
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
	auto *description = convert(*reinterpret_cast<Description **>(context + 0x30));
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
