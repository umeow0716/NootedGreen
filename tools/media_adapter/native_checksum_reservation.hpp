#pragma once
#include "native_avc_reservation.hpp"
#include "../../NootedGreen/kern_checksum_gen12_commands.hpp"

// Only exact FrameChecksum2190a: its size/builder EAX branches skip native
// submit on failure. AVC27724 ignores those values and must not use this API.
namespace NgreenNativeChecksumReservation {
class OwnedCallbackPhase {
public:
    struct Identity {uintptr_t image=0,physical=0,context=0,request=0;uint64_t generation=0;};
    // The outer exclusive owner supplies program-prepared, no-earlier-pending
    // and retained input/IOSurface authority. Readability alone is insufficient.
    using Live=bool(*)(void*,const Identity&) noexcept;
    using Read=bool(*)(uintptr_t,void*,size_t) noexcept;
    using Writable=bool(*)(uintptr_t,size_t) noexcept;
    using Commit=bool(*)(void*,const Identity&) noexcept;
    struct Result {bool invoked=false;uint64_t status=5;const char* reason="phase";};
private:
    enum class Stage {Ready,SizeRunning,Sized,BuilderRunning,Done,Failed};
    struct Domain {};
    using Scope=TglOwnedScopedContext<OwnedCallbackPhase,Domain>;
    using Request=NgreenNativeAvcReservation::OwnedAvcCallbackRequest;
    Identity identity_;
    const Request& request_;
    void* authority_=nullptr;Live live_=nullptr;Read read_=nullptr;
    Writable writable_=nullptr;Commit commit_=nullptr;
    Stage stage_=Stage::Ready;Scope scope_;
    struct Buffer {
        uintptr_t token=0,connection=0,base=0,end=0,cursor=0;
        uintptr_t sideCursor=0,sideEnd=0,body=0,side=0;
    };
    static bool disjoint(uintptr_t a,size_t an,uintptr_t b,size_t bn) noexcept {
        return a && b && a<=UINTPTR_MAX-an && b<=UINTPTR_MAX-bn && (a+an<=b || b+bn<=a);
    }
    bool live() const noexcept {
        const bool admitted=stage_!=Stage::Failed && scope_.bound() && authority_ && live_ && read_ && writable_ && commit_ &&
            identity_.image && identity_.image<=UINTPTR_MAX-0x754498 &&
            identity_.context && identity_.context<=UINTPTR_MAX-0x4748 &&
            identity_.physical && identity_.request && identity_.generation &&
            request_.matchesContextIdentity(identity_.context) && live_(authority_,identity_);
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
            readContext(0x138,width) && width==1920 && readContext(0x13c,height) && height==1080;
    }
    bool outsideInputs(uintptr_t address,size_t bytes) const noexcept {
        uintptr_t payload=0;
        const auto request=request_.callbackRequest();
        return request && read_(request+0x20,&payload,sizeof(payload)) && payload &&
            disjoint(address,bytes,reinterpret_cast<uintptr_t>(this),sizeof(*this)) &&
            disjoint(address,bytes,identity_.context,0x4748) &&
            disjoint(address,bytes,identity_.request,0x28) &&
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
    OwnedCallbackPhase(Identity identity,const Request& request,void* authority,
        Live live,Read read,Writable writable,Commit commit) noexcept
        :identity_(identity),request_(request),authority_(authority),live_(live),read_(read),
         writable_(writable),commit_(commit),scope_(reinterpret_cast<void*>(identity.context),*this) {}
    OwnedCallbackPhase(const OwnedCallbackPhase&)=delete;
    OwnedCallbackPhase& operator=(const OwnedCallbackPhase&)=delete;
    bool bound() const noexcept {return stage_==Stage::Ready && live();}
    bool started() const noexcept {return stage_!=Stage::Ready;}
    bool complete() const noexcept {return stage_==Stage::Done && live();}
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
        stage_=Stage::Sized;return {true,0,"sized"};
    }
    template<class Invoke> Result builder(uintptr_t context,uintptr_t request,uintptr_t token,Invoke invoke) noexcept {
        if(!enter(Stage::Sized,Stage::BuilderRunning,context,request) || !geometry())return failed(false,"builder-admission");
        Buffer before;
        if(!capture(token,before,false))return failed(false,"builder-buffer");
        const auto status=invoke(identity_.image+0x2c6f6,context,request_.callbackRequest(),token);
        Buffer after;uintptr_t recorded=0;
        if(status || !live() || !geometry() || !capture(token,after,true) ||
            !readContext(0x41d8,recorded) || recorded!=token || after.connection!=before.connection ||
            after.base!=before.base || after.end!=before.end || after.cursor!=before.cursor ||
            after.sideCursor!=before.sideCursor || after.sideEnd!=before.sideEnd || after.body!=before.body ||
            after.side!=before.side+36*4)return failed(true,"native-builder",status);
        if(!live() || !writable_(after.body,NgreenChecksumGen12::producedBodyWords*4) ||
            !NgreenChecksumGen12::writeReservationTrailer(reinterpret_cast<volatile uint32_t*>(after.body),
                NgreenChecksumGen12::producedBodyWords) ||
            !NgreenChecksumGen12::reservationMatches(reinterpret_cast<const volatile uint32_t*>(after.body),
                NgreenChecksumGen12::reservedStreamWords) || !live())return failed(true,"trailer");
        // Publish the native output field only through its still-owned input
        // graph. The commit is not GPU completion; pending pins outlive it.
        if(!commit_(authority_,identity_) || !live())return failed(true,"input-output");
        stage_=Stage::Done;return {true,0,"built"};
    }
};
template<class Invoke>
uint64_t sizeOrRefuse(uintptr_t context,uintptr_t request,uint32_t* used,uint32_t* capacity,Invoke invoke) noexcept {
    if(auto* phase=OwnedCallbackPhase::current(context))return phase->size(context,request,used,capacity,invoke).status;
    return 5;
}
template<class Invoke>
uint64_t builderOrRefuse(uintptr_t context,uintptr_t request,uintptr_t token,Invoke invoke) noexcept {
    if(auto* phase=OwnedCallbackPhase::current(context))return phase->builder(context,request,token,invoke).status;
    return 5;
}
}
