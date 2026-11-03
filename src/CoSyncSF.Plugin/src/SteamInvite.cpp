#include "SteamInvite.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstring>
#include <cstdio>
#include <algorithm>
#include <format>
#include <mutex>
#include <string_view>
#include <utility>

namespace cosync::plugin::steam
{
    namespace
    {
        // Flat API exports present in Starfield 1.16.244's steam_api64.dll.
        using GetFriends_t = void* (*)();
        using InviteDialog_t = void (*)(void* friends, const char* connect);
        using SetRichPresence_t = bool (*)(void* friends, const char* key, const char* value);

        struct Api
        {
            GetFriends_t friends = nullptr;
            InviteDialog_t invite = nullptr;
            SetRichPresence_t richPresence = nullptr;
        };

        const Api& Resolve()
        {
            static const Api api = [] {
                Api a;
                HMODULE dll = GetModuleHandleW(L"steam_api64.dll");
                if (!dll) return a;
                a.friends = reinterpret_cast<GetFriends_t>(GetProcAddress(dll, "SteamAPI_SteamFriends_v017"));
                a.invite = reinterpret_cast<InviteDialog_t>(
                    GetProcAddress(dll, "SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialogConnectString"));
                a.richPresence = reinterpret_cast<SetRichPresence_t>(
                    GetProcAddress(dll, "SteamAPI_ISteamFriends_SetRichPresence"));
                return a;
            }();
            return api;
        }
    }

    namespace
    {
        std::uint32_t g_fakeId = 0;
        std::uint64_t g_fakeSeen = 0;  // newest fake invite sequence already handled
        std::string FakeInviteFile()
        {
            char tmp[MAX_PATH] = {};
            GetTempPathA(MAX_PATH, tmp);
            const std::string dir = std::string(tmp) + "CoSyncFakeSteam";
            CreateDirectoryA(dir.c_str(), nullptr);
            return dir + "\\invite.txt";
        }
        // File: "<sequence> <senderId> <targetId or 0 = everyone> <connect string>".
        bool ReadFakeInvite(std::uint64_t& seq, std::uint32_t& sender, std::uint32_t& target, std::string& connect)
        {
            FILE* f = nullptr;
            if (fopen_s(&f, FakeInviteFile().c_str(), "rb") != 0 || !f) return false;
            char buf[512] = {};
            const auto n = fread(buf, 1, sizeof(buf) - 1, f);
            fclose(f);
            buf[n] = 0;
            unsigned long long s = 0;
            unsigned snd = 0, tgt = 0;
            int used = 0;
            if (sscanf_s(buf, "%llu %u %u %n", &s, &snd, &tgt, &used) < 3) return false;
            seq = s;
            sender = snd;
            target = tgt;
            connect = std::string(buf + used);
            return true;
        }

        std::mutex g_joinLock;
        std::string g_pendingJoin;

        // "+cosync_join <code>" -> "<code>" ("" if absent).
        std::string ParseConnect(const char* text)
        {
            constexpr std::string_view kKey = "+cosync_join ";
            const std::string_view s(text ? text : "");
            const auto at = s.find(kKey);
            if (at == std::string_view::npos) return {};
            auto rest = s.substr(at + kKey.size());
            rest = rest.substr(0, rest.find_first_of(" \t\"\r\n"));
            return std::string(rest);
        }

        void SetPending(std::string code)
        {
            if (code.empty()) return;
            std::lock_guard lock(g_joinLock);
            g_pendingJoin = std::move(code);
        }

        // Same declaration order as Valve's CCallbackBase (steam_api_common.h), so MSVC produces
        // the vtable layout steam_api64.dll calls into: Run(void*), Run(void*, bool, u64),
        // GetCallbackSizeBytes(); then u8 flags, int callback id.
        class JoinRequestedCallback
        {
        public:
            virtual void Run(void* param)
            {
                // GameRichPresenceJoinRequested_t: CSteamID friend (8 bytes), char connect[256].
                const char* connect = static_cast<const char*>(param) + 8;
                char copy[257] = {};
                std::memcpy(copy, connect, 256);
                SetPending(ParseConnect(copy));
            }
            virtual void Run(void* param, bool, std::uint64_t) { Run(param); }
            virtual int GetCallbackSizeBytes() { return 8 + 256; }

            std::uint8_t callbackFlags = 0;
            int callbackId = 337;  // k_iSteamFriendsCallbacks (300) + 37
        };
        JoinRequestedCallback g_joinCallback;
        bool g_listening = false;
    }

    std::uint64_t LocalSteamId()
    {
        if (g_fakeId) return g_fakeId;
        HMODULE dll = GetModuleHandleW(L"steam_api64.dll");
        if (!dll) return 0;
        using GetUser_t = void* (*)();
        using GetId_t = std::uint64_t (*)(void*);
        auto getUser = reinterpret_cast<GetUser_t>(GetProcAddress(dll, "SteamAPI_SteamUser_v021"));
        auto getId = reinterpret_cast<GetId_t>(GetProcAddress(dll, "SteamAPI_ISteamUser_GetSteamID"));
        void* user = getUser ? getUser() : nullptr;
        return user && getId ? getId(user) : 0;
    }

    std::string SteamJoinCode()
    {
        const auto id = LocalSteamId();
        return id ? std::format("steam:{}", id) : std::string();
    }

    void InstallInviteListener()
    {
        if (g_listening) return;
        SetPending(ParseConnect(GetCommandLineA()));  // launched from an invite
        if (g_fakeId) {
            g_listening = true;  // fake: TakePendingJoin polls the invite file
            return;
        }
        HMODULE dll = GetModuleHandleW(L"steam_api64.dll");
        using Register_t = void (*)(void*, int);
        auto reg = dll ? reinterpret_cast<Register_t>(GetProcAddress(dll, "SteamAPI_RegisterCallback")) : nullptr;
        if (!reg) return;
        reg(&g_joinCallback, g_joinCallback.callbackId);
        g_listening = true;
    }

    std::string TakePendingJoin()
    {
        if (g_fakeId) {
            static DWORD next = 0;
            if (GetTickCount() >= next) {
                next = GetTickCount() + 500;
                std::uint64_t seq = 0;
                std::uint32_t sender = 0, target = 0;
                std::string connect;
                if (ReadFakeInvite(seq, sender, target, connect) && seq > g_fakeSeen) {
                    g_fakeSeen = seq;
                    if (sender != g_fakeId && (target == 0 || target == g_fakeId)) SetPending(ParseConnect(connect.c_str()));
                }
            }
        }
        std::lock_guard lock(g_joinLock);
        return std::exchange(g_pendingJoin, {});
    }

    void SetFake(std::uint32_t id)
    {
        g_fakeId = id;
        std::uint64_t seq = 0;
        std::uint32_t sender = 0, target = 0;
        std::string connect;
        if (ReadFakeInvite(seq, sender, target, connect)) g_fakeSeen = seq;  // ignore invites from earlier runs
    }

    bool IsFake() { return g_fakeId != 0; }

    bool Available()
    {
        if (g_fakeId) return true;
        const auto& a = Resolve();
        return a.friends && a.invite && a.friends();
    }

    bool OpenInviteDialog(const std::string& connectString)
    {
        if (g_fakeId) {  // fake: every other copy on this PC receives the invite
            std::uint64_t seq = 0;
            std::uint32_t sender = 0, target = 0;
            std::string old;
            ReadFakeInvite(seq, sender, target, old);
            seq = std::max(seq, g_fakeSeen) + 1;
            g_fakeSeen = seq;  // never accept our own invite
            FILE* f = nullptr;
            if (fopen_s(&f, FakeInviteFile().c_str(), "wb") != 0 || !f) return false;
            fprintf(f, "%llu %u 0 %s", static_cast<unsigned long long>(seq), g_fakeId, connectString.c_str());
            fclose(f);
            return true;
        }
        const auto& a = Resolve();
        void* friends = a.friends ? a.friends() : nullptr;
        if (!friends || !a.invite) return false;
        a.invite(friends, connectString.c_str());
        return true;
    }

    bool SetJoinPresence(const std::string& connectString)
    {
        if (g_fakeId) return true;
        const auto& a = Resolve();
        void* friends = a.friends ? a.friends() : nullptr;
        if (!friends || !a.richPresence) return false;
        return a.richPresence(friends, "connect", connectString.empty() ? nullptr : connectString.c_str());
    }

    std::string LocalIPv4()
    {
        // GameNetworkingSockets has already initialised Winsock in this process; WSAStartup is
        // reference-counted, so pairing it here is harmless either way.
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return {};
        std::string result;
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s != INVALID_SOCKET) {
            sockaddr_in remote{};
            remote.sin_family = AF_INET;
            remote.sin_port = htons(53);
            inet_pton(AF_INET, "8.8.8.8", &remote.sin_addr);  // routing lookup only; UDP connect sends nothing
            if (connect(s, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) == 0) {
                sockaddr_in local{};
                int len = sizeof(local);
                if (getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0) {
                    char buf[INET_ADDRSTRLEN] = {};
                    if (inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf))) result = buf;
                }
            }
            closesocket(s);
        }
        WSACleanup();
        return result;
    }

    bool CopyToClipboard(const std::string& text)
    {
        if (!OpenClipboard(nullptr)) return false;
        EmptyClipboard();
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
        bool ok = false;
        if (mem) {
            std::memcpy(GlobalLock(mem), text.c_str(), text.size() + 1);
            GlobalUnlock(mem);
            ok = SetClipboardData(CF_TEXT, mem) != nullptr;
            if (!ok) GlobalFree(mem);
        }
        CloseClipboard();
        return ok;
    }
}
