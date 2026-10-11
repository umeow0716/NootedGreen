#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include "tgl_capability_adapter.hpp"
#include "descriptor_bridge.hpp"
#include "owned_resource.hpp"

// Mock only the enclosing synchronous child lease. Packet callbacks and
// setup/builder argument binding below use their real owned implementations.
struct TestVeboxChildLease {
    void* child;
    bool busy=false;
    bool hasPrivateChildLease(uintptr_t p) const noexcept {
        return busy && reinterpret_cast<uintptr_t>(child)==p;
    }
    template<class Invoke> int withPrivateChild(Invoke invoke) noexcept {
        if(busy || !child) return 5;
        busy=true;const int result=invoke(child);busy=false;return result;
    }
};
struct TestVeboxExecutionLease {
    template<class Invoke> int withExecution(Invoke invoke) noexcept {return invoke(7);}
};
struct TestVpCpuView {
    std::array<uint8_t,0x9d0> bytes{};
    uintptr_t overrideAddress=0;
    bool busy=false,deny=false;
    template<class Use> int withView(bool,Use use) noexcept {
        if(busy || deny) return 5;
        busy=true;const int result=use(overrideAddress ? reinterpret_cast<void*>(overrideAddress) : bytes.data());
        busy=false;return result;
    }
};

struct TestFeatureSurfaceOwner {
    TglOwnedSurfaceStorage storage{};bool busy=false,deny=false;
    bool hasSurfaceLease(const TglOwnedSurfaceStorage& expected) noexcept {
        return busy && !deny && &expected==&storage;
    }
    template<class Use> bool withPeerSurface(TestFeatureSurfaceOwner& peer,Use use) noexcept {
        if(busy || deny || peer.deny || (&peer!=this && peer.busy)) return false;
        busy=true;peer.busy=true;use(storage,peer.storage);peer.busy=false;busy=false;return true;
    }
};
struct TestFeatureExecutionOwner {
    std::array<uint8_t,0xd48> bytes{};bool busy=false;
    struct Backend {
        std::array<uint8_t,0xd48>& bytes;
        uintptr_t create() noexcept {assert(false);return 0;}
        int initialize(uintptr_t) noexcept {assert(false);return 5;}
        bool validate(uintptr_t p) noexcept {return p==reinterpret_cast<uintptr_t>(bytes.data());}
        void destroy(uintptr_t) noexcept {assert(false);}
    } backend{bytes};
    TglExecutionOwner<Backend> owner{backend};
    bool hasExecutionLease(uintptr_t expected) const noexcept {return owner.hasExecutionLease(expected);}
    template<class Use> int withLeasedFeatureReport(uintptr_t expected,const void* target,Use use) noexcept {
        return owner.withLeasedFeatureReport(expected,target,use);
    }
    template<class Use> int withExecution(Use use) noexcept {
        if(busy) return 5;
        busy=true;
        const int status=owner.withBorrowedExecution(reinterpret_cast<uintptr_t>(bytes.data()),use);
        busy=false;return status;
    }
};
static void testTrackedCpuOwner() {
    {
        struct Backend {
            std::array<uint8_t,0xd48> bytes{};bool valid=true;int status=0;
            unsigned creates=0,destroys=0,initializes=0;
            uintptr_t pointer() noexcept {return reinterpret_cast<uintptr_t>(bytes.data());}
            uintptr_t create() noexcept {++creates;return pointer();}
            bool validate(uintptr_t p) noexcept {return valid && p==pointer();}
            int initialize(uintptr_t p) noexcept {assert(p==pointer());++initializes;return status;}
            void destroy(uintptr_t p) noexcept {assert(p==pointer());++destroys;}
        } backend;
        TglExecutionOwner execution(backend);
        const auto report=backend.pointer();unsigned calls=0;
        assert(execution.withBorrowedExecution(report,[&](uintptr_t actual) noexcept {
            ++calls;assert(actual==report && execution.hasExecutionLease(report));
            execution.reset();assert(execution.get()==report && !backend.destroys);
            assert(execution.ensure()==5 && !backend.creates);
            assert(execution.withBorrowedExecution(report,[](uintptr_t) noexcept {assert(false);return 0;})==5);
            return 31;
        })==31);
        assert(calls==1 && !execution.get() && !backend.creates && !backend.destroys);
        backend.valid=false;
        assert(execution.withBorrowedExecution(report,[](uintptr_t) noexcept {assert(false);return 0;})==5);
        assert(!execution.get() && !backend.destroys);
        backend.valid=true;
        assert(execution.withBorrowedExecution(report,[](uintptr_t) noexcept {assert(false);return 0;})==5);
        execution.reset();backend.status=31;
        assert(execution.withBorrowedExecution(report,[&](uintptr_t actual) noexcept {
            return execution.withLeasedAdmissionBytes(actual,[&](auto&) noexcept {
                assert(execution.initializeLeasedAdmission(actual)==31);
                return 0; // native reset error cannot be hidden
            });
        })==31);
        assert(!execution.get() && backend.initializes==1 && !backend.destroys);
        assert(execution.withBorrowedExecution(report,[](uintptr_t) noexcept {assert(false);return 0;})==5);
        execution.reset();
        backend.status=0;
        assert(execution.ensure()==0 && backend.creates==1);
        assert(execution.withBorrowedExecution(report,[](uintptr_t) noexcept {assert(false);return 0;})==5);
        execution.reset();assert(backend.destroys==1);
    }
    {
        std::array<uint8_t,0x139> params{};
        TglVeboxPipeAcquisition frame;bool present=true;
        assert(tglAcquireOwnedColorFillPresence(params,0,frame,present) && !present);
        uintptr_t owned=0x1234;
        std::memcpy(params.data()+0xe0,&owned,sizeof(owned));
        assert(tglAcquireOwnedColorFillPresence(params,owned,frame,present) && present);
        present=false;
        assert(!tglAcquireOwnedColorFillPresence(params,owned+1,frame,present) && !present && !frame.healthy());
        assert(!tglAcquireOwnedColorFillPresence(params,owned,frame,present) && !present);
    }
    {
        struct Policy {
            TglVeboxPipeAcquisition frame;
            TglSurfaceRect source{0,0,1920,1081},target{0,0,1920,1200};
            bool fill=false,compressed=true;uint32_t format=7;
            TglOwnedSfcCompressionMode mode=TglOwnedSfcCompressionMode::Media;
            int scenario=0,status=99;unsigned directCalls=0,decompressions=0;
            bool healthy() const noexcept {return frame.healthy();}
            bool bypass() noexcept {return true;}
            bool rejectDi() noexcept {return false;}
            bool rejectLut() noexcept {return false;}
            bool direct() noexcept {
                ++directCalls;
                if(directCalls==1) {assert(target.bottom==1200);return false;}
                assert(target.bottom==source.bottom);
                if(scenario==3) frame.fail();
                return scenario!=2;
            }
            bool twoPass() noexcept {return false;}
            bool sfcAvailable() noexcept {return false;}
            bool hdr() noexcept {assert(false);return false;}
            uint32_t sfc() noexcept {assert(false);return 0;}
            bool fastFill() noexcept {
                return tglRunOwnedGen12FastFill(source,target,true,fill,compressed,mode,format,status,
                    [&]() noexcept {return direct();},[&]() noexcept {
                        ++decompressions;assert(fill && target.bottom==1081);
                        if(scenario==4 || scenario==5) frame.fail();
                        return scenario==1 || scenario==5?31:0;
                    },[&]() noexcept {return healthy();});
            }
        };
        for(int scenario=0;scenario<6;++scenario) {
            Policy p;p.scenario=scenario;uint8_t comp=0xa5;uint32_t output=77;
            const bool ok=tglSelectGen12VeboxOutputPipe(p,comp,output);
            assert(p.target.bottom==1200 && p.directCalls==2);
            if(scenario<2) {
                assert(ok && output==2 && comp==0 && p.fill && p.decompressions==1);
                assert(p.status==(scenario==1?31:0));
                assert(p.compressed==(scenario==1) && p.format==(scenario==1?7u:0u));
            } else if(scenario==2) {
                assert(ok && output==0 && comp==1 && !p.fill && p.decompressions==0 && p.status==99);
            } else {
                assert(!ok && output==77 && comp==0xa5 && !p.fill && p.status==(scenario==4?0:scenario==5?31:99));
                assert(p.decompressions==(scenario>=4?1u:0u));
                if(scenario==4) {
                    assert(!p.compressed && p.format==0 && p.mode==TglOwnedSfcCompressionMode::Disabled);
                } else assert(p.compressed && p.format==7 && p.mode==TglOwnedSfcCompressionMode::Media);
                assert(!p.healthy());
            }
        }
    }
    {
        for(int scenario=0;scenario<8;++scenario) {
            TglSurfaceRect source{0,0,1920,1081},target{0,0,1920,1200};
            bool fill=false,compressed=true,healthy=true;uint32_t format=7;
            auto mode=TglOwnedSfcCompressionMode::Media;
            int status=99;unsigned checks=0,decompressions=0;
            if(scenario==2) source.bottom=1080;
            if(scenario==3) mode=TglOwnedSfcCompressionMode::Render;
            if(scenario==4) source.bottom=1080.5f;
            if(scenario==5) target.left=1;
            auto recheck=[&]() noexcept {
                ++checks;assert(target.bottom==source.bottom && !fill);
                if(scenario==7) healthy=false;
                return scenario!=6;
            };
            auto decompress=[&]() noexcept {
                ++decompressions;assert(fill && target.bottom==source.bottom);
                return scenario==1?31:0;
            };
            const bool selected=tglRunOwnedGen12FastFill(source,target,true,fill,compressed,mode,
                format,status,recheck,decompress,[&]() noexcept {return healthy;});
            assert(target.bottom==1200);
            if(scenario>=4) {
                assert(!selected && !fill && compressed && format==7 && status==99 && !decompressions);
                assert(checks==(scenario>=6?1u:0u));
            } else {
                assert(selected && fill && checks==1);
                if(scenario==0) assert(!compressed && mode==TglOwnedSfcCompressionMode::Disabled && format==0 && status==0 && decompressions==1);
                if(scenario==1) assert(compressed && mode==TglOwnedSfcCompressionMode::Media && format==7 && status==31 && decompressions==1);
                if(scenario==2 || scenario==3) assert(compressed && format==7 && status==0 && decompressions==0);
            }
        }
    }
    {
        // Exercise the real DI acquisition through the whole selector, not
        // an invented failure callback. No later predicate may run on error.
        struct Policy {
            TglVeboxPipeAcquisition frame;
            TglOwnedSurfaceStorage source{};
            uintptr_t leased=0x1000;uint32_t mode=1;bool allow=true;
            unsigned reads=0,later=0;
            bool healthy() const noexcept {return frame.healthy();}
            bool bypass() noexcept {return true;}
            bool rejectDi() noexcept {
                bool rejected=false;
                auto read=[&](uintptr_t address,void* out,size_t n) noexcept {
                    ++reads;assert(address==leased && n==4);
                    if(!allow) return false;
                    std::memcpy(out,&mode,4);return true;
                };
                const bool acquired=tglRejectLeasedGen12DiOutput(rejected,source,leased,frame,read);
                assert(acquired==frame.healthy());return rejected;
            }
            bool rejectLut() noexcept {++later;return false;}
            bool direct() noexcept {++later;return true;}
            bool twoPass() noexcept {assert(false);return false;}
            bool sfcAvailable() noexcept {assert(false);return false;}
            bool hdr() noexcept {assert(false);return false;}
            uint32_t sfc() noexcept {assert(false);return 0;}
            bool fastFill() noexcept {assert(false);return false;}
        };
        for(unsigned scenario=0;scenario<7;++scenario) {
            Policy p;uint32_t height=1081,format=0x19;
            if(scenario==0) p.mode=2; // ADI: no BOB rejection
            if(scenario==2) p.allow=false;
            if(scenario==3) p.mode=0; // upstream BOB=0 is not Darwin BOB
            if(scenario==4) p.mode=0x10001;
            if(scenario==5) p.leased=UINTPTR_MAX-2;
            std::memcpy(p.source.prefix.data()+0x88,&p.leased,sizeof(p.leased));
            if(scenario==6) p.leased=0x2000; // foreign source pointer
            std::memcpy(p.source.prefix.data()+0xdc,&height,4);
            std::memcpy(p.source.prefix.data()+0x130,&format,4);
            uint8_t comp=0xa5;uint32_t output=0xdeadbeef;
            const bool ok=tglSelectGen12VeboxOutputPipe(p,comp,output);
            if(scenario==0) assert(ok && output==2 && comp==0 && p.later==2);
            else if(scenario==1) assert(ok && output==0 && comp==1 && p.later==0);
            else {
                assert(!ok && output==0xdeadbeef && comp==0xa5 && p.later==0 && !p.healthy());
                const unsigned reads=p.reads;
                assert(!tglSelectGen12VeboxOutputPipe(p,comp,output) && p.reads==reads);
                assert(output==0xdeadbeef && comp==0xa5);
            }
        }
    }
    {
        TglOwnedSurfaceStorage source{};
        uint32_t mode=1,height=1081,format=0x19;unsigned reads=0;bool allow=true;
        uintptr_t params=0x1000;
        std::memcpy(source.prefix.data()+0xdc,&height,4);
        std::memcpy(source.prefix.data()+0x130,&format,4);
        auto read=[&](uintptr_t p,void* out,size_t n) noexcept {
            ++reads;assert(p==params && n==4);
            if(!allow) return false;
            std::memcpy(out,&mode,4);return true;
        };
        bool rejected=true;
        TglVeboxPipeAcquisition frame,mismatch;
        assert(tglRejectLeasedGen12DiOutput(rejected,source,0,frame,read) && !rejected && reads==0);
        assert(!tglRejectLeasedGen12DiOutput(rejected,source,params,mismatch,read) && reads==0 && !mismatch.healthy());
        std::memcpy(source.prefix.data()+0x88,&params,sizeof(params));
        assert(tglRejectLeasedGen12DiOutput(rejected,source,params,frame,read) && rejected && reads==1);
        mode=2;
        assert(tglRejectLeasedGen12DiOutput(rejected,source,params,frame,read) && !rejected);
        for(uint32_t invalid : {0u,3u,0x10001u,0xffffffffu}) {
            mode=invalid;rejected=true;
            TglVeboxPipeAcquisition invalidFrame;
            assert(!tglRejectLeasedGen12DiOutput(rejected,source,params,invalidFrame,read) && rejected && !invalidFrame.healthy());
            mode=1;const unsigned failedReads=reads;
            assert(!tglRejectLeasedGen12DiOutput(rejected,source,params,invalidFrame,read) && reads==failedReads);
        }
        mode=1;allow=false;rejected=false;
        assert(!tglRejectLeasedGen12DiOutput(rejected,source,params,frame,read) && !rejected && !frame.healthy());
        const unsigned before=reads;params=UINTPTR_MAX-2;
        std::memcpy(source.prefix.data()+0x88,&params,sizeof(params));
        TglVeboxPipeAcquisition overflow;
        assert(!tglRejectLeasedGen12DiOutput(rejected,source,params,overflow,read) && reads==before && !overflow.healthy());
    }
    {
        for(bool hasDi : {false,true}) for(bool bob : {false,true})
            for(uint32_t height : {0u,1u,2u,3u,4u,1080u,1081u,0xffffffffu})
                for(uint32_t format : {0x19u,0x52u,0x53u,0x15u,0x0eu,0x10019u}) {
                    const bool planar=format==0x19 || format==0x52 || format==0x53;
                    const bool unsupported=format==0x15;
                    assert(tglRejectGen12BobOutput(hasDi,bob,height,format)==
                        (hasDi && bob && ((planar && height%4!=0)||unsupported)));
                }
        for(bool lut : {false,true}) for(uint32_t format : {0x52u,0x53u,0x19u,0x10052u,0xffffffffu})
            assert(tglRejectGen12LutOutput(lut,format)==(lut && (format==0x52 || format==0x53)));
    }
    {
        unsigned reads=0;bool allow=true;uint8_t liveFlag=0x80;
        auto read=[&](uintptr_t p,void* out,size_t n) noexcept {
            ++reads;if(!allow) return false;
            if((p==0x1000 || p==0x1090) && n==4) {uint32_t one=1;std::memcpy(out,&one,4);return true;}
            if(((p==0x2060 || p==0x2068 || p==0x10d8) && n==8) ||
               ((p==0x2081 || p==0x2082) && n==1)) {std::memset(out,0,n);return true;}
            assert(false);return false; // no read of opaque pass or flag address
        };
        TglVeboxPipeAcquisition frame;TglVeboxPipeReadGate gate(read,frame);
        assert(tglCompositionBypassFromLiveFlag(0x1000,0x2000,liveFlag,gate) && reads==7);
        liveFlag=0x81;
        assert(!tglCompositionBypassFromLiveFlag(0x1000,0x2000,liveFlag,gate) && reads==7 && frame.healthy());
        liveFlag=0x80;allow=false;
        assert(!tglCompositionBypassFromLiveFlag(0x1000,0x2000,liveFlag,gate) && reads==8 && !frame.healthy());
        allow=true;
        assert(!tglCompositionBypassFromLiveFlag(0x1000,0x2000,liveFlag,gate) && reads==8);
    }
    {
        struct Policy {
            unsigned calls=0;unsigned stop=99;uint32_t selected=1;bool fill=false,available=true,tone=false;
            unsigned failAt=0;
            bool healthy() const noexcept {return !(calls&failAt);}
            bool bypass() noexcept {calls|=1;return stop!=0;}
            bool rejectDi() noexcept {calls|=2;return stop==1;}
            bool rejectLut() noexcept {calls|=4;return stop==2;}
            bool direct() noexcept {calls|=8;return stop==3;}
            bool twoPass() noexcept {calls|=16;return stop==4;}
            bool sfcAvailable() noexcept {calls|=32;return available;}
            bool hdr() noexcept {calls|=64;return tone;}
            uint32_t sfc() noexcept {calls|=128;return selected;}
            bool fastFill() noexcept {calls|=256;return fill;}
        } policy;
        uint8_t comp=0x80;uint32_t mode=0xabcdef;
        for(unsigned stop=0;stop<5;++stop) {
            policy.calls=0;policy.stop=stop;comp=0x80;
            assert(tglSelectGen12VeboxOutputPipe(policy,comp,mode));
            assert(policy.calls==((1u<<(stop+1))-1) && mode==(stop==3?2u:0u));
            assert(comp==(stop==3?0:1));
        }
        policy.stop=99;policy.calls=0;
        assert(tglSelectGen12VeboxOutputPipe(policy,comp,mode) && mode==1 && !comp && policy.calls==255);
        policy.selected=0;policy.fill=true;policy.calls=0;
        assert(tglSelectGen12VeboxOutputPipe(policy,comp,mode) && mode==2 && !comp && policy.calls==511);
        policy.tone=true;policy.calls=0;
        assert(tglSelectGen12VeboxOutputPipe(policy,comp,mode) && mode==2 && !(policy.calls&128));
        policy.tone=false;policy.selected=3;policy.calls=0;mode=0xabcdef;comp=0x80;
        assert(!tglSelectGen12VeboxOutputPipe(policy,comp,mode) && mode==0xabcdef && comp==0x80 && !(policy.calls&256));
        TglGen12VeboxPipeSelector selector(policy);
        assert(selector(comp)==UINT32_MAX && comp==0x80);
        TglVeboxAdmissionStagesInput input;
        input.prefix={true,true,true,true,1,uint64_t{1}<<12,64,16};
        std::array<uint8_t,0xd48> execution{};TglVeboxAdmissionPassByte pass(comp);
        unsigned resets=0,setups=0,queries=0,features=0;
        auto reset=[&]() noexcept {++resets;execution.fill(0);return true;};
        auto setup=[&](auto&) noexcept {++setups;};
        auto query=[&]() noexcept {++queries;return true;};
        auto feature=[&](auto&) noexcept {++features;};
        auto sfc=[](auto&) noexcept {assert(false);return false;};
        assert(!tglRunSelectedVeboxAdmissionStages(input,execution,pass,reset,selector,setup,query,feature,sfc));
        assert(resets==1 && !setups && !queries && !features && comp==0x80);
        policy.stop=3;
        assert(tglRunSelectedVeboxAdmissionStages(input,execution,pass,reset,selector,setup,query,feature,sfc));
        assert(resets==2 && setups==1 && queries==1 && features==1 && comp==0);
        policy.stop=0;
        assert(!tglRunSelectedVeboxAdmissionStages(input,execution,pass,reset,selector,setup,query,feature,sfc));
        assert(resets==3 && comp==1); // successful composition selection still denies VEBOX admission
        for(unsigned fault : {1u,2u,4u,8u,16u,32u,64u,128u,256u}) {
            policy.stop=99;policy.selected=0;policy.fill=true;policy.calls=0;policy.failAt=fault;
            mode=0xabcdef;comp=0x80;
            assert(!tglSelectGen12VeboxOutputPipe(policy,comp,mode));
            assert(mode==0xabcdef && comp==0x80 && policy.calls==(fault*2-1));
        }
    }
    {
        unsigned reads=0;TglVeboxPipeAcquisition acquisition;
        auto read=[&](uintptr_t address,void* out,size_t bytes) noexcept {
            ++reads;if(address!=0x100 || bytes!=1) return false;
            *static_cast<uint8_t*>(out)=0x42;return true;
        };
        TglVeboxPipeReadGate gate(read,acquisition);auto copied=gate;uint8_t value=0;
        assert(copied(0x100,&value,1) && value==0x42 && acquisition.healthy() && reads==1);
        assert(!copied(0x200,&value,1) && !acquisition.healthy() && reads==2);
        assert(!gate(0x100,&value,1) && reads==2);
        TglVeboxPipeAcquisition overflowFrame;
        TglVeboxPipeReadGate overflowGate(read,overflowFrame);
        assert(!overflowGate(UINTPTR_MAX,&value,2) && !overflowFrame.healthy() && reads==2);
        TglVeboxPipeAcquisition predicateFrame;
        TglVeboxPipeReadGate predicateGate(read,predicateFrame);
        uint8_t comp=0x80;uint32_t mode=0xabcdef;
        struct Policy {
            TglVeboxPipeAcquisition& frame;decltype(predicateGate)& gate;
            uint8_t& comp;
            bool healthy() const noexcept {return frame.healthy();}
            bool bypass() noexcept {return tglCompositionBypassFromLiveFlag(0x200,0x300,comp,gate);}
            bool rejectDi() noexcept {assert(false);return false;}
            bool rejectLut() noexcept {assert(false);return false;}
            bool direct() noexcept {assert(false);return false;}
            bool twoPass() noexcept {assert(false);return false;}
            bool sfcAvailable() noexcept {assert(false);return false;}
            bool hdr() noexcept {assert(false);return false;}
            uint32_t sfc() noexcept {assert(false);return 1;}
            bool fastFill() noexcept {assert(false);return false;}
        } policy{predicateFrame,predicateGate,comp};
        assert(!tglSelectGen12VeboxOutputPipe(policy,comp,mode) && reads==3);
        assert(!predicateFrame.healthy() && comp==0x80 && mode==0xabcdef);
        assert(!tglSelectGen12VeboxOutputPipe(policy,comp,mode) && reads==3);
    }
    {
        TglVeboxAdmissionStagesInput input;
        input.prefix={true,true,true,true,1,uint64_t{1}<<12,64,16};
        input.pipe=0;std::array<uint8_t,0xd48> execution{};
        uint8_t nativeFlag=0x80;TglVeboxAdmissionPassByte pass(nativeFlag);
        unsigned order=0;
        auto reset=[&]() noexcept {assert(order++==0);execution.fill(0);return true;};
        auto select=[&](uint8_t& flag) noexcept -> uint32_t {
            assert(order++==1 && &flag==&nativeFlag && flag==0x80);flag=0x82;return 2;
        };
        auto setup=[&](auto& bytes) noexcept {
            uint32_t mode=0;std::memcpy(&mode,bytes.data()+0xa0c,4);
            assert(order++==2 && mode==2 && nativeFlag==0x82);
        };
        auto query=[&]() noexcept {assert(order++==3);return true;};
        auto feature=[&](auto&) noexcept {assert(order++==4);};
        auto sfc=[](auto&) noexcept {assert(false);return false;};
        assert(tglRunSelectedVeboxAdmissionStages(input,execution,pass,reset,select,setup,query,feature,sfc));
        assert(order==5 && nativeFlag==0x82);
        order=0;nativeFlag=0x80;
        auto invalid=[&](uint8_t& flag) noexcept -> uint32_t {
            assert(order++==1);flag=0x84;return 3;
        };
        assert(!tglRunSelectedVeboxAdmissionStages(input,execution,pass,reset,invalid,setup,query,feature,sfc));
        assert(order==2 && nativeFlag==0x84);
    }
    {
        TglVeboxFeatureBinding binding;constexpr uintptr_t image=0x10000000;
        bool exact=true,qualified=true,readable=true;uintptr_t slot=image+0x12aba0;
        auto read=[&](uintptr_t p,void* out,size_t n) noexcept {
            if(!readable) return false;
            if(p==image+0x12aba0 && n==16) {
                std::array<uint8_t,16> bytes{0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x50,
                    0x48,0x89,0x7d,0xf8,0x48,0x89,0x75,0xf0};
                if(!exact) bytes[7]^=1;
                std::memcpy(out,bytes.data(),16);return true;
            }
            if(p==image+0x757e08 && n==8) {std::memcpy(out,&slot,8);return true;}
            assert(false);return false;
        };
        auto qualify=[&](uintptr_t p) noexcept {return qualified && p==image;};
        auto executable=[](uintptr_t p,size_t n) noexcept {return p==image+0x12aba0 && n==16;};
        assert(binding.resolve(image,read,qualify,executable) && binding.entry==slot);
        exact=false;assert(!binding.resolve(image,read,qualify,executable) && !binding.entry);exact=true;
        ++slot;assert(!binding.resolve(image,read,qualify,executable) && !binding.image);--slot;
        qualified=false;assert(!binding.resolve(image,read,qualify,executable));qualified=true;
        readable=false;assert(!binding.resolve(image,read,qualify,executable));
        assert(!binding.resolve(UINTPTR_MAX,read,qualify,executable));
        TglOwnedSurfaceStorage source{},target{};
        struct Call {
            const void* source;const void* target;unsigned calls=0;
            static void prepare(void* self,const void* s,const void* t) noexcept {
                auto& call=*static_cast<Call*>(self);
                assert(s==call.source && t==call.target);++call.calls;
            }
        } call{&source,&target};
        TglNativeVeboxFeaturePreparationInvoker{}(
            reinterpret_cast<uintptr_t>(&Call::prepare),&call,source,target);
        assert(call.calls==1);
    }
    {
        std::array<uint8_t,0xd48> execution{};std::array<uint8_t,8> dn{};
        TglOwnedSurfaceStorage source{};uint32_t pipe=1,format=0x19;
        std::memcpy(execution.data()+0xa0c,&pipe,4);std::memcpy(source.prefix.data()+0x130,&format,4);
        uintptr_t p=reinterpret_cast<uintptr_t>(dn.data());std::memcpy(source.prefix.data()+0x70,&p,8);
        dn[0]=3;assert(tglPublishOwnedVeboxSfcAdmissionFlags(execution,source,dn));
        assert(execution[0x11]==1 && execution[0x0f]==1);
        execution[0x0f]=0;dn[0]=2;
        assert(tglPublishOwnedVeboxSfcAdmissionFlags(execution,source,dn) && !execution[0x11] && !execution[0x0f]);
        ++p;std::memcpy(source.prefix.data()+0x70,&p,8);execution[0x11]=0xa3;
        assert(!tglPublishOwnedVeboxSfcAdmissionFlags(execution,source,dn) && execution[0x11]==0xa3);
        p=0;std::memcpy(source.prefix.data()+0x70,&p,8);dn[0]=1;
        assert(tglPublishOwnedVeboxSfcAdmissionFlags(execution,source,dn) && !execution[0x11]);
        p=reinterpret_cast<uintptr_t>(dn.data());std::memcpy(source.prefix.data()+0x70,&p,8);
        format=0xffffffff;std::memcpy(source.prefix.data()+0x130,&format,4);
        assert(tglPublishOwnedVeboxSfcAdmissionFlags(execution,source,dn) && !execution[0x11]);
    }
    {
        std::array<uint8_t,0xd48> execution{};
        for(unsigned old=0;old<256;++old) for(unsigned dn=0;dn<256;++dn)
            for(uint32_t type : {0u,1u,2u,3u,0xffffffffu}) {
                execution[0x0e]=0x93;execution[0x10]=0x74;execution[0x12]=0x51;execution[0x18]=0xa5;
                execution[0x0f]=static_cast<uint8_t>(old);execution[0x11]=0xff;execution[0x17]=0xff;
                tglPublishVeboxSfcAdmissionFlags(execution,static_cast<uint8_t>(dn),type);
                const bool classified=type==1 || type==2;
                assert(execution[0x11]==(dn&1) && execution[0x17]==classified);
                assert(execution[0x0f]==bool((old&1)||(dn&1)||classified));
                assert(execution[0x0e]==0x93 && execution[0x10]==0x74 && execution[0x12]==0x51 && execution[0x18]==0xa5);
            }
    }
    {
        std::array<uint8_t,11> nativePass{};nativePass[7]=0x73;nativePass[9]=0x91;
        TglVeboxAdmissionPassByte pass(nativePass[8]);
        TglVeboxAdmissionStagesInput input;
        input.prefix={true,true,true,true,1,uint64_t{1}<<12,1920,1080};input.pipe=0;
        std::array<uint8_t,0xd48> execution{};
        auto reset=[]() noexcept {return true;};
        auto setup=[](auto&) noexcept {};
        auto query=[]() noexcept {return true;};
        auto feature=[&](auto&) noexcept {nativePass[8]=0x80;};
        auto sfc=[](auto&) noexcept {return false;};
        assert(tglRunVeboxAdmissionStages(input,execution,pass,reset,setup,query,feature,sfc));
        assert(nativePass[8]==0x80 && nativePass[7]==0x73 && nativePass[9]==0x91);
        input.source124=2;
        auto denied=[](auto& bytes) noexcept {bytes[0x18]=1;};
        assert(!tglRunVeboxAdmissionStages(input,execution,pass,reset,setup,query,denied,sfc));
        assert(nativePass[8]==1 && nativePass[7]==0x73 && nativePass[9]==0x91);
    }
    {
        std::array<uint8_t,0x1bc0> base{};
        std::array<uint8_t,0x140> params{};std::array<uint8_t,0x28> pass{};
        TglOwnedSurfaceStorage source{},target{};
        const auto child=reinterpret_cast<uintptr_t>(base.data());
        struct Child {uintptr_t p;bool live=true;
            bool hasPrivateChildLease(uintptr_t value) const noexcept {return live && p==value;}
        } owner{child};
        struct Execution {bool live=true;uintptr_t identity=7;
            bool hasExecutionLease(uintptr_t value) const noexcept {return live && value==identity;}
        } execution;
        TglVeboxAdmissionBorrowedInterfaces expected{0x100,0x200,0x300,0};
        std::memcpy(base.data()+0x10,&expected.os,8);
        std::memcpy(base.data()+0x18,&expected.renderHal,8);
        std::memcpy(base.data()+0x20,&expected.sku,8);
        uintptr_t p=7;std::memcpy(base.data()+0x88,&p,8);base[0x1ba9]=1;
        p=reinterpret_cast<uintptr_t>(&source);std::memcpy(pass.data()+0x10,&p,8);
        p=reinterpret_cast<uintptr_t>(&target);std::memcpy(params.data()+0x98,&p,8);
        params[0x138]=1;uint32_t width=1920;std::memcpy(source.prefix.data()+0xd8,&width,4);
        bool allow=true;
        const uintptr_t pp=reinterpret_cast<uintptr_t>(params.data()),pa=reinterpret_cast<uintptr_t>(pass.data());
        auto read=[&](uintptr_t address,void* out,size_t n) noexcept {
            if(!allow) return false;
            if(address==0x300 && n==8) {uint64_t sku=uint64_t{1}<<12;std::memcpy(out,&sku,8);return true;}
            if((address==pa+0x10 && n==8) || (address==pp+0x98 && n==8) ||
               (address==pp+0xf0 && n==8) || (address==pp+0x138 && n==1) ||
               (address==pp+0x10c && n==4)) {std::memcpy(out,reinterpret_cast<void*>(address),n);return true;}
            assert(false);return false;
        };
        TglVeboxAdmissionStagesInput input;input.params10c=0xabcdef;
        auto capture=[&]() noexcept {
            return tglReadLeasedVeboxAdmissionInputs(input,owner,execution,base.data(),pp,pa,source,target,expected,1,read);
        };
        owner.live=false;assert(!capture() && input.params10c==0xabcdef);owner.live=true;
        execution.live=false;assert(!capture() && input.params10c==0xabcdef);execution.live=true;
        allow=false;assert(!capture() && input.params10c==0xabcdef);allow=true;
        ++expected.renderHal;assert(!capture() && input.params10c==0xabcdef);--expected.renderHal;
        ++expected.paramsF0;assert(!capture() && input.params10c==0xabcdef);--expected.paramsF0;
        assert(!tglReadLeasedVeboxAdmissionInputs(input,owner,execution,base.data(),
            UINTPTR_MAX,pa,source,target,expected,1,read) && input.params10c==0xabcdef);
        assert(!tglReadLeasedVeboxAdmissionInputs(input,owner,execution,base.data(),
            pp,UINTPTR_MAX,source,target,expected,1,read) && input.params10c==0xabcdef);
        assert(!tglReadLeasedVeboxAdmissionInputs(input,owner,execution,base.data(),
            pp,pa,source,target,expected,3,read) && input.params10c==0xabcdef);
        p=123;std::memcpy(pass.data()+0x10,&p,8);assert(!capture() && input.params10c==0xabcdef);
        p=reinterpret_cast<uintptr_t>(&source);std::memcpy(pass.data()+0x10,&p,8);
        assert(capture() && input.prefix.sourceWidth==1920 && input.prefix.params138==1);
        assert(input.prefix.sku0==(uint64_t{1}<<12) && input.mmc==1 && input.pipe==1);
        std::array<uint8_t,0xd48> nativeExecution{};
        execution.identity=reinterpret_cast<uintptr_t>(nativeExecution.data());
        std::memcpy(base.data()+0x88,&execution.identity,8);
        uint32_t height=1080;std::memcpy(source.prefix.data()+0xdc,&height,4);
        unsigned resets=0,sfcCalls=0;
        auto reset=[&]() noexcept {++resets;nativeExecution.fill(0);return true;};
        auto setup=[](auto&) noexcept {};
        auto feature=[](auto&) noexcept {assert(false);}; // unknown source format
        auto sfc=[&](auto&) noexcept {++sfcCalls;pass[8]=0x80;return true;};
        auto run=[&]() noexcept {
            return tglRunCapturedVeboxAdmissionStages(owner,execution,base.data(),params,pass,
                source,target,expected,1,nativeExecution,read,reset,setup,feature,sfc);
        };
        assert(run() && resets==1 && sfcCalls==1 && pass[8]==0x80);
        ++execution.identity;
        assert(!run() && resets==1 && sfcCalls==1 && pass[8]==0x80);
        --execution.identity;
        p=123;std::memcpy(params.data()+0x98,&p,8);
        assert(!run() && resets==1 && sfcCalls==1 && pass[8]==0x80);
    }
    {
        TglVeboxAdmissionStagesInput input;
        input.prefix={true,true,true,true,1,uint64_t{1}<<12,0,0};input.pipe=2;
        TglOwnedSurfaceStorage source{};
        uint32_t width=64,height=16,format=0x19,sample=0;
        std::memcpy(source.prefix.data()+0xd8,&width,4);
        std::memcpy(source.prefix.data()+0xdc,&height,4);
        std::memcpy(source.prefix.data()+0x130,&format,4);
        std::array<uint8_t,0xd48> execution{};std::array<uint8_t,0x28> pass{};
        unsigned resets=0,features=0,sfcs=0;
        auto reset=[&]() noexcept {++resets;execution.fill(0);return true;};
        auto setup=[](auto&) noexcept {};
        auto feature=[&](auto& bytes) noexcept {++features;bytes[0x18]=0;};
        auto sfc=[&](auto&) noexcept {++sfcs;return true;};
        assert(tglRunOwnedVeboxSurfaceAdmissionStages(input,source,execution,pass,reset,setup,feature,sfc));
        assert(resets==1 && features==1 && !sfcs);
        format=0xdead;std::memcpy(source.prefix.data()+0x130,&format,4);
        assert(!tglRunOwnedVeboxSurfaceAdmissionStages(input,source,execution,pass,reset,setup,feature,sfc));
        assert(resets==2 && features==1 && !sfcs);
        input.pipe=1;
        assert(tglRunOwnedVeboxSurfaceAdmissionStages(input,source,execution,pass,reset,setup,feature,sfc));
        assert(resets==3 && features==1 && sfcs==1);
        sample=2;std::memcpy(source.prefix.data()+0x124,&sample,4);
        assert(!tglRunOwnedVeboxSurfaceAdmissionStages(input,source,execution,pass,reset,setup,feature,sfc));
        assert(resets==4 && features==1 && sfcs==1 && pass[8]==1);
        width=63;std::memcpy(source.prefix.data()+0xd8,&width,4);
        assert(!tglRunOwnedVeboxSurfaceAdmissionStages(input,source,execution,pass,reset,setup,feature,sfc));
        assert(resets==4);
    }
    {
        struct Backend {
            std::array<uint8_t,0xd48> bytes{};unsigned creates=0,destroys=0,initializes=0;
            int status=0;
            bool shape=true,invalidate=false;
            uintptr_t create() noexcept {++creates;return reinterpret_cast<uintptr_t>(bytes.data());}
            int initialize(uintptr_t p) noexcept {
                assert(validate(p));++initializes;bytes.fill(0);if(invalidate) shape=false;return status;
            }
            bool validate(uintptr_t p) noexcept {return shape && p==reinterpret_cast<uintptr_t>(bytes.data());}
            void destroy(uintptr_t p) noexcept {assert(p==reinterpret_cast<uintptr_t>(bytes.data()));++destroys;}
        } backend;
        TglExecutionOwner owner(backend);assert(!owner.ensure());
        const auto address=owner.get();
        auto denied=[](auto&) noexcept {assert(false);return 0;};
        {
            const auto before=backend.bytes;
            assert(tglPublishLeasedVeboxMode0Prelude(owner,address,77,UINT32_MAX)==5 && backend.bytes==before);
            assert(owner.withExecution([&](uintptr_t actual) noexcept {
                assert(tglPublishLeasedVeboxMode0Prelude(owner,actual+1,77,0)==5 && backend.bytes==before);
                for(uint32_t index : {0u,1u,2u,UINT32_MAX}) {
                    backend.bytes.fill(0xcc);
                    assert(!tglPublishLeasedVeboxMode0Prelude(owner,actual,77,index));
                    auto expected=before;expected.fill(0xcc);
                    constexpr size_t offsets[]={0x28,0x1c,0x20,0x2c,0x30};
                    const uint32_t values[]={77,0,1,index&1,(index+1)&1};
                    for(size_t i=0;i<5;++i) std::memcpy(expected.data()+offsets[i],&values[i],4);
                    assert(backend.bytes==expected);
                }
                return 0;
            })==0);
            backend.bytes=before;
        }
        assert(owner.withLeasedAdmissionBytes(address,denied)==5);
        assert(owner.initializeLeasedAdmission(address)==5 && backend.initializes==1);
        assert(owner.withExecution([&](uintptr_t actual) noexcept {
            assert(owner.withLeasedAdmissionBytes(actual+1,denied)==5);
            const int result=owner.withLeasedAdmissionBytes(actual,[&](auto& bytes) noexcept {
                assert(bytes.data()==backend.bytes.data());
                assert(owner.withLeasedAdmissionBytes(actual,denied)==5);
                owner.reset();assert(owner.get()==actual && !backend.destroys);
                TglVeboxAdmissionStagesInput input;
                input.prefix={true,true,true,true,1,uint64_t{1}<<12,64,16};
                input.pipe=2;input.params10c=71;input.leasedParamsF0=0x1234;
                std::array<uint8_t,0x28> pass{};
                const bool admitted=tglRunVeboxAdmissionStages(input,bytes,pass,
                    [&]() noexcept {return owner.initializeLeasedAdmission(actual)==0;},[](auto&) noexcept {},
                    []() noexcept {return true;},[](auto& view) noexcept {view[0x18]=0;},
                    [](auto&) noexcept {assert(false);return false;});
                uintptr_t params=0;std::memcpy(&params,backend.bytes.data()+0x9e8,8);
                uint32_t field=0;std::memcpy(&field,backend.bytes.data()+0xd28,4);
                assert(admitted && params==0x1234 && field==71);
                assert(backend.initializes==2 && owner.initializeLeasedAdmission(actual)==5);
                return 31;
            });
            const auto before=backend.bytes;
            assert(owner.withLeasedAdmissionBytes(actual,[&](auto&) noexcept {
                return owner.initializeLeasedAdmission(actual);
            })==5);
            assert(backend.initializes==2 && backend.bytes==before);
            return result;
        })==31);
        assert(owner.withLeasedAdmissionBytes(address,denied)==5);
        backend.status=31;
        assert(owner.withExecution([&](uintptr_t actual) noexcept {
            return owner.withLeasedAdmissionBytes(actual,[&](auto&) noexcept {
                assert(owner.initializeLeasedAdmission(actual)==31);
                assert(owner.get()==actual && !backend.destroys);
                assert(!owner.hasExecutionLease(actual));
                owner.reset();assert(!backend.destroys);
                return 0; // cannot hide native reset failure
            });
        })==31);
        assert(owner.get()==address && backend.initializes==3 && !backend.destroys);
        assert(owner.ensure()==5 && owner.preparePass()==5);
        assert(owner.withExecution([](uintptr_t) noexcept {assert(false);return 0;})==5);
        owner.reset();assert(backend.creates==1 && backend.destroys==1);
        backend.status=0;assert(!owner.ensure());backend.invalidate=true;
        assert(owner.withExecution([&](uintptr_t actual) noexcept {
            return owner.withLeasedAdmissionBytes(actual,[&](auto&) noexcept {
                assert(owner.initializeLeasedAdmission(actual)==5);
                assert(owner.get()==actual && backend.destroys==1);return 0;
            });
        })==5);
        assert(owner.ensure()==5 && backend.creates==2 && backend.destroys==1);
        owner.reset();assert(backend.destroys==2);
    }
    {
        TglVeboxAdmissionStagesInput input;
        input.prefix={true,true,true,true,1,uint64_t{1}<<12,64,16};
        input.pipe=1;input.params10c=42;input.mmc=3;
        std::array<uint8_t,0xd48> execution{};
        std::array<uint8_t,0x28> pass{};
        unsigned order=0;bool eligible=true,denyFeature=true,sfcReady=true;
        auto reset=[&]() noexcept {assert(order++==0);execution.fill(0);return true;};
        auto setup=[&](auto& bytes) noexcept {
            assert(order++==1 && bytes[0xd2c]==1);
            uint32_t d28=1;std::memcpy(&d28,bytes.data()+0xd28,4);assert(d28==0);
        };
        auto query=[&]() noexcept {assert(order++==2);return eligible;};
        auto feature=[&](auto& bytes) noexcept {
            assert(order++==3);bytes[0x18]=denyFeature;
            uint32_t d28=0;std::memcpy(&d28,bytes.data()+0xd28,4);assert(d28==42);
        };
        auto sfc=[&](auto&) noexcept {assert(order++==(eligible?4u:3u));return sfcReady;};
        assert(tglRunVeboxAdmissionStages(input,execution,pass,reset,setup,query,feature,sfc));
        assert(order==5); // feature denial must NOT bypass pipe1 SFC
        order=0;input.source124=2;
        assert(!tglRunVeboxAdmissionStages(input,execution,pass,reset,setup,query,feature,sfc));
        assert(order==4 && pass[8]==1); // fallback pipe0 skips SFC
        order=0;pass[8]=0;input.source124=0;eligible=false;
        assert(tglRunVeboxAdmissionStages(input,execution,pass,reset,setup,query,feature,sfc));
        assert(order==4);
        order=0;sfcReady=false;
        assert(!tglRunVeboxAdmissionStages(input,execution,pass,reset,setup,query,feature,sfc));
        assert(order==4);
        order=0;input.prefix.sourceWidth=63;
        assert(!tglRunVeboxAdmissionStages(input,execution,pass,reset,setup,query,feature,sfc));
        assert(!order && pass[8]==1);
    }
    {
        TestFeatureSurfaceOwner source,target;
        TestFeatureExecutionOwner execution;
        std::array<uint8_t,0x1bc0> base{};
        TestVeboxChildLease owner{base.data()};
        uintptr_t report=reinterpret_cast<uintptr_t>(execution.bytes.data());
        std::memcpy(base.data()+0x88,&report,8);
        unsigned calls=0;
        auto callback=[&](void* child,const auto& s,const auto& t) noexcept {
            ++calls;assert(child==base.data() && &s==&source.storage && &t==&target.storage);
            assert(source.busy && target.busy && owner.busy && execution.busy);
            uintptr_t p=reinterpret_cast<uintptr_t>(&t);
            std::memcpy(execution.bytes.data()+0x60,&p,8);return 31;
        };
        assert(tglWithOwnedVeboxFeatureFrame(source,target,owner,execution,callback)==31);
        uintptr_t p=1;std::memcpy(&p,execution.bytes.data()+0x60,8);
        assert(!p && calls==1 && !source.busy && !target.busy && !owner.busy && !execution.busy);
        target.deny=true;
        assert(tglWithOwnedVeboxFeatureFrame(source,target,owner,execution,callback)==5 && calls==1);
        target.deny=false;p=123;std::memcpy(execution.bytes.data()+0x60,&p,8);
        assert(tglWithOwnedVeboxFeatureFrame(source,target,owner,execution,callback)==5 && calls==1);
        uintptr_t observed=0;std::memcpy(&observed,execution.bytes.data()+0x60,8);assert(observed==123);
        p=0;std::memcpy(execution.bytes.data()+0x60,&p,8);
        assert(tglWithOwnedVeboxFeatureFrame(source,target,owner,execution,
            [&](void*,const auto&,const auto&) noexcept {
                uintptr_t bad=123;std::memcpy(execution.bytes.data()+0x60,&bad,8);return 0;
            })==5);
        std::memcpy(&observed,execution.bytes.data()+0x60,8);assert(!observed);
        assert(tglWithOwnedVeboxFeatureFrame(source,source,owner,execution,
            [&](void*,const auto& s,const auto& t) noexcept {
                assert(&s==&t && source.busy && owner.busy && execution.busy);
                assert(tglWithOwnedVeboxFeatureFrame(source,target,owner,execution,callback)==5);
                uintptr_t retained=reinterpret_cast<uintptr_t>(&t);
                std::memcpy(execution.bytes.data()+0x60,&retained,8);return -7;
            })==-7);
        std::memcpy(&observed,execution.bytes.data()+0x60,8);
        assert(!observed && calls==1 && !source.busy && !owner.busy && !execution.busy);
        p=0;std::memcpy(base.data()+0x88,&p,8);
        assert(tglWithOwnedVeboxFeatureFrame(source,target,owner,execution,callback)==5 && calls==1);
    }
    {
        std::array<uint8_t,0x1bc0> base{};
        std::array<uint8_t,16> history{};history[0]=1;history[15]=0xa5;
        TglOwnedSurfaceStorage source{},linked{};
        struct Owner {
            uintptr_t child;bool live=false;
            bool hasPrivateChildLease(uintptr_t p) const noexcept {return live && p==child;}
        } owner{reinterpret_cast<uintptr_t>(base.data())};
        uintptr_t historyPointer=reinterpret_cast<uintptr_t>(history.data());
        std::memcpy(base.data()+0x90,&historyPointer,8);
        uint32_t count=1;std::memcpy(source.prefix.data()+0xb4,&count,4);
        const auto originalBase=base;const auto originalHistory=history;
        assert(!tglUpdateOwnedVeboxHistory(owner,base.data(),history,source,nullptr,nullptr,0));
        assert(base==originalBase && history==originalHistory);
        owner.live=true;
        assert(!tglUpdateOwnedVeboxHistory(owner,base.data(),history,source,&linked,nullptr,0));
        assert(base==originalBase && history==originalHistory);
        assert(tglUpdateOwnedVeboxHistory(owner,base.data(),history,source,nullptr,nullptr,0));
        uint32_t mode=0,field=0;std::memcpy(&mode,history.data()+4,4);
        std::memcpy(&field,base.data()+0x1b5c,4);
        assert(mode==1 && field==4 && history[15]==0xa5);
        const auto beforeBase=base;const auto beforeHistory=history;
        assert(!tglUpdateOwnedVeboxHistory(owner,base.data(),history,source,nullptr,nullptr,0));
        assert(base==beforeBase && history==beforeHistory);
        uintptr_t p=reinterpret_cast<uintptr_t>(&linked);
        std::memcpy(source.prefix.data()+0xc0,&p,8);
        assert(tglUpdateOwnedVeboxHistory(owner,base.data(),history,source,&linked,nullptr,0));
        std::memcpy(&mode,history.data()+4,4);assert(mode==0);
        ++historyPointer;std::memcpy(base.data()+0x90,&historyPointer,8);
        assert(!tglUpdateOwnedVeboxHistory(owner,base.data(),history,source,&linked,nullptr,0));
    }
    {
        TglVeboxHistoryState state;
        state.enabled=1;state.radius=1;state.previous1b78=10;state.previous1b7c=11;
        TglVeboxHistoryFrame frame;frame.id=10;frame.countB4=1;
        assert(tglUpdateVeboxHistory(state,frame,0));
        assert(state.mode==1 && state.field1b5c==4);
        assert(!tglUpdateVeboxHistory(state,frame,0) && state.mode==1);
        uint32_t c0=10,c8=11;frame.linkedC0=&c0;frame.linkedC8=&c8;
        frame.has88=true;
        assert(tglUpdateVeboxHistory(state,frame,0) && state.mode==1);
        frame.has88=false;
        assert(tglUpdateVeboxHistory(state,frame,0) && state.mode==0);
        frame.countB8=1;
        assert(tglUpdateVeboxHistory(state,frame,0) && state.mode==0);
        c8=100;
        assert(tglUpdateVeboxHistory(state,frame,0) && state.mode==0);
        for(uint32_t mode=0;mode<4;++mode) {
            state.mode=mode;state.alternate=0xfe;frame.countB4=0;
            assert(tglUpdateVeboxHistory(state,frame,0));
            assert(state.mode==((mode==2 || mode==3)?0:mode));
            assert(state.alternate==(mode==2?1:0xfe));
        }
        state.mode=2;state.enabled=0;
        assert(tglUpdateVeboxHistory(state,frame,1) && state.mode==2);
        state.enabled=1;state.disabled=1;
        assert(tglUpdateVeboxHistory(state,frame,0) && state.mode==0);
        state.disabled=0;state.mode=2;
        assert(tglUpdateVeboxHistory(state,frame,2) && state.mode==0);
    }
    {
        assert(tglVeboxHistoryWithinWindow(10,10,0));
        assert(!tglVeboxHistoryWithinWindow(11,10,0));
        assert(tglVeboxHistoryWithinWindow(8,10,2));
        assert(tglVeboxHistoryWithinWindow(12,10,2));
        assert(!tglVeboxHistoryWithinWindow(7,10,2));
        assert(!tglVeboxHistoryWithinWindow(13,10,2));
        assert(tglVeboxHistoryWithinWindow(0,UINT32_MAX,1));
        assert(tglVeboxHistoryWithinWindow(UINT32_MAX,0,1));
        assert(!tglVeboxHistoryWithinWindow(1,UINT32_MAX,1));
        assert(!tglVeboxHistoryWithinWindow(0x80000000,0,0x7fffffff));
        assert(tglVeboxHistoryWithinWindow(0x80000000,0,0x80000000));
        assert(!tglVeboxHistoryWithinWindow(0,0,0x80000000));
        assert(!tglVeboxHistoryWithinWindow(0,0,UINT32_MAX));
    }
    {
        std::array<uint8_t,0xd30> execution{};
        std::array<uint8_t,0x28> pass{};
        for(uint32_t pipe=0;pipe<4;++pipe)
            for(uint32_t sample=0;sample<4;++sample)
                for(bool admitted:{false,true}) {
                    execution.fill(0xa5);pass.fill(0xa4);
                    std::memcpy(execution.data()+0xa0c,&pipe,4);
                    const auto oldExecution=execution;const auto oldPass=pass;
                    const bool valid=tglApplyVeboxAdmissionSampleFallback(execution,pass,sample,admitted);
                    assert(valid==(pipe<3));
                    if(!valid || sample!=2 || admitted) {
                        assert(execution==oldExecution && pass==oldPass);
                    } else {
                        uint32_t actual=99;std::memcpy(&actual,execution.data()+0xa0c,4);
                        assert(actual==0 && pass[8]==1);
                        for(size_t i=0;i<execution.size();++i)
                            if(i<0xa0c || i>=0xa10) assert(execution[i]==oldExecution[i]);
                        for(size_t i=0;i<pass.size();++i)
                            if(i!=8) assert(pass[i]==oldPass[i]);
                    }
                }
        for(uint8_t params:{uint8_t(0),uint8_t(1),uint8_t(2),uint8_t(0xff)})
            for(uint8_t fallback:{uint8_t(0),uint8_t(1),uint8_t(2),uint8_t(0xff)}) {
                assert(!tglVeboxAdmissionResult(params,fallback,false));
                assert(tglVeboxAdmissionResult(params,fallback,true)==!((params&1)&&(fallback&1)));
            }
        // Compose the proven CPU segments: admission starts viable, mode2 is
        // published, then sample2/feature denial forces pipe0 and final FALSE.
        TglVeboxAdmissionPrefixInput input{true,true,true,true,1,uint64_t(1)<<12,64,16};
        pass.fill(0);execution.fill(0);
        assert(tglPrepareVeboxAdmissionPrefix(input,pass)==TglVeboxAdmissionPrefixResult::Continue);
        unsigned setup=0;
        assert(tglPublishVeboxAdmissionMode(execution,2,1,7,[&](auto&) noexcept {++setup;}));
        assert(tglApplyVeboxAdmissionSampleFallback(execution,pass,2,false));
        assert(!tglVeboxAdmissionResult(input.params138,pass[8],false) && setup==1);
        uint32_t pipe=99;std::memcpy(&pipe,execution.data()+0xa0c,4);assert(pipe==0);
    }
    {
        std::array<uint8_t,0xd30> execution{};execution.fill(0xa5);
        const auto original=execution;
        unsigned calls=0;
        uint32_t selected=0;
        auto setup=[&](auto& storage) noexcept {
            uint32_t pipe=0,mode=0;
            std::memcpy(&pipe,storage.data()+0xa0c,4);
            std::memcpy(&mode,storage.data()+0xd28,4);
            assert(pipe==selected && mode==0xa5a5a5a5 && storage[0xd2c]==1);
            ++calls;
        };
        assert(!tglPublishVeboxAdmissionMode(execution,3,0xff,7,setup));
        assert(execution==original && !calls);
        for(selected=0;selected<3;++selected) {
            execution=original;
            assert(tglPublishVeboxAdmissionMode(execution,selected,0xff,7,setup));
            uint32_t mode=0;std::memcpy(&mode,execution.data()+0xd28,4);assert(mode==7);
            for(size_t i=0;i<execution.size();++i)
                if(!(i>=0xa0c && i<0xa10) && !(i>=0xd28 && i<0xd2d))
                    assert(execution[i]==original[i]);
        }
        assert(calls==3);
    }
    {
        using Result=TglVeboxAdmissionPrefixResult;
        TglVeboxAdmissionPrefixInput input{true,true,true,true,1,uint64_t(1)<<12,64,16};
        std::array<uint8_t,0x28> pass{};pass.fill(0xa5);
        const auto original=pass;
        assert(tglPrepareVeboxAdmissionPrefix(input,pass)==Result::Continue && pass==original);
        for(unsigned mask=0;mask<15;++mask) {
            auto missing=input;
            missing.os=mask&1;missing.renderHal=mask&2;
            missing.execution=mask&4;missing.source=mask&8;
            assert(tglPrepareVeboxAdmissionPrefix(missing,pass)==Result::MissingDependencies);
            assert(pass==original);
        }
        for(unsigned failure=0;failure<4;++failure) {
            auto fallback=input;pass=original;
            if(failure==0) fallback.params138=2;
            if(failure==1) fallback.sku0=~(uint64_t(1)<<12);
            if(failure==2) fallback.sourceWidth=63;
            if(failure==3) fallback.sourceHeight=15;
            assert(tglPrepareVeboxAdmissionPrefix(fallback,pass)==Result::CompositionFallback);
            for(size_t i=0;i<pass.size();++i) assert(pass[i]==(i==8?1:original[i]));
        }
        input.params138=0xff;input.sku0=UINT64_MAX;
        input.sourceWidth=UINT32_MAX;input.sourceHeight=UINT32_MAX;
        pass=original;
        assert(tglPrepareVeboxAdmissionPrefix(input,pass)==Result::Continue && pass==original);
    }
    {
        int child=0,foreign=0,params=0,pass=0;
        struct Admission {
            void* child;
            void* params;
            void* pass;
            unsigned calls=0;
            bool allowed=false;
            bool owns(void* p) const noexcept {return p==child;}
            bool admit(void* p,void* q) noexcept {
                if(p!=params || q!=pass) return false;
                ++calls;return allowed;
            }
        } context{&child,&params,&pass};
        using Resolve=TglOwnedVeboxScopedContext<Admission>;
        using Callback=TglOwnedVeboxAdmissionCallbacks<Resolve>;
        static_assert(std::is_same_v<decltype(&Callback::admit),bool(*)(void*,void*,void*) noexcept>);
        std::array<uintptr_t,20> hooks{};hooks.fill(7);
        const auto bound=tglBindOwnedVeboxAdmissionHook<Resolve>(hooks);
        for(size_t i=0;i<hooks.size();++i)
            assert(bound[i]==(i==2?reinterpret_cast<uintptr_t>(&Callback::admit):hooks[i]));
        auto admit=reinterpret_cast<bool(*)(void*,void*,void*) noexcept>(bound[2]);
        assert(!admit(&child,&params,&pass) && !context.calls);
        {Resolve scope(&child,context);assert(scope.bound());
            assert(!admit(&foreign,&params,&pass) && !context.calls);
            assert(!admit(&child,nullptr,&pass) && !context.calls);
            assert(!admit(&child,&params,nullptr) && !context.calls);
            assert(!admit(&child,&foreign,&pass) && !context.calls);
            assert(!admit(&child,&params,&pass) && context.calls==1);
            context.allowed=true;
            assert(admit(&child,&params,&pass) && context.calls==2);
        }
        assert(!admit(&child,&params,&pass) && context.calls==2);
    }
    {
        int child=0,foreign=0,params=0,pass=0;
        struct Owner {
            uintptr_t child;
            bool leased=false;
            bool hasPrivateChildLease(uintptr_t p) const noexcept {return leased && p==child;}
        } owner{reinterpret_cast<uintptr_t>(&child)};
        unsigned calls=0;bool allowed=false;
        void* nested=nullptr;bool (*reenter)(void*) noexcept=nullptr;
        auto policy=[&](void* c,void* p,void* q) noexcept {
            assert(c==&child && p==&params && q==&pass);++calls;
            if(reenter) assert(!reenter(nested));
            return allowed;
        };
        using Context=TglOwnedVeboxAdmissionContext<Owner,decltype(policy)>;
        Context context(owner,&child,&params,&pass,policy);
        using Scope=TglOwnedVeboxScopedContext<Context>;
        using Callback=TglOwnedVeboxAdmissionCallbacks<Scope>;
        assert(!context.admit(&params,&pass) && !calls);
        owner.leased=true;
        assert(!context.admit(&foreign,&pass) && !calls);
        assert(!context.admit(&params,&foreign) && !calls);
        {Scope scope(&foreign,context);assert(scope.bound());
            assert(!Callback::admit(&foreign,&params,&pass) && !calls);
        }
        {Scope scope(&child,context);assert(scope.bound());
            assert(!Callback::admit(&child,&params,&pass) && calls==1);
            struct Nested {Context* context;void* params;void* pass;} state{&context,&params,&pass};
            nested=&state;
            reenter=[](void* p) noexcept {
                auto& value=*static_cast<Nested*>(p);
                return value.context->admit(value.params,value.pass);
            };
            allowed=true;
            assert(Callback::admit(&child,&params,&pass) && calls==2);
            owner.leased=false;
            assert(!Callback::admit(&child,&params,&pass) && calls==2);
        }
        assert(!Callback::admit(&child,&params,&pass) && calls==2);
    }
    {
        int child=0,foreign=0;
        struct Owner {
            void* child;bool leased=true;
            bool hasPrivateChildLease(uintptr_t p) const noexcept {
                return leased && p==reinterpret_cast<uintptr_t>(child);
            }
        } owner{&child};
        unsigned calls=0;int result=31;
        int(*reenter)(void*) noexcept=nullptr;void* state=nullptr;
        bool invalidate=false;
        auto transaction=[&](void* p) noexcept -> int {
            assert(p==&child);++calls;
            if(reenter) assert(reenter(state)==5);
            if(invalidate) owner.leased=false;
            return result;
        };
        using Context=TglOwnedVeboxAllocationContext<Owner,decltype(transaction)>;
        Context context(owner,transaction,&child);
        using Resolve=TglOwnedVeboxScopedContext<Context>;
        using Callback=TglOwnedVeboxAllocationCallbacks<Resolve>;
        std::array<uintptr_t,20> hooks{};hooks.fill(7);
        const auto bound=tglBindOwnedVeboxAllocationHook<Resolve>(hooks);
        for(size_t i=0;i<hooks.size();++i)
            assert(bound[i]==(i==3?reinterpret_cast<uintptr_t>(&Callback::allocate):hooks[i]));
        using NativeAllocate=int(*)(void*);
        auto allocate=reinterpret_cast<NativeAllocate>(bound[3]);
        assert(allocate(&child)==5 && !calls);
        {Resolve scope(&foreign,context);assert(scope.bound());
            assert(allocate(&foreign)==5 && !calls);
        }
        {Resolve scope(&child,context);assert(scope.bound());
            assert(allocate(&foreign)==5 && !calls);
            assert(allocate(&child)==31 && calls==1);
            result=0;state=&context;
            reenter=+[](void* p) noexcept {return static_cast<Context*>(p)->allocate();};
            assert(allocate(&child)==0 && calls==2);
            owner.leased=false;
            assert(allocate(&child)==5 && calls==2);
            owner.leased=true;invalidate=true;
            assert(allocate(&child)==5 && calls==3);
        }
        assert(allocate(&child)==5 && calls==3);
    }
    {
        struct Frame {TglOwnedVeboxProducerPass pass;} frame;
        struct Child {
            bool leased=true;unsigned writes=0;TglCscCoefficients saved;
            bool hasPrivateChildLease(uintptr_t p) const noexcept {return leased && p==7;}
            bool publishCsc(const TglCscCoefficients& value) noexcept {
                if(!leased) return false; ++writes;saved=value;return true;
            }
        } child;
        struct Execution {bool leased=true;
            bool hasExecutionLease(uintptr_t p) const noexcept {return leased && p==9;}
        } execution;
        TglOwnedSurfaceStorage source,target,foreign;
        TglOwnedSurfaceInputs surfaces{};TglOwnedVeboxStateStorage state{};
        TglOwnedDiIecpPacket di{};
        TglOwnedVeboxPacketLeases leases{7,9,0,0,source,&target,surfaces,state,di};
        uint32_t from=3,to=7,format=2;
        std::memcpy(source.prefix.data(),&from,4);std::memcpy(target.prefix.data(),&to,4);
        std::memcpy(source.prefix.data()+0x130,&format,4);
        unsigned calls=0;bool allowed=true;
        auto produce=[&](TglCscCoefficients& out,uint32_t a,uint32_t b,uint32_t f) noexcept {
            assert(a==3 && b==7 && f==2);++calls;out.matrix.fill(42);return allowed;
        };
        TglOwnedVeboxCscFrameContext<Frame,Child,Execution,decltype(produce)>
            context(frame,leases,child,execution,produce);
        assert(context.csc(&source,&target)==0 && calls==1 && child.writes==1);
        assert(context.csc(&foreign,&target)==5 && context.csc(&source,&foreign)==5);
        child.leased=false;assert(context.csc(&source,&target)==5);child.leased=true;
        execution.leased=false;assert(context.csc(&source,&target)==5);execution.leased=true;
        assert(calls==1 && child.writes==1);
        allowed=false;assert(context.csc(&source,&target)==5 && calls==2 && child.writes==1);
        leases.target=nullptr;assert(context.csc(&source,&target)==5 && calls==2);
        assert(&context.pass==&frame.pass);
        leases.target=&target;allowed=true;
        using Resolve=TglOwnedVeboxScopedContext<decltype(context)>;
        using Callbacks=TglOwnedVeboxProducerCallbacks<Resolve>;
        void* nativeChild=reinterpret_cast<void*>(leases.child);
        {Resolve scope(nativeChild,context);assert(scope.bound());
            assert(context.pass.withBuilder([&]() noexcept {
                Callbacks::csc(nativeChild,&source,&target);
                return context.pass.gate([]() noexcept {return 0;});
            })==0 && calls==3 && child.writes==2);
            assert(context.pass.withBuilder([&]() noexcept {
                Callbacks::csc(nativeChild,&foreign,&target);
                return context.pass.gate([]() noexcept {return 0;});
            })==5 && calls==3 && child.writes==2);
        }
        assert(!Resolve::get(nativeChild));
    }
    {
        std::array<uint8_t,0x200> child;child.fill(0xa5);
        TglCscCoefficients csc;
        for(size_t i=0;i<9;++i) csc.matrix[i]=float(i+1);
        for(size_t i=0;i<3;++i) {csc.inputOffsets[i]=float(10+i);csc.outputOffsets[i]=float(20+i);}
        assert(!tglPublishOwnedCscFields(nullptr,child.size(),csc));
        assert(!tglPublishOwnedCscFields(child.data(),0xdb,csc));
        assert(!tglPublishOwnedCscFields(reinterpret_cast<void*>(UINTPTR_MAX-3),child.size(),csc));
        for(auto byte:child) assert(byte==0xa5);
        assert(tglPublishOwnedCscFields(child.data(),child.size(),csc));
        assert(std::memcmp(child.data()+0xa0,csc.matrix.data(),0x24)==0);
        assert(std::memcmp(child.data()+0xc4,csc.inputOffsets.data(),0xc)==0);
        assert(std::memcmp(child.data()+0xd0,csc.outputOffsets.data(),0xc)==0);
        for(size_t i=0;i<child.size();++i) if(i<0xa0 || i>=0xdc) assert(child[i]==0xa5);
    }
    {
        constexpr uintptr_t base=0x100000;
        std::array<uint8_t,15> prologue{0x55,0x48,0x89,0xe5,0x48,0x81,0xec,0x80,0,0,0,0x48,0x8d,0x45,0xc0};
        std::array<uint8_t,5> call{0xe8,0x53,0x6e,0,0};
        std::array<uint8_t,35> matrixAnchor{
            0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x20,0x89,0x7d,0xfc,
            0x89,0x75,0xf8,0x48,0x89,0x55,0xf0,0xc6,0x45,0xef,0,
            0x8b,0x7d,0xfc,0xbe,0xff,0xff,0xff,0xff,0xe8,0xfd,0xe9,0xff,0xff};
        bool allowed=true;unsigned calls=0;
        auto read=[&](uintptr_t p,void* out,size_t n) noexcept {
            if(p==base+0x4dac0 && n==prologue.size()) std::memcpy(out,prologue.data(),n);
            else if(p==base+0x4daf8 && n==call.size()) std::memcpy(out,call.data(),n);
            else if(p==base+0x54950 && n==matrixAnchor.size()) std::memcpy(out,matrixAnchor.data(),n);
            else return false;
            return true;
        };
        auto qualify=[&](uintptr_t p) noexcept {return allowed && p==base;};
        auto rx=[](uintptr_t,size_t) noexcept {return true;};
        TglCscCoefficientsBinding binding;assert(binding.resolve(base,read,qualify,rx));
        TglCscCoefficients output;
        auto invoke=[&](uintptr_t p,uint32_t from,uint32_t to,float* matrix,float* in,float* out) noexcept {
            assert(p==base+0x4dac0 && from==3 && to==7);++calls;
            for(unsigned i=0;i<9;++i) matrix[i]=float(i);
            for(unsigned i=0;i<3;++i) {in[i]=float(10+i);out[i]=float(20+i);}
        };
        bool supported=true;unsigned validations=0;
        auto matrixInvoke=[&](uintptr_t p,uint32_t from,uint32_t to,float* out) noexcept {
            assert(p==base+0x54950 && from==3 && to==7);++validations;
            for(unsigned i=0;i<12;++i) out[i]=float(i);
            return supported;
        };
        auto produce=[&]() {return tglProduceNativeCscCoefficients(binding,output,3,7,read,qualify,rx,invoke,matrixInvoke);};
        assert(produce() && calls==1);
        for(unsigned i=0;i<9;++i) assert(output.matrix[i]==float(i));
        for(unsigned i=0;i<3;++i) assert(output.inputOffsets[i]==float(10+i) && output.outputOffsets[i]==float(20+i));
        const auto saved=output;
        supported=false;assert(!produce() && calls==1 && validations==2);
        assert(output.matrix==saved.matrix && output.inputOffsets==saved.inputOffsets && output.outputOffsets==saved.outputOffsets);
        supported=true;matrixAnchor[0]^=1;assert(!produce() && validations==2 && calls==1);matrixAnchor[0]^=1;
        call[1]^=1;assert(!produce());call[1]^=1;
        prologue[0]^=1;assert(!produce());prologue[0]^=1;
        allowed=false;assert(!produce());allowed=true;
        ++binding.entry;assert(!produce() && calls==1);
        assert(output.matrix==saved.matrix && output.inputOffsets==saved.inputOffsets && output.outputOffsets==saved.outputOffsets);
        --binding.entry;
        for(uint32_t format:{0u,1u,2u,3u,UINT32_MAX}) {
            assert(tglPrepareOwnedVeboxCsc(binding,output,3,7,format,read,qualify,rx,invoke,matrixInvoke));
            for(size_t row=0;row<3;++row) for(size_t column=0;column<3;++column)
                assert(output.matrix[row*3+column]==float(row*3+
                    ((format==1 || format==2) ? 2-column : column)));
            assert(output.inputOffsets==saved.inputOffsets && output.outputOffsets==saved.outputOffsets);
        }
        const auto prepared=output;allowed=false;
        assert(!tglPrepareOwnedVeboxCsc(binding,output,3,7,1,read,qualify,rx,invoke,matrixInvoke));
        assert(output.matrix==prepared.matrix && output.inputOffsets==prepared.inputOffsets &&
            output.outputOffsets==prepared.outputOffsets);
        struct NativeMock {
            decltype(invoke)& coefficientsCall;
            decltype(matrixInvoke)& matrixCall;
            void coefficients(uintptr_t p,uint32_t a,uint32_t b,float* m,float* in,float* out) noexcept {
                coefficientsCall(p,a,b,m,in,out);
            }
            bool matrix(uintptr_t p,uint32_t a,uint32_t b,float* out) noexcept {
                return matrixCall(p,a,b,out);
            }
        } native{invoke,matrixInvoke};
        allowed=true;
        TglOwnedCscProducer<decltype(read),decltype(qualify),decltype(rx),NativeMock>
            producer(base,read,qualify,rx,native);
        const auto beforeCalls=calls;
        assert(producer(output,3,7,2) && calls==beforeCalls+1);
        const auto complete=output;
        supported=false;
        assert(!producer(output,3,7,2) && calls==beforeCalls+1);
        assert(output.matrix==complete.matrix && output.inputOffsets==complete.inputOffsets &&
            output.outputOffsets==complete.outputOffsets);
        supported=true;allowed=false;
        assert(!producer(output,3,7,2) && calls==beforeCalls+1);
        allowed=true;
    }
    {
        constexpr uintptr_t image=0x100000;
        const std::array<uint8_t,35> anchor{
            0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x20,0x89,0x7d,0xfc,
            0x89,0x75,0xf8,0x48,0x89,0x55,0xf0,0xc6,0x45,0xef,0,
            0x8b,0x7d,0xfc,0xbe,0xff,0xff,0xff,0xff,0xe8,0xfd,0xe9,0xff,0xff};
        bool admitted=true,rx=true,readable=true,nativeOk=true;unsigned calls=0;
        auto bytes=anchor;
        auto read=[&](uintptr_t p,void* out,size_t n) noexcept {
            if(!readable || p!=image+0x54950 || n!=bytes.size()) return false;
            std::memcpy(out,bytes.data(),n);return true;
        };
        auto qualify=[&](uintptr_t p) noexcept {return admitted && p==image;};
        auto executable=[&](uintptr_t p,size_t n) noexcept {
            return rx && p==image+0x54950 && n==anchor.size();
        };
        auto invoke=[&](uintptr_t p,uint32_t from,uint32_t to,float* out) noexcept {
            assert(p==image+0x54950 && from==3 && to==7);++calls;
            for(unsigned i=0;i<12;++i) out[i]=float(i);
            return nativeOk;
        };
        TglCscMatrixBinding binding;assert(binding.resolve(image,read,qualify,executable));
        std::array<uint32_t,12> output;output.fill(0xabcdef);
        auto produce=[&]() {return tglProduceNativeCscMatrix(binding,output,3,7,
            read,qualify,executable,invoke);};
        nativeOk=false;assert(!produce() && calls==1);
        for(auto word:output) assert(word==0xabcdef);
        nativeOk=true;assert(produce() && calls==2);
        for(unsigned i=0;i<12;++i) {float value=float(i);uint32_t bits;
            std::memcpy(&bits,&value,4);assert(output[i]==bits);}
        const auto saved=output;
        for(size_t i=0;i<bytes.size();++i) {
            bytes=anchor;bytes[i]^=1;assert(!produce() && output==saved && calls==2);
        }
        bytes=anchor;rx=false;assert(!produce());rx=true;
        readable=false;assert(!produce());readable=true;
        admitted=false;assert(!produce());admitted=true;
        ++binding.entry;assert(!produce() && calls==2 && output==saved);
    }
    {
        const std::array<uint32_t,9> input{0x7fc01234,0x80000000,0x3f800000,
            0xff800000,0x7f800000,0x00000001,0x12345678,0x87654321,0xffffffff};
        for(uint32_t format:{0u,1u,2u,3u,UINT32_MAX}) {
            auto output=input;
            tglOwnedVeboxSwapCscRedBlue(output,format);
            for(size_t row=0;row<3;++row) for(size_t column=0;column<3;++column)
                assert(output[row*3+column]==input[row*3+
                    ((format==1 || format==2) ? 2-column : column)]);
            tglOwnedVeboxSwapCscRedBlue(output,format);
            assert(output==input);
        }
    }
    {
        struct Frame {
            TglOwnedVeboxProducerPass pass;
            int surfaces(bool,void*) noexcept {return 11;}
            int state(bool,void*) noexcept {return 12;}
            int diIecp(bool,void*) noexcept {return 13;}
            int dndi(void*,void*,void*) noexcept {return 14;}
        } frame;
        struct Execution {} execution;
        struct Child {
            bool leased=false;unsigned calls=0;
            int setupKernelScratch(Execution&,int32_t index) noexcept {
                if(!leased) return 5;
                ++calls;return index==3 ? 31 : 5;
            }
        } child;
        TglOwnedVeboxKernelFrameContext<Frame,Child,Execution> context(frame,child,execution);
        assert(&context.pass==&frame.pass);
        assert(context.setupKernel(3)==5 && child.calls==0);
        child.leased=true;
        assert(context.setupKernel(3)==31 && child.calls==1);
        assert(context.setupKernel(-1)==5 && child.calls==2);
        assert(context.surfaces(false,nullptr)==11 && context.state(false,nullptr)==12 &&
            context.diIecp(false,nullptr)==13 && context.dndi(nullptr,nullptr,nullptr)==14);
        child.leased=false;
        assert(context.setupKernel(3)==5 && child.calls==2);
    }
    {
        std::array<uint8_t,0x1bc0> child;
        std::array<uint8_t,0xd48> execution;
        for(int index=-1;index<=8;++index) {
            child.fill(0xa5);execution.fill(0x5a);
            const int result=tglClearOwnedVeboxKernelScratch(child.data(),child.size(),
                execution.data(),execution.size(),index);
            const bool valid=index>=0 && index<8;
            assert(result==(valid ? 31 : 5));
            for(size_t i=0;i<child.size();++i)
                assert(child[i]==(valid && i>=0x118 && i<0x1a8 ? 0 : 0xa5));
            for(size_t i=0;i<execution.size();++i)
                assert(execution[i]==(valid && i>=0xa50+size_t(index)*0x58 &&
                    i<0xa50+size_t(index+1)*0x58 ? 0 : 0x5a));
        }
        child.fill(0xa5);execution.fill(0x5a);
        assert(tglClearOwnedVeboxKernelScratch(child.data(),0x1a7,execution.data(),execution.size(),0)==5);
        assert(tglClearOwnedVeboxKernelScratch(child.data(),child.size(),execution.data(),0xd0f,0)==5);
        assert(tglClearOwnedVeboxKernelScratch(child.data(),child.size(),child.data(),child.size(),0)==5);
        assert(tglClearOwnedVeboxKernelScratch(nullptr,child.size(),execution.data(),execution.size(),0)==5);
        for(auto byte:child) assert(byte==0xa5);
        for(auto byte:execution) assert(byte==0x5a);
    }
    {
        uintptr_t begin=0x1000;size_t extent=0x1000;uint32_t protection=3;
        bool readable=true;unsigned queries=0;
        const auto region=[&](uintptr_t,uintptr_t& base,size_t& size,uint32_t& prot) noexcept {
            ++queries;if(!readable) return false;
            base=begin;size=extent;prot=protection;return true;
        };
        assert(tglAdmitDndiStackPackets(0x2000,0x1000,0x1100,0x30,0x1140,0x24,region));
        assert(queries==2);
        for(protection=0;protection<8;++protection)
            assert(tglAdmitDndiStackPackets(0x2000,0x1000,0x1100,0x30,0x1140,0x24,region)==(protection==3));
        protection=3;
        const auto before=queries;
        assert(!tglAdmitDndiStackPackets(0x2000,0x1000,0x1100,0x30,0x1120,0x24,region));
        assert(!tglAdmitDndiStackPackets(0x2000,0x1000,0x1101,0x30,0x1140,0x24,region));
        assert(!tglAdmitDndiStackPackets(0x2000,0x1000,0xffc,0x30,0x1140,0x24,region));
        assert(!tglAdmitDndiStackPackets(0x2000,0x1000,0x1fe0,0x30,0x1140,0x24,region));
        assert(!tglAdmitDndiStackPackets(0x2000,0x2001,0x1100,0x30,0x1140,0x24,region));
        assert(!tglAdmitDndiStackPackets(0x2000,0x1000,0x1100,0x2f,0x1140,0x24,region));
        assert(!tglAdmitDndiStackPackets(UINTPTR_MAX,0x1000,UINTPTR_MAX-15,0x30,0x1140,0x24,region));
        assert(queries==before);
        readable=false;
        assert(!tglAdmitDndiStackPackets(0x2000,0x1000,0x1100,0x30,0x1140,0x24,region));
        readable=true;begin=0x1104;
        assert(!tglAdmitDndiStackPackets(0x2000,0x1000,0x1100,0x30,0x1140,0x24,region));
        begin=0x1000;extent=0x110;
        assert(!tglAdmitDndiStackPackets(0x2000,0x1000,0x1100,0x30,0x1140,0x24,region));
        extent=SIZE_MAX;
        assert(!tglAdmitDndiStackPackets(0x2000,0x1000,0x1100,0x30,0x1140,0x24,region));
    }
    {
        assert(tglOwnedDnStrength(-1)==64 && tglOwnedDnStrength(-0.5f)==0);
        assert(tglOwnedDnStrength(63.9f)==63 && tglOwnedDnStrength(65)==64);
#if defined(__x86_64__)
        uint32_t bits=0x12345678;
        for(unsigned i=0;i<4096;++i) {
            bits=bits*1664525u+1013904223u;
            float value;std::memcpy(&value,&bits,4);
            int64_t native;
            asm volatile("cvttss2si %1, %0" : "=r"(native) : "x"(value));
            const uint32_t low=static_cast<uint32_t>(native);
            assert(tglOwnedDnStrength(value)==(low>64 ? 64 : low));
        }
#endif
        TglOwnedSurfaceStorage source;
        uintptr_t params=0x4000;
        uint32_t width=1920,height=1080;
        std::memcpy(source.prefix.data()+0x90,&params,8);
        std::memcpy(source.prefix.data()+0xd8,&width,4);
        std::memcpy(source.prefix.data()+0xdc,&height,4);
        std::array<uint8_t,8> data{};bool readable=true;unsigned reads=0;
        const auto read=[&](uintptr_t address,void* output,size_t size) noexcept {
            ++reads;assert(address==params && size==8);
            if(!readable) return false;
            std::memcpy(output,data.data(),8);return true;
        };
        TglOwnedDndiSource admitted;
        float factor=32.75f;std::memcpy(data.data()+4,&factor,4);
        assert(tglAdmitOwnedDndiSource(admitted,source,true,params,read));
        assert(admitted.identity==reinterpret_cast<uintptr_t>(&source));
        assert(admitted.dimensions.width==1920 && admitted.dimensions.height==1080);
        assert(!admitted.automatic && admitted.strength==32);
        data[2]=0xff;
        assert(tglAdmitOwnedDndiSource(admitted,source,true,params,read));
        assert(admitted.automatic && admitted.strength==0);
        const auto saved=admitted;const auto before=reads;
        assert(!tglAdmitOwnedDndiSource(admitted,source,true,params+8,read) && reads==before);
        readable=false;
        assert(!tglAdmitOwnedDndiSource(admitted,source,true,params,read));
        assert(admitted.identity==saved.identity && admitted.automatic==saved.automatic &&
            admitted.strength==saved.strength);
        const auto after=reads;
        assert(tglAdmitOwnedDndiSource(admitted,source,false,0,read) && reads==after);
        assert(!admitted.automatic && admitted.strength==0);
    }
    {
        struct Context {
            TglOwnedVeboxProducerPass pass;
            unsigned calls=0;int result=0;
            int setupKernel(int32_t index) noexcept {
                assert(index==3);++calls;return result;
            }
            int csc(void* source,void* output) noexcept {
                assert(source && output);++calls;return result;
            }
            int dndi(void* source,void* luma,void* chroma) noexcept {
                assert(source && luma && chroma);++calls;return result;
            }
        } context;
        using Resolve=TglOwnedVeboxScopedContext<Context>;
        using Callback=TglOwnedVeboxProducerCallbacks<Resolve>;
        int child=0,source=0,foreign=0;
        TglOwnedDndiPackets packets;
        std::array<uintptr_t,20> hooks;hooks.fill(0x1234);
        hooks=tglBindOwnedVeboxDndiHook<Resolve>(hooks);
        for(size_t i=0;i<hooks.size();++i)
            assert(hooks[i]==(i==17 ? reinterpret_cast<uintptr_t>(&Callback::dndi) : 0x1234));
        using Abi=int(*)(void*,void*,void*,void*) noexcept;
        auto kernelHooks=tglBindOwnedVeboxKernelSetupHook<Resolve>(hooks);
        for(size_t i=0;i<hooks.size();++i)
            assert(kernelHooks[i]==(i==5 ? reinterpret_cast<uintptr_t>(&Callback::setupKernel) : hooks[i]));
        using KernelAbi=int(*)(void*,int32_t) noexcept;
        auto setup=reinterpret_cast<KernelAbi>(kernelHooks[5]);
        assert(setup(&child,3)==5);
        for(int status:{0,31,-7}) {
            Resolve scope(&child,context);context.result=status;
            const auto before=context.calls;
            assert(context.pass.withBuilder([&]() noexcept {
                assert(setup(&foreign,3)==5);
                assert(setup(&child,3)==status);
                return context.pass.gate([]() noexcept {return 0;});
            })==status);
            assert(context.calls==before+1);
        }
        auto callback=reinterpret_cast<Abi>(hooks[17]);
        auto cscHooks=tglBindOwnedVeboxCscHook<Resolve>(hooks);
        for(size_t i=0;i<hooks.size();++i)
            assert(cscHooks[i]==(i==14 ? reinterpret_cast<uintptr_t>(&Callback::csc) : hooks[i]));
        using CscAbi=void(*)(void*,void*,void*) noexcept;
        auto csc=reinterpret_cast<CscAbi>(cscHooks[14]);
        for(int status:{0,31,-7}) {
            Resolve scope(&child,context);context.result=status;
            const auto before=context.calls;
            assert(context.pass.withBuilder([&]() noexcept {
                csc(&foreign,&source,&foreign);
                csc(&child,&source,&foreign);
                return context.pass.gate([]() noexcept {return 0;});
            })==status);
            assert(context.calls==before+1);
        }
        {Resolve scope(&child,context);const auto before=context.calls;
            assert(context.pass.withBuilder([&]() noexcept {
                csc(&child,nullptr,&foreign);return 0;
            })==5);assert(context.calls==before);}
        assert(callback(&child,&source,packets.luma.data(),packets.chroma.data())==5);
        for(int result:{0,31,-7}) {
            Resolve scope(&child,context);assert(scope.bound());context.result=result;
            const auto before=context.calls;
            assert(context.pass.withBuilder([&]() noexcept {
                assert(callback(&foreign,&source,packets.luma.data(),packets.chroma.data())==5);
                assert(callback(&child,&source,packets.luma.data(),packets.chroma.data())==result);
                return context.pass.gate([]() noexcept {return 0;});
            })==result);
            assert(context.calls==before+1);
        }
        for(unsigned missing=0;missing<3;++missing) {
            Resolve scope(&child,context);const auto before=context.calls;
            assert(context.pass.withBuilder([&]() noexcept {
                assert(callback(&child,missing==0 ? nullptr : &source,
                    missing==1 ? nullptr : packets.luma.data(),
                    missing==2 ? nullptr : packets.chroma.data())==5);
                return context.pass.gate([]() noexcept {return 0;});
            })==5);
            assert(context.calls==before);
        }
    }
    {
        for(bool luma:{false,true}) for(bool chroma:{false,true})
        for(bool automatic:{false,true}) for(uint32_t strength=0;strength<=65;++strength) {
            TglOwnedDndiState state;state.words.fill(0xa5a5a5a5);
            TglOwnedDndiPackets packet;
            packet.luma.fill(0xa5a5a5a5);packet.chroma.fill(0xa5a5a5a5);
            tglOwnedDndiSetDn(state,packet,luma,chroma,automatic,strength);
            for(size_t i=0;i<60;++i)
                if(!luma || i<12 || i>=24) assert(state.words[i]==0xa5a5a5a5);
            for(size_t i=0;i<12;++i)
                if(!luma || i>=8) assert(packet.luma[i]==0xa5a5a5a5);
            for(size_t i=0;i<9;++i)
                if(!chroma || i==0 || (automatic && i>=3))
                    assert(packet.chroma[i]==0xa5a5a5a5);
            if(chroma) assert(packet.chroma[1]==8 && packet.chroma[2]==192);
            if(luma && automatic) {
                const std::array<uint32_t,8> expected{512,8,208,2048,512,2,128,192};
                assert(!std::memcmp(packet.luma.data(),expected.data(),sizeof(expected)));
            }
            if(luma && !automatic && strength>=64) {
                const std::array<uint32_t,8> expected{640,8,208,2304,640,2,128,192};
                const std::array<uint32_t,6> thresholds{384,576,896,1280,1920,2560};
                const std::array<uint32_t,6> weights{16,15,13,10,7,4};
                assert(!std::memcmp(packet.luma.data(),expected.data(),sizeof(expected)));
                assert(!std::memcmp(state.words.data()+12,thresholds.data(),sizeof(thresholds)));
                assert(!std::memcmp(state.words.data()+18,weights.data(),sizeof(weights)));
            }
        }
    }
    {
        TglOwnedDndiState state;
        state.words.fill(0xa5a5a5a5);
        const auto original=state.words;
        assert(tglOwnedDndiSetDi(state,false,nullptr)==0 && state.words==original);
        assert(tglOwnedDndiSetDi(state,true,nullptr)==5);
        auto fixed=original;
        const std::array<uint32_t,6> temporal{4,0,5,255,5,255};
        const std::array<uint32_t,10> checks{3,20,100,15,0,63,76,89,114,217};
        std::copy(temporal.begin(),temporal.end(),fixed.begin()+0x1f);
        fixed[0x25]=0xa5000000;fixed[0x26]=fixed[0x27]=0;
        fixed[0x28]=0x00010001;
        std::copy(checks.begin(),checks.end(),fixed.begin()+0x2a);
        assert(state.words==fixed);
        for(size_t i=0;i<0x1f;++i) assert(state.words[i]==original[i]);
        assert(state.words[0x25]==0xa5000000);
        assert(state.words[0x28]==0x00010001 && state.words[0x29]==original[0x29]);
        assert(state.words[0x2c]==100); // decimal native tearing-high value
        for(size_t i=0x34;i<60;++i) assert(state.words[i]==original[i]);
        for(uint32_t width:{767u,768u,769u})
        for(uint32_t height:{575u,576u,577u}) {
            state.words=original;
            const TglOwnedDiDimensions source{width,height};
            assert(tglOwnedDndiSetDi(state,true,&source)==0);
            const std::array<uint32_t,8> expected=width<=768 && height<=576 ?
                std::array<uint32_t,8>{0,0,0,128,128,128,255,255} :
                std::array<uint32_t,8>{0,0,0,0,32,64,128,255};
            assert(!std::memcmp(state.words.data()+0x34,expected.data(),sizeof(expected)));
            auto complete=fixed;
            std::copy(expected.begin(),expected.end(),complete.begin()+0x34);
            assert(state.words==complete);
            for(size_t i=0;i<0x1f;++i) assert(state.words[i]==original[i]);
            assert(state.words[0x25]==0xa5000000 && state.words[0x29]==original[0x29]);
        }
    }
    {
        int child=0;
        std::array<uint32_t,10> packet;packet.fill(0xa5a5a5a5);
        tglOwnedVeboxLumaDefaults(&child,packet.data()+1);
        const std::array<uint32_t,8> expected{512,8,208,2048,512,2,128,192};
        assert(packet.front()==0xa5a5a5a5 && packet.back()==0xa5a5a5a5);
        assert(!std::memcmp(packet.data()+1,expected.data(),sizeof(expected)));
        const auto saved=packet;
        tglOwnedVeboxLumaDefaults(nullptr,packet.data()+1);
        tglOwnedVeboxLumaDefaults(&child,nullptr);
        assert(packet==saved);
        TglOwnedDndiPackets dndi;
        tglOwnedVeboxLumaDefaults(&child,dndi.luma.data());
        assert(!std::memcmp(dndi.luma.data(),expected.data(),sizeof(expected)));
        for(size_t i=8;i<dndi.luma.size();++i) assert(dndi.luma[i]==0);
        for(auto value:dndi.chroma) assert(value==0);
        dndi.luma.fill(0xa5a5a5a5);
        dndi.chroma.fill(0x5a5a5a5a);
        tglOwnedDndiClearNativeTail(dndi);
        for(size_t i=0;i<10;++i) assert(dndi.luma[i]==0xa5a5a5a5);
        assert(dndi.luma[10]==0 && dndi.luma[11]==0);
        for(auto value:dndi.chroma) assert(value==0x5a5a5a5a);
    }
    {
        using Reason=TglVeboxSlotReuseReason;
        for(unsigned pending=0;pending<256;++pending)
        for(uint32_t completed:{0u,1u,0x7fffffffu,0x80000000u,0xfffffffeu,0xffffffffu})
        for(uint32_t tag:{0u,1u,0x7fffffffu,0x80000000u,0xfffffffeu,0xffffffffu})
        for(uint32_t policy:{0u,1u,1u<<13,1u<<20,(1u<<13)|(1u<<20),0xffffffffu}) {
            const uint32_t delta=completed-tag;
            const int64_t signedDelta=delta<0x80000000u ? int64_t(delta) : int64_t(delta)-0x100000000LL;
            const bool reset=(policy&((1u<<20)|(1u<<13)))!=0;
            const auto result=tglVeboxSlotReuseReason(uint8_t(pending),completed,tag,policy);
            const auto expected=!(pending&1) ? Reason::Idle : signedDelta>=0 ? Reason::TagReached :
                reset ? Reason::ResetDiscarded : Reason::Pending;
            assert(result==expected);
            // Exactly the native pending-retention predicate, while retaining
            // reset provenance instead of silently turning it into completion.
            assert((result==Reason::Pending)==bool((pending&1) && signedDelta<0 && !reset));
        }
    }
    {
        using Decision=TglOwnedFfdnHistoryDecision;
        for(unsigned mask=0;mask<32;++mask) {
            const bool changed=mask&1,valid=mask&2,flag13=mask&4,previous=mask&8,equal=mask&16;
            const auto decision=tglOwnedFfdnHistoryDecision(changed,valid,flag13,previous,
                128,equal?128:256);
            assert(decision==(!changed?Decision::Keep:
                valid && flag13 && previous && equal?Decision::CopyPrevious:Decision::Invalidate));
        }
        assert(tglOwnedFfdnHistoryDecision(true,true,true,true,0,0)==Decision::CopyPrevious);
        for(unsigned mask=0;mask<128;++mask) {
            const bool a=mask&1,b=mask&2,c=mask&4,d=mask&8,
                e=mask&16,f=mask&32,g=mask&64;
            // Follow the native branch sequence independently of the helper.
            bool source=false;
            if(a && !b && c) source=true;
            else if(e) source=false;
            else if(g) source=true;
            else if(!f) source=true;
            const auto selected=tglSelectOwnedFfdnCompression(a,b,c,d,e,f,g,4);
            assert(selected.compressed==(source?d:g));
            assert(selected.mode==(source?(e?1u:0u):4u));
        }
    }
    {
        unsigned calls=0;
        for(uint32_t id:{0xb8u,0xb9u}) for(int status:{0,5,31}) {
            auto query=[&](uintptr_t context,uint32_t key,void* data) noexcept {
                assert(!context && key==id); ++calls;
                std::array<uint8_t,0x28> expected{};
                if(id==0xb8) {expected[0]=1;expected[0x20]=1;}
                assert(!std::memcmp(data,expected.data(),expected.size()));
                const uint32_t value=77;
                std::memcpy(data,&value,4); return status;
            };
            const auto result=tglReadOwnedVeboxFeature(id,query);
            assert(result.status==status && result.value==77);
            const auto rejected=tglReadOwnedVeboxFeature(0xba,query);
            assert(rejected.status==5 && !rejected.value);
        }
        assert(calls==6);
        constexpr uintptr_t image=0x100000;
        bool admitted=true,readable=true,mutated=false;
        auto read=[&](uintptr_t entry,void* output,size_t size) noexcept {
            assert(entry==image+0x44620 && size==16);
            uint8_t bytes[]={0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x20,
                0x48,0x89,0x7d,0xf8,0x89,0x75,0xf4,0x48};
            if(mutated) bytes[15]^=1;
            std::memcpy(output,bytes,16);return readable;
        };
        auto qualify=[&](uintptr_t value) noexcept {return admitted && value==image;};
        auto executable=[](uintptr_t entry,size_t size) noexcept {
            return entry==image+0x44620 && size==16;
        };
        auto invoke=[&](uintptr_t entry,uintptr_t context,uint32_t id,void* data) noexcept {
            assert(entry==image+0x44620 && !context && id==0xb8 && data);
            ++calls;return 31;
        };
        for(unsigned failure=0;failure<4;++failure) {
            admitted=failure!=1;readable=failure!=2;mutated=failure==3;
            TglNativeVeboxFeatureQuery<decltype(invoke)> query(image,read,qualify,executable,invoke);
            assert(query.bound()==(failure==0));
            const auto result=tglReadOwnedVeboxFeature(0xb8,query);
            assert(result.status==(failure==0?31:5) && result.value==1);
            assert(query(1,0xb8,&calls)==5 && query(0,0xba,&calls)==5);
        }
        assert(calls==7);
        bool badSlot=false,badAnchor=false;
        auto reportRead=[&](uintptr_t address,void* output,size_t size) noexcept {
            if(address==image+0x757ce0 && size==8) {
                const uintptr_t slot=image+0x12ea30+(badSlot?1:0);
                std::memcpy(output,&slot,8);return true;
            }
            if(address!=image+0x12ea30 || size!=32) return false;
            std::array<uint8_t,32> bytes{
                0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x10,0x48,0x89,0x7d,0xf8,0x48,0x8b,0x45,0xf8,
                0x48,0x83,0xb8,0x88,0,0,0,0,0x48,0x89,0x45,0xf0,0x0f,0x85,0x0d,0};
            if(badAnchor) bytes[31]^=1;
            std::memcpy(output,bytes.data(),32);return true;
        };
        auto reportExecutable=[](uintptr_t address,size_t size) noexcept {
            return address==image+0x12ea30 && size==32;
        };
        unsigned reportCalls=0;
        auto reportInvoke=[&](uintptr_t address,void* child) noexcept -> uintptr_t {
            assert(address==image+0x12ea30 && child==&calls); ++reportCalls; return 0x5678;
        };
        for(unsigned failure=0;failure<4;++failure) {
            admitted=failure!=1;badSlot=failure==2;badAnchor=failure==3;
            TglNativeVeboxReportQuery<decltype(reportInvoke)> query(
                image,reportRead,qualify,reportExecutable,reportInvoke);
            assert(query.bound()==(failure==0));
            assert(query.image()==(failure==0?image:0));
            assert(!query(nullptr));
            assert(query(&calls)==(failure==0?0x5678u:0u));
        }
        assert(reportCalls==1);
    }
    {
        std::array<uint8_t,0x1bc0> child;
        TglOwnedVeboxAllocationTail tail{17,0,23,31,true,0x12345678};
        child.fill(0xa5);
        const auto untouched=child;
        assert(!tail.write(nullptr,child.size()));
        assert(!tail.write(child.data(),child.size()-1) && child==untouched);
        for(unsigned bit=0;bit<32;++bit) {
            child.fill(0xa5); tail.osFlags=1u<<bit;
            assert(tail.write(child.data(),child.size()));
            assert(child[0x1ba8]==uint8_t(bit==13 || bit==20));
            assert(!child[0x1ba9] && !child[0x1baa] && !child[0x1bab]);
            assert(child[0x30]==1 && child[0x1b74]==1);
            uint32_t feature=0,input08=0,input10=0,a=0,b=0;
            uintptr_t borrowed=0;
            std::memcpy(&feature,child.data()+0x1b9c,4);
            std::memcpy(&input08,child.data()+0x1a8,4);
            std::memcpy(&input10,child.data()+0x1b98,4);
            std::memcpy(&a,child.data()+0x1b78,4);
            std::memcpy(&b,child.data()+0x1b7c,4);
            std::memcpy(&borrowed,child.data()+0x70,8);
            assert(feature==17 && input08==23 && input10==31 && borrowed==0x12345678);
            assert(a==0xfffffc00 && b==a);
            for(size_t i=0;i<child.size();++i) {
                const bool written=i==0x30 || (i>=0x70 && i<0x78) ||
                    (i>=0x1a8 && i<0x1ac) || i==0x1b74 ||
                    (i>=0x1b78 && i<0x1b80) || (i>=0x1b98 && i<0x1ba0) ||
                    (i>=0x1ba8 && i<0x1bac);
                if(!written) assert(child[i]==0xa5);
            }
        }
        tail.input0c=false;tail.osFlags=0;tail.borrowed70=0;
        assert(tail.write(child.data(),child.size()) && !child[0x30] && !child[0x1ba8]);
    }
    {
        struct TransferBackend {
            std::array<bool,8> live{};
            unsigned allocations = 0, releases = 0;
            uintptr_t allocate(size_t n) {
                assert(n == 0x2a8 && allocations < 8);
                live[allocations] = true; return ++allocations;
            }
            void release(uintptr_t p) {
                assert(p && p <= 8 && live[p-1]);
                live[p-1] = false; ++releases;
            }
        } backend;
        std::array<uintptr_t,8> nativeFields{};
        {
            TglVeboxCpuShells<TransferBackend> shells(backend);
            assert(shells.allocate());
            std::array<uint8_t,0x1bc0> storage{};
            std::array<uint8_t,0x100> small{};
            assert(!shells.installIntoPrivateBase(small));
            assert(!shells.installIntoPrivateBase(nullptr,0x1bc0));
            assert(!shells.installIntoPrivateBase(storage.data(),storage.size()-1));
            assert(!shells.installIntoPrivateBase(
                reinterpret_cast<uint8_t*>(UINTPTR_MAX-0x100),storage.size()));
            storage[0x698]=1;
            const auto saved=storage;
            assert(!shells.installIntoPrivateBase(storage) && storage==saved && shells.get(0)==1);
            storage[0x698]=0;
            assert(shells.installIntoPrivateBase(storage));
            constexpr size_t offsets[8]={0x1b0,0x1b8,0xba8,0xbb0,0x680,0x688,0x690,0x698};
            for (size_t i=0;i<8;++i) std::memcpy(&nativeFields[i],storage.data()+offsets[i],8);
            const auto installed=storage;
            assert(!shells.installIntoPrivateBase(storage) && storage==installed);
            for (size_t i = 0; i < 8; ++i) {
                assert(nativeFields[i] == i+1 && !shells.get(i));
            }
            const auto emptyTransfer = shells.relinquishToNative();
            for (auto p : emptyTransfer) assert(!p);
            shells.reset(); assert(!backend.releases);
        }
        assert(!backend.releases); // Only the native owner now has free authority.
        for (auto p : nativeFields) backend.release(p); // Native field order, not rollback order.
        assert(backend.releases == 8);
        for (auto live : backend.live) assert(!live);
    }
    {
        constexpr uintptr_t image = 0x100000, renderHal = 0x900000;
        uintptr_t callback = image+0x1234;
        bool identity = true, rx = true, readable = true;
        auto read = [&](uintptr_t p, void* out, size_t n) {
            assert(p == renderHal+0xa30 && n == 8);
            if (!readable) return false;
            std::memcpy(out,&callback,8); return true;
        };
        auto qualify = [&](uintptr_t p, uintptr_t base) {
            return identity && p == renderHal && base == image;
        };
        auto executable = [&](uintptr_t p, size_t n) { return rx && p == callback && n == 16; };
        TglBatchReleaseBinding b;
        assert(b.resolve(renderHal,image,read,qualify,executable));
        assert(b.renderHal == renderHal && b.entry == callback);
        identity = false; assert(!b.resolve(renderHal,image,read,qualify,executable));
        assert(!b.entry && !b.renderHal); identity = true;
        rx = false; assert(!b.resolve(renderHal,image,read,qualify,executable)); rx = true;
        readable = false; assert(!b.resolve(renderHal,image,read,qualify,executable)); readable = true;
        callback = image; assert(!b.resolve(renderHal,image,read,qualify,executable));
        callback = image+0x23e03e; assert(!b.resolve(renderHal,image,read,qualify,executable));
        assert(!b.resolve(UINTPTR_MAX,image,read,qualify,executable));
        auto release = +[](void* context, void* batch) -> uint32_t {
            assert(context == reinterpret_cast<void*>(uintptr_t(0x1234)));
            assert(batch == reinterpret_cast<void*>(uintptr_t(0x5678)));
            return 31;
        };
        assert(TglNativeBatchReleaseInvoker{}(reinterpret_cast<uintptr_t>(release),0x1234,0x5678) == 31);
    }
    struct ShellBackend {
        size_t calls = 0, failAt = 8, live = 0;
        uintptr_t allocate(size_t bytes) {
            assert(bytes == 0x2a8);
            if (calls++ == failAt) return 0;
            ++live; return live;
        }
        void release(uintptr_t p) { assert(p == live && live); --live; }
    };
    for (size_t fail = 0; fail <= 8; ++fail) {
        ShellBackend b; b.failAt = fail;
        {
            TglVeboxCpuShells<ShellBackend> shells(b);
            assert(shells.allocate() == (fail == 8));
            assert(!shells.get(8));
            if (fail < 8) {
                assert(!b.live);
                for (size_t i = 0; i < 8; ++i) assert(!shells.get(i));
                b.calls = 0; b.failAt = 8;
                assert(shells.allocate());
            }
            for (size_t i = 0; i < 8; ++i) assert(shells.get(i) == i+1);
            const auto calls = b.calls;
            assert(!shells.allocate() && b.calls == calls);
            shells.reset(); shells.reset(); assert(!b.live);
            b.calls = 0; assert(shells.allocate());
        }
        assert(!b.live); // Destructor releases the complete unpublished group.
    }
    // Exercise the real function-pointer ABI only against owned test functions.
    static std::array<unsigned char,0x2a8> abiStorage{};
    static unsigned abiAllocations = 0, abiReleases = 0;
    auto allocateAbi = +[](size_t bytes) -> void* {
        assert(bytes == abiStorage.size()); ++abiAllocations;
        return abiStorage.data();
    };
    auto releaseAbi = +[](void* pointer) {
        assert(pointer == abiStorage.data()); ++abiReleases;
    };
    TglNativeTrackedCpuInvoker abi;
    assert(!abi.allocate(0,0x2a8));
    assert(!abi.allocate(reinterpret_cast<uintptr_t>(allocateAbi),0));
    abi.release(0,reinterpret_cast<uintptr_t>(abiStorage.data()));
    abi.release(reinterpret_cast<uintptr_t>(releaseAbi),0);
    assert(abiAllocations == 0 && abiReleases == 0);
    auto pointer = abi.allocate(reinterpret_cast<uintptr_t>(allocateAbi),0x2a8);
    assert(pointer == reinterpret_cast<uintptr_t>(abiStorage.data()) && abiAllocations == 1);
    abi.release(reinterpret_cast<uintptr_t>(releaseAbi),pointer);
    assert(abiReleases == 1);
    struct Backend {
        std::array<unsigned char,0x2a8> bytes{};
        unsigned allocations = 0, releases = 0, live = 0;
        bool fail = false;
        uintptr_t allocate(size_t n) {
            ++allocations; assert(n == bytes.size());
            if (fail) return 0;
            bytes.fill(0); ++live; return reinterpret_cast<uintptr_t>(bytes.data());
        }
        void release(uintptr_t p) {
            assert(p == reinterpret_cast<uintptr_t>(bytes.data()) && live == 1);
            ++releases; --live;
        }
    } backend;
    {
        TglTrackedCpuOwner<Backend> owner(backend);
        assert(!owner.allocate(0) && backend.allocations == 0);
        backend.fail = true; assert(!owner.allocate(0x2a8));
        assert(!owner.get() && !owner.size() && !backend.live && !backend.releases);
        backend.fail = false; assert(owner.allocate(0x2a8));
        assert(owner.get() && owner.size() == 0x2a8 && backend.live == 1);
        const auto attempts = backend.allocations;
        assert(!owner.allocate(0x2a8) && backend.allocations == attempts);
        for (auto byte : backend.bytes) assert(byte == 0);
        owner.reset(); owner.reset();
        assert(backend.releases == 1 && !backend.live && !owner.get() && !owner.size());
        assert(owner.allocate(0x2a8));
    }
    assert(backend.releases == 2 && backend.live == 0);
    constexpr uintptr_t image = 0x1000000;
    std::array<uint8_t,16> alloc{0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x10,
        0x48,0x89,0x7d,0xf8,0x48,0x8b,0x7d,0xf8};
    auto freed = alloc; freed[13] = 0x83;
    bool identity = true, rx = true;
    auto read = [&](uintptr_t p, void* out, size_t n) {
        assert(n == 16);
        if (p != image+0x437e0 && p != image+0x436d0) return false;
        std::memcpy(out,p == image+0x437e0 ? alloc.data() : freed.data(),n); return true;
    };
    auto qualify = [&](uintptr_t p) { return identity && p == image; };
    auto executable = [&](uintptr_t, size_t) { return rx; };
    TglTrackedCpuBinding binding;
    assert(binding.resolve(image,read,qualify,executable));
    alloc[0] = 0; assert(!binding.resolve(image,read,qualify,executable)); alloc[0] = 0x55;
    freed[0] = 0; assert(!binding.resolve(image,read,qualify,executable)); freed[0] = 0x55;
    identity = false; assert(!binding.resolve(image,read,qualify,executable)); identity = true;
    rx = false; assert(!binding.resolve(image,read,qualify,executable));
    assert(!binding.image && !binding.allocate && !binding.release);
    struct Invoke {
        Backend* backend;
        uintptr_t allocate(uintptr_t entry, size_t n) {
            assert(entry == 0x1000000+0x437e0); return backend->allocate(n);
        }
        void release(uintptr_t entry, uintptr_t p) {
            assert(entry == 0x1000000+0x436d0); backend->release(p);
        }
    };
    TglTrackedCpuBackend<Invoke> denied(image,read,qualify,executable,Invoke{&backend});
    assert(!denied.bound());
    const auto attempts = backend.allocations;
    assert(!denied.allocate(0x2a8) && backend.allocations == attempts);
    rx = true;
    TglTrackedCpuBackend<Invoke> native(image,read,qualify,executable,Invoke{&backend});
    assert(native.bound());
    {
        TglTrackedCpuOwner<TglTrackedCpuBackend<Invoke>> owner(native);
        assert(owner.allocate(0x2a8) && backend.live == 1);
    }
    assert(backend.live == 0 && backend.releases == 3);
    {
        TglTrackedCpuOwner<Backend> owner(backend);
        std::array<uint8_t,10> field{};
        field.front()=0xa5;field.back()=0x5a;
        auto* slot=field.data()+1; // deliberately unaligned source pointer field
        unsigned calls=0;
        assert(!owner.withNativeReleaseSlot(slot,[&]() noexcept { ++calls; }));
        assert(owner.allocate(0x2a8));
        const auto original=owner.get();
        uintptr_t foreign=123;
        std::memcpy(slot,&foreign,8);
        assert(!owner.withNativeReleaseSlot(slot,[&]() noexcept { ++calls; }));
        uintptr_t empty=0;std::memcpy(slot,&empty,8);
        assert(owner.withNativeReleaseSlot(slot,[&]() noexcept {
            ++calls;
            uintptr_t published=0;std::memcpy(&published,slot,8);
            assert(published==original && backend.live==1);
            owner.reset();assert(owner.get()==original && backend.live==1);
            assert(!owner.allocate(8));
            assert(!owner.withNativeReleaseSlot(slot,[&]() noexcept { ++calls; }));
        }));
        assert(owner.get()==original && owner.size()==0x2a8 && calls==1);
        uintptr_t detached=1;std::memcpy(&detached,slot,8);assert(!detached);
        assert(owner.withNativeReleaseSlot(slot,[&]() noexcept {
            ++calls;backend.release(original);std::memcpy(slot,&empty,8);
        }));
        assert(!owner.get() && !owner.size() && backend.live==0 && calls==2);
        owner.reset();assert(backend.releases==4);
        assert(owner.allocate(0x2a8));
        assert(!owner.withNativeReleaseSlot(slot,[&]() noexcept {
            std::memcpy(slot,&foreign,8);
        }));
        std::memcpy(&detached,slot,8);
        assert(!detached && owner.get() && backend.live==1);
        owner.reset();assert(backend.releases==5);
        assert(field.front()==0xa5 && field.back()==0x5a);
    }
    assert(backend.releases==5 && backend.live==0);
}

static void testOwnedVeboxVtable() {
    struct FormatRead {
        bool operator()(uintptr_t address,void* out,size_t size) const noexcept {
            if(size!=4) return false;
            std::memcpy(out,reinterpret_cast<void*>(address),size);return true;
        }
    };
    struct DeniedRead {
        bool operator()(uintptr_t,void*,size_t) const noexcept {return false;}
    };
    static_assert(!std::is_copy_constructible_v<TglOwnedVeboxVtable>);
    static_assert(!std::is_move_constructible_v<TglOwnedVeboxVtable>);
    static_assert(!std::is_copy_assignable_v<TglOwnedVeboxVtable>);
    static_assert(!std::is_move_assignable_v<TglOwnedVeboxVtable>);
    constexpr uintptr_t image = 0x1000000;
    std::array<uintptr_t,TglOwnedVeboxVtable::count> original{};
    for (size_t i = 0; i < original.size(); ++i) original[i] = image+0x2000+i*8;
    for (size_t offset : TglOwnedVeboxVtable::missing) original[offset/8] = 0;
    original[0] = image+0x12e240; original[1] = image+0x12e250;
    original[0x40/8]=image+0x12e9c0;original[0x48/8]=image+0x12ea30;
    original[0x170/8]=image+0x12aba0;original[0x180/8]=image+0x12b1d0;
    original[0x160/8]=image+0x12a740;original[0x168/8]=image+0x12ab10;
    original[0xd0/8]=image+0x71e80;
    original[0x1b8/8] = image+0x71e80;
    std::array<uintptr_t,20> hooks{};
    for (size_t i = 0; i < hooks.size(); ++i) hooks[i] = 0x2000+i*16;
    const auto unboundHooks=hooks;
    hooks=tglBindOwnedVeboxQueryHooks<FormatRead>(hooks);
    for(size_t i=0;i<hooks.size();++i)
        if(i!=8 && i!=9 && i!=10 && i!=11 && i!=12 && i!=13 && i!=15 && i!=16) assert(hooks[i]==unboundHooks[i]);
    assert(hooks[13]==reinterpret_cast<uintptr_t>(&tglOwnedVeboxUseKernelResource));
    assert(!reinterpret_cast<bool(*)(void*) noexcept>(hooks[13])(nullptr));
    assert(hooks[16]==reinterpret_cast<uintptr_t>(&tglOwnedVeboxLumaDefaults));
    std::array<uint32_t,8> hookLuma{};int hookChild=0;
    reinterpret_cast<void(*)(void*,uint32_t*) noexcept>(hooks[16])(&hookChild,hookLuma.data());
    assert((hookLuma==std::array<uint32_t,8>{512,8,208,2048,512,2,128,192}));
    bool identity = true, rx = true;
    std::array<uintptr_t,2> metadata{0,image+0x757ed0};
    bool metadataReadable = true;
    auto read = [&](uintptr_t p, void* out, size_t n) {
        if (p == image+0x757c88) {
            assert(n == 16);
            if (!metadataReadable) return false;
            std::memcpy(out,metadata.data(),n); return true;
        }
        assert(p == image+0x757c98 && n == 0x1e8); // never next table metadata
        std::memcpy(out,original.data(),n); return true;
    };
    auto qualify = [&](uintptr_t p) { return identity && p == image; };
    auto executable = [&](uintptr_t, size_t n) { assert(n == 1); return rx; };
    TglOwnedVeboxVtable table;
    auto build = [&] { return table.build(image,hooks,read,qualify,executable); };
    assert(build());
    assert(table.addressPoint() == reinterpret_cast<uintptr_t>(table.entries.data()));
    assert(table.typeInfo == image+0x757ed0 && table.offsetToTop == 0);
    // Invoke the actual owned callback through its installed ABI slot, not a
    // fake test address. The query must not mutate the private child bytes.
    std::array<uint8_t,32> childBytes{};
    childBytes.fill(0xa5);
    const auto beforeQuery = childBytes;
    using CscSupport = bool(*)(void*) noexcept;
    const auto support = reinterpret_cast<CscSupport>(table.entries[0x1b8/8]);
    assert(support(childBytes.data()) && childBytes == beforeQuery);
    using StatisticsQuery=int(*)(void*,uint32_t,uint32_t*) noexcept;
    const auto statistics=reinterpret_cast<StatisticsQuery>(table.entries[0x80/8]);
    for(uint32_t selector=0;selector<8;++selector) {
        uint32_t expected=0xa5a5a5a5,actual=expected;
        const int result=tglStatisticsQuery(selector,expected);
        assert(statistics(childBytes.data(),selector,&actual)==result && actual==expected);
    }
    uint32_t sentinel=0x12345678;
    assert(statistics(nullptr,5,&sentinel)==5 && sentinel==0x12345678);
    assert(statistics(childBytes.data(),5,nullptr)==5 && childBytes==beforeQuery);
    std::array<uint8_t,0x134> surface{};
    uint32_t format=0x0d;
    std::memcpy(surface.data()+0x130,&format,4);
    using SurfaceSupport=bool(*)(void*,uintptr_t) noexcept;
    const auto surfaceSupport=reinterpret_cast<SurfaceSupport>(table.entries[0x90/8]);
    assert(surfaceSupport(childBytes.data(),reinterpret_cast<uintptr_t>(surface.data())));
    format=0xdeadbeef;std::memcpy(surface.data()+0x130,&format,4);
    assert(!surfaceSupport(childBytes.data(),reinterpret_cast<uintptr_t>(surface.data())));
    assert(!surfaceSupport(nullptr,reinterpret_cast<uintptr_t>(surface.data())));
    assert(!surfaceSupport(childBytes.data(),0));
    assert(!surfaceSupport(childBytes.data(),UINTPTR_MAX-0x132));
    assert(!tglOwnedVeboxSurfaceSupported<DeniedRead>(childBytes.data(),0x5000));
    using TwoSurfaces=bool(*)(void*,uintptr_t,uintptr_t) noexcept;
    TwoSurfaces rt=reinterpret_cast<TwoSurfaces>(table.entries[0x98/8]);
    SurfaceSupport dn=reinterpret_cast<SurfaceSupport>(table.entries[0xa0/8]);
    SurfaceSupport di=reinterpret_cast<SurfaceSupport>(table.entries[0xa8/8]);
    std::array<uint8_t,0x134> source{};
    uint32_t sourceFormat=0x53,sourceColor=0x0b,targetFormat=0x03;
    std::memcpy(source.data(),&sourceColor,4);
    std::memcpy(source.data()+0x130,&sourceFormat,4);
    std::memcpy(surface.data()+0x130,&targetFormat,4);
    const auto src=reinterpret_cast<uintptr_t>(source.data());
    const auto dst=reinterpret_cast<uintptr_t>(surface.data());
    const auto sourceBefore=source,surfaceBefore=surface;
    assert(rt(childBytes.data(),src,dst)); // BT2020 P010 -> permitted RGB
    assert(dn(childBytes.data(),src));
    assert(!di(childBytes.data(),dst)); // RGB excluded, not generic admission
    assert(source==sourceBefore && surface==surfaceBefore && childBytes==beforeQuery);
    sourceColor=0;std::memcpy(source.data(),&sourceColor,4);
    assert(!rt(childBytes.data(),src,dst));
    assert(!rt(nullptr,src,dst) && !rt(childBytes.data(),0,dst));
    assert(!dn(nullptr,src) && !dn(childBytes.data(),UINTPTR_MAX-0x132));
    assert(!di(childBytes.data(),0));
    using DeniedFormats=TglOwnedVeboxFormatCallbacks<DeniedRead>;
    assert(!DeniedFormats::renderTarget(childBytes.data(),src,dst));
    assert(!DeniedFormats::denoise(childBytes.data(),src));
    assert(!DeniedFormats::deinterlace(childBytes.data(),dst));
    metadata[0] = 8; assert(!build() && !table.addressPoint()); metadata[0] = 0;
    metadata[1] = image+0x757ee0; assert(!build() && !table.typeInfo);
    metadata[1] = image+0x757ed0;
    metadataReadable = false; assert(!build() && !table.addressPoint());
    metadataReadable = true; assert(build());
    for (size_t i = 0; i < original.size(); ++i) {
        auto expected = original[i];
        for (size_t j = 0; j < hooks.size(); ++j)
            if (TglOwnedVeboxVtable::required[j]/8 == i) expected = hooks[j];
        assert(table.entries[i] == expected);
    }
    for (size_t i = 0; i < hooks.size(); ++i) {
        auto saved = hooks[i]; hooks[i] = 0;
        assert(!build());
        for (auto value : table.entries) assert(value == 0);
        hooks[i] = saved;
    }
    hooks[0] = image+0x12e240; assert(!build()); hooks[0] = 0x2000;
    original[0x28/8] = image+0x1000; assert(!build()); original[0x28/8] = 0;
    original[0x48/8] = 0; assert(!build()); original[0x48/8] = image+0x12ea30;
    original[0x48/8] = 0x4000; assert(!build()); original[0x48/8] = image+0x12ea30;
    for(size_t slot:{size_t(0x40/8),size_t(0x48/8),size_t(0xd0/8),size_t(0x160/8),size_t(0x168/8),size_t(0x170/8),size_t(0x180/8)}) {
        const auto native=original[slot];
        original[slot]=image+0x2048;assert(!build() && !table.addressPoint());
        original[slot]=native;
    }
    assert(build());
    assert(table.entries[0x40/8]==image+0x12e9c0 && table.entries[0x48/8]==image+0x12ea30);
    assert(table.entries[0x170/8]==image+0x12aba0 && table.entries[0x180/8]==image+0x12b1d0);
    rx = false; assert(!build()); rx = true;
    identity = false; assert(!build()); identity = true;
    assert(build());
}

static void testNativeCscBinding() {
    constexpr uintptr_t image = 0x1000000, child = 0x2000, vt = 0x3000;
    std::array<uint8_t,16> code{0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x40,
        0x48,0x89,0x7d,0xf8,0x48,0x89,0x75,0xf0};
    std::array<std::pair<uintptr_t,uintptr_t>,11> memory{{
        {child,vt},{vt+0x48,image+0x12ea30},{vt+0x90,0x4000},{vt+0x1b8,0x5000},
        {child+0x88,0x6000},{0x6000,image+0x757e90},{0x6d38,0x7000},
        {0x6d40,0x8000},{0x7000,image+0x757f78},{0x8000,image+0x757fb8},{0x9000,0xb}}};
    auto read = [&](uintptr_t p, void* out, size_t n) {
        if (p == image+0x12cfc0 && n == 16) { std::memcpy(out,code.data(),n); return true; }
        for (const auto& entry : memory) if (p == entry.first && n <= 8) {
            std::memcpy(out,&entry.second,n); return true;
        }
        if (p == 0xa000 && n == 4) { uint32_t cs = 3; std::memcpy(out,&cs,4); return true; }
        return false;
    };
    bool identity = true, rx = true;
    auto qualify = [&](uintptr_t p) { return identity && p == image; };
    auto executable = [&](uintptr_t, size_t) { return rx; };
    TglTwoPassCscBinding binding;
    assert(binding.resolve(image,read,qualify,executable));
    unsigned calls = 0; bool nativeResult = false, needed = true;
    auto invoke = [&](uintptr_t e,uintptr_t c,uintptr_t s,uintptr_t t) {
        assert(e == image+0x12cfc0 && c == child && s == 0x9000 && t == 0xa000);
        ++calls; return nativeResult;
    };
    auto query = [&] {
        return tglQueryNativeTwoPassCsc(binding,child,0x9000,0xa000,vt,0x4000,0x5000,
            needed,read,qualify,executable,invoke);
    };
    assert(query() && !needed && calls == 1);
    nativeResult = true; assert(query() && needed && calls == 2);
    for (size_t index : {size_t(0),size_t(1),size_t(2),size_t(3),size_t(4),size_t(5),size_t(8)}) {
        const auto original = memory[index].second; memory[index].second = 0;
        assert(!query() && needed && calls == 2); memory[index].second = original;
    }
    code[0] = 0; assert(!query() && needed && calls == 2); code[0] = 0x55;
    memory[3].second = image+0x71e80; // inherited base FALSE cannot masquerade as Gen12 hook
    assert(!query() && needed && calls == 2); memory[3].second = 0x5000;
    rx = false; assert(!query() && needed && calls == 2); rx = true;
    identity = false; assert(!query() && needed && calls == 2); identity = true;
    binding.entry++; assert(!query() && needed && calls == 2); binding.entry--;
    assert(!tglQueryNativeTwoPassCsc(binding,child,UINTPTR_MAX-0x132,0xa000,
        vt,0x4000,0x5000,needed,read,qualify,executable,invoke) && needed && calls == 2);
    assert(!tglQueryNativeTwoPassCsc(binding,child,0x9000,UINTPTR_MAX-2,
        vt,0x4000,0x5000,needed,read,qualify,executable,invoke) && needed && calls == 2);
    assert(!binding.resolve(0,read,qualify,executable) && !binding.image && !binding.entry);
}

static void testDirectVeboxFeasibility() {
    std::array<unsigned char, 0xf8> params{};
    std::array<unsigned char, 0x294> source{};
    std::array<unsigned char, 0x134> target{};
    std::array<unsigned char, 8> alpha{};
    const uint32_t one = 1, nv12 = 0x19;
    const uintptr_t targetAddress = 0x3000;
    const TglSurfaceRect rect{0,0,1920,1080};
    std::memcpy(params.data()+0x90, &one, 4);
    std::memcpy(params.data()+0x98, &targetAddress, 8);
    for (size_t offset : {0x30u,0x40u,0x50u}) std::memcpy(source.data()+offset, &rect, 16);
    std::memcpy(target.data()+0x40, &rect, 16);
    std::memcpy(source.data()+0x130, &nv12, 4);
    std::memcpy(target.data()+0x130, &nv12, 4);
    uintptr_t failed = 0;
    std::array<uintptr_t,32> reads{};
    size_t count = 0;
    auto read = [&](uintptr_t p, void* out, size_t n) {
        if (count < reads.size()) reads[count++] = p;
        if (p == failed) return false;
        auto copy = [&](uintptr_t base, const auto& bytes) {
            if (p < base || p-base > bytes.size() || n > bytes.size()-(p-base)) return false;
            std::memcpy(out, bytes.data()+p-base, n); return true;
        };
        return copy(0x1000,params) || copy(0x2000,source) || copy(0x3000,target) || copy(0x4000,alpha);
    };
    unsigned queries = 0;
    bool queryOk = true, twoPass = false;
    auto query = [&](uintptr_t s, uintptr_t t, bool& needed) {
        assert(s == 0x2000 && t == 0x3000); ++queries;
        needed = twoPass; return queryOk;
    };
    auto feasible = [&] { return tglDirectVeboxFeasible(0x1000,0x2000,0x3000,1,read,query); };
    assert(feasible() && queries == 1);
    const auto baselineReads = reads; const auto baselineCount = count;
    for (size_t i = 0; i < baselineCount; ++i) {
        failed = baselineReads[i]; count = 0;
        assert(!feasible());
    }
    failed = 0; queries = 0; count = 0;
    assert(!tglDirectVeboxFeasible(0x1000,0x2000,0x3000,0,read,query));
    assert(count == 0 && queries == 0);
    for (size_t offset : {0x78u,0x138u,0x290u,0x83u}) {
        source[offset] = 1; assert(!feasible()); source[offset] = 0;
    }
    queryOk = false; assert(!feasible()); queryOk = true;
    twoPass = true; assert(!feasible()); twoPass = false;
    const uintptr_t alphaAddress = 0x4000;
    std::memcpy(params.data()+0xf0, &alphaAddress, 8);
    assert(feasible());
    alpha[4] = 2; assert(!feasible()); alpha[4] = 1; assert(feasible());
    failed = 0x4004; assert(!feasible()); failed = 0;
    const uintptr_t overflowingAlpha = UINTPTR_MAX - 6;
    std::memcpy(params.data()+0xf0, &overflowingAlpha, 8);
    assert(!feasible());
    std::memcpy(params.data()+0xf0, &alphaAddress, 8);
    source[0x130] = 0; queries = 0;
    assert(!feasible() && queries == 0); source[0x130] = 0x19;
    target[0x130] = 0; queries = 0;
    assert(!feasible() && queries == 0); target[0x130] = 0x19;
    params[0x98] = 1; assert(!feasible()); // caller cannot substitute a different target
}

static void testVeboxHardwareBinding() {
    const TglSurfaceRect full{0,0,1920,1080};
    const TglSurfaceRect larger{-1,-1,1921,1081};
    assert(tglDirectVeboxGeometry(full, full, full, full));
    assert(tglDirectVeboxGeometry(full, full, larger, full)); // containment, not ICL equality
    assert(!tglDirectVeboxGeometry(full, full, {1,0,1921,1080}, full));
    assert(!tglDirectVeboxGeometry(full, {0,0,1280,720}, larger, full));
    assert(!tglDirectVeboxGeometry(full, full, larger, {0,0,1280,720}));
    assert(!tglDirectVeboxGeometry({1,0,1921,1080}, full, larger, full));
    assert(!tglDirectVeboxGeometry(full, {1,0,1921,1080}, larger, full));
    const TglSurfaceRect fractional{0,0,1920.5f,1080.5f};
    assert(tglDirectVeboxGeometry(fractional, fractional, larger, fractional));
    uintptr_t failedRect = 0;
    unsigned rectReads = 0;
    auto rectRead = [&](uintptr_t p, void* out, size_t n) {
        ++rectReads;
        assert(n == 16 && (p == 0xa030 || p == 0xa040 || p == 0xa050 || p == 0xb040));
        if (p == failedRect) return false;
        std::memcpy(out, p == 0xa050 ? &larger : &full, n); return true;
    };
    assert(tglDirectVeboxSurfaceGeometry(0xa000, 0xb000, rectRead) && rectReads == 4);
    for (uintptr_t p : {0xa030u,0xa040u,0xa050u,0xb040u}) {
        failedRect = p;
        assert(!tglDirectVeboxSurfaceGeometry(0xa000, 0xb000, rectRead));
    }
    rectReads = 0;
    assert(!tglDirectVeboxSurfaceGeometry(0, 0xb000, rectRead));
    assert(!tglDirectVeboxSurfaceGeometry(UINTPTR_MAX - 0x5e, 0xb000, rectRead));
    assert(!tglDirectVeboxSurfaceGeometry(0xa000, UINTPTR_MAX - 0x4e, rectRead));
    assert(rectReads == 0);
    for (float bad : {std::numeric_limits<float>::infinity(),
                      std::numeric_limits<float>::quiet_NaN()}) {
        assert(!tglDirectVeboxGeometry(full, full, {0,0,bad,1080}, full));
        assert(!tglDirectVeboxGeometry({0,0,bad,1080}, full, larger, full));
        assert(!tglDirectVeboxGeometry(full, {0,0,bad,1080}, larger, full));
        assert(!tglDirectVeboxGeometry(full, full, larger, {0,0,bad,1080}));
    }
    constexpr uint32_t genericFormats[]{0x0d,0x0e,0x0f,0x10,0x11,0x12,0x13,
        0x14,0x15,0x17,0x19,0x52,0x53,0x58,0x59,0x4a,0x4c,0x4d,uint32_t(-7)};
    for (uint32_t format : genericFormats) assert(tglVeboxFormatSupported(format));
    assert(!tglVeboxFormatSupported(0x16)); // AUYV is not in Gen12 input rule
    assert(!tglVeboxFormatSupported(0) && !tglVeboxFormatSupported(UINT32_MAX));
    assert(!tglVeboxFormatSupported(0x10019));
    // DI predicate alone permits formats that generic admission must reject.
    assert(tglVeboxDiFormatSupported(UINT32_MAX));
    assert(!(tglVeboxFormatSupported(UINT32_MAX) && tglVeboxDiFormatSupported(UINT32_MAX)));
    constexpr uint32_t dnFormats[]{0x0e,0x11,0x0f,0x10,0x0d,0x4a,0x19,
        0x15,0x12,0x13,0x14,0x17,0x59,0x53,0x52,0x03,0x05};
    unsigned supported = 0;
    for (uint32_t format = 0; format < 256; ++format)
        supported += tglVeboxDnFormatSupported(format);
    assert(supported == 17);
    for (uint32_t format : dnFormats) assert(tglVeboxDnFormatSupported(format));
    assert(!tglVeboxDnFormatSupported(UINT32_MAX));
    assert(!tglVeboxDnFormatSupported(0x10019)); // no enum truncation to NV12
    unsigned formatReads = 0;
    uint32_t sourceFormat = 0x19;
    bool formatReadable = true;
    auto formatRead = [&](uintptr_t p, void* out, size_t n) {
        ++formatReads;
        assert(p == 0x5130 && n == 4);
        if (!formatReadable) return false;
        std::memcpy(out, &sourceFormat, n); return true;
    };
    assert(!tglVeboxDnSurfaceSupported(0, formatRead));
    assert(!tglVeboxDnSurfaceSupported(UINTPTR_MAX - 0x132, formatRead));
    assert(formatReads == 0);
    assert(tglVeboxDnSurfaceSupported(0x5000, formatRead) && formatReads == 1);
    assert(tglVeboxSurfaceSupported(0x5000, formatRead) && formatReads == 2);
    assert(!tglVeboxSurfaceSupported(0, formatRead) && formatReads == 2);
    assert(!tglVeboxSurfaceSupported(UINTPTR_MAX - 0x132, formatRead) && formatReads == 2);
    unsigned predicateCalls = 0;
    auto countedPredicate = [&](uint32_t) { ++predicateCalls; return true; };
    formatReadable = false;
    assert(!tglVeboxSurfaceFormatPredicate(0x5000, formatRead, countedPredicate));
    assert(predicateCalls == 0); // unreadable data must never reach format policy
    formatReadable = true;
    assert(tglVeboxSurfaceFormatPredicate(0x5000, formatRead, countedPredicate));
    assert(predicateCalls == 1);
    sourceFormat = UINT32_MAX;
    assert(!tglVeboxDnSurfaceSupported(0x5000, formatRead));
    assert(!tglVeboxSurfaceSupported(0x5000, formatRead));
    for (uint32_t src : {0x52u, 0x53u}) {
        for (uint32_t dst : {3u, 4u, 0x50u}) {
            assert(tglVeboxRtFormatSupported(0x0b, src, dst));
            assert(!tglVeboxRtFormatSupported(0, src, dst));
        }
    }
    for (uint32_t dst : {1u, 2u, 5u, 0x51u, UINT32_MAX})
        assert(!tglVeboxRtFormatSupported(0x0b, 0x53, dst));
    assert(!tglVeboxRtFormatSupported(0x0b, 0x19, 3));
    unsigned rtReads = 0;
    bool rtReadable = true;
    uint32_t rtFormat = 3;
    auto rtRead = [&](uintptr_t p, void* out, size_t n) {
        ++rtReads;
        assert(n == 4);
        uint32_t value = p == 0x6130 ? rtFormat : p == 0x5000 ? 0x0b : 0x53;
        assert(p == 0x6130 || p == 0x5000 || p == 0x5130);
        if (!rtReadable) return false;
        std::memcpy(out, &value, n); return true;
    };
    assert(!tglVeboxRtSurfaceSupported(0, 0x6000, rtRead));
    assert(!tglVeboxRtSurfaceSupported(0x5000, UINTPTR_MAX - 0x132, rtRead));
    assert(rtReads == 0);
    assert(tglVeboxRtSurfaceSupported(0x5000, 0x6000, rtRead) && rtReads == 3);
    rtFormat = 0x19; rtReads = 0;
    assert(tglVeboxRtSurfaceSupported(0x5000, 0x6000, rtRead) && rtReads == 1);
    rtReadable = false;
    assert(!tglVeboxRtSurfaceSupported(0x5000, 0x6000, rtRead));
    rtFormat = 3; rtReadable = true;
    for (uintptr_t failed : {uintptr_t(0x5000), uintptr_t(0x5130)}) {
        auto failSourceRead = [&](uintptr_t p, void* out, size_t n) {
            return p != failed && rtRead(p, out, n);
        };
        assert(!tglVeboxRtSurfaceSupported(0x5000, 0x6000, failSourceRead));
    }
    constexpr uint32_t diExcluded[]{0x15,0x14,0x17,0x03,0x01,0x51,0x50,0x05,0x06};
    for (uint32_t format : diExcluded) assert(!tglVeboxDiFormatSupported(format));
    assert(tglVeboxDiFormatSupported(0x19)); // NV12
    assert(tglVeboxDiFormatSupported(0x52) && tglVeboxDiFormatSupported(0x53));
    assert(!tglVeboxDiSurfaceSupported(0, formatRead));
    sourceFormat = 0x19; formatReadable = true;
    assert(tglVeboxDiSurfaceSupported(0x5000, formatRead));
    sourceFormat = 0x17; assert(!tglVeboxDiSurfaceSupported(0x5000, formatRead));
    formatReadable = false; assert(!tglVeboxDiSurfaceSupported(0x5000, formatRead));
    sourceFormat = 0x19; formatReadable = false;
    assert(!tglVeboxDnSurfaceSupported(0x5000, formatRead));
    std::array<unsigned char, 0xe0> pipeParams{};
    std::array<unsigned char, 0x83> pipeSurface{};
    uint32_t oneSource = 1;
    std::memcpy(pipeParams.data(), &oneSource, 4);
    std::memcpy(pipeParams.data() + 0x90, &oneSource, 4);
    uint8_t compNeeded = 0;
    uintptr_t failedPipeRead = 0;
    unsigned pipeReads = 0;
    auto pipeRead = [&](uintptr_t p, void* out, size_t n) {
        ++pipeReads;
        if (p == failedPipeRead) return false;
        if (p == 0x9000 && n == 1) { std::memcpy(out, &compNeeded, 1); return true; }
        if (p >= 0x7000 && p - 0x7000 + n <= pipeParams.size()) {
            std::memcpy(out, pipeParams.data() + p - 0x7000, n); return true;
        }
        if (p >= 0x8000 && p - 0x8000 + n <= pipeSurface.size()) {
            std::memcpy(out, pipeSurface.data() + p - 0x8000, n); return true;
        }
        assert(false); return false;
    };
    assert(tglCompositionBypassFeasible(0x7000, 0x8000, 0x9000, pipeRead));
    assert(pipeReads == 8);
    for (uintptr_t p : {0x9000u,0x7000u,0x7090u,0x8060u,0x8081u,0x8082u,0x8068u,0x70d8u}) {
        failedPipeRead = p;
        assert(!tglCompositionBypassFeasible(0x7000, 0x8000, 0x9000, pipeRead));
    }
    failedPipeRead = 0;
    for (size_t offset : {size_t(0x60),size_t(0x81),size_t(0x82),size_t(0x68)}) {
        pipeSurface[offset] = 1;
        assert(!tglCompositionBypassFeasible(0x7000, 0x8000, 0x9000, pipeRead));
        pipeSurface[offset] = 0;
    }
    for (size_t offset : {size_t(0),size_t(0x90),size_t(0xd8)}) {
        auto original = pipeParams[offset]; pipeParams[offset] = offset == 0xd8 ? 1 : 2;
        assert(!tglCompositionBypassFeasible(0x7000, 0x8000, 0x9000, pipeRead));
        pipeParams[offset] = original;
    }
    compNeeded = 1;
    assert(!tglCompositionBypassFeasible(0x7000, 0x8000, 0x9000, pipeRead));
    compNeeded = 2; pipeSurface[0x81] = 2; pipeSurface[0x82] = 2;
    assert(tglCompositionBypassFeasible(0x7000, 0x8000, 0x9000, pipeRead)); // native bit0
    pipeReads = 0;
    assert(!tglCompositionBypassFeasible(UINTPTR_MAX - 0xde, 0x8000, 0x9000, pipeRead));
    assert(!tglCompositionBypassFeasible(0x7000, 0, 0x9000, pipeRead));
    assert(!tglCompositionBypassFeasible(0x7000, UINTPTR_MAX - 0x81, 0x9000, pipeRead));
    assert(!tglCompositionBypassFeasible(0x7000, 0x8000, 0, pipeRead));
    assert(pipeReads == 0);
    constexpr uintptr_t image = 0x1000000, mhw = 0x2000, os = 0x3000;
    uintptr_t vt = image + 0x759520, actualOs = os, heap = 0x4000;
    uintptr_t relocation = image + 0x3eae0;
    uint32_t osMode = 0;
    bool modeReadable=true,relocationReadable=true;
    bool identity = true, readable = true;
    auto read = [&](uintptr_t p, void* out, size_t n) {
        if(p == os + 0x8c && n == 4) {
            if(!readable || !modeReadable) return false;
            std::memcpy(out,&osMode,4);return true;
        }
        if(p == mhw + 8 && !relocationReadable) return false;
        const auto value = p == mhw ? &vt : p == mhw + 0x18 ? &actualOs :
                           p == mhw + 0x20 ? &heap : p == mhw + 8 ? &relocation : nullptr;
        if (!readable || !value || n != 8) return false;
        std::memcpy(out, value, n); return true;
    };
    auto qualify = [&](uintptr_t p) { return identity && p == image; };
    TglVeboxHardwareBinding b;
    auto resolve = [&] { return b.resolve(mhw, os, image, read, qualify); };
    assert(resolve() && b.interface == mhw && b.os == os && b.heap == heap);
    auto empty = [&] { assert(!b.interface && !b.os && !b.heap); };
    ++relocation;assert(!resolve());empty();--relocation;
    osMode=1;assert(!resolve());empty();
    relocation=image+0x3e6a0;assert(resolve());
    osMode=UINT32_MAX;assert(resolve()); // native tests the entire DWORD for zero
    osMode=0;assert(!resolve());empty();relocation=image+0x3eae0;
    modeReadable=false;assert(!resolve());empty();modeReadable=true;
    relocationReadable=false;assert(!resolve());empty();relocationReadable=true;
    assert(!b.resolve(mhw,UINTPTR_MAX,image,read,qualify));empty();
    ++vt; assert(!resolve()); empty(); --vt;
    ++actualOs; assert(!resolve()); empty(); --actualOs;
    heap = 0; assert(!resolve()); empty(); heap = 0x4000;
    identity = false; assert(!resolve()); empty(); identity = true;
    readable = false; assert(!resolve()); empty(); readable = true;
    assert(!b.resolve(UINTPTR_MAX, os, image, read, qualify)); empty();
    assert(!b.resolve(mhw, os, UINTPTR_MAX, read, qualify)); empty();
    assert(!b.resolve(mhw, 0, image, read, qualify)); empty();
    assert(resolve());
    {
        TglOwnedVeboxStateStorage parameters;parameters[0]=0x21;
        std::array<uint8_t,16> stateAnchor{
            0x55,0x48,0x89,0xe5,0x48,0x81,0xec,0xb0,
            0x01,0,0,0x48,0x89,0x7d,0xf8,0x48};
        uintptr_t stateEntry=image+0x171240;
        unsigned emits=0;bool stateRx=true;int result=31;
        auto stateRead=[&](uintptr_t p,void* out,size_t n) {
            if(p==image+0x759538 && n==8) {std::memcpy(out,&stateEntry,8);return true;}
            if(p==image+0x171240 && n==16) {std::memcpy(out,stateAnchor.data(),16);return true;}
            return read(p,out,n);
        };
        auto invoke=[&](uintptr_t entry,uintptr_t object,uintptr_t command,
                        uint8_t* data,bool kernelResource) {
            assert(entry==image+0x171240 && object==mhw && command==0x6000);
            assert(data==parameters.data() && data[0]==0x21 && !kernelResource);
            data[0x30]=0x7b; // native resource writes survive status failure
            ++emits;return result;
        };
        auto emit=[&](uintptr_t command) {return b.emitState(image,command,parameters,
            stateRead,qualify,[&](uintptr_t,size_t){return stateRx;},invoke);};
        assert(emit(0x6000)==31 && emits==1 && parameters[0x30]==0x7b);result=0;
        assert(emit(0x6000)==0 && emits==2);
        assert(emit(0)==5 && emits==2);
        ++stateEntry;assert(emit(0x6000)==5 && emits==2);--stateEntry;
        stateAnchor[0]=0;assert(emit(0x6000)==5 && emits==2);stateAnchor[0]=0x55;
        stateRx=false;assert(emit(0x6000)==5 && emits==2);stateRx=true;
        ++heap;assert(emit(0x6000)==5 && emits==2);--heap;
    }
    std::array<uint8_t, 16> assignAnchor{
        0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x40,
        0x48,0x89,0x7d,0xf8,0x48,0x8b,0x45,0xf8};
    bool rx = true;
    unsigned calls = 0;
    uint32_t nativeResult = 0;
    uint32_t stateCount = 4, nextState = 0;
    auto assignRead = [&](uintptr_t p, void* out, size_t n) {
        if (n == 4 && (p == mhw + 0x28 || p == heap + 4)) {
            std::memcpy(out, p == mhw + 0x28 ? &stateCount : &nextState, 4); return true;
        }
        if (p == image + 0xfefa0 && n == assignAnchor.size()) {
            std::memcpy(out, assignAnchor.data(), n); return true;
        }
        return read(p, out, n);
    };
    auto executable = [&](uintptr_t p, size_t n) {
        return rx && p == image + 0xfefa0 && n == assignAnchor.size();
    };
    auto invoke = [&](uintptr_t p, uintptr_t object) {
        assert(p == image + 0xfefa0 && object == mhw); ++calls; return nativeResult;
    };
    assert(b.assignState(image, assignRead, qualify, executable, invoke) == 0 && calls == 1);
    nativeResult = 31;
    assert(b.assignState(image, assignRead, qualify, executable, invoke) == 31 && calls == 2);
    rx = false;
    assert(b.assignState(image, assignRead, qualify, executable, invoke) == 5 && calls == 2);
    rx = true; assignAnchor[0] ^= 1;
    assert(b.assignState(image, assignRead, qualify, executable, invoke) == 5 && calls == 2);
    assignAnchor[0] ^= 1; ++heap;
    assert(b.assignState(image, assignRead, qualify, executable, invoke) == 5 && calls == 2);
    --heap;
    stateCount = 0;
    assert(b.assignState(image, assignRead, qualify, executable, invoke) == 5 && calls == 2);
    stateCount = 4; nextState = 4;
    assert(b.assignState(image, assignRead, qualify, executable, invoke) == 5 && calls == 2);
    nextState = 0; stateCount = UINT32_MAX;
    assert(b.assignState(image, assignRead, qualify, executable, invoke) == 5 && calls == 2);
    stateCount = 4;
    {
        std::array<uint8_t,16> markAnchor{
            0x55,0x48,0x89,0xe5,0x48,0x89,0x7d,0xf8,
            0x48,0x8b,0x45,0xf8,0xc7,0x45,0xec,0x00};
        uint32_t current=3;unsigned marks=0;bool markRx=true;
        auto markRead=[&](uintptr_t p,void* out,size_t n) {
            if(p==heap && n==4){std::memcpy(out,&current,4);return true;}
            if(p==image+0xff190 && n==16){std::memcpy(out,markAnchor.data(),16);return true;}
            return assignRead(p,out,n);
        };
        auto mark=[&] {
            return b.markSubmitted(image,markRead,qualify,
                [&](uintptr_t p,size_t n){return markRx && p==image+0xff190 && n==16;},
                [&](uintptr_t p,uintptr_t object){assert(p==image+0xff190 && object==mhw);++marks;return nativeResult;});
        };
        nativeResult=0;assert(mark()==0 && marks==1);
        nativeResult=31;assert(mark()==31 && marks==2);
        current=4;assert(mark()==5 && marks==2);current=3;
        stateCount=0;assert(mark()==5 && marks==2);stateCount=4;
        markRx=false;assert(mark()==5 && marks==2);markRx=true;
        markAnchor[0]^=1;assert(mark()==5 && marks==2);markAnchor[0]^=1;
        ++heap;assert(mark()==5 && marks==2);--heap;
    }
    uint32_t instanceBytes = 4096, current = 3;
    {
        std::array<uint8_t,0x2e8> bytes{};
        auto put32=[&](size_t offset,uint32_t value){std::memcpy(bytes.data()+offset,&value,4);};
        auto put64=[&](size_t offset,uintptr_t value){std::memcpy(bytes.data()+offset,&value,8);};
        uint32_t count=4,branch=0;uintptr_t mapped=0x90000;
        put32(8,16384);put32(0x2c,4096);put32(0x30,16388);
        put32(0x50,16388);put32(0x54,0);put64(0x90,mapped);
        put64(0x2d0,mapped);put64(0x2d8,mapped+16384);
        auto tagRead=[&](uintptr_t p,void* out,size_t n) {
            if(p==mhw+0x28 && n==4){std::memcpy(out,&count,4);return true;}
            if(p==os+0x68 && n==4){std::memcpy(out,&branch,4);return true;}
            if(p>=heap && p-heap<=bytes.size() && n<=bytes.size()-(p-heap)) {
                std::memcpy(out,bytes.data()+p-heap,n);return true;
            }
            return false;
        };
        uintptr_t tag=77;
        assert(b.heapTagSource(tagRead,tag) && tag==mapped+16384);
        {
            constexpr uintptr_t entry=0x1000,context=0x2000;
            std::array<uint8_t,47> code{
                0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x10,0x89,0xf0,0x48,0x8d,0x55,0xf8,
                0x48,0x89,0x02,0x8b,0x7f,0x18,0xbe,0x05,0x00,0x00,0x00,0xb9,0x01,0x00,
                0x00,0x00,0x45,0x31,0xc0,0x45,0x31,0xc9,0xe8,0x6c,0x42,0x00,0x00,
                0x48,0x83,0xc4,0x10,0x5d,0xc3};
            auto read=[&](uintptr_t p,void* out,size_t n) noexcept {
                if(p!=entry || n!=code.size()) return false;
                std::memcpy(out,code.data(),n);return true;
            };
            auto qualified=[](uintptr_t p) noexcept {return p==entry;};
            auto executable=[](uintptr_t p,size_t n) noexcept {return p==entry && n==47;};
            TglNativeVideoFenceEventBinding binding;
            unsigned calls=0;
            for(uint32_t result:{0u,0xe00002d6u,0xffffffffu}) {
                auto invoke=[&](uintptr_t e,uintptr_t c,uint32_t index) noexcept {
                    assert(e==entry && c==context && index==UINT32_MAX);++calls;return result;
                };
                assert(binding.finish(context,UINT32_MAX,invoke)==5 && !calls);
                assert(binding.resolve(entry,read,qualified,executable));
                assert(binding.finish(context,UINT32_MAX,invoke)==result && calls==1);
                calls=0;
                assert(binding.finish(0,0,invoke)==5 && !calls);
                assert(binding.finish(UINTPTR_MAX,0,invoke)==5 && !calls);
                assert(!binding.resolve(entry,read,[](uintptr_t){return false;},executable));
                assert(binding.finish(context,0,invoke)==5 && !calls);
            }
            for(size_t i=0;i<code.size();++i) {
                code[i]^=1;
                assert(!binding.resolve(entry,read,qualified,executable));
                code[i]^=1;
            }
            assert(!binding.resolve(entry,read,qualified,[](uintptr_t,size_t){return false;}));
            assert(!binding.resolve(entry,[](uintptr_t,void*,size_t){return false;},qualified,executable));
            assert(!binding.resolve(UINTPTR_MAX,read,qualified,executable));
        }
        {
            constexpr uintptr_t image=0x100000,entry=image+0xa9dc,context=0x2000;
            std::array<uint8_t,21> code{
                0x55,0x48,0x89,0xe5,0x41,0x56,0x53,0x48,0x89,0xfb,0x48,0x8b,0x47,0x20,
                0x48,0x85,0xc0,0x74,0x5c,0xa8,0x07};
            auto read=[&](uintptr_t p,void* out,size_t n) noexcept {
                if(p!=entry || n!=code.size()) return false;
                std::memcpy(out,code.data(),n);return true;
            };
            auto qualified=[](uintptr_t p) noexcept {return p==image;};
            auto executable=[](uintptr_t p,size_t n) noexcept {return p==entry && n==21;};
            TglNativeVideoBufferFlushBinding binding;
            unsigned calls=0;
            for(uint32_t result:{0u,0xe00002e9u,0xffffffffu}) {
                auto invoke=[&](uintptr_t e,uintptr_t c,uint8_t kind) noexcept {
                    assert(e==entry && c==context && kind==2);++calls;return result;
                };
                assert(binding.flush(context,invoke)==5 && !calls);
                assert(binding.resolve(image,read,qualified,executable));
                assert(binding.flush(context,invoke)==result && calls==1);
                calls=0;
                assert(binding.flush(0,invoke)==5 && !calls);
                assert(binding.flush(UINTPTR_MAX,invoke)==5 && !calls);
                assert(!binding.resolve(image,read,[](uintptr_t){return false;},executable));
                assert(binding.flush(context,invoke)==5 && !calls);
            }
            for(size_t i=0;i<code.size();++i) {
                code[i]^=1;assert(!binding.resolve(image,read,qualified,executable));code[i]^=1;
            }
            assert(!binding.resolve(image,read,qualified,[](uintptr_t,size_t){return false;}));
            assert(!binding.resolve(image,[](uintptr_t,void*,size_t){return false;},qualified,executable));
            assert(!binding.resolve(UINTPTR_MAX,read,qualified,executable));
        }
        TglOwnedVeboxBuilderScratch scratch;
        for(uintptr_t invalidHeap:{uintptr_t(0),UINTPTR_MAX-uintptr_t(0x2e3),UINTPTR_MAX}) {
            auto invalid=b;invalid.heap=invalidHeap;
            TglVeboxCompletionWriteSnapshot unchanged{77,88,99,111};
            unsigned reads=0;
            assert(!tglReadVeboxCompletionWrite(scratch,invalid,
                [&](uintptr_t,void*,size_t){++reads;return false;},unchanged));
            assert(!reads && unchanged.resource==77 && unchanged.tagLocation==88 &&
                unchanged.offset==99 && unchanged.value==111);
        }
        const uintptr_t resource=heap+0x40;const uint32_t offset=16384;
        std::memcpy(scratch.completion.data(),&resource,8);
        std::memcpy(scratch.completion.data()+8,&offset,4);
        for(uint32_t value:{0u,1u,0x7fffffffu,0xffffffffu}) {
            put32(0x2e0,value);std::memcpy(scratch.completion.data()+0xc,&value,4);
            TglVeboxCompletionWriteSnapshot snapshot;
            assert(tglReadVeboxCompletionWrite(scratch,b,tagRead,snapshot));
            assert(snapshot.resource==resource && snapshot.tagLocation==tag &&
                snapshot.offset==offset && snapshot.value==value);
            for(size_t field:{size_t(0),size_t(8),size_t(0xc)}) {
                scratch.completion[field]^=1;
                snapshot.resource=77;snapshot.tagLocation=88;snapshot.offset=99;snapshot.value=111;
                assert(!tglReadVeboxCompletionWrite(scratch,b,tagRead,snapshot));
                assert(snapshot.resource==77 && snapshot.tagLocation==88 && snapshot.offset==99 && snapshot.value==111);
                scratch.completion[field]^=1;
            }
        }
        for(auto mutation : {8u,0x2cu,0x30u,0x50u,0x54u,0x90u,0x2d0u,0x2d8u}) {
            const auto saved=bytes;bytes[mutation]^=mutation==0x50?4:1;tag=77;
            assert(!b.heapTagSource(tagRead,tag) && tag==77);bytes=saved;
        }
        branch=1;tag=77;assert(!b.heapTagSource(tagRead,tag) && tag==77);branch=0;
        count=0;assert(!b.heapTagSource(tagRead,tag));count=4;
        assert(!b.heapTagSource([](uintptr_t,void*,size_t){return false;},tag));
    }
    auto readHeap = [&](uintptr_t p, void* out, size_t n) {
        const auto value = p == heap ? &current : p == heap + 0x2c ? &instanceBytes : nullptr;
        if (!value || n != 4) return false;
        std::memcpy(out, value, 4); return true;
    };
    TglVeboxHardwareBinding::StateResource state;
    assert(b.selectStateResource(false, 0, false, readHeap, state));
    assert(state.resource == heap + 0x40 && state.instanceOffset == 12288);
    assert(b.selectStateResource(false, 0, true, readHeap, state));
    assert(state.resource == heap + 0x188 && state.instanceOffset == 12288);
    current = 0;
    assert(b.selectStateResource(false, 0, false, readHeap, state));
    assert(state.resource == heap + 0x40 && state.instanceOffset == 0);
    current = 3;
    assert(b.selectStateResource(true, 0x9000, true, readHeap, state));
    assert(state.resource == 0x9000 && state.instanceOffset == 0);
    assert(!b.selectStateResource(true, 0, false, readHeap, state));
    assert(!state.resource && !state.instanceOffset);
    instanceBytes = UINT32_MAX; current = 2;
    assert(!b.selectStateResource(false, 0, false, readHeap, state));
    assert(!state.resource && !state.instanceOffset);
    current = 1;
    assert(b.selectStateResource(false, 0, false, readHeap, state));
    assert(state.instanceOffset == UINT32_MAX);
    auto deny = [](uintptr_t, void*, size_t) { return false; };
    assert(!b.selectStateResource(false, 0, false, deny, state));
    assert(!state.resource && !state.instanceOffset);
    uint32_t capacity = 16384, type = 0;
    instanceBytes = 4096;
    uintptr_t handle = 7, address = 0;
    state = {heap + 0x40, 12288};
    auto resourceRead = [&](uintptr_t p, void* out, size_t n) {
        if (p == heap + 0x2c && n == 4) {
            std::memcpy(out, &instanceBytes, 4); return true;
        }
        if (n == 4 && (p == state.resource + 0x10 || p == state.resource + 0x14)) {
            std::memcpy(out, p == state.resource + 0x10 ? &capacity : &type, 4); return true;
        }
        if (n == 8 && (p == state.resource + 0x20 || p == state.resource + 0x50)) {
            std::memcpy(out, p == state.resource + 0x20 ? &handle : &address, 8); return true;
        }
        return false;
    };
    assert(b.heapStateRange(state, 4096, resourceRead));
    assert(!b.heapStateRange(state, 4097, resourceRead));
    // Native174cba adds the heap region offset before clearing 0x800 bytes.
    assert(b.heapStateRange(state, 0x800, resourceRead, 0x800));
    assert(!b.heapStateRange(state, 0x800, resourceRead, 0x801));
    assert(!b.heapStateRange(state, 0x800, resourceRead, UINT32_MAX));
    state.instanceOffset = 0; // plenty of total capacity, but not this instance
    assert(b.heapStateRange(state, 4096, resourceRead));
    assert(!b.heapStateRange(state, 4097, resourceRead));
    state.instanceOffset = 1;
    assert(!b.heapStateRange(state, 1, resourceRead));
    state.instanceOffset = 12288;
    instanceBytes = 0; assert(!b.heapStateRange(state, 1, resourceRead));
    instanceBytes = 4096;
    handle = 0; assert(!b.heapStateRange(state, 4096, resourceRead));
    address = 0x9000; assert(b.heapStateRange(state, 4096, resourceRead));
    type = 1; assert(!b.heapStateRange(state, 4096, resourceRead)); type = 0;
    assert(!b.heapStateRange(state, 0, resourceRead));
    assert(!b.heapStateRange(state, 4096, deny));
    state = {heap + 0x188, 12288};
    assert(b.heapStateRange(state, 4096, resourceRead));
    // Every required descriptor read must succeed; no partial backing proof.
    for (uintptr_t field : {uintptr_t(0x10), uintptr_t(0x14),
                            uintptr_t(0x20), uintptr_t(0x50)}) {
        auto failField = [&](uintptr_t p, void* out, size_t n) {
            return p != state.resource + field && resourceRead(p, out, n);
        };
        assert(!b.heapStateRange(state, 4096, failField));
    }
    state.instanceOffset = UINT32_MAX;
    assert(!b.heapStateRange(state, 4096, resourceRead));
    state = {0x9000, 0}; // external CM surface is explicitly not this ABI
    assert(!b.heapStateRange(state, 4096, resourceRead));
    auto composedRead = [&](uintptr_t p, void* out, size_t n) {
        if (readHeap(p, out, n)) return true;
        for (uintptr_t offset : {uintptr_t(0x40), uintptr_t(0x188)}) {
            const uintptr_t resource = heap + offset;
            if (n == 4 && (p == resource + 0x10 || p == resource + 0x14)) {
                std::memcpy(out, p == resource + 0x10 ? &capacity : &type, n); return true;
            }
            if (n == 8 && (p == resource + 0x20 || p == resource + 0x50)) {
                std::memcpy(out, p == resource + 0x20 ? &handle : &address, n); return true;
            }
        }
        return assignRead(p, out, n);
    };
    auto assign = [&](uintptr_t entry, uintptr_t object) {
        current = 1; // simulate native publishing a new current instance
        return invoke(entry, object);
    };
    nativeResult = 0; current = 3;
    const unsigned beforeInvalidSize = calls;
    assert(b.prepareHeapState(image, false, 0, 0, composedRead,
        qualify, executable, assign, state) == 5);
    assert(b.prepareHeapState(image, false, 0x800, UINT32_MAX, composedRead,
        qualify, executable, assign, state) == 5);
    assert(calls == beforeInvalidSize && current == 3 && !state.resource);
    assert(b.prepareHeapState(image, false, 0x800, 0x800, composedRead,
        qualify, executable, assign, state) == 0);
    assert(state.resource == heap + 0x40 && state.instanceOffset == 4096);
    nativeResult = 31;
    assert(b.prepareHeapState(image, true, 0x800, 0, composedRead,
        qualify, executable, assign, state) == 31);
    assert(!state.resource && !state.instanceOffset);
    nativeResult = 0; capacity = 4096;
    assert(b.prepareHeapState(image, false, 0x800, 0, composedRead,
        qualify, executable, assign, state) == 5);
    assert(!state.resource && !state.instanceOffset);
}

static bool executionReleaseFixture(uintptr_t base,uintptr_t p,void* out,size_t n) {
    if(p==base+0x12ec2d && n==14) {std::memcpy(out,TglExecutionBinding::createCounter.data(),n);return true;}
    if(p==base+0x12e0ea && n==14) {std::memcpy(out,TglExecutionBinding::releaseCounter.data(),n);return true;}
    if(p==base+0x12e10d && n==13) {std::memcpy(out,TglExecutionBinding::releaseDispatch.data(),n);return true;}
    if(p==base+0x12e11a && n==11) {std::memcpy(out,TglExecutionBinding::releaseClear.data(),n);return true;}
    return false;
}
static void testVeboxRenderBinding() {
    for(bool allocated : {false,true}) for(uint8_t report08 : {uint8_t(0xfe),uint8_t(0xff)})
      for(uint8_t report09 : {uint8_t(0xfe),uint8_t(0xff)})
        for(uint32_t pipe : {0u,1u,0x10001u}) for(bool present : {false,true}) {
            TglVeboxHistoryState state;state.previous1b78=11;state.previous1b7c=22;
            const uint32_t previous=44;
            const int result=tglPublishVeboxMode0SampleIds(state,allocated,33,report08,report09,
                pipe,present?&previous:nullptr);
            assert(result==((report08&1) && !present ? 5 : 0));
            assert(state.previous1b78==(allocated?33u:11u));
            const uint32_t expected=(report08&1) ? (present?44u:22u) :
                ((report09&1) && pipe==1 ? 22u : UINT32_MAX);
            assert(state.previous1b7c==expected && !state.mode && !state.radius && !state.field1b5c);
        }
    {
        TestFeatureSurfaceOwner current;current.busy=true;
        struct Previous {
            TglOwnedSurfaceStorage storage{};bool leased=false;unsigned queries=0;
            bool hasSurfaceLease(const TglOwnedSurfaceStorage& s) noexcept {
                ++queries;return leased && &s==&storage;
            }
        } previous;
        auto selected=TglVeboxMode0Allocation::Required;
        assert(tglSelectLeasedVeboxMode0Allocation(current,current.storage,previous,nullptr,
            1,0,1,selected)==0 && selected==TglVeboxMode0Allocation::Reuse && !previous.queries);
        assert(tglSelectLeasedVeboxMode0Allocation(current,current.storage,previous,&previous.storage,
            1,1,1,selected)==5 && selected==TglVeboxMode0Allocation::Reuse && previous.queries==1);
        previous.leased=true;
        assert(tglSelectLeasedVeboxMode0Allocation(current,current.storage,previous,&previous.storage,
            1,1,1,selected)==0 && selected==TglVeboxMode0Allocation::Reuse);
        previous.storage.prefix[0xe3]=1;
        assert(tglSelectLeasedVeboxMode0Allocation(current,current.storage,previous,&previous.storage,
            1,1,1,selected)==0 && selected==TglVeboxMode0Allocation::Required);
        previous.leased=false;const auto queries=previous.queries;
        assert(tglSelectLeasedVeboxMode0Allocation(current,current.storage,previous,nullptr,
            0,1,1,selected)==0 && selected==TglVeboxMode0Allocation::Required && previous.queries==queries);
        current.busy=false;selected=TglVeboxMode0Allocation::Reuse;
        assert(tglSelectLeasedVeboxMode0Allocation(current,current.storage,previous,nullptr,
            0,0,0,selected)==5 && selected==TglVeboxMode0Allocation::Reuse);
    }
    {
        std::array<uint8_t,0x2a0> current{},previous{};
        assert(tglVeboxMode0NativeSkipPredicate(1,current,previous));
        for(size_t offset : {size_t(0x29a),size_t(0x299)}) {
            for(unsigned a=0;a<256;++a) for(unsigned b=0;b<256;++b) {
                current[offset]=uint8_t(a);previous[offset]=uint8_t(b);
                assert(tglVeboxMode0NativeSkipPredicate(1,current,previous)==((a&1)==(b&1)));
            }
            current[offset]=previous[offset]=0;
        }
        for(size_t offset : {size_t(0x29c),size_t(0xd8),size_t(0xdc),size_t(0xe0)}) {
            for(size_t byte=0;byte<4;++byte) for(unsigned bit=0;bit<8;++bit) {
                current[offset+byte]=uint8_t(1u<<bit);
                assert(!tglVeboxMode0NativeSkipPredicate(1,current,previous));
                assert(tglVeboxMode0NativeSkipPredicate(0xfe,current,previous));
                previous[offset+byte]=current[offset+byte];
                assert(tglVeboxMode0NativeSkipPredicate(0xff,current,previous));
                current[offset+byte]=previous[offset+byte]=0;
            }
        }
        previous[0x29b]=0xff;
        assert(tglVeboxMode0NativeSkipPredicate(1,current,previous));
        previous[0xe3]=0x80;
        assert(tglVeboxMode0AllocationDecision(1,0,1,[&]() noexcept {
            return tglVeboxMode0NativeSkipPredicate(1,current,previous);
        })==TglVeboxMode0Allocation::Required);
        assert(tglVeboxMode0AllocationDecision(1,0,1,[&]() noexcept {
            return tglVeboxMode0NativeSkipPredicate(0,current,previous);
        })==TglVeboxMode0Allocation::Reuse);
    }
    for(unsigned report=0;report<256;++report)
      for(unsigned source=0;source<256;++source)
        for(uint32_t pipe : {0u,1u,2u,0x10001u}) for(bool skip : {false,true}) {
            unsigned queries=0;
            const auto result=tglVeboxMode0AllocationDecision(uint8_t(report),uint8_t(source),pipe,
                [&]() noexcept {++queries;return skip;});
            const bool candidate=(report&1) && !(source&1) && pipe==1;
            assert(queries==unsigned(candidate));
            assert(result==(candidate && skip ? TglVeboxMode0Allocation::Reuse :
                TglVeboxMode0Allocation::Required));
        }
    constexpr uintptr_t image=0x1000000;
    auto bytes=TglVeboxRenderBinding::anchor;
    bool readable=true,rx=true,identity=true;
    auto read=[&](uintptr_t p,void* out,size_t n) noexcept {
        if(!readable || p!=image+0x12c9d0 || n!=bytes.size()) return false;
        std::memcpy(out,bytes.data(),n);return true;
    };
    auto qualify=[&](uintptr_t p) noexcept {return identity && p==image;};
    auto executable=[&](uintptr_t p,size_t n) noexcept {
        return rx && p==image+0x12c9d0 && n==bytes.size();
    };
    TglVeboxRenderBinding binding;
    for(size_t i=0;i<bytes.size();++i) {
        bytes[i]^=1;assert(!binding.resolve(image,read,qualify,executable));bytes[i]^=1;
        assert(!binding.image && !binding.entry);
    }
    for(unsigned gate=0;gate<3;++gate) {
        readable=gate!=0;rx=gate!=1;identity=gate!=2;
        assert(!binding.resolve(image,read,qualify,executable));
    }
    readable=rx=identity=true;
    assert(binding.resolve(image,read,qualify,executable));
    struct RenderOwner:TestVeboxChildLease {
        uintptr_t image=0;
        bool hasPrivateChildImageLease(uintptr_t p,uintptr_t i) const noexcept {
            return image && image==i && hasPrivateChildLease(p);
        }
    } owner;
    int child=0,arg1=0,arg2=0;owner.child=&child;owner.image=image;unsigned calls=0;
    auto invoke=[&](uintptr_t p,void* c,void* a,void* b) noexcept -> int {
        assert(p==image+0x12c9d0 && c==&child && a==&arg1 && b==&arg2);
        ++calls;assert(owner.busy);return -7;
    };
    assert(tglInvokeLeasedVeboxRender(binding,owner,&child,&arg1,&arg2,invoke)==5 && !calls);
    assert(owner.withPrivateChild([&](void* c) noexcept {
        assert(tglInvokeLeasedVeboxRender(binding,owner,c,nullptr,&arg2,invoke)==5 && !calls);
        owner.image=image+0x1000;
        assert(tglInvokeLeasedVeboxRender(binding,owner,c,&arg1,&arg2,invoke)==5 && !calls);
        owner.image=image;
        return tglInvokeLeasedVeboxRender(binding,owner,c,&arg1,&arg2,invoke);
    })==-7 && calls==1);
    TestFeatureSurfaceOwner sourceOwner,targetOwner;
    unsigned surfaceCalls=0;
    auto surfaceInvoke=[&](uintptr_t p,void* c,void* a,void* b) noexcept -> int {
        assert(p==image+0x12c9d0 && c==&child && a==&arg1 && owner.busy);
        const auto* bytes=static_cast<const uint8_t*>(b);
        for(size_t i=0;i<0x10;++i) assert(!bytes[i]);
        uintptr_t source=0,target=0;
        std::memcpy(&source,bytes+0x10,8);std::memcpy(&target,bytes+0x18,8);
        assert(source==reinterpret_cast<uintptr_t>(&sourceOwner.storage));
        assert(target==reinterpret_cast<uintptr_t>(&targetOwner.storage));
        ++surfaceCalls;return 31;
    };
    assert(tglInvokeLeasedVeboxSurfaceRender(binding,owner,&child,&arg1,
        sourceOwner,sourceOwner.storage,targetOwner,targetOwner.storage,surfaceInvoke)==5);
    assert(sourceOwner.withPeerSurface(targetOwner,[&](const auto& source,const auto& target) noexcept {
        assert(owner.withPrivateChild([&](void* c) noexcept {
            TglOwnedSurfaceStorage foreign{};
            assert(tglInvokeLeasedVeboxSurfaceRender(binding,owner,c,&arg1,
                sourceOwner,foreign,targetOwner,target,surfaceInvoke)==5 && !surfaceCalls);
            assert(tglInvokeLeasedVeboxSurfaceRender(binding,owner,c,&arg1,
                sourceOwner,source,targetOwner,target,surfaceInvoke)==31 && surfaceCalls==1);
            auto revoke=[&](uintptr_t,void*,void*,void*) noexcept -> int {
                targetOwner.deny=true;return 0;
            };
            assert(tglInvokeLeasedVeboxSurfaceRender(binding,owner,c,&arg1,
                sourceOwner,source,targetOwner,target,revoke)==5);
            targetOwner.deny=false;return 0;
        })==0);
    }));
}
static void testExecutionBinding() {
    {
        struct Accounting {unsigned counter=1,deletes=0;} native;
        auto release=+[](uintptr_t* slot) {
            assert(slot && *slot);
            auto& object=*reinterpret_cast<Accounting*>(*slot);
            assert(object.counter==1);--object.counter;++object.deletes;*slot=0;
        };
        TglNativeExecutionInvoker{}.destroy(reinterpret_cast<uintptr_t>(release),
            reinterpret_cast<uintptr_t>(&native));
        assert(!native.counter && native.deletes==1);
    }
    constexpr uintptr_t base = 0x1000000;
    constexpr std::array<uintptr_t, 3> offsets{0x12ebc0, 0x12e420, 0x12e0d0};
    std::array<std::array<uint8_t, 16>, 3> bytes{{
        {0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x40,0x48,0x8b,0x35,0x59,0x54,0x62,0,0xbf},
        {0x55,0x48,0x89,0xe5,0x48,0x81,0xec,0xb0,9,0,0,0x48,0x8b,5,0x56,0x5c},
        {0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x10,0x48,0x89,0x7d,0xf8,0x48,0x8b,0x45,0xf8}
    }};
    int denied = -1;
    bool identity = true, rx = true;
    uint32_t allocationSize=0xd48;
    uintptr_t resetEntry=base+0x12e420;bool resetReadable=true;
    uintptr_t mutateAccountingSite=0;
    size_t mutateReleaseSize=0,mutateReleaseByte=0;
    auto read = [&](uintptr_t p, void* out, size_t n) {
        if(executionReleaseFixture(base,p,out,n)) {
            if(n==mutateReleaseSize && (!mutateAccountingSite || p==mutateAccountingSite))
                static_cast<uint8_t*>(out)[mutateReleaseByte]^=1;
            return true;
        }
        if(p==base+0x757ea0 && n==8 && resetReadable) {std::memcpy(out,&resetEntry,8);return true;}
        if(p==base+0x12ebd0 && n==4) {std::memcpy(out,&allocationSize,4);return true;}
        for (size_t i = 0; i < offsets.size(); ++i)
            if (p == base + offsets[i] && n == 16 && int(i) != denied) {
                std::memcpy(out, bytes[i].data(), n); return true;
            }
        return false;
    };
    auto qualify = [&](uintptr_t p) { return identity && p == base; };
    auto executable = [&](uintptr_t, size_t n) { return rx && (n == 16 || n==4 || n==14 || n==13 || n==11); };
    TglExecutionBinding b;
    for(size_t n : {14u,13u,11u}) for(size_t i=0;i<n;++i) {
        mutateReleaseSize=n;mutateReleaseByte=i;
        assert(!b.resolve(base,read,qualify,executable) && !b.image);
    }
    mutateReleaseSize=0;
    for(uintptr_t site : {base+0x12ec2d,base+0x12e0ea}) {
        mutateAccountingSite=site;mutateReleaseSize=14;
        for(size_t i=0;i<14;++i) {
            mutateReleaseByte=i;
            assert(!b.resolve(base,read,qualify,executable) && !b.image);
        }
    }
    mutateAccountingSite=0;mutateReleaseSize=0;
    assert(b.resolve(base, read, qualify, executable));
    assert(b.image == base && b.create == base + offsets[0] &&
           b.initialize == base + offsets[1] && b.destroy == base + offsets[2]);
    for (int i = 0; i < 3; ++i) {
        denied = i; assert(!b.resolve(base, read, qualify, executable));
        assert(!b.image && !b.create && !b.initialize && !b.destroy); denied = -1;
        bytes[i][15] ^= 1; assert(!b.resolve(base, read, qualify, executable)); bytes[i][15] ^= 1;
    }
    identity = false; assert(!b.resolve(base, read, qualify, executable)); identity = true;
    allocationSize=0xd40;assert(!b.resolve(base,read,qualify,executable));allocationSize=0xd48;
    resetEntry=base+0x12e3f0;assert(!b.resolve(base,read,qualify,executable) && !b.image);
    resetEntry=base+0x12e420;resetReadable=false;
    assert(!b.resolve(base,read,qualify,executable) && !b.initialize);resetReadable=true;
    rx = false; assert(!b.resolve(base, read, qualify, executable)); rx = true;
    assert(!b.resolve(UINTPTR_MAX, read, qualify, executable));
    assert(!b.resolve(0, read, qualify, executable));
}

static void testExecutionShape() {
    constexpr uintptr_t image = 0x1000000, object = 0x2000;
    uintptr_t vt = image + 0x757e90, first = 0x4000, second = 0x5000;
    uintptr_t av = image + 0x757f78, bv = image + 0x757fb8;
    bool allowed = true, qualified = true;
    bool codeValid = true, codeRx = true;
    std::array<std::array<uint8_t, 16>, 3> code{{
        {0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x40,0x48,0x8b,0x35,0x59,0x54,0x62,0,0xbf},
        {0x55,0x48,0x89,0xe5,0x48,0x81,0xec,0xb0,9,0,0,0x48,0x8b,5,0x56,0x5c},
        {0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x10,0x48,0x89,0x7d,0xf8,0x48,0x8b,0x45,0xf8}
    }};
    auto read = [&](uintptr_t p, void* out, size_t n) {
        if(codeValid && executionReleaseFixture(image,p,out,n)) return true;
        if(p==image+0x757ea0 && n==8 && codeValid) {
            const uintptr_t reset=image+0x12e420;std::memcpy(out,&reset,8);return true;
        }
        if(p==image+0x12ebd0 && n==4 && codeValid) {
            const uint32_t allocationSize=0xd48;std::memcpy(out,&allocationSize,4);return true;
        }
        if (n == 16 && codeValid) {
            const std::array<uintptr_t, 3> offsets{0x12ebc0, 0x12e420, 0x12e0d0};
            for (size_t i = 0; i < offsets.size(); ++i)
                if (p == image + offsets[i]) {
                    std::memcpy(out, code[i].data(), n); return true;
                }
        }
        if (!allowed || n != 8) return false;
        const uintptr_t* value = p == object ? &vt : p == object + 0xd38 ? &first :
            p == object + 0xd40 ? &second : p == 0x4000 ? &av : p == 0x5000 ? &bv : nullptr;
        if (!value) return false;
        std::memcpy(out, value, n); return true;
    };
    auto qualify = [&](uintptr_t p) { return qualified && p == image; };
    assert(tglExecutionShape(object, image, read, qualify));
    qualified = false; assert(!tglExecutionShape(object, image, read, qualify)); qualified = true;
    allowed = false; assert(!tglExecutionShape(object, image, read, qualify)); allowed = true;
    first = 0; assert(!tglExecutionShape(object, image, read, qualify)); first = second;
    assert(!tglExecutionShape(object, image, read, qualify)); first = 0x4000;
    ++av; assert(!tglExecutionShape(object, image, read, qualify)); --av;
    ++bv; assert(!tglExecutionShape(object, image, read, qualify)); --bv;
    ++vt; assert(!tglExecutionShape(object, image, read, qualify)); --vt;
    assert(!tglExecutionShape(UINTPTR_MAX, image, read, qualify));
    assert(!tglExecutionShape(object, UINTPTR_MAX, read, qualify));
    struct Counts { int creates = 0, initializes = 0, destroys = 0, result = 0; } calls;
    struct Invoker {
        Counts* calls;
        uintptr_t create(uintptr_t entry) noexcept {
            assert(entry == 0x1000000 + 0x12ebc0); ++calls->creates; return 0x2000;
        }
        int initialize(uintptr_t entry, uintptr_t p) noexcept {
            assert(entry == 0x1000000 + 0x12e420 && p == 0x2000);
            ++calls->initializes; return calls->result;
        }
        void destroy(uintptr_t entry, uintptr_t p) noexcept {
            assert(entry == 0x1000000 + 0x12e0d0 && p == 0x2000); ++calls->destroys;
        }
    } invoke{&calls};
    TglExecutionBinding binding{image, image + 0x12ebc0, image + 0x12e420, image + 0x12e0d0};
    auto executable = [&](uintptr_t, size_t n) { return codeRx && (n == 16 || n==4 || n==14 || n==13 || n==11); };
    TglExecutionBackend backend(binding, read, qualify, invoke, executable);
    {
        TglExecutionOwner owner(backend);
        calls.result = 31;
        assert(owner.ensure() == 31 && owner.get() == 0 && calls.destroys == 1);
        calls.result = 0; second = 0;
        assert(owner.ensure() == 5 && owner.get() == 0 && calls.destroys == 2);
        second = 0x5000;
        assert(owner.ensure() == 0 && owner.get() == object);
        assert(owner.ensure() == 0 && calls.creates == 3);
    }
    assert(calls.destroys == 3 && calls.initializes == 3);
    ++binding.destroy;
    TglExecutionBackend invalid(binding, read, qualify, invoke, executable);
    assert(invalid.create() == 0 && calls.creates == 3);
    --binding.destroy;
    codeValid = false;
    TglExecutionBackend unreadable(binding, read, qualify, invoke, executable);
    assert(unreadable.create() == 0 && calls.creates == 3);
    codeValid = true; codeRx = false;
    TglExecutionBackend nonExecutable(binding, read, qualify, invoke, executable);
    assert(nonExecutable.create() == 0 && calls.creates == 3);
    codeRx = true;
    for (auto& anchor : code) {
        anchor[15] ^= 1;
        TglExecutionBackend changedCode(binding, read, qualify, invoke, executable);
        assert(changedCode.create() == 0 && calls.creates == 3);
        anchor[15] ^= 1;
    }
}

static void testExecutionOwner() {
    {
        alignas(8) std::array<uint8_t,0xd68> allocation;
        allocation.fill(0xa5);
        const uintptr_t object=reinterpret_cast<uintptr_t>(allocation.data()+0x10);
        TglOwnedDndiState state;state.words.fill(0x12345678);
        TglNativeExecutionInvoker native;
        assert(native.commitDndi(0,state)==5 && native.commitDndi(UINTPTR_MAX,state)==5);
        assert(native.commitDndi(object,state)==0);
        for(size_t i=0;i<allocation.size();++i) {
            if(i>=0x860 && i<0x950)
                assert(allocation[i]==reinterpret_cast<const uint8_t*>(&state)[i-0x860]);
            else assert(allocation[i]==0xa5);
        }
    }
    struct Backend {
        bool allocation = true, valid = true, invalidateOnInit = false;
        int result = 0, creates = 0, initializes = 0, destroys = 0;
        uintptr_t create() noexcept { ++creates; return allocation ? 7 : 0; }
        int initialize(uintptr_t p) noexcept {
            assert(p == 7); ++initializes;
            if (invalidateOnInit) valid = false;
            return result;
        }
        bool validate(uintptr_t p) noexcept { assert(p == 7); return valid; }
        void destroy(uintptr_t p) noexcept { assert(p == 7); ++destroys; }
        TglOwnedDndiState data;
        int scratchCalls=0;
        int clearKernelScratch(uintptr_t p,void* child,size_t extent,int32_t index) noexcept {
            assert(p==7 && child==&data && extent==sizeof(data) && index==3);
            ++scratchCalls;return 31;
        }
        int commitDndi(uintptr_t p,const TglOwnedDndiState& state) noexcept {
            assert(p==7);data=state;return 0;
        }
    } b;
    {
        TglExecutionOwner<Backend> owner(b);
        assert(owner.withExecution([](uintptr_t) noexcept {assert(false);return 0;})==5);
        b.allocation = false;
        assert(owner.ensure() == 1 && owner.get() == 0 && b.destroys == 0);
        b.allocation = true; b.result = 31;
        assert(owner.ensure() == 31 && owner.get() == 0 && b.destroys == 1);
        b.result = 0; b.valid = false;
        assert(owner.ensure() == 5 && owner.get() == 0 && b.destroys == 2);
        b.valid = true;
        assert(owner.ensure() == 0 && owner.get() == 7);
        const auto calls = b.initializes;
        assert(owner.ensure() == 0 && b.initializes == calls);
        TglOwnedDndiState update;update.words.fill(0x12345678);
        assert(owner.commitDndi(7+0x850,update)==5);
        assert(owner.clearKernelScratch(&b.data,sizeof(b.data),3)==5 && b.scratchCalls==0);
        assert(owner.withExecution([&](uintptr_t) noexcept {
            return owner.clearKernelScratch(&b.data,sizeof(b.data),3);
        })==31 && b.scratchCalls==1);
        assert(owner.clearKernelScratch(&b.data,sizeof(b.data),3)==5 && b.scratchCalls==1);
        assert(owner.withExecution([&](uintptr_t execution) noexcept {
            assert(owner.commitDndi(execution+0x851,update)==5);
            return owner.commitDndi(execution+0x850,update);
        })==0);
        assert(b.data.words==update.words);
        assert(owner.commitDndi(7+0x850,update)==5);
        for(int status:{0,31,-7}) {
            const auto destroys=b.destroys;
            assert(owner.withExecution([&](uintptr_t execution) noexcept {
                assert(execution==7);
                owner.reset();assert(owner.get()==7 && b.destroys==destroys);
                assert(owner.ensure()==5 && owner.preparePass()==5 && b.initializes==calls);
                assert(owner.withExecution([](uintptr_t) noexcept {assert(false);return 0;})==5);
                return status;
            })==status);
            assert(owner.get()==7 && b.destroys==destroys);
        }
        b.valid = false;
        assert(owner.withExecution([](uintptr_t) noexcept {assert(false);return 0;})==5);
        assert(owner.get()==0 && b.destroys==3);
        b.valid = true;
        assert(owner.ensure() == 0);
        owner.reset(); owner.reset();
        assert(owner.get() == 0 && b.destroys == 4);
        assert(owner.ensure() == 0);
    }
    assert(b.destroys == 5 && b.creates == 6 && b.initializes == 5);
    {
        Backend backing;TglExecutionOwner owner(backing);assert(owner.ensure()==0);
        constexpr uintptr_t child=0x1000;
        TglOwnedDndiSource source{0x2000,{1920,1080},false,32};
        uintptr_t linked=7;bool readOk=true;unsigned commits=0;int commitStatus=0;
        std::array<uint8_t,0x14> flags{};
        TglOwnedDndiState storage;storage.words.fill(0xa5a5a5a5);
        const auto read=[&](uintptr_t address,void* out,size_t size) noexcept {
            if(!readOk) return false;
            if(address==child+0x88 && size==8) {std::memcpy(out,&linked,8);return true;}
            if(address==7 && size==flags.size()) {std::memcpy(out,flags.data(),size);return true;}
            if(address==7+0x850 && size==sizeof(storage)) {std::memcpy(out,&storage,size);return true;}
            return false;
        };
        const auto commit=[&](uintptr_t address,const TglOwnedDndiState& value) noexcept {
            ++commits;assert(address==7+0x850);
            owner.reset();assert(owner.get()==7 && !backing.destroys);
            assert(owner.preparePass()==5);
            if(commitStatus) return commitStatus;
            storage=value;return 0;
        };
        for(unsigned mode=0;mode<8;++mode) for(int result:{0,31,-7}) {
            flags.fill(0xfe);flags[0xb]|=mode&1;flags[0xc]|=(mode>>1)&1;
            flags[0x13]|=(mode>>2)&1;
            storage.words.fill(0xa5a5a5a5);const auto before=storage.words;
            TglOwnedDndiPackets output;output.luma.fill(0xabcdef01);output.chroma.fill(0x12345678);
            const auto saved=output;commitStatus=result;
            assert(tglPublishOwnedDndi(owner,child,source.identity,source,output,read,commit)==result);
            if(result) {
                assert(storage.words==before);
                assert(output.luma==saved.luma && output.chroma==saved.chroma);
            } else {
                TglOwnedDndiState expected;expected.words=before;auto packets=saved;
                tglOwnedDndiSetDn(expected,packets,mode&1,mode&2,false,32);
                assert(!tglOwnedDndiSetDi(expected,mode&4,&source.dimensions));
                assert(storage.words==expected.words);
                assert(output.luma==packets.luma && output.chroma==packets.chroma);
            }
        }
        const auto before=commits;TglOwnedDndiPackets output;
        linked=8;
        assert(tglPublishOwnedDndi(owner,child,source.identity,source,output,read,commit)==5);
        linked=7;readOk=false;
        assert(tglPublishOwnedDndi(owner,child,source.identity,source,output,read,commit)==5);
        readOk=true;
        assert(tglPublishOwnedDndi(owner,child,source.identity+1,source,output,read,commit)==5);
        assert(commits==before && owner.get()==7);
        backing.data.words.fill(0xa5a5a5a5);
        const auto ownerRead=[&](uintptr_t address,void* out,size_t size) noexcept {
            if(address==7+0x850 && size==sizeof(backing.data)) {
                std::memcpy(out,&backing.data,size);return true;
            }
            return read(address,out,size);
        };
        assert(tglPublishOwnedDndi(owner,child,source.identity,source,output,ownerRead)==0);
        assert(backing.data.words[12]==192 && backing.data.words[0x1f]==4);
        assert(output.luma[0]==576 && output.chroma[3]==2176);
        assert(tglPublishOwnedDndiInFrame(owner,7,child,source.identity,source,output,ownerRead)==5);
        assert(owner.withExecution([&](uintptr_t execution) noexcept {
            assert(owner.hasExecutionLease(execution));
            assert(tglPublishOwnedDndiInFrame(owner,execution+1,child,
                source.identity,source,output,ownerRead)==5);
            return tglPublishOwnedDndiInFrame(owner,execution,child,
                source.identity,source,output,ownerRead);
        })==0);
        assert(!owner.hasExecutionLease(7));
        TestVeboxChildLease childLease{reinterpret_cast<void*>(child)};
        struct FrameContext {TglOwnedVeboxProducerPass pass;} frame;
        using Resolve=TglOwnedVeboxScopedContext<FrameContext>;
        for(int prepareStatus:{0,31,-7}) for(int setupStatus:{0,31,-7})
        for(int builderStatus:{0,31,-7}) {
            unsigned stages=0;
            const auto check=[&](void* actual) noexcept {
                assert(actual==childLease.child && Resolve::get(actual)==&frame);
                assert(owner.hasExecutionLease(7) && childLease.busy);
                owner.reset();assert(owner.get()==7 && !backing.destroys);
                assert(owner.preparePass()==5);
                ++stages;
            };
            const int result=tglWithOwnedVeboxExecutionFrame(childLease,owner,7,frame,
                [&](void* actual) noexcept {
                    check(actual);
                    assert(tglPublishOwnedDndiInFrame(owner,7,child,
                        source.identity,source,output,ownerRead)==0);
                    return prepareStatus;
                },[&](void* actual) noexcept {check(actual);return setupStatus;},
                [&](void* actual) noexcept {
                    check(actual);frame.pass.gate([]() noexcept {return 0;});return builderStatus;
                });
            assert(result==(prepareStatus ? prepareStatus : setupStatus ? setupStatus : builderStatus));
            assert(stages==unsigned(prepareStatus ? 1 : setupStatus ? 2 : 3));
            assert(!owner.hasExecutionLease(7) && !childLease.busy && !Resolve::get(childLease.child));
        }
        assert(tglWithOwnedVeboxExecutionFrame(childLease,owner,8,frame,
            [](void*) noexcept {assert(false);return 0;},
            [](void*) noexcept {assert(false);return 0;},
            [](void*) noexcept {assert(false);return 0;})==5);
        assert(!owner.hasExecutionLease(7) && !childLease.busy);
        {
            struct ResourceBackend {
                unsigned releases=0;
                int allocate(std::array<uint8_t,0x148>& s,const TglResourceKey&) noexcept {
                    s[0]=1;return 0;
                }
                bool backed(const std::array<uint8_t,0x148>& s) const noexcept {return s[0]==1;}
                void release(std::array<uint8_t,0x148>&) noexcept {++releases;}
            } resourceBackend;
            using Group=TglOwnedResourceGroup<ResourceBackend,3>;
            Group resources(resourceBackend);
            std::array<Group::Request,3> requests{{
                {true,{1920,1080,0x19,0,0,false}},
                {true,{1920,1080,0x19,0,0,false}},
                {false,{}}}};
            const auto initialize=[](size_t,const auto&,bool) noexcept {return 0;};
            const auto denied=[](void*,const auto&) noexcept {assert(false);return 0;};
            assert(tglWithOwnedVeboxBackedExecutionFrame(resources,childLease,owner,7,frame,
                denied,denied,denied)==5);
            assert(resources.ensure(requests,initialize)==0);
            for(int p:{0,31,-7}) for(int s:{0,31,-7}) for(int b:{0,31,-7}) {
                unsigned stages=0;
                const void* snapshot=nullptr;
                const auto check=[&](void* child,const auto& views) noexcept {
                    assert(child==childLease.child && childLease.busy);
                    assert(owner.hasExecutionLease(7) && Resolve::get(child)==&frame);
                    assert(views[0] && views[1] && !views[2]);
                    if(snapshot) assert(snapshot==&views);else snapshot=&views;
                    resources.reset();assert(resources.ensure(requests,initialize)==5);
                    assert(!resources.storage(0) && resourceBackend.releases==0);
                    owner.reset();assert(owner.hasExecutionLease(7));
                    ++stages;
                };
                assert(tglWithOwnedVeboxBackedExecutionFrame(resources,childLease,owner,7,frame,
                    [&](void* child,const auto& views) noexcept {check(child,views);return p;},
                    [&](void* child,const auto& views) noexcept {check(child,views);return s;},
                    [&](void* child,const auto& views) noexcept {
                        check(child,views);frame.pass.gate([]() noexcept {return 0;});return b;
                    })==(p?p:s?s:b));
                assert(stages==unsigned(p?1:s?2:3));
                assert(!owner.hasExecutionLease(7) && !childLease.busy && !Resolve::get(childLease.child));
                assert(resources.storage(0) && resourceBackend.releases==0);
            }
            assert(tglWithOwnedVeboxBackedExecutionFrame(resources,childLease,owner,8,frame,
                denied,denied,denied)==5);
            resources.reset();assert(resourceBackend.releases==2);
        }
        TglOwnedSurfaceStorage surface;
        uintptr_t dnParams=0x5000;float factor=32;
        std::array<uint8_t,8> dnData{};std::memcpy(dnData.data()+4,&factor,4);
        std::memcpy(surface.prefix.data()+0x90,&dnParams,8);
        uint32_t width=1920,height=1080;
        std::memcpy(surface.prefix.data()+0xd8,&width,4);
        std::memcpy(surface.prefix.data()+0xdc,&height,4);
        TglOwnedDndiPackets stackPackets;
        const uintptr_t lp=reinterpret_cast<uintptr_t>(stackPackets.luma.data());
        const uintptr_t cp=reinterpret_cast<uintptr_t>(stackPackets.chroma.data());
        bool packetsAllowed=true;
        const auto packetRead=[&](uintptr_t address,void* out,size_t size) noexcept {
            if(address==7+0xb && size==2) {std::memcpy(out,flags.data()+0xb,2);return true;}
            if(address==dnParams && size==8) {std::memcpy(out,dnData.data(),8);return true;}
            if((address==lp && size==0x30) || (address==cp && size==0x24)) {
                std::memcpy(out,reinterpret_cast<const void*>(address),size);return true;
            }
            return ownerRead(address,out,size);
        };
        const auto admitPackets=[&](uintptr_t l,size_t ln,uintptr_t c,size_t cn) noexcept {
            return packetsAllowed && l==lp && ln==0x30 && c==cp && cn==0x24;
        };
        struct NativeFrame {
            TglOwnedVeboxProducerPass pass;
            struct Leases {uintptr_t child,execution;TglOwnedSurfaceStorage& source;} leases;
        } nativeFrame{{},{child,7,surface}};
        TglOwnedVeboxDndiFrameContext context(nativeFrame,owner,dnParams,packetRead,admitPackets);
        using NativeResolve=TglOwnedVeboxScopedContext<decltype(context)>;
        using NativeCallbacks=TglOwnedVeboxProducerCallbacks<NativeResolve>;
        for(unsigned denied=0;denied<4;++denied) {
            stackPackets.luma.fill(0xabcdef01);stackPackets.chroma.fill(0x12345678);
            const auto saved=stackPackets;
            packetsAllowed=denied!=1;
            const int result=tglWithOwnedVeboxExecutionFrame(childLease,owner,7,context,
                [&](void* actual) noexcept {
                    return NativeCallbacks::dndi(actual,denied==2 ? nullptr : &surface,
                        stackPackets.luma.data(),denied==3 ? stackPackets.luma.data() : stackPackets.chroma.data());
                },[](void*) noexcept {return 0;},
                [&](void*) noexcept {return context.pass.gate([]() noexcept {return 0;});});
            assert(result==(denied ? 5 : 0));
            if(denied) assert(stackPackets.luma==saved.luma && stackPackets.chroma==saved.chroma);
            else assert(stackPackets.luma[0]==576 && stackPackets.chroma[3]==2176);
            assert(!owner.hasExecutionLease(7) && !childLease.busy && !NativeResolve::get(childLease.child));
        }
    }
    Backend pass;
    {
        TglExecutionOwner<Backend> owner(pass);
        assert(owner.preparePass() == 0 && pass.initializes == 1);
        assert(owner.preparePass() == 0 && pass.initializes == 2 && pass.creates == 1);
        pass.result = 31;
        assert(owner.preparePass() == 31 && owner.get() == 0 && pass.destroys == 1);
        pass.result = 0;
        assert(owner.preparePass() == 0 && pass.creates == 2);
        pass.valid = false;
        const auto initialized = pass.initializes;
        assert(owner.preparePass() == 5 && owner.get() == 0);
        assert(pass.initializes == initialized && pass.destroys == 2);
        pass.valid = true;
        assert(owner.preparePass() == 0);
        pass.invalidateOnInit = true;
        assert(owner.preparePass() == 5 && owner.get() == 0 && pass.destroys == 3);
        owner.reset();
        assert(pass.destroys == 3); // failed post-Init shape already unwound
    }
}

static void testVeboxReport() {
    TglVeboxReportState storage;
    {
        constexpr uintptr_t image=0x100000,os=0x200000;
        auto body=TglNativeVeboxChannelBinding::body;
        auto publication=TglNativeVeboxChannelBinding::publication;
        uintptr_t slot=image+0x62cd0;bool readable=true,identity=true,rx=true;
        auto read=[&](uintptr_t p,void* out,size_t n) noexcept {
            if(!readable) return false;
            if(p==os+0xf0 && n==8) {std::memcpy(out,&slot,8);return true;}
            if(p==image+0x62cd0 && n==body.size()) {std::memcpy(out,body.data(),n);return true;}
            if(p==image+0x65fbc && n==publication.size()) {std::memcpy(out,publication.data(),n);return true;}
            return false;
        };
        auto qualify=[&](uintptr_t p) noexcept {return identity && p==image;};
        auto executable=[&](uintptr_t p,size_t n) noexcept {return rx && p==slot && n==body.size();};
        TglNativeVeboxChannelBinding binding;unsigned calls=0;
        auto invoke=[&](uintptr_t entry,uintptr_t context,uint32_t channel) noexcept -> int {
            assert(entry==image+0x62cd0 && context==os && channel==UINT32_MAX);++calls;return 31;
        };
        assert(binding.restore(UINT32_MAX,invoke)==5 && !calls);
        for(size_t i=0;i<body.size();++i) {
            body[i]^=1;assert(!binding.resolve(image,os,read,qualify,executable));body[i]^=1;
            assert(binding.restore(UINT32_MAX,invoke)==5 && !calls);
        }
        for(size_t i=0;i<publication.size();++i) {
            publication[i]^=1;assert(!binding.resolve(image,os,read,qualify,executable));publication[i]^=1;
        }
        for(unsigned gate=0;gate<4;++gate) {
            readable=gate!=0;identity=gate!=1;rx=gate!=2;slot=image+0x62cd0+(gate==3);
            assert(!binding.resolve(image,os,read,qualify,executable));
        }
        readable=identity=rx=true;slot=image+0x62cd0;
        assert(!binding.resolve(UINTPTR_MAX,os,read,qualify,executable));
        assert(!binding.resolve(image,UINTPTR_MAX,read,qualify,executable));
        assert(binding.resolve(image,os,read,qualify,executable));
        assert(binding.restore(UINT32_MAX,invoke)==31 && calls==1);
    }
    for(int stageResult:{0,31,-7}) for(int restoreResult:{0,5,31}) {
        storage.bytes.fill(0xa5);const auto before=storage.bytes;unsigned restores=0;
        auto restore=[&]() noexcept -> int {
            assert(storage.bytes==before);++restores;return restoreResult;
        };
        assert(tglFinishOwnedVeboxRender(storage,stageResult,2,0xfe,0xff,restore)==stageResult);
        assert(restores==1 && storage.bytes[0x24]==1 && storage.bytes[0x25]==1);
        auto expected=before;const uint32_t pipe=2;
        std::memcpy(expected.data()+0xc,&pipe,4);expected[0x24]=1;expected[0x25]=1;
        assert(storage.bytes==expected);
    }
    for(unsigned bits=0;bits<32;++bits) for(uint32_t pipe:{0u,1u,2u,UINT32_MAX})
        for(unsigned answers=0;answers<4;++answers) {
            std::array<uint8_t,0x15> execution{};execution.fill(0xfe);
            for(unsigned i=0;i<5;++i)
                execution[std::array<size_t,5>{8,9,0xb,0xc,0x13}[i]]|=(bits>>i)&1;
            uint8_t first=0xa5,second=0x5a;unsigned calls=0;
            auto query=[&]() noexcept -> bool {assert(calls<2);return (answers>>calls++)&1;};
            tglUpdateVeboxRenderStatisticsFlags(first,second,execution,pipe,query);
            if((bits&2) && pipe==1) {
                assert(first==0xa5 && second==0x5a && !calls);continue;
            }
            unsigned expectedCalls=0;
            auto next=[&]() {return (answers>>expectedCalls++)&1;};
            // Independent branch-by-branch model of the captured native edges.
            uint8_t expectedFirst=1;
            if(!(bits&4) && !(bits&8)) {
                bool enabled=true;
                if(!(bits&16)) enabled=next();
                expectedFirst=enabled ? !(bits&1) : 0;
            }
            uint8_t expectedSecond=0;bool enabled=true;
            if(!(bits&16)) enabled=next();
            if(enabled) expectedSecond=bits&1;
            assert(first==expectedFirst && second==expectedSecond && calls==expectedCalls);
        }
    for (uint32_t mode : {0u, 1u, 2u, UINT32_MAX}) {
        for (unsigned flag = 0; flag < 256; ++flag) {
            storage.bytes.fill(0xa5);
            auto expected = storage.bytes;
            std::memcpy(expected.data() + 0xc, &mode, 4);
            expected[0x24] = (flag ^ 1) & 1;
            expected[0x25] = flag & 1;
            tglFinalizeVeboxReport(storage, mode, uint8_t(flag), uint8_t(flag));
            assert(storage.bytes == expected);
            std::array<uint8_t, 0x26> report{};
            tglExportVeboxReport(storage, 0, report);
            uint32_t exportedMode = 0;
            std::memcpy(&exportedMode, report.data() + 0xc, 4);
            assert(exportedMode == mode && report[0x24] == expected[0x24] &&
                   report[0x25] == expected[0x25]);
        }
    }
    for (unsigned flags = 0; flags < 32; ++flags) {
        std::array<uint8_t, 0x15> execution{};
        execution[0xf] = uint8_t(0xfe | ((flags >> 0) & 1));
        execution[0xb] = uint8_t(0xfe | ((flags >> 1) & 1));
        execution[0x13] = uint8_t(0xfe | ((flags >> 2) & 1));
        execution[0x14] = uint8_t(0xfe | ((flags >> 3) & 1));
        execution[8] = uint8_t(0xfe | ((flags >> 4) & 1));
        storage.bytes.fill(0xa5);
        auto expected = storage.bytes;
        expected[0] = flags & 1; expected[2] = (flags >> 1) & 1;
        if (flags & 4) {
            const uint32_t value = (flags & 8) && !(flags & 16) ? 2 : 3;
            std::memcpy(expected.data() + 4, &value, 4);
        }
        tglUpdateVeboxReport(storage, execution);
        assert(storage.bytes == expected);
    }
    storage.bytes.fill(0xa5);
    storage.reset();
    for (size_t i = 0; i < storage.bytes.size(); ++i)
        assert(storage.bytes[i] == ((i <= 0x1c || (i >= 0x20 && i <= 0x25)) ? 0 : 0xa5));
    std::array<uint8_t, 0x26> state{}, report{}, expected{};
    for (size_t i = 0; i < state.size(); ++i) state[i] = uint8_t(0x80 + i);
    report.fill(0xa5); expected = report;
    for (auto i : {0, 2, 0x13, 0x15, 0x17, 0x19, 0x24, 0x25}) expected[i] = state[i] & 1;
    for (auto i : {4, 5, 6, 7, 0xc, 0xd, 0xe, 0xf, 0x14, 0x16, 0x18, 0x1a})
        expected[i] = state[i];
    expected[0x10] = 1;
    tglExportVeboxReport(state, 0xff, report);
    assert(report == expected);
    std::memcpy(storage.bytes.data(), state.data(), state.size());
    report.fill(0xa5);
    tglExportVeboxReport(storage, 0xff, report);
    assert(report == expected);
    auto alias = state;
    expected = state;
    tglExportVeboxReport(state, 2, expected);
    tglExportVeboxReport(alias, 2, alias);
    assert(alias == expected && alias[0x10] == 0);
}

static void testVeboxPrefix() {
    {
        constexpr uintptr_t image = 0x1000000;
        std::array<uint8_t,16> constructor{
            0x55,0x48,0x89,0xe5,0x53,0x50,0xb8,0x88,
            0x10,0,0,0xe8,0xd0,0x41,0xef,0xff};
        std::array<uint8_t,16> destructor{
            0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x50,
            0x48,0x8d,0x05,0x49,0x9e,0x62,0,0x48};
        bool identity = true, rx = true, readable = true;
        auto read = [&](uintptr_t p, void* out, size_t n) {
            assert(n == 16);
            if (!readable) return false;
            if (p == image+0x12d4e0) std::memcpy(out,constructor.data(),n);
            else { assert(p == image+0x12de30); std::memcpy(out,destructor.data(),n); }
            return true;
        };
        auto qualify = [&](uintptr_t p) { return identity && p == image; };
        auto executable = [&](uintptr_t p, size_t n) {
            assert(n == 16 && (p == image+0x12d4e0 || p == image+0x12de30)); return rx;
        };
        TglVeboxLifetimeBinding b;
        auto resolve = [&] { return b.resolve(image,read,qualify,executable); };
        assert(resolve() && b.construct == image+0x12d4e0 && b.destroy == image+0x12de30);
        for (size_t i = 0; i < 16; ++i) {
            constructor[i] ^= 1; assert(!resolve() && !b.construct && !b.destroy); constructor[i] ^= 1;
            destructor[i] ^= 1; assert(!resolve() && !b.image); destructor[i] ^= 1;
        }
        identity = false; assert(!resolve()); identity = true;
        rx = false; assert(!resolve()); rx = true;
        readable = false; assert(!resolve()); readable = true;
        assert(!b.resolve(UINTPTR_MAX,read,qualify,executable));
        assert(resolve());
        static unsigned destroys = 0;
        auto destroy = +[](void* object) {
            assert(reinterpret_cast<uintptr_t>(object) == 0x1234); ++destroys;
        };
        TglNativeVeboxDestructorInvoker{}(reinterpret_cast<uintptr_t>(destroy),reinterpret_cast<void*>(0x1234));
        assert(destroys == 1);
    }
    {
        // Exercise spilled arguments against owned code, never native Apple code.
        static unsigned constructorCalls = 0;
        auto ctor = +[](void* object, void* os, void* mhw, void* sfc, void* hal,
                        void* history, void* perf, const void* cache, int32_t* status) {
            assert(reinterpret_cast<uintptr_t>(object) == 1);
            assert(reinterpret_cast<uintptr_t>(os) == 2);
            assert(reinterpret_cast<uintptr_t>(mhw) == 3);
            assert(reinterpret_cast<uintptr_t>(sfc) == 4);
            assert(reinterpret_cast<uintptr_t>(hal) == 5);
            assert(reinterpret_cast<uintptr_t>(history) == 6);
            assert(reinterpret_cast<uintptr_t>(perf) == 7);
            assert(reinterpret_cast<uintptr_t>(cache) == 8);
            ++constructorCalls; *status = 31;
        };
        int32_t status = -1;
        TglNativeVeboxConstructorInvoker{}(reinterpret_cast<uintptr_t>(ctor),
            reinterpret_cast<void*>(1),reinterpret_cast<void*>(2),reinterpret_cast<void*>(3),
            reinterpret_cast<void*>(4),reinterpret_cast<void*>(5),reinterpret_cast<void*>(6),
            reinterpret_cast<void*>(7),reinterpret_cast<void*>(8),&status);
        assert(constructorCalls == 1 && status == 31);
    }
    {
        static unsigned calls = 0;
        auto registration = +[](void* os, const void* resource, uint32_t write, uint32_t read) {
            assert(os == reinterpret_cast<void*>(1) && resource == reinterpret_cast<void*>(2));
            assert(write == 0 && read == 1); ++calls; return 31;
        };
        assert(TglNativeRegistrationInvoker{}(reinterpret_cast<uintptr_t>(registration),1,
                                              reinterpret_cast<void*>(2),false,true) == 31);
        assert(calls == 1);
        constexpr uintptr_t image = 0x100000, os = 0x3000;
        uintptr_t target = image+0x645e0;
        std::array<uint8_t,22> anchor{
            0x55,0x48,0x89,0xe5,0x31,0xc0,0x48,0x89,0x7d,0xf8,0x48,
            0x89,0x75,0xf0,0x89,0x55,0xec,0x89,0x4d,0xe8,0x5d,0xc3};
        bool identity = true, rx = true, readable = true;
        auto read = [&](uintptr_t p, void* out, size_t n) {
            if (!readable) return false;
            if (p == os+0x248 && n == 8) std::memcpy(out,&target,8);
            else {
                assert(p == image+0x645e0 && n == anchor.size());
                std::memcpy(out,anchor.data(),n);
            }
            return true;
        };
        auto qualify = [&](uintptr_t p) { return identity && p == image; };
        auto executable = [&](uintptr_t p, size_t n) {
            assert(p == image+0x645e0 && n == anchor.size()); return rx;
        };
        TglNativeRegistrationBinding binding;
        auto resolve = [&] { return binding.resolve(os,image,read,qualify,executable); };
        assert(resolve() && binding.context == os && binding.entry == target);
        for (auto& byte : anchor) {
            byte ^= 1; assert(!resolve() && !binding.entry); byte ^= 1;
        }
        ++target; assert(!resolve()); --target;
        identity = false; assert(!resolve()); identity = true;
        rx = false; assert(!resolve()); rx = true;
        readable = false; assert(!resolve()); readable = true;
        assert(!binding.resolve(UINTPTR_MAX,image,read,qualify,executable));
        assert(resolve());
    }
    {
        TglOwnedSurfaceStorage surface;
        std::array<uint8_t,0x48> descriptor{};
        static const void* expectedSurface;
        expectedSurface = &surface;
        auto convert = +[](const void* input, void* output) {
            assert(input == expectedSurface);
            auto resource = reinterpret_cast<uintptr_t>(input)+0x148;
            std::memcpy(static_cast<uint8_t*>(output)+0x40,&resource,8);
            return 31;
        };
        assert(TglNativeSurfaceConversionInvoker{}(reinterpret_cast<uintptr_t>(convert),
                                                   surface,descriptor) == 31);
        uintptr_t borrowed = 0;
        std::memcpy(&borrowed,descriptor.data()+0x40,8);
        assert(borrowed == reinterpret_cast<uintptr_t>(surface.resource.data()));
        const uint32_t width = 1920, height = 1080, pitch = 2048, format = 0x19;
        std::memcpy(surface.prefix.data()+0xd8,&width,4);
        std::memcpy(surface.prefix.data()+0xdc,&height,4);
        std::memcpy(surface.prefix.data()+0xe0,&pitch,4);
        std::memcpy(surface.prefix.data()+0x130,&format,4);
        const auto converted = tglSurfaceDescriptor(surface);
        uint32_t convertedWidth = 0, convertedPitch = 0;
        std::memcpy(&convertedWidth,converted.data()+0xc,4);
        std::memcpy(&convertedPitch,converted.data()+0x14,4);
        std::memcpy(&borrowed,converted.data()+0x40,8);
        assert(convertedWidth == width && convertedPitch == pitch);
        assert(borrowed == reinterpret_cast<uintptr_t>(surface.resource.data()));
        std::array<TglOwnedSurfaceStorage,5> shells{};
        std::array<const TglOwnedSurfaceStorage*,5> slots{};
        for (size_t i=0; i<slots.size(); ++i) slots[i] = &shells[i];
        const auto command = tglSurfaceCommandSet(slots,0xff);
        TglOwnedSurfaceInputs inputs;
        inputs.surfaces = slots; inputs.di = 0xff;
        assert(tglSurfaceCommandSet(inputs) == command);
        std::array<const TglOwnedSurfaceStorage*,4> ffdi{
            &shells[0],&shells[1],&shells[2],&shells[3]};
        std::array<const TglOwnedSurfaceStorage*,2> ffdn{&shells[3],&shells[4]};
        auto select = [&](uint32_t pipe, bool di, bool iecp, bool dn,
                          int32_t frame, int32_t index) {
            return tglSelectOutputSurface(pipe,di,iecp,dn,frame,index,&surface,ffdi,ffdn);
        };
        assert(select(2,true,true,true,-1,-1).surface == &surface);
        assert(select(0,true,true,true,2,1).surface == &shells[2]);
        assert(select(0,false,true,true,0,1).surface == &shells[1]);
        assert(select(0,false,false,true,0,1).surface == &shells[4]);
        assert(select(1,false,false,false,0,0).valid);
        assert(!select(1,false,false,false,0,0).surface);
        assert(!select(0,false,false,false,0,0).valid);
        assert(!select(3,false,false,false,0,0).valid);
        assert(!select(0,true,false,false,-1,0).valid);
        assert(!select(0,true,false,false,4,0).valid);
        assert(!select(0,false,true,false,0,4).valid);
        assert(!select(0,false,false,true,0,2).valid);
        constexpr uintptr_t execution = 0x2000;
        std::array<int32_t,5> indices{2,3,1,0,1};
        constexpr std::array<size_t,5> offsets{0x1c,0x20,0x28,0x2c,0x30};
        unsigned reads = 0, failAt = 0;
        auto readIndex = [&](uintptr_t p, void* out, size_t n) {
            assert(n == 4);
            if (++reads == failAt) return false;
            for (size_t i=0; i<offsets.size(); ++i) if (p == execution+offsets[i]) {
                std::memcpy(out,&indices[i],4); return true;
            }
            assert(false); return false;
        };
        TglSurfaceIndices decoded;
        assert(tglReadSurfaceIndices(execution,decoded,readIndex));
        assert(decoded.frame0 == 2 && decoded.frame1 == 3 && decoded.dnOut == 1);
        assert(decoded.historyIn == 0 && decoded.historyOut == 1);
        std::array<const TglOwnedSurfaceStorage*,2> stmm{&shells[0],&shells[2]};
        TglOwnedSurfaceInputs produced;
        assert(tglPrepareSurfaceInputs(produced,&surface,&surface,0,true,false,false,
                                       decoded,ffdi,ffdn,stmm));
        assert(produced.surfaces[0] == &surface && produced.surfaces[1] == &shells[2]);
        assert(produced.surfaces[2] == &shells[0] && produced.surfaces[3] == &shells[4]);
        assert(!produced.surfaces[4] && produced.di == 1);
        const auto producedCommand = tglSurfaceCommandSet(produced);
        std::memcpy(&borrowed,producedCommand.data()+0x48+0x40,8);
        assert(borrowed == reinterpret_cast<uintptr_t>(shells[2].resource.data()));
        assert(tglPrepareSurfaceInputs(produced,&surface,nullptr,1,false,false,false,
                                       decoded,ffdi,ffdn,stmm));
        assert(!produced.surfaces[1] && !produced.di);
        const auto before = produced;
        for(int failRole : {-1,0,1,2,3}) {
            auto leased=before;
            unsigned admissions=0;
            const int result=tglPublishOwnedSurfacesToBuilder(leased,&leased,&surface,
                &surface,0,true,false,false,decoded,ffdi,ffdn,stmm,
                [&](const TglOwnedSurfaceStorage&,size_t role) noexcept {
                    ++admissions;return int(role)==failRole?31:0;
                });
            assert(result==(failRole<0?0:31));
            assert(admissions==(failRole<0?4u:unsigned(failRole+1)));
            if(result) assert(leased.surfaces==before.surfaces && leased.di==before.di);
            else assert(leased.surfaces[1]==&shells[2] && leased.di==1);
        }
        assert(tglPublishOwnedSurfacesToBuilder(produced,nullptr,&surface,&surface,0,
            true,false,false,decoded,ffdi,ffdn,stmm,
            [](const TglOwnedSurfaceStorage&,size_t) noexcept {assert(false);return 0;})==5);
        assert(produced.surfaces==before.surfaces && produced.di==before.di);
        stmm[0] = nullptr;
        assert(!tglPrepareSurfaceInputs(produced,&surface,&surface,2,false,false,false,
                                        decoded,ffdi,ffdn,stmm));
        assert(produced.surfaces == before.surfaces && produced.di == before.di);
        stmm[0] = &shells[0];
        assert(!tglPrepareSurfaceInputs(produced,&surface,nullptr,2,false,false,false,
                                        decoded,ffdi,ffdn,stmm));
        for (unsigned failure=1; failure<=5; ++failure) {
            reads = 0; failAt = failure;
            assert(!tglReadSurfaceIndices(execution,decoded,readIndex));
            assert(decoded.frame0 == 2 && decoded.historyOut == 1);
        }
        failAt = 0;
        for (size_t i=0; i<indices.size(); ++i) {
            const int32_t original = indices[i];
            indices[i] = -1;
            assert(!tglReadSurfaceIndices(execution,decoded,readIndex));
            indices[i] = i < 2 ? 4 : 2;
            assert(!tglReadSurfaceIndices(execution,decoded,readIndex));
            indices[i] = original;
        }
        assert(!tglReadSurfaceIndices(UINTPTR_MAX,decoded,readIndex));
        assert(command[0x168] == 1 && command[0x169] == 1);
        for (size_t i=0; i<slots.size(); ++i) {
            std::memcpy(&borrowed,command.data()+i*0x48+0x40,8);
            assert(borrowed == reinterpret_cast<uintptr_t>(shells[i].resource.data()));
        }
        slots[1] = nullptr;
        const auto withoutReference = tglSurfaceCommandSet(slots,0);
        assert(withoutReference[0x168] == 0 && withoutReference[0x169] == 0);
        for (size_t i=0x48; i<0x90; ++i) assert(withoutReference[i] == 0);
        constexpr uintptr_t image = 0x100000;
        std::array<uint8_t,16> bytes{
            0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x20,
            0x48,0x89,0x7d,0xf8,0x48,0x89,0x75,0xf0};
        bool identity = true, rx = true, readable = true;
        auto read = [&](uintptr_t p, void* out, size_t n) {
            assert(p == image+0x52bf0 && n == bytes.size());
            if (!readable) return false;
            std::memcpy(out,bytes.data(),n); return true;
        };
        auto qualify = [&](uintptr_t p) { return identity && p == image; };
        auto executable = [&](uintptr_t p, size_t n) {
            assert(p == image+0x52bf0 && n == 16); return rx;
        };
        TglSurfaceConversionBinding binding;
        auto resolve = [&] { return binding.resolve(image,read,qualify,executable); };
        assert(resolve() && binding.entry == image+0x52bf0);
        for (auto& byte : bytes) {
            byte ^= 1; assert(!resolve() && !binding.entry); byte ^= 1;
        }
        identity = false; assert(!resolve()); identity = true;
        rx = false; assert(!resolve()); rx = true;
        readable = false; assert(!resolve()); readable = true;
        assert(!binding.resolve(UINTPTR_MAX,read,qualify,executable));
        assert(resolve());
    }
    {
        int storage=0;unsigned calls=0,constructors=0;
        void* active=nullptr;void(*reenter)(void*)=nullptr;
        auto callback=[&](void* p) noexcept {
            assert(p==&storage);++calls;
            if(reenter) reenter(active);
        };
        using Scope=TglVeboxTeardownScope<decltype(callback),decltype(callback),decltype(callback)>;
        Scope scope(&storage,callback,callback,callback);active=&scope;
        // Each teardown stage rejects construction, even after object_ has
        // been revoked. Reset reentry also cannot recursively destroy it.
        unsigned* count=&constructors;
        auto attempt=[&]() noexcept {
            scope.reset();
            return scope.constructPrivate(&storage,[&]() noexcept {
                ++*count;return TglOwnedBaseConstruction::ConstructedSuccess;
            });
        };
        void* attemptPointer=&attempt;
        using Attempt=decltype(attempt);
        active=attemptPointer;
        reenter=[](void* p) {
            assert((*static_cast<Attempt*>(p))()==TglOwnedBaseConstruction::NotCalled);
        };
        scope.reset();
        assert(calls==3 && !constructors);
        reenter=nullptr;
        assert(attempt()==TglOwnedBaseConstruction::ConstructedSuccess && constructors==1);
        scope.reset();assert(calls==6);
    }
    {
        unsigned stage = 0;
        int ownedStorage = 0;
        auto cleanup = [&](void* p) noexcept {
            assert(p == &ownedStorage && stage == 0); stage = 1;
        };
        auto destroy = [&](void* p) noexcept {
            assert(p == &ownedStorage && stage == 1); stage = 2;
        };
        auto release = [&](void* p) noexcept {
            assert(p == &ownedStorage && stage == 2); stage = 3;
        };
        {
            TglVeboxTeardownScope scope(&ownedStorage,cleanup,destroy,release);
            scope.reset(); scope.reset();
            assert(stage == 3);
        }
        assert(stage == 3);
        stage = 0;
        { TglVeboxTeardownScope scope(&ownedStorage,cleanup,destroy,release); }
        assert(stage == 3);
        { TglVeboxTeardownScope scope(nullptr,cleanup,destroy,release); }
        assert(stage == 3);
        for(auto outcome:{TglOwnedBaseConstruction::NotCalled,
                          TglOwnedBaseConstruction::ConstructedFailure,
                          TglOwnedBaseConstruction::ConstructedSuccess}) {
            stage=0;unsigned constructors=0;
            TglVeboxTeardownScope scope(nullptr,cleanup,destroy,release);
            auto construct=[&]() noexcept {
                ++constructors;
                assert(scope.constructPrivate(&ownedStorage,[&]() noexcept {
                    ++constructors;return outcome;
                })==TglOwnedBaseConstruction::NotCalled); // reentry cannot construct twice
                return outcome;
            };
            assert(scope.constructPrivate(nullptr,construct)==TglOwnedBaseConstruction::NotCalled);
            assert(!constructors && !stage);
            assert(scope.constructPrivate(&ownedStorage,construct)==outcome && constructors==1);
            assert(stage==(outcome==TglOwnedBaseConstruction::ConstructedFailure ? 3u : 0u));
            if(outcome==TglOwnedBaseConstruction::ConstructedSuccess) {
                assert(scope.constructPrivate(&ownedStorage,construct)==TglOwnedBaseConstruction::NotCalled);
                assert(constructors==1);
            }
            scope.reset();scope.reset();
            assert(stage==(outcome==TglOwnedBaseConstruction::NotCalled ? 0u : 3u));
        }
    }
    {
        constexpr uintptr_t os = 0x5000, hal = 0x6000;
        uintptr_t sku = 0x7000, wa = 0x8000;
        uint32_t platform = 0xdeadbeef, nativePlatform = 12;
        unsigned calls = 0, failAt = 0;
        auto readDependencies = [&](uintptr_t p, void* out, size_t n) {
            if (++calls == failAt) return false;
            const void* data = nullptr;
            if (p == hal+0x78 && n == 8) data = &sku;
            if (p == hal+0x80 && n == 8) data = &wa;
            if (p == os+8 && n == 4) data = &nativePlatform;
            assert(data); std::memcpy(out,data,n); return true;
        };
        for (unsigned failure = 1; failure <= 3; ++failure) {
            TglVeboxPrefix empty;
            calls = 0; failAt = failure;
            assert(!empty.initializeBorrowed(os,hal,0x9000,platform,readDependencies));
            assert(!empty.os && !empty.renderHal && !empty.sku && !empty.wa && !empty.slot38);
            assert(platform == 0xdeadbeef);
        }
        failAt = 0; calls = 0;
        TglVeboxPrefix initialized;
        assert(initialized.initializeBorrowed(os,hal,0x9000,platform,readDependencies));
        assert(calls == 3 && platform == 12 && initialized.os == os &&
               initialized.renderHal == hal && initialized.sku == sku && initialized.wa == wa &&
               initialized.slot38 == 0x9000 && !initialized.vtable && !initialized.slot40);
        assert(!initialized.initializeBorrowed(os,hal,0,platform,readDependencies));
        assert(calls == 3 && initialized.slot38 == 0x9000);
        uintptr_t configuredSurface = 0xa000;
        uint32_t kind = 5, sourceValue = 42;
        uint8_t sourceFlag = 3;
        auto readConfiguration = [&](uintptr_t p, void* out, size_t n) {
            const void* data = nullptr;
            if (p == 0xb098 && n == 8) data = &configuredSurface;
            if (p == 0xa134 && n == 4) data = &kind;
            if (p == 0xb110 && n == 1) data = &sourceFlag;
            if (p == 0xb11c && n == 4) data = &sourceValue;
            if (!data) return false;
            std::memcpy(out,data,n); return true;
        };
        assert(initialized.configure(0xc000,0xb000,readConfiguration));
        assert(initialized.flag48 == 1 && initialized.flag49 == 1 &&
               initialized.sourceValue == 42 && initialized.parentData == 0xc9f0 &&
               initialized.os == os && initialized.renderHal == hal &&
               initialized.sku == sku && initialized.wa == wa && initialized.slot38 == 0x9000);
        TglVeboxPrefix empty;
        assert(!empty.initializeBorrowed(UINTPTR_MAX,hal,0,platform,readDependencies));
        assert(!empty.initializeBorrowed(os,UINTPTR_MAX,0,platform,readDependencies));
        assert(!empty.initializeBorrowed(0,hal,0,platform,readDependencies) && calls == 3);
        sku = wa = 0; // Native constructor copies these verbatim; later gates decide admission.
        assert(empty.initializeBorrowed(os,hal,0,platform,readDependencies));
        assert(!empty.sku && !empty.wa && !empty.slot38);
    }
    TglVeboxPrefix prefix;
    prefix.vtable = 7; prefix.os = 9; prefix.slot40 = 11;
    uintptr_t surface = 0x2000;
    uint32_t kind = 5, value = 0x12345678;
    uint8_t flag = 0xfe;
    bool allowed = true;
    const auto read = [&](uintptr_t p, void *out, size_t n) {
        if (!allowed) return false;
        const void *data = nullptr;
        if (p == 0x1098 && n == 8) data = &surface;
        if (p == 0x2134 && n == 4) data = &kind;
        if (p == 0x1110 && n == 1) data = &flag;
        if (p == 0x111c && n == 4) data = &value;
        if (!data) return false;
        std::memcpy(out, data, n); return true;
    };
    assert(prefix.configure(0x3000, 0x1000, read));
    assert(prefix.flag48 == 0 && prefix.flag49 == 1 && prefix.parentData == 0x39f0 &&
           prefix.sourceValue == value);
    assert(prefix.vtable == 7 && prefix.os == 9 && prefix.slot40 == 11);
    const auto saved = prefix;
    allowed = false; assert(!prefix.configure(0x3000, 0x1000, read)); allowed = true;
    surface = UINTPTR_MAX; assert(!prefix.configure(0x3000, 0x1000, read)); surface = 0x2000;
    assert(!prefix.configure(UINTPTR_MAX, 0x1000, read));
    assert(!prefix.configure(0x3000, UINTPTR_MAX, read));
    assert(prefix.flag48 == saved.flag48 && prefix.flag49 == saved.flag49 &&
           prefix.parentData == saved.parentData && prefix.sourceValue == saved.sourceValue);
    flag = 0xff; kind = 4;
    assert(prefix.configure(0x3000, 0x1000, read) && prefix.flag48 == 1 && prefix.flag49 == 0);
}

static void testVeboxStateNativeOwnership() {
    {
        std::array<uint8_t,0x28> base{};
        std::array<uint8_t,0x80> renderHal{};
        std::array<uint8_t,0x6c> sku{};
        const auto childAddress=reinterpret_cast<uintptr_t>(base.data());
        const auto halAddress=reinterpret_cast<uintptr_t>(renderHal.data());
        auto skuAddress=reinterpret_cast<uintptr_t>(sku.data());
        const uint64_t featureWord=uint64_t{1}<<38;
        std::memcpy(base.data()+0x18,&halAddress,8);
        std::memcpy(base.data()+0x20,&skuAddress,8);
        std::memcpy(renderHal.data()+0x78,&skuAddress,8);
        std::memcpy(sku.data()+0x64,&featureWord,8);
        auto read=[&](uintptr_t p,void* out,size_t n) {
            auto copy=[&](const auto& block) {
                const auto start=reinterpret_cast<uintptr_t>(block.data());
                if(p<start || p-start>block.size() || n>block.size()-(p-start)) return false;
                std::memcpy(out,block.data()+p-start,n);return true;
            };
            return copy(base)||copy(renderHal)||copy(sku);
        };
        uint64_t word=0;
        assert(tglReadOwnedVeboxSkuWord(word,childAddress,halAddress,skuAddress,read));
        assert(word==featureWord);
        TglOwnedStateInputs state;state.dnSpecial=true;
        assert(tglReadOwnedStateSingleVeboxSlice(state,childAddress,halAddress,skuAddress,read));
        assert(state.skuSingleVeboxSlice && state.dnSpecial);
        const uint64_t otherFeatures=~featureWord;
        std::memcpy(sku.data()+0x64,&otherFeatures,8);
        assert(tglReadOwnedStateSingleVeboxSlice(state,childAddress,halAddress,skuAddress,read));
        assert(!state.skuSingleVeboxSlice && state.dnSpecial);
        std::memcpy(sku.data()+0x64,&featureWord,8);
        for(int failed=0;failed<4;++failed) {
            int calls=0;word=0x1234;
            auto fail=[&](uintptr_t p,void* out,size_t n) {return calls++!=failed&&read(p,out,n);};
            assert(!tglReadOwnedVeboxSkuWord(word,childAddress,halAddress,skuAddress,fail));
            assert(word==0x1234);
        }
        ++skuAddress;std::memcpy(renderHal.data()+0x78,&skuAddress,8);--skuAddress;
        assert(!tglReadOwnedVeboxSkuWord(word,childAddress,halAddress,skuAddress,read));
        assert(!tglReadOwnedStateSingleVeboxSlice(state,childAddress,halAddress,skuAddress,read));
        assert(!state.skuSingleVeboxSlice && state.dnSpecial);
        assert(!tglReadOwnedVeboxSkuWord(word,UINTPTR_MAX,halAddress,skuAddress,read));
        assert(!tglReadOwnedVeboxSkuWord(word,childAddress,0,skuAddress,read));
    }
    struct Backend {
        unsigned releases=0;
        void release(std::array<uint8_t,0x148>& resource) noexcept {
            assert(resource[0x20]==0x6b);++releases;
        }
    } backend;
    {
    TglOwnedVeboxStateOwner<Backend> owner(backend);
    std::array<uint8_t,0x188> prepared{};prepared[0x180]=1;
    unsigned calls=0;
    auto native=[&](TglOwnedVeboxStateStorage& packet) noexcept {
        assert(packet[0x180]==1 && packet.data()+0x30==packet.resource.data());
        packet.resource[0x20]=0x6b;++calls;
        owner.reset(); // cannot release while native still has packet storage
        assert(!backend.releases);
        assert(!owner.prepare(prepared));
        return 31; // partial allocation retained until command discard
    };
    assert(owner.withNative(native)==5 && !calls);
    prepared[0x30]=1;assert(!owner.prepare(prepared));prepared[0x30]=0;
    assert(owner.prepare(prepared));
    assert(owner.withNative(native)==31 && calls==1 && !backend.releases);
    assert(!owner.prepare(prepared));
    owner.reset();owner.reset();assert(backend.releases==1);
    assert(owner.withNative(native)==5 && calls==1);
    assert(owner.prepare(prepared));
    assert(owner.withNative([&](TglOwnedVeboxStateStorage& packet) noexcept {
        packet.resource[0x20]=0x6b;return 0;
    })==0);
    // destructor releases the second retained allocation exactly once
    }
    assert(backend.releases==2);
}

static void testExternalResourceStorage() {
    using Storage = std::array<uint8_t,0x148>;
    struct Backend {
        Storage* expected = nullptr;
        unsigned allocations = 0, releases = 0, live = 0;
        int status = 0;
        int allocate(Storage& s, const TglResourceKey&) noexcept {
            assert(&s == expected && !live);
            const uint64_t handle = 123;
            std::memcpy(s.data()+0x20,&handle,8);
            ++allocations; ++live; return status;
        }
        void release(Storage& s) noexcept {
            assert(&s == expected && live == 1);
            --live; ++releases; s.fill(0);
        }
        bool backed(const Storage& s) noexcept { return tglNativeBackingPresent(s); }
    } backend;
    TglOwnedSurfaceStorage surface;
    surface.prefix.fill(0xa5); surface.tail.fill(0x5a);
    backend.expected = &surface.resource;
    TglResourceKey key{64,16,0x19,0,0,false};
    {
        TglOwnedResource<Backend> owner(backend,surface.resource);
        assert(&owner.storage() == &surface.resource);
        auto result = owner.ensure(key);
        assert(!result.status && result.changed && backend.live == 1);
        assert(!owner.ensure(key).changed && backend.allocations == 1);
        owner.reset(); owner.reset(); assert(!backend.live && backend.releases == 1);
        for (auto byte : surface.resource) assert(!byte);
        backend.status = 77;
        assert(owner.ensure(key).status == 77 && !backend.live && backend.releases == 2);
        backend.status = 0; assert(!owner.ensure(key).status);
    }
    assert(!backend.live && backend.allocations == backend.releases);
    for (auto byte : surface.prefix) assert(byte == 0xa5);
    for (auto byte : surface.tail) assert(byte == 0x5a);
    const auto allocations = backend.allocations, releases = backend.releases;
    alignas(8) Storage foreign{}; foreign[0x20] = 42;
    {
        TglOwnedResource<Backend> denied(backend,foreign);
        assert(denied.ensure(key).status == 5); denied.reset();
    }
    assert(foreign[0x20] == 42);
    alignas(8) Storage mutated{};
    {
        TglOwnedResource<Backend> pristine(backend,mutated);
        mutated[0x20] = 43;
        assert(pristine.ensure(key).status == 5); pristine.reset();
    }
    assert(mutated[0x20] == 43);
    struct Misaligned { uint8_t prefix = 0; Storage bytes{}; };
    static_assert(offsetof(Misaligned,bytes) == 1);
    alignas(8) Misaligned misaligned;
    {
        TglOwnedResource<Backend> denied(backend,misaligned.bytes);
        assert(denied.ensure(key).status == 5);
    }
    assert(backend.allocations == allocations && backend.releases == releases);
    std::array<TglOwnedSurfaceStorage,2> surfaces{};
    std::array<Backend,2> backends{};
    for (size_t i = 0; i < 2; ++i) backends[i].expected = &surfaces[i].resource;
    using Group = TglOwnedResourceGroup<Backend,2>;
    std::array<TglResourceStorageReference,2> views{{{surfaces[0].resource},{surfaces[1].resource}}};
    std::array<Group::Request,2> requests{{{true,key},{true,key}}};
    auto prepare = [](size_t, const auto&, bool) noexcept { return 0; };
    {
        Group group(backends,views);
        assert(!group.ensure(requests,prepare));
        for (size_t i = 0; i < 2; ++i) assert(group.storage(i) == &surfaces[i].resource);
    }
    for (const auto& b : backends) assert(b.allocations == 1 && b.releases == 1 && !b.live);
    std::array<TglResourceStorageReference,2> aliases{{{surfaces[0].resource},{surfaces[0].resource}}};
    {
        Group denied(backends,aliases);
        assert(denied.ensure(requests,prepare) == 5 && !denied.storage(0));
    }
    for (const auto& b : backends) assert(b.allocations == 1 && b.releases == 1);
    surfaces[1].resource[0x20] = 99;
    {
        Group refused(backends,views);
        assert(refused.ensure(requests,prepare) == 5);
        assert(!refused.storage(0) && !refused.storage(1));
    }
    assert(backends[0].allocations == 2 && backends[0].releases == 2 && !backends[0].live);
    assert(backends[1].allocations == 1 && backends[1].releases == 1 && !backends[1].live);
    assert(surfaces[1].resource[0x20] == 99); // Refusal never frees someone else's handle.
}

static void testSurfaceStatisticsTransaction() {
    struct Fixture {
        std::array<uint8_t,4096> bytes{};
        unsigned allocations = 0, releases = 0, live = 0, fills = 0;
        int fillStatus = 0;
        bool writable = true;
    } fixture;
    auto allocate = +[](void* context, const void* raw, void* out) noexcept {
        auto& f = *static_cast<Fixture*>(context);
        const auto* p = static_cast<const uint8_t*>(raw);
        auto* s = static_cast<uint8_t*>(out);
        uint32_t type = 0, width = 0, height = 0, format = 0, tile = 0;
        std::memcpy(&type,p,4); std::memcpy(&width,p+0x14,4);
        std::memcpy(&height,p+0x18,4); std::memcpy(&format,p+0x28,4);
        std::memcpy(&tile,p+0x24,4);
        assert(type <= 1);
        if (type == 1) assert(width == 64 && height == 16 && format == 0x19);
        else {
            assert(width <= f.bytes.size() && height == 1 && format == 0x3e && tile == 4);
            f.bytes.fill(0xa5);
            const uintptr_t address = reinterpret_cast<uintptr_t>(f.bytes.data());
            std::memcpy(s+0x50,&address,8); std::memcpy(s+0x10,&width,4);
        }
        const uint64_t handle = 1;
        std::memcpy(s+0x14,&type,4); std::memcpy(s+(type ? 0x58 : 0x20),&handle,8);
        ++f.allocations; ++f.live; return 0;
    };
    auto release = +[](void* context, void* out) noexcept {
        auto& f = *static_cast<Fixture*>(context);
        assert(f.live); --f.live; ++f.releases; std::memset(out,0,0x148);
    };
    std::array<TglNativeResourceBackend,2> backends{
        TglNativeResourceBackend(&fixture,allocate,release,1,4096),
        TglNativeResourceBackend(&fixture,allocate,release,0,4096)};
    using Group = TglOwnedResourceGroup<TglNativeResourceBackend,2>;
    {
        TglNativeResourceBackend::Storage rejected{};
        rejected.fill(0xa5);
        const auto before=rejected;
        for(const auto& key : std::array<TglResourceKey,4>{
            TglResourceKey{UINT32_MAX,2,0x19,0,0,false},
            TglResourceKey{UINT32_MAX,1,0x19,0,0,false},
            TglResourceKey{65536,65536,0x19,0,0,false},
            TglResourceKey{0xfffff001,1,0x19,0,0,false}}) {
            assert(backends[0].allocate(rejected,key)==5);
            assert(rejected==before && !fixture.allocations);
        }
        TglNativeResourceBackend unknownPage(&fixture,allocate,release,1);
        assert(unknownPage.allocate(rejected,{64,16,0x19,0,0,false})==5);
        assert(rejected==before && !fixture.allocations);
    }
    TglStatisticsAllocation layout;
    assert(!tglStatisticsAllocationSize(64,16,layout));
    std::array<Group::Request,2> requests{
        Group::Request{true,{64,16,0x19,0,0,false}},
        Group::Request{true,{layout.bytes,1,0x3e,4,0,false}}};
    {
        Group group(backends);
        auto prepare = [&](size_t i, const auto& resource, bool changed) noexcept {
            if (i != 1 || !changed) return 0;
            auto writable = [&](uintptr_t address, uint32_t bytes) noexcept {
                return fixture.writable && address == reinterpret_cast<uintptr_t>(fixture.bytes.data())
                    && bytes <= fixture.bytes.size();
            };
            auto fill = [&](uintptr_t address, uint32_t bytes, uint8_t value) noexcept {
                ++fixture.fills;
                if (fixture.fillStatus) return fixture.fillStatus;
                std::memset(reinterpret_cast<void*>(address),value,bytes); return 0;
            };
            return tglFillBuffer(resource,layout.bytes,0,writable,fill);
        };
        assert(!group.ensure(requests,prepare) && fixture.live == 2 && fixture.fills == 1);
        assert(fixture.bytes[0] == 0 && fixture.bytes[layout.bytes-1] == 0 && fixture.bytes[layout.bytes] == 0xa5);
        assert(!group.ensure(requests,prepare) && fixture.allocations == 2 && fixture.fills == 1);
        assert(!tglStatisticsAllocationSize(64,20,layout)); requests[1].key.width = layout.bytes;
        fixture.fillStatus = 77;
        assert(group.ensure(requests,prepare) == 77 && !fixture.live);
        assert(!group.storage(0) && !group.storage(1));
        fixture.fillStatus = 0; fixture.writable = false;
        const auto fills = fixture.fills;
        assert(group.ensure(requests,prepare) == 5 && !fixture.live && fixture.fills == fills);
        fixture.writable = true;
        assert(!group.ensure(requests,prepare) && fixture.live == 2);
    }
    assert(!fixture.live && fixture.allocations == fixture.releases);
}

static void testComposedStatisticsBuffer() {
    {
        struct Fixture { unsigned allocations = 0, releases = 0; } fixture;
        auto allocate = +[](void* context, const void* raw, void* out) noexcept {
            auto& f = *static_cast<Fixture*>(context);
            const auto* params = static_cast<const uint8_t*>(raw);
            auto* storage = static_cast<uint8_t*>(out);
            uint32_t type = 0, width = 0, height = 0, format = 0;
            std::memcpy(&type,params,4); std::memcpy(&width,params+0x14,4);
            std::memcpy(&height,params+0x18,4); std::memcpy(&format,params+0x28,4);
            if (f.allocations++ == 0) assert(type == 1 && width == 1920 && height == 1080 && format == 0x19);
            else assert(type == 0 && width == 4096 && height == 1 && format == 0x3e);
            std::memcpy(storage+0x14,&type,4);
            const uint64_t handle = 1;
            std::memcpy(storage+(type == 1 ? 0x58 : 0x20),&handle,8);
            return 0;
        };
        auto release = +[](void* context, void* out) noexcept {
            ++static_cast<Fixture*>(context)->releases;
            std::memset(out,0,0x148);
        };
        std::array<TglNativeResourceBackend,2> backends{
            TglNativeResourceBackend(&fixture,allocate,release,1,4096),
            TglNativeResourceBackend(&fixture,allocate,release,0,4096)};
        {
            using Group = TglOwnedResourceGroup<TglNativeResourceBackend,2>;
            Group group(backends);
            std::array<Group::Request,2> requests{
                Group::Request{true,{1920,1080,0x19,0,0,false}},
                Group::Request{true,{4096,1,0x3e,0,0,false}}};
            auto prepare = [](size_t, const auto&, bool) noexcept { return 0; };
            assert(!group.ensure(requests,prepare) && fixture.allocations == 2);
            assert(group.storage(0) && group.storage(1));
            assert((*group.storage(0))[0x14] == 1 && (*group.storage(1))[0x14] == 0);
            assert(!group.ensure(requests,prepare) && fixture.allocations == 2);
        }
        assert(fixture.releases == 2);
    }
    {
        struct Backend {
            unsigned allocations = 0, releases = 0, live = 0;
            unsigned failAt = 99;
            bool descriptorOnly = false;
            void (*onRelease)(void*) noexcept = nullptr;
            void* releaseContext = nullptr;
            int allocate(std::array<uint8_t,0x148>& s, const TglResourceKey&) noexcept {
                ++live; s[1] = 1; // Failure may still leave partially owned backing.
                if (allocations++ == failAt) return 31;
                s[0] = descriptorOnly ? 0 : 1; return 0;
            }
            void release(std::array<uint8_t,0x148>& s) noexcept {
                if (onRelease) onRelease(releaseContext);
                assert(s[1] && live); --live; ++releases; s.fill(0);
            }
            bool backed(const std::array<uint8_t,0x148>& s) noexcept { return s[0] != 0; }
        };
        using Group = TglOwnedResourceGroup<Backend,3>;
        std::array<Group::Request,3> requests{};
        for (auto& r : requests) { r.required = true; r.key.width = 64; }
        for (unsigned failure = 0; failure < 3; ++failure) {
            Backend b; b.failAt = failure;
            Group group(b);
            auto prepare = [&](size_t i, const auto&, bool changed) noexcept {
                assert(changed && i < failure && !group.storage(i)); return 0;
            };
            assert(group.ensure(requests,prepare) == 31 && !b.live);
            assert(b.releases == failure+1);
            for (size_t i = 0; i < 3; ++i) assert(!group.storage(i));
            // TGL mode0 forwards Allocate's error without calling +60.
            // Recovery must work directly, with no caller-side reset/free.
            auto retry = [](size_t, const auto&, bool changed) noexcept {
                assert(changed); return 0;
            };
            assert(!group.ensure(requests,retry) && b.live == 3);
            for (size_t i = 0; i < 3; ++i) assert(group.storage(i));
        }
        for (unsigned failure = 0; failure < 3; ++failure) {
            Backend b; Group group(b);
            auto prepare = [&](size_t i, const auto&, bool) noexcept { return i == failure ? 77 : 0; };
            assert(group.ensure(requests,prepare) == 77 && !b.live);
            const auto released=b.releases;
            auto retry = [](size_t, const auto&, bool changed) noexcept {
                assert(changed); return 0;
            };
            assert(!group.ensure(requests,retry) && b.live == 3);
            assert(b.releases == released);
        }
        Backend b;
        {
            Group group(b);
            unsigned changedCount = 0;
            auto prepare = [&](size_t i, const auto&, bool changed) noexcept {
                assert(!group.storage(i)); changedCount += changed; return 0;
            };
            assert(!group.ensure(requests,prepare) && b.live == 3 && changedCount == 3);
            unsigned reentries = 0;
            auto reentrant = [&](size_t i, const auto&, bool) noexcept {
                const auto live = b.live;
                group.reset();
                assert(b.live == live && !group.storage(i));
                assert(group.ensure(requests,prepare) == 5);
                assert(b.live == live);
                ++reentries;
                return 0;
            };
            assert(!group.ensure(requests,reentrant) && reentries == 3 && b.live == 3);
            struct ReleaseContext {
                Group& group;
                const std::array<Group::Request,3>& requests;
                unsigned hits = 0;
            } context{group,requests};
            b.releaseContext = &context;
            b.onRelease = [](void* pointer) noexcept {
                auto& c = *static_cast<ReleaseContext*>(pointer);
                ++c.hits;
                c.group.reset();
                auto prepare = [](size_t, const auto&, bool) noexcept { return 0; };
                assert(c.group.ensure(c.requests,prepare) == 5);
                for (size_t i = 0; i < 3; ++i) assert(!c.group.storage(i));
            };
            group.reset();
            assert(context.hits == 3 && !b.live);
            b.onRelease = nullptr;
            assert(!group.ensure(requests,prepare) && b.live == 3);
            for (size_t i = 0; i < 3; ++i) assert(group.storage(i));
            assert(!group.storage(3));
            assert(!group.ensure(requests,prepare) && b.allocations == 6 && changedCount == 6);
            requests[1].required = false;
            assert(!group.ensure(requests,prepare) && b.live == 2 && !group.storage(1));
            requests[0].key.width = 128; b.failAt = b.allocations;
            assert(group.ensure(requests,prepare) == 31 && !b.live);
            b.failAt = 99; b.descriptorOnly = true;
            assert(group.ensure(requests,prepare) == 5 && !b.live);
            b.descriptorOnly = false;
            assert(!group.ensure(requests,prepare) && b.live == 2);
        }
        assert(!b.live);
    }
    struct Fixture {
        std::array<uint8_t, 8192> bytes{};
        unsigned allocations = 0, releases = 0, fills = 0;
        bool writable = true;
    } fixture;
    const auto allocate = +[](void *ctx, const void *params, void *output) noexcept {
        auto &f = *static_cast<Fixture *>(ctx);
        const auto *p = static_cast<const uint8_t *>(params);
        uint32_t size = 0, type = 1;
        std::memcpy(&size, p + 0x14, 4); std::memcpy(&type, p, 4);
        assert(type == 0 && size <= f.bytes.size());
        ++f.allocations; f.bytes.fill(0xa5);
        auto *s = static_cast<uint8_t *>(output);
        const uintptr_t address = reinterpret_cast<uintptr_t>(f.bytes.data()), handle = 7;
        std::memcpy(s + 0x10, &size, 4);
        std::memcpy(s + 0x20, &handle, 8);
        std::memcpy(s + 0x50, &address, 8);
        return 0;
    };
    const auto release = +[](void *ctx, void *output) noexcept {
        ++static_cast<Fixture *>(ctx)->releases;
        std::memset(output, 0, 0x148);
    };
    TglNativeResourceBackend backend(&fixture, allocate, release, 0, 4096);
    const auto initialize = [&](const std::array<uint8_t, 0x148> &resource,
                                uint32_t bytes) noexcept {
        const auto writable = [&](uintptr_t address, uint32_t size) noexcept {
            return fixture.writable && address == reinterpret_cast<uintptr_t>(fixture.bytes.data())
                && size <= fixture.bytes.size();
        };
        const auto fill = [&](uintptr_t, uint32_t size, uint8_t value) noexcept {
            ++fixture.fills;
            std::memset(fixture.bytes.data(), value, size);
            return 0;
        };
        return tglFillBuffer(resource, bytes, 0, writable, fill);
    };
    {
        TglStatisticsResource<TglNativeResourceBackend> statistics(backend);
        assert(statistics.ensure(64, 16, initialize) == 0);
        assert(statistics.layout().bytes == 1280 && fixture.fills == 1);
        for (size_t i = 0; i < fixture.bytes.size(); ++i)
            assert(fixture.bytes[i] == (i < 1280 ? 0 : 0xa5));
        assert(statistics.ensure(64, 16, initialize) == 0 && fixture.fills == 1);
        fixture.writable = false;
        assert(statistics.ensure(128, 16, initialize) == 5);
        assert(statistics.layout().bytes == 0 && fixture.allocations == fixture.releases);
        fixture.writable = true;
        assert(statistics.ensure(128, 16, initialize) == 0 && fixture.fills == 2);
    }
    assert(fixture.allocations == 3 && fixture.releases == 3);
}

static void testBufferFill() {
    std::array<uint8_t, 0x148> resource{};
    std::array<uint8_t, 32> bytes{};
    const auto put = [&](size_t offset, auto value) {
        std::memcpy(resource.data() + offset, &value, sizeof(value));
    };
    const uintptr_t address = reinterpret_cast<uintptr_t>(bytes.data());
    put(0x10, uint32_t{32}); put(0x20, uintptr_t{7}); put(0x50, address);
    unsigned writes = 0;
    bool allowed = true;
    int status = 0;
    const auto writable = [&](uintptr_t p, uint32_t n) noexcept {
        return allowed && p == address && n <= bytes.size();
    };
    const auto fill = [&](uintptr_t p, uint32_t n, uint8_t value) noexcept {
        ++writes; assert(p == address);
        if (!status) std::memset(bytes.data(), value, n);
        return status;
    };
    assert(tglFillBuffer(resource, 16, 0x80, writable, fill) == 0 && writes == 1);
    for (size_t i = 0; i < bytes.size(); ++i) assert(bytes[i] == (i < 16 ? 0x80 : 0));
    allowed = false; assert(tglFillBuffer(resource, 16, 0, writable, fill) == 5);
    allowed = true;
    for (uint32_t n : {0u, 33u, UINT32_MAX})
        assert(tglFillBuffer(resource, n, 0, writable, fill) == 5);
    put(0x14, uint32_t{1}); assert(tglFillBuffer(resource, 16, 0, writable, fill) == 5);
    put(0x14, uint32_t{0}); put(0x20, uintptr_t{0});
    assert(tglFillBuffer(resource, 16, 0, writable, fill) == 5);
    put(0x20, uintptr_t{7}); put(0x50, UINTPTR_MAX);
    assert(tglFillBuffer(resource, 16, 0, writable, fill) == 5 && writes == 1);
    put(0x50, address); status = 31;
    assert(tglFillBuffer(resource, 16, 0, writable, fill) == 31 && writes == 2);
#if defined(__APPLE__)
    // Owned CPU memory only; fixture handle is not a GPU resource claim.
    assert(tglFillBufferNative(resource, 16, 0x55) == 0);
    for (size_t i = 0; i < 16; ++i) assert(bytes[i] == 0x55);
    assert(!tglNativeWritableRange(0, 16));
    mach_vm_address_t ro = 0;
    assert(mach_vm_allocate(mach_task_self(), &ro, 4096, VM_FLAGS_ANYWHERE) == KERN_SUCCESS);
    assert(mach_vm_protect(mach_task_self(), ro, 4096, false, VM_PROT_READ) == KERN_SUCCESS);
    assert(!tglNativeWritableRange(ro, 16));
    put(0x50, static_cast<uintptr_t>(ro));
    assert(tglFillBufferNative(resource, 16, 0) == 5);
    assert(mach_vm_deallocate(mach_task_self(), ro, 4096) == KERN_SUCCESS);
#endif
}

static void testStatisticsLifecycle() {
    struct Backend {
        int allocations = 0, releases = 0, status = 0;
        bool backing = true;
        int allocate(std::array<uint8_t, 0x148> &s, const TglResourceKey &key) noexcept {
            ++allocations;
            assert(key.format == 0x3e && key.height == 1 && !key.compressed);
            s[0] = 1;
            return status;
        }
        bool backed(const std::array<uint8_t, 0x148> &) noexcept { return backing; }
        void release(std::array<uint8_t, 0x148> &) noexcept { ++releases; }
    } backend;
    unsigned fills = 0;
    int fillStatus = 0;
    const auto fill = [&](const std::array<uint8_t, 0x148> &s, uint32_t bytes) noexcept {
        ++fills; assert(s[0] == 1 && bytes != 0); return fillStatus;
    };
    {
        TglStatisticsResource<Backend> resource(backend);
        assert(resource.ensure(1920, 1080, fill) == 0 && fills == 1);
        assert(resource.layout().bytes == 520320);
        assert(resource.ensure(1920, 1080, fill) == 0 && fills == 1);
        assert(resource.ensure(0, 1080, fill) == 5 && resource.layout().bytes == 520320);
        fillStatus = 31;
        assert(resource.ensure(1280, 720, fill) == 31 && resource.layout().bytes == 0);
        assert(backend.allocations == 2 && backend.releases == 2);
        fillStatus = 0; backend.backing = false;
        assert(resource.ensure(1280, 720, fill) == 5 && fills == 2);
        backend.backing = true; backend.status = 25;
        assert(resource.ensure(1280, 720, fill) == 25 && resource.layout().bytes == 0);
        backend.status = 0;
        assert(resource.ensure(1280, 720, fill) == 0 && fills == 3);
    }
    assert(backend.allocations == 5 && backend.releases == 5);
    {
        Backend boundBackend;
        alignas(8) std::array<uint8_t,0x148> shell{};
        {
            TglStatisticsResource<Backend> resource(boundBackend,shell);
            unsigned initializations=0;
            auto initialize=[&](const auto& storage,uint32_t bytes) noexcept {
                assert(&storage==&shell && bytes==520320);++initializations;return 0;
            };
            assert(!resource.ensure(1920,1080,initialize));
            assert(!resource.ensure(1920,1080,initialize) && initializations==1);
            assert(resource.withResource([&](const auto& storage,const auto& layout) noexcept {
                assert(&storage==&shell && layout.bytes==520320);
                resource.reset();assert(boundBackend.releases==0);return 31;
            })==31);
        }
        assert(boundBackend.allocations==1 && boundBackend.releases==1);
        for(auto byte:shell) assert(!byte);
        shell[0]=1; // never adopt a foreign native descriptor
        {
            TglStatisticsResource<Backend> invalid(boundBackend,shell);
            assert(invalid.ensure(1920,1080,fill)==5 && !invalid.layout().bytes);
        }
        assert(shell[0]==1 && boundBackend.allocations==1 && boundBackend.releases==1);
    }
    {
        TglStatisticsResource<Backend> resource(backend);
        unsigned nestedFills=0;
        auto nested=[&](const auto&,uint32_t) noexcept {++nestedFills;return 0;};
        auto guarded=[&](const auto& storage,uint32_t bytes) noexcept {
            const auto allocations=backend.allocations,releases=backend.releases;
            assert(storage[0]==1 && bytes && resource.layout().bytes==0);
            resource.reset();
            assert(resource.ensure(640,480,nested)==5 && nestedFills==0);
            assert(backend.allocations==allocations && backend.releases==releases && storage[0]==1);
            return 0;
        };
        assert(resource.ensure(1920,1080,guarded)==0 && resource.layout().bytes==520320);
        unsigned consumes=0;
        const auto consume=[&](const auto& storage,const auto& layout) noexcept {
            ++consumes;assert(storage[0]==1 && layout.bytes==520320);
            const auto allocations=backend.allocations,releases=backend.releases;
            resource.reset();
            assert(resource.ensure(640,480,nested)==5);
            assert(resource.withResource([](const auto&,const auto&) noexcept {assert(false);return 0;})==5);
            assert(backend.allocations==allocations && backend.releases==releases);
            return 31;
        };
        assert(resource.withResource(consume)==31 && consumes==1);
        backend.backing=false;
        assert(resource.withResource(consume)==5 && consumes==1);
        backend.backing=true;
        resource.reset();assert(resource.layout().bytes==0);
        assert(resource.withResource(consume)==5 && consumes==1);
        auto failing=[&](const auto& storage,uint32_t bytes) noexcept {
            assert(guarded(storage,bytes)==0);return 31;
        };
        assert(resource.ensure(1920,1080,failing)==31 && resource.layout().bytes==0);
        const auto releases=backend.releases;
        resource.reset();assert(backend.releases==releases);
    }
}

static void testSurfaceBoundary() {
    uint32_t w = 0, h = 0;
    for (uint32_t format = 0; format <= 0x33; ++format)
        for (bool di : {false, true}) for (bool align64 : {false, true}) {
            assert(tglSurfaceBoundary(format, 101, 103, 200, 200, di, align64, w, h) == 0);
            const bool two = format == 0x19 || (format >= 0xd && format <= 0x15);
            assert(w == ((di && align64) ? 128u : two ? 102u : 101u));
            assert(h == (format == 0x19 ? 104u :
                   (di && format >= 0xd && format <= 0x13) ? 104u : 103u));
        }
    assert(tglSurfaceBoundary(0, 1920, 1080, 0, 0, false, false, w, h) == 0);
    assert(w == 64 && h == 16);
    assert(tglSurfaceBoundary(0, 32, 8, 0, 0, false, false, w, h) == 0);
    assert(w == 32 && h == 8);
    for (float bad : {-1.0f, INFINITY, NAN, 4294967296.0f}) {
        w = 7; h = 9;
        assert(tglSurfaceBoundary(0, 100, 100, bad, 100, false, false, w, h) == 5);
        assert(w == 7 && h == 9);
        assert(tglSurfaceBoundary(0, 100, 100, 100, bad, false, false, w, h) == 5);
        assert(w == 7 && h == 9);
    }
}

static void testStatisticsOffsets() {
    uint32_t queried = 17;
    for (uint32_t selector = 0; selector < 8; ++selector) {
        queried = 17;
        const int result = tglStatisticsQuery(selector, queried);
        const bool supported = selector == 0 || selector == 2 || selector == 3 || selector == 5;
        assert(result == (supported ? 0 : 31));
        if (!supported) assert(queried == 17);
        else assert(queried == (selector == 0 ? 0u : selector == 2 ? 44u :
                               selector == 3 ? 68u : 128u));
    }
    TglStatisticsAllocation allocation;
    assert(tglStatisticsAllocationSize(1920, 1080, allocation) == 0);
    assert(allocation.rowBytes == 1920 && allocation.blockRows == 270 &&
           allocation.totalRows == 271 && allocation.bytes == 520320);
    assert(tglStatisticsAllocationSize(1, 1, allocation) == 0);
    assert(allocation.rowBytes == 64 && allocation.blockRows == 1 &&
           allocation.totalRows == 17 && allocation.bytes == 1088);
    for (const auto dims : {std::array<uint32_t, 2>{0, 1}, {1, 0},
                            {UINT32_MAX, 1}, {1920, UINT32_MAX}}) {
        assert(tglStatisticsAllocationSize(dims[0], dims[1], allocation) == 5);
        assert(allocation.bytes == 1088 && allocation.totalRows == 17);
    }
    unsigned calls = 0;
    int status = 0;
    const auto query = [&](uint32_t selector, uint32_t &size) noexcept {
        ++calls;
        assert(selector == 5);
        size = 128; // fixture only, not a TGL platform constant
        return status;
    };
    uint32_t first = 0, second = 0;
    for (uint8_t a : {uint8_t{0}, uint8_t{1}, uint8_t{0xfe}, uint8_t{0xff}})
        for (uint8_t b : {uint8_t{0}, uint8_t{1}, uint8_t{0xfe}, uint8_t{0xff}}) {
            const auto before = calls;
            assert(tglStatisticsOffsets(64, 16, a, b, first, second, query) == 0);
            assert(calls == before + 1);
            assert(first == ((b & 1) ? 1152u : (a & 1) ? 1024u : 0u));
            assert(second == ((b & 1) ? 1408u : (a & 1) ? 1152u : 128u));
        }
    status = 31; first = 7; second = 9;
    assert(tglStatisticsOffsets(64, 16, 1, 1, first, second, query) == 31);
    assert(first == 7 && second == 9);
    status = 0;
    assert(tglStatisticsOffsets(UINT32_MAX, 2, 1, 1, first, second, query) == 0);
    assert(first == 126 && second == 382); // exact native wrap, not size validation
}

static void testSurfaceDescriptor() {
    std::array<uint8_t, 0x2a8> shell{};
    const auto put = [&](size_t offset, uint32_t value) {
        std::memcpy(shell.data() + offset, &value, sizeof(value));
    };
    put(0x130, 0x19); put(0xd8, 1920); put(0xdc, 1080);
    put(0xe0, 2048); put(0x13c, 7); put(0xe4, 2);
    put(0xf0, 4096); put(0x100, 10240); put(0x108, 11);
    for (size_t i = 0; i < 16; ++i) shell[0x50 + i] = static_cast<uint8_t>(i + 1);
    shell[0x29a] = 0xfe;
    std::array<uint8_t, 0x48> expected{};
    const auto expect = [&](size_t offset, uint32_t value) {
        std::memcpy(expected.data() + offset, &value, sizeof(value));
    };
    expect(0, 1); expect(8, 0x19); expect(0xc, 1920); expect(0x10, 1080);
    expect(0x14, 2048); expect(0x18, 7); expect(0x24, 14); expect(0x28, 2);
    std::memcpy(expected.data() + 0x2c, shell.data() + 0x50, 16);
    const uintptr_t resource = reinterpret_cast<uintptr_t>(shell.data() + 0x148);
    std::memcpy(expected.data() + 0x40, &resource, sizeof(resource));
    assert(tglSurfaceDescriptor(shell) == expected);
    // All optional-slot combinations; only slots 0/1 copy +f8 metadata.
    put(0xf8, 0x12345678);
    for (unsigned mask = 0; mask < 32; ++mask) {
        std::array<const std::array<uint8_t, 0x2a8> *, 5> slots{};
        std::array<uint8_t, 0x170> command{};
        for (size_t i = 0; i < slots.size(); ++i) if (mask & (1u << i)) {
            slots[i] = &shell;
            std::memcpy(command.data() + i * 0x48, expected.data(), expected.size());
            if (i < 2) std::memcpy(command.data() + i * 0x48 + 0x20,
                                   shell.data() + 0xf8, 4);
        }
        command[0x168] = 1;
        command[0x169] = (mask & 2) ? 1 : 0;
        assert(tglSurfaceCommandSet(slots, 0xff) == command);
    }
    shell[0x29a] = 0xff; expected[4] = 1;
    put(0xe0, 0); expect(0x14, 0); expect(0x24, 0);
    assert(tglSurfaceDescriptor(shell) == expected); // zero pitch never divides
    put(0xe0, 1); put(0xf0, 1); put(0x100, 0); put(0x108, 2);
    expect(0x14, 1); expect(0x24, 1); // native unsigned subtraction/addition wrap
    assert(tglSurfaceDescriptor(shell) == expected);
}

static void testOwnedResourceLifecycle() {
    struct Backend {
        void (*observe)(void*) noexcept=nullptr;
        void* context=nullptr;
        int allocations = 0, releases = 0, status = 0;
        bool makeBacking = true;
        int allocate(std::array<uint8_t, 0x148> &s, const TglResourceKey &) noexcept {
            if(observe) observe(context);
            ++allocations;
            for (auto byte : s) assert(byte == 0);
            s[0] = 1; // partially created descriptor even on failure
            s[1] = makeBacking;
            return status;
        }
        bool backed(const std::array<uint8_t, 0x148> &s) const noexcept {
            if(observe) observe(context);return s[1] != 0;
        }
        void release(std::array<uint8_t, 0x148> &s) noexcept {
            if(observe) observe(context);
            assert(s[0] == 1);
            ++releases;
            // Deliberately leave stale data: the owner must clear it itself.
        }
    } backend;
    using Owner = TglOwnedResource<Backend>;
    const TglResourceKey original{1920, 1080, 0x19, 0, 0, false};
    {
        Owner owner(backend);
        assert(owner.state() == Owner::State::Empty);
        auto r = owner.ensure(original);
        assert(r.status == 0 && r.changed && r.state == Owner::State::Backed);
        r = owner.ensure(original);
        assert(!r.changed && backend.allocations == 1 && backend.releases == 0);
        for (unsigned field = 0; field < 6; ++field) {
            auto key = original;
            switch (field) {
                case 0: ++key.width; break;
                case 1: ++key.height; break;
                case 2: ++key.format; break;
                case 3: ++key.tile; break;
                case 4: ++key.compressionMode; break;
                case 5: key.compressed = true; break;
            }
            r = owner.ensure(key);
            assert(r.status == 0 && r.changed && r.state == Owner::State::Backed);
            r = owner.ensure(original);
            assert(r.changed);
        }
        backend.status = 25;
        auto changed = original;
        ++changed.width;
        const int before = backend.releases;
        r = owner.ensure(changed);
        assert(r.status == 25 && r.state == Owner::State::Empty && !r.changed);
        assert(backend.releases == before + 2); // old + partial replacement
        for (auto byte : owner.storage()) assert(byte == 0);
        owner.reset();
        assert(backend.releases == before + 2); // no double release
        backend.status = 0;
        backend.makeBacking = false;
        r = owner.ensure(original);
        assert(r.status == 0 && r.state == Owner::State::DescriptorOnly && r.changed);
        const int allocated = backend.allocations;
        r = owner.ensure(original);
        assert(r.changed && backend.allocations == allocated + 1); // never reuse absent backing
        backend.makeBacking = true;
        r = owner.ensure(original);
        assert(r.state == Owner::State::Backed);
    }
    assert(backend.allocations == backend.releases);
    {
        Backend guarded;
        Owner owner(guarded);
        struct Context {Owner& owner;const TglResourceKey& key;unsigned calls=0;};
        Context context{owner,original};
        guarded.context=&context;
        guarded.observe=+[](void* raw) noexcept {
            auto& c=*static_cast<Context*>(raw);++c.calls;
            c.owner.reset();
            assert(c.owner.ensure(c.key).status==5);
            assert(c.owner.withStorage([](const auto&) noexcept {assert(false);return 0;})==5);
        };
        assert(owner.ensure(original).status==0 && context.calls==2);
        assert(owner.withStorage([&](const auto& storage) noexcept {
            const auto before=storage;
            owner.reset();assert(owner.ensure(original).status==5);
            assert(storage==before && guarded.releases==0);
            return 31;
        })==31);
        owner.reset();
        assert(context.calls==4 && guarded.allocations==1 && guarded.releases==1);
        guarded.observe=nullptr;guarded.context=nullptr;
        assert(owner.withStorage([](const auto&) noexcept {assert(false);return 0;})==5);
    }
}

static void testOwnedResourceGroupLifetime() {
    struct Backend {
        unsigned allocations=0,releases=0;
        bool present=true;
        int allocate(std::array<uint8_t,0x148>& s,const TglResourceKey&) noexcept {
            ++allocations;s[0]=1;return 0;
        }
        bool backed(const std::array<uint8_t,0x148>& s) const noexcept {return present && s[0]==1;}
        void release(std::array<uint8_t,0x148>&) noexcept {++releases;}
    } backend;
    using Owner=TglOwnedResource<Backend>;
    Owner source(backend),history(backend),missing(backend);
    const TglResourceKey key{1920,1080,0x19,0,0,false};
    assert(source.ensure(key).status==0 && history.ensure(key).status==0);
    unsigned calls=0;
    for(int status:{0,31,-7}) {
        const std::array<Owner*,2> group{&source,&history};
        assert(tglWithOwnedResourceGroupStorage(group,[&](const auto& views) noexcept {
            ++calls;assert(views[0]==&source.storage() && views[1]==&history.storage());
            source.reset();history.reset();
            assert(source.ensure(key).status==5 && history.ensure(key).status==5);
            assert(backend.releases==0);
            assert(tglWithOwnedResourceGroupStorage(group,[](const auto&) noexcept {
                assert(false);return 0;
            })==5);
            return status;
        })==status);
        assert(backend.releases==0); // success does not authorize retirement
    }
    for(const auto group:{std::array<Owner*,2>{&source,&source},
                          std::array<Owner*,2>{&source,nullptr},
                          std::array<Owner*,2>{&source,&missing}}) {
        assert(tglWithOwnedResourceGroupStorage(group,[](const auto&) noexcept {
            assert(false);return 0;
        })==5);
        assert(source.withStorage([](const auto&) noexcept {return 31;})==31);
    }
    assert(calls==3 && backend.allocations==2 && backend.releases==0);
    source.reset();history.reset();assert(backend.releases==2);
    {
        using Group=TglOwnedResourceGroup<Backend,3>;
        Group group(backend);
        std::array<Group::Request,3> requests{{{true,key},{false,{}},{true,key}}};
        const auto prepare=[](size_t,const auto&,bool) noexcept {return 0;};
        assert(group.withStorage([](const auto&) noexcept {assert(false);return 0;})==5);
        assert(group.ensure(requests,prepare)==0);
        backend.present=false;
        assert(group.withStorage([](const auto&) noexcept {assert(false);return 0;})==5);
        assert(backend.releases==2); // failed query is not retirement permission
        backend.present=true;
        for(int status:{0,31,-7}) {
            assert(group.withStorage([&](const auto& views) noexcept {
                assert(views[0] && !views[1] && views[2]);
                assert((*views[0])[0]==1 && (*views[2])[0]==1);
                group.reset();assert(group.ensure(requests,prepare)==5);
                assert(!group.storage(0) && backend.releases==2);
                assert(group.withStorage([](const auto&) noexcept {assert(false);return 0;})==5);
                return status;
            })==status);
            assert(group.storage(0) && group.storage(2) && backend.releases==2);
        }
        group.reset();assert(backend.releases==4);
        assert(group.withStorage([](const auto&) noexcept {assert(false);return 0;})==5);
    }
}

static void testNativeResourceBoundary() {
    using Backend = TglNativeResourceBackend;
    Backend::Storage s{};
    const auto put = [&](size_t offset, auto value) {
        std::memcpy(s.data() + offset, &value, sizeof(value));
    };
    assert(!tglNativeBackingPresent(s));
    put(0x20, uint64_t{1});
    assert(tglNativeBackingPresent(s));
    put(0x20, uint64_t{0});
    put(0x50, uint64_t{1});
    assert(tglNativeBackingPresent(s));
    put(0x14, uint32_t{1});
    assert(!tglNativeBackingPresent(s)); // buffer token is not a surface
    put(0x58, uint64_t{1});
    assert(tglNativeBackingPresent(s));
    for (uint32_t type : {2u, 3u, UINT32_MAX}) {
        put(0x14, type);
        assert(!tglNativeBackingPresent(s));
    }
    struct Fixture {
        int allocations = 0, releases = 0;
        int status = 31;
        bool clearsFailure = false;
        uint32_t rewrittenWidth = 0;
        bool omitBacking=false;
        std::array<uint8_t, 0x50> expected{};
    } fixture;
    const auto allocate = +[](void *ctx, const void *params, void *output) noexcept -> int {
        auto &f = *static_cast<Fixture *>(ctx);
        assert(std::memcmp(params, f.expected.data(), f.expected.size()) == 0);
        ++f.allocations;
        if(f.rewrittenWidth)
            std::memcpy(static_cast<uint8_t*>(const_cast<void*>(params))+0x14,&f.rewrittenWidth,4);
        uint32_t type = 0;
        std::memcpy(&type, params, sizeof(type));
        std::memcpy(static_cast<uint8_t *>(output) + 0x14, &type, sizeof(type));
        const uint64_t token = 1;
        std::memcpy(static_cast<uint8_t *>(output) + (type == 1 ? 0x58 : 0x20),
                    &token, sizeof(token));
        if (f.status && f.clearsFailure) std::memset(output, 0, 0x148);
        if(f.omitBacking) std::memset(output,0,0x148);
        return f.status;
    };
    const auto release = +[](void *ctx, void *output) noexcept {
        ++static_cast<Fixture *>(ctx)->releases;
        std::memset(output, 0, 0x148);
    };
    const TglResourceKey key{4096, 1, 0x3e, 4, 0, false};
    const auto set = [&](size_t offset, uint32_t value) {
        std::memcpy(fixture.expected.data() + offset, &value, sizeof(value));
    };
    set(0x14, key.width); set(0x18, key.height);
    set(0x24, key.tile); set(0x28, key.format);
    assert(tglBufferSizeFits(1, 4096));
    assert(tglBufferSizeFits(0xfffff000u, 4096));
    assert(!tglBufferSizeFits(0xfffff001u, 4096));
    assert(!tglBufferSizeFits(UINT32_MAX, 4096));
    assert(!tglBufferSizeFits(4096, 0));
    assert(!tglBufferSizeFits(4096, 4095));
    assert(!tglBufferSizeFits(4096, uint64_t{1} << 32));
    assert(!tglBufferSizeFits(0, 4096));
    Backend backend(&fixture, allocate, release, 0, 4096);
    TglOwnedResource<Backend> owner(backend);
    auto result = owner.ensure(key);
    assert(result.status == 31 && result.state == decltype(owner)::State::Empty);
    assert(fixture.allocations == 1 && fixture.releases == 1);
    auto oversized = key; oversized.width = UINT32_MAX;
    assert(owner.ensure(oversized).status == 5);
    Backend unknownPage(&fixture, allocate, release, 0);
    TglOwnedResource<Backend> noPage(unknownPage);
    assert(noPage.ensure(key).status == 5);
    assert(fixture.allocations == 1 && fixture.releases == 1);
    auto compressed = key;
    compressed.compressed = true;
    assert(owner.ensure(compressed).status == 25);
    compressed = key; compressed.compressionMode = 1;
    assert(owner.ensure(compressed).status == 25);
    auto zero = key; zero.width = 0;
    assert(owner.ensure(zero).status == 5);
    zero = key; zero.height = 0;
    assert(owner.ensure(zero).status == 5);
    Backend unknown(&fixture, allocate, release, 2);
    TglOwnedResource<Backend> unsupported(unknown);
    assert(unsupported.ensure(key).status == 25);
    Backend missing(&fixture, allocate, nullptr, 0);
    TglOwnedResource<Backend> noRelease(missing);
    assert(noRelease.ensure(key).status == 5);
    assert(fixture.allocations == 1 && fixture.releases == 1);
    fixture.clearsFailure = true;
    assert(owner.ensure(key).status == 31);
    assert(fixture.allocations == 2 && fixture.releases == 1); // native already cleaned up
    fixture.clearsFailure = false;
    fixture.status = 0;
    const TglResourceKey surfaceKey{1920, 1080, 0x19, 2, 0, false};
    TglNativeResourceBinding nativeBinding{reinterpret_cast<uintptr_t>(&fixture),
        reinterpret_cast<uintptr_t>(allocate),reinterpret_cast<uintptr_t>(release)};
    for(unsigned missing=0;missing<3;++missing) {
        auto partial=nativeBinding;
        if(missing==0) partial.context=0;
        if(missing==1) partial.allocate=0;
        if(missing==2) partial.release=0;
        Backend disabled(partial,1,4096);
        Backend::Storage untouched{};untouched.fill(0xa5);
        const auto before=untouched;
        assert(disabled.allocate(untouched,surfaceKey)==5);
        disabled.release(untouched);
        assert(untouched==before && fixture.allocations==2 && fixture.releases==1);
    }
    set(0, 1); set(0x14, surfaceKey.width); set(0x18, surfaceKey.height);
    set(0x24, surfaceKey.tile); set(0x28, surfaceKey.format);
    {
        Backend surfaceBackend(nativeBinding,1,4096);
        TglOwnedResource<Backend> surface(surfaceBackend);
        assert(surface.ensure(surfaceKey).state == decltype(surface)::State::Backed);
        assert(!surface.ensure(surfaceKey).changed);
    }
    assert(fixture.allocations == 3 && fixture.releases == 2);
    {
        TglNativeResourceScope scope(nativeBinding,1,4096);
        assert(scope.ensure(surfaceKey).status == 0);
        scope.close();
        assert(fixture.releases == 3);
        scope.close();
        assert(scope.ensure(surfaceKey).status == 5);
        assert(fixture.allocations == 4 && fixture.releases == 3);
    }
    assert(fixture.releases == 3); // closed destructor cannot double-release
    {
        TglNativeResourceScope scope(&fixture, allocate, release, 1, 4096);
        assert(scope.ensure(surfaceKey).status == 0);
    }
    assert(fixture.allocations == 5 && fixture.releases == 4);
    {
        Backend backend(&fixture,allocate,release,1,4096);
        Backend::Storage storage{};
        fixture.rewrittenWidth=0xcafe;
        uint32_t effective=0xa5a5a5a5;
        fixture.status=31;
        assert(backend.allocate(storage,surfaceKey,&effective)==31 && effective==0xa5a5a5a5);
        backend.release(storage);
        fixture.status=0;
        assert(!backend.allocate(storage,surfaceKey,&effective) && effective==0xcafe);
        assert(effective!=surfaceKey.width && backend.backed(storage));
        backend.release(storage);
        auto invalid=surfaceKey;invalid.width=0;effective=0xa5a5a5a5;
        assert(backend.allocate(storage,invalid,&effective)==5 && effective==0xa5a5a5a5);
    }
    {
        Backend backend(&fixture,allocate,release,1,4096);
        auto key=surfaceKey;key.format=0x3d;
        set(0x28,key.format);fixture.status=0;fixture.rewrittenWidth=2048;
        TglOwnedAllocatedSurface owner(backend);
        TglOwnedSurfaceStorage source;
        source.prefix.fill(0x5a);source.resource.fill(0xff);source.tail.fill(0xcc);
        assert(!owner.copyFrameMetadata(source));
        bool changed=false;
        assert(!owner.surface() && !owner.ensure(key,&changed) && changed);
        const auto* surface=owner.surface();assert(surface);
        const auto originalSurface=*surface;
        assert(owner.copyFrameMetadata(source));
        for(size_t i=0;i<surface->prefix.size();++i) {
            const bool copied=i<4 || (i>=0x30 && i<0x60) || (i>=0x138 && i<0x13c);
            assert(surface->prefix[i]==(copied?0x5a:originalSurface.prefix[i]));
        }
        assert(surface->resource==originalSurface.resource && surface->tail==originalSurface.tail);
        assert(owner.copyFrameMetadata(*surface));
        const auto borrowedAllocations=fixture.allocations;
        const auto borrowedReleases=fixture.releases;
        unsigned borrowedCalls=0;
        assert(owner.withSurface([&](const TglOwnedSurfaceStorage& borrowed) {
            ++borrowedCalls;assert(&borrowed==surface);
            const auto snapshot=borrowed;
            owner.reset();
            bool reentrantChanged=true;
            assert(owner.ensure(key,&reentrantChanged)==5 && !reentrantChanged);
            assert(!owner.copyFrameMetadata(source));
            assert(!owner.surface());
            assert(!owner.withSurface([&](const auto&){assert(false);}));
            assert(borrowed.resource==snapshot.resource && borrowed.prefix==snapshot.prefix);
            assert(fixture.allocations==borrowedAllocations && fixture.releases==borrowedReleases);
        }));
        assert(borrowedCalls==1 && owner.surface()==surface);
        // A cached ready bit cannot authorize a lease after native token loss.
        // Corrupt only the test fixture's descriptor, then restore its identity.
        const auto savedResource=surface->resource;
        auto& fixtureResource=const_cast<TglOwnedSurfaceStorage*>(surface)->resource;
        fixtureResource.fill(0);
        assert(!owner.withSurface([&](const auto&){assert(false);}));
        assert(!owner.copyFrameMetadata(source));
        fixtureResource=savedResource;
        assert(owner.withSurface([](const auto&){}));
        {
            TglOwnedAllocatedSurface destination(backend);
            assert(!destination.copyFrameMetadataFrom(owner));
            assert(!destination.ensure(key));
            assert(destination.copyFrameMetadataFrom(owner));
            assert(destination.copyFrameMetadataFrom(destination));
            const auto* destinationSurface=destination.surface();
            {
                struct NativeReportBackend {
                    std::array<uint8_t,0xd48> bytes{};
                    uintptr_t pointer() noexcept {return reinterpret_cast<uintptr_t>(bytes.data());}
                    uintptr_t create() noexcept {assert(false);return 0;}
                    bool validate(uintptr_t p) noexcept {return p==pointer();}
                    int initialize(uintptr_t) noexcept {assert(false);return 5;}
                    void destroy(uintptr_t) noexcept {assert(false);}
                } reportBackend;
                TglExecutionOwner execution(reportBackend);
                std::array<uint8_t,0x1bc0> baseBytes{};
                TestVeboxChildLease childOwner{baseBytes.data()};
                const uintptr_t report=reportBackend.pointer();
                std::memcpy(baseBytes.data()+0x88,&report,8);
                unsigned calls=0;
                auto native=[&](void* child,const auto& s,const auto& t) noexcept -> int {
                    ++calls;assert(child==baseBytes.data() && &s==surface && &t==destinationSurface);
                    owner.reset();destination.reset();execution.reset();
                    const uintptr_t target=reinterpret_cast<uintptr_t>(&t);
                    assert(tglRunCapturedVeboxFeature(owner,destination,childOwner,execution,
                        child,report,s,t,[](void*,const auto&,const auto&) noexcept -> int {assert(false);return 0;})==5);
                    std::memcpy(reportBackend.bytes.data()+0x60,&target,8);
                    return 31;
                };
                assert(tglRunCapturedVeboxFeature(owner,destination,childOwner,execution,
                    baseBytes.data(),report,*surface,*destinationSurface,native)==5 && !calls);
                assert(owner.withPeerSurface(destination,[&](const auto& s,const auto& t) noexcept {
                    assert(childOwner.withPrivateChild([&](void* child) noexcept {
                        return execution.withBorrowedExecution(report,[&](uintptr_t actual) noexcept {
                            // Admission's live byte view may already be active.
                            return execution.withLeasedAdmissionBytes(actual,[&](auto&) noexcept {
                                return tglRunCapturedVeboxFeature(owner,destination,childOwner,execution,
                                    child,actual,s,t,native);
                            });
                        });
                    })==31);
                    uintptr_t retained=1;std::memcpy(&retained,reportBackend.bytes.data()+0x60,8);
                    assert(!retained && !execution.get());
                    assert(childOwner.withPrivateChild([&](void* child) noexcept {
                        return execution.withBorrowedExecution(report,[&](uintptr_t actual) noexcept {
                            // A foreign output is detached, never silently accepted.
                            assert(tglRunCapturedVeboxFeature(owner,destination,childOwner,execution,
                                child,actual,s,t,[&](void*,const auto& src,const auto&) noexcept {
                                    const uintptr_t wrong=reinterpret_cast<uintptr_t>(&src);
                                    std::memcpy(reportBackend.bytes.data()+0x60,&wrong,8);return 0;
                                })==5);
                            std::memcpy(&retained,reportBackend.bytes.data()+0x60,8);assert(!retained);
                            // A pre-existing borrow is not hidden or cleared.
                            const uintptr_t existing=0x1234;
                            std::memcpy(reportBackend.bytes.data()+0x60,&existing,8);
                            assert(tglRunCapturedVeboxFeature(owner,destination,childOwner,execution,
                                child,actual,s,t,[](void*,const auto&,const auto&) noexcept -> int {assert(false);return 0;})==5);
                            std::memcpy(&retained,reportBackend.bytes.data()+0x60,8);assert(retained==existing);
                            const uintptr_t zero=0;std::memcpy(reportBackend.bytes.data()+0x60,&zero,8);
                            assert(tglRunCapturedVeboxFeature(owner,destination,childOwner,execution,
                                child,actual,s,t,[&](void* base,const auto&,const auto&) noexcept {
                                    std::memcpy(static_cast<uint8_t*>(base)+0x88,&existing,8);return 0;
                                })==5);
                            std::memcpy(baseBytes.data()+0x88,&report,8);
                            return 0;
                        });
                    })==0);
                }));
                assert(calls==1 && owner.surface()==surface && destination.surface()==destinationSurface);
            }
            {
                const auto originalSource=*surface;
                const auto originalTarget=*destinationSurface;
                auto sourceMetadata=originalSource,targetMetadata=originalTarget;
                TglSurfaceRect rect{0,0,1920,1081},targetRect{0,0,1920,1200};
                for(size_t offset : {0x30u,0x40u,0x50u})
                    std::memcpy(sourceMetadata.prefix.data()+offset,&rect,sizeof(rect));
                std::memcpy(targetMetadata.prefix.data()+0x40,&targetRect,sizeof(targetRect));
                assert(owner.copyFrameMetadata(sourceMetadata) && destination.copyFrameMetadata(targetMetadata));
                const auto resourcesBefore=destinationSurface->resource;
                const auto allocations=fixture.allocations,releases=fixture.releases;
                std::array<uint8_t,0x139> params{};uint32_t ownedColor=0;
                uintptr_t color=reinterpret_cast<uintptr_t>(&ownedColor);
                std::memcpy(params.data()+0xe0,&color,sizeof(color));
                bool fill=false,compressed=false;auto mode=TglOwnedSfcCompressionMode::Disabled;
                uint32_t compressionFormat=0;int status=99;TglVeboxPipeAcquisition frame;
                unsigned rechecks=0;
                assert(owner.withPeerSurface(destination,[&](const auto& s,const auto& t) noexcept {
                    bool hasColor=false;
                    assert(tglAcquireOwnedColorFillPresence(params,color,frame,hasColor) && hasColor);
                    auto recheck=[&]() noexcept {
                        bool feasible=false;
                        const bool entered=destination.withTemporaryDestinationBottom(t,rect.bottom,
                            [&](const auto& actual) noexcept {
                                ++rechecks;assert(&actual==&t && &s==surface && &t==destinationSurface);
                                assert(actual.resource==resourcesBefore);
                                TglSurfaceRect sourceRect,destinationRect,maximum,actualTarget;
                                std::memcpy(&sourceRect,s.prefix.data()+0x30,16);
                                std::memcpy(&destinationRect,s.prefix.data()+0x40,16);
                                std::memcpy(&maximum,s.prefix.data()+0x50,16);
                                std::memcpy(&actualTarget,actual.prefix.data()+0x40,16);
                                feasible=tglDirectVeboxGeometry(sourceRect,destinationRect,maximum,actualTarget);
                                owner.reset();destination.reset();
                                assert(fixture.allocations==allocations && fixture.releases==releases);
                            });
                        if(!entered) frame.fail();
                        return feasible;
                    };
                    assert(tglRunOwnedGen12FastFill(rect,targetRect,hasColor,fill,compressed,mode,
                        compressionFormat,status,recheck,[]() noexcept -> int {assert(false);return 5;},
                        [&]() noexcept {return frame.healthy();}));
                    TglSurfaceRect restored;std::memcpy(&restored,t.prefix.data()+0x40,16);
                    assert(restored.bottom==1200 && targetRect.bottom==1200 && t.resource==resourcesBefore);
                }));
                assert(rechecks==1 && fill && !compressed && status==0 && frame.healthy());
                assert(owner.copyFrameMetadata(originalSource) && destination.copyFrameMetadata(originalTarget));
            }
            {
                const auto before=destinationSurface->prefix;
                unsigned calls=0;
                auto observe=[&](const TglOwnedSurfaceStorage& view) noexcept {
                    ++calls;assert(&view==destinationSurface && !destination.surface());
                    float bottom=0;std::memcpy(&bottom,view.prefix.data()+0x4c,4);
                    assert(bottom==1081);
                    for(size_t i=0;i<before.size();++i)
                        if(i<0x4c || i>=0x50) assert(view.prefix[i]==before[i]);
                    assert(!destination.withTemporaryDestinationBottom(view,1080,
                        [](const auto&) noexcept {assert(false);}));
                    destination.reset(); // active frame keeps backing alive
                    assert(!destination.surface());
                };
                assert(!destination.withTemporaryDestinationBottom(*destinationSurface,1081,observe));
                assert(destination.withSurface([&](const auto& target) noexcept {
                    assert(!destination.withTemporaryDestinationBottom(*surface,1081,observe));
                    assert(!destination.withTemporaryDestinationBottom(target,
                        std::numeric_limits<float>::infinity(),observe));
                    assert(destination.withTemporaryDestinationBottom(target,1081,observe));
                    assert(target.prefix==before);
                    assert(destination.withTemporaryDestinationBottom(target,1081,observe));
                    assert(target.prefix==before);
                }));
                assert(calls==2 && destination.surface()==destinationSurface && destinationSurface->prefix==before);
            }
            {
                TestFeatureExecutionOwner execution;
                std::array<uint8_t,0x1bc0> child{};
                TestVeboxChildLease childOwner{child.data()};
                uintptr_t report=reinterpret_cast<uintptr_t>(execution.bytes.data());
                std::memcpy(child.data()+0x88,&report,8);
                const auto allocations=fixture.allocations,releases=fixture.releases;
                unsigned featureCalls=0;
                struct ParametersBackend {
                    std::array<uint8_t,8> bytes{};unsigned live=0,releases=0;
                    uintptr_t allocate(size_t n) noexcept {
                        assert(n==bytes.size() && !live);live=1;
                        return reinterpret_cast<uintptr_t>(bytes.data());
                    }
                    void release(uintptr_t p) noexcept {
                        assert(p==reinterpret_cast<uintptr_t>(bytes.data()) && live);
                        live=0;++releases;
                    }
                } parameterBackend;
                TglTrackedCpuOwner parameters(parameterBackend);
                assert(parameters.allocate(8));
                unsigned parameterCalls=0;
                assert(!owner.withNativeDenoiseParameters(*surface,parameters,[&]() noexcept {++parameterCalls;}));
                assert(tglWithOwnedVeboxFeatureFrame(owner,destination,childOwner,execution,
                    [&](void* actual,const auto& s,const auto& t) noexcept {
                        ++featureCalls;
                        assert(actual==child.data() && &s==surface && &t==destinationSurface);
                        owner.reset();destination.reset();
                        assert(owner.ensure(key)==5 && destination.ensure(key)==5);
                        assert(!owner.surface() && !destination.surface());
                        assert(fixture.allocations==allocations && fixture.releases==releases);
                        assert(!destination.withNativeDenoiseParameters(s,parameters,[&]() noexcept {++parameterCalls;}));
                        assert(owner.withNativeDenoiseParameters(s,parameters,[&]() noexcept {
                            ++parameterCalls;uintptr_t p=0;
                            std::memcpy(&p,s.prefix.data()+0x90,8);
                            assert(p==parameters.get() && parameterBackend.live==1);
                            parameters.reset();assert(parameterBackend.live==1);
                            assert(!owner.withNativeDenoiseParameters(s,parameters,
                                [&]() noexcept {++parameterCalls;}));
                            uintptr_t retained=reinterpret_cast<uintptr_t>(&t);
                            std::memcpy(execution.bytes.data()+0x60,&retained,8);
                        }));
                        uintptr_t detached=1;std::memcpy(&detached,s.prefix.data()+0x90,8);
                        assert(!detached && parameters.get() && parameterCalls==1);
                        uintptr_t retained=reinterpret_cast<uintptr_t>(&t);
                        std::memcpy(execution.bytes.data()+0x60,&retained,8);
                        return 31;
                    })==31 && featureCalls==1);
                uintptr_t retained=1;std::memcpy(&retained,execution.bytes.data()+0x60,8);
                assert(!retained && owner.surface()==surface && destination.surface()==destinationSurface);
                assert(parameterBackend.live==1 && !parameterBackend.releases);
                assert(tglWithOwnedVeboxDenoiseFeatureFrame(owner,destination,childOwner,execution,parameters,
                    [&](void* actual,const auto& s,const auto& t) noexcept -> int {
                        assert(actual==child.data() && &s==surface && &t==destinationSurface);
                        uintptr_t borrowed=0;std::memcpy(&borrowed,s.prefix.data()+0x90,8);
                        assert(borrowed==parameters.get());
                        owner.reset();destination.reset();parameters.reset();
                        assert(parameterBackend.live==1 && fixture.releases==releases);
                        borrowed=reinterpret_cast<uintptr_t>(&t);
                        std::memcpy(execution.bytes.data()+0x60,&borrowed,8);
                        return -7;
                    })==-7);
                uintptr_t detached=1;std::memcpy(&detached,surface->prefix.data()+0x90,8);
                std::memcpy(&retained,execution.bytes.data()+0x60,8);
                assert(!detached && !retained && parameters.get() && !parameterBackend.releases);
                parameters.reset();assert(!parameterBackend.live && parameterBackend.releases==1);
                const auto resource=destinationSurface->resource;
                auto& invalid=const_cast<TglOwnedSurfaceStorage*>(destinationSurface)->resource;
                invalid.fill(0);
                assert(tglWithOwnedVeboxFeatureFrame(owner,destination,childOwner,execution,
                    [&](void*,const auto&,const auto&) noexcept {++featureCalls;return 0;})==5);
                assert(featureCalls==1 && !childOwner.busy && !execution.busy);
                assert(owner.surface()==surface && fixture.releases==releases);
                invalid=resource;
            }
            unsigned pairCalls=0;
            assert(owner.withPeerSurface(destination,[&](const auto& own,const auto& other) {
                ++pairCalls;
                assert(&own==surface && &other==destinationSurface);
                owner.reset();destination.reset();
                assert(owner.ensure(key)==5 && destination.ensure(key)==5);
                assert(!owner.surface() && !destination.surface());
            }));
            assert(pairCalls==1 && destination.surface());
            assert(owner.withPeerSurface(owner,[&](const auto& a,const auto& b) {
                ++pairCalls;assert(&a==&b && &a==surface);
            }));
            assert(pairCalls==2);
            const std::array roles{&owner,&destination,&owner,
                static_cast<decltype(&owner)>(nullptr),&destination};
            unsigned roleCalls=0;
            assert(tglWithOwnedSurfaceRoles(roles,[&](const auto& views) noexcept {
                ++roleCalls;
                assert(views[0]==surface && views[1]==destinationSurface &&
                    views[2]==views[0] && !views[3] && views[4]==views[1]);
                owner.reset();destination.reset();
                assert(owner.ensure(key)==5 && destination.ensure(key)==5);
                return 31;
            })==31 && roleCalls==1);
            assert(owner.surface() && destination.surface());
            bool caught=false;
            try {
                owner.withPeerSurface(destination,[](const auto&,const auto&){throw 31;});
            } catch(int status) {caught=status==31;}
            assert(caught && owner.surface() && destination.surface());
            const auto destinationBefore=*destination.surface();
            fixtureResource.fill(0);
            const std::array reverseRoles{&destination,&owner,&destination};
            assert(tglWithOwnedSurfaceRoles(reverseRoles,[&](const auto&) noexcept {
                ++roleCalls;return 0;
            })==5 && roleCalls==1);
            assert(destination.surface()); // earlier unique leases were unwound
            assert(!destination.copyFrameMetadataFrom(owner));
            assert(!destination.withPeerSurface(owner,[](const auto&,const auto&){assert(false);}));
            assert(destination.withSurface([](const auto&){}));
            assert(destination.surface()->prefix==destinationBefore.prefix);
            fixtureResource=savedResource;
            assert(owner.withSurface([&](const auto&) {
                assert(!destination.copyFrameMetadataFrom(owner));
            }));
        }
        uint32_t width=0,height=0,format=0,tile=0;
        std::memcpy(&width,surface->prefix.data()+0xd8,4);
        std::memcpy(&height,surface->prefix.data()+0xdc,4);
        std::memcpy(&format,surface->prefix.data()+0x130,4);
        std::memcpy(&tile,surface->prefix.data()+0xe4,4);
        assert(width==2048 && width!=key.width && height==key.height && format==0x3d && tile==key.tile);
        const auto allocations=fixture.allocations;
        fixture.rewrittenWidth=4096;
        auto effectiveKey=key;effectiveKey.width=2048;
        assert(!owner.ensure(effectiveKey,&changed) && !changed && fixture.allocations==allocations && owner.surface()==surface);
        assert(tglOwnedFfdnHistoryDecision(changed,true,true,true,128,256)==TglOwnedFfdnHistoryDecision::Keep);
        std::memcpy(&width,surface->prefix.data()+0xd8,4);assert(width==2048);
        auto unsupported=key;unsupported.format=0x19;
        assert(owner.ensure(unsupported)==25 && fixture.allocations==allocations && owner.surface()==surface);
        changed=false;
        assert(!owner.ensure(key,&changed) && changed && fixture.allocations==allocations+1);
        std::memcpy(&width,owner.surface()->prefix.data()+0xd8,4);assert(width==4096);
        fixture.status=31;key.height++;
        set(0x18,key.height);
        assert(owner.ensure(key)==31 && !owner.surface());
        const auto releases=fixture.releases;
        owner.reset();owner.reset();assert(fixture.releases==releases);
        fixture.status=0;fixture.omitBacking=true;changed=true;
        assert(owner.ensure(key,&changed)==5 && !owner.surface() && !changed);
        assert(!owner.copyFrameMetadata(source));
        assert(!owner.withSurface([&](const auto&){assert(false);}));
    }
}

static void testResourceBinding() {
    {
        std::array<uint8_t,48> mapped; mapped.fill(0xa5);
        const auto original=mapped;
        for(size_t capacity=0;capacity<40;++capacity) {
            assert(!tglInitializeOwnedStmmMapping(mapped.data(),capacity,8,3,16));
            assert(mapped==original);
        }
        assert(tglInitializeOwnedStmmMapping(mapped.data(),40,8,3,16));
        for(size_t i=0;i<mapped.size();++i)
            assert(mapped[i]==(i<40 && i%16<8 && i%4<2 ? 0xff : 0xa5));
        const auto filled=mapped;
        assert(!tglInitializeOwnedStmmMapping(mapped.data(),mapped.size(),7,3,16));
        assert(!tglInitializeOwnedStmmMapping(mapped.data(),mapped.size(),8,3,7));
        assert(!tglInitializeOwnedStmmMapping(mapped.data(),mapped.size(),8,UINT32_MAX,16));
        assert(!tglInitializeOwnedStmmMapping(nullptr,40,8,3,16));
        assert(mapped==filled);
        struct Api {
            TglOwnedStmmView mapping;
            int surface(uintptr_t handle) noexcept {assert(handle==1);return 1;}
            int lockResult=0,unlockResult=0;
            bool valid=true;
            unsigned locks=0,views=0,unlocks=0;
            int lock(int) noexcept {++locks;return lockResult;}
            int unlock(int) noexcept {++unlocks;return unlockResult;}
            bool view(int,TglOwnedStmmView& out) noexcept {
                ++views;if(!valid) return false;out=mapping;return true;
            }
        };
        for(int lockError : {0,5}) for(int unlockError : {0,31})
            for(bool valid : {false,true}) {
                mapped=original;
                Api api{{mapped.data(),40,8,3,16},lockError,unlockError,valid};
                const auto result=tglInitializeOwnedStmmSurface(1,api);
                assert(result.lockStatus==lockError && api.locks==1);
                assert(api.views==unsigned(!lockError) && api.unlocks==unsigned(!lockError));
                assert(result.initialized==(!lockError&&valid));
                assert(result.ready()==(!lockError&&valid&&!unlockError));
                assert(result.width==(result.ready()?8U:0U));
                assert(result.height==(result.ready()?3U:0U));
                assert(result.pitch==(result.ready()?16U:0U));
                assert(result.requiresQuarantine()==(!lockError&&unlockError!=0));
                if(lockError || !valid) assert(mapped==original);
                else assert(mapped==filled);
            }
        Api badExtent{{mapped.data(),39,8,3,16}};
        const auto failed=tglInitializeOwnedStmmSurface(1,badExtent);
        assert(!failed.initialized && badExtent.unlocks==1 && !failed.unlockStatus);
        assert(!failed.width && !failed.height && !failed.pitch);
        struct RetainedApi : Api {
            unsigned references=1,retains=0,releases=0;
            int expected=1;
            bool denyRetain=false;
            bool retain(int handle) noexcept {
                assert(handle==expected);++retains;
                if(denyRetain) return false;
                ++references;return true;
            }
            void release(int handle) noexcept {
                assert(handle==expected && references);--references;++releases;
            }
        };
        struct Quarantine {
            bool available=true,reserved=false;
            unsigned reservations=0,cancels=0,commits=0;
            int retained=0;
            bool reserve(int handle) noexcept {
                assert(handle==1);
                ++reservations;
                if(!available || reserved || retained) return false;
                reserved=true;return true;
            }
            void cancel() noexcept {assert(reserved);reserved=false;++cancels;}
            void commit(int handle) noexcept {
                assert(reserved && !retained);retained=handle;
                reserved=false;++commits;
            }
        };
        for(int lockError : {0,5}) for(int unlockError : {0,31})
            for(bool valid : {false,true}) {
                RetainedApi api;
                api.mapping={mapped.data(),40,8,3,16};
                api.lockResult=lockError;api.unlockResult=unlockError;api.valid=valid;
                Quarantine sink;
                const auto result=tglInitializeRetainedOwnedStmmSurface(1,api,sink);
                const bool quarantined=!lockError && unlockError;
                assert(result.requiresQuarantine()==quarantined);
                assert(api.retains==1 && api.references==unsigned(1+quarantined));
                assert(sink.commits==unsigned(quarantined));
                assert(sink.cancels==unsigned(!quarantined) && !sink.reserved);
                // Resource-owner unwind must not destroy a still-locked surface.
                api.release(1);
                assert(api.references==unsigned(quarantined));
                if(quarantined) {
                    const auto locks=api.locks;
                    assert(!tglInitializeRetainedOwnedStmmSurface(1,api,sink).ready());
                    assert(api.locks==locks && api.references==1 && sink.retained==1);
                }
            }
        for(bool capacity : {false,true}) {
            RetainedApi api;api.denyRetain=true;
            Quarantine sink;sink.available=capacity;
            assert(!tglInitializeRetainedOwnedStmmSurface(1,api,sink).ready());
            assert(!api.locks && !api.unlocks && api.references==1 && !sink.commits);
            assert(api.retains==unsigned(capacity) && sink.cancels==unsigned(capacity));
        }
        using Pool=TglOwnedStmmProcessQuarantine<int>;
        auto& pool=Pool::processLifetime();
        size_t count=99;
        assert(pool.count(count) && !count);
        {
            Pool::Reservation first(pool),competing(pool);
            assert(!first.reserve(0) && first.reserve(10));
            assert(!first.reserve(10) && !competing.reserve(11));
            count=99;assert(!pool.count(count) && count==99);
            // Reservation destruction cancels capacity, not committed handles.
        }
        assert(pool.count(count) && !count);
        {
            RetainedApi api;api.mapping={mapped.data(),40,8,3,16};
            Pool::Reservation reservation(pool);
            assert(tglInitializeRetainedOwnedStmmSurface(1,api,reservation).ready());
            assert(api.references==1 && api.releases==1);
        }
        assert(pool.count(count) && !count);
        for(int handle : {1,2}) {
            RetainedApi api;api.expected=handle;
            api.mapping={mapped.data(),40,8,3,16};api.unlockResult=31;
            {
                Pool::Reservation reservation(pool);
                assert(tglInitializeRetainedOwnedStmmSurface(handle,api,reservation)
                    .requiresQuarantine());
            }
            api.release(handle); // independent retained reference survives unwind
            assert(api.references==1 && pool.count(count) && count==size_t(handle));
            const auto locks=api.locks,retains=api.retains;
            Pool::Reservation duplicate(pool);
            assert(!tglInitializeRetainedOwnedStmmSurface(handle,api,duplicate).ready());
            assert(api.locks==locks && api.retains==retains && api.references==1);
        }
        {
            RetainedApi api;api.expected=3;
            Pool::Reservation full(pool);
            assert(!tglInitializeRetainedOwnedStmmSurface(3,api,full).ready());
            assert(!api.locks && !api.retains && api.references==1);
            assert(pool.count(count) && count==2);
        }
        Api absent{};
        assert(!tglInitializeOwnedStmmSurface(0,absent).ready() && !absent.locks);
        struct Backend {
            unsigned allocations=0,releases=0;
            int allocate(std::array<uint8_t,0x148>& resource,const TglResourceKey&) noexcept {
                ++allocations;uint32_t type=1,format=0x3d;uintptr_t handle=1;
                std::memcpy(resource.data()+0x14,&type,4);
                std::memcpy(resource.data()+0x18,&format,4);
                std::memcpy(resource.data()+0x58,&handle,8);return 0;
            }
            bool backed(const std::array<uint8_t,0x148>& resource) noexcept {
                return tglNativeBackingPresent(resource);
            }
            void release(std::array<uint8_t,0x148>&) noexcept {++releases;}
        } backend;
        {
            TglOwnedResourceGroup<Backend,1> group(backend);
            using Request=TglOwnedResourceGroup<Backend,1>::Request;
            std::array<Request,1> requests{Request{true,{8,3,0x3d,4,0,false}}};
            for(int lockError : {5,0}) {
                mapped=original;
                Api api{{mapped.data(),40,8,3,16},lockError,0};
                assert(group.ensure(requests,[&](size_t,const auto& resource,bool changed) noexcept {
                    assert(changed);
                    const auto result=tglInitializeOwnedStmmResource(resource,api);
                    return result.status();
                })==lockError);
                assert(api.locks==1 && api.unlocks==unsigned(!lockError));
                assert(bool(group.storage(0))==!lockError);
                assert(backend.releases==1); // failed init unwinds; successful owner retained
            }
        }
        assert(backend.allocations==2 && backend.releases==2);
        std::array<uint8_t,0x148> malformed{};
        Api denied{};
        assert(!tglInitializeOwnedStmmResource(malformed,denied).ready() && !denied.locks);
        RetainedApi retained;
        Quarantine quarantine;
        assert(!tglInitializeRetainedOwnedStmmResource(malformed,retained,quarantine).ready());
        assert(!quarantine.reservations && !retained.retains && !retained.locks);
        const uint32_t type=1,format=0x3d;const uintptr_t handle=1;
        std::memcpy(malformed.data()+0x14,&type,4);
        std::memcpy(malformed.data()+0x18,&format,4);
        std::memcpy(malformed.data()+0x58,&handle,8);
        retained.mapping={mapped.data(),40,8,3,16};retained.unlockResult=31;
        assert(tglInitializeRetainedOwnedStmmResource(malformed,retained,quarantine)
            .requiresQuarantine());
        assert(quarantine.retained==1 && retained.references==2 && !retained.releases);
        struct StmmBackend {
            RetainedApi& api;
            unsigned allocations=0,releases=0;
            bool fail=false;
            bool backingAllowed=true;
            int allocate(std::array<uint8_t,0x148>& storage,const TglResourceKey&) noexcept {
                ++allocations;++api.references;
                const uint32_t type=1,format=0x3d;const uintptr_t handle=1;
                std::memcpy(storage.data()+0x14,&type,4);
                std::memcpy(storage.data()+0x18,&format,4);
                std::memcpy(storage.data()+0x58,&handle,8);
                return fail ? 77 : 0;
            }
            bool backed(const std::array<uint8_t,0x148>& storage) noexcept {
                return backingAllowed && tglNativeBackingPresent(storage);
            }
            void release(std::array<uint8_t,0x148>& storage) noexcept {
                ++releases;api.release(1);storage.fill(0);
            }
        };
        const TglResourceKey stmmKey{8,3,0x3d,4,0,false};
        {
            RetainedApi api;api.references=0;api.mapping={mapped.data(),40,8,3,16};
            Quarantine sink;
            std::array<StmmBackend,2> backends{StmmBackend{api},StmmBackend{api}};
            TglOwnedSurfaceStorage firstSurface{},secondSurface{};
            auto& first=firstSurface.resource;
            auto& second=secondSurface.resource;
            std::array<TglResourceStorageReference,2> shells{{{first},{second}}};
            TglOwnedStmmPair<StmmBackend,RetainedApi,Quarantine> pair(backends,shells,api,sink);
            const std::array<TglResourceKey,2> keys{stmmKey,stmmKey};
            backends[1].fail=true;
            assert(pair.ensure(keys)==77 && !api.references);
            assert(backends[0].releases==1 && backends[1].releases==1);
            bool invoked=false;
            assert(pair.withStorage([&](const auto&,const auto&) noexcept {
                invoked=true;return 0;
            })==5 && !invoked);
            backends[1].fail=false;
            assert(pair.ensure(keys)==0 && api.references==2);
            const auto locks=api.locks;
            assert(pair.ensure(keys)==0 && api.locks==locks);
            TglOwnedSurfaceStorage wrongSurface{};
            assert(pair.withSurfaces(firstSurface,wrongSurface,
                [](auto&,auto&) noexcept {assert(false);return 0;})==5);
            uint32_t before=1;std::memcpy(&before,firstSurface.prefix.data()+0xe0,4);
            assert(before==0);
            assert(pair.withSurfaces(firstSurface,secondSurface,
                [&](auto& a,auto& b) noexcept {
                    uint32_t p0=0,p1=0;
                    std::memcpy(&p0,a.prefix.data()+0xe0,4);
                    std::memcpy(&p1,b.prefix.data()+0xe0,4);
                    assert(p0==16 && p1==16);
                    pair.reset();assert(api.references==2);return 31;
                })==31);
            assert(pair.withInitializedStorage([&](const auto& a,const auto& b,
                const auto& firstLayout,const auto& secondLayout) noexcept {
                const std::array<uint32_t,3> expected{8,3,16};
                assert(&a==&first && &b==&second);
                assert(firstLayout==expected && secondLayout==expected);
                pair.reset();assert(api.references==2);
                assert(pair.ensure(keys)==5);return 31;
            })==31);
            for(size_t denied=0;denied<2;++denied) {
                backends[denied].backingAllowed=false;
                assert(pair.withInitializedStorage(
                    [&](const auto&,const auto&,const auto&,const auto&) noexcept {
                        assert(false);return 0;
                    })==5);
                backends[denied].backingAllowed=true;
            }
            assert(pair.withStorage([&](const auto& a,const auto& b) noexcept {
                assert(&a==&first && &b==&second);
                pair.reset();assert(api.references==2);
                assert(pair.ensure(keys)==5);return 31;
            })==31);
            {
                StmmBackend extra{api};TglOwnedResourceGroup<StmmBackend,1> group(extra);
                using Request=TglOwnedResourceGroup<StmmBackend,1>::Request;
                std::array<Request,1> requests{Request{true,stmmKey}};
                assert(group.ensure(requests,[](size_t,const auto&,bool) noexcept {return 0;})==0);
                int child=0;TestVeboxChildLease childLease{&child};
                struct Frame {TglOwnedVeboxProducerPass pass;} frame;
                // Use the same resource lease API for a mock execution handle;
                // no native context or GPU command is invoked by this test.
                TestVeboxExecutionLease execution;
                unsigned stages=0,failAt=0;
                auto stage=[&](void* actual,const auto& views,const auto& a,const auto& b) noexcept {
                    assert(actual==&child && views[0] && &a==&first && &b==&second);
                    uint32_t p0=0,p1=0;
                    std::memcpy(&p0,firstSurface.prefix.data()+0xe0,4);
                    std::memcpy(&p1,secondSurface.prefix.data()+0xe0,4);
                    assert(p0==16 && p1==16);
                    pair.reset();group.reset();assert(api.references==3);
                    ++stages;
                    if(stages==3) frame.pass.gate([]() noexcept {return 0;});
                    return stages==failAt ? 31 : 0;
                };
                for(unsigned failure=0;failure<=3;++failure) {
                    stages=0;failAt=failure;
                    assert(tglWithOwnedVeboxHistoryFrame(pair,firstSurface,secondSurface,
                        group,childLease,execution,7,frame,
                        stage,stage,stage)==(failure?31:0));
                    assert(stages==(failure?failure:3));
                }
                stages=3;
                assert(tglWithOwnedVeboxHistoryFrame(pair,firstSurface,secondSurface,
                    group,childLease,execution,8,frame,
                    stage,stage,stage)==5 && stages==3);
                stages=0;
                assert(tglWithOwnedVeboxHistoryFrame(pair,firstSurface,wrongSurface,
                    group,childLease,execution,7,frame,
                    stage,stage,stage)==5 && stages==0);
                for(size_t denied=0;denied<2;++denied) {
                    backends[denied].backingAllowed=false;
                    assert(tglWithOwnedVeboxHistoryFrame(pair,firstSurface,secondSurface,
                        group,childLease,execution,7,frame,
                        stage,stage,stage)==5 && stages==0);
                    backends[denied].backingAllowed=true;
                }
                struct BufferBackend {
                    unsigned allocations=0,releases=0;
                    int allocate(std::array<uint8_t,0x148>& storage,const TglResourceKey& key) noexcept {
                        assert(key.format==0x3e);++allocations;storage[0]=1;return 0;
                    }
                    bool backed(const std::array<uint8_t,0x148>& storage) noexcept {return storage[0]==1;}
                    void release(std::array<uint8_t,0x148>&) noexcept {++releases;}
                } buffer;
                TglStatisticsResource<BufferBackend> statistics(buffer);
                TglOwnedVeboxBackingTransaction transaction(group,pair,statistics);
                int fillStatus=31;unsigned fills=0;
                auto fill=[&](const auto&,uint32_t bytes) noexcept {
                    assert(bytes==520320);++fills;
                    pair.reset();group.reset();assert(api.references==3);
                    return fillStatus;
                };
                const auto prepare=[](size_t,const auto&,bool) noexcept {return 0;};
                auto allocateBacking=[&]() noexcept {
                    return childLease.withPrivateChild([&](void* actual) noexcept {
                        auto allocate=[&](void* c) noexcept -> int {
                            assert(c==actual && childLease.busy);
                            return transaction.ensure(requests,prepare,keys,false,1920,1080,fill);
                        };
                        using Context=TglOwnedVeboxAllocationContext<TestVeboxChildLease,decltype(allocate)>;
                        Context context(childLease,allocate,actual);
                        using Scope=TglOwnedVeboxScopedContext<Context>;
                        using Callback=TglOwnedVeboxAllocationCallbacks<Scope>;
                        Scope scope(actual,context);
                        assert(scope.bound());
                        return Callback::allocate(actual);
                    });
                };
                assert(allocateBacking()==31);
                assert(!api.references && buffer.releases==1 && !statistics.layout().bytes);
                fillStatus=0;
                assert(allocateBacking()==0);
                assert(api.references==3 && buffer.allocations==2 && fills==2);
                assert(allocateBacking()==0 && fills==2);
                assert(transaction.withResources([&](const auto& views,const auto& a,const auto& b,
                    const auto& stats,const auto& layout) noexcept {
                    assert(views[0] && &a==&first && &b==&second && stats[0]==1 && layout.bytes);
                    transaction.reset();pair.reset();group.reset();statistics.reset();
                    assert(api.references==3 && buffer.releases==1);
                    assert(transaction.ensure(requests,prepare,keys,false,1920,1080,fill)==5);
                    return 31;
                })==31);
                unsigned graphCalls=0;
                // Same real backing graph remains pinned through all five
                // synchronous stages. No GPU work or completion is simulated.
                for(unsigned failure=0;failure<=5;++failure) for(unsigned gateMode=0;gateMode<3;++gateMode) {
                    unsigned calls=0;
                    auto step=[&](void* actual,unsigned expected) noexcept {
                        assert(actual==&child && childLease.busy && ++calls==expected);
                        assert(TglOwnedVeboxScopedContext<Frame>::get(actual)==&frame);
                        transaction.reset();pair.reset();group.reset();statistics.reset();
                        assert(api.references==3 && buffer.releases==1);
                        assert(childLease.withPrivateChild([](void*) noexcept {return 0;})==5);
                        return failure==expected ? 31 : 0;
                    };
                    const int result=transaction.withResources([&](const auto&,const auto&,const auto&,
                        const auto&,const auto&) noexcept {
                        return tglWithOwnedVeboxSubmittedFrame(childLease,frame,
                            [&](void* c) noexcept {return step(c,1);},
                            [&](void* c) noexcept {return step(c,2);},
                            [&](void* c) noexcept {
                                const int status=step(c,3);
                                if(status) return status;
                                if(gateMode==2) assert(frame.pass.record(77));
                                if(gateMode!=1) frame.pass.gate([]() noexcept {return 0;});
                                return 0;
                            },
                            [&](void* c) noexcept {return step(c,4);},
                            [&](void* c) noexcept {return step(c,5);});
                    });
                    const bool early=failure && failure<=3;
                    const int expected=early ? 31 : gateMode==1 ? 5 : gateMode==2 ? 77 : failure ? 31 : 0;
                    assert(result==expected);
                    assert(calls==(early ? failure : gateMode ? 3u : failure ? failure : 5u));
                    assert(!childLease.busy && api.references==3 && buffer.releases==1);
                    assert(!TglOwnedVeboxScopedContext<Frame>::get(&child));
                }
                auto graph=[&](const auto& views,const auto& a,const auto& b,
                    const auto& stats,const auto& layout) noexcept {
                    ++graphCalls;
                    assert(views[0] && &a==&firstSurface && &b==&secondSurface);
                    uint32_t pa=0,pb=0;
                    std::memcpy(&pa,a.prefix.data()+0xe0,4);
                    std::memcpy(&pb,b.prefix.data()+0xe0,4);
                    assert(pa==16 && pb==16 && stats[0]==1 && layout.bytes);
                    transaction.reset();pair.reset();group.reset();statistics.reset();
                    assert(api.references==3 && buffer.releases==1);
                    return 31;
                };
                assert(transaction.withSurfaces(firstSurface,wrongSurface,graph)==5 && !graphCalls);
                for(size_t denied=0;denied<2;++denied) {
                    backends[denied].backingAllowed=false;
                    assert(transaction.withSurfaces(firstSurface,secondSurface,graph)==5 && !graphCalls);
                    backends[denied].backingAllowed=true;
                }
                assert(transaction.withSurfaces(firstSurface,secondSurface,graph)==31 && graphCalls==1);
                transaction.reset();assert(!api.references && buffer.releases==2);
                // A retired graph cannot republish borrowed FF/history views.
                // Callback success/failure is not a completion certificate;
                // only this explicit CPU-only reset revokes admission here.
                unsigned retiredCalls=0;
                assert(transaction.withResources([&](const auto&,const auto&,const auto&,
                    const auto&,const auto&) noexcept {++retiredCalls;return 0;})==5);
                assert(transaction.withSurfaces(firstSurface,secondSurface,
                    [&](const auto&,const auto&,const auto&,const auto&,const auto&) noexcept {
                        ++retiredCalls;return 0;
                    })==5);
                assert(!retiredCalls && !api.references && buffer.releases==2);
                backends[1].fail=true;
                assert(transaction.ensure(requests,prepare,keys,false,1920,1080,fill)==77);
                assert(!api.references && buffer.allocations==2 && fills==2);
                backends[1].fail=false;
                const auto historyAllocations=backends[0].allocations+backends[1].allocations;
                extra.fail=true;
                assert(transaction.ensure(requests,prepare,keys,false,1920,1080,fill)==77);
                assert(!api.references && buffer.allocations==2 && fills==2);
                assert(backends[0].allocations+backends[1].allocations==historyAllocations);
                extra.fail=false;
                const auto groupAllocations=extra.allocations;
                TglStatisticsResource<BufferBackend> aliasedStatistics(buffer,first);
                TglOwnedVeboxBackingTransaction invalidGraph(group,pair,aliasedStatistics);
                assert(invalidGraph.ensure(requests,prepare,keys,false,1920,1080,fill)==5);
                invalidGraph.reset();
                assert(!api.references && buffer.allocations==2 && extra.allocations==groupAllocations);
                assert(backends[0].allocations+backends[1].allocations==historyAllocations);
                struct LineBackend {
                    unsigned live=0,releases=0;
                    int allocate(std::array<uint8_t,0x148>& s,const TglResourceKey&) noexcept {
                        ++live;s[0]=1;return 0;
                    }
                    bool backed(const std::array<uint8_t,0x148>& s) noexcept {return s[0]==1;}
                    void release(std::array<uint8_t,0x148>&) noexcept {--live;++releases;}
                } lineBackend;
                TglOwnedSfcLineBuffers<LineBackend> lines(lineBackend);
                assert(lines.ensure(1080,720)==0);
                assert(transaction.ensure(requests,prepare,keys,false,1920,1080,fill)==0);
                unsigned retirementCalls=0;
                auto denied=[&]() noexcept {++retirementCalls;return 31;};
                const auto retainedGroup=extra.allocations;
                const auto initialLineLive=lineBackend.live;
                using DeniedContext=TglOwnedVeboxModeReleaseContext<decltype(transaction),decltype(lines),decltype(denied)>;
                DeniedContext deniedContext(&child,transaction,lines,true,denied);
                using DeniedScope=TglOwnedVeboxScopedContext<DeniedContext>;
                using DeniedCallbacks=TglOwnedVeboxModeReleaseCallbacks<DeniedScope>;
                static_assert(std::is_same_v<decltype(&DeniedCallbacks::freeResources),void(*)(void*) noexcept>);
                DeniedCallbacks::freeResources(&child);assert(!retirementCalls);
                {
                    DeniedScope scope(&child,deniedContext);assert(scope.bound());
                    DeniedCallbacks::freeResources(nullptr);DeniedCallbacks::freeResources(&lines);
                    assert(!retirementCalls);
                    DeniedCallbacks::freeResources(&child);
                    assert(deniedContext.status()==31);
                }
                assert(retirementCalls==1 && api.references==3 && lineBackend.live==initialLineLive);
                unsigned reentryAdmissions=0;
                void(*reenterCleanup)(void*) noexcept=nullptr;
                auto reentrantAdmission=[&]() noexcept {
                    ++reentryAdmissions;assert(reenterCleanup);reenterCleanup(&child);return 0;
                };
                using ReentryContext=TglOwnedVeboxModeReleaseContext<decltype(transaction),decltype(lines),decltype(reentrantAdmission)>;
                ReentryContext reentryContext(&child,transaction,lines,true,reentrantAdmission);
                using ReentryScope=TglOwnedVeboxScopedContext<ReentryContext>;
                using ReentryCallbacks=TglOwnedVeboxModeReleaseCallbacks<ReentryScope>;
                reenterCleanup=&ReentryCallbacks::freeResources;
                {
                    ReentryScope scope(&child,reentryContext);assert(scope.bound());
                    ReentryCallbacks::freeResources(&child);
                    assert(reentryContext.status()==5 && reentryAdmissions==1);
                    assert(api.references==3 && statistics.layout().bytes &&
                        lineBackend.live==initialLineLive);
                }
                assert(statistics.layout().bytes);
                // A busy selected owner denies the whole operation BEFORE admission.
                assert(group.withStorage([&](const auto&) noexcept {
                    assert(transaction.releaseModeResources(lines,true,denied)==5);
                    return 0;
                })==0);
                assert(pair.withStorage([&](const auto&,const auto&) noexcept {
                    assert(transaction.releaseModeResources(lines,true,denied)==5);
                    return 0;
                })==0);
                assert(statistics.withResource([&](const auto&,const auto&) noexcept {
                    assert(transaction.releaseModeResources(lines,true,denied)==5);
                    return 0;
                })==0);
                assert(lines.withBuffers(1080,720,[&]() noexcept {
                    assert(transaction.releaseModeResources(lines,true,denied)==5);
                    return 0;
                })==0);
                assert(retirementCalls==1 && api.references==3 && lineBackend.live==initialLineLive);
                auto allowed=[&]() noexcept {
                    ++retirementCalls;
                    transaction.reset();group.reset();pair.reset();statistics.reset();lines.reset();
                    assert(api.references==3 && statistics.layout().bytes && lineBackend.live==initialLineLive);
                    assert(transaction.releaseModeResources(lines,true,denied)==5);
                    return 0;
                };
                using AllowedContext=TglOwnedVeboxModeReleaseContext<decltype(transaction),decltype(lines),decltype(allowed)>;
                AllowedContext allowedContext(&child,transaction,lines,true,allowed);
                using AllowedScope=TglOwnedVeboxScopedContext<AllowedContext>;
                using AllowedCallbacks=TglOwnedVeboxModeReleaseCallbacks<AllowedScope>;
                std::array<uintptr_t,20> cleanupHooks{};
                cleanupHooks=tglBindOwnedVeboxModeReleaseHook<AllowedScope>(cleanupHooks);
                assert(cleanupHooks[4]==reinterpret_cast<uintptr_t>(&AllowedCallbacks::freeResources));
                {
                    AllowedScope scope(&child,allowedContext);assert(scope.bound());
                    AllowedCallbacks::freeResources(&child);
                    assert(allowedContext.status()==0);
                    AllowedCallbacks::freeResources(&child);assert(allowedContext.status()==5);
                }
                assert(retirementCalls==2 && api.references==1 && !statistics.layout().bytes);
                assert(!lineBackend.live && lineBackend.releases==initialLineLive);
                // FFDN remains live/reusable; next ensure recreates only the retired owners.
                assert(transaction.ensure(requests,prepare,keys,false,1920,1080,fill)==0);
                assert(extra.allocations==retainedGroup && api.references==3);
                assert(lines.ensure(1080,720)==0);
                // Disabled SFC cleanup neither reserves nor releases that owner.
                assert(lines.withBuffers(1080,720,[&]() noexcept {
                    assert(transaction.releaseModeResources(lines,false,[]() noexcept {return 0;})==0);
                    assert(lineBackend.live==initialLineLive);
                    return 0;
                })==0);
                assert(api.references==1 && lineBackend.live==initialLineLive);
                transaction.reset();assert(!api.references);
            }
            pair.reset();assert(!api.references);
            std::array<TglResourceStorageReference,2> alias{{{first},{first}}};
            TglOwnedStmmPair<StmmBackend,RetainedApi,Quarantine> invalid(backends,alias,api,sink);
            assert(invalid.ensure(keys)==5 && !api.references);
        }
        for(int lockError : {0,5}) for(int unlockError : {0,31})
            for(bool valid : {false,true}) {
                mapped=original;
                RetainedApi api;api.references=0;
                api.mapping={mapped.data(),40,8,3,16};api.valid=valid;
                api.lockResult=lockError;api.unlockResult=unlockError;
                StmmBackend native{api};Quarantine sink;
                using Owner=TglOwnedStmmResource<StmmBackend,RetainedApi,Quarantine>;
                const int expected=lockError ? lockError : !valid ? 5 : unlockError;
                {
                    Owner owner(native,api,sink);
                    assert(owner.ensure(stmmKey)==expected);
                    bool invoked=false;
                    const int borrowed=owner.withStorage([&](const auto&) noexcept {
                        invoked=true;owner.reset();
                        assert(owner.ensure(stmmKey)==5 && !native.releases);
                        return 0;
                    });
                    assert(invoked==!expected && borrowed==(expected ? 5 : 0));
                    bool metadataInvoked=false;
                    const int metadataStatus=owner.withInitializedStorage(
                        [&](const auto&,uint32_t width,uint32_t height,uint32_t pitch) noexcept {
                            metadataInvoked=true;
                            assert(width==8 && height==3 && pitch==16);
                            owner.reset();assert(!native.releases);
                            return 31;
                        });
                    assert(metadataInvoked==!expected && metadataStatus==(expected?5:31));
                    if(!expected) {
                        mapped[0]=0x23;
                        assert(!owner.ensure(stmmKey) && api.locks==1 && mapped[0]==0x23);
                        assert(!owner.ensure(stmmKey,true) && api.locks==2 && mapped[0]==0xff);
                        assert(native.allocations==1 && !native.releases);
                        owner.reset();
                        assert(owner.withInitializedStorage(
                            [&](const auto&,uint32_t,uint32_t,uint32_t) noexcept {
                                assert(false);return 0;
                            })==5);
                    }
                }
                assert(native.allocations==1 && native.releases==1);
                const bool quarantined=!lockError && unlockError;
                assert(api.references==unsigned(quarantined));
                assert(sink.commits==unsigned(quarantined));
            }
        {
            RetainedApi api;api.references=0;StmmBackend native{api};native.fail=true;
            Quarantine sink;
            TglOwnedStmmResource<StmmBackend,RetainedApi,Quarantine> owner(native,api,sink);
            assert(owner.ensure(stmmKey)==77 && !api.references && !api.locks);
            assert(native.releases==1 && !sink.reservations);
        }
        {
            RetainedApi api;api.references=0;api.mapping={mapped.data(),40,8,3,16};
            StmmBackend native{api};Quarantine sink;
            TglOwnedStmmResource<StmmBackend,RetainedApi,Quarantine> owner(native,api,sink);
            assert(!owner.ensure(stmmKey) && api.references==1);
            api.unlockResult=31;
            assert(owner.ensure(stmmKey,true)==31);
            assert(native.allocations==1 && native.releases==1 && api.references==1);
            assert(sink.retained==1 && sink.commits==1);
            bool invoked=false;
            assert(owner.withStorage([&](const auto&) noexcept {invoked=true;return 0;})==5);
            assert(!invoked);
            owner.reset();
            assert(native.releases==1 && api.references==1);
            const auto locks=api.locks;
            assert(owner.ensure(stmmKey)==5); // quarantined handle may not be locked again
            assert(api.locks==locks && native.allocations==2 && native.releases==2);
            assert(api.references==1 && sink.commits==1);
        }
        {
            RetainedApi api;api.references=0;api.mapping={mapped.data(),40,8,3,16};
            StmmBackend native{api};Quarantine sink;
            TglOwnedSurfaceStorage surface{};
            auto& shell=surface.resource;
            {
                TglOwnedStmmResource<StmmBackend,RetainedApi,Quarantine> owner(native,shell,api,sink);
                assert(!owner.ensure(stmmKey));
                assert(!owner.withStorage([&](const auto& storage) noexcept {
                    assert(&storage==&shell && api.references==1);return 0;
                }));
                TglOwnedSurfaceStorage unrelated{};
                assert(tglWithOwnedStmmSurface(owner,unrelated,
                    [](auto&) noexcept {assert(false);return 0;})==5);
                assert(tglWithOwnedStmmSurface(owner,surface,[&](auto& actual) noexcept {
                    uint32_t pitch=0;std::memcpy(&pitch,actual.prefix.data()+0xe0,4);
                    assert(&actual==&surface && pitch==16);
                    owner.reset();assert(api.references==1);return 31;
                })==31);
            }
            assert(!api.references && native.releases==1);
            for(auto byte:shell) assert(!byte);
        }
    }
    constexpr uintptr_t os = 0x1000, image = 0x100000, renderHal = 0x2000;
    uintptr_t borrowedOs = os;
    uintptr_t allocate = image + 0x63720, release = image + 0x63c00;
    std::array<uint8_t, 16> allocAnchor{
        0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x70,
        0x48,0x89,0x7d,0xf0,0x48,0x89,0x75,0xe8};
    std::array<uint8_t, 16> freeAnchor{
        0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x20,
        0x31,0xc0,0x89,0xc1,0x48,0x89,0x7d,0xf8};
    bool identity = true, rx = true, readable = true;
    const auto read = [&](uintptr_t p, void *out, size_t n) {
        if (!readable) return false;
        const void *source = nullptr;
        if (p == renderHal && n == sizeof(borrowedOs)) source = &borrowedOs;
        if (p == os + 0x1f8 && n == sizeof(allocate)) source = &allocate;
        if (p == os + 0x200 && n == sizeof(release)) source = &release;
        if (p == image + 0x63720 && n == allocAnchor.size()) source = allocAnchor.data();
        if (p == image + 0x63c00 && n == freeAnchor.size()) source = freeAnchor.data();
        if (!source) return false;
        std::memcpy(out, source, n);
        return true;
    };
    const auto qualify = [&](uintptr_t p) { return identity && p == image; };
    const auto executable = [&](uintptr_t p, size_t n) {
        return rx && n == 16 && (p == image + 0x63720 || p == image + 0x63c00);
    };
    TglNativeResourceBinding binding;
    uint64_t pageSize=0xfeed,nativePage=4096;
    uintptr_t pageAddress=0x4000;
    bool pageReadable=true;
    auto pageRead=[&](uintptr_t p,void* out,size_t n) {
        if(!pageReadable || n!=8) return false;
        if(p==image+0x754130) {std::memcpy(out,&pageAddress,8);return true;}
        if(p==0x4000) {std::memcpy(out,&nativePage,8);return true;}
        return false;
    };
    assert(tglReadNativeResourcePageSize(pageSize,image,pageRead,qualify) && pageSize==4096);
    for(uint64_t invalid : {uint64_t{0},uint64_t{3},uint64_t{1}<<32}) {
        nativePage=invalid;
        assert(!tglReadNativeResourcePageSize(pageSize,image,pageRead,qualify) && pageSize==4096);
    }
    nativePage=16384;
    assert(tglReadNativeResourcePageSize(pageSize,image,pageRead,qualify) && pageSize==16384);
    pageReadable=false;
    assert(!tglReadNativeResourcePageSize(pageSize,image,pageRead,qualify) && pageSize==16384);
    pageReadable=true;pageAddress=UINTPTR_MAX;
    assert(!tglReadNativeResourcePageSize(pageSize,image,pageRead,qualify) && pageSize==16384);
    const auto resolve = [&] { return binding.resolve(os, image, read, qualify, executable); };
    const auto empty = [&] {
        assert(!binding.context && !binding.allocate && !binding.release);
    };
    assert(resolve() && binding.context == os && binding.allocate == allocate && binding.release == release);
    identity = false; assert(!resolve()); empty(); identity = true;
    assert(resolve()); rx = false; assert(!resolve()); empty(); rx = true;
    assert(resolve()); readable = false; assert(!resolve()); empty(); readable = true;
    ++allocate; assert(!resolve()); empty(); --allocate;
    ++release; assert(!resolve()); empty(); --release;
    allocAnchor[15] ^= 1; assert(!resolve()); empty(); allocAnchor[15] ^= 1;
    freeAnchor[15] ^= 1; assert(!resolve()); empty(); freeAnchor[15] ^= 1;
    assert(resolve());
    assert(!binding.resolve(UINTPTR_MAX, image, read, qualify, executable)); empty();
    assert(!binding.resolve(os, UINTPTR_MAX, read, qualify, executable)); empty();
    assert(!binding.resolve(0, image, read, qualify, executable)); empty();
    assert(!binding.resolve(os, 0, read, qualify, executable)); empty();
    const auto borrowed = [&] {
        return binding.resolveFromRenderHal(renderHal, image, read, qualify, executable);
    };
    assert(borrowed() && binding.context == os);
    borrowedOs = 0; assert(!borrowed()); empty(); borrowedOs = os;
    assert(borrowed()); readable = false; assert(!borrowed()); empty(); readable = true;
    assert(!binding.resolveFromRenderHal(0, image, read, qualify, executable)); empty();
    assert(!binding.resolveFromRenderHal(UINTPTR_MAX, image, read, qualify, executable)); empty();
    identity = false; assert(!borrowed()); empty();
}

// Offline layout hypothesis only: not an ABI-complete or deployable adapter.
// The pinned consumer sites shift 0x20/0x24/0x228/0x428 by eight bytes.
static void testSfcLineBufferOwnership() {
    using Storage=std::array<uint8_t,0x148>;
    struct Backend {
        unsigned allocations=0,releases=0,live=0,failAt=0;
        uint64_t deniedHandle=0;
        bool descriptorOnly=false;
        void* owner=nullptr;void(*reenter)(void*)=nullptr;
        std::array<uint32_t,16> widths{};
        int allocate(Storage& s,const TglResourceKey& key) noexcept {
            if(reenter) reenter(owner);
            assert(key.height==1 && key.format==0x3d && key.tile==4 &&
                   !key.compressed && key.compressionMode==0);
            widths.at(allocations)=key.width;
            ++allocations;
            const uint64_t handle=allocations;
            std::memcpy(s.data()+0x20,&handle,8);
            ++live;
            return allocations==failAt ? 77 : 0;
        }
        void release(Storage& s) noexcept {
            if(reenter) reenter(owner);
            assert(live); --live; ++releases; s.fill(0);
        }
        bool backed(const Storage& s) noexcept {
            uint64_t handle=0;std::memcpy(&handle,s.data()+0x20,8);
            return !descriptorOnly && handle!=deniedHandle && tglNativeBackingPresent(s);
        }
    } backend;
    {
        TglOwnedSfcLineBuffers<Backend> buffers(backend);
        backend.owner=&buffers;
        backend.reenter=[](void* pointer) {
            auto& owner=*static_cast<TglOwnedSfcLineBuffers<Backend>*>(pointer);
            assert(!owner.tryReset());
            owner.reset();
            assert(owner.ensure(1080,720)==5);
            assert(!owner.storage(0) && !owner.storage(1));
            assert(!owner.matchesDimensions(1080,720));
            assert(owner.withBuffers(1080,720,[]() noexcept {assert(false);return 0;})==5);
            TglOwnedSfcGen12Tail tail;tail.sfdLineBuffer=123;
            assert(!owner.bindSfdRow(tail) && !tail.sfdLineBuffer);
        };
        assert(!buffers.storage(0));
        assert(buffers.ensure(1080,720)==0);
        assert(backend.widths[0]==43200 && backend.widths[1]==11520);
        assert(buffers.storage(0) && buffers.storage(1) && !buffers.storage(2));
        assert(buffers.ensure(1080,720)==0 && backend.allocations==2);
        backend.descriptorOnly=true;
        assert(buffers.withBuffers(1080,720,[]() noexcept {assert(false);return 0;})==5);
        backend.descriptorOnly=false;
        for(size_t slot=0;slot<2;++slot) {
            std::memcpy(&backend.deniedHandle,buffers.storage(slot)->data()+0x20,8);
            assert(buffers.withBuffers(1080,720,[]() noexcept {assert(false);return 0;})==5);
            backend.deniedHandle=0;
            assert(buffers.withBuffers(1080,720,[]() noexcept {return 0;})==0);
        }
        for(int status:{0,31,-7}) {
            const auto allocations=backend.allocations,releases=backend.releases;
            const auto* avs=buffers.storage(0);const auto* ief=buffers.storage(1);
            assert(buffers.withBuffers(1080,720,[&]() noexcept {
                assert(!buffers.tryReset());
                buffers.reset();
                assert(buffers.ensure(1080,721)==5 && buffers.matchesDimensions(1080,720));
                assert(buffers.storage(0)==avs && buffers.storage(1)==ief);
                assert(buffers.withBuffers(1080,720,[]() noexcept {assert(false);return 0;})==5);
                return status;
            })==status);
            assert(backend.allocations==allocations && backend.releases==releases);
        }
        assert(buffers.withBuffers(1080,721,[]() noexcept {assert(false);return 0;})==5);
        assert(buffers.ensure(0,720)==5 && backend.live==2 && buffers.storage(0));
        assert(buffers.ensure(1080,721)==0 && backend.allocations==3 && backend.live==2);
        assert(buffers.tryReset());
        assert(!backend.live && !buffers.storage(0));
        const auto releases=backend.releases;
        assert(buffers.tryReset() && backend.releases==releases);
        assert(buffers.withBuffers(1080,720,[]() noexcept {assert(false);return 0;})==5);
        backend.failAt=backend.allocations+1;
        assert(buffers.ensure(1080,720)==77 && !backend.live && !buffers.storage(1));
        backend.failAt=backend.allocations+2;
        assert(buffers.ensure(1080,720)==77 && !backend.live && !buffers.storage(0));
        backend.failAt=0;
        backend.descriptorOnly=true;
        assert(buffers.ensure(1080,720)==5 && !backend.live);
        backend.descriptorOnly=false;
        assert(buffers.ensure(1080,720)==0 && backend.live==2);
        {
            struct Cpu {
                alignas(8) std::array<uint8_t,0xc00> bytes{};
                bool live=false;
                unsigned* lineLive=nullptr;
                uintptr_t allocate(size_t n) noexcept {
                    assert(!live && (n==0xf8 || n==0xc00));live=true;
                    return reinterpret_cast<uintptr_t>(bytes.data());
                }
                void release(uintptr_t value) noexcept {
                    assert(live && value==reinterpret_cast<uintptr_t>(bytes.data()));
                    assert(!lineLive || !*lineLive);live=false;
                }
            } stateCpu,avsCpu;
            struct Binding {
                uintptr_t image=0x100000;
                bool validateConstructedObject(uintptr_t,uintptr_t,
                    bool(*)(uintptr_t,void*,size_t)) const noexcept {return true;}
            } binding;
            auto noRead=+[](uintptr_t,void*,size_t) noexcept {return false;};
            TglOwnedVpSfcCpuGraph<Cpu,Cpu> graph(stateCpu,avsCpu);
            assert(graph.initialize(0x1000,0x2000,0x3000,binding,noRead,1,0));
            stateCpu.lineLive=avsCpu.lineLive=&backend.live;
            unsigned retired=0;int admission=31;
            auto retire=[&]() noexcept -> int {
                ++retired;assert(!graph.tryReset() && !buffers.tryReset());
                assert(stateCpu.live && avsCpu.live && backend.live==2);
                assert(tglRetireOwnedVpResources(graph,buffers,[]() noexcept -> int {
                    assert(false);return 0;
                })==5);
                return admission;
            };
            assert(graph.withView(false,[&](void*) noexcept {
                return tglRetireOwnedVpResources(graph,buffers,retire);
            })==5 && !retired);
            assert(buffers.withBuffers(1080,720,[&]() noexcept {
                return tglRetireOwnedVpResources(graph,buffers,retire);
            })==5 && !retired);
            const auto releases=backend.releases;
            for(int failure:{31,-7}) {
                admission=failure;
                assert(tglRetireOwnedVpResources(graph,buffers,retire)==failure);
                assert(backend.releases==releases && backend.live==2);
                assert(stateCpu.live && avsCpu.live);
            }
            admission=0;
            assert(tglRetireOwnedVpResources(graph,buffers,retire)==0 && retired==3);
            assert(!backend.live && !stateCpu.live && !avsCpu.live);
            assert(graph.withView(false,[](void*) noexcept -> int {assert(false);return 0;})==5);
        }
        assert(buffers.ensure(1080,720)==0 && backend.live==2);
        TglOwnedVpSfcCallerStorage vp;
        for(int admission:{0,31,-7}) {
            unsigned gates=0;
            auto retire=[&]() noexcept -> int {
                ++gates;
                assert(!buffers.tryReset() && buffers.ensure(1080,721)==5);
                assert(buffers.withBuffers(1080,720,[]() noexcept -> int {
                    assert(false);return 0;
                })==5);
                assert(backend.live==2);
                return admission;
            };
            using Context=TglOwnedVpLineReleaseContext<Backend,decltype(retire)>;
            Context context(vp,buffers,retire);
            using Callback=TglOwnedVpSfcCallbacks<TglOwnedVpSfcResolve<Context>>;
            const auto releases=backend.releases;
            Callback::freeResources(vp.bytes.data());
            assert(context.status()==5 && !gates && backend.releases==releases);
            {
                TglOwnedVpSfcScopedContext<Context> scope(vp.bytes.data(),context);
                assert(scope.bound());
                Callback::freeResources(nullptr);Callback::freeResources(&backend);
                assert(!gates && backend.releases==releases);
                Callback::freeResources(vp.bytes.data());
                assert(gates==1 && context.status()==admission);
                assert(backend.live==(admission ? 2u : 0u));
                const auto after=backend.releases;
                Callback::freeResources(vp.bytes.data());
                assert(context.status()==5 && gates==1 && backend.releases==after);
            }
            Callback::freeResources(vp.bytes.data());assert(gates==1);
            assert(buffers.ensure(1080,720)==0);
        }
        {
            unsigned gates=0;
            void(*reenter)(void*) noexcept=nullptr;
            auto retire=[&]() noexcept -> int {
                ++gates;assert(reenter);reenter(vp.bytes.data());return 0;
            };
            using Context=TglOwnedVpLineReleaseContext<Backend,decltype(retire)>;
            Context context(vp,buffers,retire);
            using Callback=TglOwnedVpSfcCallbacks<TglOwnedVpSfcResolve<Context>>;
            reenter=&Callback::freeResources;
            const auto releases=backend.releases;
            TglOwnedVpSfcScopedContext<Context> scope(vp.bytes.data(),context);
            assert(scope.bound());Callback::freeResources(vp.bytes.data());
            assert(context.status()==5 && gates==1 && backend.releases==releases);
            assert(backend.live==2);
        }
        {
            unsigned gates=0;
            auto retire=[&]() noexcept -> int {++gates;return 0;};
            using Context=TglOwnedVpLineReleaseContext<Backend,decltype(retire)>;
            Context context(vp,buffers,retire);
            using Callback=TglOwnedVpSfcCallbacks<TglOwnedVpSfcResolve<Context>>;
            const auto releases=backend.releases;
            assert(buffers.withBuffers(1080,720,[&]() noexcept {
                TglOwnedVpSfcScopedContext<Context> scope(vp.bytes.data(),context);
                assert(scope.bound());Callback::freeResources(vp.bytes.data());
                assert(context.status()==5 && gates==0 && backend.releases==releases);
                return 0;
            })==0);
        }
        {
            unsigned gates=0;
            auto retire=[&]() noexcept -> int {++gates;return 0;};
            using Context=TglOwnedVpLineReleaseContext<Backend,decltype(retire)>;
            Context context(vp,buffers,retire);
            using Callback=TglOwnedVpSfcCallbacks<TglOwnedVpSfcResolve<Context>>;
            struct Reentry {void* child;void(*call)(void*) noexcept;};
            Reentry reentry{vp.bytes.data(),&Callback::freeResources};
            const auto original=backend.reenter;
            backend.owner=&reentry;
            backend.reenter=[](void* pointer) {
                auto& entry=*static_cast<Reentry*>(pointer);entry.call(entry.child);
            };
            const auto releases=backend.releases;
            TglOwnedVpSfcScopedContext<Context> scope(vp.bytes.data(),context);
            assert(scope.bound());Callback::freeResources(vp.bytes.data());
            assert(context.status()==5 && gates==1 && backend.releases==releases+2);
            assert(!backend.live); // release happened; rejection is not rollback
            backend.owner=&buffers;backend.reenter=original;
        }
    }
    assert(!backend.live && backend.allocations==backend.releases);
    {
        std::array<Backend,3> backends{};
        std::array<TglOwnedSurfaceStorage,2> surfaces{};
        alignas(8) Storage sfd{};
        surfaces[0].prefix[0x130]=0x3d;surfaces[1].tail[0]=17;
        const auto prefix=surfaces[0].prefix;
        const auto tail=surfaces[1].tail;
        const std::array<TglResourceStorageReference,3> bindings{{
            {surfaces[0].resource},{surfaces[1].resource},{sfd}}};
        TglOwnedSfcLineBuffers<Backend> lines(backends,bindings);
        assert(lines.ensure(1080,5001)==0);
        assert(lines.storage(0)==&surfaces[0].resource);
        assert(lines.storage(1)==&surfaces[1].resource && lines.storage(2)==&sfd);
        assert(surfaces[0].prefix==prefix && surfaces[1].tail==tail);
        assert(lines.withBuffers(1080,5001,[&]() noexcept {
            assert(lines.storage(0)==&surfaces[0].resource && !lines.tryReset());
            return 0;
        })==0);
        assert(lines.tryReset());
        for(const auto& b:backends) assert(!b.live && b.allocations==b.releases);
        for(auto byte:surfaces[0].resource) assert(!byte);
        backends[1].failAt=backends[1].allocations+1;
        assert(lines.ensure(1080,5001)==77);
        for(const auto& b:backends) assert(!b.live);
        assert(surfaces[0].prefix==prefix && surfaces[1].tail==tail);
        const auto allocations=backends[0].allocations;
        const std::array<TglResourceStorageReference,3> duplicates{{
            {surfaces[0].resource},{surfaces[0].resource},{sfd}}};
        TglOwnedSfcLineBuffers<Backend> invalid(backends,duplicates);
        assert(invalid.ensure(1080,5001)==5 && backends[0].allocations==allocations);
    }
}

static void testSfcSfdOwnership() {
    using Storage=std::array<uint8_t,0x148>;
    struct Backend {
        unsigned allocations=0,releases=0,live=0,failAt=0;
        uint32_t lastWidth=0;
        int allocate(Storage& storage,const TglResourceKey& key) noexcept {
            assert(key.height==1 && key.format==0x3d && key.tile==4 && !key.compressed);
            ++allocations;++live;lastWidth=key.width;
            const uint64_t handle=allocations;
            std::memcpy(storage.data()+0x20,&handle,8);
            return allocations==failAt ? 79 : 0;
        }
        void release(Storage& storage) noexcept {
            assert(live);--live;++releases;storage.fill(0);
        }
        bool backed(const Storage& storage) noexcept { return tglNativeBackingPresent(storage); }
    } backend;
    {
        TglOwnedSfcLineBuffers<Backend> buffers(backend);
        TglOwnedSfcGen12Tail tail{};tail.sfdLineBuffer=0x1234;tail.avsLineTile=0x5678;
        assert(!buffers.bindSfdRow(tail) && !tail.sfdLineBuffer && tail.avsLineTile==0x5678);
        assert(buffers.ensure(1080,4000)==0 && backend.live==2 && !buffers.storage(2));
        assert(buffers.bindSfdRow(tail) && !tail.sfdLineBuffer);
        assert(buffers.ensure(1080,4001)==0 && backend.live==3 && buffers.storage(2));
        assert(buffers.bindSfdRow(tail) &&
            tail.sfdLineBuffer==reinterpret_cast<uintptr_t>(buffers.storage(2)->data()));
        assert(backend.lastWidth==25606); // pinned macro integer division
        const auto allocations=backend.allocations;
        assert(buffers.ensure(1080,4001)==0 && backend.allocations==allocations);
        assert(buffers.ensure(1080,4000)==0 && backend.live==2 && !buffers.storage(2));
        assert(buffers.bindSfdRow(tail) && !tail.sfdLineBuffer && tail.avsLineTile==0x5678);
        buffers.reset();assert(!backend.live);
        tail.sfdLineBuffer=0x1234;
        assert(!buffers.bindSfdRow(tail) && !tail.sfdLineBuffer);
        backend.failAt=backend.allocations+3;
        assert(buffers.ensure(1080,4001)==79 && !backend.live);
        for(size_t i=0;i<3;++i) assert(!buffers.storage(i));
        assert(!buffers.bindSfdRow(tail) && !tail.sfdLineBuffer);
        backend.failAt=0;
        assert(buffers.ensure(1080,4001)==0 && backend.live==3);
        struct CpuBackend {
            TglOwnedSfcStateParametersPacket bytes{};bool live=false;
            uintptr_t allocate(size_t size) {
                assert(!live && size==bytes.size());live=true;
                return reinterpret_cast<uintptr_t>(bytes.data());
            }
            void release(uintptr_t address) noexcept {
                assert(live && address==reinterpret_cast<uintptr_t>(bytes.data()));live=false;
            }
        } cpu;
        TglOwnedSfcStateParameters<CpuBackend> state(cpu);
        TglOwnedSfcCommandParameters commands{};commands.state.fill(0xa5);
        std::array<uint8_t,0xb8> prefix{};prefix[0]=1;
        const uint32_t dimension=1920,inputHeight=1080,scaledHeight=4001;
        for(size_t offset:{size_t(0x24),size_t(0x28),size_t(0x30),size_t(0x34),
                           size_t(0x3c),size_t(0x40),size_t(0x4c),size_t(0x50)})
            std::memcpy(prefix.data()+offset,&dimension,4);
        std::memcpy(prefix.data()+0x30,&inputHeight,4);
        std::memcpy(prefix.data()+0x4c,&scaledHeight,4);
        // Deliberately non-square: widths34/50 remain1920 while the pinned
        // allocations require heights1080/4001. Reading widths must fail.
        TglOwnedSurfaceStorage output;const uint64_t handle=1;
        std::memcpy(output.resource.data()+0x20,&handle,8);
        unsigned queries=0;int queryResult=0;
        auto query=[&](TglOwnedSurfaceStorage& surface) noexcept {
            assert(&surface==&output);++queries;return queryResult;
        };
        auto prepare=[&]() {
            return tglPrepareOwnedSfcCommandState(commands,state,buffers,output,
                prefix,{},true,false,false,TglOwnedSfcCompressionMode::Disabled,query);
        };
        const auto originalCommands=commands.state;
        assert(prepare()==5 && !queries && commands.state==originalCommands);
        assert(state.initialize());const auto originalState=cpu.bytes;
        commands.filtersInUse=true;
        assert(prepare()==5 && !queries && commands.state==originalCommands && cpu.bytes==originalState);
        assert(commands.filtersInUse);
        commands.filtersInUse=false;
        const auto validPrefix=prefix;
        const uint32_t mismatch=1081;std::memcpy(prefix.data()+0x30,&mismatch,4);
        assert(prepare()==5 && !queries && cpu.bytes==originalState);
        prefix=validPrefix;queryResult=89;
        assert(prepare()==89 && queries==1 && commands.state==originalCommands && cpu.bytes==originalState);
        queryResult=0;
        assert(prepare()==0 && queries==2 && commands.state==cpu.bytes);
        uintptr_t sfd=0;std::memcpy(&sfd,commands.state.data()+0xe0,8);
        assert(sfd==reinterpret_cast<uintptr_t>(buffers.storage(2)->data()));
        assert(commands.state[0x66]==1 && commands.state[0x6a]==0);
        unsigned generated=0;
        auto pinnedQuery=[&](TglOwnedSurfaceStorage& surface) noexcept {
            assert(&surface==&output);
            const auto live=backend.live;
            buffers.reset();assert(backend.live==live && live==3);
            assert(buffers.ensure(1080,4000)==5);
            return queryResult;
        };
        for(int result:{0,31,-7}) {
            auto generate=[&](TglOwnedSfcCommandParameters& actual) noexcept {
                assert(&actual==&commands);++generated;
                buffers.reset();assert(backend.live==3);
                const auto stateBefore=cpu.bytes;
                state.reset();assert(!state.initialize() && !state.data());
                assert(cpu.bytes==stateBefore);
                assert(state.withParameters([](const uint8_t*) noexcept {return 0;})==5);
                assert(buffers.withBuffers(1080,4001,[]() noexcept {assert(false);return 0;})==5);
                return result;
            };
            assert(tglWithOwnedSfcPreparedCommands(commands,state,buffers,output,prefix,{},
                true,false,false,TglOwnedSfcCompressionMode::Disabled,pinnedQuery,generate)==result);
        }
        assert(generated==3);
        queryResult=89;
        assert(tglWithOwnedSfcPreparedCommands(commands,state,buffers,output,prefix,{},
            true,false,false,TglOwnedSfcCompressionMode::Disabled,pinnedQuery,
            [&](TglOwnedSfcCommandParameters&) noexcept {++generated;return 0;})==89);
        assert(generated==3);queryResult=0;
        struct AvsBackend {
            alignas(8) std::array<uint8_t,0xc00> bytes{};bool live=false;
            uintptr_t allocate(size_t size) {
                assert(!live && size==bytes.size());live=true;
                return reinterpret_cast<uintptr_t>(bytes.data());
            }
            void release(uintptr_t address) noexcept {
                assert(live && address==reinterpret_cast<uintptr_t>(bytes.data()));live=false;
            }
        } avsBackend;
        TglOwnedSfcAvsParameters<AvsBackend> avs(avsBackend);
        assert(avs.initialize());
        TglOwnedSfcAvsTables cache;
        TglOwnedSfcAvsConfiguration config;
        config.format=0x19;config.scaleX=.5f;config.scaleY=.75f;
        config.siting=0x11;config.symmetric=true;
        auto sine=[](float x) noexcept {return std::sin(x);};
        TglOwnedVpSfcCallerStorage unleased;
        assert(tglWithOwnedVpSfcCpuView(unleased,state,avs,
            [](void*) noexcept {assert(false);return 0;})==5);
        for(int result:{0,31,-7}) {
            unsigned calls=0;
            assert(tglWithOwnedSfcCommandInputs(commands,state,avs,cache,buffers,
                output,prefix,{},true,false,false,TglOwnedSfcCompressionMode::Disabled,
                true,config,sine,pinnedQuery,[&](auto& ready) noexcept {
                    ++calls;avs.reset();state.reset();buffers.reset();
                    assert(avsBackend.live && cpu.live && backend.live==3);
                    assert(!avs.initialize() && !state.initialize());
                    assert(ready.state==cpu.bytes && ready.y.coefficients==cache.packedY &&
                        ready.uv.coefficients==cache.packedUv);
                    TglOwnedVpSfcCallerStorage view;
                    assert(tglWithOwnedVpSfcCpuView(view,state,avs,
                        [](void*) noexcept {assert(false);return 0;})==5);
                    struct MockBinding {
                        uintptr_t image=1;
                        bool validateConstructedObject(uintptr_t mhw,uintptr_t os,int) const noexcept {
                            return mhw==3 && os==1;
                        }
                    };
                    assert(view.initializeBorrowed(1,2,3,MockBinding{},0));
                    const auto original=view.bytes;
                    assert(tglWithOwnedVpSfcCpuView(view,state,avs,[&](void* base) noexcept {
                        assert(base==view.bytes.data());
                        uintptr_t packet=0;std::memcpy(&packet,view.bytes.data()+0x938,8);
                        assert(packet==reinterpret_cast<uintptr_t>(cpu.bytes.data()));
                        assert(!std::memcmp(view.bytes.data()+0x28,avs.parameters()->data(),0x30));
                        assert(tglWithOwnedVpSfcCpuView(view,state,avs,
                            [](void*) noexcept {assert(false);return 0;})==5);
                        return result;
                    })==result);
                    assert(view.bytes==original);
                    const uintptr_t foreign=1;
                    std::memcpy(view.bytes.data()+0x38,&foreign,8);
                    const auto rejected=view.bytes;
                    assert(tglWithOwnedVpSfcCpuView(view,state,avs,
                        [](void*) noexcept {assert(false);return 0;})==5);
                    assert(view.bytes==rejected);
                    return result;
                })==result && calls==1);
        }
        TglOwnedVpCscCache cscCache;
        unsigned cscProduced=0,cscGenerated=0;
        const auto originalIef=commands.ief;
        auto cscRead=[](uintptr_t,void*,size_t) noexcept {assert(false);return false;};
        auto cscProduce=[&](TglCscCoefficients& coefficients,uint32_t from,uint32_t to) noexcept {
            ++cscProduced;assert(from==3 && to==5);coefficients.matrix[0]=2;return true;
        };
        auto withCsc=[&](auto& ready) noexcept {
            return tglWithOwnedSfcCscCommands(ready,cscCache,true,3,5,0,cscRead,cscProduce,
                [&](auto& bound) noexcept {
                    ++cscGenerated;
                    assert(bound.state[0x88]==1 && bound.ief[0x20]==1);
                    uintptr_t matrix=0;std::memcpy(&matrix,bound.ief.data()+0x28,8);
                    assert(matrix && *reinterpret_cast<const float*>(matrix)==2);
                    assert(state.hasParametersLease() && !cscCache.reset());
                    avs.reset();state.reset();buffers.reset();
                    assert(avsBackend.live && cpu.live && backend.live==3);
                    assert(!avs.initialize() && !state.initialize());
                    return 31;
                });
        };
        assert(tglWithOwnedSfcCommandInputs(commands,state,avs,cache,buffers,
            output,prefix,{},true,false,false,TglOwnedSfcCompressionMode::Disabled,
            true,config,sine,pinnedQuery,withCsc)==31);
        assert(cscProduced==1 && cscGenerated==1 && commands.ief==originalIef);
        assert(commands.state==cpu.bytes && cscCache.reset());
        queryResult=89;
        assert(tglWithOwnedSfcCommandInputs(commands,state,avs,cache,buffers,
            output,prefix,{},true,false,false,TglOwnedSfcCompressionMode::Disabled,
            true,config,sine,pinnedQuery,withCsc)==89);
        assert(cscProduced==1 && cscGenerated==1 && commands.ief==originalIef);
        assert(tglWithOwnedSfcCommandInputs(commands,state,avs,cache,buffers,
            output,prefix,{},true,false,false,TglOwnedSfcCompressionMode::Disabled,
            true,config,sine,pinnedQuery,[](auto&) noexcept {assert(false);return 0;})==89);
        queryResult=0;
        avs.reset();assert(!avsBackend.live && !avs.parameters());
        const auto savedY=commands.y.coefficients;
        const auto savedUv=commands.uv.coefficients;
        unsigned bypassGenerated=0;
        assert(tglWithOwnedSfcCommandInputs(commands,state,avs,cache,buffers,
            output,prefix,{},true,false,false,TglOwnedSfcCompressionMode::Disabled,
            false,TglOwnedSfcAvsConfiguration{},
            [](float) noexcept -> float {assert(false);return 0;},pinnedQuery,
            [&](auto& ready) noexcept -> int {
                ++bypassGenerated;assert(!avsBackend.live && state.hasParametersLease());
                assert(ready.y.coefficients==savedY && ready.uv.coefficients==savedUv);
                TglOwnedVpSfcCallerStorage view;
                const uintptr_t borrowed=1;
                for(size_t offset:{size_t(0x10),size_t(0x18),size_t(0x20)})
                    std::memcpy(view.bytes.data()+offset,&borrowed,8);
                std::memset(view.bytes.data()+0x28,0xa5,0x30);
                const auto previous=view.bytes;
                assert(tglWithOwnedVpSfcCpuView(view,state,avs,
                    [](void*) noexcept -> int {assert(false);return 0;})==5);
                assert(tglWithOwnedVpSfcCpuView(view,state,avs,[&](void* child) noexcept -> int {
                    assert(child==view.bytes.data());
                    assert(!std::memcmp(view.bytes.data()+0x28,previous.data()+0x28,0x30));
                    uintptr_t packet=0;std::memcpy(&packet,view.bytes.data()+0x938,8);
                    const auto& leasedState=state;
                    assert(packet==reinterpret_cast<uintptr_t>(leasedState.data()));
                    assert(tglWithOwnedVpSfcCpuView(view,state,avs,
                        [](void*) noexcept -> int {assert(false);return 0;},false)==5);
                    return -17;
                },false)==-17);
                assert(view.bytes==previous && !avs.parameters());
                for(size_t offset:{size_t(0x921),size_t(0x9b8)}) {
                    view.bytes[offset]=1;
                    assert(tglWithOwnedVpSfcCpuView(view,state,avs,
                        [](void*) noexcept -> int {assert(false);return 0;},false)==5);
                    view.bytes[offset]=0;
                }
                assert(view.bytes==previous);
                return 31;
            })==31);
        assert(bypassGenerated==1 && !avs.parameters());
        const auto prepared=commands.state;
        buffers.reset();
        assert(prepare()==5 && queries==2 && commands.state==prepared && cpu.bytes==prepared);
    }
    assert(!backend.live && backend.allocations==backend.releases);
}

static void testSfcAvsOwnership() {
    struct Backend {
        unsigned allocations=0,releases=0;
        uintptr_t result=0x10000;
        void* context=nullptr;
        void (*reenter)(void*) noexcept=nullptr;
        uintptr_t allocate(size_t bytes) {
            assert(bytes==0xc00); ++allocations;
            if(reenter) reenter(context);
            return result;
        }
        void release(uintptr_t p) noexcept {
            assert(p==result); ++releases;
            if(reenter) reenter(context);
        }
    } backend;
    {
        TglOwnedSfcAvsParameters<Backend> avs(backend);
        backend.context=&avs;
        backend.reenter=[](void* context) noexcept {
            auto& owner=*static_cast<TglOwnedSfcAvsParameters<Backend>*>(context);
            assert(!owner.parameters());
            assert(!owner.initialize());
            owner.reset();
            assert(owner.withParameters([](const auto&) noexcept {return 0;})==5);
        };
        assert(!avs.parameters());
        backend.result=0;
        assert(!avs.initialize() && !avs.parameters() && !backend.releases);
        backend.result=UINTPTR_MAX-0xbff;
        assert(!avs.initialize() && !avs.parameters() && backend.releases==1);
        backend.result=0x10000;
        assert(avs.initialize());
        const auto& header=*avs.parameters();
        uint32_t format=0; std::memcpy(&format,header.data(),4);
        assert(format==0xffffffff);
        for (size_t i=4;i<0x10;++i) assert(header[i]==0);
        const std::array<uintptr_t,4> expected{{0x10000,0x10600,0x10400,0x10a00}};
        for (size_t i=0;i<4;++i) {
            uintptr_t p=0; std::memcpy(&p,header.data()+0x10+i*8,8);
            assert(p==expected[i]);
        }
        assert(avs.initialize() && backend.allocations==3);
        for(int status:{0,31,-7}) {
            unsigned calls=0;
            assert(avs.withParameters([&](const auto& borrowed) noexcept {
                ++calls;
                assert(&borrowed==avs.parameters());
                avs.reset();
                assert(!avs.initialize());
                assert(avs.withParameters([](const auto&) noexcept {return 0;})==5);
                assert(avs.parameters()==&borrowed && backend.releases==1);
                return status;
            })==status && calls==1);
        }
        avs.reset(); avs.reset();
        assert(!avs.parameters() && backend.releases==2);
        assert(avs.withParameters([](const auto&) noexcept {return 0;})==5);
        assert(avs.initialize());
    }
    assert(backend.allocations==4 && backend.releases==3);
    struct RealBackend {
        alignas(8) std::array<uint8_t,0xc00> bytes{};
        unsigned releases=0;
        uintptr_t allocate(size_t size) {
            assert(size==bytes.size()); return reinterpret_cast<uintptr_t>(bytes.data());
        }
        void release(uintptr_t pointer) noexcept {
            assert(pointer==reinterpret_cast<uintptr_t>(bytes.data())); ++releases;
        }
    } real;
    {
        TglOwnedSfcAvsParameters<RealBackend> owner(real);
        TglOwnedSfcAvsTables tables;
        tables.format=7; tables.scaleX=.5f; tables.scaleY=.75f;
        for(size_t i=0;i<256;++i) {tables.yX[i]=int(i);tables.yY[i]=-int(i);}
        for(size_t i=0;i<128;++i) {tables.uvX[i]=int(i+500);tables.uvY[i]=-int(i+500);}
        assert(!owner.publishTables(tables) && owner.initialize());
        assert(!owner.publishTables(tables));
        assert(owner.withParameters([&](const auto& header) noexcept {
            auto expectedHeader=header;
            std::memcpy(expectedHeader.data(),&tables.format,4);
            std::memcpy(expectedHeader.data()+4,&tables.scaleX,4);
            std::memcpy(expectedHeader.data()+8,&tables.scaleY,4);
            assert(owner.publishTables(tables));
            assert(header==expectedHeader); // Reserved bytes and four pointers survive.
            assert(!std::memcmp(real.bytes.data(),tables.yX.data(),0x400));
            assert(!std::memcmp(real.bytes.data()+0x400,tables.uvX.data(),0x200));
            assert(!std::memcmp(real.bytes.data()+0x600,tables.yY.data(),0x400));
            assert(!std::memcmp(real.bytes.data()+0xa00,tables.uvY.data(),0x200));
            const auto before=real.bytes; const auto oldHeader=header;
            auto invalid=tables;invalid.scaleX=0;
            assert(!owner.publishTables(invalid));
            assert(real.bytes==before && header==oldHeader);
            return 0;
        })==0);
        auto sine=[&](float value) noexcept {
            owner.reset(); assert(!owner.initialize());
            return std::sin(value);
        };
        for(int status:{0,31,-7}) {
            unsigned generated=0;
            assert(tglWithOwnedSfcAvsTables(owner,tables,0x19,.5f,.75f,0x11,
                false,true,0.f,0.f,sine,[&](const auto& ready) noexcept {
                    ++generated;
                    owner.reset(); assert(!owner.initialize());
                    assert(!std::memcmp(real.bytes.data(),ready.yX.data(),0x400));
                    assert(!std::memcmp(real.bytes.data()+0xa00,ready.uvY.data(),0x200));
                    return status;
                })==status && generated==1);
        }
        const auto savedBytes=real.bytes;
        TglOwnedSfcCommandParameters commands;
        commands.y.pipeMode=1;commands.uv.pipeMode=1;
        commands.y.reserved.fill(0xa5);commands.uv.reserved.fill(0x5a);
        assert(tglWithOwnedSfcAvsCommands(commands,owner,tables,0x19,.5f,.75f,
            0x11,false,true,0.f,0.f,sine,[&](const auto& packet) noexcept {
                owner.reset();assert(!owner.initialize());
                assert(packet.y.coefficients==tables.packedY && packet.uv.coefficients==tables.packedUv);
                assert(packet.y.pipeMode==1 && packet.uv.pipeMode==1);
                for(auto byte:packet.y.reserved) assert(byte==0xa5);
                for(auto byte:packet.uv.reserved) assert(byte==0x5a);
                return 31;
            })==31);
        const auto savedHeader=*owner.parameters();
        const auto savedCache=tables;
        auto never=[](const auto&) noexcept {assert(false);return 0;};
        const auto savedY=commands.y.coefficients;
        const auto savedUv=commands.uv.coefficients;
        commands.filtersInUse=true;
        auto neverSine=[](float) noexcept {assert(false);return 0.f;};
        assert(tglWithOwnedSfcAvsCommands(commands,owner,tables,0x19,.25f,.25f,
            0x11,false,true,0.f,0.f,neverSine,never)==5);
        assert(commands.filtersInUse && commands.y.coefficients==savedY &&
            commands.uv.coefficients==savedUv);
        assert(real.bytes==savedBytes && *owner.parameters()==savedHeader);
        assert(tables.yX==savedCache.yX && tables.uvY==savedCache.uvY &&
            tables.format==savedCache.format && tables.scaleX==savedCache.scaleX);
        commands.filtersInUse=false;
        TglOwnedSfcIefInput disabledIef;
        TglOwnedIefTables iefTables{};
        TglOwnedVpCscCache cscCache;
        auto noRead=[](uintptr_t,void*,size_t) noexcept {assert(false);return false;};
        auto noCsc=[](TglCscCoefficients&,uint32_t,uint32_t) noexcept {assert(false);return false;};
        assert(tglWithOwnedSfcFilterCommands(commands,disabledIef,iefTables,cscCache,
            false,0,0,0,noRead,noCsc,[&](auto& packet) noexcept {
                assert(packet.filtersInUse);
                assert(tglWithOwnedSfcAvsCommands(packet,owner,tables,0x19,.25f,.25f,
                    0x11,false,true,0.f,0.f,neverSine,never)==5);
                assert(packet.y.coefficients==savedY && packet.uv.coefficients==savedUv);
                return -37;
            })==-37);
        assert(!commands.filtersInUse && real.bytes==savedBytes &&
            *owner.parameters()==savedHeader && tables.scaleX==savedCache.scaleX);
        assert(tglWithOwnedSfcAvsTables(owner,tables,0x19,0.f,.75f,0x11,
            false,true,0.f,0.f,sine,never)==5);
        assert(real.bytes==savedBytes && *owner.parameters()==savedHeader);
        assert(tables.yX==savedCache.yX && tables.uvY==savedCache.uvY &&
            tables.format==savedCache.format && tables.scaleX==savedCache.scaleX);
    }
    assert(real.releases==1);
}

static void testVpSfcCpuGraph() {
    struct Backend {
        alignas(8) std::array<uint8_t,0xc00> bytes{};
        size_t expected;bool live=false,fail=false;unsigned frees=0;
        uintptr_t allocate(size_t n) {
            assert(n==expected && !live);
            if(fail) return 0;
            live=true;return reinterpret_cast<uintptr_t>(bytes.data());
        }
        void release(uintptr_t p) noexcept {
            assert(live && p==reinterpret_cast<uintptr_t>(bytes.data()));
            live=false;++frees;
        }
    } state{ {},0xf8 },avs{ {},0xc00 };
    struct Binding {
        uintptr_t image=0x100000;
        bool valid=true;
        bool validateConstructedObject(uintptr_t mhw,uintptr_t os,
            bool (*)(uintptr_t,void*,size_t)) const noexcept {
            return valid && mhw==0x3000 && os==0x1000;
        }
    } binding;
    auto read=+[](uintptr_t,void*,size_t) noexcept {return false;};
    TglOwnedVpSfcCpuGraph<Backend,Backend> graph(state,avs);
    auto never=[](void*) noexcept {assert(false);return 0;};
    assert(graph.withView(true,never)==5);
    auto initialize=[&] {return graph.initialize(0x1000,0x2000,0x3000,binding,read,7,0x4000);};
    avs.fail=true;assert(!initialize() && !state.live && !avs.live);
    avs.fail=false;state.fail=true;
    assert(!initialize() && !state.live && !avs.live && avs.frees==1);
    state.fail=false;assert(initialize() && state.live && avs.live);
    assert(graph.prepare([&](auto& storage,auto&,auto&) noexcept {
        uintptr_t p=0;uint32_t channel=0;
        std::memcpy(&channel,storage.bytes.data()+0x918,4);
        std::memcpy(&p,storage.bytes.data()+0x9c8,8);
        assert(channel==7 && p==0x4000);
        graph.reset();assert(graph.withView(true,never)==5);
        return 31;
    })==31);
    for(bool enabled:{false,true}) {
        assert(graph.withView(enabled,[&](void* child) noexcept {
            const auto* bytes=static_cast<const uint8_t*>(child);
            uintptr_t packet=0,coefficients=0;
            std::memcpy(&packet,bytes+0x938,8);
            std::memcpy(&coefficients,bytes+0x38,8);
            assert(packet==reinterpret_cast<uintptr_t>(state.bytes.data()));
            assert(coefficients==(enabled?reinterpret_cast<uintptr_t>(avs.bytes.data()):0));
            graph.reset();assert(state.live && avs.live);
            assert(!initialize() && graph.withView(enabled,never)==5);
            return -37;
        })==-37);
    }
    using SendEntry=int(*)(void*,void*,void*,void*,void*,void*) noexcept;
    SendEntry retained=nullptr;void* retainedChild=nullptr;
    for(bool enabled:{false,true}) {
        assert(graph.withSendView(enabled,[&](void* child,SendEntry send) noexcept {
            retained=send;retainedChild=child;
            assert(send(child,nullptr,nullptr,nullptr,nullptr,nullptr)==0);
            assert(send(nullptr,nullptr,nullptr,nullptr,nullptr,nullptr)==5);
            graph.reset();assert(state.live && avs.live);
            assert(graph.withSendView(enabled,[](void*,SendEntry) noexcept {
                assert(false);return 0;
            })==5);
            return -41;
        })==-41);
        assert(retained(retainedChild,nullptr,nullptr,nullptr,nullptr,nullptr)==5);
    }
    assert(graph.prepare([&](auto& storage,auto&,auto&) noexcept {
        uintptr_t p=1;std::memcpy(&p,storage.bytes.data()+0x938,8);assert(!p);
        for(size_t i=0x28;i<0x58;++i) assert(!storage.bytes[i]);
        storage.bytes[0x921]=1;
        return 0;
    })==0);
    assert(graph.withView(false,never)==5);
    assert(graph.withView(true,[](void*) noexcept {return 89;})==89);
    binding.valid=false;assert(!initialize() && !state.live && !avs.live);
    assert(graph.withView(true,never)==5);
    graph.reset();assert(state.frees==1 && avs.frees==2);
}

static void testSfcSetupOrder() {
    for (unsigned flags=0;flags<16;++flags) {
        for (unsigned failure=0;failure<5;++failure) {
            std::array<unsigned,4> order{};
            size_t count=0;
            auto stage=[&](unsigned id) noexcept {
                order.at(count++)=id; return failure==id ? 70+int(id) : 0;
            };
            const int result=tglSetupOwnedSfc(
                uint8_t((flags&1)?0xff:0xfe),uint8_t((flags&2)?1:0),
                uint8_t((flags&4)?1:0),uint8_t((flags&8)?1:0),
                [&]() noexcept { return stage(1); },
                [&]() noexcept { return stage(2); },
                [&]() noexcept { return stage(3); },
                [&]() noexcept { return stage(4); });
            assert(order[0]==1);
            if (failure==1) { assert(result==71 && count==1); continue; }
            assert(order[1]==2);
            if (failure==2) { assert(result==72 && count==2); continue; }
            size_t expected=2;
            if (flags&3) {
                assert(order[expected++]==3);
                if (failure==3) { assert(result==73 && count==expected); continue; }
            }
            if (flags&12) assert(order[expected++]==4);
            assert(result==0 && count==expected);
        }
    }
}

static void testSfcStateParameterOwnership() {
    struct Backend {
        TglOwnedSfcStateParametersPacket bytes{};
        unsigned live=0,allocations=0,releases=0;
        bool fail=false;
        uintptr_t allocate(size_t size) {
            assert(size==bytes.size() && live==0);
            ++allocations;
            if (fail) return 0;
            bytes.fill(0xa5); live=1;
            return reinterpret_cast<uintptr_t>(bytes.data());
        }
        void release(uintptr_t p) noexcept {
            assert(live==1 && p==reinterpret_cast<uintptr_t>(bytes.data()));
            live=0; ++releases;
        }
    } backend;
    {
        TglOwnedSfcStateParameters<Backend> state(backend);
        assert(!state.data());
        std::array<uint8_t,0xb8> prefix{};
        const uint32_t dimension=1080;
        for(size_t offset:{size_t(0x24),size_t(0x28),size_t(0x30),size_t(0x34),
                           size_t(0x3c),size_t(0x40),size_t(0x4c),size_t(0x50)})
            std::memcpy(prefix.data()+offset,&dimension,4);
        assert(!state.prepare(prefix,{},false,false,false,TglOwnedSfcCompressionMode::Disabled));
        assert(state.initialize());
        for (auto byte:backend.bytes) assert(byte==0);
        TglOwnedSfcGen12Tail tail{};tail.sfdLineTile=0x1230;
        assert(state.prepare(prefix,tail,true,true,true,TglOwnedSfcCompressionMode::Render));
        TglOwnedSfcStateParametersPacket expected{};
        assert(tglPrepareOwnedSfcGen12Pass(expected,prefix,tail,true,true,true,
            TglOwnedSfcCompressionMode::Render) && backend.bytes==expected);
        for(int status:{0,31,-7}) {
            unsigned calls=0;
            assert(state.withParameters([&](const uint8_t* packet) noexcept {
                ++calls; assert(packet==backend.bytes.data() && !state.data());
                const auto& readOnly=state;
                assert(readOnly.data()==packet);
                state.reset(); assert(!state.initialize());
                assert(!state.prepare(prefix,tail,true,true,true,TglOwnedSfcCompressionMode::Render));
                assert(state.withParameters([](const uint8_t*) noexcept {return 0;})==5);
                assert(backend.bytes==expected && backend.releases==0);
                return status;
            })==status && calls==1);
        }
        tail.engineMode=4;
        assert(!state.prepare(prefix,tail,false,false,false,TglOwnedSfcCompressionMode::Disabled));
        assert(backend.bytes==expected); // failed preparation does not corrupt live state
        state.data()[0xb7]=0x77;
        state.data()[0xf7]=0x88;
        assert(state.initialize() && backend.releases==1 && state.data()[0xb7]==0 &&
               state.data()[0xf7]==0);
        backend.fail=true;
        assert(!state.initialize() && !state.data() && !backend.live && backend.releases==2);
        backend.fail=false;
        assert(state.initialize());
        const auto& readOnly=state;
        assert(readOnly.data()==backend.bytes.data());
    }
    assert(!backend.live && backend.allocations==4 && backend.releases==3);
    {
        TglOwnedVpSfcCallerStorage storage;
        struct VpBackend:Backend {
            TglOwnedVpSfcCallerStorage* storage=nullptr;
            uintptr_t allocate(size_t size) {
                assert(storage && !live);
                for(size_t i=0x920;i<0x9c8;++i) assert(!storage->bytes[i]);
                return Backend::allocate(size);
            }
        } vpBackend;
        vpBackend.storage=&storage;
        TglOwnedSfcStateParameters<VpBackend> state(vpBackend);
        storage.bytes.fill(0xa5);
        const auto foreign=storage.bytes;
        assert(!state.initializeVpPass(storage) && storage.bytes==foreign && !vpBackend.allocations);
        std::memset(storage.bytes.data()+0x938,0,8);
        const auto original=storage.bytes;
        assert(state.initializeVpPass(storage));
        for(size_t i=0;i<storage.bytes.size();++i)
            assert(storage.bytes[i]==(i>=0x920 && i<0x9c8 ? 0 : original[i]));
        storage.bytes[0x920]=1;storage.bytes[0x9c7]=2;
        const auto leased=storage.bytes;
        assert(state.withParameters([&](const uint8_t*) noexcept {
            assert(!state.initializeVpPass(storage) && storage.bytes==leased && !vpBackend.releases);
            return 0;
        })==0);
        vpBackend.fail=true;
        assert(!state.initializeVpPass(storage) && !state.data() && !vpBackend.live && vpBackend.releases==1);
        for(size_t i=0;i<storage.bytes.size();++i)
            assert(storage.bytes[i]==(i>=0x920 && i<0x9c8 ? 0 : original[i]));
        vpBackend.fail=false;
        assert(state.initializeVpPass(storage) && vpBackend.allocations==3);
        using Context=TglOwnedVpPassInitContext<VpBackend>;
        using Callback=TglOwnedVpPassInitCallback<TglOwnedVpSfcResolve<Context>>;
        using Initialize=void(*)(void*) noexcept;
        const Initialize initialize=&Callback::initialize;
        for(bool fail:{false,true}) {
            state.reset();vpBackend.fail=fail;
            Context context(storage,state);
            const auto before=storage.bytes;
            const auto allocations=vpBackend.allocations;
            initialize(storage.bytes.data());
            assert(context.status()==5 && storage.bytes==before);
            {
                TglOwnedVpSfcScopedContext<Context> scope(storage.bytes.data(),context);
                assert(scope.bound());
                initialize(nullptr);initialize(&vpBackend);
                assert(vpBackend.allocations==allocations && storage.bytes==before);
                initialize(storage.bytes.data());
                assert(context.status()==(fail ? 5 : 0));
                assert(bool(state.data())==!fail);
                uintptr_t published=1;std::memcpy(&published,storage.bytes.data()+0x938,8);
                assert(!published);
                const auto once=vpBackend.allocations;
                initialize(storage.bytes.data());
                assert(context.status()==5 && vpBackend.allocations==once);
            }
            const auto after=storage.bytes;
            const auto once=vpBackend.allocations;
            initialize(storage.bytes.data());
            assert(storage.bytes==after && vpBackend.allocations==once);
        }
        vpBackend.fail=false;assert(state.initializeVpPass(storage));
        Context busy(storage,state);
        assert(state.withParameters([&](const uint8_t*) noexcept {
            const auto before=storage.bytes;
            const auto allocations=vpBackend.allocations,releases=vpBackend.releases;
            TglOwnedVpSfcScopedContext<Context> scope(storage.bytes.data(),busy);
            assert(scope.bound());initialize(storage.bytes.data());
            assert(busy.status()==5 && storage.bytes==before);
            assert(vpBackend.allocations==allocations && vpBackend.releases==releases);
            return 0;
        })==0);
    }
}

static void testSfcInputChroma() {
    // Cover the complete pinned classifier domain plus unknown formats.
    for (uint32_t format=0;format<=91;++format) {
        for (unsigned flags=0;flags<4;++flags) {
            std::array<uint8_t,0xb8> packet; packet.fill(0xa5);
            uint32_t siting=0x42;
            tglPrepareOwnedSfcInputChroma(packet,siting,format,flags&1,flags&2);
            uint32_t actual=0; std::memcpy(&actual,packet.data()+8,4);
            const auto pack=tglOwnedChromaColorPack(format);
            const bool special=format==0x19 || format==0x52 || format==0x53;
            const uint32_t expected=(flags&1)?4:(flags&2)?2:
                special?1:pack==1?2:pack==2?4:0;
            assert(actual==expected && packet[0]==1);
            assert(packet[0x6a]==uint8_t(expected==4));
            assert(siting==((flags&1)?0x11:0x42));
            for (size_t i=0;i<packet.size();++i)
                if (i!=0 && !(i>=8 && i<12) && i!=0x6a) assert(packet[i]==0xa5);
        }
    }
    std::array<uint8_t,0xb8> packet{};
    uint32_t siting=0;
    tglPrepareOwnedSfcInputChroma(packet,siting,UINT32_MAX,false,false);
    assert(packet[8]==0 && packet[0x6a]==0 && siting==0);
}

static void testSfcOutputChroma() {
    for (uint32_t input:{0u,1u,2u,4u,UINT32_MAX}) {
        // Independent pinned examples: NV12/422/444/OTHER.
        const std::array<uint32_t,4> formats{{0x19,0xd,1,UINT32_MAX}};
        for (unsigned pack=0;pack<4;++pack) {
            for (uint32_t siting=0;siting<128;++siting) {
                std::array<uint8_t,0xb8> packet; packet.fill(0xa5);
                std::memcpy(packet.data()+8,&input,4);
                const auto before=packet;
                tglPrepareOwnedSfcOutputChroma(packet,siting,formats[pack]);
                uint32_t mode=0,v=0,h=0;
                std::memcpy(&mode,packet.data()+0x18,4);
                std::memcpy(&v,packet.data()+0x1c,4);
                std::memcpy(&h,packet.data()+0x20,4);
                assert(v==((siting&0x20)?4u:(siting&0x40)?8u:0u));
                assert(h==((siting&2)?4u:(siting&4)?8u:0u));
                const uint32_t expected=input==4 ? (pack==0?1:pack==1?2:0xa5a5a5a5u):
                    input==2 ? (pack==0?3:0xa5a5a5a5u):0;
                assert(mode==expected);
                for (size_t i=0;i<packet.size();++i)
                    if (i<0x18 || i>=0x24) assert(packet[i]==before[i]);
            }
        }
    }
}

static void testSfcAlignedGeometry() {
    const std::array<size_t,4> offsets{{0x24,0x28,0x44,0x48}};
    for (uint16_t unit:{1,2,4,8,16,64,32768}) {
        for (uint32_t value:{0u,1u,1079u,1921u,0xffff0000u}) {
            std::array<uint8_t,0xb8> packet; packet.fill(0xa5);
            assert(tglPrepareOwnedSfcAlignedGeometry(packet,value,value,value,value,
                                                     unit,unit,unit,unit));
            const uint32_t expected=uint32_t(((uint64_t(value)+unit-1)/unit)*unit);
            for (auto offset:offsets) {
                uint32_t actual=0; std::memcpy(&actual,packet.data()+offset,4);
                assert(actual==expected);
            }
            for (size_t i=0;i<packet.size();++i) {
                bool field=false;
                for (auto offset:offsets) field|=i>=offset && i<offset+4;
                if (!field) assert(packet[i]==0xa5);
            }
        }
    }
    std::array<uint8_t,0xb8> packet; packet.fill(0x5a);
    const auto before=packet;
    for (uint16_t invalid:{0,3,65535}) {
        assert(!tglPrepareOwnedSfcAlignedGeometry(packet,1080,1920,0,0,2,2,2,invalid));
        assert(packet==before);
    }
    assert(!tglPrepareOwnedSfcAlignedGeometry(packet,1080,1920,0,UINT32_MAX,2,2,2,2));
    assert(packet==before);
    assert(tglPrepareOwnedSfcAlignedGeometry(packet,1080,1920,0,UINT32_MAX,2,2,2,1));
}

static void testSfcRegions() {
    std::array<uint8_t,0xb8> packet; packet.fill(0xa5);
    assert(tglPrepareOwnedSfcRegions(packet,1081,1921,0.75f,0.25f,
                                    1080,1920,0.5f,0.5f,2,2,2,2));
    const std::array<size_t,4> offsets{{0x3c,0x40,0x4c,0x50}};
    const std::array<uint32_t,4> expected{{1080,1920,540,960}};
    for (size_t i=0;i<4;++i) {
        uint32_t value=0; std::memcpy(&value,packet.data()+offsets[i],4);
        assert(value==expected[i]);
    }
    for (size_t i=0;i<packet.size();++i) {
        bool field=false;
        for (auto offset:offsets) field|=i>=offset && i<offset+4;
        if (!field) assert(packet[i]==0xa5);
    }
    const auto before=packet;
    for (float invalid:{-1.0f,std::numeric_limits<float>::infinity(),
                        std::numeric_limits<float>::quiet_NaN(),1.0e30f}) {
        assert(!tglPrepareOwnedSfcRegions(packet,1080,1920,0,0,1080,1920,
                                         invalid,1,2,2,2,2));
        assert(packet==before);
    }
    assert(!tglPrepareOwnedSfcRegions(packet,10,10,11,0,10,10,1,1,2,2,2,2));
    assert(packet==before);
    assert(!tglPrepareOwnedSfcRegions(packet,10,10,0,0,10,10,1,1,2,2,2,3));
    assert(packet==before);
    assert(tglPrepareOwnedSfcRegions(packet,7,9,0,0,7,9,0.5f,0.5f,1,1,2,2));
    uint32_t h=0,w=0; std::memcpy(&h,packet.data()+0x4c,4); std::memcpy(&w,packet.data()+0x50,4);
    assert(h==4 && w==6);
}

static void testSfcRotationGeometry() {
    for (uint32_t rotation:{0u,1u,2u,3u,4u,5u,6u,7u,UINT32_MAX}) {
        std::array<uint8_t,0xb8> packet; packet.fill(0xa5);
        auto put=[&](size_t offset,uint32_t value) { std::memcpy(packet.data()+offset,&value,4); };
        auto get=[&](size_t offset) { uint32_t value; std::memcpy(&value,packet.data()+offset,4); return value; };
        put(0x24,1080); put(0x28,1920); put(0x3c,540); put(0x40,960);
        put(0x4c,1500); put(0x50,2000);
        const auto before=packet;
        assert(tglPrepareOwnedSfcRotationGeometry(packet,rotation,7,13,2,4));
        const bool unrotated=rotation==0 || rotation==2 || rotation==4 || rotation==5;
        assert(get(0x4c)==(unrotated?1080u:1500u));
        assert(get(0x50)==(unrotated?1920u:1080u));
        assert(get(0x54)==6 && get(0x58)==12);
        float x=0,y=0; std::memcpy(&x,packet.data()+0x5c,4); std::memcpy(&y,packet.data()+0x60,4);
        assert(x==float(get(0x50))/960.0f && y==float(get(0x4c))/540.0f);
        for (size_t i=0;i<packet.size();++i)
            if (i<0x4c || i>=0x64) assert(packet[i]==before[i]);
        const auto valid=packet;
        assert(!tglPrepareOwnedSfcRotationGeometry(packet,rotation,0,0,2,3) && packet==valid);
        put(0x40,0); const auto zero=packet;
        assert(!tglPrepareOwnedSfcRotationGeometry(packet,rotation,0,0,2,2) && packet==zero);
    }
}

static void testSfcBypass() {
    const std::array<uint32_t,32> listed{{uint32_t(-6),0x19,0x1b,0x1c,0x1e,0x53,0x52,
        uint32_t(-4),0x20,0x21,0x22,0x23,0x29,0x2a,0x2b,0x2c,0x24,0x25,0x27,
        0x28,0x26,uint32_t(-7),0xd,0xe,0xf,0x10,0x11,0x13,0x12,0x17,0x14,0x18}};
    for (uint32_t index=0;index<99;++index) {
        const uint32_t format=index<92?index:uint32_t(-int(index-91));
        bool member=false; for (auto value:listed) member|=value==format;
        for (float x:{0.5f,1.0f,1.0001f,2.0f}) for (float y:{0.5f,1.0f,2.0f}) {
            std::array<uint8_t,0xb8> packet; packet.fill(0xa5);
            assert(tglPrepareOwnedSfcBypass(packet,format,x,y));
            const uint8_t expected=!(member && (x>1 || y>1));
            assert(packet[0x64]==expected && packet[0x65]==expected);
            for (size_t i=0;i<packet.size();++i)
                if (i!=0x64 && i!=0x65) assert(packet[i]==0xa5);
        }
    }
    std::array<uint8_t,0xb8> packet; packet.fill(0xa5); const auto before=packet;
    for (float invalid:{-1.0f,std::numeric_limits<float>::infinity(),
                        std::numeric_limits<float>::quiet_NaN()}) {
        assert(!tglPrepareOwnedSfcBypass(packet,0x19,invalid,1) && packet==before);
        assert(!tglPrepareOwnedSfcBypass(packet,0x19,1,invalid) && packet==before);
    }
}

static void testSfcFilterRotation() {
    const std::array<uint32_t,16> rgb{{5,6,1,2,3,4,0x50,0x51,uint32_t(-8),7,
                                    0xa,0xb,0xc,uint32_t(-9),0x55,0x5a}};
    const std::array<uint32_t,8> modes{{0,1,2,3,0,0,3,1}};
    for (uint32_t format=0;format<101;++format) {
        const uint32_t nativeFormat=format<92?format:uint32_t(-int(format-91));
        bool member=false; for (auto value:rgb) member|=value==nativeFormat;
        for (uint32_t rotation=0;rotation<8;++rotation) for (unsigned flags=0;flags<8;++flags) {
            std::array<uint8_t,0xb8> packet; packet.fill(0xa5);
            packet[0x6a]=(flags&1)?0xff:0xfe;
            const auto before=packet;
            tglPrepareOwnedSfcFilterRotation(packet,nativeFormat,rotation,
                                             (flags&2)?0xff:0xfe,(flags&4)?1:0);
            uint32_t mode=0,mirror=0;
            std::memcpy(&mode,packet.data()+0x6c,4); std::memcpy(&mirror,packet.data()+0x70,4);
            assert(packet[0x66]==uint8_t(member && (flags&1)));
            assert(packet[0x69]==uint8_t((flags&6)!=0));
            assert(mode==modes[rotation] && packet[0x74]==uint8_t(rotation>3));
            assert(mirror==(rotation<=3?0xa5a5a5a5u:rotation<=5?rotation-4:4));
            for (size_t i=0;i<packet.size();++i)
                if (i!=0x66 && i!=0x69 && !(i>=0x6c && i<=0x74)) assert(packet[i]==before[i]);
        }
    }
    std::array<uint8_t,0xb8> packet{};
    tglPrepareOwnedSfcFilterRotation(packet,1,UINT32_MAX,0,0);
    assert(packet[0x74]==0 && packet[0x6c]==0);
}

static void testSfcAlpha() {
    for (uint32_t format:{1u,3u,0x15u,0x19u,UINT32_MAX})
    for (uint32_t mode:{0u,1u,2u,3u,UINT32_MAX})
    for (unsigned flags=0;flags<8;++flags) for (uint32_t color=0;color<4;++color) {
        std::array<uint8_t,0xb8> packet; packet.fill(0xa5);
        const float background=0.25f; std::memcpy(packet.data()+0x84,&background,4);
        const auto before=packet;
        tglPrepareOwnedSfcAlpha(packet,format,flags&1,mode,0.75f,flags&2,
                               (flags&4)?0xff:0xfe,color);
        const bool applicable=(flags&1) && (format==1 || format==3 || format==0x15);
        const float expected=applicable && mode==0?0.75f:
            applicable && mode==2 && (flags&2)?background:1.0f;
        const float expectedBackground=!applicable || mode==2?background:mode==0?0.75f:1.0f;
        float alpha=0,fill=0; std::memcpy(&alpha,packet.data()+0xc,4);
        std::memcpy(&fill,packet.data()+0x84,4);
        assert(alpha==expected && fill==expectedBackground);
        assert(packet[0x88]==uint8_t(bool(flags&4)) && packet[0x89]==packet[0x88]);
        assert(packet[0x8a]==uint8_t(color==1 || color==2));
        for (size_t i=0;i<packet.size();++i)
            if (!(i>=0xc && i<0x10) && !(i>=0x84 && i<=0x8a)) assert(packet[i]==before[i]);
    }
}

static void testSfcResourceBinding() {
    std::array<uint8_t,0xb8> packet; packet.fill(0xa5);
    const auto before=packet;
    TglOwnedSurfaceStorage output;
    std::array<uint8_t,0x148> avs{},ief{};
    const uint64_t handle=1;
    for (auto* storage:{&output.resource,&avs,&ief}) std::memcpy(storage->data()+0x20,&handle,8);
    const uint32_t outputTag=0x12345678;
    std::memcpy(output.prefix.data()+0x140,&outputTag,4);
    void* os=reinterpret_cast<void*>(uintptr_t(7));
    unsigned callbackCalls=0;
    for(int status:{0,31,-7}) {
        assert(tglInvokeOwnedSfcOutputCallback(os,13,output,
            [&](void* actual,const TglOwnedSfcOutputRequest& request,TglOwnedSurfaceStorage& surface) noexcept {
                assert(actual==os && &surface==&output && request.channel==13 &&
                    !request.reserved && request.outputTag==outputTag);
                ++callbackCalls;return status;
            })==status);
    }
    assert(callbackCalls==3);
    assert(tglInvokeOwnedSfcOutputCallback(nullptr,13,output,
        [](void*,const auto&,auto&) noexcept {assert(false);return 0;})==5);
    unsigned calls=0;
    auto query=[&](TglOwnedSurfaceStorage& surface) noexcept {
        assert(&surface==&output); ++calls;
        const uint32_t pitch=7680; std::memcpy(surface.prefix.data()+0xf0,&pitch,4);
        const std::array<size_t,4> offsets{{0x104,0x108,0x114,0x118}};
        for (size_t i=0;i<4;++i) {
            const uint32_t value=0x12340000u+uint32_t(i+1);
            std::memcpy(surface.prefix.data()+offsets[i],&value,4);
        }
        return 0;
    };
    assert(tglBindOwnedSfcResources(packet,output,avs,ief,query)==0 && calls==1);
    const std::array<uintptr_t,3> expected{{reinterpret_cast<uintptr_t>(output.resource.data()),
        reinterpret_cast<uintptr_t>(avs.data()),reinterpret_cast<uintptr_t>(ief.data())}};
    for (size_t i=0;i<3;++i) {
        uint64_t p=0; std::memcpy(&p,packet.data()+0x90+i*8,8); assert(p==expected[i]);
    }
    uint32_t pitch=0; std::memcpy(&pitch,packet.data()+0xa8,4); assert(pitch==7680);
    for (size_t i=0;i<4;++i) {
        uint16_t value=0; std::memcpy(&value,packet.data()+0xac+i*2,2); assert(value==i+1);
    }
    for (size_t i=0;i<packet.size();++i) if (i<0x90 || i>=0xb4) assert(packet[i]==before[i]);
    const auto valid=packet;
    auto failed=[&](TglOwnedSurfaceStorage&) noexcept { ++calls; return 77; };
    assert(tglBindOwnedSfcResources(packet,output,avs,ief,failed)==77 && packet==valid && calls==2);
    ief.fill(0);
    assert(tglBindOwnedSfcResources(packet,output,avs,ief,query)==5 && packet==valid && calls==2);
    std::memcpy(ief.data()+0x20,&handle,8);
    // Bind the complete TGL argument directly; preserve its Gen12 tail.
    TglOwnedSfcStateParametersPacket gen12{};gen12.fill(0x6c);
    std::memcpy(gen12.data(),before.data(),before.size());
    auto unchanged=[](TglOwnedSurfaceStorage&) noexcept { return 0; };
    assert(tglBindOwnedSfcResources(gen12,output,avs,ief,unchanged)==0);
    assert(std::memcmp(gen12.data(),valid.data(),valid.size())==0);
    for(size_t i=0xb8;i<gen12.size();++i) assert(gen12[i]==0x6c);
    const auto gen12Valid=gen12;
    auto rejected=[](TglOwnedSurfaceStorage&) noexcept { return 83; };
    assert(tglBindOwnedSfcResources(gen12,output,avs,ief,rejected)==83 && gen12==gen12Valid);
    auto revoked=[](TglOwnedSurfaceStorage& surface) noexcept { surface.resource.fill(0); return 0; };
    assert(tglBindOwnedSfcResources(packet,output,avs,ief,revoked)==5 && packet==valid);
}

static void testSfcFill() {
    const std::array<uint8_t,4> converted{{17,83,211,0}};
    for (uint32_t format:{0x19u,0x15u,1u,2u,0x50u,3u,UINT32_MAX}) {
        std::array<uint8_t,0xb8> packet; packet.fill(0xa5); const auto before=packet;
        tglPrepareOwnedSfcFill(packet,false,format,converted,127);
        assert(packet==before);
        tglPrepareOwnedSfcFill(packet,true,format,converted,127);
        const std::array<uint8_t,3> expected=(format==0x19 || format==0x15)?
            std::array<uint8_t,3>{{83,17,211}}:(format==1 || format==2 || format==0x50)?
            std::array<uint8_t,3>{{17,83,211}}:std::array<uint8_t,3>{{211,83,17}};
        assert(packet[0x75]==1);
        for (size_t i=0;i<3;++i) {
            float value=0; std::memcpy(&value,packet.data()+0x78+i*4,4);
            assert(value==float(expected[i])/255.0f);
        }
        float alpha=0; std::memcpy(&alpha,packet.data()+0x84,4);
        assert(alpha==127.0f/255.0f);
        for (size_t i=0;i<packet.size();++i)
            if (i!=0x75 && !(i>=0x78 && i<0x88)) assert(packet[i]==before[i]);
        tglPrepareOwnedSfcAlpha(packet,1,true,2,0,true,0,0);
        float selected=0; std::memcpy(&selected,packet.data()+0xc,4);
        assert(selected==alpha);
    }
}

static void testSfcFillCache() {
    TglOwnedSfcFillCache cache;
    std::array<uint8_t,0xb8> packet{};
    std::array<uint8_t,4> sample{{17,83,211,127}};
    unsigned calls=0; bool allowed=true;
    auto convert=[&](std::array<uint8_t,4>& out,const std::array<uint8_t,4>& in,
                     uint32_t source,uint32_t target) noexcept {
        ++calls; out={{uint8_t(in[0]+source),in[1],uint8_t(in[2]-target),0}};
        return allowed;
    };
    const auto empty=packet;
    assert(cache.prepare(packet,false,0x19,sample,1,2,convert) && packet==empty && calls==0);
    assert(cache.prepare(packet,true,0x19,sample,1,2,convert) && calls==1);
    float value=0; std::memcpy(&value,packet.data()+0x7c,4); assert(value==18.0f/255.0f);
    std::memcpy(&value,packet.data()+0x84,4); assert(value==127.0f/255.0f);
    assert(cache.prepare(packet,true,1,sample,1,2,convert) && calls==1);
    std::memcpy(&value,packet.data()+0x78,4); assert(value==18.0f/255.0f);
    const auto valid=packet;
    allowed=false;
    assert(!cache.prepare(packet,true,1,sample,2,2,convert) && calls==2 && packet==valid);
    assert(cache.prepare(packet,true,1,sample,1,2,convert) && calls==2);
    allowed=true;
    assert(cache.prepare(packet,true,1,sample,2,2,convert) && calls==3);
    assert(cache.prepare(packet,true,1,sample,2,3,convert) && calls==4);
    ++sample[3];
    assert(cache.prepare(packet,true,1,sample,2,3,convert) && calls==5);
    cache.reset();
    assert(cache.prepare(packet,true,1,sample,2,3,convert) && calls==6);
}

static void testFillCscBinding() {
    TglFillCscBinding binding;
    constexpr uintptr_t base = 0x100000000;
    const std::array<uint8_t,16> prologue{0x55,0x48,0x89,0xe5,0x48,0x81,0xec,0xb0,
        0,0,0,0x48,0x8d,0x45,0xc0,0x45};
    const std::array<uint8_t,5> matrix{0xe8,0x4f,0x6f,0,0};
    const std::array<uint8_t,5> convert{0xe8,0xf7,0xf6,0xff,0xff};
    uintptr_t corrupt = 0; bool identity = true, rx = true, readable = true;
    auto read = [&](uintptr_t p, void* out, size_t n) {
        if (!readable) return false;
        if (p == base+0x4d970 && n == prologue.size()) std::memcpy(out,prologue.data(),n);
        else if (p == base+0x4d9fc && n == matrix.size()) std::memcpy(out,matrix.data(),n);
        else if (p == base+0x4da74 && n == convert.size()) std::memcpy(out,convert.data(),n);
        else return false;
        if (p == corrupt) static_cast<uint8_t*>(out)[0] ^= 1;
        return true;
    };
    auto qualify = [&](uintptr_t p) { return identity && p == base; };
    auto executable = [&](uintptr_t,size_t) { return rx; };
    auto resolve = [&] { return binding.resolve(base,read,qualify,executable); };
    assert(resolve() && binding.entry == base+0x4d970);
    for (auto offset : {0x4d970,0x4d9fc,0x4da74}) {
        corrupt = base+offset; assert(!resolve() && !binding.entry && !binding.image);
    }
    corrupt = 0; identity = false; assert(!resolve()); identity = true;
    rx = false; assert(!resolve()); rx = true;
    readable = false; assert(!resolve()); readable = true;
    assert(!binding.resolve(UINTPTR_MAX,read,qualify,executable));
    assert(resolve());
    std::array<uint8_t,4> output{{9,8,7,6}}, source{{1,2,3,4}};
    const auto original = output;
    unsigned calls = 0; bool nativeResult = false;
    auto invoke = [&](uintptr_t entry,uint8_t* out,const uint8_t* in,
                      uint32_t sourceCs,uint32_t targetCs) {
        assert(entry == base+0x4d970 && sourceCs == 3 && targetCs == 7);
        assert(std::memcmp(in,source.data(),4) == 0);
        ++calls; std::memcpy(out,in,4); return nativeResult;
    };
    auto convertFill = [&] {
        return tglConvertNativeFill(binding,output,source,3,7,read,qualify,executable,invoke);
    };
    assert(!convertFill() && output == original && calls == 1);
    nativeResult = true;
    assert(convertFill() && output == source && calls == 2);
    output = original;
    corrupt = base+0x4da74;
    assert(!convertFill() && output == original && calls == 2); corrupt = 0;
    ++binding.entry;
    assert(!convertFill() && output == original && calls == 2); --binding.entry;
    identity = false;
    assert(!convertFill() && output == original && calls == 2); identity = true;
    // Aliased input/output remains safe: publication follows native completion.
    assert(tglConvertNativeFill(binding,source,source,3,7,read,qualify,executable,invoke));
    assert(calls == 3);
    TglOwnedSfcFillCache cache;
    std::array<uint8_t,0xb8> packet{};
    auto nativeConvert = [&](std::array<uint8_t,4>& out,
                             const std::array<uint8_t,4>& in,
                             uint32_t from,uint32_t to) noexcept {
        return tglConvertNativeFill(binding,out,in,from,to,
                                    read,qualify,executable,invoke);
    };
    nativeResult = false;
    const auto empty = packet;
    assert(!cache.prepare(packet,true,1,source,3,7,nativeConvert));
    assert(packet == empty && calls == 4);
    nativeResult = true;
    assert(cache.prepare(packet,true,1,source,3,7,nativeConvert) && calls == 5);
    const auto published = packet;
    assert(cache.prepare(packet,true,1,source,3,7,nativeConvert) && calls == 5);
    // A reset forces revalidation; revoked image admission must not call native.
    cache.reset(); identity = false;
    assert(!cache.prepare(packet,true,1,source,3,7,nativeConvert));
    assert(packet == published && calls == 5);
    identity = true;
    assert(cache.prepare(packet,true,1,source,3,7,nativeConvert) && calls == 6);
}

static void testSfcFrameFields() {
    for (uint32_t format : {0x19u,0xdu,1u,UINT32_MAX})
    for (uint32_t siting=0;siting<128;++siting) {
        std::array<uint8_t,12> parameters; parameters.fill(0xa5);
        auto value=siting;
        tglPrepareOwnedSfcAvsPhases(parameters,value,format);
        uint32_t x=0,y=0,first=0;
        std::memcpy(&first,parameters.data(),4);
        std::memcpy(&x,parameters.data()+4,4); std::memcpy(&y,parameters.data()+8,4);
        assert(first==0xa5a5a5a5);
        assert(x==(siting&2 ? 4u : siting&4 ? 8u : 0u));
        assert(y==(!siting && format==0x19 ? 4u : siting&0x20 ? 4u : siting&0x40 ? 8u : 0u));
        assert(value==(siting ? siting : 0x11u));
    }
    for (unsigned gate=0;gate<4;++gate) {
        std::array<uint8_t,0xb8> packet; packet.fill(0xa5);
        const auto original=packet;
        bool invoke=true;
        assert(tglPrepareOwnedSfcCompression(packet,0x19,gate==0?0:7,
            gate==1?0:1,gate==2?0:1,gate==3?0:1,1,1,invoke));
        assert(!invoke && packet==original);
    }
    {
        std::array<uint8_t,0xb8> packet; packet.fill(0xa5);
        const auto original=packet;
        bool invoke=true;
        assert(!tglPrepareOwnedSfcCompression(packet,0x19,7,1,1,1,
            std::numeric_limits<float>::quiet_NaN(),1,invoke));
        assert(!invoke && packet==original);
    }
    for (uint32_t format : {0x19u,0x10u,0xeu,0xdu,0x53u})
    for (float y : {0.25f,0.5f,1.0f})
    for (float x : {0.25f,0.5f,1.0f}) {
        std::array<uint8_t,0xb8> packet; packet.fill(0xa5);
        const auto original=packet;
        bool invoke=false;
        assert(tglPrepareOwnedSfcCompression(packet,format,7,1,1,1,y,x,invoke));
        const bool eligible=format==0x19 || format==0x10 || format==0xe;
        assert(invoke==eligible);
        if (!eligible) assert(packet==original);
        else {
            uint32_t mode=99; std::memcpy(&mode,packet.data()+0x8c,4);
            const uint32_t expected=y>=0.5f && x>=0.5f ? 1u : y<0.5f && x<0.5f ? 2u : 0u;
            assert(mode==expected && packet[0x8b]==(expected!=0));
        }
    }
    for (uint32_t rotation=0;rotation<8;++rotation)
    for (bool field : {false,true})
    for (float scale : {0.5f,1.0f,2.0f}) {
        TglOwnedSfcGeometryInput in;
        in.inputFormat=in.outputFormat=0x19;
        in.inputHeight=field ? 540 : 1080; in.inputWidth=field ? 960 : 1920;
        in.outputHeight=1080; in.outputWidth=1920;
        in.bottom=float(in.inputHeight); in.right=float(in.inputWidth);
        in.fieldInput=field; in.rotation=rotation;
        in.scaleY=in.scaleX=scale;
        in.frameHeightAlignment=8; in.frameWidthAlignment=16;
        std::array<uint8_t,0xb8> packet{};
        assert(tglPrepareOwnedSfcGeometry(packet,in));
        const bool unrotated=rotation==0 || rotation==2 || rotation==4 || rotation==5;
        const uint32_t expectedHeight=std::min(uint32_t(1080*scale),unrotated?1080u:1920u);
        const uint32_t expectedWidth=std::min(uint32_t(1920*scale),unrotated?1920u:1080u);
        uint32_t h=0,w=0,sourceH=0,sourceW=0;
        float x=0,y=0;
        std::memcpy(&h,packet.data()+0x4c,4); std::memcpy(&w,packet.data()+0x50,4);
        std::memcpy(&sourceH,packet.data()+0x3c,4); std::memcpy(&sourceW,packet.data()+0x40,4);
        std::memcpy(&x,packet.data()+0x5c,4); std::memcpy(&y,packet.data()+0x60,4);
        assert(h==expectedHeight && w==expectedWidth && sourceH==1080 && sourceW==1920);
        assert(x==float(expectedWidth)/1920.0f && y==float(expectedHeight)/1080.0f);
    }
    {
        TglOwnedSfcGeometryInput in;
        in.inputFormat=in.outputFormat=0x19;
        in.inputHeight=in.outputHeight=1080; in.inputWidth=in.outputWidth=1920;
        in.bottom=1080; in.right=1920;
        in.frameHeightAlignment=8; in.frameWidthAlignment=16;
        std::array<uint8_t,0xb8> packet{};
        assert(tglPrepareOwnedSfcGeometry(packet,in));
        uint32_t height=0,width=0;
        std::memcpy(&height,packet.data()+0x4c,4);
        std::memcpy(&width,packet.data()+0x50,4);
        assert(height==1080 && width==1920);
        const auto valid=packet;
        in.top=1080; // zero region: late rotation-stage failure
        assert(!tglPrepareOwnedSfcGeometry(packet,in) && packet==valid);
        in.top=0; in.scaleX=std::numeric_limits<float>::infinity();
        assert(!tglPrepareOwnedSfcGeometry(packet,in) && packet==valid);
        in.scaleX=1; in.frameWidthAlignment=3;
        assert(!tglPrepareOwnedSfcGeometry(packet,in) && packet==valid);
    }
    for (uint32_t format=0;format<256;++format) {
        const auto alignment=tglOwnedSfcAlignment(format);
        const auto pack=tglOwnedChromaColorPack(format);
        assert(alignment.width==(pack<2 ? 2 : 1));
        assert(alignment.height==(pack==0 ? 2 : 1));
    }
    assert(tglOwnedSfcAlignment(0x19).width==2 && tglOwnedSfcAlignment(0x19).height==2);
    assert(tglOwnedSfcAlignment(0xd).width==2 && tglOwnedSfcAlignment(0xd).height==1);
    assert(tglOwnedSfcAlignment(1).width==1 && tglOwnedSfcAlignment(1).height==1);
    assert(tglOwnedSfcAlignment(UINT32_MAX).width==1);
    {
        int child=0,foreign=0;
        struct Context {
            void* child;
            bool owns(void* p) const noexcept {return p==child;}
        } context{&child};
        using Callback=TglOwnedVpAlignmentCallbacks<TglOwnedVpSfcResolve<Context>>;
        using Compression=TglOwnedVpCompressionFormatCallback<TglOwnedVpSfcResolve<Context>>;
        using Supports=bool(*)(void*,uint32_t) noexcept;
        const Supports supports=&Compression::supported;
        using Input=void(*)(void*,uint32_t,uint32_t,uint16_t*,uint16_t*) noexcept;
        using Output=void(*)(void*,uint32_t,uint16_t*,uint16_t*) noexcept;
        const Input input=&Callback::input;const Output output=&Callback::output;
        uint16_t w=77,h=88;
        input(&child,0x19,0,&w,&h);assert(w==77 && h==88);
        assert(!supports(&child,0x19));
        {
            TglOwnedVpSfcScopedContext<Context> scope(&child,context);
            assert(scope.bound());
            for(uint32_t f=0;f<256;++f) {
                assert(supports(&child,f)==(f==0x19 || f==0x10 || f==0xe));
                struct Guarded {uint16_t before=0xa55a,value=77,after=0x5aa5;} width,height;
                const auto expected=tglOwnedSfcAlignment(f);
                input(&child,f,UINT32_MAX,&width.value,&height.value);
                assert(width.value==expected.width && height.value==expected.height);
                assert(width.before==0xa55a && width.after==0x5aa5 &&
                    height.before==0xa55a && height.after==0x5aa5);
                output(&child,f,&w,&h);assert(w==expected.width && h==expected.height);
                uint16_t aliased=77;
                input(&child,f,0,&aliased,&aliased);
                assert(aliased==expected.width);
                output(&child,f,&aliased,&aliased);assert(aliased==expected.width);
            }
            for(void* p:{static_cast<void*>(nullptr),static_cast<void*>(&foreign)}) {
                assert(!supports(p,0x19));
                w=77;h=88;input(p,0x19,0,&w,&h);output(p,0x19,&w,&h);
                assert(w==77 && h==88);
            }
            w=77;h=88;
            input(&child,0x19,0,nullptr,&h);output(&child,0x19,&w,nullptr);
            assert(w==77 && h==88);
            assert(!supports(&child,UINT32_MAX));
        }
        output(&child,0x19,&w,&h);assert(w==77 && h==88);
        assert(!supports(&child,0x19));
    }
    {
        uint32_t h=999,w=888;
        assert(tglAdjustOwnedSfcFrame(1080,1920,1079,1919,false,8,16,h,w));
        assert(h==1080 && w==1920);
        assert(tglAdjustOwnedSfcFrame(1080,1920,0,0,false,8,16,h,w));
        assert(h==16 && w==64);
        assert(tglAdjustOwnedSfcFrame(8,32,0,0,false,8,16,h,w));
        assert(h==8 && w==32);
        assert(tglAdjustOwnedSfcFrame(540,960,539,959,true,8,16,h,w));
        assert(h==1080 && w==1920);
        const auto oldH=h,oldW=w;
        for (uint16_t alignment : {uint16_t(0),uint16_t(3),uint16_t(65535)}) {
            assert(!tglAdjustOwnedSfcFrame(1080,1920,1080,1920,false,alignment,16,h,w));
            assert(h==oldH && w==oldW);
        }
        assert(!tglAdjustOwnedSfcFrame(UINT32_MAX,1920,1080,1920,true,8,16,h,w));
        assert(!tglAdjustOwnedSfcFrame(UINT32_MAX,1920,UINT32_MAX,1920,false,8,16,h,w));
        assert(h==oldH && w==oldW);
        assert(tglAdjustOwnedSfcFloatFrame(1080,1920,1079.9f,1919.9f,false,8,16,h,w));
        assert(h==1080 && w==1920);
        assert(tglAdjustOwnedSfcFloatFrame(540,960,539.9f,959.9f,true,8,16,h,w));
        assert(h==1080 && w==1920);
        for (float bad : {-1.0f,std::numeric_limits<float>::infinity(),
                          std::numeric_limits<float>::quiet_NaN(),4294967296.0f}) {
            assert(!tglAdjustOwnedSfcFloatFrame(1080,1920,bad,1919,false,8,16,h,w));
            assert(!tglAdjustOwnedSfcFloatFrame(1080,1920,1079,bad,false,8,16,h,w));
            assert(h==oldH && w==oldW);
        }
    }
    {
        std::array<uint8_t,0xb8> packet; packet.fill(0xa5);
        auto expected=packet;
        std::fill(expected.begin()+4,expected.begin()+8,0);
        tglPrepareOwnedSfcPlatformField(packet);
        {
            TglOwnedVpSfcCallerStorage vp;
            using Context=TglOwnedVpPlatformPacketContext;
            using Callback=TglOwnedVpPlatformPacketCallback<TglOwnedVpSfcResolve<Context>>;
            using Setup=TglOwnedVpPlatformSetupCallback<TglOwnedVpSfcResolve<Context>>;
            using Send=TglOwnedVpPlatformSendCallback<TglOwnedVpSfcResolve<Context>>;
            using SendFunction=int(*)(void*,void*,void*,void*,void*,void*) noexcept;
            const SendFunction send=&Send::send;
            using SetupFunction=int(*)(void*,void*,void*,void*) noexcept;
            const SetupFunction setup=&Setup::setup;
            using Produce=void(*)(void*,void*,void*) noexcept;
            const Produce produce=&Callback::produce;
            std::array<uint8_t,0xb8> owned,foreign;owned.fill(0xa5);foreign.fill(0x5a);
            const auto saved=owned,other=foreign;
            for(bool wrongPacket:{false,true}) {
                owned=saved;
                Context context(vp,owned);
                assert(setup(vp.bytes.data(),nullptr,nullptr,nullptr)==5);
                assert(send(vp.bytes.data(),nullptr,nullptr,nullptr,nullptr,nullptr)==5);
                produce(vp.bytes.data(),nullptr,owned.data());
                assert(context.status()==5 && owned==saved);
                {
                    TglOwnedVpSfcScopedContext<Context> scope(vp.bytes.data(),context);
                    assert(scope.bound());
                    const auto vpBefore=vp.bytes;
                    assert(setup(vp.bytes.data(),owned.data(),foreign.data(),nullptr)==0);
                    assert(setup(nullptr,nullptr,nullptr,nullptr)==5);
                    assert(setup(foreign.data(),nullptr,nullptr,nullptr)==5);
                    assert(send(vp.bytes.data(),nullptr,nullptr,nullptr,nullptr,nullptr)==0);
                    assert(send(vp.bytes.data(),owned.data(),foreign.data(),owned.data(),
                                foreign.data(),owned.data())==0);
                    assert(send(nullptr,nullptr,nullptr,nullptr,nullptr,nullptr)==5);
                    assert(send(foreign.data(),nullptr,nullptr,nullptr,nullptr,nullptr)==5);
                    assert(vp.bytes==vpBefore && owned==saved && foreign==other);
                    produce(nullptr,nullptr,owned.data());
                    produce(foreign.data(),nullptr,owned.data());
                    assert(context.status()==5 && owned==saved);
                    if(wrongPacket) {
                        produce(vp.bytes.data(),nullptr,foreign.data());
                        produce(vp.bytes.data(),nullptr,owned.data());
                        assert(context.status()==5 && owned==saved && foreign==other);
                    } else {
                        produce(vp.bytes.data(),foreign.data(),owned.data());
                        assert(context.status()==0 && foreign==other);
                        for(size_t i=0;i<owned.size();++i)
                            assert(owned[i]==(i>=4 && i<8 ? 0 : saved[i]));
                    }
                }
                const auto after=owned;
                assert(setup(vp.bytes.data(),nullptr,nullptr,nullptr)==5);
                produce(vp.bytes.data(),nullptr,owned.data());assert(owned==after);
            }
        }
        assert(packet==expected);
    }
    for (uint32_t mode : {0u,1u,2u,UINT32_MAX}) {
        std::array<uint8_t,0xb8> packet; packet.fill(0xa5);
        auto expected=packet;
        const uint32_t format=0x19,width=1920,height=1080;
        const uint32_t frameMode=mode==1 ? 2u : 1u,zero=0;
        for (size_t offset : {size_t(0x10),size_t(0x14)})
            std::memcpy(expected.data()+offset,&zero,4);
        std::memcpy(expected.data()+0x2c,&format,4);
        std::memcpy(expected.data()+0x30,&height,4);
        std::memcpy(expected.data()+0x34,&width,4);
        std::memcpy(expected.data()+0x38,&frameMode,4);
        tglPrepareOwnedSfcFrameFields(packet,format,mode,width,height);
        assert(packet==expected);
    }
}

int main() {
    {
        struct Context {
            void* child;
            std::array<void*,5> captured{};
            unsigned calls=0;
            bool owns(void* value) const noexcept {return value==child;}
            void prepare(void* conditional,void* parameters,void* source,
                void* target,void* settings) noexcept {
                captured={conditional,parameters,source,target,settings};++calls;
            }
        };
        std::array<int,6> values{};
        Context context{&values[0]};
        using Callback=TglOwnedVpFrameCallback<TglOwnedVpSfcResolve<Context>>;
        using Prepare=void(*)(void*,void*,void*,void*,void*,void*) noexcept;
        const Prepare prepare=&Callback::prepare;
        auto call=[&](void* child,void* source,void* target,void* conditional) noexcept {
            prepare(child,conditional,&values[2],source,target,&values[5]);
        };
        call(context.child,&values[3],&values[4],&values[1]);
        assert(context.calls==0); // No scoped owner, no dispatch.
        {
            TglOwnedVpSfcScopedContext<Context> scope(context.child,context);
            assert(scope.bound());
            call(nullptr,&values[3],&values[4],&values[1]);
            call(&values[1],&values[3],&values[4],&values[1]);
            call(context.child,nullptr,&values[4],&values[1]);
            call(context.child,&values[3],nullptr,&values[1]);
            assert(context.calls==0);
            call(context.child,&values[3],&values[4],&values[1]);
            assert(context.calls==1);
            for(size_t i=0;i<5;++i) assert(context.captured[i]==&values[i+1]);
            call(context.child,&values[3],&values[4],nullptr);
            assert(context.calls==2 && !context.captured[0]);
            assert(context.captured[1]==&values[2] && context.captured[4]==&values[5]);
        }
        call(context.child,&values[3],&values[4],&values[1]);
        assert(context.calls==2);
    }
    {
        constexpr uintptr_t os=0x1000,image=0x100000;
        uintptr_t slot=image+0x659b0;
        std::array<uint8_t,20> anchor{
            0x55,0x48,0x89,0xe5,0x48,0x89,0x7d,0xf8,0x48,0x89,
            0x75,0xf0,0x89,0x55,0xec,0x89,0x4d,0xe8,0x5d,0xc3};
        unsigned reads=0,fail=0,invoked=0,used=0;
        auto read=[&](uintptr_t address,void* out,size_t size) noexcept {
            if(++reads==fail) return false;
            if(address==os+0x128 && size==8) {std::memcpy(out,&slot,8);return true;}
            if(address==image+0x659b0 && size==anchor.size()) {
                std::memcpy(out,anchor.data(),size);return true;
            }
            return false;
        };
        auto qualify=[](uintptr_t base) noexcept {return base==image;};
        auto executable=[](uintptr_t address,size_t size) noexcept {
            return address==image+0x659b0 && size==20;
        };
        std::array<uint8_t,0x2a8> surface{};
        auto invoke=[&](uintptr_t entry,uintptr_t context,const void* resource,uint32_t usage,uint32_t flag) noexcept {
            assert(entry==slot && context==os && resource==surface.data()+0x148);
            assert(usage==6 && flag==1);++invoked;
        };
        auto use=[&]() noexcept -> int {assert(invoked==used+1);++used;return -31;};
        TglNativeSfcOutputUsageBinding binding;
        assert(binding.withOutput(surface,use,invoke)==5 && !invoked);
        assert(binding.resolve(os,image,read,qualify,executable));
        assert(binding.withOutput(surface,use,invoke)==-31 && invoked==1 && used==1);
        surface[0xe8]=1;
        assert(binding.withOutput(surface,use,invoke)==5 && invoked==1 && used==1);
        surface[0xe8]=2;
        assert(binding.withOutput(surface,use,invoke)==-31 && invoked==2 && used==2);
        for(fail=1;fail<=2;++fail) {
            reads=0;assert(!binding.resolve(os,image,read,qualify,executable));
            assert(binding.withOutput(surface,use,invoke)==5 && invoked==2);
        }
        fail=0;++slot;
        assert(!binding.resolve(os,image,read,qualify,executable));--slot;
        for(size_t i=0;i<anchor.size();++i) {
            anchor[i]^=1;assert(!binding.resolve(os,image,read,qualify,executable));anchor[i]^=1;
        }
        assert(!binding.resolve(UINTPTR_MAX-3,image,read,qualify,executable));
        assert(!binding.resolve(os,UINTPTR_MAX-3,read,qualify,executable));
    }
    {
        unsigned reads=0,fail=0;uint8_t flag13=0,flag0b=0;
        const auto read=[&](uintptr_t address,void* out,size_t size) noexcept {
            ++reads;if(reads==fail) return false;
            assert(size==1 && (address==0x1013 || address==0x100b));
            *static_cast<uint8_t*>(out)=address==0x1013?flag13:flag0b;
            return true;
        };
        TglOwnedSfcLockParameters lock;lock.fill(0xa5);const auto sentinel=lock;
        assert(!tglReadOwnedSfcLockParameters(0,1,read,lock) && !reads && lock==sentinel);
        assert(!tglReadOwnedSfcLockParameters(UINTPTR_MAX-3,1,read,lock) && !reads);
        for(fail=1;fail<=2;++fail) {
            reads=0;assert(!tglReadOwnedSfcLockParameters(0x1000,1,read,lock));
            assert(lock==sentinel);
        }
        fail=0;reads=0;flag0b=3;
        assert(tglReadOwnedSfcLockParameters(0x1000,1,read,lock) && reads==2 && lock[8]==1);
        reads=0;flag13=1;fail=2;
        assert(tglReadOwnedSfcLockParameters(0x1000,2,read,lock) && reads==1);
        assert(lock[0]==2 && lock[8]==1);
    }
    for(unsigned a=0;a<256;++a) for(unsigned b=0;b<256;++b) {
        const auto lock=tglPrepareOwnedSfcLockParameters(1,uint8_t(a),uint8_t(b));
        static_assert(sizeof(lock)>=9,"native lock reads byte8");
        assert(lock[0]==1 && lock[8]==((a|b)&1));
        for(size_t i=1;i<lock.size();++i) if(i!=8) assert(!lock[i]);
    }
    {
        TglOwnedSfcCommandParameters parameters;
        static_assert(!std::is_copy_constructible_v<TglOwnedSfcCommandParameters> &&
            !std::is_copy_assignable_v<TglOwnedSfcCommandParameters> &&
            !std::is_move_constructible_v<TglOwnedSfcCommandParameters> &&
            !std::is_move_assignable_v<TglOwnedSfcCommandParameters>);
        parameters.ief[1]=7;parameters.state[0x88]=2;
        {
            TglOwnedIefTables tables{};
            for(size_t row=0;row<tables.size();++row)
                for(size_t column=0;column<64;++column) tables[row][column]=uint32_t(row*64+column);
            const std::array<uint16_t,3> thresholds{0x107,0x202,0x308};
            TglOwnedSfcCommandParameters original;assert(original.snapshotFrom(parameters));
            for(float strength:{0.f,1.f,63.f,64.f,-1.f}) {
                TglOwnedSfcCommandParameters candidate;assert(candidate.snapshotFrom(original));
                std::array<uint8_t,64> expected{};TglOwnedIefSurfaceFlags flags{};
                assert(tglBuildOwnedIefParameters(expected,flags,strength,3,2,thresholds,tables));
                assert(tglPrepareOwnedSfcIefParameters(candidate,true,strength,3,2,thresholds,tables));
                assert(candidate.ief==expected);
                auto state=original.state;state[0x67]=1;state[0x68]=1;
                assert(candidate.state==state);
            }
            TglOwnedSfcCommandParameters candidate;assert(candidate.snapshotFrom(original));
            assert(!tglPrepareOwnedSfcIefParameters(candidate,true,NAN,1,1,thresholds,tables));
            assert(candidate.ief==original.ief && candidate.state==original.state);
            assert(tglPrepareOwnedSfcIefParameters(candidate,false,NAN,1,1,thresholds,tables));
            assert((candidate.ief==std::array<uint8_t,64>{} && candidate.state==original.state));
            for(size_t offset:{size_t(0x28),size_t(0x30),size_t(0x38)}) {
                TglOwnedSfcCommandParameters occupied;assert(occupied.snapshotFrom(original));
                const uintptr_t borrowed=1;
                std::memcpy(occupied.ief.data()+offset,&borrowed,8);
                const auto savedIef=occupied.ief;const auto savedState=occupied.state;
                assert(!tglPrepareOwnedSfcIefParameters(occupied,false,0,0,0,thresholds,tables));
                assert(occupied.ief==savedIef && occupied.state==savedState);
                assert(!candidate.snapshotFrom(occupied) && !occupied.snapshotFrom(original));
            }
        }
        const TglOwnedIefTables compositionTables{};
        const std::array<uint16_t,3> compositionThresholds{7,2,8};
        assert(tglPrepareOwnedSfcIefParameters(parameters,true,1,1,1,
            compositionThresholds,compositionTables));
        assert(parameters.state[0x67]==1 && parameters.state[0x68]==1);
        const auto savedIef=parameters.ief;const auto savedState=parameters.state;
        TglOwnedVpCscCache cache;unsigned reads=0,produced=0,generated=0;
        auto read=[&](uintptr_t,void*,size_t) noexcept {++reads;return false;};
        auto produce=[&](TglCscCoefficients& out,uint32_t from,uint32_t to) noexcept {
            ++produced;assert(from==3 && to==5);
            for(size_t i=0;i<out.matrix.size();++i) out.matrix[i]=float(i+2);
            for(size_t i=0;i<out.inputOffsets.size();++i) {
                out.inputOffsets[i]=float(i+20);out.outputOffsets[i]=float(i+30);
            }
            return true;
        };
        auto generate=[&](TglOwnedSfcCommandParameters& ready) noexcept {
            ++generated;assert(&ready==&parameters);
            assert(ready.state[0x67]==1 && ready.state[0x68]==1);
            assert(ready.ief[3]==1 && ready.ief[4]==7 && ready.ief[5]==2 && ready.ief[6]==8);
            assert(ready.state[0x88]==1 && ready.ief[0x20]==1);
            for(size_t offset:{size_t(0x28),size_t(0x30),size_t(0x38)}) {
                uintptr_t pointer=0;std::memcpy(&pointer,ready.ief.data()+offset,8);
                assert(pointer);
                const auto* coefficients=reinterpret_cast<const float*>(pointer);
                const size_t count=offset==0x28 ? 9 : 3;
                const unsigned first=offset==0x28 ? 2 : (offset==0x30 ? 20 : 30);
                for(size_t i=0;i<count;++i) assert(coefficients[i]==float(first+i));
            }
            const auto leasedIef=ready.ief;const auto leasedState=ready.state;
            TglOwnedSfcCommandParameters escaped;escaped.state[0]=0x55;
            assert(!escaped.snapshotFrom(ready) && escaped.state[0]==0x55);
            const TglOwnedIefTables unusedTables{};
            const std::array<uint16_t,3> unusedThresholds{};
            assert(!tglPrepareOwnedSfcIefParameters(ready,false,0,0,0,
                unusedThresholds,unusedTables));
            assert(ready.ief==leasedIef && ready.state==leasedState);
            assert(!cache.reset());return 31;
        };
        assert(tglWithOwnedSfcCscCommands(parameters,cache,true,3,5,0,read,produce,generate)==31);
        assert(parameters.ief==savedIef && parameters.state==savedState);
        assert(produced==1 && generated==1 && reads==0);
        assert(tglWithOwnedSfcCscCommands(parameters,cache,true,4,5,0,read,
            [](TglCscCoefficients&,uint32_t,uint32_t) noexcept {return false;},generate)==5);
        assert(generated==1 && parameters.ief==savedIef && parameters.state==savedState);
        assert(tglWithOwnedSfcCscCommands(parameters,cache,false,4,5,UINTPTR_MAX,read,produce,
            [&](TglOwnedSfcCommandParameters& ready) noexcept {
                ++generated;assert(ready.ief==savedIef && ready.state==savedState);return 89;
            })==89);
        assert(generated==2 && reads==0 && produced==1 && cache.reset());
        TglOwnedSfcIefInput input;
        input.enabled=true;input.strength=1;input.skinEnable=1;input.detailEnable=1;
        input.thresholds=compositionThresholds;
        assert(tglWithOwnedSfcFilterCommands(parameters,input,compositionTables,
            cache,true,3,5,0,read,produce,generate)==31);
        assert(generated==3 && produced==2 && parameters.ief==savedIef && parameters.state==savedState);
        input.strength=NAN;
        assert(tglWithOwnedSfcFilterCommands(parameters,input,compositionTables,
            cache,true,3,5,0,read,produce,generate)==5);
        assert(generated==3 && produced==2 && parameters.ief==savedIef && parameters.state==savedState);
        input.enabled=false;
        assert(tglWithOwnedSfcFilterCommands(parameters,input,compositionTables,
            cache,false,3,5,UINTPTR_MAX,read,produce,
            [&](TglOwnedSfcCommandParameters& ready) noexcept {
                ++generated;assert((ready.ief==std::array<uint8_t,64>{}));return 89;
            })==89);
        assert(generated==4 && produced==2 && reads==0);
        assert(!parameters.filtersInUse && cache.reset());
        input.enabled=true;input.strength=1;
        unsigned nested=0;
        auto rejectNested=[&]() noexcept {
            const auto oldIef=parameters.ief;const auto oldState=parameters.state;
            assert(parameters.filtersInUse);
            TglOwnedSfcCommandParameters escaped;
            assert(!escaped.snapshotFrom(parameters));
            assert(!parameters.snapshotFrom(escaped));
            assert(tglWithOwnedSfcFilterCommands(parameters,input,compositionTables,
                cache,false,3,5,0,read,produce,
                [&](auto&) noexcept {++nested;return 0;})==5);
            assert(!tglPrepareOwnedSfcIefParameters(parameters,false,0,0,0,
                compositionThresholds,compositionTables));
            assert(parameters.ief==oldIef && parameters.state==oldState);
        };
        for(bool csc:{false,true}) {
            assert(tglWithOwnedSfcFilterCommands(parameters,input,compositionTables,
                cache,csc,3,5,0,read,
                [&](TglCscCoefficients& out,uint32_t,uint32_t) noexcept {
                    rejectNested();out={};return true;
                },[&](auto&) noexcept {rejectNested();return -31;})==-31);
            assert(!parameters.filtersInUse && !nested && cache.reset());
        }
    }
    {
        TglOwnedVpCscCache cache;
        unsigned reads=0,produced=0;
        auto read=[&](uintptr_t,void*,size_t) noexcept {++reads;return false;};
        auto produce=[&](TglCscCoefficients& out,uint32_t from,uint32_t to) noexcept {
            ++produced;assert(from==3 && to==5);out.matrix[0]=2.f;return true;
        };
        assert(cache.prepare(3,5,0,read,produce));
        assert(cache.prepare(3,5,UINTPTR_MAX,read,produce));
        assert(produced==1 && reads==0);
        std::array<uint8_t,64> ief{};ief[1]=7;
        std::array<uint8_t,0xb8> state{};state[0x88]=2;
        const auto originalIef=ief;const auto originalState=state;
        unsigned calls=0;
        assert(cache.withParameters(ief,state,[&]() noexcept {
            ++calls;assert(ief[0x20]==1 && state[0x88]==1 && ief[1]==7);
            uintptr_t matrix=0;std::memcpy(&matrix,ief.data()+0x28,8);
            assert(matrix && *reinterpret_cast<const float*>(matrix)==2.f);
            assert(!cache.reset() && !cache.prepare(3,5,0,read,produce));
            assert(cache.withParameters(ief,state,[&]() noexcept {++calls;return 0;})==5);
            return 31;
        })==31);
        assert(calls==1 && ief==originalIef && state==originalState);
        assert(!cache.prepare(4,5,0,read,
            [](TglCscCoefficients&,uint32_t,uint32_t) noexcept {return false;}));
        assert(cache.withParameters(ief,state,[]() noexcept {return 0;})==0);
        ief[0x28]=1;
        assert(cache.withParameters(ief,state,[&]() noexcept {++calls;return 0;})==5);
        assert(calls==1 && cache.reset());
        ief[0x28]=0;
        assert(cache.withParameters(ief,state,[]() noexcept {return 0;})==5);
    }
    {
        std::array<uint8_t,0x34> source{};source[0]=1;
        std::array<float,12> values{};
        for(size_t i=0;i<values.size();++i) values[i]=float(i)+0.25f;
        std::memcpy(source.data()+4,values.data(),sizeof(values));
        unsigned reads=0,fail=0;
        auto read=[&](uintptr_t address,void* out,size_t size) noexcept {
            if(++reads==fail || address<0x1000 || address+size>0x1034) return false;
            std::memcpy(out,source.data()+address-0x1000,size);return true;
        };
        TglCscCoefficients coefficients;bool use=false;
        assert(tglReadOwnedVpCscOverride(3,5,0x1000,read,use,coefficients));
        assert(use && reads==3 && coefficients.matrix[8]==values[8]);
        assert(coefficients.inputOffsets[2]==values[11]);
        for(auto offset:coefficients.outputOffsets) assert(offset==0);
        for(fail=1;fail<=3;++fail) {
            reads=0;use=false;coefficients.matrix[0]=99;
            assert(!tglReadOwnedVpCscOverride(3,5,0x1000,read,use,coefficients));
            assert(!use && coefficients.matrix[0]==99);
        }
        fail=0;reads=0;use=true;
        assert(tglReadOwnedVpCscOverride(3,3,UINTPTR_MAX,read,use,coefficients));
        assert(!use && reads==0 && coefficients.matrix[0]==99);
        source[0]=0;
        assert(tglReadOwnedVpCscOverride(3,5,0x1000,read,use,coefficients));
        assert(!use && reads==1 && coefficients.matrix[0]==99);
    }
    {
        TglOwnedVpFramePreparation frame;
        frame.color.modes.state.at99c=3;frame.color.at9b8=true;
        frame.regions.scale.at928=0.5f;frame.regions.scale.at92c=0.75f;
        auto state=tglPrepareOwnedVpChromaStatePrefix(frame,0x19,true,false);
        std::array<uint8_t,16> performance{};
        uint32_t window=7;float sharpening=0.25f;
        std::memcpy(performance.data()+8,&window,4);
        std::memcpy(performance.data()+12,&sharpening,4);
        unsigned reads=0,fail=0;
        auto read=[&](uintptr_t address,void* out,size_t size) noexcept {
            if(++reads==fail || address<0x1000 || address+size>0x1010) return false;
            std::memcpy(out,performance.data()+address-0x1000,size);return true;
        };
        TglOwnedSfcAvsConfiguration config;
        assert(tglReadOwnedVpAvsConfiguration(frame,state,0x1000,read,config));
        assert(reads==2 && config.format==3 && config.scaleX==0.5f && config.scaleY==0.75f);
        assert(config.window==7.f && config.sharpening==0.25f);
        assert(config.siting==0x11 && config.forceFilter && config.symmetric);
        for(fail=1;fail<=2;++fail) {
            reads=0;config.format=99;
            assert(!tglReadOwnedVpAvsConfiguration(frame,state,0x1000,read,config));
            assert(config.format==99);
        }
        reads=0;fail=0;
        assert(tglReadOwnedVpAvsConfiguration(frame,state,0,read,config));
        assert(reads==0 && config.window==0 && config.sharpening==0);
        frame.regions.scale.at928=0;
        assert(!tglReadOwnedVpAvsConfiguration(frame,state,0x1000,read,config));
        assert(reads==0 && config.scaleX==0.5f);
    }
    {
        TglOwnedVpFramePreparationInput input;
        input.geometry.frameWidth=1920;input.geometry.frameHeight=1080;
        input.geometry.targetWidth=960;input.geometry.targetHeight=810;
        input.geometry.sourceRight=1920;input.geometry.sourceBottom=1080;
        input.geometry.destinationRight=960;input.geometry.destinationBottom=810;
        input.geometry.targetFormat=0x19;input.modes.source130=0x19;
        TglOwnedVpFramePreparation frame;
        assert(tglPrepareOwnedVpFrame(input,{},frame));
        TglOwnedVpChromaStatePrefix state;state.packet.fill(0xa5);
        TglOwnedSfcFillCache cache;TglOwnedVpFillAlphaInput fill;
        TglOwnedVpCompressionInput compression{0x19,7,1,1,1};
        unsigned converts=0,applies=0;
        auto convert=[&](auto& out,const auto& sample,uint32_t,uint32_t) noexcept {
            ++converts;out=sample;return true;
        };
        auto apply=[&](uint32_t mode) noexcept {++applies;assert(mode==1);};
        assert(tglPrepareOwnedVpCpuState(frame,input.geometry,false,false,fill,0,0,
            compression,cache,convert,apply,state));
        assert(converts==0 && applies==1 && state.packet[0x8b]==1);
        uint32_t height=0,width=0;
        std::memcpy(&height,state.packet.data()+0x4c,4);
        std::memcpy(&width,state.packet.data()+0x50,4);
        assert(height==810 && width==960);
        const auto saved=state.packet;
        compression.format=3;
        assert(!tglPrepareOwnedVpCpuState(frame,input.geometry,false,false,fill,0,0,
            compression,cache,convert,apply,state));
        assert(state.packet==saved && converts==0 && applies==1);
        compression.format=0x19;frame.at920=true;
        assert(!tglPrepareOwnedVpCpuState(frame,input.geometry,false,false,fill,0,0,
            compression,cache,
            [&](auto&,const auto&,uint32_t,uint32_t) noexcept {++converts;return false;},
            apply,state));
        assert(state.packet==saved && converts==1 && applies==1);
    }
    {
        constexpr uintptr_t image=0x100000,os=0x200000;
        uintptr_t entry=image+0x65d00;
        std::array<uint8_t,19> body{
            0x55,0x48,0x89,0xe5,0x31,0xc0,0x48,0x89,0x7d,0xf8,
            0x48,0x89,0x75,0xf0,0x89,0x55,0xec,0x5d,0xc3};
        auto read=[&](uintptr_t p,void* out,size_t n) {
            if(p==os+0x3a8 && n==8) std::memcpy(out,&entry,8);
            else if(p==image+0x65d00 && n==body.size()) std::memcpy(out,body.data(),n);
            else return false;
            return true;
        };
        auto qualify=[](uintptr_t p){return p==image;};
        auto executable=[](uintptr_t p,size_t n){return p==image+0x65d00 && n==19;};
        TglNativeVpCompressionBinding binding;
        assert(binding.resolve(os,image,read,qualify,executable));
        TglOwnedSurfaceStorage surface;
        const uint64_t backing=0x1234;std::memcpy(surface.resource.data()+0x20,&backing,8);
        unsigned calls=0;
        void* reentryContext=nullptr;
        int (*reenter)(void*) noexcept=nullptr;
        unsigned reentryHits=0;
        auto invoke=[&](uintptr_t fn,uintptr_t context,const void* resource,uint32_t mode) noexcept {
            assert(fn==entry && context==os && resource==surface.resource.data() && mode==2);
            if(reenter) {assert(reenter(reentryContext)==5);++reentryHits;}
            ++calls;
        };
        auto use=[&](auto apply) noexcept {
            std::array<uint8_t,0xb8> packet{};
            TglOwnedVpCompressionInput input{0x19,7,1,1,1};
            TglOwnedVpScale scale;scale.at928=scale.at92c=0.25f;
            return tglPrepareOwnedVpCompression(packet,input,scale,apply)?31:5;
        };
        assert(binding.withOutput(surface,use,invoke)==31 && calls==1);
        surface.resource.fill(0);
        assert(binding.withOutput(surface,use,invoke)==5 && calls==1);
        std::memcpy(surface.resource.data()+0x20,&backing,8);
        for(size_t i=0;i<body.size();++i) {
            body[i]^=1;assert(!binding.resolve(os,image,read,qualify,executable));
            assert(binding.withOutput(surface,use,invoke)==5 && calls==1);
            body[i]^=1;
        }
        ++entry;assert(!binding.resolve(os,image,read,qualify,executable));--entry;
        assert(!binding.resolve(os,image,read,qualify,[](uintptr_t,size_t){return false;}));
        assert(!binding.resolve(os,UINTPTR_MAX,read,qualify,executable));
        assert(binding.resolve(os,image,read,qualify,executable));
        assert(binding.withOutput(surface,use,invoke)==31 && calls==2);
        TglOwnedVpSfcCallerStorage vp;
        const float scale=0.25f;
        std::memcpy(vp.bytes.data()+0x928,&scale,4);std::memcpy(vp.bytes.data()+0x92c,&scale,4);
        std::array<uint8_t,0xd30> settings{};
        std::array<uint8_t,0xf8> packet{};packet[0xf7]=0x5a;
        uint32_t format=0x19,mode=7,tile=1;
        std::memcpy(surface.prefix.data()+0x130,&format,4);
        std::memcpy(surface.prefix.data()+0xe4,&tile,4);surface.tail[9]=1;
        std::memcpy(settings.data()+0xd28,&mode,4);settings[0xd2c]=1;
        const auto settingsAddress=reinterpret_cast<uintptr_t>(settings.data());
        const auto targetAddress=reinterpret_cast<uintptr_t>(&surface);
        bool denyRead=false;
        auto inputRead=[&](uintptr_t address,void* destination,size_t size) noexcept {
            if(denyRead) return false;
            if(reenter) {assert(reenter(reentryContext)==5);++reentryHits;}
            for(auto region:std::array<std::pair<uintptr_t,size_t>,2>{{
                {settingsAddress,settings.size()},{targetAddress,sizeof(surface)}}})
                if(address>=region.first && address-region.first<=region.second &&
                   size<=region.second-(address-region.first)) {
                    std::memcpy(destination,reinterpret_cast<const void*>(address),size);return true;
                }
            return false;
        };
        using Context=TglOwnedVpCompressionContext<TglOwnedSurfaceStorage,decltype(inputRead),decltype(invoke),0xf8>;
        Context context(vp,surface,packet,settingsAddress,binding,inputRead,invoke);
        struct Reentry {
            Context* context;
            void* settings;
            void* target;
            std::array<uint8_t,0xf8>* packet;
            std::array<uint8_t,0xf8> expected;
        } reentry{&context,settings.data(),&surface,&packet,packet};
        using Callback=TglOwnedVpCompressionCallback<TglOwnedVpSfcResolve<Context>>;
        static_assert(std::is_same_v<decltype(&Callback::compression),int(*)(void*,void*,void*,void*) noexcept>);
        assert(Callback::compression(vp.bytes.data(),settings.data(),&surface,packet.data())==5);
        {
            TglOwnedVpSfcScopedContext<Context> scope(vp.bytes.data(),context);assert(scope.bound());
            reentryContext=&reentry;
            reenter=[](void* raw) noexcept {
                auto& value=*static_cast<Reentry*>(raw);
                assert(*value.packet==value.expected);
                const int status=value.context->compression(value.settings,value.target,value.packet->data());
                assert(*value.packet==value.expected);
                return status;
            };
            assert(Callback::compression(vp.bytes.data(),settings.data(),&surface,packet.data())==0);
            assert(reentryHits>1);reenter=nullptr;
            assert(calls==3 && packet[0x8b]==1 && packet[0x8c]==2 && packet[0xf7]==0x5a);
            const auto saved=packet;
            assert(Callback::compression(&surface,settings.data(),&surface,packet.data())==5);
            assert(Callback::compression(vp.bytes.data(),nullptr,&surface,packet.data())==5);
            assert(Callback::compression(vp.bytes.data(),settings.data(),settings.data(),packet.data())==5);
            assert(Callback::compression(vp.bytes.data(),settings.data(),&surface,settings.data())==5);
            denyRead=true;
            assert(Callback::compression(vp.bytes.data(),settings.data(),&surface,packet.data())==5);
            denyRead=false;
            assert(calls==3 && packet==saved);
            // Native format gate skips OS/backing requirements and leaves output unchanged.
            format=0x50;std::memcpy(surface.prefix.data()+0x130,&format,4);
            assert(!binding.resolve(0,image,read,qualify,executable));
            assert(Callback::compression(vp.bytes.data(),settings.data(),&surface,packet.data())==0);
            assert(calls==3 && packet==saved);
        }
    }
    {
        TglOwnedVpCompressionInput input{0x19,7,1,1,1};
        std::array<uint8_t,0xb8> packet{};
        unsigned calls=0;uint32_t observed=99;
        auto apply=[&](uint32_t mode) noexcept {++calls;observed=mode;};
        TglOwnedVpScale scale;scale.at928=0.25f;scale.at92c=0.75f;
        assert(tglPrepareOwnedVpCompression(packet,input,scale,apply));
        assert(calls==1 && observed==0 && packet[0x8b]==0);
        scale.at92c=0.25f;
        assert(tglPrepareOwnedVpCompression(packet,input,scale,apply));
        assert(calls==2 && observed==2 && packet[0x8b]==1);
        scale.at928=scale.at92c=0.5f;
        assert(tglPrepareOwnedVpCompression(packet,input,scale,apply));
        assert(calls==3 && observed==1);
        const auto saved=packet;scale.at928=std::numeric_limits<float>::infinity();
        assert(!tglPrepareOwnedVpCompression(packet,input,scale,apply));
        assert(packet==saved && calls==3);
        std::array<uint8_t,0x29a> target{};std::array<uint8_t,0xd2d> settings{};
        std::memcpy(target.data()+0x130,&input.format,4);
        std::memcpy(target.data()+0xe4,&input.surfaceMode,4);target[0x299]=1;
        std::memcpy(settings.data()+0xd28,&input.executionMode,4);settings[0xd2c]=1;
        unsigned reads=0,fail=0;
        auto read=[&](uintptr_t address,void* out,size_t size) noexcept {
            if(++reads==fail) return false;
            if(address>=0x1000 && address+size<=0x129a)
                std::memcpy(out,target.data()+address-0x1000,size);
            else if(address>=0x2000 && address+size<=0x2d2d)
                std::memcpy(out,settings.data()+address-0x2000,size);
            else return false;
            return true;
        };
        TglOwnedVpCompressionInput snapshot;
        assert(tglReadOwnedVpCompression(0x2000,0x1000,read,snapshot));
        assert(reads==5 && snapshot.surfaceMode==1);
        for(fail=1;fail<=5;++fail) {
            reads=0;snapshot.format=99;
            assert(!tglReadOwnedVpCompression(0x2000,0x1000,read,snapshot));
            assert(snapshot.format==99);
        }
        fail=0;reads=0;const uint32_t rgb=3;
        std::memcpy(target.data()+0x130,&rgb,4);
        assert(tglReadOwnedVpCompression(0,0x1000,read,snapshot));
        assert(reads==1 && snapshot.format==3);
        assert(tglPrepareOwnedVpCompression(packet,snapshot,scale,apply));
        assert(calls==3 && packet==saved); // Irrelevant format skips scale/OS.
    }
    {
        TglOwnedVpChromaStatePrefix state;
        TglOwnedSfcFillCache cache;
        TglOwnedVpFramePreparation frame;
        TglOwnedVpFillAlphaInput input;
        input.sample={{10,20,30,64}};input.fillColorSpace=3;
        input.hasAlpha=true;input.alphaMode=2;
        frame.at920=true;frame.color.modes.state.at923=true;
        std::array<uint8_t,12> fill{};std::array<uint8_t,8> alphaObject{};
        std::memcpy(fill.data()+4,input.sample.data(),4);
        std::memcpy(fill.data()+8,&input.fillColorSpace,4);
        std::memcpy(alphaObject.data()+4,&input.alphaMode,4);
        unsigned reads=0,fail=0;
        auto read=[&](uintptr_t address,void* out,size_t size) noexcept {
            if(++reads==fail) return false;
            if(address>=0x1000 && address+size<=0x100c)
                std::memcpy(out,fill.data()+address-0x1000,size);
            else if(address>=0x2000 && address+size<=0x2008)
                std::memcpy(out,alphaObject.data()+address-0x2000,size);
            else return false;
            return true;
        };
        TglOwnedVpFillAlphaInput snapshot;
        assert(tglReadOwnedVpFillAlpha(true,0x1000,0x2000,read,snapshot));
        assert(reads==3 && snapshot.sample==input.sample && snapshot.alphaMode==2);
        for(fail=1;fail<=3;++fail) {
            reads=0;snapshot.fillColorSpace=99;
            assert(!tglReadOwnedVpFillAlpha(true,0x1000,0x2000,read,snapshot));
            assert(snapshot.fillColorSpace==99);
        }
        reads=0;fail=0;
        assert(tglReadOwnedVpFillAlpha(false,UINTPTR_MAX,0,read,snapshot));
        assert(reads==0 && !snapshot.hasAlpha);
        unsigned calls=0;
        auto convert=[&](auto& result,const auto& sample,uint32_t source,uint32_t target) noexcept {
            ++calls;assert(source==3 && target==5);result=sample;return true;
        };
        assert(tglPrepareOwnedVpFillAlpha(state,cache,frame,1,5,2,input,convert));
        float alpha=0;std::memcpy(&alpha,state.packet.data()+0xc,4);
        assert(alpha==64.0f/255.0f && state.packet[0x75]==1);
        assert(state.packet[0x88]==1 && state.packet[0x89]==1 && state.packet[0x8a]==1);
        assert(tglPrepareOwnedVpFillAlpha(state,cache,frame,1,5,2,input,convert));
        assert(calls==1);
        const auto saved=state.packet;
        input.sample[0]=11;
        assert(!tglPrepareOwnedVpFillAlpha(state,cache,frame,1,5,2,input,
            [&](auto&,const auto&,uint32_t,uint32_t) noexcept {++calls;return false;}));
        assert(state.packet==saved);
        input.sample[0]=10;
        assert(tglPrepareOwnedVpFillAlpha(state,cache,frame,1,5,2,input,convert));
        assert(calls==2); // Failed conversion did not poison the cache.
        frame.at920=false;input.alphaMode=0;input.constantAlpha=0.25f;
        assert(tglPrepareOwnedVpFillAlpha(state,cache,frame,1,5,0,input,convert));
        std::memcpy(&alpha,state.packet.data()+0xc,4);
        assert(alpha==0.25f && calls==2 && state.packet[0x8a]==0);
    }
    {
        TglOwnedVpFramePreparation frame;
        frame.color.modes.state.at99c=0x19;
        frame.color.metadata.at9a8=0x21;frame.color.metadata.target294=0x21;
        const auto word=[](const auto& bytes,size_t offset) noexcept {
            uint32_t value=0;std::memcpy(&value,bytes.data()+offset,4);return value;
        };
        for(bool iecp:{false,true}) for(bool di:{false,true})
        for(uint32_t format:{0x19u,0xdu,0x15u}) {
            frame.color.modes.state.at99c=format;
            const auto prefix=tglPrepareOwnedVpChromaStatePrefix(frame,0x19,iecp,di);
            const uint32_t input=iecp?4u:di?2u:format==0x19?1u:format==0xd?2u:4u;
            assert(prefix.packet[0]==1 && word(prefix.packet,8)==input);
            assert(prefix.packet[0x6a]==(input==4));
            assert(prefix.sourceSiting==(iecp?0x11u:0x21u));
            // Unimplemented state fields remain zero, never a ready marker.
            assert(word(prefix.packet,0x30)==0 && word(prefix.packet,0x4c)==0);
        }
        TglOwnedVpFramePreparationInput input;
        input.geometry.frameWidth=input.geometry.targetWidth=1920;
        input.geometry.frameHeight=input.geometry.targetHeight=1080;
        input.geometry.sourceRight=input.geometry.destinationRight=1920;
        input.geometry.sourceBottom=input.geometry.destinationBottom=1080;
        input.geometry.targetFormat=0x19;
        input.modes.source130=0x19;input.modes.settings12=true;
        input.modes.settings60Present=true;input.modes.settings60130=1;
        assert(tglPrepareOwnedVpFrame(input,{},frame));
        assert(frame.color.modes.state.at99c==3);
        const auto prefix=tglPrepareOwnedVpChromaStatePrefix(frame,0x19,false,false);
        assert(word(prefix.packet,8)==4 && prefix.packet[0x6a]==1);
        auto state=prefix;
        frame.color.metadata.at9a4=1;
        assert(tglPrepareOwnedVpStateGeometry(state,frame,input.geometry));
        assert(word(state.packet,0x30)==1080 && word(state.packet,0x34)==1920);
        assert(word(state.packet,0x38)==2 && word(state.packet,8)==4);
        input.geometry.fieldInput=true;
        input.geometry.sourceBottom=540;
        input.geometry.sourceRight=960;
        assert(tglPrepareOwnedVpStateGeometry(state,frame,input.geometry));
        assert(word(state.packet,0x30)==1080 && word(state.packet,0x34)==1920);
        assert(word(state.packet,0x3c)==1080 && word(state.packet,0x40)==1920);
        const auto saved=state.packet;
        input.geometry.sourceBottom=std::numeric_limits<float>::infinity();
        assert(!tglPrepareOwnedVpStateGeometry(state,frame,input.geometry));
        assert(state.packet==saved);
        input.geometry.fieldInput=false;
        input.geometry.sourceBottom=1080;input.geometry.sourceRight=1920;
        input.geometry.targetWidth=960;input.geometry.targetHeight=810;
        frame.regions.scale.at928=0.5f;frame.regions.scale.at92c=0.75f;
        frame.regions.scale.scaling=true;
        assert(tglPrepareOwnedVpStatePrefix(frame,input.geometry,false,false,state));
        assert(word(state.packet,0x4c)==810 && word(state.packet,0x50)==960);
        assert(state.packet[0x69]==1 && state.packet[0x66]==1);
        frame.regions.scale.scaling=false;frame.at922=true;frame.color.at9b8=false;
        assert(tglPrepareOwnedVpStatePrefix(frame,input.geometry,false,false,state));
        assert(state.packet[0x69]==0); // IEF does not enable AVS.
        frame.color.at9b8=true;
        assert(tglPrepareOwnedVpStatePrefix(frame,input.geometry,false,false,state));
        assert(state.packet[0x69]==1);
        const auto before=state.packet;
        frame.regions.scale.at928=std::numeric_limits<float>::quiet_NaN();
        assert(!tglPrepareOwnedVpStatePrefix(frame,input.geometry,false,false,state));
        assert(state.packet==before);
    }
    {
        for(unsigned flags=0;flags<16;++flags) for(unsigned failure=0;failure<3;++failure) {
            TglOwnedVpFramePreparation frame;
            frame.regions.scale.scaling=flags&1;frame.color.at9b8=flags&2;
            frame.at922=flags&4;frame.color.modes.state.at923=flags&8;
            unsigned order=0;bool avsCalled=false,iefCalled=false;
            const int result=tglSetupOwnedVpSfcStages(frame,
                [&]() noexcept -> int {assert(order==0);order=1;return failure==1?31:0;},
                [&]() noexcept -> int {assert(order==1);order=2;avsCalled=true;return failure==2?11:0;},
                [&]() noexcept {assert(order==((flags&3)?2u:1u));order=3;iefCalled=true;});
            const bool avsExpected=failure!=1 && (flags&3);
            const bool iefExpected=failure!=1 && !(failure==2 && avsExpected) && (flags&12);
            assert(avsCalled==avsExpected && iefCalled==iefExpected);
            assert(result==(failure==1?31:failure==2 && avsExpected?11:0));
        }
    }
    {
        std::array<uint8_t,0x298> source{},target{};
        std::array<uint8_t,0x1c> mhw{};std::array<uint8_t,0x68> settings{};
        std::array<uint8_t,8> ief{};
        const auto put=[](auto& bytes,size_t offset,auto value) {
            std::memcpy(bytes.data()+offset,&value,sizeof(value));
        };
        const std::array<float,8> rectangles{0,0,1920,1080,0,0,960,540};
        std::memcpy(source.data()+0x30,rectangles.data(),sizeof(rectangles));
        const TglOwnedVpDestinationRect targetRect{0,0,960,540};
        std::memcpy(target.data()+0x40,&targetRect,16);
        put(source,0,uint32_t(3));put(target,0,uint32_t(3));
        put(source,0xd8,uint32_t(1920));put(source,0xdc,uint32_t(1080));
        put(source,0x58,1920.0f);put(source,0x5c,1080.0f);
        put(source,0x130,uint32_t(0x19));put(source,0x120,uint32_t(0x31));
        put(source,0x78,uintptr_t(0x5000));ief[0]=1;put(ief,4,0.5f);
        put(target,0xd8,uint32_t(960));put(target,0xdc,uint32_t(540));put(target,0x130,uint32_t(0x15));
        put(mhw,0x18,uint16_t(16));put(mhw,0x1a,uint16_t(8));
        unsigned calls=0,fail=0;
        const auto read=[&](uintptr_t address,void* output,size_t size) noexcept {
            if(++calls==fail) return false;
            const auto span=[&](uintptr_t base,const auto& bytes) noexcept {
                if(address<base || address-base>bytes.size() || size>bytes.size()-(address-base)) return false;
                std::memcpy(output,bytes.data()+address-base,size);return true;
            };
            return span(0x1000,source) || span(0x2000,target) || span(0x3000,mhw) ||
                   span(0x4000,settings) || span(0x5000,ief);
        };
        TglOwnedVpFramePreparation prepared;uintptr_t reference=0;
        TglOwnedVpRegionInput geometrySnapshot;
        assert(tglReadOwnedVpFramePreparation(0x1000,0x2000,0x3000,0x4000,true,read,{},prepared,reference,&geometrySnapshot));
        const unsigned total=calls;assert(total==24);
        assert(geometrySnapshot.frameWidth && geometrySnapshot.frameHeight);
        assert(prepared.at922 && !prepared.at920 && prepared.regions.scale.at928==0.5f && !reference);
        assert(prepared.color.at9b8 && prepared.color.metadata.at9a4==0x31);
        TglOwnedVpSfcCallerStorage scopedStorage;
        const auto originalStorage=scopedStorage.bytes;
        for(unsigned failedStage=0;failedStage<3;++failedStage) {
            unsigned stages=0;calls=0;
            const auto checkScope=[&](void* child) noexcept {
                assert(child==scopedStorage.bytes.data());
                uintptr_t parameters=0,scopedTarget=0;
                std::memcpy(&parameters,scopedStorage.bytes.data()+0x990,8);
                std::memcpy(&scopedTarget,scopedStorage.bytes.data()+0x9b0,8);
                assert(parameters==0x7000 && scopedTarget==0x2000);
            };
            const int result=tglSetupOwnedVpLeasedFrame(scopedStorage,
                0x1000,0x2000,0x3000,0x4000,0x6000,0x7000,{},read,
                [&](void* child,const auto& frame,const auto& geometry) noexcept -> int {
                    checkScope(child);assert(++stages==1);
                    assert(frame.at922 && geometry.frameWidth==geometrySnapshot.frameWidth);
                    return failedStage==1?31:0;
                },
                [&](void* child,const auto& frame) noexcept -> int {
                    checkScope(child);assert(++stages==2 && frame.regions.scale.scaling);
                    return failedStage==2?11:0;
                },
                [&](void* child,const auto& frame) noexcept {
                    checkScope(child);assert(++stages==3 && frame.at922);
                });
            assert(result==(failedStage==1?31:failedStage==2?11:0));
            assert(stages==(failedStage?failedStage:3) && calls==total);
            assert(scopedStorage.bytes==originalStorage);
            stages=0;calls=0;unsigned sent=0;
            const int passResult=tglWithOwnedVpLeasedPass(scopedStorage,
                0x1000,0x2000,0x3000,0x4000,0x6000,0x7000,{},read,
                [&](void* child,const auto&,const auto&) noexcept -> int {
                    checkScope(child);++stages;return failedStage==1?31:0;
                },
                [&](void* child,const auto&) noexcept -> int {
                    checkScope(child);++stages;return failedStage==2?11:0;
                },
                [&](void* child,const auto&) noexcept {checkScope(child);++stages;},
                [&](void* child,const auto& frame,const auto& geometry) noexcept -> int {
                    checkScope(child);assert(stages==3 && !sent);
                    assert(scopedStorage.bytes[0x921]==1 && scopedStorage.bytes[0x922]==1);
                    assert(frame.at922 && geometry.frameWidth==geometrySnapshot.frameWidth);
                    ++sent;return -17;
                });
            assert(passResult==(failedStage==1?31:failedStage==2?11:-17));
            assert(sent==(failedStage?0:1) && calls==total);
            assert(scopedStorage.bytes==originalStorage);
        }
        {
            struct Cpu {
                alignas(8) std::array<uint8_t,0xc00> bytes{};
                bool live=false;
                std::array<unsigned,2>* releaseOrder=nullptr;
                unsigned* releaseCount=nullptr;
                unsigned releaseId=0;
                uintptr_t allocate(size_t n) {
                    assert(!live && (n==0xf8 || n==0xc00));live=true;
                    return reinterpret_cast<uintptr_t>(bytes.data());
                }
                void release(uintptr_t p) noexcept {
                    assert(live && p==reinterpret_cast<uintptr_t>(bytes.data()));live=false;
                    if(releaseOrder) {
                        assert(releaseCount && *releaseCount<releaseOrder->size());
                        (*releaseOrder)[(*releaseCount)++]=releaseId;
                    }
                }
            } stateCpu,avsCpu;
            std::array<unsigned,2> releaseOrder{};unsigned releaseCount=0;
            stateCpu.releaseOrder=avsCpu.releaseOrder=&releaseOrder;
            stateCpu.releaseCount=avsCpu.releaseCount=&releaseCount;
            stateCpu.releaseId=2;avsCpu.releaseId=1;
            struct Binding {
                uintptr_t image=0x100000;
                bool validateConstructedObject(uintptr_t,uintptr_t,
                    bool (*)(uintptr_t,void*,size_t)) const noexcept {return true;}
            } binding;
            auto noRead=+[](uintptr_t,void*,size_t) noexcept {return false;};
            TglOwnedVpSfcCpuGraph<Cpu,Cpu> graph(stateCpu,avsCpu);
            assert(graph.initialize(0x7000,0x8000,0x3000,binding,noRead,1,0));
            auto never=[](void*) noexcept {assert(false);return 0;};
            using PlatformEntry=int(*)(void*,void*,void*,void*,void*,void*) noexcept;
            PlatformEntry escaped=nullptr;void* escapedChild=nullptr;
            for(unsigned failure=0;failure<3;++failure) {
                unsigned stages=0,sends=0;
                assert(graph.withPreparedSendPass(prepared,0x6000,0x4000,0x2000,reference,
                    [&](void*,const auto&,auto& stateOwner) noexcept -> int {
                        assert(++stages==1 && stateOwner.data());
                        graph.reset();assert(graph.withView(true,never)==5);
                        return failure==1?31:0;
                    },
                    [&](void*,const auto&,auto& avsOwner) noexcept -> int {
                        assert(++stages==2 && avsOwner.parameters());return failure==2?11:0;
                    },
                    [&](void*,const auto&) noexcept {assert(++stages==3);},
                    [&](void* child,const auto& frame,PlatformEntry platform) noexcept -> int {
                        escaped=platform;escapedChild=child;
                        assert(platform(child,nullptr,nullptr,nullptr,nullptr,nullptr)==0);
                        ++sends;assert(stages==3 && frame.regions.scale.scaling);
                        const auto* p=static_cast<const uint8_t*>(child);
                        uintptr_t stateAddress=0,targetAddress=0,coefficients=0;
                        std::memcpy(&stateAddress,p+0x938,8);
                        std::memcpy(&targetAddress,p+0x9b0,8);
                        std::memcpy(&coefficients,p+0x38,8);
                        assert(stateAddress==reinterpret_cast<uintptr_t>(stateCpu.bytes.data()) &&
                            coefficients==reinterpret_cast<uintptr_t>(avsCpu.bytes.data()) && targetAddress==0x2000);
                        graph.reset();assert(stateCpu.live && avsCpu.live);
                        return -17;
                    })==(failure==1?31:failure==2?11:-17));
                assert(sends==(failure?0:1));
                assert(escaped && escaped(escapedChild,nullptr,nullptr,nullptr,nullptr,nullptr)==5);
                assert(graph.prepare([](auto& storage,auto&,auto&) noexcept {
                    for(size_t off:{size_t(0x938),size_t(0x990),size_t(0x9b0)}) {
                        uintptr_t p=1;std::memcpy(&p,storage.bytes.data()+off,8);assert(!p);
                    }
                    return 0;
                })==0);
            }
            using SetupEntry=int(*)(void*,void*,void*,void*) noexcept;
            using SendEntry=int(*)(void*,void*,void*) noexcept;
            SetupEntry escapedSetup=nullptr;SendEntry escapedSend=nullptr;
            void* callbackChild=nullptr;int source=0,data=0,command=0;
            for(unsigned mode=0;mode<10;++mode) {
                unsigned stages=0,emissions=0;
                const int result=graph.withPreparedCallbackPass(prepared,
                    0x6000,0x4000,0x2000,reference,&source,&data,
                    [&](void*,const auto&,auto&) noexcept -> int {++stages;return mode==9?11:0;},
                    [&](void*,const auto&,auto&) noexcept -> int {++stages;return 0;},
                    [&](void*,const auto&) noexcept {++stages;},
                    [&](void* child,void* d,void* c) noexcept -> int {
                        assert(stages==3 && d==&data && c==&command);++emissions;
                        uintptr_t stateAddress=0;
                        std::memcpy(&stateAddress,static_cast<uint8_t*>(child)+0x938,8);
                        assert(stateAddress==reinterpret_cast<uintptr_t>(stateCpu.bytes.data()));
                        graph.reset();assert(stateCpu.live && avsCpu.live);
                        return mode==6?-17:0;
                    },
                    [&](void* child,SetupEntry setup,SendEntry send) noexcept -> int {
                        escapedSetup=setup;escapedSend=send;callbackChild=child;
                        assert(stages==0); // Native setup is the producer boundary.
                        if(mode==1) {assert(send(child,&data,&command)==5);return 0;}
                        if(mode==2) {assert(setup(child,&command,reinterpret_cast<void*>(0x2000),&data)==5);return 0;}
                        if(mode==9) {
                            assert(setup(child,&source,reinterpret_cast<void*>(0x2000),&data)==11);
                            assert(send(child,&data,&command)==11);
                            assert(stages==1);return 0;
                        }
                        assert(setup(child,&source,reinterpret_cast<void*>(0x2000),&data)==0);
                        assert(stages==3);
                        if(mode==3) return 0;
                        if(mode==4) {assert(setup(child,&source,reinterpret_cast<void*>(0x2000),&data)==5);return 0;}
                        const int sent=send(child,&data,&command);
                        assert(sent==(mode==6?-17:0));
                        if(mode==5) return 31;
                        if(mode==7) {assert(send(child,&data,&command)==5);return 0;}
                        if(mode==8) {assert(setup(child,&source,reinterpret_cast<void*>(0x2000),&data)==5);return 0;}
                        return 0;
                    });
                assert(result==(mode==0?0:mode==5?31:mode==6?-17:mode==9?11:5));
                assert(emissions==(mode==0 || (mode>=5 && mode<=8)?1u:0u));
                assert(stages==((mode==1 || mode==2)?0u:mode==9?1u:3u));
                assert(escapedSetup(callbackChild,&source,reinterpret_cast<void*>(0x2000),&data)==5);
                assert(escapedSend(callbackChild,&data,&command)==5);
            }
            // Acquire the native-shaped descriptors through +28 first; all
            // later callbacks use that single frozen geometry snapshot.
            for(unsigned mode=0;mode<4;++mode) {
                calls=0;fail=mode==1?1:0;
                unsigned produced=0,emitted=0,invoked=0;
                const int result=graph.withLeasedCallbackPass(0x1000,0x2000,0x3000,
                    0x4000,0x6000,0x7000,{},&data,
                    [&](uintptr_t address,void* out,size_t size) noexcept {
                        assert(!graph.tryReset());
                        graph.reset();assert(stateCpu.live && avsCpu.live);
                        assert(graph.prepare([](auto&,auto&,auto&) noexcept {return 0;})==5);
                        return read(address,out,size);
                    },
                    [&](void*,const auto&,const auto& snapshot,auto&) noexcept -> int {
                        ++produced;assert(snapshot.frameWidth==geometrySnapshot.frameWidth);
                        return mode==2?31:0;
                    },
                    [&](void*,const auto&,auto&) noexcept -> int {++produced;return 0;},
                    [&](void*,const auto&) noexcept {++produced;},
                    [&](void* child,void* d,void* c) noexcept -> int {
                        ++emitted;assert(d==&data && c==&command);
                        uintptr_t parameters=0;
                        std::memcpy(&parameters,static_cast<uint8_t*>(child)+0x990,8);
                        assert(parameters==0x7000 && produced==3);
                        return mode==3?-17:0;
                    },
                    [&](void* child,SetupEntry setup,SendEntry send) noexcept -> int {
                        ++invoked;assert(!produced && !emitted);
                        const int status=setup(child,reinterpret_cast<void*>(0x1000),
                            reinterpret_cast<void*>(0x2000),&data);
                        if(status) return status;
                        return send(child,&data,&command);
                    });
                assert(result==(mode==1?5:mode==2?31:mode==3?-17:0));
                assert(invoked==(mode==1?0u:1u));
                assert(produced==(mode==1?0u:mode==2?1u:3u));
                assert(emitted==((mode==0 || mode==3)?1u:0u));
            }
            fail=0;
            // Exercise the VP callback bridge with the real typed scheduler,
            // not the platform no-op. This records CPU emission only.
            struct CallbackCommandBackend {
                unsigned count=0;int failure=0;
                std::array<int,8> order{};
                int stage(int id) noexcept {order[count++]=id;return failure==id?-id:0;}
                int lock(const TglOwnedSfcLockParameters&) noexcept {return stage(1);}
                int state(const TglOwnedSfcStateParametersPacket&,
                    const std::array<uint8_t,0x38>&) noexcept {return stage(4);}
                int avs(const std::array<uint8_t,16>&) noexcept {return stage(5);}
                int y(const TglOwnedSfcYParameters&) noexcept {return stage(6);}
                int uv(const TglOwnedSfcUvParameters&) noexcept {return stage(7);}
                int ief(const std::array<uint8_t,64>&) noexcept {return stage(8);}
                int frameStart() noexcept {return stage(9);}
            };
            for(int failure:{0,1,3,4,5,6,7,8,9}) {
                CallbackCommandBackend backend;backend.failure=failure;
                TglOwnedSfcCommandParameters packets{};
                std::array<uint8_t,0x2a8> output{};
                unsigned stages=0;
                const int result=graph.withPreparedCallbackPass(prepared,
                    0x6000,0x4000,0x2000,reference,&source,&data,
                    [&](void*,const auto&,auto&) noexcept -> int {++stages;return 0;},
                    [&](void*,const auto&,auto&) noexcept -> int {++stages;return 0;},
                    [&](void*,const auto&) noexcept {++stages;},
                    [&](void*,void*,void*) noexcept -> int {
                        assert(stages==3 && stateCpu.live && avsCpu.live);
                        auto platform=[&](auto&) noexcept {return backend.stage(3);};
                        return tglSendOwnedSfcPreparedCommands(backend,packets,output,
                            1,1,1,1,platform);
                    },
                    [&](void* child,SetupEntry setup,SendEntry send) noexcept -> int {
                        assert(setup(child,&source,reinterpret_cast<void*>(0x2000),&data)==0);
                        // Even a caller that ignores the send error cannot turn
                        // failed/partial command construction into success.
                        (void)send(child,&data,&command);return 0;
                    });
                assert(result==(failure?-failure:0));
                unsigned expectedCount=0;
                for(int id:{1,3,4,5,6,7,8,9}) {
                    assert(backend.order[expectedCount++]==id);
                    if(id==failure) break;
                }
                assert(backend.count==expectedCount);
            }
            auto disabled=prepared;
            disabled.regions.scale.scaling=false;disabled.color.at9b8=false;
            disabled.at922=false;disabled.color.modes.state.at923=false;
            unsigned sent=0;
            assert(graph.withPreparedPass(disabled,0x6000,0x4000,0x2000,reference,
                [](void*,const auto&,auto&) noexcept -> int {return 0;},
                [](void*,const auto&,auto&) noexcept -> int {assert(false);return 0;},
                [](void*,const auto&) noexcept {assert(false);},
                [&](void* child,const auto&) noexcept -> int {
                    uintptr_t coefficients=1;
                    std::memcpy(&coefficients,static_cast<uint8_t*>(child)+0x38,8);
                    assert(!coefficients);++sent;return 89;
                })==89 && sent==1);
            assert(graph.tryReset());
            assert(!stateCpu.live && !avsCpu.live && releaseCount==2);
            assert((releaseOrder==std::array<unsigned,2>{1,2}));
            assert(graph.tryReset() && releaseCount==2); // Idempotent teardown.
        }
        for(fail=1;fail<=total;++fail) {
            calls=0;prepared.at9a0=17;reference=19;
            geometrySnapshot.frameWidth=23;
            assert(!tglReadOwnedVpFramePreparation(0x1000,0x2000,0x3000,0x4000,true,read,{},prepared,reference,&geometrySnapshot));
            assert(prepared.at9a0==17 && reference==19 && geometrySnapshot.frameWidth==23);
            calls=0;unsigned producers=0;
            assert(tglSetupOwnedVpLeasedFrame(scopedStorage,
                0x1000,0x2000,0x3000,0x4000,0x6000,0x7000,{},read,
                [&](void*,const auto&,const auto&) noexcept -> int { ++producers;return 0; },
                [&](void*,const auto&) noexcept -> int { ++producers;return 0; },
                [&](void*,const auto&) noexcept { ++producers; })==5);
            assert(!producers && scopedStorage.bytes==originalStorage);
        }
        for(bool missingSource:{false,true}) {
            calls=0;unsigned producers=0;
            assert(tglSetupOwnedVpLeasedFrame(scopedStorage,
                missingSource?0:0x1000,missingSource?0x2000:0,
                0x3000,0x4000,0x6000,0x7000,{},read,
                [&](void*,const auto&,const auto&) noexcept -> int {++producers;return 0;},
                [&](void*,const auto&) noexcept -> int {++producers;return 0;},
                [&](void*,const auto&) noexcept {++producers;})==5);
            assert(!calls && !producers && scopedStorage.bytes==originalStorage);
        }
        fail=0;calls=0;ief[0]=0;
        assert(tglReadOwnedVpFramePreparation(0x1000,0x2000,0x3000,0x4000,false,read,{},prepared,reference));
        assert(calls==total-2 && !prepared.at922 && !prepared.at920);
        put(source,0x78,uintptr_t(0));calls=0;
        assert(tglReadOwnedVpFramePreparation(0x1000,0x2000,0x3000,0x4000,false,read,{},prepared,reference));
        assert(calls==total-3 && !prepared.at922);
    }
    {
        std::array<uint8_t,0x134> source{},selected{};
        std::array<uint8_t,4> target{};std::array<uint8_t,0x68> settings{};
        const auto put=[](auto& bytes,size_t offset,auto value) {
            std::memcpy(bytes.data()+offset,&value,sizeof(value));
        };
        put(source,0,uint32_t(1));put(source,0x130,uint32_t(0x19));
        put(source,0xd8,uint32_t(1920));put(source,0xdc,uint32_t(1080));
        put(source,0xa8,uintptr_t(0x5000));settings[0x12]=1;
        put(settings,0x60,uintptr_t(0x4000));put(selected,0x130,uint32_t(1));
        unsigned calls=0,fail=0;
        const auto read=[&](uintptr_t address,void* output,size_t size) noexcept {
            if(++calls==fail) return false;
            const auto span=[&](uintptr_t base,const auto& bytes) noexcept {
                if(address<base || address-base>bytes.size() || size>bytes.size()-(address-base)) return false;
                std::memcpy(output,bytes.data()+address-base,size);return true;
            };
            return span(0x1000,source) || span(0x2000,target) || span(0x3000,settings) || span(0x4000,selected);
        };
        TglOwnedVpModeInput snapshot;TglOwnedVpModeResult result;uintptr_t reference=0;
        assert(tglReadOwnedVpModes(0x1000,0x2000,0x3000,read,{},snapshot,result,reference) && calls==9);
        assert(result.state.at998==5 && result.state.at99c==3 && result.state.at923 &&
               result.borrowSourceA8 && reference==0x5000 && snapshot.sourceA8Present);
        {
            TglOwnedVpSfcCallerStorage storage;
            storage.bytes[0x998]=17;storage.bytes[0x99c]=19;storage.bytes[0x923]=0xa5;
            const auto saved=storage.bytes;
            using Context=TglOwnedVpModeContext<decltype(read)>;
            using Callback=TglOwnedVpModeCallbacks<TglOwnedVpSfcResolve<Context>>;
            for(unsigned scenario=0;scenario<5;++scenario) {
                calls=0;
                {
                    Context context(storage,0x1000,0x2000,0x3000,read);
                    TglOwnedVpSfcScopedContext<Context> scope(storage.bytes.data(),context);
                    assert(scope.bound() && context.status()==5 && calls==9);
                    if(!scenario) {put(source,0,uint32_t(3));put(source,0x130,uint32_t(1));}
                    if(scenario==1) context.formatMode(reinterpret_cast<void*>(0x1000),reinterpret_cast<void*>(0x3000));
                    else {
                        Callback::sampleMode(storage.bytes.data(),reinterpret_cast<void*>(0x1000),
                            reinterpret_cast<void*>(scenario==2?0x6000:0x2000));
                        if(scenario==3) context.sampleMode(reinterpret_cast<void*>(0x1000),reinterpret_cast<void*>(0x2000));
                        if(scenario!=4) Callback::formatMode(storage.bytes.data(),reinterpret_cast<void*>(0x1000),reinterpret_cast<void*>(0x3000));
                    }
                    assert(context.status()==(scenario==0?0:5) && calls==9);
                    if(!scenario) {
                        uint32_t mode=0,format=0;uintptr_t ref=0;
                        std::memcpy(&mode,storage.bytes.data()+0x998,4);
                        std::memcpy(&format,storage.bytes.data()+0x99c,4);
                        std::memcpy(&ref,storage.bytes.data()+0x9c0,8);
                        assert(mode==5 && format==3 && ref==0x5000 && storage.bytes[0x923]==1);
                    }
                }
                assert(storage.bytes==saved);
                put(source,0,uint32_t(1));put(source,0x130,uint32_t(0x19));
            }
            for(fail=1;fail<=9;++fail) {
                calls=0;
                Context context(storage,0x1000,0x2000,0x3000,read);
                assert(!context.owns(storage.bytes.data()) && context.status()==5 && storage.bytes==saved);
                context.sampleMode(reinterpret_cast<void*>(0x1000),reinterpret_cast<void*>(0x2000));
                assert(storage.bytes==saved);
            }
            fail=0;const uintptr_t borrowed=0x6000;
            std::memcpy(storage.bytes.data()+0x9c0,&borrowed,8);
            const auto occupied=storage.bytes;
            {Context context(storage,0x1000,0x2000,0x3000,read);
                assert(!context.owns(storage.bytes.data()) && context.status()==5);}
            assert(storage.bytes==occupied);
        }
        for(fail=1;fail<=9;++fail) {
            calls=0;snapshot.source00=17;result.state.at998=19;reference=23;
            assert(!tglReadOwnedVpModes(0x1000,0x2000,0x3000,read,{},snapshot,result,reference));
            assert(snapshot.source00==17 && result.state.at998==19 && reference==23);
        }
        fail=0;calls=0;settings[0x13]=1;put(settings,0x60,uintptr_t(0));
        assert(tglReadOwnedVpModes(0x1000,0x2000,0x3000,read,{},snapshot,result,reference) && calls==7);
        assert(result.state.at99c==0xd);
        calls=0;settings[0x13]=0;put(source,0x130,uint32_t(1));
        assert(tglReadOwnedVpModes(0x1000,0x2000,0x3000,read,{},snapshot,result,reference) && calls==7);
        assert(result.state.at99c==0x15);
        put(source,0,uint32_t(3));put(target,0,uint32_t(3));calls=0;
        assert(tglReadOwnedVpModes(0x1000,0x2000,0x3000,read,{},snapshot,result,reference) && calls==4);
        assert(!result.borrowSourceA8 && !reference);
        calls=0;
        assert(!tglReadOwnedVpModes(0x1000,0x2000,UINTPTR_MAX-8,read,{},snapshot,result,reference) && !calls);
    }
    {
        std::array<uint8_t,0x294> source{};std::array<uint8_t,0x134> target{};
        std::array<uint8_t,0x1c> mhw{};
        const auto put=[](auto& data,size_t offset,auto value) {
            std::memcpy(data.data()+offset,&value,sizeof(value));
        };
        const std::array<float,8> rectangles{0,0,1920,1080,0,0,960,540};
        std::memcpy(source.data()+0x30,rectangles.data(),sizeof(rectangles));
        put(source,0xd8,uint32_t(1920));put(source,0xdc,uint32_t(1080));
        put(source,0x58,1920.0f);put(source,0x5c,1080.0f);
        put(target,0xd8,uint32_t(960));put(target,0xdc,uint32_t(540));
        put(target,0x130,uint32_t(0x19));
        put(mhw,0x18,uint16_t(16));put(mhw,0x1a,uint16_t(8));
        unsigned calls=0,fail=0;
        const auto read=[&](uintptr_t address,void* output,size_t size) noexcept {
            if(++calls==fail) return false;
            const auto span=[&](uintptr_t base,const auto& bytes) noexcept {
                if(address<base || address-base>bytes.size() || size>bytes.size()-(address-base)) return false;
                std::memcpy(output,bytes.data()+address-base,size);return true;
            };
            return span(0x1000,source) || span(0x2000,target) || span(0x3000,mhw);
        };
        TglOwnedVpRegionInput snapshot;TglOwnedVpRegions regions;
        assert(tglReadOwnedVpRegions(0x1000,0x2000,0x3000,read,snapshot,regions) && calls==13);
        assert(snapshot.frameWidth==1920 && snapshot.frameHeight==1080);
        assert(regions.sourceWidth==1920 && regions.sourceHeight==1080 &&
               regions.destinationWidth==960 && regions.destinationHeight==540 &&
               regions.scale.at928==0.5f && regions.scale.at92c==0.5f);
        for(fail=1;fail<=13;++fail) {
            calls=0;snapshot.frameWidth=17;regions.sourceWidth=19;
            assert(!tglReadOwnedVpRegions(0x1000,0x2000,0x3000,read,snapshot,regions));
            assert(snapshot.frameWidth==17 && regions.sourceWidth==19);
        }
        fail=0;calls=0;
        assert(!tglReadOwnedVpRegions(UINTPTR_MAX-8,0x2000,0x3000,read,snapshot,regions) && !calls);
        assert(!tglReadOwnedVpRegions(0x1000,UINTPTR_MAX-8,0x3000,read,snapshot,regions) && !calls);
    }
    {
        TglOwnedVpSfcCallerStorage storage;
        storage.bytes[0x925]=0x5a;storage.bytes[0x9b9]=0xa5;
        const auto original=storage.bytes;
        TglOwnedVpFramePreparation frame;
        frame.at920=frame.at922=frame.at924=true;frame.at9a0=3;
        frame.regions.scale={2.0f,3.0f,true};
        frame.color={{{5,3,true},true},{1,0x11,0x21},false};
        unsigned calls=0;
        const auto use=[&](void* child,const TglOwnedVpColorMetadata& metadata) noexcept {
            ++calls;assert(child==storage.bytes.data() && metadata.target294==0x21);
            assert(storage.bytes[0x920]==1 && storage.bytes[0x921]==1 &&
                   storage.bytes[0x922]==1 && storage.bytes[0x924]==1);
            float x=0,y=0;uint32_t rotation=0;uintptr_t reference=0;
            std::memcpy(&x,storage.bytes.data()+0x928,4);
            std::memcpy(&y,storage.bytes.data()+0x92c,4);
            std::memcpy(&rotation,storage.bytes.data()+0x9a0,4);
            std::memcpy(&reference,storage.bytes.data()+0x9c0,8);
            assert(x==2 && y==3 && rotation==3 && reference==0x7000);
            unsigned stages=0;
            assert(tglSetupOwnedVpSfcStages(frame,
                [&]() noexcept -> int {assert(storage.bytes[0x923]==1);++stages;return 0;},
                [&]() noexcept -> int {assert(storage.bytes[0x921]==1);++stages;return 0;},
                [&]() noexcept {assert(storage.bytes[0x922]==1);++stages;})==0 && stages==3);
            const auto live=storage.bytes;
            assert(tglWithOwnedVpPreparedFrame(storage,frame,0x1000,0x2000,0x3000,0x7000,
                [](void*,const auto&) noexcept {assert(false);return 0;})==5);
            assert(storage.bytes==live);return 31;
        };
        assert(tglWithOwnedVpPreparedFrame(storage,frame,0x1000,0x2000,0x3000,0x7000,use)==31);
        assert(calls==1 && storage.bytes==original);
        assert(tglWithOwnedVpPreparedFrame(storage,frame,0,0x2000,0x3000,0x7000,use)==5);
        assert(tglWithOwnedVpPreparedFrame(storage,frame,0x1000,0x2000,0,0x7000,use)==5);
        assert(tglWithOwnedVpPreparedFrame(storage,frame,0x1000,0x2000,0x3000,0,use)==5);
        assert(calls==1 && storage.bytes==original);
    }
    {
        TglOwnedVpFramePreparationInput input;
        auto& geometry=input.geometry;
        geometry.frameWidth=geometry.targetWidth=1920;
        geometry.frameHeight=geometry.targetHeight=1080;
        geometry.sourceRight=geometry.destinationRight=1920;
        geometry.sourceBottom=geometry.destinationBottom=1080;
        geometry.targetFormat=0x15;input.modes.source130=0x19;
        input.targetDestination={0,0,1920,1080};input.conditionalSourceAvailable=true;
        input.iefPresent=true;input.iefFlags=1;input.iefStrength=0.5f;
        TglOwnedVpFramePreparation prepared;
        assert(tglPrepareOwnedVpFrame(input,{},prepared));
        assert(!prepared.at920 && prepared.at922 && !prepared.at924 && !prepared.regions.scale.scaling);
        assert(prepared.color.at9b8 && prepared.color.metadata.at9a8==0x21);
        input.targetDestination.right=1921;input.modes.settings12=true;
        input.modes.settings60Present=true;input.modes.settings60130=1;
        assert(tglPrepareOwnedVpFrame(input,{},prepared));
        assert(prepared.at920 && prepared.at924 && !prepared.color.at9b8);
        const auto retained=prepared;input.modes.settings60Present=false;
        assert(!tglPrepareOwnedVpFrame(input,{},prepared));
        assert(prepared.at920==retained.at920 && prepared.at924==retained.at924 &&
               prepared.color.modes.state.at99c==retained.color.modes.state.at99c);
    }
    {
        const TglOwnedVpDestinationRect source{0,0,100,80};
        assert(!tglOwnedVpConditionalSource(true,source,source));
        assert(!tglOwnedVpConditionalSource(true,source,{1,1,99,79}));
        for(auto field:{&TglOwnedVpDestinationRect::left,&TglOwnedVpDestinationRect::top,
                        &TglOwnedVpDestinationRect::right,&TglOwnedVpDestinationRect::bottom}) {
            auto target=source;
            target.*field+=(field==&TglOwnedVpDestinationRect::left ||
                            field==&TglOwnedVpDestinationRect::top)?-1:1;
            assert(tglOwnedVpConditionalSource(true,source,target));
            assert(!tglOwnedVpConditionalSource(false,source,target));
            target=source;target.*field=std::numeric_limits<float>::quiet_NaN();
            assert(tglOwnedVpConditionalSource(true,source,target));
            assert(tglOwnedVpConditionalSource(true,target,source));
        }
        unsigned calls=0,fail=0;
        const auto read=[&](uintptr_t address,void* output,size_t size) noexcept {
            ++calls;if(calls==fail || size!=16 || (address!=0x1040 && address!=0x2040)) return false;
            std::memcpy(output,&source,16);return true;
        };
        bool output=true;
        assert(tglReadOwnedVpConditionalSource(false,0,0,read,output) && !output && !calls);
        assert(tglReadOwnedVpConditionalSource(true,0x1000,0x2000,read,output) && !output && calls==2);
        for(fail=1;fail<=2;++fail) for(bool original:{false,true}) {
            calls=0;output=original;
            assert(!tglReadOwnedVpConditionalSource(true,0x1000,0x2000,read,output));
            assert(output==original);
        }
        calls=0;output=true;
        assert(!tglReadOwnedVpConditionalSource(true,UINTPTR_MAX-8,0x2000,read,output));
        assert(output && !calls);
    }
    {
        TglOwnedVpRegionInput input;
        input.frameWidth=input.frameHeight=input.targetWidth=input.targetHeight=128;
        input.sourceRight=65.9f;input.sourceBottom=33.9f;
        input.sourceLeft=input.sourceTop=0.5f;
        input.destinationRight=63.9f;input.destinationBottom=31.9f;
        input.targetFormat=0x19;
        TglOwnedVpRegions regions;
        assert(tglPrepareOwnedVpRegions(input,regions));
        assert(regions.sourceWidth==64 && regions.sourceHeight==32);
        assert(regions.destinationWidth==64 && regions.destinationHeight==32 && !regions.scale.scaling);
        input.targetFormat=0xd;
        assert(tglPrepareOwnedVpRegions(input,regions));
        assert(regions.destinationWidth==64 && regions.destinationHeight==31);
        input.fieldInput=true;input.targetFormat=0x19;
        assert(tglPrepareOwnedVpRegions(input,regions));
        assert(regions.sourceWidth==128 && regions.sourceHeight==64);
        input.targetWidth=61;input.targetHeight=29;
        assert(tglPrepareOwnedVpRegions(input,regions));
        assert(regions.destinationWidth==62 && regions.destinationHeight==30);
        const auto valid=input;const auto retained=regions;
        for(auto field:{&TglOwnedVpRegionInput::sourceLeft,&TglOwnedVpRegionInput::sourceTop,
                        &TglOwnedVpRegionInput::sourceRight,&TglOwnedVpRegionInput::sourceBottom,
                        &TglOwnedVpRegionInput::destinationRight,&TglOwnedVpRegionInput::destinationBottom}) {
            input=valid;input.*field=std::numeric_limits<float>::quiet_NaN();
            assert(!tglPrepareOwnedVpRegions(input,regions));
            assert(regions.sourceWidth==retained.sourceWidth && regions.scale.at928==retained.scale.at928);
        }
        input=valid;input.sourceLeft=256;assert(!tglPrepareOwnedVpRegions(input,regions));
        input=valid;input.sourceRight=2147483648.0f;assert(!tglPrepareOwnedVpRegions(input,regions));
        input=valid;input.frameWidth=0;assert(!tglPrepareOwnedVpRegions(input,regions));
    }
    {
        TglOwnedVpSfcCallerStorage storage;
        storage.bytes.fill(0xa5);
        const uintptr_t zero=0;std::memcpy(storage.bytes.data()+0x9c0,&zero,8);
        const auto original=storage.bytes;
        TglOwnedVpFrameColor frame{{{5,0x15,true},true},{1,0x11,0x21},true};
        unsigned calls=0;
        assert(tglWithOwnedVpFrameColor(storage,frame,0x1000,[&](void* child) noexcept {
            ++calls;assert(child==storage.bytes.data());
            const auto word=[&](size_t offset) noexcept {
                uint32_t value=0;std::memcpy(&value,storage.bytes.data()+offset,4);return value;
            };
            uintptr_t reference=0;std::memcpy(&reference,storage.bytes.data()+0x9c0,8);
            assert(word(0x998)==5 && word(0x99c)==0x15 && storage.bytes[0x923]==1);
            assert(word(0x9a4)==1 && word(0x9a8)==0x11 && storage.bytes[0x9b8]==1);
            assert(reference==0x1000);
            assert(tglWithOwnedVpFrameColor(storage,frame,0x2000,
                [](void*) noexcept {assert(false);return 0;})==5);
            return 31;
        })==31);
        assert(calls==1 && storage.bytes==original);
        assert(tglWithOwnedVpFrameColor(storage,frame,0,
            [](void*) noexcept {assert(false);return 0;})==5);
        const uintptr_t foreign=0x3000;std::memcpy(storage.bytes.data()+0x9c0,&foreign,8);
        const auto occupied=storage.bytes;
        assert(tglWithOwnedVpFrameColor(storage,frame,0x1000,
            [](void*) noexcept {assert(false);return 0;})==5 && storage.bytes==occupied);
        frame.modes.borrowSourceA8=false;
        assert(tglWithOwnedVpFrameColor(storage,frame,0,[&](void*) noexcept {
            uintptr_t reference=0;std::memcpy(&reference,storage.bytes.data()+0x9c0,8);
            assert(reference==foreign);return 0;
        })==0 && storage.bytes==occupied);
        storage.bytes.fill(0);frame.modes.borrowSourceA8=true;
        const auto empty=storage.bytes;
        assert(tglWithOwnedVpFrameReferences(storage,0x4000,0x5000,0x6000,true,
            [&](void*) noexcept {
                const auto outer=storage.bytes;
                const int status=tglWithOwnedVpFrameColor(storage,frame,0x1000,
                    [&](void*) noexcept {
                        uintptr_t parameters=0;
                        std::memcpy(&parameters,storage.bytes.data()+0x990,8);
                        assert(parameters==0x5000 && storage.bytes[0x923]==1);
                        return 31;
                    });
                assert(storage.bytes==outer);return status;
            })==31 && storage.bytes==empty);
    }
    {
        TglOwnedVpModeInput input;input.source00=input.target00=3;
        input.source130=0x19;input.settings12=true;input.settings60Present=true;
        input.settings60130=1;
        TglOwnedVpFrameColor frame;
        assert(tglPrepareOwnedVpFrameColor(input,{},0x31,0,0x15,0,frame));
        assert(frame.modes.state.at99c==3 && frame.modes.state.at923);
        assert(frame.metadata.at9a4==1 && frame.metadata.at9a8==0x11);
        assert(frame.metadata.target294==0x11 && !frame.at9b8);
        input.settings13=true;
        assert(tglPrepareOwnedVpFrameColor(input,{},0x31,0,0x15,0,frame));
        assert(frame.modes.state.at99c==0xd && frame.metadata.at9a4==0x31 && frame.at9b8);
        // Effective 422 metadata does not overwrite original 420 upsample input.
        assert(frame.metadata.at9a8==0x11);
        input.settings12=false;input.settings13=false;
        assert(tglPrepareOwnedVpFrameColor(input,{},0x31,0,0x15,0,frame));
        assert(frame.modes.state.at99c==0x19 && frame.metadata.at9a8==0x21 && frame.at9b8);
        const auto retained=frame;
        input.settings12=true;input.settings60Present=false;
        assert(!tglPrepareOwnedVpFrameColor(input,{},0,0,0,0,frame));
        assert(frame.modes.state.at99c==retained.modes.state.at99c &&
               frame.metadata.at9a8==retained.metadata.at9a8 && frame.at9b8==retained.at9b8);
    }
    {
        for(uint32_t source:{0u,1u,2u,3u}) for(uint32_t target=0;target<12;++target)
        for(bool large:{false,true}) for(bool setting:{false,true}) {
            TglOwnedVpModeInput input;
            input.source00=source;input.target00=target;input.sourceD8=large?769:768;
            input.sourceDc=576;input.source130=0x19;input.settings12=setting;
            input.settings60Present=true;input.settings60130=0x19;input.sourceA8Present=true;
            TglOwnedVpModeResult result;
            assert(tglPrepareOwnedVpModes(input,{},result));
            TglOwnedVpModeResult sample,split;
            tglPrepareOwnedVpSampleMode(input,{},sample);
            assert(tglPrepareOwnedVpFormatMode(input,sample,split));
            assert(split.state.at998==result.state.at998 && split.state.at99c==result.state.at99c &&
                split.state.at923==result.state.at923 && split.borrowSourceA8==result.borrowSourceA8);
            const uint32_t color=(source==1 || source==2)?
                ((target>=3 && target<=10)?target:(large?5:3)):(setting?target:source);
            assert(result.state.at998==color && result.state.at99c==0x19);
            assert(result.state.at923==(color!=target) && result.borrowSourceA8==(color!=target));
        }
        TglOwnedVpModeInput input;input.source00=input.target00=3;input.source130=0x19;
        TglOwnedVpModeResult result;
        assert(tglPrepareOwnedVpModes(input,{3,0,true},result));
        assert(result.state.at923 && !result.borrowSourceA8); // preserve sticky state/reference
        input.settings12=true;result={{7,8,true},true};
        assert(!tglPrepareOwnedVpModes(input,{},result));
        assert(result.state.at998==7 && result.state.at99c==8 && result.borrowSourceA8);
        input.settings13=true;
        assert(tglPrepareOwnedVpModes(input,{},result) && result.state.at99c==0xd);
        input.source130=1;
        assert(tglPrepareOwnedVpModes(input,{},result) && result.state.at99c==0x15);
        input.source130=0x19;input.settings13=false;input.settings60Present=true;
        for(uint32_t format:{1u,0x51u,0x19u}) {
            input.settings60130=format;
            assert(tglPrepareOwnedVpModes(input,{},result));
            assert(result.state.at99c==(format==1?3u:format==0x51?0x50u:format));
            assert(result.state.at923==(format==1));
        }
        input.source00=1;input.target00=0;input.sourceD8=768;input.sourceDc=577;
        input.sourceA8Present=false;
        assert(tglPrepareOwnedVpModes(input,{},result));
        assert(result.state.at998==5 && result.state.at923 && !result.borrowSourceA8);
    }
    {
        struct Context {
            void* child;void* source;void* target;void* settings;
            TglOwnedVpModeInput input;
            TglOwnedVpModeResult result;
            bool admitted=true,formatOk=false;unsigned samples=0,formats=0;
            bool owns(void* p) const noexcept {return admitted && p==child;}
            void sampleMode(void* a,void* b) noexcept {
                assert(a==source && b==target);++samples;
                tglPrepareOwnedVpSampleMode(input,result.state,result);
            }
            void formatMode(void* a,void* b) noexcept {
                assert(a==source && b==settings);++formats;
                formatOk=tglPrepareOwnedVpFormatMode(input,result,result);
            }
        };
        std::array<uint8_t,4> tokens{};
        Context context{&tokens[0],&tokens[1],&tokens[2],&tokens[3],{}, {}};
        context.input.source00=1;context.input.source130=0x19;
        context.input.sourceA8Present=true;context.input.settings12=true;
        using Callback=TglOwnedVpModeCallbacks<TglOwnedVpSfcResolve<Context>>;
        using Scope=TglOwnedVpSfcScopedContext<Context>;
        static_assert(std::is_same_v<decltype(&Callback::sampleMode),void(*)(void*,void*,void*) noexcept>);
        static_assert(std::is_same_v<decltype(&Callback::formatMode),void(*)(void*,void*,void*) noexcept>);
        Callback::sampleMode(context.child,context.source,context.target);assert(!context.samples);
        {
            Scope scope(context.child,context);assert(scope.bound());
            Callback::sampleMode(context.target,context.source,context.target);
            Callback::sampleMode(context.child,nullptr,context.target);
            context.admitted=false;
            Callback::sampleMode(context.child,context.source,context.target);
            assert(!context.samples);context.admitted=true;
            Callback::sampleMode(context.child,context.source,context.target);
            assert(context.samples==1 && context.result.state.at998==3 &&
                context.result.state.at923 && context.result.borrowSourceA8);
            Callback::formatMode(context.child,context.source,nullptr);assert(!context.formats);
            Callback::formatMode(context.child,context.source,context.settings);
            assert(context.formats==1 && !context.formatOk && context.result.state.at998==3 &&
                context.result.borrowSourceA8);
            context.input.settings60Present=true;context.input.settings60130=0x51;
            Callback::formatMode(context.child,context.source,context.settings);
            assert(context.formatOk && context.result.state.at99c==0x50 &&
                context.result.state.at923 && context.result.borrowSourceA8);
        }
        Callback::formatMode(context.child,context.source,context.settings);assert(context.formats==2);
    }
    {
        TglOwnedVpExtentAdmission input;
        input.minWidth=input.minHeight=1;input.maxWidth=input.maxHeight=16384;
        input.frameWidth=input.sourceWidth=input.destinationWidth=input.targetWidth=1024;
        input.frameHeight=input.sourceHeight=input.destinationHeight=input.targetHeight=1024;
        input.source38=input.source3c=1024;input.target130=1;
        TglOwnedVpScale result;
        assert(tglPrepareOwnedVpExtentAdmission(input,result) && !result.scaling);
        const auto baseline=input;
        for(auto field:{&TglOwnedVpExtentAdmission::frameWidth,&TglOwnedVpExtentAdmission::frameHeight,
                        &TglOwnedVpExtentAdmission::sourceWidth,&TglOwnedVpExtentAdmission::sourceHeight,
                        &TglOwnedVpExtentAdmission::destinationWidth,&TglOwnedVpExtentAdmission::destinationHeight,
                        &TglOwnedVpExtentAdmission::targetWidth,&TglOwnedVpExtentAdmission::targetHeight}) {
            for(uint32_t invalid:{0u,16385u}) {
                auto bad=baseline;bad.*field=invalid;result={17,19,true};
                assert(!tglPrepareOwnedVpExtentAdmission(bad,result));
                assert(result.at928==17 && result.at92c==19 && result.scaling);
            }
        }
        input.minWidth=16385;assert(!tglPrepareOwnedVpExtentAdmission(input,result));input=baseline;
        input.minHeight=16385;assert(!tglPrepareOwnedVpExtentAdmission(input,result));input=baseline;
        input.destinationWidth=16384;input.targetWidth=1024;
        assert(tglPrepareOwnedVpExtentAdmission(input,result) && result.at928==1.0f);input=baseline;
        for(uint32_t extent:{127u,128u,8192u,8193u}) {
            input.destinationWidth=input.targetWidth=extent;
            const bool valid=extent>=128 && extent<=8192;
            result={17,19,true};
            assert(tglPrepareOwnedVpExtentAdmission(input,result)==valid);
            if(!valid) assert(result.at928==17 && result.at92c==19 && result.scaling);
        }
        input.destinationWidth=input.targetWidth=1024;
        input.source124=1;assert(!tglPrepareOwnedVpExtentAdmission(input,result));input.source124=0;
        input.target130=0x19;assert(!tglPrepareOwnedVpExtentAdmission(input,result));
        input.targetE4=1;assert(tglPrepareOwnedVpExtentAdmission(input,result));
        input.conditionalSource=true;assert(!tglPrepareOwnedVpExtentAdmission(input,result));
        input.conditionalSource=false;input.target130=1;
        input.workaroundA4Bit25=true;input.frameHeight=1200;input.source3c=1200;
        assert(tglPrepareOwnedVpExtentAdmission(input,result));
        input.source30=1;assert(!tglPrepareOwnedVpExtentAdmission(input,result));input.source30=0;
        input.source38=1023;assert(!tglPrepareOwnedVpExtentAdmission(input,result));input.source38=1024;
        input.source3c=1199;assert(!tglPrepareOwnedVpExtentAdmission(input,result));input.source3c=1200;
        input.source34=1150;assert(!tglPrepareOwnedVpExtentAdmission(input,result));input.source34=0;
        input.source3c=std::numeric_limits<float>::quiet_NaN();
        assert(!tglPrepareOwnedVpExtentAdmission(input,result));
        input.workaroundA4Bit25=false;
        input.targetWidth=16385;assert(!tglPrepareOwnedVpExtentAdmission(input,result));
        input.targetWidth=1024;input.sourceWidth=0;
        assert(!tglPrepareOwnedVpExtentAdmission(input,result));
    }
    {
        for(bool conditional:{false,true}) for(float x:{0.0f,-0.0f,1.0f})
        for(float y:{0.0f,-0.0f,1.0f}) for(uint32_t tile:{0u,1u,2u})
        for(uint32_t format:{1u,0x19u,0x52u,0x53u}) {
            const bool shortcut=!conditional && x==0 && y==0 && tile==1;
            const bool allowed=format!=0x19 && format!=0x52 && format!=0x53;
            assert(tglOwnedVpFinalAdmission(conditional,x,y,tile,format)==(shortcut || allowed));
        }
        const float nan=std::numeric_limits<float>::quiet_NaN();
        assert(!tglOwnedVpFinalAdmission(false,nan,0,1,0x19));
        assert(!tglOwnedVpFinalAdmission(false,0,nan,1,0x53));
        assert(tglOwnedVpFinalAdmission(false,nan,0,1,1));
    }
    {
        for(uint32_t source=0;source<4;++source) for(uint32_t target=0;target<4;++target)
        for(bool setting:{false,true}) for(uint32_t format:{1u,0x19u}) {
            bool output=true;
            assert(tglPrepareOwnedVpChromaUpsampling(format,setting,source,target,output));
            const bool expected=(source==0 && (target==1 || target==2)) ||
                                (source==1 && target==2);
            assert(output==(expected && !(setting && format==1)));
        }
        for(bool original:{false,true}) for(bool invalidSource:{false,true}) {
            bool output=original;
            assert(!tglPrepareOwnedVpChromaUpsampling(0x19,false,invalidSource?4:0,
                                                      invalidSource?0:4,output));
            assert(output==original);
        }
    }
    {
        for(uint32_t format:{0u,1u,7u,8u,0xau,0xcu,0xdu,0x50u,0x51u,0x55u,0x5au,
                            uint32_t(-8),uint32_t(-9),uint32_t(-7)}) {
            const bool listed=(format>=1 && format<=7) || (format>=0xa && format<=0xc) ||
                format==0x50 || format==0x51 || format==0x55 || format==0x5a ||
                format==uint32_t(-8) || format==uint32_t(-9);
            for(bool settings:{false,true}) for(uint32_t pack=0;pack<4;++pack) {
                TglOwnedVpColorMetadata metadata;
                assert(tglPrepareOwnedVpColorMetadata(format,settings,0x31,0,0x46,pack,pack,metadata));
                assert(metadata.at9a4==(settings && listed?1u:0x31u));
                assert(metadata.at9a8==(pack==1?0x11u:pack==2?0x11u:0x21u));
                assert(metadata.target294==(pack==1?0x16u:pack==2?0x11u:0x46u));
            }
        }
        for(bool invalidInput:{false,true}) {
            TglOwnedVpColorMetadata metadata{7,8,9};
            assert(!tglPrepareOwnedVpColorMetadata(1,true,0,0,0,invalidInput?4:0,
                                                   invalidInput?0:4,metadata));
            assert(metadata.at9a4==7 && metadata.at9a8==8 && metadata.target294==9);
        }
    }
    {
        for(uint32_t rotation:{0u,1u,2u,3u,4u,5u,6u,7u,0xffffffffu}) {
            TglOwnedVpScale scale;
            const bool direct=rotation==0 || rotation==2 || rotation==4 || rotation==5;
            assert(tglPrepareOwnedVpScale(64,32,128,96,rotation,scale));
            assert(scale.at928==(direct?2.0f:1.5f));
            assert(scale.at92c==(direct?3.0f:4.0f) && scale.scaling);
            assert(tglPrepareOwnedVpScale(64,32,direct?64:32,direct?32:64,rotation,scale));
            assert(scale.at928==1.0f && scale.at92c==1.0f && !scale.scaling);
        }
        for(unsigned missing=0;missing<4;++missing) {
            uint32_t extents[4]={64,32,128,96};extents[missing]=0;
            TglOwnedVpScale scale{7.0f,9.0f,true};
            assert(!tglPrepareOwnedVpScale(extents[0],extents[1],extents[2],extents[3],0,scale));
            assert(scale.at928==7.0f && scale.at92c==9.0f && scale.scaling);
        }
    }
    {
        TglOwnedVpSfcCallerStorage storage;
        storage.bytes[0x27]=0xa5;storage.bytes[0x9b8]=0x5a;
        const auto original=storage.bytes;
        for(bool enabled:{false,true}) {
            unsigned calls=0;
            assert(tglWithOwnedVpFrameReferences(storage,0x1000,0x2000,0x3000,enabled,
                [&](void* child) noexcept {
                    ++calls;assert(child==storage.bytes.data());
                    const auto at=[&](size_t offset) noexcept {
                        uintptr_t value=0;std::memcpy(&value,storage.bytes.data()+offset,8);return value;
                    };
                    assert(at(0x988)==(enabled?0x1000u:0u));
                    assert(at(0x980)==reinterpret_cast<uintptr_t>(storage.bytes.data()+0x28));
                    assert(at(0x990)==0x2000 && at(0x9b0)==0x3000);
                    assert(tglWithOwnedVpFrameReferences(storage,0,0,0,false,
                        [](void*) noexcept {assert(false);return 0;})==5);
                    return 31;
                })==31);
            assert(calls==1 && storage.bytes==original);
        }
        for(size_t offset:{size_t(0x988),size_t(0x980),size_t(0x990),size_t(0x9b0)}) {
            const uintptr_t foreign=0x4000;
            std::memcpy(storage.bytes.data()+offset,&foreign,8);
            const auto retained=storage.bytes;
            assert(tglWithOwnedVpFrameReferences(storage,0,0,0,false,
                [](void*) noexcept {assert(false);return 0;})==5);
            assert(storage.bytes==retained);storage.bytes=original;
        }
    }
    {
        std::array<uint8_t,0xe0> surface{};std::array<uint8_t,0x1c> mhw{};
        const auto put=[](auto& data,size_t offset,auto value) {
            std::memcpy(data.data()+offset,&value,sizeof(value));
        };
        put(surface,0xdc,uint32_t(1080));put(surface,0xd8,uint32_t(1920));
        put(surface,0x5c,1079.5f);put(surface,0x58,1919.5f);
        put(mhw,0x1a,uint16_t(8));put(mhw,0x18,uint16_t(16));
        unsigned calls=0,fail=0;
        const auto read=[&](uintptr_t address,void* output,size_t size) noexcept {
            if(++calls==fail) return false;
            const auto span=[&](uintptr_t base,const auto& data) noexcept {
                if(address<base || address-base>data.size() || size>data.size()-(address-base))
                    return false;
                std::memcpy(output,data.data()+address-base,size);return true;
            };
            return span(0x1000,surface) || span(0x2000,mhw);
        };
        uint32_t height=17,width=19;
        assert(tglReadOwnedSfcFrame(0x1000,0x2000,read,height,width));
        assert(height==1080 && width==1920 && calls==7);
        {
            TglOwnedVpSfcCallerStorage storage;
            const uintptr_t mhwAddress=0x2000;
            std::memcpy(storage.bytes.data()+0x20,&mhwAddress,8);
            TglOwnedVpAdmissionContext context(storage,read);
            using Context=decltype(context);
            using Callbacks=TglOwnedVpAdmissionCallbacks<TglOwnedVpSfcResolve<Context>>;
            width=19;height=17;calls=0;
            Callbacks::dimensions(storage.bytes.data(),reinterpret_cast<void*>(0x1000),&width,&height);
            assert(width==19 && height==17 && calls==0);
            {
                TglOwnedVpSfcScopedContext<Context> scope(storage.bytes.data(),context);
                assert(scope.bound());
                Callbacks::dimensions(storage.bytes.data()+8,reinterpret_cast<void*>(0x1000),&width,&height);
                assert(width==19 && height==17 && calls==0);
                Callbacks::dimensions(storage.bytes.data(),reinterpret_cast<void*>(0x1000),&width,&height);
                assert(width==1920 && height==1080 && calls==7);
                calls=0;width=19;height=17;
                Callbacks::dimensions(storage.bytes.data(),reinterpret_cast<void*>(0x1000),&width,&width);
                Callbacks::dimensions(storage.bytes.data(),reinterpret_cast<void*>(0x1000),nullptr,&height);
                assert(width==19 && height==17 && calls==0);
                for(fail=1;fail<=7;++fail) {
                    calls=0;
                    Callbacks::dimensions(storage.bytes.data(),reinterpret_cast<void*>(0x1000),&width,&height);
                    assert(width==19 && height==17);
                }
                fail=0;
            }
            calls=0;
            Callbacks::dimensions(storage.bytes.data(),reinterpret_cast<void*>(0x1000),&width,&height);
            assert(width==19 && height==17 && calls==0);
        }
        for(fail=1;fail<=7;++fail) {
            calls=0;height=17;width=19;
            assert(!tglReadOwnedSfcFrame(0x1000,0x2000,read,height,width));
            assert(calls==fail && height==17 && width==19);
        }
        fail=calls=0;
        assert(!tglReadOwnedSfcFrame(UINTPTR_MAX-8,0x2000,read,height,width) && !calls);
        assert(!tglReadOwnedSfcFrame(0x1000,UINTPTR_MAX-8,read,height,width) && !calls);
        put(surface,0x84,uint8_t(3));
        assert(tglReadOwnedSfcFrame(0x1000,0x2000,read,height,width));
        assert(height==2160 && width==3840);
        height=17;width=19;put(surface,0x5c,std::numeric_limits<float>::quiet_NaN());
        assert(!tglReadOwnedSfcFrame(0x1000,0x2000,read,height,width) && height==17 && width==19);
    }
    {
        std::array<uint8_t,0xf8> first{};
        std::array<uint8_t,0x294> second{};
        std::array<uint8_t,0x134> surface{};
        std::array<uint8_t,8> third{};
        const auto put=[](auto& bytes,size_t offset,auto value) {
            std::memcpy(bytes.data()+offset,&value,sizeof(value));
        };
        put(first,0x90,uint32_t(1));put(first,0x98,uintptr_t(0x3000));
        put(first,0xf0,uintptr_t(0x4000));
        put(second,0x130,uint32_t(0x19));put(surface,0x130,uint32_t(1));
        unsigned reads=0,failAt=0;
        const auto read=[&](uintptr_t address,void* out,size_t size) noexcept {
            if(++reads==failAt) return false;
            const auto span=[&](uintptr_t base,const auto& bytes) noexcept {
                if(address<base || address-base>bytes.size() || size>bytes.size()-(address-base))
                    return false;
                std::memcpy(out,bytes.data()+address-base,size);return true;
            };
            return span(0x1000,first) || span(0x2000,second) ||
                span(0x3000,surface) || span(0x4000,third);
        };
        assert(tglReadOwnedVpAdmission(0,0x1000,0x2000,read));
        const auto total=reads;
        for(failAt=1;failAt<=total;++failAt) {
            reads=0;assert(!tglReadOwnedVpAdmission(0,0x1000,0x2000,read));
            assert(reads==failAt);
        }
        failAt=reads=0;
        TglOwnedVpSfcCallerStorage child,foreignChild;
        TglOwnedVpAdmissionContext context(child,read);
        using Resolve=TglOwnedVpSfcResolve<decltype(context)>;
        using Callbacks=TglOwnedVpAdmissionCallbacks<Resolve>;
        void* frame=reinterpret_cast<void*>(0x1000);
        void* source=reinterpret_cast<void*>(0x2000);
        void* target=reinterpret_cast<void*>(0x3000);
        assert(!Callbacks::eligible(child.bytes.data(),frame,source,nullptr) && !reads);
        {
            TglOwnedVpSfcScopedContext<decltype(context)> scope(child.bytes.data(),context);
            assert(scope.bound());
            assert(!Callbacks::eligible(foreignChild.bytes.data(),frame,source,nullptr) && !reads);
            assert(Callbacks::eligible(child.bytes.data(),frame,source,nullptr));
            assert(Callbacks::pair(child.bytes.data(),source,target,nullptr));
            assert(Callbacks::input(child.bytes.data(),source));
            assert(Callbacks::output(child.bytes.data(),target));
            child.bytes[8]=1;reads=0;
            assert(!Callbacks::eligible(child.bytes.data(),frame,source,nullptr) && !reads);
            child.bytes[8]=0;
        }
        reads=0;
        assert(!Callbacks::pair(child.bytes.data(),source,target,nullptr) && !reads);
        assert(!Callbacks::input(child.bytes.data(),source) && !reads);
        assert(!Callbacks::output(child.bytes.data(),target) && !reads);
        {
            TglOwnedVpSfcScopedContext<decltype(context)> wrong(foreignChild.bytes.data(),context);
            assert(wrong.bound());
            assert(!Callbacks::input(foreignChild.bytes.data(),source) && !reads);
        }
        assert(!tglReadOwnedVpAdmission(1,0x1000,0x2000,read) && !reads);
        assert(!tglReadOwnedVpAdmission(0,UINTPTR_MAX-8,0x2000,read) && !reads);
        assert(!tglReadOwnedVpPairAdmission(0x2000,0x3000,UINTPTR_MAX-3,read) && !reads);
        put(second,0x290,int32_t(4));
        assert(!tglReadOwnedVpAdmission(0,0x1000,0x2000,read) && reads==2);
        put(second,0x290,int32_t(-1));put(surface,0xe4,uint32_t(1));
        reads=0;assert(tglReadOwnedVpAdmission(0,0x1000,0x2000,read));
        put(surface,0x130,uint32_t(0x53));
        reads=0;assert(tglReadOwnedVpAdmission(0,0x1000,0x2000,read));
        put(surface,0xe4,uint32_t(2));
        reads=0;assert(!tglReadOwnedVpAdmission(0,0x1000,0x2000,read));
        put(second,0x290,int32_t(0));put(surface,0xe4,uint32_t(1));
        put(first,0x98,uintptr_t(0));
        reads=0;assert(!tglReadOwnedVpAdmission(0,0x1000,0x2000,read));
    }
    {
        constexpr std::array<uint32_t,18> inputs{
            0x19,0x15,0x53,0x52,0x17,0x13,3,4,1,2,0xfffffff9u,
            0xd,0xe,0xf,0x10,0x11,0x12,0x14};
        constexpr std::array<uint32_t,11> outputs{
            1,2,3,4,0x50,0x51,0xfffffff8u,0x19,0xd,0x10,0x15};
        for(uint32_t format=0;format<256;++format) {
            const bool in=std::find(inputs.begin(),inputs.end(),format)!=inputs.end();
            const bool out=std::find(outputs.begin(),outputs.end(),format)!=outputs.end();
            assert(tglOwnedVpInputFormat(format)==in);
            for(uint32_t type:{0u,1u,2u,UINT32_MAX})
                assert(tglOwnedVpOutputFormat(format,type)==(out || (format==0x53 && type==1)));
        }
        assert(tglOwnedVpInputFormat(0xfffffff9u));
        assert(!tglOwnedVpInputFormat(0xfffffff8u) && !tglOwnedVpInputFormat(UINT32_MAX));
        assert(tglOwnedVpOutputFormat(0xfffffff8u,0));
        assert(!tglOwnedVpOutputFormat(0xfffffff9u,1) && !tglOwnedVpOutputFormat(UINT32_MAX,1));
    }
    {
        TglOwnedVpPairInputs input;
        unsigned calls70=0,calls78=0;bool allow70=true,allow78=true;
        const auto p70=[&]() noexcept {++calls70;return allow70;};
        const auto p78=[&]() noexcept {++calls78;return allow78;};
        assert(!tglOwnedVpPairAdmission(input,p70,p78) && !calls70 && !calls78);
        input.firstPresent=true;
        assert(!tglOwnedVpPairAdmission(input,p70,p78) && !calls70 && !calls78);
        input.secondPresent=true;allow70=false;
        assert(!tglOwnedVpPairAdmission(input,p70,p78) && calls70==1 && !calls78);
        allow70=true;
        for(uint32_t blocked:{0x55u,0x5au}) {
            input.second130=blocked;calls70=calls78=0;
            assert(!tglOwnedVpPairAdmission(input,p70,p78) && calls70==1 && !calls78);
        }
        input.second130=0;allow78=false;calls70=calls78=0;
        assert(!tglOwnedVpPairAdmission(input,p70,p78) && calls70==1 && calls78==1);
        allow78=true;
        for(bool third:{false,true}) for(uint32_t mode:{0u,1u,3u,4u})
            for(uint32_t a:{0u,1u,3u,0x15u,0x50u})
                for(uint32_t b:{0u,1u,3u,0x15u,0x50u,0x51u}) {
                    input.thirdPresent=third;input.third04=mode;
                    input.first130=a;input.second130=b;calls70=calls78=0;
                    const bool blocked=third && (mode==0 || mode==3) &&
                        (a==1 || a==3 || a==0x15) &&
                        (b==1 || b==3 || b==0x15 || b==0x50 || b==0x51);
                    assert(tglOwnedVpPairAdmission(input,p70,p78)==!blocked);
                    assert(calls70==1 && calls78==1);
                }
    }
    {
        TglOwnedVpAdmissionInputs input;input.first90=1;
        unsigned calls=0;bool qualified=true;
        const auto predicate=[&]() noexcept {++calls;return qualified;};
        assert(tglOwnedVpAdmission(input,predicate) && calls==1);
        input.child08=1;calls=0;
        assert(!tglOwnedVpAdmission(input,predicate) && !calls);
        input.child08=2;input.first90=0;
        assert(!tglOwnedVpAdmission(input,predicate) && !calls);
        input.first90=1;
        for(int32_t mode:{INT32_MIN,-1,0,1,2,3,4,INT32_MAX}) {
            input.second290=mode;
            for(uint32_t format:{0u,1u,2u}) {
                input.first98_e4=format;calls=0;
                const bool expected=mode==0 || (mode<=3 && format==1);
                assert(tglOwnedVpAdmission(input,predicate)==expected);
                assert(calls==unsigned(expected));
            }
        }
        input.second290=0;qualified=false;calls=0;
        assert(!tglOwnedVpAdmission(input,predicate) && calls==1);
        qualified=true;
        for(bool present:{false,true}) for(uint32_t kind:{0u,1u,2u,3u,4u})
            for(uint8_t flags:{uint8_t(0),uint8_t(1),uint8_t(2),uint8_t(3)}) {
                input.second88Present=present;input.second130=kind;input.second83=flags;
                calls=0;
                assert(tglOwnedVpAdmission(input,predicate)==
                    (!(present && (kind==1 || kind==3)) && !(flags&1)));
                assert(calls==1);
            }
    }
    {
        struct Context {
            void* child;void* source;void* target;void* data;void* command;
            bool leased=false;int result=0;unsigned setups=0,sends=0,frees=0;
            bool owns(void* candidate) const noexcept {return leased && candidate==child;}
            void freeResources() noexcept {++frees;}
            int setup(void* a,void* b,void* c) noexcept {
                assert(a==source && b==target && c==data);++setups;return result;
            }
            int send(void* a,void* b) noexcept {
                assert(a==data && b==command);++sends;return result;
            }
        };
        struct Resolve {
            static Context*& current() noexcept {static Context* context=nullptr;return context;}
            Context* operator()(void* child) const noexcept {
                auto* context=current();
                return context && context->leased && context->child==child ? context : nullptr;
            }
        };
        std::array<uint8_t,5> tokens{};
        Context context{&tokens[0],&tokens[1],&tokens[2],&tokens[3],&tokens[4]};
        Resolve::current()=&context;
        using Callbacks=TglOwnedVpSfcCallbacks<Resolve>;
        static_assert(std::is_same_v<decltype(&Callbacks::freeResources),void(*)(void*) noexcept>);
        Callbacks::freeResources(context.child);assert(!context.frees);
        assert(Callbacks::setup(context.child,context.source,context.target,context.data)==5);
        context.leased=true;
        for(int result:{0,31,-7}) {
            context.result=result;
            assert(Callbacks::setup(context.child,context.source,context.target,context.data)==result);
            assert(Callbacks::send(context.child,context.data,context.command)==result);
        }
        assert(Callbacks::setup(context.child,nullptr,context.target,context.data)==5);
        assert(Callbacks::send(context.child,context.data,nullptr)==5);
        assert(Callbacks::send(context.target,context.data,context.command)==5);
        assert(context.setups==3 && context.sends==3);
        Resolve::current()=nullptr;
        assert(Callbacks::send(context.child,context.data,context.command)==5);
        using Scoped=TglOwnedVpSfcScopedContext<Context>;
        using RealCallbacks=TglOwnedVpSfcCallbacks<TglOwnedVpSfcResolve<Context>>;
        assert(!Scoped::get(context.child));
        {
            Scoped wrong(context.target,context);assert(wrong.bound());
            const auto frees=context.frees,setups=context.setups,sends=context.sends;
            RealCallbacks::freeResources(context.target);
            assert(RealCallbacks::setup(context.target,context.source,context.target,context.data)==5);
            assert(RealCallbacks::send(context.target,context.data,context.command)==5);
            assert(context.frees==frees && context.setups==setups && context.sends==sends);
        }
        {
            Scoped invalid(nullptr,context);assert(!invalid.bound());
            Scoped scope(context.child,context);assert(scope.bound());
            Scoped nested(context.target,context);assert(!nested.bound());
            assert(!Scoped::get(context.target) && Scoped::get(context.child)==&context);
            TglOwnedVeboxScopedContext<Context> enclosing(context.target,context);
            assert(enclosing.bound()); // Different child domains do not alias.
            assert(TglOwnedVeboxScopedContext<Context>::get(context.child)==nullptr);
            assert(RealCallbacks::send(context.target,context.data,context.command)==5);
            context.result=31;
            assert(RealCallbacks::send(context.child,context.data,context.command)==31);
            RealCallbacks::freeResources(context.target);assert(!context.frees);
            RealCallbacks::freeResources(context.child);assert(context.frees==1);
        }
        assert(!Scoped::get(context.child));
        assert(RealCallbacks::send(context.child,context.data,context.command)==5);
        RealCallbacks::freeResources(context.child);assert(context.frees==1);
    }
    {
        TglOwnedVpSfcCallerStorage storage;
        for(auto byte:storage.bytes) assert(byte==0);
        TglSfcMhwBinding binding;binding.image=0x100000;
        std::array<uint8_t,0x41> mhw{};
        std::array<uint8_t,0x90> os{};
        const auto mhwAddress=reinterpret_cast<uintptr_t>(mhw.data());
        const auto osAddress=reinterpret_cast<uintptr_t>(os.data());
        const uintptr_t table=binding.image+0x759420,callback=binding.image+0x3eae0;
        const uint16_t width=16,height=4;const uint32_t flags=1;
        std::memcpy(mhw.data(),&table,8);std::memcpy(mhw.data()+8,&callback,8);
        std::memcpy(mhw.data()+0x10,&osAddress,8);
        std::memcpy(mhw.data()+0x18,&width,2);std::memcpy(mhw.data()+0x1a,&height,2);
        std::memcpy(os.data()+0x88,&flags,4);mhw[0x40]=1;
        auto read=[&](uintptr_t address,void* output,size_t length) noexcept {
            for(const auto& range:std::array<std::pair<uintptr_t,size_t>,2>{{
                {mhwAddress,mhw.size()},{osAddress,os.size()}}})
                if(address>=range.first && address-range.first<=range.second &&
                    length<=range.second-(address-range.first)) {
                    std::memcpy(output,reinterpret_cast<const void*>(address),length);return true;
                }
            return false;
        };
        mhw[0x40]=0;
        assert(!storage.initializeBorrowed(osAddress,0x3000,mhwAddress,binding,read));
        for(auto byte:storage.bytes) assert(byte==0);
        mhw[0x40]=1;
        assert(storage.initializeBorrowed(osAddress,0x3000,mhwAddress,binding,read));
        std::array<uint8_t,0x9d0> expectedBorrowed{};
        const uintptr_t renderHal=0x3000;
        std::memcpy(expectedBorrowed.data()+0x10,&osAddress,8);
        std::memcpy(expectedBorrowed.data()+0x18,&renderHal,8);
        std::memcpy(expectedBorrowed.data()+0x20,&mhwAddress,8);
        assert(storage.bytes==expectedBorrowed);
        auto bound=storage.bytes;
        assert(!storage.initializeBorrowed(osAddress,0x3000,mhwAddress,binding,read));
        assert(storage.bytes==bound);
        uintptr_t stored=0;std::memcpy(&stored,storage.bytes.data()+0x20,8);
        assert(stored==mhwAddress);
        storage.bytes.fill(0xa5);
        auto expected=storage.bytes;
        const uint32_t channel=0x12345678;
        const uintptr_t performance=0x123456789abcdef0ULL;
        std::memcpy(expected.data()+0x918,&channel,4);
        std::memcpy(expected.data()+0x9c8,&performance,8);
        storage.setCallerFields(channel,performance);
        assert(storage.bytes==expected);
        storage.setCallerFields(0,0);
        std::memset(expected.data()+0x918,0,4);
        std::memset(expected.data()+0x9c8,0,8);
        assert(storage.bytes==expected);
    }
    {
        std::array<uint8_t,0xb8> prefix{};
        prefix[0]=1;
        const std::array<size_t,8> dimensions{0x24,0x28,0x30,0x34,0x3c,0x40,0x4c,0x50};
        const uint32_t valid=1920;
        for(auto offset:dimensions) std::memcpy(prefix.data()+offset,&valid,4);
        TglOwnedSfcStateParametersPacket result{};
        assert(tglPrepareOwnedSfcGen12Pass(result,prefix,{},true,true,true,
            TglOwnedSfcCompressionMode::Render));
        assert(result[0x66]==1 && result[0x6a]==1 && result[0x8b]==1 && result[0x8c]==4);
        const auto previous=result;
        for(auto offset:dimensions) {
            for(uint32_t invalid:{0u,0x4001u,UINT32_MAX}) {
                auto bad=prefix;std::memcpy(bad.data()+offset,&invalid,4);
                assert(!tglPrepareOwnedSfcGen12Pass(result,bad,{},false,false,false,
                    TglOwnedSfcCompressionMode::Disabled) && result==previous);
            }
        }
        for(size_t offset:{size_t(0x44),size_t(0x48),size_t(0x54),size_t(0x58)}) {
            auto bad=prefix;const uint32_t invalid=offset<0x54 ? 0x4000 : 0x8000;
            std::memcpy(bad.data()+offset,&invalid,4);
            assert(!tglPrepareOwnedSfcGen12Pass(result,bad,{},false,false,false,
                TglOwnedSfcCompressionMode::Disabled) && result==previous);
        }
        assert(!tglPrepareOwnedSfcGen12Pass(result,prefix,{},false,false,true,
            TglOwnedSfcCompressionMode::Disabled) && result==previous);
        auto badTail=TglOwnedSfcGen12Tail{};badTail.engineMode=4;
        assert(!tglPrepareOwnedSfcGen12Pass(result,prefix,badTail,false,false,false,
            TglOwnedSfcCompressionMode::Disabled) && result==previous);
        for(uint32_t hcpMode:{1u,2u,3u}) {
            badTail.engineMode=hcpMode;
            assert(!tglPrepareOwnedSfcGen12Pass(result,prefix,badTail,false,false,false,
                TglOwnedSfcCompressionMode::Disabled) && result==previous);
        }
        const uint32_t maximum=0x4000;
        for(auto offset:dimensions) std::memcpy(prefix.data()+offset,&maximum,4);
        assert(tglPrepareOwnedSfcGen12Pass(result,prefix,{},false,false,false,
            TglOwnedSfcCompressionMode::Disabled));
    }
    {
        TglOwnedSfcStateParametersPacket packet{};
        auto extent=[&](size_t offset,uint32_t value) {
            std::memcpy(packet.data()+offset,&value,4);
        };
        extent(0x3c,1920);extent(0x40,1080);
        extent(0x4c,1920);extent(0x50,2160);
        std::array<uint32_t,2> steps{99,99};
        assert(tglOwnedSfcGen12ScalingSteps(packet,steps));
        assert((steps==std::array<uint32_t,2>{0x80000,0x40000}));
        extent(0x4c,960);
        assert(tglOwnedSfcGen12ScalingSteps(packet,steps) && steps[0]==0x100000);
        // Native quantization uses integer extents, not the float-scale fields.
        const float unrelated=0.125f;
        std::memcpy(packet.data()+0x5c,&unrelated,4);
        assert(tglOwnedSfcGen12ScalingSteps(packet,steps) && steps[0]==0x100000);
        for(size_t offset:{size_t(0x3c),size_t(0x40),size_t(0x4c),size_t(0x50)}) {
            const auto valid=packet;const auto previous=steps;
            extent(offset,0);
            assert(!tglOwnedSfcGen12ScalingSteps(packet,steps) && steps==previous);
            packet=valid;
        }
    }
    {
        for(auto mode:{TglOwnedSfcCompressionMode::Disabled,
                       TglOwnedSfcCompressionMode::Media,
                       TglOwnedSfcCompressionMode::Render}) {
            for(bool enabled:{false,true}) {
                TglOwnedSfcStateParametersPacket packet{};packet.fill(0xa5);
                auto expected=packet;
                const bool admitted=!enabled || mode!=TglOwnedSfcCompressionMode::Disabled;
                if(admitted) {
                    expected[0x8b]=enabled;
                    const uint32_t value=static_cast<uint32_t>(mode);
                    std::memcpy(expected.data()+0x8c,&value,sizeof(value));
                }
                assert(tglPrepareOwnedSfcGen12CompressionFields(packet,enabled,mode)==admitted);
                assert(packet==expected);
            }
        }
        for(uint32_t invalid:{1u,2u,5u,UINT32_MAX}) {
            TglOwnedSfcStateParametersPacket packet{};packet.fill(0xa5);
            const auto previous=packet;
            assert(!tglPrepareOwnedSfcGen12CompressionFields(packet,true,
                static_cast<TglOwnedSfcCompressionMode>(invalid)));
            assert(packet==previous);
        }
    }
    {
        for(unsigned flags=0;flags<4;++flags) {
            TglOwnedSfcStateParametersPacket packet{};packet.fill(0xa5);
            auto expected=packet;
            expected[0x66]=(flags&1) ? 1 : 0;
            expected[0x6a]=(flags&2) ? 1 : 0;
            tglPrepareOwnedSfcGen12FilterFields(packet,flags&1,flags&2);
            assert(packet==expected); // unrelated fields, including +38, untouched
            tglPrepareOwnedSfcGen12FilterFields(packet,false,false);
            assert(packet[0x66]==0 && packet[0x6a]==0 && packet[0x38]==0xa5);
        }
    }
    {
        std::array<uint8_t,0xb8> prefix{};prefix.fill(0xa5);
        TglOwnedSfcGen12Tail tail{3,2,1,17,1919,23,3839,0x1110,0x2220,0x3330,0x4440};
        TglOwnedSfcStateParametersPacket state{};
        assert(tglPrepareOwnedSfcGen12State(state,prefix,tail));
        assert(std::memcmp(state.data(),prefix.data(),prefix.size())==0);
        const std::array<uint32_t,7> expected{3,2,1,17,1919,23,3839};
        assert(std::memcmp(state.data()+0xb8,expected.data(),sizeof(expected))==0);
        for(size_t i=0xd4;i<0xd8;++i) assert(state[i]==0);
        const std::array<uintptr_t,4> resources{0x1110,0x2220,0x3330,0x4440};
        assert(std::memcmp(state.data()+0xd8,resources.data(),sizeof(resources))==0);
        const auto previous=state;
        auto invalid=tail;invalid.engineMode=4;
        assert(!tglPrepareOwnedSfcGen12State(state,prefix,invalid) && state==previous);
        invalid=tail;invalid.inputBitDepth=3;
        assert(!tglPrepareOwnedSfcGen12State(state,prefix,invalid) && state==previous);
        invalid=tail;invalid.tileType=2;
        assert(!tglPrepareOwnedSfcGen12State(state,prefix,invalid) && state==previous);
        invalid=tail;invalid.dstEndX=0x4000;
        assert(!tglPrepareOwnedSfcGen12State(state,prefix,invalid) && state==previous);
        assert(tglPrepareOwnedSfcGen12State(state,prefix,{}));
        for(size_t i=0xb8;i<state.size();++i) assert(state[i]==0);
    }
    {
        struct Backend {
            std::array<int,8> order{};
            size_t count=0;int failure=0;
            const TglOwnedSfcCommandParameters *expected=nullptr;
            bool leasedLock=false;uint8_t lockFlag=0;
            uintptr_t resource=0;
            int stage(int id){order[count++]=id;return failure==id?-id:0;}
            int lock(const TglOwnedSfcLockParameters& p){
                assert((&p==&expected->lock)!=leasedLock && p[0]==1);
                assert(p[8]==lockFlag);return stage(1);
            }
            int state(const TglOwnedSfcStateParametersPacket& p,const std::array<uint8_t,0x38>& d){
                assert(&p==&expected->state && d[0]==0xee);
                uintptr_t actual=0;std::memcpy(&actual,d.data()+0x28,8);assert(actual==resource);
                return stage(4);
            }
            int avs(const std::array<uint8_t,16>& p){assert(&p==&expected->avs);return stage(5);}
            int y(const TglOwnedSfcYParameters& p){assert(&p==&expected->y);return stage(6);}
            int uv(const TglOwnedSfcUvParameters& p){assert(&p==&expected->uv);return stage(7);}
            int ief(const std::array<uint8_t,64>& p){assert(&p==&expected->ief);return stage(8);}
            int frameStart(){return stage(9);}
        };
        const TglOwnedSfcCommandParameters parameters{};
        const std::array<uint8_t,0x2a8> shell{};
        {
            TglOwnedSfcCommandParameters invalid{};
            const uint32_t nv12=0x19;
            std::memcpy(invalid.state.data()+0x2c,&nv12,4);
            Backend backend;backend.expected=&invalid;
            unsigned platformCalls=0;
            auto platform=[&](auto&) {++platformCalls;return 0;};
            assert(tglSendOwnedSfcPreparedCommands(backend,invalid,shell,
                0,0,0,0,platform)==5);
            assert(!backend.count && !platformCalls);
            auto output=shell;
            const uint32_t pitch=2048;
            std::memcpy(output.data()+0x130,&nv12,4);
            std::memcpy(output.data()+0xe0,&pitch,4);
            for(unsigned mutate=0;mutate<2;++mutate) {
                Backend changed;changed.expected=&invalid;
                auto corrupt=[&](auto& descriptor) {
                    const uint32_t value=mutate?0x52:0x10000;
                    std::memcpy(descriptor.data()+(mutate?4:0x24),&value,4);
                    return 0;
                };
                assert(tglSendOwnedSfcPreparedCommands(changed,invalid,output,
                    0,0,0,0,corrupt)==5);
                assert(changed.count==1 && changed.order[0]==1);
            }
            for(uint32_t row:{uint32_t(0),uint32_t(0xffff)}) {
                std::memcpy(output.data()+0x108,&row,4);
                Backend valid;valid.expected=&invalid;
                valid.resource=reinterpret_cast<uintptr_t>(output.data()+0x148);
                auto platform=[](auto& descriptor) {descriptor[0]=0xee;return 0;};
                assert(tglSendOwnedSfcPreparedCommands(valid,invalid,output,
                    0,0,0,0,platform)==0);
                assert(valid.count==3 && valid.order[0]==1 &&
                    valid.order[1]==4 && valid.order[2]==9);
            }
        }
        for(unsigned flags=0;flags<16;++flags) for(int failure=0;failure<=9;++failure) {
            Backend backend;backend.failure=failure;backend.expected=&parameters;
            backend.resource=reinterpret_cast<uintptr_t>(shell.data()+0x148);
            auto platform=[&](std::array<uint8_t,0x38>& d){d[0]=0xee;return backend.stage(3);};
            const int status=tglSendOwnedSfcPreparedCommands(backend,parameters,shell,
                flags&1,(flags>>1)&1,(flags>>2)&1,(flags>>3)&1,platform);
            std::array<int,8> expected{};size_t count=0;int expectedStatus=0;
            for(int id : {1,3,4,5,6,7,8,9}) {
                if(id>=5 && id<=7 && !(flags&3)) continue;
                if(id==8 && !(flags&12)) continue;
                expected[count++]=id;
                if(id==failure){expectedStatus=-id;break;}
            }
            assert(status==expectedStatus && backend.count==count && backend.order==expected);
        }
        for(unsigned failRead=0;failRead<=2;++failRead) for(int failCommand:{0,1,4,9}) {
            unsigned reads=0;
            auto read=[&](uintptr_t address,void* out,size_t size) noexcept {
                assert(size==1 && (address==0x1013 || address==0x100b));
                if(++reads==failRead) return false;
                *static_cast<uint8_t*>(out)=address==0x100b?1:0;return true;
            };
            Backend backend;backend.expected=&parameters;backend.leasedLock=true;backend.lockFlag=1;
            backend.failure=failCommand;backend.resource=reinterpret_cast<uintptr_t>(shell.data()+0x148);
            auto platform=[&](auto& descriptor) {descriptor[0]=0xee;return backend.stage(3);};
            const int result=tglSendOwnedSfcLeasedCommands(backend,parameters,shell,
                0x1000,0,0,0,0,read,platform);
            if(failRead) assert(result==5 && !backend.count && reads==failRead);
            else assert(result==(failCommand?-failCommand:0) && reads==2 && backend.count);
            assert(parameters.lock==tglPrepareOwnedSfcLockParameters(1,0,0));
        }
        for(unsigned controlFlag:{0u,1u,2u,3u}) for(unsigned failRead:{0u,1u,2u}) {
            auto output=shell;output[0xe8]=uint8_t(controlFlag);
            const uintptr_t backing=1,outputResource=reinterpret_cast<uintptr_t>(output.data()+0x148);
            std::memcpy(output.data()+0x168,&backing,8);
            TglOwnedSfcCommandParameters usageParameters;assert(usageParameters.snapshotFrom(parameters));
            std::memcpy(usageParameters.state.data()+0x90,&outputResource,8);
            unsigned reads=0,updates=0,controls=0;
            auto read=[&](uintptr_t address,void* out,size_t size) noexcept {
                assert(size==1 && (address==0x1013 || address==0x100b));
                if(++reads==failRead) return false;
                *static_cast<uint8_t*>(out)=address==0x100b?1:0;return true;
            };
            Backend backend;backend.expected=&usageParameters;backend.leasedLock=true;backend.lockFlag=1;
            backend.resource=reinterpret_cast<uintptr_t>(output.data()+0x148);
            auto update=[&](const void* resource,uint32_t usage,uint32_t flag) noexcept {
                assert(!backend.count && !updates && !controls && reads==2);
                assert(resource==output.data()+0x148 && usage==6 && flag==1);++updates;
            };
            auto control=[&](const void* resource,uint32_t usage) noexcept {
                assert(!backend.count && updates==1 && !controls);
                assert(resource==output.data()+0x148 && usage==6);++controls;
            };
            auto platform=[&](auto& descriptor) {descriptor[0]=0xee;return backend.stage(3);};
            assert(tglSendOwnedSfcWithOutputUsage(backend,usageParameters,output,
                0x1000,0,0,0,0,read,update,control,platform)==(failRead?5:0));
            assert(updates==(failRead?0:1) && controls==(failRead?0:(controlFlag&1)));
            assert(failRead?!backend.count:backend.count==4);
        }
        for(bool missingBacking:{false,true}) {
            auto output=shell;TglOwnedSfcCommandParameters rejected;assert(rejected.snapshotFrom(parameters));
            const uintptr_t backing=missingBacking?0:1;
            const uintptr_t wrongOutput=missingBacking?
                reinterpret_cast<uintptr_t>(output.data()+0x148):0;
            std::memcpy(output.data()+0x168,&backing,8);
            std::memcpy(rejected.state.data()+0x90,&wrongOutput,8);
            Backend backend;
            auto read=[](uintptr_t,void*,size_t) noexcept {assert(false);return false;};
            auto update=[](const void*,uint32_t,uint32_t) noexcept {assert(false);};
            auto control=[](const void*,uint32_t) noexcept {assert(false);};
            auto platform=[](auto&) {assert(false);return 5;};
            assert(tglSendOwnedSfcWithOutputUsage(backend,rejected,output,
                0x1000,0,0,0,0,read,update,control,platform)==5);
            assert(!backend.count);
        }
        {
            constexpr uintptr_t os=0x1000,image=0x100000;
            const uintptr_t entry=image+0x659b0;
            const std::array<uint8_t,20> anchor{
                0x55,0x48,0x89,0xe5,0x48,0x89,0x7d,0xf8,0x48,0x89,
                0x75,0xf0,0x89,0x55,0xec,0x89,0x4d,0xe8,0x5d,0xc3};
            unsigned settingReads=0,invoked=0;
            auto read=[&](uintptr_t address,void* out,size_t size) noexcept {
                if(address==os+0x128 && size==8) {std::memcpy(out,&entry,8);return true;}
                if(address==entry && size==20) {std::memcpy(out,anchor.data(),20);return true;}
                if(address==0x2013 && size==1) {++settingReads;*static_cast<uint8_t*>(out)=1;return true;}
                return false;
            };
            TglNativeSfcOutputUsageBinding usage;
            assert(usage.resolve(os,image,read,[](uintptr_t value){return value==image;},
                [&](uintptr_t value,size_t size){return value==entry && size==20;}));
            for(unsigned flag:{0u,1u,2u}) for(int failure:{0,1,4,9}) {
                auto output=shell;output[0xe8]=uint8_t(flag);
                const uint32_t pitch=2048;std::memcpy(output.data()+0xe0,&pitch,4);
                const uintptr_t mockBacking=1;
                std::memcpy(output.data()+0x168,&mockBacking,8);
                TglOwnedSfcCommandParameters nativeParameters;assert(nativeParameters.snapshotFrom(parameters));
                const uint32_t supportedFormat=0x50;
                std::memcpy(nativeParameters.state.data()+0x2c,&supportedFormat,4);
                Backend backend;backend.expected=&nativeParameters;backend.leasedLock=true;backend.lockFlag=1;
                backend.failure=failure;backend.resource=reinterpret_cast<uintptr_t>(output.data()+0x148);
                std::memcpy(nativeParameters.state.data()+0x90,&backend.resource,8);
                settingReads=invoked=0;
                auto invoke=[&](uintptr_t function,uintptr_t context,const void* resource,uint32_t u,uint32_t f) noexcept {
                    assert(function==entry && context==os && resource==output.data()+0x148);
                    assert(u==6 && f==1 && settingReads==1 && !backend.count);++invoked;
                };
                auto platform=[&](auto& descriptor){assert(invoked==1);descriptor[0]=0xee;return backend.stage(3);};
                const int result=tglSendOwnedSfcNativeOutputCommands(usage,backend,nativeParameters,
                    output,0x2000,0,0,0,0,read,platform,invoke);
                if(flag&1) assert(result==5 && !invoked && !backend.count);
                else assert(result==(failure?-failure:0) && invoked==1 && backend.count);
                assert(output[0xe8]==flag && parameters.lock[8]==0);
            }
            TglOwnedSurfaceStorage typedOutput;
            typedOutput.resource[0]=0x80;
            const uintptr_t mockBacking=1;
            std::memcpy(typedOutput.resource.data()+0x20,&mockBacking,8);
            const auto originalResource=typedOutput.resource;
            const uint32_t validPitch=2048;
            std::memcpy(typedOutput.prefix.data()+0xe0,&validPitch,4);
            TglOwnedSfcCommandParameters nativeParameters;assert(nativeParameters.snapshotFrom(parameters));
            const uint32_t supportedFormat=0x50;
            std::memcpy(nativeParameters.state.data()+0x2c,&supportedFormat,4);
            Backend backend;backend.expected=&nativeParameters;backend.leasedLock=true;backend.lockFlag=1;
            backend.resource=reinterpret_cast<uintptr_t>(typedOutput.resource.data());
            std::memcpy(nativeParameters.state.data()+0x90,&backend.resource,8);
            settingReads=invoked=0;
            auto invoke=[&](uintptr_t function,uintptr_t context,const void* resource,uint32_t u,uint32_t f) noexcept {
                assert(function==entry && context==os && resource==typedOutput.resource.data());
                assert(u==6 && f==1 && settingReads==1 && !backend.count);++invoked;
            };
            auto platform=[&](auto& descriptor) {assert(invoked==1);descriptor[0]=0xee;return backend.stage(3);};
            assert(tglSendOwnedSfcNativeOutputCommands(usage,backend,nativeParameters,typedOutput,
                0x2000,0,0,0,0,read,platform,invoke)==0);
            assert(invoked==1 && backend.count==4 && typedOutput.resource==originalResource);
            const std::array<uint32_t,17> supported{1,2,3,4,7,0xd,0x10,0x12,
                0x13,0x14,0x15,0x17,0x19,0x50,0x51,0x52,0x53};
            for(uint32_t format=0;format<=0x54;++format) {
                bool expected=false;for(auto value:supported) expected|=format==value;
                assert(tglNativeSfcStateFormatSupported(format)==expected);
                if(expected) continue;
                std::memcpy(nativeParameters.state.data()+0x2c,&format,4);
                backend.count=settingReads=invoked=0;
                assert(tglSendOwnedSfcNativeOutputCommands(usage,backend,nativeParameters,typedOutput,
                    0x2000,0,0,0,0,read,platform,invoke)==31);
                assert(!backend.count && !settingReads && !invoked &&
                    typedOutput.resource==originalResource);
            }
            assert(!tglNativeSfcStateFormatSupported(UINT32_MAX));
            std::memcpy(nativeParameters.state.data()+0x2c,&supportedFormat,4);
            for(uint32_t pitch:{0u,1u,0x20000u,0x20001u,UINT32_MAX}) {
                const bool fits=pitch && pitch<=0x20000;
                assert(tglNativeSfcOutputPitchFits(pitch)==fits);
                std::memcpy(typedOutput.prefix.data()+0xe0,&pitch,4);
                backend.count=settingReads=invoked=0;
                assert(tglSendOwnedSfcNativeOutputCommands(usage,backend,nativeParameters,typedOutput,
                    0x2000,0,0,0,0,read,platform,invoke)==(fits?0:5));
                assert(fits ? invoked==1 && backend.count==4 :
                              !invoked && !backend.count && !settingReads);
            }
            std::memcpy(typedOutput.prefix.data()+0xe0,&validPitch,4);
            for(uint32_t pitch:{0u,0x20001u}) {
                backend.count=settingReads=invoked=0;
                auto corruptPitch=[&](auto& d) {
                    std::memcpy(d.data()+0x10,&pitch,4);return 0;
                };
                assert(tglSendOwnedSfcNativeOutputCommands(usage,backend,nativeParameters,typedOutput,
                    0x2000,0,0,0,0,read,corruptPitch,invoke)==5);
                assert(invoked==1 && settingReads==1 && backend.count==1);
            }
            const auto descriptor=tglOwnedSfcOutputDescriptor(typedOutput);
            uintptr_t resource=0;std::memcpy(&resource,descriptor.data()+0x28,8);
            assert(resource==reinterpret_cast<uintptr_t>(typedOutput.resource.data()));
            // Shape-only mocks above do not establish real backing lifetime.
            // Reject missing backing or stale packet identity before any side effect.
            for(bool missingBacking:{true,false}) {
                typedOutput.resource=originalResource;
                std::memcpy(nativeParameters.state.data()+0x90,&backend.resource,8);
                if(missingBacking) std::memset(typedOutput.resource.data()+0x20,0,8);
                else std::memset(nativeParameters.state.data()+0x90,0,8);
                const auto before=typedOutput.resource;
                backend.count=settingReads=invoked=0;
                assert(tglSendOwnedSfcNativeOutputCommands(usage,backend,nativeParameters,typedOutput,
                    0x2000,0,0,0,0,read,platform,invoke)==5);
                assert(!backend.count && !settingReads && !invoked && typedOutput.resource==before);
            }
        }
    }
    {
        std::array<uint8_t,0x2a8> shell{};
        auto put=[&](size_t offset,uint32_t value){std::memcpy(shell.data()+offset,&value,4);};
        put(0x294,2);put(0x130,0x52);put(0xd8,1920);put(0xdc,1080);
        put(0xe0,2048);put(0xe4,4096);put(0xf4,7);put(0xf8,9);
        put(0xf0,2048);put(0x100,6144);put(0x108,3);shell[0x299]=3;
        auto descriptor=tglOwnedSfcOutputDescriptor(shell);
        auto get=[&](size_t offset){uint32_t value=0;std::memcpy(&value,descriptor.data()+offset,4);return value;};
        assert(get(0)==2 && get(4)==0x52 && get(8)==1920 && get(0xc)==1080);
        assert(get(0x10)==2048 && get(0x14)==4096 && get(0x18)==0);
        assert(get(0x1c)==7 && get(0x20)==9 && get(0x24)==5 && descriptor[0x30]==1);
        uintptr_t resource=0;std::memcpy(&resource,descriptor.data()+0x28,8);
        assert(resource==reinterpret_cast<uintptr_t>(shell.data()+0x148));
        for(size_t i=0x31;i<descriptor.size();++i) assert(descriptor[i]==0);
        put(0xe0,0);descriptor=tglOwnedSfcOutputDescriptor(shell);assert(get(0x24)==0);
        put(0xe0,1);put(0x100,0);put(0xf0,1);put(0x108,2);
        descriptor=tglOwnedSfcOutputDescriptor(shell);assert(get(0x24)==1);
        put(0x130,0x19);put(0xe0,2048);put(0xf0,0);put(0x100,2048*1080);
        put(0x108,0);
        assert(tglOwnedSfcPlanarOutputFits(0x19,shell.data()));
        put(0x108,0xffff-1080);
        assert(tglOwnedSfcPlanarOutputFits(0x19,shell.data()));
        put(0x108,0x10000-1080);
        assert(!tglOwnedSfcPlanarOutputFits(0x19,shell.data()));
        put(0x108,UINT32_MAX); // wrapped descriptor must not pass qualification
        assert(!tglOwnedSfcPlanarOutputFits(0x19,shell.data()));
        put(0x108,0);put(0xf0,2048*1080+1);
        assert(!tglOwnedSfcPlanarOutputFits(0x19,shell.data()));
        put(0xf0,0);put(0xe0,0);
        assert(!tglOwnedSfcPlanarOutputFits(0x19,shell.data()));
        put(0xe0,2048);put(0x130,0x52);
        assert(!tglOwnedSfcPlanarOutputFits(0x19,shell.data()));
        assert(tglOwnedSfcPlanarOutputFits(0x50,shell.data()));
        put(0xf0,0);put(0x100,0);
        for(uint32_t format:{0x19u,0x53u,0x52u,0x13u,0x12u,0x17u,0x14u}) {
            put(0x130,format);put(0x108,0xffff);
            assert(tglNativeSfcUsesPlaneOffset(format));
            assert(tglOwnedSfcPlanarOutputFits(format,shell.data()));
            put(0x108,0x10000);
            assert(!tglOwnedSfcPlanarOutputFits(format,shell.data()));
        }
        for(uint32_t format:{1u,2u,3u,4u,7u,0xdu,0x10u,0x15u,0x50u,0x51u})
            assert(!tglNativeSfcUsesPlaneOffset(format));
    }
    {
        constexpr uintptr_t base=0x10000000,os=0x20000000;
        struct Invoke {
            unsigned constructs=0,destroys=0;
            bool corrupt=false;
            void (*onConstruct)(void*) noexcept=nullptr;
            void* constructionContext=nullptr;
            void construct(uintptr_t entry,void *object,uintptr_t borrowed) noexcept {
                assert(entry==base+0x16bdd0 && borrowed==os);
                ++constructs;
                auto *bytes=static_cast<uint8_t*>(object);
                auto put=[&](size_t offset,auto value){std::memcpy(bytes+offset,&value,sizeof(value));};
                put(0,uintptr_t(base+0x759420));put(8,uintptr_t(base+0x3eae0));
                put(0x10,borrowed);put(0x18,uint16_t(16));put(0x1a,uint16_t(4));
                bytes[0x40]=corrupt?0:1;
                if(onConstruct) onConstruct(constructionContext);
            }
            void destroy(uintptr_t entry,void *object) noexcept {
                assert(entry==base+0x16bed0 && object);
                ++destroys;
            }
        } invoke;
        TglSfcLifetimeBinding lifetime{base,base+0x16bdd0,base+0x16bed0};
        TglSfcMhwBinding mhw;
        mhw.image=base;
        std::array<uint32_t,2> flags{1,0};
        auto read=[&](uintptr_t address,void *dst,size_t size) {
            if(address!=os+0x88 || size!=sizeof(flags)) return false;
            std::memcpy(dst,flags.data(),size);return true;
        };
        {
            std::array<unsigned,16> events{};size_t eventCount=0;
            struct TrackedSfc {
                Invoke& native;std::array<unsigned,16>& events;size_t& count;
                void construct(uintptr_t entry,void* object,uintptr_t borrowed) noexcept {
                    native.construct(entry,object,borrowed);
                }
                void destroy(uintptr_t entry,void* object) noexcept {
                    events.at(count++)=4;native.destroy(entry,object);
                }
            } tracked{invoke,events,eventCount};
            struct Cpu {
                alignas(8) std::array<uint8_t,0x1bc0> bytes{};
                std::array<unsigned,16>& events;size_t& count;bool fail=false,live=false;
                uintptr_t allocate(size_t size) {
                    assert(!live && size==bytes.size());
                    if(fail) return 0;
                    live=true;return reinterpret_cast<uintptr_t>(bytes.data());
                }
                void release(uintptr_t address) noexcept {
                    assert(live && address==reinterpret_cast<uintptr_t>(bytes.data()));
                    events.at(count++)=3;live=false;
                }
            } cpu{{},events,eventCount};
            struct ShellBackend {
                unsigned live=0,releases=0;
                uintptr_t nativeImage=0;
                unsigned failAt=99;
                void* owner=nullptr;
                void (*reenter)(void*)=nullptr;
                uintptr_t image() const noexcept { return nativeImage; }
                uintptr_t allocate(size_t size) noexcept {
                    assert(size==0x2a8);
                    if(reenter) reenter(owner);
                    if(live==failAt) return 0;
                    return ++live;
                }
                void release(uintptr_t address) noexcept {
                    assert(address && live); --live; ++releases;
                }
            } shellBackend;
            shellBackend.nativeImage=base;
            bool shellsTransferred=false;
            unsigned helperDestroys=0;
            unsigned reportDestroys=0;
            std::array<uint8_t,0xa0> nativeHelper{};
            void* activeOwner=nullptr;void(*reenterReset)(void*)=nullptr;
            auto cleanup=[&](void* object) noexcept {
                assert(object==cpu.bytes.data() && cpu.live);
                events.at(eventCount++)=1;
                if(reenterReset) reenterReset(activeOwner);
            };
            auto destroy=[&](uintptr_t entry,void* object) noexcept {
                assert(entry==base+0x12de30 && object==cpu.bytes.data() && cpu.live);
                if(shellsTransferred) {
                    constexpr size_t offsets[]={0x1b0,0x1b8,0xba8,0xbb0,0x680,0x688,0x690,0x698};
                    for(auto offset:offsets) {
                        uintptr_t pointer=0;
                        std::memcpy(&pointer,cpu.bytes.data()+offset,8);
                        shellBackend.release(pointer);
                        std::memset(cpu.bytes.data()+offset,0,8);
                    }
                    shellsTransferred=false;
                }
                events.at(eventCount++)=2;
                uintptr_t report=0;
                std::memcpy(&report,cpu.bytes.data()+0x88,8);
                if(report) {
                    assert(report==0x5678); ++reportDestroys;
                    std::memset(cpu.bytes.data()+0x88,0,8);
                }
                uintptr_t helper=0;
                std::memcpy(&helper,cpu.bytes.data()+0x1bb8,8);
                if(helper) {
                    assert(helper==reinterpret_cast<uintptr_t>(nativeHelper.data()));
                    ++helperDestroys;
                    std::memset(cpu.bytes.data()+0x1bb8,0,8);
                }
            };
            using Owner=TglOwnedVeboxBaseOwner<Cpu,TrackedSfc,decltype(cleanup),decltype(destroy)>;
            TglVeboxLifetimeBinding baseBinding{base,base+0x12d4e0,base+0x12de30};
            int32_t status=77,result=0;unsigned constructors=0;
            auto construct=[&](uintptr_t entry,void* object,void* nativeOs,void*,void* nativeSfc,
                void*,void*,void*,const void*,int32_t* out) noexcept {
                assert(entry==base+0x12d4e0 && object==cpu.bytes.data() &&
                    nativeOs==reinterpret_cast<void*>(os) && nativeSfc);
                const uintptr_t nativeTable=base+0x757c98;
                std::memcpy(object,&nativeTable,8);
                std::memcpy(static_cast<uint8_t*>(object)+0x10,&os,8);
                ++constructors;*out=result;
            };
            {
                Owner owner(cpu,tracked,cleanup,destroy);activeOwner=&owner;
                reenterReset=[](void* p){static_cast<Owner*>(p)->reset();};
                auto initialize=[&] {
                    return owner.initialize(baseBinding,lifetime,mhw,reinterpret_cast<void*>(os),
                        nullptr,reinterpret_cast<void*>(0x3000),nullptr,nullptr,nullptr,status,read,construct);
                };
                cpu.fail=true;
                assert(initialize()==TglOwnedBaseConstruction::NotCalled && !constructors && !cpu.live);
                assert(eventCount==1 && events[0]==4);eventCount=0;cpu.fail=false;
                result=31;
                assert(initialize()==TglOwnedBaseConstruction::ConstructedFailure && status==31);
                assert(!owner.constructedBase() && !cpu.live && eventCount==4);
                assert((std::array<unsigned,4>{events[0],events[1],events[2],events[3]}==
                        std::array<unsigned,4>{1,2,3,4}));
                eventCount=0;result=0;
                assert(initialize()==TglOwnedBaseConstruction::ConstructedSuccess && status==0);
                assert(owner.constructedBase()==cpu.bytes.data() && cpu.live && !eventCount);
                TglOwnedVeboxAllocationTail tail{0,0,23,31,true,0};
                TglOwnedVeboxFeatureResult featureResult;
                unsigned featureCalls=0,flagCalls=0;
                auto feature=[&](uintptr_t context,uint32_t id,void* data) noexcept {
                    assert(!context && id==0xb8); ++featureCalls;
                    owner.reset(); assert(owner.constructedBase() && !eventCount);
                    assert(!owner.configureAllocationTail(tail));
                    const uint32_t value=19;std::memcpy(data,&value,4); return 31;
                };
                auto flags=[&](void* nativeOs) noexcept -> uint32_t {
                    assert(nativeOs==reinterpret_cast<void*>(os)); ++flagCalls;
                    owner.reset(); assert(owner.constructedBase() && !eventCount);
                    return 1u<<20;
                };
                struct BoundFeature {
                    decltype(feature)& invoke;
                    uintptr_t nativeImage=0;
                    bool admitted=false;
                    bool bound() const noexcept { return admitted; }
                    uintptr_t image() const noexcept { return nativeImage; }
                    int operator()(uintptr_t context,uint32_t id,void* data) const noexcept {
                        return invoke(context,id,data);
                    }
                } boundFeature{feature,base};
                const auto beforeQuery=cpu.bytes;
                assert(!owner.configureAllocationTailFromNative(tail,featureResult,boundFeature,flags));
                assert(cpu.bytes==beforeQuery && !featureCalls && !flagCalls && featureResult.status==5);
                boundFeature.admitted=true;
                boundFeature.nativeImage=base+1;
                assert(!owner.configureAllocationTailFromNative(tail,featureResult,boundFeature,flags));
                assert(cpu.bytes==beforeQuery && !featureCalls && !flagCalls);
                boundFeature.nativeImage=base;
                assert(owner.configureAllocationTailFromNative(tail,featureResult,boundFeature,flags));
                assert(featureCalls==1 && flagCalls==1 && featureResult.status==31 && featureResult.value==19);
                uint32_t featureValue=0;
                std::memcpy(&featureValue,cpu.bytes.data()+0x1b9c,4);
                assert(featureValue==19 && cpu.bytes[0x1ba8]==1);
                shellBackend.owner=&owner;
                shellBackend.reenter=[](void* pointer) {
                    auto& owner=*static_cast<Owner*>(pointer);
                    owner.reset();
                    assert(owner.constructedBase());
                };
                shellBackend.failAt=3;
                assert(!owner.allocateCpuShells(shellBackend));
                assert(!shellBackend.live && shellBackend.releases==3 && !eventCount);
                shellBackend.failAt=99;
                shellBackend.reenter=nullptr;
                {
                    TglVeboxCpuShells<ShellBackend> shells(shellBackend);
                    assert(shells.allocate());
                    shellBackend.nativeImage=base+1;
                    assert(!owner.installCpuShells(shells) && shells.get(0));
                    shellBackend.nativeImage=base;
                    shells.reset();
                    assert(!shellBackend.live && shellBackend.releases==11);
                }
                shellBackend.reenter=[](void* pointer) {
                    auto& owner=*static_cast<Owner*>(pointer);
                    owner.reset();
                    assert(owner.constructedBase());
                };
                assert(owner.allocateCpuShells(shellBackend));
                shellsTransferred=true;
                assert(shellBackend.live==8 && shellBackend.releases==11 && !eventCount);
                shellBackend.reenter=nullptr;
                uint32_t helperAllocationSize=0xa0;
                bool helperSizeReadable=true;
                auto helperRead=[&](uintptr_t address,void* output,size_t size) noexcept {
                    if(address==base+0x126bf0 && size==4) {
                        if(!helperSizeReadable) return false;
                        std::memcpy(output,&helperAllocationSize,4);return true;
                    }
                    assert(address==base+0x126be0 && size==16);
                    constexpr uint8_t bytes[]={0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x40,
                        0x48,0x8b,0x35,0x39,0xd4,0x62,0x00,0xbf};
                    std::memcpy(output,bytes,16); return true;
                };
                unsigned helperCreates=0;
                auto helperExecutable=[&](uintptr_t entry,size_t size) noexcept {
                    return (entry==base+0x126be0 && size==16) ||
                        (entry==base+0x126bf0 && size==4);
                };
                auto createHelper=[&](uintptr_t entry) noexcept -> uintptr_t {
                    assert(entry==base+0x126be0);
                    ++helperCreates; owner.reset();
                    assert(owner.constructedBase() && !eventCount);
                    return helperCreates==1 ? 0 : reinterpret_cast<uintptr_t>(nativeHelper.data());
                };
                unsigned reportQueries=0;
                auto reportCallback=[&](void* child) noexcept -> uintptr_t {
                    assert(child==cpu.bytes.data()); ++reportQueries;
                    owner.reset(); assert(owner.constructedBase());
                    const uintptr_t report=0x5678;
                    std::memcpy(static_cast<uint8_t*>(child)+0x88,&report,8);
                    return report;
                };
                struct BoundReport {
                    decltype(reportCallback)& invoke;
                    uintptr_t nativeImage;
                    bool bound() const noexcept {return nativeImage!=0;}
                    uintptr_t image() const noexcept {return nativeImage;}
                    uintptr_t operator()(void* child) const noexcept {return invoke(child);}
                } queryReport{reportCallback,base+1};
                assert(!owner.allocateNativeHelper(helperRead,helperExecutable,createHelper,queryReport));
                assert(!helperCreates && !reportQueries);
                queryReport.nativeImage=base;
                helperAllocationSize=0x10;
                assert(!owner.allocateNativeHelper(helperRead,helperExecutable,createHelper,queryReport));
                assert(!helperCreates && !reportQueries);
                helperAllocationSize=0xa0;helperSizeReadable=false;
                assert(!owner.allocateNativeHelper(helperRead,helperExecutable,createHelper,queryReport));
                assert(!helperCreates && !reportQueries);
                helperSizeReadable=true;
                assert(!owner.allocateNativeHelper(helperRead,helperExecutable,createHelper,queryReport));
                assert(!reportQueries);
                assert(owner.allocateNativeHelper(helperRead,helperExecutable,createHelper,queryReport));
                assert(!owner.allocateNativeHelper(helperRead,helperExecutable,createHelper,queryReport));
                uintptr_t helperChild=0,helperReport=0;
                std::memcpy(&helperChild,nativeHelper.data()+8,8);
                std::memcpy(&helperReport,nativeHelper.data()+0x10,8);
                assert(helperChild==reinterpret_cast<uintptr_t>(cpu.bytes.data()) && helperReport==0x5678);
                assert(helperCreates==2 && reportQueries==1);
                std::array<uintptr_t,TglOwnedVeboxVtable::count> nativeTable{};
                for(size_t i=0;i<nativeTable.size();++i) nativeTable[i]=base+0x2000+i*8;
                for(auto offset:TglOwnedVeboxVtable::missing) nativeTable[offset/8]=0;
                nativeTable[0]=base+0x12e240;nativeTable[1]=base+0x12e250;
                nativeTable[0x40/8]=base+0x12e9c0;nativeTable[0x48/8]=base+0x12ea30;
                nativeTable[0x170/8]=base+0x12aba0;nativeTable[0x180/8]=base+0x12b1d0;
                nativeTable[0x160/8]=base+0x12a740;nativeTable[0x168/8]=base+0x12ab10;
                nativeTable[0xd0/8]=base+0x71e80;
                nativeTable[0x1b8/8]=base+0x71e80;
                std::array<uintptr_t,20> hooks{};
                for(size_t i=0;i<hooks.size();++i) hooks[i]=0x3000+i*16;
                bool readable=false;
                auto tableRead=[&](uintptr_t address,void* out,size_t size) noexcept {
                    owner.reset();assert(cpu.live && !eventCount); // admission reentry is inert
                    if(!readable) return false;
                    if(address==base+0x757c88 && size==16) {
                        const std::array<uintptr_t,2> metadata{0,base+0x757ed0};
                        std::memcpy(out,metadata.data(),size);return true;
                    }
                    if(address==base+0x757c98 && size==sizeof(nativeTable)) {
                        std::memcpy(out,nativeTable.data(),size);return true;
                    }
                    return false;
                };
                auto qualify=[](uintptr_t image) noexcept {return image==base;};
                auto executable=[](uintptr_t,size_t) noexcept {return true;};
                assert(!owner.installPrivateVtable(hooks,tableRead,qualify,executable));
                uintptr_t address=0;std::memcpy(&address,cpu.bytes.data(),8);
                assert(address==base+0x757c98);
                readable=true;
                assert(!owner.installPrivateVtable(hooks,tableRead,qualify,executable));
                std::memcpy(&address,cpu.bytes.data(),8);
                assert(address==base+0x757c98 && cpu.live && !eventCount);
                struct DeniedFormatRead {
                    bool operator()(uintptr_t,void*,size_t) const noexcept {return false;}
                };
                struct ModeCleanupContext {
                    void* child;unsigned calls=0;int status=5;
                    bool owns(void* candidate) const noexcept {return candidate==child;}
                    void freeResources() noexcept {++calls;status=31;}
                } modeCleanup{owner.constructedBase()};
                using ModeCleanupScope=TglOwnedVeboxScopedContext<ModeCleanupContext>;
                hooks=tglBindOwnedVeboxModeReleaseHook<ModeCleanupScope>(hooks);
                std::array<uint8_t,0x140> admissionParams{};
                std::array<uint8_t,0x28> admissionPass{};
                unsigned admissionCalls=0;bool admissionAllowed=false;
                struct AdmissionBackend {
                    std::array<uint8_t,0xd48> bytes{};unsigned initializes=0,destroys=0;
                    int status=0;
                    uintptr_t create() noexcept {return reinterpret_cast<uintptr_t>(bytes.data());}
                    bool validate(uintptr_t p) noexcept {return p==reinterpret_cast<uintptr_t>(bytes.data());}
                    int initialize(uintptr_t p) noexcept {
                        assert(validate(p));++initializes;bytes.fill(0);return status;
                    }
                    void destroy(uintptr_t p) noexcept {assert(validate(p));++destroys;}
                } admissionBackend;
                TglExecutionOwner admissionExecution(admissionBackend);
                assert(!admissionExecution.ensure());
                bool runAdmissionStages=false;unsigned admissionSelections=0;
                auto admissionPolicy=[&](void* child,void* params,void* pass) noexcept {
                    assert(child==owner.constructedBase() && params==&admissionParams && pass==&admissionPass);
                    ++admissionCalls;owner.reset();
                    assert(cpu.live && owner.constructedBase()==child && !eventCount);
                    assert(owner.withPrivateChild([](void*) noexcept {assert(false);return 0;})==5);
                    if(runAdmissionStages) {
                        TglOwnedSurfaceStorage source{},target{};
                        uint32_t width=64,height=16;
                        const uint32_t sourceType=1;std::memcpy(source.prefix.data(),&sourceType,4);
                        std::memcpy(source.prefix.data()+0xd8,&width,4);
                        std::memcpy(source.prefix.data()+0xdc,&height,4);
                        uint64_t sku=uint64_t{1}<<12;
                        TglVeboxAdmissionBorrowedInterfaces interfaces{
                            reinterpret_cast<uintptr_t>(&source),reinterpret_cast<uintptr_t>(&target),
                            reinterpret_cast<uintptr_t>(&sku),0};
                        auto* baseBytes=static_cast<uint8_t*>(child);
                        struct RestoreInterfaces {
                            uint8_t* base;std::array<uint8_t,24> saved{};
                            explicit RestoreInterfaces(uint8_t* p) noexcept:base(p) {std::memcpy(saved.data(),base+0x10,24);}
                            ~RestoreInterfaces(){std::memcpy(base+0x10,saved.data(),24);}
                        } restore(baseBytes);
                        std::memcpy(baseBytes+0x10,&interfaces.os,8);
                        std::memcpy(baseBytes+0x18,&interfaces.renderHal,8);
                        std::memcpy(baseBytes+0x20,&interfaces.sku,8);
                        uintptr_t sourceLink=reinterpret_cast<uintptr_t>(&source),targetLink=reinterpret_cast<uintptr_t>(&target);
                        std::memcpy(admissionPass.data()+0x10,&sourceLink,8);
                        std::memcpy(admissionParams.data()+0x98,&targetLink,8);
                        admissionParams[0x138]=1;admissionPass[8]=0;
                        auto read=[&](uintptr_t p,void* out,size_t n) noexcept {
                            const auto paramsBase=reinterpret_cast<uintptr_t>(admissionParams.data());
                            const auto passBase=reinterpret_cast<uintptr_t>(admissionPass.data());
                            const bool valid=(p==passBase+0x10 && n==8) ||
                                (p==paramsBase+0x98 && n==8) || (p==paramsBase+0xf0 && n==8) ||
                                (p==paramsBase+0x138 && n==1) || (p==paramsBase+0x10c && n==4) ||
                                (p==interfaces.sku && n==8);
                            if(valid) std::memcpy(out,reinterpret_cast<const void*>(p),n);
                            return valid;
                        };
                        bool admitted=false;
                        const int status=admissionExecution.withExecution([&](uintptr_t execution) noexcept {
                            uintptr_t linked=0;std::memcpy(&linked,static_cast<uint8_t*>(child)+0x88,8);
                            if(linked!=execution) return 5;
                            return admissionExecution.withLeasedAdmissionBytes(execution,[&](auto& bytes) noexcept {
                                admitted=tglRunCapturedSelectedVeboxAdmissionStages(owner,admissionExecution,
                                    child,admissionParams,admissionPass,source,target,interfaces,bytes,read,
                                    [&]() noexcept {return !admissionExecution.initializeLeasedAdmission(execution);},
                                    [&](uint8_t& flag) noexcept -> uint32_t {
                                        ++admissionSelections;
                                        assert(&flag==admissionPass.data()+8 && admissionBackend.initializes==2);
                                        flag=0x80;return 1;
                                    },
                                    [](auto&) noexcept {},
                                    [](auto&) noexcept {assert(false);},
                                    [&](auto& data) noexcept {
                                        uint32_t type=0;std::memcpy(&type,source.prefix.data(),4);
                                        tglPublishVeboxSfcAdmissionFlags(data,0,type);return true;
                                    });
                                return 0;
                            });
                        });
                        return !status && admitted;
                    }
                    return admissionAllowed;
                };
                using AdmissionContext=TglOwnedVeboxAdmissionContext<std::remove_reference_t<decltype(owner)>,decltype(admissionPolicy)>;
                AdmissionContext admission(owner,owner.constructedBase(),&admissionParams,&admissionPass,admissionPolicy);
                using AdmissionScope=TglOwnedVeboxScopedContext<AdmissionContext>;
                hooks=tglBindOwnedVeboxAdmissionHook<AdmissionScope>(hooks);
                const auto unbound=hooks;
                assert(owner.withPrivateChild([](void*) noexcept {assert(false);return 0;})==5);
                hooks=tglBindOwnedVeboxQueryHooks<DeniedFormatRead>(hooks);
                assert(owner.installPrivateVtableWithQueries<DeniedFormatRead>(
                    unbound,tableRead,qualify,executable));
                {
                    struct BorrowBackend {
                        unsigned destroys=0;
                        uintptr_t create() noexcept {assert(false);return 0;}
                        bool validate(uintptr_t p) noexcept {return p==0x5678;}
                        int initialize(uintptr_t) noexcept {assert(false);return 5;}
                        void destroy(uintptr_t) noexcept {++destroys;assert(false);}
                    } borrowBackend;
                    TglExecutionOwner borrowed(borrowBackend);unsigned calls=0;
                    bool readable=true;
                    auto readHelper=[&](uintptr_t p,void* out,size_t size) noexcept {
                        assert(p==reinterpret_cast<uintptr_t>(nativeHelper.data())+0x10 && size==8);
                        if(!readable) return false;
                        std::memcpy(out,nativeHelper.data()+0x10,8);return true;
                    };
                    auto useReport=[&](void* child,uintptr_t report) noexcept {
                        ++calls;assert(child==owner.constructedBase() && report==0x5678);
                        assert(borrowed.hasExecutionLease(report));
                        owner.reset();borrowed.reset();assert(cpu.live && !eventCount);
                        return 31;
                    };
                    assert(owner.withNativeOwnedExecution(borrowed,readHelper,useReport)==31);
                    assert(calls==1 && !borrowed.get() && !borrowBackend.destroys);
                    readable=false;
                    assert(owner.withNativeOwnedExecution(borrowed,readHelper,useReport)==5 && calls==1);
                    readable=true;const uintptr_t wrong=0x5679;
                    std::memcpy(nativeHelper.data()+0x10,&wrong,8);
                    assert(owner.withNativeOwnedExecution(borrowed,readHelper,useReport)==5 && calls==1);
                    std::memcpy(nativeHelper.data()+0x10,&helperReport,8);
                    assert(owner.withNativeOwnedExecution(borrowed,readHelper,
                        [&](void* child,uintptr_t) noexcept {
                            std::memcpy(static_cast<uint8_t*>(child)+0x88,&wrong,8);return 0;
                        })==5);
                    std::memcpy(cpu.bytes.data()+0x88,&helperReport,8);
                    uintptr_t stillOwned=0;std::memcpy(&stillOwned,cpu.bytes.data()+0x88,8);
                    assert(stillOwned==helperReport && helperReport==0x5678 && cpu.live);
                }
                std::memcpy(&address,cpu.bytes.data(),8);
                assert(address!=base+0x757c98 && address);
                uintptr_t supportAddress=0;
                std::memcpy(&supportAddress,reinterpret_cast<void*>(address+0x1b8),8);
                using Support=bool(*)(void*) noexcept;
                assert(reinterpret_cast<Support>(supportAddress)(owner.constructedBase()));
                uintptr_t formatAddress=0;
                std::memcpy(&formatAddress,reinterpret_cast<void*>(address+0x90),8);
                using SurfaceFormat=bool(*)(void*,uintptr_t) noexcept;
                assert(!reinterpret_cast<SurfaceFormat>(formatAddress)(owner.constructedBase(),0x5000));
                uintptr_t admissionAddress=0;
                std::memcpy(&admissionAddress,reinterpret_cast<void*>(address+0x28),8);
                const auto admissionCallback=reinterpret_cast<bool(*)(void*,void*,void*) noexcept>(admissionAddress);
                assert(!admissionCallback(owner.constructedBase(),&admissionParams,&admissionPass) && !admissionCalls);
                {AdmissionScope scope(owner.constructedBase(),admission);assert(scope.bound());
                    assert(!admissionCallback(owner.constructedBase(),&admissionParams,&admissionPass) && !admissionCalls);
                }
                assert(owner.withPrivateChild([&](void* actual) noexcept {
                    AdmissionScope scope(actual,admission);assert(scope.bound());
                    assert(!admissionCallback(actual,&admissionPass,&admissionParams) && !admissionCalls);
                    assert(!admissionCallback(actual,&admissionParams,&admissionPass) && admissionCalls==1);
                    admissionAllowed=true;
                    assert(admissionCallback(actual,&admissionParams,&admissionPass) && admissionCalls==2);
                    return 0;
                })==0);
                assert(!admissionCallback(owner.constructedBase(),&admissionParams,&admissionPass) && admissionCalls==2);
                runAdmissionStages=true;
                assert(owner.withPrivateChild([&](void* actual) noexcept {
                    auto* field=static_cast<uint8_t*>(actual)+0x88;
                    uintptr_t original=0;std::memcpy(&original,field,8);
                    const auto execution=admissionExecution.get();std::memcpy(field,&execution,8);
                    AdmissionScope scope(actual,admission);assert(scope.bound());
                    assert(admissionCallback(actual,&admissionParams,&admissionPass));
                    assert(admissionBackend.initializes==2 && admissionBackend.bytes[0x17]==1 && admissionSelections==1);
                    admissionBackend.status=31;
                    assert(!admissionCallback(actual,&admissionParams,&admissionPass));
                    assert(admissionBackend.initializes==3 && !admissionBackend.destroys && admissionSelections==1);
                    // Unlink before releasing the poisoned execution owner.
                    std::memcpy(field,&original,8);admissionExecution.reset();
                    assert(admissionBackend.destroys==1);return 0;
                })==0);
                runAdmissionStages=false;
                {
                    std::array<uint8_t,16> history{};history[0]=1;
                    TglOwnedSurfaceStorage source{};
                    uint32_t count=1;std::memcpy(source.prefix.data()+0xb4,&count,4);
                    auto* child=static_cast<uint8_t*>(owner.constructedBase());
                    uintptr_t originalHistory=0;
                    uint32_t originalField=0;
                    std::memcpy(&originalHistory,child+0x90,8);
                    std::memcpy(&originalField,child+0x1b5c,4);
                    assert(!tglUpdateOwnedVeboxHistory(owner,child,history,source,nullptr,nullptr,0));
                    assert(owner.withPrivateChild([&](void* actual) noexcept {
                        uintptr_t borrowed=reinterpret_cast<uintptr_t>(history.data());
                        std::memcpy(child+0x90,&borrowed,8);
                        assert(tglUpdateOwnedVeboxHistory(owner,actual,history,source,nullptr,nullptr,0));
                        uint32_t mode=0,field=0;std::memcpy(&mode,history.data()+4,4);
                        std::memcpy(&field,child+0x1b5c,4);assert(mode==1 && field==4);
                        owner.reset();assert(owner.constructedBase()==actual && cpu.live);
                        std::memcpy(child+0x90,&originalHistory,8);
                        std::memcpy(child+0x1b5c,&originalField,4);
                        return 0;
                    })==0);
                    assert(!tglUpdateOwnedVeboxHistory(owner,child,history,source,nullptr,nullptr,0));
                }
                uintptr_t cleanupAddress=0;
                std::memcpy(&cleanupAddress,reinterpret_cast<void*>(address+0x60),8);
                const auto cleanupCallback=reinterpret_cast<void(*)(void*) noexcept>(cleanupAddress);
                cleanupCallback(owner.constructedBase());assert(!modeCleanup.calls);
                assert(owner.withPrivateChild([&](void* actual) noexcept {
                    ModeCleanupScope scope(actual,modeCleanup);assert(scope.bound());
                    cleanupCallback(nullptr);cleanupCallback(&modeCleanup);
                    assert(!modeCleanup.calls);
                    cleanupCallback(actual);
                    owner.reset();assert(cpu.live && owner.constructedBase()==actual && !eventCount);
                    return modeCleanup.status;
                })==31);
                assert(modeCleanup.calls==1);
                cleanupCallback(owner.constructedBase());assert(modeCleanup.calls==1);
                for(size_t i=0;i<hooks.size();++i) {
                    uintptr_t target=0;
                    std::memcpy(&target,reinterpret_cast<void*>(address+TglOwnedVeboxVtable::required[i]),8);
                    assert(target==hooks[i]);
                }
                assert(!owner.installPrivateVtable(hooks,tableRead,qualify,executable));
                {
                    struct BuilderContext {TglOwnedVeboxProducerPass pass;};
                    using Resolve=TglOwnedVeboxScopedContext<BuilderContext>;
                    BuilderContext context;
                    auto* child=owner.constructedBase();
                    unsigned builders=0;
                    for(int nativeResult : {0,31,-7}) for(int producerResult : {0,31}) {
                        assert(!Resolve::get(child));
                        const int status=tglWithOwnedVeboxBuilder(owner,context,[&](void* actual) noexcept {
                            ++builders;
                            assert(actual==child && Resolve::get(actual)==&context);
                            owner.reset();assert(cpu.live && owner.constructedBase()==child && !eventCount);
                            assert(tglWithOwnedVeboxBuilder(owner,context,[](void*) noexcept {assert(false);return 0;})==5);
                            context.pass.produce([&]() noexcept {return producerResult;});
                            if(!nativeResult) context.pass.gate([]() noexcept {return 0;});
                            return nativeResult;
                        });
                        assert(status==(nativeResult?nativeResult:producerResult));
                        assert(!Resolve::get(child) && !eventCount);
                    }
                    assert(builders==6);
                    for(int prepareResult:{0,31,-7}) for(int setupResult:{0,31,-7})
                    for(int builderResult:{0,31,-7}) {
                        unsigned stages=0;
                        const int result=tglWithOwnedVeboxFrameStages(owner,context,
                            [&](void* actual) noexcept {
                                assert(stages++==0 && actual==child && Resolve::get(actual)==&context);
                                owner.reset();assert(cpu.live && owner.constructedBase()==child);
                                assert(context.pass.produce([]() noexcept {return 0;})==0);
                                return prepareResult;
                            },[&](void* actual) noexcept {
                                assert(stages++==1 && actual==child && Resolve::get(actual)==&context);
                                return setupResult;
                            },[&](void* actual) noexcept {
                                assert(stages++==2 && actual==child && Resolve::get(actual)==&context);
                                context.pass.gate([]() noexcept {return 0;});
                                return builderResult;
                            });
                        assert(result==(prepareResult ? prepareResult : setupResult ? setupResult : builderResult));
                        assert(stages==unsigned(prepareResult ? 1 : setupResult ? 2 : 3));
                        assert(!Resolve::get(child) && cpu.live && !eventCount);
                    }
                    // An ignored preparation-producer error survives setup and
                    // reaches the final owned gate; it cannot become success.
                    assert(tglWithOwnedVeboxFrameStages(owner,context,
                        [&](void*) noexcept {context.pass.produce([]() noexcept {return 31;});return 0;},
                        [](void*) noexcept {return 0;},
                        [&](void*) noexcept {return context.pass.gate([]() noexcept {return 0;});})==31);
                    assert(!Resolve::get(child));
                    for(int setupResult : {0,31,-7}) for(int builderResult : {0,31,-7}) {
                        unsigned setups=0,invocations=0;
                        const int result=tglWithOwnedVeboxPreparedBuilder(owner,context,
                            [&](void* actual) noexcept {
                                ++setups;
                                assert(actual==child && Resolve::get(actual)==&context);
                                owner.reset();assert(cpu.live && owner.constructedBase()==child && !eventCount);
                                assert(tglWithOwnedVeboxPreparedBuilder(owner,context,
                                    [](void*) noexcept {assert(false);return 0;},
                                    [](void*) noexcept {assert(false);return 0;})==5);
                                return setupResult;
                            },[&](void* actual) noexcept {
                                ++invocations;
                                assert(actual==child && Resolve::get(actual)==&context && setups==1);
                                context.pass.gate([]() noexcept {return 0;});
                                return builderResult;
                            });
                        assert(result==(setupResult?setupResult:builderResult));
                        assert(setups==1 && invocations==unsigned(setupResult==0));
                        assert(!Resolve::get(child) && cpu.live && !eventCount);
                    }
                    assert(tglWithOwnedVeboxBuilder(owner,context,[](void*) noexcept {return 0;})==5);
                    // A conflicting external scope cannot be silently replaced;
                    // owner lease is unwound without calling the builder.
                    {
                        BuilderContext other;int foreign=0;Resolve occupied(&foreign,other);
                        assert(occupied.bound());
                        assert(tglWithOwnedVeboxBuilder(owner,context,[](void*) noexcept {assert(false);return 0;})==5);
                        assert(Resolve::get(&foreign)==&other && !Resolve::get(child));
                    }
                    assert(!Resolve::get(child) && cpu.live && !eventCount);
                }
                TglCscCoefficients leasedCsc;
                leasedCsc.matrix.fill(3);leasedCsc.inputOffsets.fill(4);leasedCsc.outputOffsets.fill(5);
                assert(!owner.publishCsc(leasedCsc));
                assert(!owner.hasPrivateChildLease(reinterpret_cast<uintptr_t>(owner.constructedBase())));
                for(int nativeResult : {0,31,-7}) {
                    assert(owner.withPrivateChild([&](void* child) noexcept {
                        assert(child==owner.constructedBase() && cpu.live && !eventCount);
                        const auto beforeCsc=cpu.bytes;
                        assert(owner.hasPrivateChildLease(reinterpret_cast<uintptr_t>(child)));
                        assert(!owner.hasPrivateChildLease(reinterpret_cast<uintptr_t>(child)+8));
                        assert(owner.hasPrivateChildImageLease(reinterpret_cast<uintptr_t>(child),base));
                        assert(!owner.hasPrivateChildImageLease(reinterpret_cast<uintptr_t>(child),base+0x1000));
                        {
                            const auto snapshot=cpu.bytes;
                            std::array<uint8_t,0x15> execution{};
                            unsigned queries=0;
                            auto query=[&]() noexcept -> bool {
                                if(queries==1) assert(cpu.bytes[0x1b88]==1);
                                ++queries;owner.reset();assert(cpu.live);return true;
                            };
                            assert(tglPublishLeasedVeboxRenderStatisticsFlags(owner,child,execution,0,query)==0);
                            assert(queries==2 && cpu.bytes[0x1b88]==1 && cpu.bytes[0x1b89]==0);
                            for(size_t i=0;i<cpu.bytes.size();++i)
                                if(i!=0x1b88 && i!=0x1b89) assert(cpu.bytes[i]==snapshot[i]);
                            execution[9]=1;const auto skipped=cpu.bytes;
                            assert(tglPublishLeasedVeboxRenderStatisticsFlags(owner,child,execution,1,query)==0);
                            assert(queries==2 && cpu.bytes==skipped);
                            cpu.bytes[0x1b88]=snapshot[0x1b88];cpu.bytes[0x1b89]=snapshot[0x1b89];
                        }
                        TglVeboxRenderBinding render{base,base+0x12c9d0};
                        int argument1=0,argument2=0;unsigned renders=0;
                        const auto invokeRender=[&](uintptr_t entry,void* actual,void* a,void* b) noexcept -> int {
                            assert(entry==base+0x12c9d0 && actual==child && a==&argument1 && b==&argument2);
                            ++renders;owner.reset();assert(owner.constructedBase()==child);
                            return nativeResult;
                        };
                        assert(tglInvokeLeasedVeboxRender(render,owner,child,&argument1,&argument2,invokeRender)==nativeResult);
                        // Actual tracked surface owners plus the actual child
                        // owner, with a CPU allocation fixture (not GPU proof).
                        struct SurfaceBackend {
                            unsigned allocated=0,released=0;
                            uint32_t resourceType() const noexcept {return 1;}
                            int allocate(std::array<uint8_t,0x148>& storage,
                                const TglResourceKey& key,uint32_t* width) noexcept {
                                storage.fill(0);storage[0]=1;*width=key.width;
                                ++allocated;return 0;
                            }
                            bool backed(const std::array<uint8_t,0x148>& storage) noexcept {
                                return storage[0]==1;
                            }
                            void release(std::array<uint8_t,0x148>& storage) noexcept {
                                assert(storage[0]==1);storage.fill(0);++released;
                            }
                        } surfaceBackend;
                        {
                            TglOwnedAllocatedSurface<SurfaceBackend> sourceOwner(surfaceBackend);
                            TglOwnedAllocatedSurface<SurfaceBackend> targetOwner(surfaceBackend);
                            TglResourceKey key{};key.width=1920;key.height=1080;key.format=0x3d;
                            assert(sourceOwner.ensure(key)==0 && targetOwner.ensure(key)==0);
                            assert(sourceOwner.withSurface([&](const auto& source) noexcept {
                                assert(targetOwner.withSurface([&](const auto& target) noexcept {
                                    unsigned calls=0;
                                    auto surfaceRender=[&](uintptr_t entry,void* c,void* a,void* b) noexcept -> int {
                                        assert(entry==render.entry && c==child && a==&argument1);
                                        uintptr_t input=0,output=0;
                                        const auto* descriptor=static_cast<uint8_t*>(b);
                                        std::memcpy(&input,descriptor+0x10,8);
                                        std::memcpy(&output,descriptor+0x18,8);
                                        assert(input==reinterpret_cast<uintptr_t>(&source) &&
                                            output==reinterpret_cast<uintptr_t>(&target));
                                        sourceOwner.reset();targetOwner.reset();owner.reset();
                                        assert(!surfaceBackend.released && cpu.live);
                                        assert(sourceOwner.hasSurfaceLease(source) && targetOwner.hasSurfaceLease(target));
                                        assert(!sourceOwner.withSurface([](const auto&) noexcept {assert(false);}));
                                        ++calls;return nativeResult;
                                    };
                                    assert(tglInvokeLeasedVeboxSurfaceRender(render,owner,child,&argument1,
                                        sourceOwner,source,targetOwner,target,surfaceRender)==nativeResult);
                                    assert(calls==1 && !surfaceBackend.released);
                                }));
                            }));
                            sourceOwner.reset();targetOwner.reset();
                            assert(surfaceBackend.allocated==2 && surfaceBackend.released==2);
                        }
                        assert(surfaceBackend.released==2); // no destructor double release
                        render.image+=0x1000;render.entry+=0x1000;
                        assert(tglInvokeLeasedVeboxRender(render,owner,child,&argument1,&argument2,invokeRender)==5 && renders==1);
                        assert(owner.publishCsc(leasedCsc));
                        assert(std::memcmp(cpu.bytes.data()+0xa0,leasedCsc.matrix.data(),0x24)==0);
                        assert(std::memcmp(cpu.bytes.data()+0xc4,leasedCsc.inputOffsets.data(),0xc)==0);
                        assert(std::memcmp(cpu.bytes.data()+0xd0,leasedCsc.outputOffsets.data(),0xc)==0);
                        for(size_t i=0;i<cpu.bytes.size();++i)
                            if(i<0xa0 || i>=0xdc) assert(cpu.bytes[i]==beforeCsc[i]);
                        owner.reset();
                        assert(owner.constructedBase()==child && cpu.live && !eventCount);
                        assert(owner.withPrivateChild([](void*) noexcept {assert(false);return 0;})==5);
                        assert(!owner.installPrivateVtable(hooks,tableRead,qualify,executable));
                        return nativeResult;
                    })==nativeResult);
                    assert(!owner.publishCsc(leasedCsc));
                }
                TestVpCpuView vp;
                {
                    struct Cpu {
                        std::array<uint8_t,0xc00> bytes{};bool live=false;
                        uintptr_t allocate(size_t n) noexcept {
                            assert(!live && n<=bytes.size());live=true;
                            return reinterpret_cast<uintptr_t>(bytes.data());
                        }
                        void release(uintptr_t p) noexcept {
                            assert(live && p==reinterpret_cast<uintptr_t>(bytes.data()));live=false;
                        }
                    } stateCpu,avsCpu;
                    struct Binding {
                        uintptr_t image=0x100000;
                        bool validateConstructedObject(uintptr_t,uintptr_t,
                            bool(*)(uintptr_t,void*,size_t) noexcept) const noexcept {return true;}
                    } binding;
                    auto noRead=+[](uintptr_t,void*,size_t) noexcept {return false;};
                    TglOwnedVpSfcCpuGraph<Cpu,Cpu> graph(stateCpu,avsCpu);
                    assert(graph.initialize(0x7000,0x8000,0x3000,binding,noRead,1,0));
                    TglOwnedVpFramePreparation frame{};
                    int source=0,data=0,command=0;unsigned producers=0,emissions=0;
                    assert(owner.withBorrowedVpCpuScope([&](auto borrow) noexcept {
                        return graph.withPreparedCallbackPass(frame,0,0x4000,0x2000,0,&source,&data,
                            [&](void*,const auto&,auto&) noexcept -> int {++producers;return 0;},
                            [](void*,const auto&,auto&) noexcept -> int {assert(false);return 0;},
                            [](void*,const auto&) noexcept {assert(false);},
                            [&](void* child,void*,void*) noexcept -> int {
                                uintptr_t linked=0;std::memcpy(&linked,cpu.bytes.data()+0x80,8);
                                assert(linked==reinterpret_cast<uintptr_t>(child) && producers==1);
                                graph.reset();owner.reset();assert(cpu.live && stateCpu.live && avsCpu.live);
                                ++emissions;return -17;
                            },
                            [&](void* child,auto setup,auto send) noexcept -> int {
                                assert(!producers);
                                return borrow(child,[&](void* parent,void* actual) noexcept -> int {
                                    assert(parent==owner.constructedBase() && actual==child);
                                    assert(setup(child,&source,reinterpret_cast<void*>(0x2000),&data)==0);
                                    return send(child,&data,&command);
                                });
                            });
                    })==-17);
                    assert(producers==1 && emissions==1);
                    uintptr_t linked=1;std::memcpy(&linked,cpu.bytes.data()+0x80,8);assert(!linked);
                    assert(!owner.hasPrivateChildLease(reinterpret_cast<uintptr_t>(owner.constructedBase())));
                    graph.reset();assert(!stateCpu.live && !avsCpu.live);
                }
                {
                    const auto before=cpu.bytes;
                    const uintptr_t parent=reinterpret_cast<uintptr_t>(cpu.bytes.data());
                    for(uintptr_t address : {parent,parent+1,parent+0x1bbf,parent-0x9cf,
                        UINTPTR_MAX-0x9cf}) {
                        vp.overrideAddress=address;
                        assert(owner.withBorrowedVpCpuView(vp,true,[](void*,void*) noexcept {
                            assert(false);return 0;
                        })==5);
                        assert(cpu.bytes==before && !vp.busy);
                    }
                    vp.overrideAddress=0;
                }
                for(int result:{0,31,-7}) {
                    assert(owner.withBorrowedVpCpuView(vp,true,[&](void* parent,void* child) noexcept {
                        uintptr_t linked=0;
                        std::memcpy(&linked,static_cast<uint8_t*>(parent)+0x80,8);
                        assert(linked==reinterpret_cast<uintptr_t>(child) && vp.busy);
                        owner.reset();assert(cpu.live && !eventCount);
                        assert(owner.withBorrowedVpCpuView(vp,true,[](void*,void*) noexcept {
                            assert(false);return 0;
                        })==5);
                        return result;
                    })==result);
                    uintptr_t linked=1;std::memcpy(&linked,cpu.bytes.data()+0x80,8);
                    assert(!linked && !vp.busy);
                }
                vp.deny=true;
                assert(owner.withBorrowedVpCpuView(vp,true,[](void*,void*) noexcept {
                    assert(false);return 0;
                })==5);
                vp.deny=false;
                const uintptr_t existing=0x1234;
                std::memcpy(cpu.bytes.data()+0x80,&existing,8);
                assert(owner.withBorrowedVpCpuView(vp,true,[](void*,void*) noexcept {
                    assert(false);return 0;
                })==5);
                uintptr_t linked=0;std::memcpy(&linked,cpu.bytes.data()+0x80,8);
                assert(linked==existing);
                linked=0;std::memcpy(cpu.bytes.data()+0x80,&linked,8);
                assert(owner.withBorrowedVpCpuView(vp,false,[](void* parent,void*) noexcept {
                    std::memset(static_cast<uint8_t*>(parent)+0x80,0,8);return -7;
                })==5);
                {
                    struct GraphCpu {
                        alignas(8) std::array<uint8_t,0xc00> bytes{};
                        size_t expected;bool live=false;
                        uintptr_t allocate(size_t n) {
                            assert(n==expected && !live);live=true;
                            return reinterpret_cast<uintptr_t>(bytes.data());
                        }
                        void release(uintptr_t p) noexcept {
                            assert(live && p==reinterpret_cast<uintptr_t>(bytes.data()));live=false;
                        }
                    } stateCpu{{},0xf8},avsCpu{{},0xc00};
                    struct Binding {
                        uintptr_t image=0x100000;
                        bool validateConstructedObject(uintptr_t,uintptr_t,
                            bool (*)(uintptr_t,void*,size_t)) const noexcept {return true;}
                    } vpBinding;
                    TglOwnedVpSfcCpuGraph<GraphCpu,GraphCpu> graph(stateCpu,avsCpu);
                    auto never=[](void*,void*) noexcept {assert(false);return 0;};
                    assert(owner.withBorrowedVpCpuView(graph,true,never)==5);
                    auto noRead=+[](uintptr_t,void*,size_t) noexcept {return false;};
                    assert(graph.initialize(0x1000,0x2000,0x3000,vpBinding,noRead,1,0));
                    assert(owner.withBorrowedVpCpuView(graph,true,[&](void* parent,void* child) noexcept {
                        uintptr_t link=0,state=0,avs=0;
                        std::memcpy(&link,static_cast<uint8_t*>(parent)+0x80,8);
                        std::memcpy(&state,static_cast<uint8_t*>(child)+0x938,8);
                        std::memcpy(&avs,static_cast<uint8_t*>(child)+0x38,8);
                        assert(link==reinterpret_cast<uintptr_t>(child));
                        assert(state==reinterpret_cast<uintptr_t>(stateCpu.bytes.data()));
                        assert(avs==reinterpret_cast<uintptr_t>(avsCpu.bytes.data()));
                        graph.reset();owner.reset();
                        assert(stateCpu.live && avsCpu.live && cpu.live && !eventCount);
                        assert(graph.prepare([](auto&,auto&,auto&) noexcept {assert(false);return 0;})==5);
                        return -43;
                    })==-43);
                    std::memcpy(&linked,cpu.bytes.data()+0x80,8);assert(!linked);
                    graph.reset();assert(!stateCpu.live && !avsCpu.live && cpu.live);
                }
                owner.reset();owner.reset();
                assert(!owner.publishCsc(leasedCsc) && !owner.hasPrivateChildLease(0));
                assert(!cpu.live && !owner.constructedBase() && eventCount==4);
                assert(!shellBackend.live && shellBackend.releases==19);
                assert(helperDestroys==1);
                assert(reportDestroys==1);
                assert(owner.withPrivateChild([](void*) noexcept {assert(false);return 0;})==5);
                assert((std::array<unsigned,4>{events[0],events[1],events[2],events[3]}==
                        std::array<unsigned,4>{1,2,3,4}));
            }
            assert(eventCount==4 && constructors==2);
            invoke.constructs=invoke.destroys=0;
        }
        {
            TglOwnedSfcMhw<Invoke> owner(invoke);
            assert(!owner.get());
            unsigned reentries=0;
            struct ConstructionReentry {
                TglOwnedSfcMhw<Invoke>& owner;
                TglSfcLifetimeBinding& lifetime;
                TglSfcMhwBinding& mhw;
                decltype(read)& reader;
                unsigned& count;
            } construction{owner,lifetime,mhw,read,reentries};
            invoke.constructionContext=&construction;
            invoke.onConstruct=[](void* opaque) noexcept {
                auto& c=*static_cast<ConstructionReentry*>(opaque);
                ++c.count;c.owner.reset();assert(!c.owner.get());
                assert(!c.owner.initialize(c.lifetime,c.mhw,os,c.reader));
            };
            auto reentrantRead=[&](uintptr_t address,void* out,size_t bytes) {
                ++reentries;owner.reset();
                assert(!owner.get());
                assert(!owner.initialize(lifetime,mhw,os,read));
                return read(address,out,bytes);
            };
            assert(owner.initialize(lifetime,mhw,os,reentrantRead) && owner.get());
            invoke.onConstruct=nullptr;invoke.constructionContext=nullptr;
            assert(reentries && invoke.destroys==0);
            assert(invoke.constructs==1 && invoke.destroys==0);
            const std::array<uintptr_t,11> offsets{
                0x16bed0,0x16bef0,0x1681e0,0x168390,0x16a580,0x16a770,
                0x16a8a0,0x16b190,0x16b390,0x16b590,0xfe6c0};
            for(size_t i=0;i<offsets.size();++i) mhw.entries[i]=base+offsets[i];
            struct CommandInvoke {
                unsigned calls=0;uintptr_t last=0;
                uintptr_t lockEntry=0;
                int result=-37;
                std::array<uintptr_t,128> sequence{};
                void *expected=nullptr,*buffer=nullptr;
                void (*onCommand)(void*)=nullptr;void* commandContext=nullptr;
                int parameters(uintptr_t entry,void *object,void *command,const void *params) {
                    assert(object==expected && command==buffer && params);
                    if(onCommand) onCommand(commandContext);
                    if(entry==lockEntry) {
                        const auto* lock=static_cast<const uint8_t*>(params);
                        assert(lock[0]==1 && lock[8]==1);
                    }
                    sequence.at(calls++)=entry;last=entry;return result;
                }
                int state(uintptr_t entry,void *object,void *command,const void *params,const void *out) {
                    assert(out && static_cast<const uint8_t*>(params)[0xf7]==0x5a);
                    if(result==0) assert(static_cast<const uint8_t*>(out)[0x37]==0x99);
                    return parameters(entry,object,command,params);
                }
                int frame(uintptr_t entry,void *object,void *command,uint8_t mode) {
                    assert(mode==1);return parameters(entry,object,command,&mode);
                }
            } commandInvoke;
            int command=0;commandInvoke.expected=owner.get();commandInvoke.buffer=&command;
            commandInvoke.lockEntry=base+0x1681e0;
            TglOwnedSfcCommandBackend<Invoke,CommandInvoke> commands(mhw,owner,&command,commandInvoke);
            struct CommandReentry {
                TglOwnedSfcMhw<Invoke>& owner;
                decltype(commands)& commands;
                unsigned count=0;
            } commandReentry{owner,commands};
            commandInvoke.commandContext=&commandReentry;
            commandInvoke.onCommand=[](void* opaque) {
                auto& c=*static_cast<CommandReentry*>(opaque);++c.count;
                void* live=c.owner.get();c.owner.reset();assert(c.owner.get()==live);
                assert(c.commands.frameStart()==5);
                assert(c.owner.withObject([](void*){assert(false);return 0;})==5);
            };
            auto lock=tglPrepareOwnedSfcLockParameters(1,0,1);
            std::array<uint8_t,16> avs{};
            std::array<uint8_t,64> ief{};TglOwnedSfcStateParametersPacket state{};
            static_assert(sizeof(state)==0xf8,"TGL State reads full pointer at f0");
            state[0xf7]=0x5a;
            std::array<uint8_t,0x38> output{};
            TglOwnedSfcYParameters y{};TglOwnedSfcUvParameters uv{};
            assert(commands.lock(lock)==-37 && commandInvoke.last==base+0x1681e0);
            assert(commands.state(state,output)==-37 && commandInvoke.last==base+0x168390);
            assert(commands.avs(avs)==-37 && commandInvoke.last==base+0x16a580);
            assert(commands.ief(ief)==-37 && commandInvoke.last==base+0x16a8a0);
            assert(commands.y(y)==-37 && commandInvoke.last==base+0x16b390);
            assert(commands.uv(uv)==-37 && commandInvoke.last==base+0x16b190);
            assert(commands.frameStart()==-37 && commandInvoke.last==base+0x16a770);
            assert(commandReentry.count==7 && invoke.destroys==0);
            commandInvoke.onCommand=nullptr;commandInvoke.commandContext=nullptr;
            ++mhw.entries[5];assert(commands.frameStart()==5 && commandInvoke.calls==7);--mhw.entries[5];
            // Exercise the scheduler through the actual typed backend/owner,
            // not the independent scheduler mock. Still no native GPU call.
            commandInvoke.result=0;
            TglOwnedSfcCommandParameters prepared{};prepared.state=state;prepared.lock=lock;
            std::array<uint8_t,0x2a8> shell{};
            unsigned platforms=0;
            auto platform=[&](std::array<uint8_t,0x38>& descriptor) {
                ++platforms;descriptor[0x37]=0x99;return 0;
            };
            for(unsigned flags=0;flags<16;++flags) {
                const auto first=commandInvoke.calls;
                assert(tglSendOwnedSfcPreparedCommands(commands,prepared,shell,
                    flags&1,(flags>>1)&1,(flags>>2)&1,(flags>>3)&1,platform)==0);
                unsigned index=first;
                assert(commandInvoke.sequence[index++]==base+0x1681e0);
                assert(commandInvoke.sequence[index++]==base+0x168390);
                if(flags&3) {
                    assert(commandInvoke.sequence[index++]==base+0x16a580);
                    assert(commandInvoke.sequence[index++]==base+0x16b390);
                    assert(commandInvoke.sequence[index++]==base+0x16b190);
                }
                if(flags&12) assert(commandInvoke.sequence[index++]==base+0x16a8a0);
                assert(commandInvoke.sequence[index++]==base+0x16a770);
                assert(index==commandInvoke.calls);
            }
            assert(platforms==16);
            auto rejectedPlatform=[](std::array<uint8_t,0x38>&) { return 91; };
            const auto beforeFailure=commandInvoke.calls;
            assert(tglSendOwnedSfcPreparedCommands(commands,prepared,shell,1,1,1,1,
                rejectedPlatform)==91 && commandInvoke.calls==beforeFailure+1);
            uintptr_t context=0x40000000,query=base+0x62ee0;
            unsigned environmentReads=0;
            auto environmentRead=[&](uintptr_t address,void* destination,size_t size) {
                ++environmentReads;
                void* live=owner.get();owner.reset();assert(owner.get()==live);
                assert(commands.frameStart()==5); // qualification holds the same lease
                if(size==8 && address==os) std::memcpy(destination,&context,8);
                else if(size==8 && address==os+0x1b0) std::memcpy(destination,&query,8);
                else return read(address,destination,size);
                return true;
            };
            commandInvoke.result=-43;
            const auto beforeQualified=commandInvoke.calls;
            assert(commands.stateQualified(state,output,environmentRead)==-43);
            assert(commandInvoke.calls==beforeQualified+1 && environmentReads==3);
            for(unsigned invalid=0;invalid<3;++invalid) {
                context=invalid==0?0:0x40000000;
                query=base+0x62ee0+(invalid==1);
                if(invalid==2) flags={0,0};
                assert(commands.stateQualified(state,output,environmentRead)==5);
                assert(commandInvoke.calls==beforeQualified+1);
            }
            flags={1,0};context=0x40000000;query=base+0x62ee0;
            assert(commands.stateQualified(state,output,[](uintptr_t,void*,size_t){return false;})==5);
            assert(commandInvoke.calls==beforeQualified+1);
            TglStateQualifiedSfcBackend<decltype(commands),decltype(environmentRead)>
                qualified(commands,environmentRead);
            commandInvoke.result=0;
            const auto firstQualifiedPass=commandInvoke.calls;
            assert(tglSendOwnedSfcPreparedCommands(qualified,prepared,shell,1,1,1,1,platform)==0);
            const std::array<uintptr_t,7> qualifiedOrder{
                0x1681e0,0x168390,0x16a580,0x16b390,0x16b190,0x16a8a0,0x16a770};
            assert(commandInvoke.calls==firstQualifiedPass+qualifiedOrder.size());
            for(size_t i=0;i<qualifiedOrder.size();++i)
                assert(commandInvoke.sequence[firstQualifiedPass+i]==base+qualifiedOrder[i]);
            context=0;
            const auto failedQualifiedPass=commandInvoke.calls;
            assert(tglSendOwnedSfcPreparedCommands(qualified,prepared,shell,1,1,1,1,platform)==5);
            assert(commandInvoke.calls==failedQualifiedPass+1 &&
                commandInvoke.last==base+0x1681e0); // partial lock only; discard required
            context=0x40000000;
            const auto completedCalls=commandInvoke.calls;
            assert(owner.initialize(lifetime,mhw,os,read));
            assert(invoke.constructs==2 && invoke.destroys==1);
            owner.reset();owner.reset();
            assert(commands.frameStart()==5 && commandInvoke.calls==completedCalls);
            assert(!owner.get() && invoke.destroys==2);
            invoke.corrupt=true;
            assert(!owner.initialize(lifetime,mhw,os,read));
            assert(invoke.constructs==3 && invoke.destroys==3 && !owner.get());
            invoke.corrupt=false;
            flags={0,0};
            assert(!owner.initialize(lifetime,mhw,os,read) && invoke.constructs==3);
            flags={1,0};
            auto wrong=lifetime;wrong.destroy+=1;
            assert(!owner.initialize(wrong,mhw,os,read) && invoke.constructs==3);
            assert(owner.initialize(lifetime,mhw,os,read));
        }
        assert(invoke.constructs==4 && invoke.destroys==4);
        {
            TglOwnedSfcMhw<Invoke> owner(invoke);
            alignas(8) std::array<uint8_t,0x1bc0> child{};
            TglVeboxLifetimeBinding baseBinding{base,base+0x12d4e0,base+0x12de30};
            int32_t status=77;
            unsigned calls=0;
            int32_t result=0;
            auto ctor=[&](uintptr_t entry,void *storage,void *nativeOs,void *nativeMhw,
                void *nativeSfc,void *renderHal,void *history,void *performance,
                const void *cache,int32_t *out) {
                assert(entry==base+0x12d4e0 && storage==child.data());
                assert(nativeOs==reinterpret_cast<void*>(os) && nativeSfc==owner.get());
                assert(!nativeMhw && renderHal==reinterpret_cast<void*>(0x3000) &&
                       !history && !performance && !cache);
                const auto destroys=invoke.destroys;
                owner.reset();
                assert(owner.get()==nativeSfc && invoke.destroys==destroys);
                assert(!owner.initialize(lifetime,mhw,os,read));
                bool nested=false;
                assert(owner.withObject([&](void*){nested=true;return 0;})==5 && !nested);
                ++calls;*out=result;
            };
            auto construct=[&](size_t capacity,void* hal=reinterpret_cast<void*>(0x3000)) {
                return tglConstructOwnedVeboxBaseWithSfc(baseBinding,child.data(),capacity,
                    owner,reinterpret_cast<void*>(os),nullptr,hal,nullptr,nullptr,nullptr,
                    status,ctor);
            };
            assert(construct(child.size())==TglOwnedBaseConstruction::NotCalled && calls==0 && status==77);
            assert(owner.initialize(lifetime,mhw,os,read));
            assert(construct(child.size(),nullptr)==TglOwnedBaseConstruction::NotCalled && calls==0 && status==77);
            assert(construct(child.size()-1)==TglOwnedBaseConstruction::NotCalled && calls==0);
            assert(construct(child.size())==TglOwnedBaseConstruction::ConstructedSuccess && calls==1);
            assert(owner.withObject([&](void*) {
                assert(construct(child.size())==TglOwnedBaseConstruction::NotCalled && calls==1);
                return 0;
            })==0);
            result=31;
            assert(construct(child.size())==TglOwnedBaseConstruction::ConstructedFailure && calls==2 && status==31);
            ++baseBinding.image;
            assert(construct(child.size())==TglOwnedBaseConstruction::NotCalled && calls==2);
        }
    }
    {
        std::array<uint32_t,128> y{};
        std::array<uint32_t,64> uv{};
        for(size_t i=0;i<y.size();++i) y[i]=uint32_t(0x12340000+i);
        for(size_t i=0;i<uv.size();++i) uv[i]=uint32_t(0x56780000+i);
        for(uint8_t mode : {uint8_t(0),uint8_t(1),uint8_t(2),uint8_t(255)}) {
            const auto yp=tglOwnedSfcTableParameters(mode,y);
            const auto uvp=tglOwnedSfcTableParameters(mode,uv);
            assert(yp.pipeMode==mode && uvp.pipeMode==mode);
            assert((yp.reserved==std::array<uint8_t,3>{}));
            assert((uvp.reserved==std::array<uint8_t,3>{}));
            assert(std::memcmp(reinterpret_cast<const uint8_t*>(&yp)+4,y.data(),0x200)==0);
            assert(std::memcmp(reinterpret_cast<const uint8_t*>(&uvp)+4,uv.data(),0x100)==0);
        }
    }
    {
        constexpr uintptr_t base=0x10000000;
        const std::array<uintptr_t,11> offsets{
            0x16bed0,0x16bef0,0x1681e0,0x168390,0x16a580,0x16a770,
            0x16a8a0,0x16b190,0x16b390,0x16b590,0xfe6c0};
        std::array<uintptr_t,11> table{};
        for(size_t i=0;i<table.size();++i) table[i]=base+offsets[i];
        const std::array<uintptr_t,2> metadata{0,base+0x759490};
        auto read=[&](uintptr_t address,void *dst,size_t size) {
            if(address==base+0x759410 && size==sizeof(metadata))
                std::memcpy(dst,metadata.data(),size);
            else if(address==base+0x759420 && size==sizeof(table))
                std::memcpy(dst,table.data(),size);
            else return false;
            return true;
        };
        auto qualify=[](uintptr_t address){return address==base;};
        auto executable=[](uintptr_t address,size_t size){
            return size==1 && address>=base+0x1160 && address<base+0x23e03f;
        };
        TglSfcMhwBinding binding;
        assert(binding.resolve(base,read,qualify,executable));
        assert(binding.image==base && binding.entries==table);
        for(size_t i=0;i<table.size();++i) {
            ++table[i];
            assert(!binding.resolve(base,read,qualify,executable));
            assert((binding.image==0 && binding.entries==std::array<uintptr_t,11>{}));
            --table[i];
        }
        assert(!binding.resolve(base,read,qualify,[](uintptr_t,size_t){return false;}));
        assert(!binding.resolve(base,read,[](uintptr_t){return false;},executable));
        assert(!binding.resolve(UINTPTR_MAX,read,qualify,executable));
        assert(binding.resolve(base,read,qualify,executable));
        constexpr uintptr_t object=0x20000000,os=0x30000000;
        std::array<uint8_t,0x41> bytes{};
        std::array<uint32_t,2> flags{1,0};
        auto put=[&](size_t offset,auto value){std::memcpy(bytes.data()+offset,&value,sizeof(value));};
        put(0,uintptr_t(base+0x759420)); put(8,uintptr_t(base+0x3eae0));
        put(0x10,os); put(0x18,uint16_t(16)); put(0x1a,uint16_t(4));
        bytes[0x40]=1;
        auto objectRead=[&](uintptr_t address,void *dst,size_t size) {
            if(address==object && size==bytes.size()) std::memcpy(dst,bytes.data(),size);
            else if(address==os+0x88 && size==sizeof(flags)) std::memcpy(dst,flags.data(),size);
            else return false;
            return true;
        };
        assert(binding.validateConstructedObject(object,os,objectRead));
        for(size_t offset : {size_t(0),size_t(8),size_t(0x10),size_t(0x18),size_t(0x1a),size_t(0x40)}) {
            bytes[offset]^=1;
            assert(!binding.validateConstructedObject(object,os,objectRead));
            bytes[offset]^=1;
        }
        flags={0,0};
        assert(!binding.validateConstructedObject(object,os,objectRead));
        flags={0,1};
        assert(!binding.validateConstructedObject(object,os,objectRead));
        put(8,uintptr_t(base+0x3e6a0));
        assert(binding.validateConstructedObject(object,os,objectRead));
        assert(!binding.validateConstructedObject(UINTPTR_MAX,os,objectRead));
        uintptr_t context=0x40000000,stateEntry=base+0x62ee0;
        bool denyContext=false,denyEntry=false;
        auto stateRead=[&](uintptr_t address,void* dst,size_t size) {
            if(address==os && size==8 && !denyContext) std::memcpy(dst,&context,8);
            else if(address==os+0x1b0 && size==8 && !denyEntry)
                std::memcpy(dst,&stateEntry,8);
            else return objectRead(address,dst,size);
            return true;
        };
        assert(binding.validateStateEnvironment(object,os,stateRead));
        for(uintptr_t invalid:{uintptr_t(0),UINTPTR_MAX}) {
            context=invalid;
            assert(!binding.validateStateEnvironment(object,os,stateRead));
        }
        context=0x40000000;
        ++stateEntry;
        assert(!binding.validateStateEnvironment(object,os,stateRead));
        --stateEntry;
        denyContext=true;
        assert(!binding.validateStateEnvironment(object,os,stateRead));
        denyContext=false;denyEntry=true;
        assert(!binding.validateStateEnvironment(object,os,stateRead));
        denyEntry=false;
        assert(binding.validateStateEnvironment(object,os,stateRead));
        bytes[0x40]=0;
        assert(!binding.validateStateEnvironment(object,os,stateRead));
    }
    {
        struct Commands {
            std::array<int,9> order{};
            size_t count=0;
            int failure=0;
            int stage(int id) {order[count++]=id;return failure==id ? -id : 0;}
            int lock(){return stage(1);} int output(){return stage(2);}
            int platform(){return stage(3);} int state(){return stage(4);}
            int avsState(){return stage(5);} int yTable(){return stage(6);}
            int uvTable(){return stage(7);} int iefState(){return stage(8);}
            int frameStart(){return stage(9);}
        };
        for(unsigned flags=0;flags<16;++flags) for(int failure=0;failure<=9;++failure) {
            Commands commands;
            commands.failure=failure;
            const int result=tglScheduleOwnedSfcCommands(flags&1,(flags>>1)&1,
                (flags>>2)&1,(flags>>3)&1,commands);
            std::array<int,9> expected{};
            size_t count=0;
            int expectedResult=0;
            for(int id=1;id<=9;++id) {
                if(id>=5 && id<=7 && !(flags&3)) continue;
                if(id==8 && !(flags&12)) continue;
                expected[count++]=id;
                if(id==failure){expectedResult=-id;break;}
            }
            assert(result==expectedResult && commands.count==count);
            assert(commands.order==expected);
        }
    }
    {
        for (uint32_t mode : {0u,1u,2u,0xffffffffu}) {
            for (int failure=0; failure<=4; ++failure) {
                std::array<int,4> order{};
                size_t count=0;
                auto stage=[&](int id) noexcept {
                    order[count++]=id;
                    return failure==id ? -100-id : 0;
                };
                const int result=tglEmitOwnedVeboxCommandTail(mode,
                    [&]() noexcept {return stage(1);},
                    [&]() noexcept {return stage(2);},
                    [&]() noexcept {return stage(3);},
                    [&]() noexcept {return stage(4);});
                const bool skipped=failure==3 && mode!=1;
                assert(result==(failure && !skipped ? -100-failure : 0));
                const std::array<int,4> expected=mode==1 ?
                    std::array<int,4>{1,2,3,4} : std::array<int,4>{1,2,4,0};
                const size_t expectedCount=(failure && !skipped) ?
                    size_t(failure-(mode!=1 && failure==4)) : size_t(mode==1?4:3);
                assert(count==expectedCount);
                for(size_t i=0;i<count;++i) assert(order[i]==expected[i]);
            }
        }
    }
    {
        for(int surfaceStatus : {0,5,31}) for(int stateStatus : {0,5,31})
            for(int diStatus : {0,5,31}) {
                TglOwnedVeboxProducerPass pass;
                unsigned diCalls=0,commandCalls=0;
                auto di=[&]() noexcept {
                    ++diCalls;pass.close();assert(!pass.begin());
                    assert(!pass.record(99));return diStatus;
                };
                assert(!pass.record(31) && pass.gate(di)==5 && !diCalls);
                assert(pass.begin() && !pass.begin());
                assert(pass.produce([&]() noexcept {
                    pass.close();assert(!pass.begin() && !pass.record(99));
                    assert(pass.produce([]() noexcept {return 99;})==5);
                    assert(pass.gate([]() noexcept {return 99;})==5);
                    return 0;
                })==0);
                assert(pass.record(surfaceStatus) && pass.record(stateStatus));
                const int status=pass.gate(di);
                const int expected=surfaceStatus?surfaceStatus:stateStatus?stateStatus:diStatus;
                assert(status==expected && diCalls==unsigned(!surfaceStatus&&!stateStatus));
                assert(pass.gate(di)==5 && !pass.record(31));
                if(!status) {
                    auto command=[&]() noexcept {++commandCalls;return 0;};
                    assert(tglEmitOwnedVeboxCommandTail(1,command,command,command,command)==0);
                }
                assert(commandCalls==(expected?0u:4u));
                pass.close();assert(pass.begin());
                assert(pass.gate([]() noexcept {return 0;})==0);
            }
    }
    {
        struct Context {
            TglOwnedVeboxProducerPass pass;
            int failure=0;
            unsigned calls=0;
            int surfaces(bool enabled,void*) noexcept {++calls;return enabled?failure:5;}
            int state(bool enabled,void*) noexcept {++calls;return enabled?0:5;}
            int diIecp(bool enabled,void*) noexcept {++calls;return enabled?0:5;}
        };
        using Resolve=TglOwnedVeboxScopedContext<Context>;
        std::array<uintptr_t,20> original{};
        original.fill(123);
        const auto hooks=tglBindOwnedVeboxProducerHooks<Resolve>(original);
        for(size_t i=0;i<hooks.size();++i)
            if(i!=6 && i!=7 && i!=18) assert(hooks[i]==original[i]);
        using Callback=int(*)(void*,bool,void*);
        const auto surface=reinterpret_cast<Callback>(hooks[7]);
        const auto state=reinterpret_cast<Callback>(hooks[18]);
        const auto di=reinterpret_cast<Callback>(hooks[6]);
        int packet=0;
        for(int failure : {0,31}) {
            Context context;context.failure=failure;
            assert(!Resolve::get(&context));
            Resolve scope(&context,context);assert(scope.bound() && Resolve::get(&context)==&context);
            {
                Context other;
                Resolve nested(&other,other);assert(!nested.bound());
                assert(!Resolve::get(&other) && Resolve::get(&context)==&context);
                assert(surface(&other,true,&packet)==5 && !other.calls);
            }
            assert(Resolve::get(&context)==&context);
            assert(surface(&context,true,&packet)==5 && !context.calls);
            assert(context.pass.begin());
            assert(surface(&context,true,&packet)==failure);
            assert(state(&context,true,&packet)==failure);
            assert(di(&context,true,&packet)==failure);
            assert(context.calls==(failure?1u:3u));
            assert(di(&context,true,&packet)==5);
            context.pass.close();
            assert(context.pass.begin());
            const auto calls=context.calls;
            assert(surface(&context,true,nullptr)==5);
            assert(state(&context,true,&packet)==5 && di(&context,true,&packet)==5);
            assert(context.calls==calls);
        }
        {
            Context context;Resolve invalid(nullptr,context);
            assert(!invalid.bound() && !Resolve::get(&context));
        }
        assert(surface(nullptr,true,&packet)==5 && state(nullptr,true,&packet)==5 &&
               di(nullptr,true,&packet)==5);
        Context scoped;
        Resolve scopedBinding(&scoped,scoped);assert(scopedBinding.bound());
        for(int nativeResult : {0,31,-7}) {
            scoped.failure=31;
            assert(scoped.pass.withBuilder([&]() noexcept {
                scoped.pass.close();assert(!scoped.pass.begin());
                assert(scoped.pass.withBuilder([]() noexcept {return 99;})==5);
                assert(surface(&scoped,true,&packet)==31);
                scoped.pass.close(); // must not discard the error between callbacks
                assert(state(&scoped,true,&packet)==31);
                if(!nativeResult) assert(di(&scoped,true,&packet)==31);
                return nativeResult;
            })==(nativeResult?nativeResult:31));
            scoped.failure=0;
            assert(scoped.pass.withBuilder([&]() noexcept {
                assert(surface(&scoped,true,&packet)==0);
                assert(state(&scoped,true,&packet)==0);
                return di(&scoped,true,&packet);
            })==0);
            assert(di(&scoped,true,&packet)==5); // lease scope ended
        }
        assert(scoped.pass.withBuilder([]() noexcept {return 0;})==5);
    }
    {
        TglOwnedIefTables tables{};
        for (size_t i=0; i<5; ++i)
            for (size_t j=0; j<64; ++j) tables[i][j] = uint32_t(i*256+j);
        std::array<uint8_t,64> out{};
        out.fill(0xa5);
        TglOwnedIefSurfaceFlags enable{};
        assert(tglBuildOwnedIefParameters(out,enable,5.9f,3,2,{0x107,2,8},tables));
        uint32_t value=0;
        std::memcpy(&value,out.data()+8,4);
        assert(value==5 && enable.enabled==1 && enable.skinEnabled==1 &&
               out[1]==0 && out[2]==1 && out[4]==7);
        std::memcpy(&value,out.data()+28,4);
        assert(value==1029 && out[32]==0xa5);
        assert(tglBuildOwnedIefParameters(out,enable,-1,0,1,{7,2,8},tables));
        std::memcpy(&value,out.data()+8,4);
        assert(value==63);
        const auto previous=out;
        assert(tglBuildOwnedIefParameters(out,enable,0,0,1,{7,2,8},tables));
        assert(std::memcmp(out.data()+8,previous.data()+8,24)==0);
        TglOwnedIefTables copied{};
        size_t reads=0;
        assert(tglReadOwnedIefTables(copied,[&](size_t offset,void *dst,size_t size) {
            assert(offset==0x741e50+reads*0x100 && size==0x100);
            std::memcpy(dst,tables[reads++].data(),size);
            return true;
        }));
        assert(reads==5 && copied==tables);
        const auto saved=copied;
        reads=0;
        assert(!tglReadOwnedIefTables(copied,[&](size_t,void *dst,size_t size) {
            std::memset(dst,0,size);
            return ++reads!=3;
        }));
        assert(reads==3 && copied==saved);
        const auto unchanged=out;
        assert(!tglBuildOwnedIefParameters(out,enable,
            std::numeric_limits<float>::infinity(),0,0,{0,0,0},tables));
        assert(out==unchanged);
        assert(tglBuildOwnedIefParameters(out,enable,65536,0,0,{0,0,0},tables));
        assert(std::memcmp(out.data()+8,previous.data()+8,24)==0);
    }
    {
        unsigned calls=0;
        auto sine=[&](float x) noexcept { ++calls; return std::sin(x); };
        TglOwnedSfcAvsTables tables;
        assert(tglUpdateOwnedSfcAvsTables(tables,0x19,0.5f,0.75f,0x11,false,true,0.f,0.f,sine));
        assert(calls>0 && tables.format==0x19);
        std::array<uint32_t,128> y{}; std::array<uint32_t,64> uv{};
        tglPackOwnedSfcAvsY(y,tables.yX,tables.yY,0x19,false);
        tglPackOwnedSfcAvsUv(uv,tables.uvX,tables.uvY);
        assert(y==tables.packedY && uv==tables.packedUv);
        const auto saved=tables; const auto count=calls;
        assert(tglUpdateOwnedSfcAvsTables(tables,0x19,0.5f,0.75f,0,true,false,9.f,9.f,sine));
        assert(calls==count && tables.packedY==saved.packedY);
        auto denied=[](float) noexcept { return std::numeric_limits<float>::quiet_NaN(); };
        assert(!tglUpdateOwnedSfcAvsTables(tables,0x19,1.f,0.25f,0x11,false,true,0.f,0.f,denied));
        assert(tables.scaleX==saved.scaleX && tables.scaleY==saved.scaleY);
        assert(tables.yX==saved.yX && tables.packedY==saved.packedY);
        assert(tglUpdateOwnedSfcAvsTables(tables,0x19,2.f,2.f,0x11,false,true,0.f,0.f,sine));
        const auto upscaled=tables; const auto before=calls;
        assert(tglUpdateOwnedSfcAvsTables(tables,0x19,3.f,4.f,0,false,true,0.f,0.f,sine));
        assert(calls==before && tables.packedY==upscaled.packedY);
        assert(tables.scaleX==3.f && tables.scaleY==4.f);
    }
    {
        auto sine=[](float x) noexcept { return std::sin(x); };
        std::array<int32_t,256> y{};
        std::array<int32_t,128> uv{},reference{};
        for (bool vertical:{false,true}) for (uint32_t siting:{0u,1u,2u,0x10u,0x20u,0x33u}) {
            assert(tglBuildOwnedSfcAvsAxis(y,uv,0x19,0.5f,siting,vertical,
                false,true,0.f,0.f,sine));
            const int offset=siting&(vertical ? 0x10u : 1u) ? 0 :
                siting&(vertical ? 0x20u : 2u) ? 8 : 16;
            assert(tglBuildOwnedSfcLeftChromaTable(reference,offset ? 3.f : 2.f,
                0.5f,sine,offset));
            assert(uv==reference);
        }
        assert(tglBuildOwnedSfcAvsAxis(y,uv,1,1.f,0,false,false,true,0.f,0.f,sine));
        std::array<int32_t,256> expectedY{};
        assert(tglPrepareOwnedSfcUnityCoefficients(expectedY,true,true));
        assert(y==expectedY);
        const auto savedY=y; const auto savedUv=uv;
        assert(!tglBuildOwnedSfcAvsAxis(y,uv,1,0.f,0,false,false,true,0.f,0.f,sine));
        assert(y==savedY && uv==savedUv);
    }
    {
        auto sine=[](float x) noexcept { return std::sin(x); };
        std::array<int32_t,256> table{},reference{};
        for (uint32_t format:{1u,0x19u,0u}) for (uint32_t mode:{0u,1u,2u})
            for (bool symmetric:{false,true}) for (float scale:{0.5f,1.f,2.f}) {
                assert(tglBuildOwnedSfcFilteredY(table,format,mode,scale,0.25f,0.f,
                    symmetric,sine));
                const size_t taps=mode ? 4 : 8;
                for (size_t phase=0;phase<32;++phase) {
                    int sum=0; for (size_t tap=0;tap<taps;++tap) sum+=table[phase*taps+tap];
                    assert(sum==64);
                }
                for (size_t i=32*taps;i<table.size();++i) assert(table[i]==0);
                assert(tglBuildOwnedSfcFilteredY(reference,format,mode,
                    scale>1.f ? 1.f : scale,0.25f,0.f,symmetric,sine));
                assert(table==reference);
            }
        const auto saved=table;
        assert(!tglBuildOwnedSfcFilteredY(table,0x19,0,0.5f,
            std::numeric_limits<float>::infinity(),0.f,true,sine));
        assert(table==saved);
    }
    {
        auto sine=[](float x) noexcept { return std::sin(x); };
        std::array<std::array<float,3>,32> kernels{};
        assert(tglBuildOwnedSfcSharpenKernels(kernels,0.f,sine));
        for (const auto& kernel:kernels) {
            assert(kernel[0]==0.f && kernel[1]==1.f && kernel[2]==0.f);
        }
        std::array<int32_t,256> direct{},convolved{};
        assert(tglBuildOwnedSfcWindowedTable<8>(direct,0.5f,3.f,nullptr,sine));
        assert(tglBuildOwnedSfcWindowedTable<8>(convolved,0.5f,3.f,&kernels,sine));
        assert(direct==convolved);
        assert(tglBuildOwnedSfcSharpenKernels(kernels,0.25f,sine));
        assert(kernels[0][0]==-0.25f && kernels[0][1]==1.5f);
        for (size_t phase=1;phase<16;++phase) assert(kernels[phase]==kernels[32-phase]);
        const auto saved=kernels;
        assert(!tglBuildOwnedSfcSharpenKernels(kernels,
            std::numeric_limits<float>::infinity(),sine));
        assert(kernels==saved);
        assert(tglBuildOwnedSfcWindowedTable<8>(convolved,0.5f,3.f,&kernels,sine));
        for (size_t phase=0;phase<32;++phase) {
            int sum=0; for (size_t tap=0;tap<8;++tap) sum+=convolved[phase*8+tap];
            assert(sum==64);
        }
    }
    {
        unsigned calls=0;
        auto sine=[&](float x) noexcept { ++calls; return std::sin(x); };
        float value=19;
        assert(tglOwnedSfcFivePointSinc(value,-3.f,2.f,sine));
        assert(value==0 && calls==0);
        assert(tglOwnedSfcFivePointSinc(value,2.01f,2.f,sine));
        assert(value==0 && calls==0);
        assert(tglOwnedSfcFivePointSinc(value,2.f,2.f,sine)); assert(calls==2);
        assert(tglOwnedSfcFivePointSinc(value,0.5f,2.f,sine));
        const float reference=value;
        assert(tglOwnedSfcFivePointSinc(value,0.5f,3.f,sine)); assert(value==reference);
        std::array<int32_t,256> table{};
        assert(tglBuildOwnedSfcWindowedTable<8>(table,0.5f,2.f,nullptr,sine,false));
        for (size_t phase=0;phase<32;++phase) {
            int total=0; for (size_t tap=0;tap<8;++tap) total+=table[phase*8+tap];
            assert(total==64);
        }
    }
    {
        float window=19;
        for (uint32_t format:{0x19u,0x17u,0xfffffffau}) {
            assert(tglSelectOwnedSfcYWindow(window,format,0,0.5f,0)); assert(window==4);
            assert(tglSelectOwnedSfcYWindow(window,format,0,1.f,0)); assert(window==8);
            assert(tglSelectOwnedSfcYWindow(window,format,0,0.5f,6)); assert(window==6);
            assert(tglSelectOwnedSfcYWindow(window,format,1,0.5f,6)); assert(window==2);
            assert(tglSelectOwnedSfcYWindow(window,format,2,0.5f,6)); assert(window==2);
        }
        assert(tglSelectOwnedSfcYWindow(window,1,0,1.f,6)); assert(window==6);
        assert(tglSelectOwnedSfcYWindow(window,1,3,1.f,6)); assert(window==2);
        assert(tglSelectOwnedSfcYWindow(window,0x19,3,1.f,6)); assert(window==6);
        assert(tglSelectOwnedSfcYWindow(window,0,0,1.f,6)); assert(window==2);
        assert(!tglSelectOwnedSfcYWindow(window,0,0,0.f,6)); assert(window==2);
    }
    {
        auto sine=[](float x) noexcept { return std::sin(x); };
        std::array<int32_t,128> table{},reference{};
        for (int32_t offset:{8,16}) for (float scale:{0.25f,0.5f,1.f,2.f}) {
            assert(tglBuildOwnedSfcLeftChromaTable(table,3.f,scale,sine,offset));
            for (size_t phase=0;phase<32;++phase) {
                int sum=0; for (size_t tap=0;tap<4;++tap) sum+=table[phase*4+tap];
                assert(sum==64);
            }
            assert(tglBuildOwnedSfcLeftChromaTable(reference,3.f,
                scale>1.f ? 1.f : scale,sine,offset));
            assert(table==reference);
        }
        const auto saved=table;
        assert(!tglBuildOwnedSfcLeftChromaTable(table,3.f,1.f,sine,7));
        assert(table==saved);
    }
    {
        auto sine=[](float x) noexcept { return std::sin(x); };
        std::array<int32_t,128> table{},reference{};
        for (float scale:{0.25f,0.5f,0.75f,1.f,2.f}) {
            assert(tglBuildOwnedSfcLeftChromaTable(table,3.f,scale,sine));
            for (size_t phase=0;phase<32;++phase) {
                int total=0; for (size_t tap=0;tap<4;++tap) total+=table[phase*4+tap];
                assert(total==64);
            }
        }
        assert(tglBuildOwnedSfcLeftChromaTable(reference,3.f,1.f,sine));
        assert(table==reference);
        assert(tglBuildOwnedSfcLeftChromaTable(table,3.f,0.5f,sine));
        assert(tglBuildOwnedSfcLeftChromaTable(reference,2.f,0.5f,sine));
        assert(table==reference);
        const auto saved=table;
        assert(!tglBuildOwnedSfcLeftChromaTable(table,3.f,0.f,sine));
        assert(table==saved);
    }
    {
        auto sine=[](float x) noexcept { return std::sin(x); };
        std::array<int32_t,256> table{};
        for (float scale:{0.25f,0.5f,0.75f,1.f}) {
            assert(tglBuildOwnedSfcWindowedTable<8>(table,scale,3.f,nullptr,sine));
            for (size_t phase=0;phase<32;++phase) {
                int total=0; for (size_t tap=0;tap<8;++tap) total+=table[phase*8+tap];
                assert(total==64);
            }
        }
        const auto saved=table;
        assert(!tglBuildOwnedSfcWindowedTable<8>(table,0.f,3.f,nullptr,sine));
        auto denied=[](float) noexcept { return std::numeric_limits<float>::quiet_NaN(); };
        assert(!tglBuildOwnedSfcWindowedTable<8>(table,0.5f,3.f,nullptr,denied));
        assert(table==saved);
        std::array<std::array<float,3>,32> identity{};
        for (auto& kernel:identity) kernel={{0,1,0}};
        assert(tglBuildOwnedSfcWindowedTable<8>(table,1.f,3.f,&identity,sine));
        assert(table==saved);
        std::array<int32_t,128> four{};
        assert(tglBuildOwnedSfcWindowedTable<4>(four,0.5f,2.f,nullptr,sine));
        for (size_t phase=0;phase<32;++phase) {
            int total=0; for (size_t tap=0;tap<4;++tap) total+=four[phase*4+tap];
            assert(total==64);
        }
    }
    {
        unsigned calls=0;
        auto sine=[&](float x) noexcept { ++calls; return std::sin(x); };
        float output=19.f;
        assert(tglOwnedSfcWindowedSinc(output,0.f,8,3.f,sine));
        assert(output==1.f && calls==0);
        assert(tglOwnedSfcWindowedSinc(output,-4.f,8,3.f,sine));
        assert(output==0.f && calls==0);
        assert(tglOwnedSfcWindowedSinc(output,2.f,4,3.f,sine));
        assert(output==0.f && calls==0);
        assert(tglOwnedSfcWindowedSinc(output,0.5f,8,3.f,sine));
        const float positive=output;
        assert(calls==2 && positive>0.f);
        assert(tglOwnedSfcWindowedSinc(output,-0.5f,8,3.f,sine));
        assert(output==positive && calls==4);
        const auto saved=output;
        assert(!tglOwnedSfcWindowedSinc(output,1.f,5,3.f,sine));
        assert(!tglOwnedSfcWindowedSinc(output,1.f,8,0.f,sine));
        assert(output==saved && calls==4);
    }
    {
        const std::array<float,4> input{{1,2,3,4}};
        std::array<float,4> output{};
        assert(tglConvolveOwnedSfcAvsPhase(output,input,{{-1,3,-1}}));
        assert((output==std::array<float,4>{{1,2,3,9}}));
        const auto saved=output;
        assert(!tglConvolveOwnedSfcAvsPhase(output,input,
            {{0,std::numeric_limits<float>::infinity(),0}}));
        assert(output==saved);
        auto alias=input;
        assert(tglConvolveOwnedSfcAvsPhase(alias,alias,{{0,1,0}}));
        assert(alias==input);
        std::array<int32_t,4> quantized{};
        assert(tglQuantizeOwnedSfcAvsPhase(quantized,output,10.f,0));
        int total=0; for (auto value:quantized) total+=value;
        assert(total==64);
    }
    {
        std::array<int32_t,4> output{};
        const std::array<float,4> weights{{1.f,1.f,1.f,0.f}};
        assert(tglQuantizeOwnedSfcAvsPhase(output,weights,3.f,16));
        assert((output==std::array<int32_t,4>{{21,22,21,0}}));
        assert(tglQuantizeOwnedSfcAvsPhase(output,weights,3.f,17));
        assert((output==std::array<int32_t,4>{{21,21,22,0}}));
        const auto saved=output;
        assert(!tglQuantizeOwnedSfcAvsPhase(output,weights,0.f,0));
        assert(!tglQuantizeOwnedSfcAvsPhase(output,weights,3.f,32));
        auto bad=weights; bad[0]=std::numeric_limits<float>::infinity();
        assert(!tglQuantizeOwnedSfcAvsPhase(output,bad,3.f,0));
        assert(output==saved);
        std::array<int32_t,8> eight{};
        const std::array<float,8> uniform{{1,1,1,1,1,1,1,1}};
        for (uint32_t phase=0;phase<32;++phase) {
            assert(tglQuantizeOwnedSfcAvsPhase(eight,uniform,8.f,phase));
            for (auto value:eight) assert(value==8);
        }
    }
    {
        for (bool fourTap : {false,true}) for (bool upper : {false,true}) {
            std::array<int32_t,256> coefficients{};
            coefficients.fill(-7);
            assert(tglPrepareOwnedSfcUnityCoefficients(coefficients,fourTap,upper));
            const size_t taps=fourTap ? 4 : 8;
            for (size_t i=0;i<coefficients.size();++i) {
                const size_t phase=i/taps,tap=i%taps;
                const bool selected=phase<32 && (phase<=16 ? tap==(fourTap ? 1u : 3u) :
                    upper && tap==(fourTap ? 2u : 4u));
                assert(coefficients[i]==(selected ? 64 : -7));
            }
        }
        std::array<int32_t,127> tooSmall{}; tooSmall.fill(19);
        const auto saved=tooSmall;
        assert(!tglPrepareOwnedSfcUnityCoefficients(tooSmall,true,true));
        assert(tooSmall==saved);
        std::array<int32_t,128> chroma{};
        assert(tglPrepareOwnedSfcUnityCoefficients(chroma,true,true));
        std::array<uint32_t,64> packed{};
        tglPackOwnedSfcAvsUv(packed,chroma,chroma);
        for (size_t phase=0;phase<32;++phase) {
            assert(packed[phase*2]==(phase<=16 ? 0x40400000u : 0u));
            assert(packed[phase*2+1]==(phase>16 ? 0x00004040u : 0u));
        }
    }
    {
        assert(tglOwnedSfcAvsCacheHit(3,1.f,0.5f,3,1.f,0.5f));
        assert(!tglOwnedSfcAvsCacheHit(3,1.f,0.5f,4,1.f,0.5f));
        assert(!tglOwnedSfcAvsCacheHit(3,1.f,0.5f,3,2.f,0.5f));
        assert(!tglOwnedSfcAvsCacheHit(3,1.f,0.5f,3,1.f,1.f));
        assert(tglOwnedSfcAvsCacheHit(3,-0.f,0.f,3,0.f,-0.f));
        const float nan=std::numeric_limits<float>::quiet_NaN();
        assert(!tglOwnedSfcAvsCacheHit(3,nan,1.f,3,nan,1.f));
        assert(!tglOwnedSfcAvsCacheHit(3,1.f,nan,3,1.f,nan));
    }
    {
        std::array<int32_t,256> h{},v{};
        std::array<uint32_t,128> output{};
        for (size_t i=0;i<256;++i) { h[i]=int32_t(i)-128; v[i]=int32_t(i)+0x1234; }
        for (uint32_t format : {0u,1u,2u,3u,4u,5u,0x50u,0x51u,0x52u,0xfffffff8u}) {
            for (bool eightTap : {false,true}) {
                const bool compact=!eightTap && (format==1 || format==2 || format==3 ||
                    format==4 || format==0x50 || format==0x51 || format==0xfffffff8u);
                output.fill(0xffffffffu);
                tglPackOwnedSfcAvsY(output,h,v,format,eightTap);
                for (size_t phase=0;phase<32;++phase) {
                    for (size_t slot=0;slot<8;++slot) {
                        uint8_t bytes[4]{};
                        std::memcpy(bytes,&output[phase*4+slot/2],4);
                        const bool padded=compact && (slot<2 || slot>=6);
                        const size_t index=phase*(compact ? 4 : 8)+slot-(compact && slot>=2 ? 2 : 0);
                        assert(bytes[(slot%2)*2]==(padded ? 0 : uint8_t(h[index])));
                        assert(bytes[(slot%2)*2+1]==(padded ? 0 : uint8_t(v[index])));
                    }
                }
            }
        }
    }
    {
        std::array<int32_t,128> horizontal{},vertical{};
        std::array<uint32_t,64> output{};
        for (size_t i=0;i<128;++i) {
            horizontal[i]=int32_t(i)-64;
            vertical[i]=int32_t(0x12340000u+127u-uint32_t(i));
        }
        output.fill(0xffffffffu);
        tglPackOwnedSfcAvsUv(output,horizontal,vertical);
        for (size_t word=0;word<64;++word) {
            uint8_t bytes[4]{};
            std::memcpy(bytes,&output[word],4);
            assert(bytes[0]==uint8_t(horizontal[word*2]));
            assert(bytes[1]==uint8_t(vertical[word*2]));
            assert(bytes[2]==uint8_t(horizontal[word*2+1]));
            assert(bytes[3]==uint8_t(vertical[word*2+1]));
        }
        horizontal.fill(-1); vertical.fill(0x100);
        tglPackOwnedSfcAvsUv(output,horizontal,vertical);
        for (auto word:output) assert(word==0x00ff00ffu);
    }
    testSfcFrameFields();
    testFillCscBinding();
    testSfcFillCache();
    testSfcFill();
    testSfcResourceBinding();
    testSfcAlpha();
    testSfcFilterRotation();
    testSfcBypass();
    testSfcRotationGeometry();
    testSfcRegions();
    testSfcAlignedGeometry();
    testSfcOutputChroma();
    testSfcInputChroma();
    testSfcStateParameterOwnership();
    testVpSfcCpuGraph();
    testSfcSetupOrder();
    testSfcAvsOwnership();
    testSfcLineBufferOwnership();
    testSfcSfdOwnership();
    {
        TglOwnedSfcLineBufferSizes sizes;
        assert(tglOwnedSfcLineBufferSizes(sizes,1080,1080));
        assert(sizes.avs==43200 && sizes.ief==17280 && sizes.sfd==0);
        const uint32_t max=UINT32_MAX;
        assert(tglOwnedSfcLineBufferSizes(sizes,max/40,max/16));
        assert(sizes.avs==(max/40)*40 && sizes.ief==(max/16)*16);
        assert(sizes.sfd==uint64_t(max/16)*64/10);
        const auto saved=sizes;
        for (const auto dimensions : {std::array<uint32_t,2>{0,1},{1,0},{max/40+1,1},{1,max/16+1}}) {
            assert(!tglOwnedSfcLineBufferSizes(sizes,dimensions[0],dimensions[1]));
            assert(sizes.avs==saved.avs && sizes.ief==saved.ief && sizes.sfd==saved.sfd);
        }
    }
    {
        std::array<uint8_t,0xd48> execution{};
        execution[8]=3; execution[0xb]=0x81; execution[0xf]=1;
        execution[0x12]=1; execution[0x13]=2; execution[0x14]=1; execution[0x19]=1;
        uint32_t pipe=2;
        std::memcpy(execution.data()+0xa0c,&pipe,4);
        const auto address=reinterpret_cast<uintptr_t>(execution.data());
        auto read=[&](uintptr_t p,void* out,size_t n) {
            if (p<address || p-address>execution.size() || n>execution.size()-(p-address)) return false;
            std::memcpy(out,execution.data()+(p-address),n); return true;
        };
        TglOwnedSurfaceStorage source;
        TglOwnedStateInputs inputs;
        inputs.source=&source; inputs.dnSpecial=true; inputs.skuSingleVeboxSlice=true;
        {
            constexpr uintptr_t child=0x5000;
            auto linked=address;
            auto childRead=[&](uintptr_t p,void* out,size_t n) {
                if(p==child+0x88 && n==8){std::memcpy(out,&linked,8);return true;}
                return read(p,out,n);
            };
            bool argument=false;
            for(unsigned flags=0;flags<256;++flags) {
                execution[0x13]=uint8_t(flags);
                assert(tglReadOwnedBuilderDiArgument(argument,child,address,childRead));
                assert(argument==bool(flags&1));
            }
            execution[0x13]=2;linked=address+1;argument=true;
            assert(!tglReadOwnedBuilderDiArgument(argument,child,address,childRead) && argument);
            linked=address;
            auto denied=[](uintptr_t,void*,size_t){return false;};
            assert(!tglReadOwnedBuilderDiArgument(argument,child,address,denied) && argument);
            assert(!tglReadOwnedBuilderDiArgument(argument,0,address,childRead));
            assert(!tglReadOwnedBuilderDiArgument(argument,UINTPTR_MAX,address,childRead));
            assert(!tglReadOwnedBuilderDiArgument(argument,child,UINTPTR_MAX,childRead));
        }
        assert(tglReadOwnedStateExecution(inputs,address,true,read));
        assert(inputs.pipe==2 && inputs.di && inputs.referenceValid && inputs.dn);
        assert(inputs.chromaIeCp && inputs.exec12 && !inputs.chromaDi && inputs.exec14 && inputs.exec19);
        assert(inputs.source==&source && inputs.dnSpecial && inputs.skuSingleVeboxSlice);
        for (int failed=0;failed<2;++failed) {
            int calls=0;
            auto fail=[&](uintptr_t p,void* out,size_t n) { return calls++!=failed && read(p,out,n); };
            assert(!tglReadOwnedStateExecution(inputs,address,false,fail));
            assert(inputs.di && inputs.pipe==2 && inputs.source==&source);
        }
        std::array<uint8_t,0x1bc0> child{};
        const auto childAddress=reinterpret_cast<uintptr_t>(child.data());
        auto sourceAddress=reinterpret_cast<uintptr_t>(&source);
        TglOwnedSurfaceStorage target;
        auto targetAddress=reinterpret_cast<uintptr_t>(&target);
        std::memcpy(child.data()+0x88,&address,8);
        std::memcpy(child.data()+0x1b0,&sourceAddress,8);
        std::memcpy(execution.data()+0x60,&targetAddress,8);
        auto graphRead=[&](uintptr_t p,void* out,size_t n) {
            if(p>=childAddress && p-childAddress<=child.size() && n<=child.size()-(p-childAddress)) {
                std::memcpy(out,child.data()+p-childAddress,n);return true;
            }
            return read(p,out,n);
        };
        assert(tglReadOwnedStateChildInputs(inputs,childAddress,address,source,&target,true,graphRead));
        assert(inputs.target==&target && inputs.dnSpecial && inputs.skuSingleVeboxSlice && inputs.di);
        for(int failed=0;failed<5;++failed) {
            int calls=0;
            auto fail=[&](uintptr_t p,void* out,size_t n) {return calls++!=failed && graphRead(p,out,n);};
            assert(!tglReadOwnedStateChildInputs(inputs,childAddress,address,source,&target,false,fail));
            assert(inputs.di && inputs.target==&target && inputs.dnSpecial);
        }
        ++sourceAddress;std::memcpy(child.data()+0x1b0,&sourceAddress,8);
        assert(!tglReadOwnedStateChildInputs(inputs,childAddress,address,source,&target,false,graphRead));
        --sourceAddress;std::memcpy(child.data()+0x1b0,&sourceAddress,8);
        assert(!tglReadOwnedStateChildInputs(inputs,childAddress,address,source,nullptr,false,graphRead));
        targetAddress=0;std::memcpy(execution.data()+0x60,&targetAddress,8);
        assert(tglReadOwnedStateChildInputs(inputs,childAddress,address,source,nullptr,true,graphRead));
        assert(!inputs.target);
        {
            std::array<uint8_t,0x80> hal{};
            std::array<uint8_t,0x6c> sku{};
            const auto halAddress=reinterpret_cast<uintptr_t>(hal.data());
            const auto skuAddress=reinterpret_cast<uintptr_t>(sku.data());
            const uint64_t features=uint64_t{1}<<38;
            std::memcpy(child.data()+0x18,&halAddress,8);
            std::memcpy(child.data()+0x20,&skuAddress,8);
            std::memcpy(hal.data()+0x78,&skuAddress,8);
            std::memcpy(sku.data()+0x64,&features,8);
            auto completeRead=[&](uintptr_t p,void* out,size_t n) {
                auto copy=[&](const auto& block) {
                    const auto start=reinterpret_cast<uintptr_t>(block.data());
                    if(p<start || p-start>block.size() || n>block.size()-(p-start)) return false;
                    std::memcpy(out,block.data()+p-start,n);return true;
                };
                return copy(hal)||copy(sku)||graphRead(p,out,n);
            };
            struct Backend {
                unsigned releases=0;
                void release(std::array<uint8_t,0x148>& resource) noexcept {
                    for(auto byte:resource) assert(!byte);++releases;
                }
            } backend;
            TglOwnedVeboxStateOwner<Backend> owner(backend);
            const auto sourceBefore=source;
            TglOwnedVeboxStateStorage leased;
            std::memset(leased.data(),0xa5,sizeof(leased));
            leased.resource.fill(0);
            const auto untouched=leased;
            auto forbiddenPacketRead=[](uintptr_t,void*,size_t) {assert(false);return false;};
            assert(tglPublishOwnedStateToBuilder(leased,nullptr,childAddress,address,
                halAddress,skuAddress,source,nullptr,true,true,forbiddenPacketRead)==5);
            assert(std::memcmp(leased.data(),untouched.data(),sizeof(leased))==0);
            for(size_t i=0;i<leased.resource.size();++i) {
                leased.resource[i]=1;
                const auto foreign=leased;
                assert(tglPublishOwnedStateToBuilder(leased,&leased,childAddress,address,
                    halAddress,skuAddress,source,nullptr,true,true,forbiddenPacketRead)==5);
                assert(std::memcmp(leased.data(),foreign.data(),sizeof(leased))==0);
                assert(source.tail==sourceBefore.tail && !backend.releases);
                leased.resource[i]=0;
            }
            for(int failed=0;failed<9;++failed) {
                int calls=0;
                auto fail=[&](uintptr_t p,void* out,size_t n) {return calls++!=failed&&completeRead(p,out,n);};
                assert(!tglPrepareOwnedStateFromChild(owner,childAddress,address,halAddress,skuAddress,
                    source,nullptr,true,true,fail));
                assert(owner.canPrepare() && !backend.releases && source.tail==sourceBefore.tail);
                calls=0;
                assert(tglPublishOwnedStateToBuilder(leased,&leased,childAddress,address,
                    halAddress,skuAddress,source,nullptr,true,true,fail)==5);
                assert(std::memcmp(leased.data(),untouched.data(),sizeof(leased))==0);
            }
            assert(tglPublishOwnedStateToBuilder(leased,&leased,childAddress,address,
                halAddress,skuAddress,source,nullptr,true,true,completeRead)==0);
            uint32_t publishedFlags=0;std::memcpy(&publishedFlags,leased.data(),4);
            assert(publishedFlags==0x1009d);
            for(size_t i=8;i<sizeof(leased);++i) assert(!leased[i]);
            {
                TglOwnedSurfaceInputs surfacePacket;
                TglOwnedDiIecpPacket diPacket;
                TglDiIecpInputs roles;
                roles.boundaryWidth=64;roles.pipe=2;roles.di=true;roles.dnNeeded=true;
                roles.referenceValid=true;roles.current=&source;roles.previous=&target;
                roles.target=&target;roles.statistics=&target;
                roles.ffdi.fill(&target);roles.ffdn.fill(&target);roles.stmm.fill(&target);
                unsigned registrations=0;int admission=0;
                auto surfaceAdmit=[&](const auto&,size_t) noexcept {return admission;};
                auto diAdmit=[&](const auto&) noexcept {return admission;};
                auto registration=[&](const auto&,bool,bool) noexcept {++registrations;return 0;};
                auto control=[](const auto&,uint32_t&,size_t,bool) noexcept {return 0;};
                using Packets=TglOwnedVeboxPacketContext<decltype(completeRead),decltype(surfaceAdmit),
                    decltype(diAdmit),decltype(registration),decltype(control)>;
                Packets packets{{},{childAddress,address,halAddress,skuAddress,source,&target,
                    surfacePacket,leased,diPacket},roles,true,completeRead,surfaceAdmit,
                    diAdmit,registration,control};
                using Resolve=TglOwnedVeboxScopedContext<Packets>;
                using Callbacks=TglOwnedVeboxProducerCallbacks<Resolve>;
                auto actualTarget=reinterpret_cast<uintptr_t>(&target);
                std::memcpy(execution.data()+0x60,&actualTarget,8);
                execution[0x13]=1;
                auto* actualChild=reinterpret_cast<void*>(childAddress);
                constexpr uintptr_t nativeImage=0x100000000,privateTable=0x9000;
                uintptr_t builderEntry=nativeImage+0x129ca0;
                std::array<uint8_t,16> builderAnchor{
                    0x55,0x48,0x89,0xe5,0x48,0x81,0xec,0xe0,0,0,0,0x48,0x8b,0x45,0x18,0x4c};
                auto builderRead=[&](uintptr_t p,void* out,size_t n) {
                    if(p==childAddress && n==8){std::memcpy(out,&privateTable,8);return true;}
                    if((p==privateTable+0x138 || p==nativeImage+0x757dd0) && n==8){std::memcpy(out,&builderEntry,8);return true;}
                    if(p==nativeImage+0x129ca0 && n==16){std::memcpy(out,builderAnchor.data(),16);return true;}
                    return completeRead(p,out,n);
                };
                auto qualified=[](uintptr_t p){return p==nativeImage;};
                auto executable=[](uintptr_t p,size_t n){return p==nativeImage+0x129ca0 && n==16;};
                TglNativeVeboxBuilderBinding binding;
                TglOwnedVeboxBuilderScratch scratch;
                constexpr uintptr_t osAddress=0x7000;
                uintptr_t writebackEntry=nativeImage+0x64d60;
                std::array<uint8_t,16> writebackAnchor{
                    0x55,0x48,0x89,0xe5,0x48,0x89,0x7d,0xf8,0x48,0x89,0x75,0xf0,0x89,0x55,0xec,0x48};
                auto writebackRead=[&](uintptr_t p,void* out,size_t n) {
                    if(p==osAddress+0x2c8 && n==8){std::memcpy(out,&writebackEntry,8);return true;}
                    if(p==nativeImage+0x64d60 && n==16){std::memcpy(out,writebackAnchor.data(),16);return true;}
                    return false;
                };
                auto writebackExecutable=[](uintptr_t p,size_t n){return p==nativeImage+0x64d60 && n==16;};
                unsigned writebacks=0;
                auto writeback=[&](uintptr_t entry,void* os,void* command) noexcept {
                    ++writebacks;assert(entry==writebackEntry && os==reinterpret_cast<void*>(osAddress));
                    assert(command==scratch.command.data());
                };
                assert(tglWritebackNativeCommandHeader(nativeImage,osAddress,scratch,
                    writebackRead,qualified,writebackExecutable,writeback) && writebacks==1);
                ++writebackEntry;
                assert(!tglWritebackNativeCommandHeader(nativeImage,osAddress,scratch,
                    writebackRead,qualified,writebackExecutable,writeback));--writebackEntry;
                writebackAnchor[0]^=1;
                assert(!tglWritebackNativeCommandHeader(nativeImage,osAddress,scratch,
                    writebackRead,qualified,writebackExecutable,writeback));writebackAnchor[0]^=1;
                assert(!tglWritebackNativeCommandHeader(nativeImage,osAddress,scratch,
                    writebackRead,qualified,[](uintptr_t,size_t){return false;},writeback));
                assert(!tglWritebackNativeCommandHeader(nativeImage,UINTPTR_MAX,scratch,
                    writebackRead,qualified,writebackExecutable,writeback) && writebacks==1);
                uintptr_t setupEntry=nativeImage+0x129a10;
                std::array<uint8_t,16> setupAnchor{
                    0x55,0x48,0x89,0xe5,0x48,0x81,0xec,0x80,0,0,0,0x48,0x89,0x7d,0xf8,0x48};
                auto setupRead=[&](uintptr_t p,void* out,size_t n) {
                    if((p==privateTable+0x128 || p==nativeImage+0x757dc0) && n==8){std::memcpy(out,&setupEntry,8);return true;}
                    if(p==nativeImage+0x129a10 && n==16){std::memcpy(out,setupAnchor.data(),16);return true;}
                    return builderRead(p,out,n);
                };
                auto setupExecutable=[](uintptr_t p,size_t n){return p==nativeImage+0x129a10 && n==16;};
                TglNativeVeboxSetupBinding setupBinding;
                uint8_t auxiliary=0;int32_t consumed=77;unsigned setupCalls=0;int setupResult=0;
                auto setupNative=[&](uintptr_t entry,void* child,void* command,
                    void* setup,void* extra,int32_t* count) noexcept {
                    ++setupCalls;
                    assert(entry==setupEntry && child==actualChild && command==scratch.command.data());
                    assert(setup==scratch.setup.data() && extra==&auxiliary && count==&consumed);
                    *count=19;scratch.setup[8]=0x5a;return setupResult;
                };
                assert(setupBinding.call(childAddress,scratch,&auxiliary,consumed,setupRead,setupNative)==5);
                assert(!setupCalls && consumed==77);
                assert(setupBinding.resolve(nativeImage,setupRead,qualified,setupExecutable));
                assert(setupBinding.call(childAddress,scratch,nullptr,consumed,setupRead,setupNative)==5 && !setupCalls);
                assert(setupBinding.call(childAddress,scratch,&auxiliary,consumed,setupRead,setupNative)==0);
                assert(setupCalls==1 && consumed==19 && scratch.setup[8]==0x5a);
                setupResult=31;
                assert(setupBinding.call(childAddress,scratch,&auxiliary,consumed,setupRead,setupNative)==31 && setupCalls==2);
                ++setupEntry;
                assert(setupBinding.call(childAddress,scratch,&auxiliary,consumed,setupRead,setupNative)==5 && setupCalls==2);
                assert(!setupBinding.resolve(nativeImage,setupRead,qualified,setupExecutable));--setupEntry;
                setupAnchor[0]^=1;assert(!setupBinding.resolve(nativeImage,setupRead,qualified,setupExecutable));
                setupAnchor[0]^=1;
                assert(!setupBinding.resolve(nativeImage,setupRead,qualified,[](uintptr_t,size_t){return false;}));
                assert(setupBinding.resolve(nativeImage,setupRead,qualified,setupExecutable));
                unsigned nativeCalls=0;int nativeResult=0;
                auto native=[&](uintptr_t entry,void* child,void* command,void* di,
                    void* surfaces,void* commands,void* state,void* completion,void* setup) noexcept {
                    ++nativeCalls;
                    assert(entry==builderEntry && child==actualChild && command==scratch.command.data());
                    assert(di==&diPacket && surfaces==&surfacePacket && commands==scratch.surfaceCommands.data());
                    assert(state==&leased && completion==scratch.completion.data() && setup==scratch.setup.data());
                    assert(Callbacks::surfaces(child,true,surfaces)==0);
                    assert(Callbacks::state(child,true,state)==0);
                    assert(Callbacks::diIecp(child,true,di)==0);
                    scratch.completion[7]=0x5a;return nativeResult;
                };
                {
                Resolve scope(actualChild,packets);assert(scope.bound());
                assert(binding.call(packets.leases,scratch,builderRead,native)==5 && !nativeCalls);
                assert(binding.resolve(nativeImage,builderRead,qualified,executable));
                assert(packets.pass.withBuilder([&]() noexcept {
                    return binding.call(packets.leases,scratch,builderRead,native);
                })==0 && registrations>0 && nativeCalls==1 && scratch.completion[7]==0x5a);
                nativeResult=-7;
                assert(packets.pass.withBuilder([&]() noexcept {
                    return binding.call(packets.leases,scratch,builderRead,native);
                })==-7 && nativeCalls==2);
                ++builderEntry;
                assert(binding.call(packets.leases,scratch,builderRead,native)==5 && nativeCalls==2);
                assert(!binding.resolve(nativeImage,builderRead,qualified,executable));--builderEntry;
                builderAnchor[0]^=1;assert(!binding.resolve(nativeImage,builderRead,qualified,executable));
                builderAnchor[0]^=1;
                assert(binding.resolve(nativeImage,builderRead,qualified,executable));
                }
                TestVeboxChildLease childLease{actualChild};
                for(int preparation : {0,31,-7}) for(int result : {0,31,-7}) {
                    setupResult=preparation;nativeResult=result;
                    const auto beforeSetup=setupCalls,beforeBuilder=nativeCalls;
                    assert(packets.withNativeBuilder(childLease,setupBinding,binding,scratch,
                        &auxiliary,consumed,setupRead,setupNative,native)==
                        (preparation?preparation:result));
                    assert(setupCalls==beforeSetup+1 &&
                        nativeCalls==beforeBuilder+unsigned(preparation==0));
                    assert(!childLease.busy && !Resolve::get(actualChild));
                }
                const auto beforeSetup=setupCalls,beforeBuilder=nativeCalls;
                childLease.child=&auxiliary;
                assert(packets.withNativeBuilder(childLease,setupBinding,binding,scratch,
                    &auxiliary,consumed,setupRead,setupNative,native)==5);
                assert(setupCalls==beforeSetup && nativeCalls==beforeBuilder && !childLease.busy);
                childLease.child=actualChild;
                Resolve resumed(actualChild,packets);assert(resumed.bound());
                const auto count=registrations;
                const auto savedSurface=surfacePacket;
                assert(packets.surfaces(false,&surfacePacket)==5);
                assert(packets.state(true,&diPacket)==5 && packets.diIecp(true,&leased)==5);
                assert(surfacePacket.surfaces==savedSurface.surfaces && registrations==count);
                uint32_t changedIndex=1;
                std::memcpy(execution.data()+0x30,&changedIndex,4);
                assert(packets.surfaces(true,&surfacePacket)==5 && registrations==count);
                changedIndex=0;std::memcpy(execution.data()+0x30,&changedIndex,4);
                execution[0xb]=0;
                assert(packets.diIecp(true,&diPacket)==5 && registrations==count);
                execution[0xb]=0x81;
                admission=31;
                assert(packets.pass.withBuilder([&]() noexcept {
                    assert(Callbacks::surfaces(actualChild,true,&surfacePacket)==31);
                    assert(Callbacks::state(actualChild,true,&leased)==31);
                    return Callbacks::diIecp(actualChild,true,&diPacket);
                })==31 && registrations==count);
                execution[0x13]=2;
                std::memcpy(execution.data()+0x60,&targetAddress,8);
            }
            struct StateContext {
                TglOwnedVeboxProducerPass pass;
                decltype(completeRead)& read;
                TglOwnedVeboxStateStorage& packet;
                TglOwnedSurfaceStorage& source;
                uintptr_t child,execution,hal,sku;
                int state(bool di,void* output) noexcept {
                    return tglPublishOwnedStateToBuilder(packet,output,child,execution,
                        hal,sku,source,nullptr,di,true,read);
                }
            } context{{},completeRead,leased,source,childAddress,address,halAddress,skuAddress};
            struct StateResolve {
                static StateContext* get(void* child) noexcept {
                    return static_cast<StateContext*>(child);
                }
            };
            using StateCallback=int(*)(void*,bool,void*);
            StateCallback callback=&TglOwnedVeboxProducerCallbacks<StateResolve>::state;
            assert(context.pass.withBuilder([&]() noexcept {
                assert(callback(&context,true,&leased)==0);
                return context.pass.gate([]() noexcept {return 0;});
            })==0);
            leased.resource[0x147]=1;
            const auto livePacket=leased;
            assert(context.pass.withBuilder([&]() noexcept {
                assert(callback(&context,true,&leased)==5);
                return context.pass.gate([]() noexcept {assert(false);return 0;});
            })==5);
            assert(std::memcmp(leased.data(),livePacket.data(),sizeof(leased))==0);
            leased.resource.fill(0);
            assert(tglPrepareOwnedStateFromChild(owner,childAddress,address,halAddress,skuAddress,
                source,nullptr,true,true,completeRead));
            assert(owner.withNative([&](TglOwnedVeboxStateStorage& packet) noexcept {
                uint32_t flags=0;std::memcpy(&flags,packet.data(),4);
                assert(flags==0x1009d && packet[0x180]==0);
                return 31;
            })==31);
            auto forbiddenRead=[](uintptr_t,void*,size_t) {assert(false);return false;};
            assert(!tglPrepareOwnedStateFromChild(owner,childAddress,address,halAddress,skuAddress,
                source,nullptr,true,true,forbiddenRead));
            assert(!backend.releases);owner.reset();assert(backend.releases==1);
        }
        pipe=3; std::memcpy(execution.data()+0xa0c,&pipe,4);
        assert(!tglReadOwnedStateExecution(inputs,address,false,read) && inputs.pipe==2);
        assert(!tglReadOwnedStateExecution(inputs,0,false,read));
        assert(!tglReadOwnedStateExecution(inputs,UINTPTR_MAX,false,read));
    }
    {
        TglOwnedSurfaceStorage source,target;
        const uint32_t format=0x19,type=1,sample=4;
        std::memcpy(source.prefix.data(),&type,4);
        std::memcpy(source.prefix.data()+0x138,&sample,4);
        std::memcpy(source.prefix.data()+0x130,&format,4);
        std::memcpy(target.prefix.data()+0x130,&format,4);
        TglOwnedStateInputs input;
        input.source=&source; input.target=&target; input.pipe=1;
        input.dn=true; input.exec12=true;
        input.chromaIeCp=true;
        std::array<uint8_t,0x188> output;
        output.fill(0xa5);
        assert(tglPrepareOwnedStatePacket(output,input));
        uint32_t flags=0,chroma=0;
        std::memcpy(&flags,output.data(),4);
        std::memcpy(&chroma,output.data()+4,4);
        assert(flags==0xb20adu && chroma==0x804 && output[0xc]==0);
        for (size_t i=8;i<output.size();++i) if (i!=0xc) assert(output[i]==0);
        const auto saved=output;
        const auto savedSource=source.prefix;
        input.pipe=3;
        assert(!tglPrepareOwnedStatePacket(output,input) && output==saved);
        assert(source.prefix==savedSource);
        input.pipe=0; input.source=nullptr;
        assert(!tglPrepareOwnedStatePacket(output,input) && output==saved);
        input.source=&source; input.dn=false; input.exec12=false;
        input.chromaIeCp=true;
        assert(tglPrepareOwnedStatePacket(output,input));
        std::memcpy(&flags,output.data(),4);
        assert((flags&4)==4); // Same exec0f drives leading IECP and chroma.
        input.chromaIeCp=false;
        assert(tglPrepareOwnedStatePacket(output,input));
        std::memcpy(&flags,output.data(),4);
        assert((flags&4)==0);
        assert(source.prefix==savedSource);
    }
    {
        // Whole producer matrix, independently derived flags and exact zero
        // tail contract. Zero180 means native heap path, NOT missing backing.
        TglOwnedSurfaceStorage source;
        const uint32_t format=0x19;
        std::memcpy(source.prefix.data()+0x130,&format,4);
        for(uint32_t pipe=0;pipe<3;++pipe) for(uint32_t bits=0;bits<512;++bits)
            for(uint32_t type=0;type<4;++type) for(uint32_t sample=0;sample<7;++sample) {
                std::memcpy(source.prefix.data(),&type,4);
                std::memcpy(source.prefix.data()+0x138,&sample,4);
                TglOwnedStateInputs in;in.source=&source;in.pipe=pipe;
                in.dn=bits&1;in.di=bits&2;in.referenceValid=bits&4;
                in.exec19=bits&8;in.exec14=bits&16;in.exec12=bits&32;
                in.dnSpecial=bits&64;in.chromaIeCp=bits&128;in.skuSingleVeboxSlice=bits&256;
                const bool history=!in.referenceValid&&(in.dn||in.di);
                uint32_t mode=(pipe==2||history)?2:0;
                if(pipe==1&&!in.exec19)
                    mode=(in.exec14||history||sample==0||sample==1||sample==3||sample==6)?2:1;
                const bool special=!in.di&&in.dn&&(in.dnSpecial||type==1||type==2);
                const uint32_t expected=0x10000u|(mode<<6)|(in.exec12?1:0)|
                    ((pipe||in.chromaIeCp||special)?4:0)|(in.dn?8:0)|(in.di?16:0)|
                    (history?0x20:0)|(special?0x20000:0)|
                    ((pipe==1&&(in.dn||in.di))?0x80000:0)|
                    ((pipe==1&&!in.skuSingleVeboxSlice)?0x2000:0);
                std::array<uint8_t,0x188> packet;packet.fill(0xa5);
                assert(tglPrepareOwnedStatePacket(packet,in));
                uint32_t actual=0;std::memcpy(&actual,packet.data(),4);
                assert(actual==expected);
                for(size_t i=8;i<packet.size();++i) assert(packet[i]==0);
            }
    }
    {
        for (uint32_t pipe=0;pipe<3;++pipe) for (bool e19 : {false,true})
            for (bool e14 : {false,true}) for (bool history : {false,true})
                for (uint32_t surface=0;surface<9;++surface) {
                    uint32_t mode=99;
                    const uint32_t flags=0xffff0000u|(history?0x20:0);
                    assert(tglSelectOwnedStateMode(mode,pipe,e19,e14,flags,surface));
                    uint32_t expected=0;
                    if (pipe==2) expected=2;
                    else if (pipe==1 && !e19) {
                        expected=1;
                        for (auto selected : {0u,1u,3u,6u}) if (surface==selected) expected=2;
                        if (e14||history) expected=2;
                    } else if (history) expected=2;
                    assert(mode==expected);
                }
        uint32_t mode=99;
        assert(!tglSelectOwnedStateMode(mode,3,false,false,0,0) && mode==99);
    }
    {
        for (uint32_t pipe=0;pipe<3;++pipe) for (bool iecp : {false,true})
            for (bool di : {false,true}) for (bool dn : {false,true})
                for (uint32_t original : {0u,0xffffffffu,0xa5a55a5au}) {
                    uint32_t flags=original;
                    assert(tglPrepareOwnedStateLeadingFlags(flags,pipe,iecp,di,dn));
                    const uint32_t expected=(original&~0x8001cu)|
                        ((pipe!=0||iecp)?4:0)|(di?16:0)|(dn?8:0)|
                        ((pipe==1&&(dn||di))?0x80000:0);
                    assert(flags==expected);
                }
        uint32_t flags=0x12345678;
        assert(!tglPrepareOwnedStateLeadingFlags(flags,3,true,true,true));
        assert(flags==0x12345678);
    }
    {
        for (bool dn : {false,true}) for (bool di : {false,true})
            for (bool iecp : {false,true}) for (bool special : {false,true})
                for (bool exec12 : {false,true}) for (uint32_t type=0;type<4;++type)
                    for (uint32_t mode : {0u,1u,2u,3u,0xffffffffu}) {
                        const uint32_t original=0xa5a70000u|(dn?8:0)|(di?16:0)|(iecp?4:0);
                        uint32_t flags=original;
                        tglPrepareOwnedStateDnControls(flags,mode,exec12,special,type);
                        const bool forced=dn&&!di&&(special||type==1||type==2);
                        uint32_t expected=(original&~0x300c1u)|((mode&3)<<6)|0x10000u;
                        if (exec12) expected|=1;
                        if (forced) expected|=0x20004;
                        assert(flags==expected);
                        assert(bool(flags&4)==(iecp||forced));
                        assert((flags&24)==(original&24));
                    }
    }
    {
        for (bool first : {false,true}) for (bool dn : {false,true})
            for (bool di : {false,true}) for (bool suppress : {false,true})
                for (uint32_t pipe=0;pipe<3;++pipe)
                    for (uint32_t original : {0u,0xffffffffu,0xa5a55a5au}) {
                        uint32_t flags=original;
                        assert(tglPrepareOwnedStateHistoryAndPipe(flags,first,dn,di,pipe,suppress));
                        const uint32_t history=(!first && (dn||di)) ? 0x20 : 0;
                        const uint32_t pipeBits=(!suppress && pipe==1) ? 0x2000 : 0;
                        assert(flags==((original&~0x6020u)|history|pipeBits));
                    }
        uint32_t flags=0x12345678;
        assert(!tglPrepareOwnedStateHistoryAndPipe(flags,false,true,true,3,false));
        assert(flags==0x12345678);
    }
    {
        TglOwnedSurfaceStorage source,target;
        const uint32_t nv12=0x19;
        std::memcpy(source.prefix.data()+0x130,&nv12,4);
        std::memcpy(target.prefix.data()+0x130,&nv12,4);
        uint32_t state=0xffffffff;
        const auto untouched=target;
        assert(tglPrepareOwnedChromaSampling(state,nullptr,&target,true,true,2));
        assert(state==0xfffffc00 && std::memcmp(&target,&untouched,sizeof(target))==0);
        state=0xffffffff;
        assert(tglPrepareOwnedChromaSampling(state,&source,nullptr,true,false,2));
        assert(state==0xfffff804);
        uint32_t siting=0;
        std::memcpy(&siting,source.tail.data()+4,4);
        assert(siting==0x21);
        state=0xffffffff;
        assert(tglPrepareOwnedChromaSampling(state,&source,&target,true,true,2));
        assert(state==0xfffff088);
        // Same owned surface is legal: native normalizes in source/target order.
        state=0;
        assert(tglPrepareOwnedChromaSampling(state,&source,&source,true,false,2));
        assert(state==0x84);
        const auto savedSource=source,savedTarget=target;
        const auto savedState=state;
        assert(!tglPrepareOwnedChromaSampling(state,&source,&target,true,true,3));
        assert(state==savedState && std::memcmp(&source,&savedSource,sizeof(source))==0);
        assert(std::memcmp(&target,&savedTarget,sizeof(target))==0);
        // Target metadata is normalized even when downsampling is bypassed.
        target.tail.fill(0);
        assert(tglPrepareOwnedChromaSampling(state,&source,&target,false,false,0));
        std::memcpy(&siting,target.tail.data()+4,4);
        assert(siting==0x21 && (state&0xfff)==0xc00);
    }
    {
        const uint32_t sitings[6]={0x21,0x22,0x11,0x12,0x41,0x42};
        const uint32_t nativeFields[6]={0x80,0xa0,0,0x20,0x100,0x120};
        for (uint32_t pack=0;pack<4;++pack) for (uint32_t type=0;type<6;++type)
            for (uint32_t pipe=0;pipe<3;++pipe)
                for (uint32_t initial : {0u,0xffffffffu,0xa5a55a5au}) {
                    uint32_t state=initial;
                    assert(tglPrepareOwnedChromaDownsampling(state,pack,sitings[type],pipe));
                    const bool active=pipe==2 && (pack==0 || (pack==1 && (type==2 || type==3)));
                    assert(state==((initial&~0xbe0u)|(active ? nativeFields[type] : 0x800)));
                }
        uint32_t state=0xffffffff;
        assert(tglPrepareOwnedChromaDownsampling(state,0,0x77,2));
        assert((state&0xbe0)==0x80);
        const auto saved=state;
        assert(!tglPrepareOwnedChromaDownsampling(state,4,0x21,2) && state==saved);
        assert(!tglPrepareOwnedChromaDownsampling(state,0,0x21,3) && state==saved);
        assert(tglPrepareOwnedChromaDownsampling(state,0,0,2));
        assert((state&0xbe0)==0x800);
    }
    {
        const uint32_t sitings[6]={0x21,0x22,0x11,0x12,0x41,0x42};
        const uint32_t nativeFields[2][6]={{4,5,0,1,8,9},{8,9,0,1,16,17}};
        for (uint32_t pack=0;pack<4;++pack) for (uint32_t type=0;type<6;++type)
            for (bool needed : {false,true}) for (bool di : {false,true})
                for (uint32_t initial : {0u,0xffffffffu,0xa5a55a5au}) {
                    uint32_t state=initial;
                    assert(tglPrepareOwnedChromaUpsampling(state,pack,sitings[type],needed,di));
                    const bool active=needed && (pack==0 || (pack==1 && (type==2 || type==3)));
                    const uint32_t fields=active ? nativeFields[di][type] : 0x400;
                    assert(state == ((initial&~0x41fu)|fields));
                }
        uint32_t state=0xffffffff;
        assert(tglPrepareOwnedChromaUpsampling(state,0,0x77,true,false));
        assert((state&0x41f)==4); // First matching type wins.
        const auto saved=state;
        assert(!tglPrepareOwnedChromaUpsampling(state,4,0x21,true,false));
        assert(state==saved);
        assert(tglPrepareOwnedChromaUpsampling(state,0,0,true,false));
        assert((state&0x41f)==0x400);
    }
    {
        // Pinned jump-table targets: -42/-33/-24/-15 => 420/422/444/other.
        const int8_t targets[90]={
            -24,-24,-24,-24,-24,-24,-24,-15,-15,-24,-24,-24,-33,-33,-33,-33,
            -33,-33,-33,-24,-24,-24,-24,-15,-42,-15,-42,-15,-15,-33,-15,-42,
            -42,-42,-42,-33,-33,-24,-15,-15,-42,-42,-42,-15,-15,-15,-15,-15,
            -15,-15,-15,-15,-15,-15,-15,-15,-15,-15,-15,-15,-15,-15,-15,-15,
            -15,-15,-15,-15,-15,-15,-15,-15,-15,-15,-15,-15,-15,-15,-15,-24,
            -24,-42,-42,-15,-24,-15,-15,-15,-15,-24};
        for (uint32_t format=1;format<=90;++format)
            assert(tglOwnedChromaColorPack(format) == uint32_t((targets[format-1]+42)/9));
        for (uint32_t format : {0u,91u,0xffffffffu}) assert(tglOwnedChromaColorPack(format)==3);
        for (uint32_t pack=0;pack<=3;++pack) for (uint32_t original : {0u,0x21u,0x42u,0xffffffffu}) {
            TglOwnedSurfaceStorage surface;
            surface.prefix.fill(0x5a); surface.resource.fill(0xa5); surface.tail.fill(0x37);
            std::memcpy(surface.tail.data()+4,&original,4);
            const auto saved=surface;
            assert(tglNormalizeOwnedChromaSiting(surface,pack));
            uint32_t actual=0;
            std::memcpy(&actual,surface.tail.data()+4,4);
            uint32_t expected=original ? original : 0x21;
            if (pack == 1) expected=(expected&7)|0x10;
            if (pack == 2) expected=0x11;
            assert(actual == expected && surface.prefix == saved.prefix && surface.resource == saved.resource);
            for (size_t i=0;i<surface.tail.size();++i)
                if (i<4 || i>=8) assert(surface.tail[i] == saved.tail[i]);
            const auto normalized=surface;
            assert(!tglNormalizeOwnedChromaSiting(surface,4));
            assert(std::memcmp(&surface,&normalized,sizeof(surface)) == 0);
        }
    }
    {
        TglOwnedSurfaceStorage target, other;
        const std::array<uint32_t,12> supported{0x19,0xd,0xe,0x10,0xf,0x11,0x15,0x14,3,5,0x53,0x13};
        for (uint32_t format=0;format<0x60;++format) {
            bool accepted=false;
            for (auto allowed : supported) if (allowed == format) accepted=true;
            assert(tglOwnedMmcFormatSupported(format) == accepted);
            std::memcpy(target.prefix.data()+0x130,&format,4);
            for (bool mmc : {false,true}) for (uint32_t execution : {0u,7u,8u})
                for (uint32_t mode : {0u,1u,4u}) {
                    std::memcpy(target.tail.data()+0xc,&mode,4);
                    TglDiIecpControlPolicy policy{mmc,execution,&target};
                    bool enabled=true;
                    assert(policy(target,5,true,enabled) == 0);
                    assert(enabled == (mmc && accepted && execution == 7 && mode == 1));
                    for (size_t slot=0;slot<=6;++slot) {
                        assert(policy(other,slot,false,enabled) == 0 && enabled == mmc);
                    }
                }
        }
        TglDiIecpControlPolicy policy{true,7,&target};
        bool enabled=true;
        assert(policy(other,5,true,enabled) == 5 && !enabled);
        assert(policy(target,4,true,enabled) == 5 && !enabled);
        assert(policy(target,7,false,enabled) == 5 && !enabled);
    }
    {
        struct Context { int registrations=0, controls=0; } context;
        auto registration=+[](void* raw,const void* resource,uint32_t write,uint32_t read)->int {
            auto& c=*static_cast<Context*>(raw); ++c.registrations;
            assert(resource && read == 1 && write <= 1); return 0;
        };
        auto control=+[](void* raw,const TglSurfaceControlParams*,uint32_t* value)->int {
            ++static_cast<Context*>(raw)->controls; *value|=0x80; return 0;
        };
        TglNativeDiIecpServices services;
        services.registration={reinterpret_cast<uintptr_t>(&context),reinterpret_cast<uintptr_t>(registration)};
        services.control={reinterpret_cast<uintptr_t>(&context),reinterpret_cast<uintptr_t>(control)};
        std::array<TglOwnedSurfaceStorage,3> storage{};
        TglDiIecpInputs input;
        input.boundaryWidth=1920; input.pipe=2;
        input.current=&storage[0]; input.target=&storage[1]; input.statistics=&storage[2];
        TglOwnedDiIecpPacket packet;
        auto admit=[](const auto&)->int { return 0; };
        size_t policyCalls=0;
        auto policy=[&](const auto& surface,size_t slot,bool direct,bool& enabled)->int {
            ++policyCalls;
            assert((slot == 0 && !direct && &surface == input.current) ||
                   (slot == 5 && direct && &surface == input.target));
            enabled=direct; return 0;
        };
        assert(tglPrepareNativeDiIecpPacket(packet,input,services,admit,policy) == 0);
        assert(context.registrations == 3 && context.controls == 1 && policyCalls == 2);
        assert(packet.controls[0] == 0 && packet.controls[5] == 0x80 && packet.controls[7] == 0);
        const auto saved=packet;
        auto deny=[](const auto&,size_t,bool,bool&)->int { return 25; };
        assert(tglPrepareNativeDiIecpPacket(packet,input,services,admit,deny) == 25);
        assert(std::memcmp(&packet,&saved,sizeof(packet)) == 0 && context.controls == 1);
        services.control.entry=0;
        const auto registrations=context.registrations;
        assert(tglPrepareNativeDiIecpPacket(packet,input,services,admit,policy) == 5);
        assert(context.registrations == registrations && std::memcmp(&packet,&saved,sizeof(packet)) == 0);
    }
    {
        TglOwnedSurfaceStorage surface;
        surface.tail[0xa]=0xff;
        uint32_t mode=4;
        std::memcpy(surface.tail.data()+0xc,&mode,4);
        auto params=tglSurfaceControlParams(surface);
        assert(params.compressed == 1 && params.mode == 4);
        assert((params.padding == std::array<uint8_t,3>{}));
        struct Context { int calls=0; uint32_t* expected; };
        uint32_t value=0x1000;
        Context context{0,&value};
        auto control = +[](void* raw,const TglSurfaceControlParams* p,uint32_t* out)->int {
            auto& c=*static_cast<Context*>(raw);
            ++c.calls;
            assert(p->compressed == 1 && p->mode == 4 && out == c.expected);
            *out|=0x180;
            return 31;
        };
        assert(TglNativeSurfaceControlInvoker{}(reinterpret_cast<uintptr_t>(control),
            reinterpret_cast<uintptr_t>(&context),surface,value) == 31);
        assert(context.calls == 1 && value == 0x1180);
        surface.tail[0xa]=0xfe;
        mode=0xffffffff;
        std::memcpy(surface.tail.data()+0xc,&mode,4);
        params=tglSurfaceControlParams(surface);
        assert(params.compressed == 0 && params.mode == 0xffffffff);
    }
    {
        std::array<TglOwnedSurfaceStorage,12> storage{};
        TglDiIecpInputs input;
        input.boundaryWidth=1920; input.di=true; input.referenceValid=true; input.dnNeeded=true;
        input.current=&storage[0]; input.previous=&storage[1]; input.statistics=&storage[2];
        input.ffdi={&storage[3],&storage[4],&storage[5],&storage[6]};
        input.ffdn={&storage[7],&storage[8]}; input.stmm={&storage[9],&storage[10]};
        input.indices.frame0=2; input.indices.frame1=3; input.indices.dnOut=1;
        input.indices.historyIn=1; input.indices.historyOut=0;
        for (size_t i=0;i<input.controls.size();++i) input.controls[i]=uint32_t(100+i);
        TglOwnedDiIecpPacket packet;
        packet.controls[11]=0xfeed;
        auto original=packet;
        size_t admits=0, registrations=0, controls=0, failAdmit=0, failRegister=0, failControl=0;
        std::vector<const void*> order;
        std::vector<bool> writes;
        auto prepare = [&] {
            admits=registrations=controls=0; order.clear(); writes.clear();
            return tglPublishOwnedDiIecpToBuilder(packet,&packet,input,
                [&](const auto&)->int { return ++admits == failAdmit ? 37 : 0; },
                [&](const auto& resource,bool write,bool read)->int {
                    assert(read); order.push_back(&resource); writes.push_back(write);
                    return ++registrations == failRegister ? 31 : 0;
                },
                [&](const auto&,uint32_t& value,size_t slot,bool directTarget)->int {
                    assert(slot != 7 && !directTarget);
                    value+=0x100; return ++controls == failControl ? 25 : 0;
                });
        };
        assert(tglPublishOwnedDiIecpToBuilder(packet,nullptr,input,
            [](const auto&)->int {assert(false);return 0;},
            [](const auto&,bool,bool)->int {assert(false);return 0;},
            [](const auto&,uint32_t&,size_t,bool)->int {assert(false);return 0;})==5);
        assert(std::memcmp(&packet,&original,sizeof(packet))==0);
        assert(prepare() == 0 && admits == 8 && registrations == 8 && controls == 7);
        const std::array<size_t,8> expected{0,1,6,5,8,10,9,2};
        for (size_t i=0;i<expected.size();++i) assert(order[i] == &storage[expected[i]].resource);
        assert((writes == std::vector<bool>{false,false,true,true,true,false,true,true}));
        assert(packet.resources[5] == &storage[6].resource && packet.resources[6] == &storage[5].resource);
        assert(packet.resources[2] == &storage[10].resource && packet.resources[3] == &storage[9].resource);
        assert(packet.controls[7] == 107 && packet.controls[5] == 0x169 && packet.controls[11] == 0);
        for (failAdmit=1;failAdmit<=8;++failAdmit) {
            packet=original; assert(prepare() == 37 && registrations == 0);
            assert(std::memcmp(&packet,&original,sizeof(packet)) == 0);
        }
        failAdmit=0;
        for (failRegister=1;failRegister<=8;++failRegister) {
            packet=original; assert(prepare() == 31 && registrations == failRegister);
            assert(std::memcmp(&packet,&original,sizeof(packet)) == 0);
        }
        failRegister=0;
        for (failControl=1;failControl<=7;++failControl) {
            packet=original; assert(prepare() == 25 && controls == failControl);
            assert(std::memcmp(&packet,&original,sizeof(packet)) == 0);
        }
        failControl=0;
        input.di=false; input.referenceValid=false; input.pipe=1;
        assert(prepare() == 0 && registrations == 4 && controls == 3);
        assert(packet.resources[4] == &storage[8].resource && packet.resources[5] == &storage[4].resource);
        assert(!packet.resources[1] && !packet.resources[2] && !packet.resources[6]);
        input.ffdn[1]=nullptr; packet=original;
        assert(prepare() == 5 && admits == 0 && registrations == 0);
        assert(std::memcmp(&packet,&original,sizeof(packet)) == 0);
        // All supported producer branches must retain every resource occurrence,
        // including repeated slot5 writes: registration is not deduplicated.
        input.ffdn[1]=&storage[8];
        for(uint32_t pipe=0;pipe<3;++pipe) for(uint32_t flags=0;flags<32;++flags) {
            input.pipe=pipe;input.di=flags&1;input.iecp=flags&2;
            input.dnNeeded=flags&4;input.stmmNeeded=flags&8;
            input.referenceValid=flags&16;input.target=&storage[11];
            std::vector<const void*> wanted{&storage[0].resource};
            std::vector<bool> wantedWrites{false};
            auto append=[&](size_t index,bool write) {
                wanted.push_back(&storage[index].resource);wantedWrites.push_back(write);
            };
            if(input.referenceValid) append(1,false);
            if(pipe==2) append(11,true);
            else if(input.di) {append(6,true);append(5,true);}
            else if(input.iecp) append(4,true);
            if(input.dnNeeded) {append(8,true);if(pipe==1 && !input.di) append(4,true);}
            if(input.di || input.stmmNeeded) {append(10,false);append(9,true);}
            append(2,true);
            admits=registrations=controls=0;order.clear();writes.clear();
            assert(tglPublishOwnedDiIecpToBuilder(packet,&packet,input,
                [&](const auto&)->int {++admits;return 0;},
                [&](const auto& resource,bool write,bool read)->int {
                    assert(read && admits==wanted.size());
                    order.push_back(&resource);writes.push_back(write);++registrations;return 0;
                },
                [&](const auto&,uint32_t&,size_t slot,bool direct)->int {
                    assert(slot!=7 && direct==(pipe==2 && slot==5));++controls;return 0;
                })==0);
            assert(order==wanted && writes==wantedWrites && admits==wanted.size());
            assert(registrations==wanted.size() && controls+1==registrations);
            // Match the qualified donor producer, without inventing optional
            // command resources or treating their absence as universal support.
            assert(!packet.resources[8] && !packet.resources[9] && !packet.resources[10]);
            // Each published reference was admitted and registered, even when
            // a later native-compatible write replaces the same packet slot.
            for(const auto* resource:packet.resources) if(resource) {
                bool found=false;for(const void* registered:order) found|=registered==resource;
                assert(found);
            }
        }
    }
    {
        std::array<TglOwnedSurfaceStorage,5> storage{};
        std::array<const TglOwnedSurfaceStorage*,4> ffdi{
            &storage[0],&storage[1],&storage[2],&storage[3]};
        uint32_t offset=0x1234;
        std::memcpy(storage[4].prefix.data()+0x144,&offset,4);
        TglSurfaceIndices indices; indices.frame0=2; indices.frame1=3; indices.dnOut=1;
        TglDiIecpOutputs output;
        assert(tglSelectDiIecpOutputs(output,2,true,true,indices,&storage[4],ffdi));
        assert(output.current == &storage[4] && !output.previous && output.currentOffset == offset);
        for (uint32_t pipe : {0u,1u}) {
            assert(tglSelectDiIecpOutputs(output,pipe,true,true,indices,nullptr,ffdi));
            assert(output.current == ffdi[3] && output.previous == ffdi[2] && output.currentOffset == 0);
            assert(tglSelectDiIecpOutputs(output,pipe,false,true,indices,nullptr,ffdi));
            assert(output.current == ffdi[1] && !output.previous && output.currentOffset == 0);
            assert(tglSelectDiIecpOutputs(output,pipe,false,false,indices,nullptr,ffdi));
            assert(!output.current && !output.previous && output.currentOffset == 0);
        }
        output={&storage[4],&storage[3],99};
        auto reject = [&](uint32_t pipe,bool di,bool iecp) {
            assert(!tglSelectDiIecpOutputs(output,pipe,di,iecp,indices,nullptr,ffdi));
            assert(output.current == &storage[4] && output.previous == &storage[3] && output.currentOffset == 99);
        };
        reject(3,false,false); reject(2,false,false);
        indices.frame0=-1; reject(0,true,false); indices.frame0=4; reject(0,true,false); indices.frame0=2;
        indices.frame1=-1; reject(0,true,false); indices.frame1=4; reject(0,true,false); indices.frame1=3;
        indices.dnOut=-1; reject(0,false,true); indices.dnOut=2; reject(0,false,true); indices.dnOut=1;
        ffdi[2]=nullptr; reject(0,true,false); ffdi[2]=&storage[2];
        ffdi[3]=nullptr; reject(0,true,false); ffdi[3]=&storage[3];
        ffdi[1]=nullptr; reject(0,false,true);
    }
    {
        constexpr uintptr_t image=0x10000000, mhw=0x20000000;
        uintptr_t table=image+0x759520, native=image+0x172080;
        uintptr_t controlNative=image+0x179530;
        std::array<uint8_t,8> controlCode{0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x50};
        std::array<uint8_t,11> code{
            0x55,0x48,0x89,0xe5,0x48,0x81,0xec,0x40,0x01,0x00,0x00};
        size_t denied=0, reads=0;
        auto read = [&](uintptr_t address,void* out,size_t size) {
            if (++reads == denied) return false;
            if (address == mhw && size == 8) std::memcpy(out,&table,8);
            else if (address == image+0x759548 && size == 8) std::memcpy(out,&native,8);
            else if (address == image+0x172080 && size == code.size())
                std::memcpy(out,code.data(),size);
            else if (address == image+0x759580 && size == 8)
                std::memcpy(out,&controlNative,8);
            else if (address == image+0x179530 && size == controlCode.size())
                std::memcpy(out,controlCode.data(),size);
            else return false;
            return true;
        };
        bool imageOk=true, rx=true;
        auto qualify = [&](uintptr_t base) { return imageOk && base == image; };
        auto executable = [&](uintptr_t address,size_t size) {
            return rx && ((address == image+0x172080 && size == code.size()) ||
                          (address == image+0x179530 && size == controlCode.size()));
        };
        TglNativeDiIecpBinding binding;
        auto resolve = [&] { reads=0; return binding.resolve(mhw,image,read,qualify,executable); };
        assert(resolve() && binding.context == mhw && binding.entry == native);
        auto rejected = [&] {
            assert(!resolve()); assert(binding.context == 0 && binding.entry == 0);
        };
        for (denied=1;denied<=3;++denied) rejected();
        denied=0;
        for (auto& byte : code) { byte^=1; rejected(); byte^=1; }
        ++table; rejected(); --table;
        ++native; rejected(); --native;
        imageOk=false; rejected(); imageOk=true;
        rx=false; rejected(); rx=true;
        assert(resolve());
        assert(!binding.resolve(UINTPTR_MAX,image,read,qualify,executable));
        assert(binding.context == 0 && binding.entry == 0);
        assert(!binding.resolve(mhw,UINTPTR_MAX,read,qualify,executable));
        assert(!binding.resolve(0,image,read,qualify,executable));
        assert(!binding.resolve(mhw,0,read,qualify,executable));
        TglNativeSurfaceControlBinding controlBinding;
        auto resolveControl = [&] {
            reads=0; return controlBinding.resolve(mhw,image,read,qualify,executable);
        };
        assert(resolveControl() && controlBinding.context == mhw && controlBinding.entry == controlNative);
        auto rejectControl = [&] {
            assert(!resolveControl());
            assert(controlBinding.context == 0 && controlBinding.entry == 0);
        };
        for (denied=1;denied<=5;++denied) rejectControl();
        denied=0;
        for (auto& byte : controlCode) { byte^=1; rejectControl(); byte^=1; }
        ++controlNative; rejectControl(); --controlNative;
        ++table; rejectControl(); --table;
        rx=false; rejectControl(); rx=true;
        imageOk=false; rejectControl(); imageOk=true;
        assert(resolveControl());
        assert(!controlBinding.resolve(mhw,UINTPTR_MAX,read,qualify,executable));
        assert(controlBinding.context == 0 && controlBinding.entry == 0);
    }
    {
        TglOwnedDiIecpPacket packet;
        const std::array<uint8_t,0xa8> zero{};
        assert(std::memcmp(&packet,zero.data(),zero.size()) == 0);
        TglOwnedSurfaceStorage storage;
        auto* bytes = reinterpret_cast<const uint8_t*>(&packet);
        for (size_t i=0;i<packet.resources.size();++i) {
            packet.resources[i] = &storage.resource;
            uintptr_t value = 0;
            std::memcpy(&value,bytes+0x20+i*8,8);
            assert(value == reinterpret_cast<uintptr_t>(&storage.resource));
        }
        for (size_t i=0;i<packet.controls.size();++i) {
            packet.controls[i] = uint32_t(0x100+i);
            uint32_t value = 0;
            std::memcpy(&value,bytes+0x78+i*4,4);
            assert(value == 0x100+i);
        }
        struct Context { int calls=0; const TglOwnedDiIecpPacket* expected; void* buffer; };
        int commandBuffer = 0;
        Context context{0,&packet,&commandBuffer};
        auto emit = +[](void* raw,void* buffer,const TglOwnedDiIecpPacket* input)->int {
            auto& c = *static_cast<Context*>(raw);
            ++c.calls;
            assert(input == c.expected && buffer == c.buffer);
            return 31;
        };
        assert(TglNativeDiIecpInvoker{}(reinterpret_cast<uintptr_t>(emit),
            reinterpret_cast<uintptr_t>(&context),&commandBuffer,packet) == 31);
        assert(context.calls == 1);
        auto saved = packet;
        assert(!tglInitializeDiIecpPacket(packet,0));
        assert(std::memcmp(&packet,&saved,sizeof(packet)) == 0);
        assert(!tglInitializeDiIecpPacket(packet,0x4001));
        assert(std::memcmp(&packet,&saved,sizeof(packet)) == 0);
        for (uint32_t width : {1u,1920u,0x4000u}) {
            assert(tglInitializeDiIecpPacket(packet,width));
            uint32_t end=0, start=1;
            std::memcpy(&end,packet.prefix.data(),4);
            std::memcpy(&start,packet.prefix.data()+4,4);
            assert(end == width-1 && start == 0);
            auto expected = zero;
            std::memcpy(expected.data(),&end,4);
            assert(std::memcmp(&packet,expected.data(),expected.size()) == 0);
        }
    }
    testTrackedCpuOwner();
    testOwnedVeboxVtable();
    testNativeCscBinding();
    testDirectVeboxFeasibility();
    testVeboxHardwareBinding();
    testVeboxRenderBinding();
    testExecutionBinding();
    testExecutionShape();
    testExecutionOwner();
    testVeboxReport();
    testVeboxPrefix();
    testVeboxStateNativeOwnership();
    testExternalResourceStorage();
    testSurfaceStatisticsTransaction();
    testComposedStatisticsBuffer();
    testBufferFill();
    testStatisticsLifecycle();
    testSurfaceBoundary();
    testStatisticsOffsets();
    testSurfaceDescriptor();
    testOwnedResourceLifecycle();
    testOwnedResourceGroupLifetime();
    testNativeResourceBoundary();
    testResourceBinding();
    std::vector<uint8_t> blob(TglKernelMetadataAdapter::blobBytes, 0x5a);
    const uint32_t offsets[] = {344224, 344448};
    std::memcpy(blob.data() + 78 * 4, offsets, sizeof(offsets));
    std::memcpy(blob.data() + TglKernelMetadataAdapter::metadataOffset,
        TglKernelMetadataAdapter::pinned.data(), sizeof(TglKernelMetadataAdapter::pinned));
    const auto originalBlob = blob;
    assert(TglKernelMetadataAdapter::translate(blob));
    for (size_t i = 0; i < blob.size(); ++i)
        if (i < TglKernelMetadataAdapter::metadataOffset ||
            i >= TglKernelMetadataAdapter::metadataOffset + sizeof(TglKernelMetadataAdapter::pinned))
            assert(blob[i] == originalBlob[i]);
    uint32_t converted[56]{};
    std::memcpy(converted, blob.data() + TglKernelMetadataAdapter::metadataOffset, sizeof(converted));
    assert(converted[0] == 0x10000 && converted[2] == 13 && converted[3] == 13);
    for (unsigned i = 4; i < 56; i += 2) {
        assert(converted[i] == TglKernelMetadataAdapter::pinned[i + 1]);
        assert(converted[i + 1] == TglKernelMetadataAdapter::pinned[i]);
    }
    for (size_t i = 0; i < sizeof(TglKernelMetadataAdapter::pinned); ++i) {
        auto bad = originalBlob;
        bad[TglKernelMetadataAdapter::metadataOffset + i] ^= 1;
        const auto savedBad = bad;
        assert(!TglKernelMetadataAdapter::translate(bad) && bad == savedBad);
    }
    auto badSize = originalBlob;
    badSize.pop_back();
    assert(!TglKernelMetadataAdapter::translate(badSize));
    auto badOffset = originalBlob;
    badOffset[78 * 4] ^= 1;
    assert(!TglKernelMetadataAdapter::translate(badOffset));
    std::array<uint8_t, 0x430> native{};
    for (size_t i = 0; i < native.size(); ++i)
        native[i] = static_cast<uint8_t>((i * 17 + i / 256) & 255);
    const auto original = native;
    std::array<uint8_t, 0x438> consumer{};
    std::memcpy(consumer.data(), native.data(), 0x18);
    std::memcpy(consumer.data() + 0x20, native.data() + 0x18,
                native.size() - 0x18);
    for (size_t offset : {size_t(0x20), size_t(0x24),
                          size_t(0x228), size_t(0x428)}) {
        assert(std::memcmp(native.data() + offset,
                           consumer.data() + offset + 8, 8) == 0);
    }
    assert(std::memcmp(native.data(), consumer.data(), 0x18) == 0);
    assert(native == original);
    TglCapabilityAdapter::Native first{}, second{};
    uintptr_t link = reinterpret_cast<uintptr_t>(&second);
    std::memcpy(first.data() + 0x428, &link, sizeof(link));
    const auto saved = first;
    TglCapabilityAdapter adapter;
    assert(adapter.build({&first, &second}));
    uintptr_t convertedLink = 0;
    std::memcpy(&convertedLink, adapter.consumer(0)->data() + 0x430, 8);
    assert(convertedLink == reinterpret_cast<uintptr_t>(adapter.consumer(1)));
    assert(adapter.native(adapter.consumer(1)) == &second);
    assert(first == saved);
    assert(!adapter.build({&first, &first}));
    assert(!adapter.consumer(0));
    assert(!adapter.build({&first})); // foreign link
    link = reinterpret_cast<uintptr_t>(&first);
    std::memcpy(second.data() + 0x428, &link, 8);
    assert(!adapter.build({&first, &second})); // cycle
    assert(!adapter.build({nullptr}));
    assert(!adapter.build({}));
    second.fill(0);
    std::array<uintptr_t, 35> roots{};
    roots.fill(reinterpret_cast<uintptr_t>(&first));
    // Group-list bytes remain native-owned and opaque to the layout bridge.
    const std::array<uint64_t, 3> groups{{0x11, 0x22, 0x33}};
    AvdDescriptor descriptor{3, 0, reinterpret_cast<uintptr_t>(groups.data()),
                             35, 0, reinterpret_cast<uintptr_t>(roots.data())};
    auto read = [](uintptr_t address, void *out, size_t size) {
        if (!address) return false;
        std::memcpy(out, reinterpret_cast<const void *>(address), size);
        return true;
    };
    AvdDescriptorBridge bridge;
    assert(bridge.build(reinterpret_cast<uintptr_t>(&descriptor), read));
    assert(bridge.descriptor()->groupList == descriptor.groupList);
    assert(bridge.descriptor()->groups == descriptor.groups);
    assert(bridge.descriptor()->count == descriptor.count);
    assert(bridge.descriptor()->reserved == descriptor.reserved);
    assert(bridge.descriptor()->reserved2 == descriptor.reserved2);
    assert((groups == std::array<uint64_t, 3>{{0x11, 0x22, 0x33}}));
    auto convertedRoots = reinterpret_cast<const uintptr_t *>(bridge.descriptor()->roots);
    assert(convertedRoots[0] != roots[0]);
    assert(convertedRoots[0] == convertedRoots[34]);
    assert(first == saved);
    // Native create matches every linked node and requires simultaneous ends.
    // Exercise a decoder -> processing -> scaler topology with shared roots.
    TglCapabilityAdapter::Native decoder{}, processing{}, scaler{};
    auto header = [](auto &record, uint32_t type, uint64_t usage) {
        const uint64_t group = 1001;
        std::memcpy(record.data(), &type, sizeof(type));
        std::memcpy(record.data() + 8, &usage, sizeof(usage));
        std::memcpy(record.data() + 16, &group, sizeof(group));
    };
    header(decoder, 1, 0x200);
    header(processing, 0x10, 1);
    header(scaler, 2, 0x800);
    link = reinterpret_cast<uintptr_t>(&processing);
    std::memcpy(decoder.data() + 0x428, &link, sizeof(link));
    link = reinterpret_cast<uintptr_t>(&scaler);
    std::memcpy(processing.data() + 0x428, &link, sizeof(link));
    const auto savedDecoder = decoder, savedProcessing = processing, savedScaler = scaler;
    roots.fill(reinterpret_cast<uintptr_t>(&decoder));
    roots[1] = reinterpret_cast<uintptr_t>(&processing);
    roots[2] = reinterpret_cast<uintptr_t>(&scaler);
    assert(bridge.build(reinterpret_cast<uintptr_t>(&descriptor), read));
    convertedRoots = reinterpret_cast<const uintptr_t *>(bridge.descriptor()->roots);
    uintptr_t next = 0;
    for (size_t i = 0; i < 3; ++i) {
        const auto *converted = reinterpret_cast<const uint8_t *>(convertedRoots[i]);
        const auto *source = reinterpret_cast<const uint8_t *>(roots[i]);
        assert(std::memcmp(converted, source, 0x18) == 0);
        assert(std::memcmp(converted + 0x20, source + 0x18, 0x410) == 0);
        std::memcpy(&next, converted + 0x430, sizeof(next));
        assert(next == (i == 2 ? 0 : convertedRoots[i + 1]));
    }
    assert(convertedRoots[34] == convertedRoots[0]);
    assert(decoder == savedDecoder && processing == savedProcessing && scaler == savedScaler);
    auto unpublished = [&] {
        const AvdDescriptor empty{};
        assert(std::memcmp(bridge.descriptor(), &empty, sizeof(empty)) == 0);
    };
    descriptor.count = 34;
    assert(!bridge.build(reinterpret_cast<uintptr_t>(&descriptor), read));
    unpublished();
    descriptor.count = 35;
    assert(bridge.build(reinterpret_cast<uintptr_t>(&descriptor), read));
    auto denyRead = [](uintptr_t, void *, size_t) { return false; };
    assert(!bridge.build(reinterpret_cast<uintptr_t>(&descriptor), denyRead));
    unpublished();
    assert(bridge.build(reinterpret_cast<uintptr_t>(&descriptor), read));
    link = reinterpret_cast<uintptr_t>(&decoder);
    std::memcpy(scaler.data() + 0x428, &link, sizeof(link));
    assert(!bridge.build(reinterpret_cast<uintptr_t>(&descriptor), read));
    unpublished();
}
