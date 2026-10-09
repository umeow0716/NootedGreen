// Owned-code-only native-policy test. No GPU, Apple image, task attachment,
// entitlement change, text write, or instruction-pointer modification.
#include <mach/mach.h>
#include <mach/i386/thread_status.h>
#include <signal.h>
#include <sys/ucontext.h>
#include <unistd.h>
#include <cstdio>
#include <cstdint>

extern "C" int owned_debug_target();
extern "C" const char owned_debug_site[];
asm(".text\n.p2align 4\n.globl _owned_debug_target\n"
    "_owned_debug_target:\nmovl $0x1357, %eax\n"
    ".globl _owned_debug_site\n_owned_debug_site:\nretq\n");

static volatile sig_atomic_t hits = 0;
static volatile sig_atomic_t captured = 0;
static void trap(int signal, siginfo_t *, void *context) {
    auto *state = static_cast<ucontext_t *>(context);
    if (signal != SIGTRAP || !state || !state->uc_mcontext || hits ||
        state->uc_mcontext->__ss.__rip != reinterpret_cast<uintptr_t>(owned_debug_site) ||
        state->uc_mcontext->__ss.__rax != 0x1357) _exit(70);
    captured = static_cast<sig_atomic_t>(state->uc_mcontext->__ss.__rax);
    hits = 1;
    // RF resumes the same instruction once; never change RIP or result registers.
    state->uc_mcontext->__ss.__rflags |= 0x10000;
}

int main() {
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
        finalState.__dr7 == original.__dr7;
    std::printf("SELF_DEBUG_SET result=%d hits=%d captured=%d returned=%d restore=%d state=%d handler=%d\n",
        set, int(hits), int(captured), result, restored, int(stateRestored), handlerRestored);
    if (set != KERN_SUCCESS || hits != 1 || captured != 0x1357 || result != 0x1357 ||
        restored != KERN_SUCCESS || !stateRestored || handlerRestored) return 1;
    std::puts("SELF_DEBUG_OWNED_CODE_OK no-gpu no-apple-text-write");
    return 0;
}
