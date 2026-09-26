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
}
