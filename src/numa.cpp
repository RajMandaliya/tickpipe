#include "numa.hpp"
#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

bool ThreadPinner::pin_to_core(int core_id) noexcept {
    if (core_id < 0 || core_id >= core_count()) return false;

#ifdef _WIN32
    HANDLE thread  = GetCurrentThread();
    DWORD_PTR mask = static_cast<DWORD_PTR>(1) << core_id;
    return SetThreadAffinityMask(thread, mask) != 0;
#else
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    return pthread_setaffinity_np(
        pthread_self(), sizeof(cpu_set_t), &cpuset) == 0;
#endif
}

int ThreadPinner::current_core() noexcept {
#ifdef _WIN32
    return static_cast<int>(GetCurrentProcessorNumber());
#else
    return sched_getcpu();
#endif
}