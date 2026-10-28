// CoSync-SF SFSE plugin entry point (CommonLibSF).
//
// Load: read config, register for SFSE messages and a per-frame main-thread
// task. After game data has loaded, start the network worker. Engine access
// (reading the local player) happens only in GameBridge::Tick on the main
// thread, and only when the Address Library database is installed.

#include "GameBridge.h"
#include "PluginConfig.h"

#include "cosync/net/NetworkWorker.h"
#include "cosync/shared/GameRuntime.h"
#include "cosync/shared/Log.h"

#include <SFSE/SFSE.h>

#include <chrono>
#include <fstream>
#include <memory>

using namespace cosync;

static_assert(SFSE::RUNTIME_SF_1_16_244.pack() == game::kTargetRuntime,
              "CommonLibSF and CoSync-SF disagree on the target runtime packing");

namespace
{
    // SFSE message ids beyond the four CommonLibSF names (sfse/PluginAPI.h).
    constexpr std::uint32_t kMessage_PreLoadGame = 6;
    constexpr std::uint32_t kMessage_PostLoadGame = 7;

    std::uint32_t g_runtimeVersion = 0;
    plugin::Config g_config;

    // Intentionally never destroyed: SFSE does not unload plugins, and joining a
    // thread during DLL_PROCESS_DETACH would deadlock on the loader lock. On game
    // exit the OS ends the thread; the server sees a timeout and frees the slot.
    net::NetworkWorker* g_worker = nullptr;

    std::ofstream g_logFile;

    void OpenLog()
    {
        std::error_code ec;
        const auto dir = plugin::LogDirectory();
        std::filesystem::create_directories(dir, ec);
        g_logFile.open(dir / plugin::LogFileName(), std::ios::out | std::ios::trunc);
        log::SetSink([](log::Level level, std::string_view message) {
            if (!g_logFile) return;
            const auto now = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
            g_logFile << std::format("{:%H:%M:%S} [{}] {}\n", now, log::ToString(level), message);
            g_logFile.flush();
        });
    }

    void StartNetworking()
    {
        if (g_config.mode == plugin::Mode::Off) {
            log::Info("networking disabled (Mode=off)");
            return;
        }
        if (g_worker) {
            return;
        }

        net::WorkerConfig wc;
        wc.client.playerName = g_config.playerName;
        wc.client.password = g_config.password;
        wc.client.gameRuntime = g_runtimeVersion;
        wc.client.clientBuild = "CoSyncSF.dll/0.2.0";

        if (g_config.mode == plugin::Mode::Host) {
            wc.mode = net::WorkerMode::Host;
            wc.listenPort = g_config.port;
            wc.discoveryPort = g_config.discoveryPort;
            wc.server.serverName = g_config.serverName;
            wc.server.maxPlayers = g_config.maxPlayers;
            wc.server.password = g_config.password;
            wc.server.requiredGameRuntime = g_runtimeVersion;  // everyone must run the same game build
        } else {
            const auto endpoint = net::Endpoint::Parse(g_config.address, 37015);
            if (!endpoint) {
                log::Error("invalid Address '{}' in config; networking not started", g_config.address);
                return;
            }
            wc.mode = net::WorkerMode::Client;
            wc.connectTo = *endpoint;
        }

        auto worker = std::make_unique<net::NetworkWorker>(std::move(wc));
        std::string error;
        if (!worker->Start(error)) {
            log::Error("networking failed to start: {}", error);
            return;
        }
        g_worker = worker.release();
        plugin::GameBridge::Get().Attach(g_worker);
        log::Info("networking started in {} mode", g_config.mode == plugin::Mode::Host ? "host" : "client");
    }

    void OnSfseMessage(SFSE::MessagingInterface::Message* msg)
    {
        if (!msg) return;
        switch (msg->type) {
        case SFSE::MessagingInterface::kPostLoad:
            log::Debug("SFSE PostLoad");
            break;
        case SFSE::MessagingInterface::kPostDataLoad:
            log::Info("game data loaded");
            StartNetworking();
            break;
        case kMessage_PreLoadGame:
            log::Info("save game loading");
            break;
        case kMessage_PostLoadGame:
            log::Info("save game loaded");
            break;
        default:
            break;
        }
    }
}

SFSE_PLUGIN_VERSION = []() noexcept {
    SFSE::PluginVersionData v{};
    v.PluginVersion({ 0, 2, 0, 0 });
    v.PluginName("CoSyncSF");
    v.AuthorName("CoSync-SF");
    v.UsesAddressLibrary(true);
    v.IsLayoutDependent(true);
    v.CompatibleVersions({ SFSE::RUNTIME_SF_1_16_244 });
    return v;
}();

SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* a_sfse)
{
    // CoSync keeps its own log; no CommonLibSF hooks or trampoline are used.
    SFSE::Init(a_sfse, { .log = false, .hook = false });

    OpenLog();
    g_runtimeVersion = a_sfse->RuntimeVersion().pack();
    log::Info("CoSyncSF 0.2.0 loading (Starfield {}, SFSE {:08x}, protocol {}.{})",
              game::FormatRuntimeVersion(g_runtimeVersion), SFSE::GetSFSEVersion(), proto::kProtocolMajor,
              proto::kProtocolMinor);
    if (g_runtimeVersion != game::kTargetRuntime) {
        log::Warn("built for Starfield {}, running {}", game::FormatRuntimeVersion(game::kTargetRuntime),
                  game::FormatRuntimeVersion(g_runtimeVersion));
    }

    std::string warnings;
    const auto iniPath = plugin::ConfigPath();
    if (!std::filesystem::exists(iniPath)) {
        log::Warn("config file not found: {} (using defaults: networking off)", iniPath.string());
    }
    g_config = plugin::LoadConfig(iniPath, warnings);
    log::SetMinLevel(g_config.logLevel);
    log::Info("config: {}", iniPath.string());
    if (!warnings.empty()) {
        log::Warn("config: {}", warnings);
    }

    // CommonLibSF terminates the game if an Address Library ID is used without
    // the database, so check first and degrade to network-only if it is missing.
    const auto addressLibrary = plugin::AddressLibraryPath(g_runtimeVersion);
    const bool haveAddressLibrary = std::filesystem::exists(addressLibrary);
    if (haveAddressLibrary) {
        log::Info("Address Library found: {}", addressLibrary.filename().string());
    } else {
        log::Error("Address Library not found ({}); player position sync disabled. "
                   "Install 'Address Library for SFSE Plugins' for this game version.",
                   addressLibrary.string());
    }
    plugin::GameBridge::Get().EnableLocalPlayerReads(haveAddressLibrary);

    const auto* messaging = SFSE::GetMessagingInterface();
    const auto* tasks = SFSE::GetTaskInterface();
    if (!messaging || !tasks) {
        log::Error("required SFSE interfaces unavailable (messaging={}, task={})", messaging != nullptr,
                   tasks != nullptr);
        return false;
    }
    if (!messaging->RegisterListener(OnSfseMessage)) {
        log::Error("could not register for SFSE messages");
        return false;
    }
    tasks->AddPermanentTask([] { plugin::GameBridge::Get().Tick(); });
    log::Info("loaded; waiting for game data");
    return true;
}
