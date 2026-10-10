#pragma once
#include <array>
#include <algorithm>
#include <cstdint>
#include <utility>
#include <cstring>
#include <limits>
#include <cmath>
#include <cstddef>

// Tahoe TGL Ief 125e90/126210. Tables must come from the authenticated
// native image (741e50..742350), not from a different GPU generation.
// This is a CPU data producer only; it does not issue an SFC command.
using TglOwnedIefTables = std::array<std::array<uint32_t,64>,5>;
// Synchronous callback binding only. Each domain has independent TLS so a VP
// child cannot resolve through its enclosing VEBOX child's scope.
template<class Context,class Domain> class TglOwnedScopedContext {
    struct Binding {void* child=nullptr;Context* context=nullptr;};
    inline static thread_local Binding current_{};
    bool bound_=false;
public:
    TglOwnedScopedContext(void* child,Context& context) noexcept {
        if(child && !current_.child && !current_.context) {
            current_={child,&context};bound_=true;
        }
    }
    ~TglOwnedScopedContext(){if(bound_) current_={};}
    TglOwnedScopedContext(const TglOwnedScopedContext&)=delete;
    TglOwnedScopedContext& operator=(const TglOwnedScopedContext&)=delete;
    bool bound() const noexcept {return bound_;}
    static Context* get(void* child) noexcept {
        return child && child==current_.child ? current_.context : nullptr;
    }
};
struct TglOwnedVpSfcContextDomain {};
template<class Context> using TglOwnedVpSfcScopedContext=
    TglOwnedScopedContext<Context,TglOwnedVpSfcContextDomain>;
template<class Context> struct TglOwnedVpSfcResolve {
    Context* operator()(void* child) const noexcept {
        return TglOwnedVpSfcScopedContext<Context>::get(child);
    }
};
// TGL12c460/12c480 write channel918 and borrowed performance9c8 through
// the VP base pointer. This proves a minimum caller-visible extent9d0,
// not a native TGL constructor, complete object size, or callable vtable.
// Keep this private/unpublished until every VP callback and owner is bound.
struct alignas(8) TglOwnedVpSfcCallerStorage {
    std::array<uint8_t,0x9d0> bytes{};
    // Borrowed constructor inputs follow ICL1dd150 base10/18/20. Authenticate
    // the TGL MHW object independently; callers retain all interface/image
    // leases. This initializes data only, not a callable native object.
    template<class Binding,class Read>
    bool initializeBorrowed(uintptr_t os,uintptr_t renderHal,uintptr_t mhw,
        const Binding& binding,Read read) {
        if(!os || !renderHal || !mhw || !binding.image) return false;
        for(auto byte:bytes) if(byte) return false;
        if(!binding.validateConstructedObject(mhw,os,read)) return false;
        std::memcpy(bytes.data()+0x10,&os,8);
        std::memcpy(bytes.data()+0x18,&renderHal,8);
        std::memcpy(bytes.data()+0x20,&mhw,8);
        return true;
    }
    void setCallerFields(uint32_t channel,uintptr_t performance) noexcept {
        std::memcpy(bytes.data()+0x918,&channel,4);
        std::memcpy(bytes.data()+0x9c8,&performance,8);
    }
};
static_assert(sizeof(TglOwnedVpSfcCallerStorage)==0x9d0);
// Caller ABI: TGL12c010 invokes VP+40(this,source,target,renderData),
// TGL12a120 invokes VP+48(this,renderData,command). Resolve must admit only
// the exact currently leased owned VP base; no native/foreign object cast.
// These two entries alone are NOT a complete/publishable VP vtable.
template<class Resolve> struct TglOwnedVpSfcCallbacks {
    // ICL1f88a7 invokes VP+30 as void(this). The enclosing owner must
    // independently authorize GPU retirement; this adapter never invents a
    // fence, destroys the CPU object, or converts the void ABI into status.
    static void freeResources(void* child) noexcept {
        static_assert(noexcept(Resolve{}(child)),"VP lease resolution must not throw");
        auto* context=Resolve{}(child);
        if(!context) return;
        static_assert(noexcept(context->freeResources()),"VP resource cleanup must not throw");
        context->freeResources();
    }
    static int setup(void* child,void* source,void* target,void* renderData) noexcept {
        static_assert(noexcept(Resolve{}(child)),"VP lease resolution must not throw");
        auto* context=Resolve{}(child);
        if(!context || !source || !target || !renderData) return 5;
        static_assert(noexcept(context->setup(source,target,renderData)),"VP setup status ABI");
        return context->setup(source,target,renderData);
    }
    static int send(void* child,void* renderData,void* command) noexcept {
        static_assert(noexcept(Resolve{}(child)),"VP lease resolution must not throw");
        auto* context=Resolve{}(child);
        if(!context || !renderData || !command) return 5;
        static_assert(noexcept(context->send(renderData,command)),"VP send status ABI");
        return context->send(renderData,command);
    }
};
// Code-only binding to the exact Tahoe TGL MhwSfcInterfaceG12 vtable.
// The owner must retain the authenticated image and separately construct
// the native MHW object (including its borrowed OS). Resolution is not
// object-lifetime, packet-shape or GPU execution proof.
struct TglSfcMhwBinding {
    uintptr_t image=0;
    std::array<uintptr_t,11> entries{};
    template<class Read,class Qualify,class Executable>
    bool resolve(uintptr_t base,Read read,Qualify qualify,Executable executable) {
        image=0; entries.fill(0);
        constexpr std::array<uintptr_t,11> offsets{
            0x16bed0,0x16bef0,0x1681e0,0x168390,0x16a580,0x16a770,
            0x16a8a0,0x16b190,0x16b390,0x16b590,0xfe6c0};
        if (!base || base>std::numeric_limits<uintptr_t>::max()-0x759478 ||
            !qualify(base)) return false;
        std::array<uintptr_t,2> metadata{};
        std::array<uintptr_t,11> candidate{};
        if (!read(base+0x759410,metadata.data(),sizeof(metadata)) ||
            metadata[0]!=0 || metadata[1]!=base+0x759490 ||
            !read(base+0x759420,candidate.data(),sizeof(candidate))) return false;
        for(size_t i=0;i<offsets.size();++i)
            if(candidate[i]!=base+offsets[i] || !executable(candidate[i],1)) return false;
        entries=candidate; image=base; return true;
    }
    // Post-construction admission only. Does not infer the allocation size or
    // create an object from raw storage. OS must outlive the admitted object.
    template<class Read>
    bool validateConstructedObject(uintptr_t object,uintptr_t os,Read read) const {
        if(!image || !object || !os || object>UINTPTR_MAX-0x41 || os>UINTPTR_MAX-0x90)
            return false;
        std::array<uint8_t,0x41> bytes{};
        std::array<uint32_t,2> osFlags{};
        if(!read(object,bytes.data(),bytes.size()) ||
           !read(os+0x88,osFlags.data(),sizeof(osFlags)) ||
           (!osFlags[0] && !osFlags[1])) return false;
        uintptr_t table=0,callback=0,borrowedOs=0;
        uint16_t widthAlign=0,heightAlign=0;
        std::memcpy(&table,bytes.data(),8);
        std::memcpy(&callback,bytes.data()+8,8);
        std::memcpy(&borrowedOs,bytes.data()+0x10,8);
        std::memcpy(&widthAlign,bytes.data()+0x18,2);
        std::memcpy(&heightAlign,bytes.data()+0x1a,2);
        return table==image+0x759420 && borrowedOs==os &&
            callback==image+(osFlags[1]?0x3e6a0:0x3eae0) &&
            widthAlign==16 && heightAlign==4 && bytes[0x40]==1;
    }
};
struct TglSfcLifetimeBinding {
    uintptr_t image=0,construct=0,destroy=0;
    template<class Read,class Qualify,class Executable>
    bool resolve(uintptr_t base,Read read,Qualify qualify,Executable executable) {
        image=construct=destroy=0;
        constexpr std::array<uint8_t,16> ctor{
            0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x20,
            0x48,0x89,0x7d,0xf8,0x48,0x89,0x75,0xf0};
        constexpr std::array<uint8_t,16> dtor{
            0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x10,
            0x48,0x89,0x7d,0xf8,0x48,0x8b,0x7d,0xf8};
        std::array<uint8_t,16> c{},d{};
        if(!base || base>UINTPTR_MAX-0x16bee0 || !qualify(base) ||
           !executable(base+0x16bdd0,16) || !executable(base+0x16bed0,16) ||
           !read(base+0x16bdd0,c.data(),16) || !read(base+0x16bed0,d.data(),16) ||
           c!=ctor || d!=dtor) return false;
        image=base;construct=base+0x16bdd0;destroy=base+0x16bed0;
        return true;
    }
};
struct TglNativeSfcLifetimeInvoker {
    void construct(uintptr_t entry,void *object,uintptr_t os) noexcept {
        using Fn=void(*)(void*,void*);
        reinterpret_cast<Fn>(entry)(object,reinterpret_cast<void*>(os));
    }
    void destroy(uintptr_t entry,void *object) noexcept {
        using Fn=void(*)(void*);
        reinterpret_cast<Fn>(entry)(object);
    }
};
// Factory1c9780 proves complete allocation0x48; ctor16be80 forwards16bdd0.
// The native ctor/dtor own no nested allocations. Borrowed OS, image lease,
// invoker and serialization must outlive this nonmoving owner.
template<class Invoker> class TglOwnedSfcMhw {
    alignas(8) std::array<uint8_t,0x48> storage{};
    Invoker &invoke;
    TglSfcLifetimeBinding lifetime{};
    uintptr_t borrowedOs=0;
    bool constructed=false,ready=false;
public:
    explicit TglOwnedSfcMhw(Invoker &invoker):invoke(invoker){}
    ~TglOwnedSfcMhw(){reset();}
    TglOwnedSfcMhw(const TglOwnedSfcMhw&)=delete;
    TglOwnedSfcMhw& operator=(const TglOwnedSfcMhw&)=delete;
    template<class Read>
    bool initialize(const TglSfcLifetimeBinding &binding,const TglSfcMhwBinding &mhw,
                    uintptr_t os,Read read) {
        reset();
        if(!binding.image || binding.image!=mhw.image ||
           binding.construct!=binding.image+0x16bdd0 ||
           binding.destroy!=binding.image+0x16bed0 ||
           !os || os>UINTPTR_MAX-0x90) return false;
        std::array<uint32_t,2> flags{};
        if(!read(os+0x88,flags.data(),sizeof(flags)) || (!flags[0]&&!flags[1])) return false;
        lifetime=binding;
        invoke.construct(lifetime.construct,storage.data(),os);
        constructed=true;
        const uintptr_t object=reinterpret_cast<uintptr_t>(storage.data());
        auto objectRead=[&](uintptr_t address,void *dst,size_t size) {
            if(address==object && size==0x41) {
                std::memcpy(dst,storage.data(),size);return true;
            }
            return read(address,dst,size);
        };
        if(!mhw.validateConstructedObject(object,os,objectRead)){reset();return false;}
        borrowedOs=os;ready=true;return true;
    }
    void *get() noexcept {return ready?storage.data():nullptr;}
    uintptr_t image() const noexcept {return ready?lifetime.image:0;}
    uintptr_t os() const noexcept {return ready?borrowedOs:0;}
    void reset() noexcept {
        ready=false;
        if(constructed){constructed=false;invoke.destroy(lifetime.destroy,storage.data());}
        lifetime={};borrowedOs=0;storage.fill(0);
    }
};
// TGL16b490/16b290 consume Y 0x200 / UV 0x100 bytes at argument+4.
// The wrapper reads byte0 as pipe mode; these are CPU arguments, NOT GPU
// command packets (native adds its own 0x204 / 0x104 command header).
template<size_t Dwords> struct TglOwnedSfcTableParameters {
    uint8_t pipeMode=0;
    std::array<uint8_t,3> reserved{};
    std::array<uint32_t,Dwords> coefficients{};
};
using TglOwnedSfcYParameters=TglOwnedSfcTableParameters<128>;
using TglOwnedSfcUvParameters=TglOwnedSfcTableParameters<64>;
// Exact TGL168390 reads pointers through +f0 (8 bytes). The ICLb8
// producer below is a reference-layout specification, NOT this argument ABI.
using TglOwnedSfcStateParametersPacket=std::array<uint8_t,0xf8>;
// TGL168390: +66 feeds DW4 bit10; +6a feeds BOTH bit6 and bit3.
// Pinned Gen12 MHW names these RGB adaptive and 8-tap chroma filtering.
// ICL175950 instead derives bit6 from frame mode (+38), so that derivation
// is not a compatible producer for TGL. Caller chooses the pass semantics.
inline void tglPrepareOwnedSfcGen12FilterFields(
    TglOwnedSfcStateParametersPacket &packet,bool rgbAdaptive,
    bool eightTapChroma) noexcept {
    packet[0x66]=rgbAdaptive ? 1u : 0u;
    packet[0x6a]=eightTapChroma ? 1u : 0u;
}
enum class TglOwnedSfcCompressionMode : uint32_t {
    Disabled=0, Media=3, Render=4
};
// TGL168390 enables compression only for modes3/4, selecting render with4.
// ICL175950 tests mode2 instead. Never reinterpret its mode1/2 producer.
// This encodes an already-qualified resource mode; it does not enable MMC
// on the resource or replace OS registration/capability validation.
inline bool tglPrepareOwnedSfcGen12CompressionFields(
    TglOwnedSfcStateParametersPacket &packet,bool enabled,
    TglOwnedSfcCompressionMode mode) noexcept {
    if(mode!=TglOwnedSfcCompressionMode::Disabled &&
       mode!=TglOwnedSfcCompressionMode::Media &&
       mode!=TglOwnedSfcCompressionMode::Render) return false;
    if(enabled && mode==TglOwnedSfcCompressionMode::Disabled) return false;
    const uint32_t value=static_cast<uint32_t>(mode);
    packet[0x8b]=enabled ? 1u : 0u;
    std::memcpy(packet.data()+0x8c,&value,sizeof(value));
    return true;
}
// TGL1693eb..1694xx uses unsigned region extents, double division,
// 524288.0 and +0.5, truncates to int64, then retains23 bits. This is a
// CPU reference for native command verification, NOT an ICL float-scale
// command producer. Zero output extent is rejected before native division.
inline bool tglOwnedSfcGen12ScalingSteps(const TglOwnedSfcStateParametersPacket &packet,
    std::array<uint32_t,2> &steps) noexcept {
    uint32_t inputX,inputY,outputX,outputY;
    std::memcpy(&inputX,packet.data()+0x3c,4);
    std::memcpy(&inputY,packet.data()+0x40,4);
    std::memcpy(&outputX,packet.data()+0x4c,4);
    std::memcpy(&outputY,packet.data()+0x50,4);
    if(!inputX || !inputY || !outputX || !outputY) return false;
    // Every DWORD ratio is finite and the scaled result fits int64.
    const auto step=[](uint32_t input,uint32_t output) {
        const double ratio=double(input)/double(output);
        return uint32_t(int64_t(ratio*524288.0+0.5))&0x7fffffu;
    };
    steps={step(inputX,outputX),step(inputY,outputY)};
    return true;
}
// TGL168390 native loads, correlated by command bitfields with pinned Intel
// Gen12 state semantics. Darwin offsets, never sizeof(Linux struct).
struct TglOwnedSfcGen12Tail {
    uint32_t engineMode=0,inputBitDepth=0,tileType=0;
    uint32_t srcStartX=0,srcEndX=0,dstStartX=0,dstEndX=0;
    // Histogram is a SURFACE shell (native also reads its +194 DWORD),
    // unlike the three scratch RESOURCE pointers. Never pass a bare
    // resource allocation as histogramSurface; backing/registration remain
    // caller obligations. Native62a40 qualifies each optional tail resource.
    // Native relocation DW26/38/44 identifies SFD row/AVS column/SFD
    // column respectively. Darwin has no IEF-column slot in this tail;
    // do not transplant the six-resource Linux Gen12 struct ordering.
    uintptr_t histogramSurface=0,sfdLineBuffer=0,avsLineTile=0,sfdLineTile=0;
};
// Prefix must be independently qualified for the current TGL pass. This
// encoder does not establish ICL/TGL prefix equivalence or allocate backing.
// Consumer audit: ICL175950 masks dimensions at +30/+34 to 12 bits;
// TGL168390 uses the same input offsets but 14-bit command fields, consumes
// +8a as an additional format flag, and accepts additional format cases.
// Therefore matching offsets alone must never qualify an ICL-produced pass.
// Nonnull resources are borrowed and require registration/lifetime by caller.
inline bool tglPrepareOwnedSfcGen12State(TglOwnedSfcStateParametersPacket &output,
    const std::array<uint8_t,0xb8> &prefix,const TglOwnedSfcGen12Tail &tail) noexcept {
    if(tail.engineMode>3 || tail.inputBitDepth>2 || tail.tileType>1 ||
       tail.srcStartX>0x3fff || tail.srcEndX>0x3fff ||
       tail.dstStartX>0x3fff || tail.dstEndX>0x3fff) return false;
    TglOwnedSfcStateParametersPacket candidate{};
    std::memcpy(candidate.data(),prefix.data(),prefix.size());
    const std::array<uint32_t,7> scalars{tail.engineMode,tail.inputBitDepth,tail.tileType,
        tail.srcStartX,tail.srcEndX,tail.dstStartX,tail.dstEndX};
    std::memcpy(candidate.data()+0xb8,scalars.data(),sizeof(scalars));
    const std::array<uintptr_t,4> resources{tail.histogramSurface,tail.sfdLineBuffer,
        tail.avsLineTile,tail.sfdLineTile};
    static_assert(sizeof(uintptr_t)==8,"Darwin x86_64 state pointers");
    std::memcpy(candidate.data()+0xd8,resources.data(),sizeof(resources));
    output=candidate;return true;
}
// Compose the audited Gen12 fields transactionally. The remaining prefix
// semantics and borrowed resources still require caller qualification; this
// is not permission to transplant an ICL object/pass or submit GPU commands.
inline bool tglPrepareOwnedSfcGen12Pass(TglOwnedSfcStateParametersPacket &output,
    const std::array<uint8_t,0xb8> &prefix,const TglOwnedSfcGen12Tail &tail,
    bool rgbAdaptive,bool eightTapChroma,bool compressionEnabled,
    TglOwnedSfcCompressionMode compressionMode) noexcept {
    // Pinned Gen12 engineMode describes HCP column scalability, not a
    // VEBOX engine selector. Our VEBOX producer emits pipe1; never silently
    // accept a caller's HCP mode for that pass. Other pipe policies remain
    // separately qualified by the caller, not inferred here.
    if(prefix[0]==1 && tail.engineMode!=0) return false;
    TglOwnedSfcStateParametersPacket candidate{};
    if(!tglPrepareOwnedSfcGen12State(candidate,prefix,tail)) return false;
    // Native count-minus-one fields are14 bits; reject silent wraparound.
    for(size_t offset:{size_t(0x24),size_t(0x28),size_t(0x30),size_t(0x34),
                       size_t(0x3c),size_t(0x40),size_t(0x4c),size_t(0x50)}) {
        uint32_t extent;std::memcpy(&extent,candidate.data()+offset,4);
        if(!extent || extent>0x4000) return false;
    }
    for(size_t offset:{size_t(0x44),size_t(0x48),size_t(0x54),size_t(0x58)}) {
        uint32_t position;std::memcpy(&position,candidate.data()+offset,4);
        const uint32_t maximum=offset<0x54 ? 0x3fff : 0x7fff;
        if(position>maximum) return false;
    }
    tglPrepareOwnedSfcGen12FilterFields(candidate,rgbAdaptive,eightTapChroma);
    if(!tglPrepareOwnedSfcGen12CompressionFields(candidate,compressionEnabled,
                                                 compressionMode)) return false;
    output=candidate;return true;
}
static_assert(offsetof(TglOwnedSfcYParameters,coefficients)==4);
static_assert(offsetof(TglOwnedSfcUvParameters,coefficients)==4);
static_assert(sizeof(TglOwnedSfcYParameters)==0x204);
static_assert(sizeof(TglOwnedSfcUvParameters)==0x104);
struct TglNativeSfcCommandInvoker {
    int parameters(uintptr_t entry,void *object,void *command,const void *parameters) const {
        using Fn=int(*)(void*,void*,const void*);
        return reinterpret_cast<Fn>(entry)(object,command,parameters);
    }
    int state(uintptr_t entry,void *object,void *command,const void *parameters,
              const void *output) const {
        using Fn=int(*)(void*,void*,const void*,const void*);
        return reinterpret_cast<Fn>(entry)(object,command,parameters,output);
    }
    int frame(uintptr_t entry,void *object,void *command,uint8_t mode) const {
        using Fn=int(*)(void*,void*,uint8_t);
        return reinterpret_cast<Fn>(entry)(object,command,mode);
    }
};
// Typed native command bridge, not the complete SendSfcCmd implementation.
// Caller owns command capacity, resource registration, platform callback and
// submission/fence lifetime. No native invocation occurs with a retired owner.
template<class SfcInvoker,class Invoke> class TglOwnedSfcCommandBackend {
    const TglSfcMhwBinding &binding;
    TglOwnedSfcMhw<SfcInvoker> &owner;
    void *command;
    Invoke &invoke;
    bool valid(size_t slot,uintptr_t offset) const noexcept {
        return command && owner.get() && binding.image==owner.image() &&
            binding.image && binding.entries[slot/8]==binding.image+offset;
    }
public:
    TglOwnedSfcCommandBackend(const TglSfcMhwBinding &b,TglOwnedSfcMhw<SfcInvoker> &o,
                             void *c,Invoke &i):binding(b),owner(o),command(c),invoke(i){}
    int lock(const std::array<uint8_t,8>& parameters) {
        return valid(0x10,0x1681e0)?invoke.parameters(binding.entries[2],owner.get(),command,parameters.data()):5;
    }
    int state(const TglOwnedSfcStateParametersPacket& parameters,const std::array<uint8_t,0x38>& output) {
        return valid(0x18,0x168390)?invoke.state(binding.entries[3],owner.get(),command,parameters.data(),output.data()):5;
    }
    int avs(const std::array<uint8_t,16>& parameters) {
        return valid(0x20,0x16a580)?invoke.parameters(binding.entries[4],owner.get(),command,parameters.data()):5;
    }
    int ief(const std::array<uint8_t,64>& parameters) {
        return valid(0x30,0x16a8a0)?invoke.parameters(binding.entries[6],owner.get(),command,parameters.data()):5;
    }
    int uv(const TglOwnedSfcUvParameters& parameters) {
        return valid(0x38,0x16b190)?invoke.parameters(binding.entries[7],owner.get(),command,&parameters):5;
    }
    int y(const TglOwnedSfcYParameters& parameters) {
        return valid(0x40,0x16b390)?invoke.parameters(binding.entries[8],owner.get(),command,&parameters):5;
    }
    int frameStart() {
        return valid(0x28,0x16a770)?invoke.frame(binding.entries[5],owner.get(),command,uint8_t(1)):5;
    }
};
template<size_t Dwords>
TglOwnedSfcTableParameters<Dwords> tglOwnedSfcTableParameters(
    uint8_t pipeMode,const std::array<uint32_t,Dwords> &coefficients) noexcept {
    TglOwnedSfcTableParameters<Dwords> result{};
    result.pipeMode=pipeMode;
    result.coefficients=coefficients;
    return result;
}
// readImage is an owner-supplied authenticated-image reader, not an arbitrary
// process pointer. All five bounded reads must succeed before publication.
template<class ReadImage>
bool tglReadOwnedIefTables(TglOwnedIefTables &output, ReadImage readImage) {
    TglOwnedIefTables candidate{};
    for (size_t i=0; i<candidate.size(); ++i)
        if (!readImage(size_t(0x741e50+i*0x100),candidate[i].data(),size_t(0x100)))
            return false;
    output=candidate;
    return true;
}
struct TglOwnedIefSurfaceFlags {
    uint8_t enabled = 0; // native surface+0x67
    uint8_t skinEnabled = 0; // native surface+0x68
};
inline bool tglBuildOwnedIefParameters(
    std::array<uint8_t,64> &output, TglOwnedIefSurfaceFlags &surface,
    float strength, uint8_t skinEnable, uint8_t detailEnable,
    const std::array<uint16_t,3> &thresholds,
    const TglOwnedIefTables &tables) noexcept {
    // Avoid undefined C++ conversion outside CVTTSS2SI's defined range.
    // Native stores the low WORD before clamping the unsigned index.
    if (!std::isfinite(strength) || double(strength) < -2147483648.0 ||
        double(strength) >= 2147483648.0) return false;
    uint16_t index = uint16_t(int32_t(strength));
    if (index >= 64) index = 63;
    auto candidate = output;
    candidate[1] = detailEnable & 1;
    candidate[2] = 1;
    candidate[3] = 1;
    for (size_t i=0; i<3; ++i) candidate[4+i] = uint8_t(thresholds[i]);
    // Native deliberately leaves the six DWORDs untouched at strength zero.
    if (index != 0) {
        const uint32_t factor = index;
        std::memcpy(candidate.data()+8, &factor, sizeof(factor));
        for (size_t i=0; i<5; ++i)
            std::memcpy(candidate.data()+12+i*4, &tables[i][index], 4);
    }
    output = candidate;
    surface.enabled = 1;
    surface.skinEnabled = skinEnable & 1;
    return true;
}
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <IOSurface/IOSurfaceRef.h>
#endif

// Gen12 g12_base.cpp242 + MOS IS_PA_FORMAT, not DI's exclusion-only rule.
// Preserve the explicit generic Format_PA=-7 enum; do not truncate unknowns.
constexpr bool tglVeboxFormatSupported(uint32_t format) {
    if (format >= 0x0d && format <= 0x15) return true;
    switch (format) {
        case uint32_t(-7): case 0x17: case 0x19:
        case 0x52: case 0x53: case 0x58: case 0x59:
        case 0x4a: case 0x4c: case 0x4d: return true;
        default: return false;
    }
}

// Intel Gen12 IsDnFormatSupported (pinned g12_base.cpp329), with Darwin format
// values independently pinned by Tahoe ICL1fc530. Pure format predicate only:
// does not assert that DN is requested, eligible, allocated or GPU-complete.
constexpr bool tglVeboxDnFormatSupported(uint32_t format) {
    switch (format) {
        case 0x0e: case 0x11: case 0x0f: case 0x10: case 0x0d:
        case 0x4a: case 0x19: case 0x15: case 0x12: case 0x13:
        case 0x14: case 0x17: case 0x59: case 0x53: case 0x52:
        case 0x03: case 0x05: return true;
        default: return false;
    }
}
template<class Read, class Predicate>
bool tglVeboxSurfaceFormatPredicate(uintptr_t surface, Read read, Predicate predicate) {
    uint32_t format = 0;
    return surface && surface <= std::numeric_limits<uintptr_t>::max() - 0x133 &&
        read(surface + 0x130, &format, sizeof(format)) && predicate(format);
}
template<class Read>
bool tglVeboxSurfaceSupported(uintptr_t surface, Read read) {
    return tglVeboxSurfaceFormatPredicate(surface, read, tglVeboxFormatSupported);
}
// Native 12d0ae..12d0c4 calls slot90 as bool(this, surface). Keep this
// boundary distinct from the pure enum predicate: a failed bounded read must
// reject the surface, not silently interpret an absent format as zero.
template<class Read>
bool tglOwnedVeboxSurfaceSupported(void* child, uintptr_t surface) noexcept {
    static_assert(noexcept(Read{}(uintptr_t{},static_cast<void*>(nullptr),size_t{})),
                  "format callback reader must fail without throwing");
    return child && tglVeboxSurfaceSupported(surface,Read{});
}
#if defined(__APPLE__)
struct TglOwnedFormatReader {
    bool operator()(uintptr_t address,void* output,size_t size) const noexcept {
        // This reader serves only DWORD format/colour queries, never an
        // arbitrary object dump. Native VM policy remains authoritative.
        if(!address || !output || size!=sizeof(uint32_t) ||
            address>std::numeric_limits<uintptr_t>::max()-(size-1)) return false;
        mach_vm_size_t actual=0;
        return mach_vm_read_overwrite(mach_task_self(),address,size,
            reinterpret_cast<mach_vm_address_t>(output),&actual)==KERN_SUCCESS &&
            actual==size;
    }
};
#endif
// Gen12 IsRTFormatSupported: generic YUV/monochrome output, or the intersection
// of IS_RGB32_FORMAT and IS_RGB_NO_SWAP for BT2020 P010/P016 input.
// Darwin ICL1fc360 independently pins the two-surface ABI, ColorSpace at +0
// (BT2020=0xb), and Format at +0x130; do not reuse its older output whitelist.
constexpr bool tglVeboxRtFormatSupported(uint32_t sourceColorSpace,
                                       uint32_t sourceFormat, uint32_t targetFormat) {
    return tglVeboxFormatSupported(targetFormat) ||
        (sourceColorSpace == 0x0b && (sourceFormat == 0x53 || sourceFormat == 0x52) &&
         (targetFormat == 0x03 || targetFormat == 0x04 || targetFormat == 0x50));
}
template<class Read>
bool tglVeboxRtSurfaceSupported(uintptr_t source, uintptr_t target, Read read) {
    constexpr auto max = std::numeric_limits<uintptr_t>::max();
    if (!source || !target || source > max - 0x133 || target > max - 0x133) return false;
    uint32_t targetFormat = 0, sourceFormat = 0, sourceColorSpace = 0;
    if (!read(target + 0x130, &targetFormat, sizeof(targetFormat))) return false;
    if (tglVeboxFormatSupported(targetFormat)) return true;
    return read(source, &sourceColorSpace, sizeof(sourceColorSpace)) &&
        read(source + 0x130, &sourceFormat, sizeof(sourceFormat)) &&
        tglVeboxRtFormatSupported(sourceColorSpace, sourceFormat, targetFormat);
}
template<class Read>
bool tglVeboxDnSurfaceSupported(uintptr_t surface, Read read) {
    return tglVeboxSurfaceFormatPredicate(surface, read, tglVeboxDnFormatSupported);
}
// Gen12 g12_base.cpp370, NOT the shorter ICL1fc690 exclusion table.
// MOS_FORMAT values: AYUV15/Y41614/Y41017/ABGR3/ARGB1/B10..51/R10..50/
// ABGR64=5/ARGB64=6. This is the DI predicate only; generic format admission
// remains mandatory (a format not excluded here is NOT automatically usable).
constexpr bool tglVeboxDiFormatSupported(uint32_t format) {
    switch (format) {
        case 0x15: case 0x14: case 0x17: case 0x03: case 0x01:
        case 0x51: case 0x50: case 0x05: case 0x06: return false;
        default: return true;
    }
}
template<class Read>
bool tglVeboxDiSurfaceSupported(uintptr_t surface, Read read) {
    return tglVeboxSurfaceFormatPredicate(surface, read, tglVeboxDiFormatSupported);
}

// Darwin method signatures (ICL1fc360/1fc530/1fc690), with Gen12 predicates
// above rather than the older ICL whitelists. These are callbacks only; they
// do not allocate resources or establish mode feasibility. TGL12a882/12a95e
// confirm a0/a8 bool(this,surface); ICL secondary794508+98 uses the two-surface
// adjustment thunk1fc500. Never transplant that thunk or its adjusted this.
template<class Read>
struct TglOwnedVeboxFormatCallbacks {
    static_assert(noexcept(Read{}(uintptr_t{},static_cast<void*>(nullptr),size_t{})),
                  "format callback reader must fail without throwing");
    static bool renderTarget(void* child,uintptr_t source,uintptr_t target) noexcept {
        return child && tglVeboxRtSurfaceSupported(source,target,Read{});
    }
    static bool denoise(void* child,uintptr_t surface) noexcept {
        return child && tglVeboxDnSurfaceSupported(surface,Read{});
    }
    static bool deinterlace(void* child,uintptr_t surface) noexcept {
        return child && tglVeboxDiSurfaceSupported(surface,Read{});
    }
};

struct TglSurfaceRect {
    float left, top, right, bottom;
    bool finite() const {
        return std::isfinite(left) && std::isfinite(top) &&
            std::isfinite(right) && std::isfinite(bottom);
    }
};
// Geometry portion of Gen12 IS_OUTPUT_PIPE_VEBOX_FEASIBLE. Darwin uses float
// rectangles at surface+30/+40/+50, not Linux integer RECT storage. Do not
// copy ICL1fb2ec's max-source SIZE EQUALITY in place of Gen12 containment.
// This alone is NOT direct-pipe admission: feature/format/CSC/alpha gates remain.
inline bool tglDirectVeboxGeometry(const TglSurfaceRect& source,
                                   const TglSurfaceRect& destination,
                                   const TglSurfaceRect& maximumSource,
                                   const TglSurfaceRect& targetDestination) {
    if (!source.finite() || !destination.finite() || !maximumSource.finite() ||
        !targetDestination.finite()) return false;
    auto sameSize = [](const TglSurfaceRect& a, const TglSurfaceRect& b) {
        const float aw = a.right - a.left, ah = a.bottom - a.top;
        const float bw = b.right - b.left, bh = b.bottom - b.top;
        return std::isfinite(aw) && std::isfinite(ah) && std::isfinite(bw) &&
            std::isfinite(bh) && aw == bw && ah == bh;
    };
    return sameSize(source, destination) && sameSize(destination, targetDestination) &&
        maximumSource.left <= source.left && maximumSource.top <= source.top &&
        maximumSource.right >= source.right && maximumSource.bottom >= source.bottom &&
        source.top == 0 && source.left == 0 && destination.top == 0 && destination.left == 0;
}

template<class Read>
bool tglDirectVeboxSurfaceGeometry(uintptr_t source, uintptr_t target, Read read) {
    constexpr auto max = std::numeric_limits<uintptr_t>::max();
    if (!source || !target || source > max - 0x5f || target > max - 0x4f) return false;
    static_assert(sizeof(TglSurfaceRect) == 16, "Darwin float rectangle ABI");
    TglSurfaceRect src{}, dst{}, maximum{}, targetDst{};
    return read(source + 0x30, &src, sizeof(src)) &&
        read(source + 0x40, &dst, sizeof(dst)) &&
        read(source + 0x50, &maximum, sizeof(maximum)) &&
        read(target + 0x40, &targetDst, sizeof(targetDst)) &&
        tglDirectVeboxGeometry(src, dst, maximum, targetDst);
}

// Gen12 IS_COMP_BYPASS_FEASIBLE; this is only the first output-pipe gate, not
// VEBOX/SFC admission. Darwin field reads pinned by ICL1fb16b..1fb205.
template<class Read>
bool tglCompositionBypassFeasible(uintptr_t params, uintptr_t source,
                                  uintptr_t pass, Read read) {
    constexpr auto max = std::numeric_limits<uintptr_t>::max();
    if (!params || !source || !pass || params > max - 0xdf || source > max - 0x82)
        return false;
    uint32_t sources = 0, targets = 0;
    uintptr_t blending = 0, lumaKey = 0, constriction = 0;
    uint8_t compNeeded = 0, interlaced = 0, weaving = 0;
    return read(pass, &compNeeded, 1) && !(compNeeded & 1) &&
        read(params, &sources, 4) && sources == 1 &&
        read(params + 0x90, &targets, 4) && targets == 1 &&
        read(source + 0x60, &blending, 8) && !blending &&
        read(source + 0x81, &interlaced, 1) && !(interlaced & 1) &&
        read(source + 0x82, &weaving, 1) && !(weaving & 1) &&
        read(source + 0x68, &lumaKey, 8) && !lumaKey &&
        read(params + 0xd8, &constriction, 8) && !constriction;
}

// Complete Gen12 direct-output feasibility after composition bypass. Caller
// owns the pass lock/image lease. queryTwoPass must call the authenticated TGL
// native CSC predicate, returning false if binding/read fails (not "no CSC").
// No mode/state is published here; SFC and composition remain separate branches.
template<class Read, class QueryTwoPass>
bool tglDirectVeboxFeasible(uintptr_t params, uintptr_t source, uintptr_t target,
                           uint32_t bypassMode, Read read, QueryTwoPass queryTwoPass) {
    constexpr auto max = std::numeric_limits<uintptr_t>::max();
    if (!bypassMode || !params || !source || !target || params > max - 0xf7 ||
        source > max - 0x293 || target > max - 0x133) return false;
    uint32_t targets = 0, sample = 0, rotation = 0, alphaMode = 0;
    uintptr_t actualTarget = 0, ief = 0, alpha = 0;
    uint8_t variance = 0;
    if (!read(params + 0x90, &targets, 4) || targets != 1 ||
        !read(params + 0x98, &actualTarget, 8) || actualTarget != target ||
        !tglDirectVeboxSurfaceGeometry(source, target, read) ||
        !read(source + 0x78, &ief, 8) || ief || // Gen12 requires NULL, not ICL disabled IEF
        !read(source + 0x138, &sample, 4) || sample != 0 ||
        !read(source + 0x290, &rotation, 4) || rotation != 0 ||
        !read(source + 0x83, &variance, 1) || (variance & 1) ||
        !tglVeboxSurfaceSupported(source, read) ||
        !tglVeboxRtSurfaceSupported(source, target, read)) return false;
    bool needsTwoPass = true;
    if (!queryTwoPass(source, target, needsTwoPass) || needsTwoPass) return false;
    if (!read(params + 0xf0, &alpha, 8)) return false;
    return !alpha || (alpha <= max - 7 && read(alpha + 4, &alphaMode, 4) && alphaMode != 2);
}

// Native CPU shell allocator437e0 zeroes storage and increments7f3218 only on
// success; release436d0 decrements the SAME counter then frees. Not GPU backing.
struct TglTrackedCpuBinding {
    uintptr_t image = 0, allocate = 0, release = 0;
    template<class Read, class Qualify, class Executable>
    bool resolve(uintptr_t base, Read read, Qualify qualify, Executable executable) {
        image = allocate = release = 0;
        constexpr std::array<uint8_t,16> allocBytes{
            0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x10,
            0x48,0x89,0x7d,0xf8,0x48,0x8b,0x7d,0xf8};
        constexpr std::array<uint8_t,16> freeBytes{
            0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x10,
            0x48,0x89,0x7d,0xf8,0x48,0x83,0x7d,0xf8};
        std::array<uint8_t,16> a{}, f{};
        if (!base || base > std::numeric_limits<uintptr_t>::max() - 0x757e80 ||
            !qualify(base) || !executable(base+0x437e0,16) || !executable(base+0x436d0,16) ||
            !read(base+0x437e0,a.data(),16) || a != allocBytes ||
            !read(base+0x436d0,f.data(),16) || f != freeBytes) return false;
        image = base; allocate = base+0x437e0; release = base+0x436d0; return true;
    }
};

// Invoker ABI: uintptr_t allocate(entry,size_t), void release(entry,pointer).
// Caller retains the loaded image lease until backend and all owners are gone.
struct TglNativeTrackedCpuInvoker {
    uintptr_t allocate(uintptr_t entry, size_t bytes) const {
        if (!entry || !bytes) return 0;
        using Allocate = void* (*)(size_t);
        return reinterpret_cast<uintptr_t>(reinterpret_cast<Allocate>(entry)(bytes));
    }
    void release(uintptr_t entry, uintptr_t pointer) const noexcept {
        if (!entry || !pointer) return;
        using Release = void (*)(void*);
        reinterpret_cast<Release>(entry)(reinterpret_cast<void*>(pointer));
    }
};

template<class Invoker> class TglTrackedCpuBackend {
    TglTrackedCpuBinding binding;
    Invoker invoke;
public:
    template<class Read, class Qualify, class Executable>
    TglTrackedCpuBackend(uintptr_t image, Read read, Qualify qualify,
                         Executable executable, Invoker invoker) : invoke(std::move(invoker)) {
        binding.resolve(image,read,qualify,executable);
    }
    bool bound() const { return binding.image != 0; }
    uintptr_t image() const noexcept { return binding.image; }
    uintptr_t allocate(size_t bytes) {
        return bound() && bytes ? invoke.allocate(binding.allocate,bytes) : 0;
    }
    void release(uintptr_t pointer) noexcept {
        if (bound() && pointer) invoke.release(binding.release,pointer);
    }
};

// Backend must own the authenticated binding/image lease, call native paired
// allocate(size_t)/release(pointer), and outlive this non-transferable owner.
// Never adopt arbitrary pointers: that would corrupt native allocation accounting.
template<class Backend> class TglTrackedCpuOwner {
    Backend& backend;
    uintptr_t storage = 0;
    size_t extent = 0;
public:
    explicit TglTrackedCpuOwner(Backend& b) : backend(b) {}
    TglTrackedCpuOwner(const TglTrackedCpuOwner&) = delete;
    TglTrackedCpuOwner& operator=(const TglTrackedCpuOwner&) = delete;
    ~TglTrackedCpuOwner() { reset(); }
    bool allocate(size_t bytes) {
        if (storage || !bytes) return false;
        auto candidate = backend.allocate(bytes);
        if (!candidate) return false;
        storage = candidate; extent = bytes; return true;
    }
    void reset() noexcept {
        if (!storage) return;
        const auto old = storage; storage = 0; extent = 0;
        backend.release(old);
    }
    uintptr_t get() const { return storage; }
    size_t size() const { return extent; }
};

// Native Allocate126740 creates eight 0x2a8 CPU shells: 1b0/1b8,
// ba8[2],680[4]. This owned transaction publishes none until all exist.
// It does not install child fields or allocate GPU backing. Backend outlives it.
template<class Backend> class TglVeboxCpuShells {
    Backend& backend;
    std::array<uintptr_t,8> shells{};
public:
    explicit TglVeboxCpuShells(Backend& b) : backend(b) {}
    TglVeboxCpuShells(const TglVeboxCpuShells&) = delete;
    TglVeboxCpuShells& operator=(const TglVeboxCpuShells&) = delete;
    ~TglVeboxCpuShells() { reset(); }
    bool allocate() {
        if (shells[0]) return false;
        std::array<uintptr_t,8> candidate{};
        for (size_t i = 0; i < candidate.size(); ++i) {
            candidate[i] = backend.allocate(0x2a8);
            if (!candidate[i]) {
                while (i) backend.release(candidate[--i]);
                return false;
            }
        }
        shells = candidate;
        return true;
    }
    uintptr_t get(size_t i) const { return i < shells.size() ? shells[i] : 0; }
    uintptr_t image() const noexcept { return backend.image(); }
    // Private native-base storage only, after successful native construction.
    // No observer may access it during installation. Validate every field first,
    // then publish all pointers and revoke this owner's release authority.
    template<size_t N>
    bool installIntoPrivateBase(std::array<uint8_t,N>& storage) noexcept {
        return installIntoPrivateBase(storage.data(),N);
    }
    bool installIntoPrivateBase(uint8_t* storage,size_t extent) noexcept {
        if (!storage || extent<0x1bc0 ||
            reinterpret_cast<uintptr_t>(storage)>UINTPTR_MAX-extent) return false;
        constexpr size_t offsets[8]={0x1b0,0x1b8,0xba8,0xbb0,0x680,0x688,0x690,0x698};
        for (size_t i=0;i<8;++i) {
            uintptr_t existing=0;
            std::memcpy(&existing,storage+offsets[i],8);
            if (existing || !shells[i]) return false;
        }
        for (size_t i=0;i<8;++i)
            std::memcpy(storage+offsets[i],&shells[i],8);
        shells.fill(0);
        return true;
    }
    // Only after installing ALL eight fields into a private constructed child:
    // native non-deleting destructor12de30 then owns their paired436d0 frees.
    // Never retain ownership here as well, or native teardown double-frees them.
    // Caller must hold the child/image lease and handle all returned ownership.
    std::array<uintptr_t,8> relinquishToNative() noexcept {
        const auto transferred = shells;
        shells.fill(0);
        return transferred;
    }
    void reset() noexcept {
        const auto old = shells;
        shells.fill(0);
        for (size_t i = old.size(); i; --i)
            if (old[i-1]) backend.release(old[i-1]);
    }
};

// Native destructor12dfc8 uses RenderHal+a30, NOT OS+a30. The borrowed
// RenderHal and image lease must remain live through the last batch release.
// Qualification is supplied by the owner that authenticated the parent link.
struct TglBatchReleaseBinding {
    uintptr_t renderHal = 0, entry = 0;
    template<class Read, class Qualify, class Executable>
    bool resolve(uintptr_t object, uintptr_t image, Read read,
                 Qualify qualify, Executable executable) {
        renderHal = entry = 0;
        constexpr auto max = std::numeric_limits<uintptr_t>::max();
        uintptr_t callback = 0;
        if (!object || object > max - 0xa37 || !image || image > max - 0x23e03e ||
            !qualify(object,image) || !read(object+0xa30,&callback,8) ||
            callback < image+0x1160 || callback > image + 0x23e03e - 16 ||
            !executable(callback,16)) return false;
        renderHal = object; entry = callback; return true;
    }
};

// Native MOS_STATUS result is preserved; caller owns batch descriptor/backing
// and decides teardown policy. This alone neither allocates nor owns a buffer.
struct TglNativeBatchReleaseInvoker {
    uint32_t operator()(uintptr_t entry, uintptr_t renderHal, uintptr_t batch) const {
        using Release = uint32_t (*)(void*,void*);
        return reinterpret_cast<Release>(entry)(reinterpret_cast<void*>(renderHal),
                                                reinterpret_cast<void*>(batch));
    }
};

// Full VEBOX table, NOT a live child/object replacement. Owned constructor and
// destructor implementations must exist before this table is installed anywhere.
// +1e8 belongs to the following execution table's metadata and MUST NOT be copied.
// +1b8 is the bool(this) Gen12 CSC-support query, not a status callback.
// Native base 71e80 returns false; the owned Gen12 implementation must expose
// true without touching child state. Other derived slots still require their
// own producers and are NOT supplied by this callback.
inline bool tglOwnedVeboxGen12CscSupported(void*) noexcept { return true; }
// Gen12 UseKernelResource (g12_base.cpp1968), independently Darwin1fa4c0:
// bool(this)==false selects the driver heap, not a missing-resource fallback.
// Heap allocation, completion tags and reuse remain mandatory and separate.
inline bool tglOwnedVeboxUseKernelResource(void*) noexcept { return false; }
// Darwin ICL1f9a40 writes exactly eight DWORDs (base slot1c8 via thunk1f9ab0).
// Pinned Gen12 GetLumaDefaultValue independently specifies these same values:
// ASD, history delta/max, STAD, SCM, motion pixels, LTD, TD. This is owned
// parameter data, not an ICL object, kernel payload or GPU command transplant.
struct TglOwnedDndiPackets {
    // TGL1284f0 clears these exact extents before calling slot1d0.
    // Native f8/128490 owns luma DWORD10/11 clearing; native100/1282d0
    // consumes the packet and publishes execution850. Keep all 12 DWORDs,
    // not just the eight default-noise fields supplied by slot1c8.
    std::array<uint32_t,12> luma{};
    std::array<uint32_t,9> chroma{};
};
static_assert(sizeof(TglOwnedDndiPackets::luma)==0x30);
static_assert(sizeof(TglOwnedDndiPackets::chroma)==0x24);
// Exact TGL MHW slot30/176030 reads execution850 through execution93c.
// This is CPU parameter storage, never the c4-byte packed hardware state.
struct TglOwnedDndiState {
    std::array<uint32_t,60> words{};
};
static_assert(sizeof(TglOwnedDndiState)==0xf0);
struct TglOwnedDiDimensions { uint32_t width,height; };
// Tahoe SetupVeboxKernel1fc150 clears SearchFilter and selected KernelEntry
// before returning31 (unsupported). TGL Initialize12e6ec independently loops
// eight entries, each58 bytes at a50, agreeing with Gen12's max-eight array.
// Preserve that native unsupported path; this does NOT implement an automatic
// denoise update kernel or make its call eligible. Both buffers must be owned,
// exclusively leased allocations, not merely readable native object pointers.
inline int tglClearOwnedVeboxKernelScratch(void* child,size_t childExtent,
    void* execution,size_t executionExtent,int32_t index) noexcept {
    if(!child || !execution || childExtent<0x1a8 || executionExtent<0xd10 ||
        index<0 || index>=8) return 5;
    const uintptr_t c=reinterpret_cast<uintptr_t>(child),e=reinterpret_cast<uintptr_t>(execution);
    if(c>UINTPTR_MAX-childExtent || e>UINTPTR_MAX-executionExtent ||
        (c<e+executionExtent && e<c+childExtent)) return 5;
    std::memset(static_cast<uint8_t*>(child)+0x118,0,0x90);
    std::memset(static_cast<uint8_t*>(execution)+0xa50+size_t(index)*0x58,0,0x58);
    return 31;
}
// Integer strength is already normalized by the caller. All 65 Gen12 table
// entries are represented exactly by these piecewise integer expressions;
// this avoids importing a Linux structure or an ICL binary lookup table.
inline void tglOwnedDndiSetDn(TglOwnedDndiState& state,TglOwnedDndiPackets& packets,
                              bool luma,bool chroma,bool automatic,
                              uint32_t strength) noexcept {
    const uint32_t f=strength>64 ? 64 : strength;
    if(luma) {
        const std::array<uint32_t,8> values=automatic ?
            std::array<uint32_t,8>{512,8,208,2048,512,2,128,192} :
            std::array<uint32_t,8>{512+2*f,4+f/16,144+f,2048+4*f,
                                    512+2*f,f/32,64+f,128+f};
        std::copy(values.begin(),values.end(),packets.luma.begin());
        const uint32_t lo=f<32 ? f : 32,hi=f>32 ? f-32 : 0;
        const std::array<uint32_t,6> thresholds=automatic ?
            std::array<uint32_t,6>{192,256,512,640,896,1280} :
            std::array<uint32_t,6>{32+5*lo+6*hi,64+6*lo+10*hi,128+12*f,
                                    128+16*lo+20*hi,128+24*lo+32*hi,128+36*lo+40*hi};
        const std::array<uint32_t,6> weights=automatic ?
            std::array<uint32_t,6>{16,14,10,5,2,1} :
            std::array<uint32_t,6>{16,f<32 ? 9+5*f/32 : 14+f/64,
                f<32 ? 2+f/4 : 10+3*(f-32)/32,5*f/32,
                f<32 ? f/16 : 2+5*(f-32)/32,f<32 ? 0 : 1+3*(f-32)/32};
        std::copy(thresholds.begin(),thresholds.end(),state.words.begin()+12);
        std::copy(weights.begin(),weights.end(),state.words.begin()+18);
    }
    if(chroma) {
        packets.chroma[1]=8;packets.chroma[2]=192;
        if(!automatic) {
            packets.chroma[3]=packets.chroma[4]=2048+4*f;
            packets.chroma[5]=packets.chroma[6]=64+f;
            packets.chroma[7]=packets.chroma[8]=160+f;
        }
    }
}
// Gen12 DI defaults agree with Darwin1f9eb0 and TGL176030's field reads.
// Preserve reserved bytes/words. Like native, a missing enabled source is
// rejected AFTER fixed defaults, before the resolution-dependent LUT writes.
// Not a slot1d0 callback: frame scope and native execution publication remain
// separate integration requirements.
inline int tglOwnedDndiSetDi(TglOwnedDndiState& state,bool enabled,
                             const TglOwnedDiDimensions* source) noexcept {
    if(!enabled) return 0;
    auto& w=state.words;
    constexpr std::array<uint32_t,6> temporal{4,0,5,255,5,255};
    std::memcpy(w.data()+0x1f,temporal.data(),sizeof(temporal));
    w[0x25]&=0xff000000u; // three individual bool bytes; fourth is reserved
    w[0x26]=0;w[0x27]=0;
    w[0x28]=0x00010001u;
    constexpr std::array<uint32_t,10> checks{3,20,100,15,0,63,76,89,114,217};
    std::memcpy(w.data()+0x2a,checks.data(),sizeof(checks));
    if(!source) return 5;
    constexpr std::array<uint32_t,8> sd{0,0,0,128,128,128,255,255};
    constexpr std::array<uint32_t,8> hd{0,0,0,0,32,64,128,255};
    const auto& lut=source->width<=768 && source->height<=576 ? sd : hd;
    std::memcpy(w.data()+0x34,lut.data(),sizeof(lut));
    return 0;
}
// Exact TGL slot f8/128490: only these two trailing DWORDs are cleared.
// This packet operation does not publish execution state or encode GPU commands.
inline void tglOwnedDndiClearNativeTail(TglOwnedDndiPackets& packets) noexcept {
    packets.luma[10]=0;
    packets.luma[11]=0;
}
inline void tglOwnedVeboxLumaDefaults(void* child,uint32_t* output) noexcept {
    if(!child || !output) return;
    constexpr std::array<uint32_t,8> values{512,8,208,2048,512,2,128,192};
    std::memcpy(output,values.data(),sizeof(values));
}
inline int tglStatisticsQuery(uint32_t selector,uint32_t& value) noexcept;
// Native1295fd invokes slot80 as status(this, selector, DWORD* output).
// Output is owned by the native caller (stack local at1295f9), not a user
// resource descriptor. Unsupported queries preserve it exactly.
inline int tglOwnedVeboxStatisticsQuery(void* child,uint32_t selector,
                                       uint32_t* output) noexcept {
    if(!child || !output) return 5;
    uint32_t candidate=0;
    const int status=tglStatisticsQuery(selector,candidate);
    if(!status) *output=candidate;
    return status;
}

// Centralize the proven TGL slots. The other twelve entries remain the
// caller's derived lifecycle/state/resource producers, not fake fallbacks.
template<class Read>
std::array<uintptr_t,20> tglBindOwnedVeboxQueryHooks(
    std::array<uintptr_t,20> hooks) noexcept {
    using Formats=TglOwnedVeboxFormatCallbacks<Read>;
    hooks[8]=reinterpret_cast<uintptr_t>(&tglOwnedVeboxStatisticsQuery);
    hooks[9]=reinterpret_cast<uintptr_t>(&tglOwnedVeboxSurfaceSupported<Read>);
    hooks[10]=reinterpret_cast<uintptr_t>(&Formats::renderTarget);
    hooks[11]=reinterpret_cast<uintptr_t>(&Formats::denoise);
    hooks[12]=reinterpret_cast<uintptr_t>(&Formats::deinterlace);
    hooks[13]=reinterpret_cast<uintptr_t>(&tglOwnedVeboxUseKernelResource);
    hooks[15]=reinterpret_cast<uintptr_t>(&tglOwnedVeboxGen12CscSupported);
    hooks[16]=reinterpret_cast<uintptr_t>(&tglOwnedVeboxLumaDefaults);
    return hooks;
}

struct TglOwnedVeboxVtable {
    TglOwnedVeboxVtable()=default;
    // Native child stores addressPoint(), not a copy of the entries. Keep
    // the table address stable for the entire child/fence lifetime.
    TglOwnedVeboxVtable(const TglOwnedVeboxVtable&)=delete;
    TglOwnedVeboxVtable& operator=(const TglOwnedVeboxVtable&)=delete;
    TglOwnedVeboxVtable(TglOwnedVeboxVtable&&)=delete;
    TglOwnedVeboxVtable& operator=(TglOwnedVeboxVtable&&)=delete;
    static constexpr size_t count = 0x1e8 / 8;
    static constexpr std::array<size_t,17> missing{
        0x28,0x58,0x60,0x68,0x70,0x78,0x80,0x90,0x98,0xa0,0xa8,0xb0,0xb8,
        0x1c8,0x1d0,0x1d8,0x1e0};
    // Lifecycle slots cannot inherit native delete: owned resources/table need
    // teardown. +1b8 must implement Gen12 TRUE rather than base FALSE.
    static constexpr std::array<size_t,20> required{
        0,8,0x28,0x58,0x60,0x68,0x70,0x78,0x80,0x90,0x98,0xa0,0xa8,0xb0,0xb8,
        0x1b8,0x1c8,0x1d0,0x1d8,0x1e0};
    // Preserve the primary native-base ABI metadata. This does not invent
    // derived RTTI or prove a constructed object; owners must retain its lease.
    intptr_t offsetToTop = 0;
    uintptr_t typeInfo = 0;
    std::array<uintptr_t,count> entries{};
    uintptr_t addressPoint() const noexcept {
        return typeInfo ? reinterpret_cast<uintptr_t>(entries.data()) : 0;
    }
    template<class Read, class Qualify, class Executable>
    bool build(uintptr_t image, const std::array<uintptr_t,20>& owned,
               Read read, Qualify qualify, Executable executable) {
        offsetToTop = 0; typeInfo = 0; entries = {};
        constexpr auto max = std::numeric_limits<uintptr_t>::max();
        if (!image || image > max - 0x757ed0 || !qualify(image)) return false;
        std::array<uintptr_t,2> metadata{};
        if (!read(image+0x757c88,metadata.data(),sizeof(metadata)) ||
            metadata[0] != 0 || metadata[1] != image+0x757ed0) return false;
        std::array<uintptr_t,count> candidate{};
        if (!read(image + 0x757c98, candidate.data(), sizeof(candidate))) return false;
        for (size_t i = 0; i < count; ++i) {
            bool mustBeNull = false;
            for (size_t offset : missing) mustBeNull |= i * 8 == offset;
            if (mustBeNull != (candidate[i] == 0)) return false;
            if (candidate[i] && (candidate[i] < image+0x1160 ||
                candidate[i] - image >= 0x23e03f || !executable(candidate[i], 1))) return false;
        }
        if (candidate[0] != image+0x12e240 || candidate[1] != image+0x12e250 ||
            candidate[0x40/8] != image+0x12e9c0 || candidate[0x48/8] != image+0x12ea30 ||
            candidate[0xd0/8] != image+0x71e80 ||
            candidate[0x1b8/8] != image+0x71e80) return false;
        for (size_t i = 0; i < required.size(); ++i) {
            // Owned replacements are trusted compiled addresses, not pointers
            // discovered in an object. Never substitute another native callback.
            if (!owned[i] || (owned[i] >= image && owned[i] - image < 0x23e03f) ||
                !executable(owned[i], 1)) return false;
            candidate[required[i]/8] = owned[i];
        }
        entries = candidate; typeInfo = metadata[1]; return true;
    }
};
static_assert(offsetof(TglOwnedVeboxVtable, entries) == 16);
static_assert(sizeof(TglOwnedVeboxVtable) == 16 + 0x1e8);

// Authenticated native CSC entry. This binds code only: invocation also requires
// a fully constructed owned child with native execution and Gen12 overrides.
struct TglTwoPassCscBinding {
    uintptr_t image = 0, entry = 0;
    template<class Read, class Qualify, class Executable>
    bool resolve(uintptr_t base, Read read, Qualify qualify, Executable executable) {
        image = entry = 0;
        constexpr std::array<uint8_t,16> anchor{
            0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x40,
            0x48,0x89,0x7d,0xf8,0x48,0x89,0x75,0xf0};
        std::array<uint8_t,16> bytes{};
        if (!base || base > std::numeric_limits<uintptr_t>::max() - 0x757fd0 ||
            !qualify(base) || !executable(base + 0x12cfc0, anchor.size()) ||
            !read(base + 0x12cfc0, bytes.data(), bytes.size()) || bytes != anchor) return false;
        image = base; entry = base + 0x12cfc0; return true;
    }
};

// ICL1fbfd1..1fc0f4: after matrix production, source formats 1/2
// exchange the first/third coefficient of each row. Integer storage retains
// exact float bits (including NaN payloads); offsets/other columns unchanged.
// This is only the owned coefficient transform, not a complete +b8 producer.
inline void tglOwnedVeboxSwapCscRedBlue(std::array<uint32_t,9>& coefficients,
                                      uint32_t sourceFormat) noexcept {
    if(sourceFormat!=1 && sourceFormat!=2) return;
    for(size_t row=0;row<3;++row) {
        const uint32_t first=coefficients[row*3];
        coefficients[row*3]=coefficients[row*3+2];
        coefficients[row*3+2]=first;
    }
}

// Exact TGL three-argument CSC matrix producer, shared by native fill conversion.
struct TglCscMatrixBinding {
    uintptr_t image=0,entry=0;
    template<class Read,class Qualify,class Executable>
    bool resolve(uintptr_t base,Read read,Qualify qualify,Executable executable) {
        image=entry=0;
        constexpr std::array<uint8_t,35> anchor{
            0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x20,0x89,0x7d,0xfc,
            0x89,0x75,0xf8,0x48,0x89,0x55,0xf0,0xc6,0x45,0xef,0,
            0x8b,0x7d,0xfc,0xbe,0xff,0xff,0xff,0xff,0xe8,0xfd,0xe9,0xff,0xff};
        auto bytes=anchor;
        if(!base || base>UINTPTR_MAX-0x757fd0 || !qualify(base) ||
            !executable(base+0x54950,anchor.size()) ||
            !read(base+0x54950,bytes.data(),bytes.size()) || bytes!=anchor) return false;
        image=base;entry=base+0x54950;return true;
    }
};
struct TglCscCoefficients {
    std::array<float,9> matrix{};
    std::array<float,3> inputOffsets{},outputOffsets{};
};
// TGL base constructor12d619/12d65e independently fixes 9+3+3 floats
// at a0/c4/d0. Only a caller holding the owned base lease may publish.
inline bool tglPublishOwnedCscFields(void* child,size_t extent,
                                    const TglCscCoefficients& input) noexcept {
    const auto address=reinterpret_cast<uintptr_t>(child);
    if(!address || extent<0xdc || address>UINTPTR_MAX-extent) return false;
    const TglCscCoefficients candidate=input;
    auto* bytes=static_cast<uint8_t*>(child);
    std::memcpy(bytes+0xa0,candidate.matrix.data(),0x24);
    std::memcpy(bytes+0xc4,candidate.inputOffsets.data(),0xc);
    std::memcpy(bytes+0xd0,candidate.outputOffsets.data(),0xc);
    return true;
}
struct TglCscCoefficientsBinding {
    uintptr_t image=0,entry=0;
    template<class Read,class Qualify,class Executable>
    bool resolve(uintptr_t base,Read read,Qualify qualify,Executable executable) {
        image=entry=0;
        constexpr std::array<uint8_t,15> prologue{
            0x55,0x48,0x89,0xe5,0x48,0x81,0xec,0x80,0,0,0,0x48,0x8d,0x45,0xc0};
        constexpr std::array<uint8_t,5> matrixCall{0xe8,0x53,0x6e,0,0};
        if(!base || base>UINTPTR_MAX-0x757fd0 || !qualify(base)) return false;
        auto matches=[&](uintptr_t offset,const auto& anchor) {
            auto bytes=anchor;
            return executable(base+offset,bytes.size()) &&
                read(base+offset,bytes.data(),bytes.size()) && bytes==anchor;
        };
        if(!matches(0x4dac0,prologue) || !matches(0x4daf8,matrixCall)) return false;
        image=base;entry=base+0x4dac0;return true;
    }
};
// Native void(from,to,matrix9,input3,output3), not a status-returning ABI.
// Return true here reports authenticated invocation only, never GPU success.
template<class Read,class Qualify,class Executable,class Invoke,class MatrixInvoke>
bool tglProduceNativeCscCoefficients(const TglCscCoefficientsBinding& binding,
    TglCscCoefficients& output,uint32_t from,uint32_t to,
    Read read,Qualify qualify,Executable executable,Invoke invoke,MatrixInvoke matrixInvoke) {
    TglCscCoefficientsBinding current;
    if(!current.resolve(binding.image,read,qualify,executable) ||
        current.entry!=binding.entry) return false;
    // The native void wrapper ignores its internal bool and copies stack
    // coefficients even on unsupported conversion. Authenticate and query
    // that same pure producer first; never fabricate successful coefficients.
    TglCscMatrixBinding matrix;
    if(!matrix.resolve(binding.image,read,qualify,executable)) return false;
    std::array<float,12> validation{};
    if(!matrixInvoke(matrix.entry,from,to,validation.data())) return false;
    TglCscCoefficients candidate;
    invoke(current.entry,from,to,candidate.matrix.data(),
        candidate.inputOffsets.data(),candidate.outputOffsets.data());
    output=candidate;return true;
}
// Complete owned CPU candidate for +b8. Child-field publication is separate:
// its base-relative layout must be authenticated, not copied from ICL RTTI.
template<class Read,class Qualify,class Executable,class Invoke,class MatrixInvoke>
bool tglPrepareOwnedVeboxCsc(const TglCscCoefficientsBinding& binding,
    TglCscCoefficients& output,uint32_t from,uint32_t to,uint32_t sourceFormat,
    Read read,Qualify qualify,Executable executable,Invoke invoke,MatrixInvoke matrixInvoke) {
    TglCscCoefficients candidate;
    if(!tglProduceNativeCscCoefficients(binding,candidate,from,to,
        read,qualify,executable,invoke,matrixInvoke)) return false;
    std::array<uint32_t,9> bits{};
    std::memcpy(bits.data(),candidate.matrix.data(),sizeof(bits));
    tglOwnedVeboxSwapCscRedBlue(bits,sourceFormat);
    std::memcpy(candidate.matrix.data(),bits.data(),sizeof(bits));
    output=candidate;return true;
}
// Native bool(uint32_t,uint32_t,float[12]); retain the loaded image lease.
// Matrix layout is three rows of four floats, not the child's packed 3x3.
template<class Read,class Qualify,class Executable,class Invoke>
bool tglProduceNativeCscMatrix(const TglCscMatrixBinding& binding,
    std::array<uint32_t,12>& output,uint32_t from,uint32_t to,
    Read read,Qualify qualify,Executable executable,Invoke invoke) {
    TglCscMatrixBinding current;
    if(!current.resolve(binding.image,read,qualify,executable) ||
        current.entry!=binding.entry) return false;
    std::array<float,12> candidate{};
    if(!invoke(current.entry,from,to,candidate.data())) return false;
    static_assert(sizeof(candidate)==sizeof(output),"CSC matrix ABI");
    std::memcpy(output.data(),candidate.data(),sizeof(candidate));
    return true;
}

struct TglNativeCscInvoker {
    bool matrix(uintptr_t entry,uint32_t from,uint32_t to,float* output) const noexcept {
        return reinterpret_cast<bool(*)(uint32_t,uint32_t,float*)>(entry)(from,to,output);
    }
    void coefficients(uintptr_t entry,uint32_t from,uint32_t to,
        float* matrix,float* input,float* output) const noexcept {
        reinterpret_cast<void(*)(uint32_t,uint32_t,float*,float*,float*)>(entry)
            (from,to,matrix,input,output);
    }
};
// Image lifetime is retained by the enclosing renderer. Every production
// revalidates identity/RX/anchors; no native child or GPU context is adopted.
template<class Read,class Qualify,class Executable,class Invoke=TglNativeCscInvoker>
class TglOwnedCscProducer {
    TglCscCoefficientsBinding binding_;
    Read read_;Qualify qualify_;Executable executable_;Invoke invoke_;
public:
    TglOwnedCscProducer(uintptr_t image,Read read,Qualify qualify,Executable executable,
        Invoke invoke=Invoke{}) : read_(read),qualify_(qualify),executable_(executable),invoke_(invoke) {
        (void)binding_.resolve(image,read_,qualify_,executable_);
    }
    bool operator()(TglCscCoefficients& output,uint32_t from,uint32_t to,uint32_t format) noexcept {
        return tglPrepareOwnedVeboxCsc(binding_,output,from,to,format,read_,qualify_,executable_,
            [&](uintptr_t entry,uint32_t a,uint32_t b,float* matrix,float* input,float* out) noexcept {
                invoke_.coefficients(entry,a,b,matrix,input,out);
            },[&](uintptr_t entry,uint32_t a,uint32_t b,float* out) noexcept {
                return invoke_.matrix(entry,a,b,out);
            });
    }
};

// Four-argument native fill converter: output, source, source CS, target CS.
// Resolution is not execution proof; the caller retains the authenticated image.
struct TglFillCscBinding {
    uintptr_t image = 0, entry = 0;
    template<class Read, class Qualify, class Executable>
    bool resolve(uintptr_t base, Read read, Qualify qualify, Executable executable) {
        image = entry = 0;
        constexpr std::array<uint8_t,16> prologue{
            0x55,0x48,0x89,0xe5,0x48,0x81,0xec,0xb0,
            0x00,0x00,0x00,0x48,0x8d,0x45,0xc0,0x45};
        constexpr std::array<uint8_t,5> matrix{0xe8,0x4f,0x6f,0,0};
        constexpr std::array<uint8_t,5> convert{0xe8,0xf7,0xf6,0xff,0xff};
        if (!base || base > std::numeric_limits<uintptr_t>::max()-0x757fd0 ||
            !qualify(base)) return false;
        auto matches = [&](uintptr_t offset, const auto& anchor) {
            auto bytes = anchor;
            return executable(base+offset,anchor.size()) &&
                read(base+offset,bytes.data(),bytes.size()) && bytes == anchor;
        };
        if (!matches(0x4d970,prologue) || !matches(0x4d9fc,matrix) ||
            !matches(0x4da74,convert)) return false;
        image = base; entry = base+0x4d970; return true;
    }
};

// Native writes are confined to a private candidate. A false native result or
// stale image binding cannot publish partially converted fill data.
template<class Read, class Qualify, class Executable, class Invoke>
bool tglConvertNativeFill(const TglFillCscBinding& binding,
    std::array<uint8_t,4>& output, const std::array<uint8_t,4>& source,
    uint32_t sourceColorSpace, uint32_t targetColorSpace,
    Read read, Qualify qualify, Executable executable, Invoke invoke) {
    TglFillCscBinding current;
    if (!current.resolve(binding.image,read,qualify,executable) ||
        binding.entry != current.entry) return false;
    std::array<uint8_t,4> candidate{};
    if (!invoke(current.entry,candidate.data(),source.data(),
                sourceColorSpace,targetColorSpace)) return false;
    output = candidate;
    return true;
}

// Read-only shape gate for an already-owned native execution object. Identity
// qualification must authenticate the loaded image; this is NOT proof that
// mode/state producers or GPU commands are valid. Run under owner's lock.
template<class Read, class Qualify>
bool tglExecutionShape(uintptr_t object, uintptr_t image, Read read, Qualify qualify) {
    constexpr auto max = std::numeric_limits<uintptr_t>::max();
    if (!object || !image || object > max - 0xd48 || image > max - 0x757fd0 ||
        !qualify(image)) return false;
    uintptr_t vt = 0, first = 0, second = 0;
    if (!read(object, &vt, 8) || vt != image + 0x757e90 ||
        !read(object + 0xd38, &first, 8) || !read(object + 0xd40, &second, 8) ||
        !first || !second || first == second || first == object || second == object ||
        first > max - 0x20 || second > max - 0x48) return false;
    uintptr_t firstVt = 0, secondVt = 0;
    return read(first, &firstVt, 8) && firstVt == image + 0x757f78 &&
           read(second, &secondVt, 8) && secondVt == image + 0x757fb8;
}

// trustedVtable/genericHook/cscSupportHook are addresses in our owned subclass,
// never values learned from an untrusted native object. Hold its owner lock and
// image lease throughout. Invoke ABI: bool entry(child, source, target).
template<class Read, class Qualify, class Executable, class Invoke>
bool tglQueryNativeTwoPassCsc(const TglTwoPassCscBinding& binding,
                            uintptr_t child, uintptr_t source, uintptr_t target,
                            uintptr_t trustedVtable, uintptr_t genericHook,
                            uintptr_t cscSupportHook, bool& needed,
                            Read read, Qualify qualify, Executable executable, Invoke invoke) {
    needed = true; // failed query must not admit direct output
    constexpr auto max = std::numeric_limits<uintptr_t>::max();
    if (!child || child > max - 0x8f || !source || !target ||
        source > max - 0x133 || target > max - 3 ||
        !trustedVtable || trustedVtable > max - 0x1bf || !genericHook || !cscSupportHook ||
        genericHook == cscSupportHook || !binding.image ||
        binding.image > max - 0x757fd0 || binding.entry != binding.image + 0x12cfc0 ||
        trustedVtable == binding.image + 0x757c98) return false;
    TglTwoPassCscBinding verified;
    if (!verified.resolve(binding.image, read, qualify, executable)) return false;
    uintptr_t vt = 0, getter = 0, generic = 0, support = 0, execution = 0;
    uint32_t sourceColor = 0, targetColor = 0;
    if (!read(child, &vt, 8) || vt != trustedVtable ||
        !read(vt + 0x48, &getter, 8) || getter != binding.image + 0x12ea30 ||
        !read(vt + 0x90, &generic, 8) || generic != genericHook ||
        !read(vt + 0x1b8, &support, 8) || support != cscSupportHook ||
        !executable(genericHook, 1) || !executable(cscSupportHook, 1) ||
        !read(child + 0x88, &execution, 8) ||
        !tglExecutionShape(execution, binding.image, read, qualify) ||
        !read(source, &sourceColor, 4) || !read(target, &targetColor, 4)) return false;
    needed = invoke(verified.entry, child, source, target);
    return true;
}

struct TglExecutionBinding {
    uintptr_t image = 0, create = 0, initialize = 0, destroy = 0;
    // Caller retains the authenticated image for the entire owner lifetime.
    template<class Read, class Qualify, class Executable>
    bool resolve(uintptr_t base, Read read, Qualify qualify, Executable executable) {
        *this = {};
        constexpr std::array<uintptr_t, 3> offsets{0x12ebc0, 0x12e420, 0x12e3f0};
        constexpr std::array<std::array<uint8_t, 16>, 3> anchors{{
            {0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x40,0x48,0x8b,0x35,0x59,0x54,0x62,0x00,0xbf},
            {0x55,0x48,0x89,0xe5,0x48,0x81,0xec,0xb0,0x09,0x00,0x00,0x48,0x8b,0x05,0x56,0x5c},
            {0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x10,0x48,0x89,0x7d,0xf8,0x48,0x8b,0x45,0xf8}
        }};
        if (!base || base > std::numeric_limits<uintptr_t>::max() - 0x757fd0 ||
            !qualify(base)) return false;
        for (size_t i = 0; i < offsets.size(); ++i) {
            std::array<uint8_t, 16> bytes{};
            if (!executable(base + offsets[i], bytes.size()) ||
                !read(base + offsets[i], bytes.data(), bytes.size()) || bytes != anchors[i])
                return false;
        }
        uint32_t allocationSize=0;
        if(!executable(base+0x12ebd0,4) ||
            !read(base+0x12ebd0,&allocationSize,4) || allocationSize!=0xd48) return false;
        image = base;
        create = base + offsets[0]; initialize = base + offsets[1]; destroy = base + offsets[2];
        return true;
    }
};

// Invoker is injected for offline tests. Native invoker must use the exact
// x86_64 signatures: uintptr_t factory(), int Init(uintptr_t), void delete(uintptr_t).
// Binding/image lease must remain valid until every owned object is destroyed.
struct TglNativeExecutionInvoker {
    uintptr_t create(uintptr_t entry) const noexcept {
        return reinterpret_cast<uintptr_t(*)()>(entry)();
    }
    int initialize(uintptr_t entry,uintptr_t object) const noexcept {
        return reinterpret_cast<int(*)(uintptr_t)>(entry)(object);
    }
    void destroy(uintptr_t entry,uintptr_t object) const noexcept {
        reinterpret_cast<void(*)(uintptr_t)>(entry)(object);
    }
    int commitDndi(uintptr_t object,const TglOwnedDndiState& state) const noexcept {
        // Factory12ebcf allocates d48 bytes. Owner retains that allocation and
        // serializes access: this cannot adopt an arbitrary readable pointer.
        // Ordinary owned heap memcpy, not a partially failing VM write API;
        // not hardware-atomic and not usable without the enclosing owner lease.
        if(!object || object>UINTPTR_MAX-0xd48) return 5;
        std::memcpy(reinterpret_cast<void*>(object+0x850),&state,sizeof(state));
        return 0;
    }
    int clearKernelScratch(uintptr_t object,void* child,size_t childExtent,
                           int32_t index) const noexcept {
        return tglClearOwnedVeboxKernelScratch(child,childExtent,
            reinterpret_cast<void*>(object),0xd48,index);
    }
};
template<class Read, class Qualify, class Invoker> class TglExecutionBackend {
    TglExecutionBinding binding;
    Read read;
    Qualify qualify;
    Invoker invoke;
public:
    template<class Executable>
    TglExecutionBackend(TglExecutionBinding b, Read r, Qualify q, Invoker i, Executable rx)
        : binding(b), read(r), qualify(q), invoke(i) {
        TglExecutionBinding verified;
        if (!bound() || !verified.resolve(b.image, read, qualify, rx)) binding = {};
        else binding = verified;
    }
    bool bound() const {
        return binding.image && binding.image <= std::numeric_limits<uintptr_t>::max() - 0x757fd0 &&
            binding.create == binding.image + 0x12ebc0 &&
            binding.initialize == binding.image + 0x12e420 &&
            binding.destroy == binding.image + 0x12e3f0 && qualify(binding.image);
    }
    uintptr_t create() noexcept {
        return bound() ? invoke.create(binding.create) : 0;
    }
    int initialize(uintptr_t object) noexcept {
        return bound() && object ? invoke.initialize(binding.initialize, object) : 5;
    }
    bool validate(uintptr_t object) noexcept {
        return bound() && tglExecutionShape(object, binding.image, read, qualify);
    }
    int commitDndi(uintptr_t object,const TglOwnedDndiState& state) noexcept {
        return validate(object) ? invoke.commitDndi(object,state) : 5;
    }
    int clearKernelScratch(uintptr_t object,void* child,size_t childExtent,
                           int32_t index) noexcept {
        return validate(object) ? invoke.clearKernelScratch(object,child,childExtent,index) : 5;
    }
    void destroy(uintptr_t object) noexcept {
        // Authenticated lease is a lifetime requirement, not reacquired here.
        if (object && binding.destroy) invoke.destroy(binding.destroy, object);
    }
};

// Integration must bind Create to the constructor factory12ebc0, NOT
// child factory12e9c0 (which discards Initialize errors). Backend validation
// must authenticate the image/ABI and nested d38/d40 objects. Their mere
// presence is necessary, not sufficient. Backend must outlive this owner.
template<class Backend> class TglExecutionOwner {
    Backend& backend;
    uintptr_t object = 0;
    bool leased_=false;
public:
    explicit TglExecutionOwner(Backend& b) noexcept : backend(b) {}
    TglExecutionOwner(const TglExecutionOwner&) = delete;
    TglExecutionOwner& operator=(const TglExecutionOwner&) = delete;
    ~TglExecutionOwner() { reset(); }
    void reset() noexcept {
        if(leased_) return;
        const auto old = object;
        object = 0;
        if (old) backend.destroy(old);
    }
    int ensure() noexcept {
        if(leased_) return 5;
        if (object) {
            if (backend.validate(object)) return 0;
            reset();
            return 5;
        }
        const auto candidate = backend.create();
        if (!candidate) return 1;
        const int result = backend.initialize(candidate);
        if (result || !backend.validate(candidate)) {
            backend.destroy(candidate);
            return result ? result : 5;
        }
        object = candidate; // publish only after native result AND validation
        return 0;
    }
    // Eligibility initializes execution on each admitted pass. ensure() alone
    // only supplies lifetime ownership; it must not preserve stale frame flags.
    // A newly created object is already initialized by ensure(), exactly once.
    int preparePass() noexcept {
        if(leased_) return 5;
        if (!object) return ensure();
        if (!backend.validate(object)) { reset(); return 5; }
        const int result = backend.initialize(object);
        if (result || !backend.validate(object)) {
            reset();
            return result ? result : 5;
        }
        return 0;
    }
    uintptr_t get() const noexcept { return object; }
    bool hasExecutionLease(uintptr_t expected) const noexcept {
        return leased_ && object && object==expected;
    }
    // Synchronous execution-data publication must retain the native object.
    // Reject reset/reinitialize/nested use during the lease. This pins CPU
    // lifetime only: callers still own serialization and GPU fence lifetimes.
    template<class Use> int withExecution(Use use) noexcept {
        static_assert(noexcept(use(uintptr_t{})),"execution lease status ABI");
        if(leased_ || !object) return 5;
        leased_=true;
        if(!backend.validate(object)) {
            leased_=false;reset();return 5;
        }
        const int status=use(object);
        leased_=false;
        return status;
    }
    int commitDndi(uintptr_t address,const TglOwnedDndiState& state) noexcept {
        if(!leased_ || !object || object>UINTPTR_MAX-0xd48 || address!=object+0x850) return 5;
        return backend.commitDndi(object,state);
    }
    int clearKernelScratch(void* child,size_t childExtent,int32_t index) noexcept {
        if(!hasExecutionLease(object)) return 5;
        return backend.clearKernelScratch(object,child,childExtent,index);
    }
};

// These are owned frame inputs, NOT a reinterpret_cast of native source+90.
// Frame admission must authenticate their origin and retain the source lease.
struct TglOwnedDndiSource {
    uintptr_t identity=0;
    TglOwnedDiDimensions dimensions{};
    bool automatic=false;
    uint32_t strength=0;
};
// Darwin1f9c07 performs CVTTSS2SI r64, keeps low32, then unsigned-clamps64.
// Model the invalid-conversion indefinite value without C++ float-cast UB.
inline uint32_t tglOwnedDnStrength(float value) noexcept {
    const double d=value;
    if(!std::isfinite(d) || d>=0x1p63 || d< -0x1p63) return 0;
    const uint32_t low=static_cast<uint32_t>(static_cast<int64_t>(d));
    return low>64 ? 64 : low;
}
// CPU publication under the native execution lifetime lease. Commit must be
// the serialized allocation owner's all-or-nothing write of this f0 extent;
// a generic mach_vm_write with potentially partial failure is NOT sufficient.
// Native f8/100 still own subsequent luma-tail/flag/execution850 publication.
// No command, resource adoption or asynchronous lifetime is implied here.
template<class Read,class Commit>
int tglPublishLeasedDndiData(uintptr_t execution,uintptr_t child,uintptr_t callbackSource,
    const TglOwnedDndiSource& source,TglOwnedDndiPackets& output,
    Read read,Commit commit) noexcept {
    static_assert(noexcept(commit(uintptr_t{},std::declval<const TglOwnedDndiState&>())),
        "serialized execution publication status ABI");
    if(!child || !source.identity || callbackSource!=source.identity ||
        child>std::numeric_limits<uintptr_t>::max()-0x90) return 5;
    if(!execution) return 5;
        uintptr_t linked=0;
        if(execution>std::numeric_limits<uintptr_t>::max()-0x940 ||
            !read(child+0x88,&linked,sizeof(linked)) || linked!=execution) return 5;
        std::array<uint8_t,0x14> flags{};
        TglOwnedDndiState state;
        if(!read(execution,flags.data(),flags.size()) ||
            !read(execution+0x850,&state,sizeof(state))) return 5;
        auto candidate=output;
        tglOwnedDndiSetDn(state,candidate,flags[0xb]&1,flags[0xc]&1,
            source.automatic,source.strength);
        const int status=tglOwnedDndiSetDi(state,flags[0x13]&1,&source.dimensions);
        if(status) return status;
        const int published=commit(execution+0x850,state);
        if(published) return published;
        output=candidate;
        return 0;
}
template<class Owner,class Read,class Commit>
int tglPublishOwnedDndi(Owner& owner,uintptr_t child,uintptr_t callbackSource,
    const TglOwnedDndiSource& source,TglOwnedDndiPackets& output,
    Read read,Commit commit) noexcept {
    return owner.withExecution([&](uintptr_t execution) noexcept {
        return tglPublishLeasedDndiData(execution,child,callbackSource,source,output,read,commit);
    });
}
// Explicit in-frame entry: it may use an existing lease but never acquires or
// replaces one. Full-frame orchestration retains execution through all stages.
template<class Owner,class Read>
int tglPublishOwnedDndiInFrame(Owner& owner,uintptr_t execution,uintptr_t child,
    uintptr_t callbackSource,const TglOwnedDndiSource& source,
    TglOwnedDndiPackets& output,Read read) noexcept {
    if(!owner.hasExecutionLease(execution)) return 5;
    return tglPublishLeasedDndiData(execution,child,callbackSource,source,output,read,
        [&](uintptr_t address,const TglOwnedDndiState& state) noexcept {
            return owner.commitDndi(address,state);
        });
}
template<class Owner,class Read>
int tglPublishOwnedDndi(Owner& owner,uintptr_t child,uintptr_t callbackSource,
    const TglOwnedDndiSource& source,TglOwnedDndiPackets& output,Read read) noexcept {
    return tglPublishOwnedDndi(owner,child,callbackSource,source,output,read,
        [&](uintptr_t address,const TglOwnedDndiState& state) noexcept {
            return owner.commitDndi(address,state);
        });
}

// Borrowed MHW argument from creator1e66ef -> renderer Allocate. The image,
// interface, OS and heap leases must outlive every child using this snapshot.
// This authenticates interface identity/presence, NOT heap contents or commands.
struct alignas(8) TglOwnedVeboxStateStorage {
    std::array<uint8_t,0x30> prefix{};
    std::array<uint8_t,0x148> resource{};
    std::array<uint8_t,0x10> tail{};
    uint8_t* data() noexcept {return reinterpret_cast<uint8_t*>(this);}
    const uint8_t* data() const noexcept {return reinterpret_cast<const uint8_t*>(this);}
    uint8_t& operator[](size_t offset) noexcept {return data()[offset];}
};
static_assert(offsetof(TglOwnedVeboxStateStorage,resource)==0x30);
static_assert(offsetof(TglOwnedVeboxStateStorage,tail)==0x178);
static_assert(sizeof(TglOwnedVeboxStateStorage)==0x188);

enum class TglVeboxSlotReuseReason { Idle,Pending,TagReached,ResetDiscarded };
// fee30 clears pending on signed32(completed-tag)>=0 OR OS370 reset bits
// 20/13. Preserve that distinction: even TagReached is slot-reuse evidence,
// not an independent certificate for releasing every borrowed GPU resource.
// Unsigned subtraction implements native wraparound without C++ signed UB.
inline TglVeboxSlotReuseReason tglVeboxSlotReuseReason(uint8_t pending,
    uint32_t completed,uint32_t tag,uint32_t policy) noexcept {
    if(!(pending&1)) return TglVeboxSlotReuseReason::Idle;
    if(!((completed-tag)&0x80000000u)) return TglVeboxSlotReuseReason::TagReached;
    if(policy&((1u<<20)|(1u<<13))) return TglVeboxSlotReuseReason::ResetDiscarded;
    return TglVeboxSlotReuseReason::Pending;
}

struct TglVeboxHardwareBinding {
    uintptr_t interface = 0, os = 0, heap = 0;
    // Native129ca0 passes argument6 to MHW+18 (171240), with ECX=0.
    // Caller must have completed state/resource validation and heap assignment,
    // and owns command capacity/discard on failure. No implicit submit/retry.
    // Packet180 bit0 selects inline resource30; native may allocate into that
    // 148-byte descriptor (DummyIecpResource). Preserve those writes even on
    // failure so its enclosing resource owner can release them. Never pass a
    // temporary/const packet or pretend this method supplies that ownership.
    template<class Read,class Qualify,class Executable,class Invoke>
    int emitState(uintptr_t image,uintptr_t command,
                  TglOwnedVeboxStateStorage& parameters,
                  Read read,Qualify qualify,Executable executable,Invoke invoke) const {
        TglVeboxHardwareBinding fresh;
        if(!command || !fresh.resolve(interface,os,image,read,qualify) ||
            fresh.heap!=heap) return 5;
        constexpr std::array<uint8_t,16> anchor{
            0x55,0x48,0x89,0xe5,0x48,0x81,0xec,0xb0,
            0x01,0,0,0x48,0x89,0x7d,0xf8,0x48};
        uintptr_t entry=0;
        auto actual=anchor;
        if(!read(image+0x759538,&entry,8) || entry!=image+0x171240 ||
            !executable(entry,anchor.size()) || !read(entry,actual.data(),actual.size()) ||
            actual!=anchor) return 5;
        return invoke(entry,interface,command,parameters.data(),false);
    }
    struct StateResource { uintptr_t resource = 0; uint32_t instanceOffset = 0; };
    // Keep the same external serialization/image lease across all stages.
    // Publication is atomic, not native-state rollback: assignment may advance
    // the heap even if the subsequent owned range validation fails.
    // Owned setup ONLY: native1296e0 already calls assignment at1297b7.
    // Never call this before/after that native setup for the same pass.
    template<class Read, class Qualify, class Executable, class Invoke>
    uint32_t prepareHeapState(uintptr_t image, bool kernelResource,
                              uint32_t bytes, uint32_t regionOffset,
                              Read read, Qualify qualify, Executable executable,
                              Invoke invoke, StateResource& out) const {
        out = {};
        uint32_t stride = 0;
        if (!bytes || !heap || heap > std::numeric_limits<uintptr_t>::max() - 0x2c ||
            !read(heap + 0x2c, &stride, 4) || !stride ||
            uint64_t(regionOffset) + bytes > stride) return 5;
        const uint32_t result = assignState(image, read, qualify, executable, invoke);
        if (result) return result;
        StateResource candidate;
        if (!selectStateResource(false, 0, kernelResource, read, candidate) ||
            !heapStateRange(candidate, bytes, read, regionOffset)) return 5;
        out = candidate;
        return 0;
    }
    // Reuse native refresh/wait/assignment, including OS reset policy and tag
    // wraparound. Caller serializes the borrowed interface and keeps its image
    // lease alive. Do not select a state or emit commands after nonzero result.
    // fefa0 waits ONLY for heap38[next] when its pending bit is set: at most
    // 1000 OS190(6,5) polls, completion from OS290(6) or *heap2d8, signed
    // tag delta >=0; exhaustion returns31. Success captures a NEW tag from
    // OS278(6)/heap2e0 and advances next modulo interface28. Thus success is
    // slot-reuse admission, NOT completion of the subsequently submitted frame
    // and NEVER permission to retire its borrowed source/history resources.
    // Initial OS table: +278=65a80 and +290=65aa0 are constant-zero
    // stubs; +190=65bb0 delays then returns zero. Do not treat any of these
    // callback results as a completion certificate. Actual OS table overrides,
    // heap tag backing and the selected OS+68 branch require separate proof.
    template<class Read, class Qualify, class Executable, class Invoke>
    uint32_t assignState(uintptr_t image, Read read, Qualify qualify,
                         Executable executable, Invoke invoke) const {
        TglVeboxHardwareBinding fresh;
        if (!fresh.resolve(interface, os, image, read, qualify) || fresh.heap != heap) return 5;
        uint32_t count = 0, next = 0;
        if (heap > std::numeric_limits<uintptr_t>::max() - 0x3f ||
            !read(interface + 0x28, &count, 4) || !count || count > INT32_MAX ||
            !read(heap + 4, &next, 4) || next >= count) return 5;
        constexpr std::array<uint8_t, 16> anchor{
            0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x40,
            0x48,0x89,0x7d,0xf8,0x48,0x8b,0x45,0xf8};
        std::array<uint8_t, 16> actual{};
        const uintptr_t entry = image + 0xfefa0;
        if (!executable(entry, anchor.size()) ||
            !read(entry, actual.data(), actual.size()) || actual != anchor) return 5;
        return invoke(entry, interface);
    }
    // Native12a230 calls ff190 AFTER OS2d0 succeeds, only when child1ba8 bit0
    // is clear. This records heap38[current].pending and the native tag; it
    // does not wait. Caller must retain resources and serialize the same pass.
    // Never call on failed submit or duplicate native12a230's own bookkeeping.
    // OS2d0=64df0 calls226e6(context15b0,index8,0,1000000 VALUE), tail-jumping
    // IOAccelVideoContextFinishFenceEvent after context28/count40 admission.
    // It ignores that IOReturn: even native submit result0 is NOT a verified
    // fence completion. Do not retire sources on that result alone. Ghidra's
    // &UNK_000f4240 is an immediate ECX value, not a timeout pointer.
    template<class Read,class Qualify,class Executable,class Invoke>
    uint32_t markSubmitted(uintptr_t image,Read read,Qualify qualify,
                           Executable executable,Invoke invoke) const {
        TglVeboxHardwareBinding fresh;
        if(!fresh.resolve(interface,os,image,read,qualify) || fresh.heap!=heap) return 5;
        uint32_t count=0,current=0;
        if(heap>std::numeric_limits<uintptr_t>::max()-0x2e3 ||
            !read(interface+0x28,&count,4) || !count || count>INT32_MAX ||
            !read(heap,&current,4) || current>=count) return 5;
        constexpr std::array<uint8_t,16> anchor{
            0x55,0x48,0x89,0xe5,0x48,0x89,0x7d,0xf8,
            0x48,0x8b,0x45,0xf8,0xc7,0x45,0xec,0x00};
        std::array<uint8_t,16> actual{};
        const uintptr_t entry=image+0xff190;
        if(!executable(entry,anchor.size()) ||
            !read(entry,actual.data(),actual.size()) || actual!=anchor) return 5;
        return invoke(entry,interface);
    }
    // Heap resources use the proven native buffer layout (resource size148).
    // Not a CM/surface validator. Caller holds the heap/resource lifetime lock;
    // this proves CPU descriptor bounds/backing only, not GPU completion/sync.
    // Include regionOffset (native174cba) as well as the instance base; checking
    // only the instance base would miss a region that crosses buffer capacity.
    template<class Read>
    bool heapStateRange(const StateResource& state, uint32_t bytes, Read read,
                        uint32_t regionOffset = 0) const {
        constexpr auto max = std::numeric_limits<uintptr_t>::max();
        if (!interface || !os || !heap || !bytes || heap > max - 0x2d0 ||
            (state.resource != heap + 0x40 && state.resource != heap + 0x188)) return false;
        uint32_t capacity = 0, type = 0, instanceBytes = 0;
        uintptr_t handle = 0, address = 0;
        return read(heap + 0x2c, &instanceBytes, 4) && instanceBytes &&
            uint64_t(regionOffset) + bytes <= instanceBytes &&
            state.instanceOffset % instanceBytes == 0 &&
            read(state.resource + 0x10, &capacity, 4) &&
            read(state.resource + 0x14, &type, 4) && type == 0 &&
            uint64_t(state.instanceOffset) + regionOffset + bytes <= capacity &&
            read(state.resource + 0x20, &handle, 8) &&
            read(state.resource + 0x50, &address, 8) && (handle || address);
    }
    // ff290 locks DriverResource40 and publishes heap2d8=heap2d0+heap8.
    // Authenticate only the mapped CPU tag location for OS68==0. The caller
    // retains the native lock/resource lease; this does not read a completion
    // value, establish cache visibility or permit source-resource retirement.
    template<class Read>
    bool heapTagSource(Read read,uintptr_t& out) const {
        constexpr auto max=std::numeric_limits<uintptr_t>::max();
        if(!interface || interface>max-0x2c || !os || os>max-0x6c ||
            !heap || heap>max-0x2e0) return false;
        uint32_t branch=0,count=0,stride=0,used=0,total=0,capacity=0,type=0;
        uintptr_t mapped=0,tag=0,address=0;
        if(!read(os+0x68,&branch,4) || branch ||
            !read(interface+0x28,&count,4) || !count || count>INT32_MAX ||
            !read(heap+0x2c,&stride,4) || !stride || !read(heap+8,&used,4) ||
            uint64_t(stride)*count!=used || !read(heap+0x30,&total,4) ||
            uint64_t(used)+4>total || !read(heap+0x50,&capacity,4) || capacity<total ||
            !read(heap+0x54,&type,4) || type ||
            !read(heap+0x90,&address,8) || !address ||
            !read(heap+0x2d0,&mapped,8) || mapped!=address || mapped>max-total ||
            !read(heap+0x2d8,&tag,8) || tag!=mapped+used) return false;
        out=tag;return true;
    }
    // Native171351..1713ba. Selection only: no GPU address or command emitted.
    template<class Read>
    bool selectStateResource(bool cmBuffer, uintptr_t parameterSurface,
                             bool kernelResource, Read read, StateResource& out) const {
        out = {};
        if (!interface || !os || !heap) return false;
        if (cmBuffer) {
            if (!parameterSurface) return false;
            out.resource = parameterSurface;
            return true;
        }
        const uintptr_t resourceOffset = kernelResource ? 0x188 : 0x40;
        if (heap > std::numeric_limits<uintptr_t>::max() - 0x2d0) return false;
        uint32_t instanceBytes = 0, current = 0;
        if (!read(heap + 0x2c, &instanceBytes, 4) || !read(heap, &current, 4)) return false;
        const uint64_t offset = uint64_t(instanceBytes) * current;
        if (offset > UINT32_MAX) return false;
        out = {heap + resourceOffset, uint32_t(offset)};
        return true; // backing/sync/capacity must be validated before native use
    }
    template<class Read, class Qualify>
    bool resolve(uintptr_t mhw, uintptr_t expectedOs, uintptr_t image,
                 Read read, Qualify qualify) {
        *this = {};
        constexpr auto max = std::numeric_limits<uintptr_t>::max();
        if (!mhw || mhw > max - 0x28 || !expectedOs || !image ||
            image > max - 0x7595b0 || !qualify(image)) return false;
        uintptr_t vt = 0, actualOs = 0, actualHeap = 0;
        if (!read(mhw, &vt, 8) || vt != image + 0x759520 ||
            !read(mhw + 0x18, &actualOs, 8) || actualOs != expectedOs ||
            !read(mhw + 0x20, &actualHeap, 8) || !actualHeap) return false;
        interface = mhw; os = actualOs; heap = actualHeap;
        return true;
    }
};

// Exact base prefix touched by parent130ea0 -> nonvirtual126520. NOT a
// complete VEBOX object, not publishable as a native child by itself.
// SysV x86_64 native constructor12d4e0: six register arguments including this,
// followed by performance-data, cache-control reference and status on stack.
// This is an invocation mechanism, NOT storage qualification/publication.
// Owner must authenticate entry, complete extent, dependencies and image lease.
struct TglVeboxLifetimeBinding {
    uintptr_t image = 0, construct = 0, destroy = 0;
    template<class Read, class Qualify, class Executable>
    bool resolve(uintptr_t base, Read read, Qualify qualify, Executable executable) {
        image = construct = destroy = 0;
        constexpr std::array<uint8_t,16> constructor{
            0x55,0x48,0x89,0xe5,0x53,0x50,0xb8,0x88,
            0x10,0x00,0x00,0xe8,0xd0,0x41,0xef,0xff};
        constexpr std::array<uint8_t,16> destructor{
            0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x50,
            0x48,0x8d,0x05,0x49,0x9e,0x62,0x00,0x48};
        std::array<uint8_t,16> c{}, d{};
        if (!base || base > std::numeric_limits<uintptr_t>::max() - 0x12de40 ||
            !qualify(base) || !executable(base+0x12d4e0,16) ||
            !executable(base+0x12de30,16) || !read(base+0x12d4e0,c.data(),16) ||
            !read(base+0x12de30,d.data(),16) || c != constructor || d != destructor)
            return false;
        image = base; construct = base+0x12d4e0; destroy = base+0x12de30;
        return true;
    }
};

// Non-deleting native base teardown only; owned storage release is separate.
// Owned GPU resources must be cleaned before this resets the native vtable.
struct TglNativeVeboxDestructorInvoker {
    void operator()(uintptr_t entry, void* object) const noexcept {
        using Destroy = void (*)(void*);
        reinterpret_cast<Destroy>(entry)(object);
    }
};

struct TglNativeVeboxConstructorInvoker {
    void operator()(uintptr_t entry, void* object, void* os, void* mhw,
                    void* sfc, void* renderHal, void* history,
                    void* performanceData, const void* cacheControl, int32_t* status) const noexcept {
        using Construct = void (*)(void*,void*,void*,void*,void*,void*,void*,const void*,int32_t*);
        reinterpret_cast<Construct>(entry)(object,os,mhw,sfc,renderHal,history,
                                           performanceData,cacheControl,status);
    }
};
enum class TglOwnedBaseConstruction { NotCalled, ConstructedSuccess, ConstructedFailure };
// Bridge the owned native SFC dependency into the proven base ctor's RCX
// argument (stored at child+68). This is NOT publication or derived wiring.
// Any Constructed* result obliges the owner to run base teardown, including
// a nonzero native status; NotCalled must not trigger a native destructor.
template<class SfcInvoker,class Construct>
TglOwnedBaseConstruction tglConstructOwnedVeboxBaseWithSfc(
    const TglVeboxLifetimeBinding &binding,void *privateStorage,size_t capacity,
    TglOwnedSfcMhw<SfcInvoker> &sfc,void *os,void *mhw,void *renderHal,
    void *history,void *performanceData,const void *cacheControl,
    int32_t &nativeStatus,Construct construct) {
    if(!binding.image || binding.image>UINTPTR_MAX-0x12de30 ||
       binding.construct!=binding.image+0x12d4e0 ||
       binding.destroy!=binding.image+0x12de30 ||
       !privateStorage || (reinterpret_cast<uintptr_t>(privateStorage)&7) ||
       capacity<0x1bc0 || !sfc.get() || sfc.image()!=binding.image ||
       sfc.os()!=reinterpret_cast<uintptr_t>(os)) return TglOwnedBaseConstruction::NotCalled;
    construct(binding.construct,privateStorage,os,mhw,sfc.get(),renderHal,history,
              performanceData,cacheControl,&nativeStatus);
    return nativeStatus==0?TglOwnedBaseConstruction::ConstructedSuccess:
                           TglOwnedBaseConstruction::ConstructedFailure;
}

// Arm only after construction of private owned storage has completed. This
// scope does not infer object size, publish a child, or adopt native storage.
// Cleanup owns GPU descriptors; native base teardown owns transferred CPU
// shells. Storage release is last and must not call native operator delete.
template<class Cleanup, class Destroy, class Release> class TglVeboxTeardownScope {
    void* object_;
    bool constructing_=false;
    Cleanup& cleanup_;
    Destroy& destroy_;
    Release& release_;
public:
    TglVeboxTeardownScope(void* constructedPrivateObject, Cleanup& cleanup,
                         Destroy& destroy, Release& release) noexcept
        : object_(constructedPrivateObject), cleanup_(cleanup),
          destroy_(destroy), release_(release) {
        static_assert(noexcept(cleanup_(object_)) && noexcept(destroy_(object_)) &&
                      noexcept(release_(object_)), "teardown must not throw");
    }
    TglVeboxTeardownScope(const TglVeboxTeardownScope&) = delete;
    TglVeboxTeardownScope& operator=(const TglVeboxTeardownScope&) = delete;
    ~TglVeboxTeardownScope() { reset(); }
    // Combine the exact native-constructor bridge with teardown admission.
    // NotCalled leaves storage with its caller. Once native construction ran,
    // this scope owns GPU cleanup/base destruction/storage release; failed
    // construction unwinds immediately. No object is published by this API.
    template<class Construct>
    TglOwnedBaseConstruction constructPrivate(void* storage,Construct construct) noexcept {
        static_assert(noexcept(construct()),"native construction reports status without throwing");
        if(!storage || object_ || constructing_) return TglOwnedBaseConstruction::NotCalled;
        constructing_=true;
        const auto result=construct();
        constructing_=false;
        if(result==TglOwnedBaseConstruction::NotCalled) return result;
        object_=storage;
        if(result==TglOwnedBaseConstruction::ConstructedFailure) reset();
        return result;
    }
    void reset() noexcept {
        void* object = object_;
        object_ = nullptr; // revoke before callbacks, including reentrant reset
        if (!object) return;
        cleanup_(object);
        destroy_(object);
        release_(object);
    }
};

// Private native base + native SFC dependency composition. This is not the
// completed derived child: optional private vtable installation does not
// initialize derived resources or publish a parent slot. Leases outlive it.
struct TglOwnedVeboxFeatureResult {
    int status=5;
    uint32_t value=0;
};
struct TglNativeVeboxReportInvoker {
    uintptr_t operator()(uintptr_t entry,void* child) const noexcept {
        using Query=void* (*)(void*);
        return reinterpret_cast<uintptr_t>(reinterpret_cast<Query>(entry)(child));
    }
};
template<class Invoke=TglNativeVeboxReportInvoker> class TglNativeVeboxReportQuery {
    uintptr_t image_=0;
    Invoke invoke_;
public:
    template<class Read,class Qualify,class Executable>
    TglNativeVeboxReportQuery(uintptr_t image,Read read,Qualify qualify,
        Executable executable,Invoke invoke=Invoke{}) : invoke_(std::move(invoke)) {
        constexpr std::array<uint8_t,32> anchor{
            0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x10,0x48,0x89,0x7d,0xf8,0x48,0x8b,0x45,0xf8,
            0x48,0x83,0xb8,0x88,0,0,0,0,0x48,0x89,0x45,0xf0,0x0f,0x85,0x0d,0};
        std::array<uint8_t,32> bytes{}; uintptr_t slot=0;
        if(image && image<=UINTPTR_MAX-0x757ce8 && qualify(image) &&
           read(image+0x757ce0,&slot,8) && slot==image+0x12ea30 &&
           executable(slot,32) && read(slot,bytes.data(),32) && bytes==anchor) image_=image;
    }
    bool bound() const noexcept {return image_!=0;}
    uintptr_t image() const noexcept {return image_;}
    uintptr_t operator()(void* child) const noexcept {
        static_assert(noexcept(invoke_(uintptr_t{},nullptr)));
        return image_ && child ? invoke_(image_+0x12ea30,child) : 0;
    }
};
struct TglNativeVeboxFeatureInvoker {
    int operator()(uintptr_t entry,uintptr_t context,uint32_t id,void* data) const noexcept {
        using Query=int (*)(void*,uint32_t,void*);
        return reinterpret_cast<Query>(entry)(reinterpret_cast<void*>(context),id,data);
    }
};
template<class Invoke=TglNativeVeboxFeatureInvoker> class TglNativeVeboxFeatureQuery {
    uintptr_t entry_=0;
    Invoke invoke_;
public:
    // Qualified exact image and its native interface lease outlive this object.
    template<class Read,class Qualify,class Executable>
    TglNativeVeboxFeatureQuery(uintptr_t image,Read read,Qualify qualify,
        Executable executable,Invoke invoke=Invoke{}) : invoke_(std::move(invoke)) {
        constexpr std::array<uint8_t,16> anchor{
            0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x20,
            0x48,0x89,0x7d,0xf8,0x89,0x75,0xf4,0x48};
        std::array<uint8_t,16> bytes{};
        if(image && image<=UINTPTR_MAX-0x44630 && qualify(image) &&
           executable(image+0x44620,16) && read(image+0x44620,bytes.data(),16) && bytes==anchor)
            entry_=image+0x44620;
    }
    bool bound() const noexcept { return entry_!=0; }
    uintptr_t image() const noexcept { return entry_ ? entry_-0x44620 : 0; }
    int operator()(uintptr_t context,uint32_t id,void* data) const noexcept {
        static_assert(noexcept(invoke_(uintptr_t{},uintptr_t{},uint32_t{},nullptr)));
        if(!entry_ || context || !data || (id!=0xb8 && id!=0xb9)) return 5;
        return invoke_(entry_,context,id,data);
    }
};
// Allocate126a08/126a80 builds a 0x28 value record. +20=1 only for b8;
// b8 starts with value1, b9 with0. Native consumes the resulting first dword
// even on query error; retain status for diagnostics, do not invent success.
template<class Query>
TglOwnedVeboxFeatureResult tglReadOwnedVeboxFeature(uint32_t id,Query query) noexcept {
    static_assert(noexcept(query(uintptr_t{},uint32_t{},static_cast<void*>(nullptr))),
                  "native feature query must not throw");
    if(id!=0xb8 && id!=0xb9) return {};
    std::array<uint8_t,0x28> data{};
    const uint32_t one=1;
    if(id==0xb8) {
        std::memcpy(data.data(),&one,4);
        std::memcpy(data.data()+0x20,&one,4);
    }
    TglOwnedVeboxFeatureResult result;
    result.status=query(0,id,data.data());
    std::memcpy(&result.value,data.data(),4);
    return result;
}

// Exact1269fa..126bc6 Allocate tail; callers provide the native b8 query result
// and OS370 flags, NOT host/Linux defaults. Borrowed70 must outlive the child.
// Does not create SFC, initialize GPU backing, or publish a complete child.
struct TglOwnedVeboxAllocationTail {
    uint32_t nativeFeatureB8=0,osFlags=0,input08=0,input10=0;
    bool input0c=false;
    uintptr_t borrowed70=0;
    bool write(uint8_t* child,size_t extent) const noexcept {
        if(!child || extent<0x1bc0 || reinterpret_cast<uintptr_t>(child)>UINTPTR_MAX-extent)
            return false;
        const uint32_t sentinel=0xfffffc00;
        std::memcpy(child+0x1b9c,&nativeFeatureB8,4);
        child[0x1ba9]=child[0x1baa]=child[0x1bab]=0;
        child[0x1ba8]=uint8_t((osFlags&((1u<<13)|(1u<<20)))!=0);
        child[0x30]=uint8_t(input0c);
        std::memcpy(child+0x1b98,&input10,4);
        std::memcpy(child+0x1a8,&input08,4);
        std::memcpy(child+0x70,&borrowed70,8);
        std::memcpy(child+0x1b78,&sentinel,4);
        std::memcpy(child+0x1b7c,&sentinel,4);
        child[0x1b74]=1;
        return true;
    }
};

template<class CpuBackend,class SfcInvoke,class Cleanup,class Destroy>
class TglOwnedVeboxBaseOwner {
    TglTrackedCpuOwner<CpuBackend> storage_;
    TglOwnedSfcMhw<SfcInvoke> sfc_;
    TglOwnedVeboxVtable table_;
    Cleanup& cleanup_;
    Destroy& destroy_;
    uintptr_t destructor_=0,image_=0;
    bool constructed_=false,ready_=false,busy_=false,tableInstalled_=false;
public:
    TglOwnedVeboxBaseOwner(CpuBackend& cpu,SfcInvoke& sfc,Cleanup& cleanup,Destroy& destroy)
        :storage_(cpu),sfc_(sfc),cleanup_(cleanup),destroy_(destroy) {
        static_assert(noexcept(cleanup_(nullptr)) && noexcept(destroy_(uintptr_t{},nullptr)),
                      "native base cleanup must not throw");
    }
    TglOwnedVeboxBaseOwner(const TglOwnedVeboxBaseOwner&)=delete;
    TglOwnedVeboxBaseOwner& operator=(const TglOwnedVeboxBaseOwner&)=delete;
    ~TglOwnedVeboxBaseOwner(){reset();}
    template<class Read,class Construct>
    TglOwnedBaseConstruction initialize(const TglVeboxLifetimeBinding& binding,
        const TglSfcLifetimeBinding& sfcLifetime,const TglSfcMhwBinding& sfcBinding,
        void* os,void* mhw,void* renderHal,void* history,void* performance,
        const void* cache,int32_t& status,Read read,Construct construct) {
        static_assert(noexcept(construct(uintptr_t{},nullptr,nullptr,nullptr,nullptr,
            nullptr,nullptr,nullptr,nullptr,&status)),"constructor reports native status");
        if(busy_) return TglOwnedBaseConstruction::NotCalled;
        reset();busy_=true;
        if(binding.image!=sfcLifetime.image ||
           !sfc_.initialize(sfcLifetime,sfcBinding,reinterpret_cast<uintptr_t>(os),read)) {
            busy_=false;reset();return TglOwnedBaseConstruction::NotCalled;
        }
        if(!storage_.allocate(0x1bc0)){busy_=false;reset();return TglOwnedBaseConstruction::NotCalled;}
        const auto result=tglConstructOwnedVeboxBaseWithSfc(binding,
            reinterpret_cast<void*>(storage_.get()),storage_.size(),sfc_,os,mhw,
            renderHal,history,performance,cache,status,construct);
        if(result==TglOwnedBaseConstruction::NotCalled){busy_=false;reset();return result;}
        constructed_=true;destructor_=binding.destroy;image_=binding.image;
        ready_=result==TglOwnedBaseConstruction::ConstructedSuccess;
        busy_=false;
        if(!ready_) reset();
        return result;
    }
    void* constructedBase() const noexcept {
        return ready_ ? reinterpret_cast<void*>(storage_.get()) : nullptr;
    }
    bool configureAllocationTail(const TglOwnedVeboxAllocationTail& tail) noexcept {
        if(!ready_ || busy_ || tableInstalled_) return false;
        return tail.write(reinterpret_cast<uint8_t*>(storage_.get()),storage_.size());
    }
    // Caller binds the authenticated native44620 feature query and OS370
    // callback. OS comes from native-constructed child10, never a caller's
    // substituted interface. Keep the exact query status separate from tail
    // configuration success: native itself consumes the value even on error.
    template<class Feature,class Flags>
    bool configureAllocationTailFromNative(TglOwnedVeboxAllocationTail tail,
        TglOwnedVeboxFeatureResult& featureResult,Feature feature,Flags flags) noexcept {
        static_assert(noexcept(flags(static_cast<void*>(nullptr))) && noexcept(feature.bound()),
                      "OS flags callback must not throw");
        if(!ready_ || busy_ || tableInstalled_) return false;
        uintptr_t os=0;
        std::memcpy(&os,reinterpret_cast<uint8_t*>(storage_.get())+0x10,8);
        if(!os) return false;
        busy_=true;
        // Binding failure is not a native query result. Do not silently consume
        // b8's default1 when no authenticated native callback can be invoked.
        if(!feature.bound() || feature.image()!=image_) {busy_=false;return false;}
        const auto result=tglReadOwnedVeboxFeature(0xb8,feature);
        tail.nativeFeatureB8=result.value;
        tail.osFlags=flags(reinterpret_cast<void*>(os));
        const bool configured=tail.write(reinterpret_cast<uint8_t*>(storage_.get()),storage_.size());
        featureResult=result;
        busy_=false;
        return configured;
    }
    // Before private-table publication, transfer all CPU shells to the native
    // base destructor. Shell backend MUST use the matching native436d0 allocator
    // family; this transfers CPU ownership only, never GPU backing or fences.
    template<class ShellBackend>
    bool installCpuShells(TglVeboxCpuShells<ShellBackend>& shells) noexcept {
        if(!ready_ || busy_ || tableInstalled_ || shells.image()!=image_) return false;
        busy_=true;
        const bool installed=shells.installIntoPrivateBase(
            reinterpret_cast<uint8_t*>(storage_.get()),storage_.size());
        busy_=false;
        return installed;
    }
    template<class ShellBackend>
    bool allocateCpuShells(ShellBackend& backend) {
        if(!ready_ || busy_ || tableInstalled_ || backend.image()!=image_) return false;
        struct Guard {
            bool& flag;
            explicit Guard(bool& value) noexcept : flag(value) { flag=true; }
            ~Guard() { flag=false; }
        } guard(busy_);
        // Guard outlives shells, including failed-allocation unwind callbacks.
        TglVeboxCpuShells<ShellBackend> shells(backend);
        return shells.allocate() && shells.installIntoPrivateBase(
            reinterpret_cast<uint8_t*>(storage_.get()),storage_.size());
    }
    // Native Allocate126929 also constructs helper1bb8. It is NOT a raw CPU
    // shell: factory126be0 accounts its allocation, and base destructor calls
    // 12e130(pointer-to-field), decrements that counter and invokes vtable+8.
    // Never release this object through tracked436d0 or memcpy a foreign one.
    template<class Read,class Executable,class Create,class Query>
    bool allocateNativeHelper(Read read,Executable executable,Create create,Query query) noexcept {
        static_assert(noexcept(create(uintptr_t{})) && noexcept(query(nullptr)),
                      "native helper factory/query must not throw");
        if(!ready_ || busy_ || tableInstalled_ || !query.bound() || query.image()!=image_) return false;
        uintptr_t existing=0;
        auto* field=reinterpret_cast<uint8_t*>(storage_.get())+0x1bb8;
        std::memcpy(&existing,field,8);
        if(existing) return false;
        constexpr std::array<uint8_t,16> anchor{
            0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x40,
            0x48,0x8b,0x35,0x39,0xd4,0x62,0x00,0xbf};
        struct Guard {
            bool& flag;
            explicit Guard(bool& value) noexcept : flag(value) {flag=true;}
            ~Guard() {flag=false;}
        } guard(busy_);
        std::array<uint8_t,16> bytes{};
        const uintptr_t entry=image_+0x126be0;
        if(!executable(entry,16) || !read(entry,bytes.data(),16) || bytes!=anchor) return false;
        const uintptr_t helper=create(entry);
        if(!helper) return false;
        // Allocate12696a: helper+8=this, helper+10=virtual48(this).
        // Native+48=12ea30 lazily calls +40=12e9c0 when child88 is null;
        // that creates/initializes the report through12ebc0 and report vtable10.
        // Child owns report88 (base destructor12e00f), helper merely borrows it.
        // Null is preserved exactly as native. Caller binds qualified +48, not
        // a raw pointer getter that skips report construction/initialization.
        const uintptr_t child=storage_.get();
        const uintptr_t report=query(reinterpret_cast<void*>(child));
        std::memcpy(reinterpret_cast<uint8_t*>(helper)+8,&child,8);
        std::memcpy(reinterpret_cast<uint8_t*>(helper)+0x10,&report,8);
        std::memcpy(field,&helper,8); // Native base destructor now owns it.
        return true;
    }
    // Synchronous child lease for the authenticated builder/producer context.
    // Reset/reinitialize/table replacement are inert during invocation, so
    // callbacks cannot invalidate their own base, private table or SFC object.
    // This does NOT cover asynchronous commands: caller retains this owner
    // and its resource owners through discard/fence completion separately.
    template<class Invoke> int withPrivateChild(Invoke invoke) noexcept {
        static_assert(noexcept(invoke(static_cast<void*>(nullptr))),
                      "child invocation reports native status");
        if(!ready_ || !tableInstalled_ || busy_) return 5;
        busy_=true;
        const int result=invoke(reinterpret_cast<void*>(storage_.get()));
        busy_=false;
        return result;
    }
    template<class ExecutionOwner>
    int setupKernelScratch(ExecutionOwner& execution,int32_t index) noexcept {
        if(!ready_ || !tableInstalled_ || !busy_ ||
           !execution.hasExecutionLease(execution.get())) return 5;
        uintptr_t linked=0;
        std::memcpy(&linked,reinterpret_cast<const uint8_t*>(storage_.get())+0x88,8);
        if(linked!=execution.get()) return 5;
        // Both owners remain leased; unsupported native status 31 is retained.
        return execution.clearKernelScratch(reinterpret_cast<void*>(storage_.get()),0x1bc0,index);
    }
    bool publishCsc(const TglCscCoefficients& candidate) noexcept {
        if(!ready_ || !tableInstalled_ || !busy_) return false;
        return tglPublishOwnedCscFields(reinterpret_cast<void*>(storage_.get()),0x1bc0,candidate);
    }
    bool hasPrivateChildLease(uintptr_t expected) const noexcept {
        return ready_ && tableInstalled_ && busy_ && storage_.get()==expected;
    }
    template<class FormatRead,class Read,class Qualify,class Executable>
    bool installPrivateVtableWithQueries(const std::array<uintptr_t,20>& hooks,
        Read read,Qualify qualify,Executable executable) noexcept {
        return installPrivateVtable(tglBindOwnedVeboxQueryHooks<FormatRead>(hooks),
                                    read,qualify,executable);
    }
    template<class Read,class Qualify,class Executable>
    bool installPrivateVtable(const std::array<uintptr_t,20>& hooks,
        Read read,Qualify qualify,Executable executable) noexcept {
        static_assert(noexcept(read(uintptr_t{},static_cast<void*>(nullptr),size_t{})) &&
            noexcept(qualify(uintptr_t{})) && noexcept(executable(uintptr_t{},size_t{})),
            "private table admission must report failure without throwing");
        if(!ready_ || busy_ || tableInstalled_) return false;
        // This slot is implemented, not an injectable platform policy. Reject
        // an accidental native FALSE or arbitrary replacement before mutation.
        if(hooks[15]!=reinterpret_cast<uintptr_t>(&tglOwnedVeboxGen12CscSupported))
            return false;
        uintptr_t nativeTable=0;
        std::memcpy(&nativeTable,reinterpret_cast<void*>(storage_.get()),8);
        if(nativeTable!=image_+0x757c98) return false;
        busy_=true;
        const bool valid=table_.build(image_,hooks,read,qualify,executable);
        if(valid) {
            const auto address=table_.addressPoint();
            std::memcpy(reinterpret_cast<void*>(storage_.get()),&address,8);
            tableInstalled_=true;
        }
        busy_=false;
        return valid;
    }
    void reset() noexcept {
        if(busy_) return;
        busy_=true;
        ready_=false;
        if(constructed_) {
            constructed_=false;
            void* object=reinterpret_cast<void*>(storage_.get());
            cleanup_(object);
            destroy_(destructor_,object);
        }
        destructor_=image_=0;storage_.reset();sfc_.reset();
        tableInstalled_=false;table_.entries={};table_.typeInfo=0;
        busy_=false;
    }
};

struct TglVeboxPrefix {
    uintptr_t vtable = 0, slot08 = 0, os = 0, renderHal = 0, sku = 0, wa = 0;
    std::array<uint8_t, 8> slot30{};
    uintptr_t slot38 = 0, slot40 = 0;
    uint8_t flag48 = 0, flag49 = 0;
    std::array<uint8_t, 6> padding4a{};
    uintptr_t parentData = 0;
    uint32_t sourceValue = 0, padding5c = 0;
    // Constructor-data preparation only. No vtable is installed, no native
    // object is published, and no report/resource ownership is transferred.
    // Caller authenticates/leases both borrowed interfaces before this read.
    // platform belongs at full-child1bb0, NOT inside this 0x60 prefix.
    template<class Read>
    bool initializeBorrowed(uintptr_t nativeOs, uintptr_t nativeRenderHal,
                            uintptr_t performanceData, uint32_t& platform, Read read) {
        constexpr auto max = std::numeric_limits<uintptr_t>::max();
        if (!nativeOs || nativeOs > max - 0xb || !nativeRenderHal ||
            nativeRenderHal > max - 0x87) return false;
        // Do not overwrite a prefix that could already carry live ownership.
        if (vtable || slot08 || os || renderHal || sku || wa || slot38 || slot40 ||
            flag48 || flag49 || parentData || sourceValue) return false;
        uintptr_t nativeSku = 0, nativeWa = 0;
        uint32_t nativePlatform = 0;
        if (!read(nativeRenderHal+0x78,&nativeSku,8) ||
            !read(nativeRenderHal+0x80,&nativeWa,8) ||
            !read(nativeOs+8,&nativePlatform,4)) return false;
        TglVeboxPrefix candidate;
        candidate.os = nativeOs; candidate.renderHal = nativeRenderHal;
        candidate.sku = nativeSku; candidate.wa = nativeWa;
        candidate.slot38 = performanceData;
        *this = candidate; platform = nativePlatform;
        return true;
    }
    template<class Read>
    bool configure(uintptr_t parent, uintptr_t source, Read read) {
        constexpr auto max = std::numeric_limits<uintptr_t>::max();
        if (!parent || parent > max - 0x9f0 || !source || source > max - 0x120)
            return false;
        uintptr_t surface = 0;
        uint32_t kind = 0, value = 0;
        uint8_t flag = 0;
        if (!read(source + 0x98, &surface, sizeof(surface)) || !surface ||
            surface > max - 0x138 || !read(surface + 0x134, &kind, sizeof(kind)) ||
            !read(source + 0x110, &flag, sizeof(flag)) ||
            !read(source + 0x11c, &value, sizeof(value))) return false;
        flag48 = flag & 1;
        flag49 = kind == 5;
        parentData = parent + 0x9f0;
        sourceValue = value;
        return true; // all fields borrow parent/source lifetime; no ownership transfer
    }
};
static_assert(sizeof(TglVeboxPrefix) == 0x60);
static_assert(offsetof(TglVeboxPrefix, os) == 0x10);
static_assert(offsetof(TglVeboxPrefix, renderHal) == 0x18);
static_assert(offsetof(TglVeboxPrefix, flag48) == 0x48);
static_assert(offsetof(TglVeboxPrefix, flag49) == 0x49);
static_assert(offsetof(TglVeboxPrefix, parentData) == 0x50);
static_assert(offsetof(TglVeboxPrefix, sourceValue) == 0x58);

// child40 owns a separate28-byte CPU report (1335c0), NOT execution
// state child88. Native1338b0 initializes fields, leaving padding untouched.
struct TglVeboxReportState {
    std::array<uint8_t, 0x28> bytes{};
    void reset() noexcept {
        for (size_t i = 0; i <= 0x1c; ++i) bytes[i] = 0;
        for (size_t i = 0x20; i <= 0x25; ++i) bytes[i] = 0;
    }
};
static_assert(sizeof(TglVeboxReportState) == 0x28);

// 127502..12756c + virtualc8=12eaf0. Offsets are proven; semantic
// flag names remain unassigned. Do not reset DWORD4 when execution13 is false.
inline void tglUpdateVeboxReport(TglVeboxReportState& report,
                                 const std::array<uint8_t, 0x15>& execution) noexcept {
    report.bytes[0] = execution[0x0f] & 1;
    report.bytes[2] = execution[0x0b] & 1;
    if (execution[0x13] & 1) {
        const uint32_t value = (execution[0x14] & 1) && !(execution[8] & 1) ? 2 : 3;
        std::memcpy(report.bytes.data() + 4, &value, sizeof(value));
    }
}

// Native Render tail12cdcb..12ce07. This exports a decision already made
// by the platform producer; it must NOT choose or normalize outputPipe.
// execution+a0c is the output-pipe decision, NOT the persistent VEBOX
// execution/transition state at history+4 (child90). Never exchange them.
inline void tglFinalizeVeboxReport(TglVeboxReportState& report, uint32_t outputPipe,
                                   uint8_t execution18, uint8_t execution8f4) noexcept {
    std::memcpy(report.bytes.data() + 0xc, &outputPipe, sizeof(outputPipe));
    report.bytes[0x24] = (execution18 ^ 1) & 1;
    report.bytes[0x25] = execution8f4 & 1;
}

// Exact post-render export12d240 ->12d120/12d1a0. Source is the
// owned equivalent of child40, not the child object itself. Preserve every
// destination byte not written by native; source/destination may alias.
inline void tglExportVeboxReport(const std::array<uint8_t, 0x26>& state,
                                 uint8_t feature1ba9,
                                 std::array<uint8_t, 0x26>& report) noexcept {
    const auto source = state;
    for (auto offset : {0x00, 0x02, 0x13, 0x15, 0x17, 0x19, 0x24, 0x25})
        report[offset] = source[offset] & 1;
    for (auto offset : {0x14, 0x16, 0x18, 0x1a}) report[offset] = source[offset];
    std::memcpy(report.data() + 4, source.data() + 4, 4);
    std::memcpy(report.data() + 0xc, source.data() + 0xc, 4);
    report[0x10] = feature1ba9 & 1;
}
inline void tglExportVeboxReport(const TglVeboxReportState& state,
                                 uint8_t feature1ba9,
                                 std::array<uint8_t, 0x26>& report) noexcept {
    std::array<uint8_t, 0x26> fields{};
    std::memcpy(fields.data(), state.bytes.data(), fields.size());
    tglExportVeboxReport(fields, feature1ba9, report);
}

// Owned lifecycle only. A Darwin backend must separately prove allocation,
// backing inspection and release ABI; this class never fabricates backing.
// Native evidence: 4e060 reuse/reallocation; 63720 failure -> 63c00 cleanup.
struct TglResourceKey {
    uint32_t width, height, format, tile, compressionMode;
    bool compressed;
    bool operator==(const TglResourceKey &b) const noexcept {
        return width == b.width && height == b.height && format == b.format &&
            tile == b.tile && compressionMode == b.compressionMode && compressed == b.compressed;
    }
};

// Pinned native 62a40 predicate, not a GPU execution/completion guarantee.
inline bool tglNativeBackingPresent(const std::array<uint8_t, 0x148> &s) noexcept {
    uint32_t type = 0;
    uint64_t a = 0, b = 0;
    std::memcpy(&type, s.data() + 0x14, sizeof(type));
    if (type == 0) {
        std::memcpy(&a, s.data() + 0x20, sizeof(a));
        std::memcpy(&b, s.data() + 0x50, sizeof(b));
        return a != 0 || b != 0;
    }
    if (type == 1) {
        std::memcpy(&a, s.data() + 0x58, sizeof(a));
        return a != 0;
    }
    return false;
}

// 63030 allocates VM at resource+50, registers the resource at+20 and stores
// shared metadata at+28/+30. Only +50 is the buffer byte address. Caller holds
// resource lifetime; Writable and Fill must use native policy, not bypass it.
template<class Writable, class Fill>
int tglFillBuffer(const std::array<uint8_t, 0x148> &resource, uint32_t bytes,
                  uint8_t value, Writable writable, Fill fill) noexcept {
    static_assert(noexcept(writable(uintptr_t{}, uint32_t{})) &&
                  noexcept(fill(uintptr_t{}, uint32_t{}, uint8_t{})),
                  "memory access callbacks must report failure without exceptions");
    uint32_t type = 0, capacity = 0;
    uintptr_t address = 0, handle = 0;
    std::memcpy(&type, resource.data() + 0x14, sizeof(type));
    std::memcpy(&capacity, resource.data() + 0x10, sizeof(capacity));
    std::memcpy(&address, resource.data() + 0x50, sizeof(address));
    std::memcpy(&handle, resource.data() + 0x20, sizeof(handle));
    if (type || !bytes || bytes > capacity || !handle || !address ||
        address > std::numeric_limits<uintptr_t>::max() - bytes ||
        !writable(address, bytes)) return 5;
    return fill(address, bytes, value);
}

#if defined(__APPLE__)
inline bool tglNativeWritableRange(uintptr_t address, uint32_t bytes) noexcept {
    if (!address || !bytes || address > UINTPTR_MAX - bytes) return false;
    const mach_vm_address_t end = address + bytes;
    mach_vm_address_t cursor = address;
    for (unsigned regions = 0; cursor < end && regions < 64; ++regions) {
        mach_vm_address_t base = cursor;
        mach_vm_size_t size = 0;
        natural_t depth = 0;
        vm_region_submap_info_data_64_t info{};
        for (;;) {
            mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
            if (mach_vm_region_recurse(mach_task_self(), &base, &size, &depth,
                reinterpret_cast<vm_region_recurse_info_t>(&info), &count) != KERN_SUCCESS ||
                count != VM_REGION_SUBMAP_INFO_COUNT_64 || base > cursor || !size ||
                base > UINT64_MAX - size || cursor >= base + size) return false;
            if (!info.is_submap) break;
            if (depth >= 16) return false;
            ++depth;
        }
        if (!(info.protection & VM_PROT_WRITE)) return false;
        cursor = base + size < end ? base + size : end;
    }
    return cursor == end;
}

// No mprotect/cs_allow_invalid/permission escalation. Mach decides each write.
inline int tglFillBufferNative(const std::array<uint8_t, 0x148> &resource,
                               uint32_t bytes, uint8_t value) noexcept {
    const auto write = [](uintptr_t address, uint32_t length, uint8_t byte) noexcept {
        std::array<uint8_t, 4096> block;
        block.fill(byte);
        for (uint32_t offset = 0; offset < length;) {
            const uint32_t n = length - offset < block.size() ? length - offset : block.size();
            const kern_return_t status = mach_vm_write(mach_task_self(), address + offset,
                reinterpret_cast<vm_offset_t>(block.data()), n);
            if (status != KERN_SUCCESS) return static_cast<int>(status);
            offset += n;
        }
        return 0;
    };
    return tglFillBuffer(resource, bytes, value, tglNativeWritableRange, write);
}
#endif

// 637e5..6380d rounds in 64 bits then stores only 32 bits. Require the
// caller's authenticated native page size; do not assume a host page size.
inline bool tglBufferSizeFits(uint32_t bytes, uint64_t pageSize) noexcept {
    if (!bytes || !pageSize || (pageSize & (pageSize - 1)) ||
        pageSize > uint64_t{UINT32_MAX}) return false;
    const uint64_t rounded = (uint64_t{bytes} + pageSize - 1) & ~(pageSize - 1);
    return rounded != 0 && rounded <= UINT32_MAX;
}

// Exact 52bf0 surface-to-command descriptor conversion. The +40 pointer
// borrows shell storage, so shell must outlive every consumer of the result.
// This converts existing state; it does not create backing or validate formats.
inline std::array<uint8_t, 0x48> tglSurfaceDescriptorBytes(const uint8_t* shell) noexcept {
    std::array<uint8_t, 0x48> out{};
    const auto get = [&](size_t offset) {
        uint32_t value = 0;
        std::memcpy(&value, shell + offset, sizeof(value));
        return value;
    };
    const auto put = [&](size_t offset, uint32_t value) {
        std::memcpy(out.data() + offset, &value, sizeof(value));
    };
    put(0, 1);
    put(8, get(0x130)); put(0xc, get(0xd8)); put(0x10, get(0xdc));
    put(0x14, get(0xe0)); put(0x18, get(0x13c)); put(0x28, get(0xe4));
    std::memcpy(out.data() + 0x2c, shell + 0x50, 16);
    static_assert(sizeof(uintptr_t) == 8, "pinned x86_64 ABI");
    const uintptr_t resource = reinterpret_cast<uintptr_t>(shell + 0x148);
    std::memcpy(out.data() + 0x40, &resource, sizeof(resource));
    out[4] = shell[0x29a] & 1;
    const uint32_t pitch = get(0xe0);
    if (pitch) put(0x24, (get(0x100) - get(0xf0)) / pitch + get(0x108));
    return out;
}

// SFC output descriptor ABI (ICL1dcf30), sourced from the already-closed
// Darwin surface-shell fields. Unlike52bf0 this is0x38 bytes, with the borrowed
// resource pointer at28, not40. No ICL object or GPU command is reused.
inline std::array<uint8_t,0x38> tglOwnedSfcOutputDescriptor(
        const std::array<uint8_t,0x2a8>& shell) noexcept {
    std::array<uint8_t,0x38> out{};
    const auto get=[&](size_t offset) {
        uint32_t value=0;std::memcpy(&value,shell.data()+offset,4);return value;
    };
    const auto put=[&](size_t offset,uint32_t value) {
        std::memcpy(out.data()+offset,&value,4);
    };
    put(0,get(0x294));put(4,get(0x130));
    put(8,get(0xd8));put(0xc,get(0xdc));put(0x10,get(0xe0));put(0x14,get(0xe4));
    put(0x1c,get(0xf4));put(0x20,get(0xf8));
    const uint32_t pitch=get(0xe0);
    if(pitch) put(0x24,(get(0x100)-get(0xf0))/pitch+get(0x108));
    const uintptr_t resource=reinterpret_cast<uintptr_t>(shell.data()+0x148);
    std::memcpy(out.data()+0x28,&resource,8);
    out[0x30]=shell[0x299]&1;
    return out;
}

inline std::array<uint8_t,0x48> tglSurfaceDescriptor(
        const std::array<uint8_t,0x2a8>& shell) noexcept {
    return tglSurfaceDescriptorBytes(shell.data());
}

// 52d60: five optional borrowed shells, no allocation or ownership transfer.
inline std::array<uint8_t, 0x170> tglSurfaceCommandSetBytes(
        const std::array<const uint8_t*,5>& surfaces,
        uint8_t di) noexcept {
    std::array<uint8_t, 0x170> out{};
    out[0x168] = di & 1;
    for (size_t i = 0; i < surfaces.size(); ++i) {
        if (!surfaces[i]) continue;
        const auto descriptor = tglSurfaceDescriptorBytes(surfaces[i]);
        std::memcpy(out.data() + i * 0x48, descriptor.data(), descriptor.size());
        if (i < 2) std::memcpy(out.data() + i * 0x48 + 0x20,
                               surfaces[i] + 0xf8, 4);
    }
    out[0x169] = surfaces[1] ? 1 : 0;
    return out;
}

inline std::array<uint8_t,0x170> tglSurfaceCommandSet(
        const std::array<const std::array<uint8_t,0x2a8>*,5>& surfaces,
        uint8_t di) noexcept {
    std::array<const uint8_t*,5> views{};
    for (size_t i=0; i<views.size(); ++i)
        if (surfaces[i]) views[i] = surfaces[i]->data();
    return tglSurfaceCommandSetBytes(views,di);
}

// 1295c0: query selector5 once, preserve its failure and caller outputs.
// query's platform implementation remains separately required; do not import
// ICL's statistics size into the TGL producer.
template<class Query>
int tglStatisticsOffsets(uint32_t width, uint32_t height, uint8_t flag1b88,
                         uint8_t flag1b89, uint32_t &first, uint32_t &second,
                         Query query) noexcept {
    static_assert(noexcept(query(uint32_t{5}, std::declval<uint32_t &>())),
                  "native query must return status without throwing");
    uint32_t size = 0;
    const int status = query(5, size);
    if (status) return status;
    const uint32_t pixels = width * height; // native 32-bit arithmetic
    if (flag1b89 & 1) {
        first = pixels + size;
        second = pixels + 3u * size;
    } else if (flag1b88 & 1) {
        first = pixels;
        second = pixels + size;
    } else {
        first = 0;
        second = size;
    }
    return 0;
}

// Intel Gen12 VeboxQueryStatLayout (2a32c7f), enum values independently
// matched to Darwin's query call. Hardware layout only, no Linux object ABI.
inline int tglStatisticsQuery(uint32_t selector, uint32_t &value) noexcept {
    switch (selector) {
        case 0: value = 0; return 0;
        case 2: value = 0x2c; return 0;
        case 3: value = 0x44; return 0;
        case 5: value = 128; return 0;
        default: return 31;
    }
}

struct TglStatisticsAllocation {
    uint32_t rowBytes = 0, blockRows = 0, totalRows = 0, bytes = 0;
};
// Gen12 allocation: 64-byte rows, ceil(height/4) per-block rows, plus
// ceil(1024/rowBytes) reserved rows. Publication is atomic on validation.
inline int tglStatisticsAllocationSize(uint32_t boundaryWidth, uint32_t boundaryHeight,
                                      TglStatisticsAllocation &out) noexcept {
    if (!boundaryWidth || !boundaryHeight) return 5;
    const uint64_t row = (uint64_t{boundaryWidth} + 63) & ~uint64_t{63};
    const uint64_t blocks = (uint64_t{boundaryHeight} + 3) / 4;
    const uint64_t total = blocks + (1024 + row - 1) / row;
    const uint64_t bytes = row * total;
    if (row > UINT32_MAX || total > UINT32_MAX || bytes > UINT32_MAX) return 5;
    out = {static_cast<uint32_t>(row), static_cast<uint32_t>(blocks),
           static_cast<uint32_t>(total), static_cast<uint32_t>(bytes)};
    return 0;
}

// 170ef0 valid-input boundary contract. Reject malformed float conversions
// and rounding overflow instead of reproducing native truncation hazards.
inline int tglSurfaceBoundary(uint32_t format, uint32_t width, uint32_t height,
                              float sourceWidth, float sourceHeight,
                              bool di, bool alignDi64, uint32_t &outWidth,
                              uint32_t &outHeight) noexcept {
    if (!std::isfinite(sourceWidth) || !std::isfinite(sourceHeight) ||
        sourceWidth < 0 || sourceHeight < 0 ||
        double(sourceWidth) > UINT32_MAX || double(sourceHeight) > UINT32_MAX)
        return 5;
    uint32_t wa = 1, ha = 1;
    if (format == 0x19) { wa = 2; ha = di ? 4 : 2; }
    else if (format >= 0xd && format <= 0x13) { wa = 2; ha = di ? 2 : 1; }
    else if (format == 0x14 || format == 0x15) wa = 2;
    if (di && alignDi64) wa = 64;
    const uint32_t sw = static_cast<uint32_t>(sourceWidth);
    const uint32_t sh = static_cast<uint32_t>(sourceHeight);
    const uint32_t wc = sw > 64 ? sw : 64, hc = sh > 16 ? sh : 16;
    const uint64_t w = (uint64_t{width < wc ? width : wc} + wa - 1) & ~uint64_t{wa - 1};
    const uint64_t h = (uint64_t{height < hc ? height : hc} + ha - 1) & ~uint64_t{ha - 1};
    if (w > UINT32_MAX || h > UINT32_MAX) return 5;
    outWidth = static_cast<uint32_t>(w); outHeight = static_cast<uint32_t>(h);
    return 0;
}

struct TglNativeRegistrationBinding {
    uintptr_t context = 0, entry = 0;
    template<class Read, class Qualify, class Executable>
    bool resolve(uintptr_t os, uintptr_t image, Read read, Qualify qualify,
                 Executable executable) {
        *this = {};
        if (!os || !image || os > UINTPTR_MAX-0x24f || image > UINTPTR_MAX-0x645f6 ||
            !qualify(image)) return false;
        uintptr_t native = 0;
        constexpr std::array<uint8_t,22> expected{
            0x55,0x48,0x89,0xe5,0x31,0xc0,0x48,0x89,0x7d,0xf8,0x48,
            0x89,0x75,0xf0,0x89,0x55,0xec,0x89,0x4d,0xe8,0x5d,0xc3};
        std::array<uint8_t,22> actual{};
        if (!read(os+0x248,&native,sizeof(native)) || native != image+0x645e0 ||
            !executable(native,expected.size()) ||
            !read(native,actual.data(),actual.size()) || actual != expected) return false;
        context = os; entry = native;
        return true;
    }
};

// Call native even though the exact TGL implementation is a success stub.
// Registration does not establish resource backing or GPU completion.
struct TglNativeRegistrationInvoker {
    int operator()(uintptr_t entry, uintptr_t os, const void* resource,
                   bool write, bool read) const noexcept {
        using Register = int (*)(void*,const void*,uint32_t,uint32_t);
        return reinterpret_cast<Register>(entry)(reinterpret_cast<void*>(os),resource,
                                                 write ? 1u : 0u,read ? 1u : 0u);
    }
};

struct TglNativeResourceBinding {
    uintptr_t context = 0, allocate = 0, release = 0;
    // 132ec0..132ed6: renderer stores RenderHal at +2cc0 and borrows
    // RenderHal[0] as OS at +2cc8. Never acquire ownership of either object.
    // Caller must keep RenderHal alive through all resource destruction.
    template<class Read, class Qualify, class Executable>
    bool resolveFromRenderHal(uintptr_t renderHal, uintptr_t image, Read read,
                              Qualify qualify, Executable executable) {
        *this = {};
        uintptr_t os = 0;
        if (!renderHal || renderHal > std::numeric_limits<uintptr_t>::max() - sizeof(os) ||
            !read(renderHal, &os, sizeof(os))) return false;
        return resolve(os, image, read, qualify, executable);
    }
    // Must run under the owner's lifetime lock. Qualify must authenticate the
    // loaded native image; anchors alone are not an image identity check.
    template<class Read, class Qualify, class Executable>
    bool resolve(uintptr_t os, uintptr_t image, Read read, Qualify qualify,
                 Executable executable) {
        *this = {};
        constexpr auto max = std::numeric_limits<uintptr_t>::max();
        if (!os || !image || os > max - 0x208 || image > max - 0x63c10 ||
            !qualify(image)) return false;
        uintptr_t alloc = 0, free = 0;
        if (!read(os + 0x1f8, &alloc, sizeof(alloc)) ||
            !read(os + 0x200, &free, sizeof(free)) ||
            alloc != image + 0x63720 || free != image + 0x63c00 ||
            !executable(alloc, 16) || !executable(free, 16)) return false;
        constexpr std::array<uint8_t, 16> allocAnchor{
            0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x70,
            0x48,0x89,0x7d,0xf0,0x48,0x89,0x75,0xe8};
        constexpr std::array<uint8_t, 16> freeAnchor{
            0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x20,
            0x31,0xc0,0x89,0xc1,0x48,0x89,0x7d,0xf8};
        std::array<uint8_t, 16> a{}, f{};
        if (!read(alloc, a.data(), a.size()) || !read(free, f.data(), f.size()) ||
            a != allocAnchor || f != freeAnchor) return false;
        context = os; allocate = alloc; release = free;
        return true;
    }
};

// Exact allocator637b9/638f0 reads the image's imported page-size pointer
// at754130. Read the same 64-bit value used by type0; type1 uses its low DWORD.
// No host-page-size assumption, guessed symbol lookup, or output on failure.
template<class Read,class Qualify>
bool tglReadNativeResourcePageSize(uint64_t& output,uintptr_t image,
                                 Read read,Qualify qualify) {
    if(!image || image>UINTPTR_MAX-0x754138 || !qualify(image)) return false;
    uintptr_t address=0;
    uint64_t candidate=0;
    if(!read(image+0x754130,&address,8) || !address || address>UINTPTR_MAX-7 ||
       !read(address,&candidate,8) || !candidate || candidate>UINT32_MAX ||
       (candidate&(candidate-1))) return false;
    output=candidate;return true;
}
// Injection boundary only: no symbol lookup or unproved OS-object construction.
// Caller must validate native image, entrypoints and context lifetime separately.
class TglNativeResourceBackend {
public:
    using Storage = std::array<uint8_t, 0x148>;
    using Allocate = int (*)(void *, const void *, void *) noexcept;
    using Release = void (*)(void *, void *) noexcept;
    uint32_t resourceType() const noexcept {return type_;}
    TglNativeResourceBackend(void *context, Allocate allocate, Release release,
                             uint32_t resourceType, uint64_t nativePageSize = 0) noexcept
        : context_(context), allocate_(allocate), release_(release), type_(resourceType),
          pageSize_(nativePageSize) {}
    // Snapshot only: caller resolves binding against the exact native image
    // under its lifetime lock and retains OS/image through final release.
    // Public binding fields alone are NOT authentication. Incomplete snapshots
    // disable both allocation and release, including partial failure cleanup.
    TglNativeResourceBackend(const TglNativeResourceBinding& binding,
                            uint32_t resourceType,uint64_t nativePageSize) noexcept
        :TglNativeResourceBackend(
            binding.context && binding.allocate && binding.release ?
                reinterpret_cast<void*>(binding.context) : nullptr,
            binding.context && binding.allocate && binding.release ?
                reinterpret_cast<Allocate>(binding.allocate) : nullptr,
            binding.context && binding.allocate && binding.release ?
                reinterpret_cast<Release>(binding.release) : nullptr,
            resourceType,nativePageSize) {}
    int allocate(Storage &storage, const TglResourceKey &key,
                 uint32_t* effectiveWidth=nullptr) noexcept {
        if (!context_ || !allocate_ || !release_) return 5;
        if (type_ > 1 || key.compressed || key.compressionMode) return 25;
        if (!key.width || !key.height) return 5;
        if (type_ == 0 && !tglBufferSizeFits(key.width, pageSize_)) return 5;
        // Native638ed multiplies width*height in 32 bits;638fd rounds the
        // result in 32 bits. Reject both wraps before native mutates storage.
        // The same authenticated Darwin page-size lease is required for type1.
        if(type_==1) {
            const uint64_t bytes=uint64_t{key.width}*key.height;
            if(bytes>UINT32_MAX || !tglBufferSizeFits(uint32_t(bytes),pageSize_)) return 5;
        }
        // Exact known fields only; compression allocation ABI remains unproved.
        alignas(8) std::array<uint8_t, 0x50> params{};
        const auto put = [&](size_t offset, uint32_t value) {
            std::memcpy(params.data() + offset, &value, sizeof(value));
        };
        put(0, type_);
        put(0x14, key.width); // type0: requested byte count; type1: width
        put(0x18, key.height);
        put(0x24, key.tile);
        put(0x28, key.format);
        // Full4e060 consumes post-call params14 as surface+d8, while dc/130/e4
        // come from requested height/format/tile and e0 resets to0. This backend
        // owns only the148 resource: callers must not publish requested width
        // as effective surface width without capturing native's output params.
        // Its unsupported type/format branches are descriptor-only, not backing.
        const int status=allocate_(context_, params.data(), storage.data());
        // Caller-owned per-allocation output, never backend-global state shared
        // between resources. Failure preserves it, even if native changed params.
        // Width capture does not imply the returned descriptor has GPU backing.
        if(!status && effectiveWidth)
            std::memcpy(effectiveWidth,params.data()+0x14,4);
        return status;
    }
    bool backed(const Storage &storage) const noexcept { return tglNativeBackingPresent(storage); }
    void release(Storage &storage) noexcept {
        if (!context_ || !release_) return;
        for (auto byte : storage) if (byte != 0) {
            release_(context_, storage.data());
            return;
        }
    }
private:
    void *context_;
    Allocate allocate_;
    Release release_;
    uint32_t type_;
    uint64_t pageSize_;
};

template<class Backend> class TglOwnedResource {
public:
    using Storage = std::array<uint8_t, 0x148>;
    static_assert(noexcept(std::declval<Backend &>().allocate(
        std::declval<Storage &>(), std::declval<const TglResourceKey &>())),
        "allocation must report failure through status, not exceptions");
    static_assert(noexcept(std::declval<Backend &>().release(std::declval<Storage &>())),
        "release must not throw during unwind");
    static_assert(noexcept(std::declval<Backend &>().backed(std::declval<const Storage &>())),
        "backing inspection must not throw");
    enum class State { Empty, DescriptorOnly, Backed };
    struct Result { int status; State state; bool changed; };

    explicit TglOwnedResource(Backend &backend) noexcept : backend_(backend), storage_(localStorage_) {}
    // Borrow storage, not pre-existing handles. External storage and backend
    // outlive this owner; caller serializes all native access under its lease.
    TglOwnedResource(Backend &backend, Storage& emptyStorage) noexcept
        : backend_(backend), storage_(emptyStorage) {
        admitted_ = reinterpret_cast<uintptr_t>(storage_.data()) % 8 == 0;
        for (auto byte : storage_) if (byte) admitted_ = false;
    }
    ~TglOwnedResource() { reset(); }
    TglOwnedResource(const TglOwnedResource &) = delete;
    TglOwnedResource &operator=(const TglOwnedResource &) = delete;
    TglOwnedResource(TglOwnedResource &&) = delete;
    TglOwnedResource &operator=(TglOwnedResource &&) = delete;

    Result ensure(const TglResourceKey &key) noexcept {
        if (!admitted_ || busy_) return {5, State::Empty, false};
        struct Guard {
            bool& busy;
            explicit Guard(bool& b) noexcept:busy(b) {busy=true;}
            ~Guard() {busy=false;}
        } guard(busy_);
        if (!live_) for (auto byte : storage_)
            if (byte) return {5, State::Empty, false}; // never erase/adopt foreign handles
        if (live_ && state_ == State::Backed && key_ == key && backend_.backed(storage_))
            return {0, state_, false};
        // Match native ordering: retire old backing before replacement.
        retire();
        live_ = true; // partial allocation must also be released on failure
        const int status = backend_.allocate(storage_, key);
        if (status != 0) {
            retire();
            return {status, State::Empty, false}; // preserve allocation error
        }
        key_ = key;
        state_ = backend_.backed(storage_) ? State::Backed : State::DescriptorOnly;
        return {0, state_, true}; // native success is NOT proof of GPU backing
    }
    void reset() noexcept {
        if(busy_) return;
        busy_=true;retire();busy_=false;
    }
    // Synchronous mapping/initializer lease. Not an asynchronous GPU fence.
    // All native queries and user-supplied callbacks run with reentry blocked.
    template<class Invoke> int withStorage(Invoke invoke) noexcept {
        static_assert(noexcept(invoke(std::declval<const Storage&>())),"resource lease status ABI");
        if(busy_ || !live_ || state_!=State::Backed) return 5;
        busy_=true;
        const int status=backend_.backed(storage_) ? invoke(storage_) : 5;
        busy_=false;
        return status;
    }
    State state() const noexcept { return state_; }
    const Storage &storage() const noexcept { return storage_; }

private:
    void retire() noexcept {
        if (!live_) return;
        live_ = false;
        state_ = State::Empty;
        key_ = {};
        backend_.release(storage_);
        storage_.fill(0);
    }
    Backend &backend_; // must outlive this owner
    alignas(8) Storage localStorage_{};
    Storage& storage_;
    TglResourceKey key_{};
    bool live_ = false;
    State state_ = State::Empty;
    bool admitted_ = true;
    bool busy_ = false;
};

// Native state consumer may allocate resource30 itself. This owner arms before
// invocation, retains partial writes on failure, and releases exactly once
// after caller has discarded/completed every referencing command. Backend/OS
// outlive this scope; callers serialize it with command/fence ownership.
template<class Backend> class TglOwnedVeboxStateOwner {
    Backend& backend_;
    TglOwnedVeboxStateStorage packet_{};
    bool live_=false,busy_=false,prepared_=false;
public:
    explicit TglOwnedVeboxStateOwner(Backend& backend) noexcept :backend_(backend) {}
    ~TglOwnedVeboxStateOwner() {reset();}
    TglOwnedVeboxStateOwner(const TglOwnedVeboxStateOwner&)=delete;
    TglOwnedVeboxStateOwner& operator=(const TglOwnedVeboxStateOwner&)=delete;
    bool canPrepare() const noexcept {return !live_ && !busy_;}
    bool prepare(const std::array<uint8_t,0x188>& bytes) noexcept {
        if(!canPrepare()) return false;
        // Never adopt a foreign descriptor through a prepared CPU packet.
        for(size_t i=0x30;i<0x178;++i) if(bytes[i]) return false;
        std::memcpy(packet_.data(),bytes.data(),bytes.size());
        prepared_=true;return true;
    }
    template<class Invoke> int withNative(Invoke invoke) noexcept {
        static_assert(noexcept(invoke(std::declval<TglOwnedVeboxStateStorage&>())),
                      "state native call must report status without throwing");
        if(!prepared_ || busy_) return 5;
        busy_=true;live_=true;
        const int result=invoke(packet_);
        busy_=false;
        return result; // command discard/fence precedes release, including errors
    }
    void reset() noexcept {
        static_assert(noexcept(backend_.release(packet_.resource)),"state release must not throw");
        if(busy_) return;
        busy_=true;
        if(live_) {live_=false;backend_.release(packet_.resource);}
        packet_={};prepared_=false;busy_=false;
    }
};

// Owned representation of the proven Darwin 0x2a8 surface extent. Native
// OsResource is at +0x148; rotation and trailing fields start at +0x290.
// Not a Linux struct transplant and not a cast/adoption of an existing surface.
struct alignas(8) TglOwnedSurfaceStorage {
    std::array<uint8_t,0x148> prefix{};
    std::array<uint8_t,0x148> resource{};
    std::array<uint8_t,0x18> tail{};
};
static_assert(offsetof(TglOwnedSurfaceStorage,resource) == 0x148);
static_assert(offsetof(TglOwnedSurfaceStorage,tail) == 0x290);
static_assert(sizeof(TglOwnedSurfaceStorage) == 0x2a8);
// Source and its DN parameter allocation are separately leased by the frame
// owner. Equality checks prevent silently adopting an opaque source+90 pointer.
// Read only eight parameter bytes; automatic mode does not consume strength.
template<class Read>
bool tglAdmitOwnedDndiSource(TglOwnedDndiSource& output,
    const TglOwnedSurfaceStorage& source,bool denoiseNeeded,
    uintptr_t leasedDnParams,Read read) noexcept {
    TglOwnedDndiSource candidate;
    candidate.identity=reinterpret_cast<uintptr_t>(&source);
    std::memcpy(&candidate.dimensions.width,source.prefix.data()+0xd8,4);
    std::memcpy(&candidate.dimensions.height,source.prefix.data()+0xdc,4);
    if(denoiseNeeded) {
        uintptr_t params=0;
        std::memcpy(&params,source.prefix.data()+0x90,sizeof(params));
        if(!params || params!=leasedDnParams || params>UINTPTR_MAX-8) return false;
        std::array<uint8_t,8> bytes{};
        if(!read(params,bytes.data(),bytes.size())) return false;
        candidate.automatic=bytes[2]&1;
        if(!candidate.automatic) {
            float factor=0;std::memcpy(&factor,bytes.data()+4,sizeof(factor));
            candidate.strength=tglOwnedDnStrength(factor);
        }
    }
    output=candidate;
    return true;
}
// Common FFDI/FFDN frame metadata from Allocate1f7990. Copy no resource bytes,
// borrowed handle1a0, opaque90/b0, rotation, compression or dimensions here.
// Source lifetime/authentication and branch-specific link ownership remain
// caller obligations; this is not native12eb40's unsafe whole-shell copy.
inline void tglCopyOwnedSurfaceFrameMetadata(TglOwnedSurfaceStorage& target,
    const TglOwnedSurfaceStorage& source) noexcept {
    if(&target==&source) return;
    std::memcpy(target.prefix.data(),source.prefix.data(),4);
    std::memcpy(target.prefix.data()+0x30,source.prefix.data()+0x30,0x30);
    std::memcpy(target.prefix.data()+0x138,source.prefix.data()+0x138,4);
}
enum class TglOwnedFfdnHistoryDecision { Keep, Invalidate, CopyPrevious };
// Complete ICL1f7990 changed-FFDN branch. Source+200 in decompiler output
// is decimal200 (0xc8), a previous-surface link, NOT resource field0x200.
// No whole-shell copy or ownership transfer here. CopyPrevious still requires
// an independently retained previous-source lease and a safe copy destination.
inline TglOwnedFfdnHistoryDecision tglOwnedFfdnHistoryDecision(bool changed,
    bool referenceValid,bool executionFlag13,bool hasPrevious,
    uint32_t allocatedPitch,uint32_t previousPitch) noexcept {
    if(!changed) return TglOwnedFfdnHistoryDecision::Keep;
    return referenceValid && executionFlag13 && hasPrevious && allocatedPitch==previousPitch
        ? TglOwnedFfdnHistoryDecision::CopyPrevious : TglOwnedFfdnHistoryDecision::Invalidate;
}
// Backed subset of native4e060: type0 or type1/format3d. Other combinations
// must not be promoted from native's descriptor-only branch to a GPU surface.
// This owns the embedded resource and captures each allocation's effective
// width; source/history metadata and asynchronous fence ownership are separate.
// Do NOT use this owner for borrowed FFDI/FFDN shells: complete ICL1f87b0
// frees only inline STMM[2], statistics and conditional SFC resources, not those
// CPU-shell handles. Their source lease must be modeled independently; ordinary
// resource-owner release would incorrectly free borrowed source backing.
// TGL63c00 type1 release walks resource+0x60 tokens at +0x68/stride0x38,
// calling IOAccelResourceRelease (stub23e0c8), then CFRelease on resource+0x58
// (stub23e080), and zeros all0x148 bytes. Retaining only the IOSurface is NOT
// sufficient evidence that these separately released tokens remain alive.
template<class NativeBackend=TglNativeResourceBackend> class TglOwnedAllocatedSurface {
    using Storage=std::array<uint8_t,0x148>;
    struct Capture {
        NativeBackend& native;
        uint32_t width=0;
        int allocate(Storage& storage,const TglResourceKey& key) noexcept {
            return native.allocate(storage,key,&width);
        }
        bool backed(const Storage& storage) noexcept {return native.backed(storage);}
        void release(Storage& storage) noexcept {native.release(storage);width=0;}
    } capture_;
    TglOwnedSurfaceStorage surface_{};
    TglOwnedResource<Capture> resource_;
    TglResourceKey effectiveKey_{};
    bool ready_=false,busy_=false;
public:
    explicit TglOwnedAllocatedSurface(NativeBackend& native) noexcept
        :capture_{native},resource_(capture_,surface_.resource) {}
    ~TglOwnedAllocatedSurface() {reset();}
    TglOwnedAllocatedSurface(const TglOwnedAllocatedSurface&)=delete;
    TglOwnedAllocatedSurface& operator=(const TglOwnedAllocatedSurface&)=delete;
    int ensure(const TglResourceKey& key,bool* changed=nullptr) noexcept {
        if(busy_) return 5;
        // Native4e060 clears changed before admission/allocation. Only a
        // successfully published backed surface may trigger history rebuild.
        if(changed) *changed=false;
        const auto type=capture_.native.resourceType();
        if(type>1 || (type==1 && key.format!=0x3d)) return 25;
        busy_=true;
        if(ready_ && effectiveKey_==key && capture_.backed(surface_.resource)) {
            if(changed) *changed=false;
            busy_=false;return 0;
        }
        ready_=false;
        // Native4e060 compares surface+d8 (effective width), not the previous
        // request. Generic resource request-key reuse alone is insufficient.
        resource_.reset();
        const auto result=resource_.ensure(key);
        using State=typename TglOwnedResource<Capture>::State;
        if(result.status || result.state!=State::Backed) {
            resource_.reset();busy_=false;
            return result.status ? result.status : 5;
        }
        if(result.changed) {
            effectiveKey_=key;effectiveKey_.width=capture_.width;
            const uint32_t zero=0;
            std::memcpy(surface_.prefix.data()+0xd8,&capture_.width,4);
            std::memcpy(surface_.prefix.data()+0xdc,&key.height,4);
            std::memcpy(surface_.prefix.data()+0x130,&key.format,4);
            std::memcpy(surface_.prefix.data()+0xe4,&key.tile,4);
            std::memcpy(surface_.prefix.data()+0xe0,&zero,4);
        }
        if(changed) *changed=result.changed;
        ready_=true;busy_=false;return 0;
    }
    const TglOwnedSurfaceStorage* surface() const noexcept {return ready_ && !busy_ ? &surface_ : nullptr;}
    // Synchronous borrowed access keeps ALL native resource tokens alive by
    // pinning their owner, not by guessing which individual handles to retain.
    // The callback must not escape references or submit asynchronous work;
    // GPU/history retention still requires a completion-bound source lease.
    template<class Consume> bool withSurface(Consume&& consume) {
        if(!ready_ || busy_) return false;
        busy_=true;
        struct Unlock {bool& busy;~Unlock(){busy=false;}} unlock{busy_};
        std::forward<Consume>(consume)(static_cast<const TglOwnedSurfaceStorage&>(surface_));
        return true;
    }
    bool copyFrameMetadata(const TglOwnedSurfaceStorage& source) noexcept {
        if(!ready_ || busy_) return false;
        tglCopyOwnedSurfaceFrameMetadata(surface_,source);return true;
    }
    void reset() noexcept {
        if(busy_) return;
        busy_=true;ready_=false;resource_.reset();
        surface_.prefix.fill(0);surface_.tail.fill(0);busy_=false;
        effectiveKey_={};
    }
};
// Exact ICL classifier4a500 and its 90-entry relative jump table4a55c.
// 3 is the native OTHER color pack, not an invalid format admission result.
inline uint32_t tglOwnedChromaColorPack(uint32_t format) noexcept {
    switch (format) {
        case 0x19: case 0x1b: case 0x20: case 0x21: case 0x22:
        case 0x23: case 0x29: case 0x2a: case 0x2b: case 0x52: case 0x53: return 0;
        case 0xd: case 0xe: case 0xf: case 0x10: case 0x11: case 0x12:
        case 0x13: case 0x1e: case 0x24: case 0x25: return 1;
        case 1: case 2: case 3: case 4: case 5: case 6: case 7:
        case 0xa: case 0xb: case 0xc: case 0x14: case 0x15: case 0x16:
        case 0x17: case 0x26: case 0x50: case 0x51: case 0x55: case 0x5a: return 2;
        default: return 3;
    }
}
struct TglOwnedSfcAlignment { uint16_t width=1,height=1; };
// Exact Y window policy3fde2..3fffb. Nonzero requested window survives only
// the native format/mode branch. Zero selects4 for downscale,8 otherwise.
inline bool tglSelectOwnedSfcYWindow(float& output,uint32_t format,uint32_t mode,
    float scale,float requested) noexcept {
    if (!std::isfinite(scale) || scale<=0.f || !std::isfinite(requested) || requested<0.f)
        return false;
    bool listed=false;
    switch (format) {
        case 0xfffffffau: case 0x19: case 0x1b: case 0x1c: case 0x1e:
        case 0x53: case 0x52: case 0xfffffffcu: case 0x20: case 0x21:
        case 0x22: case 0x23: case 0x29: case 0x2a: case 0x2b: case 0x2c:
        case 0x24: case 0x25: case 0x27: case 0x28: case 0x26:
        case 0xfffffff9u: case 0xd: case 0xe: case 0xf: case 0x10:
        case 0x11: case 0x13: case 0x12: case 0x17: case 0x14: case 0x18:
            listed=true; break;
        default: break;
    }
    const bool packed=format==1 || format==2 || format==3 || format==4 ||
        format==0x50 || format==0x51 || format==0xfffffff8u || format==0x17;
    const bool configurable=(listed && mode!=1 && mode!=2) || (packed && mode==0);
    output=configurable ? (requested!=0.f ? requested : scale<1.f ? 4.f : 8.f) : 2.f;
    return true;
}
// 46be0..46c69 and 46c90..46d55. Math callback remains explicit: using a
// host libm here is not evidence of bit-identical Darwin math. Native
// stub23e4c4/GOT760608 resolves to_sinf (indirect symbol266). Constants are
// exact image bits24ce94=40490fdb and24ce98=3089705f.
template <typename NativeSin>
inline float tglOwnedSfcSinc(float x,NativeSin&& sine) noexcept {
    const float magnitude=x>0.f ? x : -x;
    if (magnitude<1.e-9f) return 1.f;
    return sine(x)/x;
}
template <typename NativeSin>
inline bool tglOwnedSfcWindowedSinc(float& output,float x,uint32_t taps,
    float window,NativeSin&& sine) noexcept {
    if ((taps!=4 && taps!=6 && taps!=8) || !std::isfinite(x) ||
        !std::isfinite(window) || window==0.f) return false;
    const float magnitude=x>0.f ? x : -x;
    if (magnitude>=float(taps/2)) { output=0.f; return true; }
    volatile float angle=3.1415927410125732421875f*x;
    const float first=tglOwnedSfcSinc(angle,sine);
    volatile float secondAngle=angle/window;
    const float second=tglOwnedSfcSinc(secondAngle,sine);
    const float candidate=first*second;
    if (!std::isfinite(candidate)) return false;
    output=candidate; return true;
}
// 46d60..46e3d with native caller400e9 parameter5: asymmetric support
// (-3,2], window clamped to at least3. Native math callback stays explicit.
template <typename NativeSin>
inline bool tglOwnedSfcFivePointSinc(float& output,float x,float window,
    NativeSin&& sine) noexcept {
    if (!std::isfinite(x) || !std::isfinite(window)) return false;
    if (window<3.f) window=3.f;
    if (x>2.f || -x>=3.f) { output=0.f; return true; }
    volatile float angle=3.1415927410125732421875f*x;
    const float first=tglOwnedSfcSinc(angle,sine);
    volatile float secondAngle=angle/window;
    const float second=tglOwnedSfcSinc(secondAngle,sine);
    const float candidate=first*second;
    if (!std::isfinite(candidate)) return false;
    output=candidate; return true;
}
// 40155..40213: mode0 sharpening kernel. Phase16 belongs to the first
// half; use mirrored phase distance afterwards. Preserve separate mul/add.
template <typename NativeSin>
inline bool tglBuildOwnedSfcSharpenKernels(std::array<std::array<float,3>,32>& output,
    float sharpening,NativeSin&& sine) noexcept {
    if (!std::isfinite(sharpening)) return false;
    std::array<std::array<float,3>,32> candidate{};
    for (uint32_t phase=0;phase<32;++phase) {
        const float distance=float(phase<=16 ? phase : 32-phase)/32.f;
        volatile float angle=3.1415927410125732421875f*distance;
        const float sinc=tglOwnedSfcSinc(angle,sine);
        volatile float side=(-sharpening)*sinc;
        volatile float product=2.f*sharpening;
        const float center=1.f+product;
        if (!std::isfinite(side) || !std::isfinite(center)) return false;
        candidate[phase]={{side,center,side}};
    }
    output=candidate; return true;
}
// 4022f..402ff: finite three-point convolution with zero outside the
// input. Preserve separate SSE multiply/add rounding, including edge taps.
template <size_t N>
inline bool tglConvolveOwnedSfcAvsPhase(std::array<float,N>& output,
    const std::array<float,N>& input,const std::array<float,3>& kernel) noexcept {
    static_assert(N==4 || N==8,"native AVS tap count");
    for (auto value:input) if (!std::isfinite(value)) return false;
    for (auto value:kernel) if (!std::isfinite(value)) return false;
    std::array<float,N> candidate{};
    for (size_t tap=0;tap<N;++tap) {
        float sum=0.f;
        for (int neighbor=-1;neighbor<=1;++neighbor) {
            const int index=int(tap)+neighbor;
            if (index<0 || index>=int(N)) continue;
            volatile float product=input[size_t(index)]*kernel[size_t(neighbor+1)];
            sum=product+sum;
        }
        if (!std::isfinite(sum)) return false;
        candidate[tap]=sum;
    }
    output=candidate; return true;
}
// 40304..40431: quantize using the original kernel sum (before optional
// convolution), then correct the appropriate center tap to total64.
template <size_t N>
inline bool tglQuantizeOwnedSfcAvsPhase(std::array<int32_t,N>& output,
    const std::array<float,N>& weights,float originalSum,uint32_t phase) noexcept {
    static_assert(N==4 || N==8,"native AVS tap count");
    if (phase>=32 || !std::isfinite(originalSum) || originalSum==0.f) return false;
    std::array<int32_t,N> candidate{};
    int64_t total=0;
    for (size_t i=0;i<N;++i) {
        volatile float product=64.f*weights[i];
        volatile float quotient=product/originalSum;
        const float rounded=std::floor(quotient+0.5f);
        if (!std::isfinite(rounded) || double(rounded)<double(INT32_MIN) ||
            double(rounded)>double(INT32_MAX)) return false;
        candidate[i]=int32_t(rounded); total+=candidate[i];
    }
    const size_t center=N/2-1+(phase>16 ? 1 : 0);
    const int64_t corrected=int64_t(candidate[center])+64-total;
    if (corrected<INT32_MIN || corrected>INT32_MAX) return false;
    candidate[center]=int32_t(corrected);
    output=candidate; return true;
}
// Exact UV left-sited producer40490..408a2: double phase/sum/quantization,
// float windowed-sinc. The native extrema arrays are diagnostic-only.
template <typename NativeSin>
inline bool tglBuildOwnedSfcLeftChromaTable(std::array<int32_t,128>& output,
    float window,float scale,NativeSin&& sine,int32_t phaseOffset=0) noexcept {
    if (!std::isfinite(scale) || scale<=0.f || !std::isfinite(window) || window<=0.f)
        return false;
    // 408b0 counterpart: same quantization, shifted origin, diameter6,
    // downscale window3 and signed (phase-offset)<=16 center selection.
    if (phaseOffset!=0 && phaseOffset!=8 && phaseOffset!=16) return false;
    const uint32_t diameter=phaseOffset ? 6 : 4;
    const double bounded=scale>1.f ? 1.0 : double(scale);
    if (bounded<1.0) window=phaseOffset ? 3.f : 2.f;
    std::array<int32_t,128> candidate{};
    for (uint32_t phase=0;phase<32;++phase) {
        std::array<double,4> weights{};
        double sum=0;
        const double base=-1.0+double(phaseOffset)/32.0;
        const double origin=base-double(phase)/32.0;
        for (size_t tap=0;tap<4;++tap) {
            float weight=0;
            if (!tglOwnedSfcWindowedSinc(weight,float((origin+double(tap))*bounded),
                diameter,window,sine)) return false;
            weights[tap]=double(weight); sum=weights[tap]+sum;
        }
        if (!std::isfinite(sum) || sum==0.0) return false;
        int64_t total=0;
        for (size_t tap=0;tap<4;++tap) {
            volatile double normalized=weights[tap]/sum;
            volatile double product=64.0*normalized;
            const double rounded=std::floor(0.5+product);
            if (!std::isfinite(rounded) || rounded<double(INT32_MIN) ||
                rounded>double(INT32_MAX)) return false;
            candidate[phase*4+tap]=int32_t(rounded); total+=int32_t(rounded);
        }
        const size_t center=phase*4+(int32_t(phase)-phaseOffset<=16 ? 1 : 2);
        const int64_t corrected=int64_t(candidate[center])+64-total;
        if (corrected<INT32_MIN || corrected>INT32_MAX) return false;
        candidate[center]=int32_t(corrected);
    }
    output=candidate; return true;
}
// Compose the proven 40014..40431 windowed branch. Format/window policy and
// Darwin sine qualification are caller obligations, not inferred here.
template <size_t Taps,typename NativeSin>
inline bool tglBuildOwnedSfcWindowedTable(std::array<int32_t,32*Taps>& output,
    float scale,float window,const std::array<std::array<float,3>,32>* convolution,
    NativeSin&& sine,bool symmetric=true) noexcept {
    static_assert(Taps==4 || Taps==8,"native AVS tap count");
    if (!std::isfinite(scale) || scale<=0.f || scale>1.f ||
        !std::isfinite(window) || window<=0.f) return false;
    std::array<int32_t,32*Taps> candidate{};
    for (uint32_t phase=0;phase<32;++phase) {
        std::array<float,Taps> weights{};
        float sum=0.f;
        const float origin=-float(Taps/2-1)-float(phase)/32.f;
        for (size_t tap=0;tap<Taps;++tap) {
            volatile float distance=origin+float(tap);
            volatile float scaled=distance*scale;
            const bool generated=symmetric ?
                tglOwnedSfcWindowedSinc(weights[tap],scaled,Taps,window,sine) :
                tglOwnedSfcFivePointSinc(weights[tap],scaled,window,sine);
            if (!generated) return false;
            sum=weights[tap]+sum;
        }
        if (convolution && !tglConvolveOwnedSfcAvsPhase(weights,weights,(*convolution)[phase]))
            return false;
        std::array<int32_t,Taps> quantized{};
        if (!tglQuantizeOwnedSfcAvsPhase(quantized,weights,sum,phase)) return false;
        for (size_t tap=0;tap<Taps;++tap) candidate[phase*Taps+tap]=quantized[tap];
    }
    output=candidate; return true;
}
// Compose3fd00 filtered path with the caller's fe9b5 scale cap. Unity
// bypass/cache selection remains the outer AVS dispatcher's responsibility.
template <typename NativeSin>
inline bool tglBuildOwnedSfcFilteredY(std::array<int32_t,256>& output,
    uint32_t format,uint32_t mode,float scale,float sharpening,float requestedWindow,
    bool symmetric,NativeSin&& sine) noexcept {
    float window=0;
    if (!tglSelectOwnedSfcYWindow(window,format,mode,scale,requestedWindow)) return false;
    const float bounded=scale>1.f ? 1.f : scale;
    std::array<std::array<float,3>,32> kernels{};
    if (mode==0 && !tglBuildOwnedSfcSharpenKernels(kernels,sharpening,sine)) return false;
    const auto* convolution=mode==0 ? &kernels : nullptr;
    std::array<int32_t,256> candidate{};
    if (mode==0) {
        if (!tglBuildOwnedSfcWindowedTable<8>(candidate,bounded,window,convolution,
            sine,symmetric)) return false;
    } else {
        std::array<int32_t,128> four{};
        if (!tglBuildOwnedSfcWindowedTable<4>(four,bounded,window,nullptr,
            sine,symmetric)) return false;
        for (size_t i=0;i<four.size();++i) candidate[i]=four[i];
    }
    output=candidate; return true;
}
// TGL 3f480..3f56f unity-scale coefficient producer. Caller clears the
// coefficient storage separately; this helper preserves all other entries.
// Phases 0..16 select the lower center tap; 17..31 select the upper center.
template <size_t N>
inline bool tglPrepareOwnedSfcUnityCoefficients(std::array<int32_t,N>& coefficients,
    bool fourTap,bool includeUpperPhases) noexcept {
    const size_t taps=fourTap ? 4 : 8;
    if (N<32*taps) return false;
    const size_t center=fourTap ? 1 : 3;
    for (size_t phase=0;phase<=16;++phase) coefficients[phase*taps+center]=64;
    if (includeUpperPhases)
        for (size_t phase=17;phase<32;++phase) coefficients[phase*taps+center+1]=64;
    return true;
}
// fe897..feadd / feb31..feda4: one axis. Native unity bypass precedes
// chroma-siting dispatch; force-filter preserves the filtered path at1.0.
template <typename NativeSin>
inline bool tglBuildOwnedSfcAvsAxis(std::array<int32_t,256>& y,
    std::array<int32_t,128>& uv,uint32_t format,float scale,uint32_t siting,
    bool vertical,bool forceFilter,bool symmetric,float sharpening,float window,
    NativeSin&& sine) noexcept {
    if (!std::isfinite(scale) || scale<=0.f) return false;
    const bool four=format==1 || format==2 || format==3 || format==4 ||
        format==0x50 || format==0x51 || format==0xfffffff8u;
    std::array<int32_t,256> candidateY{};
    std::array<int32_t,128> candidateUv{};
    if (scale==1.f && !forceFilter) {
        if (!tglPrepareOwnedSfcUnityCoefficients(candidateY,four,true) ||
            !tglPrepareOwnedSfcUnityCoefficients(candidateUv,true,true)) return false;
    } else {
        if (!tglBuildOwnedSfcFilteredY(candidateY,format,four ? 1u : 0u,scale,
            sharpening,window,symmetric,sine)) return false;
        const uint32_t left=vertical ? 0x10u : 1u;
        const uint32_t centered=vertical ? 0x20u : 2u;
        const int32_t offset=siting&left ? 0 : siting&centered ? 8 : 16;
        if (!tglBuildOwnedSfcLeftChromaTable(candidateUv,offset ? 3.f : 2.f,
            scale,sine,offset)) return false;
    }
    y=candidateY; uv=candidateUv; return true;
}
// fe77f..fe7c7: ordered float equality; no phase/force flag participates in
// this native early-return key. This predicate does not publish cache state.
inline bool tglOwnedSfcAvsCacheHit(uint32_t cachedFormat,float cachedX,float cachedY,
    uint32_t format,float scaleX,float scaleY) noexcept {
    return cachedFormat==format && cachedX==scaleX && cachedY==scaleY;
}
// Exact TGL fd9b0..fdf4e. Compact mode consumes four consecutive taps per
// phase (not the middle four of an eight-tap input), and pads both ends.
inline void tglPackOwnedSfcAvsY(std::array<uint32_t,128>& output,
    const std::array<int32_t,256>& horizontal,
    const std::array<int32_t,256>& vertical,uint32_t format,bool eightTap) noexcept {
    const bool compact=!eightTap && (format==1 || format==2 || format==3 ||
        format==4 || format==0x50 || format==0x51 || format==0xfffffff8u);
    const size_t taps=compact ? 4 : 8;
    std::array<uint32_t,128> candidate{};
    for (size_t phase=0;phase<32;++phase) {
        for (size_t tap=0;tap<taps;++tap) {
            const size_t slot=tap+(compact ? 2 : 0);
            const size_t word=phase*4+slot/2;
            const unsigned shift=unsigned(slot%2)*16;
            candidate[word]|=(uint32_t(horizontal[phase*taps+tap])&0xffu)<<shift;
            candidate[word]|=(uint32_t(vertical[phase*taps+tap])&0xffu)<<(shift+8);
        }
    }
    output=candidate;
}
// Exact TGL fd7d0..fd9ad: 32 phases, four signed coefficient dwords per
// axis. Only their low bytes survive; horizontal/vertical occupy alternating
// bytes of two output dwords. Native does not dereference its this argument.
inline void tglPackOwnedSfcAvsUv(std::array<uint32_t,64>& output,
    const std::array<int32_t,128>& horizontal,
    const std::array<int32_t,128>& vertical) noexcept {
    std::array<uint32_t,64> candidate{};
    for (size_t phase=0;phase<32;++phase) {
        for (size_t tap=0;tap<4;++tap) {
            const size_t word=phase*2+tap/2;
            const unsigned shift=unsigned(tap%2)*16;
            candidate[word]|=(uint32_t(horizontal[phase*4+tap])&0xffu)<<shift;
            candidate[word]|=(uint32_t(vertical[phase*4+tap])&0xffu)<<(shift+8);
        }
    }
    output=candidate;
}
struct TglOwnedSfcAvsTables {
    uint32_t format=0xffffffffu;
    float scaleX=0.f,scaleY=0.f;
    std::array<int32_t,256> yX{},yY{};
    std::array<int32_t,128> uvX{},uvY{};
    std::array<uint32_t,128> packedY{};
    std::array<uint32_t,64> packedUv{};
};
// fe77f..fee16 success-path composition; private candidate ensures failures
// cannot publish half-updated axes. This is stronger than native partial writes.
template <typename NativeSin>
inline bool tglUpdateOwnedSfcAvsTables(TglOwnedSfcAvsTables& tables,uint32_t format,
    float scaleX,float scaleY,uint32_t siting,bool forceFilter,bool symmetric,
    float sharpening,float window,NativeSin&& sine) noexcept {
    if (!std::isfinite(scaleX) || !std::isfinite(scaleY) || scaleX<=0.f || scaleY<=0.f)
        return false;
    if (tglOwnedSfcAvsCacheHit(tables.format,tables.scaleX,tables.scaleY,
        format,scaleX,scaleY)) return true;
    auto candidate=tables;
    const bool sameFormat=tables.format==format;
    const bool reuseX=sameFormat && (tables.scaleX==scaleX ||
        (tables.scaleX>1.f && scaleX>1.f));
    const bool reuseY=sameFormat && (tables.scaleY==scaleY ||
        (tables.scaleY>1.f && scaleY>1.f));
    if (!reuseX && !tglBuildOwnedSfcAvsAxis(candidate.yX,candidate.uvX,format,
        scaleX,siting,false,forceFilter,symmetric,sharpening,window,sine)) return false;
    if (!reuseY && !tglBuildOwnedSfcAvsAxis(candidate.yY,candidate.uvY,format,
        scaleY,siting,true,forceFilter,symmetric,sharpening,window,sine)) return false;
    tglPackOwnedSfcAvsY(candidate.packedY,candidate.yX,candidate.yY,format,false);
    tglPackOwnedSfcAvsUv(candidate.packedUv,candidate.uvX,candidate.uvY);
    candidate.format=format; candidate.scaleX=scaleX; candidate.scaleY=scaleY;
    tables=candidate; return true;
}
// AVS producer1e07d4..1e088c: source siting is updated only after initial
// phase extraction; zero-siting 420 overrides vertical phase to4 (horizontal0).
inline void tglPrepareOwnedSfcAvsPhases(std::array<uint8_t,12>& parameters,
    uint32_t& sourceSiting,uint32_t inputFormat) noexcept {
    const uint32_t horizontal=sourceSiting&2 ? 4u : sourceSiting&4 ? 8u : 0u;
    uint32_t vertical=sourceSiting&0x20 ? 4u : sourceSiting&0x40 ? 8u : 0u;
    if (!sourceSiting) {
        sourceSiting=0x11;
        if (tglOwnedChromaColorPack(inputFormat)==0) vertical=4;
    }
    std::memcpy(parameters.data()+4,&horizontal,4);
    std::memcpy(parameters.data()+8,&vertical,4);
}
// SFC output +88=1df350 and Gen12 input +80 thunk1f73a0 ->1f7330
// use the same classifier: 420 aligns both axes, 422 only width.
// Gen12 input accepts output format too but never reads that argument.
inline TglOwnedSfcAlignment tglOwnedSfcAlignment(uint32_t format) noexcept {
    const auto pack=tglOwnedChromaColorPack(format);
    return {uint16_t(pack==0 || pack==1 ? 2 : 1),uint16_t(pack==0 ? 2 : 1)};
}

// Exact Darwin chroma producer1fa603..673. Changes owned surface metadata
// only, not OS backing or GPU bytes. Color-pack classification is supplied
// by the qualified format classifier:0=420,1=422,2=444,3=other.
inline bool tglNormalizeOwnedChromaSiting(TglOwnedSurfaceStorage& surface,
                                        uint32_t colorPack) noexcept {
    if (colorPack > 3) return false;
    uint32_t siting=0;
    std::memcpy(&siting,surface.tail.data()+4,4);
    if (!siting) siting=0x21;
    if (colorPack == 1) siting=(siting&7)|0x10;
    else if (colorPack == 2) siting=0x11;
    std::memcpy(surface.tail.data()+4,&siting,4);
    return true;
}
// Exact Darwin1fa675..1faa18. Input siting has already been normalized.
// Only upsample fields are owned here; downsample and other state stay intact.
inline bool tglPrepareOwnedChromaUpsampling(uint32_t& state, uint32_t pack,
                                          uint32_t siting, bool needed, bool di) noexcept {
    if (pack > 3) return false;
    uint32_t candidate=(state&~0x41fu)|0x400u;
    if (needed) {
        const uint32_t masks[6]={0x21,0x22,0x11,0x12,0x41,0x42};
        for (uint32_t type=0;type<6;++type) {
            if ((siting&masks[type]) != masks[type]) continue;
            if (pack == 0 || (pack == 1 && (type==2 || type==3))) {
                const uint32_t horizontal=type&1;
                const uint32_t vertical=type<2 ? (di ? 2u : 1u) :
                                        type<4 ? 0u : (di ? 4u : 2u);
                candidate=(candidate&~0x41fu)|horizontal|(vertical<<2);
            }
            break; // Native ordered else-if chain, even with mixed siting bits.
        }
    }
    state=candidate;
    return true;
}
// Exact Darwin1faaa4..1fad78. Darwin enables downsampling only for pipe2;
// unlike later Linux code this does not add a deinterlace/non-YUY2 condition.
inline bool tglPrepareOwnedChromaDownsampling(uint32_t& state, uint32_t pack,
                                            uint32_t siting, uint32_t pipe) noexcept {
    if (pack > 3 || pipe > 2) return false;
    uint32_t candidate=(state&~0xbe0u)|0x800u;
    if (pipe == 2) {
        const uint32_t masks[6]={0x21,0x22,0x11,0x12,0x41,0x42};
        for (uint32_t type=0;type<6;++type) {
            if ((siting&masks[type]) != masks[type]) continue;
            if (pack==0 || (pack==1 && (type==2 || type==3))) {
                const uint32_t vertical=type<2 ? 1u : type<4 ? 0u : 2u;
                candidate=(candidate&~0xbe0u)|((type&1)<<5)|(vertical<<7);
            }
            break;
        }
    }
    state=candidate;
    return true;
}
// Owns only chroma fields of the state DWORD and metadata of owned surfaces.
// Native1fa55e initializes both directions before checking source; missing
// source skips target normalization, missing target preserves completed upsample.
inline bool tglPrepareOwnedChromaSampling(uint32_t& state,
                                        TglOwnedSurfaceStorage* source,
                                        TglOwnedSurfaceStorage* target,
                                        bool iecp, bool di, uint32_t pipe) noexcept {
    if (pipe>2) return false;
    state=(state&~0xfffu)|0xc00u;
    if (!source) return true;
    uint32_t format=0, siting=0;
    std::memcpy(&format,source->prefix.data()+0x130,4);
    const auto sourcePack=tglOwnedChromaColorPack(format);
    tglNormalizeOwnedChromaSiting(*source,sourcePack);
    std::memcpy(&siting,source->tail.data()+4,4);
    tglPrepareOwnedChromaUpsampling(state,sourcePack,siting,iecp,di);
    if (!target) return true;
    std::memcpy(&format,target->prefix.data()+0x130,4);
    const auto targetPack=tglOwnedChromaColorPack(format);
    tglNormalizeOwnedChromaSiting(*target,targetPack);
    std::memcpy(&siting,target->tail.data()+4,4);
    tglPrepareOwnedChromaDownsampling(state,targetPack,siting,pipe);
    return true;
}
// Closed state flags from Darwin1faecf..1faf2d and1fb086..1fb0f4.
// These are scalar inputs, not transplanted ICL object offsets. Other flags
// remain owned by the enclosing state producer until their sources are closed.
inline bool tglPrepareOwnedStateHistoryAndPipe(uint32_t& flags,
                                             bool referenceValid, bool dn, bool di,
                                             uint32_t pipe, bool skuSingleVeboxSlice) noexcept {
    if (pipe>2) return false;
    uint32_t candidate=flags&~0x6020u;
    if (!referenceValid && (dn || di)) candidate|=0x20;
    if (!skuSingleVeboxSlice && pipe==1) candidate|=0x2000;
    flags=candidate;
    return true;
}
// Exact ICL base+50 target1e6cb0..1e6d92, pure selection (no state writes).
// Semantic inputs avoid borrowing ICL layout for the owned TGL child.
inline bool tglSelectOwnedStateMode(uint32_t& mode, uint32_t pipe,
                                   bool exec19, bool exec14, uint32_t flags,
                                   uint32_t source138) noexcept {
    if (pipe>2) return false;
    const bool history=(flags&0x20)!=0;
    if (pipe==1 && !exec19)
        mode=(exec14 || history || source138==6 || source138==3 ||
              source138==1 || source138==0) ? 2u : 1u;
    else mode=(pipe==2 || history) ? 2u : 0u;
    return true;
}
// Darwin1faf78..1fb05f: base+50 return contributes only two bits; exec12
// contributes bit0. DN-only special input forces IECP, but a false special
// condition must not clear an IECP bit already chosen by earlier stages.
inline void tglPrepareOwnedStateDnControls(uint32_t& flags, uint32_t nativeMode,
                                          bool exec12, bool dnSpecial,
                                          uint32_t sourceType) noexcept {
    uint32_t candidate=(flags&~0x300c1u)|((nativeMode&3)<<6)|0x10000u;
    if (exec12) candidate|=1;
    const bool special=!(flags&0x10) && (flags&8) &&
                       (dnSpecial || sourceType==1 || sourceType==2);
    if (special) candidate|=0x20004;
    flags=candidate;
}
// Leading flags1faded..1faecd. primaryIeCp is the qualified semantic
// callback result, not a call through the incompatible TGL base+c8 slot.
inline bool tglPrepareOwnedStateLeadingFlags(uint32_t& flags, uint32_t pipe,
                                            bool primaryIeCp, bool di, bool dn) noexcept {
    if (pipe>2) return false;
    uint32_t candidate=flags&~0x8001cu;
    if (pipe==1 || pipe==2 || primaryIeCp) candidate|=4;
    if (di) candidate|=16;
    if (dn) candidate|=8;
    if (pipe==1 && (dn||di)) candidate|=0x80000;
    flags=candidate;
    return true;
}
struct TglOwnedStateInputs {
    uint32_t pipe=0;
    bool di=false, dn=false, referenceValid=false;
    bool exec19=false, exec14=false, exec12=false, dnSpecial=false;
    bool chromaIeCp=false, chromaDi=false, skuSingleVeboxSlice=false;
    TglOwnedSurfaceStorage* source=nullptr;
    TglOwnedSurfaceStorage* target=nullptr;
};
struct TglOwnedSfcLineBufferSizes {
    uint32_t avs=0, ief=0, sfd=0;
};
// Exact Darwin 1df4a4..1df59e prefix, not later Linux enum values.
// IECP takes priority over DI; only IECP changes the source siting field.
// Caller initializes the rest of the b8-byte packet and owns both arguments.
inline void tglPrepareOwnedSfcInputChroma(std::array<uint8_t,0xb8>& packet,
                                         uint32_t& sourceSiting,uint32_t inputFormat,
                                         bool iecp,bool di) noexcept {
    uint32_t subsampling=0;
    uint8_t eightTap=0;
    if (iecp) { subsampling=4; eightTap=1; sourceSiting=0x11; }
    else if (di) subsampling=2;
    else if (inputFormat==0x19 || inputFormat==0x53 || inputFormat==0x52)
        subsampling=1;
    else {
        const auto pack=tglOwnedChromaColorPack(inputFormat);
        if (pack==1) subsampling=2;
        else if (pack==2) { subsampling=4; eightTap=1; }
    }
    packet[0]=1;
    std::memcpy(packet.data()+8,&subsampling,4);
    packet[0x6a]=eightTap;
}
// Darwin 1df5a6..1df6a5: output siting phases and chroma downsampling mode.
// Preserve conditional no-write branches; full producer starts with zero packet.
inline void tglPrepareOwnedSfcOutputChroma(std::array<uint8_t,0xb8>& packet,
                                          uint32_t outputSiting,uint32_t outputFormat) noexcept {
    const uint32_t vertical=(outputSiting&0x20)?4:(outputSiting&0x40)?8:0;
    const uint32_t horizontal=(outputSiting&2)?4:(outputSiting&4)?8:0;
    std::memcpy(packet.data()+0x1c,&vertical,4);
    std::memcpy(packet.data()+0x20,&horizontal,4);
    uint32_t input=0,mode=0;
    std::memcpy(&input,packet.data()+8,4);
    const auto pack=tglOwnedChromaColorPack(outputFormat);
    if (input==4) {
        if (pack==0) mode=1;
        else if (pack==1) mode=2;
        else return;
    } else if (input==2) {
        if (pack==0) mode=3;
        else return;
    }
    std::memcpy(packet.data()+0x18,&mode,4);
}
// Darwin 1df785..1df81a integer alignment stage only. Offsets are native
// output height/width and converted source top/left; float conversion is upstream.
// Alignment units come from authenticated +88/+80 callbacks, not Linux enums.
inline bool tglPrepareOwnedSfcAlignedGeometry(std::array<uint8_t,0xb8>& packet,
    uint32_t outputHeight,uint32_t outputWidth,uint32_t sourceTop,uint32_t sourceLeft,
    uint16_t outputHeightAlign,uint16_t outputWidthAlign,
    uint16_t inputHeightAlign,uint16_t inputWidthAlign) noexcept {
    const std::array<uint32_t,4> values{{outputHeight,outputWidth,sourceTop,sourceLeft}};
    const std::array<uint32_t,4> units{{outputHeightAlign,outputWidthAlign,
                                      inputHeightAlign,inputWidthAlign}};
    std::array<uint32_t,4> aligned{};
    for (size_t i=0;i<4;++i) {
        const uint32_t unit=units[i];
        if (!unit || (unit&(unit-1)) ||
            values[i]>std::numeric_limits<uint32_t>::max()-(unit-1)) return false;
        aligned[i]=(values[i]+unit-1)&~(unit-1);
    }
    const std::array<size_t,4> offsets{{0x24,0x28,0x44,0x48}};
    for (size_t i=0;i<4;++i) std::memcpy(packet.data()+offsets[i],&aligned[i],4);
    return true;
}
// Darwin 1df81d..1df96f. Preserve single-precision sub/mul/add ordering;
// 315708 is float 0.5. Inputs bottom/right already underwent native truncation.
inline bool tglPrepareOwnedSfcRegions(std::array<uint8_t,0xb8>& packet,
    uint32_t bottom,uint32_t right,float top,float left,
    uint32_t frameHeight,uint32_t frameWidth,float scaleY,float scaleX,
    uint16_t inputHeightAlign,uint16_t inputWidthAlign,
    uint16_t outputHeightAlign,uint16_t outputWidthAlign) noexcept {
    const std::array<uint32_t,4> units{{inputHeightAlign,inputWidthAlign,
                                      outputHeightAlign,outputWidthAlign}};
    for (auto unit:units) if (!unit || (unit&(unit-1))) return false;
    auto convert=[](float value,uint32_t& result) noexcept {
        if (!std::isfinite(value) || value<0 || double(value)>double(UINT32_MAX)) return false;
        result=static_cast<uint32_t>(value); return true;
    };
    uint32_t height=0,width=0;
    if (!convert(float(bottom)-top,height) || !convert(float(right)-left,width)) return false;
    height=(height<frameHeight?height:frameHeight)&~(units[0]-1);
    width=(width<frameWidth?width:frameWidth)&~(units[1]-1);
    // Volatile intermediate prevents contraction into an FMA unlike native mulss/addss.
    const volatile float productY=scaleY*float(height),productX=scaleX*float(width);
    uint32_t scaledHeight=0,scaledWidth=0;
    if (!convert(productY+0.5f,scaledHeight) || !convert(productX+0.5f,scaledWidth) ||
        scaledHeight>UINT32_MAX-(units[2]-1) || scaledWidth>UINT32_MAX-(units[3]-1)) return false;
    scaledHeight=(scaledHeight+units[2]-1)&~(units[2]-1);
    scaledWidth=(scaledWidth+units[3]-1)&~(units[3]-1);
    const std::array<uint32_t,4> values{{height,width,scaledHeight,scaledWidth}};
    const std::array<size_t,4> offsets{{0x3c,0x40,0x4c,0x50}};
    for (size_t i=0;i<4;++i) std::memcpy(packet.data()+offsets[i],&values[i],4);
    return true;
}
// Darwin 1df972..1dfafd: rotation-dependent clipping, resulting scale ratios,
// and down-aligned destination offsets (already native-truncated upstream).
inline bool tglPrepareOwnedSfcRotationGeometry(std::array<uint8_t,0xb8>& packet,
    uint32_t rotation,uint32_t destinationTop,uint32_t destinationLeft,
    uint16_t outputHeightAlign,uint16_t outputWidthAlign) noexcept {
    if (!outputHeightAlign || (outputHeightAlign&(outputHeightAlign-1)) ||
        !outputWidthAlign || (outputWidthAlign&(outputWidthAlign-1))) return false;
    uint32_t frameH=0,frameW=0,sourceH=0,sourceW=0,scaledH=0,scaledW=0;
    auto read=[&](size_t offset,uint32_t& value) noexcept {
        std::memcpy(&value,packet.data()+offset,4);
    };
    read(0x24,frameH); read(0x28,frameW); read(0x3c,sourceH);
    read(0x40,sourceW); read(0x4c,scaledH); read(0x50,scaledW);
    if (!sourceH || !sourceW) return false; // no manufactured inf/NaN ratios
    const bool unrotated=rotation==0 || rotation==2 || rotation==4 || rotation==5;
    const uint32_t limitH=unrotated?frameH:frameW,limitW=unrotated?frameW:frameH;
    if (scaledH>=limitH) scaledH=limitH;
    if (scaledW>=limitW) scaledW=limitW;
    const float ratioX=float(scaledW)/float(sourceW),ratioY=float(scaledH)/float(sourceH);
    const uint32_t top=destinationTop&~(uint32_t(outputHeightAlign)-1);
    const uint32_t left=destinationLeft&~(uint32_t(outputWidthAlign)-1);
    std::memcpy(packet.data()+0x4c,&scaledH,4);
    std::memcpy(packet.data()+0x50,&scaledW,4);
    std::memcpy(packet.data()+0x54,&top,4);
    std::memcpy(packet.data()+0x58,&left,4);
    std::memcpy(packet.data()+0x5c,&ratioX,4);
    std::memcpy(packet.data()+0x60,&ratioY,4);
    return true;
}
// Darwin 1dfb00..1dfd48: exact format set, not the chroma classifier.
// Disable both bypass flags only for these formats when either original scale>1.
inline bool tglPrepareOwnedSfcBypass(std::array<uint8_t,0xb8>& packet,
                                     uint32_t inputFormat,float scaleX,float scaleY) noexcept {
    if (!std::isfinite(scaleX) || !std::isfinite(scaleY) || scaleX<0 || scaleY<0) return false;
    const bool listed=(inputFormat>=0xd && inputFormat<=0x14) ||
        (inputFormat>=0x17 && inputFormat<=0x19) || inputFormat==0x1b ||
        inputFormat==0x1c || inputFormat==0x1e ||
        (inputFormat>=0x20 && inputFormat<=0x2c) || inputFormat==0x52 ||
        inputFormat==0x53 || inputFormat==uint32_t(-4) ||
        inputFormat==uint32_t(-6) || inputFormat==uint32_t(-7);
    const uint8_t bypass=!(listed && (scaleX>1.0f || scaleY>1.0f));
    packet[0x64]=bypass; packet[0x65]=bypass;
    return true;
}
// Darwin 1dfd48..1dff21; rotation mapper 1dd080 table at 1dd0f8.
// Signed native rotation comparisons and conditional no-write at +70 preserved.
inline void tglPrepareOwnedSfcFilterRotation(std::array<uint8_t,0xb8>& packet,
    uint32_t inputFormat,uint32_t rotation,uint8_t scaling,uint8_t forceAvs) noexcept {
    const bool rgb=(inputFormat>=1 && inputFormat<=7) ||
        (inputFormat>=0xa && inputFormat<=0xc) || inputFormat==0x50 ||
        inputFormat==0x51 || inputFormat==0x55 || inputFormat==0x5a ||
        inputFormat==uint32_t(-8) || inputFormat==uint32_t(-9);
    packet[0x66]=rgb && (packet[0x6a]&1);
    packet[0x69]=(scaling&1)?1:(forceAvs&1);
    constexpr std::array<uint32_t,8> mapping{{0,1,2,3,4,5,3,1}};
    const uint32_t mapped=rotation<8?mapping[rotation]:0;
    uint32_t mode=0,mirror=0;
    if (rotation<=3 || rotation>=0x80000000u) {
        mode=mapped; packet[0x74]=0;
    } else {
        if (rotation<=5) mirror=mapped-4;
        else { mirror=4; mode=mapped; }
        packet[0x74]=1;
        std::memcpy(packet.data()+0x70,&mirror,4);
    }
    std::memcpy(packet.data()+0x6c,&mode,4);
}
// Darwin 1dffc3..1e0331. Converted sample is owned CPU data from the
// authenticated CSC producer; alpha is the original (unconverted) sample byte.
// Fill's YUV list additionally includes 15, unlike the bypass predicate.
inline void tglPrepareOwnedSfcFill(std::array<uint8_t,0xb8>& packet,bool enabled,
    uint32_t outputFormat,const std::array<uint8_t,4>& converted,uint8_t originalAlpha) noexcept {
    if (!enabled) return;
    const bool yuv=(outputFormat>=0xd && outputFormat<=0x15) ||
        (outputFormat>=0x17 && outputFormat<=0x19) || outputFormat==0x1b ||
        outputFormat==0x1c || outputFormat==0x1e ||
        (outputFormat>=0x20 && outputFormat<=0x2c) || outputFormat==0x52 ||
        outputFormat==0x53 || outputFormat==uint32_t(-4) ||
        outputFormat==uint32_t(-6) || outputFormat==uint32_t(-7);
    const bool direct=outputFormat==1 || outputFormat==2 || outputFormat==0x50;
    const std::array<uint8_t,3> channels=yuv ?
        std::array<uint8_t,3>{{converted[1],converted[0],converted[2]}} : direct ?
        std::array<uint8_t,3>{{converted[0],converted[1],converted[2]}} :
        std::array<uint8_t,3>{{converted[2],converted[1],converted[0]}};
    packet[0x75]=1;
    for (size_t i=0;i<3;++i) {
        const float value=float(channels[i])/255.0f;
        std::memcpy(packet.data()+0x78+i*4,&value,4);
    }
    const float alpha=float(originalAlpha)/255.0f;
    std::memcpy(packet.data()+0x84,&alpha,4);
}
// Owned counterpart of 1dff61..1dffc3 cache and fill output. Converter binds
// the authenticated native CSC ABI; false is fail-closed rather than cached.
class TglOwnedSfcFillCache {
    bool valid=false;
    std::array<uint8_t,4> original{},converted{};
    uint32_t sourceSpace=0,targetSpace=0;
public:
    void reset() noexcept { valid=false; original.fill(0); converted.fill(0); sourceSpace=targetSpace=0; }
    template<class Convert>
    bool prepare(std::array<uint8_t,0xb8>& packet,bool enabled,uint32_t outputFormat,
        const std::array<uint8_t,4>& sample,uint32_t source,uint32_t target,
        Convert convert) noexcept {
        static_assert(noexcept(convert(std::declval<std::array<uint8_t,4>&>(),sample,source,target)),
                      "CSC converter must report failure without exceptions");
        if (!enabled) return true;
        auto result=converted;
        const bool changed=!valid || original!=sample || sourceSpace!=source || targetSpace!=target;
        if (changed && !convert(result,sample,source,target)) return false;
        tglPrepareOwnedSfcFill(packet,true,outputFormat,result,sample[3]);
        if (changed) {
            original=sample; converted=result; sourceSpace=source; targetSpace=target; valid=true;
        }
        return true;
    }
};
// ICL +50 (1ddc70..1dde0e), after caller's float-to-integer conversion.
// Surface dc/d8 are height/width; rect 5c/58 are bottom/right, not extents.
// Native doubles all four for field input, clamps to max(bottom,16) /
// max(right,64), then aligns UP using SFC interface words +1a/+18.
// Reject malformed alignment/overflow instead of publishing wrapped geometry.
inline bool tglAdjustOwnedSfcFrame(uint32_t surfaceHeight,uint32_t surfaceWidth,
    uint32_t bottom,uint32_t right,bool fieldInput,
    uint16_t heightAlignment,uint16_t widthAlignment,
    uint32_t& height,uint32_t& width) noexcept {
    auto valid=[](uint32_t n) { return n && !(n&(n-1)); };
    if (!valid(heightAlignment) || !valid(widthAlignment)) return false;
    if (fieldInput) {
        for (auto n : {surfaceHeight,surfaceWidth,bottom,right})
            if (n>UINT32_MAX/2) return false;
        surfaceHeight*=2; surfaceWidth*=2; bottom*=2; right*=2;
    }
    uint32_t h=std::min(surfaceHeight,std::max(bottom,16u));
    uint32_t w=std::min(surfaceWidth,std::max(right,64u));
    if (h>UINT32_MAX-(heightAlignment-1u) ||
        w>UINT32_MAX-(widthAlignment-1u)) return false;
    height=(h+heightAlignment-1u)&~(heightAlignment-1u);
    width=(w+widthAlignment-1u)&~(widthAlignment-1u);
    return true;
}

// Native 1ddcda/1ddce7 convert float endpoints to int64 then retain low DWORD.
// Owned geometry admits only finite, nonnegative DWORD-domain endpoints; this
// avoids native indefinite/negative-wrap results without changing valid truncation.
inline bool tglAdjustOwnedSfcFloatFrame(uint32_t surfaceHeight,uint32_t surfaceWidth,
    float bottom,float right,bool fieldInput,
    uint16_t heightAlignment,uint16_t widthAlignment,
    uint32_t& height,uint32_t& width) noexcept {
    auto valid=[](float value) {
        return std::isfinite(value) && value>=0 && double(value)<=double(UINT32_MAX);
    };
    if (!valid(bottom) || !valid(right)) return false;
    return tglAdjustOwnedSfcFrame(surfaceHeight,surfaceWidth,
        static_cast<uint32_t>(bottom),static_cast<uint32_t>(right),fieldInput,
        heightAlignment,widthAlignment,height,width);
}

// Exact ICL base/Gen12 SFC +c0 slot is 1df3c0: a single DWORD zero
// at packet+4, not a no-op and not a GPU command producer.
inline void tglPrepareOwnedSfcPlatformField(std::array<uint8_t,0xb8>& packet) noexcept {
    const uint32_t zero=0;
    std::memcpy(packet.data()+4,&zero,4);
}

// Darwin 1df6ba..1df72b: scalar fields after the platform +c0 producer.
// frameWidth/Height are outputs of the separately authenticated +50 adjuster;
// this function neither guesses that geometry nor installs an ICL callback.
inline void tglPrepareOwnedSfcFrameFields(std::array<uint8_t,0xb8>& packet,
    uint32_t outputFormat,uint32_t inputFrameMode,
    uint32_t frameWidth,uint32_t frameHeight) noexcept {
    const float zero=0.0f;
    const uint32_t frameMode=inputFrameMode==1 ? 2u : 1u;
    std::memcpy(packet.data()+0x2c,&outputFormat,4);
    std::memcpy(packet.data()+0x10,&zero,4);
    std::memcpy(packet.data()+0x14,&zero,4);
    std::memcpy(packet.data()+0x38,&frameMode,4);
    std::memcpy(packet.data()+0x30,&frameWidth,4);
    std::memcpy(packet.data()+0x34,&frameHeight,4);
}

// SFC +60 is narrower than VEBOX's MMC predicate:1dd120 admits only
// 19/10/e. 1e0610 gates mode7, enabled low bit, surface299 low bit and e4==1.
// This computes metadata only; OS+3a8 invocation remains a separate ABI boundary.
inline bool tglPrepareOwnedSfcCompression(std::array<uint8_t,0xb8>& packet,
    uint32_t format,uint32_t executionMode,uint8_t mmcEnabled,
    uint8_t surfaceEnabled,uint32_t surfaceMode,float scaleY,float scaleX,
    bool& invokeOs) noexcept {
    invokeOs=false;
    if (!(format==0x19 || format==0x10 || format==0xe) || executionMode!=7 ||
        !(mmcEnabled&1) || !(surfaceEnabled&1) || surfaceMode!=1) return true;
    if (!std::isfinite(scaleY) || !std::isfinite(scaleX) || scaleY<0 || scaleX<0) return false;
    const uint32_t mode=scaleY>=0.5f && scaleX>=0.5f ? 1u :
                        scaleY<0.5f && scaleX<0.5f ? 2u : 0u;
    packet[0x8b]=mode!=0;
    std::memcpy(packet.data()+0x8c,&mode,4);
    invokeOs=true; // native calls even when selected mode is zero
    return true;
}

struct TglOwnedSfcGeometryInput {
    uint32_t inputFormat=0,outputFormat=0,inputFrameMode=0,rotation=0;
    uint32_t inputHeight=0,inputWidth=0,outputHeight=0,outputWidth=0;
    float top=0,left=0,bottom=0,right=0;
    float scaleY=1,scaleX=1;
    uint32_t destinationTop=0,destinationLeft=0;
    uint16_t frameHeightAlignment=1,frameWidthAlignment=1;
    bool fieldInput=false;
};
// Compose the closed geometry portion only. SFC interface alignment is borrowed
// explicitly; format-derived units are not substituted for hardware frame units.
inline bool tglPrepareOwnedSfcGeometry(std::array<uint8_t,0xb8>& packet,
    const TglOwnedSfcGeometryInput& in) noexcept {
    auto endpoint=[](float value,uint32_t& out) {
        if (!std::isfinite(value) || value<0 || double(value)>double(UINT32_MAX)) return false;
        out=static_cast<uint32_t>(value); return true;
    };
    uint32_t top=0,left=0,bottom=0,right=0,height=0,width=0;
    if (!endpoint(in.top,top) || !endpoint(in.left,left) ||
        !endpoint(in.bottom,bottom) || !endpoint(in.right,right) ||
        !tglAdjustOwnedSfcFrame(in.inputHeight,in.inputWidth,bottom,right,in.fieldInput,
            in.frameHeightAlignment,in.frameWidthAlignment,height,width)) return false;
    if (in.fieldInput) { bottom*=2; right*=2; } // adjuster checked these multiplications
    const auto input=tglOwnedSfcAlignment(in.inputFormat);
    const auto output=tglOwnedSfcAlignment(in.outputFormat);
    auto candidate=packet;
    tglPrepareOwnedSfcFrameFields(candidate,in.outputFormat,in.inputFrameMode,width,height);
    if (!tglPrepareOwnedSfcAlignedGeometry(candidate,in.outputHeight,in.outputWidth,top,left,
            output.height,output.width,input.height,input.width) ||
        !tglPrepareOwnedSfcRegions(candidate,bottom,right,in.top,in.left,height,width,
            in.scaleY,in.scaleX,input.height,input.width,output.height,output.width) ||
        !tglPrepareOwnedSfcRotationGeometry(candidate,in.rotation,in.destinationTop,
            in.destinationLeft,output.height,output.width)) return false;
    packet=candidate;
    return true;
}

// Darwin 1e0339..1e049f, alpha table 1e05f4: mode0 constant,
// mode2 background; modes1/3/unknown opaque. Fill alpha is produced upstream.
inline void tglPrepareOwnedSfcAlpha(std::array<uint8_t,0xb8>& packet,
    uint32_t outputFormat,bool hasAlpha,uint32_t alphaMode,float constantAlpha,
    bool fillEnabled,uint8_t csc,uint32_t inputColorSpace) noexcept {
    float alpha=1.0f;
    if (hasAlpha && (outputFormat==1 || outputFormat==3 || outputFormat==0x15)) {
        if (alphaMode==0) {
            alpha=constantAlpha;
            std::memcpy(packet.data()+0x84,&alpha,4);
        } else if (alphaMode==2) {
            if (fillEnabled) std::memcpy(&alpha,packet.data()+0x84,4);
        } else std::memcpy(packet.data()+0x84,&alpha,4);
    }
    std::memcpy(packet.data()+0xc,&alpha,4);
    packet[0x88]=packet[0x89]=csc&1;
    packet[0x8a]=inputColorSpace==1 || inputColorSpace==2;
}
// TGL129e3e/129e65 ignore surface/state producer returns;129ec3 (+70)
// is the first checked status. Owned callbacks must share this per-pass latch
// so an early failed producer cannot be followed by native command emission.
// Caller serializes begin/record/gate/close; this is NOT a GPU fence owner.
class TglOwnedVeboxProducerPass {
    bool active_=false,gated_=false,busy_=false,builder_=false;
    int firstError_=0;
public:
    TglOwnedVeboxProducerPass()=default;
    TglOwnedVeboxProducerPass(const TglOwnedVeboxProducerPass&)=delete;
    TglOwnedVeboxProducerPass& operator=(const TglOwnedVeboxProducerPass&)=delete;
    // Enclose the full authenticated native129ca0 invocation (all arguments
    // and their leases are supplied by the owning caller). Never reset the
    // latch between callbacks, including native early exits before slot70.
    // Native failures are preserved. Success additionally requires the owned
    // gate to have succeeded; an incomplete builder cannot authorize submit.
    // This does not release GPU resources or undo already emitted commands.
    template<class Invoke> int withBuilder(Invoke invoke) noexcept {
        static_assert(noexcept(invoke()),"native builder status ABI");
        if(!begin()) return 5;
        builder_=true;
        const int nativeResult=invoke();
        const int result=nativeResult ? nativeResult :
            !gated_ ? 5 : firstError_;
        builder_=false;
        close();
        return result;
    }
    bool begin() noexcept {
        if(active_) return false;
        active_=true;gated_=false;firstError_=0;return true;
    }
    bool record(int status) noexcept {
        if(!active_ || gated_ || busy_) return false;
        if(status && !firstError_) firstError_=status;
        return true;
    }
    template<class Produce> int produce(Produce producer) noexcept {
        static_assert(noexcept(producer()),"producer status ABI");
        if(!active_ || gated_ || busy_) return 5;
        if(firstError_) return firstError_;
        busy_=true;
        const int status=producer();
        busy_=false;
        record(status);
        return status;
    }
    template<class Produce> int gate(Produce produce) noexcept {
        static_assert(noexcept(produce()),"producer must report status without throwing");
        if(!active_ || gated_ || busy_) return 5;
        gated_=true;
        if(firstError_) return firstError_;
        busy_=true;
        firstError_=produce();
        busy_=false;
        return firstError_;
    }
    void close() noexcept {
        if(!busy_ && !builder_) {active_=gated_=false;firstError_=0;}
    }
};

// Native preparation and129ca0 invoke producer virtuals on their calling thread.
// Bind around the synchronous frame stages INSIDE the child owner's lifetime
// lease. No native-child cast, invented tail field, global address registry or
// async lifetime promise. The context and child must outlive the scope.
struct TglOwnedVeboxContextDomain {};
template<class Context> using TglOwnedVeboxScopedContext=
    TglOwnedScopedContext<Context,TglOwnedVeboxContextDomain>;

// One entry point for an authenticated synchronous native builder invocation.
// Owner pins child/table/SFC; scope resolves callbacks only on this thread;
// pass retains ignored producer failures through the checked DI gate.
// Invoke supplies the exact native builder ABI/arguments. No submit occurs here
// and successful return does not establish asynchronous resource lifetime.
template<class Owner,class Context,class Invoke>
int tglWithOwnedVeboxBuilder(Owner& owner,Context& context,Invoke invoke) noexcept {
    static_assert(noexcept(invoke(static_cast<void*>(nullptr))),"native builder status ABI");
    return owner.withPrivateChild([&](void* child) noexcept {
        TglOwnedVeboxScopedContext<Context> scope(child,context);
        if(!scope.bound()) return 5;
        return context.pass.withBuilder([&]() noexcept {return invoke(child);});
    });
}

// Preparation118 invokes f0/slot1d0 BEFORE command setup128/builder138.
// Keep its context and error latch inside the SAME child lease, with no nested
// scope or latch reset between stages. A native stage failure skips successors.
// Caller still authenticates preparation ABI and supplies resource leases;
// this orchestration alone does not construct a complete frame or submit it.
template<class Owner,class Context,class Prepare,class Setup,class Invoke>
int tglWithOwnedVeboxFrameStages(Owner& owner,Context& context,
    Prepare prepare,Setup setup,Invoke invoke) noexcept {
    static_assert(noexcept(prepare(static_cast<void*>(nullptr))),"native preparation status ABI");
    static_assert(noexcept(setup(static_cast<void*>(nullptr))),"native setup status ABI");
    static_assert(noexcept(invoke(static_cast<void*>(nullptr))),"native builder status ABI");
    return tglWithOwnedVeboxBuilder(owner,context,[&](void* child) noexcept {
        const int prepared=prepare(child);
        if(prepared) return prepared;
        const int status=setup(child);
        return status ? status : invoke(child);
    });
}

// Pin native execution outside child/context stages, so native preparation,
// DNDI publication and subsequent command construction share its lifetime.
// This is CPU lifetime serialization, not asynchronous fence ownership.
template<class ChildOwner,class ExecutionOwner,class Context,class Prepare,class Setup,class Invoke>
int tglWithOwnedVeboxExecutionFrame(ChildOwner& childOwner,ExecutionOwner& executionOwner,
    uintptr_t expectedExecution,Context& context,Prepare prepare,Setup setup,Invoke invoke) noexcept {
    return executionOwner.withExecution([&](uintptr_t actual) noexcept {
        if(actual!=expectedExecution) return 5;
        return tglWithOwnedVeboxFrameStages(childOwner,context,prepare,setup,invoke);
    });
}

// One synchronous lease covers setup128 AND builder138. Match native Submit:
// a setup error skips the builder entirely, without replacing the raw status.
// Command cleanup and completion leases remain the enclosing owner's duty.
template<class Owner,class Context,class Setup,class Invoke>
int tglWithOwnedVeboxPreparedBuilder(Owner& owner,Context& context,
    Setup setup,Invoke invoke) noexcept {
    static_assert(noexcept(setup(static_cast<void*>(nullptr))),"native setup status ABI");
    static_assert(noexcept(invoke(static_cast<void*>(nullptr))),"native builder status ABI");
    return tglWithOwnedVeboxFrameStages(owner,context,
        [](void*) noexcept {return 0;},setup,invoke);
}

// TGL129e34/129e5b/129eb9: status(this, bool, packet*), slots78/1d8/70.
// Resolve supplies a separately owned, leased context; NEVER cast a native
// child to a larger invented layout. The builder owner begins/closes pass
// around the entire native builder call, not inside individual callbacks.
// Context implements surfaces/state/diIecp(bool,void*) noexcept and owns pass.
// This binder is not permission to publish an incomplete child or submit GPU work.
template<class Resolve> struct TglOwnedVeboxProducerCallbacks {
    // Gen12 VeboxGetBeCSCMatrix is void(this, source, output). Preserve the
    // ABI; route owned admission errors through the enclosing pass latch.
    static void csc(void* child,void* source,void* output) noexcept {
        auto* context=Resolve::get(child);
        if(!context) return;
        (void)context->pass.produce([&]() noexcept {
            return source && output ? context->csc(source,output) : 5;
        });
    }
    // Native +68: status(this, signed kernel index). Context must retain
    // both child and execution owners; no foreign-object adoption here.
    static int setupKernel(void* child,int32_t index) noexcept {
        auto* context=Resolve::get(child);
        if(!context) return 5;
        return context->pass.produce([&]() noexcept {
            return context->setupKernel(index);
        });
    }
    // TGL1284f0: status(this, source, luma30, chroma24), slot1d0.
    // Native owns the stack packets. Context must qualify source and publish
    // only its leased execution data; no invented native-child tail cast.
    static int dndi(void* child,void* source,void* luma,void* chroma) noexcept {
        auto* context=Resolve::get(child);
        if(!context) return 5;
        return context->pass.produce([&]() noexcept {
            return source && luma && chroma ? context->dndi(source,luma,chroma) : 5;
        });
    }
    static int surfaces(void* child,bool enabled,void* packet) noexcept {
        auto* context=Resolve::get(child);
        if(!context) return 5;
        return context->pass.produce([&]() noexcept {
            return packet ? context->surfaces(enabled,packet) : 5;
        });
    }
    static int state(void* child,bool enabled,void* packet) noexcept {
        auto* context=Resolve::get(child);
        if(!context) return 5;
        return context->pass.produce([&]() noexcept {
            return packet ? context->state(enabled,packet) : 5;
        });
    }
    static int diIecp(void* child,bool enabled,void* packet) noexcept {
        auto* context=Resolve::get(child);
        if(!context) return 5;
        return context->pass.gate([&]() noexcept {
            return packet ? context->diIecp(enabled,packet) : 5;
        });
    }
};
// Kept separate until Context actually supplies a qualified DNDI producer.
// Installing the surface/state hooks must not silently claim slot1d0 support.
template<class Resolve>
std::array<uintptr_t,20> tglBindOwnedVeboxCscHook(std::array<uintptr_t,20> hooks) noexcept {
    hooks[14]=reinterpret_cast<uintptr_t>(&TglOwnedVeboxProducerCallbacks<Resolve>::csc);
    return hooks;
}
template<class Resolve>
std::array<uintptr_t,20> tglBindOwnedVeboxKernelSetupHook(std::array<uintptr_t,20> hooks) noexcept {
    hooks[5]=reinterpret_cast<uintptr_t>(&TglOwnedVeboxProducerCallbacks<Resolve>::setupKernel);
    return hooks;
}
template<class Resolve>
std::array<uintptr_t,20> tglBindOwnedVeboxDndiHook(std::array<uintptr_t,20> hooks) noexcept {
    hooks[17]=reinterpret_cast<uintptr_t>(&TglOwnedVeboxProducerCallbacks<Resolve>::dndi);
    return hooks;
}
template<class Resolve>
std::array<uintptr_t,20> tglBindOwnedVeboxProducerHooks(
        std::array<uintptr_t,20> hooks) noexcept {
    using Callbacks=TglOwnedVeboxProducerCallbacks<Resolve>;
    hooks[6]=reinterpret_cast<uintptr_t>(&Callbacks::diIecp);
    hooks[7]=reinterpret_cast<uintptr_t>(&Callbacks::surfaces);
    hooks[18]=reinterpret_cast<uintptr_t>(&Callbacks::state);
    return hooks;
}

// TGL129ca0 command tail 12a0a9..12a14c. Callers bind authenticated
// native MHW/owned-child arguments; this helper neither invents packets nor
// submits them. Preserve exact native status and short-circuit order.
template<class State, class Surfaces, class Sfc, class DiIecp>
int tglEmitOwnedVeboxCommandTail(uint32_t mode, State state, Surfaces surfaces,
                                Sfc sfc, DiIecp diIecp) noexcept {
    static_assert(noexcept(state()) && noexcept(surfaces()) &&
                  noexcept(sfc()) && noexcept(diIecp()),"command status ABI");
    int status=state();
    if (status) return status;
    status=surfaces();
    if (status) return status;
    if (mode==1) {
        status=sfc();
        if (status) return status;
    }
    return diIecp();
}
// Owned scheduling specification from ICL1e0dd0. This does NOT qualify an
// ICL object or its command bytes for TGL: each callback must be separately
// bound to authenticated TGL MHW ABI before runtime integration.
// Resource usage registration precedes this sequence in the owning caller.
template<class Commands>
int tglScheduleOwnedSfcCommands(uint8_t scaling,uint8_t forceAvs,
                                uint8_t ief,uint8_t csc,Commands &commands) {
    int status=commands.lock(); // MHW+10
    if (status) return status;
    status=commands.output(); // CPU output descriptor 1dcf30
    if (status) return status;
    status=commands.platform(); // owned child+c8
    if (status) return status;
    status=commands.state(); // MHW+18
    if (status) return status;
    if ((scaling|forceAvs)&1) {
        status=commands.avsState(); // MHW+20
        if (status) return status;
        status=commands.yTable(); // MHW+40 (not +38)
        if (status) return status;
        status=commands.uvTable(); // MHW+38
        if (status) return status;
    }
    if ((ief|csc)&1) {
        status=commands.iefState(); // MHW+30
        if (status) return status;
    }
    return commands.frameStart(); // MHW+28, literal mode1
}
struct TglOwnedSfcCommandParameters {
    std::array<uint8_t,8> lock{{1,0,0,0,0,0,0,0}};
    TglOwnedSfcStateParametersPacket state{};
    std::array<uint8_t,16> avs{};
    std::array<uint8_t,64> ief{};
    TglOwnedSfcYParameters y{};
    TglOwnedSfcUvParameters uv{};
};
// Compose the prepared CPU arguments with the typed native backend. Resource
// usage registration must already be complete; shell/owners must remain live.
// On any failure the caller must discard the partial command buffer, never
// submit it. Platform is an explicit qualified producer, NOT an assumed no-op.
template<class Backend,class Platform>
int tglSendOwnedSfcPreparedCommands(Backend &backend,
    const TglOwnedSfcCommandParameters &parameters,
    const std::array<uint8_t,0x2a8> &shell,uint8_t scaling,uint8_t forceAvs,
    uint8_t ief,uint8_t csc,Platform &platform) {
    struct Commands {
        Backend &backend;
        const TglOwnedSfcCommandParameters &parameters;
        const std::array<uint8_t,0x2a8> &shell;
        Platform &platformProducer;
        std::array<uint8_t,0x38> descriptor{};
        int lock(){return backend.lock(parameters.lock);}
        int output(){descriptor=tglOwnedSfcOutputDescriptor(shell);return 0;}
        int platform(){return platformProducer(descriptor);}
        int state(){return backend.state(parameters.state,descriptor);}
        int avsState(){return backend.avs(parameters.avs);}
        int yTable(){return backend.y(parameters.y);}
        int uvTable(){return backend.uv(parameters.uv);}
        int iefState(){return backend.ief(parameters.ief);}
        int frameStart(){return backend.frameStart();}
    } commands{backend,parameters,shell,platform,{}};
    return tglScheduleOwnedSfcCommands(scaling,forceAvs,ief,csc,commands);
}
// ICL 1e0ca0..1e0dcb setup order. Producers operate on owned state;
// caller binds the closed argument ABI, never an ICL object to TGL code.
// Flags are native renderData+1/+2/+3 and base+9b8, low bit only.
template<class PlatformState, class StateParameters, class Avs, class Ief>
int tglSetupOwnedSfc(uint8_t scalingFlag,uint8_t forceAvs,
                    uint8_t iefFlag,uint8_t cscFlag,
                    PlatformState platformState,StateParameters stateParameters,
                    Avs avs,Ief ief) noexcept {
    static_assert(noexcept(platformState()) && noexcept(stateParameters()) &&
                  noexcept(avs()) && noexcept(ief()),"SFC producers must report status");
    // +a8 platform hook (ICL 1e0c80 no-op), then +90 state producer
    // (ICL 1df3e0, zeroes b8 bytes at borrowed renderData+18).
    int status=platformState();
    if (status) return status;
    status=stateParameters();
    if (status) return status;
    if ((scalingFlag|forceAvs)&1) {
        status=avs();
        if (status) return status;
    }
    // Native 1e0dbb calls slot98 but does not assign EAX to saved status.
    if ((iefFlag|cscFlag)&1) (void)ief();
    return status;
}
// ICL derived InitRenderData 1f7450 allocates b8, but the owned TGL state
// must cover native168390's last pointer at f0..f7. Never use ICL's size.
// Backend returns writable, CPU-owned native allocation, never a GPU address.
// Reinitialization deliberately retires the old state even if allocation fails.
template<class Backend> class TglOwnedSfcStateParameters {
    TglTrackedCpuOwner<Backend> owner;
    bool leased=false,mutating=false;
public:
    explicit TglOwnedSfcStateParameters(Backend& backend) : owner(backend) {}
    ~TglOwnedSfcStateParameters() { reset(); }
    bool initialize() {
        if(leased || mutating) return false;
        mutating=true;
        owner.reset();
        if (!owner.allocate(sizeof(TglOwnedSfcStateParametersPacket))) {
            mutating=false; return false;
        }
        std::memset(reinterpret_cast<void*>(owner.get()),0,sizeof(TglOwnedSfcStateParametersPacket));
        mutating=false; return true;
    }
    bool prepare(const std::array<uint8_t,0xb8>& prefix,
        const TglOwnedSfcGen12Tail& tail,bool rgbAdaptive,bool eightTapChroma,
        bool compressionEnabled,TglOwnedSfcCompressionMode compressionMode) noexcept {
        if(leased || mutating || !owner.get()) return false;
        TglOwnedSfcStateParametersPacket candidate{};
        if(!tglPrepareOwnedSfcGen12Pass(candidate,prefix,tail,rgbAdaptive,
            eightTapChroma,compressionEnabled,compressionMode)) return false;
        std::memcpy(reinterpret_cast<void*>(owner.get()),candidate.data(),candidate.size());
        return true;
    }
    void reset() noexcept {
        if(leased || mutating) return;
        mutating=true; owner.reset(); mutating=false;
    }
    uint8_t* data() noexcept {
        return leased || mutating ? nullptr : reinterpret_cast<uint8_t*>(owner.get());
    }
    const uint8_t* data() const noexcept {
        return mutating ? nullptr : reinterpret_cast<const uint8_t*>(owner.get());
    }
    bool hasParametersLease() const noexcept {return leased && !mutating && owner.get();}
    template<class Use> int withParameters(Use&& use) noexcept {
        static_assert(noexcept(use(static_cast<const uint8_t*>(nullptr))),
            "SFC state borrower must not throw");
        if(leased || mutating || !owner.get()) return 5;
        leased=true;
        const int result=use(reinterpret_cast<const uint8_t*>(owner.get()));
        leased=false; return result;
    }
};

// ICL SFC base+28 uses 4d4d0(400,200): one c00-byte allocation,
// Y-X/Y-Y/UV-X/UV-Y pointers at 10/18/20/28. Owned header only;
// never pass it to native 4d600, which would free our tracked allocation.
template<class Backend> class TglOwnedSfcAvsParameters {
    TglTrackedCpuOwner<Backend> coefficients;
    std::array<uint8_t,0x30> header{};
    bool leased=false,mutating=false;
public:
    explicit TglOwnedSfcAvsParameters(Backend& backend) : coefficients(backend) {}
    ~TglOwnedSfcAvsParameters() { reset(); }
    bool initialize() {
        if(leased || mutating) return false;
        if (coefficients.get()) return true;
        mutating=true;
        if (!coefficients.allocate(0xc00)) { mutating=false; return false; }
        const uintptr_t base=coefficients.get();
        if (base>std::numeric_limits<uintptr_t>::max()-0xc00) {
            coefficients.reset(); mutating=false; return false;
        }
        header.fill(0);
        const uint32_t invalidFormat=0xffffffff;
        std::memcpy(header.data(),&invalidFormat,4);
        const std::array<uint64_t,4> pointers{{base,base+0x600,base+0x400,base+0xa00}};
        for (size_t i=0;i<pointers.size();++i)
            std::memcpy(header.data()+0x10+i*8,&pointers[i],8);
        mutating=false; return true;
    }
    void reset() noexcept {
        if(leased || mutating) return;
        mutating=true; header.fill(0); coefficients.reset(); mutating=false;
    }
    const std::array<uint8_t,0x30>* parameters() const noexcept {
        return !mutating && coefficients.get() ? &header : nullptr;
    }
    bool hasParametersLease() const noexcept {return leased && !mutating && coefficients.get();}
    // Native 4d4d0 layout: Y-X, UV-X, Y-Y, UV-Y share one backing.
    // Publish only under our synchronous borrow, never into a foreign object.
    bool publishTables(const TglOwnedSfcAvsTables& tables) noexcept {
        if(!leased || mutating || !coefficients.get()) return false;
        if(!std::isfinite(tables.scaleX) || !std::isfinite(tables.scaleY) ||
            tables.scaleX<=0.f || tables.scaleY<=0.f) return false;
        const auto candidate=tables;
        auto* base=reinterpret_cast<uint8_t*>(coefficients.get());
        std::memcpy(base,candidate.yX.data(),0x400);
        std::memcpy(base+0x400,candidate.uvX.data(),0x200);
        std::memcpy(base+0x600,candidate.yY.data(),0x400);
        std::memcpy(base+0xa00,candidate.uvY.data(),0x200);
        std::memcpy(header.data(),&candidate.format,4);
        std::memcpy(header.data()+4,&candidate.scaleX,4);
        std::memcpy(header.data()+8,&candidate.scaleY,4);
        return true;
    }
    // Synchronous CPU borrowing only; this does not certify GPU completion.
    template<class Use> int withParameters(Use&& use) noexcept {
        static_assert(noexcept(use(static_cast<const std::array<uint8_t,0x30>&>(header))),
            "AVS borrower must not throw");
        if(leased || mutating || !coefficients.get()) return 5;
        leased=true;
        const int result=use(static_cast<const std::array<uint8_t,0x30>&>(header));
        leased=false; return result;
    }
};
// Borrowed CPU view at the proven VP base offsets28/938. Never adopt existing
// pointers or leave borrowed owners installed after this synchronous scope.
// This does not construct/install a VP vtable or authorize a native invocation.
template<class CpuBackend,class AvsBackend,class Invoke>
int tglWithOwnedVpSfcCpuView(TglOwnedVpSfcCallerStorage& storage,
    const TglOwnedSfcStateParameters<CpuBackend>& state,
    const TglOwnedSfcAvsParameters<AvsBackend>& avs,Invoke invoke) noexcept {
    static_assert(noexcept(invoke(static_cast<void*>(nullptr))),"VP CPU view borrower must not throw");
    if(!state.hasParametersLease() || !avs.hasParametersLease()) return 5;
    for(size_t offset:{size_t(0x10),size_t(0x18),size_t(0x20)}) {
        uintptr_t borrowed=0;std::memcpy(&borrowed,storage.bytes.data()+offset,8);
        if(!borrowed) return 5;
    }
    uintptr_t oldState=0;
    std::memcpy(&oldState,storage.bytes.data()+0x938,8);
    if(oldState) return 5;
    std::array<uint8_t,0x30> oldHeader;
    std::memcpy(oldHeader.data(),storage.bytes.data()+0x28,oldHeader.size());
    for(size_t offset:{size_t(0x10),size_t(0x18),size_t(0x20),size_t(0x28)}) {
        uintptr_t existing=0;std::memcpy(&existing,oldHeader.data()+offset,8);
        if(existing) return 5;
    }
    const uintptr_t packet=reinterpret_cast<uintptr_t>(state.data());
    std::memcpy(storage.bytes.data()+0x28,avs.parameters()->data(),oldHeader.size());
    std::memcpy(storage.bytes.data()+0x938,&packet,8);
    const int result=invoke(storage.bytes.data());
    std::memcpy(storage.bytes.data()+0x28,oldHeader.data(),oldHeader.size());
    std::memcpy(storage.bytes.data()+0x938,&oldState,8);
    return result;
}
// Hold backing through coefficient generation, publication and synchronous
// command construction. Cache commits only after backing publication succeeds;
// command failure preserves that valid CPU cache, not a GPU-completion claim.
template<class Owner,class NativeSin,class Generate>
inline int tglWithOwnedSfcAvsTables(Owner& owner,TglOwnedSfcAvsTables& cache,
    uint32_t format,float scaleX,float scaleY,uint32_t siting,bool forceFilter,
    bool symmetric,float sharpening,float window,NativeSin&& sine,
    Generate&& generate) noexcept {
    static_assert(noexcept(generate(static_cast<const TglOwnedSfcAvsTables&>(cache))),
        "AVS command generator must not throw");
    return owner.withParameters([&](const auto&) noexcept {
        auto candidate=cache;
        if(!tglUpdateOwnedSfcAvsTables(candidate,format,scaleX,scaleY,siting,
            forceFilter,symmetric,sharpening,window,sine) ||
            !owner.publishTables(candidate)) return 5;
        cache=candidate;
        return generate(static_cast<const TglOwnedSfcAvsTables&>(cache));
    });
}
template<class Owner,class NativeSin,class Generate>
inline int tglWithOwnedSfcAvsCommands(TglOwnedSfcCommandParameters& parameters,
    Owner& owner,TglOwnedSfcAvsTables& cache,uint32_t format,float scaleX,float scaleY,
    uint32_t siting,bool forceFilter,bool symmetric,float sharpening,float window,
    NativeSin&& sine,Generate&& generate) noexcept {
    static_assert(noexcept(generate(parameters)),"AVS command generation status ABI");
    return tglWithOwnedSfcAvsTables(owner,cache,format,scaleX,scaleY,siting,
        forceFilter,symmetric,sharpening,window,sine,
        [&](const TglOwnedSfcAvsTables& ready) noexcept {
            // Preserve caller-qualified pipe modes/reserved bytes. These are
            // native CPU arguments, not raw GPU packets or a submission.
            parameters.y.coefficients=ready.packedY;
            parameters.uv.coefficients=ready.packedUv;
            return generate(parameters);
        });
}
// Darwin1df245/1df2c1: two linear byte buffers. Scalar dimensions only;
// do not copy ICL Format_Buffer3e into TGL (native TGL buffer enum3d).
inline bool tglOwnedSfcLineBufferSizes(TglOwnedSfcLineBufferSizes& output,
                                     uint32_t inputHeight, uint32_t scaledHeight) noexcept {
    constexpr auto max=std::numeric_limits<uint32_t>::max();
    if (!inputHeight || !scaledHeight || inputHeight>max/40 || scaledHeight>max/16)
        return false;
    // Pinned Intel media-driver2a32c7f mhw_sfc.h: threshold4000 (S3/S4
    // safety margin). Its ceil(height*64/10) macro performs INTEGER division
    // before ceil. Preserve that reference byte budget using wide arithmetic;
    // this is owned allocation policy, not a claim of a Darwin allocator ABI.
    const uint64_t sfd=scaledHeight>4000 ? uint64_t(scaledHeight)*64/10 : 0;
    if(sfd>max) return false;
    output={inputHeight*40,scaledHeight*16,uint32_t(sfd)};
    return true;
}
// Caller authenticates and retains the native execution object/image lease.
// Decode only closed scalar fields; preserve owned surface and child policy inputs.
// Builder129ca0 uses execution13 bit0, otherwise base slotd0 (71e80 FALSE).
// This helper is valid only for the private table preserving that exact slot.
// Reconcile this value with the callback bool rather than inventing DI policy.
template<class Read>
bool tglReadOwnedBuilderDiArgument(bool& output,uintptr_t child,
    uintptr_t execution,Read read) {
    if(!child || child>UINTPTR_MAX-0x8f || !execution || execution>UINTPTR_MAX-0x13)
        return false;
    uintptr_t actual=0;uint8_t flags=0;
    if(!read(child+0x88,&actual,8) || actual!=execution ||
        !read(execution+0x13,&flags,1)) return false;
    output=(flags&1)!=0;return true;
}
template<class Read>
bool tglReadOwnedStateExecution(TglOwnedStateInputs& output, uintptr_t execution,
                                bool diArgument, Read read) {
    constexpr auto max=std::numeric_limits<uintptr_t>::max();
    if (!execution || execution>max-0xa0f) return false;
    std::array<uint8_t,0x12> bytes{}; // native execution+8..19
    uint32_t pipe=0;
    if (!read(execution+8,bytes.data(),bytes.size()) ||
        !read(execution+0xa0c,&pipe,4) || pipe>2) return false;
    auto candidate=output;
    candidate.pipe=pipe;
    candidate.di=diArgument;
    candidate.referenceValid=(bytes[0]&1)!=0;
    candidate.dn=(bytes[3]&1)!=0;
    candidate.chromaIeCp=(bytes[7]&1)!=0;
    candidate.exec12=(bytes[0xa]&1)!=0;
    candidate.chromaDi=(bytes[0xb]&1)!=0;
    candidate.exec14=(bytes[0xc]&1)!=0;
    candidate.exec19=(bytes[0x11]&1)!=0;
    output=candidate;
    return true;
}
// Exact TGL1264bc..126506 copies RenderHal into base18 and RenderHal78 SKU
// pointer into base20. Validate that snapshot against held leases before reading
// SKU64. This resolves provenance only; consumers still qualify feature bits.
template<class Read>
bool tglReadOwnedVeboxSkuWord(uint64_t& output,uintptr_t child,
    uintptr_t renderHal,uintptr_t sku,Read read) {
    constexpr auto max=std::numeric_limits<uintptr_t>::max();
    if(!child || child>max-0x27 || !renderHal || renderHal>max-0x7f ||
        !sku || sku>max-0x6b) return false;
    uintptr_t actualRenderHal=0,baseSku=0,currentSku=0;
    uint64_t candidate=0;
    if(!read(child+0x18,&actualRenderHal,8) || actualRenderHal!=renderHal ||
        !read(child+0x20,&baseSku,8) || baseSku!=sku ||
        !read(renderHal+0x78,&currentSku,8) || currentSku!=sku ||
        !read(sku+0x64,&candidate,8)) return false;
    output=candidate;return true;
}

// Full ICL1fad80 final branch and pinned Gen12 SetupVeboxState2356 identify
// this Darwin SKU64 bit38 as FtrSingleVeboxSlice. Linux bitfield layout is
// NOT used: only its feature meaning is the reference.
template<class Read>
bool tglReadOwnedStateSingleVeboxSlice(TglOwnedStateInputs& output,uintptr_t child,
    uintptr_t renderHal,uintptr_t sku,Read read) {
    uint64_t word=0;
    if(!tglReadOwnedVeboxSkuWord(word,child,renderHal,sku,read)) return false;
    output.skuSingleVeboxSlice=((word>>38)&1)!=0;
    return true;
}

// Bridge only pointers whose leases are already held by the enclosing owned
// child. Never adopt arbitrary native source/target storage from the graph.
// ICL1fad80/1fa4e0 pin source=base1b0,target=execution60; TGL base getter48
// pins execution=base88. DN-special/SKU remain qualified semantic inputs: do
// not infer their TGL meaning merely from equal ICL member offsets.
template<class Read>
bool tglReadOwnedStateChildInputs(TglOwnedStateInputs& output,uintptr_t child,
    uintptr_t execution,TglOwnedSurfaceStorage& source,TglOwnedSurfaceStorage* target,
    bool diArgument,Read read) {
    constexpr auto max=std::numeric_limits<uintptr_t>::max();
    if(!child || child>max-0x1b7 || !execution || execution>max-0xa0f) return false;
    uintptr_t actualExecution=0,actualSource=0,actualTarget=0;
    if(!read(child+0x88,&actualExecution,8) || actualExecution!=execution ||
        !read(child+0x1b0,&actualSource,8) || actualSource!=reinterpret_cast<uintptr_t>(&source) ||
        !read(execution+0x60,&actualTarget,8) || actualTarget!=reinterpret_cast<uintptr_t>(target)) return false;
    auto candidate=output;
    if(!tglReadOwnedStateExecution(candidate,execution,diArgument,read)) return false;
    candidate.source=&source;candidate.target=target;
    output=candidate;return true;
}

// Pure owned-data composition of the closed Darwin state producer. Exact
// base+c8 (1e8390) reads the same exec0f as chroma IECP; primary+70 (1fa4b0)
// returns false. Do not expose contradictory duplicate callback inputs.
// Full1fad80 clears188 then writes only flags0/chroma4/bytec. In this mode,
// resource pointers10..28, inline30 and controls17c/180 stay zero: TGL171240
// therefore uses the assigned heap, not DummyIecpResource. This CPU producer
// alone still does not establish live backing, heap assignment or child ABI.
inline bool tglPrepareOwnedStatePacket(std::array<uint8_t,0x188>& output,
                                      const TglOwnedStateInputs& input) noexcept {
    if (input.pipe>2) return false;
    // Native mode selection/DN special can dereference currentSurface.
    if (!input.source) return false;
    uint32_t sourceType=0, source138=0;
    std::memcpy(&sourceType,input.source->prefix.data(),4);
    std::memcpy(&source138,input.source->prefix.data()+0x138,4);
    std::array<uint8_t,0x188> candidate{};
    uint32_t flags=0, mode=0, chroma=0;
    tglPrepareOwnedStateLeadingFlags(flags,input.pipe,input.chromaIeCp,input.di,input.dn);
    tglPrepareOwnedStateHistoryAndPipe(flags,input.referenceValid,input.dn,input.di,input.pipe,true);
    tglSelectOwnedStateMode(mode,input.pipe,input.exec19,input.exec14,flags,source138);
    tglPrepareOwnedStateDnControls(flags,mode,input.exec12,input.dnSpecial,sourceType);
    candidate[0xc]=0; // Exact primary+70 implementation1fa4b0..1fa4bd.
    tglPrepareOwnedChromaSampling(chroma,input.source,input.target,
                                 input.chromaIeCp,input.chromaDi,input.pipe);
    tglPrepareOwnedStateHistoryAndPipe(flags,input.referenceValid,input.dn,input.di,
                                     input.pipe,input.skuSingleVeboxSlice);
    std::memcpy(candidate.data(),&flags,4);
    std::memcpy(candidate.data()+4,&chroma,4);
    output=candidate;
    return true;
}
// Enclosing child holds all leases and its serialization lock. The temporal-DN
// policy is owned configuration, NOT an assumed read of ICL base1baa. Resolve
// links/SKU before normalization, then transfer the CPU packet to its native
// allocation owner. No heap assignment, command emission or submission here.
template<class Read>
bool tglComposeOwnedStateFromChild(std::array<uint8_t,0x188>& packet,
    uintptr_t child,uintptr_t execution,uintptr_t renderHal,uintptr_t sku,
    TglOwnedSurfaceStorage& source,TglOwnedSurfaceStorage* target,
    bool diArgument,bool disableTemporalDenoise,Read read) {
    TglOwnedStateInputs input;
    input.dnSpecial=disableTemporalDenoise;
    if(!tglReadOwnedStateChildInputs(input,child,execution,source,target,diArgument,read) ||
        !tglReadOwnedStateSingleVeboxSlice(input,child,renderHal,sku,read)) return false;
    return tglPrepareOwnedStatePacket(packet,input);
}
template<class Backend,class Read>
bool tglPrepareOwnedStateFromChild(TglOwnedVeboxStateOwner<Backend>& owner,
    uintptr_t child,uintptr_t execution,uintptr_t renderHal,uintptr_t sku,
    TglOwnedSurfaceStorage& source,TglOwnedSurfaceStorage* target,
    bool diArgument,bool disableTemporalDenoise,Read read) {
    if(!owner.canPrepare()) return false;
    std::array<uint8_t,0x188> packet{};
    return tglComposeOwnedStateFromChild(packet,child,execution,renderHal,sku,
        source,target,diArgument,disableTemporalDenoise,read) && owner.prepare(packet);
}
// Submit12a230 borrows state stack[-6f8,-570), passing it in R9 at12a4f3.
// Builder129ca0 forwards the SAME pointer to slot1d8 and later MHW+18.
// The scoped caller must supply that exact writable extent; no pointer probing
// or ownership adoption. Default state180=0 uses native heap resources, so
// this producer publishes CPU data only, NOT an inline allocation descriptor.
template<class Read>
int tglPublishOwnedStateToBuilder(TglOwnedVeboxStateStorage& leasedPacket,
    void* callbackPacket,uintptr_t child,uintptr_t execution,uintptr_t renderHal,
    uintptr_t sku,TglOwnedSurfaceStorage& source,TglOwnedSurfaceStorage* target,
    bool diArgument,bool disableTemporalDenoise,Read read) {
    if(callbackPacket!=static_cast<void*>(&leasedPacket)) return 5;
    // The caller may reuse packet storage only after its native inline
    // resource has been released. Never erase/adopt even a partial descriptor
    // left by a prior failed consumer; this CPU producer owns no OS allocator.
    for(auto byte:leasedPacket.resource) if(byte) return 5;
    std::array<uint8_t,0x188> candidate{};
    if(!tglComposeOwnedStateFromChild(candidate,child,execution,renderHal,sku,
        source,target,diArgument,disableTemporalDenoise,read)) return 5;
    std::memcpy(leasedPacket.data(),candidate.data(),candidate.size());
    return 0;
}

// Exact Darwin caller1f9111/26 packs compressed bit0 and mode DWORD into
// an eight-byte parameter. TGL179530 consumes mode+4 and calls native OS1a0;
// do not replace the whole callback with a superficially equivalent bit OR.
struct TglSurfaceControlParams {
    uint8_t compressed = 0;
    std::array<uint8_t,3> padding{};
    uint32_t mode = 0;
};
static_assert(offsetof(TglSurfaceControlParams,mode) == 4);
static_assert(sizeof(TglSurfaceControlParams) == 8);
inline TglSurfaceControlParams tglSurfaceControlParams(
        const TglOwnedSurfaceStorage& surface) noexcept {
    TglSurfaceControlParams params;
    params.compressed=surface.tail[0xa]&1;
    std::memcpy(&params.mode,surface.tail.data()+0xc,4);
    return params;
}
struct TglNativeSurfaceControlInvoker {
    int operator()(uintptr_t entry, uintptr_t mhw,
            const TglOwnedSurfaceStorage& surface, uint32_t& value) const noexcept {
        using Control = int (*)(void*,const TglSurfaceControlParams*,uint32_t*);
        const auto params=tglSurfaceControlParams(surface);
        return reinterpret_cast<Control>(entry)(reinterpret_cast<void*>(mhw),&params,&value);
    }
};
// Hook78's borrowed surface inputs, not hook70's a8-byte DI/IECP state.
// Native52d60 consumes five pointers and flag28; builder's stack reserves30.
struct TglOwnedSurfaceInputs {
    std::array<const TglOwnedSurfaceStorage*,5> surfaces{};
    uint8_t di = 0;
    std::array<uint8_t,7> padding{};
};
static_assert(offsetof(TglOwnedSurfaceInputs,di) == 0x28);
static_assert(sizeof(TglOwnedSurfaceInputs) == 0x30);
// Hook70 packet: TGL MHW vtable759520+28 ->172080 independently proves
// resource20..70, paired control78..a0 and final DWORDa4. Preserve unknown
// prefix as bytes; this is storage, NOT a complete command producer.
// Resource pointers are borrowed: their owners must outlive native emission.
struct TglOwnedDiIecpPacket {
    std::array<uint8_t,0x20> prefix{};
    std::array<const std::array<uint8_t,0x148>*,11> resources{};
    std::array<uint32_t,12> controls{};
};
static_assert(offsetof(TglOwnedDiIecpPacket,resources) == 0x20);
static_assert(offsetof(TglOwnedDiIecpPacket,controls) == 0x78);
static_assert(sizeof(TglOwnedDiIecpPacket) == 0xa8);
// ICL producer1f9006/17 emits start=0/end=boundaryWidth-1, independently
// consumed as fourteen-bit values by TGL172a2e..68. Do not silently wrap
// malformed dimensions. This initializes storage only; subsequent feature,
// registration, compression and resource producers are still required.
inline bool tglInitializeDiIecpPacket(TglOwnedDiIecpPacket& output,
                                    uint32_t boundaryWidth) noexcept {
    if (!boundaryWidth || boundaryWidth > 0x4000) return false;
    TglOwnedDiIecpPacket candidate;
    const uint32_t end = boundaryWidth-1;
    std::memcpy(candidate.prefix.data(),&end,sizeof(end));
    output = candidate;
    return true;
}
struct TglNativeDiIecpBinding {
    uintptr_t context = 0, entry = 0;
    template<class Read, class Qualify, class Executable>
    bool resolve(uintptr_t mhw, uintptr_t image, Read read, Qualify qualify,
                 Executable executable) {
        *this = {};
        // Exact image's primary table and consumer, not an interchangeable
        // ICL callback. The caller leases the borrowed MHW object throughout.
        if (!mhw || !image || mhw > UINTPTR_MAX-7 ||
            image > UINTPTR_MAX-0x759550 || !qualify(image)) return false;
        uintptr_t table=0, native=0;
        constexpr std::array<uint8_t,11> expected{
            0x55,0x48,0x89,0xe5,0x48,0x81,0xec,0x40,0x01,0x00,0x00};
        std::array<uint8_t,11> actual{};
        if (!read(mhw,&table,sizeof(table)) || table != image+0x759520 ||
            !read(table+0x28,&native,sizeof(native)) || native != image+0x172080 ||
            !executable(native,expected.size()) ||
            !read(native,actual.data(),actual.size()) || actual != expected) return false;
        context=mhw; entry=native;
        return true;
    }
};
struct TglNativeSurfaceControlBinding {
    uintptr_t context = 0, entry = 0;
    template<class Read, class Qualify, class Executable>
    bool resolve(uintptr_t mhw, uintptr_t image, Read read, Qualify qualify,
                 Executable executable) {
        *this = {};
        if (image > UINTPTR_MAX-0x759588) return false;
        TglNativeDiIecpBinding consumer;
        if (!consumer.resolve(mhw,image,read,qualify,executable)) return false;
        uintptr_t native=0;
        constexpr std::array<uint8_t,8> expected{
            0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x50};
        std::array<uint8_t,8> actual{};
        if (!read(image+0x759580,&native,sizeof(native)) || native != image+0x179530 ||
            !executable(native,expected.size()) ||
            !read(native,actual.data(),actual.size()) || actual != expected) return false;
        context=mhw; entry=native;
        return true;
    }
};
// Native builder12a14c calls MHW+28 with exactly these three arguments.
// Binding/image authentication, backing and complete packet admission belong
// to the caller; this invoker neither creates resources nor changes status.
struct TglNativeDiIecpInvoker {
    int operator()(uintptr_t entry, uintptr_t mhw, void* commandBuffer,
                   const TglOwnedDiIecpPacket& packet) const noexcept {
        using Emit = int (*)(void*,void*,const TglOwnedDiIecpPacket*);
        return reinterpret_cast<Emit>(entry)(reinterpret_cast<void*>(mhw),
                                             commandBuffer,&packet);
    }
};
// Gen12 GetSurfOutput decision tree, independent of Darwin object offsets.
// TGL mode2/target60 is pinned separately; mode1 is SFC. Caller must decode
// and authenticate execution fields, and lease/back the selected surface.
struct TglOutputSurfaceSelection {
    const TglOwnedSurfaceStorage* surface = nullptr;
    bool valid = false;
};
struct TglSurfaceIndices {
    int32_t frame0 = 0, frame1 = 0, dnOut = 0, historyIn = 0, historyOut = 0;
};
struct TglDiIecpOutputs {
    const TglOwnedSurfaceStorage* current = nullptr;
    const TglOwnedSurfaceStorage* previous = nullptr;
    uint32_t currentOffset = 0;
};
// ICL1f89a0 output helper agrees with pinned Gen12 logical policy. Only
// packet48/50/10 are transported; never copy ICL C++ object offsets/vtables.
// Compression/control/registration remain separate required producer stages.
inline bool tglSelectDiIecpOutputs(TglDiIecpOutputs& output, uint32_t pipe,
        bool di, bool iecp, const TglSurfaceIndices& indices,
        const TglOwnedSurfaceStorage* target,
        const std::array<const TglOwnedSurfaceStorage*,4>& ffdi) noexcept {
    if (pipe > 2) return false;
    TglDiIecpOutputs candidate;
    if (pipe == 2) {
        if (!target) return false;
        candidate.current = target;
        std::memcpy(&candidate.currentOffset,target->prefix.data()+0x144,4);
    } else if (di) {
        if (indices.frame0 < 0 || indices.frame0 >= 4 ||
            indices.frame1 < 0 || indices.frame1 >= 4 ||
            !ffdi[indices.frame0] || !ffdi[indices.frame1]) return false;
        candidate.current = ffdi[indices.frame1];
        candidate.previous = ffdi[indices.frame0];
    } else if (iecp) {
        if (indices.dnOut < 0 || indices.dnOut >= 2 || !ffdi[indices.dnOut]) return false;
        candidate.current = ffdi[indices.dnOut];
    }
    output = candidate;
    return true;
}
struct TglDiIecpInputs {
    uint32_t boundaryWidth = 0, pipe = 0;
    bool di = false, iecp = false, referenceValid = false;
    bool dnNeeded = false, stmmNeeded = false;
    TglSurfaceIndices indices{};
    const TglOwnedSurfaceStorage* current = nullptr;
    const TglOwnedSurfaceStorage* previous = nullptr;
    const TglOwnedSurfaceStorage* target = nullptr;
    const TglOwnedSurfaceStorage* statistics = nullptr;
    std::array<const TglOwnedSurfaceStorage*,4> ffdi{};
    std::array<const TglOwnedSurfaceStorage*,2> ffdn{}, stmm{};
    std::array<uint32_t,11> controls{};
};
// Owned logical policy reconstructed from exact Darwin1f88d0/1f7500.
// Scalar decisions only; no ICL object offsets, vtables or GPU commands.
inline bool tglOwnedMmcFormatSupported(uint32_t format) noexcept {
    switch (format) {
        case 0x19: case 0xd: case 0xe: case 0x10: case 0xf: case 0x11:
        case 0x15: case 0x14: case 3: case 5: case 0x53: case 0x13: return true;
        default: return false;
    }
}
struct TglDiIecpControlPolicy {
    bool mmcEnabled = false;
    uint32_t executionMode = 0;
    const TglOwnedSurfaceStorage* target = nullptr;
    int operator()(const TglOwnedSurfaceStorage& surface,size_t slot,
                   bool directTarget,bool& enabled) const noexcept {
        enabled=false;
        if (slot > 6 || (directTarget && (slot != 5 || target != &surface))) return 5;
        if (!directTarget) { enabled=mmcEnabled; return 0; }
        uint32_t format=0;
        std::memcpy(&format,surface.prefix.data()+0x130,4);
        const auto params=tglSurfaceControlParams(surface);
        enabled=mmcEnabled && tglOwnedMmcFormatSupported(format) &&
                executionMode == 7 && params.mode == 1;
        return 0;
    }
};
// Producer orchestration, not native object-layout transplantation. Darwin
// ICL1f8ec0/1f89a0 supplies the branch/order evidence, TGL172080 the packet ABI.
// Caller leases every surface. Admit verifies backing/identity; control must
// implement qualified native compression policy, never silently omit it.
// Register/control side effects cannot be rolled back here (native also exits
// at first error), but no partial packet is published or emitted on failure.
template<class Admit, class Register, class Control>
int tglPrepareDiIecpPacket(TglOwnedDiIecpPacket& output,
        const TglDiIecpInputs& input, Admit admit, Register registerResource,
        Control control) {
    TglOwnedDiIecpPacket candidate;
    TglDiIecpOutputs selected;
    if (!tglInitializeDiIecpPacket(candidate,input.boundaryWidth) ||
        !tglSelectDiIecpOutputs(selected,input.pipe,input.di,input.iecp,
            input.indices,input.target,input.ffdi) || !input.current ||
        !input.statistics || (input.referenceValid && !input.previous)) return 5;
    struct Step { size_t slot; const TglOwnedSurfaceStorage* surface; bool write; bool directTarget; };
    std::array<Step,10> steps{};
    size_t count=0;
    auto add = [&](size_t slot,const TglOwnedSurfaceStorage* surface,bool write,bool directTarget=false) {
        steps[count++]={slot,surface,write,directTarget};
    };
    add(0,input.current,false);
    if (input.referenceValid) add(1,input.previous,false);
    if (selected.current) add(5,selected.current,true,input.pipe == 2);
    if (selected.previous) add(6,selected.previous,true);
    if (input.dnNeeded) {
        if (input.indices.dnOut < 0 || input.indices.dnOut >= 2 ||
            !input.ffdn[input.indices.dnOut]) return 5;
        add(4,input.ffdn[input.indices.dnOut],true);
        if (input.pipe == 1 && !input.di) {
            if (!input.ffdi[input.indices.dnOut]) return 5;
            add(5,input.ffdi[input.indices.dnOut],true);
        }
    }
    if (input.di || input.stmmNeeded) {
        if (input.indices.historyIn < 0 || input.indices.historyIn >= 2 ||
            input.indices.historyOut < 0 || input.indices.historyOut >= 2 ||
            !input.stmm[input.indices.historyIn] || !input.stmm[input.indices.historyOut]) return 5;
        add(2,input.stmm[input.indices.historyIn],false);
        add(3,input.stmm[input.indices.historyOut],true);
    }
    add(7,input.statistics,true);
    for (size_t i=0;i<count;++i) {
        const int result=admit(*steps[i].surface);
        if (result) return result;
    }
    uint32_t offset=0;
    std::memcpy(&offset,input.current->prefix.data()+0x144,4);
    std::memcpy(candidate.prefix.data()+8,&offset,4);
    if (input.referenceValid) {
        std::memcpy(&offset,input.previous->prefix.data()+0x144,4);
        std::memcpy(candidate.prefix.data()+0xc,&offset,4);
    }
    std::memcpy(candidate.prefix.data()+0x10,&selected.currentOffset,4);
    for (size_t i=0;i<count;++i) {
        const auto& step=steps[i];
        int result=registerResource(step.surface->resource,step.write,true);
        if (result) return result;
        uint32_t value=input.controls[step.slot];
        if (step.slot != 7) { // native statistics has no compression-control call
            result=control(*step.surface,value,step.slot,step.directTarget);
            if (result) return result;
        }
        candidate.resources[step.slot]=&step.surface->resource;
        candidate.controls[step.slot]=value;
    }
    output=candidate;
    return 0;
}
// Submit12a4de leases the DI/IECP argument at stack-3d0; builder129ea1
// passes the same pointer to checked slot70. Registration/control are native
// side effects: preserve their exact failure and leave packet unchanged, but
// do not claim rollback of OS registration. Resource/fence leases stay with
// the enclosing child until every referencing command is discarded/completed.
template<class Admit,class Register,class Control>
int tglPublishOwnedDiIecpToBuilder(TglOwnedDiIecpPacket& leasedPacket,
    void* callbackPacket,const TglDiIecpInputs& input,Admit admit,
    Register registerResource,Control control) {
    if(callbackPacket!=static_cast<void*>(&leasedPacket)) return 5;
    return tglPrepareDiIecpPacket(leasedPacket,input,admit,registerResource,control);
}
struct TglNativeDiIecpServices {
    TglNativeRegistrationBinding registration{};
    TglNativeSurfaceControlBinding control{};
    template<class Read, class Qualify, class Executable>
    bool resolve(uintptr_t os, uintptr_t mhw, uintptr_t image, Read read,
                 Qualify qualify, Executable executable) {
        *this={};
        TglNativeDiIecpServices candidate;
        if (!candidate.registration.resolve(os,image,read,qualify,executable) ||
            !candidate.control.resolve(mhw,image,read,qualify,executable)) return false;
        *this=candidate;
        return true;
    }
};
// The compression policy must distinguish native direct-output predicate80
// from global MMC policy for intermediate/input/history surfaces. No default
// "compression disabled" fallback; caller must supply qualified policy.
template<class Admit, class ControlPolicy>
int tglPrepareNativeDiIecpPacket(TglOwnedDiIecpPacket& output,
        const TglDiIecpInputs& input,const TglNativeDiIecpServices& services,
        Admit admit,ControlPolicy policy) {
    if (!services.registration.context || !services.registration.entry ||
        !services.control.context || !services.control.entry) return 5;
    return tglPrepareDiIecpPacket(output,input,admit,
        [&](const auto& resource,bool write,bool read) {
            return TglNativeRegistrationInvoker{}(services.registration.entry,
                services.registration.context,&resource,write,read);
        },
        [&](const auto& surface,uint32_t& value,size_t slot,bool directTarget) {
            bool enabled=false;
            const int result=policy(surface,slot,directTarget,enabled);
            if (result || !enabled) return result;
            return TglNativeSurfaceControlInvoker{}(services.control.entry,
                services.control.context,surface,value);
        });
}
// TGL12b9a0 writes these five execution fields. No pointer adoption or state
// mutation; failed reads/invalid indices preserve the caller's snapshot.
template<class Read>
bool tglReadSurfaceIndices(uintptr_t execution, TglSurfaceIndices& result, Read read) {
    if (!execution || execution > UINTPTR_MAX-0x33) return false;
    TglSurfaceIndices candidate;
    if (!read(execution+0x1c,&candidate.frame0,4) ||
        !read(execution+0x20,&candidate.frame1,4) ||
        !read(execution+0x28,&candidate.dnOut,4) ||
        !read(execution+0x2c,&candidate.historyIn,4) ||
        !read(execution+0x30,&candidate.historyOut,4)) return false;
    if (candidate.frame0 < 0 || candidate.frame0 >= 4 ||
        candidate.frame1 < 0 || candidate.frame1 >= 4 ||
        candidate.dnOut < 0 || candidate.dnOut >= 2 ||
        candidate.historyIn < 0 || candidate.historyIn >= 2 ||
        candidate.historyOut < 0 || candidate.historyOut >= 2) return false;
    result = candidate;
    return true;
}
inline TglOutputSurfaceSelection tglSelectOutputSurface(
        uint32_t pipe, bool di, bool iecp, bool denoise,
        int32_t frame0, int32_t dnOut, const TglOwnedSurfaceStorage* target,
        const std::array<const TglOwnedSurfaceStorage*,4>& ffdi,
        const std::array<const TglOwnedSurfaceStorage*,2>& ffdn) noexcept {
    if (pipe > 2) return {};
    if (pipe == 2) return {target,true};
    if (di) {
        if (frame0 < 0 || size_t(frame0) >= ffdi.size()) return {};
        return {ffdi[size_t(frame0)],true};
    }
    if (iecp) {
        if (dnOut < 0 || size_t(dnOut) >= ffdi.size()) return {};
        return {ffdi[size_t(dnOut)],true};
    }
    if (denoise) {
        if (dnOut < 0 || size_t(dnOut) >= ffdn.size()) return {};
        return {ffdn[size_t(dnOut)],true};
    }
    return {nullptr,pipe == 1}; // SFC intentionally writes no memory output
}

// Gen12 SetupSurfaceStates composition. This populates borrowed CPU shell
// pointers only; allocation/backing/feature admission remain separate gates.
// STMM and FFDN shells must exist even when that feature's backing is unused.
inline bool tglPrepareSurfaceInputs(
        TglOwnedSurfaceInputs& output, const TglOwnedSurfaceStorage* current,
        const TglOwnedSurfaceStorage* target, uint32_t pipe, bool di, bool iecp,
        bool denoise, const TglSurfaceIndices& indices,
        const std::array<const TglOwnedSurfaceStorage*,4>& ffdi,
        const std::array<const TglOwnedSurfaceStorage*,2>& ffdn,
        const std::array<const TglOwnedSurfaceStorage*,2>& stmm) noexcept {
    if (!current || indices.historyIn < 0 || indices.historyIn >= 2 ||
        indices.dnOut < 0 || indices.dnOut >= 2) return false;
    const auto selected = tglSelectOutputSurface(pipe,di,iecp,denoise,
        indices.frame0,indices.dnOut,target,ffdi,ffdn);
    if (!selected.valid || !stmm[size_t(indices.historyIn)] ||
        !ffdn[size_t(indices.dnOut)]) return false;
    // Only SFC without a memory-output feature intentionally selects null.
    if (!selected.surface && !(pipe == 1 && !di && !iecp && !denoise)) return false;
    TglOwnedSurfaceInputs candidate;
    candidate.surfaces = {current,selected.surface,stmm[size_t(indices.historyIn)],
                          ffdn[size_t(indices.dnOut)],nullptr};
    candidate.di = di ? 1 : 0;
    output = candidate;
    return true;
}
// Submit12a4e5 leases stack[-400,-3d0) to builder slot78. Publish only
// after every selected role is admitted by the owning resource set. Role-aware
// admission distinguishes inactive STMM/FFDN shells from GPU-backed inputs;
// this function must not infer backing from a nonnull shell or allocate it.
template<class Admit>
int tglPublishOwnedSurfacesToBuilder(TglOwnedSurfaceInputs& leasedPacket,
    void* callbackPacket,const TglOwnedSurfaceStorage* current,
    const TglOwnedSurfaceStorage* target,uint32_t pipe,bool di,bool iecp,bool dn,
    const TglSurfaceIndices& indices,
    const std::array<const TglOwnedSurfaceStorage*,4>& ffdi,
    const std::array<const TglOwnedSurfaceStorage*,2>& ffdn,
    const std::array<const TglOwnedSurfaceStorage*,2>& stmm,Admit admit) noexcept {
    static_assert(noexcept(admit(std::declval<const TglOwnedSurfaceStorage&>(),size_t{})),
                  "surface admission status ABI");
    if(callbackPacket!=static_cast<void*>(&leasedPacket)) return 5;
    TglOwnedSurfaceInputs candidate;
    if(!tglPrepareSurfaceInputs(candidate,current,target,pipe,di,iecp,dn,
        indices,ffdi,ffdn,stmm)) return 5;
    for(size_t role=0;role<candidate.surfaces.size();++role) {
        if(!candidate.surfaces[role]) continue;
        const int result=admit(*candidate.surfaces[role],role);
        if(result) return result;
    }
    leasedPacket=candidate;
    return 0;
}
struct TglOwnedVeboxPacketLeases {
    uintptr_t child,execution,renderHal,sku;
    TglOwnedSurfaceStorage& source;
    TglOwnedSurfaceStorage* target;
    TglOwnedSurfaceInputs& surfaces;
    TglOwnedVeboxStateStorage& state;
    TglOwnedDiIecpPacket& diIecp;
};
// Exact Submit12a230 stack extents. setup is produced by native slot128;
// completion is written by builder138. Neither is an OS resource descriptor.
struct TglOwnedVeboxBuilderScratch {
    std::array<uint8_t,0x2f0> command{};
    std::array<uint8_t,0x170> surfaceCommands{};
    std::array<uint8_t,0x20> completion{},setup{};
};
struct TglVeboxCompletionWriteSnapshot {
    uintptr_t resource=0,tagLocation=0;
    uint32_t offset=0,value=0;
};
// After successful native builder138, correlate its MI+48 packet with the
// leased heap. Native129ca0 already emits the command: NEVER emit it twice.
// A matching packet proves intended destination/value, not GPU execution or
// completion. In particular, value zero is legal and is not a ready fence.
template<class Read>
bool tglReadVeboxCompletionWrite(const TglOwnedVeboxBuilderScratch& scratch,
    const TglVeboxHardwareBinding& binding,Read read,TglVeboxCompletionWriteSnapshot& out) {
    TglVeboxCompletionWriteSnapshot candidate;
    uint32_t offset=0,value=0;
    if(!binding.heapTagSource(read,candidate.tagLocation) ||
        !read(binding.heap+8,&offset,4) || !read(binding.heap+0x2e0,&value,4)) return false;
    std::memcpy(&candidate.resource,scratch.completion.data(),8);
    std::memcpy(&candidate.offset,scratch.completion.data()+8,4);
    std::memcpy(&candidate.value,scratch.completion.data()+0xc,4);
    if(candidate.resource!=binding.heap+0x40 || candidate.offset!=offset || candidate.value!=value)
        return false;
    out=candidate;return true;
}
struct TglNativeCommandHeaderWritebackInvoker {
    void operator()(uintptr_t entry,void* os,void* command) const noexcept {
        using Writeback=void(*)(void*,void*,uint32_t);
        reinterpret_cast<Writeback>(entry)(os,command,0);
    }
};
// OS+2c8/64d60 writes command+150/158/15c back to the current native
// context. It neither frees command storage nor waits for GPU completion.
// The caller retains the OS/current-context/command lease across this call.
template<class Read,class Qualify,class Executable,
    class Invoke=TglNativeCommandHeaderWritebackInvoker>
bool tglWritebackNativeCommandHeader(uintptr_t image,uintptr_t os,
    TglOwnedVeboxBuilderScratch& scratch,Read read,Qualify qualify,
    Executable executable,Invoke invoke=Invoke{}) noexcept {
    static_assert(noexcept(invoke(uintptr_t{},nullptr,nullptr)),"native void writeback ABI");
    constexpr std::array<uint8_t,16> expected{
        0x55,0x48,0x89,0xe5,0x48,0x89,0x7d,0xf8,0x48,0x89,0x75,0xf0,0x89,0x55,0xec,0x48};
    uintptr_t entry=0;std::array<uint8_t,16> actual{};
    if(!image || image>UINTPTR_MAX-0x64d70 || !os || os>UINTPTR_MAX-0x2d0 ||
        !qualify(image) || !read(os+0x2c8,&entry,8) || entry!=image+0x64d60 ||
        !executable(entry,16) || !read(entry,actual.data(),16) || actual!=expected) return false;
    invoke(entry,reinterpret_cast<void*>(os),scratch.command.data());return true;
}
struct TglNativeVeboxSetupInvoker {
    int operator()(uintptr_t entry,void* child,void* command,void* setup,
        void* auxiliary,int32_t* consumed) const noexcept {
        using Setup=int(*)(void*,void*,void*,void*,int32_t*);
        return reinterpret_cast<Setup>(entry)(child,command,setup,auxiliary,consumed);
    }
};
// Native slot128 retains its own command acquisition/context bookkeeping.
// The caller must lease the auxiliary argument and all child/OS dependencies;
// this binding does not manufacture them or authorize submission.
class TglNativeVeboxSetupBinding {
    uintptr_t image_=0,entry_=0;
public:
    template<class Read,class Qualify,class Executable>
    bool resolve(uintptr_t image,Read read,Qualify qualify,Executable executable) {
        image_=entry_=0;
        constexpr std::array<uint8_t,16> expected{
            0x55,0x48,0x89,0xe5,0x48,0x81,0xec,0x80,0,0,0,0x48,0x89,0x7d,0xf8,0x48};
        uintptr_t entry=0;std::array<uint8_t,16> actual{};
        if(!image || image>UINTPTR_MAX-0x757dc8 || !qualify(image) ||
            !read(image+0x757dc0,&entry,8) || entry!=image+0x129a10 ||
            !executable(entry,16) || !read(entry,actual.data(),16) || actual!=expected) return false;
        image_=image;entry_=entry;return true;
    }
    template<class Read,class Invoke=TglNativeVeboxSetupInvoker>
    int call(uintptr_t child,TglOwnedVeboxBuilderScratch& scratch,void* auxiliary,
        int32_t& consumed,Read read,Invoke invoke=Invoke{}) const noexcept {
        static_assert(noexcept(invoke(uintptr_t{},nullptr,nullptr,nullptr,nullptr,
            static_cast<int32_t*>(nullptr))),"native setup status ABI");
        uintptr_t table=0,entry=0;
        if(!image_ || !entry_ || !child || child>UINTPTR_MAX-8 || !auxiliary ||
            !read(child,&table,8) || !table || table>UINTPTR_MAX-0x12f ||
            !read(table+0x128,&entry,8) || entry!=entry_) return 5;
        return invoke(entry_,reinterpret_cast<void*>(child),scratch.command.data(),
            scratch.setup.data(),auxiliary,&consumed);
    }
};
struct TglNativeVeboxBuilderInvoker {
    int operator()(uintptr_t entry,void* child,void* command,void* di,
        void* surfaces,void* surfaceCommands,void* state,void* completion,void* setup) const noexcept {
        using Builder=int(*)(void*,void*,void*,void*,void*,void*,void*,void*);
        return reinterpret_cast<Builder>(entry)(child,command,di,surfaces,
            surfaceCommands,state,completion,setup);
    }
};
class TglNativeVeboxBuilderBinding {
    uintptr_t image_=0,entry_=0;
public:
    template<class Read,class Qualify,class Executable>
    bool resolve(uintptr_t image,Read read,Qualify qualify,Executable executable) {
        image_=entry_=0;
        constexpr std::array<uint8_t,16> expected{
            0x55,0x48,0x89,0xe5,0x48,0x81,0xec,0xe0,0,0,0,0x48,0x8b,0x45,0x18,0x4c};
        uintptr_t entry=0;std::array<uint8_t,16> actual{};
        if(!image || image>UINTPTR_MAX-0x757dd8 || !qualify(image) ||
            !read(image+0x757dd0,&entry,8) || entry!=image+0x129ca0 ||
            !executable(entry,16) || !read(entry,actual.data(),16) || actual!=expected) return false;
        image_=image;entry_=entry;return true;
    }
    // Called ONLY inside the owned context/child pass, after native setup128.
    // setup128 (129a10) is NOT just scratch initialization: it obtains the
    // native command through OS+2a8, propagates that status, then invokes
    // child+d8 and propagates its status before OS+310/+300 bookkeeping.
    // Zeroing scratch cannot substitute for those resource/context effects.
    // Requires resource/heap/command leases; binding code is not admission to
    // GPU execution. Native138 is invoked once and its exact result preserved.
    template<class Read,class Invoke=TglNativeVeboxBuilderInvoker>
    int call(TglOwnedVeboxPacketLeases& leases,TglOwnedVeboxBuilderScratch& scratch,
        Read read,Invoke invoke=Invoke{}) const noexcept {
        static_assert(noexcept(invoke(uintptr_t{},nullptr,nullptr,nullptr,nullptr,
            nullptr,nullptr,nullptr,nullptr)),"native builder status ABI");
        uintptr_t table=0,entry=0;
        if(!image_ || !entry_ || !leases.child || leases.child>UINTPTR_MAX-8 ||
            !read(leases.child,&table,8) || !table || table>UINTPTR_MAX-0x13f ||
            !read(table+0x138,&entry,8) || entry!=entry_) return 5;
        return invoke(entry_,reinterpret_cast<void*>(leases.child),scratch.command.data(),
            &leases.diIecp,&leases.surfaces,scratch.surfaceCommands.data(),
            &leases.state,scratch.completion.data(),scratch.setup.data());
    }
};
// Concrete synchronous producers sharing ONE immutable role snapshot and
// exact writable packet leases. Enclosing owner still retains all resources;
// this object does not adopt handles or authorize asynchronous submission.
template<class Read,class SurfaceAdmit,class DiAdmit,class Register,class Control>
struct TglOwnedVeboxPacketContext {
    TglOwnedVeboxProducerPass pass;
    TglOwnedVeboxPacketLeases leases;
    const TglDiIecpInputs input;
    bool disableTemporalDenoise;
    Read read;
    SurfaceAdmit admitSurface;
    DiAdmit admitDi;
    Register registerResource;
    Control control;
    bool matches(bool di) noexcept {
        bool nativeDi=false;TglOwnedStateInputs snapshot;TglSurfaceIndices indices;
        return input.current==&leases.source && input.target==leases.target && input.di==di &&
            tglReadOwnedBuilderDiArgument(nativeDi,leases.child,leases.execution,read) && nativeDi==di &&
            tglReadOwnedStateChildInputs(snapshot,leases.child,leases.execution,
                leases.source,leases.target,di,read) && snapshot.pipe==input.pipe &&
            snapshot.referenceValid==input.referenceValid && snapshot.dn==input.dnNeeded &&
            tglReadSurfaceIndices(leases.execution,indices,read) &&
            indices.frame0==input.indices.frame0 && indices.frame1==input.indices.frame1 &&
            indices.dnOut==input.indices.dnOut && indices.historyIn==input.indices.historyIn &&
            indices.historyOut==input.indices.historyOut;
    }
    int surfaces(bool di,void* packet) noexcept {
        if(packet!=&leases.surfaces || !matches(di)) return 5;
        return tglPublishOwnedSurfacesToBuilder(leases.surfaces,packet,input.current,
            input.target,input.pipe,di,input.iecp,input.dnNeeded,input.indices,
            input.ffdi,input.ffdn,input.stmm,admitSurface);
    }
    int state(bool di,void* packet) noexcept {
        if(packet!=&leases.state || !matches(di)) return 5;
        return tglPublishOwnedStateToBuilder(leases.state,packet,leases.child,
            leases.execution,leases.renderHal,leases.sku,leases.source,
            leases.target,di,disableTemporalDenoise,read);
    }
    int diIecp(bool di,void* packet) noexcept {
        if(packet!=&leases.diIecp || !matches(di)) return 5;
        return tglPublishOwnedDiIecpToBuilder(leases.diIecp,packet,input,
            admitDi,registerResource,control);
    }
    template<class Owner,class Invoke> int withBuilder(Owner& owner,Invoke invoke) noexcept {
        return tglWithOwnedVeboxBuilder(owner,*this,[&](void* child) noexcept {
            return reinterpret_cast<uintptr_t>(child)==leases.child ? invoke(child) : 5;
        });
    }
    template<class Owner,class NativeRead,class SetupInvoke=TglNativeVeboxSetupInvoker,
        class BuilderInvoke=TglNativeVeboxBuilderInvoker>
    int withNativeBuilder(Owner& owner,const TglNativeVeboxSetupBinding& setup,
        const TglNativeVeboxBuilderBinding& builder,TglOwnedVeboxBuilderScratch& scratch,
        void* auxiliary,int32_t& consumed,NativeRead nativeRead,
        SetupInvoke setupInvoke=SetupInvoke{},BuilderInvoke builderInvoke=BuilderInvoke{}) noexcept {
        return tglWithOwnedVeboxPreparedBuilder(owner,*this,[&](void* child) noexcept {
            return reinterpret_cast<uintptr_t>(child)==leases.child ?
                setup.call(leases.child,scratch,auxiliary,consumed,nativeRead,setupInvoke) : 5;
        },[&](void* child) noexcept {
            return reinterpret_cast<uintptr_t>(child)==leases.child ?
                builder.call(leases,scratch,nativeRead,builderInvoke) : 5;
        });
    }
};

template<class Region>
bool tglAdmitDndiStackPackets(uintptr_t top,size_t stackSize,uintptr_t luma,size_t ln,
    uintptr_t chroma,size_t cn,Region region) noexcept {
    if(!top || !stackSize || stackSize>top || ln!=0x30 || cn!=0x24 ||
        !luma || !chroma || (luma&3) || (chroma&3) ||
        luma>UINTPTR_MAX-ln || chroma>UINTPTR_MAX-cn ||
        luma<top-stackSize || chroma<top-stackSize || luma+ln>top || chroma+cn>top ||
        (luma<chroma+cn && chroma<luma+ln)) return false;
    const auto writable=[&](uintptr_t address,size_t size) noexcept {
        uintptr_t begin=0;size_t extent=0;uint32_t protection=0;
        return region(address,begin,extent,protection) && begin<=address &&
            extent<=UINTPTR_MAX-begin && address+size<=begin+extent && (protection&7)==3;
    };
    return writable(luma,ln) && writable(chroma,cn);
}
#if defined(__APPLE__)
struct TglNativeDndiStackAdmission {
    bool operator()(uintptr_t luma,size_t ln,uintptr_t chroma,size_t cn) const noexcept {
        const auto thread=pthread_self();
        const uintptr_t top=reinterpret_cast<uintptr_t>(pthread_get_stackaddr_np(thread));
        const size_t size=pthread_get_stacksize_np(thread);
        const auto region=[](uintptr_t pointer,uintptr_t& begin,size_t& extent,
                             uint32_t& protection) noexcept {
            mach_vm_address_t address=pointer;mach_vm_size_t length=0;
            natural_t depth=0;
            for(unsigned level=0;level<16;++level) {
                vm_region_submap_info_data_64_t info{};
                mach_msg_type_number_t count=VM_REGION_SUBMAP_INFO_COUNT_64;
                if(mach_vm_region_recurse(mach_task_self(),&address,&length,&depth,
                    reinterpret_cast<vm_region_recurse_info_t>(&info),&count)!=KERN_SUCCESS ||
                    address>pointer || length>SIZE_MAX) return false;
                if(info.is_submap) {++depth;continue;}
                begin=address;extent=length;protection=info.protection;return true;
            }
            return false;
        };
        return tglAdmitDndiStackPackets(top,size,luma,ln,chroma,cn,region);
    }
};
#endif

// Wrap the real packet context without copying its pass or resource leases.
// CSC accepts only this frame's owned surfaces and retained execution. Produce
// authenticates the native image and fills a private candidate, never child RAM.
template<class Frame,class ChildOwner,class ExecutionOwner,class Produce>
struct TglOwnedVeboxCscFrameContext {
    Frame& frame;
    TglOwnedVeboxPacketLeases& leases;
    ChildOwner& childOwner;
    ExecutionOwner& executionOwner;
    Produce produce;
    TglOwnedVeboxProducerPass& pass;
    TglOwnedVeboxCscFrameContext(Frame& f,TglOwnedVeboxPacketLeases& l,
        ChildOwner& child,ExecutionOwner& execution,Produce p) noexcept
        : frame(f),leases(l),childOwner(child),executionOwner(execution),produce(p),pass(f.pass) {}
    int csc(void* source,void* target) noexcept {
        if(source!=&leases.source || !leases.target || target!=leases.target ||
            !childOwner.hasPrivateChildLease(leases.child) ||
            !executionOwner.hasExecutionLease(leases.execution)) return 5;
        uint32_t from=0,to=0,format=0;
        std::memcpy(&from,leases.source.prefix.data(),4);
        std::memcpy(&to,leases.target->prefix.data(),4);
        std::memcpy(&format,leases.source.prefix.data()+0x130,4);
        TglCscCoefficients candidate;
        if(!produce(candidate,from,to,format)) return 5;
        return childOwner.publishCsc(candidate) ? 0 : 5;
    }
    int setupKernel(int32_t index) noexcept {return frame.setupKernel(index);}
    int surfaces(bool di,void* packet) noexcept {return frame.surfaces(di,packet);}
    int state(bool di,void* packet) noexcept {return frame.state(di,packet);}
    int diIecp(bool di,void* packet) noexcept {return frame.diIecp(di,packet);}
    int dndi(void* source,void* luma,void* chroma) noexcept {return frame.dndi(source,luma,chroma);}
};
// Add +68 only to a context retaining the actual child/execution owners.
// The enclosing frame orchestration acquires their leases before callbacks.
template<class Frame,class ChildOwner,class ExecutionOwner>
struct TglOwnedVeboxKernelFrameContext {
    Frame& frame;
    ChildOwner& childOwner;
    ExecutionOwner& executionOwner;
    TglOwnedVeboxProducerPass& pass;
    TglOwnedVeboxKernelFrameContext(Frame& f,ChildOwner& child,ExecutionOwner& execution) noexcept
        : frame(f),childOwner(child),executionOwner(execution),pass(f.pass) {}
    int setupKernel(int32_t index) noexcept {
        return childOwner.setupKernelScratch(executionOwner,index);
    }
    int surfaces(bool di,void* packet) noexcept {return frame.surfaces(di,packet);}
    int state(bool di,void* packet) noexcept {return frame.state(di,packet);}
    int diIecp(bool di,void* packet) noexcept {return frame.diIecp(di,packet);}
    int dndi(void* source,void* luma,void* chroma) noexcept {return frame.dndi(source,luma,chroma);}
};
// PacketAdmit must qualify BOTH exact native writable stack extents on the
// current calling thread. A readable heap address alone is not admission.
template<class Frame,class ExecutionOwner,class Read,class PacketAdmit>
struct TglOwnedVeboxDndiFrameContext {
    Frame& frame;
    ExecutionOwner& executionOwner;
    uintptr_t leasedDnParams;
    Read read;
    PacketAdmit admitPackets;
    TglOwnedVeboxProducerPass& pass;
    TglOwnedVeboxDndiFrameContext(Frame& f,ExecutionOwner& owner,uintptr_t params,
        Read r,PacketAdmit admit) noexcept : frame(f),executionOwner(owner),
        leasedDnParams(params),read(r),admitPackets(admit),pass(f.pass) {}
    int surfaces(bool di,void* packet) noexcept {return frame.surfaces(di,packet);}
    int state(bool di,void* packet) noexcept {return frame.state(di,packet);}
    int diIecp(bool di,void* packet) noexcept {return frame.diIecp(di,packet);}
    int dndi(void* callbackSource,void* luma,void* chroma) noexcept {
        const uintptr_t lp=reinterpret_cast<uintptr_t>(luma),cp=reinterpret_cast<uintptr_t>(chroma);
        const auto& leases=frame.leases;
        if(callbackSource!=&leases.source || !executionOwner.hasExecutionLease(leases.execution) ||
            !lp || !cp || (lp&3) || (cp&3) || lp>UINTPTR_MAX-0x30 || cp>UINTPTR_MAX-0x24 ||
            (lp<cp+0x24 && cp<lp+0x30) || !admitPackets(lp,0x30,cp,0x24)) return 5;
        std::array<uint8_t,2> dnFlags{};
        if(leases.execution>UINTPTR_MAX-0xd ||
            !read(leases.execution+0xb,dnFlags.data(),dnFlags.size())) return 5;
        TglOwnedDndiSource source;
        if(!tglAdmitOwnedDndiSource(source,leases.source,(dnFlags[0]|dnFlags[1])&1,
            leasedDnParams,read)) return 5;
        TglOwnedDndiPackets packets;
        if(!read(lp,packets.luma.data(),0x30) || !read(cp,packets.chroma.data(),0x24)) return 5;
        const int result=tglPublishOwnedDndiInFrame(executionOwner,leases.execution,leases.child,
            reinterpret_cast<uintptr_t>(callbackSource),source,packets,read);
        if(result) return result;
        // Current-thread stack extents remain writable until native returns.
        std::memcpy(luma,packets.luma.data(),0x30);
        std::memcpy(chroma,packets.chroma.data(),0x24);
        return 0;
    }
};

inline std::array<uint8_t,0x48> tglSurfaceDescriptor(
        const TglOwnedSurfaceStorage& shell) noexcept {
    return tglSurfaceDescriptorBytes(reinterpret_cast<const uint8_t*>(&shell));
}
inline std::array<uint8_t,0x170> tglSurfaceCommandSet(
        const std::array<const TglOwnedSurfaceStorage*,5>& surfaces,
        uint8_t di) noexcept {
    std::array<const uint8_t*,5> views{};
    for (size_t i=0; i<views.size(); ++i)
        if (surfaces[i]) views[i] = reinterpret_cast<const uint8_t*>(surfaces[i]);
    return tglSurfaceCommandSetBytes(views,di);
}
inline std::array<uint8_t,0x170> tglSurfaceCommandSet(
        const TglOwnedSurfaceInputs& inputs) noexcept {
    return tglSurfaceCommandSet(inputs.surfaces,inputs.di);
}
// Darwin ICL1e5180 and pinned Intel VeboxInitSTMMHistory agree on this
// data pattern. This bounded CPU implementation requires an independently
// acquired writable mapping and aligned dimensions; it is NOT a TGL lock ABI
// or history-validity proof. Preserve DN-history bytes and row padding.
// TGL63da0 discards IOSurfaceLock's result before returning a plane pointer.
// That pointer alone must NEVER authorize this write: the enclosing owned
// mapper must separately prove successful writable locking and byte capacity.
inline bool tglInitializeOwnedStmmMapping(uint8_t* mapping,size_t capacity,
    uint32_t width,uint32_t height,uint32_t pitch) noexcept {
    if(!mapping || !width || !height || (width&3) || pitch<width ||
       height>uint32_t{INT32_MAX}) return false;
    const uint64_t extent=uint64_t{height-1}*pitch+width;
    if(extent>capacity || extent>SIZE_MAX ||
       reinterpret_cast<uintptr_t>(mapping)>UINTPTR_MAX-size_t(extent-1)) return false;
    for(uint32_t y=0;y<height;++y) {
        auto* row=mapping+size_t(y)*pitch;
        for(uint32_t x=0;x<width;x+=4) {row[x]=0xff;row[x+1]=0xff;}
    }
    return true;
}
struct TglOwnedStmmView {
    uint8_t* bytes=nullptr;
    size_t capacity=0;
    uint32_t width=0,height=0,pitch=0;
};
struct TglOwnedStmmInitialization {
    int lockStatus=5,unlockStatus=5;
    bool initialized=false;
    int status() const noexcept {
        return lockStatus ? lockStatus : !initialized ? 5 : unlockStatus;
    }
    bool ready() const noexcept {return status()==0;}
};
// Surface is borrowed from the resource owner. Lock success is necessary but
// not a GPU fence: owning caller must first exclude all GPU users of STMM.
// Always unlock exactly once after a successful lock, including bad metadata.
template<class Surface,class Api>
TglOwnedStmmInitialization tglInitializeOwnedStmmSurface(Surface surface,Api& api) noexcept {
    TglOwnedStmmInitialization result;
    if(!surface) return result;
    result.lockStatus=api.lock(surface);
    if(result.lockStatus) return result;
    TglOwnedStmmView view;
    if(api.view(surface,view))
        result.initialized=tglInitializeOwnedStmmMapping(view.bytes,view.capacity,
            view.width,view.height,view.pitch);
    result.unlockStatus=api.unlock(surface);
    return result;
}
#if defined(__APPLE__)
struct TglDarwinStmmSurfaceApi {
    IOSurfaceRef surface(uintptr_t handle) const noexcept {
        return reinterpret_cast<IOSurfaceRef>(handle);
    }
    int lock(IOSurfaceRef surface) const noexcept {return IOSurfaceLock(surface,0,nullptr);}
    int unlock(IOSurfaceRef surface) const noexcept {return IOSurfaceUnlock(surface,0,nullptr);}
    bool view(IOSurfaceRef surface,TglOwnedStmmView& output) const noexcept {
        // Native63720 format3d -> IOSurface pixel-format8 at63a41, one plane.
        // SDK explicitly permits plane0 queries on a non-planar (count0) surface.
        if(IOSurfaceGetPixelFormat(surface)!=8 || IOSurfaceGetPlaneCount(surface)>1) return false;
        const auto base=reinterpret_cast<uintptr_t>(IOSurfaceGetBaseAddress(surface));
        const auto plane=reinterpret_cast<uintptr_t>(IOSurfaceGetBaseAddressOfPlane(surface,0));
        const size_t capacity=IOSurfaceGetAllocSize(surface);
        const size_t width=IOSurfaceGetWidthOfPlane(surface,0);
        const size_t height=IOSurfaceGetHeightOfPlane(surface,0);
        const size_t pitch=IOSurfaceGetBytesPerRowOfPlane(surface,0);
        if(!base || !plane || plane<base || plane-base>capacity ||
           capacity>UINTPTR_MAX-base || width>UINT32_MAX || height>UINT32_MAX ||
           pitch>UINT32_MAX) return false;
        output={reinterpret_cast<uint8_t*>(plane),capacity-size_t(plane-base),
            uint32_t(width),uint32_t(height),uint32_t(pitch)};
        return true;
    }
};
#endif
// Only an owned, backed native63720 type1/format3d descriptor is admitted.
// Descriptor identity is not provenance: caller must hold its resource owner
// and exclude release/reallocation/GPU access throughout this synchronous call.
// Direct IOSurface APIs do not mutate the native descriptor's cached addresses.
// A nonzero unlockStatus after successful lock is NOT release permission:
// enclosing runtime owner must retain/quarantine the resource, not blindly
// route that case through group reset. Its recovery policy remains separate.
template<class Api>
TglOwnedStmmInitialization tglInitializeOwnedStmmResource(
    const std::array<uint8_t,0x148>& resource,Api& api) noexcept {
    uint32_t type=0,format=0;
    uintptr_t handle=0;
    std::memcpy(&type,resource.data()+0x14,4);
    std::memcpy(&format,resource.data()+0x18,4);
    std::memcpy(&handle,resource.data()+0x58,8);
    if(type!=1 || format!=0x3d || !handle) return {};
    return tglInitializeOwnedStmmSurface(api.surface(handle),api);
}
struct TglResourceStorageReference { std::array<uint8_t,0x148>& storage; };

// Native 52bf0 borrows surface+148 at descriptor+40. Both the surface and its
// backing owner must outlive every consumer of this descriptor.
struct TglNativeSurfaceConversionInvoker {
    int operator()(uintptr_t entry, const TglOwnedSurfaceStorage& surface,
                   std::array<uint8_t,0x48>& descriptor) const noexcept {
        using Convert = int (*)(const void*,void*);
        return reinterpret_cast<Convert>(entry)(&surface,descriptor.data());
    }
};

struct TglSurfaceConversionBinding {
    uintptr_t entry = 0;
    template<class Read, class Qualify, class Executable>
    bool resolve(uintptr_t image, Read read, Qualify qualify, Executable executable) {
        entry = 0;
        constexpr std::array<uint8_t,16> expected{
            0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x20,
            0x48,0x89,0x7d,0xf8,0x48,0x89,0x75,0xf0};
        std::array<uint8_t,16> actual{};
        if (!image || image > UINTPTR_MAX-0x52c00 || !qualify(image) ||
            !executable(image+0x52bf0,16) ||
            !read(image+0x52bf0,actual.data(),actual.size()) || actual != expected)
            return false;
        entry = image+0x52bf0;
        return true;
    }
};

// Gen12 AllocateResources finish calls FreeResources on ANY failure. This
// composes descriptor owners, not Linux object offsets or feature admission.
// Prepare handles per-resource initialization/metadata and preserves its error.
// Only a complete backed set is exposed; this proves neither GPU sync nor execution.
// SFC and app-owned LUT surfaces are separate owners, never adopted by this group.
template<class Backend, size_t Count> class TglOwnedResourceGroup {
    static_assert(Count > 0);
    using Resource = TglOwnedResource<Backend>;
    std::array<Resource,Count> resources;
    bool ready = false;
    bool bindingsValid = true;
    bool busy = false;
    void retire() noexcept {
        ready = false;
        for (auto& resource : resources) resource.reset();
    }
    template<size_t... I>
    TglOwnedResourceGroup(Backend& backend, std::index_sequence<I...>) noexcept
        : resources{(static_cast<void>(I),Resource(backend))...} {}
    template<size_t... I>
    TglOwnedResourceGroup(std::array<Backend,Count>& backends, std::index_sequence<I...>) noexcept
        : resources{Resource(backends[I])...} {}
    template<size_t... I>
    TglOwnedResourceGroup(std::array<Backend,Count>& backends,
                         const std::array<TglResourceStorageReference,Count>& storage,
                         std::index_sequence<I...>) noexcept
        : resources{Resource(backends[I],storage[I].storage)...} {
        for (size_t i = 0; i < Count; ++i)
            for (size_t j = 0; j < i; ++j)
                if (&storage[i].storage == &storage[j].storage) bindingsValid = false;
    }
public:
    struct Request { bool required = false; TglResourceKey key{}; };
    explicit TglOwnedResourceGroup(Backend& backend) noexcept
        : TglOwnedResourceGroup(backend,std::make_index_sequence<Count>{}) {}
    // Surface and buffer allocators have different native resource types.
    // The complete backend array, and its borrowed OS context, outlive this group.
    explicit TglOwnedResourceGroup(std::array<Backend,Count>& backends) noexcept
        : TglOwnedResourceGroup(backends,std::make_index_sequence<Count>{}) {}
    TglOwnedResourceGroup(std::array<Backend,Count>& backends,
                         const std::array<TglResourceStorageReference,Count>& storage) noexcept
        : TglOwnedResourceGroup(backends,storage,std::make_index_sequence<Count>{}) {}
    TglOwnedResourceGroup(const TglOwnedResourceGroup&) = delete;
    TglOwnedResourceGroup& operator=(const TglOwnedResourceGroup&) = delete;
    template<class Prepare>
    int ensure(const std::array<Request,Count>& requests, Prepare prepare) noexcept {
        static_assert(noexcept(std::declval<Prepare&>()(size_t{},
            std::declval<const typename Resource::Storage&>(),bool{})),
            "resource initialization must report errors through status");
        if (busy) return 5;
        struct Guard {
            bool& flag;
            explicit Guard(bool& value) noexcept : flag(value) { flag = true; }
            ~Guard() { flag = false; }
        } guard(busy);
        ready = false;
        if (!bindingsValid) return 5;
        for (size_t i = 0; i < Count; ++i) {
            if (!requests[i].required) { resources[i].reset(); continue; }
            const auto result = resources[i].ensure(requests[i].key);
            if (result.status || result.state != Resource::State::Backed) {
                retire(); return result.status ? result.status : 5;
            }
            const int status = resources[i].withStorage([&](const auto& storage) noexcept {
                return prepare(i,storage,result.changed);
            });
            if (status) { retire(); return status; }
        }
        ready = true; return 0;
    }
    void reset() noexcept {
        if (busy) return;
        busy = true;
        retire();
        busy = false;
    }
    const typename Resource::Storage* storage(size_t i) const noexcept {
        return !busy && ready && i < Count && resources[i].state() == Resource::State::Backed
            ? &resources[i].storage() : nullptr;
    }
};

// Separate SFC owner: reference Darwin1df210 AVS/IEF sizing plus pinned
// Gen12 conditional SFD row buffer policy. All required allocations unwind
// on failure. Backend must be configured for native resource type0.
// TGL's buffer format is 3d (not ICL's 3e); linear tile is 4.
template<class Backend> class TglOwnedSfcLineBuffers {
    TglOwnedResourceGroup<Backend,3> owners;
    uint32_t inputHeight_=0,scaledHeight_=0;
    bool leased_=false,mutating_=false;
public:
    explicit TglOwnedSfcLineBuffers(Backend& backend) noexcept : owners(backend) {}
    ~TglOwnedSfcLineBuffers() { reset(); }
    int ensure(uint32_t inputHeight, uint32_t scaledHeight) noexcept {
        if(leased_ || mutating_) return 5;
        TglOwnedSfcLineBufferSizes sizes;
        // Invalid dimensions do not retire an already valid allocation.
        if (!tglOwnedSfcLineBufferSizes(sizes,inputHeight,scaledHeight)) return 5;
        using Request=typename TglOwnedResourceGroup<Backend,3>::Request;
        const std::array<Request,3> requests{{
            {true,{sizes.avs,1,0x3d,4,0,false}},
            {true,{sizes.ief,1,0x3d,4,0,false}},
            {sizes.sfd!=0,{sizes.sfd,1,0x3d,4,0,false}}
        }};
        mutating_=true;
        const int status=owners.ensure(requests,[](size_t,
            const typename TglOwnedResource<Backend>::Storage&,bool) noexcept { return 0; });
        inputHeight_=status ? 0 : inputHeight;
        scaledHeight_=status ? 0 : scaledHeight;
        mutating_=false;
        return status;
    }
    void reset() noexcept {
        if(leased_ || mutating_) return;
        mutating_=true;
        inputHeight_=scaledHeight_=0;owners.reset();
        mutating_=false;
    }
    // Pins synchronous CPU preparation only, NOT GPU completion. The enclosing
    // frame still retains this owner through discard or an actual fence.
    template<class Use> int withBuffers(uint32_t inputHeight,uint32_t scaledHeight,Use use) noexcept {
        static_assert(noexcept(use()),"SFC buffer lease status ABI");
        if(leased_ || mutating_ || !matchesDimensions(inputHeight,scaledHeight)) return 5;
        leased_=true;
        const int result=use();
        leased_=false;return result;
    }
    bool matchesDimensions(uint32_t inputHeight,uint32_t scaledHeight) const noexcept {
        return !mutating_ && owners.storage(0) && owners.storage(1) &&
            inputHeight_==inputHeight && scaledHeight_==scaledHeight;
    }
    // Borrow only until reset/ensure/destruction; caller retains the owner
    // through command completion and rebinds after any resource mutation.
    // Failed admission explicitly revokes the SFD pointer, not other fields.
    bool bindSfdRow(TglOwnedSfcGen12Tail& tail) const noexcept {
        tail.sfdLineBuffer=0;
        if(mutating_) return false;
        if(!owners.storage(0) || !owners.storage(1)) return false;
        const auto* sfd=owners.storage(2);
        if(sfd) tail.sfdLineBuffer=reinterpret_cast<uintptr_t>(sfd->data());
        return true;
    }
    const typename TglOwnedResource<Backend>::Storage* storage(size_t i) const noexcept {
        return mutating_ ? nullptr : owners.storage(i);
    }
};

// Darwin 1e04de..1e05de tail. Borrowed resources outlive the submitted packet.
// Query is the authenticated surface-info producer (native 57ba0 equivalent).
// It may update owned output metadata, but failed queries never publish a packet.
template<size_t PacketSize,class Query>
int tglBindOwnedSfcResources(std::array<uint8_t,PacketSize>& packet,
    TglOwnedSurfaceStorage& output,const std::array<uint8_t,0x148>& avs,
    const std::array<uint8_t,0x148>& ief,Query query) noexcept {
    static_assert(PacketSize==0xb8 || PacketSize==sizeof(TglOwnedSfcStateParametersPacket),
                  "only audited reference prefix or complete TGL state ABI");
    static_assert(noexcept(query(output)),"surface query must report native status");
    uint32_t avsType=0,iefType=0;
    std::memcpy(&avsType,avs.data()+0x14,4);
    std::memcpy(&iefType,ief.data()+0x14,4);
    if (avsType || iefType || !tglNativeBackingPresent(avs) ||
        !tglNativeBackingPresent(ief) || !tglNativeBackingPresent(output.resource)) return 5;
    const int status=query(output);
    if (status) return status;
    if (!tglNativeBackingPresent(output.resource)) return 5;
    auto candidate=packet;
    const std::array<uint64_t,3> pointers{{reinterpret_cast<uintptr_t>(output.resource.data()),
        reinterpret_cast<uintptr_t>(avs.data()),reinterpret_cast<uintptr_t>(ief.data())}};
    for (size_t i=0;i<3;++i) std::memcpy(candidate.data()+0x90+i*8,&pointers[i],8);
    std::memcpy(candidate.data()+0xa8,output.prefix.data()+0xf0,4);
    const std::array<size_t,4> sources{{0x104,0x108,0x114,0x118}};
    for (size_t i=0;i<4;++i) std::memcpy(candidate.data()+0xac+i*2,output.prefix.data()+sources[i],2);
    packet=candidate;
    return 0;
}

// Single serialized CPU preparation path. Resources/image lease must remain
// retained until native completion; this neither registers nor submits them.
// Failure preserves state/command parameters, but Query may mutate output
// metadata under its documented native contract (not a native rollback).
template<class CpuBackend,class ResourceBackend,class Query>
int tglPrepareOwnedSfcCommandState(TglOwnedSfcCommandParameters& parameters,
    TglOwnedSfcStateParameters<CpuBackend>& state,
    const TglOwnedSfcLineBuffers<ResourceBackend>& lines,TglOwnedSurfaceStorage& output,
    const std::array<uint8_t,0xb8>& prefix,TglOwnedSfcGen12Tail tail,
    bool rgbAdaptive,bool eightTapChroma,bool compressionEnabled,
    TglOwnedSfcCompressionMode compressionMode,Query query) noexcept {
    uint32_t inputHeight,scaledHeight;
    std::memcpy(&inputHeight,prefix.data()+0x34,4);
    std::memcpy(&scaledHeight,prefix.data()+0x50,4);
    if(!state.data() || !lines.matchesDimensions(inputHeight,scaledHeight) ||
       !lines.bindSfdRow(tail)) return 5;
    TglOwnedSfcStateParametersPacket checked{};
    if(!tglPrepareOwnedSfcGen12Pass(checked,prefix,tail,rgbAdaptive,eightTapChroma,
        compressionEnabled,compressionMode)) return 5;
    auto bound=prefix;
    const int status=tglBindOwnedSfcResources(bound,output,*lines.storage(0),
                                            *lines.storage(1),query);
    if(status) return status;
    if(!state.prepare(bound,tail,rgbAdaptive,eightTapChroma,compressionEnabled,
                      compressionMode)) return 5;
    std::memcpy(parameters.state.data(),state.data(),parameters.state.size());
    return 0;
}

// Keep all three backing owners pinned through query, packet preparation and
// command generation. This scope ends after CPU emission, not GPU completion;
// the caller still retains resources until discard/fence and owns output lease.
template<class CpuBackend,class ResourceBackend,class Query,class Generate>
int tglWithOwnedSfcPreparedCommands(TglOwnedSfcCommandParameters& parameters,
    TglOwnedSfcStateParameters<CpuBackend>& state,
    TglOwnedSfcLineBuffers<ResourceBackend>& lines,TglOwnedSurfaceStorage& output,
    const std::array<uint8_t,0xb8>& prefix,TglOwnedSfcGen12Tail tail,
    bool rgbAdaptive,bool eightTapChroma,bool compressionEnabled,
    TglOwnedSfcCompressionMode compressionMode,Query query,Generate generate) noexcept {
    static_assert(noexcept(generate(parameters)),"native command generation status ABI");
    uint32_t inputHeight=0,scaledHeight=0;
    std::memcpy(&inputHeight,prefix.data()+0x34,4);
    std::memcpy(&scaledHeight,prefix.data()+0x50,4);
    return lines.withBuffers(inputHeight,scaledHeight,[&]() noexcept {
        const int status=tglPrepareOwnedSfcCommandState(parameters,state,lines,output,
            prefix,tail,rgbAdaptive,eightTapChroma,compressionEnabled,compressionMode,query);
        if(status) return status;
        return state.withParameters([&](const uint8_t*) noexcept {
            return generate(parameters);
        });
    });
}

struct TglOwnedSfcAvsConfiguration {
    uint32_t format=0xffffffffu,siting=0;
    float scaleX=0.f,scaleY=0.f,sharpening=0.f,window=0.f;
    bool forceFilter=false,symmetric=false;
};
// Single CPU construction scope for the three distinct owners. No submit,
// wait, GPU retirement or output-resource ownership is implied here.
template<class CpuBackend,class AvsBackend,class ResourceBackend,
    class NativeSin,class Query,class Generate>
int tglWithOwnedSfcCommandInputs(TglOwnedSfcCommandParameters& parameters,
    TglOwnedSfcStateParameters<CpuBackend>& state,
    TglOwnedSfcAvsParameters<AvsBackend>& avs,TglOwnedSfcAvsTables& cache,
    TglOwnedSfcLineBuffers<ResourceBackend>& lines,TglOwnedSurfaceStorage& output,
    const std::array<uint8_t,0xb8>& prefix,TglOwnedSfcGen12Tail tail,
    bool rgbAdaptive,bool eightTapChroma,bool compressionEnabled,
    TglOwnedSfcCompressionMode compressionMode,const TglOwnedSfcAvsConfiguration& config,
    NativeSin sine,Query query,Generate generate) noexcept {
    return tglWithOwnedSfcAvsCommands(parameters,avs,cache,config.format,
        config.scaleX,config.scaleY,config.siting,config.forceFilter,
        config.symmetric,config.sharpening,config.window,sine,
        [&](TglOwnedSfcCommandParameters& ready) noexcept {
            return tglWithOwnedSfcPreparedCommands(ready,state,lines,output,prefix,
                tail,rgbAdaptive,eightTapChroma,compressionEnabled,
                compressionMode,query,generate);
        });
}

// Gen12 statistics lifecycle, not a native fill ABI implementation. Backend
// must allocate a buffer; Fill must validate and initialize actual backing.
template<class Backend> class TglStatisticsResource {
public:
    explicit TglStatisticsResource(Backend &backend) noexcept : owner_(backend) {}
    ~TglStatisticsResource() {reset();}
    template<class Fill>
    int ensure(uint32_t width, uint32_t height, Fill fill) noexcept {
        static_assert(noexcept(fill(owner_.storage(), uint32_t{})),
                      "initialization must report status without throwing");
        if(busy_) return 5;
        TglStatisticsAllocation candidate;
        const int shape = tglStatisticsAllocationSize(width, height, candidate);
        if (shape) return shape; // invalid request does not retire a valid resource
        busy_=true;
        struct Unlock {bool& busy;~Unlock(){busy=false;}} unlock{busy_};
        layout_={}; // no stale layout publication during allocation/fill callbacks
        const auto result = owner_.ensure({candidate.bytes, 1, 0x3e, 4, 0, false});
        if (result.status) { layout_ = {}; return result.status; }
        if (result.state != TglOwnedResource<Backend>::State::Backed) {
            retire(); return 5;
        }
        if (result.changed) {
            const int status = owner_.withStorage([&](const auto& storage) noexcept {
                return fill(storage,candidate.bytes);
            });
            if (status) { retire(); return status; }
        }
        layout_ = candidate;
        return 0;
    }
    void reset() noexcept {
        if(busy_) return;
        busy_=true;retire();busy_=false;
    }
    // Synchronous descriptor/state consumption only. A GPU submission must
    // separately retain this owner until verified completion; never escape
    // resource/layout references from the callback.
    template<class Consume> int withResource(Consume consume) noexcept {
        static_assert(noexcept(consume(owner_.storage(),layout_)),"statistics lease status ABI");
        if(busy_ || !layout_.bytes) return 5;
        busy_=true;
        struct Unlock {bool& busy;~Unlock(){busy=false;}} unlock{busy_};
        return owner_.withStorage([&](const auto& storage) noexcept {
            return consume(storage,static_cast<const TglStatisticsAllocation&>(layout_));
        });
    }
    const TglStatisticsAllocation &layout() const noexcept { return layout_; }
private:
    void retire() noexcept { owner_.reset();layout_={}; }
    TglOwnedResource<Backend> owner_;
    TglStatisticsAllocation layout_{};
    bool busy_=false;
};

// One borrowed-OS resource lifetime. The renderer must close every such scope
// before retiring RenderHal/OS. Member order guarantees resource destruction
// while its backend still exists; close is terminal and idempotent.
class TglNativeResourceScope {
public:
    using Owner = TglOwnedResource<TglNativeResourceBackend>;
    TglNativeResourceScope(const TglNativeResourceBinding& binding,
                           uint32_t type,uint64_t nativePageSize) noexcept
        :backend_(binding,type,nativePageSize),owner_(backend_) {}
    TglNativeResourceScope(void *context, TglNativeResourceBackend::Allocate allocate,
                           TglNativeResourceBackend::Release release,
                           uint32_t type, uint64_t nativePageSize = 0) noexcept
        : backend_(context, allocate, release, type, nativePageSize), owner_(backend_) {}
    ~TglNativeResourceScope() { close(); }
    TglNativeResourceScope(const TglNativeResourceScope &) = delete;
    TglNativeResourceScope &operator=(const TglNativeResourceScope &) = delete;
    Owner::Result ensure(const TglResourceKey &key) noexcept {
        if (closed_) return {5, Owner::State::Empty, false};
        return owner_.ensure(key);
    }
    void close() noexcept {
        owner_.reset();
        closed_ = true;
    }
private:
    TglNativeResourceBackend backend_;
    Owner owner_;
    bool closed_ = false;
};
