#include "descriptor_bridge.hpp"
#include <CommonCrypto/CommonDigest.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>

namespace {
constexpr const char *nativePath = "/Library/Extensions/AppleIntelTGLGraphicsVADriver.bundle/Contents/MacOS/AppleIntelTGLGraphicsVADriver";
constexpr uint8_t expectedHash[32] = {
    0xd2,0x65,0xf2,0x03,0x81,0x35,0xc2,0xc5,0xc3,0x64,0x9b,0x62,0x1d,0x9a,0xea,0x48,
    0xcf,0x1c,0xb7,0x4a,0x1f,0xb3,0x60,0x69,0xbb,0x1b,0x35,0x62,0xb3,0x75,0x01,0x29};
using Create = int (*)(void **, const void *, AvdMetadata *);
using Destroy = int (*)(void *);
struct Library { void *handle = nullptr; Create create = nullptr; Destroy destroy = nullptr; };
Library library;
std::once_flag loadOnce;
std::mutex registryLock;
std::map<void *, std::unique_ptr<AvdDescriptorBridge>> registry;
bool read(uintptr_t address, void *output, size_t length) {
    mach_vm_size_t copied = 0;
    return address && mach_vm_read_overwrite(mach_task_self(), address, length,
        reinterpret_cast<mach_vm_address_t>(output), &copied) == KERN_SUCCESS
        && copied == length;
}
bool pinnedFile() {
    FILE *file = std::fopen(nativePath, "rb");
    if (!file) return false;
    CC_SHA256_CTX context;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CC_SHA256_Init(&context);
    uint8_t buffer[8192], hash[32];
    size_t total = 0, count;
    while ((count = std::fread(buffer, 1, sizeof(buffer), file))) {
        total += count;
        if (total > 8450544) { std::fclose(file); return false; }
        CC_SHA256_Update(&context, buffer, static_cast<CC_LONG>(count));
    }
    bool valid = !std::ferror(file) && total == 8450544;
    CC_SHA256_Final(hash, &context);
#pragma clang diagnostic pop
    std::fclose(file);
    return valid && std::memcmp(hash, expectedHash, sizeof(hash)) == 0;
}
void load() {
    if (!pinnedFile()) return;
    library.handle = dlopen(nativePath, RTLD_NOW | RTLD_LOCAL);
    if (!library.handle) return;
    library.create = reinterpret_cast<Create>(dlsym(library.handle, "AVD_CreateAVDAccelerator"));
    library.destroy = reinterpret_cast<Destroy>(dlsym(library.handle, "AVD_DestroyAVDAccelerator"));
    // Keep the native image loaded for all forwarded callback lifetimes.
}
}

extern "C" __attribute__((visibility("default")))
int AVD_CreateAVDAccelerator(void **output, const void *input, AvdMetadata *metadata) {
    if (!output || !input || !metadata) return 5;
    *output = nullptr;
    *metadata = {};
    void *table = nullptr;
    try {
        std::call_once(loadOnce, load);
        if (!library.create || !library.destroy) return 10;
        AvdMetadata native{};
        int result = library.create(&table, input, &native);
        if (result) { if (table) library.destroy(table); return result; }
        auto bridge = std::make_unique<AvdDescriptorBridge>();
        if (!table || native.renderer != 0x1080080 || !bridge->build(native.descriptor, read)) {
            if (table) library.destroy(table);
            return 10;
        }
        native.descriptor = reinterpret_cast<uintptr_t>(bridge->descriptor());
        {
            std::lock_guard<std::mutex> lock(registryLock);
            if (registry.count(table)) { library.destroy(table); return 10; }
            registry.emplace(table, std::move(bridge));
        }
        // Keep the original 0x88 table and every native callback/context intact.
        *metadata = native;
        *output = table;
        std::fprintf(stderr, "NGRN_ADAPTER_FACTORY_TRANSLATED native=430 consumer=438\n");
        return 0;
    } catch (...) {
        if (table && library.destroy) library.destroy(table);
        return 7;
    }
}

extern "C" __attribute__((visibility("default")))
int AVD_DestroyAVDAccelerator(void *table) {
    if (!table) return 5;
    try {
    std::unique_ptr<AvdDescriptorBridge> bridge;
    {
        std::lock_guard<std::mutex> lock(registryLock);
        auto found = registry.find(table);
        if (found == registry.end()) return 5;
        bridge = std::move(found->second);
        registry.erase(found);
    }
    bridge.reset();
    return library.destroy(table);
    } catch (...) { return 7; }
}
