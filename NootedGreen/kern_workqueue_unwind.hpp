#pragma once

namespace NGWorkQueue {
inline void *failedInitMarker() { return reinterpret_cast<void *>(1); }

inline bool markFailedInit(const void *accelerator, const void *lock,
                           const void *buffer, void *&process) {
    if (accelerator || lock || buffer || process)
        return false;
    process = failedInitMarker();
    return true;
}

inline bool consumeFailedInit(const void *accelerator, const void *lock,
                              const void *buffer, void *&process) {
    if (accelerator || lock || buffer || process != failedInitMarker())
        return false;
    process = nullptr;
    return true;
}

// Native published-queue free has already released/nullified its lock and
// mapped buffer. Transfer the still-owned accelerator retain to the caller and
// clear the non-owned process pointer before OSObject base destruction.
inline bool consumePublishedAfterNativeFree(void *&accelerator, const void *lock,
                                            const void *buffer, void *&process,
                                            void *&ownedAccelerator) {
    if (!accelerator || lock || buffer || !process || ownedAccelerator)
        return false;
    ownedAccelerator = accelerator;
    accelerator = nullptr;
    process = nullptr;
    return true;
}

// Only for a fresh, unpublished pinned-TGL workqueue whose native init returned
// false. That function has exactly two failure exits: lock allocation failed,
// or buffer allocation failed with its new lock still held by this thread.
// A non-null buffer does NOT fit this contract: leave everything untouched.
// This helper does not destroy the OSObject or roll back its caller's context.
template <class Operations>
bool unwindFailedInit(void *&accelerator, void *&lock, const void *buffer,
                      Operations &operations) {
    if (buffer)
        return false;
    if (lock) {
        void *ownedLock = lock;
        operations.unlock(ownedLock);
        lock = nullptr;
        operations.freeLock(ownedLock);
    }
    if (accelerator) {
        void *ownedAccelerator = accelerator;
        accelerator = nullptr;
        operations.release(ownedAccelerator);
    }
    return true;
}

// CTB has DIFFERENT native failure exits from WorkQueue. If its second lock
// allocation failed, native already freed the first lock but left its pointer.
// If both locks exist and buffer allocation failed, both are still held.
template <class Operations>
bool unwindFailedCtbInit(void *&accelerator, void *&h2g, void *&g2h,
                         const void *buffer, Operations &operations) {
    if (buffer || (g2h && (!h2g || g2h == h2g)))
        return false;
    if (g2h) {
        void *ownedH2g = h2g, *ownedG2h = g2h;
        operations.unlock(ownedG2h);
        operations.unlock(ownedH2g);
        h2g = nullptr;
        g2h = nullptr;
        operations.freeLock(ownedG2h);
        operations.freeLock(ownedH2g);
    } else {
        h2g = nullptr; // already freed by native, NEVER free it twice
    }
    if (accelerator) {
        void *ownedAccelerator = accelerator;
        accelerator = nullptr;
        operations.release(ownedAccelerator);
    }
    return true;
}
}
