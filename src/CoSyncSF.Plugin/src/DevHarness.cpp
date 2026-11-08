#include "DevHarness.h"

#include "cosync/shared/Log.h"

#include <RE/Starfield.h>
#include <REL/Relocation.h>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

namespace cosync::plugin::dev
{
    namespace
    {
        using Clock = std::chrono::steady_clock;
        using namespace std::chrono_literals;

        constexpr std::uint64_t kConsoleExecuteId = 113576;  // 0x1EA1E10 (runner, format, ...)
        constexpr std::uint64_t kConsoleGlobalId = 938528;   // 0x61E26B0
        // Global event getters: an unbroken block of one shape, 0x70 bytes apart (RE 12.11).
        constexpr std::uint64_t kFirstGetterId = 64095;
        constexpr std::uint64_t kLastGetterId = 64237;
        constexpr std::uint64_t kNoAddressId = 64114;  // the address library has no entry
        constexpr std::array<std::uint8_t, 4> kGetterPrologue{ 0x48, 0x83, 0xEC, 0x28 };  // sub rsp, 28h
        constexpr std::size_t kSampleBytes = 192;
        constexpr std::uint32_t kSamplesPerSource = 4;
        constexpr auto kMenuSettle = 8s;
        constexpr auto kCommandGap = 12s;
        constexpr auto kCommandWindow = 8s;
        constexpr auto kSummaryEvery = 30s;

        struct Opaque
        {
            std::byte data[kSampleBytes];
        };

        bool Copy(const void* from, void* to, std::size_t size) noexcept
        {
            __try {
                std::memcpy(to, from, size);
                return true;
            } __except (1) {
                return false;
            }
        }

        // What a payload word points at, when it is a form: read here, while the event is current.
        struct FormRead
        {
            std::uint32_t id = 0;
            std::uint8_t type = 0;
        };

        struct Sample
        {
            std::uint64_t id = 0;
            std::uint32_t thread = 0;
            std::array<std::uint64_t, kSampleBytes / 8> words{};
            std::array<FormRead, kSampleBytes / 8> forms{};
        };

        std::uintptr_t g_imageBase = 0;
        std::uintptr_t g_imageEnd = 0;

        bool ReadForm(std::uint64_t word, FormRead& out) noexcept
        {
            if (word < 0x10000 || word > 0x7FFFFFFFFFFF || (word & 7) != 0) return false;
            __try {
                const auto vtable = *reinterpret_cast<const std::uintptr_t*>(word);
                if (vtable < g_imageBase || vtable >= g_imageEnd) return false;
                out.id = *reinterpret_cast<const std::uint32_t*>(word + 0x28);
                out.type = *reinterpret_cast<const std::uint8_t*>(word + 0x2E);
                return out.id != 0;
            } __except (1) {
                return false;
            }
        }

        std::mutex g_sampleLock;
        std::vector<Sample> g_samples;

        // Every source has the one layout (RE 12.11), so a sink need not know its event type. Only
        // the payload's bytes are read, and only as numbers.
        class CountingSink final : public RE::BSTEventSink<Opaque>
        {
        public:
            explicit CountingSink(std::uint64_t id) noexcept : id(id) {}

            RE::BSEventNotifyControl ProcessEvent(const Opaque& event, RE::BSTEventSource<Opaque>*) override
            {
                count.fetch_add(1, std::memory_order_relaxed);
                if (sampled.fetch_add(1, std::memory_order_relaxed) < kSamplesPerSource) {
                    Sample s{ id, GetCurrentThreadId() };
                    if (Copy(&event, s.words.data(), sizeof(s.words))) {
                        for (std::size_t i = 0; i < s.words.size(); ++i) ReadForm(s.words[i], s.forms[i]);
                        std::scoped_lock lock(g_sampleLock);
                        g_samples.push_back(s);
                    }
                }
                return RE::BSEventNotifyControl::kContinue;
            }

            std::uint64_t id;
            std::atomic<std::uint64_t> count{ 0 };
            std::atomic<std::uint32_t> sampled{ 0 };
            std::uint64_t reported = 0;
            std::uint64_t atCommand = 0;
        };

        Options g_options;
        std::function<bool(const std::string&)> g_handler;
        std::vector<std::unique_ptr<CountingSink>> g_sinks;
        bool g_censusInstalled = false;
        bool g_loadSent = false;
        std::optional<Clock::time_point> g_menuSince;
        std::optional<Clock::time_point> g_nextCommand;
        std::size_t g_command = 0;
        Clock::time_point g_nextSummary{};
        std::optional<Clock::time_point> g_commandReport;
        std::string g_lastCommand;

        // Marks where a scripted command begins, so what fires next can be told from the noise.
        void MarkCommand(const std::string& command)
        {
            g_lastCommand = command;
            g_commandReport = Clock::now() + kCommandWindow;
            for (auto& sink : g_sinks) {
                sink->atCommand = sink->count.load(std::memory_order_relaxed);
                sink->sampled.store(0, std::memory_order_relaxed);
            }
        }

        void ReportCommand()
        {
            std::string line;
            for (auto& sink : g_sinks) {
                const auto n = sink->count.load(std::memory_order_relaxed);
                if (n != sink->atCommand) line += std::format(" {}:+{}", sink->id, n - sink->atCommand);
            }
            log::Info("[census] after '{}':{}", g_lastCommand, line);
            g_commandReport.reset();
        }

        void InstallCensus()
        {
            g_censusInstalled = true;
            g_imageBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            g_imageEnd = g_imageBase + 0x8000000;
            std::size_t refused = 0;
            for (auto id = kFirstGetterId; id <= kLastGetterId; ++id) {
                if (id == kNoAddressId) continue;
                const auto address = REL::ID(id).address();
                if (std::memcmp(reinterpret_cast<const void*>(address), kGetterPrologue.data(), kGetterPrologue.size()) != 0) {
                    ++refused;
                    continue;
                }
                using Getter_t = RE::BSTEventSource<Opaque>* (*)();
                auto* source = reinterpret_cast<Getter_t>(address)();
                if (!source) {
                    ++refused;
                    continue;
                }
                auto sink = std::make_unique<CountingSink>(id);
                source->RegisterSink(sink.get());
                g_sinks.push_back(std::move(sink));
            }
            log::Info("[census] {} sinks installed, {} refused", g_sinks.size(), refused);
        }

        void ReportCensus(Clock::time_point now)
        {
            std::vector<Sample> samples;
            {
                std::scoped_lock lock(g_sampleLock);
                samples.swap(g_samples);
            }
            for (const auto& s : samples) {
                std::string words;
                for (std::size_t i = 0; i < s.words.size(); ++i) {
                    words += std::format(" {:016x}", s.words[i]);
                    if (s.forms[i].id != 0) words += std::format("[form {:08X} type {}]", s.forms[i].id, s.forms[i].type);
                    for (const auto half : { static_cast<std::uint32_t>(s.words[i]), static_cast<std::uint32_t>(s.words[i] >> 32) }) {
                        if (half == 0 || half == 0xFFFFFFFF || s.forms[i].id != 0) continue;
                        if (const auto* form = RE::TESForm::LookupByID(half)) {
                            words += std::format("[id {:08X} type {}]", half, static_cast<std::uint32_t>(form->GetFormType()));
                        }
                    }
                }
                log::Info("[census] sample id={} thread={}:{}", s.id, s.thread, words);
            }
            if (g_commandReport && now >= *g_commandReport) ReportCommand();
            if (now < g_nextSummary) return;
            g_nextSummary = now + kSummaryEvery;
            std::string line;
            for (auto& sink : g_sinks) {
                const auto n = sink->count.load(std::memory_order_relaxed);
                if (n == sink->reported) continue;
                line += std::format(" {}:+{}", sink->id, n - sink->reported);
                sink->reported = n;
            }
            if (!line.empty()) log::Info("[census] fired{}", line);
        }

        bool MainMenuOpen()
        {
            auto* ui = RE::UI::GetSingleton();
            return ui && ui->IsMenuOpen("MainMenu");
        }
    }

    void Configure(Options options) { g_options = std::move(options); }

    namespace
    {
        // '@name' commands are test steps the plugin carries out itself, anything else goes to the console.
        void RunCommand(const std::string& command)
        {
            if (command.starts_with('@')) {
                log::Info("[dev] step: {}", command);
                if (!g_handler || !g_handler(command)) log::Warn("[dev] step not handled: {}", command);
                return;
            }
            RunConsole(command);
        }
    }

    void RunConsole(const std::string& command)
    {
        using Execute_t = void (*)(void*, const char*, ...);
        static REL::Relocation<Execute_t> execute{ REL::ID(kConsoleExecuteId) };
        static REL::Relocation<void**> runner{ REL::ID(kConsoleGlobalId) };
        std::string escaped;
        for (const auto c : command) {
            escaped += c;
            if (c == '%') escaped += c;
        }
        log::Info("[dev] console: {}", command);
        auto* console = RE::ConsoleLog::GetSingleton();
        const std::string before = console ? std::string(console->buffer.c_str() ? console->buffer.c_str() : "") : std::string();
        execute(*runner, escaped.c_str());
        // What the command printed, so a refusal shows its reason in the log.
        if (console && console->buffer.c_str()) {
            std::string after = console->buffer.c_str();
            const auto added = after.size() > before.size() && after.starts_with(before) ? after.substr(before.size()) : after.substr(after.size() > 400 ? after.size() - 400 : 0);
            if (!added.empty() && after != before) {
                std::string line = added;
                std::ranges::replace(line, '\n', '|');
                log::Info("[dev] console said: {}", line);
            }
        }
    }

    void SetCommandHandler(std::function<bool(const std::string&)> handler) { g_handler = std::move(handler); }

    void Tick(bool inWorld)
    {
        const auto now = Clock::now();
        if (g_options.eventCensus) {
            if (!g_censusInstalled) InstallCensus();
            ReportCensus(now);
        }
        if (!g_options.autoLoad.empty() && !g_loadSent) {
            if (MainMenuOpen()) {
                if (!g_menuSince) g_menuSince = now;
                if (now - *g_menuSince >= kMenuSettle) {
                    g_loadSent = true;
                    RunConsole(std::format("LoadGame {}", g_options.autoLoad));
                }
            } else {
                g_menuSince.reset();
            }
        }
        if (g_command < g_options.commands.size() && inWorld) {
            if (!g_nextCommand) g_nextCommand = now + std::chrono::seconds(g_options.commandDelay);
            if (now >= *g_nextCommand) {
                MarkCommand(g_options.commands[g_command]);
                RunCommand(g_options.commands[g_command++]);
                g_nextCommand = now + kCommandGap;
            }
        }
    }
}
