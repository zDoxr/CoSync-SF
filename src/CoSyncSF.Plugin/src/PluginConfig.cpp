#include "PluginConfig.h"

#include "cosync/shared/GameRuntime.h"
#include "cosync/shared/Protocol.h"

#include <Windows.h>
#include <KnownFolders.h>
#include <ShlObj.h>

#include <algorithm>
#include <charconv>
#include <cwctype>
#include <format>
#include <string>

namespace cosync::plugin
{
    namespace
    {
        std::string ToUtf8(std::wstring_view w)
        {
            if (w.empty()) return {};
            const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
            std::string out(static_cast<std::size_t>(n), '\0');
            WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
            return out;
        }

        std::string ReadString(const std::filesystem::path& ini, const wchar_t* section, const wchar_t* key, const std::string& fallback)
        {
            wchar_t buf[512]{};
            constexpr wchar_t kMissing[] = L"\x1F__missing__";
            GetPrivateProfileStringW(section, key, kMissing, buf, static_cast<DWORD>(std::size(buf)), ini.c_str());
            if (std::wstring_view(buf) == kMissing) {
                return fallback;
            }
            return ToUtf8(buf);
        }

        bool ParsePort(const std::string& s, std::uint16_t& out, bool allowZero)
        {
            unsigned v = 0;
            auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
            if (ec != std::errc{} || p != s.data() + s.size() || v > 65535 || (!allowZero && v == 0)) return false;
            out = static_cast<std::uint16_t>(v);
            return true;
        }
    }

    Config LoadConfig(const std::filesystem::path& iniPath, std::string& warnings)
    {
        Config c;
        auto warn = [&](std::string msg) { warnings += msg + "\n"; };

        const auto mode = ReadString(iniPath, L"Network", L"Mode", "off");
        if (mode == "host") c.mode = Mode::Host;
        else if (mode == "client") c.mode = Mode::Client;
        else if (mode != "off") warn(std::format("unknown Mode '{}', using off", mode));

        c.address = ReadString(iniPath, L"Network", L"Address", c.address);
        if (const auto v = ReadString(iniPath, L"Network", L"Port", "37015"); !ParsePort(v, c.port, false)) {
            warn(std::format("invalid Port '{}'", v));
        }
        if (const auto v = ReadString(iniPath, L"Network", L"DiscoveryPort", "37016"); !ParsePort(v, c.discoveryPort, true)) {
            warn(std::format("invalid DiscoveryPort '{}'", v));
        }
        c.serverName = ReadString(iniPath, L"Network", L"ServerName", c.serverName);
        if (c.serverName.empty() || !proto::IsValidText(c.serverName, proto::kMaxServerNameBytes)) {
            warn("invalid ServerName, using default");
            c.serverName = Config{}.serverName;
        }
        {
            std::uint16_t maxPlayers = 0;
            const auto v = ReadString(iniPath, L"Network", L"MaxPlayers", "8");
            if (ParsePort(v, maxPlayers, false) && maxPlayers <= proto::kMaxPlayersLimit) c.maxPlayers = maxPlayers;
            else warn(std::format("invalid MaxPlayers '{}'", v));
        }

        c.playerName = ReadString(iniPath, L"Player", L"Name", c.playerName);
        if (!proto::IsValidPlayerName(c.playerName)) {
            warn(std::format("invalid player Name '{}', using 'Player'", c.playerName));
            c.playerName = "Player";
        }
        c.password = ReadString(iniPath, L"Security", L"Password", "");
        if (c.password.size() > proto::kMaxPasswordBytes) {
            warn("Password longer than 64 bytes; truncated");
            c.password.resize(proto::kMaxPasswordBytes);
        }

        c.remoteAvatars = ReadString(iniPath, L"Experimental", L"RemoteAvatars", "0") == "1";
        {
            const auto freeze = ReadString(iniPath, L"Experimental", L"AvatarFreezeAI", "0");
            c.avatarFreezeMode = freeze == "1" ? 1 : freeze == "2" ? 2 : 0;
        }
        try {
            c.avatarFreezeDelay = std::clamp(std::stof(ReadString(iniPath, L"Experimental", L"AvatarFreezeDelay", "3")), 0.0f, 60.0f);
        } catch (...) {
            c.avatarFreezeDelay = 3.0f;
        }
        c.avatarNearbyBase = ReadString(iniPath, L"Experimental", L"AvatarBase", "player") == "nearby";
        {
            const auto diag = ReadString(iniPath, L"Experimental", L"AvatarAnimDiag", "0");
            c.avatarAnimDiag = diag == "2" ? 2 : diag == "1" ? 1 : 0;
        }
        c.avatarAnimate = ReadString(iniPath, L"Experimental", L"AvatarAnimate", "0") == "1";
        c.avatarMove = ReadString(iniPath, L"Experimental", L"AvatarMove", "1") == "1";
        {
            const auto moveMode = ReadString(iniPath, L"Experimental", L"AvatarMoveMode", "teleport");
            c.avatarPath = moveMode == "path";
            c.avatarMotion = moveMode == "motion";
            c.avatarMirror = moveMode == "mirror";
            c.avatarControls = moveMode == "controls";
            if (moveMode != "teleport" && moveMode != "path" && moveMode != "motion" && moveMode != "mirror" &&
                moveMode != "controls") {
                warn(std::format("unknown AvatarMoveMode '{}' (teleport | path | motion | mirror | controls)",
                                 moveMode));
            }
        }
        try {
            c.avatarScale = std::clamp(std::stof(ReadString(iniPath, L"Experimental", L"AvatarScale", "1.0")), 0.1f, 10.0f);
        } catch (...) {
            c.avatarScale = 1.0f;
        }

        const auto level = ReadString(iniPath, L"Debug", L"LogLevel", "info");
        if (level == "debug") c.logLevel = log::Level::Debug;
        else if (level == "warn") c.logLevel = log::Level::Warn;
        else if (level == "error") c.logLevel = log::Level::Error;
        else if (level != "info") warn(std::format("unknown LogLevel '{}'", level));
        return c;
    }

    namespace
    {
        std::wstring GetEnv(const wchar_t* name)
        {
            const DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
            if (n == 0) return {};
            std::wstring value(n, wchar_t{});
            const DWORD written = GetEnvironmentVariableW(name, value.data(), n);
            value.resize(written);
            return value;
        }
    }

    std::filesystem::path ConfigPath()
    {
        if (auto custom = GetEnv(L"COSYNC_INI"); !custom.empty()) {
            return std::filesystem::path(custom);
        }
        return ThisModuleDirectory() / L"CoSyncSF.ini";
    }

    std::wstring LogFileName()
    {
        auto label = GetEnv(L"COSYNC_INSTANCE");
        // Keep it a safe file-name fragment.
        std::erase_if(label, [](wchar_t c) { return !(iswalnum(c) || c == L'-' || c == L'_'); });
        if (label.size() > 32) label.resize(32);
        return label.empty() ? L"CoSyncSF.log" : L"CoSyncSF-" + label + L".log";
    }

    std::filesystem::path ThisModuleDirectory()
    {
        HMODULE module = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&ThisModuleDirectory), &module);
        std::wstring path(MAX_PATH, L'\0');
        for (;;) {
            const DWORD n = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
            if (n < path.size()) {
                path.resize(n);
                break;
            }
            path.resize(path.size() * 2);
        }
        return std::filesystem::path(path).parent_path();
    }

    std::filesystem::path AddressLibraryPath(std::uint32_t packedRuntime)
    {
        const auto name = std::format(L"versionlib-{}-{}-{}-0.bin", game::RuntimeMajor(packedRuntime),
                                      game::RuntimeMinor(packedRuntime), game::RuntimeBuild(packedRuntime));
        return ThisModuleDirectory() / name;
    }

    std::filesystem::path LogDirectory()
    {
        PWSTR docs = nullptr;
        std::filesystem::path dir;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &docs))) {
            dir = std::filesystem::path(docs) / L"My Games" / L"Starfield" / L"SFSE" / L"Logs";
        }
        CoTaskMemFree(docs);
        if (dir.empty()) {
            dir = ThisModuleDirectory();
        }
        return dir;
    }
}
