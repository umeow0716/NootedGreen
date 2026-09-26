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
    std::puts("PASS workqueue init failure states, cleanup ordering and repeat cleanup");
}
