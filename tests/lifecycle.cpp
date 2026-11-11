#include <atomic>
#include <cstdlib>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

#if REGPU_REXGLUE
#include <rex/ppc/context.h>
#else
#include <ppc_context.h>
#endif

#include "regpu/driver.h"
#include "regpu/regpu.h"

namespace
{
std::mutex g_mutex;
std::vector<std::thread> g_threads;
std::atomic<int> g_created{ 0 };
std::atomic<int> g_joined{ 0 };
std::atomic<int> g_joinCalls{ 0 };

void CreateThread(const char*, void (*body)(PPCContext&))
{
    std::lock_guard lock(g_mutex);
    g_created++;
    g_threads.emplace_back([body] {
        PPCContext context{};
        body(context);
    });
}

void JoinCreatedThreads()
{
    std::vector<std::thread> threads;
    {
        std::lock_guard lock(g_mutex);
        threads.swap(g_threads);
    }
    for (auto& thread : threads)
        thread.join();
    g_joined += int(threads.size());
}

void JoinThreads()
{
    g_joinCalls++;
    JoinCreatedThreads();
}
}

int main(int argc, char** argv)
{
    _putenv_s("REGPU_HEADLESS", "1");
    _putenv_s("REGPU_LOG_ASYNC", "1");

    regpu::Stop();
    const bool withoutHostJoin = argc == 2 && std::string_view(argv[1]) == "--without-host-join";
    regpu::GetHost().createThread = CreateThread;
    regpu::GetHost().joinThreads = withoutHostJoin ? nullptr : JoinThreads;
    regpu::Start();
    if (argc == 2 && std::string_view(argv[1]) == "--without-stop")
        return 0;
    regpu::driver::SetInterruptCallback(0, 0);

    std::thread first([] { regpu::Stop(); });
    std::thread second([] { regpu::Stop(); });
    first.join();
    second.join();
    regpu::Stop();
    regpu::driver::SetInterruptCallback(1, 1);
    if (withoutHostJoin)
        JoinCreatedThreads();

    const int expectedJoinCalls = withoutHostJoin ? 0 : 1;
    return g_created == 2 && g_joined == 2 && g_joinCalls == expectedJoinCalls ? 0 : 1;
}
