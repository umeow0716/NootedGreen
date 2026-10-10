#include "tgl_capability_adapter.hpp"
#include "owned_resource.hpp"
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <cstdio>
#include <memory>
#include <algorithm>
#include <mach-o/loader.h>

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

// CPU-only signed TGL CSC invocation: no factory/context/resource/GPU calls.
int cscReference() {
    void* library=dlopen(path,RTLD_NOW|RTLD_LOCAL);
    if(!library) return 1;
    const int status=[&]() {
        Dl_info info{};mach_header_64 header{};
        auto* symbol=dlsym(library,"AVD_CreateAVDAccelerator"); // identify only
        if(!symbol || !dladdr(symbol,&info) || !info.dli_fbase || !info.dli_fname ||
            std::strcmp(info.dli_fname,path)) return 2;
        const uintptr_t base=reinterpret_cast<uintptr_t>(info.dli_fbase);
        if(!read(base,&header,sizeof(header)) || header.magic!=MH_MAGIC_64 ||
            header.cputype!=CPU_TYPE_X86_64 || header.ncmds>256 ||
            header.sizeofcmds>32768 || base>UINTPTR_MAX-sizeof(header)-header.sizeofcmds) return 2;
        std::vector<uint8_t> commands(header.sizeofcmds);
        if(!read(base+sizeof(header),commands.data(),commands.size())) return 2;
        constexpr std::array<uint8_t,16> expected{
            0x2b,0x20,0x8b,0x8f,0xfe,0xb0,0x34,0xfb,0x89,0x66,0x3b,0x40,0xc2,0x47,0x69,0x26};
        bool identity=false;unsigned uuids=0;size_t offset=0;
        for(uint32_t i=0;i<header.ncmds;++i) {
            load_command command{};
            if(offset>commands.size() || commands.size()-offset<sizeof(command)) return 2;
            std::memcpy(&command,commands.data()+offset,sizeof(command));
            if(command.cmdsize<sizeof(command) || command.cmdsize>commands.size()-offset) return 2;
            if(command.cmd==LC_UUID) {
                if(command.cmdsize!=sizeof(uuid_command) || ++uuids!=1) return 2;
                uuid_command uuid{};std::memcpy(&uuid,commands.data()+offset,sizeof(uuid));
                identity=!std::memcmp(uuid.uuid,expected.data(),expected.size());
            }
            offset+=command.cmdsize;
        }
        if(!identity || offset!=commands.size()) return 2;
        auto qualify=[&](uintptr_t image) noexcept {return image==base && identity;};
        auto rx=[](uintptr_t address,size_t length) noexcept {
            if(!length || address>UINTPTR_MAX-length) return false;
            mach_vm_address_t start=address;mach_vm_size_t extent=0;natural_t depth=0;
            for(unsigned level=0;level<16;++level) {
                vm_region_submap_info_data_64_t region{};
                mach_msg_type_number_t count=VM_REGION_SUBMAP_INFO_COUNT_64;
                if(mach_vm_region_recurse(mach_task_self(),&start,&extent,&depth,
                    reinterpret_cast<vm_region_recurse_info_t>(&region),&count)!=KERN_SUCCESS) return false;
                if(region.is_submap) {++depth;continue;}
                return start<=address && extent<=UINTPTR_MAX-start && address+length<=start+extent &&
                    (region.protection&7)==(VM_PROT_READ|VM_PROT_EXECUTE);
            }
            return false;
        };
        auto boundedRead=[](uintptr_t p,void* out,size_t n) noexcept {return n<=64 && read(p,out,n);};
        TglOwnedCscProducer<decltype(boundedRead),decltype(qualify),decltype(rx)>
            producer(base,boundedRead,qualify,rx);
        TglCscCoefficients plain,swapped;
        if(!producer(plain,3,3,0) || !producer(swapped,3,3,1)) return 3;
        // Swap consistency alone could accept matching NaN/Inf payloads.
        // This CPU reference must also produce usable finite coefficients.
        for(const auto* coefficients : {&plain,&swapped}) {
            for(float value:coefficients->matrix) if(!std::isfinite(value)) return 4;
            for(float value:coefficients->inputOffsets) if(!std::isfinite(value)) return 4;
            for(float value:coefficients->outputOffsets) if(!std::isfinite(value)) return 4;
        }
        for(size_t row=0;row<3;++row) for(size_t column=0;column<3;++column)
            if(std::memcmp(&swapped.matrix[row*3+column],&plain.matrix[row*3+2-column],4)) return 4;
        if(plain.inputOffsets!=swapped.inputOffsets || plain.outputOffsets!=swapped.outputOffsets) return 4;
        std::puts("TGL_CSC_CPU_REFERENCE_OK exact-uuid native-matrix offsets format-swap no-gpu");
        return 0;
    }();
    dlclose(library);return status;
}

// Read our own loaded, sealed Tahoe framework. Never invoke a GPU/resource API.
int resourceReference(bool fenceOnly=false) {
    constexpr const char *framework = "/System/Library/PrivateFrameworks/IOAccelerator.framework/IOAccelerator";
    void *library = dlopen(framework, RTLD_NOW | RTLD_LOCAL);
    if (!library) { std::fprintf(stderr, "reference dlopen: %s\n", dlerror()); return 1; }
    int status = 1;
    struct Reference { const char *name; const char *symbol; size_t callOffset; bool block = false; };
    for (const auto &reference : {
            Reference{"IOAccelResourceCreate", "IOAccelResourceCreate", 0},
            Reference{"IOAccelResourceGetClientShared", "IOAccelResourceGetClientShared", 0},
            Reference{"IOAccelResourceFinishSysMem", "IOAccelResourceFinishSysMem", 0},
            // Read implementation only: a submit result and the word "Finish"
            // are not proof of completion or resource-retention semantics.
            Reference{"IOAccelVideoContextFinishFenceEvent", "IOAccelVideoContextFinishFenceEvent", 0},
            Reference{"IOAccelVideoContextSubmitDataBuffers", "IOAccelVideoContextSubmitDataBuffers", 0},
            Reference{"client-shared-generation", "IOAccelResourceGetClientShared", 0x15},
            Reference{"client-shared-map-setup", "IOAccelResourceGetClientShared", 0x5b},
            Reference{"client-shared-once-invoke", "IOAccelResourceGetClientShared", 0x5b, true}}) {
        if(fenceOnly && std::strcmp(reference.name,"IOAccelVideoContextFinishFenceEvent") &&
            std::strcmp(reference.name,"IOAccelVideoContextSubmitDataBuffers")) continue;
        const char *name = reference.name;
        void *symbol = dlsym(library, reference.symbol);
        Dl_info info{};
        mach_header_64 header{};
        if (!symbol || !dladdr(symbol, &info) || !info.dli_fbase || !info.dli_fname ||
            !read(reinterpret_cast<uintptr_t>(info.dli_fbase), &header, sizeof(header)) ||
            header.magic != MH_MAGIC_64 || header.ncmds > 256 ||
            header.sizeofcmds > 32768) goto done;
        std::vector<uint8_t> commands(header.sizeofcmds);
        if (!read(reinterpret_cast<uintptr_t>(info.dli_fbase) + sizeof(header),
                  commands.data(), commands.size())) goto done;
        uintptr_t textAddress = 0, textSize = 0;
        uuid_command uuid{};
        bool hasUuid = false;
        size_t offset = 0;
        for (uint32_t i = 0; i < header.ncmds; ++i) {
            load_command command{};
            if (offset > commands.size() || commands.size() - offset < sizeof(command)) goto done;
            std::memcpy(&command, commands.data() + offset, sizeof(command));
            if (command.cmdsize < sizeof(command) || command.cmdsize > commands.size() - offset) goto done;
            if (command.cmd == LC_UUID && command.cmdsize == sizeof(uuid)) {
                std::memcpy(&uuid, commands.data() + offset, sizeof(uuid)); hasUuid = true;
            }
            if (command.cmd == LC_SEGMENT_64 && command.cmdsize >= sizeof(segment_command_64)) {
                segment_command_64 segment{};
                std::memcpy(&segment, commands.data() + offset, sizeof(segment));
                if (!std::memcmp(segment.segname, "__TEXT\0", 7)) {
                    textAddress = segment.vmaddr; textSize = segment.vmsize;
                }
            }
            offset += command.cmdsize;
        }
        const uintptr_t base = reinterpret_cast<uintptr_t>(info.dli_fbase);
        uintptr_t address = reinterpret_cast<uintptr_t>(symbol);
        if (!hasUuid || !textAddress || address < base || address - base >= textSize) goto done;
        constexpr uint8_t expectedUuid[16] = {0x19,0x8a,0x77,0x6a,0xfe,0x03,0x35,0xee,
            0x8e,0x53,0x7b,0x9f,0x14,0xca,0xab,0xaa};
        if (std::memcmp(uuid.uuid, expectedUuid, sizeof(expectedUuid))) goto done;
        if (reference.callOffset) {
            // Exact sealed 25G229 image and the two previously captured direct
            // calls only. Read the callees; never execute either mapping helper.
            const uintptr_t call = address + reference.callOffset;
            uint8_t instruction[5]{};
            int32_t displacement = 0;
            if (textSize - (address - base) < reference.callOffset + sizeof(instruction) ||
                !read(call, instruction, sizeof(instruction)) || instruction[0] != 0xe8) goto done;
            std::memcpy(&displacement, instruction + 1, sizeof(displacement));
            const int32_t expected = reference.callOffset == 0x15 ? 0x58ed : 0x56d9;
            if (displacement != expected) goto done;
            address = call + sizeof(instruction) + displacement;
            if (address < base || address - base >= textSize) goto done;
        }
        if (reference.block) {
            // Proven setup LEA addresses its immutable global dispatch block.
            // Read only the block's invoke pointer, then bound it to this image.
            uint8_t lea[7]{};
            int32_t displacement = 0;
            uintptr_t invoke = 0;
            if (textSize - (address - base) < 18 ||
                !read(address + 11, lea, sizeof(lea)) ||
                std::memcmp(lea, "\x48\x8d\x35\x8a\x64\xd3\x31", sizeof(lea))) goto done;
            std::memcpy(&displacement, lea + 3, sizeof(displacement));
            const uintptr_t block = address + 18 + displacement;
            if (!read(block + 16, &invoke, sizeof(invoke)) ||
                invoke < base || invoke - base >= textSize) goto done;
            address = invoke;
        }
        const size_t length = std::min<size_t>(1024, textSize - (address - base));
        std::array<uint8_t, 1024> bytes{};
        if (!length || !read(address, bytes.data(), length)) goto done;
        if (length >= 6 && bytes[0] == 0xff && bytes[1] == 0x25) {
            int32_t displacement = 0;
            uintptr_t target = 0;
            Dl_info imported{};
            std::memcpy(&displacement, bytes.data() + 2, sizeof(displacement));
            if (!read(address + 6 + displacement, &target, sizeof(target)) ||
                !target || !dladdr(reinterpret_cast<void *>(target), &imported) ||
                !imported.dli_fname) goto done;
            std::printf("RESOURCE_IMPORT reference=%s image=%s symbol=%s\n", name,
                imported.dli_fname, imported.dli_sname ? imported.dli_sname : "<unavailable>");
        }
        std::printf("RESOURCE_REFERENCE symbol=%s image=%s uuid=", name, info.dli_fname);
        for (uint8_t byte : uuid.uuid) std::printf("%02x", byte);
        std::printf(" preferred=%llx length=%zu\n",
                    static_cast<unsigned long long>(textAddress + address - base), length);
        for (size_t i = 0; i < length; i += 32) {
            std::printf("%04zx:", i);
            for (size_t j = i; j < std::min(i + 32, length); ++j) std::printf("%02x", bytes[j]);
            std::putchar('\n');
        }
    }
    std::puts(fenceOnly ? "FENCE_REFERENCE_OK no-resource-call no-GPU no-text-write" :
        "RESOURCE_REFERENCE_OK no-resource-call no-GPU no-text-write");
    status = 0;
done:
    dlclose(library);
    return status;
}
}
int main(int argc, char **argv) {
    if(argc==2 && !std::strcmp(argv[1],"--csc-reference")) return cscReference();
    if(argc==2 && !std::strcmp(argv[1],"--fence-reference")) return resourceReference(true);
    if (argc == 2 && !std::strcmp(argv[1], "--resource-reference")) return resourceReference();
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
                if (i == 3) {
                    if (callbacks[i] != reinterpret_cast<uintptr_t>(dlsym(library, "NGRN_ObservedCreateContexts"))) goto cleanup;
                    continue;
                }
                Dl_info info{};
                if (!dladdr(reinterpret_cast<void *>(callbacks[i]), &info) ||
                    !info.dli_fname || std::strcmp(info.dli_fname, path)) goto cleanup;
            }
            std::puts("ADAPTER_NATIVE_CALLBACKS_UNCHANGED_OK slots=15 scoped-observer-slot=18");
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
