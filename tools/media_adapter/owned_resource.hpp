#pragma once
#include <array>
#include <algorithm>
#include <cstdint>
#include <utility>
#include <cstring>
#include <limits>
#include <cmath>
#include <cstddef>
#include <type_traits>
#include <atomic>
#include <new>
#if defined(__APPLE__)
#include <dlfcn.h>
#endif

// Tahoe TGL Ief 125e90/126210. Tables must come from the authenticated
// native image (741e50..742350), not from a different GPU generation.
// This is a CPU data producer only; it does not issue an SFC command.
using TglOwnedIefTables = std::array<std::array<uint32_t,64>,5>;
// Exact ICL VP +28, 1dea75..1deb5b: ratios published at +928/+92c.
// Inputs are the already aligned source and destination extents. This owned
// producer rejects degenerate dimensions rather than publishing infinities.
struct TglOwnedVpScale {
    float at928=0,at92c=0;
    bool scaling=false;
};
struct TglOwnedVpDestinationRect {
    float left=0,top=0,right=0,bottom=0;
};
static_assert(sizeof(TglOwnedVpDestinationRect)==16);
// +18 1de411..1de4bc and +28 1deb61..1dec07 share this ordered predicate.
// NaN fails containment and therefore requires the conditional source, just
// like native ucomiss/jb; geometry admission separately rejects malformed data.
inline bool tglOwnedVpConditionalSource(bool available,
    const TglOwnedVpDestinationRect& source,const TglOwnedVpDestinationRect& target) noexcept {
    return available && !(target.left>=source.left && target.top>=source.top &&
                          source.right>=target.right && source.bottom>=target.bottom);
}
template<class Read>
bool tglReadOwnedVpConditionalSource(bool available,uintptr_t source,uintptr_t target,
    Read read,bool& output) noexcept {
    static_assert(noexcept(read(uintptr_t{},nullptr,size_t{})),"VP rectangle leased read ABI");
    if(!available) {output=false;return true;}
    if(!source || !target || source>UINTPTR_MAX-0x50 || target>UINTPTR_MAX-0x50) return false;
    TglOwnedVpDestinationRect sourceRect,targetRect;
    if(!read(source+0x40,&sourceRect,16) || !read(target+0x40,&targetRect,16)) return false;
    output=tglOwnedVpConditionalSource(true,sourceRect,targetRect);return true;
}
// Exact VP +68=1ddbe0..1ddc6c, final +18 admission predicate. Ordered
// equality is intentional: NaN source origins cannot take the zero-origin
// shortcut. This is a data predicate, not a resource allocation hook.
inline bool tglOwnedVpFinalAdmission(bool conditionalSource,float source40,
    float source44,uint32_t targetE4,uint32_t target130) noexcept {
    if(!conditionalSource && source44==0.0f && source40==0.0f && targetE4==1)
        return true;
    return target130!=0x19 && target130!=0x53 && target130!=0x52;
}
inline bool tglPrepareOwnedVpScale(uint32_t sourceWidth,uint32_t sourceHeight,
    uint32_t destinationWidth,uint32_t destinationHeight,uint32_t rotation,
    TglOwnedVpScale& output) noexcept {
    if(!sourceWidth || !sourceHeight || !destinationWidth || !destinationHeight)
        return false;
    const bool direct=rotation==0 || rotation==2 || rotation==4 || rotation==5;
    TglOwnedVpScale candidate;
    candidate.at928=float(direct?destinationWidth:destinationHeight)/float(sourceWidth);
    candidate.at92c=float(direct?destinationHeight:destinationWidth)/float(sourceHeight);
    candidate.scaling=candidate.at928!=1.0f || candidate.at92c!=1.0f;
    output=candidate;
    return true;
}
// VP +18 1de11a..1de4ef, after +20 eligibility and +50/rectangle alignment.
// Extents are qualified/aligned CPU values, not unchecked user descriptors.
struct TglOwnedVpExtentAdmission {
    uint32_t minWidth=0,maxWidth=0,minHeight=0,maxHeight=0;
    uint32_t frameWidth=0,frameHeight=0,sourceWidth=0,sourceHeight=0;
    uint32_t destinationWidth=0,destinationHeight=0,targetWidth=0,targetHeight=0;
    uint32_t rotation=0,source124=0,targetE4=0,target130=0;
    float source30=0,source34=0,source38=0,source3c=0,source40=0,source44=0;
    bool workaroundA4Bit25=false,conditionalSource=false;
};
inline bool tglPrepareOwnedVpExtentAdmission(const TglOwnedVpExtentAdmission& in,
                                             TglOwnedVpScale& output) noexcept {
    if(in.minWidth>in.maxWidth || in.minHeight>in.maxHeight) return false;
    const auto width=[&](uint32_t value) noexcept {
        return value>=in.minWidth && value<=in.maxWidth;
    };
    const auto height=[&](uint32_t value) noexcept {
        return value>=in.minHeight && value<=in.maxHeight;
    };
    if(!width(in.frameWidth) || !height(in.frameHeight) ||
       !width(in.sourceWidth) || !height(in.sourceHeight) ||
       !width(in.destinationWidth) || !height(in.destinationHeight) ||
       !width(in.targetWidth) || !height(in.targetHeight)) return false;
    TglOwnedVpScale candidate;
    if(!tglPrepareOwnedVpScale(in.sourceWidth,in.sourceHeight,
        std::min(in.destinationWidth,in.targetWidth),
        std::min(in.destinationHeight,in.targetHeight),in.rotation,candidate)) return false;
    if(candidate.at928<0.125f || candidate.at928>8.0f ||
       candidate.at92c<0.125f || candidate.at92c>8.0f) return false;
    // Authenticated 780788 constant is 1120. Native converts frameHeight
    // through signed int32 in this workaround; reject an unrepresentable shape.
    if(in.workaroundA4Bit25 && in.frameHeight>1120) {
        if(in.frameHeight>uint32_t(std::numeric_limits<int32_t>::max()) ||
           !std::isfinite(in.source30) || !std::isfinite(in.source34) ||
           !std::isfinite(in.source38) || !std::isfinite(in.source3c)) return false;
        const float h=float(int32_t(in.frameHeight));
        if(in.source30>0 || float(in.frameWidth)-in.source38>0 ||
           (in.source3c>1120.0f && h>in.source3c) ||
           (in.source34>1120.0f && h>in.source34) || h>in.source3c) return false;
    }
    if(in.source124==1 || !tglOwnedVpFinalAdmission(in.conditionalSource,
        in.source40,in.source44,in.targetE4,in.target130)) return false;
    output=candidate;
    return true;
}
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
    // Donor factory1fc790 allocates9d8; ctor1fcaa0 constructs this interface
    // at object+8. Factory1fc773 applies primary vtable[-18]=8 (793fc0).
    // Success1fc809 increments829ff8; that is native allocation accounting,
    // NOT permission to publish this data-only caller storage to native delete.
    std::array<uint8_t,0x9d0> bytes{};
    // Borrowed constructor inputs follow ICL1dd150 base10/18/20. Authenticate
    // the TGL MHW object independently; callers retain all interface/image
    // leases. This initializes data only, not a callable native object.
    // Exact ICL1dd150..1ddb58 ends with 4d4d0(this+28,400,200):
    // zero storage is NOT a complete constructor. The separate owned AVS
    // owner supplies its invalid-format sentinel and tracked coefficient
    // allocation under a lease; never publish this storage before binding it.
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
// Exact1dece6/1decf4/1decff/1ded0a borrowed publications from slot28.
// Input leases and the AVS header lease must enclose this entire scope; this
// does not initialize remaining VP state or permit references to escape.
template<class Use>
int tglWithOwnedVpFrameReferences(TglOwnedVpSfcCallerStorage& storage,
    uintptr_t conditionalSource,uintptr_t parameters,uintptr_t target,
    bool sourceEnabled,Use use) noexcept {
    static_assert(noexcept(use(static_cast<void*>(nullptr))),"VP borrowed frame scope ABI");
    constexpr std::array<size_t,4> offsets{0x988,0x980,0x990,0x9b0};
    for(size_t offset:offsets) {
        uintptr_t existing=0;std::memcpy(&existing,storage.bytes.data()+offset,8);
        if(existing) return 5; // never overwrite another live frame's references
    }
    const std::array<uintptr_t,4> values{
        sourceEnabled?conditionalSource:0,
        reinterpret_cast<uintptr_t>(storage.bytes.data()+0x28),parameters,target};
    for(size_t i=0;i<offsets.size();++i)
        std::memcpy(storage.bytes.data()+offsets[i],&values[i],8);
    const int result=use(storage.bytes.data());
    const uintptr_t zero=0;
    for(size_t offset:offsets) std::memcpy(storage.bytes.data()+offset,&zero,8);
    return result;
}
// Exact donor1dde10 eligibility data, not an ICL object view or TGL binding.
// Caller snapshots these fields only under authenticated object leases.
// Keep offset names until their semantic producer is independently qualified.
struct TglOwnedVpAdmissionInputs {
    uint8_t child08=0;
    uint32_t first90=0;
    int32_t second290=0;
    uint32_t first98_e4=0;
    bool second88Present=false;
    uint32_t second130=0;
    uint8_t second83=0;
};
struct TglOwnedVpPairInputs {
    bool firstPresent=false,secondPresent=false,thirdPresent=false;
    uint32_t first130=0,second130=0,third04=0;
};
// Exact donor secondary VP address point7940e8, reached by ctor1fcaa0:
// slot70 thunk1f71f0->1f70a0; slot78 thunk1f7300->1f7220. Translate
// only their format data; never install the virtual-base-adjusting thunks.
inline bool tglOwnedVpInputFormat(uint32_t format) noexcept {
    switch(format) {
        case 0x19:case 0x15:case 0x53:case 0x52:case 0x17:case 0x13:
        case 3:case 4:case 1:case 2:case 0xfffffff9u:case 0xd:case 0xe:
        case 0xf:case 0x10:case 0x11:case 0x12:case 0x14:return true;
        default:return false;
    }
}
inline bool tglOwnedVpOutputFormat(uint32_t format,uint32_t surfaceE4) noexcept {
    switch(format) {
        case 1:case 2:case 3:case 4:case 0x50:case 0x51:case 0xfffffff8u:
        case 0x19:case 0xd:case 0x10:case 0x15:return true;
        default:return format==0x53 && surfaceE4==1;
    }
}
// Donor1df050: retain virtual70/78 as separately qualified predicates. No
// cross-generation function pointer or guessed format support is supplied.
template<class Qualify70,class Qualify78>
bool tglOwnedVpPairAdmission(const TglOwnedVpPairInputs& input,
                            Qualify70 qualify70,Qualify78 qualify78) noexcept {
    static_assert(noexcept(qualify70()) && noexcept(qualify78()),"VP pair predicate ABI");
    if(!input.firstPresent || !input.secondPresent || !qualify70()) return false;
    if(input.second130==0x5a || input.second130==0x55 || !qualify78()) return false;
    if(input.thirdPresent && (input.third04==0 || input.third04==3)) {
        const bool second=input.second130==1 || input.second130==3 ||
            input.second130==0x50 || input.second130==0x51 || input.second130==0x15;
        const bool first=input.first130==3 || input.first130==1 || input.first130==0x15;
        if(second && first) return false;
    }
    return true;
}
template<class Qualify38>
bool tglOwnedVpAdmission(const TglOwnedVpAdmissionInputs& input,
                        Qualify38 qualify38) noexcept {
    static_assert(noexcept(qualify38()),"VP slot38 predicate must not throw");
    // 38480 reads only child08 bit0. Mode compare at1dde6c is SIGNED;
    // do not silently turn negative modes into an unsigned >3 rejection.
    if((input.child08&1) || input.first90!=1 ||
       (input.second290!=0 && (input.second290>3 || input.first98_e4!=1))) return false;
    if(!qualify38()) return false;
    if(input.second88Present && (input.second130==1 || input.second130==3)) return false;
    return !(input.second83&1);
}
// Read must authorize every span against currently held owned/borrowed
// object leases, not merely test whether an address is mapped. This evaluator
// performs no object adoption, writes or native calls.
template<class Read>
bool tglReadOwnedVpPairAdmission(uintptr_t first,uintptr_t second,
                               uintptr_t third,Read read) noexcept {
    static_assert(noexcept(read(uintptr_t{},nullptr,size_t{})),"VP leased read ABI");
    if(!first || !second || first>UINTPTR_MAX-0x134 || second>UINTPTR_MAX-0x134 ||
       (third && third>UINTPTR_MAX-8)) return false;
    TglOwnedVpPairInputs input;input.firstPresent=input.secondPresent=true;
    input.thirdPresent=third!=0;
    if(!read(first+0x130,&input.first130,4) || !tglOwnedVpInputFormat(input.first130) ||
       !read(second+0x130,&input.second130,4) ||
       input.second130==0x5a || input.second130==0x55) return false;
    uint32_t e4=0;
    if(input.second130==0x53 && !read(second+0xe4,&e4,4)) return false;
    if(!tglOwnedVpOutputFormat(input.second130,e4)) return false;
    if(third && !read(third+4,&input.third04,4)) return false;
    return tglOwnedVpPairAdmission(input,[]() noexcept {return true;},
        []() noexcept {return true;});
}
template<class Read>
bool tglReadOwnedVpAdmission(uint8_t child08,uintptr_t first,
                           uintptr_t second,Read read) noexcept {
    static_assert(noexcept(read(uintptr_t{},nullptr,size_t{})),"VP leased read ABI");
    if((child08&1) || !first || !second || first>UINTPTR_MAX-0xf8 ||
       second>UINTPTR_MAX-0x294) return false;
    TglOwnedVpAdmissionInputs input;input.child08=child08;
    if(!read(first+0x90,&input.first90,4) || input.first90!=1 ||
       !read(second+0x290,&input.second290,4) || input.second290>3) return false;
    uintptr_t surface=0,third=0;
    if(!read(first+0x98,&surface,8) || !surface || surface>UINTPTR_MAX-0x134) return false;
    if(input.second290!=0 &&
       (!read(surface+0xe4,&input.first98_e4,4) || input.first98_e4!=1)) return false;
    if(!read(first+0xf0,&third,8) || !tglReadOwnedVpPairAdmission(second,surface,third,read))
        return false;
    uintptr_t optional=0;
    if(!read(second+0x88,&optional,8)) return false;
    input.second88Present=optional!=0;
    if(optional && (!read(second+0x130,&input.second130,4) ||
       input.second130==1 || input.second130==3)) return false;
    if(!read(second+0x83,&input.second83,1)) return false;
    // The qualified pair was already evaluated once above; never invoke it
    // twice when composing the outer data predicate.
    return tglOwnedVpAdmission(input,[]() noexcept {return true;});
}
// Synchronous admission context only: the caller retains storage and every
// reader-authorized input lease through the enclosing scoped callback. This
// is not a VP constructor, vtable publisher or GPU submission lifetime.
template<class Read>
bool tglReadOwnedSfcFrame(uintptr_t surface,uintptr_t mhw,Read read,
                        uint32_t& height,uint32_t& width) noexcept;
template<class Read> class TglOwnedVpAdmissionContext {
    static_assert(noexcept(std::declval<Read&>()(uintptr_t{},nullptr,size_t{})),"VP leased read ABI");
    TglOwnedVpSfcCallerStorage& storage_;
    Read& read_;
    bool format(void* surface,bool output) noexcept {
        const uintptr_t address=reinterpret_cast<uintptr_t>(surface);
        uint32_t value=0,e4=0;
        if(!address || address>UINTPTR_MAX-0x134 || !read_(address+0x130,&value,4)) return false;
        if(!output) return tglOwnedVpInputFormat(value);
        if(value==0x53 && !read_(address+0xe4,&e4,4)) return false;
        return tglOwnedVpOutputFormat(value,e4);
    }
public:
    TglOwnedVpAdmissionContext(TglOwnedVpSfcCallerStorage& storage,Read& read) noexcept
        :storage_(storage),read_(read) {}
    TglOwnedVpAdmissionContext(const TglOwnedVpAdmissionContext&)=delete;
    TglOwnedVpAdmissionContext& operator=(const TglOwnedVpAdmissionContext&)=delete;
    bool owns(void* child) const noexcept {return child==storage_.bytes.data();}
    bool eligible(void* first,void* second,void*) noexcept {
        return tglReadOwnedVpAdmission(storage_.bytes[8],reinterpret_cast<uintptr_t>(first),
            reinterpret_cast<uintptr_t>(second),read_);
    }
    bool pair(void* first,void* second,void* third) noexcept {
        return tglReadOwnedVpPairAdmission(reinterpret_cast<uintptr_t>(first),
            reinterpret_cast<uintptr_t>(second),reinterpret_cast<uintptr_t>(third),read_);
    }
    bool input(void* surface) noexcept {return format(surface,false);}
    bool output(void* surface) noexcept {return format(surface,true);}
    bool finalAdmission(uint8_t conditional,void* source,void* target) noexcept {
        const auto s=reinterpret_cast<uintptr_t>(source),t=reinterpret_cast<uintptr_t>(target);
        if(!s || !t || s>UINTPTR_MAX-0x48 || t>UINTPTR_MAX-0x134) return false;
        if(!(conditional&1)) {
            float top=0,left=0;uint32_t tile=0;
            if(!read_(s+0x44,&top,4)) return false;
            if(top==0.0f) {
                if(!read_(s+0x40,&left,4)) return false;
                if(left==0.0f) {
                    if(!read_(t+0xe4,&tile,4)) return false;
                    if(tile==1) return true;
                }
            }
        }
        uint32_t value=0;
        return read_(t+0x130,&value,4) && tglOwnedVpFinalAdmission(true,0,0,0,value);
    }
    bool dimensions(void* surface,uint32_t* width,uint32_t* height) noexcept {
        if(!width || !height || width==height) return false;
        uintptr_t mhw=0;std::memcpy(&mhw,storage_.bytes.data()+0x20,8);
        uint32_t candidateHeight=0,candidateWidth=0;
        if(!tglReadOwnedSfcFrame(reinterpret_cast<uintptr_t>(surface),mhw,read_,
                                candidateHeight,candidateWidth)) return false;
        // Native slot50 RDX is width, RCX is height. The CPU helper's API
        // intentionally names height first; do not forward arguments blindly.
        *height=candidateHeight;*width=candidateWidth;return true;
    }
};
template<class Resolve> struct TglOwnedVpAdmissionCallbacks {
    static_assert(noexcept(Resolve{}(nullptr)),"VP admission resolver ABI");
    static bool eligible(void* child,void* first,void* second,void* third) noexcept {
        auto* context=Resolve{}(child);
        return context && context->owns(child) && context->eligible(first,second,third);
    }
    static bool pair(void* child,void* first,void* second,void* third) noexcept {
        auto* context=Resolve{}(child);
        return context && context->owns(child) && context->pair(first,second,third);
    }
    static bool input(void* child,void* surface) noexcept {
        auto* context=Resolve{}(child);
        return context && context->owns(child) && context->input(surface);
    }
    static bool output(void* child,void* surface) noexcept {
        auto* context=Resolve{}(child);
        return context && context->owns(child) && context->output(surface);
    }
    static bool finalAdmission(void* child,uint8_t conditional,void* source,void* target) noexcept {
        auto* context=Resolve{}(child);
        return context && context->owns(child) && context->finalAdmission(conditional,source,target);
    }
    static void dimensions(void* child,void* surface,uint32_t* width,uint32_t* height) noexcept {
        auto* context=Resolve{}(child);
        if(context && context->owns(child)) context->dimensions(surface,width,height);
    }
};
// Caller ABI: TGL12c010 invokes VP+40(this,source,target,renderData),
// TGL12a120 invokes VP+48(this,renderData,command). Resolve must admit only
// the exact currently leased owned VP base; no native/foreign object cast.
// These two entries alone are NOT a complete/publishable VP vtable.
// Exact donor base ctor1dd173 installs7937f0; its slots18/20/28 are
// 1ddf10/1dde10/1de810, before30=1df1b0 and38=1df050. Slot18 saves
// three arguments, calls bool20(this,arg3,arg1,arg2), then50 with dimension
// outputs. Slot20 itself calls bool38 and rejects unsupported surface state.
// Slot18 is NOT bool ABI: 1ddf33 initializes a DWORD result and
// 1de4df/1de4e8 store1/0; 1de4f1..1de4f7 returns the full EAX. The future
// complete table must use int(this,arg1,arg2,arg3), unlike bool20/bool38.
// These are donor dependency evidence, NOT callable TGL bindings: complete
// owned preflight/initialization must precede publication through VEBOX1e0.
template<class Resolve> struct TglOwnedVpSfcCallbacks {
    // ICL1f88a7 invokes VP+30 as void(this). The enclosing owner must
    // independently authorize GPU retirement; this adapter never invents a
    // fence, destroys the CPU object, or converts the void ABI into status.
    static void freeResources(void* child) noexcept {
        static_assert(noexcept(Resolve{}(child)),"VP lease resolution must not throw");
        auto* context=Resolve{}(child);
        if(!context || !context->owns(child)) return;
        static_assert(noexcept(context->freeResources()),"VP resource cleanup must not throw");
        context->freeResources();
    }
    static int setup(void* child,void* source,void* target,void* renderData) noexcept {
        static_assert(noexcept(Resolve{}(child)),"VP lease resolution must not throw");
        auto* context=Resolve{}(child);
        if(!context || !context->owns(child) || !source || !target || !renderData) return 5;
        static_assert(noexcept(context->setup(source,target,renderData)),"VP setup status ABI");
        return context->setup(source,target,renderData);
    }
    static int send(void* child,void* renderData,void* command) noexcept {
        static_assert(noexcept(Resolve{}(child)),"VP lease resolution must not throw");
        auto* context=Resolve{}(child);
        if(!context || !context->owns(child) || !renderData || !command) return 5;
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
    // State168443..168470 calls OS+1b0 and rejects a NULL result. Exact
    // initializer6603a/66041 binds62ee0;62ee0..62f24 returns *(OS)+15ce4
    // or NULL for a missing context. Authenticate without invoking the callback.
    // This is only a state-builder prerequisite, NOT table-content validation,
    // a resource lease, or permission to submit/retire GPU work.
    template<class Read>
    bool validateStateEnvironment(uintptr_t object,uintptr_t os,Read read) const {
        if(!validateConstructedObject(object,os,read) || os>UINTPTR_MAX-0x1b8)
            return false;
        uintptr_t entry=0,context=0;
        return read(os+0x1b0,&entry,8) && entry==image+0x62ee0 &&
            read(os,&context,8) && context && context<=UINTPTR_MAX-0x15ce4;
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
    uint64_t generation_=0;
    bool constructed=false,ready=false,busy=false,reserved=false;
    void clear() noexcept {
        if(generation_!=UINT64_MAX) ++generation_;
        ready=false;
        if(constructed){constructed=false;invoke.destroy(lifetime.destroy,storage.data());}
        lifetime={};borrowedOs=0;storage.fill(0);
    }
public:
    explicit TglOwnedSfcMhw(Invoker &invoker):invoke(invoker){}
    ~TglOwnedSfcMhw(){reset();}
    TglOwnedSfcMhw(const TglOwnedSfcMhw&)=delete;
    TglOwnedSfcMhw& operator=(const TglOwnedSfcMhw&)=delete;
    template<class Read>
    bool initialize(const TglSfcLifetimeBinding &binding,const TglSfcMhwBinding &mhw,
                    uintptr_t os,Read read) {
        if(busy || reserved || generation_==UINT64_MAX) return false;
        reset();
        if(generation_==UINT64_MAX) return false;
        struct Guard {
            bool& value;
            explicit Guard(bool& flag) noexcept:value(flag){value=true;}
            ~Guard(){value=false;}
        } guard(busy);
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
        if(!mhw.validateConstructedObject(object,os,objectRead)){clear();return false;}
        borrowedOs=os;ready=true;return true;
    }
    void *get() noexcept {return ready?storage.data():nullptr;}
    uintptr_t image() const noexcept {return ready?lifetime.image:0;}
    uintptr_t os() const noexcept {return ready?borrowedOs:0;}
    uint64_t generation() const noexcept {return generation_;}
    // Pin CPU/native-object lifetime across a multi-command transaction while
    // retaining the independent per-call reentry guard. Not a GPU fence lease.
    template<class Use> int withCommandReservation(Use use) {
        if(busy || reserved || !ready) return 5;
        struct Guard {
            bool& flag;explicit Guard(bool& f):flag(f){flag=true;}
            ~Guard(){flag=false;}
        } guard(reserved);
        return use();
    }
    template<class Use> int withObject(Use use) {
        if(busy || !ready) return 5;
        struct Guard {
            bool& value;
            explicit Guard(bool& flag) noexcept:value(flag){value=true;}
            ~Guard(){value=false;}
        } guard(busy);
        return use(storage.data());
    }
    void reset() noexcept {
        if(busy || reserved) return;
        busy=true;clear();busy=false;
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
// TGL168972..1689e5 consumes +5c/+60 as ordered float equality to1.0
// (constant24ce84): DW4 bit7 is set unless BOTH ratios equal1, including NaN.
// TGL format19 dispatch16a494 ->16872e emits DW3 format4 and reads the
// separate output argument+24 as a16-bit plane value. Prefix qualification
// alone therefore cannot authorize an unqualified output-surface argument.
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
// Exact TGL168311 reads pipeMode byte0 and168333 reads flag byte8.
// Twelve owned bytes include both fields plus zeroed padding; eight bytes is
// the generated GPU command length, NOT the native CPU argument extent.
using TglOwnedSfcLockParameters=std::array<uint8_t,12>;
inline TglOwnedSfcLockParameters tglPrepareOwnedSfcLockParameters(
    uint8_t pipeMode,uint8_t settings13,uint8_t settings0b) noexcept {
    TglOwnedSfcLockParameters result{};
    result[0]=pipeMode;
    // Qualified donor1e0ebf..1e0ed9: either low bit enables the flag.
    result[8]=(settings13|settings0b)&1;
    return result;
}
template<class Read>
bool tglReadOwnedSfcLockParameters(uintptr_t settings,uint8_t pipeMode,Read read,
    TglOwnedSfcLockParameters& output) noexcept {
    static_assert(noexcept(read(uintptr_t{},nullptr,size_t{})),"SFC lock leased read ABI");
    if(!settings || settings>UINTPTR_MAX-0x14) return false;
    uint8_t flag13=0,flag0b=0;
    if(!read(settings+0x13,&flag13,1)) return false;
    if(!(flag13&1) && !read(settings+0xb,&flag0b,1)) return false;
    output=tglPrepareOwnedSfcLockParameters(pipeMode,flag13,flag0b);
    return true;
}
template<class SfcInvoker,class Invoke> class TglOwnedSfcCommandBackend {
    const TglSfcMhwBinding &binding;
    TglOwnedSfcMhw<SfcInvoker> &owner;
    void *command;
    Invoke &invoke;
    const uint64_t generation_;
    bool valid(size_t slot,uintptr_t offset) const noexcept {
        return command && owner.get() && generation_==owner.generation() &&
            generation_!=UINT64_MAX && binding.image==owner.image() &&
            binding.image && binding.entries[slot/8]==binding.image+offset;
    }
    template<class Use> int call(size_t slot,uintptr_t offset,Use use) {
        return owner.withObject([&](void* object) {
            if(!valid(slot,offset)) return 5;
            return use(binding.entries[slot/8],object);
        });
    }
public:
    TglOwnedSfcCommandBackend(const TglSfcMhwBinding &b,TglOwnedSfcMhw<SfcInvoker> &o,
                             void *c,Invoke &i):binding(b),owner(o),command(c),invoke(i),generation_(o.generation()){}
    template<class Use> int withCommandSequence(Use use) {
        return owner.withCommandReservation([&]() -> int {
            if(!valid(0x10,0x1681e0)) return 5;
            return use();
        });
    }
    int lock(const TglOwnedSfcLockParameters& parameters) {
        return call(0x10,0x1681e0,[&](uintptr_t entry,void* object){return invoke.parameters(entry,object,command,parameters.data());});
    }
    int state(const TglOwnedSfcStateParametersPacket& parameters,const std::array<uint8_t,0x38>& output) {
        return call(0x18,0x168390,[&](uintptr_t entry,void* object){return invoke.state(entry,object,command,parameters.data(),output.data());});
    }
    // Authenticate the leased OS state dependency immediately before native
    // dispatch. Keep the owner busy across both reader callbacks and dispatch:
    // a reader must not retire/reinitialize the object it is qualifying.
    template<class Read>
    int stateQualified(const TglOwnedSfcStateParametersPacket& parameters,
                       const std::array<uint8_t,0x38>& output,Read read) {
        return call(0x18,0x168390,[&](uintptr_t entry,void* object) {
            const uintptr_t address=reinterpret_cast<uintptr_t>(object);
            auto objectRead=[&](uintptr_t source,void* destination,size_t size) {
                if(source==address && size==0x41) {
                    std::memcpy(destination,object,size);return true;
                }
                return read(source,destination,size);
            };
            if(!binding.validateStateEnvironment(address,owner.os(),objectRead)) return 5;
            return invoke.state(entry,object,command,parameters.data(),output.data());
        });
    }
    int avs(const std::array<uint8_t,16>& parameters) {
        return call(0x20,0x16a580,[&](uintptr_t entry,void* object){return invoke.parameters(entry,object,command,parameters.data());});
    }
    int ief(const std::array<uint8_t,64>& parameters) {
        return call(0x30,0x16a8a0,[&](uintptr_t entry,void* object){return invoke.parameters(entry,object,command,parameters.data());});
    }
    int uv(const TglOwnedSfcUvParameters& parameters) {
        return call(0x38,0x16b190,[&](uintptr_t entry,void* object){return invoke.parameters(entry,object,command,&parameters);});
    }
    int y(const TglOwnedSfcYParameters& parameters) {
        return call(0x40,0x16b390,[&](uintptr_t entry,void* object){return invoke.parameters(entry,object,command,&parameters);});
    }
    int frameStart() {
        return call(0x28,0x16a770,[&](uintptr_t entry,void* object){return invoke.frame(entry,object,command,uint8_t(1));});
    }
};
// Scheduler-facing view: state cannot accidentally use the unqualified ABI
// adapter. Backend and the reader's borrowed OS/image leases must outlive this
// view. Qualification failure after lock means discard the partial command
// buffer, NOT submission; this view does not own asynchronous resources.
template<class Backend,class Read> class TglStateQualifiedSfcBackend {
    Backend& backend_;
    Read read_;
public:
    TglStateQualifiedSfcBackend(Backend& backend,Read read):backend_(backend),read_(read){}
    int lock(const TglOwnedSfcLockParameters& p){return backend_.lock(p);}
    int state(const TglOwnedSfcStateParametersPacket& p,const std::array<uint8_t,0x38>& o){
        return backend_.stateQualified(p,o,read_);
    }
    int avs(const std::array<uint8_t,16>& p){return backend_.avs(p);}
    int ief(const std::array<uint8_t,64>& p){return backend_.ief(p);}
    int y(const TglOwnedSfcYParameters& p){return backend_.y(p);}
    int uv(const TglOwnedSfcUvParameters& p){return backend_.uv(p);}
    int frameStart(){return backend_.frameStart();}
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

// Gen12 GetOutputPipe2456..2476 (media-driver73c8d14), before direct/SFC.
// hasDi/isBob/hasLut must come from qualified owned parameters; this does NOT
// assign a Darwin DIMode enum or guess a native LUT pointer offset. These are
// rejection predicates only, never generic-format or GPU admission proof.
// Cross-reference: media-driver73c8d14 vp_common_defs.h490 has BOB=0/ADI=1;
// vphal_render_vebox_base.cpp2401 enables ADI || (SFC && BOB). Tahoe
// 12a993/12a9a7/12a9d9 implements mode2 || (pipe1 && mode1), supporting
// a Darwin 2/1 semantic mapping, NOT numeric enum reuse. An owned DI producer
// and its lifetime/identity qualification are still required before wiring.
constexpr bool tglRejectGen12BobOutput(bool hasDi,bool isBob,uint32_t height,uint32_t format) noexcept {
    const bool planar420=format==0x19 || format==0x52 || format==0x53;
    return hasDi && isBob && (((height&3) && planar420) || !tglVeboxDiFormatSupported(format));
}
constexpr bool tglRejectGen12LutOutput(bool hasLut,uint32_t targetFormat) noexcept {
    return hasLut && (targetFormat==0x52 || targetFormat==0x53);
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
// Gen12 GetOutputPipe2499..2547. These references are owned state, NOT a
// Darwin surface layout: the caller must bind its qualified frame and native
// decompression resource. No guessed TGL compression/fast-fill offsets.
// Recheck observes the temporary live target rectangle. Bottom is restored
// on every exit. Native decompression failure preserves compressed state
// but does not cancel fast-fill; expose its exact status to the caller.
template<class Recheck,class Decompress,class Healthy>
bool tglRunOwnedGen12FastFill(const TglSurfaceRect& source,TglSurfaceRect& target,
    bool hasColorFill,bool& fastFill,bool& compressed,TglOwnedSfcCompressionMode& mode,
    uint32_t& compressionFormat,int& decompressionStatus,
    Recheck recheck,Decompress decompress,Healthy healthy) noexcept {
    static_assert(noexcept(recheck()) && noexcept(decompress()) && noexcept(healthy()) &&
        std::is_same_v<decltype(recheck()),bool> && std::is_same_v<decltype(decompress()),int> &&
        std::is_same_v<decltype(healthy()),bool>,"fast-fill synchronous resource policy ABI");
    if(!healthy() || !hasColorFill || !source.finite() || !target.finite() ||
       source.left!=target.left || source.top!=target.top || source.right!=target.right ||
       !(source.bottom<target.bottom)) return false;
    // Gen12 uses integer RECT heights. Darwin float storage must be qualified
    // before converting; no truncation/undefined float-to-integer conversion.
    const double height=double(source.bottom)-double(source.top);
    if(height<=0 || height>double(INT32_MAX) || std::trunc(height)!=height) return false;
    if(mode!=TglOwnedSfcCompressionMode::Disabled && mode!=TglOwnedSfcCompressionMode::Media &&
       mode!=TglOwnedSfcCompressionMode::Render) return false;
    const float original=target.bottom;
    struct Restore {float& value;float original;~Restore(){value=original;}} restore{target.bottom,original};
    target.bottom=source.bottom;
    const bool feasible=recheck();
    if(!healthy() || !feasible) return false;
    struct FlagRollback {bool& value;bool original;bool committed=false;
        ~FlagRollback(){if(!committed) value=original;}} flag{fastFill,fastFill};
    fastFill=true;
    int status=0;
    if(compressed && mode==TglOwnedSfcCompressionMode::Media && (int32_t(height)%8)!=0) {
        status=decompress();
        decompressionStatus=status; // completed native call, even on frame abort
        // Decompression is an irreversible resource operation, not a pending
        // selector publication. Reflect native success even if acquisition
        // became invalid during the call; do not reuse stale compression
        // metadata on a later frame. Pipe/fast-fill publication still aborts.
        if(status==0) {compressed=false;mode=TglOwnedSfcCompressionMode::Disabled;compressionFormat=0;}
        if(!healthy()) return false;
    }
    flag.committed=true;decompressionStatus=status;return true;
}
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
bool tglCompositionBypassFromLiveFlag(uintptr_t params,uintptr_t source,
    const uint8_t& compNeeded,Read read) {
    constexpr auto max = std::numeric_limits<uintptr_t>::max();
    if (!params || !source || params > max - 0xdf || source > max - 0x82)
        return false;
    uint32_t sources = 0, targets = 0;
    uintptr_t blending = 0, lumaKey = 0, constriction = 0;
    uint8_t interlaced = 0, weaving = 0;
    return !(compNeeded & 1) &&
        read(params, &sources, 4) && sources == 1 &&
        read(params + 0x90, &targets, 4) && targets == 1 &&
        read(source + 0x60, &blending, 8) && !blending &&
        read(source + 0x81, &interlaced, 1) && !(interlaced & 1) &&
        read(source + 0x82, &weaving, 1) && !(weaving & 1) &&
        read(source + 0x68, &lumaKey, 8) && !lumaKey &&
        read(params + 0xd8, &constriction, 8) && !constriction;
}
// Pointer compatibility entry: pass denotes the bCompNeeded BYTE, not the
// start of an opaque render pass. Admission uses the live-reference entry
// above with its already-pinned pass08, avoiding address/offset confusion.
template<class Read>
bool tglCompositionBypassFeasible(uintptr_t params,uintptr_t source,uintptr_t pass,Read read) {
    if(!params || !source || !pass || params>UINTPTR_MAX-0xdf || source>UINTPTR_MAX-0x82) return false;
    uint8_t comp=0;
    return read(pass,&comp,1) && tglCompositionBypassFromLiveFlag(params,source,comp,read);
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

// One acquisition lifetime per pinned frame. There is deliberately no reset:
// a failed frame cannot become healthy by reusing its reader/selector.
class TglVeboxPipeAcquisition {
    bool failed_=false;
public:
    TglVeboxPipeAcquisition()=default;
    TglVeboxPipeAcquisition(const TglVeboxPipeAcquisition&)=delete;
    TglVeboxPipeAcquisition& operator=(const TglVeboxPipeAcquisition&)=delete;
    bool healthy() const noexcept {return !failed_;}
    void fail() noexcept {failed_=true;}
};
// Copies passed through existing predicates share the same failure latch.
// Missing/overflowing named reads are acquisition failure, not format rejection.
template<class Read> class TglVeboxPipeReadGate {
    Read& read_;TglVeboxPipeAcquisition& acquisition_;
public:
    TglVeboxPipeReadGate(Read& read,TglVeboxPipeAcquisition& acquisition) noexcept
        :read_(read),acquisition_(acquisition){}
    bool operator()(uintptr_t address,void* output,size_t bytes) const noexcept {
        static_assert(noexcept(read_(address,output,bytes)) &&
            std::is_same_v<decltype(read_(address,output,bytes)),bool>,"bounded pipe read ABI");
        if(!acquisition_.healthy()) return false;
        if(!address || !output || !bytes || address>UINTPTR_MAX-(bytes-1) ||
           !read_(address,output,bytes)) {acquisition_.fail();return false;}
        return true;
    }
};

// ICL1fb581/1fb585 reads render params+e0 before its fast-fill geometry
// checks; Gen12 GetOutputPipe2503 tests pColorFillParams at the same stage.
// Only presence is consumed here. The enclosing frame owns that separately
// leased allocation; no dereference, guessed parameter extent or adoption.
template<size_t N>
bool tglAcquireOwnedColorFillPresence(const std::array<uint8_t,N>& params,
    uintptr_t leasedColorFill,TglVeboxPipeAcquisition& frame,bool& present) noexcept {
    static_assert(N>=0xe8,"qualified render params color-fill slot extent");
    if(!frame.healthy()) return false;
    uintptr_t actual=0;std::memcpy(&actual,params.data()+0xe0,sizeof(actual));
    if(actual!=leasedColorFill) {frame.fail();return false;}
    present=actual!=0;return true;
}

// Gen12 GetOutputPipe, intel/media-driver73c8d14 g12_base.cpp2426..2550.
// Policy owns qualified params/source/target leases and implements complete
// predicates, SFC selection and fast-fill temporary rectangle/decompression
// transaction. No ICL this-adjustment or TGL base68 dispatch belongs here.
// Invalid SFC output fails without publishing mode/comp flags; normal exits
// overwrite the whole bCompNeeded byte, not OR a stale admission flag.
template<class Policy>
bool tglSelectGen12VeboxOutputPipe(Policy& policy,uint8_t& compNeeded,uint32_t& output) noexcept {
    static_assert(noexcept(policy.bypass()) && noexcept(policy.rejectDi()) &&
        noexcept(policy.rejectLut()) && noexcept(policy.direct()) && noexcept(policy.twoPass()) &&
        noexcept(policy.sfcAvailable()) && noexcept(policy.hdr()) && noexcept(policy.sfc()) &&
        noexcept(policy.fastFill()) && noexcept(policy.healthy()),"qualified synchronous pipe policy ABI");
    static_assert(std::is_same_v<decltype(policy.sfc()),uint32_t>,"SFC pipe mode ABI");
    static_assert(std::is_same_v<decltype(policy.bypass()),bool> &&
        std::is_same_v<decltype(policy.rejectDi()),bool> && std::is_same_v<decltype(policy.rejectLut()),bool> &&
        std::is_same_v<decltype(policy.direct()),bool> && std::is_same_v<decltype(policy.twoPass()),bool> &&
        std::is_same_v<decltype(policy.sfcAvailable()),bool> && std::is_same_v<decltype(policy.hdr()),bool> &&
        std::is_same_v<decltype(policy.fastFill()),bool> && std::is_same_v<decltype(policy.healthy()),bool>,
        "pipe predicates must not interpret error status as true");
    auto finish=[&](uint32_t pipe) noexcept {output=pipe;compNeeded=pipe==0?1:0;return true;};
    if(!policy.healthy()) return false;
    bool value=policy.bypass();if(!policy.healthy()) return false;
    if(!value) return finish(0);
    value=policy.rejectDi();if(!policy.healthy()) return false;
    if(value) return finish(0);
    value=policy.rejectLut();if(!policy.healthy()) return false;
    if(value) return finish(0);
    value=policy.direct();if(!policy.healthy()) return false;
    if(value) return finish(2);
    value=policy.twoPass();if(!policy.healthy()) return false;
    if(value) return finish(0);
    uint32_t pipe=0;
    const bool available=policy.sfcAvailable();if(!policy.healthy()) return false;
    bool hdr=false;
    if(available) {hdr=policy.hdr();if(!policy.healthy()) return false;}
    if(available && !hdr) {
        pipe=policy.sfc();
        if(!policy.healthy() || pipe>2) return false;
    }
    if(pipe==0) {
        value=policy.fastFill();if(!policy.healthy()) return false;
        if(value) pipe=2;
    }
    return finish(pipe);
}

// Bridge checked policy to admission's uint32 mode ABI. A failed selector
// must NOT turn into composition0 (which is a legitimate successful choice).
// Admission rejects this out-of-domain sentinel before mode/history publication.
template<class Policy> class TglGen12VeboxPipeSelector {
    Policy& policy_;
public:
    explicit TglGen12VeboxPipeSelector(Policy& policy) noexcept:policy_(policy){}
    uint32_t operator()(uint8_t& compNeeded) const noexcept {
        uint32_t mode=UINT32_MAX;
        return tglSelectGen12VeboxOutputPipe(policy_,compNeeded,mode)?mode:UINT32_MAX;
    }
};

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
    bool borrowed = false;
public:
    explicit TglTrackedCpuOwner(Backend& b) : backend(b) {}
    TglTrackedCpuOwner(const TglTrackedCpuOwner&) = delete;
    TglTrackedCpuOwner& operator=(const TglTrackedCpuOwner&) = delete;
    ~TglTrackedCpuOwner() { reset(); }
    bool allocate(size_t bytes) {
        if (borrowed || storage || !bytes) return false;
        auto candidate = backend.allocate(bytes);
        if (!candidate) return false;
        storage = candidate; extent = bytes; return true;
    }
    void reset() noexcept {
        if (borrowed || !storage) return;
        const auto old = storage; storage = 0; extent = 0;
        backend.release(old);
    }
    uintptr_t get() const { return storage; }
    size_t size() const { return extent; }
    // Non-consuming synchronous borrow. No native release/ownership transfer
    // is permitted; reset, reallocation and consuming-slot entry stay blocked.
    template<class Use>
    bool withBorrowedAddress(Use use) noexcept {
        static_assert(noexcept(use(uintptr_t{},size_t{})) &&
            std::is_same_v<decltype(use(uintptr_t{},size_t{})),void>,"tracked CPU borrow ABI");
        if(borrowed || !storage) return false;
        borrowed=true;
        struct Restore {bool& borrowed;~Restore(){borrowed=false;}} restore{borrowed};
        use(storage,extent);return true;
    }
    // Exact native160 source90 contract: unchanged means still owned; null
    // means paired native436d0 consumed it. Caller leases the surface slot
    // and authenticates the native callback/image. Never adopt foreign data.
    template<class Use>
    bool withNativeReleaseSlot(uint8_t* slot,Use use) noexcept {
        static_assert(noexcept(use()),"native consuming callback must not throw");
        static_assert(std::is_same_v<decltype(use()),void>,"native feature ABI is void");
        if(borrowed || !storage || !slot) return false;
        uintptr_t previous=0;std::memcpy(&previous,slot,sizeof(previous));
        if(previous) return false;
        const auto published=storage;
        borrowed=true;
        std::memcpy(slot,&published,sizeof(published));
        use();
        uintptr_t observed=0;std::memcpy(&observed,slot,sizeof(observed));
        // Detach the borrowed field even on an unexpected native mutation.
        std::memcpy(slot,&previous,sizeof(previous));
        if(!observed) { storage=0;extent=0; }
        borrowed=false;
        return !observed || observed==published;
    }
};

// Metadata1f7570 reads the first DWORD of source88 only while its actual
// tracked allocation is borrowed. Neither a surface lease nor a matching raw
// pointer alone permits this read. Preserve output on mismatch/read failure.
template<class Backend,class Read>
bool tglReadOwnedFfdiParameterKind(TglTrackedCpuOwner<Backend>& owner,
    uintptr_t expected,uint32_t& output,Read read) noexcept {
    static_assert(noexcept(read(uintptr_t{},static_cast<void*>(nullptr),size_t{})),
        "tracked metadata reader must not throw");
    if(!expected) return false;
    bool copied=false;uint32_t candidate=0;
    const bool entered=owner.withBorrowedAddress([&](uintptr_t pointer,size_t extent) noexcept {
        if(pointer!=expected || extent<sizeof(candidate) || pointer>UINTPTR_MAX-3) return;
        copied=read(pointer,&candidate,sizeof(candidate));
    });
    if(!entered || !copied) return false;
    output=candidate;return true;
}
// Native base helper126740 creates eight 0x2a8 CPU shells: 1b0/1b8,
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
// Exact TGL caller 12c1c3..12c1dd invokes child vtable+58 with child in
// RDI and consumes EAX as status; failure returns directly via 12c44e.
// This caller does NOT prove a FreeResources call on allocation failure.
// Do not substitute base helper 126740 (which saves RSI/RDX), nor copy
// ICL's virtual-base-adjusting Allocate/FreeResources thunks into this table.
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
    // Derived table75bba0 +28=131fa0 is NOT a compatible base hook: 131ff2/
    // 132000 read this+2cc8/+2cc0, beyond our exact1bc0 base allocation. Its
    // nonnull input98/resource148 gate (132015..132048) does not establish
    // this-layout compatibility. Keep +28 owned; never reuse that native entry.
    // This is the OUTER renderer multi-pass entry, not VEBOX's missing +28:
    // 1320e2 copies139 bytes of input; 132105 loops input+90 entries through
    // MOS61b50; 13218f writes MOS+9c, then MOS+300/+318 execute. Platform
    // +38 at13228f produces the pass count; each pass republishes88 bytes,
    // re-registers entries and calls renderer+40 (132467). +48 (1324a8) is
    // reached only after all passes; errors jump1324ab and increment+9ec.
    // A source-registration/status0 gate alone cannot replace this lifecycle.
    // +1e0 is pointer(this), not status(this): Allocate126998 tests SKU64
    // bit17 and null child80, calls1269ca with RDI=this, then stores RAX at80.
    // NULL becomes allocation error1 at1269e9. Do not bind a generic status
    // callback or omit the owned VP/SFC object merely to finish this table.
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
            // Admission retains these native base methods. +180 uses the
            // owned base90 history/mode state; neither is a donor thunk.
            // +170 calls +160 unconditionally (12ac01), +168 conditionally
            // (12ad14), and writes execution flags through +48's report.
            // +168=12ab10 is void(this,source): source138==3/4 writes
            // execution15; source138==3/5 writes execution16. These are the
            // full source SAMPLE enum, not the unrelated params138 flag.
            // +160=12a740 is NOT a read-only feature query: 12a929..12a940
            // releases source90 through436d0 and clears that pointer when
            // report0b is false.436d0 decrements native7f3218 before its
            // external release call. A borrowed/owned source90 therefore
            // needs allocator/accounting qualification, not just a base
            // lease, before +170 can transitively invoke this native method.
            // It also calls virtual+a0/+a8 and retains target in report60;
            // source/target backing must outlive the retained report use.
            candidate[0x160/8] != image+0x12a740 || candidate[0x168/8] != image+0x12ab10 ||
            candidate[0x170/8] != image+0x12aba0 || candidate[0x180/8] != image+0x12b1d0 ||
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
// Qualified VP+a0 1e09c7..1e0b1d override branch. +9c0 is a leased
// flag/matrix/input-offset object, not an executable callback. Equal color
// spaces or a disabled flag fall back to the native coefficient producer.
// This only snapshots owned data; it neither publishes pointer fields nor
// substitutes the VEBOX base's unrelated a0/c4/d0 object offsets.
template<class Read>
bool tglReadOwnedVpCscOverride(uint32_t from,uint32_t to,uintptr_t sourceA8,
    Read read,bool& useOverride,TglCscCoefficients& output) noexcept {
    static_assert(noexcept(read(uintptr_t{},nullptr,size_t{})),"VP CSC leased read ABI");
    if(!sourceA8 || from==to) {useOverride=false;return true;}
    if(sourceA8>UINTPTR_MAX-0x34) return false;
    uint8_t enabled=0;
    if(!read(sourceA8,&enabled,1)) return false;
    if(!(enabled&1)) {useOverride=false;return true;}
    TglCscCoefficients candidate;
    if(!read(sourceA8+4,candidate.matrix.data(),sizeof(candidate.matrix)) ||
       !read(sourceA8+0x28,candidate.inputOffsets.data(),sizeof(candidate.inputOffsets)))
        return false;
    // Native override explicitly writes all three output offsets as zero.
    output=candidate;useOverride=true;return true;
}
// Serialized owned counterpart of VP+a0's from/to cache and synchronous
// pointer publication. Native coefficient producer must be authenticated by
// the caller. No GPU completion/retirement certificate is implied here.
// Exact 1e0b70..1e0bac publishes this+38c/+3b0/+3bc to packet+28/+30/+38:
// contiguous 9 matrix, 3 input-offset and 3 output-offset floats. The borrowed
// pointers below are only valid inside use(), not for deferred GPU consumers.
class TglOwnedVpCscCache {
    TglCscCoefficients coefficients_{};
    uint32_t from_=0,to_=0;
    bool valid_=false,leased_=false,mutating_=false;
public:
    TglOwnedVpCscCache()=default;
    TglOwnedVpCscCache(const TglOwnedVpCscCache&)=delete;
    TglOwnedVpCscCache& operator=(const TglOwnedVpCscCache&)=delete;
    bool reset() noexcept {
        if(leased_ || mutating_) return false;
        coefficients_={};from_=to_=0;valid_=false;return true;
    }
    template<class Read,class Produce>
    bool prepare(uint32_t from,uint32_t to,uintptr_t sourceA8,Read read,Produce produce) noexcept {
        static_assert(noexcept(produce(std::declval<TglCscCoefficients&>(),from,to)),
                      "VP CSC coefficient producer ABI");
        if(leased_ || mutating_) return false;
        if(valid_ && from_==from && to_==to) return true;
        mutating_=true;TglCscCoefficients candidate;bool overridden=false;
        const bool success=tglReadOwnedVpCscOverride(from,to,sourceA8,read,overridden,candidate) &&
            (overridden || produce(candidate,from,to));
        if(success) {coefficients_=candidate;from_=from;to_=to;valid_=true;}
        mutating_=false;return success;
    }
    template<class Use> int withCoefficients(Use use) noexcept {
        static_assert(noexcept(use(static_cast<const TglCscCoefficients&>(coefficients_))),
            "VP CSC coefficient borrower ABI");
        if(!valid_ || leased_ || mutating_) return 5;
        leased_=true;
        const int result=use(static_cast<const TglCscCoefficients&>(coefficients_));
        leased_=false;return result;
    }
    template<size_t StateSize,class Use>
    int withParameters(std::array<uint8_t,64>& ief,std::array<uint8_t,StateSize>& state,
                       Use use) noexcept {
        static_assert(StateSize==0xb8 || StateSize==0xf8,"qualified VP/TGL state extent");
        static_assert(noexcept(use()),"VP CSC parameter borrower ABI");
        if(!valid_ || leased_ || mutating_) return 5;
        for(size_t offset:{size_t(0x28),size_t(0x30),size_t(0x38)}) {
            uintptr_t occupied=0;std::memcpy(&occupied,ief.data()+offset,8);
            if(occupied) return 5;
        }
        const auto saved=ief;const uint8_t savedState=state[0x88];
        const std::array<uintptr_t,3> pointers{{
            reinterpret_cast<uintptr_t>(coefficients_.matrix.data()),
            reinterpret_cast<uintptr_t>(coefficients_.inputOffsets.data()),
            reinterpret_cast<uintptr_t>(coefficients_.outputOffsets.data())}};
        leased_=true;ief[0x20]=1;state[0x88]=1;
        std::memcpy(ief.data()+0x28,pointers.data(),sizeof(pointers));
        const int result=use();
        ief=saved;state[0x88]=savedState;leased_=false;return result;
    }
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

enum class TglVeboxMode0Allocation { Required, Reuse };
// Native12bf00: owned-prefix data comparison only. Callers retain both surface
// leases; equality is NOT proof that their native backing or GPU work retired.
template<size_t N,size_t M>
bool tglVeboxMode0NativeSkipPredicate(uint8_t report0b,
    const std::array<uint8_t,N>& current,const std::array<uint8_t,M>& previous) noexcept {
    static_assert(N>=0x2a0 && M>=0x2a0,"complete native comparison fields");
    if(!(report0b&1)) return true;
    if((current[0x29a]&1)!=(previous[0x29a]&1) ||
       (current[0x299]&1)!=(previous[0x299]&1)) return false;
    for(size_t offset : {size_t(0x29c),size_t(0xd8),size_t(0xdc),size_t(0xe0)}) {
        uint32_t a=0,b=0;
        std::memcpy(&a,current.data()+offset,4);std::memcpy(&b,previous.data()+offset,4);
        if(a!=b) return false;
    }
    return true;
}
// Exact12c14b..12c1cd decision only. Native tests bit0 of report09/source86,
// full DWORD reporta0c==1, then calls12bf00. Preserve that short-circuit order:
// the skip predicate can inspect native DN/history state and is not an eager
// input. Reuse here does NOT establish backing validity or fence completion.
template<class SkipPredicate>
TglVeboxMode0Allocation tglVeboxMode0AllocationDecision(uint8_t report09,
    uint8_t source86,uint32_t reportA0c,SkipPredicate skipPredicate) noexcept {
    static_assert(noexcept(skipPredicate()) &&
        std::is_same_v<decltype(skipPredicate()),bool>,"native mode0 skip predicate ABI");
    if((report09&1) && !(source86&1) && reportA0c==1 && skipPredicate())
        return TglVeboxMode0Allocation::Reuse;
    return TglVeboxMode0Allocation::Required;
}

// Exact native mode dispatcher, not the outer renderer131fa0. The prologue
// stores RDI/RSI/RDX and immediately consumes argument2+10/+18. Keep those
// arguments opaque until their complete producer/backing contract is bound.
// This binding does NOT qualify all derived callbacks or GPU retirement.
struct TglVeboxRenderBinding {
    uintptr_t image=0,entry=0;
    inline static constexpr std::array<uint8_t,20> anchor{
        0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x70,
        0x48,0x89,0x7d,0xf8,0x48,0x89,0x75,0xf0,0x48,0x89,0x55,0xe8};
    template<class Read,class Qualify,class Executable>
    bool resolve(uintptr_t base,Read read,Qualify qualify,Executable executable) noexcept {
        *this={};std::array<uint8_t,anchor.size()> bytes{};
        if(!base || base>UINTPTR_MAX-0x12c9e4 || !qualify(base) ||
           !executable(base+0x12c9d0,bytes.size()) ||
           !read(base+0x12c9d0,bytes.data(),bytes.size()) || bytes!=anchor) return false;
        image=base;entry=base+0x12c9d0;return true;
    }
};
template<class Owner,class Invoke>
int tglInvokeLeasedVeboxRender(const TglVeboxRenderBinding& binding,Owner& owner,
    void* child,void* argument1,void* argument2,Invoke invoke) noexcept {
    static_assert(noexcept(invoke(uintptr_t{},nullptr,nullptr,nullptr)) &&
        std::is_same_v<decltype(invoke(uintptr_t{},nullptr,nullptr,nullptr)),int>,
        "native Render status ABI");
    if(!binding.image || binding.image>UINTPTR_MAX-0x12c9e4 ||
       binding.entry!=binding.image+0x12c9d0 || !child || !argument1 || !argument2 ||
       !owner.hasPrivateChildImageLease(reinterpret_cast<uintptr_t>(child),binding.image)) return 5;
    return invoke(binding.entry,child,argument1,argument2);
}

struct TglExecutionBinding {
    uintptr_t image = 0, create = 0, initialize = 0, destroy = 0;
    inline static constexpr std::array<uint8_t,14> createCounter{
        0x48,0x8d,0x05,0xe4,0x45,0x6c,0,0x8b,0x08,0x83,0xc1,0x01,0x89,0x08};
    inline static constexpr std::array<uint8_t,14> releaseCounter{
        0x48,0x8d,0x05,0x27,0x51,0x6c,0,0x8b,0x08,0x83,0xc1,0xff,0x89,0x08};
    inline static constexpr std::array<uint8_t,13> releaseDispatch{
        0x48,0x8b,0x45,0xf0,0x48,0x8b,0x08,0x48,0x89,0xc7,0xff,0x51,0x08};
    inline static constexpr std::array<uint8_t,11> releaseClear{
        0x48,0x8b,0x45,0xf8,0x48,0xc7,0,0,0,0,0};
    // Caller retains the authenticated image for the entire owner lifetime.
    template<class Read, class Qualify, class Executable>
    bool resolve(uintptr_t base, Read read, Qualify qualify, Executable executable) {
        *this = {};
        constexpr std::array<uintptr_t, 3> offsets{0x12ebc0, 0x12e420, 0x12e0d0};
        constexpr std::array<std::array<uint8_t, 16>, 3> anchors{{
            {0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x40,0x48,0x8b,0x35,0x59,0x54,0x62,0x00,0xbf},
            {0x55,0x48,0x89,0xe5,0x48,0x81,0xec,0xb0,0x09,0x00,0x00,0x48,0x8b,0x05,0x56,0x5c},
            {0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x10,0x48,0x89,0x7d,0xf8,0x48,0x8b,0x45,0xf8}
        }};
        if (!base || base > std::numeric_limits<uintptr_t>::max() - 0x7f321c ||
            !qualify(base)) return false;
        for (size_t i = 0; i < offsets.size(); ++i) {
            std::array<uint8_t, 16> bytes{};
            if (!executable(base + offsets[i], bytes.size()) ||
                !read(base + offsets[i], bytes.data(), bytes.size()) || bytes != anchors[i])
                return false;
        }
        std::array<uint8_t,14> created{},counter{};std::array<uint8_t,13> dispatch{};
        std::array<uint8_t,11> clear{};
        if(!executable(base+0x12ec2d,created.size()) || !read(base+0x12ec2d,created.data(),created.size()) || created!=createCounter ||
           !executable(base+0x12e0ea,counter.size()) || !read(base+0x12e0ea,counter.data(),counter.size()) || counter!=releaseCounter ||
           !executable(base+0x12e10d,dispatch.size()) || !read(base+0x12e10d,dispatch.data(),dispatch.size()) || dispatch!=releaseDispatch ||
           !executable(base+0x12e11a,clear.size()) || !read(base+0x12e11a,clear.data(),clear.size()) || clear!=releaseClear) return false;
        uint32_t allocationSize=0;
        if(!executable(base+0x12ebd0,4) ||
            !read(base+0x12ebd0,&allocationSize,4) || allocationSize!=0xd48) return false;
        // Donor admission resets execution through vtable+10. Exact Tahoe
        // TGL757e90+10 is the SAME12e420 initializer used by preparePass();
        // do not invent a second reset or bind destructor/reset by slot alone.
        uintptr_t resetEntry=0;
        if(!read(base+0x757ea0,&resetEntry,8) || resetEntry!=base+0x12e420) return false;
        image = base;
        create = base + offsets[0]; initialize = base + offsets[1]; destroy = base + offsets[2];
        return true;
    }
};

// Invoker is injected for offline tests. Native invoker must use the exact
// x86_64 signatures: uintptr_t factory(), int Init(uintptr_t), void Release(uintptr_t*).
// Release12e0d0 pairs factory accounting before virtual deleting destruction;
// direct12e3f0 would free storage but leak the native allocation counter.
// Binding/image lease must remain valid until every owned object is destroyed.
struct TglNativeExecutionInvoker {
    uintptr_t create(uintptr_t entry) const noexcept {
        return reinterpret_cast<uintptr_t(*)()>(entry)();
    }
    int initialize(uintptr_t entry,uintptr_t object) const noexcept {
        return reinterpret_cast<int(*)(uintptr_t)>(entry)(object);
    }
    void destroy(uintptr_t entry,uintptr_t object) const noexcept {
        uintptr_t slot=object;
        reinterpret_cast<void(*)(uintptr_t*)>(entry)(&slot);
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
        return binding.image && binding.image <= std::numeric_limits<uintptr_t>::max() - 0x7f321c &&
            binding.create == binding.image + 0x12ebc0 &&
            binding.initialize == binding.image + 0x12e420 &&
            binding.destroy == binding.image + 0x12e0d0 && qualify(binding.image);
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
template<class Backend> class TglExecutionOwner;
// Non-owning native execution bytes, constructible only by its leased owner.
// Never reinterpret a native allocation as a std::array object. The view and
// all pointers/references derived from it must remain inside the callback.
class TglLeasedAdmissionExecution {
    template<class Backend> friend class TglExecutionOwner;
    uint8_t* bytes_;
    explicit TglLeasedAdmissionExecution(uintptr_t object) noexcept
        :bytes_(reinterpret_cast<uint8_t*>(object)){}
public:
    TglLeasedAdmissionExecution(const TglLeasedAdmissionExecution&)=delete;
    TglLeasedAdmissionExecution& operator=(const TglLeasedAdmissionExecution&)=delete;
    uint8_t* data() noexcept {return bytes_;}
    uint8_t& operator[](size_t index) noexcept {return bytes_[index];}
};
template<class T> struct TglAdmissionExecutionExtent;
template<size_t N> struct TglAdmissionExecutionExtent<std::array<uint8_t,N>>
    :std::integral_constant<size_t,N>{};
template<> struct TglAdmissionExecutionExtent<TglLeasedAdmissionExecution>
    :std::integral_constant<size_t,0xd48>{};

template<class Backend> class TglExecutionOwner {
    Backend& backend;
    uintptr_t object = 0;
    bool leased_=false,admissionBytesBusy_=false,admissionResetAttempted_=false,borrowed_=false,featureBusy_=false;
    int admissionError_=0;
public:
    explicit TglExecutionOwner(Backend& b) noexcept : backend(b) {}
    TglExecutionOwner(const TglExecutionOwner&) = delete;
    TglExecutionOwner& operator=(const TglExecutionOwner&) = delete;
    ~TglExecutionOwner() { reset(); }
    void reset() noexcept {
        if(leased_ || borrowed_) return;
        const auto old = object;
        object = 0;
        admissionError_=0;
        if (old) backend.destroy(old);
    }
    int ensure() noexcept {
        if(leased_ || admissionError_) return 5;
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
        if(leased_ || admissionError_) return 5;
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
    // Borrow the native base's report only while that base is itself pinned.
    // No ownership transfer: even validation/reset failure must never call
    // backend.destroy for this pointer. No mixing with an existing owned
    // execution. Detach identity on exit, but retain admission failure until
    // the enclosing native owner rebuilds its report and explicitly resets
    // this wrapper; scope exit must not silently revive a poisoned report.
    template<class Use> int withBorrowedExecution(uintptr_t native,Use use) noexcept {
        static_assert(noexcept(use(uintptr_t{})),"borrowed execution status ABI");
        if(!native || object || leased_ || borrowed_ || admissionError_) return 5;
        object=native;borrowed_=true;
        struct Detach {
            TglExecutionOwner& owner;
            ~Detach(){owner.object=0;owner.borrowed_=false;
                owner.admissionResetAttempted_=false;}
        } detach{*this};
        return withExecution(use);
    }
    bool hasExecutionLease(uintptr_t expected) const noexcept {
        return leased_ && !admissionError_ && object && object==expected;
    }
    // Synchronous execution-data publication must retain the native object.
    // Reject reset/reinitialize/nested use during the lease. This pins CPU
    // lifetime only: callers still own serialization and GPU fence lifetimes.
    template<class Use> int withExecution(Use use) noexcept {
        static_assert(noexcept(use(uintptr_t{})),"execution lease status ABI");
        if(leased_ || admissionError_ || !object) return 5;
        leased_=true;
        admissionResetAttempted_=false;
        if(!backend.validate(object)) {
            if(borrowed_) admissionError_=5;
            leased_=false;reset();return 5;
        }
        const int status=use(object);
        leased_=false;
        return admissionError_ ? admissionError_ : status;
    }
    template<class Use> int withLeasedAdmissionBytes(uintptr_t expected,Use use) noexcept {
        static_assert(noexcept(use(std::declval<TglLeasedAdmissionExecution&>())),
                      "admission execution view callback must not throw");
        if(admissionBytesBusy_ || !hasExecutionLease(expected)) return 5;
        admissionBytesBusy_=true;
        TglLeasedAdmissionExecution view(object);
        const int result=use(view);
        admissionBytesBusy_=false;
        return admissionError_ ? admissionError_ : result;
    }
    // Share the existing execution lease without recursively acquiring it.
    // Native170's report60 target borrow must not outlive the source/target
    // frame. This lock is separate from admission's live byte-view lock.
    template<class Use> int withLeasedFeatureReport(uintptr_t expected,const void* target,Use use) noexcept {
        static_assert(noexcept(use()) && std::is_same_v<decltype(use()),int>,"feature report status ABI");
        if(featureBusy_ || !target || !hasExecutionLease(expected)) return 5;
        auto* field=reinterpret_cast<uint8_t*>(object)+0x60;
        uintptr_t previous=0;std::memcpy(&previous,field,8);
        if(previous) return 5;
        featureBusy_=true;
        struct Detach {
            uint8_t* field;bool& busy;
            ~Detach(){const uintptr_t zero=0;std::memcpy(field,&zero,8);busy=false;}
        } detach{field,featureBusy_};
        const int status=use();
        uintptr_t observed=0;std::memcpy(&observed,field,8);
        if(observed && observed!=reinterpret_cast<uintptr_t>(target)) return 5;
        return admissionError_?admissionError_:status;
    }
    // Admission reset12e420 must run inside the execution/frame lease, once.
    // On failure keep storage alive for outstanding base/report borrows, but
    // poison further use. Caller detaches those borrows before explicit reset.
    int initializeLeasedAdmission(uintptr_t expected) noexcept {
        if(!admissionBytesBusy_ || admissionResetAttempted_ || !hasExecutionLease(expected)) return 5;
        admissionResetAttempted_=true;
        const int result=backend.initialize(object);
        if(result || !backend.validate(object)) admissionError_=result ? result : 5;
        return admissionError_;
    }
    int commitDndi(uintptr_t address,const TglOwnedDndiState& state) noexcept {
        if(!leased_ || admissionError_ || !object || object>UINTPTR_MAX-0xd48 || address!=object+0x850) return 5;
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
// Mode0 report prelude12c0c2..12c120. Inputs are snapshots of owned child
// fields1b58/1b60 under its lease. This does not advance the child history,
// allocate resources, reset admission or certify a reusable GPU submission.
template<class ExecutionOwner>
int tglPublishLeasedVeboxMode0Prelude(ExecutionOwner& owner,uintptr_t expected,
    uint32_t child1b58,uint32_t child1b60) noexcept {
    return owner.withLeasedAdmissionBytes(expected,[&](auto& view) noexcept {
        static_assert(TglAdmissionExecutionExtent<std::decay_t<decltype(view)>>::value>=0x34,
            "mode0 report prelude extent");
        constexpr std::array<size_t,5> offsets{0x28,0x1c,0x20,0x2c,0x30};
        const std::array<uint32_t,5> values{child1b58,0,1,child1b60&1,(child1b60+1)&1};
        for(size_t i=0;i<offsets.size();++i) std::memcpy(view.data()+offsets[i],&values[i],4);
        return 0;
    });
}

// Exact1f7e0c..1f7e3d whole-byte report13/14 publication after a changed
// FFDi allocation. Reuse the owner's existing bounded byte-view lock: reject
// unleased/foreign/poisoned report or recursive admission before any write.
template<class ExecutionOwner>
int tglPublishLeasedFfdiCompression(ExecutionOwner& owner,uintptr_t expected,
    uint8_t flag,uint8_t mode) noexcept {
    return owner.withLeasedAdmissionBytes(expected,[&](auto& view) noexcept {
        static_assert(TglAdmissionExecutionExtent<std::decay_t<decltype(view)>>::value>=0x15,
            "FFDi report publication extent");
        const std::array<uint8_t,2> values{flag,mode};
        std::memcpy(view.data()+0x13,values.data(),values.size());
        return 0;
    });
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
    // Exact 25G229 IOAccelerator CPU reference (UUID198a776a...,4634cf):
    // FinishFenceEvent consumes only context18 and zero-extended ESI index,
    // sends selector5 with one scalar and no outputs, then returns its status.
    // Incoming EDX/ECX are not consumed: the apparent timeout above is not
    // evidence of a timeout enforced by this userspace wrapper. Kernel fence
    // semantics and actual completion still require independent proof.
    // TGL IGAccelVideoContext::getTargetAndMethodForIndex at77452 handles
    // only100..102 locally; selector5 tail-dispatches the parent vtable950.
    // Consequently the fence contract belongs to exact IOAcceleratorFamily
    // IOAccelVideoContext2, not to this TGL-specific selector wrapper.
    // 25G229 KDK family7450 validates the64-byte event index, copies via1b0,
    // drops accelerator locks, then calls178=finishEventUnlocked(30280).
    // That polls signed32-bit stamp deltas for eight channels and waits via238;
    // timeout e00002d6 signalsHardwareError(3) then retries, not bounded return.
    // It also skips a pending stamp when accelerator+dc8!=0. Thus result0 is
    // not an independent no-reset hardware-completion certificate. Retain the
    // associated resource generation until completion AND reset exclusion are
    // established; do not invent a one-second deadline from the ignored ECX.
    // Separate successful work from safe failure disposal: local i915-sriov
    // 2026.03.05.7 i915_request_mark_eio sets fence.error=-EIO then marks the
    // request complete; i915_request_retire may retire that failed request.
    // This is not a Tahoe ABI or a reset-quiescence certificate. A future
    // owned retire path must distinguish verified success from independently
    // proved drained discard, preserving the failure rather than returning0.
    // Fast table238=IOAccelEventMachine2::waitForStamp(12954): dc8 is
    // rechecked before/inside its wait loop and the bypass also returns zero.
    // IOAccelContext2::clientClose(7954) likewise returns zero after invoking
    // close/finalize dispatch; that return alone is not a drain certificate.
    // KDK context_finish(ad8a) waits on context5d0 through the SAME178 entry,
    // maps only -1 to timeout and otherwise returns0: no stronger reset proof.
    // contextStop(7eea..8064) cancels a fence callback, releases context-side
    // references/tables, then calls freeAllDataBuffers(8162). No finish wait
    // occurs in that interval; neither callback cancellation nor dropping a
    // context-side reference proves the caller's backing generation is idle.
    // Follow the enclosing finalize/drain order before connecting retirement.
    // finalize(7988) gathers orphan mappings through context slot990 and calls
    // Shared2::add_orphaned_mappings(2a9ea). With a live shared user client110,
    // that merges into or copies a retained set at client150. Context close is
    // therefore not identical to freeing every mapping. The set's eventual
    // release is in SharedUserClient2::free(2b25d..2b26f). Table77210 resolves
    // stopLocked's998 to sharedStop(2d7bc): clear Shared2::client110, release
    // Shared2, remove from accelerator's shared-client list. Slot678 is native
    // IOService::detach, not a fence. stop(2b2ec)'s850/858 are DidLock/WillUnlock
    // trace hooks (42100/4212c), also inherited by TGL tabled1060 slotsd18c0/8.
    // None supplies a drain certificate. Establish resource-side GPU reference
    // ownership before retiring an independently owned backing generation.
    // Resource2::retain/release(27d04/27d1c) changes refcount2c; last release
    // dispatches free90. Separately addToChannel(24100) increments activea0
    // and records channel9c/access98. removeFromChannel(23f46) stamps relevant
    // backing events through Channel2::setEventStamp(41d8), then decrementsa0;
    // zero clears98/9c. That is event bookkeeping, NOT a completion wait or
    // evidence that an independent userspace allocation can now be freed.
    // Resource2::complete(27412) is another separate counter28: last complete
    // tears down type1 memory-map references or invokes type3 parent1e8. It
    // does not wait on channel stamps. Do not interpret its name as GPU done.
    // TGL IGAccelResource::free(6f59e..6f808) unmaps aux ranges, frees private
    // metadata and forwards to Resource2::free. No direct fence wait appears;
    // Child220 comes from addToAperture(73576): Memory::createMappingInTask138
    // (family22ee), then Map::prepare138; published only after success736a6.
    // Map140=complete(5288c) only decrements prepare countc, not a fence wait.
    // Map::release(52364) checks last reference and mapped flag, tests event38
    // via EventMachine190, and on its deferred branch5240c moves task list1e8
    // to orphan list200 WITHOUT decrementing the final object reference.
    // Task::prune_orphaned_mappings(393fa) reads that same200 list and releases
    // only while Map::testEvent(52462) succeeds; first pending event stops it.
    // This is native deferred mapping ownership, not an owned-layer fence.
    // Family testEventUnlocked30414..3047d tests eight packed events: low
    // DWORD channel (-1 skips), high DWORD target stamp. It compares signed
    // target-observed deltas, refreshing EventMachine+f8+24*channel from the
    // hardware stamp pointer array+28 when needed. Critically30461..3046c
    // ALSO accepts a still-pending stamp when device+dc8 is nonzero. Thus
    // native testEvent=true is not alone a reset-free completion certificate;
    // the owned retirement admission must exclude that device-state branch
    // and retain exact submission/channel/generation provenance. Do not
    // reproduce this kernel-private predicate against guessed user pointers.
    // deviceTerminatedUnlocked319f4..31a3c independently copies each channel's
    // submitted stamp at+104+24*i into its hardware stamp pointer, then sfence.
    // Therefore even stamp equality after termination cannot distinguish real
    // execution from teardown; a live device/generation gate is indispensable.
    // Prove the owned allocation reaches this mapping path before relying on
    // it; external CPU buffers and failed STMM unlock remain separate cases.
    // Release-only CPU capture198a776a... at46268e is a null-guarded import
    // tail call, not a fence. Its adjacent cleanup sends scalar selector1 with
    // the resource ID, but adjacency alone does NOT prove CF finalizer linkage.
    // Family delete_resource(2c470) looks up that ID and, except typea, invokes
    // resource160 with Shared2. Family Resource2 vptr766d0+160 resolves218cc
    // sharedRelease, NOT databuffer_release(21a10). Both invalidate the ID,
    // updates allocation accounting, removes the shared list entry and drops
    // a reference via28; sharedRelease also drops168 and conditional60 without
    // waiting. TGL IGAccelResource vptrd93e0+160 atd9540 has an external
    // relocation to this SAME sharedRelease, not a TGL override. Joining this
    // token-delete path to deferred Map ownership for each owned allocation
    // remains a separate proof obligation from CF finalizer linkage below.
    // CPU class capture59710ed (25G229, UUID198a776a...) now identifies
    // className IOAccelResource and class+20=7ff80f4626a5: its first256
    // bytes match the retained Release+17 cleanup exactly. Apple's
    // CFRuntimeClass layout names +20 finalize (version/className/init/copy
    // precede it): https://github.com/apple-oss-distributions/CF/blob/main/CFRuntime.h
    // Thus the CF finalizer-to-selector1 cleanup linkage has static evidence;
    // this does NOT establish native mapping ownership for each owned backing,
    // nor make the finalizer a wait or a reset-free completion certificate.
    // Family Resource2::free21478 common path2157c..215a5 loads map40,
    // calls Map::remove_resource529c2, releases Map via28, then clears40/70/20.
    // TGL free6f7bf..6f7e1 separately completes140/releases28 aperture map220
    // before its base free. Both releases reach native Map lifetime policy;
    // neither call-site itself waits. Family type82 cleanup216b8..2173e
    // unlinks the child's e8/f0 parent list, releases parent's memorye0 then
    // parent Resource2. Do not replace these with a shallow descriptor copy.
    // Remaining obligation: prove each published owned resource acquires the
    // matching map/retained memory and event stamps before GPU submission,
    // especially type80 SysMemory and type82 IOSurface parent generations.
    // Map::init521e6 stores the supplied IOAccelMemory at18 (52252), retains
    // it via20 (5225c), then registers this mapping with Memory::add_mapping.
    // Map::free52508 removes that association and releases memory18 at5252a.
    // Thus a deferred Map holds its memory independently of Resource2's life.
    // Resource2::removeFromChannel23f59..23f78 stamps map40+38 when resource
    // flag e.bit4 is clear and Map10.bit2 is set; the alternate flag uses20.
    // Channel140=setEventStamp41d8 forwards channel index to EventMachine1d0.
    // Type82 selects parent e0 at23ff1 and invokes parent198 at2406d;
    // family vptr766d0+198=23f46, so parent stamps follow the same dispatch.
    // This closes native retention/stamp relationships, not the separate
    // proof that our owned registration/prepare reaches them before submit.
    // Native prepare26efa is distinct from channel admission: it creates map40
    // through map26c70 only when absent, invokes Map138 at26ffd, and on success
    // increments resource28, completes Map140, then sets resource e.bit4.
    // prepareInTask27486 instead returns a task mapping; do not substitute it
    // for the resource's prepared/channel state merely because both use138.
    // addToChannel24100 increments a0 and merges usage98 (first admission also
    // records channel index9c). Type82 forwards parent e0 via190 at24183.
    // Neither this observed admission prefix nor allocation establishes that
    // the owned submission actually executes prepare/add/remove in order.
    // Context::bindResource ac0a now supplies the native ordering proof:
    // ac85 Resource170=prepare26efa; only AL!=0 reaches ac9f Resource190=
    // addToChannel24100. unbindResource accc calls Resource198=removeFromChannel
    // at acde BEFORE tail-calling Resource178=complete27412 at acf0.
    // These four slots are fixed in family vptr766d0+170..198, not guessed
    // from method names. prepareResources acf6 is a separate prefix-count
    // helper: it stops on the first failed prepare and returns successes,
    // not an IOReturn and not channel admission. For an owned submit using
    // this bind/unbind path, prove every backing token is registered there;
    // CPU leases alone do not reproduce stamps or partial-prepare bookkeeping.
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
        if (!mhw || mhw > max - 0x28 || !expectedOs || expectedOs > max - 0x8f || !image ||
            image > max - 0x7595b0 || !qualify(image)) return false;
        uintptr_t vt = 0, actualOs = 0, actualHeap = 0, relocation = 0;
        uint32_t osMode = 0;
        // Constructor16f140 ->16f3d0 ->ff690 selects this callback from
        // OS8c: nonzero=3e6a0, zero=3eae0 (ff709..ff735). Consumer172080
        // calls MHW8 with OS18/command/0x48-byte relocation packet at1721f1.
        // 3e6a0 gets the address through OS260 at3e807; 3eae0 passes a
        // 0x40-byte input to OS268 at3ed2a. Initializer65ea0 sets OS8c=0
        // (65f5e), and OS268=641e0 (662a5): default uses real relocation,
        // NOT zero stub OS260=65060. Follow that selected branch first.
        // 641e0 emits a 0x14-byte record at6442b..645bf; its token DWORD
        // comes from640e0: type0 resource38, surface variants80 with b8/f0.
        // Kernel patch_codechal_command_buffer77a6e reads the 0x14-byte
        // records at77b97: token low16 from record+4 reaches context b78
        // at77bd2. TGL vptrda440+b78=77eb8 bind_resource; its a40 call77f1c
        // uses relocation atdae80 to family bindResource(ac0a), hence native
        // prepare->channel admission. This closes the static native route,
        // not completeness of owned token publication or dynamic execution.
        // This validates the producer identity, not kernel admission/completion.
        if (!read(mhw, &vt, 8) || vt != image + 0x759520 ||
            !read(mhw + 0x18, &actualOs, 8) || actualOs != expectedOs ||
            !read(mhw + 0x20, &actualHeap, 8) || !actualHeap ||
            !read(expectedOs + 0x8c, &osMode, 4) ||
            !read(mhw + 8, &relocation, 8) ||
            relocation != image + (osMode ? 0x3e6a0 : 0x3eae0)) return false;
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
// TGL primary757c98+170 ->12aba0. Native takes this/source/target and
// returns void; it transitively calls160 and may consume tracked source90.
// Binding code is NOT complete object/platform qualification or permission
// to invoke without the denoise-feature frame transaction and all callbacks.
struct TglVeboxFeatureBinding {
    uintptr_t image=0,entry=0;
    template<class Read,class Qualify,class Executable>
    bool resolve(uintptr_t base,Read read,Qualify qualify,Executable executable) {
        image=entry=0;
        constexpr std::array<uint8_t,16> anchor{
            0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x50,
            0x48,0x89,0x7d,0xf8,0x48,0x89,0x75,0xf0};
        std::array<uint8_t,16> actual{};uintptr_t slot=0;
        if(!base || base>UINTPTR_MAX-0x757e10 || !qualify(base) ||
           !executable(base+0x12aba0,anchor.size()) ||
           !read(base+0x12aba0,actual.data(),actual.size()) || actual!=anchor ||
           !read(base+0x757c98+0x170,&slot,8) || slot!=base+0x12aba0) return false;
        image=base;entry=slot;return true;
    }
};
struct TglOwnedSurfaceStorage;
struct TglNativeVeboxFeaturePreparationInvoker {
    void operator()(uintptr_t entry,void* child,const TglOwnedSurfaceStorage& source,
        const TglOwnedSurfaceStorage& target) const noexcept {
        using Prepare=void(*)(void*,const void*,const void*);
        reinterpret_cast<Prepare>(entry)(child,&source,&target);
    }
};

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
       // Native superclass 126480 unconditionally reads renderHal+78/+80
       // at 1264f3/126502, before reporting constructor status.
       !renderHal || !privateStorage || (reinterpret_cast<uintptr_t>(privateStorage)&7) ||
       capacity<0x1bc0 || !sfc.get() || sfc.image()!=binding.image ||
       sfc.os()!=reinterpret_cast<uintptr_t>(os)) return TglOwnedBaseConstruction::NotCalled;
    // Keep the borrowed child+68 dependency alive across the native call.
    // A raw get() alone permits reentrant reset/reinitialization of its owner.
    bool called=false;
    sfc.withObject([&](void* nativeSfc) {
        called=true;
        construct(binding.construct,privateStorage,os,mhw,nativeSfc,renderHal,history,
                  performanceData,cacheControl,&nativeStatus);
        return 0;
    });
    if(!called) return TglOwnedBaseConstruction::NotCalled;
    return nativeStatus==0?TglOwnedBaseConstruction::ConstructedSuccess:
                           TglOwnedBaseConstruction::ConstructedFailure;
}

// Arm only after construction of private owned storage has completed. This
// scope does not infer object size, publish a child, or adopt native storage.
// Cleanup owns GPU descriptors; native base teardown owns transferred CPU
// shells. Storage release is last and must not call native operator delete.
template<class Cleanup, class Destroy, class Release> class TglVeboxTeardownScope {
    void* object_;
    bool constructing_=false,tearingDown_=false;
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
        if(!storage || object_ || constructing_ || tearingDown_) return TglOwnedBaseConstruction::NotCalled;
        constructing_=true;
        const auto result=construct();
        constructing_=false;
        if(result==TglOwnedBaseConstruction::NotCalled) return result;
        object_=storage;
        if(result==TglOwnedBaseConstruction::ConstructedFailure) reset();
        return result;
    }
    void reset() noexcept {
        if(constructing_ || tearingDown_) return;
        void* object = object_;
        object_ = nullptr; // revoke before callbacks, including reentrant reset
        if (!object) return;
        // Cleanup callbacks may reenter. Do not admit a new construction while
        // the old native base or its CPU shell storage is being destroyed.
        tearingDown_=true;
        cleanup_(object);
        destroy_(object);
        release_(object);
        tearingDown_=false;
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

// This is the OUTER renderer child interface, not the inner VP interface.
// Exact TGL base table757c98: slot10=126740 (Allocate), slot18=126cf0
// (cleanup, gated through virtual60). Renderer132818 calls child.virtual10;
// renderer132a46 calls child.virtual18 before132bb0 performs native-counted
// deleting destruction. The owned VP27 table belongs under this child's80,
// never directly at renderer910/918. Owned allocation must remain outside
// native132bb0 accounting; a borrowed publication must be detached first.
template<class CpuBackend,class SfcInvoke,class Cleanup,class Destroy>
class TglOwnedVeboxBaseOwner {
    TglTrackedCpuOwner<CpuBackend> storage_;
    TglOwnedSfcMhw<SfcInvoke> sfc_;
    TglOwnedVeboxVtable table_;
    Cleanup& cleanup_;
    Destroy& destroy_;
    uintptr_t destructor_=0,image_=0;
    uint64_t allocationGeneration_=0;
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
        if(busy_ || allocationGeneration_==UINT64_MAX) return TglOwnedBaseConstruction::NotCalled;
        reset();busy_=true;
        if(binding.image!=sfcLifetime.image ||
           !sfc_.initialize(sfcLifetime,sfcBinding,reinterpret_cast<uintptr_t>(os),read)) {
            busy_=false;reset();return TglOwnedBaseConstruction::NotCalled;
        }
        if(!storage_.allocate(0x1bc0)){busy_=false;reset();return TglOwnedBaseConstruction::NotCalled;}
        ++allocationGeneration_;
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
    uint64_t allocationGeneration() const noexcept {return allocationGeneration_;}
    bool ownsLifecycleAllocation(void* child,uint64_t generation) const noexcept {
        return child && generation && generation==allocationGeneration_ &&
            reinterpret_cast<uintptr_t>(child)==storage_.get();
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
        // Both link writes below require the authenticated 0xa0 allocation,
        // not merely a matching function prologue.
        uint32_t allocationSize=0;
        if(!executable(image_+0x126bf0,4) ||
           !read(image_+0x126bf0,&allocationSize,4) || allocationSize!=0xa0) return false;
        const uintptr_t helper=create(entry);
        if(!helper) return false;
        // Allocate12696a: helper+8=this, helper+10=virtual48(this).
        // Native+48=12ea30 lazily calls +40=12e9c0 when child88 is null;
        // that creates/initializes the report through12ebc0 and report vtable10.
        // Child owns report88 (base destructor12e00f), helper merely borrows it.
        // withNativeOwnedExecution borrows this report without transferring
        // ownership. Never overwrite native-owned88 with an independently
        // allocated TglExecutionOwner report; helper10 must share its lifetime.
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
    // Pin the sole native report owner before borrowing execution logic.
    template<class ExecutionOwner,class Read,class Use>
    int withNativeOwnedExecution(ExecutionOwner& execution,Read read,Use use) noexcept {
        static_assert(noexcept(read(uintptr_t{},static_cast<void*>(nullptr),size_t{})) &&
            std::is_same_v<decltype(read(uintptr_t{},static_cast<void*>(nullptr),size_t{})),bool> &&
            noexcept(use(nullptr,uintptr_t{})),"native report borrow ABI");
        return withPrivateChild([&](void* child) noexcept {
            uintptr_t report=0,helper=0,helperReport=0;
            const auto* bytes=static_cast<const uint8_t*>(child);
            std::memcpy(&report,bytes+0x88,8);std::memcpy(&helper,bytes+0x1bb8,8);
            if(!report) return 5;
            if(helper && (helper>UINTPTR_MAX-0x17 ||
                !read(helper+0x10,&helperReport,8) || helperReport!=report)) return 5;
            // Native base remains the sole report owner. Neither pointer is
            // replaced; its helper can continue borrowing the same report.
            const int status=execution.withBorrowedExecution(report,[&](uintptr_t actual) noexcept {
                return use(child,actual);
            });
            uintptr_t observedReport=0,observedHelper=0;
            std::memcpy(&observedReport,bytes+0x88,8);std::memcpy(&observedHelper,bytes+0x1bb8,8);
            if(observedReport!=report || observedHelper!=helper) return 5;
            if(helper && (!read(helper+0x10,&helperReport,8) || helperReport!=report)) return 5;
            return status;
        });
    }
    // CPU-only composition: keep both owners leased and restore child80
    // before native base teardown can reach accounting helper12e190.
    // The VP view is data-only until its complete vtable is independently
    // qualified. No ownership transfer or asynchronous GPU retention.
    // Scope supplies an already-leased child and its synchronous consumer.
    // This permits prepared setup->send to span the parent80 borrow without
    // recursively acquiring the VP graph. Scope must pin the entire9d0 view;
    // no callable-vtable or asynchronous lifetime claim is made here.
    template<class Use>
    int withLeasedBorrowedVpCpuView(void* parent,void* child,Use use) noexcept {
        if(!hasPrivateChildLease(reinterpret_cast<uintptr_t>(parent))) return 5;
        static_assert(noexcept(use(nullptr,nullptr)),"borrowed VP CPU view ABI");
        const uintptr_t p=reinterpret_cast<uintptr_t>(parent);
        const uintptr_t c=reinterpret_cast<uintptr_t>(child);
        // Exact base extents: parent1bc0 and VP9d0. Distinct starting
        // addresses alone do not establish independent allocations.
        if(!child || p>UINTPTR_MAX-0x1bc0 || c>UINTPTR_MAX-0x9d0 ||
           (p<c+0x9d0 && c<p+0x1bc0)) return 5;
        auto* field=static_cast<uint8_t*>(parent)+0x80;
        uintptr_t previous=0;
        std::memcpy(&previous,field,8);
        if(previous) return 5; // never hide an existing native owner
        const uintptr_t borrowed=reinterpret_cast<uintptr_t>(child);
        std::memcpy(field,&borrowed,8);
        const int result=use(parent,child);
        uintptr_t observed=0;std::memcpy(&observed,field,8);
        std::memcpy(field,&previous,8);
        return observed==borrowed ? result : 5;
    }
    template<class Scope>
    int withBorrowedVpCpuScope(Scope scope) noexcept {
        return withPrivateChild([&](void* parent) noexcept {
            auto borrow=[&](void* child,auto use) noexcept -> int {
                return withLeasedBorrowedVpCpuView(parent,child,use);
            };
            static_assert(noexcept(scope(borrow)),"borrowed VP scope must not throw");
            return scope(borrow);
        });
    }
    template<class VpOwner,class Use>
    int withBorrowedVpCpuView(VpOwner& vp,bool avsEnabled,Use use) noexcept {
        return withBorrowedVpCpuScope([&](auto borrow) noexcept {
            return vp.withView(avsEnabled,[&](void* child) noexcept {
                return borrow(child,use);
            });
        });
    }
    // Synchronous private dispatch: VP graph/resolver leases enclose this
    // call. Restore parent80 before removing VP's temporary dispatch table.
    // No factory ownership transfer, RTTI admission or GPU retention implied.
    template<class VpOwner,class Table,class Use>
    int withLeasedBorrowedVpFlatDispatch(void* parent,VpOwner& vp,void* child,
        const Table& table,Use use) noexcept {
        if(!hasPrivateChildLease(reinterpret_cast<uintptr_t>(parent))) return 5;
        return vp.withLeasedFlatDispatch(child,table,[&](void* admitted) noexcept {
            return withLeasedBorrowedVpCpuView(parent,admitted,use);
        });
    }
    template<class VpOwner,class Table,class Use>
    int withBorrowedVpFlatDispatch(VpOwner& vp,void* child,const Table& table,Use use) noexcept {
        return withPrivateChild([&](void* parent) noexcept {
            return withLeasedBorrowedVpFlatDispatch(parent,vp,child,table,use);
        });
    }
    // Parent80 is borrowed only while the graph's admission/resource entries
    // are live. Restore it before either owner can enter native teardown.
    template<class VpOwner,class Read,class Lines,class Retire,class Use>
    int withBorrowedVpAssemblyView(VpOwner& vp,bool avsEnabled,Read& read,
        Lines& lines,Retire& retire,Use use) noexcept {
        return withBorrowedVpCpuScope([&](auto borrow) noexcept {
            return vp.withAssemblyView(avsEnabled,read,lines,retire,
                [&](void* child,const auto& entries) noexcept {
                    return borrow(child,[&](void* parent,void* borrowed) noexcept {
                        static_assert(noexcept(use(parent,borrowed,entries)),
                            "borrowed VP assembly consumer ABI");
                        return use(parent,borrowed,entries);
                    });
                });
        });
    }
    template<class VpOwner,class Read,class Lines,class Retire,class Use>
    int withBorrowedVpModeAssemblyView(VpOwner& vp,bool avsEnabled,Read& read,
        Lines& lines,Retire& retire,uintptr_t source,uintptr_t target,uintptr_t settings,Use use) noexcept {
        return withBorrowedVpCpuScope([&](auto borrow) noexcept {
            return vp.withModeAssemblyView(avsEnabled,read,lines,retire,source,target,settings,
                [&](void* child,const auto& entries) noexcept {
                    return borrow(child,[&](void* parent,void* borrowed) noexcept {
                        static_assert(noexcept(use(parent,borrowed,entries)),"parent VP mode assembly ABI");
                        return use(parent,borrowed,entries);
                    });
                });
        });
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
    bool hasPrivateChildImageLease(uintptr_t expected,uintptr_t image) const noexcept {
        return image && image_==image && hasPrivateChildLease(expected);
    }
    // Parent CPU lease and native SFC reservation cover the same command
    // sequence. Never expose a persistent backend or transfer SFC ownership.
    template<class CommandInvoke,class Use>
    int withLeasedSfcCommandSequence(void* parent,const TglSfcMhwBinding& binding,
        void* command,CommandInvoke& invoke,Use use) noexcept {
        if(!command || !hasPrivateChildImageLease(reinterpret_cast<uintptr_t>(parent),binding.image)) return 5;
        TglOwnedSfcCommandBackend<SfcInvoke,CommandInvoke> commands(binding,sfc_,command,invoke);
        return commands.withCommandSequence([&]() noexcept -> int {
            static_assert(noexcept(use(commands)),"parent SFC command consumer ABI");
            return use(commands);
        });
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
    // Owned non-deleting lifecycle: invalidate callbacks and destroy members,
    // but retain the enclosing allocation for a later deleting lifecycle.
    bool destroyInPlace() noexcept {return teardown(false);}
    bool deleteAllocation() noexcept {return teardown(true);}
    void reset() noexcept {(void)teardown(true);}
private:
    bool teardown(bool releaseAllocation) noexcept {
        if(busy_) return false;
        busy_=true;
        ready_=false;
        if(constructed_) {
            constructed_=false;
            void* object=reinterpret_cast<void*>(storage_.get());
            // Native12de30 overwrites vptr with base757c98 at12de4d, then
            // frees CPU shells (12de6f etc). Retire bound resource tokens here,
            // while shell storage is still alive; the native base destructor
            // cannot dispatch the owned +60 hook to do that cleanup for us.
            // VP child80 has an additional ownership boundary: when SKU64
            // bit17 is set,12e097 calls12e190(&child80), which decrements
            // native allocation counter7f3218 BEFORE invoking vtable+8.
            // An owned/borrowed VP must therefore be detached by cleanup,
            // or have a separately proved native allocation-accounting pair.
            // Merely providing an owned deleting-destructor slot is NOT enough.
            cleanup_(object);
            destroy_(destructor_,object);
        }
        destructor_=image_=0;
        if(releaseAllocation) storage_.reset();
        sfc_.reset();
        tableInstalled_=false;table_.entries={};table_.typeInfo=0;
        busy_=false;
        return true;
    }
};

// These callbacks are owned lifecycle only, not permission for native
// allocation accounting or GPU retirement. The context outlives allocation.
template<class Owner> class TglOwnedVeboxLifetimeContext {
    Owner& owner_;void* child_;uint64_t generation_;
    bool destroyed_=false,deleted_=false;
    int status_=5;
public:
    explicit TglOwnedVeboxLifetimeContext(Owner& owner) noexcept
        :owner_(owner),child_(owner.constructedBase()),generation_(owner.allocationGeneration()) {}
    bool owns(void* child) const noexcept {
        return !deleted_ && child==child_ && owner_.ownsLifecycleAllocation(child,generation_);
    }
    void destroy(bool deleting) noexcept {
        if(!owns(child_) || (destroyed_ && !deleting)) {status_=5;return;}
        const bool done=deleting?owner_.deleteAllocation():owner_.destroyInPlace();
        status_=done?0:5;
        if(done) {destroyed_=true;deleted_=deleting;}
    }
    int status() const noexcept {return status_;}
};
template<class Resolve> struct TglOwnedVeboxLifetimeCallbacks {
    static void destroy(void* child) noexcept {invoke(child,false);}
    static void deletingDestroy(void* child) noexcept {invoke(child,true);}
private:
    static void invoke(void* child,bool deleting) noexcept {
        static_assert(noexcept(Resolve{}(child)),"VEBOX lifetime resolver ABI");
        auto* context=Resolve{}(child);
        static_assert(noexcept(context->owns(child)) && noexcept(context->destroy(deleting)),"VEBOX lifecycle ABI");
        if(context && context->owns(child)) context->destroy(deleting);
    }
};
template<class Resolve>
bool tglBindOwnedVeboxLifetimeHooks(void* child,std::array<uintptr_t,20>& hooks) noexcept {
    static_assert(noexcept(Resolve{}(child)),"VEBOX lifecycle hook resolver ABI");
    auto* context=Resolve{}(child);
    if(!context || !context->owns(child) || hooks[0] || hooks[1]) return false;
    using Callback=TglOwnedVeboxLifetimeCallbacks<Resolve>;
    auto candidate=hooks;
    candidate[0]=reinterpret_cast<uintptr_t>(&Callback::destroy);
    candidate[1]=reinterpret_cast<uintptr_t>(&Callback::deletingDestroy);
    hooks=candidate;return true;
}
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
struct TglNativeVeboxChannelInvoker {
    int operator()(uintptr_t entry,uintptr_t os,uint32_t channel) const noexcept {
        using Restore=int(*)(void*,uint32_t);
        return reinterpret_cast<Restore>(entry)(reinterpret_cast<void*>(os),channel);
    }
};
class TglNativeVeboxChannelBinding {
    uintptr_t os_=0,entry_=0;
public:
    inline static constexpr std::array<uint8_t,56> body{
        0x55,0x48,0x89,0xe5,0x48,0x89,0x7d,0xf0,0x89,0x75,0xec,
        0x48,0x83,0x7d,0xf0,0,0x0f,0x85,0x0c,0,0,0,0xc7,0x45,0xfc,5,0,0,0,
        0xe9,0x11,0,0,0,0x8b,0x45,0xec,0x48,0x8b,0x4d,0xf0,0x89,0x41,8,
        0xc7,0x45,0xfc,0,0,0,0,0x8b,0x45,0xfc,0x5d,0xc3};
    inline static constexpr std::array<uint8_t,14> publication{
        0x4c,0x8d,0x05,0x0d,0xcd,0xff,0xff,0x4c,0x89,0x87,0xf0,0,0,0};
    template<class Read,class Qualify,class Executable>
    bool resolve(uintptr_t image,uintptr_t os,Read read,Qualify qualify,Executable executable) noexcept {
        os_=entry_=0;uintptr_t entry=0;
        std::array<uint8_t,body.size()> actual{};
        std::array<uint8_t,publication.size()> published{};
        if(!image || image>UINTPTR_MAX-0x65fca || !os || os>UINTPTR_MAX-0xf8 ||
           !qualify(image) || !read(os+0xf0,&entry,8) || entry!=image+0x62cd0 ||
           !executable(entry,body.size()) || !read(entry,actual.data(),actual.size()) || actual!=body ||
           !read(image+0x65fbc,published.data(),published.size()) || published!=publication) return false;
        os_=os;entry_=entry;return true;
    }
    template<class Invoke=TglNativeVeboxChannelInvoker>
    int restore(uint32_t channel,Invoke invoke=Invoke{}) const noexcept {
        static_assert(noexcept(invoke(uintptr_t{},uintptr_t{},uint32_t{})),"OS channel ABI");
        return os_ && entry_ ? invoke(entry_,os_,channel) : 5;
    }
};
// Native12cdac..12ce18 restores OS channel through OS+f0 BEFORE publishing
// the report, even when a previous Render stage failed. Its return is ignored;
// EAX at Render return is the saved stage result, not the restore status.
// Restore must already be qualified against65fbc/65fc3 ->62cd0, whose only
// mutation is OS+8=channel. This helper is not a fence or an admission gate.
template<class Restore>
int tglFinishOwnedVeboxRender(TglVeboxReportState& report,int renderResult,
    uint32_t outputPipe,uint8_t execution18,uint8_t execution8f4,Restore restore) noexcept {
    static_assert(noexcept(restore()) && std::is_same_v<decltype(restore()),int>,
        "native channel restore status ABI");
    (void)restore();
    tglFinalizeVeboxReport(report,outputPipe,execution18,execution8f4);
    return renderResult;
}

// Native12cccb..12cda6: update the two child statistics-selection flags.
// report09/pipe1 skips BOTH writes. Slotd0 is lazy and may be called twice;
// do not memoize it across the independent native branches. These flags
// describe layout selection, not submission success or GPU completion.
template<class Query>
void tglUpdateVeboxRenderStatisticsFlags(uint8_t& flag1b88,uint8_t& flag1b89,
    const std::array<uint8_t,0x15>& execution,uint32_t outputPipe,Query query) noexcept {
    static_assert(noexcept(query()) && std::is_same_v<decltype(query()),bool>,
        "native slotd0 bool ABI");
    if((execution[9]&1) && outputPipe==1) return;
    bool first=(execution[0xb]&1) || (execution[0xc]&1);
    if(!first) first=((execution[0x13]&1) || query()) && !(execution[8]&1);
    flag1b88=first;
    flag1b89=((execution[0x13]&1) || query()) && (execution[8]&1);
}
template<class Owner,class Query>
int tglPublishLeasedVeboxRenderStatisticsFlags(Owner& owner,void* child,
    const std::array<uint8_t,0x15>& execution,uint32_t outputPipe,Query query) noexcept {
    const uintptr_t address=reinterpret_cast<uintptr_t>(child);
    if(!child || !owner.hasPrivateChildLease(address)) return 5;
    if((execution[9]&1) && outputPipe==1) return 0;
    auto* bytes=static_cast<uint8_t*>(child);
    // Native publishes1b88 before the second independent slotd0 call.
    // Preserve that visibility rather than staging both writes atomically.
    tglUpdateVeboxRenderStatisticsFlags(bytes[0x1b88],bytes[0x1b89],execution,outputPipe,query);
    return owner.hasPrivateChildLease(address) ? 0 : 5;
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
// Type0 release63c5e calls62fa0: IOAccelResourceRelease(resource20) first,
// then vm_deallocate(task754128,resource50,resource10); import stubs23e0c8
// and23e524 resolve those exact symbols. No fence wait intervenes. Native
//62fa0 ignores vm_deallocate's status and returns its initialized zero, after
// which63c83 clears the descriptor. Thus release's success/zeroed shell is
// neither proof of CPU deallocation nor a hardware completion certificate.
// Native deferred mappings may preserve wired backing, but do not infer that
// an arbitrary external userspace source has the same lifetime protection.
// Family newResourceWithClientBuffer21ab6 page-aligns the client range and
// calls Shared2::memory_with_client_buffer29e9a. Cache reuse requires matching
// address/length AND client_address_persistent(); otherwise withClientBuffer
//542be validates VM regions, creates a descriptor via accelerator910 and
// stores it at SysMemory+d0 (54450), task30, client addressc8 and length40.
// TGL slotd1980 imports native createMemoryDescriptorWithAddressRange3f32a;
// there is no TGL-specific descriptor constructor. This establishes descriptor
// ownership. Map::prepare52754 cold57a32 calls Memory148=prepare258e;
// SysMemory1b0=wire54edc then prepares descriptor+d0 via1f0 at55066.
// Successful commit_pte527d0 sets map flag4 and calls Memory1d0, resolved
// in SysMemory vptr80570 to increment_wire_count55420 (counter14).
// release_pte52488 clears flag4 and calls1d8=decrement_wire_count5542a;
// unwire1b8 is reached only when counter14 AND getPrepareCount25d4 are zero.
// SysMemory::complete54e9a uses the same gate. Consequently map complete
// alone does not unwire a committed pending map: deferred release retains
// its PTE wire count until actual release_pte. This closes that native mapped
// backing mechanism, not proof every owned allocation reaches the map or that
// an independent borrowed source/CPU buffer can be released after submission.
// Client-buffer creation21bcd publishes SysMemory at Resource80, not88.
// Generic Resource::map26c70 uses88 when available for type0/a, otherwise80,
// calls Memory138 and publishes the mapping at Resource40 (26e28), selected
// Memory70 and event pointer20=Map38. Resource::free2157c removes its map link
// then calls Map28=release before dropping Memory80 at2162e; Map::free52508
// independently releases its retained Memory18. This connects generic mapped
// client backing to deferred retirement, separately from TGL aperture220/88.
// Owned execution still must prove its token type and actual map preparation;
// allocation alone is not evidence that the GPU backing is mapped or wired.
// Type0 native63030 writes descriptor DWORD0=0x80 at63118 (not kernel type0),
// calls IOAccelResourceCreate via23e0a4 at631a9 and gets shared metadata via
//23e0b0. Family new_resource2b806 accepts low-nibble-zero then exact0x80 at
//2bc07; branch2bca1 passes args58/40/48/50 to newResourceWithClientBuffer
//21ab6 at2bcd8. This closes native type0 allocation -> client SysMemory80.
// Do not apply this proof to native type1 IOSurface planes or borrowed FFDI.
// Type0 release63c5e calls62fa0:23e0c8 is IOAccelResourceRelease, then
// 23e524 is vm_deallocate(resource+50, size+10). Both results are ignored;
// nonnull input returns the local zero at63014, so63c67's success branch
// does not certify completion. Keep submitted backing generations alive
// before invoking this release; deferred kernel map references alone do not
// prove that a userspace virtual allocation cannot be reused prematurely.
// Type1 allocator63250 instead creates an IOSurface (stub23e152 at6342b),
// writes descriptor0=0x82 at634b2 and creates a token per plane at6356f.
// Family2b882 resolves descriptor40 via IOSurfaceRoot::lookupSurface(task),
// validates descriptor44 against getPlaneCount, then2be46 dispatches
// newResourceWithIOSurface229c2(surface,plane44,options48). These are real
// IOSurface-backed tokens, not type0 client VM ranges. newResourceWithIOSurface
// takes device-cache Resource20 as parent, retains it at22b00, stores childe0,
// plane indexf8 and client-aligned+plane offsetfc. Type82 map26d31 selects
// parent88, falling back to parent80 at26dee, then the SAME generic Map40
// construction/deferred release. Type82 free216b8 unlinks the parent's child
// list and releases parente0 plus parent after the map-release step2157c.
// deviceCacheForIOSurface3ec58 creates its parent via2277c: getMemoryDescriptor
// (relocation227c0) -> SysMemory::withMemoryDescriptor54a90 -> parent80 at227dd.
// That wrapper stores descriptor+d0 at54ae7 and explicitly retains it at54af5.
// Thus the plane's parent80 fallback reaches the same SysMemory wire-count
// lifetime described above; parent88 can instead select migrated backing.
// Borrowed source/history token leases and actual submitted map preparation
// remain obligations; userspace IOSurface retain alone still proves neither.
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
struct TglOwnedSurfaceStorage;
inline const uint8_t* tglOwnedSfcSurfaceBytes(const TglOwnedSurfaceStorage& surface) noexcept;
inline const uint8_t* tglOwnedSfcSurfaceBytes(const std::array<uint8_t,0x2a8>& surface) noexcept {
    return surface.data();
}
inline std::array<uint8_t,0x38> tglOwnedSfcOutputDescriptorBytes(const uint8_t* shell) noexcept {
    std::array<uint8_t,0x38> out{};
    const auto get=[&](size_t offset) {
        uint32_t value=0;std::memcpy(&value,shell+offset,4);return value;
    };
    const auto put=[&](size_t offset,uint32_t value) {
        std::memcpy(out.data()+offset,&value,4);
    };
    put(0,get(0x294));put(4,get(0x130));
    put(8,get(0xd8));put(0xc,get(0xdc));put(0x10,get(0xe0));put(0x14,get(0xe4));
    put(0x1c,get(0xf4));put(0x20,get(0xf8));
    const uint32_t pitch=get(0xe0);
    // Native TGL168744..16874b consumes this independent output+24 value
    // through a16-bit temporary for format19. Preserve native arithmetic
    // here; this descriptor builder is NOT a range/plane qualification gate.
    if(pitch) put(0x24,(get(0x100)-get(0xf0))/pitch+get(0x108));
    const uintptr_t resource=reinterpret_cast<uintptr_t>(shell+0x148);
    std::memcpy(out.data()+0x28,&resource,8);
    out[0x30]=shell[0x299]&1;
    return out;
}
template<class Surface>
inline std::array<uint8_t,0x38> tglOwnedSfcOutputDescriptor(const Surface& surface) noexcept {
    return tglOwnedSfcOutputDescriptorBytes(tglOwnedSfcSurfaceBytes(surface));
}
// TGL168744/1687a5/1687dd/168810/168848/16887b/1688b3 load output+24
// into the same16-bit temporary for these seven planar format branches.
inline bool tglNativeSfcUsesPlaneOffset(uint32_t format) noexcept {
    return format==0x19 || format==0x53 || format==0x52 || format==0x13 ||
           format==0x12 || format==0x17 || format==0x14;
}
// Planar output+24 is truncated to16 bits. Validate
// before any command write, not after the descriptor's uint32 arithmetic has
// already wrapped. Other format branches retain their separate qualification.
inline bool tglOwnedSfcPlanarOutputFits(uint32_t format,const uint8_t* shell) noexcept {
    if(!tglNativeSfcUsesPlaneOffset(format)) return true;
    const auto get=[&](size_t offset) noexcept {
        uint32_t value=0;std::memcpy(&value,shell+offset,4);return value;
    };
    const uint32_t pitch=get(0xe0),y=get(0xf0),uv=get(0x100);
    if(get(0x130)!=format || !pitch || uv<y) return false;
    const uint64_t row=uint64_t(uv-y)/pitch+get(0x108);
    return row<=0xffff;
}
// Exact TGL16864c..16867f: format-1 indexes83 signed-relative entries
// at16a434. These17 cases avoid default1688c3 (status31). This proves
// state-builder format admission only, not backing/plane/scaling eligibility.
inline bool tglNativeSfcStateFormatSupported(uint32_t format) noexcept {
    switch(format) {
        case 1:case 2:case 3:case 4:case 7:case 0xd:case 0x10:
        case 0x12:case 0x13:case 0x14:case 0x15:case 0x17:case 0x19:
        case 0x50:case 0x51:case 0x52:case 0x53:return true;
        default:return false;
    }
}
// TGL1695ef..169617 encodes (output pitch-1) in17 bits at DW29 bits3..19.
// Independent output+14 tile encoding at169567..1695bf sets DW29 bit0
// iff tile==1 and bit1 iff tile!=4. This is an encoding, NOT evidence of
// the admissible tile set; do not reject other values from these tests alone.
inline bool tglNativeSfcOutputPitchFits(uint32_t pitch) noexcept {
    return pitch && pitch<=0x20000;
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
struct TglOwnedStatisticsNoPublication {
    template<class Storage> void operator()(const Storage&,const TglStatisticsAllocation&) const noexcept {}
};
// Owned TGL child publication, not an ICL virtual-base pointer transplant.
template<class ChildOwner>
int tglPublishOwnedStatisticsLayout(ChildOwner& owner,void* child,
    const TglStatisticsAllocation& layout) noexcept {
    if(!child || !owner.hasPrivateChildLease(reinterpret_cast<uintptr_t>(child)) ||
       !layout.rowBytes || (layout.rowBytes&63) || !layout.blockRows ||
       uint64_t{layout.totalRows}!=uint64_t{layout.blockRows}+
           (uint64_t{1024}+layout.rowBytes-1)/layout.rowBytes ||
       uint64_t{layout.rowBytes}*layout.totalRows!=layout.bytes) return 5;
    auto* bytes=static_cast<uint8_t*>(child);
    std::memcpy(bytes+0x1114,&layout.rowBytes,4);
    std::memcpy(bytes+0x1118,&layout.blockRows,4);
    return 0;
}
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

// ICL1f8557..1f85fc data-stage order. Qualified producers supply their own
// surface-info ABI; this does not reinterpret an ICL virtual-base object.
// Keep source backing pinned and publish boundary dimensions only on success.
struct TglOwnedStatisticsBoundaryNoFinish {
    int operator()(uint32_t,uint32_t) const noexcept {return 0;}
};
template<class Info,class Owner,class QueryDi,class ReadInfo,class Boundary,
    class Finish=TglOwnedStatisticsBoundaryNoFinish>
int tglPrepareOwnedStatisticsBoundary(Owner& owner,uint8_t execution13,
    QueryDi queryDi,ReadInfo readInfo,Boundary boundary,uint32_t& width,uint32_t& height,
    Finish finish={},const TglOwnedSurfaceStorage* captured=nullptr) noexcept {
    static_assert(noexcept(queryDi()),"statistics DI query ABI");
    int result=5;
    const auto consume=[&](const auto& source) noexcept {
        if(!owner.hasSurfaceLease(source)) return;
        const bool di=(execution13&1)!=0 || (queryDi()&1)!=0;
        Info info{};
        static_assert(noexcept(readInfo(source,info)) &&
            std::is_same_v<decltype(readInfo(source,info)),int>,"statistics source-info ABI");
        result=readInfo(source,info);
        if(result) return;
        uint32_t candidateWidth=0,candidateHeight=0;
        static_assert(noexcept(boundary(info,candidateWidth,candidateHeight,di)) &&
            std::is_same_v<decltype(boundary(info,candidateWidth,candidateHeight,di)),int>,
            "statistics boundary ABI");
        result=boundary(info,candidateWidth,candidateHeight,di);
        if(!owner.hasSurfaceLease(source)) {result=5;return;}
        if(result) return;
        static_assert(noexcept(finish(candidateWidth,candidateHeight)) &&
            std::is_same_v<decltype(finish(candidateWidth,candidateHeight)),int>,"statistics finish ABI");
        result=finish(candidateWidth,candidateHeight);
        if(!owner.hasSurfaceLease(source)) {result=5;return;}
        if(!result) {width=candidateWidth;height=candidateHeight;}
    };
    if(captured) {
        if(!owner.hasSurfaceLease(*captured)) return 5;
        consume(*captured);
        return owner.hasSurfaceLease(*captured)?result:5;
    }
    const bool leased=owner.withSurface(consume);
    return leased?result:5;
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

struct TglNativeSfcOutputUsageInvoker {
    void operator()(uintptr_t entry,uintptr_t os,const void* resource,
                    uint32_t usage,uint32_t flag) const noexcept {
        using Update=void(*)(void*,const void*,uint32_t,uint32_t);
        reinterpret_cast<Update>(entry)(reinterpret_cast<void*>(os),resource,usage,flag);
    }
};
// Exact TGL initializer65ea0:66340/66347 installs659b0 at OS+128.
// Unlike OS+248 relocation registration, this is a four-argument void stub.
// OS+130 is NOT proved here. Flagged surfaces must remain unsupported until
// that independent callback is authenticated; no neighboring stub substitute.
// Bounded initializer65ea0..666da contains no direct OS+130 assignment:
// 66352/66359 installs659d0 at+178, NOT+130. Its earlier62750 call receives
// the second (context) argument; neither this search nor the adjacent stubs
// proves a completion/retirement callback or permission to release resources.
// Bounded62750..628f5 instead initializes16 context records of stride15c0:
// allocations2f0/10 at record+10/+20, with allocation-failure unwind.
// These data-pointer stores are NOT OS+130 callback publication.
class TglNativeSfcOutputUsageBinding {
    uintptr_t context_=0,entry_=0;
public:
    template<class Read,class Qualify,class Executable>
    bool resolve(uintptr_t os,uintptr_t image,Read read,Qualify qualify,Executable executable) {
        context_=entry_=0;
        constexpr std::array<uint8_t,20> anchor{
            0x55,0x48,0x89,0xe5,0x48,0x89,0x7d,0xf8,0x48,0x89,
            0x75,0xf0,0x89,0x55,0xec,0x89,0x4d,0xe8,0x5d,0xc3};
        uintptr_t entry=0;std::array<uint8_t,20> actual{};
        if(!os || os>UINTPTR_MAX-0x130 || !image || image>UINTPTR_MAX-0x659c4 ||
            !qualify(image) || !read(os+0x128,&entry,8) || entry!=image+0x659b0 ||
            !executable(entry,anchor.size()) || !read(entry,actual.data(),actual.size()) ||
            actual!=anchor) return false;
        context_=os;entry_=entry;return true;
    }
    // Enclosing caller owns OS/image/surface leases throughout command use.
    template<class Surface,class Use,class Invoke=TglNativeSfcOutputUsageInvoker>
    int withOutput(const Surface& surface,Use use,Invoke invoke=Invoke{}) const noexcept {
        static_assert(noexcept(use()) && noexcept(invoke(uintptr_t{},uintptr_t{},
            static_cast<const void*>(nullptr),uint32_t{},uint32_t{})),"SFC usage scope ABI");
        const auto* bytes=tglOwnedSfcSurfaceBytes(surface);
        if(!context_ || !entry_ || (bytes[0xe8]&1)) return 5;
        invoke(entry_,context_,bytes+0x148,6,1);
        return use();
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

// Statistics donor calls OS+240, not a raw userspace memset: native41440
// acquires the resource through OS+220 (FinishSysMem for type0), fills, then
// releases through OS+228. A writable mapping alone is not that protocol.
// Borrowed OS/image lifetime and exclusion of in-flight GPU users are caller
// obligations. This binding is not a completion or retirement certificate.
struct TglNativeBufferFillBinding {
    uintptr_t context = 0, entry = 0;
    template<class Read, class Qualify, class Executable>
    bool resolve(uintptr_t os, uintptr_t image, Read read, Qualify qualify,
                 Executable executable) {
        *this = {};
        if (!os || os > UINTPTR_MAX - 0x248 || !image ||
            image > UINTPTR_MAX - 0x63f50 || !qualify(image)) return false;
        uintptr_t lock = 0, unlock = 0, fill = 0;
        if (!read(os + 0x220, &lock, sizeof(lock)) ||
            !read(os + 0x228, &unlock, sizeof(unlock)) ||
            !read(os + 0x240, &fill, sizeof(fill)) ||
            lock != image + 0x63da0 || unlock != image + 0x63f40 ||
            fill != image + 0x41440 || !executable(lock, 16) ||
            !executable(unlock, 16) || !executable(fill, 22)) return false;
        constexpr std::array<uint8_t, 22> anchor{
            0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x40,
            0x48,0x89,0x7d,0xf0,0x48,0x89,0x75,0xe8,
            0x89,0x55,0xe4,0x88,0x4d,0xe3};
        std::array<uint8_t, 22> actual{};
        if (!read(fill, actual.data(), actual.size()) || actual != anchor) return false;
        context = os; entry = fill; return true;
    }
    // Mutable descriptor is deliberate: native lock/unlock updates its CPU
    // access bookkeeping. Never cast away an owner's const storage lease.
    template<class Writable>
    int fill(std::array<uint8_t, 0x148>& resource, uint32_t bytes,
             uint8_t value, Writable writable) const noexcept {
        if (!context || !entry) return 5;
        auto invoke = [&](uintptr_t, uint32_t length, uint8_t byte) noexcept {
            using Native = int (*)(uintptr_t, void*, uint32_t, uint8_t);
            return reinterpret_cast<Native>(entry)(context, resource.data(), length, byte);
        };
        return tglFillBuffer(resource, bytes, value, writable, invoke);
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
    bool nativeFillCompatible(const TglNativeBufferFillBinding& binding) const noexcept {
        return context_ && allocate_ && release_ && type_ == 0 && binding.entry &&
            binding.context == reinterpret_cast<uintptr_t>(context_);
    }
    template<class Writable>
    int fillNative(Storage& storage, uint32_t bytes, uint8_t value,
                   const TglNativeBufferFillBinding& binding, Writable writable) const noexcept {
        // An authenticated Fill from a different OS cannot initialize a
        // descriptor allocated by this backend. Never call it on type1 here.
        if (!nativeFillCompatible(binding)) return 5;
        return binding.fill(storage, bytes, value, writable);
    }
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
        const int status=backend_.backed(storage_) ? invoke(static_cast<const Storage&>(storage_)) : 5;
        busy_=false;
        return status;
    }
    // CPU initialization may update native descriptor lock bookkeeping.
    // Caller must exclude submitted GPU users; this is only a synchronous lease.
    template<class Invoke> int withMutableStorage(Invoke invoke) noexcept {
        static_assert(noexcept(invoke(std::declval<Storage&>())),"mutable resource lease status ABI");
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

// One synchronous command/submit/wait scope over all source/history backing.
// No implicit release on success OR error, and no GPU-completion claim. The
// caller retains the owners after return, including on reset/timeout ambiguity.
// This must not be used to let an asynchronous submission escape its owners.
template<class Owner,size_t Count,class Invoke>
int tglWithOwnedResourceGroupStorage(const std::array<Owner*,Count>& owners,
                                    Invoke invoke) noexcept {
    static_assert(Count>0 && Count<=16,"bounded owned backing group");
    using Storage=typename Owner::Storage;
    using Views=std::array<const Storage*,Count>;
    static_assert(noexcept(invoke(std::declval<const Views&>())),"group status ABI");
    for(size_t i=0;i<Count;++i) {
        if(!owners[i]) return 5;
        for(size_t j=0;j<i;++j) if(owners[i]==owners[j]) return 5;
    }
    Views views{};
    const auto enter=[&](const auto& self,size_t index) noexcept -> int {
        if(index==Count) return invoke(static_cast<const Views&>(views));
        return owners[index]->withStorage([&](const Storage& storage) noexcept {
            views[index]=&storage;
            const int result=self(self,index+1);
            views[index]=nullptr;
            return result;
        });
    };
    return enter(enter,0);
}

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
// Concrete statistics producer:52bf0 maps format130/widthd8/heightdc and
// copies source50..5f to info2c..3b. Native170ef0 consumes info34/38 as
// floats directly (not right-left/bottom-top). Reuse the proven converter
// and bounded boundary arithmetic; never substitute requested VM dimensions.
// alignDi64 is the qualified boundary object's2052 bit, NOT a VEBOX-child
// offset: the1bc0-byte owned child cannot contain that field.
template<class Owner,class QueryDi,class Finish>
int tglPrepareOwnedStatisticsFromLeasedSource(Owner& owner,
    const TglOwnedSurfaceStorage& source,uint8_t execution13,QueryDi queryDi,
    bool alignDi64,uint32_t& width,uint32_t& height,Finish finish) noexcept {
    using Info=std::array<uint8_t,0x48>;
    auto readInfo=[&](const TglOwnedSurfaceStorage& leased,Info& info) noexcept -> int {
        if(!owner.hasSurfaceLease(leased)) return 5;
        info=tglSurfaceDescriptorBytes(reinterpret_cast<const uint8_t*>(&leased));
        return 0;
    };
    auto boundary=[&](const Info& info,uint32_t& w,uint32_t& h,bool di) noexcept -> int {
        uint32_t format=0,nativeWidth=0,nativeHeight=0;
        float sourceWidth=0,sourceHeight=0;
        std::memcpy(&format,info.data()+8,4);
        std::memcpy(&nativeWidth,info.data()+0xc,4);
        std::memcpy(&nativeHeight,info.data()+0x10,4);
        std::memcpy(&sourceWidth,info.data()+0x34,4);
        std::memcpy(&sourceHeight,info.data()+0x38,4);
        return tglSurfaceBoundary(format,nativeWidth,nativeHeight,sourceWidth,
            sourceHeight,di,alignDi64,w,h);
    };
    return tglPrepareOwnedStatisticsBoundary<Info>(owner,execution13,queryDi,
        readInfo,boundary,width,height,finish,&source);
}
// Render12c9ec..12ca00 snapshots argument2+10/+18, then passes those
// surfaces to mode190/188/198 and tail1a8. The descriptor itself is not
// retained or otherwise consumed in 12c9d0..12ce18. Keep it stack-owned
// inside the source/target AND child-image leases. Slot1a0 may select a
// different source; that callback still needs its own complete owner contract.
// This closes only the outer descriptor shape, not full Render admission,
// asynchronous backing retention or permission to publish an incomplete child.
template<class ChildOwner,class SourceOwner,class TargetOwner,class Invoke>
int tglInvokeLeasedVeboxSurfaceRender(const TglVeboxRenderBinding& binding,
    ChildOwner& childOwner,void* child,void* argument1,
    SourceOwner& sourceOwner,const TglOwnedSurfaceStorage& source,
    TargetOwner& targetOwner,const TglOwnedSurfaceStorage& target,Invoke invoke) noexcept {
    if(!sourceOwner.hasSurfaceLease(source) || !targetOwner.hasSurfaceLease(target)) return 5;
    alignas(8) std::array<uint8_t,0x20> descriptor{};
    const uintptr_t sourceAddress=reinterpret_cast<uintptr_t>(&source);
    const uintptr_t targetAddress=reinterpret_cast<uintptr_t>(&target);
    std::memcpy(descriptor.data()+0x10,&sourceAddress,8);
    std::memcpy(descriptor.data()+0x18,&targetAddress,8);
    const int result=tglInvokeLeasedVeboxRender(binding,childOwner,child,
        argument1,descriptor.data(),invoke);
    if(!sourceOwner.hasSurfaceLease(source) || !targetOwner.hasSurfaceLease(target) ||
       !childOwner.hasPrivateChildImageLease(reinterpret_cast<uintptr_t>(child),binding.image))
        return 5;
    return result;
}
inline bool tglVeboxMode0NativeSkipPredicate(uint8_t report0b,
    const TglOwnedSurfaceStorage& current,const TglOwnedSurfaceStorage& previous) noexcept {
    if(!(report0b&1)) return true;
    if((current.tail[0xa]&1)!=(previous.tail[0xa]&1) ||
       (current.tail[9]&1)!=(previous.tail[9]&1)) return false;
    // Only the six metadata fields are read; never copy/adopt OsResource148.
    return std::memcmp(current.tail.data()+0xc,previous.tail.data()+0xc,4)==0 &&
        std::memcmp(current.prefix.data()+0xd8,previous.prefix.data()+0xd8,12)==0;
}
template<class CurrentOwner,class PreviousOwner>
int tglSelectLeasedVeboxMode0Allocation(CurrentOwner& currentOwner,
    const TglOwnedSurfaceStorage& current,PreviousOwner& previousOwner,
    const TglOwnedSurfaceStorage* previous,uint8_t report09,uint8_t report0b,
    uint32_t pipe,TglVeboxMode0Allocation& output) noexcept {
    if(!currentOwner.hasSurfaceLease(current)) return 5;
    int failure=0;
    const auto selected=tglVeboxMode0AllocationDecision(report09,current.prefix[0x86],pipe,
        [&]() noexcept {
            if(!(report0b&1)) return true; // native never reads previous here
            if(!previous || !previousOwner.hasSurfaceLease(*previous)) {failure=5;return false;}
            return tglVeboxMode0NativeSkipPredicate(report0b,current,*previous);
        });
    if(failure || !currentOwner.hasSurfaceLease(current)) return 5;
    output=selected;return 0;
}
inline const uint8_t* tglOwnedSfcSurfaceBytes(const TglOwnedSurfaceStorage& surface) noexcept {
    return reinterpret_cast<const uint8_t*>(&surface);
}
// Selection-only DI snapshot, inside the caller's source/parameter frame
// leases. Equality authenticates the separately leased parameter identity;
// no allocation adoption, publication, or assumption about its total extent.
// Only the proven first DWORD is read. False means acquisition failure, not
// "DI allowed": every failure latches the frame so selection cannot publish.
template<class Read>
bool tglRejectLeasedGen12DiOutput(bool& rejected,const TglOwnedSurfaceStorage& source,
    uintptr_t leasedDiParams,TglVeboxPipeAcquisition& frame,Read read) noexcept {
    static_assert(noexcept(read(uintptr_t{},static_cast<void*>(nullptr),size_t{})) &&
        std::is_same_v<decltype(read(uintptr_t{},static_cast<void*>(nullptr),size_t{})),bool>,
        "DI snapshot reader must return bool without throwing");
    if(!frame.healthy()) return false;
    auto fail=[&]() noexcept {frame.fail();return false;};
    uintptr_t params=0;std::memcpy(&params,source.prefix.data()+0x88,sizeof(params));
    if(params!=leasedDiParams) return fail();
    bool candidate=false;
    if(params) {
        if(params>UINTPTR_MAX-3) return fail();
        uint32_t mode=0,height=0,format=0;
        if(!read(params,&mode,4) || (mode!=1 && mode!=2)) return fail();
        std::memcpy(&height,source.prefix.data()+0xdc,4);
        std::memcpy(&format,source.prefix.data()+0x130,4);
        candidate=tglRejectGen12BobOutput(true,mode==1,height,format);
    }
    rejected=candidate;return true;
}
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
struct TglOwnedFfdiMetadataQuery {uint32_t colorSpace=0,sampleType=0;};
// FFDI1f7cc8..1f7e0a uses query+a0's two DWORDs for target0/138,
// not input0/138; rotation290 and frame rectangles come from input.
// Borrowed handle1a0 and conditional200 belong to a separate
// retained backing lease and must never be copied by this metadata producer.
inline bool tglPrepareOwnedFfdiFrameMetadata(TglOwnedSurfaceStorage& target,
    const TglOwnedSurfaceStorage& source,TglOwnedFfdiMetadataQuery query) noexcept {
    if(&target==&source) return false;
    std::memcpy(target.prefix.data(),&query.colorSpace,4);
    std::memcpy(target.prefix.data()+0x30,source.prefix.data()+0x30,0x30);
    std::memcpy(target.prefix.data()+0x138,&query.sampleType,4);
    std::memcpy(target.tail.data(),source.tail.data(),4); // surface290
    return true;
}
enum class TglOwnedFfdnHistoryDecision { Keep, Invalidate, CopyPrevious };
// ICL1f81c1..1f8311: frame rectangles, ColorSpace0, SampleType138 and DWORDb0.
// QWORD90 is the DN parameter pointer (tglAdmitOwnedDndiSource); its copied
// target lifetime is not closed. Keep it out of CPU metadata: a surface
// lease alone does not pin the separate tracked parameter owner.
// No rotation290/resource ownership transfer is proven for this FFDN tail.
inline bool tglPrepareOwnedFfdnFrameMetadata(TglOwnedSurfaceStorage& target,
    const TglOwnedSurfaceStorage& source) noexcept {
    if(&target==&source) return false;
    tglCopyOwnedSurfaceFrameMetadata(target,source);
    std::memcpy(target.prefix.data()+0xb0,source.prefix.data()+0xb0,4);
    return true;
}
struct TglOwnedFfdiCompressionSelection {
    bool compressed=false;
    uint32_t mode=0;
};
// Scope the FFDN90 DN pointer separately from scalar metadata. Source must
// already be leased and identify this actual tracked allocation. Publication
// into stack-owned metadata is restored before the parameter lease ends.
// This permits no consuming native160 callback or asynchronous pointer use.
template<class Parameters,class Use>
int tglWithOwnedFfdnDnMetadata(TglOwnedSurfaceStorage& metadata,
    const TglOwnedSurfaceStorage& source,Parameters& parameters,Use use) noexcept {
    static_assert(noexcept(use(std::declval<const TglOwnedSurfaceStorage&>())),"FFDN DN metadata ABI");
    if(&metadata==&source) return 5;
    uintptr_t existing=0,expected=0;
    std::memcpy(&existing,metadata.prefix.data()+0x90,8);
    std::memcpy(&expected,source.prefix.data()+0x90,8);
    if(existing || !expected) return 5;
    int result=5;
    const bool leased=parameters.withBorrowedAddress([&](uintptr_t pointer,size_t extent) noexcept {
        if(pointer!=expected || extent<8) return;
        std::memcpy(metadata.prefix.data()+0x90,&pointer,8);
        struct Restore {uint8_t* slot;~Restore(){std::memset(slot,0,8);}} restore{metadata.prefix.data()+0x90};
        result=use(static_cast<const TglOwnedSurfaceStorage&>(metadata));
    });
    return leased?result:5;
}
// ICL1f7c14..1f7cc0: scalar subset of execution60 metadata. The borrowed
// handle1a0 is deliberately excluded and remains available only via its
// actual leased owner view. No token/reference is retained by this copy.
inline void tglCopyOwnedFfdiPrimaryScalars(TglOwnedSurfaceStorage& target,
    const TglOwnedSurfaceStorage& execution) noexcept {
    if(&target==&execution) return;
    constexpr size_t base=offsetof(TglOwnedSurfaceStorage,resource);
    std::memcpy(target.resource.data()+0x1c8-base,execution.resource.data()+0x1c8-base,4);
    uint32_t field200=0;
    std::memcpy(&field200,execution.resource.data()+0x200-base,4);
    if(field200!=0 && field200!=0xffffffffu)
        std::memcpy(target.resource.data()+0x200-base,&field200,4);
}
// ICL1f7e52..1f7eb4, after the allocation loop: execution68 overrides
// only slot0's scalar fields1c8/200. Unlike execution60, field200 is copied
// even when zero/-1. No IOSurface handle1a0 or other resource bytes transfer.
// Caller must hold the secondary owner lease; this is not backing adoption.
inline void tglCopyOwnedFfdiSecondaryMetadata(TglOwnedSurfaceStorage& target,
    const TglOwnedSurfaceStorage& secondary) noexcept {
    if(&target==&secondary) return;
    constexpr size_t resourceOffset=offsetof(TglOwnedSurfaceStorage,resource);
    std::memcpy(target.resource.data()+0x1c8-resourceOffset,
        secondary.resource.data()+0x1c8-resourceOffset,4);
    std::memcpy(target.resource.data()+0x200-resourceOffset,
        secondary.resource.data()+0x200-resourceOffset,4);
}
// ICL1f7a50..1f7af2 allocation data policy. +a8 selects source metadata;
// otherwise child1ba9 and tile==1 gate +90(format). This is not a derived
// object's ABI, permission to enable MMC, or proof of compressed backing.
template<class Supports>
TglOwnedFfdiCompressionSelection tglSelectOwnedFfdiCompression(bool followSource,
    bool source299,bool source29a,bool child1ba9,uint32_t format,uint32_t tile,
    Supports supports) noexcept {
    static_assert(noexcept(supports(format)),"FFDI compression query ABI");
    if(followSource) return {source299,source29a?1u:0u};
    if(child1ba9 && tile==1 && supports(format)) return {true,1};
    return {};
}
// ICL1f7e0c..1f7e3d updates execution13/14 only after allocation changed.
// These are whole-byte assignments, not bit merges or backing admission.
inline void tglUpdateOwnedFfdiExecutionCompression(bool allocationChanged,
    TglOwnedFfdiCompressionSelection selected,uint8_t& execution13,
    uint8_t& execution14) noexcept {
    if(!allocationChanged) return;
    execution13=selected.compressed?1u:0u;
    execution14=static_cast<uint8_t>(selected.mode);
}
struct TglOwnedFfdnCompressionSelection {
    bool compressed;
    uint32_t mode;
};
enum class TglOwnedFfdiAllocationKind { DescriptorOnly, NativeBacking };
struct TglOwnedFfdiAllocationRequest {
    TglResourceKey key{};
    uint32_t type=1;
    TglOwnedFfdiAllocationKind kind=TglOwnedFfdiAllocationKind::DescriptorOnly;
};
// ICL1f7b6c..1f7be4 passes queried format/tile, type1, and source d8/dc.
// Its allocator49fe0 distinguishes type1/format3d at4a0a8..4a0c2; other
// type1 formats take descriptor-only4a14d. A request is never backing proof
// or a resource-token adoption. Borrowed source/history leases remain required.
inline bool tglPrepareOwnedFfdiAllocationRequest(const TglOwnedSurfaceStorage& source,
    uint32_t format,uint32_t tile,TglOwnedFfdiCompressionSelection compression,
    TglOwnedFfdiAllocationRequest& output) noexcept {
    uint32_t width=0,height=0;
    std::memcpy(&width,source.prefix.data()+0xd8,4);
    std::memcpy(&height,source.prefix.data()+0xdc,4);
    if(!width || !height) return false;
    TglOwnedFfdiAllocationRequest candidate;
    candidate.key={width,height,format,tile,compression.mode,compression.compressed};
    candidate.kind=format==0x3d?TglOwnedFfdiAllocationKind::NativeBacking:
        TglOwnedFfdiAllocationKind::DescriptorOnly;
    output=candidate;return true;
}
// Private descriptor-only slot: native49fe0/4a14d does not allocate GPU
// backing for type1/format!=3d. Never expose withSurface()/resource-token
// ownership: callers must separately retain the borrowed source resource.
class TglOwnedDescriptorOnlySurface {
    TglOwnedSurfaceStorage storage_{};
    bool ready_=false,busy_=false;
public:
    TglOwnedDescriptorOnlySurface()=default;
    TglOwnedDescriptorOnlySurface(const TglOwnedDescriptorOnlySurface&)=delete;
    TglOwnedDescriptorOnlySurface& operator=(const TglOwnedDescriptorOnlySurface&)=delete;
    bool prepare(const TglOwnedFfdiAllocationRequest& request,bool& changed) noexcept {
        if(busy_ || request.type!=1 || request.kind!=TglOwnedFfdiAllocationKind::DescriptorOnly ||
           request.key.format==0x3d || !request.key.width || !request.key.height) return false;
        // Preserve unrelated shell metadata, exactly as the descriptor branch.
        const uint32_t zero=0,one=1;
        std::memcpy(storage_.prefix.data()+0x134,&zero,4);
        std::memcpy(storage_.resource.data()+0x14,&one,4); // surface15c
        std::memcpy(storage_.prefix.data()+0xd8,&request.key.width,4);
        std::memcpy(storage_.prefix.data()+0xdc,&request.key.height,4);
        std::memcpy(storage_.prefix.data()+0x130,&request.key.format,4);
        std::memcpy(storage_.prefix.data()+0xe4,&request.key.tile,4);
        std::memcpy(storage_.prefix.data()+0xe0,&zero,4);
        ready_=true;changed=true;return true; //4a1af writes1 even on reuse
    }
    // Persist only qualified frame scalars. Borrowed handle1a0 and DN90
    // are deliberately absent; owners of those allocations must remain
    // separately pinned when commands consume this descriptor.
    bool publishFfdiFrame(const TglOwnedSurfaceStorage& input,TglOwnedFfdiMetadataQuery query,
        const TglOwnedSurfaceStorage* execution=nullptr,
        const TglOwnedSurfaceStorage* secondary=nullptr) noexcept {
        if(!ready_ || busy_ || &input==&storage_ || execution==&storage_ || secondary==&storage_)
            return false;
        if(!tglPrepareOwnedFfdiFrameMetadata(storage_,input,query)) return false;
        if(execution) tglCopyOwnedFfdiPrimaryScalars(storage_,*execution);
        if(secondary) tglCopyOwnedFfdiSecondaryMetadata(storage_,*secondary);
        return true;
    }
    bool publishFfdnFrame(const TglOwnedSurfaceStorage& input) noexcept {
        return ready_ && !busy_ && tglPrepareOwnedFfdnFrameMetadata(storage_,input);
    }
    // ICL1f7e52 executes outside the +b0 FFDI allocation gate. An existing
    // slot0 can receive execution68's two scalars without another prepare or
    // primary frame publication. No handle/DN adoption or backing allocation.
    template<class SourceOwner>
    bool publishFfdiSecondaryFrom(SourceOwner& owner,const TglOwnedSurfaceStorage& secondary) noexcept {
        if(!ready_ || busy_ || &secondary==&storage_ || !owner.hasSurfaceLease(secondary)) return false;
        tglCopyOwnedFfdiSecondaryMetadata(storage_,secondary);return true;
    }
    // Native execution60->FFDI1a0 is a borrowed IOSurface/resource58, not
    // a separately owned resource. Require the complete actual type1 source
    // owner lease (including its token list), publish only during this call,
    // and detach before the caller can release that source owner.
    template<class SourceOwner,class Use>
    bool withBorrowedExecutionHandle(SourceOwner& owner,const TglOwnedSurfaceStorage& source,
        Use use) noexcept {
        static_assert(noexcept(use(std::declval<const TglOwnedSurfaceStorage&>())) &&
            std::is_same_v<decltype(use(std::declval<const TglOwnedSurfaceStorage&>())),void>,
            "borrowed execution handle ABI");
        if(!ready_ || busy_ || !owner.hasSurfaceLease(source)) return false;
        uint32_t type=0;uintptr_t pointer=0,existing=0;
        std::memcpy(&type,source.resource.data()+0x14,4);
        std::memcpy(&pointer,source.resource.data()+0x58,8);
        std::memcpy(&existing,storage_.resource.data()+0x58,8);
        if(type!=1 || !pointer || existing || !tglNativeBackingPresent(source.resource)) return false;
        busy_=true;
        struct Restore {uint8_t* slot;bool& busy;
            ~Restore(){std::memset(slot,0,8);busy=false;}} restore{storage_.resource.data()+0x58,busy_};
        std::memcpy(storage_.resource.data()+0x58,&pointer,8);
        use(static_cast<const TglOwnedSurfaceStorage&>(storage_));
        return owner.hasSurfaceLease(source);
    }
    // Only a synchronous, non-consuming DN view. The original surface owner
    // pins all resource tokens; the parameter owner pins the allocation.
    // Native160 and asynchronous GPU consumers are not admitted here.
    template<class SourceOwner,class Parameters,class Use>
    bool withBorrowedDenoiseMetadata(SourceOwner& owner,const TglOwnedSurfaceStorage& source,
        Parameters& parameters,Use use) noexcept {
        static_assert(noexcept(use(source,std::declval<const TglOwnedSurfaceStorage&>())) &&
            std::is_same_v<decltype(use(source,std::declval<const TglOwnedSurfaceStorage&>())),void>,
            "descriptor DN callback ABI");
        if(!ready_ || busy_ || !owner.hasSurfaceLease(source)) return false;
        busy_=true;
        struct Unlock {bool& busy;~Unlock(){busy=false;}} unlock{busy_};
        return owner.withBorrowedDenoiseMetadata(source,parameters,storage_,use);
    }
    // Compose the two independently qualified roles without recursively
    // acquiring this descriptor. Caller must already pin both actual owners;
    // no inference that input and execution are the same surface is made.
    template<class SourceOwner,class Parameters,class ExecutionOwner,class Use>
    bool withBorrowedExecutionAndDenoise(SourceOwner& owner,const TglOwnedSurfaceStorage& source,
        Parameters& parameters,ExecutionOwner& executionOwner,
        const TglOwnedSurfaceStorage& execution,Use use) noexcept {
        if(!owner.hasSurfaceLease(source)) return false;
        bool consumed=false;
        const bool leased=withBorrowedExecutionHandle(executionOwner,execution,
            [&](const auto&) noexcept {
                consumed=owner.withBorrowedDenoiseMetadata(source,parameters,storage_,use);
            });
        return leased && consumed;
    }
    template<class Use> bool withDescriptor(Use use) noexcept {
        static_assert(noexcept(use(std::declval<const TglOwnedSurfaceStorage&>())) &&
            std::is_same_v<decltype(use(std::declval<const TglOwnedSurfaceStorage&>())),void>,
            "descriptor-only callback ABI");
        if(!ready_ || busy_) return false;
        busy_=true;
        struct Unlock {bool& busy;~Unlock(){busy=false;}} unlock{busy_};
        use(static_cast<const TglOwnedSurfaceStorage&>(storage_));return true;
    }
    void reset() noexcept {if(!busy_) {storage_={};ready_=false;}}
};

// ICL1f7eba..1f7fb1 data policy only, not its virtual-base object ABI.
// Keep field provenance explicit until the TGL allocation caller supplies
// qualified feature flags. This neither enables MMC nor allocates backing.
inline TglOwnedFfdnCompressionSelection tglSelectOwnedFfdnCompression(
    bool child1b89,bool child1b88,bool execution0b,bool source299,
    bool source29a,bool history29a,bool ffdiCompressed,uint32_t ffdiMode) noexcept {
    const bool useSource=(child1b89 && !child1b88 && execution0b) ||
        (!source29a && (ffdiCompressed || !history29a));
    return useSource ? TglOwnedFfdnCompressionSelection{source299,source29a?1u:0u} :
        TglOwnedFfdnCompressionSelection{ffdiCompressed,ffdiMode};
}
template<class SourceOwner,class HistoryOwner>
bool tglSelectOwnedFfdnCompressionFromLeases(SourceOwner& sourceOwner,
    const TglOwnedSurfaceStorage& source,HistoryOwner& historyOwner,
    const TglOwnedSurfaceStorage& history,bool child1b89,bool child1b88,
    bool execution0b,bool ffdiCompressed,uint32_t ffdiMode,
    TglOwnedFfdnCompressionSelection& output) noexcept {
    if(!sourceOwner.hasSurfaceLease(source) || !historyOwner.hasSurfaceLease(history)) return false;
    constexpr size_t tailBase=offsetof(TglOwnedSurfaceStorage,tail);
    output=tglSelectOwnedFfdnCompression(child1b89,child1b88,execution0b,
        (source.tail[0x299-tailBase]&1)!=0,(source.tail[0x29a-tailBase]&1)!=0,
        (history.tail[0x29a-tailBase]&1)!=0,ffdiCompressed,ffdiMode);
    return true;
}

// ICL1f800f/1f802b and1f8048/1f8065: FFDN uses the source's format130,
// tilee4 and dimensionsd8/dc, not the FFDI query98 output. Same allocator
// type1 contract, including descriptor-only formats; no backing is created.
using TglOwnedFfdnAllocationRequest=TglOwnedFfdiAllocationRequest;
inline bool tglPrepareOwnedFfdnAllocationRequest(const TglOwnedSurfaceStorage& source,
    TglOwnedFfdnCompressionSelection compression,TglOwnedFfdnAllocationRequest& output) noexcept {
    uint32_t format=0,tile=0;
    std::memcpy(&format,source.prefix.data()+0x130,4);
    std::memcpy(&tile,source.prefix.data()+0xe4,4);
    return tglPrepareOwnedFfdiAllocationRequest(source,format,tile,
        {compression.compressed,compression.mode},output);
}
// ICL1f8436..1f84da: two inline STMM resources use source dimensions,
// format3d/type1/tile1; the already-qualified child1ba9 supplies compression.
// Produces requests only; neither feature admission nor backing/fence proof.
inline bool tglPrepareOwnedStmmHistoryKeys(const TglOwnedSurfaceStorage& source,
    bool child1ba9,std::array<TglResourceKey,2>& output) noexcept {
    uint32_t width=0,height=0;
    std::memcpy(&width,source.prefix.data()+0xd8,4);
    std::memcpy(&height,source.prefix.data()+0xdc,4);
    if(!width || !height) return false;
    const TglResourceKey key{width,height,0x3d,1,child1ba9?1u:0u,child1ba9};
    output={key,key};return true;
}
// Complete ICL1f7990 changed-FFDN branch. Source+200 in decompiler output
// is decimal200 (0xc8), a previous-surface link, NOT resource field0x200.
// No whole-shell copy or ownership transfer here. CopyPrevious still requires
// an independently retained previous-source lease and a safe copy destination.
// ICL1f8159..1f8196 passes child+1b8 as destination and source+c8
// as previous source to its virtual1c0. The newly allocated ba8[index]
// contributes only the pitch comparison; it is NOT the copy destination.
// Neither that virtual slot nor the virtual-base adjustment is a TGL ABI.
inline TglOwnedFfdnHistoryDecision tglOwnedFfdnHistoryDecision(bool changed,
    bool referenceValid,bool executionFlag13,bool hasPrevious,
    uint32_t allocatedPitch,uint32_t previousPitch) noexcept {
    if(!changed) return TglOwnedFfdnHistoryDecision::Keep;
    return referenceValid && executionFlag13 && hasPrevious && allocatedPitch==previousPitch
        ? TglOwnedFfdnHistoryDecision::CopyPrevious : TglOwnedFfdnHistoryDecision::Invalidate;
}
// ICL1f80b6..1f81a9: consume the decision in native order. The copy's
// return register is not tested at1f819c; require a void callback rather
// than inventing an error return contract. Caller supplies leased child1b8
// and previous sourcec8 to that callback, never the newly allocated slot.
template<class Copy>
TglOwnedFfdnHistoryDecision tglApplyOwnedFfdnHistory(bool changed,bool& referenceValid,
    bool executionFlag13,bool hasPrevious,uint32_t allocatedPitch,uint32_t previousPitch,
    Copy copy) noexcept {
    static_assert(noexcept(copy()) && std::is_same_v<decltype(copy()),void>,"FFDN history copy ABI");
    const auto decision=tglOwnedFfdnHistoryDecision(changed,referenceValid,executionFlag13,
        hasPrevious,allocatedPitch,previousPitch);
    if(decision==TglOwnedFfdnHistoryDecision::CopyPrevious) copy();
    else if(decision==TglOwnedFfdnHistoryDecision::Invalidate) referenceValid=false;
    return decision;
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
    TglOwnedSurfaceStorage localSurface_{};
    TglOwnedSurfaceStorage& surface_;
    TglOwnedResource<Capture> resource_;
    TglResourceKey effectiveKey_{};
    bool ready_=false,busy_=false,rectangleBusy_=false,admitted_=true;
public:
    explicit TglOwnedAllocatedSurface(NativeBackend& native) noexcept
        :capture_{native},surface_(localSurface_),resource_(capture_,surface_.resource) {}
    // The caller owns the complete empty surface shell and retains it through
    // this owner's destruction/retirement. Bind resource identity in place;
    // never adopt tokens or overwrite a foreign prefix/tail on failed admission.
    TglOwnedAllocatedSurface(NativeBackend& native,TglOwnedSurfaceStorage& empty) noexcept
        :capture_{native},surface_(empty),resource_(capture_,surface_.resource) {
        for(auto byte:surface_.prefix) if(byte) admitted_=false;
        for(auto byte:surface_.resource) if(byte) admitted_=false;
        for(auto byte:surface_.tail) if(byte) admitted_=false;
    }
    ~TglOwnedAllocatedSurface() {reset();}
    TglOwnedAllocatedSurface(const TglOwnedAllocatedSurface&)=delete;
    TglOwnedAllocatedSurface& operator=(const TglOwnedAllocatedSurface&)=delete;
    // Identity only: does not grant access or prove a ready/backed lease.
    const void* surfaceBinding() const noexcept {return &surface_;}
    int ensure(const TglResourceKey& key,bool* changed=nullptr) noexcept {
        // Native4e060 clears changed before admission/allocation. Only a
        // successfully published backed surface may trigger history rebuild.
        if(changed) *changed=false;
        if(!admitted_ || busy_) return 5;
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
    int ensureFfdiBacking(const TglOwnedFfdiAllocationRequest& request,bool& changed) noexcept {
        if(request.type!=1 || request.kind!=TglOwnedFfdiAllocationKind::NativeBacking ||
           request.key.format!=0x3d || capture_.native.resourceType()!=1) return 5;
        return ensure(request.key,&changed);
    }
    // FFDN's type1/format3d branch requires real backing. The allocator writes
    // surface e0=0 on changed allocation; preserve that exact shell value for
    // the subsequent native history comparison, not inferred IOSurface pitch.
    int ensureFfdnBacking(const TglOwnedFfdnAllocationRequest& request,
        bool& changed,uint32_t& pitch) noexcept {
        if(request.type!=1 || request.kind!=TglOwnedFfdiAllocationKind::NativeBacking ||
           request.key.format!=0x3d || capture_.native.resourceType()!=1) return 5;
        const int result=ensure(request.key,&changed);
        if(result) return result;
        std::memcpy(&pitch,surface_.prefix.data()+0xe0,4);return 0;
    }
    const TglOwnedSurfaceStorage* surface() const noexcept {return ready_ && !busy_ ? &surface_ : nullptr;}
    bool hasSurfaceLease(const TglOwnedSurfaceStorage& expected) noexcept {
        return ready_ && busy_ && &expected==&surface_ && capture_.backed(surface_.resource);
    }
    // Synchronous borrowed access keeps ALL native resource tokens alive by
    // pinning their owner, not by guessing which individual handles to retain.
    // The callback must not escape references or submit asynchronous work;
    // GPU/history retention still requires a completion-bound source lease.
    template<class Consume> bool withSurface(Consume&& consume) {
        if(!ready_ || busy_) return false;
        busy_=true;
        struct Unlock {bool& busy;~Unlock(){busy=false;}} unlock{busy_};
        if(!capture_.backed(surface_.resource)) return false;
        std::forward<Consume>(consume)(static_cast<const TglOwnedSurfaceStorage&>(surface_));
        return true;
    }
    // Called only inside this owner's already-active frame lease. Native160
    // may consume source90 via tracked436d0; the parameter owner reconciles
    // that exact release contract before the frame unlocks. No const-cast or
    // foreign shell/pointer adoption is exposed to the enclosing policy.
    template<class Parameters,class Use>
    bool withNativeDenoiseParameters(const TglOwnedSurfaceStorage& expected,
        Parameters& parameters,Use use) noexcept {
        static_assert(noexcept(use()),"native DN parameter callback ABI");
        if(!ready_ || !busy_ || &expected!=&surface_ ||
           !capture_.backed(surface_.resource)) return false;
        return parameters.withNativeReleaseSlot(surface_.prefix.data()+0x90,use);
    }
    // Synchronous non-consuming FFDN90 publication from the actual parameter
    // owner. Both source and CPU metadata are detached before its lease ends.
    // Not valid for native160 (which can consume/free DN parameters), nor GPU
    // work retaining either pointer after this callback.
    template<class Parameters,class Use>
    bool withBorrowedDenoiseMetadata(const TglOwnedSurfaceStorage& expected,
        Parameters& parameters,TglOwnedSurfaceStorage& metadata,Use use) noexcept {
        static_assert(noexcept(use(expected,static_cast<const TglOwnedSurfaceStorage&>(metadata))) &&
            std::is_same_v<decltype(use(expected,static_cast<const TglOwnedSurfaceStorage&>(metadata))),void>,
            "borrowed DN metadata ABI");
        if(!ready_ || !busy_ || &expected!=&surface_ || &metadata==&surface_ ||
           !capture_.backed(surface_.resource)) return false;
        uintptr_t sourcePointer=0,targetPointer=0;
        std::memcpy(&sourcePointer,surface_.prefix.data()+0x90,8);
        std::memcpy(&targetPointer,metadata.prefix.data()+0x90,8);
        if(sourcePointer || targetPointer) return false;
        bool consumed=false;
        const bool leased=parameters.withBorrowedAddress([&](uintptr_t pointer,size_t extent) noexcept {
            if(extent<8) return;
            auto* sourceSlot=surface_.prefix.data()+0x90;
            auto* targetSlot=metadata.prefix.data()+0x90;
            struct Restore {uint8_t* source;uint8_t* target;
                ~Restore(){std::memset(source,0,8);std::memset(target,0,8);}} restore{sourceSlot,targetSlot};
            std::memcpy(sourceSlot,&pointer,8);std::memcpy(targetSlot,&pointer,8);
            use(expected,static_cast<const TglOwnedSurfaceStorage&>(metadata));
            consumed=true;
        });
        return leased && consumed;
    }
    // Fast-fill recheck must observe the actual leased target, not a detached
    // rectangle copy. Only its proven +4c bottom is borrowed; retain every
    // resource token and restore original bytes before releasing the frame.
    // This does not bind compression callbacks or publish a TGL fast-fill bit.
    template<class Use>
    bool withTemporaryDestinationBottom(const TglOwnedSurfaceStorage& expected,
        float bottom,Use use) noexcept {
        static_assert(noexcept(use(expected)),"temporary rectangle callback must not throw");
        if(!ready_ || !busy_ || rectangleBusy_ || &expected!=&surface_ ||
           !std::isfinite(bottom) || !capture_.backed(surface_.resource)) return false;
        std::array<uint8_t,4> original{};
        std::memcpy(original.data(),surface_.prefix.data()+0x4c,4);
        rectangleBusy_=true;
        struct Restore {
            uint8_t* slot;std::array<uint8_t,4> original;bool& busy;
            ~Restore(){std::memcpy(slot,original.data(),4);busy=false;}
        } restore{surface_.prefix.data()+0x4c,original,rectangleBusy_};
        std::memcpy(surface_.prefix.data()+0x4c,&bottom,4);
        use(static_cast<const TglOwnedSurfaceStorage&>(surface_));return true;
    }
    // Native-backed FFDi metadata never adopts input resource tokens. Source
    // and optional execution surface stay pinned by their real owners; target
    // backing and unrelated bytes survive the scalar-only publication.
    template<class SourceOwner>
    bool publishFfdiFrameFrom(SourceOwner& owner,const TglOwnedSurfaceStorage& source,
        TglOwnedFfdiMetadataQuery query,SourceOwner* executionOwner=nullptr,
        const TglOwnedSurfaceStorage* execution=nullptr) noexcept {
        if(!ready_ || busy_ || &source==&surface_ || !owner.hasSurfaceLease(source) ||
           bool(executionOwner)!=bool(execution) || execution==&surface_ ||
           (execution && !executionOwner->hasSurfaceLease(*execution))) return false;
        busy_=true;
        struct Unlock {bool& busy;~Unlock(){busy=false;}} unlock{busy_};
        if(!capture_.backed(surface_.resource) ||
           !tglPrepareOwnedFfdiFrameMetadata(surface_,source,query)) return false;
        if(execution) tglCopyOwnedFfdiPrimaryScalars(surface_,*execution);
        return capture_.backed(surface_.resource);
    }
    template<class SourceOwner>
    bool publishFfdiSecondaryFrom(SourceOwner& owner,const TglOwnedSurfaceStorage& source) noexcept {
        if(!ready_ || busy_ || &source==&surface_ || !owner.hasSurfaceLease(source)) return false;
        busy_=true;
        struct Unlock {bool& busy;~Unlock(){busy=false;}} unlock{busy_};
        if(!capture_.backed(surface_.resource)) return false;
        tglCopyOwnedFfdiSecondaryMetadata(surface_,source);
        return capture_.backed(surface_.resource);
    }
    // Publish only the proven FFDN scalars, never source tokens/handles or
    // DN90. The actual source owner must remain leased through publication.
    template<class SourceOwner>
    bool publishFfdnFrameFrom(SourceOwner& owner,const TglOwnedSurfaceStorage& source) noexcept {
        if(!ready_ || busy_ || &source==&surface_ || !owner.hasSurfaceLease(source)) return false;
        busy_=true;
        struct Unlock {bool& busy;~Unlock(){busy=false;}} unlock{busy_};
        if(!capture_.backed(surface_.resource)) return false;
        return tglPrepareOwnedFfdnFrameMetadata(surface_,source);
    }
    // ICL1f839b..1f83d8: after the FFDN pair, pipe2 alone copies the
    // source's two rectangle qwords into execution60. No resource adoption.
    template<class SourceOwner>
    bool publishPipe2RectangleFrom(uint32_t pipe,SourceOwner& owner,
        const TglOwnedSurfaceStorage& source) noexcept {
        if(pipe!=2) return true; // native skip does not access either surface
        if(!ready_ || busy_ || &source==&surface_ || !owner.hasSurfaceLease(source)) return false;
        busy_=true;
        struct Unlock {bool& busy;~Unlock(){busy=false;}} unlock{busy_};
        if(!capture_.backed(surface_.resource)) return false;
        std::memcpy(surface_.prefix.data()+0x50,source.prefix.data()+0x50,16);
        return true;
    }
    bool copyFrameMetadata(const TglOwnedSurfaceStorage& source) noexcept {
        if(!ready_ || busy_) return false;
        busy_=true;
        struct Unlock {bool& busy;~Unlock(){busy=false;}} unlock{busy_};
        if(!capture_.backed(surface_.resource)) return false;
        tglCopyOwnedSurfaceFrameMetadata(surface_,source);return true;
    }
    // Prefer this over a naked source view: retain all source tokens through
    // the synchronous copy. No handle transfer or asynchronous history lease.
    bool copyFrameMetadataFrom(TglOwnedAllocatedSurface& source) noexcept {
        if(&source==this) return copyFrameMetadata(surface_);
        bool copied=false;
        const bool leased=source.withSurface([&](const TglOwnedSurfaceStorage& view) noexcept {
            copied=copyFrameMetadata(view);
        });
        return leased && copied;
    }
    // Pin two complete token owners for one synchronous producer/builder call.
    // Aliasing is admitted with one lease, never a recursive acquisition.
    // References must not escape; pending GPU/history ownership is separate.
    template<class Consume> bool withPeerSurface(TglOwnedAllocatedSurface& peer,
                                                Consume&& consume) {
        bool consumed=false;
        const bool first=withSurface([&](const TglOwnedSurfaceStorage& own) {
            if(&peer==this) {
                std::forward<Consume>(consume)(own,own);consumed=true;
            } else {
                consumed=peer.withSurface([&](const TglOwnedSurfaceStorage& other) {
                    std::forward<Consume>(consume)(own,other);
                });
            }
        });
        return first && consumed;
    }
    void reset() noexcept {
        if(!admitted_ || busy_) return;
        busy_=true;ready_=false;resource_.reset();
        surface_.prefix.fill(0);surface_.tail.fill(0);busy_=false;
        effectiveKey_={};
    }
};
// Borrow the source owners behind FFDI/FFDN roles without copying their native
// descriptors. A role may alias another role; acquire each unique owner once.
// Null roles stay null, but all nonnull roles must have live backing. This is
// a synchronous frame scope, NOT a retained previous-frame/GPU fence lease.
template<class Owner,size_t Count,class Invoke>
int tglWithCapturedOwnedSurfaceRoles(const std::array<Owner*,Count>& owners,
    const std::array<const TglOwnedSurfaceStorage*,Count>& captured,Invoke invoke) noexcept {
    static_assert(Count>0 && Count<=16,"bounded surface role set");
    using Views=std::array<const TglOwnedSurfaceStorage*,Count>;
    static_assert(noexcept(invoke(std::declval<const Views&>())),"surface role borrow ABI");
    Views seeds=captured,views{};
    for(size_t i=0;i<Count;++i) {
        if(seeds[i] && (!owners[i] || !owners[i]->hasSurfaceLease(*seeds[i]))) return 5;
        for(size_t j=0;j<i;++j)
            if(owners[i] && owners[i]==owners[j] && seeds[i] && seeds[j] && seeds[i]!=seeds[j]) return 5;
    }
    for(size_t i=0;i<Count;++i) if(owners[i] && !seeds[i])
        for(size_t j=0;j<Count;++j) if(owners[i]==owners[j] && seeds[j]) {seeds[i]=seeds[j];break;}
    const auto valid=[&]() noexcept {
        for(size_t i=0;i<Count;++i) if(seeds[i] && !owners[i]->hasSurfaceLease(*seeds[i])) return false;
        return true;
    };
    const auto enter=[&](const auto& self,size_t index) noexcept -> int {
        if(index==Count) {
            if(!valid()) return 5;
            const int result=invoke(static_cast<const Views&>(views));
            return valid()?result:5;
        }
        if(!owners[index]) return self(self,index+1);
        if(seeds[index]) {
            views[index]=seeds[index];const int result=self(self,index+1);views[index]=nullptr;return result;
        }
        for(size_t previous=0;previous<index;++previous) {
            if(owners[previous]!=owners[index]) continue;
            views[index]=views[previous];
            const int result=self(self,index+1);views[index]=nullptr;return result;
        }
        int result=5;
        const bool leased=owners[index]->withSurface([&](const TglOwnedSurfaceStorage& surface) noexcept {
            views[index]=&surface;
            result=self(self,index+1);views[index]=nullptr;
        });
        return leased ? result : 5;
    };
    return enter(enter,0);
}
template<class Owner,size_t Count,class Invoke>
int tglWithOwnedSurfaceRoles(const std::array<Owner*,Count>& owners,Invoke invoke) noexcept {
    return tglWithCapturedOwnedSurfaceRoles(owners,
        std::array<const TglOwnedSurfaceStorage*,Count>{},invoke);
}

// Hold the real source owner across request/metadata preparation and its
// consumer. Metadata is CPU-only and contains no borrowed resource tokens;
// only sourceView may supply backing under this lease. Neither view can be
// retained past the callback or treated as asynchronous GPU ownership.
template<class Owner,class Use>
int tglWithOwnedFfdiPreparation(Owner& source,uint32_t format,uint32_t tile,
    TglOwnedFfdiCompressionSelection compression,TglOwnedFfdiMetadataQuery query,Use use) noexcept {
    static_assert(noexcept(use(std::declval<const TglOwnedSurfaceStorage&>(),
        std::declval<const TglOwnedSurfaceStorage&>(),
        std::declval<const TglOwnedFfdiAllocationRequest&>())),"leased FFDI preparation ABI");
    int result=5;
    const bool leased=source.withSurface([&](const TglOwnedSurfaceStorage& sourceView) noexcept {
        TglOwnedFfdiAllocationRequest request;
        TglOwnedSurfaceStorage metadata;
        if(!tglPrepareOwnedFfdiAllocationRequest(sourceView,format,tile,compression,request) ||
           !tglPrepareOwnedFfdiFrameMetadata(metadata,sourceView,query)) return;
        result=use(sourceView,static_cast<const TglOwnedSurfaceStorage&>(metadata),
            static_cast<const TglOwnedFfdiAllocationRequest&>(request));
    });
    return leased?result:5;
}

// Keep allocation and its post-success metadata consumer under the source
// lease. Allocate must implement the request kind; success alone is not proof
// of GPU backing. The consumer owns publication/retirement, not this scope.
template<class Owner,class Allocate,class Use>
int tglWithAllocatedOwnedFfdiRoles(Owner& source,Owner* execution,uint32_t format,uint32_t tile,
    TglOwnedFfdiCompressionSelection compression,TglOwnedFfdiMetadataQuery query,
    uint8_t execution13,uint8_t execution14,Allocate allocate,Use use) noexcept {
    static_assert(noexcept(allocate(std::declval<const TglOwnedFfdiAllocationRequest&>(),
        std::declval<bool&>())),"FFDI allocation ABI");
    static_assert(noexcept(use(std::declval<const TglOwnedSurfaceStorage&>(),
        static_cast<const TglOwnedSurfaceStorage*>(nullptr),
        std::declval<const TglOwnedSurfaceStorage&>(),uint8_t{},uint8_t{})),"FFDI publication ABI");
    // Native source60 is optional. Keep its actual token owner, including
    // aliases, pinned across allocation/use; never copy its borrowed handle
    // into CPU metadata or pretend this scope covers asynchronous submission.
    const std::array<Owner*,2> roles{&source,execution};
    return tglWithOwnedSurfaceRoles(roles,[&](const auto& views) noexcept {
            TglOwnedFfdiAllocationRequest request;
            TglOwnedSurfaceStorage metadata;
            if(!tglPrepareOwnedFfdiAllocationRequest(*views[0],format,tile,compression,request) ||
               !tglPrepareOwnedFfdiFrameMetadata(metadata,*views[0],query)) return 5;
            bool changed=false;
            const int result=allocate(request,changed);
            if(result!=0) return result;
            if(views[1]) tglCopyOwnedFfdiPrimaryScalars(metadata,*views[1]);
            tglUpdateOwnedFfdiExecutionCompression(changed,compression,execution13,execution14);
            return use(*views[0],views[1],static_cast<const TglOwnedSurfaceStorage&>(metadata),
                execution13,execution14);
        });
}
template<class Owner,class Allocate,class Use>
int tglWithAllocatedOwnedFfdiPreparation(Owner& source,uint32_t format,uint32_t tile,
    TglOwnedFfdiCompressionSelection compression,TglOwnedFfdiMetadataQuery query,
    uint8_t execution13,uint8_t execution14,Allocate allocate,Use use) noexcept {
    static_assert(noexcept(use(std::declval<const TglOwnedSurfaceStorage&>(),
        std::declval<const TglOwnedSurfaceStorage&>(),uint8_t{},uint8_t{})),"FFDI publication ABI");
    return tglWithAllocatedOwnedFfdiRoles(source,static_cast<Owner*>(nullptr),format,tile,
        compression,query,execution13,execution14,allocate,
        [&](const auto& input,const auto*,const auto& metadata,uint8_t flag,uint8_t mode) noexcept {
            return use(input,metadata,flag,mode);
        });
}

// Bounded owned FFDI assembly: all three input roles remain pinned through
// the native-ordered allocation loop and final consumer. This is not the
// native zero-count/reused-slot path or an asynchronous submission lease.
// A failed allocation leaves earlier allocator-owned slots intact for the
// caller's real retirement policy; no fabricated rollback or handle copies.
struct TglOwnedFfdiNoReportPublication {
    int operator()(uint8_t,uint8_t) const noexcept {return 0;}
};
template<size_t Capacity,class Owner,class Allocate,class Use,class Publish=TglOwnedFfdiNoReportPublication>
int tglWithAllocatedOwnedFfdiBatch(Owner& input,Owner* execution,Owner* secondary,
    size_t count,uint32_t format,uint32_t tile,TglOwnedFfdiCompressionSelection compression,
    TglOwnedFfdiMetadataQuery query,uint8_t flag,uint8_t mode,Allocate allocate,Use use,
    std::array<const TglOwnedSurfaceStorage*,3> captured={},Publish publish={}) noexcept {
    static_assert(Capacity>0 && Capacity<=4,"owned FFDI slot capacity");
    using Metadata=std::array<TglOwnedSurfaceStorage,Capacity>;
    using Views=std::array<const TglOwnedSurfaceStorage*,3>;
    static_assert(noexcept(allocate(size_t{},std::declval<const TglOwnedFfdiAllocationRequest&>(),
        std::declval<bool&>())),"FFDI batch allocation ABI");
    static_assert(noexcept(use(std::declval<const Views&>(),std::declval<const Metadata&>(),
        size_t{},uint8_t{},uint8_t{})),"FFDI batch consumer ABI");
    static_assert(noexcept(publish(uint8_t{},uint8_t{})) &&
        std::is_same_v<decltype(publish(uint8_t{},uint8_t{})),int>,"FFDI report publisher ABI");
    if(count==0 || count>Capacity) return 5;
    const std::array<Owner*,3> roles{&input,execution,secondary};
    return tglWithCapturedOwnedSurfaceRoles(roles,captured,[&](const Views& views) noexcept {
        TglOwnedFfdiAllocationRequest request;
        if(!tglPrepareOwnedFfdiAllocationRequest(*views[0],format,tile,compression,request)) return 5;
        Metadata metadata{};
        for(size_t index=0;index<count;++index) {
            bool changed=false;
            const int result=allocate(index,request,changed);
            if(result!=0) return result;
            if(!tglPrepareOwnedFfdiFrameMetadata(metadata[index],*views[0],query)) return 5;
            if(views[1]) tglCopyOwnedFfdiPrimaryScalars(metadata[index],*views[1]);
            tglUpdateOwnedFfdiExecutionCompression(changed,compression,flag,mode);
            // Native1f7e0c..1f7e3d publishes each changed slot before the
            // next allocation. A later failure must not erase that update.
            // The default preserves legacy CPU-only consumers; runtime must
            // supply its held execution-report owner, not defer to final use.
            if(changed) {
                const int published=publish(flag,mode);
                if(published) return published;
            }
        }
        if(views[2]) tglCopyOwnedFfdiSecondaryMetadata(metadata[0],*views[2]);
        return use(views,static_cast<const Metadata&>(metadata),count,flag,mode);
    });
}

// Concrete descriptor-only FFDI target assembly. Keep every output descriptor
// leased through the final consumer, alongside the existing input role leases.
// A nonnull execution role must be a leased type1 source; its handle1a0 is
// borrowed only through the final consumer. No submission/fence is implied.
template<size_t Count,class Owner,class Use,class Publish=TglOwnedFfdiNoReportPublication>
int tglWithOwnedDescriptorFfdiBatch(std::array<TglOwnedDescriptorOnlySurface,Count>& targets,
    Owner& input,Owner* execution,Owner* secondary,size_t count,uint32_t format,uint32_t tile,
    TglOwnedFfdiCompressionSelection compression,TglOwnedFfdiMetadataQuery query,
    uint8_t flag,uint8_t mode,Use use,std::array<const TglOwnedSurfaceStorage*,3> captured={},
    Publish publish={}) noexcept {
    using Outputs=std::array<const TglOwnedSurfaceStorage*,Count>;
    using Inputs=std::array<const TglOwnedSurfaceStorage*,3>;
    static_assert(noexcept(use(std::declval<const Inputs&>(),std::declval<const Outputs&>(),
        size_t{},uint8_t{},uint8_t{})),"descriptor FFDI consumer ABI");
    static_assert(Count>0 && Count<=4,"owned descriptor FFDi capacity");
    static_assert(noexcept(publish(uint8_t{},uint8_t{})) &&
        std::is_same_v<decltype(publish(uint8_t{},uint8_t{})),int>,"FFDi report publisher ABI");
    if(!count || count>Count) return 5;
    return tglWithCapturedOwnedSurfaceRoles(std::array<Owner*,3>{&input,execution,secondary},captured,
        [&](const Inputs& inputs) noexcept {
            TglOwnedFfdiAllocationRequest request;
            if(!tglPrepareOwnedFfdiAllocationRequest(*inputs[0],format,tile,compression,request)) return 5;
            for(size_t index=0;index<count;++index) {
                bool changed=false;
                if(!targets[index].prepare(request,changed) ||
                   !targets[index].publishFfdiFrame(*inputs[0],query,inputs[1])) return 5;
                tglUpdateOwnedFfdiExecutionCompression(changed,compression,flag,mode);
                if(changed) {const int status=publish(flag,mode);if(status) return status;}
            }
            // Native1f7e52 copies secondary only after the complete FFDi loop.
            if(inputs[2] && !targets[0].publishFfdiSecondaryFrom(*secondary,*inputs[2])) return 5;
            Outputs outputs{};
            const auto enter=[&](auto&& self,size_t index) noexcept -> int {
                if(index==count) return use(inputs,static_cast<const Outputs&>(outputs),count,flag,mode);
                int result=5;
                const auto consume=[&](const auto& descriptor) noexcept {
                    outputs[index]=&descriptor;result=self(self,index+1);outputs[index]=nullptr;
                };
                const bool leased=inputs[1]
                    ?targets[index].withBorrowedExecutionHandle(*execution,*inputs[1],consume)
                    :targets[index].withDescriptor(consume);
                return leased?result:5;
            };
            return enter(enter,0);
        });
}

// Concrete execution-owner composition. Caller already holds the native
// report/frame lease; snapshot13/14 through its bounded byte view, then
// publish changed slots through that same owner, never a final-only copy.
template<size_t Count,class Owner,class ExecutionOwner,class Use>
int tglWithOwnedDescriptorFfdiReportBatch(std::array<TglOwnedDescriptorOnlySurface,Count>& targets,
    Owner& input,Owner* executionSurface,Owner* secondary,size_t count,
    uint32_t format,uint32_t tile,TglOwnedFfdiCompressionSelection compression,
    TglOwnedFfdiMetadataQuery query,ExecutionOwner& reportOwner,uintptr_t report,Use use,
    std::array<const TglOwnedSurfaceStorage*,3> captured={}) noexcept {
    std::array<uint8_t,2> original{};
    const int read=reportOwner.withLeasedAdmissionBytes(report,[&](auto& view) noexcept {
        static_assert(TglAdmissionExecutionExtent<std::decay_t<decltype(view)>>::value>=0x15,
            "FFDi report snapshot extent");
        std::memcpy(original.data(),view.data()+0x13,original.size());return 0;
    });
    if(read) return read;
    return tglWithOwnedDescriptorFfdiBatch(targets,input,executionSurface,secondary,count,
        format,tile,compression,query,original[0],original[1],use,captured,
        [&](uint8_t flag,uint8_t mode) noexcept {
            return tglPublishLeasedFfdiCompression(reportOwner,report,flag,mode);
        });
}
// Native-backed counterpart of the descriptor batch. Pin every input role
// across allocation/metadata/report and every output across the final use.
// This is synchronous CPU lifetime only, not a GPU completion lease.
template<size_t Count,class TargetOwner,class Owner,class Publish,class Use>
int tglWithOwnedBackedFfdiBatch(const std::array<TargetOwner*,Count>& targets,
    Owner& input,Owner* execution,Owner* secondary,size_t count,
    const TglOwnedFfdiAllocationRequest& request,TglOwnedFfdiMetadataQuery query,
    uint8_t flag,uint8_t mode,Publish publish,Use use,
    std::array<const TglOwnedSurfaceStorage*,3> captured={}) noexcept {
    static_assert(Count>0 && Count<=4,"FFDi backing capacity");
    static_assert(noexcept(publish(uint8_t{},uint8_t{})) &&
        std::is_same_v<decltype(publish(uint8_t{},uint8_t{})),int>,"FFDi publisher ABI");
    if(!count || count>Count || request.kind!=TglOwnedFfdiAllocationKind::NativeBacking ||
       request.type!=1 || request.key.format!=0x3d) return 5;
    const std::array<Owner*,3> roles{&input,execution,secondary};
    for(size_t i=0;i<roles.size();++i) for(size_t j=0;j<i;++j)
        if(roles[i] && roles[j] && roles[i]!=roles[j] &&
           roles[i]->surfaceBinding()==roles[j]->surfaceBinding()) return 5;
    for(size_t i=0;i<count;++i) {
        if(!targets[i]) return 5;
        for(size_t j=0;j<i;++j)
            if(targets[i]==targets[j] || targets[i]->surfaceBinding()==targets[j]->surfaceBinding()) return 5;
        for(auto* role:roles) if(role &&
            (static_cast<const void*>(targets[i])==static_cast<const void*>(role) ||
             targets[i]->surfaceBinding()==role->surfaceBinding())) return 5;
    }
    return tglWithCapturedOwnedSurfaceRoles(roles,captured,[&](const auto& inputs) noexcept {
        uint32_t width=0,height=0;
        std::memcpy(&width,inputs[0]->prefix.data()+0xd8,4);
        std::memcpy(&height,inputs[0]->prefix.data()+0xdc,4);
        if(request.key.width!=width || request.key.height!=height) return 5;
        for(size_t i=0;i<count;++i) {
            bool changed=false;
            const int result=targets[i]->ensureFfdiBacking(request,changed);
            if(result) return result;
            if(!targets[i]->publishFfdiFrameFrom(input,*inputs[0],query,execution,inputs[1])) return 5;
            tglUpdateOwnedFfdiExecutionCompression(changed,
                {request.key.compressed,request.key.compressionMode},flag,mode);
            if(changed) {const int status=publish(flag,mode);if(status) return status;}
        }
        if(inputs[2] && !targets[0]->publishFfdiSecondaryFrom(*secondary,*inputs[2])) return 5;
        std::array<const TglOwnedSurfaceStorage*,Count> outputs{};
        const auto enter=[&](auto&& self,size_t i) noexcept -> int {
            if(i==count) {
                static_assert(noexcept(use(inputs,outputs,count,flag,mode)) &&
                    std::is_same_v<decltype(use(inputs,outputs,count,flag,mode)),int>,"FFDi backed consumer ABI");
                return use(inputs,static_cast<const decltype(outputs)&>(outputs),count,flag,mode);
            }
            int result=5;
            const bool leased=targets[i]->withSurface([&](const auto& output) noexcept {
                outputs[i]=&output;result=self(self,i+1);outputs[i]=nullptr;
            });
            return leased?result:5;
        };
        return enter(enter,0);
    });
}
// Allocation-disabled 1f7b06 -> 1f7e52 edge for an existing descriptor slot0.
// Also reached when signed base1b5c is nonpositive (1f7b48 ->1f7e50).
// Pin the secondary and destination through the synchronous next stage;
// do not call prepare, copy the primary frame, or invent execution flags.
template<class Owner,class Use>
int tglWithOwnedDescriptorFfdiSecondary(TglOwnedDescriptorOnlySurface& target,
    Owner& secondary,Use use,const TglOwnedSurfaceStorage* captured=nullptr) noexcept {
    static_assert(noexcept(use(std::declval<const TglOwnedSurfaceStorage&>(),
        std::declval<const TglOwnedSurfaceStorage&>())),"FFDI secondary consumer ABI");
    return tglWithCapturedOwnedSurfaceRoles(std::array<Owner*,1>{&secondary},
        std::array<const TglOwnedSurfaceStorage*,1>{captured},[&](const auto& views) noexcept {
        const auto& source=*views[0];
        if(!target.publishFfdiSecondaryFrom(secondary,source)) return 5;
        int result=5;
        target.withDescriptor([&](const auto& output) noexcept {result=use(source,output);});
        return result;
    });
}

// Same secondary-only edge when slot0 already has native backing. The caller
// must select the actual slot owner, not infer it from a new frame's format.
// No ensure/allocate/release: retain the existing backing and borrow both
// surfaces until the synchronous consumer returns.
template<class TargetOwner,class Owner,class Use>
int tglWithOwnedBackedFfdiSecondary(TargetOwner& target,Owner& secondary,
    Use use,const TglOwnedSurfaceStorage* captured=nullptr) noexcept {
    static_assert(noexcept(use(std::declval<const TglOwnedSurfaceStorage&>(),
        std::declval<const TglOwnedSurfaceStorage&>())) &&
        std::is_same_v<decltype(use(std::declval<const TglOwnedSurfaceStorage&>(),
        std::declval<const TglOwnedSurfaceStorage&>())),int>,"FFDi backed secondary ABI");
    return tglWithCapturedOwnedSurfaceRoles(std::array<Owner*,1>{&secondary},
        std::array<const TglOwnedSurfaceStorage*,1>{captured},[&](const auto& views) noexcept {
        const auto& source=*views[0];
        if(!target.publishFfdiSecondaryFrom(secondary,source)) return 5;
        int result=5;
        const bool leased=target.withSurface([&](const auto& output) noexcept {
            result=use(source,output);
        });
        return leased?result:5;
    });
}

// Two FFDN allocations (1f7fd1), each followed by history handling and
// per-slot metadata publication. Earlier successful allocations/publications
// are not rolled back on a later failure. Copy destination is child1b8,
// NOT the allocated ba8[index]. All borrowed roles remain synchronous leases.
struct TglOwnedFfdnNoFinish {
    template<class... Views> int operator()(const Views&...) const noexcept {return 0;}
};
template<class Owner,class Allocate,class Copy,class Publish,class Finish=TglOwnedFfdnNoFinish,
    class Compression=TglOwnedFfdnCompressionSelection>
int tglWithAllocatedOwnedFfdnPair(Owner& input,Owner* previous,Owner* copyDestination,
    Compression selection,bool& referenceValid,bool executionFlag13,
    uint8_t& execution15,uint8_t& execution16,Allocate allocate,Copy copy,Publish publish,
    Finish finish={},std::array<const TglOwnedSurfaceStorage*,3> captured={}) noexcept {
    static_assert(noexcept(allocate(size_t{},std::declval<const TglOwnedFfdnAllocationRequest&>(),
        std::declval<bool&>(),std::declval<uint32_t&>())),"FFDN pair allocation ABI");
    static_assert(noexcept(copy(std::declval<const TglOwnedSurfaceStorage&>(),
        std::declval<const TglOwnedSurfaceStorage&>())) &&
        std::is_same_v<decltype(copy(std::declval<const TglOwnedSurfaceStorage&>(),
        std::declval<const TglOwnedSurfaceStorage&>())),void>,"FFDN pair copy ABI");
    using PublicationResult=decltype(publish(size_t{},std::declval<const TglOwnedSurfaceStorage&>(),
        std::declval<const TglOwnedSurfaceStorage&>()));
    static_assert(noexcept(publish(size_t{},std::declval<const TglOwnedSurfaceStorage&>(),
        std::declval<const TglOwnedSurfaceStorage&>())) &&
        (std::is_same_v<PublicationResult,void> || std::is_same_v<PublicationResult,int>),
        "FFDN pair metadata ABI");
    const std::array<Owner*,3> roles{&input,previous,copyDestination};
    return tglWithCapturedOwnedSurfaceRoles(roles,captured,[&](const auto& views) noexcept {
        TglOwnedFfdnCompressionSelection compression{};
        if constexpr(std::is_same_v<Compression,TglOwnedFfdnCompressionSelection>) compression=selection;
        else {
            static_assert(noexcept(selection(views,compression)) &&
                std::is_same_v<decltype(selection(views,compression)),bool>,"leased FFDN selection ABI");
            if(!selection(views,compression)) return 5;
        }
        TglOwnedFfdnAllocationRequest request;
        if(!tglPrepareOwnedFfdnAllocationRequest(*views[0],compression,request)) return 5;
        uint32_t previousPitch=0;
        if(views[1]) std::memcpy(&previousPitch,views[1]->prefix.data()+0xe0,4);
        for(size_t index=0;index<2;++index) {
            bool changed=false;uint32_t pitch=0;
            const int result=allocate(index,request,changed,pitch);
            if(result!=0) return result;
            const auto decision=tglOwnedFfdnHistoryDecision(changed,referenceValid,executionFlag13,
                views[1]!=nullptr,pitch,previousPitch);
            if(decision==TglOwnedFfdnHistoryDecision::CopyPrevious && !views[2]) return 5;
            tglApplyOwnedFfdnHistory(changed,referenceValid,executionFlag13,views[1]!=nullptr,pitch,
                previousPitch,[&]() noexcept {copy(*views[2],*views[1]);});
            TglOwnedSurfaceStorage metadata;
            if(!tglPrepareOwnedFfdnFrameMetadata(metadata,*views[0])) return 5;
            if constexpr(std::is_same_v<PublicationResult,int>) {
                const int published=publish(index,*views[0],static_cast<const TglOwnedSurfaceStorage&>(metadata));
                if(published) return published;
            } else publish(index,*views[0],static_cast<const TglOwnedSurfaceStorage&>(metadata));
            //1f8355..1f8386: after metadata publication, only changed slots
            // update both whole bytes. Preserve previous successful updates
            // if the next allocation fails; unchanged does not restore them.
            if(changed) {
                execution15=compression.compressed?1u:0u;
                execution16=static_cast<uint8_t>(compression.mode);
            }
        }
        static_assert(noexcept(finish(views)) && std::is_same_v<decltype(finish(views)),int>,
            "FFDN pair final consumer ABI");
        return finish(views);
    });
}

// Persistent descriptor-only FFDN targets. Native-backed format3d remains
// owned by the separate resource path. DN90 is deliberately not retained;
// this preparation is not asynchronous command admission or retirement.
template<class Owner,class Copy,class Finish=TglOwnedFfdnNoFinish,
    class Compression=TglOwnedFfdnCompressionSelection>
int tglPrepareOwnedDescriptorFfdnPair(std::array<TglOwnedDescriptorOnlySurface,2>& targets,
    Owner& input,Owner* previous,Owner* copyDestination,
    Compression compression,bool& referenceValid,bool executionFlag13,
    uint8_t& execution15,uint8_t& execution16,Copy copy,Finish finish={},
    std::array<const TglOwnedSurfaceStorage*,3> captured={}) noexcept {
    return tglWithAllocatedOwnedFfdnPair(input,previous,copyDestination,compression,
        referenceValid,executionFlag13,execution15,execution16,
        [&](size_t index,const auto& request,bool& changed,uint32_t& pitch) noexcept -> int {
            if(!targets[index].prepare(request,changed)) return 5;
            pitch=0; // exact descriptor-only allocator branch
            return 0;
        },copy,[&](size_t index,const auto& source,const auto&) noexcept -> int {
            return targets[index].publishFfdnFrame(source)?0:5;
        },[&](const auto& inputs) noexcept -> int {
            int result=5;
            const bool first=targets[0].withDescriptor([&](const auto& a) noexcept {
                const bool second=targets[1].withDescriptor([&](const auto& b) noexcept {
                    static_assert(noexcept(finish(inputs,a,b)) &&
                        std::is_same_v<decltype(finish(inputs,a,b)),int>,"descriptor FFDN frame consumer ABI");
                    result=finish(inputs,a,b);
                });
                if(!second) result=5;
            });
            return first?result:5;
        },captured);
}

// Real type1/format3d target owners. Keep target roles distinct from every
// source/history role; source roles are already leased by the pair producer.
// No rollback of an earlier successful target on later allocation failure.
template<class TargetOwner,class Owner,class Copy,class Finish=TglOwnedFfdnNoFinish,
    class Compression=TglOwnedFfdnCompressionSelection>
int tglPrepareOwnedBackedFfdnPair(const std::array<TargetOwner*,2>& targets,
    Owner& input,Owner* previous,Owner* copyDestination,
    Compression compression,bool& referenceValid,bool executionFlag13,
    uint8_t& execution15,uint8_t& execution16,Copy copy,Finish finish={},
    std::array<const TglOwnedSurfaceStorage*,3> captured={}) noexcept {
    if(!targets[0] || !targets[1] || targets[0]==targets[1]) return 5;
    struct Binding {const void* owner;const void* storage;};
    std::array<Binding,5> bindings{};
    bindings[0]={targets[0],targets[0]->surfaceBinding()};
    bindings[1]={targets[1],targets[1]->surfaceBinding()};
    const std::array<Owner*,3> roles{&input,previous,copyDestination};
    for(size_t i=0;i<roles.size();++i)
        if(roles[i]) bindings[i+2]={roles[i],roles[i]->surfaceBinding()};
    for(size_t i=0;i<bindings.size();++i) for(size_t j=0;j<i;++j)
        if(bindings[i].storage && bindings[i].storage==bindings[j].storage &&
           bindings[i].owner!=bindings[j].owner) return 5;
    for(auto* target:targets)
        for(auto* role:roles)
            if(static_cast<const void*>(target)==static_cast<const void*>(role)) return 5;
    return tglWithAllocatedOwnedFfdnPair(input,previous,copyDestination,compression,
        referenceValid,executionFlag13,execution15,execution16,
        [&](size_t index,const auto& request,bool& changed,uint32_t& pitch) noexcept {
            return targets[index]->ensureFfdnBacking(request,changed,pitch);
        },copy,[&](size_t index,const auto& source,const auto&) noexcept -> int {
            return targets[index]->publishFfdnFrameFrom(input,source)?0:5;
        },[&](const auto& inputs) noexcept -> int {
            int result=5;
            const bool first=targets[0]->withSurface([&](const auto& a) noexcept {
                const bool second=targets[1]->withSurface([&](const auto& b) noexcept {
                    static_assert(noexcept(finish(inputs,a,b)) &&
                        std::is_same_v<decltype(finish(inputs,a,b)),int>,"backed FFDN frame consumer ABI");
                    result=finish(inputs,a,b);
                });
                if(!second) result=5;
            });
            return first?result:5;
        },captured);
}

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
// Owned VP +80/+88 ABI, from exact ICL1f7330/1df350. +80 receives
// both formats but consumes only the input format; +88 has no second format.
// Resolve must admit the currently leased owned VP, never a donor this thunk.
// Keep native write order, including aliases: 422 writes width=2 LAST, so
// a shared width/height pointer ends at2 rather than a synthesized height1.
template<class Resolve> struct TglOwnedVpAlignmentCallbacks {
    static void input(void* child,uint32_t format,uint32_t,
        uint16_t* width,uint16_t* height) noexcept {
        static_assert(noexcept(Resolve{}(child)),"VP alignment resolver ABI");
        auto* context=Resolve{}(child);
        static_assert(noexcept(context->owns(child)),"VP alignment ownership ABI");
        if(!context || !context->owns(child) || !width || !height) return;
        const auto alignment=tglOwnedSfcAlignment(format);
        *width=1;*height=1;
        if(alignment.width==2) *width=2;
        if(alignment.height==2) *height=2;
    }
    static void output(void* child,uint32_t format,uint16_t* width,uint16_t* height) noexcept {
        input(child,format,0,width,height);
    }
};

// Exact Darwin chroma producer1fa603..673. Changes owned surface metadata
// only, not OS backing or GPU bytes. Color-pack classification is supplied
// by the qualified format classifier:0=420,1=422,2=444,3=other.
inline bool tglNormalizeOwnedChromaSitingValue(uint32_t original,uint32_t colorPack,
                                             uint32_t& output) noexcept {
    if (colorPack > 3) return false;
    uint32_t siting=original;
    if (!siting) siting=0x21;
    if (colorPack == 1) siting=(siting&7)|0x10;
    else if (colorPack == 2) siting=0x11;
    output=siting;
    return true;
}
inline bool tglNormalizeOwnedChromaSiting(TglOwnedSurfaceStorage& surface,
                                        uint32_t colorPack) noexcept {
    uint32_t original=0,siting=0;
    std::memcpy(&original,surface.tail.data()+4,4);
    if(!tglNormalizeOwnedChromaSitingValue(original,colorPack,siting)) return false;
    std::memcpy(surface.tail.data()+4,&siting,4);
    return true;
}
// VP +28 1ded25..1def21. Target metadata is returned as owned data, never
// written through a borrowed target pointer. Pack inputs require qualification
// by the native format classifier. The later +9b8 predicate is separate below.
struct TglOwnedVpColorMetadata {
    uint32_t at9a4=0,at9a8=0,target294=0;
};
inline bool tglOwnedVpForceColor(uint32_t format99c,bool settings12) noexcept {
    return settings12 && ((format99c>=1 && format99c<=7) ||
        (format99c>=0xa && format99c<=0xc) || format99c==0x50 ||
        format99c==0x51 || format99c==uint32_t(-8) ||
        format99c==uint32_t(-9) || format99c==0x55 || format99c==0x5a);
}
struct TglOwnedVpModeState {
    uint32_t at998=0,at99c=0;
    bool at923=false;
};
struct TglOwnedVpModeInput {
    uint32_t source00=0,target00=0,sourceD8=0,sourceDc=0,source130=0;
    uint32_t settings60130=0;
    bool settings12=false,settings13=false,settings60Present=false,sourceA8Present=false;
};
struct TglOwnedVpModeResult {
    TglOwnedVpModeState state;
    // A borrow directive, not a saved pointer: the caller must scope the exact
    // source+A8 reference separately. False means preserve +9c0, not clear it.
    bool borrowSourceA8=false;
};
// VP +b0=1de510..64f, then +b8=1de650..808. These mutate a candidate
// inherited state; native +923 is sticky and +9c0 is only conditionally written.
// Slot+b0=1de510..64f, called at1dec81 with source/target only.
inline void tglPrepareOwnedVpSampleMode(const TglOwnedVpModeInput& in,
    const TglOwnedVpModeState& original,TglOwnedVpModeResult& output) noexcept {
    TglOwnedVpModeResult candidate{original,false};
    if(in.source00==1 || in.source00==2) {
        if(in.target00>=3 && in.target00<=0xa) candidate.state.at998=in.target00;
        else candidate.state.at998=(in.sourceD8>0x300 || in.sourceDc>0x240)?5:3;
    } else candidate.state.at998=in.settings12?in.target00:in.source00;
    if(candidate.state.at998!=in.target00) {
        candidate.state.at923=true;
        candidate.borrowSourceA8=in.sourceA8Present;
    }
    output=candidate;
}
// Slot+b8=1de650..808, called at1dec96 with source/settings only.
// Preserve the preceding b0 result, including its conditional borrow directive.
inline bool tglPrepareOwnedVpFormatMode(const TglOwnedVpModeInput& in,
    const TglOwnedVpModeResult& original,TglOwnedVpModeResult& output) noexcept {
    auto candidate=original;
    if(tglOwnedVpForceColor(in.source130,true)) candidate.state.at99c=0x15;
    else if(in.settings13) candidate.state.at99c=0xd;
    else if(in.settings12) {
        if(!in.settings60Present) return false;
        if(in.settings60130==0x51) candidate.state.at99c=0x50;
        else if(in.settings60130==1) {
            candidate.state.at99c=3;candidate.state.at923=true;
        } else candidate.state.at99c=in.settings60130;
    } else candidate.state.at99c=in.source130;
    output=candidate;
    return true;
}
inline bool tglPrepareOwnedVpModes(const TglOwnedVpModeInput& in,
    const TglOwnedVpModeState& original,TglOwnedVpModeResult& output) noexcept {
    TglOwnedVpModeResult sample;
    tglPrepareOwnedVpSampleMode(in,original,sample);
    return tglPrepareOwnedVpFormatMode(in,sample,output);
}
// Both native callers ignore the result; allocation/admission failure must be
// retained by the enclosing owned context, not invented as an int callback ABI.
template<class Resolve> struct TglOwnedVpModeCallbacks {
    static void sampleMode(void* child,void* source,void* target) noexcept {
        static_assert(noexcept(Resolve{}(child)),"VP mode resolver ABI");
        auto* context=Resolve{}(child);
        static_assert(noexcept(context->owns(child)) && noexcept(context->sampleMode(source,target)));
        if(context && source && target && context->owns(child)) context->sampleMode(source,target);
    }
    static void formatMode(void* child,void* source,void* settings) noexcept {
        static_assert(noexcept(Resolve{}(child)),"VP mode resolver ABI");
        auto* context=Resolve{}(child);
        static_assert(noexcept(context->owns(child)) && noexcept(context->formatMode(source,settings)));
        if(context && source && settings && context->owns(child)) context->formatMode(source,settings);
    }
};
// Acquire mode inputs and an optional CSC borrow only under caller leases.
// No pointer is dereferenced without Read authorization; all three outputs
// remain unchanged on failure. The returned reference's lease must enclose use.
template<class Read>
bool tglReadOwnedVpModes(uintptr_t source,uintptr_t target,uintptr_t settings,Read read,
    const TglOwnedVpModeState& original,TglOwnedVpModeInput& snapshot,
    TglOwnedVpModeResult& output,uintptr_t& sourceA8) noexcept {
    static_assert(noexcept(read(uintptr_t{},nullptr,size_t{})),"VP mode leased read ABI");
    if(!source || !target || !settings || source>UINTPTR_MAX-0x134 ||
       target>UINTPTR_MAX-4 || settings>UINTPTR_MAX-0x68) return false;
    TglOwnedVpModeInput candidate;std::array<uint8_t,2> flags{};
    if(!read(source,&candidate.source00,4) || !read(target,&candidate.target00,4) ||
       !read(source+0x130,&candidate.source130,4) ||
       !read(settings+0x12,flags.data(),2)) return false;
    candidate.settings12=flags[0]&1;candidate.settings13=flags[1]&1;
    if((candidate.source00==1 || candidate.source00==2) &&
       !(candidate.target00>=3 && candidate.target00<=0xa)) {
        if(!read(source+0xd8,&candidate.sourceD8,4) ||
           !read(source+0xdc,&candidate.sourceDc,4)) return false;
    }
    if(!tglOwnedVpForceColor(candidate.source130,true) &&
       !candidate.settings13 && candidate.settings12) {
        uintptr_t surface=0;
        if(!read(settings+0x60,&surface,8) || !surface || surface>UINTPTR_MAX-0x134 ||
           !read(surface+0x130,&candidate.settings60130,4)) return false;
        candidate.settings60Present=true;
    }
    TglOwnedVpModeResult result;
    if(!tglPrepareOwnedVpModes(candidate,original,result)) return false;
    uintptr_t reference=0;
    if(result.state.at998!=candidate.target00) {
        if(!read(source+0xa8,&reference,8)) return false;
        candidate.sourceA8Present=reference!=0;
        result.borrowSourceA8=candidate.sourceA8Present;
    }
    snapshot=candidate;output=result;sourceA8=reference;return true;
}
// Frozen, lease-scoped context for the two void mode callbacks. All native
// descriptor acquisition happens before either callback is admitted. Preserve
// original private fields on every exit, including a partial b0-only sequence;
// the source+A8 object remains borrowed under the caller's descriptor lease.
template<class Read> class TglOwnedVpModeContext {
    TglOwnedVpSfcCallerStorage& storage_;
    uintptr_t source_,target_,settings_,reference_=0;
    TglOwnedVpModeInput input_;
    TglOwnedVpModeResult result_;
    std::array<uint8_t,17> saved_{};
    bool prepared_=false,sampled_=false,formatted_=false,invalid_=false;
    static constexpr std::array<size_t,4> offsets{0x998,0x99c,0x923,0x9c0};
    static constexpr std::array<size_t,4> sizes{4,4,1,8};
public:
    TglOwnedVpModeContext(TglOwnedVpSfcCallerStorage& storage,uintptr_t source,
        uintptr_t target,uintptr_t settings,Read read) noexcept
        :storage_(storage),source_(source),target_(target),settings_(settings) {
        size_t cursor=0;
        for(size_t i=0;i<offsets.size();++i) {
            std::memcpy(saved_.data()+cursor,storage_.bytes.data()+offsets[i],sizes[i]);cursor+=sizes[i];
        }
        TglOwnedVpModeState original;
        std::memcpy(&original.at998,saved_.data(),4);
        std::memcpy(&original.at99c,saved_.data()+4,4);original.at923=saved_[8]&1;
        prepared_=tglReadOwnedVpModes(source,target,settings,read,original,input_,result_,reference_);
        uintptr_t existingReference=0;
        std::memcpy(&existingReference,saved_.data()+9,8);
        if(result_.borrowSourceA8 && existingReference) prepared_=false;
        // result_ will be produced by the ordered callbacks, not prepublished.
        result_={original,false};
    }
    TglOwnedVpModeContext(const TglOwnedVpModeContext&)=delete;
    TglOwnedVpModeContext& operator=(const TglOwnedVpModeContext&)=delete;
    ~TglOwnedVpModeContext() {
        if(!sampled_) return;
        size_t cursor=0;
        for(size_t i=0;i<offsets.size();++i) {
            std::memcpy(storage_.bytes.data()+offsets[i],saved_.data()+cursor,sizes[i]);cursor+=sizes[i];
        }
    }
    bool owns(void* child) const noexcept {
        return prepared_ && !invalid_ && child==storage_.bytes.data();
    }
    int status() const noexcept {return prepared_ && sampled_ && formatted_ && !invalid_ ? 0 : 5;}
    // Exact ICL1f7690/1f77a0 return the VP mode fields998/99c. Here
    // those bytes belong to this owned, fully prepared scoped context, not
    // a foreign donor object. Reject an out-of-band overwrite before export.
    bool outputColorSpace(uint32_t& output) const noexcept {
        if(status()) return false;
        uint32_t actual=0;std::memcpy(&actual,storage_.bytes.data()+0x998,4);
        if(actual!=result_.state.at998) return false;
        output=actual;return true;
    }
    bool outputFormat(uint32_t& output) const noexcept {
        if(status()) return false;
        uint32_t actual=0;std::memcpy(&actual,storage_.bytes.data()+0x99c,4);
        if(actual!=result_.state.at99c) return false;
        output=actual;return true;
    }
    void sampleMode(void* source,void* target) noexcept {
        if(!prepared_ || invalid_ || sampled_ || reinterpret_cast<uintptr_t>(source)!=source_ ||
            reinterpret_cast<uintptr_t>(target)!=target_) {invalid_=true;return;}
        tglPrepareOwnedVpSampleMode(input_,result_.state,result_);sampled_=true;
        std::memcpy(storage_.bytes.data()+0x998,&result_.state.at998,4);
        storage_.bytes[0x923]=result_.state.at923;
        if(result_.borrowSourceA8) std::memcpy(storage_.bytes.data()+0x9c0,&reference_,8);
    }
    void formatMode(void* source,void* settings) noexcept {
        if(!prepared_ || invalid_ || !sampled_ || formatted_ ||
            reinterpret_cast<uintptr_t>(source)!=source_ ||
            reinterpret_cast<uintptr_t>(settings)!=settings_ ||
            !tglPrepareOwnedVpFormatMode(input_,result_,result_)) {invalid_=true;return;}
        formatted_=true;
        std::memcpy(storage_.bytes.data()+0x99c,&result_.state.at99c,4);
        storage_.bytes[0x923]=result_.state.at923;
    }
};
// VP +9b8, 1def21..1df03a ->4a6d0. Unlike +9a8's classifier,
// sourcePack is from source+130, not VP+99c. Upsampling is only 420->422/444
// or 422->444; the force-color branch bypasses the predicate entirely.
inline bool tglPrepareOwnedVpChromaUpsampling(uint32_t format99c,bool settings12,
    uint32_t sourcePack,uint32_t targetPack,bool& output) noexcept {
    if(sourcePack>3 || targetPack>3) return false;
    output=!tglOwnedVpForceColor(format99c,settings12) &&
        ((sourcePack==0 && (targetPack==1 || targetPack==2)) ||
         (sourcePack==1 && targetPack==2));
    return true;
}
inline bool tglPrepareOwnedVpColorMetadata(uint32_t format99c,bool settings12,
    uint32_t source120,uint32_t source294,uint32_t target294,
    uint32_t inputPack,uint32_t outputPack,TglOwnedVpColorMetadata& output) noexcept {
    const bool force=tglOwnedVpForceColor(format99c,settings12);
    TglOwnedVpColorMetadata candidate;
    candidate.at9a4=force?1:source120;
    if(!tglNormalizeOwnedChromaSitingValue(source294,inputPack,candidate.at9a8) ||
       !tglNormalizeOwnedChromaSitingValue(target294,outputPack,candidate.target294))
        return false;
    output=candidate;
    return true;
}
struct TglOwnedVpFrameColor {
    TglOwnedVpModeResult modes;
    TglOwnedVpColorMetadata metadata;
    bool at9b8=false;
};
// Ordered +28 data chain: +b0 -> +b8 -> color metadata -> +9b8.
// The effective format classifies +9a8; the original source format classifies
// upsampling. All output is atomic and remains private CPU data.
inline bool tglPrepareOwnedVpFrameColor(const TglOwnedVpModeInput& in,
    const TglOwnedVpModeState& original,uint32_t source120,uint32_t source294,
    uint32_t target130,uint32_t target294,TglOwnedVpFrameColor& output) noexcept {
    TglOwnedVpFrameColor candidate;
    if(!tglPrepareOwnedVpModes(in,original,candidate.modes)) return false;
    const uint32_t effective=candidate.modes.state.at99c;
    const uint32_t targetPack=tglOwnedChromaColorPack(target130);
    if(!tglPrepareOwnedVpColorMetadata(effective,in.settings12,source120,source294,
        target294,tglOwnedChromaColorPack(effective),targetPack,candidate.metadata) ||
       !tglPrepareOwnedVpChromaUpsampling(effective,in.settings12,
        tglOwnedChromaColorPack(in.source130),targetPack,candidate.at9b8)) return false;
    output=candidate;
    return true;
}
// Private synchronous VP view, not a native constructor or published vtable.
// Caller retains the exact source+A8 lease through use(). Target294 remains
// owned output in frame.metadata: never mutate a borrowed target surface here.
template<class Use>
int tglWithOwnedVpFrameColor(TglOwnedVpSfcCallerStorage& storage,
    const TglOwnedVpFrameColor& frame,uintptr_t sourceA8,Use use) noexcept {
    static_assert(noexcept(use(static_cast<void*>(nullptr))),"VP color scope ABI");
    uintptr_t previousReference=0;
    std::memcpy(&previousReference,storage.bytes.data()+0x9c0,8);
    if(frame.modes.borrowSourceA8 && (!sourceA8 || previousReference)) return 5;
    constexpr std::array<size_t,6> offsets{0x998,0x99c,0x923,0x9a4,0x9a8,0x9b8};
    constexpr std::array<size_t,6> lengths{4,4,1,4,4,1};
    std::array<std::array<uint8_t,4>,6> saved{};
    for(size_t i=0;i<offsets.size();++i)
        std::memcpy(saved[i].data(),storage.bytes.data()+offsets[i],lengths[i]);
    const uint8_t csc=frame.modes.state.at923,upsampling=frame.at9b8;
    std::memcpy(storage.bytes.data()+0x998,&frame.modes.state.at998,4);
    std::memcpy(storage.bytes.data()+0x99c,&frame.modes.state.at99c,4);
    std::memcpy(storage.bytes.data()+0x923,&csc,1);
    std::memcpy(storage.bytes.data()+0x9a4,&frame.metadata.at9a4,4);
    std::memcpy(storage.bytes.data()+0x9a8,&frame.metadata.at9a8,4);
    std::memcpy(storage.bytes.data()+0x9b8,&upsampling,1);
    if(frame.modes.borrowSourceA8)
        std::memcpy(storage.bytes.data()+0x9c0,&sourceA8,8);
    const int result=use(storage.bytes.data());
    for(size_t i=0;i<offsets.size();++i)
        std::memcpy(storage.bytes.data()+offsets[i],saved[i].data(),lengths[i]);
    std::memcpy(storage.bytes.data()+0x9c0,&previousReference,8);
    return result;
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

// Slot50 field acquisition, under the same surface/MHW leases as its caller.
// Publish both outputs only after every bounded read and geometry gate passes.
template<class Read>
bool tglReadOwnedSfcFrame(uintptr_t surface,uintptr_t mhw,Read read,
                        uint32_t& height,uint32_t& width) noexcept {
    static_assert(noexcept(read(uintptr_t{},nullptr,size_t{})),"SFC frame leased read ABI");
    if(!surface || !mhw || surface>UINTPTR_MAX-0xe0 || mhw>UINTPTR_MAX-0x1c) return false;
    uint32_t sourceHeight=0,sourceWidth=0;
    float bottom=0,right=0;uint8_t field=0;
    uint16_t heightAlign=0,widthAlign=0;
    if(!read(surface+0xdc,&sourceHeight,4) || !read(surface+0xd8,&sourceWidth,4) ||
       !read(surface+0x5c,&bottom,4) || !read(surface+0x58,&right,4) ||
       !read(surface+0x84,&field,1) || !read(mhw+0x1a,&heightAlign,2) ||
       !read(mhw+0x18,&widthAlign,2)) return false;
    uint32_t candidateHeight=0,candidateWidth=0;
    if(!tglAdjustOwnedSfcFloatFrame(sourceHeight,sourceWidth,bottom,right,field&1,
        heightAlign,widthAlign,candidateHeight,candidateWidth)) return false;
    height=candidateHeight;width=candidateWidth;return true;
}
struct TglOwnedVpRegionInput {
    uint32_t frameWidth=0,frameHeight=0,targetWidth=0,targetHeight=0,targetFormat=0,rotation=0;
    float sourceLeft=0,sourceTop=0,sourceRight=0,sourceBottom=0;
    float destinationLeft=0,destinationTop=0,destinationRight=0,destinationBottom=0;
    bool fieldInput=false;
};
struct TglOwnedVpRegions {
    uint32_t sourceWidth=0,sourceHeight=0,destinationWidth=0,destinationHeight=0;
    TglOwnedVpScale scale;
};
// VP +28 1de86f..1deb5b, frame dimensions already returned by +50.
// Source endpoint truncation precedes doubling/subtraction; source extent is
// aligned DOWN, destination extent UP after target clipping. Do not substitute
// the +18 admission rectangle formula (which has a different truncation order).
inline bool tglPrepareOwnedVpRegions(const TglOwnedVpRegionInput& in,
                                     TglOwnedVpRegions& output) noexcept {
    const auto truncate=[](float value,uint32_t& result) noexcept {
        if(!std::isfinite(value) || value<0 || double(value)>double(UINT32_MAX)) return false;
        result=static_cast<uint32_t>(value);return true;
    };
    for(float value:{in.sourceLeft,in.sourceTop,in.destinationLeft,in.destinationTop})
        if(!std::isfinite(value) || value<0) return false;
    uint32_t right=0,bottom=0,width=0,height=0,dw=0,dh=0;
    if(!truncate(in.sourceRight,right) || !truncate(in.sourceBottom,bottom)) return false;
    if(in.fieldInput) {
        if(right>UINT32_MAX/2 || bottom>UINT32_MAX/2) return false;
        right*=2;bottom*=2;
    }
    if(!truncate(float(right)-in.sourceLeft,width) ||
       !truncate(float(bottom)-in.sourceTop,height) ||
       !truncate(in.destinationRight-in.destinationLeft,dw) ||
       !truncate(in.destinationBottom-in.destinationTop,dh)) return false;
    const auto alignment=tglOwnedSfcAlignment(in.targetFormat);
    TglOwnedVpRegions candidate;
    candidate.sourceWidth=std::min(width,in.frameWidth)&~uint32_t(alignment.width-1);
    candidate.sourceHeight=std::min(height,in.frameHeight)&~uint32_t(alignment.height-1);
    dw=std::min(dw,in.targetWidth);dh=std::min(dh,in.targetHeight);
    if(dw>UINT32_MAX-(alignment.width-1u) || dh>UINT32_MAX-(alignment.height-1u)) return false;
    candidate.destinationWidth=(dw+alignment.width-1u)&~uint32_t(alignment.width-1);
    candidate.destinationHeight=(dh+alignment.height-1u)&~uint32_t(alignment.height-1);
    if(!tglPrepareOwnedVpScale(candidate.sourceWidth,candidate.sourceHeight,
        candidate.destinationWidth,candidate.destinationHeight,in.rotation,candidate.scale)) return false;
    output=candidate;return true;
}
// Exact VP18 1de023..1de117: subtract floats BEFORE integer truncation;
// source clips to the separately qualified slot50 frame then aligns down.
// Destination aligns up WITHOUT target clipping; scale alone uses min(target).
// Native18 only special-cases formats d/10 (width2) and19 (width2,height2),
// and never performs slot28's field-input endpoint doubling.
inline bool tglPrepareOwnedVpAdmissionRegions(const TglOwnedVpRegionInput& in,
    TglOwnedVpRegions& output) noexcept {
    auto extent=[](float first,float last,uint32_t& value) noexcept {
        const float delta=last-first;
        if(!std::isfinite(first) || !std::isfinite(last) ||
            !std::isfinite(delta) || delta<0 || double(delta)>double(UINT32_MAX)) return false;
        value=static_cast<uint32_t>(delta);return true;
    };
    uint32_t sw=0,sh=0,dw=0,dh=0;
    if(!extent(in.sourceLeft,in.sourceRight,sw) || !extent(in.sourceTop,in.sourceBottom,sh) ||
        !extent(in.destinationLeft,in.destinationRight,dw) ||
        !extent(in.destinationTop,in.destinationBottom,dh)) return false;
    const uint32_t widthAlignment=(in.targetFormat==0xd || in.targetFormat==0x10 ||
        in.targetFormat==0x19)?2:1;
    const uint32_t heightAlignment=in.targetFormat==0x19?2:1;
    if(dw>UINT32_MAX-(widthAlignment-1) || dh>UINT32_MAX-(heightAlignment-1)) return false;
    TglOwnedVpRegions candidate;
    candidate.sourceWidth=std::min(sw,in.frameWidth)&~(widthAlignment-1);
    candidate.sourceHeight=std::min(sh,in.frameHeight)&~(heightAlignment-1);
    candidate.destinationWidth=(dw+widthAlignment-1)&~(widthAlignment-1);
    candidate.destinationHeight=(dh+heightAlignment-1)&~(heightAlignment-1);
    if(!tglPrepareOwnedVpScale(candidate.sourceWidth,candidate.sourceHeight,
        std::min(candidate.destinationWidth,in.targetWidth),
        std::min(candidate.destinationHeight,in.targetHeight),in.rotation,candidate.scale)) return false;
    output=candidate;return true;
}
// Bind the independently qualified MHW limits/workaround/conditional-source
// policy to the exact slot18 geometry, rather than accepting caller-invented
// aligned extents or reusing slot28's different rectangle producer.
inline bool tglPrepareOwnedVpAdmission(const TglOwnedVpExtentAdmission& policy,
    const TglOwnedVpRegionInput& input,TglOwnedVpScale& output) noexcept {
    TglOwnedVpRegions regions;
    if(!tglPrepareOwnedVpAdmissionRegions(input,regions)) return false;
    auto candidate=policy;
    candidate.frameWidth=input.frameWidth;candidate.frameHeight=input.frameHeight;
    candidate.sourceWidth=regions.sourceWidth;candidate.sourceHeight=regions.sourceHeight;
    candidate.destinationWidth=regions.destinationWidth;candidate.destinationHeight=regions.destinationHeight;
    candidate.targetWidth=input.targetWidth;candidate.targetHeight=input.targetHeight;
    candidate.rotation=input.rotation;candidate.target130=input.targetFormat;
    candidate.source30=input.sourceLeft;candidate.source34=input.sourceTop;
    candidate.source38=input.sourceRight;candidate.source3c=input.sourceBottom;
    candidate.source40=input.destinationLeft;candidate.source44=input.destinationTop;
    return tglPrepareOwnedVpExtentAdmission(candidate,output);
}

// Slot18 leased acquisition. The enclosing owner authenticates/pins MHW,
// RenderHal, settings and both surfaces. Eligibility uses native20's reordered
// (settings,source,target) arguments; dimensions is the qualified owned50
// width-first adjuster, not an arbitrary native function pointer.
template<class Read,class Eligible,class Dimensions>
bool tglReadOwnedVpExtentAdmission(const TglOwnedVpSfcCallerStorage& vp,
    uintptr_t source,uintptr_t target,uintptr_t settings,Read read,
    Eligible eligible,Dimensions dimensions,TglOwnedVpScale& output) noexcept {
    static_assert(noexcept(read(uintptr_t{},nullptr,size_t{})) &&
        noexcept(eligible(settings,source,target)) &&
        noexcept(dimensions(source,std::declval<uint32_t&>(),std::declval<uint32_t&>())),
        "VP18 leased acquisition ABI");
    uintptr_t mhw=0,renderHal=0;
    std::memcpy(&mhw,vp.bytes.data()+0x20,8);std::memcpy(&renderHal,vp.bytes.data()+0x18,8);
    if(!source || source>UINTPTR_MAX-0x294 || !target || target>UINTPTR_MAX-0x134 ||
        !settings || settings>UINTPTR_MAX-0xe8 || !mhw || mhw>UINTPTR_MAX-0x2c ||
        !renderHal || renderHal>UINTPTR_MAX-0x88) return false;
    std::array<uint32_t,4> limits{};
    if(!read(mhw+0x1c,limits.data(),sizeof(limits)) || !eligible(settings,source,target)) return false;
    TglOwnedVpRegionInput input;
    if(!dimensions(source,input.frameWidth,input.frameHeight)) return false;
    std::array<float,8> rectangles{};
    if(!read(source+0x30,rectangles.data(),sizeof(rectangles)) ||
        !read(source+0x290,&input.rotation,4) ||
        !read(target+0xd8,&input.targetWidth,4) || !read(target+0xdc,&input.targetHeight,4) ||
        !read(target+0x130,&input.targetFormat,4)) return false;
    input.sourceLeft=rectangles[0];input.sourceTop=rectangles[1];
    input.sourceRight=rectangles[2];input.sourceBottom=rectangles[3];
    input.destinationLeft=rectangles[4];input.destinationTop=rectangles[5];
    input.destinationRight=rectangles[6];input.destinationBottom=rectangles[7];
    TglOwnedVpExtentAdmission policy;
    // 1ddf72/7c/86/90 -> 1de11a..1de1c0: maxima precede minima.
    policy.maxWidth=limits[0];policy.maxHeight=limits[1];
    policy.minWidth=limits[2];policy.minHeight=limits[3];
    uintptr_t workaround=0;uint64_t flags=0,available=0;
    if(!read(renderHal+0x80,&workaround,8) || !workaround || workaround>UINTPTR_MAX-0xac ||
        !read(workaround+0xa4,&flags,8) || !read(source+0x124,&policy.source124,4) ||
        !read(target+0xe4,&policy.targetE4,4) || !read(settings+0xe0,&available,8)) return false;
    policy.workaroundA4Bit25=(flags>>25)&1;
    if(available) {
        TglOwnedVpDestinationRect targetRect;
        if(!read(target+0x40,&targetRect,sizeof(targetRect))) return false;
        const TglOwnedVpDestinationRect sourceRect{rectangles[4],rectangles[5],rectangles[6],rectangles[7]};
        policy.conditionalSource=tglOwnedVpConditionalSource(true,sourceRect,targetRect);
    }
    return tglPrepareOwnedVpAdmission(policy,input,output);
}

template<class Read,class Eligible,class Dimensions> class TglOwnedVpExtentContext {
    TglOwnedVpSfcCallerStorage& vp_;Read& read_;Eligible& eligible_;Dimensions& dimensions_;
    bool busy_=false;
public:
    TglOwnedVpExtentContext(TglOwnedVpSfcCallerStorage& vp,Read& read,
        Eligible& eligible,Dimensions& dimensions) noexcept
        :vp_(vp),read_(read),eligible_(eligible),dimensions_(dimensions){}
    bool owns(void* child) const noexcept {return child==vp_.bytes.data();}
    int admit(void* source,void* target,void* settings) noexcept {
        if(busy_) return 0;
        busy_=true;TglOwnedVpScale scale;
        const bool admitted=tglReadOwnedVpExtentAdmission(vp_,reinterpret_cast<uintptr_t>(source),
            reinterpret_cast<uintptr_t>(target),reinterpret_cast<uintptr_t>(settings),
            read_,eligible_,dimensions_,scale);
        busy_=false;return admitted?1:0;
    }
};
template<class Resolve> struct TglOwnedVpExtentCallback {
    static int admit(void* child,void* source,void* target,void* settings) noexcept {
        static_assert(noexcept(Resolve{}(child)),"VP18 scoped resolver ABI");
        auto* context=Resolve{}(child);
        static_assert(noexcept(context->owns(child)) && noexcept(context->admit(source,target,settings)) &&
            std::is_same_v<decltype(context->admit(source,target,settings)),int>,"VP18 DWORD return ABI");
        return context && source && target && settings && context->owns(child) ?
            context->admit(source,target,settings):0;
    }
};
// Named ABI collection, NOT a complete/native-publishable vtable. Keep all
// admission entries under the same VP/input lease and expire them together.
// VP18 is the GetOutputPipe(source,target,renderParams) decision, NOT an
// unconditional prelude to VP28. Intel common/vphal_render_sfc_base.cpp414
// and gen12/vphal_render_vebox_g12_base.cpp2495 establish this donor role;
// exact ICL1de4df..1de4f7 returns DWORD composition0/SFC1 after VP68.
// Do not import the Linux decompression tail: this exact native return has
// no such call. Caller1fb55e qualifies the pipe-selection edge below;
// this still does not authorize publication of an incomplete full vtable.
// ABI catalogue for the complete flat owned VP, NOT a published vtable.
// Exact ICL7940e8 has 27 slots. Donor adjustment thunks/RTTI are deliberately
// absent; all entries must be supplied by independently qualified producers.
// hasAllEntries establishes presence only, not policy/lifetime/GPU readiness.
struct TglOwnedVpEntrySet {
    void (*destroy)(void*) noexcept=nullptr;
    void (*deletingDestroy)(void*) noexcept=nullptr;
    void (*initialize)(void*) noexcept=nullptr;
    int (*extent)(void*,void*,void*,void*) noexcept=nullptr;
    bool (*eligible)(void*,void*,void*,void*) noexcept=nullptr;
    void (*prepare)(void*,void*,void*,void*,void*,void*) noexcept=nullptr;
    void (*freeResources)(void*) noexcept=nullptr;
    bool (*pair)(void*,void*,void*,void*) noexcept=nullptr;
    int (*setup)(void*,void*,void*,void*) noexcept=nullptr;
    int (*send)(void*,void*,void*) noexcept=nullptr;
    void (*dimensions)(void*,void*,uint32_t*,uint32_t*) noexcept=nullptr;
    int (*allocate)(void*) noexcept=nullptr;
    bool (*compressionFormat)(void*,uint32_t) noexcept=nullptr;
    bool (*finalAdmission)(void*,uint8_t,void*,void*) noexcept=nullptr;
    bool (*inputFormat)(void*,void*) noexcept=nullptr;
    bool (*outputFormat)(void*,void*) noexcept=nullptr;
    void (*inputAlignment)(void*,uint32_t,uint32_t,uint16_t*,uint16_t*) noexcept=nullptr;
    void (*outputAlignment)(void*,uint32_t,uint16_t*,uint16_t*) noexcept=nullptr;
    int (*state)(void*,void*,void*,void*) noexcept=nullptr;
    void (*filters)(void*,void*,void*,void*) noexcept=nullptr;
    void (*csc)(void*,void*,void*) noexcept=nullptr; // state, IEF
    int (*platformSetup)(void*,void*,void*,void*) noexcept=nullptr;
    void (*sampleMode)(void*,void*,void*) noexcept=nullptr;
    void (*formatMode)(void*,void*,void*) noexcept=nullptr;
    void (*platformPacket)(void*,void*,void*) noexcept=nullptr;
    int (*platformSend)(void*,void*,void*,void*,void*,void*) noexcept=nullptr;
    int (*compression)(void*,void*,void*,void*) noexcept=nullptr;
    uint32_t missingMask() const noexcept {
        const std::array<bool,27> present{bool(destroy),bool(deletingDestroy),bool(initialize),
            bool(extent),bool(eligible),bool(prepare),bool(freeResources),bool(pair),bool(setup),
            bool(send),bool(dimensions),bool(allocate),bool(compressionFormat),bool(finalAdmission),
            bool(inputFormat),bool(outputFormat),bool(inputAlignment),bool(outputAlignment),bool(state),
            bool(filters),bool(csc),bool(platformSetup),bool(sampleMode),bool(formatMode),
            bool(platformPacket),bool(platformSend),bool(compression)};
        uint32_t missing=0;
        for(size_t i=0;i<present.size();++i) if(!present[i]) missing|=uint32_t{1}<<i;
        return missing;
    }
    bool hasAllEntries() const noexcept {return missingMask()==0;}
    // Compose typed, already-leased callback subsets without overwriting any
    // slot. Atomic on conflict; this neither extends scopes nor qualifies a
    // complete native object, vtable metadata or GPU resource lifetime.
    bool appendDisjoint(const TglOwnedVpEntrySet& additions) noexcept {
        auto candidate=*this;
#define NGRN_APPEND_VP(field) \
        if(additions.field) {if(candidate.field) return false;candidate.field=additions.field;}
        NGRN_APPEND_VP(destroy) NGRN_APPEND_VP(deletingDestroy) NGRN_APPEND_VP(initialize)
        NGRN_APPEND_VP(extent) NGRN_APPEND_VP(eligible) NGRN_APPEND_VP(prepare)
        NGRN_APPEND_VP(freeResources) NGRN_APPEND_VP(pair) NGRN_APPEND_VP(setup)
        NGRN_APPEND_VP(send) NGRN_APPEND_VP(dimensions) NGRN_APPEND_VP(allocate)
        NGRN_APPEND_VP(compressionFormat) NGRN_APPEND_VP(finalAdmission)
        NGRN_APPEND_VP(inputFormat) NGRN_APPEND_VP(outputFormat)
        NGRN_APPEND_VP(inputAlignment) NGRN_APPEND_VP(outputAlignment)
        NGRN_APPEND_VP(state) NGRN_APPEND_VP(filters) NGRN_APPEND_VP(csc)
        NGRN_APPEND_VP(platformSetup) NGRN_APPEND_VP(sampleMode) NGRN_APPEND_VP(formatMode)
        NGRN_APPEND_VP(platformPacket) NGRN_APPEND_VP(platformSend) NGRN_APPEND_VP(compression)
#undef NGRN_APPEND_VP
        *this=candidate;return true;
    }
};
#define NGRN_VP_SLOT(field,offset) static_assert(offsetof(TglOwnedVpEntrySet,field)==offset,"VP slot ABI")
NGRN_VP_SLOT(destroy,0);NGRN_VP_SLOT(deletingDestroy,8);NGRN_VP_SLOT(initialize,0x10);
NGRN_VP_SLOT(extent,0x18);NGRN_VP_SLOT(eligible,0x20);NGRN_VP_SLOT(prepare,0x28);
NGRN_VP_SLOT(freeResources,0x30);NGRN_VP_SLOT(pair,0x38);NGRN_VP_SLOT(setup,0x40);
NGRN_VP_SLOT(send,0x48);NGRN_VP_SLOT(dimensions,0x50);NGRN_VP_SLOT(allocate,0x58);
NGRN_VP_SLOT(compressionFormat,0x60);NGRN_VP_SLOT(finalAdmission,0x68);
NGRN_VP_SLOT(inputFormat,0x70);NGRN_VP_SLOT(outputFormat,0x78);
NGRN_VP_SLOT(inputAlignment,0x80);NGRN_VP_SLOT(outputAlignment,0x88);
NGRN_VP_SLOT(state,0x90);NGRN_VP_SLOT(filters,0x98);NGRN_VP_SLOT(csc,0xa0);
NGRN_VP_SLOT(platformSetup,0xa8);NGRN_VP_SLOT(sampleMode,0xb0);NGRN_VP_SLOT(formatMode,0xb8);
NGRN_VP_SLOT(platformPacket,0xc0);NGRN_VP_SLOT(platformSend,0xc8);NGRN_VP_SLOT(compression,0xd0);
#undef NGRN_VP_SLOT
static_assert(std::is_standard_layout_v<TglOwnedVpEntrySet> && sizeof(TglOwnedVpEntrySet)==0xd8);
// Stable private dispatch for a FLAT, all-owned VP only. Zero offset-to-top
// and absent RTTI are deliberate: no donor virtual-base metadata/thunks are
// transplanted. This does not authorize native RTTI consumers or publication;
// the object, all resolver scopes and backing owners need separate admission.
class TglOwnedVpFlatDispatchTable {
    struct Layout {
        intptr_t offsetToTop=0;
        uintptr_t typeInfo=0;
        TglOwnedVpEntrySet entries;
    } layout_;
    static_assert(offsetof(Layout,entries)==16 && sizeof(uintptr_t)==8);
    bool ready_=false,building_=false;
public:
    TglOwnedVpFlatDispatchTable() noexcept=default;
    TglOwnedVpFlatDispatchTable(const TglOwnedVpFlatDispatchTable&)=delete;
    TglOwnedVpFlatDispatchTable& operator=(const TglOwnedVpFlatDispatchTable&)=delete;
    template<class OwnedExecutable>
    bool build(const TglOwnedVpEntrySet& entries,OwnedExecutable ownedExecutable) noexcept {
        static_assert(noexcept(ownedExecutable(uintptr_t{},size_t{})),"owned VP code admission ABI");
        static_assert(std::is_same_v<decltype(ownedExecutable(uintptr_t{},size_t{})),bool>,
                      "owned VP code admission returns a predicate, not native status");
        if(ready_ || building_) return false;
        const auto candidate=entries;
        if(!candidate.hasAllEntries()) return false;
        building_=true;
        struct Unlock {bool& busy;~Unlock(){busy=false;}} unlock{building_};
        std::array<uintptr_t,27> addresses{};
        std::memcpy(addresses.data(),&candidate,sizeof(candidate));
        for(const auto address:addresses)
            if(!ownedExecutable(address,1)) return false;
        layout_.entries=candidate;ready_=true;return true;
    }
    uintptr_t addressPoint() const noexcept {
        return ready_ ? reinterpret_cast<uintptr_t>(&layout_.entries) : 0;
    }
};
template<class Resolve>
bool tglBindOwnedVpModeEntries(void* child,TglOwnedVpEntrySet& target) noexcept {
    static_assert(noexcept(Resolve{}(child)),"VP mode binding resolver ABI");
    auto* context=Resolve{}(child);
    static_assert(noexcept(context->owns(child)),"VP mode binding ownership ABI");
    if(!context || !context->owns(child) || target.sampleMode || target.formatMode) return false;
    using Callback=TglOwnedVpModeCallbacks<Resolve>;
    auto candidate=target;
    candidate.sampleMode=&Callback::sampleMode;candidate.formatMode=&Callback::formatMode;
    target=candidate;return true;
}
// Extend an already leased catalogue without exporting the mode context.
// The caller retains every base entry's resolver/inputs through Use.
template<class Read,class Use>
int tglWithOwnedVpModeAssembly(TglOwnedVpSfcCallerStorage& storage,
    uintptr_t source,uintptr_t target,uintptr_t settings,Read& read,
    const TglOwnedVpEntrySet& base,Use use) noexcept {
    using Context=TglOwnedVpModeContext<Read>;
    using Resolve=TglOwnedVpSfcResolve<Context>;
    Context context(storage,source,target,settings,read);
    void* child=storage.bytes.data();
    TglOwnedVpSfcScopedContext<Context> scope(child,context);
    if(!scope.bound()) return 5;
    auto entries=base;
    if(!tglBindOwnedVpModeEntries<Resolve>(child,entries)) return 5;
    static_assert(noexcept(use(child,entries)),"VP mode assembly consumer ABI");
    const int result=use(child,entries);
    return result ? result : context.status();
}
template<class Resolve> struct TglOwnedVpLifetimeCallbacks;
template<class Resolve>
bool tglBindOwnedVpLifetimeEntries(void* child,TglOwnedVpEntrySet& target) noexcept {
    static_assert(noexcept(Resolve{}(child)),"VP lifecycle binding resolver ABI");
    auto* context=Resolve{}(child);
    static_assert(noexcept(context->owns(child)),"VP lifecycle binding ownership ABI");
    if(!context || !context->owns(child) || target.destroy || target.deletingDestroy) return false;
    using Callback=TglOwnedVpLifetimeCallbacks<Resolve>;
    auto candidate=target;
    candidate.destroy=&Callback::destroy;candidate.deletingDestroy=&Callback::deletingDestroy;
    target=candidate;return true;
}
template<class Resolve> struct TglOwnedVpCompressionFormatCallback;
struct TglOwnedVpAdmissionEntries {
    int (*extent)(void*,void*,void*,void*) noexcept=nullptr;
    bool (*eligible)(void*,void*,void*,void*) noexcept=nullptr;
    bool (*pair)(void*,void*,void*,void*) noexcept=nullptr;
    bool (*input)(void*,void*) noexcept=nullptr;
    bool (*output)(void*,void*) noexcept=nullptr;
    void (*dimensions)(void*,void*,uint32_t*,uint32_t*) noexcept=nullptr;
    bool (*finalAdmission)(void*,uint8_t,void*,void*) noexcept=nullptr;
    bool (*compressionFormat)(void*,uint32_t) noexcept=nullptr;
    void (*inputAlignment)(void*,uint32_t,uint32_t,uint16_t*,uint16_t*) noexcept=nullptr;
    void (*outputAlignment)(void*,uint32_t,uint16_t*,uint16_t*) noexcept=nullptr;
};
// Bind only these ten qualified ABI entries, atomically and without replacing
// another producer. The functions remain subject to their original TLS scope;
// retaining this catalogue cannot extend admission or publish a VP object.
inline bool tglBindOwnedVpAdmissionEntries(TglOwnedVpEntrySet& target,
    const TglOwnedVpAdmissionEntries& source) noexcept {
    if(!source.extent || !source.eligible || !source.pair || !source.dimensions ||
       !source.compressionFormat || !source.finalAdmission || !source.input || !source.output ||
       !source.inputAlignment || !source.outputAlignment) return false;
    if(target.extent || target.eligible || target.pair || target.dimensions ||
       target.compressionFormat || target.finalAdmission || target.inputFormat || target.outputFormat ||
       target.inputAlignment || target.outputAlignment) return false;
    auto candidate=target;
    candidate.extent=source.extent;candidate.eligible=source.eligible;candidate.pair=source.pair;
    candidate.dimensions=source.dimensions;candidate.compressionFormat=source.compressionFormat;
    candidate.finalAdmission=source.finalAdmission;candidate.inputFormat=source.input;
    candidate.outputFormat=source.output;candidate.inputAlignment=source.inputAlignment;
    candidate.outputAlignment=source.outputAlignment;target=candidate;return true;
}
inline bool tglBindOwnedVpResourceEntries(TglOwnedVpEntrySet& target,
    int(*allocate)(void*) noexcept,void(*freeResources)(void*) noexcept) noexcept {
    if(!allocate || !freeResources || target.allocate || target.freeResources) return false;
    auto candidate=target;candidate.allocate=allocate;candidate.freeResources=freeResources;
    target=candidate;return true;
}
template<class Read,class Use>
int tglWithOwnedVpAdmissionEntries(TglOwnedVpSfcCallerStorage& vp,Read& read,Use use) noexcept {
    static_assert(noexcept(use(nullptr,std::declval<const TglOwnedVpAdmissionEntries&>())),
        "VP admission entry consumer ABI");
    using AdmissionContext=TglOwnedVpAdmissionContext<Read>;
    using Admission=TglOwnedVpAdmissionCallbacks<TglOwnedVpSfcResolve<AdmissionContext>>;
    using Compression=TglOwnedVpCompressionFormatCallback<TglOwnedVpSfcResolve<AdmissionContext>>;
    using Alignment=TglOwnedVpAlignmentCallbacks<TglOwnedVpSfcResolve<AdmissionContext>>;
    AdmissionContext admission(vp,read);void* child=vp.bytes.data();
    TglOwnedVpSfcScopedContext<AdmissionContext> admissionScope(child,admission);
    if(!admissionScope.bound()) return 5;
    auto eligible=[&](uintptr_t settings,uintptr_t source,uintptr_t target) noexcept {
        return Admission::eligible(child,reinterpret_cast<void*>(settings),
            reinterpret_cast<void*>(source),reinterpret_cast<void*>(target));
    };
    auto dimensions=[&](uintptr_t source,uint32_t& width,uint32_t& height) noexcept {
        width=height=0;
        Admission::dimensions(child,reinterpret_cast<void*>(source),&width,&height);
        return width!=0 && height!=0;
    };
    using ExtentContext=TglOwnedVpExtentContext<Read,decltype(eligible),decltype(dimensions)>;
    using Extent=TglOwnedVpExtentCallback<TglOwnedVpSfcResolve<ExtentContext>>;
    ExtentContext extent(vp,read,eligible,dimensions);
    TglOwnedVpSfcScopedContext<ExtentContext> extentScope(child,extent);
    if(!extentScope.bound()) return 5;
    const TglOwnedVpAdmissionEntries entries{&Extent::admit,&Admission::eligible,&Admission::pair,
        &Admission::input,&Admission::output,&Admission::dimensions,&Admission::finalAdmission,
        &Compression::supported,&Alignment::input,&Alignment::output};
    return use(child,entries);
}

// ICL primary794210+68=1fb140, caller1fb55e: child80 VP18 receives
// (source, renderParams98 target, renderParams). Only the SFC branch invokes
// it; direct/composition/fast-fill remain the enclosing Gen12 policy's job.
// Owner pins all three inputs and policy dependencies through this scope.
template<class Policy> struct TglOwnedVpOutputPipePolicy {
    Policy& policy;
    const TglOwnedVpAdmissionEntries& entries;
    void* child;void* source;void* target;void* parameters;
    bool bypass() noexcept {return policy.bypass();}
    bool rejectDi() noexcept {return policy.rejectDi();}
    bool rejectLut() noexcept {return policy.rejectLut();}
    bool direct() noexcept {return policy.direct();}
    bool twoPass() noexcept {return policy.twoPass();}
    bool sfcAvailable() noexcept {return policy.sfcAvailable();}
    bool hdr() noexcept {return policy.hdr();}
    bool fastFill() noexcept {return policy.fastFill();}
    bool healthy() noexcept {return policy.healthy();}
    uint32_t sfc() noexcept {
        return static_cast<uint32_t>(entries.extent(child,source,target,parameters));
    }
};
template<class Read,class Policy>
bool tglSelectOwnedVpOutputPipe(TglOwnedVpSfcCallerStorage& vp,Read& read,Policy& policy,
    void* source,void* target,void* parameters,uint8_t& compNeeded,uint32_t& output) noexcept {
    static_assert(noexcept(policy.bypass()) && noexcept(policy.rejectDi()) &&
        noexcept(policy.rejectLut()) && noexcept(policy.direct()) && noexcept(policy.twoPass()) &&
        noexcept(policy.sfcAvailable()) && noexcept(policy.hdr()) &&
        noexcept(policy.fastFill()) && noexcept(policy.healthy()),"owned pipe policy must not throw");
    if(!source || !target || !parameters) return false;
    return tglWithOwnedVpAdmissionEntries(vp,read,
        [&](void* child,const TglOwnedVpAdmissionEntries& entries) noexcept {
            TglOwnedVpOutputPipePolicy<Policy> scoped{policy,entries,child,source,target,parameters};
            return tglSelectGen12VeboxOutputPipe(scoped,compNeeded,output)?0:5;
        })==0;
}

// Adapter for the selected admission stage's (pass08)->DWORD ABI. Failure
// must remain outside pipe0..2, not masquerade as composition fallback.
// References are borrowed from the enclosing leased admission frame.
template<class Read,class Policy> class TglOwnedVpPipeSelector {
    TglOwnedVpSfcCallerStorage& vp_;
    Read& read_;Policy& policy_;
    void* source_;void* target_;void* parameters_;
public:
    TglOwnedVpPipeSelector(TglOwnedVpSfcCallerStorage& vp,Read& read,Policy& policy,
        void* source,void* target,void* parameters) noexcept:
        vp_(vp),read_(read),policy_(policy),source_(source),target_(target),parameters_(parameters){}
    uint32_t operator()(uint8_t& compNeeded) const noexcept {
        uint32_t pipe=UINT32_MAX;
        return tglSelectOwnedVpOutputPipe(vp_,read_,policy_,source_,target_,parameters_,
            compNeeded,pipe)?pipe:UINT32_MAX;
    }
};

// Bounded slot28 geometry acquisition. Read authorizes every surface/MHW
// span under leases held through the caller's complete preparation/use scope.
template<class Read>
bool tglReadOwnedVpRegions(uintptr_t source,uintptr_t target,uintptr_t mhw,Read read,
    TglOwnedVpRegionInput& snapshot,TglOwnedVpRegions& output) noexcept {
    static_assert(noexcept(read(uintptr_t{},nullptr,size_t{})),"VP region leased read ABI");
    if(!source || !target || source>UINTPTR_MAX-0x294 || target>UINTPTR_MAX-0x134) return false;
    TglOwnedVpRegionInput candidate;
    std::array<float,8> rectangles{};uint8_t field=0;
    if(!tglReadOwnedSfcFrame(source,mhw,read,candidate.frameHeight,candidate.frameWidth) ||
       !read(source+0x30,rectangles.data(),sizeof(rectangles)) ||
       !read(source+0x84,&field,1) || !read(source+0x290,&candidate.rotation,4) ||
       !read(target+0xd8,&candidate.targetWidth,4) ||
       !read(target+0xdc,&candidate.targetHeight,4) ||
       !read(target+0x130,&candidate.targetFormat,4)) return false;
    candidate.sourceLeft=rectangles[0];candidate.sourceTop=rectangles[1];
    candidate.sourceRight=rectangles[2];candidate.sourceBottom=rectangles[3];
    candidate.destinationLeft=rectangles[4];candidate.destinationTop=rectangles[5];
    candidate.destinationRight=rectangles[6];candidate.destinationBottom=rectangles[7];
    candidate.fieldInput=field&1;
    TglOwnedVpRegions regions;
    if(!tglPrepareOwnedVpRegions(candidate,regions)) return false;
    snapshot=candidate;output=regions;return true;
}
struct TglOwnedVpFramePreparationInput {
    TglOwnedVpRegionInput geometry;
    TglOwnedVpModeInput modes;
    TglOwnedVpDestinationRect targetDestination;
    uint32_t source120=0,source294=0,target294=0;
    bool conditionalSourceAvailable=false,iefPresent=false;
    uint8_t iefFlags=0;
    float iefStrength=0;
};
struct TglOwnedVpFramePreparation {
    TglOwnedVpRegions regions;
    TglOwnedVpFrameColor color;
    uint32_t at9a0=0;
    bool at920=false,at922=false,at924=false;
};
// VP +28=1de810. Six pointer arguments INCLUDING this, void result.
// 1decfb publishes RDX at +990; 1ded21 consumes R9 mode flags. They
// are not interchangeable. RSI is conditional and may be null. Source
// and target nulls return without preparation (1de83b..1de84e).
// The scoped owner authenticates all remaining arguments and records any
// preparation failure; this void entry must never fabricate a status ABI.
// This entry alone does not publish a complete VP object or retain leases.
template<class Resolve> struct TglOwnedVpFrameCallback {
    static void prepare(void* child,void* conditionalSource,void* parameters,
        void* source,void* target,void* settings) noexcept {
        static_assert(noexcept(Resolve{}(child)),"VP frame resolver ABI");
        auto* context=Resolve{}(child);
        static_assert(noexcept(context->owns(child)) &&
            noexcept(context->prepare(conditionalSource,parameters,source,target,settings)),
            "VP frame context ABI");
        if(context && source && target && context->owns(child))
            context->prepare(conditionalSource,parameters,source,target,settings);
    }
};
template<class Resolve>
bool tglBindOwnedVpFrameEntry(void* child,TglOwnedVpEntrySet& target) noexcept {
    static_assert(noexcept(Resolve{}(child)),"VP frame binding resolver ABI");
    auto* context=Resolve{}(child);
    static_assert(noexcept(context->owns(child)),"VP frame binding ownership ABI");
    if(!context || !context->owns(child) || target.prepare) return false;
    auto candidate=target;
    candidate.prepare=&TglOwnedVpFrameCallback<Resolve>::prepare;
    target=candidate;return true;
}
// +40 caller 1e0d3b..1e0d4a invokes status90(data,source,target).
// 1e0da9..1e0dbb instead invokes void98(data,state938,source).
// Exact98=1e0bb0 ignores RSI and rejects null RDX/RCX before clearing
// its IEF packet. A scoped context must authenticate/pin the actual state
// packet and source, and keep producer failure visible outside this void ABI.
template<class Resolve> struct TglOwnedVpProducerCallbacks {
    static int state(void* child,void* data,void* source,void* target) noexcept {
        static_assert(noexcept(Resolve{}(child)),"VP producer resolver ABI");
        auto* context=Resolve{}(child);
        static_assert(noexcept(context->owns(child)) && noexcept(context->state(data,source,target)),
            "VP state producer ABI");
        return context && source && target && context->owns(child) ?
            context->state(data,source,target) : 5;
    }
    static void filters(void* child,void*,void* state,void* source) noexcept {
        static_assert(noexcept(Resolve{}(child)),"VP filter resolver ABI");
        auto* context=Resolve{}(child);
        static_assert(noexcept(context->owns(child)) && noexcept(context->filters(state,source)),
            "VP filter producer ABI");
        if(context && state && source && context->owns(child)) context->filters(state,source);
    }
};
template<class Resolve>
bool tglBindOwnedVpProducerEntries(void* child,TglOwnedVpEntrySet& target) noexcept {
    static_assert(noexcept(Resolve{}(child)),"VP producer binding resolver ABI");
    auto* context=Resolve{}(child);
    static_assert(noexcept(context->owns(child)),"VP producer binding ownership ABI");
    if(!context || !context->owns(child) || target.state || target.filters) return false;
    using Callback=TglOwnedVpProducerCallbacks<Resolve>;
    auto candidate=target;
    candidate.state=&Callback::state;candidate.filters=&Callback::filters;
    target=candidate;return true;
}
template<class Resolve>
bool tglBindOwnedVpSetupSendEntries(void* child,TglOwnedVpEntrySet& target) noexcept {
    static_assert(noexcept(Resolve{}(child)),"VP setup binding resolver ABI");
    auto* context=Resolve{}(child);
    static_assert(noexcept(context->owns(child)),"VP setup binding ownership ABI");
    if(!context || !context->owns(child) || target.setup || target.send) return false;
    using Callback=TglOwnedVpSfcCallbacks<Resolve>;
    auto candidate=target;
    candidate.setup=&Callback::setup;candidate.send=&Callback::send;
    target=candidate;return true;
}
// Exact ICL donor VP+a0=1e0960 saves RSI=state and RDX=IEF, returns
// void:1e0991 writes RSI+88,1e099c writes RDX+20. Both are pointers,
// so function-pointer type checking alone cannot detect their reversal.
// Mutation is gated on child923 low bit. The scoped producer owns
// that policy/cache and must keep coefficient publication leased through
// command emission; this ABI entry does not invent or shorten that lease.
template<class Resolve> struct TglOwnedVpCscCallback {
    static void csc(void* child,void* state,void* ief) noexcept {
        static_assert(noexcept(Resolve{}(child)),"VP CSC resolver ABI");
        auto* context=Resolve{}(child);
        static_assert(noexcept(context->owns(child)) && noexcept(context->csc(state,ief)),
            "VP CSC producer ABI");
        if(context && ief && state && context->owns(child)) context->csc(state,ief);
    }
};
template<class Resolve>
bool tglBindOwnedVpCscEntry(void* child,TglOwnedVpEntrySet& target) noexcept {
    static_assert(noexcept(Resolve{}(child)),"VP CSC binding resolver ABI");
    auto* context=Resolve{}(child);
    static_assert(noexcept(context->owns(child)),"VP CSC binding ownership ABI");
    if(!context || !context->owns(child) || target.csc) return false;
    auto candidate=target;
    candidate.csc=&TglOwnedVpCscCallback<Resolve>::csc;
    target=candidate;return true;
}
template<size_t StateSize> class TglOwnedVpCscProducerContext {
    static_assert(StateSize==0xb8 || StateSize==0xf8,"VP CSC state extent");
    void* child_;std::array<uint8_t,64>& ief_;uint8_t* state_;
    const TglCscCoefficients* coefficients_;
    std::array<uint8_t,64> savedIef_;uint8_t savedState_;
    bool valid_=true,called_=false,applied_=false,invalid_=false;
public:
    TglOwnedVpCscProducerContext(void* child,std::array<uint8_t,64>& ief,
        uint8_t* state,const TglCscCoefficients* coefficients) noexcept
        :child_(child),ief_(ief),state_(state),coefficients_(coefficients),
         savedIef_(ief),savedState_(state?state[0x88]:0) {
        valid_=child && state;
        if(coefficients_) for(size_t offset:{size_t(0x28),size_t(0x30),size_t(0x38)}) {
            uintptr_t occupied=0;std::memcpy(&occupied,ief_.data()+offset,8);
            if(occupied) valid_=false;
        }
    }
    TglOwnedVpCscProducerContext(const TglOwnedVpCscProducerContext&)=delete;
    TglOwnedVpCscProducerContext& operator=(const TglOwnedVpCscProducerContext&)=delete;
    ~TglOwnedVpCscProducerContext() {
        if(applied_) {ief_=savedIef_;state_[0x88]=savedState_;}
    }
    bool valid() const noexcept {return valid_;}
    bool owns(void* child) const noexcept {return valid_ && child && child==child_;}
    void csc(void* state,void* ief) noexcept {
        if(state!=state_ || ief!=ief_.data()) {invalid_=true;return;}
        called_=true;
        if(!coefficients_ || applied_ || invalid_) return;
        const std::array<uintptr_t,3> pointers{
            reinterpret_cast<uintptr_t>(coefficients_->matrix.data()),
            reinterpret_cast<uintptr_t>(coefficients_->inputOffsets.data()),
            reinterpret_cast<uintptr_t>(coefficients_->outputOffsets.data())};
        state_[0x88]=1;ief_[0x20]=1;
        std::memcpy(ief_.data()+0x28,pointers.data(),sizeof(pointers));applied_=true;
    }
    int status() const noexcept {return valid_ && called_ && !invalid_?0:5;}
};
// Caller retains the VP/frame leases. Only callback invocation publishes;
// pointers remain borrowed until Use returns, then restoration precedes
// release of the cache lease. No native object or GPU lifetime is implied.
template<size_t StateSize,class Read,class Produce,class Use>
int tglWithOwnedVpCscProducerSpan(TglOwnedVpSfcCallerStorage& vp,std::array<uint8_t,64>& ief,
    uint8_t* state,TglOwnedVpCscCache& cache,uint32_t from,uint32_t to,
    uintptr_t sourceA8,Read read,Produce produce,Use use) noexcept {
    if(!state || reinterpret_cast<uintptr_t>(state)>UINTPTR_MAX-StateSize) return 5;
    auto invoke=[&](const TglCscCoefficients* coefficients) noexcept {
        using Context=TglOwnedVpCscProducerContext<StateSize>;
        using Resolve=TglOwnedVpSfcResolve<Context>;
        Context context(vp.bytes.data(),ief,state,coefficients);
        if(!context.valid()) return 5;
        TglOwnedVpSfcScopedContext<Context> scope(vp.bytes.data(),context);
        if(!scope.bound()) return 5;
        TglOwnedVpEntrySet entries;
        if(!tglBindOwnedVpCscEntry<Resolve>(vp.bytes.data(),entries)) return 5;
        static_assert(noexcept(use(vp.bytes.data(),entries.csc)),"VP CSC entry consumer ABI");
        const int result=use(vp.bytes.data(),entries.csc);
        return result?result:context.status();
    };
    if(!(vp.bytes[0x923]&1)) return invoke(nullptr);
    if(!cache.prepare(from,to,sourceA8,read,produce)) return 5;
    return cache.withCoefficients([&](const TglCscCoefficients& coefficients) noexcept {
        return invoke(&coefficients);
    });
}
template<size_t StateSize,class Read,class Produce,class Use>
int tglWithOwnedVpCscProducer(TglOwnedVpSfcCallerStorage& vp,std::array<uint8_t,64>& ief,
    std::array<uint8_t,StateSize>& state,TglOwnedVpCscCache& cache,uint32_t from,uint32_t to,
    uintptr_t sourceA8,Read read,Produce produce,Use use) noexcept {
    return tglWithOwnedVpCscProducerSpan<StateSize>(vp,ief,state.data(),cache,
        from,to,sourceA8,read,produce,use);
}
// Qualified VP +40=1e0ca0..1e0dcb. Its +a8=1e0c80 is a proven native
// no-op, not an unresolved hook. +90 failure prevents AVS/IEF; AVS failure
// prevents IEF. Native +98 is called for its side effect without a status read.
// Producers execute inside the complete frame/reference scope; this function
// neither creates resources nor substitutes for any of those producers.
// Exact slot+a8 (1e0c80..1e0c9f) saves four pointer arguments and returns
// zero without dereferencing them. Preserve that ABI only for our leased VP;
// an unmatched child is not admitted merely because this hook does no work.
template<class Resolve> struct TglOwnedVpPlatformSetupCallback {
    static int setup(void* child,void*,void*,void*) noexcept {
        static_assert(noexcept(Resolve{}(child)),"VP platform setup resolver ABI");
        auto* context=Resolve{}(child);
        static_assert(noexcept(context->owns(child)),"VP platform setup ownership ABI");
        return context && context->owns(child) ? 0 : 5;
    }
};
// VP+c8 caller1e0f1e..1e0f43 passes this/MHW/RenderHal+18/OS/output/command,
// then consumes EAX. Exact target5d2c0..5d2df saves those six arguments,
// dereferences none and returns0. This owned callback reproduces that ABI;
// it does not qualify the surrounding ICL object, output or command packets.
template<class Resolve> struct TglOwnedVpPlatformSendCallback {
    static int send(void* child,void*,void*,void*,void*,void*) noexcept {
        static_assert(noexcept(Resolve{}(child)),"VP platform send resolver ABI");
        auto* context=Resolve{}(child);
        static_assert(noexcept(context->owns(child)),"VP platform send ownership ABI");
        return context && context->owns(child) ? 0 : 5;
    }
};
template<class Resolve>
bool tglBindOwnedVpPlatformAdmissionEntries(void* child,TglOwnedVpEntrySet& target) noexcept {
    static_assert(noexcept(Resolve{}(child)),"VP platform binding resolver ABI");
    auto* context=Resolve{}(child);
    static_assert(noexcept(context->owns(child)),"VP platform binding ownership ABI");
    if(!context || !context->owns(child) || target.platformSetup || target.platformSend) return false;
    auto candidate=target;
    candidate.platformSetup=&TglOwnedVpPlatformSetupCallback<Resolve>::setup;
    candidate.platformSend=&TglOwnedVpPlatformSendCallback<Resolve>::send;
    target=candidate;return true;
}
template<class State,class Avs,class Ief>
int tglSetupOwnedVpSfcStages(const TglOwnedVpFramePreparation& frame,
                            State state,Avs avs,Ief ief) noexcept {
    static_assert(noexcept(state()) && noexcept(avs()) && noexcept(ief()),"VP setup producer ABI");
    static_assert(std::is_same_v<decltype(state()),int> && std::is_same_v<decltype(avs()),int> &&
                  std::is_same_v<decltype(ief()),void>,"VP setup result ABI");
    int result=state();
    if(result) return result;
    if(frame.regions.scale.scaling || frame.color.at9b8) {
        result=avs();if(result) return result;
    }
    if(frame.at922 || frame.color.modes.state.at923) ief();
    return result;
}
// Composed slot28 CPU preparation, before scoped references/publication.
// No resource allocation, borrowed surface writes, vtable or GPU submission.
inline bool tglPrepareOwnedVpFrame(const TglOwnedVpFramePreparationInput& in,
    const TglOwnedVpModeState& original,TglOwnedVpFramePreparation& output) noexcept {
    TglOwnedVpFramePreparation candidate;
    if(!tglPrepareOwnedVpRegions(in.geometry,candidate.regions) ||
       !tglPrepareOwnedVpFrameColor(in.modes,original,in.source120,in.source294,
        in.geometry.targetFormat,in.target294,candidate.color)) return false;
    const TglOwnedVpDestinationRect sourceDestination{
        in.geometry.destinationLeft,in.geometry.destinationTop,
        in.geometry.destinationRight,in.geometry.destinationBottom};
    candidate.at920=tglOwnedVpConditionalSource(in.conditionalSourceAvailable,
                                               sourceDestination,in.targetDestination);
    candidate.at922=in.iefPresent && (in.iefFlags&1) && in.iefStrength>0.0f;
    candidate.at924=in.modes.settings12;
    candidate.at9a0=in.geometry.rotation;
    output=candidate;return true;
}
// One bounded acquisition path for slot28. Optional IEF/conditional fields
// follow native branch order; Read must authorize pointed-to leases as well as
// parent descriptors. No partial frame or CSC reference escapes on failure.
template<class Read>
bool tglReadOwnedVpFramePreparation(uintptr_t source,uintptr_t target,uintptr_t mhw,
    uintptr_t settings,bool conditionalAvailable,Read read,const TglOwnedVpModeState& original,
    TglOwnedVpFramePreparation& output,uintptr_t& sourceA8,
    TglOwnedVpRegionInput* geometrySnapshot=nullptr) noexcept {
    static_assert(noexcept(read(uintptr_t{},nullptr,size_t{})),"VP frame leased read ABI");
    if(!source || !target || source>UINTPTR_MAX-0x298 || target>UINTPTR_MAX-0x298) return false;
    TglOwnedVpFramePreparationInput input;TglOwnedVpRegions regions;
    TglOwnedVpModeResult modes;uintptr_t reference=0;
    if(!tglReadOwnedVpRegions(source,target,mhw,read,input.geometry,regions) ||
       !tglReadOwnedVpModes(source,target,settings,read,original,input.modes,modes,reference) ||
       !read(source+0x120,&input.source120,4) || !read(source+0x294,&input.source294,4) ||
       !read(target+0x294,&input.target294,4)) return false;
    input.conditionalSourceAvailable=conditionalAvailable;
    if(conditionalAvailable && !read(target+0x40,&input.targetDestination,16)) return false;
    uintptr_t ief=0;
    if(!read(source+0x78,&ief,8)) return false;
    if(ief) {
        if(ief>UINTPTR_MAX-8 || !read(ief,&input.iefFlags,1)) return false;
        input.iefPresent=true;
        if((input.iefFlags&1) && !read(ief+4,&input.iefStrength,4)) return false;
    }
    TglOwnedVpFramePreparation candidate;
    if(!tglPrepareOwnedVpFrame(input,original,candidate)) return false;
    output=candidate;sourceA8=reference;
    // Setup and state generation must share the same leased descriptor read,
    // rather than reread potentially changing source/target geometry.
    if(geometrySnapshot) *geometrySnapshot=input.geometry;
    return true;
}
struct TglOwnedVpChromaStatePrefix {
    std::array<uint8_t,0xb8> packet{};
    uint32_t sourceSiting=0;
};
// Qualified +90 1df496..1df6a5: zero the owned packet then compose the
// existing exact input/output producers. IECP may update the owned siting
// value consumed by later AVS; this prefix is NOT a complete SFC state packet.
inline TglOwnedVpChromaStatePrefix tglPrepareOwnedVpChromaStatePrefix(
    const TglOwnedVpFramePreparation& frame,uint32_t targetFormat,
    bool settings0f,bool settings13) noexcept {
    TglOwnedVpChromaStatePrefix result;
    result.sourceSiting=frame.color.metadata.at9a8;
    tglPrepareOwnedSfcInputChroma(result.packet,result.sourceSiting,
                                frame.color.modes.state.at99c,settings0f,settings13);
    tglPrepareOwnedSfcOutputChroma(result.packet,frame.color.metadata.target294,targetFormat);
    return result;
}
// One synchronous slot28 view. Leases for source, parameters, target and CSC
// must enclose this call. Owned target metadata is supplied to use(), not
// written through the borrowed target. No vtable or object is published here.
template<class Use>
int tglWithOwnedVpPreparedFrame(TglOwnedVpSfcCallerStorage& storage,
    const TglOwnedVpFramePreparation& frame,uintptr_t conditionalSource,
    uintptr_t parameters,uintptr_t target,uintptr_t sourceA8,Use use) noexcept {
    static_assert(noexcept(use(static_cast<void*>(nullptr),frame.color.metadata)),"VP frame scope ABI");
    if(!target || (frame.at920 && !conditionalSource)) return 5;
    return tglWithOwnedVpFrameReferences(storage,conditionalSource,parameters,target,frame.at920,
        [&](void*) noexcept {
            return tglWithOwnedVpFrameColor(storage,frame.color,sourceA8,[&](void* child) noexcept {
                constexpr std::array<size_t,7> offsets{0x920,0x921,0x922,0x924,0x928,0x92c,0x9a0};
                constexpr std::array<size_t,7> lengths{1,1,1,1,4,4,4};
                std::array<std::array<uint8_t,4>,7> saved{};
                for(size_t i=0;i<offsets.size();++i)
                    std::memcpy(saved[i].data(),storage.bytes.data()+offsets[i],lengths[i]);
                storage.bytes[0x920]=frame.at920;
                storage.bytes[0x921]=frame.regions.scale.scaling;
                storage.bytes[0x922]=frame.at922;
                storage.bytes[0x924]=frame.at924;
                std::memcpy(storage.bytes.data()+0x928,&frame.regions.scale.at928,4);
                std::memcpy(storage.bytes.data()+0x92c,&frame.regions.scale.at92c,4);
                std::memcpy(storage.bytes.data()+0x9a0,&frame.at9a0,4);
                const int result=use(child,frame.color.metadata);
                for(size_t i=0;i<offsets.size();++i)
                    std::memcpy(storage.bytes.data()+offsets[i],saved[i].data(),lengths[i]);
                return result;
            });
        });
}
// Compose acquisition, owned frame publication and +40 producer ordering.
// The caller retains all descriptor/image leases for the complete call and
// supplies an authenticated settings view; this is not a published VP entry.
// State receives the SAME geometry snapshot used to prepare the frame.
template<class Read,class Invoke,class Use>
int tglWithOwnedVpAcquiredFrameEntry(TglOwnedVpSfcCallerStorage& storage,
    uintptr_t source,uintptr_t target,uintptr_t mhw,uintptr_t settings,
    uintptr_t conditionalSource,uintptr_t parameters,const TglOwnedVpModeState& original,
    Read read,Invoke invoke,Use use) noexcept {
    static_assert(noexcept(use(
        std::declval<const TglOwnedVpFramePreparation&>(),
        std::declval<const TglOwnedVpRegionInput&>(),uintptr_t{})),"VP acquired frame ABI");
    TglOwnedVpFramePreparation frame;TglOwnedVpRegionInput geometry;
    uintptr_t sourceA8=0;
    auto acquire=[&]() noexcept {
        return tglReadOwnedVpFramePreparation(source,target,mhw,settings,
            conditionalSource!=0,read,original,frame,sourceA8,&geometry);
    };
    struct FrameContext {
        void* child;
        std::array<uintptr_t,5> expected;
        decltype(acquire)& acquireFrame;
        bool called=false,valid=false;
        bool owns(void* value) const noexcept {return value==child;}
        void prepare(void* conditional,void* parameters,void* source,
            void* target,void* settings) noexcept {
            const std::array<uintptr_t,5> actual{
                reinterpret_cast<uintptr_t>(conditional),reinterpret_cast<uintptr_t>(parameters),
                reinterpret_cast<uintptr_t>(source),reinterpret_cast<uintptr_t>(target),
                reinterpret_cast<uintptr_t>(settings)};
            if(called || actual!=expected) {valid=false;called=true;return;}
            called=true;valid=acquireFrame();
        }
    } context{storage.bytes.data(),{conditionalSource,parameters,source,target,settings},acquire};
    {
        // The callback performs acquisition once under the caller's leases.
        // Do not leave a resolver active during downstream setup/send: those
        // callbacks bind their own contexts for the same owned child.
        TglOwnedVpSfcScopedContext<FrameContext> scope(context.child,context);
        if(!scope.bound()) return 5;
        using Resolve=TglOwnedVpSfcResolve<FrameContext>;
        TglOwnedVpEntrySet entries;
        if(!tglBindOwnedVpFrameEntry<Resolve>(context.child,entries)) return 5;
        static_assert(noexcept(invoke(context.child,entries.prepare)),"VP acquisition invocation ABI");
        const int result=invoke(context.child,entries.prepare);
        if(result) return result;
    }
    if(!context.called || !context.valid) return 5;
    return use(frame,geometry,sourceA8);
}
template<class Read,class Use>
int tglWithOwnedVpAcquiredFrame(TglOwnedVpSfcCallerStorage& storage,
    uintptr_t source,uintptr_t target,uintptr_t mhw,uintptr_t settings,
    uintptr_t conditionalSource,uintptr_t parameters,const TglOwnedVpModeState& original,
    Read read,Use use) noexcept {
    return tglWithOwnedVpAcquiredFrameEntry(storage,source,target,mhw,settings,
        conditionalSource,parameters,original,read,[&](void* child,auto prepare) noexcept {
            prepare(child,reinterpret_cast<void*>(conditionalSource),reinterpret_cast<void*>(parameters),
                reinterpret_cast<void*>(source),reinterpret_cast<void*>(target),reinterpret_cast<void*>(settings));
            return 0;
        },use);
}
template<class Read,class State,class Avs,class Ief,class Send>
int tglWithOwnedVpLeasedPass(TglOwnedVpSfcCallerStorage& storage,
    uintptr_t source,uintptr_t target,uintptr_t mhw,uintptr_t settings,
    uintptr_t conditionalSource,uintptr_t parameters,const TglOwnedVpModeState& original,
    Read read,State state,Avs avs,Ief ief,Send send) noexcept {
    static_assert(noexcept(send(static_cast<void*>(nullptr),
        std::declval<const TglOwnedVpFramePreparation&>(),
        std::declval<const TglOwnedVpRegionInput&>())),"VP scoped send ABI");
    return tglWithOwnedVpAcquiredFrame(storage,source,target,mhw,settings,
        conditionalSource,parameters,original,read,[&](const auto& frame,
            const auto& geometry,uintptr_t sourceA8) noexcept -> int {
    // Native 1decfb..1decff publishes incoming RDX, not the R9 view
    // consumed by the mode producer. Keep these independently leased inputs.
    return tglWithOwnedVpPreparedFrame(storage,frame,conditionalSource,parameters,target,sourceA8,
        [&](void* child,const TglOwnedVpColorMetadata&) noexcept {
            const int result=tglSetupOwnedVpSfcStages(frame,
                [&]() noexcept { return state(child,frame,geometry); },
                [&]() noexcept { return avs(child,frame); },
                [&]() noexcept { ief(child,frame); });
            if(result) return result;
            // Send must run before target/settings/CSC references and native
            // VP flags are restored. GPU resource retirement remains outside
            // this CPU-emission scope and still requires the actual fence.
            return send(child,frame,geometry);
        });
        });
}
template<class Read,class State,class Avs,class Ief>
int tglSetupOwnedVpLeasedFrame(TglOwnedVpSfcCallerStorage& storage,
    uintptr_t source,uintptr_t target,uintptr_t mhw,uintptr_t settings,
    uintptr_t conditionalSource,uintptr_t parameters,const TglOwnedVpModeState& original,
    Read read,State state,Avs avs,Ief ief) noexcept {
    return tglWithOwnedVpLeasedPass(storage,source,target,mhw,settings,
        conditionalSource,parameters,original,read,state,avs,ief,
        [](void*,const TglOwnedVpFramePreparation&,const TglOwnedVpRegionInput&) noexcept -> int {
            return 0;
        });
}
// Exact ICL base/Gen12 SFC +c0 slot is 1df3c0: a single DWORD zero
// at packet+4, not a no-op and not a GPU command producer.
template<size_t PacketBytes>
inline void tglPrepareOwnedSfcPlatformField(std::array<uint8_t,PacketBytes>& packet) noexcept {
    static_assert(PacketBytes>=0xb8,"platform packet requires complete native prefix");
    const uint32_t zero=0;
    std::memcpy(packet.data()+4,&zero,4);
}
// A +c0 producer may write only the exact owned packet leased by its caller,
// not any pointer passed by a native-looking object. Keep this context inside
// the VP/frame scope; status must be checked before later command emission.
class TglOwnedVpPlatformPacketContext {
    TglOwnedVpSfcCallerStorage& storage_;
    uint8_t* const packet_;
    bool applied_=false;
    int error_=0;
public:
    template<size_t PacketBytes>
    TglOwnedVpPlatformPacketContext(TglOwnedVpSfcCallerStorage& storage,
        std::array<uint8_t,PacketBytes>& packet) noexcept :storage_(storage),packet_(packet.data()) {
        static_assert(PacketBytes>=0xb8,"platform packet requires complete native prefix");
    }
    TglOwnedVpPlatformPacketContext(const TglOwnedVpPlatformPacketContext&)=delete;
    TglOwnedVpPlatformPacketContext& operator=(const TglOwnedVpPlatformPacketContext&)=delete;
    bool owns(void* child) const noexcept {return child==storage_.bytes.data();}
    void produce(void* packet) noexcept {
        if(error_) return;
        if(packet!=packet_) {error_=5;return;}
        const uint32_t zero=0;
        std::memcpy(packet_+4,&zero,4);applied_=true;
    }
    int status() const noexcept {return error_ ? error_ : applied_ ? 0 : 5;}
};
// Exact donor1df3c0 takes this, an ignored surface in RSI, packet in RDX;
// writes one DWORD at packet+4 and has void ABI. No donor code is invoked.
template<class Resolve> struct TglOwnedVpPlatformPacketCallback {
    static void produce(void* child,void*,void* packet) noexcept {
        static_assert(noexcept(Resolve{}(child)),"VP platform resolver ABI");
        auto* context=Resolve{}(child);
        static_assert(noexcept(context->owns(child)) && noexcept(context->produce(packet)),
            "VP platform producer ABI");
        if(context && context->owns(child)) context->produce(packet);
    }
};

template<class Resolve>
bool tglBindOwnedVpPlatformPacketEntry(void* child,TglOwnedVpEntrySet& target) noexcept {
    static_assert(noexcept(Resolve{}(child)),"VP platform packet binding resolver ABI");
    auto* context=Resolve{}(child);
    static_assert(noexcept(context->owns(child)),"VP platform packet binding ownership ABI");
    if(!context || !context->owns(child) || target.platformPacket) return false;
    auto candidate=target;
    candidate.platformPacket=&TglOwnedVpPlatformPacketCallback<Resolve>::produce;
    target=candidate;return true;
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
    std::memcpy(packet.data()+0x30,&frameHeight,4);
    std::memcpy(packet.data()+0x34,&frameWidth,4);
}

// SFC +60 is narrower than VEBOX's MMC predicate:1dd120 admits only
// 19/10/e. 1e0610 gates mode7, enabled low bit, surface299 low bit and e4==1.
// This computes metadata only; OS+3a8 invocation remains a separate ABI boundary.
inline bool tglOwnedVpCompressionFormat(uint32_t format) noexcept {
    return format==0x19 || format==0x10 || format==0xe;
}
template<class Resolve> struct TglOwnedVpCompressionFormatCallback {
    static bool supported(void* child,uint32_t format) noexcept {
        static_assert(noexcept(Resolve{}(child)),"VP compression resolver ABI");
        auto* context=Resolve{}(child);
        static_assert(noexcept(context->owns(child)),"VP compression ownership ABI");
        return context && context->owns(child) && tglOwnedVpCompressionFormat(format);
    }
};
inline bool tglPrepareOwnedSfcCompression(std::array<uint8_t,0xb8>& packet,
    uint32_t format,uint32_t executionMode,uint8_t mmcEnabled,
    uint8_t surfaceEnabled,uint32_t surfaceMode,float scaleY,float scaleX,
    bool& invokeOs) noexcept {
    invokeOs=false;
    if (!tglOwnedVpCompressionFormat(format) || executionMode!=7 ||
        !(mmcEnabled&1) || !(surfaceEnabled&1) || surfaceMode!=1) return true;
    if (!std::isfinite(scaleY) || !std::isfinite(scaleX) || scaleY<0 || scaleX<0) return false;
    const uint32_t mode=scaleY>=0.5f && scaleX>=0.5f ? 1u :
                        scaleY<0.5f && scaleX<0.5f ? 2u : 0u;
    packet[0x8b]=mode!=0;
    std::memcpy(packet.data()+0x8c,&mode,4);
    invokeOs=true; // native calls even when selected mode is zero
    return true;
}

struct TglOwnedVpCompressionInput {
    uint32_t format=0,executionMode=0,surfaceMode=0;
    uint8_t mmcEnabled=0,surfaceEnabled=0;
};
struct TglNativeVpCompressionInvoker {
    void operator()(uintptr_t entry,uintptr_t os,const void* resource,uint32_t mode) const noexcept {
        using Fn=int(*)(void*,const void*,uint32_t);
        (void)reinterpret_cast<Fn>(entry)(reinterpret_cast<void*>(os),resource,mode);
    }
};
// TGL66502/66509 installs65d00 at OS+3a8. Its exact19-byte body saves
// OS/resource/mode and returns0 without dereferencing any argument. Donor
// 1e0753..1e0779 ignores the result. Bind the TGL entry, never ICL code.
class TglNativeVpCompressionBinding {
    uintptr_t os_=0,entry_=0;
public:
    template<class Read,class Qualify,class Executable>
    bool resolve(uintptr_t os,uintptr_t image,Read read,Qualify qualify,Executable executable) {
        os_=entry_=0;
        constexpr std::array<uint8_t,19> anchor{
            0x55,0x48,0x89,0xe5,0x31,0xc0,0x48,0x89,0x7d,0xf8,
            0x48,0x89,0x75,0xf0,0x89,0x55,0xec,0x5d,0xc3};
        uintptr_t entry=0;std::array<uint8_t,19> actual{};
        if(!os || os>UINTPTR_MAX-0x3b0 || !image || image>UINTPTR_MAX-0x65d13 ||
           !qualify(image) || !read(os+0x3a8,&entry,8) || entry!=image+0x65d00 ||
           !executable(entry,anchor.size()) || !read(entry,actual.data(),actual.size()) ||
           actual!=anchor) return false;
        os_=os;entry_=entry;return true;
    }
    // Enclosing caller pins OS/image/output through this synchronous scope.
    // Backing presence is not GPU completion or asynchronous lifetime evidence.
    template<class Surface,class Use,class Invoke=TglNativeVpCompressionInvoker>
    int withOutput(const Surface& surface,Use use,Invoke invoke=Invoke{}) const noexcept {
        const auto* bytes=tglOwnedSfcSurfaceBytes(surface);
        std::array<uint8_t,0x148> backing{};
        std::memcpy(backing.data(),bytes+0x148,backing.size());
        if(!os_ || !entry_ || !tglNativeBackingPresent(backing)) return 5;
        auto apply=[&](uint32_t mode) noexcept {invoke(entry_,os_,bytes+0x148,mode);};
        static_assert(noexcept(use(apply)) && noexcept(invoke(uintptr_t{},uintptr_t{},
            static_cast<const void*>(nullptr),uint32_t{})),"compression output lease ABI");
        return use(apply);
    }
};
// +d0 reads the OUTPUT surface, not the effective input format +99c.
// Short-circuit reads follow 1e0637..1e069c; leases enclose all reads/use.
template<class Read>
bool tglReadOwnedVpCompression(uintptr_t settings,uintptr_t target,Read read,
    TglOwnedVpCompressionInput& output) noexcept {
    static_assert(noexcept(read(uintptr_t{},nullptr,size_t{})),"VP compression leased read ABI");
    if(!target || target>UINTPTR_MAX-0x134) return false;
    TglOwnedVpCompressionInput candidate;
    if(!read(target+0x130,&candidate.format,4)) return false;
    if(candidate.format==0x19 || candidate.format==0x10 || candidate.format==0xe) {
        if(!settings || settings>UINTPTR_MAX-0xd2d ||
           !read(settings+0xd28,&candidate.executionMode,4)) return false;
        if(candidate.executionMode==7) {
            if(!read(settings+0xd2c,&candidate.mmcEnabled,1)) return false;
            if(candidate.mmcEnabled&1) {
                if(target>UINTPTR_MAX-0x29a ||
                   !read(target+0x299,&candidate.surfaceEnabled,1)) return false;
                if((candidate.surfaceEnabled&1) &&
                   !read(target+0xe4,&candidate.surfaceMode,4)) return false;
            }
        }
    }
    output=candidate;return true;
}
// Native +d0 ignores OS+3a8's return and returns zero. Bind the authenticated
// OS/surface+148 ABI upstream as a void callback; even mixed-scale mode0 must
// call it. Packet publication is atomic, but external OS effects are not undone.
template<class Apply>
bool tglPrepareOwnedVpCompression(std::array<uint8_t,0xb8>& packet,
    const TglOwnedVpCompressionInput& input,const TglOwnedVpScale& scale,
    Apply apply) noexcept {
    static_assert(noexcept(apply(uint32_t{})) &&
                  std::is_same_v<decltype(apply(uint32_t{})),void>,"VP compression OS ABI");
    auto candidate=packet;bool invoke=false;
    if(!tglPrepareOwnedSfcCompression(candidate,input.format,input.executionMode,
           input.mmcEnabled,input.surfaceEnabled,input.surfaceMode,
           scale.at92c,scale.at928,invoke)) return false;
    if(invoke) {
        uint32_t mode=0;std::memcpy(&mode,candidate.data()+0x8c,4);apply(mode);
    }
    packet=candidate;return true;
}

// Complete owned +d0 CPU entry. All inputs are identity-bound to an enclosing
// VP/output/packet lease; raw native arguments cannot select arbitrary memory.
template<class Surface,class Read,class Invoke=TglNativeVpCompressionInvoker,size_t PacketBytes=0xb8>
class TglOwnedVpCompressionContext {
    static_assert(PacketBytes>=0xb8,"compression producer requires complete native prefix");
    TglOwnedVpSfcCallerStorage& vp_;
    const Surface& output_;
    std::array<uint8_t,PacketBytes>& packet_;
    uintptr_t settings_;
    const TglNativeVpCompressionBinding& binding_;
    Read read_;
    Invoke invoke_;
    bool busy_=false;
public:
    TglOwnedVpCompressionContext(TglOwnedVpSfcCallerStorage& vp,const Surface& output,
        std::array<uint8_t,PacketBytes>& packet,uintptr_t settings,
        const TglNativeVpCompressionBinding& binding,Read read,Invoke invoke=Invoke{}) noexcept
        :vp_(vp),output_(output),packet_(packet),settings_(settings),binding_(binding),
         read_(read),invoke_(invoke){}
    TglOwnedVpCompressionContext(const TglOwnedVpCompressionContext&)=delete;
    TglOwnedVpCompressionContext& operator=(const TglOwnedVpCompressionContext&)=delete;
    bool owns(void* child) const noexcept {return child==vp_.bytes.data();}
    int compression(void* settings,void* target,void* packet) noexcept {
        if(busy_ || !settings_ || reinterpret_cast<uintptr_t>(settings)!=settings_ ||
           target!=tglOwnedSfcSurfaceBytes(output_) || packet!=packet_.data()) return 5;
        busy_=true;
        struct Unlock {bool& busy;~Unlock(){busy=false;}} unlock{busy_};
        TglOwnedVpCompressionInput input;
        if(!tglReadOwnedVpCompression(settings_,reinterpret_cast<uintptr_t>(target),read_,input)) return 5;
        float scaleX=0,scaleY=0;
        std::memcpy(&scaleX,vp_.bytes.data()+0x928,4);
        std::memcpy(&scaleY,vp_.bytes.data()+0x92c,4);
        std::array<uint8_t,0xb8> candidate{};
        std::memcpy(candidate.data(),packet_.data(),candidate.size());bool invokeOs=false;
        if(!tglPrepareOwnedSfcCompression(candidate,input.format,input.executionMode,
            input.mmcEnabled,input.surfaceEnabled,input.surfaceMode,scaleY,scaleX,invokeOs)) return 5;
        if(invokeOs) {
            const int status=binding_.withOutput(output_,[&](auto apply) noexcept {
                uint32_t mode=0;std::memcpy(&mode,candidate.data()+0x8c,4);apply(mode);return 0;
            },invoke_);
            if(status) return status;
        }
        // Preserve the TGL f8 tail: the donor producer owns only the b8 prefix.
        std::memcpy(packet_.data(),candidate.data(),candidate.size());return 0;
    }
};
template<class Resolve> struct TglOwnedVpCompressionCallback {
    static int compression(void* child,void* settings,void* target,void* packet) noexcept {
        static_assert(noexcept(Resolve{}(child)),"VP compression scoped resolver ABI");
        auto* context=Resolve{}(child);
        return context && context->owns(child) ? context->compression(settings,target,packet) : 5;
    }
};

template<class Resolve>
bool tglBindOwnedVpCompressionEntry(void* child,TglOwnedVpEntrySet& target) noexcept {
    static_assert(noexcept(Resolve{}(child)),"VP compression binding resolver ABI");
    auto* context=Resolve{}(child);
    static_assert(noexcept(context->owns(child)),"VP compression binding ownership ABI");
    if(!context || !context->owns(child) || target.compression) return false;
    auto candidate=target;
    candidate.compression=&TglOwnedVpCompressionCallback<Resolve>::compression;
    target=candidate;return true;
}
// Caller retains VP, output and packet backing throughout this CPU-only
// assembly. Both resolver scopes expire before returning to that caller.
template<class Surface,size_t PacketBytes,class Read,class Invoke,class Use>
int tglWithOwnedVpPlatformCompressionAssembly(TglOwnedVpSfcCallerStorage& vp,
    const Surface& output,std::array<uint8_t,PacketBytes>& packet,uintptr_t settings,
    const TglNativeVpCompressionBinding& binding,Read read,Invoke invoke,
    const TglOwnedVpEntrySet& base,Use use) noexcept {
    if(base.platformSetup || base.platformPacket || base.platformSend || base.compression) return 5;
    const uintptr_t packetAddress=reinterpret_cast<uintptr_t>(packet.data());
    const uintptr_t vpAddress=reinterpret_cast<uintptr_t>(vp.bytes.data());
    const uintptr_t outputAddress=reinterpret_cast<uintptr_t>(tglOwnedSfcSurfaceBytes(output));
    if(packetAddress>UINTPTR_MAX-PacketBytes || vpAddress>UINTPTR_MAX-vp.bytes.size() ||
       outputAddress>UINTPTR_MAX-0x2a8) return 5;
    if((packetAddress<vpAddress+vp.bytes.size() && vpAddress<packetAddress+PacketBytes) ||
       (packetAddress<outputAddress+0x2a8 && outputAddress<packetAddress+PacketBytes)) return 5;
    using Compression=TglOwnedVpCompressionContext<Surface,Read,Invoke,PacketBytes>;
    TglOwnedVpPlatformPacketContext platform(vp,packet);
    Compression compression(vp,output,packet,settings,binding,read,invoke);
    void* child=vp.bytes.data();
    using PlatformResolve=TglOwnedVpSfcResolve<TglOwnedVpPlatformPacketContext>;
    using CompressionResolve=TglOwnedVpSfcResolve<Compression>;
    TglOwnedVpSfcScopedContext<TglOwnedVpPlatformPacketContext> platformScope(child,platform);
    TglOwnedVpSfcScopedContext<Compression> compressionScope(child,compression);
    if(!platformScope.bound() || !compressionScope.bound()) return 5;
    TglOwnedVpEntrySet additions;
    if(!tglBindOwnedVpPlatformAdmissionEntries<PlatformResolve>(child,additions) ||
       !tglBindOwnedVpPlatformPacketEntry<PlatformResolve>(child,additions) ||
       !tglBindOwnedVpCompressionEntry<CompressionResolve>(child,additions)) return 5;
    auto combined=base;
    if(!combined.appendDisjoint(additions)) return 5;
    static_assert(noexcept(use(child,combined)),"VP platform/compression assembly ABI");
    return use(child,combined);
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

// +90 geometry after the chroma prefix. frameWidth/Height already came from
// +50: never run the frame adjuster again (field input would be doubled twice).
// Native +80 consumes effective VP+99c; packet+38 consumes VP+9a4.
// This remains a partial state packet, not a publishable SFC command.
inline bool tglPrepareOwnedVpStateGeometry(TglOwnedVpChromaStatePrefix& state,
    const TglOwnedVpFramePreparation& frame,const TglOwnedVpRegionInput& in) noexcept {
    auto truncate=[](float value,uint32_t& out) noexcept {
        if(!std::isfinite(value) || value<0 || double(value)>double(UINT32_MAX)) return false;
        out=static_cast<uint32_t>(value);return true;
    };
    uint32_t top=0,left=0,bottom=0,right=0,destTop=0,destLeft=0;
    if(!truncate(in.sourceTop,top) || !truncate(in.sourceLeft,left) ||
       !truncate(in.sourceBottom,bottom) || !truncate(in.sourceRight,right) ||
       !truncate(in.destinationTop,destTop) || !truncate(in.destinationLeft,destLeft)) return false;
    if(in.fieldInput) {
        if(bottom>UINT32_MAX/2 || right>UINT32_MAX/2) return false;
        bottom*=2;right*=2;
    }
    const auto input=tglOwnedSfcAlignment(frame.color.modes.state.at99c);
    const auto output=tglOwnedSfcAlignment(in.targetFormat);
    auto candidate=state.packet;
    tglPrepareOwnedSfcFrameFields(candidate,in.targetFormat,frame.color.metadata.at9a4,
                                  in.frameWidth,in.frameHeight);
    if(!tglPrepareOwnedSfcAlignedGeometry(candidate,in.targetHeight,in.targetWidth,top,left,
           output.height,output.width,input.height,input.width) ||
       !tglPrepareOwnedSfcRegions(candidate,bottom,right,in.sourceTop,in.sourceLeft,
           in.frameHeight,in.frameWidth,frame.regions.scale.at92c,frame.regions.scale.at928,
           input.height,input.width,output.height,output.width) ||
       !tglPrepareOwnedSfcRotationGeometry(candidate,frame.at9a0,destTop,destLeft,
           output.height,output.width)) return false;
    state.packet=candidate;return true;
}

// Closed +90 prefix through 1dff21 only. Both format predicates consume
// effective +99c, and AVS enable is +921 OR +9b8, not the IEF flag +922.
// Fill/alpha/compression/resource publication remain separate boundaries.
inline bool tglPrepareOwnedVpStatePrefix(
    const TglOwnedVpFramePreparation& frame,const TglOwnedVpRegionInput& geometry,
    bool settings0f,bool settings13,TglOwnedVpChromaStatePrefix& output) noexcept {
    auto candidate=tglPrepareOwnedVpChromaStatePrefix(frame,geometry.targetFormat,
                                                     settings0f,settings13);
    tglPrepareOwnedSfcPlatformField(candidate.packet);
    if(!tglPrepareOwnedVpStateGeometry(candidate,frame,geometry) ||
       !tglPrepareOwnedSfcBypass(candidate.packet,frame.color.modes.state.at99c,
           frame.regions.scale.at928,frame.regions.scale.at92c)) return false;
    tglPrepareOwnedSfcFilterRotation(candidate.packet,frame.color.modes.state.at99c,
        frame.at9a0,frame.regions.scale.scaling,frame.color.at9b8);
    output=candidate;return true;
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
// Owned snapshot of the conditional-source +4/+8 fill metadata and optional
// alpha object. Acquisition/leases and the native CSC binding are upstream;
// this CPU composition does not authorize resource publication or GPU submit.
struct TglOwnedVpFillAlphaInput {
    std::array<uint8_t,4> sample{};
    uint32_t fillColorSpace=0,alphaMode=0;
    float constantAlpha=1;
    bool hasAlpha=false;
};
// +988 is the conditional-source object (sample DWORD+4/color space+8),
// +990 is the optional alpha object (constant float+0/mode DWORD+4).
// Caller-held leases must enclose both this snapshot and its producer use.
template<class Read>
bool tglReadOwnedVpFillAlpha(bool fillEnabled,uintptr_t conditionalSource,
    uintptr_t alpha,Read read,TglOwnedVpFillAlphaInput& output) noexcept {
    static_assert(noexcept(read(uintptr_t{},nullptr,size_t{})),"VP fill/alpha leased read ABI");
    TglOwnedVpFillAlphaInput candidate;
    if(fillEnabled) {
        if(!conditionalSource || conditionalSource>UINTPTR_MAX-12 ||
           !read(conditionalSource+4,candidate.sample.data(),4) ||
           !read(conditionalSource+8,&candidate.fillColorSpace,4)) return false;
    }
    if(alpha) {
        if(alpha>UINTPTR_MAX-8 || !read(alpha+4,&candidate.alphaMode,4)) return false;
        candidate.hasAlpha=true;
        if(candidate.alphaMode==0 && !read(alpha,&candidate.constantAlpha,4)) return false;
    }
    output=candidate;return true;
}
template<class Convert>
bool tglPrepareOwnedVpFillAlpha(TglOwnedVpChromaStatePrefix& state,
    TglOwnedSfcFillCache& cache,const TglOwnedVpFramePreparation& frame,
    uint32_t outputFormat,uint32_t outputColorSpace,uint32_t sourceColorSpace,
    const TglOwnedVpFillAlphaInput& input,Convert convert) noexcept {
    auto candidate=state.packet;
    auto candidateCache=cache;
    if(!candidateCache.prepare(candidate,frame.at920,outputFormat,input.sample,
            input.fillColorSpace,outputColorSpace,convert)) return false;
    tglPrepareOwnedSfcAlpha(candidate,outputFormat,input.hasAlpha,input.alphaMode,
        input.constantAlpha,frame.at920,frame.color.modes.state.at923,sourceColorSpace);
    state.packet=candidate;cache=candidateCache;return true;
}

// Complete CPU scalar composition through +d0, BEFORE +58 resource allocation
// and the final surface query/publication. This is the reference VP packet,
// NOT a qualified TGL Gen12 pass. In particular, Gen12 MMC modes and filter
// semantics still require the separate audited Gen12 producer.
template<class Convert,class Apply>
bool tglPrepareOwnedVpCpuState(const TglOwnedVpFramePreparation& frame,
    const TglOwnedVpRegionInput& geometry,bool settings0f,bool settings13,
    const TglOwnedVpFillAlphaInput& fillAlpha,uint32_t outputColorSpace,
    uint32_t sourceColorSpace,const TglOwnedVpCompressionInput& compression,
    TglOwnedSfcFillCache& cache,Convert convert,Apply apply,
    TglOwnedVpChromaStatePrefix& output) noexcept {
    // Reject mixed snapshots before any CSC/OS effects.
    if(compression.format!=geometry.targetFormat) return false;
    TglOwnedVpChromaStatePrefix candidate;auto candidateCache=cache;
    if(!tglPrepareOwnedVpStatePrefix(frame,geometry,settings0f,settings13,candidate) ||
       !tglPrepareOwnedVpFillAlpha(candidate,candidateCache,frame,geometry.targetFormat,
            outputColorSpace,sourceColorSpace,fillAlpha,convert) ||
       !tglPrepareOwnedVpCompression(candidate.packet,compression,frame.regions.scale,apply))
        return false;
    output=candidate;cache=candidateCache;return true;
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

// Outer12d280 obtains VEBOX child from renderer910[index]. 12d38b..12d39d
// passes child/params/pass, then12d3a0 tests AL bit0. This is bool admission,
// NOT an int status producer (error5 would be interpreted as TRUE). The scoped
// context must implement the full platform admission and own params/pass leases;
// this ABI bridge alone is not a completed admission policy or runtime table.
// Exact donor secondary794508+28=1fbf30 adjusts via vptr[-38] to1fb950.
// Full native flow (do not transplant ICL this adjustment): prerequisites
// OS/RenderHal/pass10; params138/SKU bit12/source d8>=64,dc>=16, otherwise
// pass08=1 and FALSE. Execution+10 reset; optional VP+10 reset; owned+68
// selects execution a0c, publishes child1ba9->execution d2c and params10c
// ->execution d28; +180/+170 and feature query can change admission. For
// pipe1, VP+28 consumes params e0/f0, source,target,execution before format/
// DN flag publication. sample124==2 plus failed admission forces pipe0 and
// pass08=1. Final params138 && pass08 forces FALSE. Return1fbf1c normalizes
// AL; local status5 writes on prerequisite failure are NOT returned as bool.
// Thus direct geometry/format predicates cannot stand in for the full policy,
// and a denied admission need not mean that execution/pass stayed unchanged.
// Native12b1d0..12b5e4 (+180) uses base90 mode/history, base1a8 radius,
// base1b78/1b7c previous frame IDs and source b0/b4/b8/c0/c8 links.
// It makes no calls: it mutates history+4/+8 and sometimes base1b5c=4.
// Nonzero history mode at12b498 dereferences source c0 unconditionally;
// count/pointer validation must precede any retained native invocation.
// Its signed comparisons follow 32-bit SUB, not widened signed arithmetic.
// This helper is only that comparison, NOT the complete history transition.
inline bool tglVeboxHistoryWithinWindow(uint32_t frame,uint32_t previous,
                                       uint32_t radius) noexcept {
    const uint32_t deltaBits=frame-previous,lowerBits=uint32_t{0}-radius;
    int32_t delta=0,lower=0,upper=0;
    std::memcpy(&delta,&deltaBits,4);
    std::memcpy(&lower,&lowerBits,4);
    std::memcpy(&upper,&radius,4);
    return delta>=lower && delta<=upper;
}
struct TglVeboxHistoryState {
    uint32_t mode=0,previous1b78=0,previous1b7c=0,radius=0,field1b5c=0;
    uint8_t enabled=0,disabled=0,alternate=0;
};
// ICL1f7b30..1f7b48 compares the zero-based FFDi index against signed
// base1b5c. Consume the existing history producer, not a fixed fixture count.
// Native nonpositive counts execute no slots; reject positive counts beyond
// the caller's real owner capacity without changing its output.
inline bool tglOwnedFfdiCountFromHistory(size_t& output,
    const TglVeboxHistoryState& state,size_t capacity) noexcept {
    int32_t nativeCount=0;
    std::memcpy(&nativeCount,&state.field1b5c,sizeof(nativeCount));
    const size_t count=nativeCount>0 ? static_cast<size_t>(nativeCount) : 0;
    if(count>capacity) return false;
    output=count;return true;
}
// Exact mode0 sample publication12c1e2..12c266, separate from180/history
// mode selection. allocated means +58 actually returned success, not merely
// an allocation decision. previousId is an owned, leased snapshot, never a
// native linked surface pointer. Preserve the native partial side effect:
// missing required previous returns5 AFTER updating current when allocated.
inline int tglPublishVeboxMode0SampleIds(TglVeboxHistoryState& state,bool allocated,
    uint32_t currentId,uint8_t report08,uint8_t report09,uint32_t pipe,
    const uint32_t* previousId) noexcept {
    if(allocated) state.previous1b78=currentId;
    if(report08&1) {
        if(!previousId) return 5;
        state.previous1b7c=*previousId;
    } else if(!(report09&1) || pipe!=1) state.previous1b7c=UINT32_MAX;
    return 0;
}
struct TglVeboxHistoryFrame {
    uint32_t id=0,countB4=0,countB8=0;
    const uint32_t* linkedC0=nullptr;
    const uint32_t* linkedC8=nullptr;
    bool has88=false;
};
// Complete CPU transition12b1d0..12b5e4. Inputs are snapshots acquired under
// frame/history leases, NOT native pointer adoption. Missing required C0
// fails before publication instead of reproducing the native null dereference.
inline bool tglUpdateVeboxHistory(TglVeboxHistoryState& state,
    const TglVeboxHistoryFrame& source,uint32_t pipe) noexcept {
    auto next=state;
    if(!(next.enabled&1)) return true;
    if((next.disabled&1) || pipe!=0) next.mode=0;
    else if(!source.countB4) {
        if(next.mode==2) { next.mode=3;next.alternate=(~next.alternate)&1; }
        if(next.mode==3) next.mode=0;
    } else if(!next.mode) {
        next.field1b5c=4;
        const bool linked=source.countB8 && source.linkedC8;
        const bool matches=linked &&
            tglVeboxHistoryWithinWindow(source.id,next.previous1b78,next.radius) &&
            tglVeboxHistoryWithinWindow(*source.linkedC8,next.previous1b7c,next.radius);
        const bool discontinuous=linked &&
            !tglVeboxHistoryWithinWindow(*source.linkedC8,next.previous1b78,next.radius);
        if(!matches && !discontinuous) next.mode=1;
    } else {
        if(!source.linkedC0) return false;
        const bool matches=
            tglVeboxHistoryWithinWindow(*source.linkedC0,next.previous1b78,next.radius) &&
            tglVeboxHistoryWithinWindow(source.id,next.previous1b7c,next.radius);
        const bool discontinuous=
            !tglVeboxHistoryWithinWindow(source.id,next.previous1b78,next.radius);
        if((!matches && discontinuous) || (matches && !source.has88)) next.mode=0;
    }
    state=next;return true;
}
// Caller holds surface/history owners throughout this synchronous CPU stage.
// Native links must equal the provided owned shells; never follow opaque C0/C8.
template<class Owner,size_t N>
bool tglUpdateOwnedVeboxHistory(Owner& owner,void* child,
    std::array<uint8_t,N>& history,const TglOwnedSurfaceStorage& source,
    const TglOwnedSurfaceStorage* c0,const TglOwnedSurfaceStorage* c8,
    uint32_t pipe) noexcept {
    static_assert(N>=11,"native history flags through +a");
    if(!child || !owner.hasPrivateChildLease(reinterpret_cast<uintptr_t>(child))) return false;
    auto* base=static_cast<uint8_t*>(child);
    uintptr_t linked=0,p0=0,p8=0;
    std::memcpy(&linked,base+0x90,8);
    if(linked!=reinterpret_cast<uintptr_t>(history.data())) return false;
    std::memcpy(&p0,source.prefix.data()+0xc0,8);
    std::memcpy(&p8,source.prefix.data()+0xc8,8);
    if(p0!=reinterpret_cast<uintptr_t>(c0) || p8!=reinterpret_cast<uintptr_t>(c8)) return false;
    TglVeboxHistoryState state;
    state.enabled=history[0];state.disabled=history[0xa];state.alternate=history[8];
    std::memcpy(&state.mode,history.data()+4,4);
    std::memcpy(&state.radius,base+0x1a8,4);
    std::memcpy(&state.previous1b78,base+0x1b78,4);
    std::memcpy(&state.previous1b7c,base+0x1b7c,4);
    std::memcpy(&state.field1b5c,base+0x1b5c,4);
    TglVeboxHistoryFrame frame;uint32_t id0=0,id8=0;
    std::memcpy(&frame.id,source.prefix.data()+0xb0,4);
    std::memcpy(&frame.countB4,source.prefix.data()+0xb4,4);
    std::memcpy(&frame.countB8,source.prefix.data()+0xb8,4);
    uintptr_t feature=0;std::memcpy(&feature,source.prefix.data()+0x88,8);
    frame.has88=feature!=0;
    if(c0) { std::memcpy(&id0,c0->prefix.data()+0xb0,4);frame.linkedC0=&id0; }
    if(c8) { std::memcpy(&id8,c8->prefix.data()+0xb0,4);frame.linkedC8=&id8; }
    if(!tglUpdateVeboxHistory(state,frame,pipe)) return false;
    std::memcpy(history.data()+4,&state.mode,4);history[8]=state.alternate;
    std::memcpy(base+0x1b5c,&state.field1b5c,4);
    return true;
}
// Enclose feature preparation AND every synchronous consumer of report60 in
// one frame lease. Native160 retains target there; clear it before either
// surface owner unlocks. This is not permission for asynchronous GPU submission.
// Callback supplies the qualified native/owned stages; this scope alone is
// neither complete admission nor authorization to call an incomplete vtable.
template<class SurfaceOwner,class ChildOwner,class ExecutionOwner,class Invoke>
int tglRunCapturedVeboxFeature(SurfaceOwner& sourceOwner,SurfaceOwner& targetOwner,
    ChildOwner& childOwner,ExecutionOwner& executionOwner,void* child,uintptr_t execution,
    const TglOwnedSurfaceStorage& source,const TglOwnedSurfaceStorage& target,Invoke invoke) noexcept {
    static_assert(noexcept(invoke(child,source,target)) &&
        std::is_same_v<decltype(invoke(child,source,target)),int>,"captured feature status ABI");
    if(!child || !sourceOwner.hasSurfaceLease(source) || !targetOwner.hasSurfaceLease(target) ||
       !childOwner.hasPrivateChildLease(reinterpret_cast<uintptr_t>(child)) ||
       !executionOwner.hasExecutionLease(execution)) return 5;
    uintptr_t linked=0;std::memcpy(&linked,static_cast<uint8_t*>(child)+0x88,8);
    if(linked!=execution) return 5;
    return executionOwner.withLeasedFeatureReport(execution,&target,[&]() noexcept {
        const int status=invoke(child,source,target);
        std::memcpy(&linked,static_cast<uint8_t*>(child)+0x88,8);
        if(linked!=execution || !sourceOwner.hasSurfaceLease(source) ||
           !targetOwner.hasSurfaceLease(target) ||
           !childOwner.hasPrivateChildLease(reinterpret_cast<uintptr_t>(child))) return 5;
        return status;
    });
}
template<class SurfaceOwner,class ChildOwner,class ExecutionOwner,class Invoke>
int tglWithOwnedVeboxFeatureFrame(SurfaceOwner& sourceOwner,SurfaceOwner& targetOwner,
    ChildOwner& childOwner,ExecutionOwner& executionOwner,Invoke invoke) noexcept {
    static_assert(noexcept(invoke(nullptr,std::declval<const TglOwnedSurfaceStorage&>(),
        std::declval<const TglOwnedSurfaceStorage&>())),"feature frame callback ABI");
    int result=5;
    const bool leased=sourceOwner.withPeerSurface(targetOwner,
        [&](const TglOwnedSurfaceStorage& source,const TglOwnedSurfaceStorage& target) noexcept {
        result=childOwner.withPrivateChild([&](void* child) noexcept {
            return executionOwner.withExecution([&](uintptr_t execution) noexcept {
                return tglRunCapturedVeboxFeature(sourceOwner,targetOwner,childOwner,executionOwner,
                    child,execution,source,target,invoke);
            });
        });
    });
    return leased ? result : 5;
}
// Unified synchronous feature transaction. Native170 may consume tracked
// source90, and160 retains target in report60. Reconcile source90 while both
// surface owners are still pinned; detach report60 before either owner unlocks.
// Invoke must include all synchronous report consumers, not an async submit.
template<class SurfaceOwner,class ChildOwner,class ExecutionOwner,class Parameters,class Invoke>
int tglWithOwnedVeboxDenoiseFeatureFrame(SurfaceOwner& sourceOwner,SurfaceOwner& targetOwner,
    ChildOwner& childOwner,ExecutionOwner& executionOwner,Parameters& parameters,Invoke invoke) noexcept {
    static_assert(noexcept(invoke(nullptr,std::declval<const TglOwnedSurfaceStorage&>(),
        std::declval<const TglOwnedSurfaceStorage&>())) &&
        std::is_same_v<decltype(invoke(nullptr,std::declval<const TglOwnedSurfaceStorage&>(),
        std::declval<const TglOwnedSurfaceStorage&>())),int>,"complete synchronous feature status ABI");
    return tglWithOwnedVeboxFeatureFrame(sourceOwner,targetOwner,childOwner,executionOwner,
        [&](void* child,const auto& source,const auto& target) noexcept {
            int result=5;
            const bool reconciled=sourceOwner.withNativeDenoiseParameters(source,parameters,
                [&]() noexcept {result=invoke(child,source,target);});
            return reconciled?result:5;
        });
}

enum class TglVeboxAdmissionPrefixResult : uint8_t {
    MissingDependencies, CompositionFallback, Continue
};
struct TglVeboxAdmissionPrefixInput {
    bool os=false,renderHal=false,execution=false,source=false;
    uint8_t params138=0;
    uint64_t sku0=0;
    uint32_t sourceWidth=0,sourceHeight=0;
};
// CPU counterpart of donor1fb974..1fba92 only. Field acquisition and exact
// Gen12 platform qualification remain the enclosing policy's responsibility.
// source means pass10 (native's a8), not params98. Owned execution must also
// be leased before entry; this extra safety gate is not a native NULL check.
// Missing dependencies leave pass untouched; fallback writes the whole byte1
// just as native1fba35/1fba62/1fba89 (not OR with an old flag). Continue does
// not clear prior pass state: execution reset/mode selection still follow.
// Only pass08 participates in this CPU policy. The enclosing authenticated
// frame owns and pins this byte; no opaque pass object is copied or adopted.
class TglVeboxAdmissionPassByte {
    uint8_t& flags_;
public:
    explicit TglVeboxAdmissionPassByte(uint8_t& flags) noexcept:flags_(flags){}
    TglVeboxAdmissionPassByte(const TglVeboxAdmissionPassByte&)=delete;
    uint8_t& flags() noexcept {return flags_;}
};
inline uint8_t& tglAdmissionPassFlags(TglVeboxAdmissionPassByte& pass) noexcept {
    return pass.flags();
}
template<size_t N> uint8_t& tglAdmissionPassFlags(std::array<uint8_t,N>& pass) noexcept {
    static_assert(N>=9,"native admission requires pass08");return pass[8];
}
template<class Pass> struct TglAdmissionPassExtent;
template<size_t N> struct TglAdmissionPassExtent<std::array<uint8_t,N>> {
    static constexpr size_t value=N;
};
template<> struct TglAdmissionPassExtent<TglVeboxAdmissionPassByte> {
    static constexpr size_t value=9;
};
template<class Pass>
TglVeboxAdmissionPrefixResult tglPrepareVeboxAdmissionPrefix(
    const TglVeboxAdmissionPrefixInput& input,Pass& pass) noexcept {
    static_assert(TglAdmissionPassExtent<Pass>::value>=9,"native admission requires pass08");
    if(!input.os || !input.renderHal || !input.execution || !input.source)
        return TglVeboxAdmissionPrefixResult::MissingDependencies;
    if(!(input.params138&1) || !(input.sku0&(uint64_t(1)<<12)) ||
       input.sourceWidth<64 || input.sourceHeight<16) {
        tglAdmissionPassFlags(pass)=1;
        return TglVeboxAdmissionPrefixResult::CompositionFallback;
    }
    return TglVeboxAdmissionPrefixResult::Continue;
}

// Preserve donor1fbb08..1fbb68 ordering: +180 observes the NEW output pipe
// and MMC flag, but OLD d28. Only after that void stage returns is params10c
// published at d28. This is metadata, not permission to call an unqualified
// native +180 or evidence of command completion. Enclosing owner pins storage.
template<class Execution,class Setup>
bool tglPublishVeboxAdmissionMode(Execution& execution,
    uint32_t pipe,uint8_t mmc,uint32_t params10c,Setup setup) noexcept {
    static_assert(TglAdmissionExecutionExtent<Execution>::value>=0xd2d,"complete admission metadata extent");
    static_assert(noexcept(setup(execution)) &&
        std::is_same_v<decltype(setup(execution)),void>,"native +180 void stage");
    if(pipe>2) return false;
    std::memcpy(execution.data()+0xa0c,&pipe,4);
    execution[0xd2c]=mmc&1;
    setup(execution);
    std::memcpy(execution.data()+0xd28,&params10c,4);
    return true;
}

// donor1fbbd6..1fbc11: source124==2 plus unsuccessful feature admission
// forces pipe0 and composition fallback BEFORE the pipe1 VP+28 dispatch.
// Other sample types preserve both values. Invalid owned pipe is rejected
// without modifying either buffer; it is not silently converted to pipe0.
template<class Execution,class Pass>
bool tglApplyVeboxAdmissionSampleFallback(Execution& execution,
    Pass& pass,uint32_t source124,bool admitted) noexcept {
    static_assert(TglAdmissionExecutionExtent<Execution>::value>=0xa10 && TglAdmissionPassExtent<Pass>::value>=9,"admission sample field extents");
    uint32_t pipe=0;std::memcpy(&pipe,execution.data()+0xa0c,4);
    if(pipe>2) return false;
    if(source124==2 && !admitted) {
        pipe=0;std::memcpy(execution.data()+0xa0c,&pipe,4);tglAdmissionPassFlags(pass)=1;
    }
    return true;
}
// donor1fbe8f..1fbef6, after qualified VP28 and DN eligibility acquisition.
// The caller supplies platform-qualified DN request (not an ICL format list).
// Source type is its first DWORD, NOT source124 sample or params138 flags.
// Preserve native low-bit normalization and overwrite whole destination bytes.
// This tail alone neither invokes VP28 nor admits an unfinished SFC stage.
template<class Execution>
void tglPublishVeboxSfcAdmissionFlags(Execution& execution,uint8_t qualifiedDn,
    uint32_t sourceType) noexcept {
    static_assert(TglAdmissionExecutionExtent<Execution>::value>=0x18,"SFC admission flag extents");
    execution[0x11]=qualifiedDn&1;
    execution[0x17]=(sourceType==1 || sourceType==2)?1:0;
    execution[0x0f]=((execution[0x0f]&1) || execution[0x11] || execution[0x17])?1:0;
}
// Gen12 DN formats: intel/media-driver73c8d14, g12_base.cpp329.
// Invoke only after complete qualified VP28. The frame pins source and this
// owned DN block; a foreign source70 is refused, never dereferenced/adopted.
// NULL source70 means no request. Format support alone does not request DN.
template<class Execution,size_t N>
bool tglPublishOwnedVeboxSfcAdmissionFlags(Execution& execution,
    const TglOwnedSurfaceStorage& source,const std::array<uint8_t,N>& dn) noexcept {
    static_assert(N>=1 && TglAdmissionExecutionExtent<Execution>::value>=0xa10,"DN request/pipe extents");
    uintptr_t linked=0;uint32_t format=0,type=0,pipe=0;
    std::memcpy(&linked,source.prefix.data()+0x70,8);
    std::memcpy(&format,source.prefix.data()+0x130,4);
    std::memcpy(&type,source.prefix.data(),4);
    std::memcpy(&pipe,execution.data()+0xa0c,4);
    if(pipe!=1 || (linked && linked!=reinterpret_cast<uintptr_t>(dn.data()))) return false;
    const uint8_t requested=linked && tglVeboxDnFormatSupported(format)?dn[0]:0;
    tglPublishVeboxSfcAdmissionFlags(execution,requested,type);return true;
}

// donor1fbefd..1fbf21. The caller supplies the final admission after any
// pipe1 work/flag publication. This does not skip that work or authorize SFC.
constexpr bool tglVeboxAdmissionResult(uint8_t params138,uint8_t pass08,
    bool admitted) noexcept {
    return admitted && !((params138&1) && (pass08&1));
}

struct TglVeboxAdmissionStagesInput {
    TglVeboxAdmissionPrefixInput prefix;
    uint32_t pipe=0,params10c=0,source124=0;
    uint8_t mmc=0;
    uintptr_t leasedParamsF0=0;
};
struct TglVeboxAdmissionBorrowedInterfaces {
    uintptr_t os=0,renderHal=0,sku=0,paramsF0=0;
};
// Read only named admission inputs. Context authenticates params/pass and
// retains their owners; source/target and interfaces are the same frame's
// leased identities. No arbitrary pointer traversal or donor this adjustment.
template<class ChildOwner,class ExecutionOwner,class Read>
bool tglReadLeasedVeboxAdmissionInputs(TglVeboxAdmissionStagesInput& output,
    ChildOwner& childOwner,ExecutionOwner& executionOwner,void* child,
    uintptr_t params,uintptr_t pass,const TglOwnedSurfaceStorage& source,
    const TglOwnedSurfaceStorage& target,const TglVeboxAdmissionBorrowedInterfaces& expected,
    uint32_t pipe,Read read) noexcept {
    static_assert(noexcept(read(uintptr_t{},nullptr,size_t{})),"admission bounded read ABI");
    if(!child || !childOwner.hasPrivateChildLease(reinterpret_cast<uintptr_t>(child)) ||
       !params || params>UINTPTR_MAX-0x139 || !pass || pass>UINTPTR_MAX-0x18 ||
       !expected.os || !expected.renderHal || !expected.sku || pipe>2) return false;
    const auto* base=static_cast<const uint8_t*>(child);
    uintptr_t os=0,renderHal=0,sku=0,execution=0,sourceLink=0,targetLink=0,f0=0;
    std::memcpy(&os,base+0x10,8);std::memcpy(&renderHal,base+0x18,8);
    std::memcpy(&sku,base+0x20,8);std::memcpy(&execution,base+0x88,8);
    if(os!=expected.os || renderHal!=expected.renderHal || sku!=expected.sku ||
       !executionOwner.hasExecutionLease(execution)) return false;
    TglVeboxAdmissionStagesInput candidate;
    if(!read(pass+0x10,&sourceLink,8) || sourceLink!=reinterpret_cast<uintptr_t>(&source) ||
       !read(params+0x98,&targetLink,8) || targetLink!=reinterpret_cast<uintptr_t>(&target) ||
       !read(params+0xf0,&f0,8) || f0!=expected.paramsF0 ||
       !read(params+0x138,&candidate.prefix.params138,1) ||
       !read(params+0x10c,&candidate.params10c,4) || !read(sku,&candidate.prefix.sku0,8)) return false;
    candidate.prefix.os=candidate.prefix.renderHal=candidate.prefix.execution=candidate.prefix.source=true;
    candidate.mmc=base[0x1ba9];candidate.pipe=pipe;candidate.leasedParamsF0=f0;
    std::memcpy(&candidate.prefix.sourceWidth,source.prefix.data()+0xd8,4);
    std::memcpy(&candidate.prefix.sourceHeight,source.prefix.data()+0xdc,4);
    std::memcpy(&candidate.source124,source.prefix.data()+0x124,4);
    output=candidate;return true;
}

// CPU sequencing of1fb950, not a substitute for Gen12 policy/context.
// Qualified callbacks own reset/optional VP reset, history setup, platform
// feature query, native170 and complete VP28+flag publication respectively.
// All frame/report/parameter owners must remain leased across this call.
template<class Execution,class Pass,class Reset,class Select,class Setup,class Query,class Feature,class Sfc>
bool tglRunSelectedVeboxAdmissionStages(const TglVeboxAdmissionStagesInput& input,
    Execution& execution,Pass& pass,
    Reset reset,Select select,Setup setup,Query query,Feature feature,Sfc sfc) noexcept {
    static_assert(TglAdmissionExecutionExtent<Execution>::value>=0xd2d && TglAdmissionPassExtent<Pass>::value>=9,"complete admission field extents");
    static_assert(noexcept(reset()) && std::is_same_v<decltype(reset()),bool>,"checked reset ABI");
    static_assert(noexcept(select(tglAdmissionPassFlags(pass))) &&
        std::is_same_v<decltype(select(tglAdmissionPassFlags(pass))),uint32_t>,"owned output-pipe policy ABI");
    static_assert(noexcept(query()) && std::is_same_v<decltype(query()),bool>,"feature query ABI");
    static_assert(noexcept(feature(execution)) &&
        std::is_same_v<decltype(feature(execution)),void>,"native170 void ABI");
    static_assert(noexcept(sfc(execution)) && std::is_same_v<decltype(sfc(execution)),bool>,
        "checked complete SFC stage ABI");
    if(tglPrepareVeboxAdmissionPrefix(input.prefix,pass)!=TglVeboxAdmissionPrefixResult::Continue)
        return false;
    if(!reset()) return false;
    // ICL PRIMARY68 at1fbaec..1fbb08 observes reset and updates pass08.
    // This is an OWNED policy callback, NEVER TGL BASE68: that slot is
    // int setupKernel(this,index) and must retain its producer binding.
    const uint32_t selected=select(tglAdmissionPassFlags(pass));
    if(!tglPublishVeboxAdmissionMode(execution,selected,input.mmc,input.params10c,setup)) return false;
    bool admitted=false;
    if(query()) {
        uint32_t currentPipe=0;std::memcpy(&currentPipe,execution.data()+0xa0c,4);
        if(currentPipe>2) return false;
        if(currentPipe==2) std::memcpy(execution.data()+0x9e8,&input.leasedParamsF0,8);
        feature(execution);
        admitted=!(execution[0x18]&1);
    }
    if(!tglApplyVeboxAdmissionSampleFallback(execution,pass,input.source124,admitted)) return false;
    uint32_t pipe=0;std::memcpy(&pipe,execution.data()+0xa0c,4);
    if(pipe==1) {
        if(!sfc(execution)) return false;
        admitted=true;
    }
    return tglVeboxAdmissionResult(input.prefix.params138,tglAdmissionPassFlags(pass),admitted);
}
// Snapshot-only CPU entry retained for callers that already selected a pipe.
// Complete policy uses the selected entry above, not a precomputed snapshot.
template<class Execution,class Pass,class Reset,class Setup,class Query,class Feature,class Sfc>
bool tglRunVeboxAdmissionStages(const TglVeboxAdmissionStagesInput& input,
    Execution& execution,Pass& pass,Reset reset,Setup setup,Query query,Feature feature,Sfc sfc) noexcept {
    if(input.pipe>2) return false;
    return tglRunSelectedVeboxAdmissionStages(input,execution,pass,reset,
        [&](uint8_t&) noexcept -> uint32_t {return input.pipe;},setup,query,feature,sfc);
}

// Donor primary794210+48=1fc230 is a surface-format feature query, NOT the
// VP48 command callback or virtual-base48 report getter. Gen12 uses the
// independently pinned tglVeboxFormatSupported predicate, not ICL's list.
// Enclosing feature frame pins this source and its backing through all stages.
// Derive geometry/sample from that same shell rather than caller snapshots.
template<class Execution,class Pass,class Reset,class Setup,class Feature,class Sfc>
bool tglRunOwnedVeboxSurfaceAdmissionStages(TglVeboxAdmissionStagesInput input,
    const TglOwnedSurfaceStorage& source,Execution& execution,Pass& pass,
    Reset reset,Setup setup,Feature feature,Sfc sfc) noexcept {
    uint32_t format=0;
    std::memcpy(&input.prefix.sourceWidth,source.prefix.data()+0xd8,4);
    std::memcpy(&input.prefix.sourceHeight,source.prefix.data()+0xdc,4);
    std::memcpy(&input.source124,source.prefix.data()+0x124,4);
    std::memcpy(&format,source.prefix.data()+0x130,4);
    return tglRunVeboxAdmissionStages(input,execution,pass,reset,setup,
        [&]() noexcept {return tglVeboxFormatSupported(format);},feature,sfc);
}

// Join input capture to the stage chain without publishing an intermediate
// caller-editable input or copying opaque pass state. Enclosing feature-frame
// scope pins surfaces/interfaces; execution is that same owner's leased view.
template<class ChildOwner,class ExecutionOwner,class Execution,size_t ParamsN,size_t PassN,
    class Read,class Reset,class Select,class Setup,class Feature,class Sfc>
bool tglRunCapturedSelectedVeboxAdmissionStages(ChildOwner& childOwner,ExecutionOwner& executionOwner,
    void* child,std::array<uint8_t,ParamsN>& params,std::array<uint8_t,PassN>& pass,
    const TglOwnedSurfaceStorage& source,const TglOwnedSurfaceStorage& target,
    const TglVeboxAdmissionBorrowedInterfaces& expected,
    Execution& execution,Read read,Reset reset,Select select,Setup setup,Feature feature,Sfc sfc) noexcept {
    static_assert(ParamsN>=0x139 && PassN>=0x18,"complete named admission input extents");
    if(!child || !childOwner.hasPrivateChildLease(reinterpret_cast<uintptr_t>(child))) return false;
    uintptr_t linked=0;std::memcpy(&linked,static_cast<uint8_t*>(child)+0x88,8);
    if(linked!=reinterpret_cast<uintptr_t>(execution.data())) return false;
    TglVeboxAdmissionStagesInput input;
    if(!tglReadLeasedVeboxAdmissionInputs(input,childOwner,executionOwner,child,
        reinterpret_cast<uintptr_t>(params.data()),reinterpret_cast<uintptr_t>(pass.data()),
        source,target,expected,0,read)) return false; // owned pipe policy runs after reset
    TglVeboxAdmissionPassByte flags(pass[8]);
    uint32_t format=0;std::memcpy(&format,source.prefix.data()+0x130,4);
    return tglRunSelectedVeboxAdmissionStages(input,execution,flags,reset,select,setup,
        [&]() noexcept {return tglVeboxFormatSupported(format);},feature,sfc);
}
// Snapshot compatibility entry for CPU fixtures; complete platform policy
// uses the selected entry so owned selection observes reset and live flags;
// do not dispatch TGL base68, whose separate ABI is setupKernel(this,index).
template<class ChildOwner,class ExecutionOwner,class Execution,size_t ParamsN,size_t PassN,
    class Read,class Reset,class Setup,class Feature,class Sfc>
bool tglRunCapturedVeboxAdmissionStages(ChildOwner& childOwner,ExecutionOwner& executionOwner,
    void* child,std::array<uint8_t,ParamsN>& params,std::array<uint8_t,PassN>& pass,
    const TglOwnedSurfaceStorage& source,const TglOwnedSurfaceStorage& target,
    const TglVeboxAdmissionBorrowedInterfaces& expected,uint32_t pipe,
    Execution& execution,Read read,Reset reset,Setup setup,Feature feature,Sfc sfc) noexcept {
    if(pipe>2) return false;
    return tglRunCapturedSelectedVeboxAdmissionStages(childOwner,executionOwner,child,
        params,pass,source,target,expected,execution,read,reset,
        [&](uint8_t&) noexcept -> uint32_t {return pipe;},setup,feature,sfc);
}

template<class Owner,class Policy> class TglOwnedVeboxAdmissionContext {
    Owner& owner_;
    void* child_;
    void* params_;
    void* pass_;
    Policy& policy_;
    bool busy_=false;
public:
    TglOwnedVeboxAdmissionContext(Owner& owner,void* child,void* params,void* pass,
        Policy& policy) noexcept
        :owner_(owner),child_(child),params_(params),pass_(pass),policy_(policy){}
    TglOwnedVeboxAdmissionContext(const TglOwnedVeboxAdmissionContext&)=delete;
    TglOwnedVeboxAdmissionContext& operator=(const TglOwnedVeboxAdmissionContext&)=delete;
    bool owns(void* child) const noexcept {return child && child==child_;}
    bool admit(void* params,void* pass) noexcept {
        static_assert(noexcept(policy_(child_,params_,pass_)) &&
            std::is_same_v<decltype(policy_(child_,params_,pass_)),bool>,
            "complete platform policy must report native bool");
        if(busy_ || !child_ || !params_ || !pass_ || params!=params_ || pass!=pass_ ||
           !owner_.hasPrivateChildLease(reinterpret_cast<uintptr_t>(child_))) return false;
        busy_=true;
        struct Unlock {bool& busy;~Unlock(){busy=false;}} unlock{busy_};
        return policy_(child_,params_,pass_);
    }
};

template<class Resolve> struct TglOwnedVeboxAdmissionCallbacks {
    static bool admit(void* child,void* params,void* pass) noexcept {
        if(!child || !params || !pass) return false;
        auto* context=Resolve::get(child);
        if(!context || !context->owns(child)) return false;
        static_assert(noexcept(context->admit(params,pass)) &&
            std::is_same_v<decltype(context->admit(params,pass)),bool>,
            "VEBOX admission must return native bool, not status");
        return context->admit(params,pass);
    }
};
template<class Resolve>
std::array<uintptr_t,20> tglBindOwnedVeboxAdmissionHook(
    std::array<uintptr_t,20> hooks) noexcept {
    static_assert(TglOwnedVeboxVtable::required[2]==0x28,"exact VEBOX admission slot");
    hooks[2]=reinterpret_cast<uintptr_t>(&TglOwnedVeboxAdmissionCallbacks<Resolve>::admit);
    return hooks;
}

// Allocation input admission, not ICL virtual-base interpretation. Only the
// already-leased owned TGL child/source/execution may reach the planner.
// Feature policy is deliberately not inferred from equal donor offsets.
template<class SurfaceOwner,class ChildOwner,class ExecutionOwner,class Invoke>
int tglRunCapturedVeboxAllocation(SurfaceOwner& sourceOwner,ChildOwner& childOwner,
    ExecutionOwner& executionOwner,void* child,uintptr_t execution,
    const TglOwnedSurfaceStorage& source,Invoke invoke) noexcept {
    static_assert(noexcept(invoke(child,execution,source)) &&
        std::is_same_v<decltype(invoke(child,execution,source)),int>,"captured allocation ABI");
    const auto admitted=[&]() noexcept {
        if(!child || !sourceOwner.hasSurfaceLease(source) ||
           !childOwner.hasPrivateChildLease(reinterpret_cast<uintptr_t>(child)) ||
           !executionOwner.hasExecutionLease(execution)) return false;
        uintptr_t report=0,input=0;
        const auto* bytes=static_cast<const uint8_t*>(child);
        std::memcpy(&report,bytes+0x88,8);std::memcpy(&input,bytes+0x1b0,8);
        return report==execution && input==reinterpret_cast<uintptr_t>(&source);
    };
    if(!admitted()) return 5;
    const int result=invoke(child,execution,source);
    return admitted()?result:5;
}

template<class SurfaceOwner,class ChildOwner,class ExecutionOwner,class Invoke>
int tglWithOwnedVeboxAllocationFrame(SurfaceOwner& sourceOwner,ChildOwner& childOwner,
    ExecutionOwner& executionOwner,Invoke invoke) noexcept {
    int result=5;
    const bool sourceLeased=sourceOwner.withSurface([&](const auto& source) noexcept {
        result=childOwner.withPrivateChild([&](void* child) noexcept {
            return executionOwner.withExecution([&](uintptr_t execution) noexcept {
                return tglRunCapturedVeboxAllocation(sourceOwner,childOwner,executionOwner,
                    child,execution,source,invoke);
            });
        });
    });
    return sourceLeased?result:5;
}

// TGL mode0 12c1cd calls +58 with only child in RDI and consumes EAX.
// Allocation precedes builder admission: do not gate it on producer-pass state.
// Context owns the complete allocation transaction and must unwind failures
// itself; this native caller returns errors without invoking FreeResources.
// Legacy callables already own their unwind. Explicit rollback callables must
// retire only allocation-owned resources with GPU users excluded; this hook
// must not be substituted with native selective FreeResources.
struct TglOwnedVeboxNoAllocationUnwind {void operator()(void*) const noexcept {}};
// Qualified data-policy gates supplied by the owned frame, not donor this
// offsets. ICL1f7afc..1f875f orders these stages; the enclosing slot58 context
// owns failure unwind. Each stage retains its exact status and is called once.
template<class QueryFfdi,class QuerySecondary,class QueryFfdn,class QueryPipe,class QueryStmm,
    class Ffdi,class Secondary,class Ffdn,class Rectangle,class Stmm,class Statistics>
int tglRunOwnedVeboxAllocationQueriedStages(QueryFfdi ffdiEnabled,QuerySecondary hasSecondary,
    QueryFfdn ffdnEnabled,QueryPipe pipe,QueryStmm stmmEnabled,Ffdi ffdi,Secondary secondary,Ffdn ffdn,Rectangle rectangle,
    Stmm stmm,Statistics statistics) noexcept {
    static_assert(noexcept(ffdiEnabled()) && noexcept(hasSecondary()) && noexcept(ffdnEnabled()) && noexcept(pipe()) && noexcept(stmmEnabled()),
        "allocation gate queries must not throw");
    static_assert(std::is_same_v<decltype(ffdiEnabled()),bool> && std::is_same_v<decltype(hasSecondary()),bool> &&
        std::is_same_v<decltype(ffdnEnabled()),bool> && std::is_same_v<decltype(pipe()),uint32_t> && std::is_same_v<decltype(stmmEnabled()),bool>,"allocation gate ABI");
    static_assert(noexcept(ffdi()) && noexcept(secondary()) && noexcept(ffdn()) &&
        noexcept(rectangle()) && noexcept(stmm()) && noexcept(statistics()),"allocation stages must not throw");
    static_assert(std::is_same_v<decltype(ffdi()),int> && std::is_same_v<decltype(secondary()),int> &&
        std::is_same_v<decltype(ffdn()),int> && std::is_same_v<decltype(rectangle()),int> &&
        std::is_same_v<decltype(stmm()),int> && std::is_same_v<decltype(statistics()),int>,"allocation stage status ABI");
    int status=0;
    if(ffdiEnabled() && (status=ffdi())) return status;
    if(hasSecondary() && (status=secondary())) return status;
    // Exact Darwin1f7fbb queries primary+b8; false skips to1f839b.
    if(ffdnEnabled() && (status=ffdn())) return status;
    if(pipe()==2 && (status=rectangle())) return status;
    if(stmmEnabled() && (status=stmm())) return status;
    return statistics();
}
// One source/child/execution frame covers every allocation stage. Revalidate
// the captured identities around each query and stage, not only at frame exit:
// a stage must never hand an invalidated child to the next stage. Stage owners
// still supply qualified allocation policy and explicit captured source views.
struct TglOwnedVeboxFrameNoUnwind {
    void operator()(void*,uintptr_t,const TglOwnedSurfaceStorage&) const noexcept {}
};
template<class SurfaceOwner,class ChildOwner,class ExecutionOwner,class QueryFfdi,
    class QuerySecondary,class QueryFfdn,class QueryPipe,class QueryStmm,class Ffdi,class Secondary,
    class Ffdn,class Rectangle,class Stmm,class Statistics,class Unwind=TglOwnedVeboxFrameNoUnwind>
int tglRunCapturedVeboxAllocationStages(SurfaceOwner& sourceOwner,ChildOwner& childOwner,
    ExecutionOwner& executionOwner,void* capturedChild,uintptr_t capturedExecution,
    const TglOwnedSurfaceStorage& capturedSource,QueryFfdi ffdiEnabled,QuerySecondary hasSecondary,
    QueryFfdn ffdnEnabled,QueryPipe pipe,QueryStmm stmmEnabled,Ffdi ffdi,Secondary secondary,Ffdn ffdn,
    Rectangle rectangle,Stmm stmm,Statistics statistics,Unwind unwind={}) noexcept {
    return tglRunCapturedVeboxAllocation(sourceOwner,childOwner,executionOwner,
        capturedChild,capturedExecution,capturedSource,
        [&](void* child,uintptr_t execution,const auto& source) noexcept {
            const auto stage=[&](auto& invoke) noexcept {
                return tglRunCapturedVeboxAllocation(sourceOwner,childOwner,executionOwner,
                    child,execution,source,invoke);
            };
            const auto gate=[&](auto& query,auto& value) noexcept {
                auto read=[&](void* c,uintptr_t e,const auto& s) noexcept {
                    static_assert(noexcept(query(c,e,s)),"allocation frame query ABI");
                    static_assert(std::is_same_v<decltype(query(c,e,s)),
                        std::remove_reference_t<decltype(value)>>,"allocation frame query result ABI");
                    value=query(c,e,s);return 0;
                };
                return stage(read);
            };
            const auto run=[&]() noexcept -> int {
            bool enabled=false;uint32_t selectedPipe=0;int status=0;
            if((status=gate(ffdiEnabled,enabled))) return status;
            if(enabled && (status=stage(ffdi))) return status;
            if((status=gate(hasSecondary,enabled))) return status;
            if(enabled && (status=stage(secondary))) return status;
            if((status=gate(ffdnEnabled,enabled))) return status;
            if(enabled && (status=stage(ffdn))) return status;
            if((status=gate(pipe,selectedPipe))) return status;
            if(selectedPipe==2 && (status=stage(rectangle))) return status;
            if((status=gate(stmmEnabled,enabled))) return status;
            if(enabled && (status=stage(stmm))) return status;
            return stage(statistics);
            };
            const int result=run();
            if(!result) return 0;
            // Only allocation-owned resources, with GPU users excluded. Never
            // replace this with native selective FreeResources. Do not invoke
            // cleanup through a revoked child/source/execution identity.
            auto rollback=[&](void* c,uintptr_t e,const auto& s) noexcept {
                static_assert(noexcept(unwind(c,e,s)) &&
                    std::is_same_v<decltype(unwind(c,e,s)),void>,"allocation frame unwind ABI");
                unwind(c,e,s);return result;
            };
            return stage(rollback);
        });
}
// Standalone entry acquires the frame. Native slot58 already runs inside the
// child lease: it must use the captured entry above rather than reacquire it.
template<class SurfaceOwner,class ChildOwner,class ExecutionOwner,class QueryFfdi,
    class QuerySecondary,class QueryFfdn,class QueryPipe,class QueryStmm,class Ffdi,class Secondary,
    class Ffdn,class Rectangle,class Stmm,class Statistics,class Unwind=TglOwnedVeboxFrameNoUnwind>
int tglWithOwnedVeboxAllocationStages(SurfaceOwner& sourceOwner,ChildOwner& childOwner,
    ExecutionOwner& executionOwner,QueryFfdi ffdiEnabled,QuerySecondary hasSecondary,
    QueryFfdn ffdnEnabled,QueryPipe pipe,QueryStmm stmmEnabled,Ffdi ffdi,Secondary secondary,Ffdn ffdn,
    Rectangle rectangle,Stmm stmm,Statistics statistics,Unwind unwind={}) noexcept {
    return tglWithOwnedVeboxAllocationFrame(sourceOwner,childOwner,executionOwner,
        [&](void* child,uintptr_t execution,const auto& source) noexcept {
            return tglRunCapturedVeboxAllocationStages(sourceOwner,childOwner,executionOwner,
                child,execution,source,ffdiEnabled,hasSecondary,ffdnEnabled,pipe,stmmEnabled,
                ffdi,secondary,ffdn,rectangle,stmm,statistics,unwind);
        });
}
// Convenience only for immutable, already-qualified policy inputs. Runtime
// frame producers use the queried form so later gates observe earlier writes.
template<class Ffdi,class Secondary,class Ffdn,class Rectangle,class Stmm,class Statistics>
int tglRunOwnedVeboxAllocationStages(bool ffdiEnabled,bool hasSecondary,bool ffdnEnabled,uint32_t pipe,
    bool stmmEnabled,Ffdi ffdi,Secondary secondary,Ffdn ffdn,Rectangle rectangle,
    Stmm stmm,Statistics statistics) noexcept {
    return tglRunOwnedVeboxAllocationQueriedStages(
        [=]() noexcept {return ffdiEnabled;},[=]() noexcept {return hasSecondary;},
        [=]() noexcept {return ffdnEnabled;},[=]() noexcept {return pipe;},[=]() noexcept {return stmmEnabled;},
        ffdi,secondary,ffdn,rectangle,stmm,statistics);
}
template<class Owner,class Allocate,class Unwind=TglOwnedVeboxNoAllocationUnwind>
class TglOwnedVeboxAllocationContext {
    Owner& owner_;
    Allocate& allocate_;
    void* child_;
    uint64_t generation_;
    Unwind unwind_;
    bool busy_=false;
public:
    TglOwnedVeboxAllocationContext(Owner& owner,Allocate& allocate,void* child,Unwind unwind={}) noexcept
        :owner_(owner),allocate_(allocate),child_(child),
         generation_(owner.allocationGeneration()),unwind_(unwind) {}
    TglOwnedVeboxAllocationContext(const TglOwnedVeboxAllocationContext&)=delete;
    TglOwnedVeboxAllocationContext& operator=(const TglOwnedVeboxAllocationContext&)=delete;
    bool owns(void* child) const noexcept {
        return child && child==child_ && generation_ &&
            generation_==owner_.allocationGeneration() &&
            owner_.hasPrivateChildLease(reinterpret_cast<uintptr_t>(child));
    }
    int allocate() noexcept {
        static_assert(noexcept(allocate_(child_)) &&
            std::is_same_v<decltype(allocate_(child_)),int>,"native allocation status ABI");
        static_assert(noexcept(unwind_(child_)) &&
            std::is_same_v<decltype(unwind_(child_)),void>,"allocation unwind ABI");
        if(busy_ || !owns(child_)) return 5;
        busy_=true;
        struct Unlock {bool& busy;~Unlock(){busy=false;}} unlock{busy_};
        const int result=allocate_(child_);
        if(result) unwind_(child_);
        return owns(child_) ? result : 5;
    }
};
template<class Resolve> struct TglOwnedVeboxAllocationCallbacks {
    static int allocate(void* child) noexcept {
        auto* context=Resolve::get(child);
        if(!context || !context->owns(child)) return 5;
        static_assert(noexcept(context->allocate()),"allocation reports native status");
        return context->allocate();
    }
};
template<class Resolve>
std::array<uintptr_t,20> tglBindOwnedVeboxAllocationHook(
    std::array<uintptr_t,20> hooks) noexcept {
    static_assert(TglOwnedVeboxVtable::required[3]==0x58,"exact TGL allocation slot");
    hooks[3]=reinterpret_cast<uintptr_t>(&TglOwnedVeboxAllocationCallbacks<Resolve>::allocate);
    return hooks;
}

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

// Enclose this in the execution/backing leases. Unlike putting submission in
// the builder callback, this checks the completed producer latch FIRST. Keep
// child/context publication through submission and synchronous finish. Finish
// must report its native status, not silently discard it; zero alone does not
// authorize resource retirement (reset/skip and hardware stamps need proof).
// An unresolved submission still requires the enclosing owners to survive
// this scope; this function does not invent an asynchronous fence owner.
template<class Owner,class Context,class Prepare,class Setup,class Build,class Submit,class Finish>
int tglWithOwnedVeboxSubmittedFrame(Owner& owner,Context& context,
    Prepare prepare,Setup setup,Build build,Submit submit,Finish finish) noexcept {
    static_assert(noexcept(prepare(nullptr)) && noexcept(setup(nullptr)) &&
        noexcept(build(nullptr)) && noexcept(submit(nullptr)) && noexcept(finish(nullptr)),
        "native frame stages report status without throwing");
    return owner.withPrivateChild([&](void* child) noexcept {
        TglOwnedVeboxScopedContext<Context> scope(child,context);
        if(!scope.bound()) return 5;
        const int built=context.pass.withBuilder([&]() noexcept {
            const int prepared=prepare(child);
            if(prepared) return prepared;
            const int configured=setup(child);
            return configured ? configured : build(child);
        });
        if(built) return built;
        const int submitted=submit(child);
        return submitted ? submitted : finish(child);
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

// Backing pins enclose execution, child, producer-pass and every frame stage.
// Each stage receives the SAME borrowed descriptor views; it may not retain
// them past this scope. No GPU submit/fence or automatic retirement is added.
template<class Group,class ChildOwner,class ExecutionOwner,class Context,
         class Prepare,class Setup,class Invoke>
int tglWithOwnedVeboxBackedExecutionFrame(Group& resources,ChildOwner& childOwner,
    ExecutionOwner& executionOwner,uintptr_t expectedExecution,Context& context,
    Prepare prepare,Setup setup,Invoke invoke) noexcept {
    return resources.withStorage([&](const auto& views) noexcept {
        return tglWithOwnedVeboxExecutionFrame(childOwner,executionOwner,expectedExecution,context,
            [&](void* child) noexcept {
                static_assert(noexcept(prepare(child,views)),"backed preparation status ABI");
                return prepare(child,views);
            },[&](void* child) noexcept {
                static_assert(noexcept(setup(child,views)),"backed setup status ABI");
                return setup(child,views);
            },[&](void* child) noexcept {
                static_assert(noexcept(invoke(child,views)),"backed builder status ABI");
                return invoke(child,views);
            });
    });
}

// History backing is pinned before all other frame owners. Native preparation
// can consume history before setup/builder; acquiring it only in slot78 is too
// late. Both descriptors are borrowed through every synchronous stage, never
// copied into independently owned native tokens. Async fence ownership remains
// the enclosing frame transaction's responsibility.
template<class History,class Group,class ChildOwner,class ExecutionOwner,class Context,
         class Prepare,class Setup,class Invoke>
int tglWithOwnedVeboxHistoryFrame(History& history,TglOwnedSurfaceStorage& firstSurface,
    TglOwnedSurfaceStorage& secondSurface,Group& resources,ChildOwner& childOwner,
    ExecutionOwner& executionOwner,uintptr_t expectedExecution,Context& context,
    Prepare prepare,Setup setup,Invoke invoke) noexcept {
    return history.withSurfaces(firstSurface,secondSurface,
        [&](const auto& firstShell,const auto& secondShell) noexcept {
        const auto& first=firstShell.resource;
        const auto& second=secondShell.resource;
        return tglWithOwnedVeboxBackedExecutionFrame(resources,childOwner,executionOwner,
            expectedExecution,context,
            [&](void* child,const auto& views) noexcept {
                static_assert(noexcept(prepare(child,views,first,second)),"history preparation ABI");
                return prepare(child,views,first,second);
            },[&](void* child,const auto& views) noexcept {
                static_assert(noexcept(setup(child,views,first,second)),"history setup ABI");
                return setup(child,views,first,second);
            },[&](void* child,const auto& views) noexcept {
                static_assert(noexcept(invoke(child,views,first,second)),"history builder ABI");
                return invoke(child,views,first,second);
            });
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
    TglOwnedSfcLockParameters lock=tglPrepareOwnedSfcLockParameters(1,0,0);
    TglOwnedSfcStateParametersPacket state{};
    std::array<uint8_t,16> avs{};
    std::array<uint8_t,64> ief{};
    TglOwnedSfcYParameters y{};
    TglOwnedSfcUvParameters uv{};
    // CPU owner state, never part of a native command packet. Serialized use.
    bool filtersInUse=false;
    TglOwnedSfcCommandParameters()=default;
    TglOwnedSfcCommandParameters(const TglOwnedSfcCommandParameters&)=delete;
    TglOwnedSfcCommandParameters& operator=(const TglOwnedSfcCommandParameters&)=delete;
    TglOwnedSfcCommandParameters(TglOwnedSfcCommandParameters&&)=delete;
    TglOwnedSfcCommandParameters& operator=(TglOwnedSfcCommandParameters&&)=delete;
    // Explicit CPU snapshot only. Resource addresses still require their
    // enclosing owner leases; this is not a resource-lifetime certificate.
    bool snapshotFrom(const TglOwnedSfcCommandParameters& source) noexcept {
        if(filtersInUse || source.filtersInUse) return false;
        for(size_t offset:{size_t(0x28),size_t(0x30),size_t(0x38)}) {
            uintptr_t from=0,to=0;
            std::memcpy(&from,source.ief.data()+offset,8);
            std::memcpy(&to,ief.data()+offset,8);
            if(from || to) return false;
        }
        lock=source.lock;state=source.state;avs=source.avs;
        ief=source.ief;y=source.y;uv=source.uv;return true;
    }
};
// VP+98 (1e0bb0) clears64 bytes before optional IEF then CSC. Its IEF
// producer1dcbd0 writes state+67/+68 and the same parameter offsets as our
// qualified TGL IEF builder. Use TGL tables, never execute the ICL producer.
// This CPU producer must run before the CSC coefficient lease; never erase
// pointers already borrowed by a command consumer. Surface/image acquisition
// and mode admission remain the enclosing owner's responsibility.
inline bool tglPrepareOwnedSfcIefParameters(TglOwnedSfcCommandParameters& parameters,
    bool enabled,float strength,uint8_t skinEnable,uint8_t detailEnable,
    const std::array<uint16_t,3>& thresholds,const TglOwnedIefTables& tables) noexcept {
    if(parameters.filtersInUse) return false;
    for(size_t offset:{size_t(0x28),size_t(0x30),size_t(0x38)}) {
        uintptr_t borrowed=0;std::memcpy(&borrowed,parameters.ief.data()+offset,8);
        if(borrowed) return false;
    }
    std::array<uint8_t,64> candidate{};
    TglOwnedIefSurfaceFlags flags{parameters.state[0x67],parameters.state[0x68]};
    if(enabled && !tglBuildOwnedIefParameters(candidate,flags,strength,
        skinEnable,detailEnable,thresholds,tables)) return false;
    parameters.ief=candidate;
    if(enabled) {parameters.state[0x67]=flags.enabled;parameters.state[0x68]=flags.skinEnabled;}
    return true;
}

// Enclose actual CPU command generation in the coefficient lease, rather
// than storing pointers into a temporary snapshot. Disabled CSC does not
// inspect its optional source or reset an existing cache. This does not
// authenticate the TGL pass/backend or certify GPU retirement.
template<class Read,class Produce,class Generate>
int tglWithOwnedSfcCscCommands(TglOwnedSfcCommandParameters& parameters,
    TglOwnedVpCscCache& cache,bool enabled,uint32_t from,uint32_t to,
    uintptr_t sourceA8,Read read,Produce produce,Generate generate) noexcept {
    static_assert(noexcept(generate(parameters)),"SFC CSC command generator ABI");
    if(!enabled) return generate(parameters);
    if(!cache.prepare(from,to,sourceA8,read,produce)) return 5;
    return cache.withParameters(parameters.ief,parameters.state,[&]() noexcept {
        return generate(parameters);
    });
}
struct TglOwnedSfcIefInput {
    bool enabled=false;
    float strength=0;
    uint8_t skinEnable=0,detailEnable=0;
    std::array<uint16_t,3> thresholds{};
};
// VP98 prepares IEF before VPa0 publishes CSC pointers. Keep the actual
// command consumer inside the coefficient scope instead of returning borrowed
// pointers from an isolated callback. This is CPU composition, not submission.
template<class Read,class Produce,class Generate>
int tglWithOwnedSfcFilterCommands(TglOwnedSfcCommandParameters& parameters,
    const TglOwnedSfcIefInput& input,const TglOwnedIefTables& tables,
    TglOwnedVpCscCache& cache,bool csc,uint32_t from,uint32_t to,
    uintptr_t sourceA8,Read read,Produce produce,Generate generate) noexcept {
    if(!tglPrepareOwnedSfcIefParameters(parameters,input.enabled,input.strength,
        input.skinEnable,input.detailEnable,input.thresholds,tables)) return 5;
    parameters.filtersInUse=true;
    const int result=tglWithOwnedSfcCscCommands(parameters,cache,csc,from,to,sourceA8,
        read,produce,generate);
    parameters.filtersInUse=false;
    return result;
}
// Compose the prepared CPU arguments with the typed native backend. Resource
// usage registration must already be complete; shell/owners must remain live.
// On any failure the caller must discard the partial command buffer, never
// submit it. Platform is an explicit qualified producer, NOT an assumed no-op.
template<class Backend,class Platform,class Surface>
int tglSendOwnedSfcPreparedCommands(Backend &backend,
    const TglOwnedSfcCommandParameters &parameters,
    const Surface &shell,uint8_t scaling,uint8_t forceAvs,
    uint8_t ief,uint8_t csc,Platform &platform,
    const TglOwnedSfcLockParameters* lockOverride=nullptr) {
    uint32_t outputFormat=0;
    std::memcpy(&outputFormat,parameters.state.data()+0x2c,4);
    if(!tglOwnedSfcPlanarOutputFits(outputFormat,tglOwnedSfcSurfaceBytes(shell))) return 5;
    struct Commands {
        Backend &backend;
        const TglOwnedSfcCommandParameters &parameters;
        const TglOwnedSfcLockParameters &lockParameters;
        const Surface &shell;
        Platform &platformProducer;
        std::array<uint8_t,0x38> descriptor{};
        int lock(){return backend.lock(lockParameters);}
        int output(){descriptor=tglOwnedSfcOutputDescriptor(shell);return 0;}
        int platform(){return platformProducer(descriptor);}
        int state(){
            // Platform owns a mutable descriptor. Recheck the exact values
            // consumed by the planar branches after that callback, not merely
            // the original shell before command emission. Failure requires
            // discarding the already-started command buffer as documented.
            uint32_t format=0,descriptorFormat=0,plane=0;
            std::memcpy(&format,parameters.state.data()+0x2c,4);
            std::memcpy(&descriptorFormat,descriptor.data()+4,4);
            std::memcpy(&plane,descriptor.data()+0x24,4);
            if(tglNativeSfcUsesPlaneOffset(format) &&
               (descriptorFormat!=format || plane>0xffff)) return 5;
            return backend.state(parameters.state,descriptor);
        }
        int avsState(){return backend.avs(parameters.avs);}
        int yTable(){return backend.y(parameters.y);}
        int uvTable(){return backend.uv(parameters.uv);}
        int iefState(){return backend.ief(parameters.ief);}
        int frameStart(){return backend.frameStart();}
    } commands{backend,parameters,lockOverride?*lockOverride:parameters.lock,shell,platform,{}};
    return tglScheduleOwnedSfcCommands(scaling,forceAvs,ief,csc,commands);
}
// Exact send-time settings snapshot. Acquire before the first command write;
// read denial cannot leave a partial command buffer. The owned lock argument
// stays live through emission without mutating the reusable parameter owner.
// Output registration/image/resource leases and discard-on-error remain the
// enclosing sender's obligations, as for the prepared-command entry above.
template<class Backend,class Platform,class Read,class Surface>
int tglSendOwnedSfcLeasedCommands(Backend& backend,
    const TglOwnedSfcCommandParameters& parameters,
    const Surface& shell,uintptr_t settings,
    uint8_t scaling,uint8_t forceAvs,uint8_t ief,uint8_t csc,
    Read read,Platform& platform) {
    TglOwnedSfcLockParameters lock;
    if(!tglReadOwnedSfcLockParameters(settings,1,read,lock)) return 5;
    return tglSendOwnedSfcPreparedCommands(backend,parameters,shell,
        scaling,forceAvs,ief,csc,platform,&lock);
}
// Compose the exact TGL binding with leased settings and owned emission.
// No use of the generic injected OS+130 path until its binding is proved.
template<class Backend,class Platform,class Read,class Surface,class Invoke=TglNativeSfcOutputUsageInvoker>
int tglSendOwnedSfcNativeOutputCommands(const TglNativeSfcOutputUsageBinding& usage,
    Backend& backend,const TglOwnedSfcCommandParameters& parameters,
    const Surface& shell,uintptr_t settings,
    uint8_t scaling,uint8_t forceAvs,uint8_t ief,uint8_t csc,
    Read read,Platform& platform,Invoke invoke=Invoke{}) {
    // Recheck at the emission boundary, not only during earlier packet
    // construction: reject descriptor-only output or a packet bound to a
    // different resource before any usage callback or command write.
    const auto* bytes=tglOwnedSfcSurfaceBytes(shell);
    std::array<uint8_t,0x148> resource{};
    std::memcpy(resource.data(),bytes+0x148,resource.size());
    uintptr_t boundOutput=0;
    std::memcpy(&boundOutput,parameters.state.data()+0x90,8);
    if(!tglNativeBackingPresent(resource) ||
       boundOutput!=reinterpret_cast<uintptr_t>(bytes+0x148)) return 5;
    uint32_t format=0;std::memcpy(&format,parameters.state.data()+0x2c,4);
    if(!tglNativeSfcStateFormatSupported(format)) return 31;
    uint32_t pitch=0;std::memcpy(&pitch,bytes+0xe0,4);
    if(!tglNativeSfcOutputPitchFits(pitch) ||
       !tglOwnedSfcPlanarOutputFits(format,bytes)) return 5;
    TglOwnedSfcLockParameters lock;
    if(!tglReadOwnedSfcLockParameters(settings,1,read,lock)) return 5;
    return usage.withOutput(shell,[&]() noexcept -> int {
        auto checkedPlatform=[&](auto& descriptor) -> int {
            const int result=platform(descriptor);
            if(result) return result;
            uint32_t finalPitch=0;std::memcpy(&finalPitch,descriptor.data()+0x10,4);
            return tglNativeSfcOutputPitchFits(finalPitch)?0:5;
        };
        return tglSendOwnedSfcPreparedCommands(backend,parameters,shell,
            scaling,forceAvs,ief,csc,checkedPlatform,&lock);
    },invoke);
}
// Native send1e0e56..1e0eb5 uses OS+128(resource,6,1), then optional
// OS+130(resource,6) when surface+e8 bit0 is set. Neither result is read.
// This is NOT OS+248 relocation registration. The injected producers must
// already bind authenticated native entries and retain the OS/output leases.
template<class Backend,class Platform,class Read,class Update,class Control,class Surface>
int tglSendOwnedSfcWithOutputUsage(Backend& backend,
    const TglOwnedSfcCommandParameters& parameters,
    const Surface& shell,uintptr_t settings,
    uint8_t scaling,uint8_t forceAvs,uint8_t ief,uint8_t csc,
    Read read,Update update,Control control,Platform& platform) {
    const auto* bytes=tglOwnedSfcSurfaceBytes(shell);
    const void* resource=bytes+0x148;
    static_assert(noexcept(update(resource,uint32_t{},uint32_t{})) &&
                  noexcept(control(resource,uint32_t{})),"SFC output usage ABI");
    static_assert(std::is_same_v<decltype(update(resource,uint32_t{},uint32_t{})),void> &&
                  std::is_same_v<decltype(control(resource,uint32_t{})),void>,
                  "native SFC usage return values are not consumed");
    std::array<uint8_t,0x148> backing{};
    std::memcpy(backing.data(),resource,backing.size());
    uintptr_t boundOutput=0;
    std::memcpy(&boundOutput,parameters.state.data()+0x90,8);
    if(!tglNativeBackingPresent(backing) ||
       boundOutput!=reinterpret_cast<uintptr_t>(resource)) return 5;
    TglOwnedSfcLockParameters lock;
    if(!tglReadOwnedSfcLockParameters(settings,1,read,lock)) return 5;
    update(resource,6,1);
    if(bytes[0xe8]&1) control(resource,6);
    return tglSendOwnedSfcPreparedCommands(backend,parameters,shell,
        scaling,forceAvs,ief,csc,platform,&lock);
}
// ICL 1e0ca0..1e0dcb setup order. Producers operate on owned state;
// caller binds the closed argument ABI, never an ICL object to TGL code.
// Qualified native flags are VP base+921/+922/+923 and +9b8, low bit
// only; do not confuse them with the settings pointer passed to +40/+48.
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
private:
    template<class ResetView> bool initializeWithReset(ResetView resetView) {
        static_assert(noexcept(resetView()),"VP state reset ABI");
        if(leased || mutating) return false;
        mutating=true;
        owner.reset();
        resetView();
        if (!owner.allocate(sizeof(TglOwnedSfcStateParametersPacket))) {
            mutating=false; return false;
        }
        std::memset(reinterpret_cast<void*>(owner.get()),0,sizeof(TglOwnedSfcStateParametersPacket));
        mutating=false; return true;
    }
public:
    bool initialize() {return initializeWithReset([]() noexcept {});}
    // ICL slot10: 1f7450 calls1e1080 (release old state, zero920..9c7),
    // then allocates state. Translate owned data only, never invoke the donor
    // free against our borrowed938. A nonzero published state rejects the
    // entire operation. Caller also owns/quiesces the private VP and GPU work;
    // this CPU reset neither frees borrowed surfaces nor certifies retirement.
    bool initializeVpPass(TglOwnedVpSfcCallerStorage& storage) {
        uintptr_t published=0;
        std::memcpy(&published,storage.bytes.data()+0x938,8);
        if(published) return false;
        return initializeWithReset([&]() noexcept {
            std::memset(storage.bytes.data()+0x920,0,0xa8);
        });
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
    template<class Use> int withPublishedVpParameters(
        const TglOwnedVpSfcCallerStorage& vp,Use use) const noexcept {
        static_assert(noexcept(use(static_cast<const uint8_t*>(nullptr))),"VP state view ABI");
        if(!hasParametersLease()) return 5;
        uintptr_t published=0;std::memcpy(&published,vp.bytes.data()+0x938,8);
        if(published!=owner.get()) return 5;
        return use(reinterpret_cast<const uint8_t*>(published));
    }
    template<class Use> int withParameters(Use&& use) noexcept {
        static_assert(noexcept(use(static_cast<const uint8_t*>(nullptr))),
            "SFC state borrower must not throw");
        if(leased || mutating || !owner.get()) return 5;
        leased=true;
        const int result=use(reinterpret_cast<const uint8_t*>(owner.get()));
        leased=false; return result;
    }
    // State-producing VP90 reaches VP58 at ICL1e04cd before binding line
    // resources into the packet. Publish this exact state only under its
    // parameters lease; no ad-hoc pointer store or foreign owner adoption.
    template<class Use> int withVpParameterPublication(TglOwnedVpSfcCallerStorage& vp,
        Use use) noexcept {
        static_assert(noexcept(use(static_cast<const uint8_t*>(nullptr))),
            "VP state publication consumer ABI");
        uintptr_t previous=0;std::memcpy(&previous,vp.bytes.data()+0x938,8);
        if(previous) return 5;
        return withParameters([&](const uint8_t* packet) noexcept -> int {
            const uintptr_t published=reinterpret_cast<uintptr_t>(packet);
            std::memcpy(vp.bytes.data()+0x938,&published,8);
            const int result=use(packet);
            uintptr_t observed=0;std::memcpy(&observed,vp.bytes.data()+0x938,8);
            std::memcpy(vp.bytes.data()+0x938,&previous,8);
            return observed==published ? result : 5;
        });
    }
};

// Exact VP+10 thunk1f74e0 reaches void1f7450(this): there is no status ABI.
// Keep allocation failure separately visible to the enclosing owner before
// publishing a VP or emitting commands. This lease does not authorize GPU
// retirement and deliberately leaves938 unpublished until the CPU-view scope.
template<class Backend> class TglOwnedVpPassInitContext {
    TglOwnedVpSfcCallerStorage& storage_;
    TglOwnedSfcStateParameters<Backend>& state_;
    bool called_=false;
    int status_=5;
public:
    TglOwnedVpPassInitContext(TglOwnedVpSfcCallerStorage& storage,
        TglOwnedSfcStateParameters<Backend>& state) noexcept :storage_(storage),state_(state) {}
    TglOwnedVpPassInitContext(const TglOwnedVpPassInitContext&)=delete;
    TglOwnedVpPassInitContext& operator=(const TglOwnedVpPassInitContext&)=delete;
    bool owns(void* child) const noexcept {return child==storage_.bytes.data();}
    void initialize() noexcept {
        if(called_) {status_=5;return;}
        called_=true;
        status_=state_.initializeVpPass(storage_) ? 0 : 5;
    }
    int status() const noexcept {return status_;}
};
template<class Resolve> struct TglOwnedVpPassInitCallback {
    static void initialize(void* child) noexcept {
        static_assert(noexcept(Resolve{}(child)),"VP initialization resolver ABI");
        auto* context=Resolve{}(child);
        static_assert(noexcept(context->owns(child)) && noexcept(context->initialize()),
            "VP initialization context ABI");
        if(context && context->owns(child)) context->initialize();
    }
};

template<class Resolve>
bool tglBindOwnedVpInitializeEntry(void* child,TglOwnedVpEntrySet& target) noexcept {
    static_assert(noexcept(Resolve{}(child)),"VP initialize binding resolver ABI");
    auto* context=Resolve{}(child);
    static_assert(noexcept(context->owns(child)),"VP initialize binding ownership ABI");
    if(!context || !context->owns(child) || target.initialize) return false;
    auto candidate=target;
    candidate.initialize=&TglOwnedVpPassInitCallback<Resolve>::initialize;
    target=candidate;return true;
}
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
    const TglOwnedSfcAvsParameters<AvsBackend>& avs,Invoke invoke,bool avsEnabled=true) noexcept {
    static_assert(noexcept(invoke(static_cast<void*>(nullptr))),"VP CPU view borrower must not throw");
    // Match the command-input gate: disabled AVS has no parameter borrower.
    // The caller must use this same gate for actual command scheduling.
    if(!state.hasParametersLease() || (avsEnabled && !avs.hasParametersLease())) return 5;
    if(!avsEnabled && ((storage.bytes[0x921]|storage.bytes[0x9b8])&1)) return 5;
    for(size_t offset:{size_t(0x10),size_t(0x18),size_t(0x20)}) {
        uintptr_t borrowed=0;std::memcpy(&borrowed,storage.bytes.data()+offset,8);
        if(!borrowed) return 5;
    }
    uintptr_t oldState=0;
    std::memcpy(&oldState,storage.bytes.data()+0x938,8);
    if(oldState) return 5;
    std::array<uint8_t,0x30> oldHeader;
    std::memcpy(oldHeader.data(),storage.bytes.data()+0x28,oldHeader.size());
    if(avsEnabled) {
        for(size_t offset:{size_t(0x10),size_t(0x18),size_t(0x20),size_t(0x28)}) {
            uintptr_t existing=0;std::memcpy(&existing,oldHeader.data()+offset,8);
            if(existing) return 5;
        }
    }
    const uintptr_t packet=reinterpret_cast<uintptr_t>(state.data());
    if(avsEnabled) {
        std::memcpy(storage.bytes.data()+0x28,avs.parameters()->data(),oldHeader.size());
    }
    std::memcpy(storage.bytes.data()+0x938,&packet,8);
    const int result=invoke(storage.bytes.data());
    std::memcpy(storage.bytes.data()+0x28,oldHeader.data(),oldHeader.size());
    std::memcpy(storage.bytes.data()+0x938,&oldState,8);
    return result;
}
// Unified CPU graph for the caller-visible VP prefix, state and AVS backing.
// This is deliberately NOT a callable VP object: no vtable/publication/native
// invocation is authorized. Borrowed interfaces must outlive this owner;
// asynchronous GPU retention remains a separate prerequisite for integration.
template<class Lines,class State,class Retire,class Use>
int tglWithOwnedVpResourceEntries(TglOwnedVpSfcCallerStorage&,Lines&,State&,Retire&,Use) noexcept;
template<class Read,class Lines,class State,class Retire,class Use>
int tglWithOwnedVpAssemblyEntries(TglOwnedVpSfcCallerStorage&,Read&,Lines&,State&,Retire&,Use) noexcept;
template<class StateBackend,class AvsBackend> class TglOwnedVpSfcCpuGraph {
    TglOwnedVpSfcCallerStorage storage_;
    TglOwnedSfcStateParameters<StateBackend> state_;
    TglOwnedSfcAvsParameters<AvsBackend> avs_;
    bool ready_=false,busy_=false,resourcesBound_=false;
    uint64_t generation_=0;
    struct SendContext {
        void* child;
        bool owns(void* candidate) const noexcept {return candidate==child;}
    };
    template<class Use> static int withSendAdmission(void* child,Use use) noexcept {
        using Resolve=TglOwnedVpSfcResolve<SendContext>;
        using Entry=int(*)(void*,void*,void*,void*,void*,void*) noexcept;
        static_assert(noexcept(use(nullptr,Entry{})),"CPU graph send admission ABI");
        SendContext context{child};
        TglOwnedVpSfcScopedContext<SendContext> scope(child,context);
        if(!scope.bound()) return 5;
        TglOwnedVpEntrySet entries;
        if(!tglBindOwnedVpPlatformAdmissionEntries<Resolve>(child,entries)) return 5;
        return use(child,entries.platformSend);
    }
    void clear() noexcept {
        ready_=false;
        if(generation_!=UINT64_MAX) ++generation_;
        // Donor 1ddb7e..1ddba1 releases AVS before state+938. Use our
        // tracked owners, never its native free/deleting destructor.
        avs_.reset();state_.reset();storage_.bytes.fill(0);
    }
    struct Guard {
        bool& flag;
        explicit Guard(bool& value) noexcept:flag(value){flag=true;}
        ~Guard(){flag=false;}
    };
public:
    TglOwnedVpSfcCpuGraph(StateBackend& state,AvsBackend& avs):state_(state),avs_(avs){}
    TglOwnedVpSfcCpuGraph(const TglOwnedVpSfcCpuGraph&)=delete;
    TglOwnedVpSfcCpuGraph& operator=(const TglOwnedVpSfcCpuGraph&)=delete;
    ~TglOwnedVpSfcCpuGraph(){reset();}
    bool ownsStorage(const void* child) const noexcept {return ready_ && child==storage_.bytes.data();}
    uint64_t generation() const noexcept {return generation_;}
    template<class Binding,class Read>
    bool initialize(uintptr_t os,uintptr_t renderHal,uintptr_t mhw,
        const Binding& binding,Read read,uint32_t channel,uintptr_t performance) {
        if(busy_ || resourcesBound_ || generation_==UINT64_MAX) return false;
        Guard guard(busy_);clear();
        if(!storage_.initializeBorrowed(os,renderHal,mhw,binding,read) ||
           !avs_.initialize()) {
            clear();return false;
        }
        // Use the same void VP10 entry as the eventual owned table. Native
        // initialization has no return status; the scoped owner keeps that
        // separate and must verify it before marking the graph ready.
        using InitContext=TglOwnedVpPassInitContext<StateBackend>;
        using InitResolve=TglOwnedVpSfcResolve<InitContext>;
        InitContext initialization(storage_,state_);
        {
            void* child=storage_.bytes.data();
            TglOwnedVpSfcScopedContext<InitContext> scope(child,initialization);
            if(!scope.bound()) {clear();return false;}
            TglOwnedVpEntrySet entries;
            if(!tglBindOwnedVpInitializeEntry<InitResolve>(child,entries)) {clear();return false;}
            entries.initialize(child);
        }
        if(initialization.status()) {clear();return false;}
        storage_.setCallerFields(channel,performance);ready_=true;return true;
    }
    // Separate VP10 construction phase from frame publication. Caller must
    // exclude GPU users; no resource-owner scope or published state may exist.
    // Native void initialization has an independently checked owned status.
    template<class Use> int withPassInitializationEntry(Use use) noexcept {
        using InitContext=TglOwnedVpPassInitContext<StateBackend>;
        using InitResolve=TglOwnedVpSfcResolve<InitContext>;
        using Entry=void(*)(void*) noexcept;
        static_assert(noexcept(use(nullptr,Entry{})),"VP construction consumer ABI");
        if(!ready_ || busy_ || resourcesBound_ || generation_==UINT64_MAX) return 5;
        Guard guard(busy_);ready_=false;++generation_;
        InitContext initialization(storage_,state_);
        void* child=storage_.bytes.data();
        TglOwnedVpSfcScopedContext<InitContext> scope(child,initialization);
        TglOwnedVpEntrySet entries;
        if(!scope.bound() || !tglBindOwnedVpInitializeEntry<InitResolve>(child,entries)) {
            clear();return 5;
        }
        const int result=use(child,entries.initialize);
        const int status=result ? result : initialization.status();
        if(status) {clear();return status;}
        ready_=true;return 0;
    }
    // Construction catalogue only. Base callbacks must retain their own
    // scopes through Use; the returned subset is never a persistent vtable.
    template<class Use> int withInitializationAssemblyEntries(
        const TglOwnedVpEntrySet& base,Use use) noexcept {
        if(base.initialize) return 5; // reject before changing state/generation
        return withPassInitializationEntry([&](void* child,auto initialize) noexcept {
            auto entries=base;
            TglOwnedVpEntrySet addition;addition.initialize=initialize;
            if(!entries.appendDisjoint(addition)) return 5;
            static_assert(noexcept(use(child,entries)),"VP construction catalogue ABI");
            return use(child,static_cast<const TglOwnedVpEntrySet&>(entries));
        });
    }
    // CPU teardown only: the enclosing runtime owner still has to establish
    // GPU retirement before calling this or releasing the VP allocation.
    bool tryReset() noexcept {
        if(busy_ || resourcesBound_) return false;
        Guard guard(busy_);clear();return true;
    }
    void reset() noexcept {(void)tryReset();}
    template<class Use> int withTeardownReservation(Use use) noexcept {
        if(busy_ || resourcesBound_) return 5;
        Guard guard(busy_);bool cleared=false;
        auto releaseCpu=[&]() noexcept {
            if(!cleared) {cleared=true;clear();}
        };
        static_assert(noexcept(use(releaseCpu)),"VP teardown reservation ABI");
        return use(releaseCpu);
    }
    template<class Prepare> int prepare(Prepare prepare) noexcept {
        static_assert(noexcept(prepare(storage_,state_,avs_)),"CPU graph preparation ABI");
        if(!ready_ || busy_ || resourcesBound_) return 5;
        Guard guard(busy_);return prepare(storage_,state_,avs_);
    }
    // Construct resource owners inside this callback and destroy them before
    // returning. Their typed shells live in this VP storage, which must not
    // be zeroed/reinitialized until those owners are gone. Frame/setup/send
    // operations remain available; this is not an asynchronous fence lease.
    template<class Use> int withResourceOwnerStorage(Use use) noexcept {
        static_assert(noexcept(use(storage_)),"VP resource owner lifetime ABI");
        if(!ready_ || busy_ || resourcesBound_) return 5;
        Guard guard(resourcesBound_);return use(storage_);
    }
    // Native pipe selection and subsequent VP28/40/48 must refer to this
    // same initialized VP owner, not a separate admission-only storage shell.
    // Selection itself does not acquire VP28 or allocate/submit resources.
    template<class Read,class Policy>
    uint32_t selectOutputPipe(Read& read,Policy& policy,void* source,void* target,
        void* parameters,uint8_t& compNeeded) noexcept {
        if(!ready_ || busy_) return UINT32_MAX;
        Guard guard(busy_);
        // ICL1fb51b..1fb557 obtains VP18's target from params98. The public
        // graph API must not qualify format on that surface but geometry on
        // a different caller-supplied target. Pin this identity before policy
        // callbacks; unlike native SFC rejection, a broken frame is an error.
        const uintptr_t params=reinterpret_cast<uintptr_t>(parameters);
        uintptr_t actualTarget=0;
        if(!source || !target || !params || params>UINTPTR_MAX-0xa0 ||
            !read(params+0x98,&actualTarget,8) ||
            actualTarget!=reinterpret_cast<uintptr_t>(target)) return UINT32_MAX;
        TglOwnedVpPipeSelector selector(storage_,read,policy,source,target,parameters);
        return selector(compNeeded);
    }
    template<class Use> int withView(bool avsEnabled,Use use) noexcept {
        static_assert(noexcept(use(static_cast<void*>(nullptr))),"CPU graph view ABI");
        if(!ready_ || busy_) return 5;
        Guard guard(busy_);
        return state_.withParameters([&](const uint8_t*) noexcept {
            auto invoke=[&]() noexcept {
                return tglWithOwnedVpSfcCpuView(storage_,state_,avs_,use,avsEnabled);
            };
            if(!avsEnabled) return invoke();
            return avs_.withParameters([&](const auto&) noexcept {return invoke();});
        });
    }
    // Private flat dispatch only, inside an existing graph/state lease. Caller
    // retains all callback resolver scopes. Not native RTTI or persistent
    // parent publication; restore the data-only view before releasing owners.
    template<class Use> int withLeasedFlatDispatch(void* child,
        const TglOwnedVpFlatDispatchTable& table,Use use) noexcept {
        static_assert(noexcept(use(child)),"VP private dispatch consumer ABI");
        if(!ready_ || !busy_ || child!=storage_.bytes.data() || !table.addressPoint()) return 5;
        return state_.withPublishedVpParameters(storage_,[&](const uint8_t*) noexcept {
            uintptr_t previous=0;std::memcpy(&previous,storage_.bytes.data(),8);
            if(previous) return 5;
            const auto address=table.addressPoint();
            std::memcpy(storage_.bytes.data(),&address,8);
            const int result=use(child);
            uintptr_t observed=0;std::memcpy(&observed,storage_.bytes.data(),8);
            std::memcpy(storage_.bytes.data(),&previous,8);
            return observed==address ? result : 5;
        });
    }
    // Admission entries share the real graph/state/AVS lease. In particular,
    // lifecycle teardown cannot retire this storage while a callback is live.
    template<class Read,class Use>
    int withLeasedAdmissionEntries(void* child,Read& read,Use use) noexcept {
        if(!ready_ || !busy_ || child!=storage_.bytes.data()) return 5;
        return state_.withPublishedVpParameters(storage_,[&](const uint8_t*) noexcept {
            return tglWithOwnedVpAdmissionEntries(storage_,read,
                [&](void* admitted,const TglOwnedVpAdmissionEntries& entries) noexcept {
                    if(admitted!=child) return 5;
                    static_assert(noexcept(use(child,entries)),"leased VP admission ABI");
                    return use(child,entries);
                });
        });
    }
    template<class Read,class Use>
    int withAdmissionView(bool avsEnabled,Read& read,Use use) noexcept {
        return withView(avsEnabled,[&](void* child) noexcept {
            return withLeasedAdmissionEntries(child,read,use);
        });
    }
    // Expose the qualified admission/resource subset as typed slots on the
    // same child. Other slots remain NULL; this is not a publishable vtable.
    template<class Read,class Lines,class Retire,class Use>
    int withLeasedAssemblyEntries(void* child,Read& read,Lines& lines,Retire& retire,Use use) noexcept {
        if(!ready_ || !busy_ || child!=storage_.bytes.data()) return 5;
        return state_.withPublishedVpParameters(storage_,[&](const uint8_t*) noexcept {
            return tglWithOwnedVpAssemblyEntries(storage_,read,lines,state_,retire,
                [&](void* assembled,const TglOwnedVpAdmissionEntries& entries,
                    auto allocate,auto release) noexcept {
                    if(assembled!=child) return 5;
                    TglOwnedVpEntrySet bindings;
                    if(!tglBindOwnedVpAdmissionEntries(bindings,entries) ||
                       !tglBindOwnedVpResourceEntries(bindings,allocate,release)) return 5;
                    static_assert(noexcept(use(child,static_cast<const TglOwnedVpEntrySet&>(bindings))),
                        "leased VP assembly ABI");
                    return use(child,static_cast<const TglOwnedVpEntrySet&>(bindings));
                });
        });
    }
    template<class Read,class Lines,class Retire,class Use>
    int withAssemblyView(bool avsEnabled,Read& read,Lines& lines,Retire& retire,Use use) noexcept {
        return withView(avsEnabled,[&](void* child) noexcept {
            return withLeasedAssemblyEntries(child,read,lines,retire,use);
        });
    }
    template<class Read,class Lines,class Retire,class Use>
    int withLeasedModeAssemblyEntries(void* child,Read& read,Lines& lines,Retire& retire,
        uintptr_t source,uintptr_t target,uintptr_t settings,Use use) noexcept {
        return withLeasedAssemblyEntries(child,read,lines,retire,
            [&](void* assemblyChild,const TglOwnedVpEntrySet& entries) noexcept {
                if(assemblyChild!=child) return 5;
                return tglWithOwnedVpModeAssembly(storage_,source,target,settings,read,entries,
                    [&](void* modeChild,const TglOwnedVpEntrySet& complete) noexcept {
                        if(modeChild!=child) return 5;
                        static_assert(noexcept(use(child,complete)),"VP graph mode assembly ABI");
                        return use(child,complete);
                    });
            });
    }
    template<class Read,class Lines,class Retire,class Use>
    int withModeAssemblyView(bool avsEnabled,Read& read,Lines& lines,Retire& retire,
        uintptr_t source,uintptr_t target,uintptr_t settings,Use use) noexcept {
        return withView(avsEnabled,[&](void* child) noexcept {
            return withLeasedModeAssemblyEntries(child,read,lines,retire,source,target,settings,use);
        });
    }
    template<class Surface,size_t PacketBytes,class Read,class Invoke,class Use>
    int withLeasedPlatformCompressionAssemblyEntries(void* child,const Surface& output,
        std::array<uint8_t,PacketBytes>& packet,uintptr_t settings,
        const TglNativeVpCompressionBinding& binding,Read read,Invoke invoke,
        const TglOwnedVpEntrySet& base,Use use) noexcept {
        if(!ready_ || !busy_ || child!=storage_.bytes.data()) return 5;
        return state_.withPublishedVpParameters(storage_,[&](const uint8_t* state) noexcept {
            if(state!=packet.data()) return 5;
            return tglWithOwnedVpPlatformCompressionAssembly(storage_,output,packet,settings,
                binding,read,invoke,base,use);
        });
    }
    // Only the actual currently published state packet may receive VP+a0.
    // The enclosing setup/send owner retains the graph/frame lease; no copy
    // or unrelated packet is admitted as native state938.
    template<class Read,class Produce,class Use>
    int withCscProducerEntries(void* child,std::array<uint8_t,64>& ief,
        TglOwnedVpCscCache& cache,uint32_t from,uint32_t to,uintptr_t sourceA8,
        Read read,Produce produce,Use use) noexcept {
        if(!ready_ || !busy_ || child!=storage_.bytes.data()) return 5;
        return state_.withPublishedVpParameters(storage_,[&](const uint8_t* state) noexcept {
            return tglWithOwnedVpCscProducerSpan<0xf8>(storage_,ief,const_cast<uint8_t*>(state),cache,
                from,to,sourceA8,read,produce,[&](void* admitted,auto csc) noexcept {
                    static_assert(noexcept(use(admitted,csc,const_cast<uint8_t*>(state))),"VP CSC graph consumer ABI");
                    return use(admitted,csc,const_cast<uint8_t*>(state));
                });
        });
    }
    // Extend a caller-leased catalogue while the real state938 and CSC cache
    // remain pinned. Base resolver lifetimes are still the caller's obligation.
    template<class Read,class Produce,class Use>
    int withCscAssemblyEntries(void* child,std::array<uint8_t,64>& ief,
        TglOwnedVpCscCache& cache,uint32_t from,uint32_t to,uintptr_t sourceA8,
        Read read,Produce produce,const TglOwnedVpEntrySet& base,Use use) noexcept {
        if(base.csc) return 5;
        return withCscProducerEntries(child,ief,cache,from,to,sourceA8,read,produce,
            [&](void* admitted,auto csc,uint8_t* state) noexcept {
                auto entries=base;
                TglOwnedVpEntrySet addition;addition.csc=csc;
                if(!entries.appendDisjoint(addition)) return 5;
                static_assert(noexcept(use(admitted,entries,state)),"VP CSC assembly consumer ABI");
                return use(admitted,entries,state);
            });
    }
    // Expose only the qualified platform-send entry during the existing
    // state/AVS/graph lease, not a partially populated callable VP vtable.
    // Retaining the function pointer cannot retain its TLS admission context.
    template<class Use> int withSendView(bool avsEnabled,Use use) noexcept {
        return withView(avsEnabled,[&](void* child) noexcept {
            return withSendAdmission(child,use);
        });
    }
    // Enclose frame publication, ordered producers and command-input borrowing
    // in one graph lease. Producers receive the owned backing, not copies of
    // native pointers. The consumer must emit/discard synchronously; submitting
    // work still requires the separately proven GPU resource/fence owner.
    // IEF preparation must not let temporary CSC pointers escape: the consumer
    // encloses emission in tglWithOwnedSfcFilterCommands' coefficient lease.
    template<class State,class Avs,class Ief,class Send>
    int withPreparedPass(const TglOwnedVpFramePreparation& frame,
        uintptr_t conditionalSource,uintptr_t settings,uintptr_t target,uintptr_t sourceA8,
        State state,Avs avs,Ief ief,Send send) noexcept {
        static_assert(noexcept(state(nullptr,frame,state_)) &&
            noexcept(avs(nullptr,frame,avs_)) && noexcept(ief(nullptr,frame)) &&
            noexcept(send(nullptr,frame)),"CPU graph pass callback ABI");
        if(!ready_ || busy_) return 5;
        Guard guard(busy_);
        return tglWithOwnedVpPreparedFrame(storage_,frame,conditionalSource,
            settings,target,sourceA8,[&](void* child,const auto&) noexcept {
                const int result=tglSetupOwnedVpSfcStages(frame,
                    [&]() noexcept {return state(child,frame,state_);},
                    [&]() noexcept {return avs(child,frame,avs_);},
                    [&]() noexcept {ief(child,frame);});
                if(result) return result;
                const bool enabled=frame.regions.scale.scaling || frame.color.at9b8;
                return state_.withParameters([&](const uint8_t*) noexcept {
                    auto consume=[&]() noexcept {
                        return tglWithOwnedVpSfcCpuView(storage_,state_,avs_,
                            [&](void* view) noexcept {return send(view,frame);},enabled);
                    };
                    if(!enabled) return consume();
                    return avs_.withParameters([&](const auto&) noexcept {return consume();});
                });
            });
    }
    // Bridge setup+40 and send+48 within one prepared-frame lease. Producers
    // execute at setup, not eagerly before the native caller chooses setup.
    // End-to-end CPU path: +28 acquisition -> deferred +40 setup -> +48
    // command construction. Keep RDX frame parameters separate from R9 mode
    // settings. Read callbacks cannot reset/reenter the owner. The caller
    // retains source/target/settings/interface leases through the whole call.
    template<class Read,class Invoke,class Use>
    int withFrameAcquisitionEntry(uintptr_t source,uintptr_t target,uintptr_t mhw,
        uintptr_t settings,uintptr_t conditionalSource,uintptr_t parameters,
        const TglOwnedVpModeState& original,Read read,Invoke invoke,Use use) noexcept {
        if(!ready_ || busy_) return 5;
        Guard guard(busy_);
        return tglWithOwnedVpAcquiredFrameEntry(storage_,source,target,mhw,settings,
            conditionalSource,parameters,original,read,invoke,use);
    }
    template<class Read,class Use>
    int withFrameAcquisition(uintptr_t source,uintptr_t target,uintptr_t mhw,
        uintptr_t settings,uintptr_t conditionalSource,uintptr_t parameters,
        const TglOwnedVpModeState& original,Read read,Use use) noexcept {
        return withFrameAcquisitionEntry(source,target,mhw,settings,conditionalSource,parameters,
            original,read,[&](void* child,auto prepare) noexcept {
                prepare(child,reinterpret_cast<void*>(conditionalSource),reinterpret_cast<void*>(parameters),
                    reinterpret_cast<void*>(source),reinterpret_cast<void*>(target),reinterpret_cast<void*>(settings));
                return 0;
            },use);
    }
    template<class Read,class State,class Avs,class Ief,class Emit,class Invoke>
    int withLeasedCallbackPass(uintptr_t source,uintptr_t target,uintptr_t mhw,
        uintptr_t settings,uintptr_t conditionalSource,uintptr_t parameters,
        const TglOwnedVpModeState& original,void* renderData,Read read,
        State state,Avs avs,Ief ief,Emit emit,Invoke invoke) noexcept {
        if(!ready_ || busy_ || !renderData) return 5;
        TglOwnedVpFramePreparation frame;TglOwnedVpRegionInput geometry;
        uintptr_t reference=0;
        {
            const int result=withFrameAcquisition(source,target,mhw,
                settings,conditionalSource,parameters,original,read,
                [&](const auto& prepared,const auto& snapshot,uintptr_t sourceA8) noexcept {
                    frame=prepared;geometry=snapshot;reference=sourceA8;return 0;
                });
            if(result) return result;
        }
        return withPreparedCallbackPass(frame,conditionalSource,parameters,target,reference,
            reinterpret_cast<void*>(source),renderData,
            [&](void* child,const auto& prepared,auto& owner) noexcept -> int {
                static_assert(noexcept(state(child,prepared,geometry,owner)),"VP leased state ABI");
                return state(child,prepared,geometry,owner);
            },avs,ief,emit,invoke);
    }
    // Acquire snapshots under a separate lease before publishing the pass
    // catalogue. No acquisition resolver survives into setup/emission.
    template<class Read,class State,class Avs,class Ief,class Emit,class Use>
    int withLeasedCallbackAssemblyPass(const TglOwnedVpEntrySet& base,
        uintptr_t source,uintptr_t target,uintptr_t mhw,uintptr_t settings,
        uintptr_t conditionalSource,uintptr_t parameters,const TglOwnedVpModeState& original,
        void* renderData,Read read,State state,Avs avs,Ief ief,Emit emit,Use use) noexcept {
        if(!ready_ || busy_ || !renderData || base.setup || base.send || base.state || base.filters) return 5;
        const auto retainedBase=base; // Reader callbacks must not change the admitted catalogue.
        TglOwnedVpFramePreparation frame;TglOwnedVpRegionInput geometry;
        uintptr_t reference=0;
        const int acquired=withFrameAcquisition(source,target,mhw,settings,
            conditionalSource,parameters,original,read,
            [&](const auto& prepared,const auto& snapshot,uintptr_t sourceA8) noexcept {
                frame=prepared;geometry=snapshot;reference=sourceA8;return 0;
            });
        if(acquired) return acquired;
        return withPreparedCallbackAssemblyPass(retainedBase,frame,conditionalSource,parameters,target,reference,
            reinterpret_cast<void*>(source),renderData,
            [&](void* child,const auto& prepared,auto& owner) noexcept -> int {
                static_assert(noexcept(state(child,prepared,geometry,owner)),"VP assembly state ABI");
                return state(child,prepared,geometry,owner);
            },avs,ief,emit,use);
    }
    template<class Read,class Lines,class Retire,class State,class Avs,class Ief,class Emit,class Invoke>
    int withLeasedResourceCallbackPass(uintptr_t source,uintptr_t target,uintptr_t mhw,
        uintptr_t settings,uintptr_t conditionalSource,uintptr_t parameters,
        const TglOwnedVpModeState& original,void* renderData,Read read,
        Lines& lines,Retire& retire,State state,Avs avs,Ief ief,Emit emit,Invoke invoke) noexcept {
        if(!ready_ || busy_ || !renderData || !lines.boundToVp(storage_)) return 5;
        TglOwnedVpFramePreparation frame;TglOwnedVpRegionInput geometry;
        uintptr_t reference=0;
        {
            const int result=withFrameAcquisition(source,target,mhw,
                settings,conditionalSource,parameters,original,read,
                [&](const auto& prepared,const auto& snapshot,uintptr_t sourceA8) noexcept {
                    frame=prepared;geometry=snapshot;reference=sourceA8;return 0;
                });
            if(result) return result;
        }
        return withPreparedResourceCallbackPass(frame,conditionalSource,parameters,target,reference,
            reinterpret_cast<void*>(source),renderData,lines,retire,
            [&](void* child,const auto& prepared,auto& owner,auto allocate) noexcept -> int {
                static_assert(noexcept(state(child,prepared,geometry,owner,allocate)),
                    "VP leased resource state ABI");
                return state(child,prepared,geometry,owner,allocate);
            },avs,ief,emit,invoke);
    }
    // One owner for setup +40, allocation +58, send +48 and cleanup +30.
    // The state producer must publish its prepared packet before invoking the
    // supplied allocator, matching native +90 ordering. Retirement remains an
    // explicit external admission, not an inference from synchronous send.
    template<class Lines,class Retire,class State,class Avs,class Ief,class Emit,class Invoke>
    int withPreparedResourceCallbackPass(const TglOwnedVpFramePreparation& frame,
        uintptr_t conditionalSource,uintptr_t settings,uintptr_t target,uintptr_t sourceA8,
        void* source,void* renderData,Lines& lines,Retire& retire,
        State state,Avs avs,Ief ief,Emit emit,Invoke invoke) noexcept {
        if(!ready_ || busy_ || !lines.boundToVp(storage_)) return 5;
        return tglWithOwnedVpResourceEntries(storage_,lines,state_,retire,
            [&](void* resourceChild,auto allocate,auto release) noexcept {
                auto makeState=[&](void* child,const auto& prepared,auto& owner) noexcept {
                    if(child!=resourceChild) return 5;
                    return state(child,prepared,owner,allocate);
                };
                auto use=[&](void* child,auto setup,auto send) noexcept {
                    if(child!=resourceChild) return 5;
                    return invoke(child,setup,send,release);
                };
                return withPreparedCallbackPass(frame,conditionalSource,settings,target,sourceA8,
                    source,renderData,makeState,avs,ief,emit,use);
            });
    }
    // This is synchronous command construction, not a publishable VP vtable
    // or permission to retire GPU resources. Outer surface leases remain required.
    template<class State,class Avs,class Ief,class Emit,class Invoke>
    int withPreparedCallbackPass(const TglOwnedVpFramePreparation& frame,
        uintptr_t conditionalSource,uintptr_t settings,uintptr_t target,uintptr_t sourceA8,
        void* source,void* renderData,State state,Avs avs,Ief ief,Emit emit,Invoke invoke) noexcept {
        static_assert(noexcept(state(nullptr,frame,state_)) &&
            noexcept(avs(nullptr,frame,avs_)) && noexcept(ief(nullptr,frame)) &&
            noexcept(emit(nullptr,nullptr,nullptr)),"VP callback producer ABI");
        if(!ready_ || busy_ || !source || !renderData || !target) return 5;
        Guard guard(busy_);
        return tglWithOwnedVpPreparedFrame(storage_,frame,conditionalSource,
            settings,target,sourceA8,[&](void* child,const auto&) noexcept {
                    auto produceState=[&]() noexcept {return state(child,frame,state_);};
                    auto produceFilters=[&]() noexcept {ief(child,frame);};
                    struct ProducerContext {
                        void* child;void* data;void* source;void* target;
                        decltype(produceState)& makeState;
                        decltype(produceFilters)& makeFilters;
                        const uint8_t* leasedState=nullptr;
                        bool active=false,stateSeen=false,filtersSeen=false;int error=0;
                        bool owns(void* value) const noexcept {return value==child;}
                        int state(void* d,void* s,void* t) noexcept {
                            if(!active) return 5;
                            if(error) return error;
                            if(stateSeen || d!=data || s!=source || t!=target) return error=5;
                            stateSeen=true;return error=makeState();
                        }
                        void filters(void* packet,void* s) noexcept {
                            if(!active) return;
                            if(error) return;
                            if(!stateSeen || filtersSeen || !leasedState ||
                                packet!=leasedState || s!=source) {error=5;return;}
                            filtersSeen=true;makeFilters();
                        }
                    } producer{child,renderData,source,reinterpret_cast<void*>(target),
                        produceState,produceFilters};
                    using ProducerResolve=TglOwnedVpSfcResolve<ProducerContext>;
                    TglOwnedVpSfcScopedContext<ProducerContext> producerScope(child,producer);
                    if(!producerScope.bound()) return 5;
                    TglOwnedVpEntrySet producerEntries;
                    if(!tglBindOwnedVpProducerEntries<ProducerResolve>(child,producerEntries)) return 5;
                auto produce=[&]() noexcept -> int {
                    if(producer.active) return 5;
                    Guard productionGuard(producer.active);
                    const int result=tglSetupOwnedVpSfcStages(frame,
                        [&]() noexcept {return producerEntries.state(child,renderData,source,
                            reinterpret_cast<void*>(target));},
                        [&]() noexcept {return avs(child,frame,avs_);},
                        [&]() noexcept {
                            const int filterResult=state_.withVpParameterPublication(storage_,[&](const uint8_t* packet) noexcept {
                                producer.leasedState=packet;
                                producerEntries.filters(child,renderData,const_cast<uint8_t*>(packet),source);
                                producer.leasedState=nullptr;
                                return producer.error;
                            });
                            if(filterResult && !producer.error) producer.error=filterResult;
                        });
                    return result ? result : producer.error;
                };
                auto consume=[&](void* d,void* command) noexcept -> int {
                    return state_.withParameters([&](const uint8_t*) noexcept {
                        auto view=[&]() noexcept -> int {
                            return tglWithOwnedVpSfcCpuView(storage_,state_,avs_,
                                [&](void* leased) noexcept {return emit(leased,d,command);},
                                frame.regions.scale.scaling || frame.color.at9b8);
                        };
                        if(!(frame.regions.scale.scaling || frame.color.at9b8)) return view();
                        return avs_.withParameters([&](const auto&) noexcept {return view();});
                    });
                };
                struct Context {
                    void* child;void* source;void* target;void* data;
                    decltype(produce)& produce;decltype(consume)& consume;
                    bool setupSeen=false,sendSeen=false,busy=false;int error=0;
                    bool owns(void* candidate) const noexcept {return candidate==child;}
                    int setup(void* s,void* t,void* d) noexcept {
                        if(error) return error;
                        if(setupSeen || busy || s!=source || t!=target || d!=data)
                            return error=5;
                        setupSeen=true;busy=true;
                        const int result=produce();busy=false;
                        if(!error) error=result;
                        return error;
                    }
                    int send(void* d,void* command) noexcept {
                        if(error) return error;
                        if(!setupSeen || sendSeen || busy || d!=data || !command)
                            return error=5;
                        sendSeen=true;busy=true;
                        const int result=consume(d,command);
                        busy=false;
                        if(!error) error=result;
                        return error;
                    }
                } context{child,source,reinterpret_cast<void*>(target),renderData,produce,consume};
                using SetupResolve=TglOwnedVpSfcResolve<Context>;
                TglOwnedVpSfcScopedContext<Context> scope(child,context);
                if(!scope.bound()) return 5;
                TglOwnedVpEntrySet entries;
                if(!tglBindOwnedVpSetupSendEntries<SetupResolve>(child,entries)) return 5;
                const int result=[&]() noexcept {
                    if constexpr(std::is_nothrow_invocable_r_v<int,Invoke,void*,decltype(entries.setup),
                        decltype(entries.send),const TglOwnedVpEntrySet&>)
                        return invoke(child,entries.setup,entries.send,producerEntries);
                    else {
                        static_assert(noexcept(invoke(child,entries.setup,entries.send)),"VP callback invocation ABI");
                        return invoke(child,entries.setup,entries.send);
                    }
                }();
                if(result) return result;
                if(context.error) return context.error;
                return context.setupSeen && context.sendSeen ? 0 : 5;
            });
    }
    // Compose setup/send and phase-gated state/filter while the pass is live. Existing
    // entries retain their outer resolver leases; this does not publish a
    // complete vtable or extend any callback lifetime beyond the consumer.
    template<class State,class Avs,class Ief,class Emit,class Use>
    int withPreparedCallbackAssemblyPass(const TglOwnedVpEntrySet& base,
        const TglOwnedVpFramePreparation& frame,uintptr_t conditionalSource,
        uintptr_t settings,uintptr_t target,uintptr_t sourceA8,void* source,void* renderData,
        State state,Avs avs,Ief ief,Emit emit,Use use) noexcept {
        if(base.setup || base.send || base.state || base.filters) return 5;
        const auto retained=base;
        const TglOwnedVpEntrySet* active=nullptr;
        auto emitAssembly=[&](void* child,void* data,void* command) noexcept -> int {
            if(!active) return 5;
            if constexpr(std::is_nothrow_invocable_r_v<int,Emit,void*,void*,void*,const TglOwnedVpEntrySet&>)
                return emit(child,data,command,*active);
            else {
                static_assert(std::is_nothrow_invocable_r_v<int,Emit,void*,void*,void*>,"VP emission ABI");
                return emit(child,data,command);
            }
        };
        return withPreparedCallbackPass(frame,conditionalSource,settings,target,sourceA8,
            source,renderData,state,avs,ief,emitAssembly,[&](void* child,auto setup,auto send,
                const TglOwnedVpEntrySet& producerEntries) noexcept {
                auto additions=producerEntries;
                additions.setup=setup;additions.send=send;
                auto combined=retained;
                if(!combined.appendDisjoint(additions)) return 5;
                static_assert(noexcept(use(child,combined)),"VP setup/send assembly ABI");
                active=&combined;
                const int result=use(child,combined);
                active=nullptr;
                return result;
            });
    }
    // The qualified six-argument platform entry exists only in the final
    // consumer, after successful producers and while all CPU inputs are leased.
    template<class State,class Avs,class Ief,class Send>
    int withPreparedSendPass(const TglOwnedVpFramePreparation& frame,
        uintptr_t conditionalSource,uintptr_t settings,uintptr_t target,uintptr_t sourceA8,
        State state,Avs avs,Ief ief,Send send) noexcept {
        return withPreparedPass(frame,conditionalSource,settings,target,sourceA8,
            state,avs,ief,[&](void* child,const auto& prepared) noexcept {
                return withSendAdmission(child,[&](void* view,auto entry) noexcept {
                    static_assert(noexcept(send(view,prepared,entry)),"prepared send ABI");
                    return send(view,prepared,entry);
                });
            });
    }
};
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
    // The IEF/CSC consumer owns the entire command-input snapshot. Reject
    // reentrant AVS preparation before changing its cache or CPU backing.
    if(parameters.filtersInUse) return 5;
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
// ICL1f77c0 IsDNOnly reads report0b/13 and base d0/c8. The owned TGL
// table preserves d0=71e80 (false) and c8=12eaf0 (report0f bit0).
// Resolve the report through the held child, not an ICL primary-vtable slot.
template<class Read>
bool tglReadOwnedDnOnly(bool& output,uintptr_t child,uintptr_t execution,Read read) {
    if(!child || child>UINTPTR_MAX-0x8f || !execution || execution>UINTPTR_MAX-0x13)
        return false;
    uintptr_t actual=0;std::array<uint8_t,9> flags{};
    if(!read(child+0x88,&actual,8) || actual!=execution ||
        !read(execution+0xb,flags.data(),flags.size())) return false;
    output=(flags[0]&1) && !(flags[8]&1) && !(flags[4]&1);
    return true;
}
struct TglOwnedVeboxAllocationPolicy {
    bool ffdi=false,ffdn=false,stmm=false;
    uint32_t pipe=0;
};
// Exact ICL1f7860/1f78f0/1f7930 data policies, using the owned TGL
// table's preserved d0=false. Report13 is the allocation DI flag; caller's
// builder argument or report12 cannot replace it. Higher flag bits are ignored.
template<class Read>
bool tglReadOwnedVeboxAllocationPolicy(TglOwnedVeboxAllocationPolicy& output,
    uintptr_t child,uintptr_t execution,Read read) {
    if(!child || child>UINTPTR_MAX-0x8f || !execution || execution>UINTPTR_MAX-0xa0f)
        return false;
    uintptr_t actual=0;std::array<uint8_t,9> flags{};uint32_t pipe=0;
    if(!read(child+0x88,&actual,8) || actual!=execution ||
        !read(execution+0xb,flags.data(),flags.size()) ||
        !read(execution+0xa0c,&pipe,4) || pipe>2) return false;
    const bool dn=(flags[0]&1)!=0,di=(flags[8]&1)!=0,iecp=(flags[4]&1)!=0;
    output={di||iecp||(dn&&pipe==1),dn,dn||di,pipe};
    return true;
}
// Real frame entry: each gate rereads the current report, because FFDi may
// update report13/14 before FFDN/STMM. A denied read stops all later producers
// and returns failure through the captured runner's allocation-only unwind.
template<class SurfaceOwner,class ChildOwner,class ExecutionOwner,class Read,
    class QuerySecondary,class Ffdi,class Secondary,class Ffdn,class Rectangle,
    class Stmm,class Statistics,class Unwind=TglOwnedVeboxFrameNoUnwind>
int tglRunCapturedVeboxAllocationFromReport(SurfaceOwner& sourceOwner,ChildOwner& childOwner,
    ExecutionOwner& executionOwner,void* child,uintptr_t execution,
    const TglOwnedSurfaceStorage& source,Read read,QuerySecondary hasSecondary,
    Ffdi ffdi,Secondary secondary,Ffdn ffdn,Rectangle rectangle,Stmm stmm,
    Statistics statistics,Unwind unwind={}) noexcept {
    static_assert(noexcept(read(uintptr_t{},static_cast<void*>(nullptr),size_t{})),
        "allocation report reader must not throw");
    bool denied=false;
    const auto policy=[&](void* c,uintptr_t e,const auto&) noexcept {
        TglOwnedVeboxAllocationPolicy value;
        if(!denied && !tglReadOwnedVeboxAllocationPolicy(value,
            reinterpret_cast<uintptr_t>(c),e,read)) denied=true;
        return value;
    };
    const auto guarded=[&](auto& invoke) noexcept {
        return [&invoke,&denied](void* c,uintptr_t e,const auto& s) noexcept -> int {
            static_assert(noexcept(invoke(c,e,s)) && std::is_same_v<decltype(invoke(c,e,s)),int>,
                "allocation stage must preserve integer status without throwing");
            return denied?5:invoke(c,e,s);
        };
    };
    return tglRunCapturedVeboxAllocationStages(sourceOwner,childOwner,executionOwner,
        child,execution,source,
        [&](void* c,uintptr_t e,const auto& s) noexcept {return policy(c,e,s).ffdi;},
        [&](void* c,uintptr_t e,const auto& s) noexcept {
            static_assert(noexcept(hasSecondary(c,e,s)) &&
                std::is_same_v<decltype(hasSecondary(c,e,s)),bool>,"secondary query ABI");
            return !denied&&hasSecondary(c,e,s);
        },
        [&](void* c,uintptr_t e,const auto& s) noexcept {return policy(c,e,s).ffdn;},
        [&](void* c,uintptr_t e,const auto& s) noexcept {return policy(c,e,s).pipe;},
        [&](void* c,uintptr_t e,const auto& s) noexcept {return policy(c,e,s).stmm;},
        guarded(ffdi),guarded(secondary),guarded(ffdn),guarded(rectangle),
        guarded(stmm),guarded(statistics),unwind);
}
// Connect the real report policy to the existing compression selector. The
// caller still holds source/child/report leases and qualifies native backing.
template<class Read,class Supports>
bool tglSelectOwnedFfdiCompressionFromReport(TglOwnedFfdiCompressionSelection& output,
    const TglOwnedSurfaceStorage& source,uintptr_t child,uintptr_t execution,
    bool child1ba9,uint32_t format,uint32_t tile,Read read,Supports supports) noexcept {
    static_assert(noexcept(read(uintptr_t{},static_cast<void*>(nullptr),size_t{})),
        "allocation report reader must not throw");
    bool followSource=false;
    if(!tglReadOwnedDnOnly(followSource,child,execution,read)) return false;
    constexpr size_t base=offsetof(TglOwnedSurfaceStorage,resource);
    output=tglSelectOwnedFfdiCompression(followSource,
        (source.resource[0x299-base]&1)!=0,(source.resource[0x29a-base]&1)!=0,
        child1ba9,format,tile,supports);
    return true;
}
// ICL1f76b0 output parameters feed1f7b6c's allocation directly. Keep the
// exact Darwin format values (3/d), not Linux enum numbers. Pipe1's format
// producer is supplied by a held owned SFC child; never read ICL child+99c
// through a TGL object. Publish only after every report/source read succeeds.
template<class Read,class SfcFormat,class Supports>
bool tglPrepareOwnedFfdiRequestFromReport(TglOwnedFfdiAllocationRequest& output,
    const TglOwnedSurfaceStorage& source,uintptr_t child,uintptr_t execution,
    Read read,SfcFormat sfcFormat,Supports supports) noexcept {
    static_assert(noexcept(read(uintptr_t{},static_cast<void*>(nullptr),size_t{})) &&
        noexcept(sfcFormat(std::declval<uint32_t&>())),"allocation producer ABI");
    if(!child || child>UINTPTR_MAX-0x1ba9 || !execution || execution>UINTPTR_MAX-0xa0f)
        return false;
    uintptr_t actualReport=0,actualSource=0;std::array<uint8_t,2> flags{};
    uint32_t pipe=0,format=0,tile=0;uint8_t compression=0;
    if(!read(child+0x88,&actualReport,8) || actualReport!=execution ||
        !read(child+0x1b0,&actualSource,8) || actualSource!=reinterpret_cast<uintptr_t>(&source) ||
        !read(execution+0x12,flags.data(),flags.size()) ||
        !read(execution+0xa0c,&pipe,4) || pipe>2 ||
        !read(child+0x1ba9,&compression,1)) return false;
    if(flags[0]&1) {format=3;tile=1;}
    else if(flags[1]&1) {format=0xd;tile=1;}
    else {
        if(pipe==1) {if(!sfcFormat(format)) return false;}
        else std::memcpy(&format,source.prefix.data()+0x130,4);
        std::memcpy(&tile,source.prefix.data()+0xe4,4);
    }
    TglOwnedFfdiCompressionSelection selected;
    if(!tglSelectOwnedFfdiCompressionFromReport(selected,source,child,execution,
        (compression&1)!=0,format,tile,read,supports)) return false;
    return tglPrepareOwnedFfdiAllocationRequest(source,format,tile,selected,output);
}
// Exact1f7570: primary+a0 produces ColorSpace/SampleType, NOT compression.
// Parameter88 must be read through its own qualified owner callback; a held
// surface does not itself prove that separately allocated parameter's lifetime.
template<class Read,class SfcColor,class Parameter88>
bool tglPrepareOwnedFfdiMetadataFromReport(TglOwnedFfdiMetadataQuery& output,
    const TglOwnedSurfaceStorage& source,uintptr_t child,uintptr_t execution,
    Read read,SfcColor sfcColor,Parameter88 parameter88) noexcept {
    static_assert(noexcept(read(uintptr_t{},static_cast<void*>(nullptr),size_t{})) &&
        noexcept(sfcColor(std::declval<uint32_t&>())) &&
        noexcept(parameter88(uintptr_t{},std::declval<uint32_t&>())),"FFDi metadata producer ABI");
    if(!child || child>UINTPTR_MAX-0x1b7 || !execution || execution>UINTPTR_MAX-0xa0f)
        return false;
    uintptr_t actualReport=0,actualSource=0;uint32_t pipe=0;uint8_t iecp=0;
    if(!read(child+0x88,&actualReport,8) || actualReport!=execution ||
        !read(child+0x1b0,&actualSource,8) || actualSource!=reinterpret_cast<uintptr_t>(&source) ||
        !read(execution+0xf,&iecp,1) || !read(execution+0xa0c,&pipe,4) || pipe>2) return false;
    TglOwnedFfdiMetadataQuery candidate;
    if(pipe==1) {if(!sfcColor(candidate.colorSpace)) return false;}
    else std::memcpy(&candidate.colorSpace,source.prefix.data(),4);
    bool preserveSample=false;
    if(iecp&1) {
        uintptr_t parameter=0;std::memcpy(&parameter,source.prefix.data()+0x88,8);
        if(parameter) {
            uint32_t kind=0;if(!parameter88(parameter,kind)) return false;
            preserveSample=kind==1;
        }
    }
    preserveSample=preserveSample || (source.prefix[0x81]&1)!=0;
    if(preserveSample) std::memcpy(&candidate.sampleType,source.prefix.data()+0x138,4);
    output=candidate;return true;
}
// One captured frame composes report-derived request/metadata with concrete
// descriptor owners and execution-owner publication. The platform's held SFC
// and parameter owners supply only their qualified scalar readers. Count is
// still caller-qualified; this entry does not invent a platform slot count.
template<class Owner,class ChildOwner,class ExecutionOwner,class Read,
    class SfcFormat,class SfcColor,class ParameterKind,class Supports,class Consume>
int tglRunCapturedFfdiProducers(Owner& input,ChildOwner& childOwner,ExecutionOwner& reportOwner,
    void* child,uintptr_t report,const TglOwnedSurfaceStorage& source,
    Read read,SfcFormat sfcFormat,SfcColor sfcColor,ParameterKind parameterKind,
    Supports supports,Consume consume) noexcept {
    return tglRunCapturedVeboxAllocation(input,childOwner,reportOwner,child,report,source,
        [&](void* currentChild,uintptr_t currentReport,const auto& currentSource) noexcept {
            TglOwnedFfdiAllocationRequest request;TglOwnedFfdiMetadataQuery metadata;
            const auto address=reinterpret_cast<uintptr_t>(currentChild);
            if(!tglPrepareOwnedFfdiRequestFromReport(request,currentSource,address,currentReport,
                read,sfcFormat,supports) ||
               !tglPrepareOwnedFfdiMetadataFromReport(metadata,currentSource,address,currentReport,
                read,sfcColor,parameterKind)) return 5;
            static_assert(noexcept(consume(currentSource,currentReport,request,metadata)) &&
                std::is_same_v<decltype(consume(currentSource,currentReport,request,metadata)),int>,
                "FFDi producer consumer ABI");
            return consume(currentSource,currentReport,request,metadata);
        });
}
template<size_t Count,class Owner,class ChildOwner,class ExecutionOwner,class Read,
    class SfcFormat,class SfcColor,class ParameterKind,class Supports,class Use>
int tglRunCapturedDescriptorFfdiFromReport(
    std::array<TglOwnedDescriptorOnlySurface,Count>& targets,Owner& input,
    Owner* executionSurface,Owner* secondary,ChildOwner& childOwner,ExecutionOwner& reportOwner,
    void* child,uintptr_t report,const TglOwnedSurfaceStorage& source,size_t count,
    Read read,SfcFormat sfcFormat,SfcColor sfcColor,ParameterKind parameterKind,
    Supports supports,Use use,std::array<const TglOwnedSurfaceStorage*,3> captured={}) noexcept {
    return tglRunCapturedFfdiProducers(input,childOwner,reportOwner,child,report,source,
        read,sfcFormat,sfcColor,parameterKind,supports,
        [&](const auto& currentSource,uintptr_t currentReport,const auto& request,const auto& metadata) noexcept {
            if(captured[0] && captured[0]!=&currentSource) return 5;
            captured[0]=&currentSource;
            return tglWithOwnedDescriptorFfdiReportBatch(targets,input,executionSurface,secondary,
                count,request.key.format,request.key.tile,
                {request.key.compressed,request.key.compressionMode},metadata,
                reportOwner,currentReport,use,captured);
        });
}
// Dispatch only on the native request's allocation kind. Both paths share
// the same frame/producers and report owner; backing is never represented by
// descriptor-only storage, and an inactive path is neither allocated nor reset.
template<size_t Count,class TargetOwner,class Owner,class ChildOwner,class ExecutionOwner,class Read,
    class SfcFormat,class SfcColor,class ParameterKind,class Supports,class Use>
int tglRunCapturedFfdiFromReport(std::array<TglOwnedDescriptorOnlySurface,Count>& descriptors,
    const std::array<TargetOwner*,Count>& backed,Owner& input,Owner* executionSurface,Owner* secondary,
    ChildOwner& childOwner,ExecutionOwner& reportOwner,void* child,uintptr_t report,
    const TglOwnedSurfaceStorage& source,Read read,SfcFormat sfcFormat,
    SfcColor sfcColor,ParameterKind parameterKind,Supports supports,Use use,
    std::array<const TglOwnedSurfaceStorage*,3> captured={}) noexcept {
    return tglRunCapturedFfdiProducers(input,childOwner,reportOwner,child,report,source,
        read,sfcFormat,sfcColor,parameterKind,supports,
        [&](const auto& currentSource,uintptr_t currentReport,const auto& request,const auto& metadata) noexcept {
            // Read the history-produced count only inside captured child/report
            // admission. Zero-slot secondary-only assembly is not implemented;
            // refuse it rather than silently skip its native metadata work.
            TglVeboxHistoryState history;size_t count=0;
            const auto address=reinterpret_cast<uintptr_t>(child);
            if(address>UINTPTR_MAX-0x1b5f ||
                !read(address+0x1b5c,&history.field1b5c,4) ||
                !tglOwnedFfdiCountFromHistory(count,history,Count) || !count) return 5;
            if(captured[0] && captured[0]!=&currentSource) return 5;
            captured[0]=&currentSource;
            if(request.kind==TglOwnedFfdiAllocationKind::DescriptorOnly)
                return tglWithOwnedDescriptorFfdiReportBatch(descriptors,input,executionSurface,secondary,
                    count,request.key.format,request.key.tile,
                    {request.key.compressed,request.key.compressionMode},metadata,
                    reportOwner,currentReport,use,captured);
            std::array<uint8_t,2> original{};
            const int status=reportOwner.withLeasedAdmissionBytes(currentReport,[&](auto& view) noexcept {
                static_assert(TglAdmissionExecutionExtent<std::decay_t<decltype(view)>>::value>=0x15,
                    "FFDi report snapshot extent");
                std::memcpy(original.data(),view.data()+0x13,2);return 0;
            });
            if(status) return status;
            return tglWithOwnedBackedFfdiBatch(backed,input,executionSurface,secondary,count,request,metadata,
                original[0],original[1],[&](uint8_t flag,uint8_t mode) noexcept {
                    return tglPublishLeasedFfdiCompression(reportOwner,currentReport,flag,mode);
                },use,captured);
        });
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
// Optional slots8/9/10 are real relocation inputs, not padding: exact native
// 17281c/1728ba/172958 gate pointers60/68/70 independently, pair controls
// 98/9c/a0, and patch command bytes48/50/58 (DWORD offsets12/14/16).
// Each uses relocation usage21 and write=true. The current owned producer
// leaves these slots absent; do not claim their feature/owner admission from
// the eight-slot producer tests or populate them without a qualified source.
// Donor producer1f8f50 clears the entire a8 packet before1f9006..1f9929;
// its direct resource stores end at58. Thus absent60/68/70 matches that
// producer, not proof that every native platform/mode omits these features.
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
// Exact 25G229 IOAccelerator FinishFenceEvent implementation at preferred
// 7ff80f4634cf. Resolve receives the authenticated export implementation (not
// its thunk); qualify must enforce the pinned framework UUID. Keep its image
// and context leases alive through finish. Raw success is NOT a retirement
// certificate: family reset/skip policy and actual stamps remain independent.
// Exact userspace47-byte body passes scalar selector5/index on context+18.
// Family finish_fence_event7450 validates fence storage6a8 and index against
// 6d8>>6, then invokes EventMachine+178. Fast2 vtable78930+10+178 contains
// 30280 (finishEventUnlocked), NOT30810 (finishEvent). At302ef..302fa,
// device+dc8!=0 skips a pending stamp; initial EAX0 can survive to return.
// Outer756e only maps -1 to e00002d6; other results leave success unchanged.
// Consequently the native API's success also does not distinguish termination
// from execution. Separately, waitForStamp12954 returns0 immediately when
// dc8!=0 (12974..1297d->12b78); a wait result is not a live-generation gate.
struct TglNativeVideoFenceEventInvoker {
    uint32_t operator()(uintptr_t entry,uintptr_t context,uint32_t index) const noexcept {
        using Finish=uint32_t(*)(void*,uint32_t);
        return reinterpret_cast<Finish>(entry)(reinterpret_cast<void*>(context),index);
    }
};
class TglNativeVideoFenceEventBinding {
    uintptr_t entry_=0;
public:
    template<class Read,class Qualify,class Executable>
    bool resolve(uintptr_t implementation,Read read,Qualify qualify,Executable executable) noexcept {
        entry_=0;
        constexpr std::array<uint8_t,47> expected{
            0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x10,0x89,0xf0,0x48,0x8d,0x55,0xf8,
            0x48,0x89,0x02,0x8b,0x7f,0x18,0xbe,0x05,0x00,0x00,0x00,0xb9,0x01,0x00,
            0x00,0x00,0x45,0x31,0xc0,0x45,0x31,0xc9,0xe8,0x6c,0x42,0x00,0x00,
            0x48,0x83,0xc4,0x10,0x5d,0xc3};
        std::array<uint8_t,47> actual{};
        if(!implementation || implementation>UINTPTR_MAX-expected.size() ||
            !qualify(implementation) || !executable(implementation,expected.size()) ||
            !read(implementation,actual.data(),actual.size()) || actual!=expected) return false;
        entry_=implementation;return true;
    }
    template<class Invoke=TglNativeVideoFenceEventInvoker>
    uint32_t finish(uintptr_t context,uint32_t index,Invoke invoke=Invoke{}) const noexcept {
        static_assert(noexcept(invoke(uintptr_t{},uintptr_t{},uint32_t{})),"native fence result ABI");
        if(!entry_ || !context || context>UINTPTR_MAX-0x1c) return 5;
        return invoke(entry_,context,index);
    }
};
// OS2d0/64df0 calls a9dc(context,2) only after command/sideband bounds demand
// a flush. a9dc finalizes both buffers, then a893 sets class-used lengths and
// calls SubmitDataBuffers(context8,flags50,&flags54,&error). Native a893
// rejects error/flag20000000 and refreshes buffer resources on success. Do
// not replace this with FinishFenceEvent or call it twice per native flush.
// Family affectedByHardwareReset ae6e sets context638 bits20000000 (affected)
// and40000000 (always), records644 and increments63c on the affected branch.
// submit_data_buffers6f74 emits644 at output+4 and, ONLY while device dc8==0,
// merges638 into output+8 at6f98. It then clears644/648 and masks638 down to
// 08000000 at6fc5/6fd0. TGL a8eb passes context+54 as that output-flags sink;
// a903..a912 rejects20000000/error. These are consumable submission reports,
// not a persistent userspace generation counter: zero flags before/after a
// fence cannot exclude an intervening reset or the terminated skip branch.
// get_data_buffer7144 is not an established reset-generation query either:
// getDataBuffer a6da..a74e publishes resource+38->100 at output28, shared
// addresses18/20 and mapped buffer address/size0/8. Its pending-buffer wait
// a1cb again uses EventMachine+178. None of those examined fields establishes
// a persistent context reset counter; do not reinterpret output28 as one.
// Caller owns the native context, writable command/sideband and all resource
// leases through this call AND subsequent independently verified completion.
struct TglNativeVideoBufferFlushInvoker {
    uint32_t operator()(uintptr_t entry,uintptr_t context,uint8_t kind) const noexcept {
        using Flush=uint32_t(*)(void*,uint8_t);
        return reinterpret_cast<Flush>(entry)(reinterpret_cast<void*>(context),kind);
    }
};
class TglNativeVideoBufferFlushBinding {
    uintptr_t entry_=0;
public:
    template<class Read,class Qualify,class Executable>
    bool resolve(uintptr_t image,Read read,Qualify qualify,Executable executable) noexcept {
        entry_=0;
        constexpr std::array<uint8_t,21> expected{
            0x55,0x48,0x89,0xe5,0x41,0x56,0x53,0x48,0x89,0xfb,0x48,0x8b,0x47,0x20,
            0x48,0x85,0xc0,0x74,0x5c,0xa8,0x07};
        std::array<uint8_t,21> actual{};
        if(!image || image>UINTPTR_MAX-0xa9f1 || !qualify(image) ||
            !executable(image+0xa9dc,expected.size()) ||
            !read(image+0xa9dc,actual.data(),actual.size()) || actual!=expected) return false;
        entry_=image+0xa9dc;return true;
    }
    template<class Invoke=TglNativeVideoBufferFlushInvoker>
    uint32_t flush(uintptr_t context,Invoke invoke=Invoke{}) const noexcept {
        static_assert(noexcept(invoke(uintptr_t{},uintptr_t{},uint8_t{})),"native flush result ABI");
        if(!entry_ || !context || context>UINTPTR_MAX-0xe4) return 5;
        return invoke(entry_,context,2);
    }
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
    // heapTagSource authenticates fields through +2d8 only; this reader also
    // consumes the full DWORD at +2e0. Reject wrapping ranges before reads.
    if(!binding.heap || binding.heap>UINTPTR_MAX-0x2e4) return false;
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
    // Numeric snapshot only; never retain the unlocked CPU mapping pointer.
    // Published only after successful initialization AND unlock.
    uint32_t width=0,height=0,pitch=0;
    int status() const noexcept {
        return lockStatus ? lockStatus : !initialized ? 5 : unlockStatus;
    }
    bool ready() const noexcept {return status()==0;}
    // Distinguish incomplete CPU ownership release from an ordinary prepare
    // error. Never pass this case to ensure()'s automatic error unwind.
    bool requiresQuarantine() const noexcept {return lockStatus==0 && unlockStatus!=0;}
};
// A failed unlock has no proven retry/release authority. Keep its independent
// retained IOSurface reference until process teardown, with a hard two-slot
// budget (the STMM pair). No frame-owned destructor can release these slots.
// This is CPU-lock quarantine, NOT a GPU retirement/fence implementation.
template<class Surface> class TglOwnedStmmProcessQuarantine {
    static_assert(std::is_pointer<Surface>::value || std::is_integral<Surface>::value,
        "quarantine stores native handles, not destructible smart owners");
    std::array<Surface,2> slots_{};
    std::atomic_flag gate_=ATOMIC_FLAG_INIT;
    TglOwnedStmmProcessQuarantine()=default;
    ~TglOwnedStmmProcessQuarantine()=default;
public:
    TglOwnedStmmProcessQuarantine(const TglOwnedStmmProcessQuarantine&)=delete;
    TglOwnedStmmProcessQuarantine& operator=(const TglOwnedStmmProcessQuarantine&)=delete;
    static TglOwnedStmmProcessQuarantine& processLifetime() noexcept {
        static TglOwnedStmmProcessQuarantine storage;
        return storage;
    }
    class Reservation {
        TglOwnedStmmProcessQuarantine& owner_;
        size_t index_=2;
    public:
        explicit Reservation(TglOwnedStmmProcessQuarantine& owner) noexcept :owner_(owner) {}
        Reservation(const Reservation&)=delete;
        Reservation& operator=(const Reservation&)=delete;
        ~Reservation(){cancel();}
        bool reserve(Surface surface) noexcept {
            if(!surface || index_!=2 || owner_.gate_.test_and_set(std::memory_order_acquire)) return false;
            for(const auto old:owner_.slots_) if(old==surface) {
                owner_.gate_.clear(std::memory_order_release);return false;
            }
            for(size_t i=0;i<2;++i) if(!owner_.slots_[i]) {index_=i;return true;}
            owner_.gate_.clear(std::memory_order_release);return false;
        }
        void cancel() noexcept {
            if(index_==2) return;
            index_=2;owner_.gate_.clear(std::memory_order_release);
        }
        // Contract: successful reserve and a nonnull independently retained
        // handle. Assignment cannot allocate/fail; no native call while locked.
        void commit(Surface surface) noexcept {
            if(index_==2) return;
            owner_.slots_[index_]=surface;
            cancel();
        }
    };
    bool count(size_t& output) noexcept {
        if(gate_.test_and_set(std::memory_order_acquire)) return false;
        size_t value=0;for(const auto surface:slots_) if(surface) ++value;
        gate_.clear(std::memory_order_release);output=value;return true;
    }
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
    if(result.ready()) {
        result.width=view.width;result.height=view.height;result.pitch=view.pitch;
    }
    return result;
}
// Independent surface retention protects a failed writable unlock even when
// the enclosing resource transaction unwinds. The sink must reserve durable
// capacity BEFORE lock, then own the retained reference after commit; cancel
// releases only the reservation. It must outlive resource/frame owners.
// No unlock retry, GPU completion inference or automatic quarantine release.
// Runtime integration still requires an actual persistent sink implementation.
template<class Surface,class Api,class Quarantine>
TglOwnedStmmInitialization tglInitializeRetainedOwnedStmmSurface(
    Surface surface,Api& api,Quarantine& quarantine) noexcept {
    static_assert(noexcept(api.retain(surface)) && noexcept(api.release(surface)),
        "surface retention must not throw");
    static_assert(noexcept(quarantine.reserve(surface)) && noexcept(quarantine.cancel()) &&
        noexcept(quarantine.commit(surface)),"durable quarantine transaction must not throw");
    static_assert(std::is_same<decltype(quarantine.reserve(surface)),bool>::value &&
        std::is_same<decltype(quarantine.commit(surface)),void>::value &&
        std::is_same<decltype(quarantine.cancel()),void>::value,
        "reserve reports capacity; commit cannot fail after reservation");
    if(!surface || !quarantine.reserve(surface)) return {};
    if(!api.retain(surface)) {quarantine.cancel();return {};}
    const auto result=tglInitializeOwnedStmmSurface(surface,api);
    if(result.requiresQuarantine()) quarantine.commit(surface);
    else {api.release(surface);quarantine.cancel();}
    return result;
}
#if defined(__APPLE__)
inline bool tglPinOwnedStmmQuarantineImage() noexcept {
    // Hold our own loaded image and its fixed budget across dlclose/reopen.
    // Native dyld policy decides; failure denies retain and hence writable lock.
    static const bool pinned=[]() noexcept {
        Dl_info image{};
        if(!dladdr(reinterpret_cast<const void*>(&tglPinOwnedStmmQuarantineImage),
            &image) || !image.dli_fname || !image.dli_fbase) return false;
        return dlopen(image.dli_fname,RTLD_NOW|RTLD_LOCAL|RTLD_NOLOAD|RTLD_NODELETE)!=nullptr;
    }();
    return pinned;
}
struct TglDarwinStmmSurfaceApi {
    IOSurfaceRef surface(uintptr_t handle) const noexcept {
        return reinterpret_cast<IOSurfaceRef>(handle);
    }
    int lock(IOSurfaceRef surface) const noexcept {return IOSurfaceLock(surface,0,nullptr);}
    int unlock(IOSurfaceRef surface) const noexcept {return IOSurfaceUnlock(surface,0,nullptr);}
    bool retain(IOSurfaceRef surface) const noexcept {
        return surface && tglPinOwnedStmmQuarantineImage() && CFRetain(surface);
    }
    void release(IOSurfaceRef surface) const noexcept {CFRelease(surface);}
    bool view(IOSurfaceRef surface,TglOwnedStmmView& output) const noexcept {
        // Native63720 format3d -> IOSurface pixel-format8 at63a41, one plane.
        // SDK explicitly permits plane0 queries on a non-planar (count0) surface.
        if(IOSurfaceGetPixelFormat(surface)!=8 || IOSurfaceGetPlaneCount(surface)>1) return false;
        const auto base=reinterpret_cast<uintptr_t>(IOSurfaceGetBaseAddress(surface));
        const auto plane=reinterpret_cast<uintptr_t>(IOSurfaceGetBaseAddressOfPlane(surface,0));
        const size_t capacity=IOSurfaceGetAllocSize(surface);
        const size_t width=IOSurfaceGetWidthOfPlane(surface,0);
        const size_t height=IOSurfaceGetHeightOfPlane(surface,0);
        // Exact TGL63e9f imports IOSurfaceGetBytesPerRowOfPlane(0), then
        //63ea8 writes resource+8. STMM12b03b/12b05f publishes that value to
        //surface+e0 only after OS220 lock returned nonnull. Thus pitch comes
        //from the locked native surface, never requested allocation width.
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
template<class Api,class Quarantine>
TglOwnedStmmInitialization tglInitializeRetainedOwnedStmmResource(
    const std::array<uint8_t,0x148>& resource,Api& api,Quarantine& quarantine) noexcept {
    uint32_t type=0,format=0;
    uintptr_t handle=0;
    std::memcpy(&type,resource.data()+0x14,4);
    std::memcpy(&format,resource.data()+0x18,4);
    std::memcpy(&handle,resource.data()+0x58,8);
    if(type!=1 || format!=0x3d || !handle) return {};
    return tglInitializeRetainedOwnedStmmSurface(api.surface(handle),api,quarantine);
}
// STMM-specific resource transaction. Backend/API/sink outlive this owner;
// the sink independently owns failed-unlock references, not native tokens.
// Before ensure/reset the caller must exclude GPU users (not proven here).
template<class Backend,class Api,class Quarantine> class TglOwnedStmmResource {
    TglOwnedResource<Backend> resource_;
    Api& api_;
    Quarantine& quarantine_;
    TglOwnedStmmInitialization initialization_{};
    bool ready_=false,busy_=false;
public:
    using Storage=typename TglOwnedResource<Backend>::Storage;
    TglOwnedStmmResource(Backend& backend,Api& api,Quarantine& quarantine) noexcept
        :resource_(backend),api_(api),quarantine_(quarantine) {}
    // Bind an empty native shell's resource bytes without moving/copying handles.
    // Shell storage, backend, API and sink must all outlive this owner.
    TglOwnedStmmResource(Backend& backend,Storage& storage,Api& api,Quarantine& quarantine) noexcept
        :resource_(backend,storage),api_(api),quarantine_(quarantine) {}
    TglOwnedStmmResource(const TglOwnedStmmResource&)=delete;
    TglOwnedStmmResource& operator=(const TglOwnedStmmResource&)=delete;
    ~TglOwnedStmmResource(){reset();}
    const void* storageBinding() const noexcept {return resource_.storage().data();}
    int ensure(const TglResourceKey& key,bool reinitializeHistory=false) noexcept {
        if(busy_ || key.format!=0x3d) return 5;
        busy_=true;
        const auto result=resource_.ensure(key);
        using State=typename TglOwnedResource<Backend>::State;
        int status=result.status;
        if(!status && result.state!=State::Backed) status=5;
        if(!status && (result.changed || !ready_ || reinitializeHistory))
            status=resource_.withStorage([&](const Storage& storage) noexcept {
                initialization_=tglInitializeRetainedOwnedStmmResource(storage,api_,quarantine_);
                return initialization_.status();
            });
        ready_=status==0;
        if(status) {initialization_={};resource_.reset();}
        busy_=false;return status;
    }
    template<class Invoke> int withStorage(Invoke invoke) noexcept {
        if(busy_ || !ready_) return 5;
        busy_=true;
        const int status=resource_.withStorage(invoke);
        busy_=false;return status;
    }
    // Revalidate backing and pin this owner while publishing numeric metadata.
    // No unlocked mapping pointer or ownership-bearing resource copy escapes.
    template<class Invoke> int withInitializedStorage(Invoke invoke) noexcept {
        return withStorage([&](const Storage& storage) noexcept {
            return invoke(storage,initialization_.width,initialization_.height,
                          initialization_.pitch);
        });
    }
    void reset() noexcept {
        if(busy_) return;
        busy_=true;ready_=false;initialization_={};resource_.reset();busy_=false;
    }
};
struct TglResourceStorageReference { std::array<uint8_t,0x148>& storage; };
// Native12b05f publishes the locked IOSurface pitch to this exact shell.
// Bind identity, not equal descriptor bytes: copying tokens cannot establish
// ownership. The enclosing frame still excludes asynchronous GPU users.
template<class Owner,class Invoke>
int tglWithOwnedStmmSurface(Owner& owner,TglOwnedSurfaceStorage& surface,
                            Invoke invoke) noexcept {
    return owner.withInitializedStorage([&](const auto& storage,uint32_t,
                                           uint32_t,uint32_t pitch) noexcept {
        if(&storage!=&surface.resource || !pitch) return 5;
        std::memcpy(surface.prefix.data()+0xe0,&pitch,4);
        return invoke(surface);
    });
}
// The native history graph has two STMM surfaces. Allocate/init failure must
// retire BOTH native tokens without depending on caller FreeResources. A
// failed-unlock reference stays with the process quarantine independently.
// The enclosing frame owner excludes GPU users before ensure/reset.
template<class Backend,class Api,class Quarantine> class TglOwnedStmmPair {
    using Owner=TglOwnedStmmResource<Backend,Api,Quarantine>;
    std::array<Owner,2> owners_;
    bool busy_=false,ready_=false,valid_=false;
public:
    using Storage=typename Owner::Storage;
    TglOwnedStmmPair(std::array<Backend,2>& backends,
        const std::array<TglResourceStorageReference,2>& storage,
        Api& api,Quarantine& quarantine) noexcept
        :owners_{Owner(backends[0],storage[0].storage,api,quarantine),
                 Owner(backends[1],storage[1].storage,api,quarantine)},
         valid_(&storage[0].storage!=&storage[1].storage) {}
    TglOwnedStmmPair(const TglOwnedStmmPair&)=delete;
    TglOwnedStmmPair& operator=(const TglOwnedStmmPair&)=delete;
    ~TglOwnedStmmPair(){reset();}
    template<class Visit> void visitStorageBindings(Visit visit) const noexcept {
        for(const auto& owner:owners_) visit(owner.storageBinding());
    }
    int ensure(const std::array<TglResourceKey,2>& keys,bool reinitialize=false) noexcept {
        if(busy_ || !valid_) return 5;
        busy_=true;ready_=false;
        int status=0;
        for(size_t i=0;i<2 && !status;++i) status=owners_[i].ensure(keys[i],reinitialize);
        if(status) for(auto& owner:owners_) owner.reset();
        ready_=status==0;busy_=false;return status;
    }
    template<class Invoke> int withStorage(Invoke invoke) noexcept {
        if(busy_ || !ready_) return 5;
        busy_=true;
        const int status=owners_[0].withStorage([&](const Storage& first) noexcept {
            return owners_[1].withStorage([&](const Storage& second) noexcept {
                return invoke(first,second);
            });
        });
        busy_=false;return status;
    }
    template<class Invoke> int withInitializedStorage(Invoke invoke) noexcept {
        if(busy_ || !ready_) return 5;
        busy_=true;
        const int status=owners_[0].withInitializedStorage(
            [&](const Storage& first,uint32_t w0,uint32_t h0,uint32_t p0) noexcept {
                return owners_[1].withInitializedStorage(
                    [&](const Storage& second,uint32_t w1,uint32_t h1,uint32_t p1) noexcept {
                        const std::array<uint32_t,3> a{w0,h0,p0},b{w1,h1,p1};
                        return invoke(first,second,a,b);
                    });
            });
        busy_=false;return status;
    }
    template<class Invoke> int withSurfaces(TglOwnedSurfaceStorage& first,
        TglOwnedSurfaceStorage& second,Invoke invoke) noexcept {
        return withInitializedStorage([&](const Storage& a,const Storage& b,
            const auto& firstLayout,const auto& secondLayout) noexcept {
            if(&a!=&first.resource || &b!=&second.resource ||
                !firstLayout[2] || !secondLayout[2]) return 5;
            // Both backing leases and identities precede either publication.
            std::memcpy(first.prefix.data()+0xe0,&firstLayout[2],4);
            std::memcpy(second.prefix.data()+0xe0,&secondLayout[2],4);
            return invoke(first,second);
        });
    }
    void reset() noexcept {
        if(busy_) return;
        busy_=true;ready_=false;
        for(auto& owner:owners_) owner.reset();
        busy_=false;
    }
    // Reserve before a multi-owner retirement decision. Release is synchronous
    // and single-use; it must not escape this callback. No GPU proof is inferred.
    template<class Use> int withReleaseReservation(Use use) noexcept {
        if(busy_ || !valid_) return 5;
        busy_=true;bool released=false;
        auto release=[&]() noexcept {
            if(released) return;
            released=true;ready_=false;
            for(auto& owner:owners_) owner.reset();
        };
        static_assert(noexcept(use(release)),"release reservation must not throw");
        const int result=use(release);
        busy_=false;return result;
    }
};
#if defined(__APPLE__)
// Call only under the resource owner's synchronous lease, before GPU use.
// The pool is process-owned, not a member destroyed by frame-error unwind.
inline TglOwnedStmmInitialization tglInitializeQuarantinedOwnedStmmResource(
    const std::array<uint8_t,0x148>& resource) noexcept {
    TglDarwinStmmSurfaceApi api;
    using Pool=TglOwnedStmmProcessQuarantine<IOSurfaceRef>;
    Pool::Reservation reservation(Pool::processLifetime());
    return tglInitializeRetainedOwnedStmmResource(resource,api,reservation);
}
#endif
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

// Owned all-or-nothing allocation, following the reference Gen12 resource
// transaction, not a claim about TGL's virtual Allocate failure cleanup.
// TGL mode0 returns a +58 failure directly; this owner must unwind its own
// partial allocations rather than depend on a subsequent native +60 call.
// Composes descriptor owners, not Linux object offsets or feature admission.
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
    template<class Visit> void visitStorageBindings(Visit visit) const noexcept {
        for(const auto& resource:resources) visit(static_cast<const void*>(resource.storage().data()));
    }
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
    // Retain every present backing while a frame callback uses the group.
    // Empty optional resources are represented by null views. This is a CPU
    // lifetime scope, not permission to release an asynchronously used frame.
    template<class Invoke> int withStorage(Invoke invoke) noexcept {
        using Views=std::array<const typename Resource::Storage*,Count>;
        static_assert(noexcept(invoke(std::declval<const Views&>())),"group borrow ABI");
        if(busy || !ready || !bindingsValid) return 5;
        busy=true;
        Views views{};
        const auto enter=[&](const auto& self,size_t index) noexcept -> int {
            if(index==Count) return invoke(static_cast<const Views&>(views));
            if(resources[index].state()==Resource::State::Empty)
                return self(self,index+1);
            return resources[index].withStorage([&](const auto& storage) noexcept {
                views[index]=&storage;
                const int result=self(self,index+1);
                views[index]=nullptr;
                return result;
            });
        };
        const int result=enter(enter,0);
        busy=false;
        return result;
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
    using Storage=typename TglOwnedResource<Backend>::Storage;
    std::array<const Storage*,3> leasedViews_{};
    uint32_t inputHeight_=0,scaledHeight_=0;
    bool leased_=false,mutating_=false;
public:
    explicit TglOwnedSfcLineBuffers(Backend& backend) noexcept : owners(backend) {}
    // Native +58 1df25a/1df2d5 passes surfaces at VP+3c8/+670;
    // resource storage is surface+148, not the start of either surface.
    // Bind empty owned descriptor storage by identity, never copy an existing
    // native token. The enclosing VP/surface/SFD storage and backend array
    // outlive this owner; this constructor does not prove native metadata.
    TglOwnedSfcLineBuffers(std::array<Backend,3>& backends,
        const std::array<TglResourceStorageReference,3>& storage) noexcept
        :owners(backends,storage) {}
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
    // ICL VP+30 (1df1b0..1df20b) releases line resources via OS+200;
    // it neither destroys the VP nor waits for GPU completion. Our Gen12
    // owner also retains SFD. The caller must establish discard/retirement
    // before this CPU release; success here is NOT a fence certificate.
    bool tryReset() noexcept {
        if(leased_ || mutating_) return false;
        mutating_=true;
        inputHeight_=scaledHeight_=0;owners.reset();
        mutating_=false;return true;
    }
    void reset() noexcept {(void)tryReset();}
    template<class Use> int withReleaseReservation(Use use) noexcept {
        if(leased_ || mutating_) return 5;
        mutating_=true;bool released=false;
        auto release=[&]() noexcept {
            if(released) return;
            released=true;inputHeight_=scaledHeight_=0;owners.reset();
        };
        static_assert(noexcept(use(release)),"release reservation must not throw");
        const int result=use(release);
        mutating_=false;return result;
    }
    // Pins synchronous CPU preparation only, NOT GPU completion. The enclosing
    // frame still retains this owner through discard or an actual fence.
    template<class Use> int withBuffers(uint32_t inputHeight,uint32_t scaledHeight,Use use) noexcept {
        static_assert(noexcept(use()),"SFC buffer lease status ABI");
        if(leased_ || mutating_ || !matchesDimensions(inputHeight,scaledHeight)) return 5;
        leased_=true;
        const int result=owners.withStorage([&](const auto& views) noexcept {
            leasedViews_=views;
            const int status=use();
            leasedViews_={};return status;
        });
        leased_=false;return result;
    }
    bool matchesDimensions(uint32_t inputHeight,uint32_t scaledHeight) const noexcept {
        return !mutating_ && storage(0) && storage(1) &&
            inputHeight_==inputHeight && scaledHeight_==scaledHeight;
    }
    // Borrow only until reset/ensure/destruction; caller retains the owner
    // through command completion and rebinds after any resource mutation.
    // Failed admission explicitly revokes the SFD pointer, not other fields.
    bool bindSfdRow(TglOwnedSfcGen12Tail& tail) const noexcept {
        tail.sfdLineBuffer=0;
        if(mutating_) return false;
        if(!storage(0) || !storage(1)) return false;
        const auto* sfd=storage(2);
        if(sfd) tail.sfdLineBuffer=reinterpret_cast<uintptr_t>(sfd->data());
        return true;
    }
    const typename TglOwnedResource<Backend>::Storage* storage(size_t i) const noexcept {
        return mutating_ ? nullptr : leased_ ?
            (i<leasedViews_.size() ? leasedViews_[i] : nullptr) : owners.storage(i);
    }
};

// Native VP+58 has two full surfaces (3c8/670), not two copied resource
// descriptors. Empty caller-owned shells/backends outlive this transaction.
// SFD is a separate Gen12 resource; this pair does not claim to supply it.
template<class Backend> class TglOwnedVpLineSurfaces {
    std::array<Backend,2>& backends_;
    std::array<TglOwnedAllocatedSurface<Backend>,2> owners_;
    std::array<uintptr_t,2> bindings_{};
    bool valid_=false,busy_=false,ready_=false;
    uint32_t inputHeight_=0,scaledHeight_=0;
    struct Guard {
        bool& flag;
        explicit Guard(bool& value) noexcept:flag(value){flag=true;}
        ~Guard(){flag=false;}
    };
    void clear() noexcept {
        ready_=false;inputHeight_=scaledHeight_=0;
        for(auto& owner:owners_) owner.reset();
    }
public:
    TglOwnedVpLineSurfaces(std::array<Backend,2>& backends,
        TglOwnedSurfaceStorage& avs,TglOwnedSurfaceStorage& ief) noexcept
        :backends_(backends),owners_{TglOwnedAllocatedSurface<Backend>(backends[0],avs),
            TglOwnedAllocatedSurface<Backend>(backends[1],ief)} {
        const auto a=reinterpret_cast<uintptr_t>(&avs),b=reinterpret_cast<uintptr_t>(&ief);
        bindings_={a,b};
        valid_=a<=UINTPTR_MAX-sizeof(avs) && b<=UINTPTR_MAX-sizeof(ief) &&
            !(a<b+sizeof(ief) && b<a+sizeof(avs));
    }
    ~TglOwnedVpLineSurfaces(){reset();}
    TglOwnedVpLineSurfaces(const TglOwnedVpLineSurfaces&)=delete;
    TglOwnedVpLineSurfaces& operator=(const TglOwnedVpLineSurfaces&)=delete;
    bool boundToVp(const TglOwnedVpSfcCallerStorage& vp) const noexcept {
        const auto base=reinterpret_cast<uintptr_t>(vp.bytes.data());
        return valid_ && base<=UINTPTR_MAX-0x9d0 &&
            bindings_[0]==base+0x3c8 && bindings_[1]==base+0x670;
    }
private:
    int allocateReserved(uint32_t inputHeight,uint32_t scaledHeight) noexcept {
        TglOwnedSfcLineBufferSizes sizes;
        if(!tglOwnedSfcLineBufferSizes(sizes,inputHeight,scaledHeight)) return 5;
        if(backends_[0].resourceType()!=0 || backends_[1].resourceType()!=0) return 25;
        ready_=false;
        const std::array<uint32_t,2> widths{sizes.avs,sizes.ief};
        for(size_t i=0;i<2;++i) {
            const int status=owners_[i].ensure({widths[i],1,0x3d,4,0,false});
            if(status) {clear();return status;}
        }
        inputHeight_=inputHeight;scaledHeight_=scaledHeight;
        ready_=true;return 0;
    }
public:
    template<class Use> int withAllocationReservation(Use use) noexcept {
        if(!valid_ || busy_) return 5;
        Guard guard(busy_);
        auto allocate=[&](uint32_t input,uint32_t scaled) noexcept {
            return allocateReserved(input,scaled);
        };
        auto release=[&]() noexcept {clear();};
        static_assert(noexcept(use(allocate,release)),"VP line allocation reservation ABI");
        return use(allocate,release);
    }
    int ensure(uint32_t inputHeight,uint32_t scaledHeight) noexcept {
        return withAllocationReservation([&](auto allocate,auto) noexcept {
            return allocate(inputHeight,scaledHeight);
        });
    }
    template<class Use> int withDimensions(uint32_t input,uint32_t scaled,Use use) noexcept {
        if(!input || !scaled || input!=inputHeight_ || scaled!=scaledHeight_) return 5;
        return withSurfaces(use);
    }
    template<class Use> int withSurfaces(Use use) noexcept {
        static_assert(noexcept(use(std::declval<const TglOwnedSurfaceStorage&>(),
            std::declval<const TglOwnedSurfaceStorage&>())),"VP line surface lease ABI");
        if(!valid_ || busy_ || !ready_) return 5;
        Guard guard(busy_);int result=5;
        owners_[0].withPeerSurface(owners_[1],[&](const auto& avs,const auto& ief) noexcept {
            result=use(avs,ief);
        });
        return result;
    }
    template<class Use> int withReleaseReservation(Use use) noexcept {
        if(!valid_ || busy_) return 5;
        Guard guard(busy_);bool released=false;
        auto release=[&]() noexcept {if(!released){released=true;clear();}};
        static_assert(noexcept(use(release)),"VP line surface retirement ABI");
        return use(release);
    }
    bool tryReset() noexcept {
        if(busy_) return false;
        Guard guard(busy_);clear();return true;
    }
    void reset() noexcept {(void)tryReset();}
};

// Separate Gen12 SFD backing; never alias the AVS/IEF surface descriptors.
// Caller establishes retirement/discard before ensure/reset. The lease below
// protects CPU emission only, not asynchronous GPU completion.
template<class Backend> class TglOwnedSfcSfdBuffer {
    Backend& backend_;
    TglOwnedAllocatedSurface<Backend> owner_;
    uint32_t height_=0;
    bool busy_=false;
public:
    explicit TglOwnedSfcSfdBuffer(Backend& backend) noexcept:backend_(backend),owner_(backend){}
private:
    int allocateReserved(uint32_t height) noexcept {
        if(!height) return 5;
        TglOwnedSfcLineBufferSizes sizes;
        if(!tglOwnedSfcLineBufferSizes(sizes,1,height)) return 5;
        if(backend_.resourceType()!=0) return 25;
        height_=0;
        int status=0;
        if(sizes.sfd) status=owner_.ensure({sizes.sfd,1,0x3d,4,0,false});
        else owner_.reset();
        if(!status) height_=height;
        return status;
    }
public:
    template<class Use> int withAllocationReservation(Use use) noexcept {
        if(busy_) return 5;
        busy_=true;
        auto allocate=[&](uint32_t height) noexcept {return allocateReserved(height);};
        auto release=[&]() noexcept {height_=0;owner_.reset();};
        static_assert(noexcept(use(allocate,release)),"SFD allocation reservation ABI");
        const int result=use(allocate,release);busy_=false;return result;
    }
    int ensure(uint32_t height) noexcept {
        return withAllocationReservation([&](auto allocate,auto) noexcept {return allocate(height);});
    }
    template<class Use> int withBuffer(uint32_t height,Use use) noexcept {
        using Storage=std::array<uint8_t,0x148>;
        static_assert(noexcept(use(static_cast<const Storage*>(nullptr))),"SFD descriptor lease ABI");
        if(busy_ || !height || height!=height_) return 5;
        TglOwnedSfcLineBufferSizes sizes;
        if(!tglOwnedSfcLineBufferSizes(sizes,1,height)) return 5;
        busy_=true;int result=5;
        if(!sizes.sfd) result=use(static_cast<const Storage*>(nullptr));
        else owner_.withSurface([&](const auto& surface) noexcept {
            result=use(&surface.resource);
        });
        busy_=false;return result;
    }
    bool tryReset() noexcept {
        if(busy_) return false;
        busy_=true;height_=0;owner_.reset();busy_=false;return true;
    }
    template<class Use> int withReleaseReservation(Use use) noexcept {
        if(busy_) return 5;
        busy_=true;bool released=false;
        auto release=[&]() noexcept {
            if(released) return;
            released=true;height_=0;owner_.reset();
        };
        static_assert(noexcept(use(release)),"SFD release reservation ABI");
        const int result=use(release);busy_=false;return result;
    }
};

// Feed the existing VP retirement/void-release contexts the entire line
// resource set. Neither native admission nor any release runs until BOTH
// reservations succeed. Failed admission retains all resources; this does
// not turn a CPU reservation into proof of native GPU completion.
template<class Lines,class Sfd> class TglOwnedVpLineResourceSet {
    Lines& lines_;Sfd& sfd_;
public:
    TglOwnedVpLineResourceSet(Lines& lines,Sfd& sfd) noexcept:lines_(lines),sfd_(sfd){}
    bool boundToVp(const TglOwnedVpSfcCallerStorage& vp) const noexcept {return lines_.boundToVp(vp);}
    int ensure(uint32_t input,uint32_t scaled) noexcept {
        TglOwnedSfcLineBufferSizes sizes;
        if(!tglOwnedSfcLineBufferSizes(sizes,input,scaled)) return 5;
        return lines_.withAllocationReservation([&](auto allocateLines,auto releaseLines) noexcept {
            return sfd_.withAllocationReservation([&](auto allocateSfd,auto releaseSfd) noexcept {
                int status=allocateLines(input,scaled);
                if(!status) status=allocateSfd(scaled);
                if(status) {releaseLines();releaseSfd();}
                return status;
            });
        });
    }
    template<class Use> int withReleaseReservation(Use use) noexcept {
        return lines_.withReleaseReservation([&](auto releaseLines) noexcept {
            return sfd_.withReleaseReservation([&](auto releaseSfd) noexcept {
                bool released=false;
                auto release=[&]() noexcept {
                    if(released) return;
                    released=true;releaseLines();releaseSfd();
                };
                static_assert(noexcept(use(release)),"VP line resource-set retirement ABI");
                return use(release);
            });
        });
    }
};

// Exact slot58=1df210 is status(this). State+30/+4c are read only from
// the actual leased owner already published at VP+938, not arbitrary args.
template<class Lines,class State> class TglOwnedVpLineAllocationContext {
    TglOwnedVpSfcCallerStorage& vp_;
    Lines& lines_;
    State& state_;
    bool busy_=false;
public:
    TglOwnedVpLineAllocationContext(TglOwnedVpSfcCallerStorage& vp,Lines& lines,State& state) noexcept
        :vp_(vp),lines_(lines),state_(state) {}
    bool owns(void* child) const noexcept {return child==vp_.bytes.data();}
    int allocate() noexcept {
        if(busy_ || !lines_.boundToVp(vp_)) return 5;
        busy_=true;
        const int result=state_.withPublishedVpParameters(vp_,[&](const uint8_t* parameters) noexcept {
            uint32_t inputHeight=0,scaledHeight=0;
            std::memcpy(&inputHeight,parameters+0x30,4);
            std::memcpy(&scaledHeight,parameters+0x4c,4);
            return lines_.ensure(inputHeight,scaledHeight);
        });
        busy_=false;return result;
    }
};
template<class Resolve> struct TglOwnedVpLineAllocationCallback {
    static int allocate(void* child) noexcept {
        static_assert(noexcept(Resolve{}(child)),"VP line allocation resolver ABI");
        auto* context=Resolve{}(child);
        static_assert(noexcept(context->owns(child)) && noexcept(context->allocate()),
            "VP line allocation context ABI");
        return context && context->owns(child) ? context->allocate() : 5;
    }
};

// Reserve both owners before asking for retirement. No partial teardown when
// a CPU/line lease blocks release; a failed retirement leaves both intact.
// This composes actual tracked line resources with the CPU graph, but does
// not authenticate a fence or free the enclosing VP allocation itself.
template<class Graph,class Lines,class Retire>
int tglRetireOwnedVpResources(Graph& graph,Lines& lines,
    Retire retire) noexcept {
    static_assert(noexcept(retire()) && std::is_same_v<decltype(retire()),int>,
        "VP retirement admission ABI");
    return graph.withTeardownReservation([&](auto releaseCpu) noexcept -> int {
        return lines.withReleaseReservation([&](auto releaseLines) noexcept -> int {
            const int status=retire();
            if(status) return status;
            releaseLines();releaseCpu();return 0;
        });
    });
}

// Owned lifecycle entries use a flat owned this, never donor adjustment
// thunks. The context itself must outlive releaseAllocation. Its caller owns
// the allocation and parent link/accounting; these callbacks do not authorize
// native parent12e190 to decrement its counter for an owned allocation.
template<class Graph,class Lines,class Retire,class ReleaseAllocation>
class TglOwnedVpLifetimeContext {
    Graph& graph_;Lines& lines_;Retire& retire_;ReleaseAllocation& release_;
    void* child_;
    uint64_t generation_=0;
    bool valid_=false,busy_=false,reentered_=false,destroyed_=false,deleted_=false;
    uint64_t attempts_=0;
    int status_=5;
public:
    TglOwnedVpLifetimeContext(void* child,Graph& graph,Lines& lines,Retire& retire,
        ReleaseAllocation& release) noexcept
        :graph_(graph),lines_(lines),retire_(retire),release_(release),child_(child),
         generation_(graph.generation()),valid_(child && graph.ownsStorage(child)) {}
    TglOwnedVpLifetimeContext(const TglOwnedVpLifetimeContext&)=delete;
    TglOwnedVpLifetimeContext& operator=(const TglOwnedVpLifetimeContext&)=delete;
    bool owns(void* child) const noexcept {
        return valid_ && child && child==child_ && !deleted_ && graph_.generation()==generation_;
    }
    void destroy(bool deleting) noexcept {
        static_assert(noexcept(release_(child_)) &&
            std::is_same_v<decltype(release_(child_)),void>,"owned VP allocation release ABI");
        if(attempts_==UINT64_MAX) {status_=5;return;}
        ++attempts_;
        if(busy_) {reentered_=true;status_=5;return;}
        if(!valid_ || deleted_ || graph_.generation()!=generation_ ||
           (destroyed_ && !deleting)) {status_=5;return;}
        busy_=true;
        struct Unlock {bool& busy;~Unlock(){busy=false;}} unlock{busy_};
        reentered_=false;
        if(!destroyed_) {
            status_=tglRetireOwnedVpResources(graph_,lines_,[&]() noexcept -> int {
                static_assert(noexcept(retire_()) && std::is_same_v<decltype(retire_()),int>,
                    "owned VP lifecycle retirement ABI");
                const int result=retire_();return reentered_?5:result;
            });
            if(status_) return;
            destroyed_=true;
            generation_=graph_.generation();
        }
        if(deleting) {deleted_=true;release_(child_);}
        status_=reentered_?5:0;
    }
    int status() const noexcept {return status_;}
    uint64_t attempts() const noexcept {return attempts_;}
    bool destroyed() const noexcept {return destroyed_;}
    bool deleted() const noexcept {return deleted_;}
};
template<class Resolve> struct TglOwnedVpLifetimeCallbacks {
    static void destroy(void* child) noexcept {
        static_assert(noexcept(Resolve{}(child)),"owned VP lifecycle resolver ABI");
        auto* context=Resolve{}(child);
        if(context && context->owns(child)) context->destroy(false);
    }
    static void deletingDestroy(void* child) noexcept {
        auto* context=Resolve{}(child);
        if(context && context->owns(child)) context->destroy(true);
    }
};

// The lifetime owner outlives each phase catalogue. In particular initialization
// may advance the graph generation: never silently refresh an old destructor
// context to make its entries usable again.
template<class Lifetime,class Use>
int tglWithOwnedVpLifetimeAssemblyEntries(void* child,Lifetime& lifetime,
    const TglOwnedVpEntrySet& base,Use use) noexcept {
    if(!child || !lifetime.owns(child) || base.destroy || base.deletingDestroy) return 5;
    const auto attempts=lifetime.attempts();
    if(attempts==UINT64_MAX) return 5;
    TglOwnedVpSfcScopedContext<Lifetime> scope(child,lifetime);
    if(!scope.bound()) return 5;
    auto entries=base;
    using Resolve=TglOwnedVpSfcResolve<Lifetime>;
    if(!tglBindOwnedVpLifetimeEntries<Resolve>(child,entries)) return 5;
    static_assert(noexcept(use(child,entries)),"VP lifecycle assembly consumer ABI");
    const int result=use(child,entries);
    if(result) return result;
    return lifetime.attempts()!=attempts ? lifetime.status():0;
}

// CPU construction-to-lifetime transition. Construct must invoke the owned
// initialization entry exactly once. Only after its lease ends do we capture
// the new generation for destructor ownership. No GPU retirement is inferred.
template<class Graph,class Lines,class Retire,class Release,class Construct,class Use>
int tglWithOwnedVpInitializedLifetimeAssembly(Graph& graph,Lines& lines,Retire& retire,
    Release& release,const TglOwnedVpEntrySet& base,Construct construct,Use use) noexcept {
    if(base.initialize || base.destroy || base.deletingDestroy) return 5;
    const auto retainedBase=base;
    void* child=nullptr;
    const int initialized=graph.withInitializationAssemblyEntries(retainedBase,
        [&](void* admitted,const auto& entries) noexcept {
            static_assert(noexcept(construct(admitted,entries)),"VP lifecycle construction ABI");
            child=admitted;return construct(admitted,entries);
        });
    if(initialized) return initialized;
    TglOwnedVpLifetimeContext<Graph,Lines,Retire,Release> lifetime(child,graph,lines,retire,release);
    return tglWithOwnedVpLifetimeAssemblyEntries(child,lifetime,retainedBase,use);
}

// Owned VP+30 void callback context. Retirement is an external, fallible
// admission operation, not a bool invented from FinishFenceEvent's return.
// A future runtime owner must qualify that operation against its actual
// submission/token. This CPU context alone is not GPU completion evidence.
template<class Lines,class Retire> class TglOwnedVpLineReleaseContext {
    TglOwnedVpSfcCallerStorage& storage_;
    Lines& lines_;
    Retire& retire_;
    bool attempted_=false,reentered_=false;
    int status_=5;
public:
    TglOwnedVpLineReleaseContext(TglOwnedVpSfcCallerStorage& storage,
        Lines& lines,Retire& retire) noexcept
        :storage_(storage),lines_(lines),retire_(retire) {}
    TglOwnedVpLineReleaseContext(const TglOwnedVpLineReleaseContext&)=delete;
    TglOwnedVpLineReleaseContext& operator=(const TglOwnedVpLineReleaseContext&)=delete;
    bool owns(void* child) const noexcept {return child==storage_.bytes.data();}
    void freeResources() noexcept {
        static_assert(noexcept(retire_()) && std::is_same_v<decltype(retire_()),int>,
            "VP retirement admission status ABI");
        if(attempted_) {reentered_=true;status_=5;return;}
        attempted_=true;
        // Reserve before admission, not after: a line lease must prevent both
        // the retirement side effect and release. Keep the reservation while
        // native retirement/release callbacks can reenter this void entry.
        status_=lines_.withReleaseReservation([&](auto release) noexcept -> int {
            const int admission=retire_();
            if(reentered_) return 5;
            if(admission) return admission;
            release();
            // Release may already have happened; this is rejection, not rollback.
            return reentered_ ? 5 : 0;
        });
    }
    int status() const noexcept {return status_;}
    bool attempted() const noexcept {return attempted_;}
};

// Assemble the real VP58/VP30 entries on one authenticated resource owner.
// VP30 remains void; propagate its separate retirement failure to the scoped
// consumer instead of silently accepting a failed native-shaped cleanup.
// This does not publish a complete vtable or qualify GPU retirement itself.
template<class Lines,class State,class Retire> class TglOwnedVpResourceContext {
    TglOwnedVpLineAllocationContext<Lines,State> allocation_;
    TglOwnedVpLineReleaseContext<Lines,Retire> release_;
public:
    TglOwnedVpResourceContext(TglOwnedVpSfcCallerStorage& vp,Lines& lines,State& state,
        Retire& retire) noexcept:allocation_(vp,lines,state),release_(vp,lines,retire){}
    bool owns(void* child) const noexcept {return allocation_.owns(child);}
    int allocate() noexcept {return allocation_.allocate();}
    void freeResources() noexcept {release_.freeResources();}
    int releaseResult() const noexcept {return release_.attempted()?release_.status():0;}
};
template<class Lines,class State,class Retire,class Use>
int tglWithOwnedVpResourceEntries(TglOwnedVpSfcCallerStorage& vp,Lines& lines,State& state,
    Retire& retire,Use use) noexcept {
    if(!lines.boundToVp(vp)) return 5;
    using Context=TglOwnedVpResourceContext<Lines,State,Retire>;
    using Resolve=TglOwnedVpSfcResolve<Context>;
    using Allocate=TglOwnedVpLineAllocationCallback<Resolve>;
    using Release=TglOwnedVpSfcCallbacks<Resolve>;
    Context context(vp,lines,state,retire);void* child=vp.bytes.data();
    TglOwnedVpSfcScopedContext<Context> scope(child,context);
    if(!scope.bound()) return 5;
    static_assert(noexcept(use(child,&Allocate::allocate,&Release::freeResources)),
        "VP resource-entry consumer ABI");
    const int result=use(child,&Allocate::allocate,&Release::freeResources);
    return result?result:context.releaseResult();
}

// One owned child, with both admission and allocation/free entries alive only
// during the consumer. This is not a published vtable or GPU retirement proof.
template<class Read,class Lines,class State,class Retire,class Use>
int tglWithOwnedVpAssemblyEntries(TglOwnedVpSfcCallerStorage& vp,Read& read,
    Lines& lines,State& state,Retire& retire,Use use) noexcept {
    return tglWithOwnedVpAdmissionEntries(vp,read,
        [&](void* admitted,const TglOwnedVpAdmissionEntries& admission) noexcept {
            return tglWithOwnedVpResourceEntries(vp,lines,state,retire,
                [&](void* child,auto allocate,auto release) noexcept {
                    if(child!=admitted) return 5;
                    static_assert(noexcept(use(child,admission,allocate,release)),
                        "VP assembly-entry consumer ABI");
                    return use(child,admission,allocate,release);
                });
        });
}

// Exact ICL donor1e052d..1e0566 callback ABI. Its target57ba0 is a three-arg
// constant-zero stub, NOT a TGL surface-info entry or metadata proof. A TGL
// adapter must separately authenticate the platform callback and output data.
struct TglOwnedSfcOutputRequest {uint32_t channel=0,reserved=0,outputTag=0;};
static_assert(sizeof(TglOwnedSfcOutputRequest)==12);
template<class Invoke>
int tglInvokeOwnedSfcOutputCallback(void* os,uint32_t channel,
    TglOwnedSurfaceStorage& output,Invoke invoke) noexcept {
    static_assert(noexcept(invoke(os,std::declval<const TglOwnedSfcOutputRequest&>(),output)),
                  "platform output callback status ABI");
    if(!os || !tglNativeBackingPresent(output.resource)) return 5;
    TglOwnedSfcOutputRequest request;request.channel=channel;
    std::memcpy(&request.outputTag,output.prefix.data()+0x140,4);
    return invoke(os,static_cast<const TglOwnedSfcOutputRequest&>(request),output);
}
// Darwin 1e04de..1e05de tail. Borrowed resources outlive the submitted packet.
// Query is a separately qualified platform callback closure, including OS and
// channel/request ABI above. Success does not certify initialized metadata.
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
    // A filter consumer may still be reading state and borrowed coefficients.
    // Reject before Query can mutate output metadata or state can be replaced.
    if(parameters.filtersInUse) return 5;
    uint32_t inputHeight,scaledHeight;
    // +50 writes height at30; +90 writes scaled height at4c. Widths34/50
    // are not allocation dimensions for the qualified 1df245/1df2c1 policy.
    std::memcpy(&inputHeight,prefix.data()+0x30,4);
    std::memcpy(&scaledHeight,prefix.data()+0x4c,4);
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
    std::memcpy(&inputHeight,prefix.data()+0x30,4);
    std::memcpy(&scaledHeight,prefix.data()+0x4c,4);
    return lines.withBuffers(inputHeight,scaledHeight,[&]() noexcept {
        const int status=tglPrepareOwnedSfcCommandState(parameters,state,lines,output,
            prefix,tail,rgbAdaptive,eightTapChroma,compressionEnabled,compressionMode,query);
        if(status) return status;
        return state.withParameters([&](const uint8_t*) noexcept {
            return generate(parameters);
        });
    });
}

// Full VP+58 surfaces feed the same authenticated descriptor fields, without
// copying tokens into the legacy descriptor-only owner. Both shells stay
// pinned through query, state preparation and synchronous command emission.
// Gen12 SFD is a separate optional owner, pinned around the same consumer.
// A caller-supplied raw SFD pointer is never admitted.
template<class CpuBackend,class ResourceBackend,class Query,class Generate,
    class SfdBackend=ResourceBackend>
int tglWithOwnedVpSurfaceCommands(TglOwnedSfcCommandParameters& parameters,
    TglOwnedSfcStateParameters<CpuBackend>& state,
    TglOwnedVpLineSurfaces<ResourceBackend>& lines,TglOwnedSurfaceStorage& output,
    const std::array<uint8_t,0xb8>& prefix,TglOwnedSfcGen12Tail tail,
    bool rgbAdaptive,bool eightTapChroma,bool compressionEnabled,
    TglOwnedSfcCompressionMode compressionMode,Query query,Generate generate,
    TglOwnedSfcSfdBuffer<SfdBackend>* sfd=nullptr) noexcept {
    static_assert(noexcept(generate(parameters)),"VP surface command consumer ABI");
    if(parameters.filtersInUse || !state.data() || tail.sfdLineBuffer) return 5;
    uint32_t input=0,scaled=0;
    std::memcpy(&input,prefix.data()+0x30,4);std::memcpy(&scaled,prefix.data()+0x4c,4);
    TglOwnedSfcLineBufferSizes sizes;
    if(!tglOwnedSfcLineBufferSizes(sizes,input,scaled) || (sizes.sfd && !sfd)) return 5;
    auto consume=[&]() noexcept {
      return lines.withDimensions(input,scaled,[&](const auto& avs,const auto& ief) noexcept {
        TglOwnedSfcStateParametersPacket checked{};
        if(!tglPrepareOwnedSfcGen12Pass(checked,prefix,tail,rgbAdaptive,eightTapChroma,
            compressionEnabled,compressionMode)) return 5;
        auto bound=prefix;
        const int status=tglBindOwnedSfcResources(bound,output,avs.resource,ief.resource,query);
        if(status) return status;
        if(!state.prepare(bound,tail,rgbAdaptive,eightTapChroma,compressionEnabled,compressionMode)) return 5;
        std::memcpy(parameters.state.data(),state.data(),parameters.state.size());
        return state.withParameters([&](const uint8_t*) noexcept {return generate(parameters);});
      });
    };
    if(!sfd) return consume();
    return sfd->withBuffer(scaled,[&](const auto* descriptor) noexcept {
        tail.sfdLineBuffer=descriptor ? reinterpret_cast<uintptr_t>(descriptor->data()) : 0;
        return consume();
    });
}

// VP90 orders geometry -> VP58 allocation -> resource binding -> final state.
// Only the two proven allocation dimensions are temporarily published; restore
// the packet before final validation/binding. Allocation failure is not send.
template<class CpuBackend,class ResourceBackend,class SfdBackend,class Allocate,class Query,class Generate>
int tglWithAllocatedOwnedVpSurfaceCommands(TglOwnedVpSfcCallerStorage& vp,
    TglOwnedSfcCommandParameters& parameters,TglOwnedSfcStateParameters<CpuBackend>& state,
    TglOwnedVpLineSurfaces<ResourceBackend>& lines,TglOwnedSurfaceStorage& output,
    const std::array<uint8_t,0xb8>& prefix,TglOwnedSfcGen12Tail tail,
    bool rgbAdaptive,bool eightTapChroma,bool compressionEnabled,
    TglOwnedSfcCompressionMode compressionMode,Allocate allocate,Query query,Generate generate,
    TglOwnedSfcSfdBuffer<SfdBackend>* sfd=nullptr) noexcept {
    static_assert(noexcept(allocate(vp.bytes.data())),"VP58 allocation status ABI");
    auto* packet=state.data();
    if(!packet || parameters.filtersInUse || tail.sfdLineBuffer || !lines.boundToVp(vp)) return 5;
    uint32_t input=0,scaled=0;
    std::memcpy(&input,prefix.data()+0x30,4);std::memcpy(&scaled,prefix.data()+0x4c,4);
    TglOwnedSfcLineBufferSizes sizes;
    if(!tglOwnedSfcLineBufferSizes(sizes,input,scaled) || (sizes.sfd && !sfd)) return 5;
    TglOwnedSfcStateParametersPacket saved{};std::memcpy(saved.data(),packet,saved.size());
    std::memcpy(packet+0x30,&input,4);std::memcpy(packet+0x4c,&scaled,4);
    const int status=state.withVpParameterPublication(vp,[&](const uint8_t*) noexcept {
        return allocate(vp.bytes.data());
    });
    std::memcpy(packet,saved.data(),saved.size());
    if(status) return status;
    return tglWithOwnedVpSurfaceCommands(parameters,state,lines,output,prefix,tail,
        rgbAdaptive,eightTapChroma,compressionEnabled,compressionMode,query,generate,sfd);
}

struct TglOwnedSfcAvsConfiguration {
    uint32_t format=0xffffffffu,siting=0;
    float scaleX=0.f,scaleY=0.f,sharpening=0.f,window=0.f;
    bool forceFilter=false,symmetric=false;
};
// Qualified VP 1e088e..1e0940. AVS consumes effective +99c, X/Y +928/+92c,
// and the siting AFTER the state producer's IECP adjustment. Optional +9c8
// contributes float+0c sharpening and UINT32+8 window (not a float load).
// No native object/table is published by this leased CPU snapshot.
template<class Read>
bool tglReadOwnedVpAvsConfiguration(const TglOwnedVpFramePreparation& frame,
    const TglOwnedVpChromaStatePrefix& state,uintptr_t performance,Read read,
    TglOwnedSfcAvsConfiguration& output) noexcept {
    static_assert(noexcept(read(uintptr_t{},nullptr,size_t{})),"VP AVS leased read ABI");
    TglOwnedSfcAvsConfiguration candidate;
    candidate.format=frame.color.modes.state.at99c;
    candidate.scaleX=frame.regions.scale.at928;candidate.scaleY=frame.regions.scale.at92c;
    candidate.siting=state.sourceSiting ? state.sourceSiting : 0x11;
    candidate.forceFilter=frame.color.at9b8;candidate.symmetric=true;
    if(!std::isfinite(candidate.scaleX) || !std::isfinite(candidate.scaleY) ||
       candidate.scaleX<=0 || candidate.scaleY<=0) return false;
    if(performance) {
        uint32_t window=0;
        if(performance>UINTPTR_MAX-16 ||
           !read(performance+12,&candidate.sharpening,4) ||
           !read(performance+8,&window,4)) return false;
        candidate.window=float(window);
        if(!std::isfinite(candidate.sharpening)) return false;
    }
    output=candidate;return true;
}
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
    TglOwnedSfcCompressionMode compressionMode,bool avsEnabled,const TglOwnedSfcAvsConfiguration& config,
    NativeSin sine,Query query,Generate generate) noexcept {
    // VP +40 only produces AVS when scaling or forced chroma filtering is
    // active. A bypass pass must not require an AVS allocation or call sine.
    if(!avsEnabled)
        return tglWithOwnedSfcPreparedCommands(parameters,state,lines,output,prefix,
            tail,rgbAdaptive,eightTapChroma,compressionEnabled,compressionMode,query,generate);
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
    using Storage=typename TglOwnedResource<Backend>::Storage;
    explicit TglStatisticsResource(Backend &backend) noexcept : backend_(backend),owner_(backend) {}
    // Bind the resource bytes in an empty native statistics shell. The shell
    // outlives this owner and is freed only after reset/native-token release.
    TglStatisticsResource(Backend& backend,Storage& storage) noexcept :backend_(backend),owner_(backend,storage) {}
    TglStatisticsResource(const TglStatisticsResource&)=delete;
    TglStatisticsResource& operator=(const TglStatisticsResource&)=delete;
    ~TglStatisticsResource() {reset();}
    template<class Visit> void visitStorageBindings(Visit visit) const noexcept {
        visit(static_cast<const void*>(owner_.storage().data()));
    }
    template<class Fill>
    int ensure(uint32_t width, uint32_t height, Fill fill) noexcept {
        return ensure(width,height,fill,[](const Storage&,const TglStatisticsAllocation&) noexcept {});
    }
    // Production initialization path: use the allocator's OS and exact
    // native Fill; no CPU-only memset substitute and no const-cast. The
    // caller holds OS/image lifetime and excludes submitted GPU users.
    bool nativeFillCompatible(const TglNativeBufferFillBinding& binding) const noexcept {
        static_assert(std::is_same_v<Backend,TglNativeResourceBackend>,
                      "native statistics require the native resource backend");
        return backend_.nativeFillCompatible(binding);
    }
    template<class Writable,class Publish>
    int ensureNative(uint32_t width,uint32_t height,
                     const TglNativeBufferFillBinding& binding,
                     Writable writable,Publish publish) noexcept {
        static_assert(std::is_same_v<Backend,TglNativeResourceBackend>,
                      "native statistics require the native resource backend");
        if (!nativeFillCompatible(binding)) return 5;
        return ensure(width,height,[&](Storage& storage,uint32_t bytes) noexcept {
            return backend_.fillNative(storage,bytes,0,binding,writable);
        },publish);
    }
    template<class Fill,class Publish>
    int ensure(uint32_t width,uint32_t height,Fill fill,Publish publish) noexcept {
        static_assert(noexcept(fill(std::declval<Storage&>(), uint32_t{})),
                      "initialization must report status without throwing");
        using PublicationResult=decltype(publish(owner_.storage(),std::declval<const TglStatisticsAllocation&>()));
        static_assert(noexcept(publish(owner_.storage(),std::declval<const TglStatisticsAllocation&>())) &&
            (std::is_same_v<PublicationResult,void> || std::is_same_v<PublicationResult,int>),
            "statistics changed-layout publication ABI");
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
            const int status = owner_.withMutableStorage([&](auto& storage) noexcept {
                const int filled=fill(storage,candidate.bytes);
                if(!filled) {
                    if constexpr(std::is_same_v<PublicationResult,int>)
                        return publish(static_cast<const Storage&>(storage),
                            static_cast<const TglStatisticsAllocation&>(candidate));
                    else publish(static_cast<const Storage&>(storage),
                        static_cast<const TglStatisticsAllocation&>(candidate));
                }
                return filled;
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
    template<class Use> int withReleaseReservation(Use use) noexcept {
        if(busy_) return 5;
        busy_=true;bool released=false;
        auto release=[&]() noexcept {
            if(released) return;
            released=true;retire();
        };
        static_assert(noexcept(use(release)),"release reservation must not throw");
        const int result=use(release);
        busy_=false;return result;
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
    Backend& backend_;
    TglOwnedResource<Backend> owner_;
    TglStatisticsAllocation layout_{};
    bool busy_=false;
};

// Allocation transaction for a history-enabled frame. The qualified platform
// planner supplies resource keys and statistics boundary dimensions; this does
// not infer feature admission or SFC resources. All three owners outlive this
// coordinator, and GPU users must be excluded before ensure/reset/destruction.
// In particular, a +58 error cannot leave earlier groups alive waiting for a
// FreeResources call that TGL's mode0 caller never makes.
// Donor ICL1f875f explicitly invokes its virtual18 on allocation failure.
// This owned transaction supplies equivalent unwind responsibility, not an
// invocation of ICL virtual18 on the unrelated TGL child vtable.
// Do NOT publish reset() directly as runtime FreeResources: donor1f87b0
// releases STMM[0/1] (1f8823), statistics (1f8858), then conditionally
// VP+30 (1f88a7). It does NOT release the FFDN group here. This transaction's
// all-owner unwind is for allocation failure/complete teardown, not that
// selective reuse boundary; runtime retirement admission remains separate.
template<class Group,class History,class Statistics> class TglOwnedVeboxBackingTransaction {
    Group& group_;
    History& history_;
    Statistics& statistics_;
    bool busy_=false,ready_=false,bindingsValid_=false;
    bool distinctBindings() const noexcept {
        // Bound graph admission, not a handle registry. Keep identities only
        // on this stack; do not inspect, copy or adopt descriptor contents.
        std::array<const void*,32> bindings{};size_t count=0;bool valid=true;
        const auto visit=[&](const void* identity) noexcept {
            if(!identity || count==bindings.size()) {valid=false;return;}
            const uintptr_t begin=reinterpret_cast<uintptr_t>(identity);
            if(begin>UINTPTR_MAX-0x148) {valid=false;return;}
            for(size_t i=0;i<count;++i) {
                const uintptr_t other=reinterpret_cast<uintptr_t>(bindings[i]);
                if(begin<other+0x148 && other<begin+0x148) valid=false;
            }
            bindings[count++]=identity;
        };
        group_.visitStorageBindings(visit);history_.visitStorageBindings(visit);
        statistics_.visitStorageBindings(visit);return valid;
    }
    void retire() noexcept {
        ready_=false;
        statistics_.reset();history_.reset();group_.reset();
    }
    template<class Requests,class Prepare,class InitializeStatistics>
    int ensureWithStatistics(const Requests& requests,Prepare prepare,
        const std::array<TglResourceKey,2>& historyKeys,bool reinitialize,
        InitializeStatistics initialize) noexcept {
        static_assert(noexcept(initialize()),"statistics initialization status ABI");
        if(busy_ || !bindingsValid_) return 5;
        busy_=true;ready_=false;
        int status=group_.ensure(requests,prepare);
        if(!status) status=group_.withStorage([&](const auto&) noexcept {
            const int allocated=history_.ensure(historyKeys,reinitialize);
            if(allocated) return allocated;
            return history_.withStorage([&](const auto&,const auto&) noexcept {
                return initialize();
            });
        });
        if(status) retire();else ready_=true;
        busy_=false;return status;
    }
public:
    TglOwnedVeboxBackingTransaction(Group& group,History& history,Statistics& statistics) noexcept
        :group_(group),history_(history),statistics_(statistics),bindingsValid_(distinctBindings()) {}
    TglOwnedVeboxBackingTransaction(const TglOwnedVeboxBackingTransaction&)=delete;
    TglOwnedVeboxBackingTransaction& operator=(const TglOwnedVeboxBackingTransaction&)=delete;
    ~TglOwnedVeboxBackingTransaction(){reset();}
    // Caller holds the actual source owner's synchronous surface lease and
    // excludes GPU users. Statistics dimensions are independently calculated;
    // STMM dimensions alone do not establish the statistics allocation shape.
    template<class SourceOwner,class Requests,class Prepare,class Fill,class Publish=TglOwnedStatisticsNoPublication>
    int ensureFromLeasedSource(SourceOwner& owner,const TglOwnedSurfaceStorage& source,
        bool child1ba9,const Requests& requests,Prepare prepare,bool reinitialize,
        uint32_t statisticsWidth,uint32_t statisticsHeight,Fill fill,Publish publish={}) noexcept {
        if(!owner.hasSurfaceLease(source)) return 5;
        std::array<TglResourceKey,2> keys{};
        if(!tglPrepareOwnedStmmHistoryKeys(source,child1ba9,keys)) return 5;
        const int status=ensure(requests,prepare,keys,reinitialize,
                                statisticsWidth,statisticsHeight,fill,publish);
        // Producers may reject or revoke a captured identity. Do not leave
        // newly allocated graph resources ready after losing that source.
        // This synchronous allocation API excludes GPU users.
        if(!owner.hasSurfaceLease(source)) {reset();return 5;}
        return status;
    }
    template<class Requests,class Prepare,class Fill,class Publish=TglOwnedStatisticsNoPublication>
    int ensure(const Requests& requests,Prepare prepare,
        const std::array<TglResourceKey,2>& historyKeys,bool reinitialize,
        uint32_t width,uint32_t height,Fill fill,Publish publish={}) noexcept {
        return ensureWithStatistics(requests,prepare,historyKeys,reinitialize,[&]() noexcept {
            if constexpr(std::is_same_v<Publish,TglOwnedStatisticsNoPublication>)
                return statistics_.ensure(width,height,fill);
            else return statistics_.ensure(width,height,fill,publish);
        });
    }
    // Same allocation/unwind transaction, with native statistics initialization
    // selected explicitly. Reject a foreign OS before allocating any group.
    template<class Requests,class Prepare,class Writable,class Publish>
    int ensureNative(const Requests& requests,Prepare prepare,
        const std::array<TglResourceKey,2>& historyKeys,bool reinitialize,
        uint32_t width,uint32_t height,const TglNativeBufferFillBinding& binding,
        Writable writable,Publish publish) noexcept {
        if(!statistics_.nativeFillCompatible(binding)) return 5;
        return ensureWithStatistics(requests,prepare,historyKeys,reinitialize,[&]() noexcept {
            return statistics_.ensureNative(width,height,binding,writable,publish);
        });
    }
    template<class SourceOwner,class Requests,class Prepare,class Writable,class Publish>
    int ensureNativeFromLeasedSource(SourceOwner& owner,const TglOwnedSurfaceStorage& source,
        bool child1ba9,const Requests& requests,Prepare prepare,bool reinitialize,
        uint32_t width,uint32_t height,const TglNativeBufferFillBinding& binding,
        Writable writable,Publish publish) noexcept {
        if(!owner.hasSurfaceLease(source)) return 5;
        std::array<TglResourceKey,2> keys{};
        if(!tglPrepareOwnedStmmHistoryKeys(source,child1ba9,keys)) return 5;
        const int status=ensureNative(requests,prepare,keys,reinitialize,
            width,height,binding,writable,publish);
        if(!owner.hasSurfaceLease(source)) {reset();return 5;}
        return status;
    }
    // End-to-end statistics input path under the captured source lease:
    // actual surface metadata -> boundary -> backing transaction -> native
    // zero-fill -> caller's leased child layout publication.
    template<class SourceOwner,class QueryDi,class Requests,class Prepare,class Writable,class Publish>
    int ensureNativeFromSourceBoundary(SourceOwner& owner,const TglOwnedSurfaceStorage& source,
        uint8_t execution13,QueryDi queryDi,bool alignDi64,bool child1ba9,
        const Requests& requests,Prepare prepare,bool reinitialize,
        const TglNativeBufferFillBinding& binding,Writable writable,Publish publish) noexcept {
        uint32_t width=0,height=0;
        return tglPrepareOwnedStatisticsFromLeasedSource(owner,source,execution13,
            queryDi,alignDi64,width,height,[&](uint32_t w,uint32_t h) noexcept {
                return ensureNativeFromLeasedSource(owner,source,child1ba9,requests,
                    prepare,reinitialize,w,h,binding,writable,publish);
            });
    }
    // Slot58/frame integration: capture actual child/report/source identities
    // before consuming metadata. The private table preserves d0=71e80 FALSE,
    // so DI comes from the exact leased report13, not an injected policy stub.
    template<class SourceOwner,class ChildOwner,class ExecutionOwner,class Read,
        class Requests,class Prepare,class Writable,class Publish>
    int ensureNativeFromCapturedFrame(SourceOwner& sourceOwner,ChildOwner& childOwner,
        ExecutionOwner& executionOwner,void* child,uintptr_t execution,
        const TglOwnedSurfaceStorage& source,Read read,bool alignDi64,
        const Requests& requests,Prepare prepare,bool reinitialize,
        const TglNativeBufferFillBinding& binding,Writable writable,Publish publish) noexcept {
        using Storage=typename Statistics::Storage;
        using Publication=decltype(publish(std::declval<const Storage&>(),
            std::declval<const TglStatisticsAllocation&>()));
        static_assert(noexcept(read(uintptr_t{},static_cast<void*>(nullptr),size_t{})) &&
            noexcept(prepare(size_t{},std::declval<const Storage&>(),bool{})) &&
            noexcept(writable(uintptr_t{},uint32_t{})) &&
            noexcept(publish(std::declval<const Storage&>(),
                std::declval<const TglStatisticsAllocation&>())) &&
            (std::is_same_v<Publication,int> || std::is_same_v<Publication,void>),
            "captured allocation callbacks must preserve their nonthrowing ABI");
        bool entered=false;
        const int status=tglRunCapturedVeboxAllocation(sourceOwner,childOwner,executionOwner,
            child,execution,source,[&](void* captured,uintptr_t report,const auto& surface) noexcept {
                entered=true;
                auto frameStage=[&](auto invoke) noexcept {
                    return tglRunCapturedVeboxAllocation(sourceOwner,childOwner,executionOwner,
                        captured,report,surface,[&](void*,uintptr_t,const auto&) noexcept {
                            return invoke();
                        });
                };
                bool di=false;uint8_t historyCompression=0;
                const uintptr_t address=reinterpret_cast<uintptr_t>(captured);
                if(address>UINTPTR_MAX-0x1baa ||
                   !tglReadOwnedBuilderDiArgument(di,address,report,read) ||
                   !read(address+0x1ba9,&historyCompression,1) || historyCompression>1) return 5;
                if(frameStage([]() noexcept {return 0;})) return 5;
                auto guardedPrepare=[&](size_t index,const auto& storage,bool changed) noexcept {
                    return frameStage([&]() noexcept {return prepare(index,storage,changed);});
                };
                auto guardedWritable=[&](uintptr_t mapping,uint32_t bytes) noexcept {
                    bool allowed=false;
                    const int checked=frameStage([&]() noexcept {
                        allowed=writable(mapping,bytes);return allowed?0:5;
                    });
                    return !checked && allowed;
                };
                auto guardedPublish=[&](const auto& storage,const auto& layout) noexcept -> int {
                    return frameStage([&]() noexcept -> int {
                        if constexpr(std::is_same_v<decltype(publish(storage,layout)),void>) {
                            publish(storage,layout);return 0;
                        } else return publish(storage,layout);
                    });
                };
                return ensureNativeFromSourceBoundary(sourceOwner,surface,di?1u:0u,
                    []() noexcept {return uint8_t{0};},alignDi64,historyCompression!=0,
                    requests,guardedPrepare,reinitialize,binding,guardedWritable,guardedPublish);
            });
        if(entered && status) reset();
        return status;
    }
    template<class Invoke> int withResources(Invoke invoke) noexcept {
        if(busy_ || !ready_) return 5;
        busy_=true;
        const int result=statistics_.withResource([&](const auto& stats,const auto& layout) noexcept {
            return history_.withStorage([&](const auto& first,const auto& second) noexcept {
                return group_.withStorage([&](const auto& views) noexcept {
                    static_assert(noexcept(invoke(views,first,second,stats,layout)),"backing graph lease ABI");
                    return invoke(views,first,second,stats,layout);
                });
            });
        });
        busy_=false;return result;
    }
    void reset() noexcept {
        if(busy_ || !bindingsValid_) return;
        busy_=true;retire();busy_=false;
    }
    // Selective native mode cleanup, not full teardown: retain the FFDN group.
    // Reserve every selected owner BEFORE invoking external retirement admission
    // or releasing anything. Caller supplies actual submission/discard evidence;
    // raw FinishFenceEvent success alone is not valid retirement admission.
    template<class Lines,class Retire>
    int releaseModeResources(Lines& lines,bool releaseSfc,Retire retire) noexcept {
        static_assert(noexcept(retire()) && std::is_same_v<decltype(retire()),int>,
                      "mode retirement admission status ABI");
        if(busy_ || !bindingsValid_) return 5;
        busy_=true;
        const int result=group_.withStorage([&](const auto&) noexcept {
          return history_.withReleaseReservation([&](auto releaseHistory) noexcept {
            return statistics_.withReleaseReservation([&](auto releaseStatistics) noexcept {
                auto release=[&](auto releaseLines) noexcept {
                    const int admission=retire();
                    if(admission) return admission;
                    ready_=false;
                    releaseHistory();releaseStatistics();releaseLines();
                    return 0;
                };
                if(releaseSfc) return lines.withReleaseReservation(release);
                return release([]() noexcept {});
            });
          });
        });
        busy_=false;return result;
    }
    // Frame admission includes statistics and every group resource before
    // publishing either history pitch. All owners remain pinned through the
    // builder; this is synchronous admission, not asynchronous GPU retirement.
    template<class Invoke> int withSurfaces(TglOwnedSurfaceStorage& first,
        TglOwnedSurfaceStorage& second,Invoke invoke) noexcept {
        if(busy_ || !ready_) return 5;
        busy_=true;
        const int result=statistics_.withResource([&](const auto& stats,const auto& layout) noexcept {
            return group_.withStorage([&](const auto& views) noexcept {
                return history_.withSurfaces(first,second,[&](const auto& a,const auto& b) noexcept {
                    static_assert(noexcept(invoke(views,a,b,stats,layout)),"surface graph lease ABI");
                    return invoke(views,a,b,stats,layout);
                });
            });
        });
        busy_=false;return result;
    }
};

// TGL126d28..126d35 invokes +60 as void(this), without consuming a result.
// Preserve that ABI and retain admission status in the enclosing owned scope.
template<class Transaction,class Lines,class Retire> class TglOwnedVeboxModeReleaseContext {
    void* child_;
    Transaction& transaction_;
    Lines& lines_;
    Retire& retire_;
    bool releaseSfc_,attempted_=false,reentered_=false;
    int status_=5;
public:
    TglOwnedVeboxModeReleaseContext(void* child,Transaction& transaction,Lines& lines,
        bool releaseSfc,Retire& retire) noexcept
        :child_(child),transaction_(transaction),lines_(lines),retire_(retire),releaseSfc_(releaseSfc){}
    TglOwnedVeboxModeReleaseContext(const TglOwnedVeboxModeReleaseContext&)=delete;
    TglOwnedVeboxModeReleaseContext& operator=(const TglOwnedVeboxModeReleaseContext&)=delete;
    bool owns(void* child) const noexcept {return child && child==child_;}
    void freeResources() noexcept {
        static_assert(noexcept(retire_()) && std::is_same_v<decltype(retire_()),int>,
                      "mode retirement evidence must report status without throwing");
        if(attempted_) {reentered_=true;status_=5;return;}
        attempted_=true;
        status_=transaction_.releaseModeResources(lines_,releaseSfc_,[&]() noexcept {
            const int admission=retire_();
            return reentered_ ? 5 : admission;
        });
        if(reentered_) status_=5;
    }
    int status() const noexcept {return status_;}
};
template<class Resolve> struct TglOwnedVeboxModeReleaseCallbacks {
    static void freeResources(void* child) noexcept {
        static_assert(noexcept(Resolve::get(child)),"mode cleanup scope resolution ABI");
        auto* context=Resolve::get(child);
        if(context && context->owns(child)) context->freeResources();
    }
};
template<class Resolve>
std::array<uintptr_t,20> tglBindOwnedVeboxModeReleaseHook(std::array<uintptr_t,20> hooks) noexcept {
    static_assert(TglOwnedVeboxVtable::required[4]==0x60,"exact TGL mode cleanup slot");
    hooks[4]=reinterpret_cast<uintptr_t>(&TglOwnedVeboxModeReleaseCallbacks<Resolve>::freeResources);
    return hooks;
}

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
