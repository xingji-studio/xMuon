#pragma once

#include <atomic>
#include <cstddef>

struct BrowserEvent
{
    unsigned long long type, high, low;
    unsigned int epoch = 0;
};

// One kernel window-message thread produces events; the browser main thread
// consumes them. No allocator, STL container, or application state is touched
// by the producer. Release/acquire publishes each complete event.
class BrowserEventQueue
{
public:
    bool push(const BrowserEvent &event)
    {
        const unsigned head = head_.load(std::memory_order_relaxed);
        const unsigned next = (head + 1) % capacity;
        if (next == tail_.load(std::memory_order_acquire)) return false;
        events_[head] = event;
        head_.store(next, std::memory_order_release);
        return true;
    }

    // Only the consumer peeks. It can leave a full deferred-input backlog in
    // place without consuming and losing the next event during resource IO.
    bool peek(BrowserEvent &event) const
    {
        const unsigned tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) return false;
        event = events_[tail];
        return true;
    }

    bool pop(BrowserEvent &event)
    {
        const unsigned tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) return false;
        event = events_[tail];
        tail_.store((tail + 1) % capacity, std::memory_order_release);
        return true;
    }

private:
    static constexpr unsigned capacity = 256;
    BrowserEvent events_[capacity] {};
    std::atomic<unsigned> head_ {0}, tail_ {0};
};
