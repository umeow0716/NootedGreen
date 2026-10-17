// Native root-only kdebug transport, not a GPU executor or policy bypass.
// ABI pinned to XNU 12377.121.6 kdebug_private.h/kdebug.c. In particular,
// KDREADTR receives a byte capacity but RETURNS AN EVENT COUNT, not bytes.
#include <array>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../../NootedGreen/kern_flight_trace.hpp"
#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <mach/mach_time.h>
#include <dlfcn.h>
#endif
extern char** environ;

namespace {
constexpr uint32_t traceFirst=0x06fe0000U,traceLast=0x06ff0000U; // exclusive end
constexpr int getBuffer=5,readTrace=10,enable=3,setBuffer=4,setup=6,removeTrace=7,setRegister=8;
constexpr uint32_t wrapped=8,lp64=0x100;
bool compatibleFilters(uint32_t flags) {
    // KDSETUP reallocates buffers but does NOT clear an old PID/type filter
    // or stop matcher. Refuse those rather than silently producing no data
    // or resetting somebody else's tracing configuration.
    return !(flags & (0x2U|0x10U|0x40U|0x800U|0x400000U));
}
struct Event {uint64_t timestamp,args[5];uint32_t debugid,cpu;uint64_t unused;};
struct BufferInfo {int events,disabled;uint32_t flags;int threads,owner;};
bool qualifiedLive(const BufferInfo& state,int owner,size_t maximumEvents) {
    return state.owner==owner && !state.disabled && state.events>0 &&
        size_t(state.events)<=maximumEvents && (state.flags&lp64) &&
        !(state.flags&wrapped) && compatibleFilters(state.flags);
}
struct Register {uint32_t type,first,last,reserved1,reserved2;};
struct Packet {uint32_t magic,kind,bytes,crc;uint64_t sequence;};
struct Clock {uint64_t wallNs,absoluteTicks,continuousTicks;uint32_t numer,denom;};
struct Platform {char build[32],bootUUID[64];};
constexpr uint32_t calibrationId=0x06fefff0U;
constexpr uint64_t calibrationMagic=UINT64_C(0x4e474b4443414c31);
static_assert(sizeof(Event)==64 && sizeof(BufferInfo)==20 && sizeof(Register)==20 && sizeof(Packet)==24,"native trace ABI");
static_assert(sizeof(Clock)==32,"clock anchor ABI");
constexpr size_t eventCapacity=1024,bufferEvents=262144; // 64KiB reads, 16MiB native storage
volatile sig_atomic_t interrupted=0;
void stop(int) {interrupted=1;}
int control(int op,int value,void* data,size_t* bytes) {
#if defined(__APPLE__)
    int mib[]{1,24,op,value};
    return sysctl(mib,4,data,bytes,nullptr,0);
#else
    (void)op;(void)value;(void)data;(void)bytes;errno=ENOTSUP;return -1;
#endif
}
bool info(BufferInfo& out) {
    size_t bytes=sizeof(out);
    return !control(getBuffer,0,&out,&bytes) && bytes==sizeof(out);
}
bool readableCount(size_t returned,size_t capacity,size_t& bytes) {
    if(returned>capacity || capacity>eventCapacity)return false;
    bytes=returned*sizeof(Event);return true;
}
bool lostEvents(const Event* events,size_t count) {
    for(size_t i=0;i<count;++i)if((events[i].debugid&~3U)==0x07020008U)return true;
    return false; // XNU TRACE_LOST_EVENTS; flags can be cleared by KDREADTR.
}
bool clockAnchor(Clock& value) {
#if defined(__APPLE__)
    mach_timebase_info_data_t base{};
    if(mach_timebase_info(&base) || !base.denom)return false;
    value.wallNs=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    value.absoluteTicks=mach_absolute_time();value.continuousTicks=mach_continuous_time();
    value.numer=base.numer;value.denom=base.denom;return true;
#else
    (void)value;return false;
#endif
}
uint32_t crc32(const void* raw,size_t bytes,uint32_t crc=0xffffffffU) {
    auto* p=static_cast<const uint8_t*>(raw);
    for(size_t i=0;i<bytes;++i){crc^=p[i];for(unsigned b=0;b<8;++b)crc=(crc>>1)^(0xedb88320U & (0U-(crc&1U)));}
    return crc;
}
bool exactWrite(int fd,const void* raw,size_t bytes) {
    auto* p=static_cast<const uint8_t*>(raw);
    while(bytes){ssize_t n=write(fd,p,bytes);if(n<0 && errno==EINTR)continue;
        if(n<=0)return false;p+=n;bytes-=size_t(n);}
    return true;
}
// Base64 text avoids PTY output translation corrupting binary native events.
// Host archives this channel verbatim; local file retains the binary packets.
bool stream64(const void* raw,size_t bytes) {
    constexpr char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto* p=static_cast<const uint8_t*>(raw);
    std::array<char,4096> output{};size_t used=0;
    for(size_t i=0;i<bytes;i+=3){uint32_t v=uint32_t(p[i])<<16;
        if(i+1<bytes)v|=uint32_t(p[i+1])<<8;if(i+2<bytes)v|=p[i+2];
        output[used++]=alphabet[(v>>18)&63];output[used++]=alphabet[(v>>12)&63];
        output[used++]=i+1<bytes?alphabet[(v>>6)&63]:'=';output[used++]=i+2<bytes?alphabet[v&63]:'=';
        if(used==output.size()){if(!exactWrite(STDOUT_FILENO,output.data(),used))return false;used=0;}}
    return !used || exactWrite(STDOUT_FILENO,output.data(),used);
}
struct Sink {
    int fd;uint64_t sequence=0,total=0;
    bool packet(uint32_t kind,const void* data,size_t bytes) {
        if(bytes>eventCapacity*sizeof(Event) || total+sizeof(Packet)+bytes>uint64_t(64)*1024*1024*1024)return false;
        Packet h{0x444b474eU,kind,uint32_t(bytes),0,sequence};
        h.crc=~crc32(data,bytes,crc32(&h,sizeof(h)));
        if(!exactWrite(fd,&h,sizeof(h)) || (bytes && !exactWrite(fd,data,bytes)))return false;
        // Distinct H/P labels avoid padding in a separately encoded header
        // being confused with a continuous base64 payload.
        if(!exactWrite(STDOUT_FILENO,"NGKD H=",7) || !stream64(&h,sizeof(h)) ||
           !exactWrite(STDOUT_FILENO," P=",3) || !stream64(data,bytes) ||
           !exactWrite(STDOUT_FILENO,"\n",1))return false;
        ++sequence;total+=sizeof(Packet)+bytes;return true;
    }
};
// Only the parent writes Sink/PTY. Child output travels through a bounded
// nonblocking pipe, so log stream cannot interleave a native packet's base64.
struct UnifiedLog {
    int fd=-1;pid_t pid=-1;bool ended=false,ready=false;
    std::array<char,256> greeting{};size_t greetingBytes=0;
    bool start(const char* executable,char* const argv[]) {
        int pipefd[2];if(pipe(pipefd))return false;
        posix_spawn_file_actions_t actions;
        if(posix_spawn_file_actions_init(&actions)){close(pipefd[0]);close(pipefd[1]);return false;}
        bool ok=!posix_spawn_file_actions_addopen(&actions,STDIN_FILENO,"/dev/null",O_RDONLY,0) &&
            !posix_spawn_file_actions_adddup2(&actions,pipefd[1],STDOUT_FILENO) &&
            !posix_spawn_file_actions_adddup2(&actions,pipefd[1],STDERR_FILENO) &&
            !posix_spawn_file_actions_addclose(&actions,pipefd[0]) &&
            !posix_spawn_file_actions_addclose(&actions,pipefd[1]);
        if(ok)ok=!posix_spawn(&pid,executable,&actions,nullptr,argv,environ);
        posix_spawn_file_actions_destroy(&actions);close(pipefd[1]);
        if(!ok){close(pipefd[0]);pid=-1;return false;}
        fd=pipefd[0];int flags=fcntl(fd,F_GETFL);
        return flags>=0 && !fcntl(fd,F_SETFL,flags|O_NONBLOCK);
    }
    bool drain(Sink& sink) {
        std::array<uint8_t,65536> chunk{};
        // Bound per-iteration work to preserve native trace drain fairness.
        ssize_t n=read(fd,chunk.data(),chunk.size());
        if(n>0){
            if(!ready){
                for(ssize_t i=0;i<n;++i){
                    if(greetingBytes==greeting.size()-1){
                        memmove(greeting.data(),greeting.data()+1,--greetingBytes);
                    }
                    greeting[greetingBytes++]=char(chunk[size_t(i)]);greeting[greetingBytes]=0;
                    if(strstr(greeting.data(),"Filtering the log data using"))ready=true;
                }
            }
            return sink.packet(9,chunk.data(),size_t(n));
        }
        if(n==0){ended=true;return false;}
        return errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR;
    }
    bool stop(Sink* sink=nullptr) {
        bool ok=true;
        if(pid>0){
            (void)kill(pid,SIGTERM);
            bool reaped=false;
            for(unsigned i=0;i<100;++i){
                if(sink && fd>=0 && !ended && !drain(*sink) && !ended)ok=false;
                pid_t result=waitpid(pid,nullptr,WNOHANG);
                if(result==pid || (result<0 && errno==ECHILD)){reaped=true;break;}
                usleep(10000);
            }
            if(!reaped){ok=false;(void)kill(pid,SIGKILL);while(waitpid(pid,nullptr,0)<0 && errno==EINTR){}}
            pid=-1;
        }
        // Preserve remaining pipe bytes, but do not wait indefinitely if an
        // unexpected descendant retained the writer. Missing EOF is failure.
        if(sink && fd>=0){
            for(unsigned i=0;i<32 && !ended;++i){
                if(!drain(*sink) && !ended){ok=false;break;}
                if(!ended)usleep(1000);
            }
            ok=ok && ended;
        }
        if(fd>=0){close(fd);fd=-1;}
        return ok;
    }
    ~UnifiedLog(){stop();}
};
bool calibrate(Sink& sink,std::array<Event,eventCapacity>& events) {
#if defined(__APPLE__)
    using Emit=int(*)(uint32_t,uint64_t,uint64_t,uint64_t,uint64_t);
    auto emit=reinterpret_cast<Emit>(dlsym(RTLD_DEFAULT,"kdebug_trace"));
    const uint64_t nonce=mach_absolute_time();
    if(!emit || emit(calibrationId,calibrationMagic,nonce,sizeof(Event),uint64_t(getpid())))return false;
    for(unsigned attempt=0;attempt<100;++attempt) {
        size_t count=sizeof(events),payload=0;
        if(control(readTrace,0,events.data(),&count) || !readableCount(count,events.size(),payload) ||
           lostEvents(events.data(),count))return false;
        const Event* match=nullptr;
        for(size_t i=0;i<count;++i) {
            const auto& e=events[i];
            // Reject a byte-count/event-count ABI mismatch before LIVE: the
            // initially zeroed unread tail must not be accepted as events.
            if(!e.timestamp || !e.debugid)return false;
            if(e.debugid==calibrationId) {
                if(match || e.args[0]!=calibrationMagic || e.args[1]!=nonce ||
                   e.args[2]!=sizeof(Event) || e.args[3]!=uint64_t(getpid()) || !e.args[4])return false;
                match=&e;
            }
        }
        if(match && !sink.packet(8,match,sizeof(*match)))return false;
        if(payload && !sink.packet(2,events.data(),payload))return false;
        if(match)return true;
        usleep(10000);
    }
    return false;
#else
    (void)sink;(void)events;return false;
#endif
}
std::array<Event,260> fixtureEvents{};size_t fixtureCount=0;
void fixtureTrace(uint32_t id,uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d,uintptr_t ignored) {
    assert(!ignored && fixtureCount<fixtureEvents.size());
    fixtureEvents[fixtureCount]={uint64_t(100+fixtureCount),{a,b,c,d,77},id,0,0};++fixtureCount;
}
int ownedFixture(const char* path) {
    int fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);if(fd<0)return 5;
    Sink sink{fd};BufferInfo start{int(bufferEvents),0,lp64,1,123};Clock clock{1000,100,100,1,1};
    Platform source{};strcpy(source.build,"OWNED-CPU-FIXTURE");strcpy(source.bootUUID,"00000000-0000-0000-0000-000000000000");
    const char origin[]="OWNED_CPU_FIXTURE";
    Event beacon{99,{calibrationMagic,55,sizeof(Event),123,88},calibrationId,0,0};
    const uint32_t words[]={0xffffffff,0x12345678,3,4,5,6,7};fixtureCount=0;
    bool ok=NGFlightTrace::emitCpuWords(&fixtureTrace,[](){return 1U;},3,42,0x40010000,words,7) &&
        sink.packet(1,&start,sizeof(start)) && sink.packet(5,&clock,sizeof(clock)) &&
        sink.packet(6,origin,sizeof(origin)-1) && sink.packet(7,&source,sizeof(source)) &&
        sink.packet(8,&beacon,sizeof(beacon)) && sink.packet(2,&beacon,sizeof(beacon)) &&
        sink.packet(2,fixtureEvents.data(),fixtureCount*sizeof(Event));
    start.disabled=1;ok=ok && sink.packet(3,&start,sizeof(start)) && !fsync(fd);
    close(fd);fprintf(stderr,"OWNED_CPU_FIXTURE_ONLY no-kernel-trace-configured\n");return ok?0:8;
}
struct Owner {
    bool acquired=false;
    int (*call)(int,int,void*,size_t*)=&control;
    bool (*readInfo)(BufferInfo&)=&info;
    bool stillOurs() const {BufferInfo b{};return acquired && readInfo(b) && b.owner==getpid();}
    bool disable() {
        if(!acquired)return true;
        if(!stillOurs())return false; // Never disable/reset someone else's session.
        size_t bytes=0;
        return !call(enable,0,nullptr,&bytes);
    }
    bool close() {
        if(!acquired)return true;
        if(!disable())return false;
        size_t bytes=0;
        if(call(removeTrace,0,nullptr,&bytes))return false;
        acquired=false;return true;
    }
    ~Owner(){if(acquired)(void)close();}
};
// Fake backend is used only by --self-test; capture always constructs Owner
// with native function defaults. No environment/CLI backend substitution.
int testOwner=0;unsigned testCalls=0;
bool testInfo(BufferInfo& b){b.owner=testOwner;return true;}
int testControl(int op,int value,void*,size_t*) {
    assert(value==0 && (op==enable || op==removeTrace));++testCalls;return 0;
}
bool nativeVersion() {
#if defined(__APPLE__)
    char build[32]{};size_t bytes=sizeof(build);
    return !sysctlbyname("kern.osversion",build,&bytes,nullptr,0) && bytes>0 && bytes<=sizeof(build) &&
        build[bytes-1]==0 && !strcmp(build,"25G229") && sizeof(uintptr_t)==8;
#else
    return false;
#endif
}
bool platform(Platform& out) {
#if defined(__APPLE__)
    size_t build=sizeof(out.build),uuid=sizeof(out.bootUUID);
    return !sysctlbyname("kern.osversion",out.build,&build,nullptr,0) &&
        !sysctlbyname("kern.bootsessionuuid",out.bootUUID,&uuid,nullptr,0) &&
        build>0 && build<=sizeof(out.build) && out.build[build-1]==0 &&
        uuid==37 && out.bootUUID[36]==0 && !strcmp(out.build,"25G229");
#else
    (void)out;return false;
#endif
}
int capture(const char* path,unsigned seconds,bool unified=false,bool readyFile=false) {
    if(geteuid()!=0 || !nativeVersion() || !seconds || seconds>600)return 2;
    if(readyFile){
        struct stat status{};
        if(fstat(3,&status) || !S_ISREG(status.st_mode) || status.st_uid ||
           status.st_nlink!=1 || (status.st_mode&0777)!=0600)return 2;
    }
    Platform source{};if(!platform(source))return 2;
    BufferInfo before{};
    if(!info(before)){perror("kdebug read policy");return 3;}
    if(before.owner!=-1 || !before.disabled || !compatibleFilters(before.flags)){fprintf(stderr,"kdebug owned/enabled or conflicting filter; refusing takeover/reset\n");return 4;}
    int fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0){perror("exclusive archive");return 5;}
    struct statvfs initialSpace{};
    if(fstatvfs(fd,&initialSpace) || uint64_t(initialSpace.f_bavail)*initialSpace.f_frsize<(uint64_t(1)<<30)){close(fd);return 5;}
    Sink sink{fd};Owner owner;size_t zero=0;
    // The first configuration call atomically asks native ktrace for ownership.
    // A competing foreground owner produces EBUSY; do not remove/reset first.
    if(control(setBuffer,int(bufferEvents),nullptr,&zero)){close(fd);perror("ktrace acquire");return 6;}
    owner.acquired=true;
    Register range{0x40000,traceFirst,traceLast,0,0};size_t bytes=sizeof(range);
    bool configured=owner.stillOurs() && !control(setup,0,nullptr,&zero) &&
        !control(setRegister,0,&range,&bytes) && !control(enable,1,nullptr,&zero);
    if(!configured){(void)owner.close();close(fd);perror("ktrace configure");return 7;}
    signal(SIGINT,stop);signal(SIGTERM,stop);signal(SIGPIPE,SIG_IGN);
    std::array<Event,eventCapacity> events{};
    BufferInfo armed{};bool ok=info(armed) && qualifiedLive(armed,getpid(),2*bufferEvents) &&
        sink.packet(1,&armed,sizeof(armed));
    Clock anchor{};ok=ok && clockAnchor(anchor) && sink.packet(5,&anchor,sizeof(anchor));
    const char origin[]="NATIVE_ROOT_TRACE";
    ok=ok && sink.packet(6,origin,sizeof(origin)-1);
    ok=ok && sink.packet(7,&source,sizeof(source));
    ok=ok && calibrate(sink,events);
    UnifiedLog log;
    if(ok && unified){
        char executable[]="/usr/bin/log",stream[]="stream",style[]="--style",compact[]="compact";
        char level[]="--level",debug[]="debug",predicate[]="--predicate";
        char filter[]="(process == \"kernel\" AND (eventMessage CONTAINS[c] \"NootedGreen\" OR eventMessage CONTAINS[c] \"ngreen\" OR eventMessage CONTAINS[c] \"Lilu\" OR eventMessage CONTAINS[c] \"AppleIntelTGLGraphics\" OR eventMessage CONTAINS[c] \"CODE SIGNING\" OR eventMessage CONTAINS[c] \"vm_map_protect\")) OR process == \"VTEncoderXPCService\" OR process == \"VTDecoderXPCService\"";
        char* args[]={executable,stream,style,compact,level,debug,predicate,filter,nullptr};
        ok=log.start(executable,args);
        for(unsigned attempt=0;ok && !log.ready && attempt<100;++attempt){
            ok=log.drain(sink);if(!log.ready)usleep(10000);
        }
        ok=ok && log.ready;
    }
    // Do not admit a workload until provenance and calibration are durable.
    ok=ok && !fsync(fd);
    if(ok && readyFile)ok=dprintf(3,"boot_uuid=%s\npid=%ld\ncalibrated=1\n",source.bootUUID,long(getpid()))>0 && !fsync(3);
    if(ok){fprintf(stderr,"NGREEN_KTRACE_LIVE buffer_bytes=%zu upstream_complete=0\n",size_t(armed.events)*sizeof(Event));fflush(stderr);}
    if(ok && unified){fprintf(stderr,"NGREEN_MEDIA_ROOT_LOGGER_LIVE\n");fflush(stderr);}
    const auto began=std::chrono::steady_clock::now();auto syncAt=began;
    while(ok && !interrupted && std::chrono::steady_clock::now()-began<std::chrono::seconds(seconds)) {
        BufferInfo state{};
        if(!info(state) || !qualifiedLive(state,getpid(),2*bufferEvents)){ok=false;break;}
        bytes=sizeof(events);
        if(control(readTrace,0,events.data(),&bytes)){ok=false;break;}
        size_t payload=0;
        if(!readableCount(bytes,events.size(),payload)){ok=false;break;}
        if(payload && !sink.packet(2,events.data(),payload)){ok=false;break;}
        if(lostEvents(events.data(),bytes)){ok=false;break;}
        if(unified && !log.drain(sink)){ok=false;break;}
        auto now=std::chrono::steady_clock::now();
        if(now-syncAt>=std::chrono::seconds(1)) {
            struct statvfs space{};
            if(fstatvfs(fd,&space) || uint64_t(space.f_bavail)*space.f_frsize<(uint64_t(1)<<30) || fsync(fd)){ok=false;break;}
            syncAt=now;
        }
        if(!payload)usleep(10000);
    }
    // Disable before the final drain: otherwise a successful terminal could
    // silently omit events still queued when the native trace is removed.
    if(!owner.disable())ok=false;
    bool drained=false;
    for(size_t chunk=0;ok && chunk<=2*bufferEvents/eventCapacity;++chunk) {
        bytes=sizeof(events);
        if(control(readTrace,0,events.data(),&bytes)){ok=false;break;}
        size_t payload=0;
        if(!readableCount(bytes,events.size(),payload)){ok=false;break;}
        if(!payload){drained=true;break;}
        if(!sink.packet(2,events.data(),payload)){ok=false;break;}
        if(lostEvents(events.data(),bytes)){ok=false;break;}
    }
    ok=ok && drained;
    // A bounded shutdown may omit logd's pending messages: never label the
    // unified sideband complete, even when native kdebug drains successfully.
    if(unified && !log.stop(&sink))ok=false;
    // A stop/deadline and intact file are not proof of GPU completion.
    BufferInfo last{};if(!info(last) || last.owner!=getpid() || last.flags&wrapped)ok=false;
    if(!owner.close())ok=false;
    if(!sink.packet(ok?3U:4U,&last,sizeof(last)))ok=false;
    if(readyFile){(void)dprintf(3,"terminal=%s\n",ok?"complete":"failed");if(fsync(3))ok=false;}
    if(fsync(fd))ok=false;
    close(fd);return ok?0:8;
}
void selfTest() {
    {
        UnifiedLog child;
        char executable[]="/usr/bin/printf",message[]="OWNED_PIPE_FIXTURE";
        char* args[]={executable,message,nullptr};
        assert(child.start(executable,args));
        char captured[64]{};ssize_t count=-1;
        for(unsigned attempt=0;attempt<100;++attempt){
            count=read(child.fd,captured,sizeof(captured));
            if(count>=0)break;
            assert(errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR);usleep(10000);
        }
        assert(count==ssize_t(strlen(message)) && !memcmp(captured,message,size_t(count)));
        int discarded=open("/dev/null",O_WRONLY);assert(discarded>=0);
        Sink sink{discarded};assert(child.stop(&sink));close(discarded);
        assert(child.ended && child.fd==-1 && child.pid==-1);
    }
    BufferInfo live{int(bufferEvents),0,lp64,1,123};
    assert(qualifiedLive(live,123,2*bufferEvents));
    assert(!qualifiedLive(live,124,2*bufferEvents));
    for(uint32_t flag:{wrapped,0x2U,0x10U,0x40U,0x800U,0x400000U}) {
        auto bad=live;bad.flags|=flag;assert(!qualifiedLive(bad,123,2*bufferEvents));
    }
    {auto bad=live;bad.disabled=1;assert(!qualifiedLive(bad,123,2*bufferEvents));}
    {auto bad=live;bad.events=0;assert(!qualifiedLive(bad,123,2*bufferEvents));}
    assert(!qualifiedLive(live,123,bufferEvents-1));
    assert(compatibleFilters(lp64));
    for(uint32_t flag:{0x2U,0x10U,0x40U,0x800U,0x400000U})assert(!compatibleFilters(lp64|flag));
    {Owner o;o.call=&testControl;o.readInfo=&testInfo;assert(o.close() && testCalls==0);}
    {Owner o;o.acquired=true;o.call=&testControl;o.readInfo=&testInfo;
        testOwner=int(getpid())+1;assert(!o.close() && testCalls==0);}
    {Owner o;o.acquired=true;o.call=&testControl;o.readInfo=&testInfo;
        testOwner=int(getpid());assert(o.close() && testCalls==2 && !o.acquired);}
    size_t bytes=123;
    assert(readableCount(0,eventCapacity,bytes) && bytes==0);
    assert(readableCount(1,eventCapacity,bytes) && bytes==64);
    assert(readableCount(eventCapacity,eventCapacity,bytes) && bytes==65536);
    assert(!readableCount(eventCapacity+1,eventCapacity,bytes));
    assert(!readableCount(1,eventCapacity+1,bytes));
    Event loss{};loss.debugid=0x07020008U;assert(lostEvents(&loss,1));
    loss.debugid|=3;assert(lostEvents(&loss,1));
    loss.debugid=traceFirst;assert(!lostEvents(&loss,1));
    assert(~crc32("123456789",9)==0xcbf43926U);
    puts("NATIVE_KDEBUG_ABI_COUNT_SELFTEST_OK no-kernel-trace-configured");
}
}
int main(int argc,char** argv) {
    if(argc==2 && !strcmp(argv[1],"--self-test")){selfTest();return 0;}
    if(argc==3 && !strcmp(argv[1],"--owned-fixture"))return ownedFixture(argv[2]);
    if(argc==2 && !strcmp(argv[1],"--probe")) {
        if(geteuid()!=0 || !nativeVersion())return 2;
        BufferInfo b{};if(!info(b)){perror("kdebug read policy");return 3;}
        printf("KDEBUG_READONLY_PROBE owner=%d disabled=%d flags=%x event_bytes=%zu\n",b.owner,b.disabled,b.flags,sizeof(Event));
        return b.owner==-1 && b.disabled && compatibleFilters(b.flags)?0:4;
    }
    if((argc==4 || ((argc==5 || (argc==6 && !strcmp(argv[5],"--ready-fd3"))) &&
        !strcmp(argv[4],"--unified-log"))) && !strcmp(argv[1],"--capture")) {
        char* end=nullptr;errno=0;unsigned long seconds=strtoul(argv[3],&end,10);
        if(errno || !end || *end || seconds==0 || seconds>600)return 2;
        return capture(argv[2],unsigned(seconds),argc>=5,argc==6);
    }
    fprintf(stderr,"usage: kdebug-recorder --probe | --self-test | --capture EXCLUSIVE_FILE SECONDS [--unified-log]\n");return 2;
}
