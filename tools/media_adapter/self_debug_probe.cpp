// Owned-code-only native-policy test. No GPU, Apple image, task attachment,
// entitlement change, text write, or instruction-pointer modification.
#include <mach/mach.h>
#include <mach/i386/thread_status.h>
#include <signal.h>
#include <sys/ucontext.h>
#include <unistd.h>
#include <cstdio>
#include <cstdint>
#include "native_return_observer.hpp"

extern "C" int owned_debug_target();
extern "C" const char owned_debug_site[];
extern "C" int owned_debug_target_b();
extern "C" int owned_debug_target_c();
extern "C" int owned_debug_target_d();
extern "C" const char owned_debug_site_b[], owned_debug_site_c[], owned_debug_site_d[];
extern "C" uintptr_t owned_debug_data_target();
extern "C" const char owned_debug_data_site[];
extern "C" const char owned_debug_data[];
extern "C" uint32_t owned_debug_copy_target(void *, size_t, const void *, size_t);
extern "C" const char owned_debug_copy_site[];
extern "C" __attribute__((noinline)) uint32_t owned_debug_copy_impl(
    void *destination, size_t capacity, const void *source, size_t bytes) {
    if (capacity < bytes || bytes != sizeof(uint32_t)) return 0;
    std::memcpy(destination, source, bytes);
    uint32_t result;
    std::memcpy(&result, destination, sizeof(result));
    return result;
}
asm(".text\n.p2align 4\n.globl _owned_debug_copy_target\n"
    "_owned_debug_copy_target:\npushq %rbp\nmovq %rsp, %rbp\n"
    ".globl _owned_debug_copy_site\n_owned_debug_copy_site:\ncallq _owned_debug_copy_impl\npopq %rbp\nretq\n");
asm(".text\n.p2align 4\n.globl _owned_debug_data_target\n"
    "_owned_debug_data_target:\nleaq _owned_debug_data(%rip), %rax\nmovq %rax, %rdx\nmovl $0x2468, %ecx\n"
    ".globl _owned_debug_data_site\n_owned_debug_data_site:\ncmpl $0x10000, (%rax)\nretq\n"
    ".p2align 2\n.globl _owned_debug_data\n_owned_debug_data:\n.long 0x12345678\n");
asm(".text\n.p2align 4\n.globl _owned_debug_target\n"
    "_owned_debug_target:\nmovl $0x1357, %eax\n"
    ".globl _owned_debug_site\n_owned_debug_site:\nretq\n");
asm(".text\n.p2align 4\n.globl _owned_debug_target_b\n"
    "_owned_debug_target_b:\nmovl $0x2468, %eax\n"
    ".globl _owned_debug_site_b\n_owned_debug_site_b:\nretq\n"
    ".p2align 4\n.globl _owned_debug_target_c\n"
    "_owned_debug_target_c:\nmovl $0x369c, %eax\n"
    ".globl _owned_debug_site_c\n_owned_debug_site_c:\nretq\n"
    ".p2align 4\n.globl _owned_debug_target_d\n"
    "_owned_debug_target_d:\nmovl $0x48ad, %eax\n"
    ".globl _owned_debug_site_d\n_owned_debug_site_d:\nretq\n");

static volatile sig_atomic_t hits = 0;
static volatile sig_atomic_t captured = 0;
static thread_t observedThread = MACH_PORT_NULL;
static x86_debug_state64_t savedDebug{};
static volatile sig_atomic_t handlerRestore = KERN_FAILURE;
// XNU dr7d_is_valid enforces Intel's fixed DR7 bits on every set. A thread
// without allocated debug state initially reads all zeros, not canonical 0x400.
// Compare the complete control word after that exact normalization, not a mask
// which could hide enabled breakpoints or changed access/length controls.
static constexpr uint64_t canonicalDr7(uint64_t value) {
    return (value | uint64_t(0x400)) & ~uint64_t(0xd800);
}
static_assert(canonicalDr7(0) == 0x400);
static_assert(canonicalDr7(0x400) == 0x400);
static void hex(uint64_t value) {
    char out[17];
    constexpr char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i != 16; ++i) out[i] = digits[(value >> (60 - 4 * i)) & 15];
    out[16] = '\n';
    (void)write(STDERR_FILENO, out, sizeof(out));
}
static void trap(int signal, siginfo_t *, void *context) {
    auto *state = static_cast<ucontext_t *>(context);
    if (signal != SIGTRAP || !state || !state->uc_mcontext || hits ||
        state->uc_mcontext->__ss.__rip != reinterpret_cast<uintptr_t>(owned_debug_site) ||
        state->uc_mcontext->__ss.__rax != 0x1357) {
        constexpr char message[] = "SELF_DEBUG_TRAP_REJECT signal/hits/rip/expected/rax:\n";
        (void)write(STDERR_FILENO, message, sizeof(message) - 1);
        hex(static_cast<uint64_t>(signal));
        hex(static_cast<uint64_t>(hits));
        hex(state && state->uc_mcontext ? state->uc_mcontext->__ss.__rip : 0);
        hex(reinterpret_cast<uintptr_t>(owned_debug_site));
        hex(state && state->uc_mcontext ? state->uc_mcontext->__ss.__rax : 0);
        _exit(70);
    }
    captured = static_cast<sig_atomic_t>(state->uc_mcontext->__ss.__rax);
    hits = 1;
    // Darwin signal return did not retain RF in the owned-code CI experiment.
    // Disarm before resuming instead. The trap site is our RET, outside any MIG
    // client call, so this same-thread debug-state RPC cannot interrupt itself.
    // Never change RIP, flags, result registers, or any instruction bytes.
    handlerRestore = thread_set_state(observedThread, x86_DEBUG_STATE64,
        reinterpret_cast<thread_state_t>(&savedDebug), x86_DEBUG_STATE64_COUNT);
    if (handlerRestore != KERN_SUCCESS) {
        constexpr char message[] = "SELF_DEBUG_HANDLER_RESTORE_FAILED\n";
        (void)write(STDERR_FILENO, message, sizeof(message) - 1);
        hex(static_cast<uint64_t>(handlerRestore));
        _exit(74);
    }
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    thread_t thread = mach_thread_self();
    x86_debug_state64_t original{};
    mach_msg_type_number_t count = x86_DEBUG_STATE64_COUNT;
    kern_return_t got = thread_get_state(thread, x86_DEBUG_STATE64,
        reinterpret_cast<thread_state_t>(&original), &count);
    std::printf("SELF_DEBUG_GET result=%d count=%u\n", got, count);
    if (got != KERN_SUCCESS || count != x86_DEBUG_STATE64_COUNT ||
        (original.__dr7 & 0xff)) {
        mach_port_deallocate(mach_task_self(), thread);
        return 1;
    }
    observedThread = thread;
    savedDebug = original;
    struct sigaction previous{}, action{};
    if (sigaction(SIGTRAP, nullptr, &previous) || previous.sa_handler != SIG_DFL) {
        mach_port_deallocate(mach_task_self(), thread);
        return 1;
    }
    action.sa_sigaction = trap;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGTRAP, &action, nullptr)) {
        mach_port_deallocate(mach_task_self(), thread);
        return 1;
    }
    auto armed = original;
    armed.__dr0 = reinterpret_cast<uintptr_t>(owned_debug_site);
    armed.__dr7 = (original.__dr7 & ~uint64_t(0xf0003)) | 1;
    alarm(3); // A broken exception/resume path must terminate this probe only.
    kern_return_t set = thread_set_state(thread, x86_DEBUG_STATE64,
        reinterpret_cast<thread_state_t>(&armed), x86_DEBUG_STATE64_COUNT);
    int result = set == KERN_SUCCESS ? owned_debug_target() : -1;
    kern_return_t restored = thread_set_state(thread, x86_DEBUG_STATE64,
        reinterpret_cast<thread_state_t>(&original), x86_DEBUG_STATE64_COUNT);
    alarm(0);
    int handlerRestored = sigaction(SIGTRAP, &previous, nullptr);
    x86_debug_state64_t finalState{};
    count = x86_DEBUG_STATE64_COUNT;
    kern_return_t verified = thread_get_state(thread, x86_DEBUG_STATE64,
        reinterpret_cast<thread_state_t>(&finalState), &count);
    mach_port_deallocate(mach_task_self(), thread);
    bool stateRestored = verified == KERN_SUCCESS && count == x86_DEBUG_STATE64_COUNT &&
        finalState.__dr0 == original.__dr0 && finalState.__dr1 == original.__dr1 &&
        finalState.__dr2 == original.__dr2 && finalState.__dr3 == original.__dr3 &&
        finalState.__dr7 == canonicalDr7(original.__dr7);
    std::printf("SELF_DEBUG_DR7 original=%llx expected=%llx actual=%llx\n",
        static_cast<unsigned long long>(original.__dr7),
        static_cast<unsigned long long>(canonicalDr7(original.__dr7)),
        static_cast<unsigned long long>(finalState.__dr7));
    std::printf("SELF_DEBUG_SET result=%d hits=%d captured=%d returned=%d restore=%d state=%d handler=%d handler-debug=%d\n",
        set, int(hits), int(captured), result, restored, int(stateRestored), handlerRestored, int(handlerRestore));
    if (set != KERN_SUCCESS || hits != 1 || captured != 0x1357 || result != 0x1357 ||
        restored != KERN_SUCCESS || !stateRestored || handlerRestored ||
        handlerRestore != KERN_SUCCESS) return 1;
    alarm(3);
    bool scoped = false;
    {
        NativeReturnObserver observer(reinterpret_cast<uintptr_t>(owned_debug_site));
        const int returned = owned_debug_target();
        scoped = returned == 0x1357 && observer.observed(0x1357);
    }
    alarm(0);
    if (!scoped) return 1;
    {
        NativeReturnObserver unhit(reinterpret_cast<uintptr_t>(owned_debug_site));
        if (!unhit.isArmed() || unhit.observed(0x1357)) return 1;
    } // Early native returns must restore even when the watched site is absent.
    const std::array<uintptr_t, 4> sites = {
        reinterpret_cast<uintptr_t>(owned_debug_site),
        reinterpret_cast<uintptr_t>(owned_debug_site_b),
        reinterpret_cast<uintptr_t>(owned_debug_site_c),
        reinterpret_cast<uintptr_t>(owned_debug_site_d)};
    alarm(3);
    {
        NativeReturnObserver data({reinterpret_cast<uintptr_t>(owned_debug_data_site), 0, 0, 0}, 0, 0, 1);
        if (!owned_debug_data_target() || !data.observed(0, 0x12345678)) return 1;
    }
    {
        NativeReturnObserver rcx({reinterpret_cast<uintptr_t>(owned_debug_data_site), 0, 0, 0}, 0, 1);
        if (!owned_debug_data_target() || !rcx.observed(0, 0x2468)) return 1;
    }
    {
        NativeReturnObserver relative({reinterpret_cast<uintptr_t>(owned_debug_data_site), 0, 0, 0},
            0, 0, 0, 1, reinterpret_cast<uintptr_t>(owned_debug_data), 4);
        if (!owned_debug_data_target() || !relative.observed(0, 0)) return 1;
    }
    {
        NativeReturnObserver outside({reinterpret_cast<uintptr_t>(owned_debug_data_site), 0, 0, 0},
            0, 0, 0, 1, reinterpret_cast<uintptr_t>(owned_debug_data) + 1, 3);
        if (!owned_debug_data_target() || !outside.observed(0, -1)) return 1;
    }
    {
        NativeReturnObserver outsideRcx({reinterpret_cast<uintptr_t>(owned_debug_data_site), 0, 0, 0},
            0, 0, 0, 0, 0x2469, 4, {}, 1);
        if (!owned_debug_data_target() || !outsideRcx.observed(0, -1)) return 1;
    }
    {
        const uint32_t source = 0x1234, replacement = 0x5678, other = 0xabcd;
        uint32_t destination = 0;
        int journal[2];
        if (pipe(journal)) return 1;
        NativeReturnObserver copy({reinterpret_cast<uintptr_t>(owned_debug_copy_site), 0, 0, 0},
            0, 0, 0, 0, 0, 0,
            {reinterpret_cast<uintptr_t>(&source), reinterpret_cast<uintptr_t>(&replacement), 4, journal[1]});
        if (owned_debug_copy_target(&destination, 4, &source, 4) != replacement ||
            destination != replacement || source != 0x1234 || !copy.observed(0, 1)) return 1;
        if (owned_debug_copy_target(&destination, 4, &other, 4) != other) return 1;
        close(journal[1]);
        uint32_t records[12]{};
        if (read(journal[0], records, sizeof(records)) != sizeof(records)) return 1;
        close(journal[0]);
        for (unsigned i = 0; i != 3; ++i)
            if (records[4*i] != 0x4e47524e || records[4*i+1] != i+1) return 1;
        if (records[2] != 0 || records[6] != 1 || records[10] != KERN_SUCCESS) return 1;
    }
    {
        // Capture a register-only relative identity before the pinned copy,
        // keeping the remaining hardware site armed until the final hit.
        const uint32_t source = 0x1234, replacement = 0x5678;
        uint32_t destination = 0;
        int journal[2];
        if (pipe(journal)) return 1;
        NativeReturnObserver mixed({reinterpret_cast<uintptr_t>(owned_debug_copy_site),
            reinterpret_cast<uintptr_t>(owned_debug_data_site), 0, 0},
            0, 0, 0, 0, 0x2000, 0x1000,
            {reinterpret_cast<uintptr_t>(&source), reinterpret_cast<uintptr_t>(&replacement), 4, journal[1]}, 2);
        if (!mixed.isArmed() || !owned_debug_data_target() || !mixed.observed(1, 0x468) ||
            owned_debug_copy_target(&destination, 4, &source, 4) != replacement ||
            destination != replacement || source != 0x1234 || !mixed.observed(0, 1)) return 1;
        close(journal[1]);
        uint32_t records[24]{};
        if (read(journal[0], records, sizeof(records)) != sizeof(records)) return 1;
        close(journal[0]);
        const uint32_t stages[] = {1, 5, 3, 1, 2, 3};
        for (unsigned i = 0; i != 6; ++i)
            if (records[4*i] != 0x4e47524e || records[4*i+1] != stages[i]) return 1;
        if (records[2] != 1 || records[6] != 0x468 || records[11] != 2 ||
            records[18] != 1 || records[22] != KERN_SUCCESS || records[23] != 3) return 1;
    }
    {
        const uint32_t source = 0x1234, replacement = 0x5678, other = 0xabcd;
        uint32_t destination = 0;
        NativeReturnObserver mismatch({reinterpret_cast<uintptr_t>(owned_debug_copy_site), 0, 0, 0},
            0, 0, 0, 0, 0, 0,
            {reinterpret_cast<uintptr_t>(&source), reinterpret_cast<uintptr_t>(&replacement), 4});
        if (owned_debug_copy_target(&destination, 4, &other, 4) != other ||
            !mismatch.observed(0, -1)) return 1;
    }
    for (unsigned mismatch = 0; mismatch != 2; ++mismatch) {
        const uint32_t source = 0x1234, replacement = 0x5678;
        uint32_t destination = 0;
        NativeReturnObserver shape({reinterpret_cast<uintptr_t>(owned_debug_copy_site), 0, 0, 0},
            0, 0, 0, 0, 0, 0,
            {reinterpret_cast<uintptr_t>(&source), reinterpret_cast<uintptr_t>(&replacement), 4});
        const size_t capacity = mismatch ? 4 : 3, bytes = mismatch ? 3 : 4;
        if (owned_debug_copy_target(&destination, capacity, &source, bytes) != 0 ||
            destination != 0 || !shape.observed(0, -1)) return 1;
    }
    {
        NativeReturnObserver boolean(sites, 1);
        if (owned_debug_target() != 0x1357 || !boolean.observed(0, 1)) return 1;
    }
    {
        NativeReturnObserver all(sites);
        if (owned_debug_target_c() != 0x369c || owned_debug_target() != 0x1357 ||
            owned_debug_target_d() != 0x48ad || owned_debug_target_b() != 0x2468 ||
            !all.observed(0, 0x1357) || !all.observed(1, 0x2468) ||
            !all.observed(2, 0x369c) || !all.observed(3, 0x48ad)) return 1;
    }
    {
        NativeReturnObserver partial(sites);
        if (owned_debug_target_c() != 0x369c || !partial.observed(2, 0x369c) ||
            partial.observed(0, 0x1357)) return 1;
    } // Restore the other three armed slots after an early native failure.
    alarm(0);
    std::puts("SELF_DEBUG_OWNED_CODE_OK no-gpu no-apple-text-write scoped-observer=1");
    return 0;
}
