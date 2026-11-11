#include "lifecycle.h"

#include <atomic>
#include <condition_variable>
#include <mutex>

namespace
{
std::atomic<bool> g_stopping{ true };
std::mutex g_mutex;
std::condition_variable g_cv;
}

void ReGpuLifecycleStart()
{
    g_stopping.store(false, std::memory_order_release);
}

void ReGpuRequestStop()
{
    g_stopping.store(true, std::memory_order_release);
    g_cv.notify_all();
}

bool ReGpuStopping()
{
    return g_stopping.load(std::memory_order_acquire);
}

bool ReGpuWaitForStop(std::chrono::milliseconds timeout)
{
    std::unique_lock lock(g_mutex);
    return g_cv.wait_for(lock, timeout, [] { return ReGpuStopping(); });
}
