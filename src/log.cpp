#include "log.h"

#include "regpu/regpu.h"

#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
std::mutex g_logMutex;
std::condition_variable g_logCv;
std::deque<std::string> g_queue; // lines waiting for the writer thread
bool g_writerStarted = false;
std::mutex g_fileMutex;       // g_logFile, g_fileReady and g_early
FILE* g_logFile = nullptr;
bool g_fileReady = false;     // LogStart has run: Config::userDir is final
std::deque<std::string> g_early; // lines from before then, written to the file when it opens
const auto g_start = std::chrono::steady_clock::now();
std::deque<std::string> g_recent; // the last lines, for the debug server's log command

// regpu.log goes in Config::userDir, or in REGPU_LOG_DIR (FM4 port: a slow or removable drive stalled
// game threads on every write). With g_fileMutex held.
FILE* OpenLogFile()
{
    if (const char* dir = getenv("REGPU_LOG_DIR"); dir && *dir)
        return fopen((std::string(dir) + "\\regpu.log").c_str(), "w");
    const std::filesystem::path& dir = regpu::GetConfig().userDir;
    std::error_code ec;
    if (!dir.empty())
        std::filesystem::create_directories(dir, ec);
    return _wfopen((dir / "regpu.log").c_str(), L"w");
}

void WriteLines(const std::deque<std::string>& lines)
{
    std::lock_guard lock(g_fileMutex);
    for (const std::string& line : lines)
    {
        fputs(line.c_str(), stderr);
        if (g_logFile)
            fputs(line.c_str(), g_logFile);
        else if (!g_fileReady && g_early.size() < 5000)
            g_early.push_back(line);
    }
    fflush(stderr);
    if (g_logFile)
        fflush(g_logFile);
}
}

void LogStart()
{
    std::lock_guard lock(g_fileMutex);
    if (g_fileReady)
        return;
    g_fileReady = true;
    g_logFile = OpenLogFile();
    if (g_logFile)
    {
        for (const std::string& line : g_early)
            fputs(line.c_str(), g_logFile);
        fflush(g_logFile);
    }
    g_early.clear();
}

namespace
{
// The writer thread: lines are written off the calling thread, so a stalling drive doesn't hold up the
// renderer or the game.
void WriterLoop()
{
    std::deque<std::string> batch;
    for (;;)
    {
        {
            std::unique_lock lock(g_logMutex);
            g_logCv.wait(lock, [] { return !g_queue.empty(); });
            batch.swap(g_queue);
        }
        WriteLines(batch);
        batch.clear();
    }
}

void Write(const char* prefix, const char* fmt, va_list args)
{
    char buffer[2048];
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();

    char line[2120];
    snprintf(line, sizeof(line), "[%9.4f] %s%s\n", t, prefix, buffer);
    // REGPU_LOG_ASYNC=1: the writer thread (opt-in: a process ending through ExitProcess loses what is still
    // queued unless it calls LogFlush first).
    static const bool sync = getenv("REGPU_LOG_ASYNC") == nullptr;
    std::lock_guard lock(g_logMutex);
    g_recent.emplace_back(line, strlen(line) - 1);
    if (g_recent.size() > 2000)
        g_recent.pop_front();
    if (sync)
    {
        WriteLines({ line });
        return;
    }
    if (!g_writerStarted)
    {
        g_writerStarted = true;
        std::thread(WriterLoop).detach();
    }
    if (g_queue.size() < 200000)
        g_queue.emplace_back(line);
    g_logCv.notify_one();
}
}

void Log(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    Write("", fmt, args);
    va_end(args);
}

void LogFlush()
{
    for (int i = 0; i < 40; i++)
    {
        {
            std::lock_guard lock(g_logMutex);
            if (g_queue.empty() || !g_writerStarted)
                break;
        }
        Sleep(50);
    }
    Sleep(50); // (the batch the writer took may still be in flight)
}

void Fatal(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    Write("FATAL: ", fmt, args);
    va_end(args);
    LogFlush();
    std::_Exit(1);
}

std::vector<std::string> LogTail(size_t n)
{
    std::lock_guard lock(g_logMutex);
    const size_t from = g_recent.size() > n ? g_recent.size() - n : 0;
    return std::vector<std::string>(g_recent.begin() + ptrdiff_t(from), g_recent.end());
}
