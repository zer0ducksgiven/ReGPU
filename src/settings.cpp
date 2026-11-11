// Config and Host storage, and Start/Stop (include/regpu/regpu.h).
#include "settings.h"

#include <algorithm>
#include <condition_variable>
#include <mutex>

#include "debug.h"
#include "gpu/shader_scan.h"
#include "lifecycle.h"
#include "log.h"
#include "memory.h"
#include "gpu/renderer.h"
#include "render/presenter.h"

void FrameStop();
void GpuStop();
void GpuLogState();
void GpuInit();

namespace regpu
{
namespace
{
enum class Lifecycle
{
    NotStarted,
    Running,
    Stopping,
    Stopped
};

std::mutex g_lifecycleMutex;
std::condition_variable g_lifecycleCv;
Lifecycle g_lifecycle = Lifecycle::NotStarted;
}

Config& GetConfig()
{
    static Config config;
    return config;
}

Host& GetHost()
{
    static Host host;
    return host;
}

void Start()
{
    std::lock_guard lock(g_lifecycleMutex);
    if (g_lifecycle != Lifecycle::NotStarted)
        Fatal("regpu::Start called more than once");
    ReGpuLifecycleStart();
    g_base = GetHost().base;
    DebugServerStart(); // REGPU_DEBUG=1: tools/regpuctl.py drives the running game
    PresenterInit();
    GpuInit();
    g_lifecycle = Lifecycle::Running;
}

void Stop()
{
    {
        std::unique_lock lock(g_lifecycleMutex);
        if (g_lifecycle == Lifecycle::NotStarted || g_lifecycle == Lifecycle::Stopped)
            return;
        if (g_lifecycle == Lifecycle::Stopping)
        {
            g_lifecycleCv.wait(lock, [] { return g_lifecycle == Lifecycle::Stopped; });
            return;
        }
        g_lifecycle = Lifecycle::Stopping;
    }

    Log("lifecycle: stop requested");
    ReGpuRequestStop();
    DebugServerStop();
    GpuStop();
    ShaderScanStop();
    FrameStop();
    RendererStop();
    PresenterStop();
    Log("lifecycle: all ReGPU threads joined");
    LogStop();

    {
        std::lock_guard lock(g_lifecycleMutex);
        g_lifecycle = Lifecycle::Stopped;
    }
    g_lifecycleCv.notify_all();
}

void Config::RenderSize(uint32_t& w, uint32_t& h) const
{
    w = width;
    h = height;
    // The forced aspect, or the resolution's own; never narrower than 16:9 (narrower views
    // are not supported yet: such resolutions get 16:9 with bars).
    uint64_t aw = aspectW, ah = aspectH;
    if (!aw || !ah)
        aw = width, ah = height;
    if (aw * 9 < ah * 16)
        aw = 16, ah = 9;
    // Fit that aspect inside the resolution.
    if (uint64_t(width) * ah > uint64_t(height) * aw)
        w = uint32_t(uint64_t(height) * aw / ah) & ~1u;
    else
        h = uint32_t(uint64_t(width) * ah / aw) & ~1u;
}

float Config::SceneScale() const
{
    if (upscaler == Upscaler::Off)
        return 1.0f;
    // The usual presets (native, 1.5x, 1.7x, 2x, 3x per axis).
    static const float scales[] = { 1.0f, 1.0f / 1.5f, 1.0f / 1.7f, 0.5f, 1.0f / 3.0f };
    return scales[std::min<uint32_t>(quality, 4)];
}
} // namespace regpu

void regpu::SaveCaches()
{
    std::lock_guard lock(g_lifecycleMutex);
    if (g_lifecycle == Lifecycle::Running)
        RendererSaveCaches();
}

bool regpu::WindowHasFocus() { return PresenterHasFocus(); }
void regpu::LogState() { GpuLogState(); }
void regpu::LogCrashContext() { RendererLogCrashContext(); }
