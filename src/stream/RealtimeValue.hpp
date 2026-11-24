#pragma once

#include <array>
#include <atomic>

namespace openstone::stream {

// A single control thread owns the writable slot and a single render thread
// owns the readable slot. The exchange keeps ownership separate without blocking.
template<class Value>
class RealtimeValue {
public:
    Value& writable() noexcept { return slots_[writer_]; }

    void publish() noexcept
    {
        writer_ = exchange_.exchange(writer_ | pending, std::memory_order_acq_rel) & ~pending;
    }

    [[nodiscard]] const Value& read() noexcept
    {
        if (exchange_.load(std::memory_order_acquire) & pending) {
            reader_ = exchange_.exchange(reader_, std::memory_order_acq_rel) & ~pending;
        }
        return slots_[reader_];
    }

private:
    static_assert(std::atomic<unsigned>::is_always_lock_free);
    static constexpr unsigned pending = 4;
    std::array<Value, 3> slots_{};
    unsigned writer_ = 0;
    unsigned reader_ = 1;
    std::atomic<unsigned> exchange_{2};
};

}
