#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>
#include <new>

// Single-producer single-consumer byte ring.
//
// The receive thread must never do work beyond emptying the socket — every
// microsecond it spends parsing is a microsecond the kernel buffer is filling.
// So it copies raw datagrams in here and goes straight back to recv(); the
// book thread drains at its own pace.
//
// Bursts then land in userspace memory, which can be 128 MB, instead of the
// ~8 MB the OS will give a socket.
//
// Records are length-prefixed: [4-byte LE length][payload]. Capacity is a
// power of two so the wrap is a mask.

class SpscRing {
public:
    explicit SpscRing(uint32_t capacity_pow2 = 27)      // 1<<27 = 128 MB
        : capacity_(uint64_t{1} << capacity_pow2)
        , mask_(capacity_ - 1)
        , buf_(new uint8_t[capacity_])
        , head_(0)
        , tail_(0)
        , dropped_(0)
        , high_water_(0)
    {}

    ~SpscRing() { delete[] buf_; }

    SpscRing(const SpscRing&)            = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    // producer side. false means the ring was full and the record was dropped —
    // that is a real loss and gets counted, not hidden.
    bool push(const uint8_t* data, uint32_t len) noexcept {
        const uint64_t head = head_.load(std::memory_order_relaxed);
        const uint64_t tail = tail_.load(std::memory_order_acquire);
        const uint64_t used = head - tail;
        const uint64_t need = 4 + len;

        if (used + need > capacity_) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (used > high_water_) high_water_ = used;

        write(head, reinterpret_cast<const uint8_t*>(&len), 4);
        write(head + 4, data, len);

        head_.store(head + need, std::memory_order_release);
        return true;
    }

    // consumer side. returns bytes written to out, or 0 when empty.
    uint32_t pop(uint8_t* out, uint32_t out_cap) noexcept {
        const uint64_t tail = tail_.load(std::memory_order_relaxed);
        const uint64_t head = head_.load(std::memory_order_acquire);
        if (head == tail) return 0;

        uint32_t len = 0;
        read(tail, reinterpret_cast<uint8_t*>(&len), 4);
        if (len == 0 || len > out_cap) {        // cannot happen; bail safely
            tail_.store(head, std::memory_order_release);
            return 0;
        }
        read(tail + 4, out, len);
        tail_.store(tail + 4 + len, std::memory_order_release);
        return len;
    }

    uint64_t dropped()    const noexcept { return dropped_.load(std::memory_order_relaxed); }
    uint64_t high_water() const noexcept { return high_water_; }
    uint64_t capacity()   const noexcept { return capacity_; }
    uint64_t used() const noexcept {
        return head_.load(std::memory_order_acquire)
             - tail_.load(std::memory_order_acquire);
    }

private:
    const uint64_t capacity_;
    const uint64_t mask_;
    uint8_t* const buf_;

    // keep the two cursors off each other's cache line
    alignas(64) std::atomic<uint64_t> head_;
    alignas(64) std::atomic<uint64_t> tail_;
    alignas(64) std::atomic<uint64_t> dropped_;
    uint64_t high_water_;

    void write(uint64_t at, const uint8_t* src, uint32_t n) noexcept {
        const uint64_t off = at & mask_;
        const uint64_t first = (off + n <= capacity_) ? n : capacity_ - off;
        std::memcpy(buf_ + off, src, first);
        if (first < n) std::memcpy(buf_, src + first, n - first);
    }

    void read(uint64_t at, uint8_t* dst, uint32_t n) const noexcept {
        const uint64_t off = at & mask_;
        const uint64_t first = (off + n <= capacity_) ? n : capacity_ - off;
        std::memcpy(dst, buf_ + off, first);
        if (first < n) std::memcpy(dst + first, buf_, n - first);
    }
};