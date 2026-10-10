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
#include <array>
#include <cerrno>

struct NativeOwnedCopy {
    uintptr_t source = 0, replacement = 0, bytes = 0;
    int journal = -1;
};

class NativeReturnObserver {
    inline static std::mutex lock;
    inline static std::atomic<NativeReturnObserver *> active{nullptr};
    static_assert(std::atomic<NativeReturnObserver *>::is_always_lock_free);
    std::unique_lock<std::mutex> guard{lock, std::try_to_lock};
    thread_t thread = MACH_PORT_NULL;
    x86_debug_state64_t saved{};
    x86_debug_state64_t programmed{};
    struct sigaction previous{};
    std::array<uintptr_t, 4> sites{};
    sig_atomic_t wanted = 0;
    volatile sig_atomic_t hitMask = 0;
    volatile sig_atomic_t values[4]{};
    unsigned booleanSites = 0;
    unsigned rcxSites = 0;
    unsigned readRax32Sites = 0;
    unsigned rdxImageSites = 0;
    unsigned rcxImageSites = 0;
    uintptr_t imageBase = 0;
    uintptr_t imageBytes = 0;
    NativeOwnedCopy ownedCopy{};
    volatile sig_atomic_t handlerRestore = KERN_FAILURE;
    bool installed = false;
    bool armed = false;
    // Fixed records, no pointers; write is async-signal-safe. Diagnostic failure
    // must never alter native signal/debug policy or the copy decision.
    void record(uint32_t stage, uint32_t value) const {
        if (ownedCopy.journal < 0) return;
        const int savedErrno = errno;
        const uint32_t words[] = {0x4e47524e, stage, value, uint32_t(hitMask)};
        (void)write(ownedCopy.journal, words, sizeof(words));
        errno = savedErrno;
    }
    static constexpr uint64_t canonical(uint64_t dr7) {
        return (dr7 | uint64_t(0x400)) & ~uint64_t(0xd800);
    }
    static void trap(int signal, siginfo_t *, void *context) {
        auto *self = active.load(std::memory_order_relaxed);
        auto *state = static_cast<ucontext_t *>(context);
        unsigned site = 4;
        if (signal == SIGTRAP && self && state && state->uc_mcontext) {
            for (unsigned i = 0; i != 4; ++i)
                if (self->sites[i] && state->uc_mcontext->__ss.__rip == self->sites[i] &&
                    !(self->hitMask & (1 << i))) site = i;
        }
        if (self) self->record(1, site);
        if (site == 4) {
            // Admission requires SIG_DFL. Preserve its terminating semantics;
            // never consume another thread's unrelated debugger exception.
            struct sigaction action{};
            action.sa_handler = SIG_DFL;
            sigemptyset(&action.sa_mask);
            sigaction(SIGTRAP, &action, nullptr);
            raise(SIGTRAP);
            return;
        }
        auto raw = state->uc_mcontext->__ss.__rax;
        if (self->ownedCopy.bytes && site == 0) {
            // Pinned memcpy_s call arguments only. The owned source remains live
            // until native returns; no Apple code, RIP, flags or result changes.
            const auto &copy = self->ownedCopy;
            const bool exact = state->uc_mcontext->__ss.__rdx == copy.source &&
                state->uc_mcontext->__ss.__rcx == copy.bytes &&
                state->uc_mcontext->__ss.__rsi == copy.bytes &&
                state->uc_mcontext->__ss.__rdi != 0;
            if (exact) state->uc_mcontext->__ss.__rdx = copy.replacement;
            raw = exact ? 1 : uintptr_t(-1);
            self->record(2, exact ? 1 : 0);
        }
        if (self->rcxSites & (1u << site)) raw = state->uc_mcontext->__ss.__rcx;
        // Only caller-pinned native DWORD-load sites opt in. Exactly the same
        // four bytes that the interrupted instruction will read; no RPC/allocation.
        if (self->readRax32Sites & (1u << site))
            raw = *reinterpret_cast<const volatile uint32_t *>(raw);
        // Report only a bounded image-relative offset, never the pointer itself.
        if (self->rdxImageSites & (1u << site)) {
            const uintptr_t pointer = state->uc_mcontext->__ss.__rdx;
            raw = pointer >= self->imageBase && pointer - self->imageBase < self->imageBytes
                ? pointer - self->imageBase : uintptr_t(-1);
        }
        if (self->rcxImageSites & (1u << site)) {
            const uintptr_t pointer = state->uc_mcontext->__ss.__rcx;
            raw = pointer >= self->imageBase && pointer - self->imageBase < self->imageBytes
                ? pointer - self->imageBase : uintptr_t(-1);
        }
        if ((self->rdxImageSites | self->rcxImageSites) & (1u << site))
            self->record(4 + site, uint32_t(raw));
        self->values[site] = self->booleanSites & (1u << site)
            ? sig_atomic_t(raw != 0) : static_cast<sig_atomic_t>(raw);
        self->hitMask |= 1 << site;
        auto next = self->programmed;
        for (unsigned i = 0; i != 4; ++i)
            if (self->hitMask & (1 << i)) next.__dr7 &= ~(uint64_t(3) << (2 * i));
        // Keep only unvisited sites armed. The final hit restores everything.
        if (self->hitMask == self->wanted) next = self->saved;
        self->handlerRestore = thread_set_state(self->thread, x86_DEBUG_STATE64,
            reinterpret_cast<thread_state_t>(&next), x86_DEBUG_STATE64_COUNT);
        self->record(3, uint32_t(self->handlerRestore));
        if (self->handlerRestore != KERN_SUCCESS) _exit(74);
    }
public:
    explicit NativeReturnObserver(uintptr_t site)
        : NativeReturnObserver(std::array<uintptr_t, 4>{site, 0, 0, 0}) {}
    explicit NativeReturnObserver(const std::array<uintptr_t, 4> &selected, unsigned booleans = 0,
        unsigned rcx = 0, unsigned readRax32 = 0, unsigned rdxImage = 0,
        uintptr_t base = 0, uintptr_t bytes = 0, NativeOwnedCopy copy = {}, unsigned rcxImage = 0)
        : sites(selected), booleanSites(booleans), rcxSites(rcx), readRax32Sites(readRax32),
          rdxImageSites(rdxImage), rcxImageSites(rcxImage), imageBase(base), imageBytes(bytes), ownedCopy(copy) {
        for (unsigned i = 0; i != 4; ++i) {
            if (!sites[i]) continue;
            for (unsigned j = 0; j != i; ++j) if (sites[j] == sites[i]) return;
            wanted |= 1 << i;
        }
        if (!guard.owns_lock() || !wanted ||
            ((booleanSites | rcxSites | readRax32Sites | rdxImageSites | rcxImageSites) & ~unsigned(wanted)) ||
            (rcxSites & readRax32Sites) ||
            (rdxImageSites & (booleanSites | rcxSites | readRax32Sites)) ||
            (rcxImageSites & (booleanSites | rcxSites | readRax32Sites | rdxImageSites)) ||
            ((rdxImageSites | rcxImageSites) && (!imageBase || !imageBytes || imageBytes > 0x7fffffff ||
                imageBase + imageBytes < imageBase)) ||
            (ownedCopy.bytes && (!(wanted & 1) || booleanSites || rcxSites || readRax32Sites ||
                ((rdxImageSites | rcxImageSites) & 1) ||
                ((unsigned(wanted) & ~1u) != (rdxImageSites | rcxImageSites)) ||
                !ownedCopy.source || !ownedCopy.replacement || ownedCopy.source == ownedCopy.replacement ||
                ownedCopy.bytes > 0x7fffffff || ownedCopy.source + ownedCopy.bytes < ownedCopy.source ||
                ownedCopy.replacement + ownedCopy.bytes < ownedCopy.replacement))) {
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
        programmed = saved;
        for (unsigned i = 0; i != 4; ++i) {
            if (!sites[i]) continue;
            switch (i) {
                case 0: programmed.__dr0 = sites[i]; break;
                case 1: programmed.__dr1 = sites[i]; break;
                case 2: programmed.__dr2 = sites[i]; break;
                case 3: programmed.__dr3 = sites[i]; break;
            }
            programmed.__dr7 &= ~(uint64_t(0xf) << (16 + 4 * i));
            programmed.__dr7 |= uint64_t(1) << (2 * i);
        }
        const kern_return_t result = thread_set_state(thread, x86_DEBUG_STATE64,
            reinterpret_cast<thread_state_t>(&programmed), x86_DEBUG_STATE64_COUNT);
        armed = result == KERN_SUCCESS;
        std::fprintf(stderr, "NGRN_NATIVE_RETURN_OBSERVER_ARM result=%d armed=%d\n", result, armed);
        os_log_error(OS_LOG_DEFAULT, "NGRN_NATIVE_RETURN_OBSERVER_ARM result=%{public}d armed=%{public}d", result, int(armed));
    }
    NativeReturnObserver(const NativeReturnObserver &) = delete;
    NativeReturnObserver &operator=(const NativeReturnObserver &) = delete;
    bool observed(unsigned site, int expected) const {
        return site < 4 && armed && (hitMask & (1 << site)) && values[site] == expected;
    }
    bool observed(int expected) const { return observed(0, expected); }
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
            int hits = 0;
            for (unsigned i = 0; i != 4; ++i) {
                if (hitMask & (1 << i)) ++hits;
                if (!sites[i]) continue;
                std::fprintf(stderr, "NGRN_NATIVE_RETURN_SITE site=%u hit=%d raw=%d\n",
                    i, int(bool(hitMask & (1 << i))), int(values[i]));
                os_log_error(OS_LOG_DEFAULT,
                    "NGRN_NATIVE_RETURN_SITE site=%{public}u hit=%{public}d raw=%{public}d",
                    i, int(bool(hitMask & (1 << i))), int(values[i]));
            }
            std::fprintf(stderr,
                "NGRN_NATIVE_RETURN_OBSERVER_DONE armed=%d hits=%d raw=%d restore=%d state=%d handler=%d handler-debug=%d\n",
                armed, hits, int(values[0]), restored, same, handler, handlerRestore);
            os_log_error(OS_LOG_DEFAULT,
                "NGRN_NATIVE_RETURN_OBSERVER_DONE armed=%{public}d hits=%{public}d raw=%{public}d restore=%{public}d state=%{public}d handler=%{public}d handler-debug=%{public}d",
                int(armed), hits, int(values[0]), restored, int(same), handler, int(handlerRestore));
            if (restored != KERN_SUCCESS || !same || handler || (hits && handlerRestore != KERN_SUCCESS)) _exit(74);
        }
        if (thread != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), thread);
    }
};
