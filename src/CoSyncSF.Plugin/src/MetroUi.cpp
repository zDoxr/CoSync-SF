#include "MetroUi.h"

#include "cosync/net/SessionClient.h"

#include <imgui.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <format>
#include <string>

namespace cosync::plugin::metroui
{
    namespace
    {
        // ---- Design tokens ------------------------------------------------------------------
        constexpr std::uint32_t kBg = 0x0B0E13, kSurface = 0x12161D, kSurface2 = 0x1A1F28, kStroke = 0x262C37;
        constexpr std::uint32_t kText = 0xEEF1F5, kDim = 0x7D8796, kFaint = 0x4A5260;
        constexpr float kRadius = 16.0f, kCard = 12.0f, kControl = 10.0f;

        // Segoe Fluent Icons (merged into the body font).
        constexpr const char* kIconCopy = "\xEE\xA3\x88";     // U+E8C8
        constexpr const char* kIconInvite = "\xEE\xA3\xBA";   // U+E8FA
        constexpr const char* kIconClose = "\xEE\x9C\x91";    // U+E711

        ImFont* g_heading = nullptr;
        ImFont* g_body = nullptr;
        ImFont* g_semibold = nullptr;
        ImFont* g_mono = nullptr;

        ImVec4 Col(std::uint32_t rgb, float a = 1.0f)
        {
            return { ((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a };
        }
        ImU32 U32(std::uint32_t rgb, float a = 1.0f) { return ImGui::GetColorU32(Col(rgb, a)); }

        void Font(ImFont* f, float size) { ImGui::PushFont(f, size); }
        void PopFont() { ImGui::PopFont(); }

        void Style(const ui::Theme& t)
        {
            auto& s = ImGui::GetStyle();
            s.WindowRounding = kRadius;
            s.ChildRounding = kCard;
            s.FrameRounding = s.PopupRounding = s.GrabRounding = kControl;
            s.WindowBorderSize = 1.0f;
            s.ChildBorderSize = 0.0f;
            s.FrameBorderSize = 0.0f;
            s.WindowPadding = { 28, 24 };
            s.FramePadding = { 14, 11 };
            s.ItemSpacing = { 12, 12 };
            s.GrabMinSize = 14;
            auto* c = s.Colors;
            c[ImGuiCol_WindowBg] = Col(kBg, 0.97f);
            c[ImGuiCol_ChildBg] = Col(kSurface);
            c[ImGuiCol_Border] = Col(kStroke);
            c[ImGuiCol_Text] = Col(kText);
            c[ImGuiCol_TextDisabled] = Col(kDim);
            c[ImGuiCol_FrameBg] = Col(kSurface2);
            c[ImGuiCol_FrameBgHovered] = Col(0x202634);
            c[ImGuiCol_FrameBgActive] = Col(0x232A39);
            c[ImGuiCol_SliderGrab] = Col(t.accent);
            c[ImGuiCol_SliderGrabActive] = Col(t.accent);
            c[ImGuiCol_TextSelectedBg] = Col(t.accent, 0.35f);
            c[ImGuiCol_NavHighlight] = Col(t.accent, 0.0f);
        }

        // ---- Controls ----------------------------------------------------------------------
        bool Button(const char* label, float width, std::uint32_t fill, std::uint32_t text, bool enabled = true,
                    float height = 46.0f)
        {
            ImGui::BeginDisabled(!enabled);
            ImGui::PushStyleColor(ImGuiCol_Button, Col(fill));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Col(fill, 0.88f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, Col(fill, 0.75f));
            ImGui::PushStyleColor(ImGuiCol_Text, Col(text));
            Font(g_semibold, 17);
            const bool clicked = ImGui::Button(label, { width, height });
            PopFont();
            ImGui::PopStyleColor(4);
            ImGui::EndDisabled();
            return clicked;
        }
        bool Primary(const char* label, float width, const ui::Theme& t, bool enabled = true)
        {
            return Button(label, width, t.accent, 0x111317, enabled);
        }
        bool Ghost(const char* label, float width, std::uint32_t text, float height = 46.0f)
        {
            return Button(label, width, kSurface2, text, true, height);
        }
        bool Icon(const char* id, const char* glyph, std::uint32_t text, float size = 38.0f)
        {
            ImGui::PushID(id);
            const bool clicked = Button(glyph, size, kSurface2, text, true, size);
            ImGui::PopID();
            return clicked;
        }

        void Field(const char* id, const char* hint, char* buf, std::size_t size, ImGuiInputTextFlags flags = 0)
        {
            ImGui::SetNextItemWidth(-1);
            Font(g_body, 17);
            ImGui::InputTextWithHint(id, hint, buf, size, flags);
            PopFont();
        }

        void Label(const char* text)
        {
            Font(g_semibold, 13);
            ImGui::PushStyleColor(ImGuiCol_Text, Col(kDim));
            ImGui::TextUnformatted(text);
            ImGui::PopStyleColor();
            PopFont();
        }

        // Rounded chip drawn inline.
        void Chip(const char* text, std::uint32_t colour)
        {
            Font(g_semibold, 12);
            const auto size = ImGui::CalcTextSize(text);
            const auto pos = ImGui::GetCursorScreenPos();
            const ImVec2 pad{ 8, 3 };
            auto* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(pos, { pos.x + size.x + pad.x * 2, pos.y + size.y + pad.y * 2 }, U32(colour, 0.16f), 999);
            dl->AddText({ pos.x + pad.x, pos.y + pad.y }, U32(colour), text);
            ImGui::Dummy({ size.x + pad.x * 2, size.y + pad.y * 2 });
            PopFont();
        }

        void Dot(ImVec2 centre, std::uint32_t colour, float r = 4.5f)
        {
            auto* dl = ImGui::GetWindowDrawList();
            dl->AddCircleFilled(centre, r * 2.2f, U32(colour, 0.18f));
            dl->AddCircleFilled(centre, r, U32(colour));
        }

        // ---- State ---------------------------------------------------------------------------
        struct Ui
        {
            char hostName[65] = "CoSync";
            int hostPort = 37015;
            int hostMax = 8;
            char hostPassword[65] = "";
            char joinCode[96] = "";
            char joinPassword[65] = "";
            int tab = 0;  // 0 Play, 1 Lobby
            std::string toast;
            std::chrono::steady_clock::time_point toastAt{};
            std::string lastSeenError;
        } u;

        void Toast(std::string text)
        {
            u.toast = std::move(text);
            u.toastAt = std::chrono::steady_clock::now();
        }

        void Emit(const char* intent, const ui::IntentArgs& args, const std::function<void(ui::UiCommand)>& emit)
        {
            std::string error;
            if (auto c = ui::ParseIntent(intent, args, error)) emit(std::move(*c));
            else Toast(error);
        }

        bool Connected(net::ClientState s)
        {
            return s == net::ClientState::InSession || s == net::ClientState::Connecting ||
                   s == net::ClientState::Handshaking || s == net::ClientState::Reconnecting;
        }

        // ---- Sections ------------------------------------------------------------------------
        void Header(const ui::UiSnapshot& s, const ui::Theme& t, bool& close)
        {
            Font(g_heading, 30);
            ImGui::TextUnformatted("COSYNC");
            PopFont();
            ImGui::SameLine(0, 10);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 9);
            Font(g_semibold, 13);
            ImGui::TextColored(Col(t.accent), "MULTIPLAYER");
            PopFont();

            // Status pill + close, right-aligned.
            const bool online = s.state == net::ClientState::InSession;
            const bool busy = Connected(s.state) && !online;
            std::string status = online ? std::format("Online  {} ms", s.rttMs) : busy ? "Connecting" : "Offline";
            Font(g_semibold, 14);
            const auto ts = ImGui::CalcTextSize(status.c_str());
            PopFont();
            const float pillW = ts.x + 40, right = ImGui::GetWindowContentRegionMax().x;
            ImGui::SameLine(right - pillW - 50);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 4);
            const auto p = ImGui::GetCursorScreenPos();
            auto* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p, { p.x + pillW, p.y + 34 }, U32(kSurface2), 999);
            Dot({ p.x + 18, p.y + 17 }, online ? t.good : busy ? t.warn : kFaint);
            Font(g_semibold, 14);
            dl->AddText({ p.x + 30, p.y + 17 - ts.y * 0.5f }, U32(online ? kText : kDim), status.c_str());
            PopFont();
            ImGui::Dummy({ pillW, 34 });
            ImGui::SameLine(0, 12);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 2);
            if (Icon("close", kIconClose, kDim, 36)) close = true;
        }

        void Tabs(const ui::Theme& t)
        {
            const char* names[] = { "Play", "Lobby" };
            const ImVec2 size{ 220, 40 };
            const auto p = ImGui::GetCursorScreenPos();
            auto* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p, { p.x + size.x, p.y + size.y }, U32(kSurface), 999);
            const float w = size.x / 2;
            dl->AddRectFilled({ p.x + 4 + w * u.tab, p.y + 4 }, { p.x + w * (u.tab + 1) - 4, p.y + size.y - 4 },
                              U32(kSurface2), 999);
            Font(g_semibold, 15);
            for (int i = 0; i < 2; ++i) {
                const auto ts = ImGui::CalcTextSize(names[i]);
                dl->AddText({ p.x + w * i + (w - ts.x) * 0.5f, p.y + (size.y - ts.y) * 0.5f },
                            U32(u.tab == i ? kText : kDim), names[i]);
                ImGui::SetCursorScreenPos({ p.x + w * i, p.y });
                ImGui::PushID(i);
                if (ImGui::InvisibleButton("tab", { w, size.y })) u.tab = i;
                ImGui::PopID();
            }
            PopFont();
            (void)t;
            ImGui::SetCursorScreenPos({ p.x, p.y + size.y + 20 });
        }

        // Code bar: tag chip, the code, copy, optional Invite.
        void CodeRow(const char* id, const char* tag, const std::string& code, const ui::Theme& t,
                     const std::function<void(ui::UiCommand)>& emit, bool invite, bool inviteEnabled)
        {
            ImGui::PushID(id);
            const bool have = !code.empty();
            const float avail = ImGui::GetContentRegionAvail().x;
            const float inviteW = invite ? 150.0f : 0.0f;
            const float codeW = avail - 46 - 8 - (invite ? inviteW + 10 : 0);
            const auto p = ImGui::GetCursorScreenPos();
            auto* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p, { p.x + codeW, p.y + 46 }, U32(kSurface2), kControl);
            Font(g_semibold, 11);
            const auto tagSize = ImGui::CalcTextSize(tag);
            dl->AddRectFilled({ p.x + 10, p.y + 13 }, { p.x + 26 + tagSize.x, p.y + 33 }, U32(t.accentAlt, 0.16f), 999);
            dl->AddText({ p.x + 18, p.y + 23 - tagSize.y * 0.5f }, U32(t.accentAlt), tag);
            PopFont();
            Font(g_mono, 16);
            const char* text = have ? code.c_str() : "-";
            const auto ts = ImGui::CalcTextSize(text);
            dl->AddText({ p.x + 38 + tagSize.x, p.y + 23 - ts.y * 0.5f }, U32(have ? kText : kFaint), text);
            PopFont();
            ImGui::Dummy({ codeW, 46 });
            ImGui::SameLine(0, 8);
            ImGui::BeginDisabled(!have);
            if (Icon("copy", kIconCopy, kText, 46)) {
                ImGui::SetClipboardText(code.c_str());
                Toast("Copied");
            }
            ImGui::EndDisabled();
            if (invite) {
                ImGui::SameLine(0, 10);
                const std::string label = std::string(kIconInvite) + "  Invite";
                if (Button(label.c_str(), inviteW, t.accent, 0x111317, have && inviteEnabled)) Emit("invite", {}, emit);
            }
            ImGui::PopID();
        }

        // Steam code (with Invite) and LAN code.
        void Codes(const ui::UiSnapshot& s, const ui::Theme& t, const std::function<void(ui::UiCommand)>& emit)
        {
            const bool steamCode = s.joinCode.starts_with("steam:");
            if (steamCode) CodeRow("steam", "STEAM", s.joinCode, t, emit, true, s.steamAvailable);
            CodeRow("lan", "LAN", s.lanCode.empty() && !steamCode ? s.joinCode : s.lanCode, t, emit, !steamCode,
                    false);
        }

        // Nearby LAN sessions (discovery), refreshed every 5 s while visible.
        void Nearby(const ui::UiSnapshot& s, const ui::Theme& t, const std::function<void(ui::UiCommand)>& emit)
        {
            static std::chrono::steady_clock::time_point next{};
            const auto now = std::chrono::steady_clock::now();
            if (now >= next) {
                next = now + std::chrono::seconds(5);
                emit(ui::cmd::RefreshServers{});
            }
            Label("NEARBY");
            if (s.servers.empty()) {
                Font(g_body, 15);
                ImGui::TextColored(Col(kFaint), "No games on your network");
                PopFont();
                return;
            }
            for (std::size_t i = 0; i < s.servers.size() && i < 3; ++i) {
                const auto& sv = s.servers[i];
                ImGui::PushID(static_cast<int>(i));
                const auto p = ImGui::GetCursorScreenPos();
                const float w = ImGui::GetContentRegionAvail().x, h = 44;
                auto* dl = ImGui::GetWindowDrawList();
                const bool hovered = ImGui::IsMouseHoveringRect(p, { p.x + w, p.y + h });
                dl->AddRectFilled(p, { p.x + w, p.y + h }, U32(hovered ? 0x202634 : kSurface2), kControl);
                Font(g_semibold, 15);
                dl->AddText({ p.x + 14, p.y + 12 }, U32(kText), sv.name.c_str());
                PopFont();
                Font(g_body, 14);
                const std::string meta = std::format("{}/{}  {} ms", sv.players, sv.maxPlayers, sv.pingMs);
                const auto ms = ImGui::CalcTextSize(meta.c_str());
                dl->AddText({ p.x + w - ms.x - 14, p.y + 22 - ms.y * 0.5f }, U32(kDim), meta.c_str());
                PopFont();
                if (ImGui::InvisibleButton("srv", { w, h })) {
                    std::snprintf(u.joinCode, sizeof(u.joinCode), "%s", sv.address.c_str());
                    Emit("join", { { "address", sv.address }, { "password", u.joinPassword } }, emit);
                }
                ImGui::Dummy({ 0, 2 });
                ImGui::PopID();
            }
            (void)t;
        }

        void Play(const ui::UiSnapshot& s, const ui::Theme& t, const std::function<void(ui::UiCommand)>& emit)
        {
            if (Connected(s.state)) {
                ImGui::BeginChild("session", { 0, 0 }, ImGuiChildFlags_AlwaysUseWindowPadding);
                Font(g_heading, 24);
                ImGui::TextUnformatted(s.hosting ? "Hosting" : "Connected");
                PopFont();
                Font(g_body, 15);
                ImGui::TextColored(Col(kDim), "%zu %s", s.players.size(), s.players.size() == 1 ? "player" : "players");
                PopFont();
                ImGui::Dummy({ 0, 8 });
                Label("JOIN CODES");
                Codes(s, t, emit);
                ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 46 - ImGui::GetStyle().WindowPadding.y);
                if (Ghost("Leave session", 180, t.bad)) Emit("leave", {}, emit);
                ImGui::EndChild();
                return;
            }
            const float half = (ImGui::GetContentRegionAvail().x - 16) * 0.5f;
            ImGui::BeginChild("host", { half, 0 }, ImGuiChildFlags_AlwaysUseWindowPadding);
            Font(g_heading, 24);
            ImGui::TextUnformatted("Host");
            PopFont();
            ImGui::Dummy({ 0, 4 });
            Field("##name", "Session name", u.hostName, sizeof(u.hostName));
            const float col = (ImGui::GetContentRegionAvail().x - 12) * 0.5f;
            ImGui::SetNextItemWidth(col);
            Font(g_body, 17);
            ImGui::InputInt("##port", &u.hostPort, 0);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(col);
            ImGui::SliderInt("##max", &u.hostMax, 2, 16, "%d players");
            PopFont();
            Field("##hpw", "Password (optional)", u.hostPassword, sizeof(u.hostPassword), ImGuiInputTextFlags_Password);
            ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 46 - ImGui::GetStyle().WindowPadding.y);
            if (Primary("Start hosting", -1, t)) {
                Emit("host",
                     { { "name", u.hostName }, { "port", std::to_string(u.hostPort) },
                       { "maxPlayers", std::to_string(u.hostMax) }, { "password", u.hostPassword } },
                     emit);
            }
            ImGui::EndChild();
            ImGui::SameLine(0, 16);
            ImGui::BeginChild("join", { half, 0 }, ImGuiChildFlags_AlwaysUseWindowPadding);
            Font(g_heading, 24);
            ImGui::TextUnformatted("Join");
            PopFont();
            ImGui::Dummy({ 0, 4 });
            Field("##code", "Join code", u.joinCode, sizeof(u.joinCode));
            Field("##jpw", "Password (optional)", u.joinPassword, sizeof(u.joinPassword), ImGuiInputTextFlags_Password);
            ImGui::Dummy({ 0, 2 });
            Nearby(s, t, emit);
            ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 46 - ImGui::GetStyle().WindowPadding.y);
            if (Button("Join", -1, t.accentAlt, 0x0B1418)) {
                Emit("join", { { "address", u.joinCode }, { "password", u.joinPassword } }, emit);
            }
            ImGui::EndChild();
        }

        void Lobby(const ui::UiSnapshot& s, const ui::Theme& t, const std::function<void(ui::UiCommand)>& emit)
        {
            Font(g_heading, 22);
            ImGui::TextUnformatted("Players");
            PopFont();
            ImGui::SameLine(0, 10);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4);
            Chip(std::to_string(s.players.size()).c_str(), kDim);

            const float footer = 46 * 2 + 12 + 24;
            ImGui::BeginChild("list", { 0, ImGui::GetContentRegionAvail().y - footer }, ImGuiChildFlags_None,
                              ImGuiWindowFlags_NoBackground);
            if (s.players.empty()) {
                Font(g_body, 16);
                ImGui::TextColored(Col(kFaint), "Not in a session");
                PopFont();
            }
            for (const auto& pl : s.players) {
                ImGui::PushID(static_cast<int>(pl.id));
                const auto p = ImGui::GetCursorScreenPos();
                const float w = ImGui::GetContentRegionAvail().x, h = 64;
                auto* dl = ImGui::GetWindowDrawList();
                const bool hovered = ImGui::IsMouseHoveringRect(p, { p.x + w, p.y + h });
                dl->AddRectFilled(p, { p.x + w, p.y + h }, U32(hovered ? kSurface2 : kSurface), kCard);
                // Avatar: initial in a tinted circle.
                const std::uint32_t tint = pl.isLocal ? t.accentAlt : t.accent;
                const ImVec2 c{ p.x + 36, p.y + h * 0.5f };
                dl->AddCircleFilled(c, 18, U32(tint, 0.18f));
                Font(g_heading, 18);
                const std::string initial = pl.name.empty() ? "?" : pl.name.substr(0, 1);
                const auto is = ImGui::CalcTextSize(initial.c_str());
                dl->AddText({ c.x - is.x * 0.5f, c.y - is.y * 0.5f }, U32(tint), initial.c_str());
                PopFont();
                // Name + chips.
                ImGui::SetCursorScreenPos({ p.x + 68, p.y + 12 });
                Font(g_semibold, 17);
                ImGui::TextUnformatted(pl.name.c_str());
                PopFont();
                ImGui::SetCursorScreenPos({ p.x + 68, p.y + 36 });
                if (pl.isLocal) {
                    Chip("YOU", t.accentAlt);
                    ImGui::SameLine(0, 6);
                }
                if (pl.isHost) {
                    Chip("HOST", t.accent);
                    ImGui::SameLine(0, 6);
                }
                if (!pl.isLocal) Chip(pl.sameLocation ? "NEARBY" : "AWAY", pl.sameLocation ? t.good : kDim);
                // Right side: presence dot and kick.
                Dot({ p.x + w - (s.hosting && !pl.isLocal ? 76 : 28), p.y + h * 0.5f },
                    pl.isLocal || pl.sameLocation ? t.good : kFaint);
                if (s.hosting && !pl.isLocal) {
                    ImGui::SetCursorScreenPos({ p.x + w - 50, p.y + 13 });
                    if (Icon("kick", kIconClose, t.bad, 38)) Emit("kick", { { "player", std::to_string(pl.id) } }, emit);
                }
                ImGui::SetCursorScreenPos({ p.x, p.y + h + 8 });
                ImGui::Dummy({ 0, 0 });
                ImGui::PopID();
            }
            ImGui::EndChild();
            ImGui::Dummy({ 0, 4 });
            Codes(s, t, emit);
        }

        void ToastLayer(const ui::UiSnapshot& s, const ui::Theme& t)
        {
            if (s.lastError != u.lastSeenError) {
                u.lastSeenError = s.lastError;
                if (!s.lastError.empty()) Toast(s.lastError);
            }
            if (u.toast.empty()) return;
            const float age = std::chrono::duration<float>(std::chrono::steady_clock::now() - u.toastAt).count();
            if (age > 4.0f) {
                u.toast.clear();
                return;
            }
            const float alpha = std::clamp(4.0f - age, 0.0f, 1.0f);
            const bool ok = u.toast == "Copied";
            Font(g_semibold, 15);
            const auto ts = ImGui::CalcTextSize(u.toast.c_str());
            const auto wp = ImGui::GetWindowPos();
            const auto ws = ImGui::GetWindowSize();
            const ImVec2 a{ wp.x + (ws.x - ts.x) * 0.5f - 18, wp.y + ws.y - 64 };
            auto* dl = ImGui::GetForegroundDrawList();
            dl->AddRectFilled(a, { a.x + ts.x + 36, a.y + 38 }, U32(ok ? 0x173326 : 0x3A1A1C, 0.96f * alpha), 999);
            dl->AddText({ a.x + 18, a.y + 19 - ts.y * 0.5f }, U32(ok ? t.good : 0xFF8A80, alpha), u.toast.c_str());
            PopFont();
        }
    }

    void LoadFonts()
    {
        auto& io = ImGui::GetIO();
        auto load = [&](const char* path) -> ImFont* {
            return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES ? io.Fonts->AddFontFromFileTTF(path, 17.0f) : nullptr;
        };
        g_body = load("C:\\Windows\\Fonts\\segoeui.ttf");
        if (g_body) {
            ImFontConfig merge;
            merge.MergeMode = true;
            merge.GlyphOffset = { 0, 2 };
            if (GetFileAttributesA("C:\\Windows\\Fonts\\SegoeIcons.ttf") != INVALID_FILE_ATTRIBUTES) {
                io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\SegoeIcons.ttf", 17.0f, &merge);
            } else if (GetFileAttributesA("C:\\Windows\\Fonts\\segmdl2.ttf") != INVALID_FILE_ATTRIBUTES) {
                io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segmdl2.ttf", 17.0f, &merge);
            }
        }
        g_semibold = load("C:\\Windows\\Fonts\\seguisb.ttf");
        g_heading = load("C:\\Windows\\Fonts\\bahnschrift.ttf");
        g_mono = load("C:\\Windows\\Fonts\\consola.ttf");
        ImFont* fallback = g_body ? g_body : io.Fonts->AddFontDefault();
        if (!g_body) g_body = fallback;
        if (!g_semibold) g_semibold = g_body;
        if (!g_heading) g_heading = g_semibold;
        if (!g_mono) g_mono = g_body;
        io.FontDefault = g_body;
    }

    bool Draw(const ui::UiSnapshot& s, const std::function<void(ui::UiCommand)>& emit)
    {
        const auto& t = s.theme;
        Style(t);
        const auto& io = ImGui::GetIO();
        const ImVec2 size{ std::min(980.0f, io.DisplaySize.x * 0.86f), std::min(640.0f, io.DisplaySize.y * 0.86f) };
        const ImVec2 pos{ (io.DisplaySize.x - size.x) * 0.5f, (io.DisplaySize.y - size.y) * 0.5f };
        // Soft shadow and scrim behind the panel.
        auto* bg = ImGui::GetBackgroundDrawList();
        bg->AddRectFilled({ 0, 0 }, io.DisplaySize, U32(0x000000, 0.35f));
        for (int i = 1; i <= 6; ++i) {
            const float g = i * 6.0f;
            bg->AddRectFilled({ pos.x - g, pos.y - g + 10 }, { pos.x + size.x + g, pos.y + size.y + g + 10 },
                              U32(0x000000, 0.06f), kRadius + g);
        }
        ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(size, ImGuiCond_Always);
        ImGui::Begin("##cosync", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoScrollbar);
        bool close = false;
        Header(s, t, close);
        ImGui::Dummy({ 0, 10 });
        Tabs(t);
        if (u.tab == 0) Play(s, t, emit);
        else Lobby(s, t, emit);
        ToastLayer(s, t);
        ImGui::End();
        return close;
    }
}
