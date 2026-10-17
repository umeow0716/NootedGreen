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
#include <sys/statvfs.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <mach/mach_time.h>
#endif

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
struct Register {uint32_t type,first,last,reserved1,reserved2;};
struct Packet {uint32_t magic,kind,bytes,crc;uint64_t sequence;};
struct Clock {uint64_t wallNs,absoluteTicks,continuousTicks;uint32_t numer,denom;};
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
        if(bytes>eventCapacity*sizeof(Event) || total+bytes>uint64_t(64)*1024*1024*1024)return false;
        Packet h{0x444b474eU,kind,uint32_t(bytes),0,sequence};
        h.crc=~crc32(data,bytes,crc32(&h,sizeof(h)));
        if(!exactWrite(fd,&h,sizeof(h)) || (bytes && !exactWrite(fd,data,bytes)))return false;
        // Distinct H/P labels avoid padding in a separately encoded header
        // being confused with a continuous base64 payload.
        if(!exactWrite(STDOUT_FILENO,"NGKD H=",7) || !stream64(&h,sizeof(h)) ||
           !exactWrite(STDOUT_FILENO," P=",3) || !stream64(data,bytes) ||
           !exactWrite(STDOUT_FILENO,"\n",1))return false;
        ++sequence;total+=bytes;return true;
    }
};
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
int capture(const char* path,unsigned seconds) {
    if(geteuid()!=0 || !nativeVersion() || !seconds || seconds>600)return 2;
    BufferInfo before{};
    if(!info(before)){perror("kdebug read policy");return 3;}
    if(before.owner!=-1 || !before.disabled || !compatibleFilters(before.flags)){fprintf(stderr,"kdebug owned/enabled or conflicting filter; refusing takeover/reset\n");return 4;}
    int fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
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
    BufferInfo armed{};bool ok=info(armed) && armed.owner==getpid() && armed.events>0 &&
        size_t(armed.events)<=2*bufferEvents &&
        (armed.flags&lp64) && !(armed.flags&wrapped) && sink.packet(1,&armed,sizeof(armed));
    Clock anchor{};ok=ok && clockAnchor(anchor) && sink.packet(5,&anchor,sizeof(anchor));
    if(ok){fprintf(stderr,"NGREEN_KTRACE_LIVE buffer_bytes=%zu upstream_complete=0\n",size_t(armed.events)*sizeof(Event));fflush(stderr);}
    const auto began=std::chrono::steady_clock::now();auto syncAt=began;
    while(ok && !interrupted && std::chrono::steady_clock::now()-began<std::chrono::seconds(seconds)) {
        BufferInfo state{};
        if(!info(state) || state.owner!=getpid() || !(state.flags&lp64) || state.flags&wrapped){ok=false;break;}
        bytes=sizeof(events);
        if(control(readTrace,0,events.data(),&bytes)){ok=false;break;}
        size_t payload=0;
        if(!readableCount(bytes,events.size(),payload)){ok=false;break;}
        if(payload && !sink.packet(2,events.data(),payload)){ok=false;break;}
        if(lostEvents(events.data(),bytes)){ok=false;break;}
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
    // A stop/deadline and intact file are not proof of GPU completion.
    BufferInfo last{};if(!info(last) || last.owner!=getpid() || last.flags&wrapped)ok=false;
    if(!owner.close())ok=false;
    if(!sink.packet(ok?3U:4U,&last,sizeof(last)))ok=false;
    if(fsync(fd))ok=false;
    close(fd);return ok?0:8;
}
void selfTest() {
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
    if(argc==2 && !strcmp(argv[1],"--probe")) {
        if(geteuid()!=0 || !nativeVersion())return 2;
        BufferInfo b{};if(!info(b)){perror("kdebug read policy");return 3;}
        printf("KDEBUG_READONLY_PROBE owner=%d disabled=%d flags=%x event_bytes=%zu\n",b.owner,b.disabled,b.flags,sizeof(Event));
        return b.owner==-1 && b.disabled && compatibleFilters(b.flags)?0:4;
    }
    if(argc==4 && !strcmp(argv[1],"--capture")) {
        char* end=nullptr;errno=0;unsigned long seconds=strtoul(argv[3],&end,10);
        if(errno || !end || *end || seconds==0 || seconds>600)return 2;
        return capture(argv[2],unsigned(seconds));
    }
    fprintf(stderr,"usage: kdebug-recorder --probe | --self-test | --capture EXCLUSIVE_FILE SECONDS\n");return 2;
}
