#pragma once
#include <array>
#include <cstdint>
#include <utility>

// Owned lifecycle only. A Darwin backend must separately prove allocation,
// backing inspection and release ABI; this class never fabricates backing.
// Native evidence: 4e060 reuse/reallocation; 63720 failure -> 63c00 cleanup.
struct TglResourceKey {
    uint32_t width, height, format, tile, compressionMode;
    bool compressed;
    bool operator==(const TglResourceKey &b) const noexcept {
        return width == b.width && height == b.height && format == b.format &&
            tile == b.tile && compressionMode == b.compressionMode && compressed == b.compressed;
    }
};

template<class Backend> class TglOwnedResource {
public:
    using Storage = std::array<uint8_t, 0x148>;
    static_assert(noexcept(std::declval<Backend &>().allocate(
        std::declval<Storage &>(), std::declval<const TglResourceKey &>())),
        "allocation must report failure through status, not exceptions");
    static_assert(noexcept(std::declval<Backend &>().release(std::declval<Storage &>())),
        "release must not throw during unwind");
    static_assert(noexcept(std::declval<Backend &>().backed(std::declval<const Storage &>())),
        "backing inspection must not throw");
    enum class State { Empty, DescriptorOnly, Backed };
    struct Result { int status; State state; bool changed; };

    explicit TglOwnedResource(Backend &backend) noexcept : backend_(backend) {}
    ~TglOwnedResource() { reset(); }
    TglOwnedResource(const TglOwnedResource &) = delete;
    TglOwnedResource &operator=(const TglOwnedResource &) = delete;
    TglOwnedResource(TglOwnedResource &&) = delete;
    TglOwnedResource &operator=(TglOwnedResource &&) = delete;

    Result ensure(const TglResourceKey &key) noexcept {
        if (live_ && state_ == State::Backed && key_ == key && backend_.backed(storage_))
            return {0, state_, false};
        // Match native ordering: retire old backing before replacement.
        reset();
        live_ = true; // partial allocation must also be released on failure
        const int status = backend_.allocate(storage_, key);
        if (status != 0) {
            reset();
            return {status, State::Empty, false}; // preserve allocation error
        }
        key_ = key;
        state_ = backend_.backed(storage_) ? State::Backed : State::DescriptorOnly;
        return {0, state_, true}; // native success is NOT proof of GPU backing
    }
    void reset() noexcept {
        if (live_) backend_.release(storage_);
        storage_.fill(0);
        key_ = {};
        live_ = false;
        state_ = State::Empty;
    }
    State state() const noexcept { return state_; }
    const Storage &storage() const noexcept { return storage_; }

private:
    Backend &backend_; // must outlive this owner
    alignas(8) Storage storage_{};
    TglResourceKey key_{};
    bool live_ = false;
    State state_ = State::Empty;
};
