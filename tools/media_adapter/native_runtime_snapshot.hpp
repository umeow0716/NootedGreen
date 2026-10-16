#pragma once
// Diagnostic only: the interrupted submitting thread waits only for the small
// critical producer/command/sideband/stack copy. A pre-created worker then streams
// the rest asynchronously using native Mach policy. No suspension
// of unrelated threads, mapping changes, text patches, or native-result edits.
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/i386/thread_status.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/ucontext.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <atomic>
#include <cstdlib>
#include <initializer_list>

class NativeRuntimeSnapshot {
    // All addresses stay in the private binary evidence, never unified logs.
    struct Packet { uint32_t kind, bytes; uint64_t address, detail; };
    struct Trigger { uint32_t magic, bytes; x86_thread_state64_t interrupted; };
    int request[2]{-1,-1}, response[2]{-1,-1}, output=-1;
    pthread_t worker{};
    bool started=false,joined=false;
    std::atomic<bool> used{false};
    static_assert(std::atomic<bool>::is_always_lock_free);
    char filename[1024]{};
    static constexpr uint64_t maximumBytes=uint64_t(64)*1024*1024*1024;
    static constexpr uint64_t maximumSeconds=60;
    uint64_t byteLimit=maximumBytes,secondLimit=maximumSeconds;
    static constexpr size_t chunkBytes=64*1024;
    static bool exactRead(int fd,void* raw,size_t bytes) noexcept {
        auto* p=static_cast<unsigned char*>(raw);
        while(bytes) {const ssize_t n=read(fd,p,bytes);
            if(n<0 && errno==EINTR)continue;
            if(n<=0)return false;p+=n;bytes-=size_t(n);}
        return true;
    }
    static bool exactWrite(int fd,const void* raw,size_t bytes) noexcept {
        const auto* p=static_cast<const unsigned char*>(raw);
        while(bytes) {const ssize_t n=write(fd,p,bytes);
            if(n<0 && errno==EINTR)continue;
            if(n<=0)return false;p+=n;bytes-=size_t(n);}
        return true;
    }
    bool packet(uint32_t kind,uint64_t address,uint64_t detail,
                const void* data,size_t bytes) noexcept {
        if(bytes>UINT32_MAX)return false;
        const Packet header{kind,uint32_t(bytes),address,detail};
        return exactWrite(output,&header,sizeof(header)) &&
            (!bytes || exactWrite(output,data,bytes));
    }
    static uint64_t seconds() noexcept {
        timespec now{};return clock_gettime(CLOCK_MONOTONIC,&now)?0:uint64_t(now.tv_sec);
    }
    bool timestamp(uint64_t phase) noexcept {
        timespec wall{},mono{};
        if(clock_gettime(CLOCK_REALTIME,&wall) || clock_gettime(CLOCK_MONOTONIC,&mono))return false;
        const uint64_t values[]{uint64_t(wall.tv_sec)*1000000000+uint64_t(wall.tv_nsec),
            uint64_t(mono.tv_sec)*1000000000+uint64_t(mono.tv_nsec),uint64_t(getpid()),phase};
        return packet(10,0,phase,values,sizeof(values));
    }
    bool critical(const Trigger& trigger) noexcept {
        // kind8 is pre-submit critical memory; kind9 describes that phase.
        // Mach reads fail safely on invalid mappings. No direct pointer loads.
        constexpr size_t limit=16*1024*1024;
        unsigned char wrapper[0x88]{};
        mach_vm_size_t actual=0;
        const auto base=trigger.interrupted.__rdi;
        if(base>UINT64_MAX-sizeof(wrapper) || mach_vm_read_overwrite(mach_task_self(),base,sizeof(wrapper),
            reinterpret_cast<mach_vm_address_t>(wrapper),&actual)!=KERN_SUCCESS || actual!=sizeof(wrapper))return false;
        uint64_t data=0,end=0,cursor=0,sideband=0,sideCursor=0;
        memcpy(&data,wrapper+0x10,8);memcpy(&end,wrapper+0x18,8);memcpy(&cursor,wrapper+0x20,8);
        memcpy(&sideband,wrapper+0x80,8);memcpy(&sideCursor,wrapper+0x60,8);
        if(!data || cursor<data || cursor>end || cursor-data>limit ||
           !sideband || sideCursor<sideband || sideCursor-sideband>limit ||
           (cursor-data)+(sideCursor-sideband)>limit)return false;
        if(!packet(8,base,0,wrapper,sizeof(wrapper)))return false;
        unsigned char buffer[chunkBytes];
        const uint64_t starts[]{data,sideband,trigger.interrupted.__rsp};
        // The caller chain is preserved as raw stack, not an unsafe stack walk.
        // Save up to 128KiB above interrupted RSP, with native read failures
        // explicitly reported; the full asynchronous pass captures its region.
        mach_vm_address_t stack=trigger.interrupted.__rsp;mach_vm_size_t stackBytes=0;
        natural_t depth=0;vm_region_submap_info_data_64_t info{};kern_return_t status=KERN_FAILURE;
        for(unsigned nesting=0;nesting<64;++nesting) {
            auto count=VM_REGION_SUBMAP_INFO_COUNT_64;
            status=mach_vm_region_recurse(mach_task_self(),&stack,&stackBytes,&depth,
                reinterpret_cast<vm_region_recurse_info_t>(&info),&count);
            if(status!=KERN_SUCCESS || !info.is_submap)break;
            ++depth;
        }
        const uint64_t rsp=trigger.interrupted.__rsp;
        if(status!=KERN_SUCCESS || stack>rsp || !stackBytes || stack>UINT64_MAX-stackBytes ||
           rsp>=stack+stackBytes || !(info.protection&VM_PROT_READ))return false;
        const uint64_t remaining=stack+stackBytes-rsp;
        const uint64_t lengths[]{cursor-data,sideCursor-sideband,remaining<128*1024?remaining:128*1024};
        uint64_t copied=sizeof(wrapper),failed=0;
        for(unsigned i=0;i<3;++i) {
            if(starts[i]>UINT64_MAX-lengths[i])return false;
            for(uint64_t at=0;at<lengths[i];) {
                const auto bytes=size_t(lengths[i]-at>chunkBytes?chunkBytes:lengths[i]-at);
                actual=0;
                const auto result=mach_vm_read_overwrite(mach_task_self(),starts[i]+at,bytes,
                    reinterpret_cast<mach_vm_address_t>(buffer),&actual);
                if(result!=KERN_SUCCESS || actual!=bytes) {
                    ++failed;
                    if(!packet(9,starts[i]+at,uint64_t(result==KERN_SUCCESS?KERN_FAILURE:result),nullptr,0))return false;
                    if(i<2)return false;
                    break;
                }
                if(!packet(8,starts[i]+at,i+1,buffer,bytes))return false;
                copied+=bytes;at+=bytes;
            }
        }
        const uint64_t phase[]{copied,failed,remaining>128*1024?1u:0u};
        return packet(9,0,0,phase,sizeof(phase));
    }
    bool threads() noexcept {
        thread_act_array_t list=nullptr;mach_msg_type_number_t count=0;
        const auto status=task_threads(mach_task_self(),&list,&count);
        if(status!=KERN_SUCCESS)return packet(7,0,uint64_t(status),nullptr,0);
        bool ok=true;
        for(mach_msg_type_number_t i=0;i<count;++i) {
            x86_thread_state64_t state{};auto n=x86_THREAD_STATE64_COUNT;
            const auto result=thread_get_state(list[i],x86_THREAD_STATE64,
                reinterpret_cast<thread_state_t>(&state),&n);
            if(ok)ok=packet(2,i,uint64_t(result),result==KERN_SUCCESS?&state:nullptr,
                result==KERN_SUCCESS?sizeof(state):0);
            mach_port_deallocate(mach_task_self(),list[i]);
        }
        vm_deallocate(mach_task_self(),reinterpret_cast<vm_address_t>(list),
            vm_size_t(count)*sizeof(thread_t));
        return ok;
    }
    bool snapshot(const Trigger& trigger) noexcept {
        // kind1 is the exact interrupted user state. kind2 thread samples are
        // NOT atomic with each other. kind3 is a leaf region, kind4 memory,
        // kind5 a native read failure, kind6 the mandatory terminal record.
        (void)trigger;
        if(!threads())return false;
        const uint64_t start=seconds();uint64_t copied=0,failures=0,regions=0;
        uint32_t terminal=0;mach_vm_address_t address=0;natural_t depth=0;
        unsigned char buffer[chunkBytes];
        for(;;) {
            if(copied>=byteLimit) {terminal=1;break;}
            const uint64_t now=seconds();
            if(!start || !now || now-start>=secondLimit) {terminal=2;break;}
            const auto requested=address;
            mach_vm_size_t extent=0;
            vm_region_submap_info_data_64_t info{};
            kern_return_t status=KERN_SUCCESS;
            for(unsigned nesting=0;nesting<64;++nesting) {
                auto count=VM_REGION_SUBMAP_INFO_COUNT_64;
                status=mach_vm_region_recurse(mach_task_self(),&address,&extent,&depth,
                    reinterpret_cast<vm_region_recurse_info_t>(&info),&count);
                if(status!=KERN_SUCCESS || !info.is_submap)break;
                ++depth;
                if(nesting==63)status=KERN_FAILURE;
            }
            if(status==KERN_INVALID_ADDRESS)break;
            if(status!=KERN_SUCCESS || !extent || address<requested || address>UINT64_MAX-extent) {terminal=3;break;}
            if(!packet(3,address,extent,&info,sizeof(info)))return false;
            ++regions;
            if(info.protection&VM_PROT_READ) {
                const uint64_t end=address+extent;
                for(uint64_t at=address;at<end;) {
                    const auto current=seconds();
                    if(!current || current-start>=secondLimit) {terminal=2;break;}
                    if(copied>=byteLimit) {terminal=1;break;}
                    uint64_t bytes=end-at;
                    if(bytes>chunkBytes)bytes=chunkBytes;
                    if(bytes>byteLimit-copied)bytes=byteLimit-copied;
                    mach_vm_size_t actual=0;
                    const auto result=mach_vm_read_overwrite(mach_task_self(),at,bytes,
                        reinterpret_cast<mach_vm_address_t>(buffer),&actual);
                    if(result==KERN_SUCCESS && actual==bytes) {
                        if(!packet(4,at,0,buffer,size_t(bytes)))return false;
                        copied+=bytes;
                    } else {
                        ++failures;if(!packet(5,at,uint64_t(result),&bytes,sizeof(bytes)))return false;
                    }
                    at+=bytes;
                }
            }
            if(terminal)break;
            address+=extent;
        }
        const uint64_t footer[]{terminal,copied,failures,regions,seconds()-start};
        return packet(6,0,0,footer,sizeof(footer));
    }
    static void* run(void* raw) noexcept {
        auto& self=*static_cast<NativeRuntimeSnapshot*>(raw);Trigger trigger{};
        if(exactRead(self.request[0],&trigger,sizeof(trigger)) && trigger.magic==0x4e475354 &&
           trigger.bytes==sizeof(trigger)) {
            const uint32_t format[]{0x4e47534e,1,sizeof(Packet),sizeof(x86_thread_state64_t)};
            const unsigned char result=exactWrite(self.output,format,sizeof(format)) &&
                self.packet(1,0,0,&trigger.interrupted,sizeof(trigger.interrupted)) &&
                self.timestamp(0) && self.critical(trigger) && !fsync(self.output) && self.timestamp(1)?1:0;
            // Release native submit BEFORE the slow full-process traversal.
            // A critical failure also releases it unchanged, without a retry.
            (void)exactWrite(self.response[1],&result,1);
            if(result) { (void)self.snapshot(trigger);(void)fsync(self.output); }
            close(self.output);self.output=-1;
        }
        return nullptr;
    }
public:
    explicit NativeRuntimeSnapshot(uint64_t limit=maximumBytes,uint64_t secondsLimit=maximumSeconds) noexcept {
        if(!limit || limit>maximumBytes || !secondsLimit || secondsLimit>maximumSeconds)return;
        byteLimit=limit;secondLimit=secondsLimit;
        char directory[900]{};
        const auto n=confstr(_CS_DARWIN_USER_TEMP_DIR,directory,sizeof(directory));
        if(!n || n>sizeof(directory))return;
        const int length=snprintf(filename,sizeof(filename),"%sngreen-runtime-XXXXXX",directory);
        if(length<0 || size_t(length)>=sizeof(filename)) {filename[0]=0;return;}
        output=mkstemp(filename);
        struct statfs space{};
        if(output<0 || fstatfs(output,&space) || !space.f_bsize ||
           uint64_t(space.f_bavail)>UINT64_MAX/uint64_t(space.f_bsize))return;
        const uint64_t available=uint64_t(space.f_bavail)*uint64_t(space.f_bsize);
        constexpr uint64_t reserve=uint64_t(2)*1024*1024*1024;
        if(available<=reserve+chunkBytes)return;
        if(byteLimit>available-reserve)byteLimit=available-reserve;
        if(fchmod(output,S_IRUSR|S_IWUSR) ||
           fcntl(output,F_SETFD,FD_CLOEXEC)<0 || pipe(request) || pipe(response))return;
        if(pthread_create(&worker,nullptr,&run,this))return;
        started=true;
    }
    ~NativeRuntimeSnapshot() noexcept {
        // EOF cancels an untriggered worker without a fabricated capture.
        if(request[1]>=0) {close(request[1]);request[1]=-1;}
        wait();
        for(int fd:{request[0],response[0],response[1],output})if(fd>=0)close(fd);
    }
    bool ready() const noexcept {return started;}
    void wait() noexcept {if(started && !joined) {pthread_join(worker,nullptr);joined=true;}}
    const char* path() const noexcept {return started?filename:"denied";}
    // Only call from an exact, owner-qualified breakpoint on this same thread.
    // read/write are async-signal-safe; all Mach walking is on the worker.
    bool trigger(const x86_thread_state64_t& interrupted) noexcept {
        if(!started || used.exchange(true,std::memory_order_acq_rel))return false;
        const int saved=errno;const Trigger value{0x4e475354,sizeof(Trigger),interrupted};
        unsigned char result=0;
        const bool ok=exactWrite(request[1],&value,sizeof(value)) &&
            exactRead(response[0],&result,1) && result==1;
        errno=saved;return ok;
    }
    NativeRuntimeSnapshot(const NativeRuntimeSnapshot&)=delete;
    NativeRuntimeSnapshot& operator=(const NativeRuntimeSnapshot&)=delete;
};
