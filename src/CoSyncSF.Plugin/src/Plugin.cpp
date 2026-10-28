// CoSync-SF SFSE plugin entry point.
//
// Milestone 1 scope: load under SFSE, read CoSyncSF.ini, start the network
// worker after game data has loaded, and pump session events on the main
// thread via an SFSE permanent task. No game addresses, hooks or engine
// structures are used, which is what the version data below declares.

#include "GameBridge.h"
#include "PluginConfig.h"

#include "cosync/net/NetworkWorker.h"
#include "cosync/shared/GameRuntime.h"
#include "cosync/shared/Log.h"

#include <Windows.h>

#include <sfse/PluginAPI.h>

#include <chrono>
#include <fstream>
#include <memory>

using namespace cosync;

namespace
{
    PluginHandle g_handle = kPluginHandle_Invalid;
    SFSEMessagingInterface* g_messaging = nullptr;
    SFSETaskInterface* g_tasks = nullptr;
    std::uint32_t g_runtimeVersion = 0;
    plugin::Config g_config;

    // Intentionally never destroyed: SFSE does not unload plugins, and joining a
    // thread during DLL_PROCESS_DETACH would deadlock on the loader lock. On game
    // exit the OS ends the thread; the server sees a timeout and frees the slot.
    net::NetworkWorker* g_worker = nullptr;

    std::ofstream g_logFile;

    class MainThreadTask final : public SFSETaskInterface::ITaskDelegate
    {
    public:
        void Run() override { plugin::GameBridge::Get().Tick(); }
        void Destroy() override {}  // static lifetime
    };
    MainThreadTask g_mainThreadTask;

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
        if (g_worker || g_config.mode == plugin::Mode::Off) {
            if (g_config.mode == plugin::Mode::Off) {
                log::Info("networking disabled (Mode=off)");
            }
            return;
        }

        net::WorkerConfig wc;
        wc.client.playerName = g_config.playerName;
        wc.client.password = g_config.password;
        wc.client.gameRuntime = g_runtimeVersion;
        wc.client.clientBuild = std::format("CoSyncSF.dll/{}", "0.1.0");

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
                log::Error("invalid Address '{}' in CoSyncSF.ini; networking not started", g_config.address);
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

    void OnSfseMessage(SFSEMessagingInterface::Message* msg)
    {
        if (!msg) return;
        switch (msg->type) {
        case SFSEMessagingInterface::kMessage_PostLoad:
            log::Debug("SFSE PostLoad");
            break;
        case SFSEMessagingInterface::kMessage_PostDataLoad:
            log::Info("game data loaded");
            StartNetworking();
            break;
        case SFSEMessagingInterface::kMessage_PreLoadGame:
            log::Info("save game loading");
            break;
        case SFSEMessagingInterface::kMessage_PostLoadGame:
            log::Info("save game loaded");
            break;
        default:
            break;
        }
    }
}

extern "C"
{
    __declspec(dllexport) SFSEPluginVersionData SFSEPlugin_Version = {
        SFSEPluginVersionData::kVersion,
        1,  // plugin version
        "CoSyncSF",
        "CoSync-SF",
        0,  // no address independence claimed: we use no game addresses, so we pin the runtime list instead
        SFSEPluginVersionData::kStructureIndependence_NoStructs,
        { game::kTargetRuntime, 0 },
        0,  // any SFSE version
        0,
        0,
    };

    __declspec(dllexport) bool SFSEPlugin_Load(const SFSEInterface* sfse)
    {
        OpenLog();
        g_runtimeVersion = sfse->runtimeVersion;
        log::Info("CoSyncSF 0.1.0 loading (Starfield {}, SFSE {:08x}, protocol {}.{})",
                  game::FormatRuntimeVersion(g_runtimeVersion), sfse->sfseVersion, proto::kProtocolMajor,
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
        log::Info("config: {}", iniPath.string());
        log::SetMinLevel(g_config.logLevel);
        if (!warnings.empty()) {
            log::Warn("CoSyncSF.ini: {}", warnings);
        }

        g_handle = sfse->GetPluginHandle();
        g_messaging = static_cast<SFSEMessagingInterface*>(sfse->QueryInterface(kInterface_Messaging));
        g_tasks = static_cast<SFSETaskInterface*>(sfse->QueryInterface(kInterface_Task));
        if (!g_messaging || !g_tasks) {
            log::Error("required SFSE interfaces unavailable (messaging={}, task={})", g_messaging != nullptr,
                       g_tasks != nullptr);
            return false;
        }
        if (!g_messaging->RegisterListener(g_handle, "SFSE", OnSfseMessage)) {
            log::Error("could not register for SFSE messages");
            return false;
        }
        g_tasks->AddTaskPermanent(&g_mainThreadTask);
        log::Info("loaded; waiting for game data");
        return true;
    }
}
