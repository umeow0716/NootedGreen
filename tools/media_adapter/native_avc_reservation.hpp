#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <array>
#include <memory>
#include <new>
#include "owned_resource.hpp"
#include "../../NootedGreen/kern_avc_gen12_commands.hpp"
#include "../../NootedGreen/kern_vf_device_observation.hpp"

// Exact signed producer ABI, not a library-validation or ownership bypass.
// A successful read does not authorize a write: the caller still needs the
// live native builder interval and a kernel protocol handshake.
namespace NgreenNativeAvcReservation {
// Exact2732c/2737e consume request+20 only. This view is for those two
// callbacks, NEVER outer27724: the outer retains original output aliases.
// Admission must hold source exclusion and the owned payload/surface leases
// throughout BOTH callbacks. Readability and the stable copy are not leases.
class OwnedAvcCallbackRequest {
    static_assert(sizeof(uintptr_t)==8,"exact x86_64 native request ABI");
    alignas(uintptr_t) std::array<unsigned char,0x28> bytes_{};
    bool attempted_=false,ready_=false;
public:
    OwnedAvcCallbackRequest()=default;
    OwnedAvcCallbackRequest(const OwnedAvcCallbackRequest&)=delete;
    OwnedAvcCallbackRequest& operator=(const OwnedAvcCallbackRequest&)=delete;
    OwnedAvcCallbackRequest(OwnedAvcCallbackRequest&&)=delete;
    OwnedAvcCallbackRequest& operator=(OwnedAvcCallbackRequest&&)=delete;
    template<class Read,class Admit>
    bool capture(uintptr_t original,uintptr_t originalPayload,uintptr_t ownedPayload,
                 Read read,Admit admit) noexcept {
        if(attempted_)return false;
        attempted_=true;
        const auto self=reinterpret_cast<uintptr_t>(this);
        if(!original || original>UINTPTR_MAX-bytes_.size() || !originalPayload ||
           !ownedPayload || originalPayload==ownedPayload ||
           (original<self+sizeof(*this) && self<original+bytes_.size()) ||
           !admit(original,originalPayload,ownedPayload))return false;
        std::array<unsigned char,0x28> first{},second{};
        uintptr_t payload=0;
        if(!read(original,first.data(),first.size()) ||
           !read(original,second.data(),second.size()) || first!=second)return false;
        std::memcpy(&payload,first.data()+0x20,sizeof(payload));
        if(payload!=originalPayload || !admit(original,originalPayload,ownedPayload))return false;
        bytes_=first;
        std::memcpy(bytes_.data()+0x20,&ownedPayload,sizeof(ownedPayload));
        ready_=true;
        return true;
    }
    uintptr_t callbackRequest() const noexcept {
        return ready_?reinterpret_cast<uintptr_t>(bytes_.data()):0;
    }
};
// Synchronous size->builder pairing only. The surrounding frame owner retains
// input/object pins and supplies the SAME live authority throughout. This is
// not a completion lease and must never outlive those owners.
class OwnedAvcCallbackPhase {
public:
    struct Identity {uintptr_t image=0,physical=0,context=0,request=0;uint64_t generation=0;};
    using Live=bool(*)(void*,const Identity&) noexcept;
    struct [[nodiscard]] Result {bool invoked=false,mustStop=true;uint64_t nativeStatus=0;};
private:
    enum class Stage {Ready,SizeRunning,Sized,BuilderRunning,Done,Failed};
    struct Domain {};
    using Scope=TglOwnedScopedContext<OwnedAvcCallbackPhase,Domain>;
    Identity identity_;
    const OwnedAvcCallbackRequest& request_;
    void* authority_=nullptr;Live live_=nullptr;
    Stage stage_=Stage::Ready;
    Scope scope_;
    bool live() const noexcept {
        return scope_.bound() && authority_ && live_ && request_.callbackRequest() &&
            identity_.image && identity_.image<=UINTPTR_MAX-0x2737e &&
            identity_.physical && identity_.context && identity_.request && identity_.generation &&
            live_(authority_,identity_);
    }
    bool enter(Stage expected,Stage running,uintptr_t context,uintptr_t request) noexcept {
        if(stage_!=expected || context!=identity_.context || request!=identity_.request || !live()) {
            stage_=Stage::Failed;return false;
        }
        stage_=running;return true;
    }
    Result finish(Stage running,Stage next,uint64_t status) noexcept {
        // Native outer ignores BOTH callback EAX values. A failure/reentry
        // requires owned fail-stop, never merely returning5 to outer27724.
        if(stage_!=running || status || !live()) {stage_=Stage::Failed;return {true,true,status};}
        stage_=next;return {true,false,status};
    }
public:
    OwnedAvcCallbackPhase(Identity identity,const OwnedAvcCallbackRequest& request,
        void* authority,Live live) noexcept
        :identity_(identity),request_(request),authority_(authority),live_(live),
         scope_(reinterpret_cast<void*>(identity.context),*this) {}
    OwnedAvcCallbackPhase(const OwnedAvcCallbackPhase&)=delete;
    OwnedAvcCallbackPhase& operator=(const OwnedAvcCallbackPhase&)=delete;
    bool bound() const noexcept {return stage_==Stage::Ready && live();}
    bool complete() const noexcept {return stage_==Stage::Done && live();}
    static OwnedAvcCallbackPhase* current(uintptr_t context) noexcept {
        return Scope::get(reinterpret_cast<void*>(context));
    }
    template<class Invoke> Result size(uintptr_t context,uintptr_t request,
        uint32_t* used,uint32_t* capacity,Invoke invoke) noexcept {
        if(!used || !capacity || !enter(Stage::Ready,Stage::SizeRunning,context,request)) {
            stage_=Stage::Failed;return {};
        }
        const auto status=invoke(identity_.image+0x2732c,context,request_.callbackRequest(),used,capacity);
        return finish(Stage::SizeRunning,Stage::Sized,status);
    }
    template<class Invoke> Result builder(uintptr_t context,uintptr_t request,uintptr_t token,Invoke invoke) noexcept {
        if(!token || !enter(Stage::Sized,Stage::BuilderRunning,context,request)) {
            stage_=Stage::Failed;return {};
        }
        const auto status=invoke(identity_.image+0x2737e,context,request_.callbackRequest(),token);
        return finish(Stage::BuilderRunning,Stage::Done,status);
    }
};
// The exact AVC frame cannot fall back to untranslated native submission when
// the kernel publication handshake is absent. Other frame families retain
// their existing admission; this is not a claim that readiness proves safety.
inline bool frameAdmission(uintptr_t image,uintptr_t entry,bool publicationReady) noexcept {
    if(!image || image>UINTPTR_MAX-0x27724 || !entry)return false;
    return entry!=image+0x27724 || publicationReady;
}
// Exact23fad clears sixteen DWORDs at token+c78, then uses each non-null
// payload reference handle's DWORD index directly as the destination slot.
// Check BEFORE native frame entry, not after its resource/token writes.
// This bounded observation does not freeze caller memory or prove lifetime;
// the caller still needs the same exclusive input graph throughout native use.
template<class Read>
bool referenceIndices(uintptr_t request,Read read,uintptr_t expectedPayload=0) noexcept {
    uintptr_t payload=0;
    if(!request || request>UINTPTR_MAX-0x28 ||
       !read(request+0x20,&payload,sizeof(payload)) || !payload ||
       (expectedPayload && payload!=expectedPayload) ||
       payload>UINTPTR_MAX-0x40c)return false;
    for(size_t i=0;i<16;++i) {
        uint64_t handle=0;uint32_t index=0;
        if(!read(payload+0x38+i*0x40,&handle,sizeof(handle)))return false;
        if(handle && (!read(payload+0x48+i*0x40,&index,sizeof(index)) || index>=16))return false;
    }
    uintptr_t current=0;
    return read(request+0x20,&current,sizeof(current)) && current==payload;
}
// Exact23543 indexes the adjacent 52-DWORD tables31f5c0/31f690/...
// directly by payload438. This is the supported 8-bit AVC QP domain,
// not permission to clamp unsupported input or change native rate control.
template<class Read>
bool quantizerIndex(uintptr_t request,Read read,uintptr_t expectedPayload=0) noexcept {
    uintptr_t payload=0,current=0;uint32_t qp=0;
    if(!request || request>UINTPTR_MAX-0x28 ||
       !read(request+0x20,&payload,sizeof(payload)) || !payload ||
       (expectedPayload && payload!=expectedPayload) ||
       payload>UINTPTR_MAX-0x43c || !read(payload+0x438,&qp,sizeof(qp)) || qp>51)return false;
    return read(request+0x20,&current,sizeof(current)) && current==payload;
}
// Exact2737e stores the first slice's override (or PPS default) plus one
// into outer+ac0/ac4 = builder-view+998/99c. Exact25cf6 consumes those
// counts using an eight-bit loop cursor and a 32-byte output array.
// Preserve native uint32 increment/wrap: ffffffff becomes the zero count.
// Like the other preflights, this is not an immutable-input/lifetime proof.
template<class Read>
bool activeReferenceCounts(uintptr_t request,Read read,uintptr_t expectedPayload=0) noexcept {
    uintptr_t payload=0,table=0,slice=0,current=0;
    uint32_t overridden=0,minusOne[2]{};
    if(!request || request>UINTPTR_MAX-0x28 ||
       !read(request+0x20,&payload,sizeof(payload)) || !payload ||
       (expectedPayload && payload!=expectedPayload) || payload>UINTPTR_MAX-0x438 ||
       !read(payload+0x430,&table,sizeof(table)) || !table || table>UINTPTR_MAX-8 ||
       !read(table,&slice,sizeof(slice)) || !slice || slice>UINTPTR_MAX-0x3c ||
       !read(slice+0x38,&overridden,sizeof(overridden)))return false;
    if(overridden) {
        if(slice>UINTPTR_MAX-0x44 || !read(slice+0x3c,minusOne,sizeof(minusOne)))return false;
    } else {
        uintptr_t picture=0;
        if(!read(payload+0x418,&picture,sizeof(picture)) || !picture || picture>UINTPTR_MAX-0x98 ||
           !read(picture+0x90,minusOne,sizeof(minusOne)))return false;
    }
    if(minusOne[0]+uint32_t(1)>32 || minusOne[1]+uint32_t(1)>32)return false;
    return read(request+0x20,&current,sizeof(current)) && current==payload;
}
// One observed payload identity for ALL component checks, not three
// independently stable roots. This is still only a preflight: field mutation,
// unobserved ABA and lifetime across native use require stronger ownership.
template<class Read>
bool framePreflight(uintptr_t request,Read read) noexcept {
    uintptr_t payload=0;
    if(!request || request>UINTPTR_MAX-0x28 ||
       !read(request+0x20,&payload,sizeof(payload)) || !payload)return false;
    return referenceIndices(request,read,payload) && quantizerIndex(request,read,payload) &&
        activeReferenceCounts(request,read,payload);
}
// Owned subgraph for the EXACT native VDEnc template path: size reads all
// slice lengths, while 2663c calls 2618d with source index0 only. This does
// not enable indexed substitution, capture SPS/PPS/slices/surface ownership,
// or authorize native submission. Not connected to runtime callbacks yet.
// Read must enforce authenticated SOURCE allocation extents/lifetime; a
// readable VM page alone is not that authority. Capture is not atomic with
// external writers. Validate the resulting complete frame before native use.
class OwnedTemplateNalInput {
public:
    static constexpr size_t MaxSlices=32;
    // Native 12-bit PAK payload length, not a source-allocation size claim.
    static constexpr size_t MaxPayloadBytes=0xfffU*4U;
    OwnedTemplateNalInput(const OwnedTemplateNalInput&)=delete;
    OwnedTemplateNalInput& operator=(const OwnedTemplateNalInput&)=delete;
    OwnedTemplateNalInput(OwnedTemplateNalInput&&)=delete;
    OwnedTemplateNalInput& operator=(OwnedTemplateNalInput&&)=delete;
    template<class Read>
    static std::unique_ptr<OwnedTemplateNalInput> capture(uintptr_t metadata,size_t slices,Read read) noexcept {
        if(!metadata || metadata>UINTPTR_MAX-0x68 || !slices || slices>MaxSlices)return {};
        auto owned=std::unique_ptr<OwnedTemplateNalInput>(new(std::nothrow) OwnedTemplateNalInput{});
        if(!owned || !owned->initialize(metadata,slices,read))return {};
        return owned;
    }
    const uint8_t* metadataBytes() const noexcept {return metadata_.data();}
    size_t sliceCount() const noexcept {return slices_;}
private:
    OwnedTemplateNalInput()=default;
    template<class T> T field(size_t offset) const noexcept {
        T value{};std::memcpy(&value,metadata_.data()+offset,sizeof(value));return value;
    }
    void pointer(size_t offset,uintptr_t value) noexcept {
        std::memcpy(metadata_.data()+offset,&value,sizeof(value));
    }
    template<class Read> bool initialize(uintptr_t metadata,size_t slices,Read read) noexcept {
        static_assert(sizeof(uintptr_t)==8,"exact native x86_64 NAL pointer ABI");
        const auto self=reinterpret_cast<uintptr_t>(this);
        if(self>UINTPTR_MAX-sizeof(*this))return false;
        auto source=[&](uintptr_t address,void* output,size_t bytes) noexcept {
            if(!bytes)return true;
            if(!address || address>UINTPTR_MAX-bytes ||
               (address<self+sizeof(*this) && self<address+bytes))return false;
            return read(address,output,bytes);
        };
        if(!source(metadata,metadata_.data(),metadata_.size()))return false;
        const auto lengths=field<uintptr_t>(0x60),headers=field<uintptr_t>(0x58);
        if(!source(lengths,lengths_.data(),slices*sizeof(uint32_t)))return false;
        for(size_t i=0;i<slices;++i)if(lengths_[i]>MaxPayloadBytes*8U)return false;
        auto copy=[&](size_t index,uintptr_t address,uint32_t bits) noexcept {
            if(bits>MaxPayloadBytes*8U)return false;
            const auto bytes=size_t((uint64_t(bits)+7U)/8U);
            return source(address,payloads_[index].data(),bytes);
        };
        // flag/source/bits triples from25f96/2603d/260e5/25eee.
        constexpr size_t flags[]{0,0x14,0x24,0x34};
        constexpr size_t pointers[]{8,0x18,0x28,0x38};
        constexpr size_t bits[]{0x10,0x20,0x30,0x40};
        for(size_t i=0;i<4;++i) {
            if(field<uint32_t>(flags[i])) {
                if(!copy(i,field<uintptr_t>(pointers[i]),field<uint32_t>(bits[i])))return false;
                pointer(pointers[i],reinterpret_cast<uintptr_t>(payloads_[i].data()));
            } else pointer(pointers[i],0); // no borrowed pointer for an inactive record
        }
        uintptr_t first=0;
        if(!source(headers,&first,sizeof(first)) || !copy(4,first,lengths_[0]))return false;
        // Other source entries are deliberately absent: only the current
        // exact index0 template consumer is qualified, not an indexed reader.
        sources_[0]=reinterpret_cast<uintptr_t>(payloads_[4].data());
        pointer(0x58,reinterpret_cast<uintptr_t>(sources_.data()));
        pointer(0x60,reinterpret_cast<uintptr_t>(lengths_.data()));
        slices_=slices;return true;
    }
    alignas(8) std::array<uint8_t,0x68> metadata_{};
    std::array<uintptr_t,MaxSlices> sources_{};
    std::array<uint32_t,MaxSlices> lengths_{};
    std::array<std::array<uint8_t,MaxPayloadBytes>,5> payloads_{};
    size_t slices_=0;
};
// Joint immutable syntax subgraph, NOT a native frame/payload or surface lease.
// SliceInput is the audited owned slice table; parameterization avoids a
// dependency on renderer/handle ownership. Reader authenticates allocations
// and capture authority. One CPU read is not an atomic snapshot with writers.
template<class SliceInput> class OwnedSliceNalInput {
public:
    OwnedSliceNalInput(const OwnedSliceNalInput&)=delete;
    OwnedSliceNalInput& operator=(const OwnedSliceNalInput&)=delete;
    OwnedSliceNalInput(OwnedSliceNalInput&&)=delete;
    OwnedSliceNalInput& operator=(OwnedSliceNalInput&&)=delete;
    template<class Read> static std::unique_ptr<OwnedSliceNalInput>
    capture(uintptr_t payload,Read read) noexcept {
        static_assert(sizeof(uintptr_t)==8 && SliceInput::MaxSlices==OwnedTemplateNalInput::MaxSlices);
        if(!payload || payload>UINTPTR_MAX-0x488)return {};
        // Count420, table430, metadata480 from ONE captured header, never
        // independent borrowed rereads between slice and NAL factories.
        std::array<uint8_t,0x68> header{};
        if(!read(payload+0x420,header.data(),header.size()))return {};
        uint32_t count=0;uintptr_t table=0,metadata=0;
        std::memcpy(&count,header.data(),4);
        std::memcpy(&table,header.data()+0x10,8);
        std::memcpy(&metadata,header.data()+0x60,8);
        return captureRoots(count,table,metadata,read);
    }
    // Roots already copied by a containing syntax graph; no borrowed root
    // reread. These scalar roots do not authorize access or submission.
    template<class Read> static std::unique_ptr<OwnedSliceNalInput>
    captureRoots(uint32_t count,uintptr_t table,uintptr_t metadata,Read read) noexcept {
        if(!count || count>SliceInput::MaxSlices)return {};
        auto owned=std::unique_ptr<OwnedSliceNalInput>(new(std::nothrow) OwnedSliceNalInput{});
        if(!owned)return {};
        owned->slices_=SliceInput::capture(table,count,read);
        if(!owned->slices_)return {};
        owned->nal_=OwnedTemplateNalInput::capture(metadata,count,read);
        if(!owned->nal_)return {};
        return owned;
    }
    size_t sliceCount() const noexcept {return slices_->count();}
    const uintptr_t* sliceTable() const noexcept {return slices_->table();}
    const uint8_t* nalMetadata() const noexcept {return nal_->metadataBytes();}
private:
    OwnedSliceNalInput()=default;
    std::unique_ptr<SliceInput> slices_;
    std::unique_ptr<OwnedTemplateNalInput> nal_;
};
// CF object pins for exact2737e/23fad/243ec's raw/recon,17 downscale and16
// reference surfaces. Acquire only while caller excludes source destruction/writes;
// a CF retain alone is NOT an input-content lock or a GPU completion lease.
// A future frame owner must retain this object until proven retirement.
template<class Api> class OwnedAvcSurfaceReferences {
    using Pin=TglOwnedTypedCfReference<Api>;
    std::array<std::unique_ptr<Pin>,35> pins_{};
    std::array<uintptr_t,35> handles_{};
    size_t count_=0;
public:
    OwnedAvcSurfaceReferences(const OwnedAvcSurfaceReferences&)=delete;
    OwnedAvcSurfaceReferences& operator=(const OwnedAvcSurfaceReferences&)=delete;
    template<class Admit> static std::unique_ptr<OwnedAvcSurfaceReferences>
    capture(const std::array<uintptr_t,35>& handles,Api api,Admit admit) noexcept {
        if(!handles[0] || !handles[1] || !handles[2] || !admit())return {};
        const auto type=api.surfaceType();
        if(!type)return {};
        auto result=std::unique_ptr<OwnedAvcSurfaceReferences>(new(std::nothrow) OwnedAvcSurfaceReferences{});
        if(!result)return {};
        for(auto handle:handles) {
            if(!handle || result->owns(handle))continue;
            if(!admit())return {};
            auto pin=std::unique_ptr<Pin>(new(std::nothrow) Pin(api));
            if(!pin || !pin->acquire(handle,type))return {};
            result->handles_[result->count_]=handle;
            result->pins_[result->count_++]=std::move(pin);
        }
        if(!admit())return {};
        return result;
    }
    bool owns(uintptr_t handle) const noexcept {
        if(!handle)return false;
        for(size_t i=0;i<count_;++i)if(handles_[i]==handle)return true;
        return false;
    }
    size_t size() const noexcept {return count_;}
private:
    OwnedAvcSurfaceReferences()=default;
};
// All audited syntax inputs share one captured root header. This is NOT a
// native frame: payload scalars/reference handles/downscale leases/output
// aliases and complete SPS/PPS inheritance still require separate proof.
class OwnedAvcSyntaxInput {
    using SliceNal=OwnedSliceNalInput<TglOwnedAvcSliceInputTable>;
public:
    OwnedAvcSyntaxInput(const OwnedAvcSyntaxInput&)=delete;
    OwnedAvcSyntaxInput& operator=(const OwnedAvcSyntaxInput&)=delete;
    OwnedAvcSyntaxInput(OwnedAvcSyntaxInput&&)=delete;
    OwnedAvcSyntaxInput& operator=(OwnedAvcSyntaxInput&&)=delete;
    template<class Read> static std::unique_ptr<OwnedAvcSyntaxInput>
    capture(uintptr_t payload,uint32_t width,uint32_t height,TglOwnedAvcScalingDefaults defaults,Read read) noexcept {
        if(!payload || payload>UINTPTR_MAX-0x488)return {};
        auto owned=std::unique_ptr<OwnedAvcSyntaxInput>(new(std::nothrow) OwnedAvcSyntaxInput{});
        if(!owned)return {};
        const auto self=reinterpret_cast<uintptr_t>(owned.get());
        if(self>UINTPTR_MAX-sizeof(*owned) || (payload<self+sizeof(*owned) && self<payload+0x488) ||
           !read(payload,owned->input_.data(),owned->input_.size()))return {};
        //2737e reads440[0];243ec reads440[1+referenceIndex]. The producer
        //owns17 entries at frontend4abf8..4ac7f, not a single downscale handle.
        // Allocation/lifetime/exclusion authority is still supplied by Read.
        uintptr_t downscale=0;std::memcpy(&downscale,owned->input_.data()+0x440,8);
        std::array<uintptr_t,17> secondDownscale{};
        if(!downscale || downscale>UINTPTR_MAX-sizeof(owned->downscale_) ||
           (downscale<self+sizeof(*owned) && self<downscale+sizeof(owned->downscale_)) ||
           !read(downscale,owned->downscale_.data(),sizeof(owned->downscale_)) ||
           !read(downscale,secondDownscale.data(),sizeof(secondDownscale)) ||
           owned->downscale_!=secondDownscale || !owned->downscale_[0])return {};
        const auto* header=owned->input_.data()+0x410;
        uintptr_t sequence=0,picture=0,table=0,metadata=0;uint32_t count=0;
        std::memcpy(&sequence,header,8);std::memcpy(&picture,header+8,8);
        std::memcpy(&count,header+0x10,4);std::memcpy(&table,header+0x20,8);
        std::memcpy(&metadata,header+0x70,8);
        if(!count || count>TglOwnedAvcSliceInputTable::MaxSlices)return {};
        uint32_t qp=0;std::memcpy(&qp,owned->input_.data()+0x438,4);
        if(qp>51)return {};
        for(size_t i=0;i<16;++i) {
            uint64_t handle=0;uint32_t index=0;
            std::memcpy(&handle,owned->input_.data()+0x38+0x40*i,8);
            std::memcpy(&index,owned->input_.data()+0x48+0x40*i,4);
            if(handle && index>=16)return {};
        }
        owned->sequence_=TglOwnedAvcConsumedSequenceInput::capture(sequence,width,height,read);
        if(!owned->sequence_)return {};
        owned->picture_=TglOwnedAvcPictureInput::capture(picture,defaults,read);
        if(!owned->picture_)return {};
        owned->sliceNal_=SliceNal::captureRoots(count,table,metadata,read);
        if(!owned->sliceNal_)return {};
        // Validate the bytes native will actually see, not an earlier
        // borrowed preflight.2737e's uint32 increment/wrap is intentional.
        const auto* first=reinterpret_cast<const uint8_t*>(owned->sliceNal_->sliceTable()[0]);
        uint32_t fieldPicture=0,overridden=0,minusOne[2]{};
        std::memcpy(&fieldPicture,first+0x14,4);
        // Exact2737e's field-picture branch calls906b, an eight-byte
        // return-zero stub with no list/count outputs. Do not treat it as
        // an implemented counterpart of8b4f or fabricate initialized lists.
        if(fieldPicture)return {};
        //9073 has no local record bound: opcode3 is its only normal loop
        // terminator. Qualify at most32 triples per list in this owned path;
        // this is an admission budget, not full reorder operand semantics.
        constexpr size_t flags[]{0x48,0x1cc},records[]{0x4c,0x1d0};
        for(size_t list=0;list<2;++list) {
            uint32_t active=0;std::memcpy(&active,first+flags[list],4);
            if(!active)continue;
            bool terminated=false;
            for(size_t i=0;i<32;++i) {
                uint32_t operation=0;std::memcpy(&operation,first+records[list]+12*i,4);
                if(operation==3){terminated=true;break;}
                if(operation>3)return {};
            }
            if(!terminated)return {};
        }
        std::memcpy(&overridden,first+0x38,4);
        std::memcpy(minusOne,overridden?first+0x3c:owned->picture_->data()+0x90,sizeof(minusOne));
        // Frame-list source records occupy8c0..9bf and9c0..abf,
        // sixteen16-byte entries each;ac0/ac4 are COUNTS, not entries.
        //25cf6 reads one record per active count despite its32-byte output.
        // Field-list expansion is not implemented/admitted above.
        if(minusOne[0]+uint32_t(1)>16 || minusOne[1]+uint32_t(1)>16)return {};
        return owned;
    }
    const uint8_t* sequenceBytes() const noexcept {return sequence_->data();}
    const uint8_t* pictureBytes() const noexcept {return picture_->data();}
    size_t sliceCount() const noexcept {return sliceNal_->sliceCount();}
    const uintptr_t* sliceTable() const noexcept {return sliceNal_->sliceTable();}
    const uint8_t* nalMetadata() const noexcept {return sliceNal_->nalMetadata();}
    // Handle identities come from the SAME private root capture as syntax,
    // not a second borrowed payload/table read. This does not authorize the
    // native context's derived indices or expose a complete native payload.
    template<class Api,class Admit>
    std::unique_ptr<OwnedAvcSurfaceReferences<Api>> pinSurfaces(Api api,Admit admit) const noexcept {
        if(!admit())return {};
        std::array<uintptr_t,35> handles{};
        std::memcpy(&handles[0],input_.data()+8,8);
        std::memcpy(&handles[1],input_.data()+0x478,8);
        for(size_t i=0;i<17;++i)handles[2+i]=downscale_[i];
        for(size_t i=0;i<16;++i)std::memcpy(&handles[19+i],input_.data()+0x38+i*0x40,8);
        auto result=OwnedAvcSurfaceReferences<Api>::capture(handles,api,admit);
        if(!result || !admit())return {};
        return result;
    }
private:
    OwnedAvcSyntaxInput()=default;
    // Input span through metadata480, excluding outer output pointer488 and
    // outputs498..4a0. Contains BORROWED handle identities/table440: never
    // expose it as a native payload until a qualified lease/rebind exists.
    std::array<uint8_t,0x488> input_{};
    std::array<uintptr_t,17> downscale_{};
    std::unique_ptr<TglOwnedAvcConsumedSequenceInput> sequence_;
    std::unique_ptr<TglOwnedAvcPictureInput> picture_;
    std::unique_ptr<SliceNal> sliceNal_;
};
struct Builder {
    uintptr_t argument=0,connection=0,base=0,cursor=0,end=0;
    uint32_t previousUsed=0,classes=0;
};
template<class Read,class Executable>
bool abi(uintptr_t image,Read read,Executable executable) noexcept {
    if(!image || image>UINTPTR_MAX-0x23e115)return false;
    // Complete used-byte arithmetic, class walk and both native import calls.
    constexpr unsigned char bytes[]{
        0x49,0x8d,0x5e,0x44,0x45,0x31,0xff,
        0x8b,0x53,0xdc,0x2b,0x53,0xcc,0x03,0x13,0x89,0x13,
        0x49,0x8b,0x7e,0x08,0x44,0x89,0xfe,0xe8,0x3f,0x38,0x23,0x00,
        0x48,0xc7,0x43,0xf4,0x00,0x00,0x00,0x00,
        0x49,0xff,0xc7,0x41,0x8b,0x46,0x58,0x48,0x83,0xc3,0x40,
        0x49,0x39,0xc7,0x72,0xd2,
        0x49,0x8b,0x7e,0x08,0x41,0x8b,0x76,0x50,0x49,0x8d,0x5e,0x54,
        0x48,0x8d,0x4d,0xd4,0x48,0x89,0xda,0xe8,0x15,0x38,0x23,0x00};
    unsigned char actual[sizeof(bytes)]{};
    return executable(image+0xa8ae,sizeof(bytes)) &&
        read(image+0xa8ae,actual,sizeof(actual)) && !std::memcmp(actual,bytes,sizeof(bytes));
}
template<class Read>
bool capture(uintptr_t argument,Read read,Builder& output) noexcept {
    output={};
    if(!argument || (argument&7U) || argument>UINTPTR_MAX-0x5c)return false;
    unsigned char bytes[0x5c]{};
    if(!read(argument,bytes,sizeof(bytes)))return false;
    Builder candidate;candidate.argument=argument;
    static_assert(sizeof(uintptr_t)==8,"native Tahoe x86_64 producer ABI");
    std::memcpy(&candidate.connection,bytes+0x08,8);
    std::memcpy(&candidate.base,bytes+0x10,8);
    std::memcpy(&candidate.end,bytes+0x18,8);
    std::memcpy(&candidate.cursor,bytes+0x20,8);
    std::memcpy(&candidate.previousUsed,bytes+0x44,4);
    std::memcpy(&candidate.classes,bytes+0x58,4);
    // Only the captured single-class AVC producer is reviewed. Do not infer
    // an arbitrary multi-class layout from the first class's cursor fields.
    if(!candidate.connection || candidate.classes!=1 || !candidate.base ||
       ((candidate.base|candidate.end|candidate.cursor|candidate.previousUsed)&3U) ||
       candidate.cursor<candidate.base || candidate.end<candidate.cursor ||
       candidate.cursor-candidate.base>6144)return false;
    output=candidate;return true;
}
inline bool unchanged(const Builder& a,const Builder& b) noexcept {
    return a.argument==b.argument && a.connection==b.connection && a.base==b.base &&
        a.cursor==b.cursor && a.end==b.end && a.previousUsed==b.previousUsed && a.classes==b.classes;
}
inline NgreenAvcGen12::Reservation plan(const Builder& builder,size_t outputWords) noexcept {
    if(!builder.argument || !builder.connection || builder.classes!=1)return {};
    return NgreenAvcGen12::reserve(builder.base,builder.cursor,builder.end,builder.previousUsed,
        (builder.cursor-builder.base)/4,outputWords);
}
struct CommitResult {
    bool applied=false,mustStop=false;
    size_t extraBytes=0;
    uint32_t usedBytes=0;
    const char* reason="admission";
};
// Owns no mapping or lock. The caller's SAME native builder interval and
// physical/context lease must surround every callback through native submit.
// Only unused tail and the aligned producer cursor are written; neither the
// original commands nor native sideband offsets/counts are translated here.
template<class Read,class Write,class Writable,class Live>
CommitResult commit(uintptr_t argument,const NGVfAvcPublication::Reply& capability,uint64_t registryId,
    uint32_t width,uint32_t height,NgreenAvcGen12::PublishScratch& scratch,
    Read read,Write write,Writable writable,Live live) noexcept {
    CommitResult result;Builder before,again;
    if(!NGVfAvcPublication::ready(capability,registryId) || !live() || !capture(argument,read,before))return result;
    const uintptr_t work=reinterpret_cast<uintptr_t>(&scratch);
    if(work>UINTPTR_MAX-sizeof(scratch) ||
       (before.base<argument+0x5c && argument<before.end) ||
       (before.base<work+sizeof(scratch) && work<before.end) ||
       (argument<work+sizeof(scratch) && work<argument+0x5c)) {
        result.reason="storage-alias";return result;
    }
    const size_t inputWords=(before.cursor-before.base)/4;
    if(inputWords<2 || inputWords>1024 || !read(before.base,scratch.source,inputWords*4))return result;
    const auto measured=NgreenAvcGen12::sizeBeforeRelocation(scratch.source,inputWords,width,height);
    if(!measured.bounded){result.reason=measured.reason;return result;}
    const auto reservation=plan(before,measured.outputWords);
    if(!reservation.ok || !writable(before.cursor,reservation.extraBytes) ||
       !writable(argument+0x20,8) || !live())return result;
    if(!capture(argument,read,again) || !unchanged(before,again) ||
       !read(before.base,scratch.undo,inputWords*4) ||
       std::memcmp(scratch.source,scratch.undo,inputWords*4))return result;
    // No pre-relocation command buffer was produced. Keep the original input
    // frozen; use the other arrays only for tail undo/zeros.
    if(!read(before.cursor,scratch.undo,reservation.extraBytes))return result;
    std::memset(scratch.produced,0,reservation.extraBytes);
    if(!live() || !capture(argument,read,again) || !unchanged(before,again))return result;
    auto restore=[&]() noexcept {
        Builder current;
        if(!live() || !capture(argument,read,current))return false;
        Builder original=current;original.cursor=before.cursor;
        if(!unchanged(before,original) ||
           (current.cursor!=before.cursor && current.cursor!=reservation.cursor))return false;
        if(current.cursor!=before.cursor) {
            if(!write(argument+0x20,&before.cursor,8) ||
               !capture(argument,read,current) || !unchanged(before,current))return false;
        }
        if(!read(before.cursor,scratch.produced,reservation.extraBytes))return false;
        if(std::memcmp(scratch.undo,scratch.produced,reservation.extraBytes) &&
           !write(before.cursor,scratch.undo,reservation.extraBytes))return false;
        if(!read(before.cursor,scratch.produced,reservation.extraBytes) ||
           std::memcmp(scratch.undo,scratch.produced,reservation.extraBytes))return false;
        return live() && capture(argument,read,current) && unchanged(before,current);
    };
    result.reason="tail-write";
    bool ok=write(before.cursor,scratch.produced,reservation.extraBytes);
    if(ok) {
        // Verify the zeroed tail before publishing a larger native used span.
        ok=read(before.cursor,scratch.produced,reservation.extraBytes);
        for(size_t i=0;ok && i<reservation.extraBytes/4;++i)ok=scratch.produced[i]==0;
    }
    if(ok)ok=live() && capture(argument,read,again) && unchanged(before,again);
    if(ok) {
        result.reason="cursor-write";
        ok=write(argument+0x20,&reservation.cursor,8);
    }
    Builder advanced=before;advanced.cursor=reservation.cursor;
    if(ok)ok=live() && capture(argument,read,again) && unchanged(advanced,again);
    if(!ok){result.mustStop=!restore();return result;}
    result.applied=true;result.extraBytes=reservation.extraBytes;
    result.usedBytes=reservation.usedBytes;result.reason="reserved";return result;
}
}
