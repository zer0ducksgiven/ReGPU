#pragma once

#include <chrono>
#include <thread>
#include <utility>

void ReGpuLifecycleStart();
void ReGpuRequestStop();
bool ReGpuStopping();
bool ReGpuWaitForStop(std::chrono::milliseconds timeout);

// Stop joins owned threads. Detaching only at static destruction preserves the old process-exit
// behavior for hosts that have not adopted Stop yet instead of terminating on a joinable thread.
class ReGpuOwnedThread
{
public:
    ReGpuOwnedThread() noexcept = default;
    template <typename Function, typename... Args>
    explicit ReGpuOwnedThread(Function&& function, Args&&... args)
        : m_thread(std::forward<Function>(function), std::forward<Args>(args)...)
    {
    }
    ReGpuOwnedThread(const ReGpuOwnedThread&) = delete;
    ReGpuOwnedThread& operator=(const ReGpuOwnedThread&) = delete;
    ReGpuOwnedThread(ReGpuOwnedThread&&) noexcept = default;
    ReGpuOwnedThread& operator=(ReGpuOwnedThread&& other) noexcept
    {
        Detach();
        m_thread = std::move(other.m_thread);
        return *this;
    }
    ReGpuOwnedThread& operator=(std::thread&& thread) noexcept
    {
        Detach();
        m_thread = std::move(thread);
        return *this;
    }
    ~ReGpuOwnedThread() { Detach(); }

    bool joinable() const noexcept { return m_thread.joinable(); }
    void join() { m_thread.join(); }
    std::thread::native_handle_type native_handle() { return m_thread.native_handle(); }

private:
    void Detach() noexcept
    {
        if (m_thread.joinable())
            m_thread.detach();
    }

    std::thread m_thread;
};
