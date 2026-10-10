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
    template<class Invoke> int withPrivateChild(Invoke invoke) noexcept {
        if(busy || !child) return 5;
        busy=true;const int result=invoke(child);busy=false;return result;
    }
};

static void testTrackedCpuOwner() {
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
    for(size_t slot:{size_t(0x40/8),size_t(0x48/8),size_t(0xd0/8)}) {
        const auto native=original[slot];
        original[slot]=image+0x2048;assert(!build() && !table.addressPoint());
        original[slot]=native;
    }
    assert(build());
    assert(table.entries[0x40/8]==image+0x12e9c0 && table.entries[0x48/8]==image+0x12ea30);
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
    bool identity = true, readable = true;
    auto read = [&](uintptr_t p, void* out, size_t n) {
        const auto value = p == mhw ? &vt : p == mhw + 0x18 ? &actualOs :
                           p == mhw + 0x20 ? &heap : nullptr;
        if (!readable || !value || n != 8) return false;
        std::memcpy(out, value, n); return true;
    };
    auto qualify = [&](uintptr_t p) { return identity && p == image; };
    TglVeboxHardwareBinding b;
    auto resolve = [&] { return b.resolve(mhw, os, image, read, qualify); };
    assert(resolve() && b.interface == mhw && b.os == os && b.heap == heap);
    auto empty = [&] { assert(!b.interface && !b.os && !b.heap); };
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
        TglOwnedVeboxBuilderScratch scratch;
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

static void testExecutionBinding() {
    constexpr uintptr_t base = 0x1000000;
    constexpr std::array<uintptr_t, 3> offsets{0x12ebc0, 0x12e420, 0x12e3f0};
    std::array<std::array<uint8_t, 16>, 3> bytes{{
        {0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x40,0x48,0x8b,0x35,0x59,0x54,0x62,0,0xbf},
        {0x55,0x48,0x89,0xe5,0x48,0x81,0xec,0xb0,9,0,0,0x48,0x8b,5,0x56,0x5c},
        {0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x10,0x48,0x89,0x7d,0xf8,0x48,0x8b,0x45,0xf8}
    }};
    int denied = -1;
    bool identity = true, rx = true;
    uint32_t allocationSize=0xd48;
    auto read = [&](uintptr_t p, void* out, size_t n) {
        if(p==base+0x12ebd0 && n==4) {std::memcpy(out,&allocationSize,4);return true;}
        for (size_t i = 0; i < offsets.size(); ++i)
            if (p == base + offsets[i] && n == 16 && int(i) != denied) {
                std::memcpy(out, bytes[i].data(), n); return true;
            }
        return false;
    };
    auto qualify = [&](uintptr_t p) { return identity && p == base; };
    auto executable = [&](uintptr_t, size_t n) { return rx && (n == 16 || n==4); };
    TglExecutionBinding b;
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
        if(p==image+0x12ebd0 && n==4 && codeValid) {
            const uint32_t allocationSize=0xd48;std::memcpy(out,&allocationSize,4);return true;
        }
        if (n == 16 && codeValid) {
            const std::array<uintptr_t, 3> offsets{0x12ebc0, 0x12e420, 0x12e3f0};
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
            assert(entry == 0x1000000 + 0x12e3f0 && p == 0x2000); ++calls->destroys;
        }
    } invoke{&calls};
    TglExecutionBinding binding{image, image + 0x12ebc0, image + 0x12e420, image + 0x12e3f0};
    auto executable = [&](uintptr_t, size_t n) { return codeRx && (n == 16 || n==4); };
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
        }
        for (unsigned failure = 0; failure < 3; ++failure) {
            Backend b; Group group(b);
            auto prepare = [&](size_t i, const auto&, bool) noexcept { return i == failure ? 77 : 0; };
            assert(group.ensure(requests,prepare) == 77 && !b.live);
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
            assert(owner.ensure(key)==5 && !owner.copyFrameMetadata(source));
            assert(!owner.surface());
            assert(!owner.withSurface([&](const auto&){assert(false);}));
            assert(borrowed.resource==snapshot.resource && borrowed.prefix==snapshot.prefix);
            assert(fixture.allocations==borrowedAllocations && fixture.releases==borrowedReleases);
        }));
        assert(borrowedCalls==1 && owner.surface()==surface);
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
                if(lockError || !valid) assert(mapped==original);
                else assert(mapped==filled);
            }
        Api badExtent{{mapped.data(),39,8,3,16}};
        const auto failed=tglInitializeOwnedStmmSurface(1,badExtent);
        assert(!failed.initialized && badExtent.unlocks==1 && !failed.unlockStatus);
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
            return !descriptorOnly && tglNativeBackingPresent(s);
        }
    } backend;
    {
        TglOwnedSfcLineBuffers<Backend> buffers(backend);
        backend.owner=&buffers;
        backend.reenter=[](void* pointer) {
            auto& owner=*static_cast<TglOwnedSfcLineBuffers<Backend>*>(pointer);
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
        for(int status:{0,31,-7}) {
            const auto allocations=backend.allocations,releases=backend.releases;
            const auto* avs=buffers.storage(0);const auto* ief=buffers.storage(1);
            assert(buffers.withBuffers(1080,720,[&]() noexcept {
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
        buffers.reset();
        assert(!backend.live && !buffers.storage(0));
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
    }
    assert(!backend.live && backend.allocations==backend.releases);
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
        std::memcpy(prefix.data()+0x34,&inputHeight,4);
        std::memcpy(prefix.data()+0x50,&scaledHeight,4);
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
        const auto validPrefix=prefix;
        const uint32_t mismatch=1081;std::memcpy(prefix.data()+0x34,&mismatch,4);
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
                config,sine,pinnedQuery,[&](auto& ready) noexcept {
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
        queryResult=89;
        assert(tglWithOwnedSfcCommandInputs(commands,state,avs,cache,buffers,
            output,prefix,{},true,false,false,TglOwnedSfcCompressionMode::Disabled,
            config,sine,pinnedQuery,[](auto&) noexcept {assert(false);return 0;})==89);
        queryResult=0;
        avs.reset();assert(!avsBackend.live && !avs.parameters());
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
        assert(tglWithOwnedSfcAvsTables(owner,tables,0x19,0.f,.75f,0x11,
            false,true,0.f,0.f,sine,never)==5);
        assert(real.bytes==savedBytes && *owner.parameters()==savedHeader);
        assert(tables.yX==savedCache.yX && tables.uvY==savedCache.uvY &&
            tables.format==savedCache.format && tables.scaleX==savedCache.scaleX);
    }
    assert(real.releases==1);
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
        std::memcpy(expected.data()+0x30,&width,4);
        std::memcpy(expected.data()+0x34,&height,4);
        std::memcpy(expected.data()+0x38,&frameMode,4);
        tglPrepareOwnedSfcFrameFields(packet,format,mode,width,height);
        assert(packet==expected);
    }
}

int main() {
    {
        struct Context {
            void* child;void* source;void* target;void* data;void* command;
            bool leased=false;int result=0;unsigned setups=0,sends=0,frees=0;
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
            uintptr_t resource=0;
            int stage(int id){order[count++]=id;return failure==id?-id:0;}
            int lock(const std::array<uint8_t,8>& p){assert(&p==&expected->lock && p[0]==1);return stage(1);}
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
    }
    {
        constexpr uintptr_t base=0x10000000,os=0x20000000;
        struct Invoke {
            unsigned constructs=0,destroys=0;
            bool corrupt=false;
            void construct(uintptr_t entry,void *object,uintptr_t borrowed) noexcept {
                assert(entry==base+0x16bdd0 && borrowed==os);
                ++constructs;
                auto *bytes=static_cast<uint8_t*>(object);
                auto put=[&](size_t offset,auto value){std::memcpy(bytes+offset,&value,sizeof(value));};
                put(0,uintptr_t(base+0x759420));put(8,uintptr_t(base+0x3eae0));
                put(0x10,borrowed);put(0x18,uint16_t(16));put(0x1a,uint16_t(4));
                bytes[0x40]=corrupt?0:1;
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
                        nullptr,nullptr,nullptr,nullptr,nullptr,status,read,construct);
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
                auto helperRead=[&](uintptr_t address,void* output,size_t size) noexcept {
                    assert(address==base+0x126be0 && size==16);
                    constexpr uint8_t bytes[]={0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x40,
                        0x48,0x8b,0x35,0x39,0xd4,0x62,0x00,0xbf};
                    std::memcpy(output,bytes,16); return true;
                };
                unsigned helperCreates=0;
                auto helperExecutable=[&](uintptr_t entry,size_t size) noexcept {
                    return entry==base+0x126be0 && size==16;
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
                const auto unbound=hooks;
                assert(owner.withPrivateChild([](void*) noexcept {assert(false);return 0;})==5);
                hooks=tglBindOwnedVeboxQueryHooks<DeniedFormatRead>(hooks);
                assert(owner.installPrivateVtableWithQueries<DeniedFormatRead>(
                    unbound,tableRead,qualify,executable));
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
            assert(owner.initialize(lifetime,mhw,os,read) && owner.get());
            assert(invoke.constructs==1 && invoke.destroys==0);
            const std::array<uintptr_t,11> offsets{
                0x16bed0,0x16bef0,0x1681e0,0x168390,0x16a580,0x16a770,
                0x16a8a0,0x16b190,0x16b390,0x16b590,0xfe6c0};
            for(size_t i=0;i<offsets.size();++i) mhw.entries[i]=base+offsets[i];
            struct CommandInvoke {
                unsigned calls=0;uintptr_t last=0;
                int result=-37;
                std::array<uintptr_t,128> sequence{};
                void *expected=nullptr,*buffer=nullptr;
                int parameters(uintptr_t entry,void *object,void *command,const void *params) {
                    assert(object==expected && command==buffer && params);
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
            TglOwnedSfcCommandBackend<Invoke,CommandInvoke> commands(mhw,owner,&command,commandInvoke);
            std::array<uint8_t,8> lock{};std::array<uint8_t,16> avs{};
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
            ++mhw.entries[5];assert(commands.frameStart()==5 && commandInvoke.calls==7);--mhw.entries[5];
            // Exercise the scheduler through the actual typed backend/owner,
            // not the independent scheduler mock. Still no native GPU call.
            commandInvoke.result=0;
            TglOwnedSfcCommandParameters prepared{};prepared.state=state;
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
                assert(!nativeMhw && !renderHal && !history && !performance && !cache);
                ++calls;*out=result;
            };
            auto construct=[&](size_t capacity) {
                return tglConstructOwnedVeboxBaseWithSfc(baseBinding,child.data(),capacity,
                    owner,reinterpret_cast<void*>(os),nullptr,nullptr,nullptr,nullptr,nullptr,
                    status,ctor);
            };
            assert(construct(child.size())==TglOwnedBaseConstruction::NotCalled && calls==0 && status==77);
            assert(owner.initialize(lifetime,mhw,os,read));
            assert(construct(child.size()-1)==TglOwnedBaseConstruction::NotCalled && calls==0);
            assert(construct(child.size())==TglOwnedBaseConstruction::ConstructedSuccess && calls==1);
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
