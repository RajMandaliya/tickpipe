#pragma once
#include <cstdint>
#include <thread>

// pins the calling thread to a specific CPU core
// eliminates OS scheduler moving the thread between cores
// which causes cache invalidation and latency spikes
//
// real HFT systems dedicate entire cores to matching:
//   core 0 — matching engine (us)
//   core 1 — network I/O
//   core 2 — risk engine
//   core 3 — logging
//
// on Windows we use SetThreadAffinityMask
// on Linux we'd use pthread_setaffinity_np
class ThreadPinner {
public:
    // pin calling thread to specific core, returns false if core unavailable
    static bool pin_to_core(int core_id) noexcept;

    // pin to core 0 — matching engine default
    static bool pin_to_matching_core() noexcept {
        return pin_to_core(0);
    }

    // how many logical cores are available
    static int core_count() noexcept {
        return static_cast<int>(std::thread::hardware_concurrency());
    }

    // returns which core the calling thread is on
    static int current_core() noexcept;
};