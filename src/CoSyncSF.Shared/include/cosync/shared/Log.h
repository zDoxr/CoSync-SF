#pragma once

#include <format>
#include <functional>
#include <string>
#include <string_view>

namespace cosync::log
{
    enum class Level
    {
        Debug,
        Info,
        Warn,
        Error,
    };

    using Sink = std::function<void(Level, std::string_view)>;

    // Replaces the process-wide sink (default: stderr). Thread-safe; the sink
    // itself is invoked under a lock, so it must not call back into logging.
    void SetSink(Sink sink);
    void SetMinLevel(Level level);
    [[nodiscard]] Level MinLevel();

    void Write(Level level, std::string_view message);
    [[nodiscard]] std::string_view ToString(Level level) noexcept;

    template <class... Args>
    void Debug(std::format_string<Args...> fmt, Args&&... args)
    {
        if (MinLevel() <= Level::Debug) Write(Level::Debug, std::format(fmt, std::forward<Args>(args)...));
    }
    template <class... Args>
    void Info(std::format_string<Args...> fmt, Args&&... args)
    {
        if (MinLevel() <= Level::Info) Write(Level::Info, std::format(fmt, std::forward<Args>(args)...));
    }
    template <class... Args>
    void Warn(std::format_string<Args...> fmt, Args&&... args)
    {
        if (MinLevel() <= Level::Warn) Write(Level::Warn, std::format(fmt, std::forward<Args>(args)...));
    }
    template <class... Args>
    void Error(std::format_string<Args...> fmt, Args&&... args)
    {
        Write(Level::Error, std::format(fmt, std::forward<Args>(args)...));
    }
}
