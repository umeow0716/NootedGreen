#pragma once
#include "native_avc_reservation.hpp"
#include "../../NootedGreen/kern_checksum_gen12_commands.hpp"

// Only exact FrameChecksum2190a. Size refusal precedes native22708.
// Builder errors skip submit but leave22708's producer mutex held; the actual
// builder bridge must fail-stop instead of returning into ordinary cleanup.
// AVC27724 ignores callback EAX and must not use this API.
namespace NgreenNativeChecksumReservation {
// Captured data and one typed CF pin, not a surface-content lock, native
// context lease, backing coherency proof or permission to submit. RootAdmit
// must independently exclude source destruction/writes throughout capture
// and native callback execution. It must not recursively call this object.
struct InputGeometry {
    uint32_t contextWidth=0,contextHeight=0,requestWidth=0,requestHeight=0;
    bool operator==(const InputGeometry& other) const noexcept {
        return contextWidth==other.contextWidth && contextHeight==other.contextHeight &&
            requestWidth==other.requestWidth && requestHeight==other.requestHeight;
    }
};
// Immutable IOSurface metadata only. Offsets describe allocation bounds;
// they are NEVER GPU addresses or a DMA/coherency/lifetime authorization.
// Native9acb retains responsibility for each plane's resource+100 GPU value.
struct InputSurfaceShape {
    uint64_t id=0,format=0,planes=0,width=0,height=0,bytes=0;
    std::array<uint64_t,2> planeWidth{},planeHeight{},pitch{},offset{};
    bool operator==(const InputSurfaceShape& other) const noexcept {
        return id==other.id && format==other.format && planes==other.planes &&
            width==other.width && height==other.height && bytes==other.bytes &&
            planeWidth==other.planeWidth && planeHeight==other.planeHeight &&
            pitch==other.pitch && offset==other.offset;
    }
    bool covers(const InputGeometry& geometry) const noexcept {
        // Exact native9458 maps both420v and420f to the same two-plane class.
        if(!id || (format!=0x34323076 && format!=0x34323066) || planes!=2 ||
            !width || !height || width>16384 || height>16384 ||
            (width&1) || (height&1) || !bytes ||
            !geometry.contextWidth || !geometry.contextHeight ||
            !geometry.requestWidth || !geometry.requestHeight ||
            geometry.contextWidth>width || geometry.requestWidth>width ||
            geometry.contextHeight>height || geometry.requestHeight>height ||
            planeWidth[0]!=width || planeWidth[1]!=width/2 ||
            planeHeight[0]!=height || planeHeight[1]!=height/2)return false;
        std::array<uint64_t,2> end{};
        for(size_t i=0;i<2;++i) {
            // Nativeae2e stores pitch-1 in18bits; validate before truncation.
            if(pitch[i]<width || pitch[i]>0x40000 || offset[i]>bytes ||
                planeHeight[i]>(bytes-offset[i])/pitch[i])return false;
            end[i]=offset[i]+planeHeight[i]*pitch[i];
        }
        return end[0]<=offset[1] || end[1]<=offset[0];
    }
};
enum class InputSurfaceShapeStage : uint32_t {
    Ready, Handle, AdmissionBeforeLock, Lock, AdmissionBeforeCapture,
    CaptureBefore, GeometryBefore, AdmissionBetweenCaptures, CaptureAfter,
    GeometryAfter, Stability, AdmissionBeforeUnlock, Unlock, AdmissionAfterUnlock
};
struct InputSurfaceShapeResult {
    bool valid=false,mustStop=false;
    InputSurfaceShapeStage stage=InputSurfaceShapeStage::Ready;
    InputSurfaceShape observed{};
    uint32_t nativeStage=0;
    int32_t lockStatus=0,unlockStatus=0;
};
// The native wrapper and CPU fault tests share this exact lock transaction.
// Every successful lock has exactly one unlock attempt, including refusal.
template<class Admit,class Lock,class Unlock,class Capture>
inline InputSurfaceShapeResult captureInputSurfaceShape(uintptr_t handle,const InputGeometry& geometry,
    Admit admit,Lock lock,Unlock unlock,Capture capture) noexcept {
    static_assert(noexcept(admit()) && noexcept(lock()) && noexcept(unlock()) &&
        noexcept(capture(std::declval<InputSurfaceShape&>())));
    InputSurfaceShapeResult result;
    const auto denied=[&](InputSurfaceShapeStage stage) noexcept {result.stage=stage;return result;};
    if(!handle)return denied(InputSurfaceShapeStage::Handle);
    if(!admit())return denied(InputSurfaceShapeStage::AdmissionBeforeLock);
    if(!lock())return denied(InputSurfaceShapeStage::Lock);
    InputSurfaceShape before,after;
    bool valid=false;
    if(!admit())result.stage=InputSurfaceShapeStage::AdmissionBeforeCapture;
    else if(!capture(before))result.stage=InputSurfaceShapeStage::CaptureBefore;
    else if(!before.covers(geometry))result.stage=InputSurfaceShapeStage::GeometryBefore;
    else if(!admit())result.stage=InputSurfaceShapeStage::AdmissionBetweenCaptures;
    else if(!capture(after))result.stage=InputSurfaceShapeStage::CaptureAfter;
    else if(!after.covers(geometry))result.stage=InputSurfaceShapeStage::GeometryAfter;
    else if(!(before==after))result.stage=InputSurfaceShapeStage::Stability;
    else if(!admit())result.stage=InputSurfaceShapeStage::AdmissionBeforeUnlock;
    else valid=true;
    result.observed=before;
    const bool unlocked=unlock();
    result.mustStop=!unlocked;
    if(!unlocked)result.stage=InputSurfaceShapeStage::Unlock;
    else if(valid && !admit()){valid=false;result.stage=InputSurfaceShapeStage::AdmissionAfterUnlock;}
    result.valid=valid && unlocked;
    return result;
}
#if defined(__APPLE__)
// Caller keeps the typed CF reference and exclusive context authority alive.
// Only metadata/CPU address differences are observed, never contents or GPU
// addresses. AvoidSync refuses a required GPU readback; native work starts
// only AFTER the symmetric read-only unlock has returned successfully.
template<class Admit>
inline InputSurfaceShapeResult darwinInputSurfaceShape(uintptr_t handle,const InputGeometry& geometry,Admit admit) noexcept {
    const auto surface=reinterpret_cast<IOSurfaceRef>(handle);
    constexpr IOSurfaceLockOptions options=kIOSurfaceLockReadOnly|kIOSurfaceLockAvoidSync;
    uint32_t nativeStage=0;int32_t lockStatus=0,unlockStatus=0;
    const auto capture=[&](InputSurfaceShape& shape) noexcept {
        nativeStage=1;
        if(!admit())return false;
        nativeStage=2;
        shape.id=IOSurfaceGetID(surface);shape.format=IOSurfaceGetPixelFormat(surface);
        shape.planes=IOSurfaceGetPlaneCount(surface);
        if(!admit() || shape.planes!=2 ||
            (shape.format!=0x34323076 && shape.format!=0x34323066))return false;
        nativeStage=3;
        shape.width=IOSurfaceGetWidth(surface);shape.height=IOSurfaceGetHeight(surface);
        shape.bytes=IOSurfaceGetAllocSize(surface);
        const auto base=reinterpret_cast<uintptr_t>(IOSurfaceGetBaseAddress(surface));
        nativeStage=4;
        if(!base || shape.bytes>UINTPTR_MAX-base)return false;
        for(size_t i=0;i<2;++i) {
            nativeStage=5+static_cast<uint32_t>(i)*2;
            if(!admit())return false;
            const auto plane=reinterpret_cast<uintptr_t>(IOSurfaceGetBaseAddressOfPlane(surface,i));
            ++nativeStage;
            if(plane<base || plane-base>shape.bytes)return false;
            shape.offset[i]=plane-base;
            shape.planeWidth[i]=IOSurfaceGetWidthOfPlane(surface,i);
            shape.planeHeight[i]=IOSurfaceGetHeightOfPlane(surface,i);
            shape.pitch[i]=IOSurfaceGetBytesPerRowOfPlane(surface,i);
        }
        nativeStage=9;
        return admit() && shape.covers(geometry);
    };
    auto result=captureInputSurfaceShape(handle,geometry,admit,
        [&]() noexcept {lockStatus=IOSurfaceLock(surface,options,nullptr);return lockStatus==KERN_SUCCESS;},
        [&]() noexcept {unlockStatus=IOSurfaceUnlock(surface,options,nullptr);return unlockStatus==KERN_SUCCESS;},capture);
    result.nativeStage=nativeStage;result.lockStatus=lockStatus;result.unlockStatus=unlockStatus;
    return result;
}
#endif
// Fixed native2c314/9710/9628 allocation record. These are CPU and CF/shared
// pointers, NEVER GPU addresses. A stable observation supplies no lifetime
// lease or write authority; the enclosing prepared owner/producer must do so.
struct SurfaceHeapObservation {
    uint32_t mode=0,count=0,bytes=0,ready=0;
    uintptr_t cpu=0,resource=0,sharedRO=0,sharedRW=0;
    template<class Read,class Admit>
    static bool capture(uintptr_t context,Read read,Admit admit,SurfaceHeapObservation& output) noexcept {
        static_assert(sizeof(SurfaceHeapObservation)==48);
        if(!context || (context&7U) || context>UINTPTR_MAX-0x21d8 || !admit())return false;
        uint32_t count=0,index=0;
        SurfaceHeapObservation before,after;
        if(!read(context+0x174,&count,4) || !read(context+0x178,&index,4) ||
            count!=1 || index || !admit() ||
            !read(context+0x21a8,&before,sizeof(before)) || !admit() ||
            !read(context+0x21a8,&after,sizeof(after)) || !admit() ||
            std::memcmp(&before,&after,sizeof(before)) || before.mode || before.count!=1 ||
            before.ready!=1 || before.bytes<0x240 || before.bytes>65536 ||
            !before.cpu || (before.cpu&4095U) || before.bytes>UINTPTR_MAX-before.cpu ||
            !before.resource || !before.sharedRO || !before.sharedRW ||
            ((before.resource|before.sharedRO|before.sharedRW)&7U))return false;
        uint32_t finalCount=0,finalIndex=0;
        if(!read(context+0x174,&finalCount,4) || !read(context+0x178,&finalIndex,4) ||
            finalCount!=count || finalIndex!=index || !admit())return false;
        output=before;return true;
    }
};
// Runs only after original builder setup, under its existing producer lock
// and the prepared exclusive owner. A failure after entry MUST fail-stop;
// do not roll back or flush an ambiguous native stream.
struct SurfaceCacheResult {bool ok=false,mutated=false;const char* reason="ssh";};
template<class Read,class Writable,class Write,class Admit,class Exclude>
inline SurfaceCacheResult normalizeSurfaceCache(uintptr_t context,const SurfaceHeapObservation& expected,
    Read read,Writable writable,Write write,Admit admit,Exclude exclude) noexcept {
    bool mutated=false;
    const auto failed=[&](const char* reason) noexcept {return SurfaceCacheResult{false,mutated,reason};};
    const auto same=[&]() noexcept {
        SurfaceHeapObservation current;
        return admit() && SurfaceHeapObservation::capture(context,read,admit,current) &&
            !std::memcmp(&current,&expected,sizeof(current)) && admit();
    };
    if(!same())return failed("ssh-provenance");
    if(!exclude(expected.cpu,256) || !writable(expected.cpu,256) || !same())return failed("ssh-span");
    std::array<uint32_t,64> original{},again{},wanted{};
    std::array<uint32_t,3> replacement{};
    if(!read(expected.cpu,original.data(),sizeof(original)) || !same() ||
        !read(expected.cpu,again.data(),sizeof(again)) || original!=again || !same())return failed("ssh-snapshot");
    if(!NgreenChecksumGen12::checksumSurfaceLayoutCompatible(original.data(),64))return failed("ssh-layout");
    if(!NgreenChecksumGen12::checksumSurfaceCacheWords(original.data(),64,replacement.data(),3))return failed("ssh-profile");
    wanted=original;
    for(unsigned i=0;i<3;++i) {
        const size_t word=17+i*16;const uintptr_t at=expected.cpu+word*4;
        if(!same() || !exclude(expected.cpu,256) || !writable(at,4) ||
            !read(expected.cpu,again.data(),sizeof(again)) || again!=wanted || !same())return failed("ssh-write-authority");
        mutated=true; // Even failed write is ambiguous; never infer old contents.
        if(!write(at,replacement[i]))return failed("ssh-write");
        wanted[word]=replacement[i];
        if(!same() || !read(expected.cpu,again.data(),sizeof(again)) || again!=wanted || !same())return failed("ssh-readback");
    }
    return {true,mutated,"ssh-normalized"};
}
template<class Api> class OwnedCallbackInput {
    using Pin=TglOwnedTypedCfReference<Api>;
    using Request=NgreenNativeAvcReservation::OwnedAvcCallbackRequest;
    Pin surface_;
    alignas(16) std::array<uint8_t,24> payload_{},expected_{};
    Request request_;
    InputGeometry geometry_;
    uintptr_t context_=0,originalRequest_=0,originalPayload_=0,surfaceHandle_=0,commitTarget_=0;
    bool ready_=false,commitAttempted_=false,committing_=false,committed_=false;
    explicit OwnedCallbackInput(Api api) noexcept :surface_(std::move(api)) {}
    template<class Read,class RootAdmit>
    bool contextGeometry(Read read,RootAdmit admit,InputGeometry& output) const noexcept {
        std::array<uint32_t,2> first{},second{};
        if(!admit() || !read(context_+0x138,first.data(),sizeof(first)) || !admit() ||
            !read(context_+0x138,second.data(),sizeof(second)) || first!=second || !admit())return false;
        output=geometry_;output.contextWidth=first[0];output.contextHeight=first[1];return true;
    }
    bool ownedPrefixMatches() const noexcept {
        return !std::memcmp(payload_.data(),expected_.data(),16) &&
            (!committed_ || !std::memcmp(payload_.data()+16,expected_.data()+16,8));
    }
public:
    OwnedCallbackInput(const OwnedCallbackInput&)=delete;
    OwnedCallbackInput& operator=(const OwnedCallbackInput&)=delete;
    OwnedCallbackInput(OwnedCallbackInput&&)=delete;
    OwnedCallbackInput& operator=(OwnedCallbackInput&&)=delete;
    // Shape runs only after the typed retain. It validates actual immutable
    // IOSurface format/plane/backing metadata against BOTH captured geometries.
    // Neither a coded/visible equality nor padded1088 is inferred here.
    template<class Read,class Shape,class Writable,class RootAdmit>
    static std::unique_ptr<OwnedCallbackInput> captureForContext(uintptr_t context,uintptr_t request,
        Api api,Read read,Shape shape,Writable writable,RootAdmit admit) noexcept {
        if(!context || (context&7U) || context>UINTPTR_MAX-0x4748 ||
            !request || (request&7U) || request>UINTPTR_MAX-0x28 || !admit())return {};
        std::array<uintptr_t,2> root{},afterRoot{};
        if(!read(request+0x18,root.data(),sizeof(root)) || !admit() || root[0]!=context ||
            !root[1] || (root[1]&7U) || root[1]>UINTPTR_MAX-24 ||
            !writable(root[1]+16,8) || !admit())return {};
        auto result=std::unique_ptr<OwnedCallbackInput>(new(std::nothrow) OwnedCallbackInput(api));
        if(!result || !NgreenNativeAvcReservation::disjointOwnedObject(request,0x28,result.get()) ||
            !NgreenNativeAvcReservation::disjointOwnedObject(root[1],24,result.get()) ||
            !(request+0x28<=root[1] || root[1]+24<=request) ||
            !(context+0x4748<=request || request+0x28<=context) ||
            !(context+0x4748<=root[1] || root[1]+24<=context))return {};
        result->context_=context;result->originalRequest_=request;result->originalPayload_=root[1];
        std::array<uint8_t,24> payload{},again{};
        if(!read(root[1],payload.data(),payload.size()) || !admit() ||
            !read(root[1],again.data(),again.size()) || payload!=again || !admit())return {};
        std::memcpy(&result->surfaceHandle_,payload.data(),8);
        std::memcpy(&result->geometry_.requestWidth,payload.data()+8,4);
        std::memcpy(&result->geometry_.requestHeight,payload.data()+12,4);
        InputGeometry geometry;
        if(!result->surfaceHandle_ || !result->contextGeometry(read,admit,geometry) ||
            !geometry.contextWidth || !geometry.contextHeight || !geometry.requestWidth || !geometry.requestHeight ||
            geometry.contextWidth>16384 || geometry.contextHeight>16384 ||
            geometry.requestWidth>16384 || geometry.requestHeight>16384)return {};
        result->geometry_=geometry;
        const auto type=api.surfaceType();
        if(!type || !admit() || !result->surface_.acquire(result->surfaceHandle_,type) || !admit() ||
            !shape(result->surfaceHandle_,geometry) || !admit())return {};
        result->payload_=payload;result->expected_=payload;
        const auto owned=reinterpret_cast<uintptr_t>(result->payload_.data());
        if(!result->request_.capture(request,root[1],owned,read,
            [&](uintptr_t r,uintptr_t p,uintptr_t o) noexcept {
                return r==request && p==root[1] && o==owned && admit();
            }) || !read(request+0x18,afterRoot.data(),sizeof(afterRoot)) || root!=afterRoot || !admit())return {};
        result->ready_=true;
        if(!result->matchesContext(context,read,admit))return {};
        return result;
    }
    const Request& callbackRequest() const noexcept {return request_;}
    uintptr_t nativeContext() const noexcept {return ready_?context_:0;}
    uintptr_t originalRequest() const noexcept {return ready_?originalRequest_:0;}
    uintptr_t originalPayload() const noexcept {return ready_?originalPayload_:0;}
    uintptr_t callbackPayload() const noexcept {return ready_?reinterpret_cast<uintptr_t>(payload_.data()):0;}
    uintptr_t surface() const noexcept {return ready_ && surface_.owns(surfaceHandle_)?surfaceHandle_:0;}
    const InputGeometry& geometry() const noexcept {return geometry_;}
    bool excludesOwnedStorage(uintptr_t address,size_t bytes) const noexcept {
        return NgreenNativeAvcReservation::disjointOwnedObject(address,bytes,this);
    }
    template<class Read,class RootAdmit>
    bool matchesContext(uintptr_t context,Read read,RootAdmit admit) const noexcept {
        if(!ready_ || context!=context_ || !surface_.owns(surfaceHandle_) || !ownedPrefixMatches() ||
            !request_.matchesContextIdentity(context) || !admit() ||
            !request_.matchesOriginal(originalRequest_,originalPayload_,read,admit))return false;
        InputGeometry current;
        std::array<uint8_t,24> first{},second{};
        if(!contextGeometry(read,admit,current) || !(current==geometry_) ||
            !read(originalPayload_,first.data(),first.size()) || !admit() ||
            !read(originalPayload_,second.data(),second.size()) || first!=second || !admit() ||
            std::memcmp(first.data(),expected_.data(),16))return false;
        uintptr_t observed=0,expected=0;
        std::memcpy(&observed,first.data()+16,8);std::memcpy(&expected,expected_.data()+16,8);
        return observed==expected || (committing_ && observed==commitTarget_);
    }
    // Exact native ad00 writes payload+16 from context+4720. Commit only that
    // pointer-sized field to the original caller's data; never dereference it,
    // manufacture checksum contents, or treat this as GPU completion.
    template<class Read,class Write,class Writable,class RootAdmit>
    bool commitNativeOutput(Read read,Write write,Writable writable,RootAdmit admit) noexcept {
        if(commitAttempted_)return false;
        commitAttempted_=true;
        if(!matchesContext(context_,read,admit))return false;
        uintptr_t candidate=0,native=0;
        std::memcpy(&candidate,payload_.data()+16,8);
        if(!candidate || !admit() || !read(context_+0x4720,&native,8) || !admit() || candidate!=native ||
            !writable(originalPayload_+16,8) || !matchesContext(context_,read,admit))return false;
        commitTarget_=candidate;committing_=true;
        const bool written=write(originalPayload_+16,candidate);
        std::array<uint8_t,24> after{},expected=expected_;
        std::memcpy(expected.data()+16,&candidate,8);
        const bool verified=written && admit() && read(originalPayload_,after.data(),after.size()) &&
            after==expected && admit() && read(context_+0x4720,&native,8) && native==candidate &&
            matchesContext(context_,read,admit);
        if(verified){expected_=expected;committed_=true;}
        committing_=false;
        return verified && matchesContext(context_,read,admit);
    }
};

class OwnedCallbackPhase {
public:
    struct Identity {uintptr_t image=0,physical=0,context=0,request=0;uint64_t generation=0;uintptr_t output=0;};
    // The outer exclusive owner supplies program-prepared, no-earlier-pending
    // and retained input/IOSurface authority. Readability alone is insufficient.
    using Live=bool(*)(void*,const Identity&) noexcept;
    using Read=bool(*)(uintptr_t,void*,size_t) noexcept;
    using Writable=bool(*)(uintptr_t,size_t) noexcept;
    using Write=bool(*)(uintptr_t,uintptr_t) noexcept;
    struct Result {bool invoked=false;uint64_t status=5;const char* reason="phase";};
private:
    enum class Stage {Ready,SizeRunning,Sized,BuilderRunning,Done,Failed};
    struct Domain {};
    using Scope=TglOwnedScopedContext<OwnedCallbackPhase,Domain>;
    using Request=NgreenNativeAvcReservation::OwnedAvcCallbackRequest;
    Identity identity_;
    const Request& request_;
    void* authority_=nullptr;Live live_=nullptr;Read read_=nullptr;
    Writable writable_=nullptr;Write write_=nullptr;
    void* input_=nullptr;uintptr_t originalPayload_=0;
    bool(*inputExcludes_)(const void*,uintptr_t,size_t) noexcept=nullptr;
    bool(*inputMatches_)(const void*,const OwnedCallbackPhase&) noexcept=nullptr;
    bool(*inputCommit_)(void*,OwnedCallbackPhase&) noexcept=nullptr;
    InputGeometry inputGeometry_;
    Stage stage_=Stage::Ready;bool builderEntered_=false,sizeSucceeded_=false;Scope scope_;
    struct Buffer {
        uintptr_t token=0,connection=0,base=0,end=0,cursor=0;
        uintptr_t sideCursor=0,sideEnd=0,body=0,side=0;
    };
    static bool disjoint(uintptr_t a,size_t an,uintptr_t b,size_t bn) noexcept {
        return a && b && a<=UINTPTR_MAX-an && b<=UINTPTR_MAX-bn && (a+an<=b || b+bn<=a);
    }
    bool live() const noexcept {
        const bool admitted=stage_!=Stage::Failed && scope_.bound() && authority_ && live_ && read_ && writable_ && write_ && input_ && originalPayload_ && inputExcludes_ && inputMatches_ && inputCommit_ &&
            identity_.image && identity_.image<=UINTPTR_MAX-0x754498 &&
            identity_.context && identity_.context<=UINTPTR_MAX-0x4748 &&
            identity_.physical && identity_.request && identity_.generation &&
            live_(authority_,identity_) && inputMatches_(input_,*this);
        return admitted && stage_!=Stage::Failed;
    }
    template<class T> bool readContext(size_t offset,T& value) const noexcept {
        return live() && offset<=0x4748-sizeof(T) &&
            read_(identity_.context+offset,&value,sizeof(value)) && live();
    }
    bool geometry() const noexcept {
        uint8_t media=0,shift=0;uint32_t inner=0,side=0,count=0,index=0,width=0,height=0;
        return readContext(0x124,media) && media==1 && readContext(0x12c,shift) && shift==0 &&
            readContext(0x130,inner) && inner==69 && readContext(0x134,side) && side==36 &&
            readContext(0x174,count) && count==1 && readContext(0x178,index) && index==0 &&
            readContext(0x138,width) && width==inputGeometry_.contextWidth &&
            readContext(0x13c,height) && height==inputGeometry_.contextHeight;
    }
    bool outsideInputs(uintptr_t address,size_t bytes) const noexcept {
        uintptr_t payload=0;
        const auto request=request_.callbackRequest();
        return request && read_(request+0x20,&payload,sizeof(payload)) && payload &&
            inputExcludes_(input_,address,bytes) && disjoint(address,bytes,originalPayload_,24) &&
            disjoint(address,bytes,reinterpret_cast<uintptr_t>(this),sizeof(*this)) &&
            disjoint(address,bytes,identity_.context,0x4748) &&
            disjoint(address,bytes,identity_.request,0x28) &&
            (!identity_.output || disjoint(address,bytes,identity_.output,8)) &&
            disjoint(address,bytes,request,0x28) && disjoint(address,bytes,payload,24);
    }
    bool capture(uintptr_t token,Buffer& out,bool after) const noexcept {
        uintptr_t producer=0;
        if(!live() || !token || (token&7U) || token>UINTPTR_MAX-0x80 ||
            !readContext(0x10,producer) || !producer || producer>UINTPTR_MAX-0x130 ||
            token!=producer+0x48)return false;
        std::array<uint8_t,0x80> first{},second{};
        if(!read_(token,first.data(),first.size()) || !live() ||
            !read_(token,second.data(),second.size()) || first!=second || !live())return false;
        Buffer value;value.token=token;
        std::memcpy(&value.connection,first.data()+0x08,8);
        std::memcpy(&value.base,first.data()+0x10,8);std::memcpy(&value.end,first.data()+0x18,8);
        std::memcpy(&value.cursor,first.data()+0x20,8);std::memcpy(&value.sideCursor,first.data()+0x60,8);
        std::memcpy(&value.sideEnd,first.data()+0x68,8);std::memcpy(&value.body,first.data()+0x70,8);
        std::memcpy(&value.side,first.data()+0x78,8);
        constexpr size_t bodyBytes=NgreenChecksumGen12::producedBodyWords*4;
        constexpr size_t sideBytes=39*4;
        if(!value.connection || !value.base || !value.body || !value.side ||
            ((value.base|value.end|value.cursor|value.sideCursor|value.sideEnd|value.body|value.side)&3U) ||
            value.body<value.base || value.body>UINTPTR_MAX-bodyBytes-8 ||
            value.cursor!=value.body+bodyBytes || value.end<value.cursor+8 ||
            value.end-value.base>65536 || value.sideCursor>value.sideEnd ||
            value.side>(after?UINTPTR_MAX-12:UINTPTR_MAX-sideBytes) ||
            value.sideCursor!=value.side+(after?12:sideBytes) ||
            !outsideInputs(value.body,bodyBytes) || !outsideInputs(value.side,after?12:sideBytes) ||
            !disjoint(value.body,bodyBytes,producer,0x130) ||
            !disjoint(value.side,after?12:sideBytes,producer,0x130) ||
            !disjoint(value.body,bodyBytes,value.side,after?12:sideBytes) ||
            !writable_(value.body,bodyBytes) || !writable_(value.side,after?12:sideBytes) || !live())return false;
        // The native builder alone advances side by 36 DWORD; it never owns
        // the two GPU END reserve words. Those remain for native a9dc/FIFO.
        out=value;return true;
    }
    Result failed(bool invoked,const char* reason,uint64_t status=5) noexcept {
        stage_=Stage::Failed;
        // The exact outer checks only EAX. A high-only error must not become
        // success when truncated by its native branch.
        return {invoked,uint32_t(status)?status:5,reason};
    }
    bool enter(Stage expected,Stage running,uintptr_t context,uintptr_t request) noexcept {
        if(stage_!=expected || context!=identity_.context || request!=identity_.request || !live()) {
            stage_=Stage::Failed;return false;
        }
        stage_=running;return true;
    }
public:
    // No raw-request constructor: a typed retained input must outlive this
    // synchronous phase, including after transfer into its pending ticket.
    template<class Api>
    OwnedCallbackPhase(Identity identity,OwnedCallbackInput<Api>& input,void* authority,
        Live live,Read read,Writable writable,Write write) noexcept
        :identity_(identity),request_(input.callbackRequest()),authority_(authority),live_(live),read_(read),
         writable_(writable),write_(write),input_(&input),originalPayload_(input.originalPayload()),
         inputExcludes_([](const void* object,uintptr_t address,size_t bytes) noexcept {
             return static_cast<const OwnedCallbackInput<Api>*>(object)->excludesOwnedStorage(address,bytes);
         }),inputMatches_([](const void* object,const OwnedCallbackPhase& phase) noexcept {
             const auto& held=*static_cast<const OwnedCallbackInput<Api>*>(object);
             return held.nativeContext()==phase.identity_.context && held.originalRequest()==phase.identity_.request &&
                 held.surface() && held.matchesContext(phase.identity_.context,phase.read_,[&]() noexcept {
                     return phase.live_(phase.authority_,phase.identity_);
                 });
         }),inputCommit_([](void* object,OwnedCallbackPhase& phase) noexcept {
             return static_cast<OwnedCallbackInput<Api>*>(object)->commitNativeOutput(
                 phase.read_,phase.write_,phase.writable_,[&]() noexcept {
                     return phase.live_(phase.authority_,phase.identity_);
                 });
         }),inputGeometry_(input.geometry()),scope_(reinterpret_cast<void*>(identity.context),*this) {}
    OwnedCallbackPhase(const OwnedCallbackPhase&)=delete;
    OwnedCallbackPhase& operator=(const OwnedCallbackPhase&)=delete;
    bool bound() const noexcept {return stage_==Stage::Ready && live();}
    bool started() const noexcept {return stage_!=Stage::Ready;}
    bool builderStarted() const noexcept {return builderEntered_;}
    bool callbacksComplete() const noexcept {return stage_==Stage::Done && live();}
    // Call with ACTUAL native2190a status, before receipt/bind changes the
    // reservation. After size succeeds, begin/acquire may fail BEFORE the
    // builder and leave its mutex held; post-builder7 also bypasses release. This is
    // not a GPU-completion test and must not consume an owned bind status.
    bool nativeReturnRequiresStop(int nativeStatus) const noexcept {
        return ((sizeSucceeded_ || builderEntered_) && nativeStatus!=0) || (nativeStatus==0 && !callbacksComplete());
    }
    bool admitsOuter(uintptr_t image,uintptr_t physical,uint64_t generation,uintptr_t context,uintptr_t request) const noexcept {
        return identity_.image==image && identity_.physical==physical && identity_.generation==generation &&
            identity_.context==context && identity_.request==request && bound();
    }
    static OwnedCallbackPhase* current(uintptr_t context) noexcept {return Scope::get(reinterpret_cast<void*>(context));}
    template<class Invoke> Result size(uintptr_t context,uintptr_t request,
        uint32_t* used,uint32_t* capacity,Invoke invoke) noexcept {
        const auto u=reinterpret_cast<uintptr_t>(used),c=reinterpret_cast<uintptr_t>(capacity);
        if(!used || !capacity || (u&3U) || (c&3U) || !disjoint(u,4,c,4) ||
            !enter(Stage::Ready,Stage::SizeRunning,context,request) ||
            !outsideInputs(u,4) || !outsideInputs(c,4) || !writable_(u,4) || !writable_(c,4) || !live())
            return failed(false,"size-admission");
        const auto status=invoke(identity_.image+0x2c66a,context,request_.callbackRequest(),used,capacity);
        uint32_t body=0,side=0;
        if(status || !live() || !read_(u,&body,4) || !read_(c,&side,4) ||
            body!=NgreenChecksumGen12::nativeBodyWords || side!=36 || !geometry())
            return failed(true,"native-size",status);
        if(!writable_(u,4) || !live())return failed(true,"size-write-authority");
        *used=NgreenChecksumGen12::producedBodyWords;
        if(!live() || *used!=NgreenChecksumGen12::producedBodyWords || *capacity!=36 || !geometry())
            return failed(true,"size-readback");
        sizeSucceeded_=true;stage_=Stage::Sized;return {true,0,"sized"};
    }
    template<class Invoke> Result builder(uintptr_t context,uintptr_t request,uintptr_t token,Invoke invoke) noexcept {
        builderEntered_=true; // Native2190a has passed22708 before this callback.
        if(!enter(Stage::Sized,Stage::BuilderRunning,context,request) || !geometry())return failed(false,"builder-admission");
        Buffer before;
        if(!capture(token,before,false))return failed(false,"builder-buffer");
        SurfaceHeapObservation surface;
        if(!SurfaceHeapObservation::capture(identity_.context,read_,[&]() noexcept {return live();},surface))
            return failed(false,"ssh-before-builder");
        const auto status=invoke(identity_.image+0x2c6f6,context,request_.callbackRequest(),token);
        Buffer after;uintptr_t recorded=0;
        if(status || !live() || !geometry() || !capture(token,after,true) ||
            !readContext(0x41d8,recorded) || recorded!=token || after.connection!=before.connection ||
            after.base!=before.base || after.end!=before.end || after.cursor!=before.cursor ||
            after.sideCursor!=before.sideCursor || after.sideEnd!=before.sideEnd || after.body!=before.body ||
            after.side!=before.side+36*4)return failed(true,"native-builder",status);
        const auto cache=normalizeSurfaceCache(identity_.context,surface,read_,writable_,
            [&](uintptr_t at,uint32_t value) noexcept {
                if(!live() || !writable_(at,4))return false;
                *reinterpret_cast<volatile uint32_t*>(at)=value;
                return live();
            },[&]() noexcept {return live();},[&](uintptr_t at,size_t bytes) noexcept {
                uintptr_t producer=0;
                return live() && outsideInputs(at,bytes) && readContext(0x10,producer) &&
                    disjoint(at,bytes,producer,0x130) &&
                    disjoint(at,bytes,after.base,size_t(after.end-after.base)) &&
                    disjoint(at,bytes,before.side,39*4);
            });
        if(!cache.ok)return failed(true,cache.reason);
        if(!live() || !writable_(after.body,NgreenChecksumGen12::producedBodyWords*4) ||
            !NgreenChecksumGen12::writeReservationTrailer(reinterpret_cast<volatile uint32_t*>(after.body),
                NgreenChecksumGen12::producedBodyWords) ||
            !NgreenChecksumGen12::reservationMatches(reinterpret_cast<const volatile uint32_t*>(after.body),
                NgreenChecksumGen12::reservedStreamWords) || !live())return failed(true,"trailer");
        // Publish the native output field only through its still-owned input
        // graph. The commit is not GPU completion; pending pins outlive it.
        if(!inputCommit_(input_,*this) || !live())return failed(true,"input-output");
        stage_=Stage::Done;return {true,0,"built"};
    }
};
template<class Invoke>
OwnedCallbackPhase::Result sizeResult(uintptr_t context,uintptr_t request,uint32_t* used,uint32_t* capacity,Invoke invoke) noexcept {
    if(auto* phase=OwnedCallbackPhase::current(context))return phase->size(context,request,used,capacity,invoke);
    return {false,5,"missing-phase"};
}
template<class Invoke>
uint64_t sizeOrRefuse(uintptr_t context,uintptr_t request,uint32_t* used,uint32_t* capacity,Invoke invoke) noexcept {
    return sizeResult(context,request,used,capacity,invoke).status;
}
template<class Invoke,class Stop>
uint64_t builderOrStop(uintptr_t context,uintptr_t request,uintptr_t token,Invoke invoke,Stop stop) noexcept {
    if(auto* phase=OwnedCallbackPhase::current(context)) {
        const auto result=phase->builder(context,request,token,invoke);
        if(!result.status)return 0;
        stop(result);std::abort();
    }
    // No call to native227e0: it acquires an event/stamp and submits. Until
    // a separately proven CPU-only unlock/cancel exists, do not let native
    // teardown flush partial buffers or retain22708's user mutex forever.
    stop(OwnedCallbackPhase::Result{false,5,"missing-phase"});std::abort();
}
}
