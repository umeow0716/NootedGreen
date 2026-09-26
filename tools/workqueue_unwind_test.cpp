#include "../NootedGreen/kern_workqueue_unwind.hpp"
#include <cassert>
#include <cstdio>
#include <vector>

struct Operations {
    void **accelerator;
    void **lock;
    void *expectedAccelerator;
    void *expectedLock;
    bool locked;
    bool freed = false;
    bool released = false;
    std::vector<int> events;
    void unlock(void *value) {
        assert(value == expectedLock && locked && !freed && *lock == value);
        locked = false;
        events.push_back(1);
    }
    void freeLock(void *value) {
        assert(value == expectedLock && !locked && !freed && !*lock);
        freed = true;
        events.push_back(2);
    }
    void release(void *value) {
        assert(value == expectedAccelerator && !released && !*accelerator);
        assert(!*lock && !locked);
        released = true;
        events.push_back(3);
    }
};

struct CtbOperations {
    void **acceleratorSlot, **h2gSlot, **g2hSlot;
    void *accelerator, *h2g, *g2h;
    bool hLocked, gLocked;
    bool hFreed, gFreed = false, released = false;
    std::vector<int> events;
    void unlock(void *value) {
        if (value == g2h) {
            assert(gLocked && hLocked && !gFreed);
            gLocked = false;
            events.push_back(1);
        } else {
            assert(value == h2g && hLocked && !gLocked && !hFreed);
            hLocked = false;
            events.push_back(2);
        }
    }
    void freeLock(void *value) {
        assert(!hLocked && !gLocked && !*h2gSlot && !*g2hSlot);
        if (value == g2h) {
            assert(!gFreed);
            gFreed = true;
            events.push_back(3);
        } else {
            assert(value == h2g && !hFreed && gFreed);
            hFreed = true;
            events.push_back(4);
        }
    }
    void release(void *value) {
        assert(value == accelerator && !released && !*acceleratorSlot);
        assert(!*h2gSlot && !*g2hSlot && !hLocked && !gLocked);
        released = true;
        events.push_back(5);
    }
};

int main() {
    int acceleratorObject, lockObject, bufferObject;
    for (unsigned mask = 0; mask < 8; ++mask) {
        void *accelerator = mask & 1 ? &acceleratorObject : nullptr;
        void *lock = mask & 2 ? &lockObject : nullptr;
        void *buffer = mask & 4 ? &bufferObject : nullptr;
        void *savedAccelerator = accelerator, *savedLock = lock;
        Operations ops {&accelerator, &lock, accelerator, lock, lock != nullptr};
        const bool result = NGWorkQueue::unwindFailedInit(accelerator, lock, buffer, ops);
        if (buffer) {
            assert(!result && accelerator == savedAccelerator && lock == savedLock);
            assert(ops.events.empty() && !ops.freed && !ops.released);
        } else {
            assert(result && !accelerator && !lock && !ops.locked);
            std::vector<int> expected;
            if (savedLock) { expected.push_back(1); expected.push_back(2); }
            if (savedAccelerator) expected.push_back(3);
            assert(ops.events == expected);
            assert(NGWorkQueue::unwindFailedInit(accelerator, lock, nullptr, ops));
            assert(ops.events == expected); // no duplicate unlock/free/release
        }
    }
    for (unsigned resources = 0; resources < 8; ++resources) {
        const void *accelerator = resources & 1 ? &acceleratorObject : nullptr;
        const void *lock = resources & 2 ? &lockObject : nullptr;
        const void *buffer = resources & 4 ? &bufferObject : nullptr;
        for (void *initial : {static_cast<void *>(nullptr), static_cast<void *>(&bufferObject),
                              NGWorkQueue::failedInitMarker()}) {
            void *process = initial;
            const bool marked = NGWorkQueue::markFailedInit(accelerator, lock, buffer, process);
            assert(marked == (resources == 0 && initial == nullptr));
            assert(process == (marked ? NGWorkQueue::failedInitMarker() : initial));
            process = initial;
            const bool consumed = NGWorkQueue::consumeFailedInit(accelerator, lock, buffer, process);
            assert(consumed == (resources == 0 && initial == NGWorkQueue::failedInitMarker()));
            assert(process == (consumed ? nullptr : initial));
            if (consumed)
                assert(!NGWorkQueue::consumeFailedInit(nullptr, nullptr, nullptr, process));
        }
    }
    int secondLockObject;
    for (unsigned mask = 0; mask < 16; ++mask) {
        void *accelerator = mask & 1 ? &acceleratorObject : nullptr;
        void *h2g = mask & 2 ? &lockObject : nullptr;
        void *g2h = mask & 4 ? &secondLockObject : nullptr;
        void *buffer = mask & 8 ? &bufferObject : nullptr;
        void *savedAccelerator = accelerator, *savedH2g = h2g, *savedG2h = g2h;
        CtbOperations ops {&accelerator, &h2g, &g2h, accelerator, h2g, g2h,
                           h2g && g2h, h2g && g2h, h2g && !g2h};
        const bool valid = !buffer && !(g2h && !h2g);
        const bool result = NGWorkQueue::unwindFailedCtbInit(accelerator, h2g, g2h, buffer, ops);
        assert(result == valid);
        if (!valid) {
            assert(accelerator == savedAccelerator && h2g == savedH2g && g2h == savedG2h);
            assert(ops.events.empty());
        } else {
            assert(!accelerator && !h2g && !g2h);
            std::vector<int> expected;
            if (savedG2h) expected = {1, 2, 3, 4};
            if (savedAccelerator) expected.push_back(5);
            assert(ops.events == expected);
            assert(NGWorkQueue::unwindFailedCtbInit(accelerator, h2g, g2h, nullptr, ops));
            assert(ops.events == expected);
        }
    }
    std::puts("PASS workqueue unwind, 24 destruction-marker states and 16 CTB failure states");
    // An impossible duplicate-lock state must never unlock/free one lock twice.
    void *accelerator = &acceleratorObject, *h2g = &lockObject, *g2h = &lockObject;
    CtbOperations ops {&accelerator, &h2g, &g2h, accelerator, h2g, g2h, true, true, false};
    assert(!NGWorkQueue::unwindFailedCtbInit(accelerator, h2g, g2h, nullptr, ops));
    assert(ops.events.empty() && accelerator == &acceleratorObject && h2g == g2h);
}
