#pragma once
// Scoped, same-thread, native-policy observation. No text/register-result writes.
#include <mach/mach.h>
#include <mach/i386/thread_status.h>
#include <signal.h>
#include <sys/ucontext.h>
#include <unistd.h>
#include <os/log.h>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <mutex>

class NativeReturnObserver {
    inline static std::mutex lock;
    inline static std::atomic<NativeReturnObserver *> active{nullptr};
    static_assert(std::atomic<NativeReturnObserver *>::is_always_lock_free);
    std::unique_lock<std::mutex> guard{lock, std::try_to_lock};
    thread_t thread = MACH_PORT_NULL;
    x86_debug_state64_t saved{};
    struct sigaction previous{};
    uintptr_t target = 0;
    volatile sig_atomic_t hits = 0;
    volatile sig_atomic_t value = 0;
    volatile sig_atomic_t handlerRestore = KERN_FAILURE;
    bool installed = false;
    bool armed = false;
    static constexpr uint64_t canonical(uint64_t dr7) {
        return (dr7 | uint64_t(0x400)) & ~uint64_t(0xd800);
    }
    static void trap(int signal, siginfo_t *, void *context) {
        auto *self = active.load(std::memory_order_relaxed);
        auto *state = static_cast<ucontext_t *>(context);
        if (signal != SIGTRAP || !self || self->hits || !state ||
            !state->uc_mcontext || state->uc_mcontext->__ss.__rip != self->target) {
            // Admission requires SIG_DFL. Preserve its terminating semantics;
            // never consume another thread's unrelated debugger exception.
            struct sigaction action{};
            action.sa_handler = SIG_DFL;
            sigemptyset(&action.sa_mask);
            sigaction(SIGTRAP, &action, nullptr);
            raise(SIGTRAP);
            return;
        }
        self->value = static_cast<sig_atomic_t>(state->uc_mcontext->__ss.__rax);
        self->hits = 1;
        self->handlerRestore = thread_set_state(self->thread, x86_DEBUG_STATE64,
            reinterpret_cast<thread_state_t>(&self->saved), x86_DEBUG_STATE64_COUNT);
        if (self->handlerRestore != KERN_SUCCESS) _exit(74);
    }
public:
    explicit NativeReturnObserver(uintptr_t site) : target(site) {
        if (!guard.owns_lock() || !target) {
            os_log_error(OS_LOG_DEFAULT, "NGRN_NATIVE_RETURN_OBSERVER_SKIP busy-or-no-site");
            return;
        }
        thread = mach_thread_self();
        mach_msg_type_number_t count = x86_DEBUG_STATE64_COUNT;
        if (thread_get_state(thread, x86_DEBUG_STATE64,
                reinterpret_cast<thread_state_t>(&saved), &count) != KERN_SUCCESS ||
            count != x86_DEBUG_STATE64_COUNT || (saved.__dr7 & 0xff)) {
            os_log_error(OS_LOG_DEFAULT, "NGRN_NATIVE_RETURN_OBSERVER_SKIP native-state-or-existing-debugger");
            return;
        }
        if (sigaction(SIGTRAP, nullptr, &previous) || previous.sa_handler != SIG_DFL) {
            os_log_error(OS_LOG_DEFAULT, "NGRN_NATIVE_RETURN_OBSERVER_SKIP existing-signal-handler");
            return;
        }
        struct sigaction action{};
        action.sa_sigaction = trap;
        action.sa_flags = SA_SIGINFO;
        sigemptyset(&action.sa_mask);
        active = this;
        if (sigaction(SIGTRAP, &action, nullptr)) {
            active = nullptr;
            os_log_error(OS_LOG_DEFAULT, "NGRN_NATIVE_RETURN_OBSERVER_SKIP signal-install-denied");
            return;
        }
        installed = true;
        auto debug = saved;
        debug.__dr0 = target;
        debug.__dr7 = (debug.__dr7 & ~uint64_t(0xf0003)) | uint64_t(1);
        const kern_return_t result = thread_set_state(thread, x86_DEBUG_STATE64,
            reinterpret_cast<thread_state_t>(&debug), x86_DEBUG_STATE64_COUNT);
        armed = result == KERN_SUCCESS;
        std::fprintf(stderr, "NGRN_NATIVE_RETURN_OBSERVER_ARM result=%d armed=%d\n", result, armed);
        os_log_error(OS_LOG_DEFAULT, "NGRN_NATIVE_RETURN_OBSERVER_ARM result=%{public}d armed=%{public}d", result, int(armed));
    }
    NativeReturnObserver(const NativeReturnObserver &) = delete;
    NativeReturnObserver &operator=(const NativeReturnObserver &) = delete;
    bool observed(int expected) const { return armed && hits == 1 && value == expected; }
    bool isArmed() const { return armed; }
    ~NativeReturnObserver() {
        if (installed && !armed) {
            if (sigaction(SIGTRAP, &previous, nullptr)) _exit(74);
            active = nullptr;
        }
        if (installed && armed) {
            const kern_return_t restored = thread_set_state(thread, x86_DEBUG_STATE64,
                reinterpret_cast<thread_state_t>(&saved), x86_DEBUG_STATE64_COUNT);
            x86_debug_state64_t actual{};
            mach_msg_type_number_t count = x86_DEBUG_STATE64_COUNT;
            const kern_return_t readback = thread_get_state(thread, x86_DEBUG_STATE64,
                reinterpret_cast<thread_state_t>(&actual), &count);
            const bool same = readback == KERN_SUCCESS && count == x86_DEBUG_STATE64_COUNT &&
                actual.__dr0 == saved.__dr0 && actual.__dr1 == saved.__dr1 &&
                actual.__dr2 == saved.__dr2 && actual.__dr3 == saved.__dr3 &&
                actual.__dr7 == canonical(saved.__dr7);
            const int handler = sigaction(SIGTRAP, &previous, nullptr);
            active = nullptr;
            std::fprintf(stderr,
                "NGRN_NATIVE_RETURN_OBSERVER_DONE armed=%d hits=%d raw=%d restore=%d state=%d handler=%d handler-debug=%d\n",
                armed, hits, value, restored, same, handler, handlerRestore);
            os_log_error(OS_LOG_DEFAULT,
                "NGRN_NATIVE_RETURN_OBSERVER_DONE armed=%{public}d hits=%{public}d raw=%{public}d restore=%{public}d state=%{public}d handler=%{public}d handler-debug=%{public}d",
                int(armed), int(hits), int(value), restored, int(same), handler, int(handlerRestore));
            if (restored != KERN_SUCCESS || !same || handler || (hits && handlerRestore != KERN_SUCCESS)) _exit(74);
        }
        if (thread != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), thread);
    }
};
