#include "SteamInvite.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstring>

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

    bool Available()
    {
        const auto& a = Resolve();
        return a.friends && a.invite && a.friends();
    }

    bool OpenInviteDialog(const std::string& connectString)
    {
        const auto& a = Resolve();
        void* friends = a.friends ? a.friends() : nullptr;
        if (!friends || !a.invite) return false;
        a.invite(friends, connectString.c_str());
        return true;
    }

    bool SetJoinPresence(const std::string& connectString)
    {
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
