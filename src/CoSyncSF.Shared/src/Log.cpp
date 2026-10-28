#include "cosync/shared/Log.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>

namespace cosync::log
{
    namespace
    {
        std::mutex g_mutex;
        Sink g_sink;
        std::atomic<Level> g_minLevel{ Level::Info };

        void DefaultSink(Level level, std::string_view message)
        {
            const auto now = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
            const auto line = std::format("{:%H:%M:%S} [{}] {}\n", now, ToString(level), message);
            std::fwrite(line.data(), 1, line.size(), stderr);
            std::fflush(stderr);
        }
    }

    void SetSink(Sink sink)
    {
        std::scoped_lock lock(g_mutex);
        g_sink = std::move(sink);
    }

    void SetMinLevel(Level level) { g_minLevel.store(level, std::memory_order_relaxed); }
    Level MinLevel() { return g_minLevel.load(std::memory_order_relaxed); }

    void Write(Level level, std::string_view message)
    {
        if (level < MinLevel()) {
            return;
        }
        std::scoped_lock lock(g_mutex);
        if (g_sink) {
            g_sink(level, message);
        } else {
            DefaultSink(level, message);
        }
    }

    std::string_view ToString(Level level) noexcept
    {
        switch (level) {
        case Level::Debug: return "debug";
        case Level::Info: return "info";
        case Level::Warn: return "warn";
        case Level::Error: return "error";
        }
        return "?";
    }
}
