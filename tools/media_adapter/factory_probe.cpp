#include "tgl_capability_adapter.hpp"
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <cstdio>
#include <memory>

// Diagnostic only: no physical-init callback, IOConnect, media or installation.
namespace {
constexpr const char *path = "/Library/Extensions/AppleIntelTGLGraphicsVADriver.bundle/Contents/MacOS/AppleIntelTGLGraphicsVADriver";
struct Descriptor { uint32_t groups, reserved; uintptr_t groupList;
                    uint32_t count, reserved2; uintptr_t roots; };
struct Metadata { uint32_t renderer, reserved; uintptr_t descriptor;
                  uint64_t version, reserved2; };
static_assert(sizeof(Descriptor) == 0x20 && sizeof(Metadata) == 0x20);
bool read(uintptr_t address, void *output, size_t length) {
    mach_vm_size_t copied = 0;
    return address && mach_vm_read_overwrite(mach_task_self(), address, length,
        reinterpret_cast<mach_vm_address_t>(output), &copied) == KERN_SUCCESS
        && copied == length;
}
}
int main(int argc, char **argv) {
    const bool adapterMode = argc == 2 && std::strcmp(argv[1], "--adapter") == 0;
    if (argc != 1 && !adapterMode) return 64;
    std::puts("ADAPTER_PROBE_BEGIN factory-only no-physical-init no-text-patch");
    const char *selected = adapterMode ? "/Users/umeow/NootedGreenTGLMediaAdapter.bundle/Contents/MacOS/NootedGreenTGLMediaAdapter" : path;
    void *library = dlopen(selected, RTLD_NOW | RTLD_LOCAL);
    if (!library) { std::fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    using Create = int (*)(void **, const void *, Metadata *);
    using Destroy = int (*)(void *);
    auto create = reinterpret_cast<Create>(dlsym(library, "AVD_CreateAVDAccelerator"));
    auto destroy = reinterpret_cast<Destroy>(dlsym(library, "AVD_DestroyAVDAccelerator"));
    if (!create || !destroy) { dlclose(library); return 2; }
    std::array<uint64_t, 5> input{};
    Metadata metadata{};
    void *table = nullptr;
    int result = create(&table, input.data(), &metadata);
    std::printf("ADAPTER_FACTORY result=%d renderer=%x\n", result, metadata.renderer);
    if (result || !table) { if (table) destroy(table); dlclose(library); return 3; }
    int status = 4;
    {
        Descriptor descriptor{};
        std::array<uintptr_t, 35> roots{};
        std::vector<uintptr_t> addresses;
        std::vector<const TglCapabilityAdapter::Native *> records;
        TglCapabilityAdapter adapter;
        if (metadata.renderer != 0x1080080 ||
            !read(metadata.descriptor, &descriptor, sizeof(descriptor)) ||
            descriptor.groups != 3 || descriptor.count != roots.size() ||
            !read(descriptor.roots, roots.data(), sizeof(roots))) goto cleanup;
        for (uintptr_t root : roots) {
            uintptr_t address = root;
            size_t hops = 0;
            while (address) {
                if (++hops > 85) goto cleanup;
                bool seen = false;
                for (uintptr_t old : addresses) if (old == address) seen = true;
                if (seen) break;
                if (addresses.size() == 85) goto cleanup;
                std::array<uint8_t, 0x438> snapshot{};
                if (!read(address, snapshot.data(), adapterMode ? 0x438 : 0x430)) goto cleanup;
                if (adapterMode)
                    for (size_t i = 0x18; i < 0x20; ++i) if (snapshot[i]) goto cleanup;
                addresses.push_back(address);
                records.push_back(reinterpret_cast<const TglCapabilityAdapter::Native *>(address));
                std::memcpy(&address, snapshot.data() + (adapterMode ? 0x430 : 0x428), sizeof(address));
            }
        }
        if (adapterMode) {
            std::array<uintptr_t, 17> callbacks{};
            if (!read(reinterpret_cast<uintptr_t>(table), callbacks.data(), sizeof(callbacks))) goto cleanup;
            for (size_t i = 1; i < callbacks.size(); ++i) {
                Dl_info info{};
                if (!dladdr(reinterpret_cast<void *>(callbacks[i]), &info) ||
                    !info.dli_fname || std::strcmp(info.dli_fname, path)) goto cleanup;
            }
            std::puts("ADAPTER_NATIVE_CALLBACKS_UNCHANGED_OK slots=16");
        } else {
            if (!adapter.build(records)) goto cleanup;
            for (size_t i = 0; i < records.size(); ++i)
                if (adapter.native(adapter.consumer(i)) != records[i]) goto cleanup;
        }
        std::printf("ADAPTER_DATA_TRANSLATION_OK roots=%u records=%zu\n", descriptor.count, records.size());
        status = 0;
cleanup:
        adapter.clear(); // owned copies released before native descriptor
    }
    const int destroyed = destroy(table);
    dlclose(library);
    std::printf("ADAPTER_FACTORY_DESTROY result=%d\n", destroyed);
    return status ? status : (destroyed ? 5 : 0);
}
