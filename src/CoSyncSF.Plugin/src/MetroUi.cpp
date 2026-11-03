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
        // ---- Design tokens ------------------------------------------------------------------------
        constexpr std::uint32_t kBg = 0x0A0D12, kSide = 0x0E1218, kSurface = 0x13171E, kSurface2 = 0x1A1F28;
        constexpr std::uint32_t kHover = 0x212734, kStroke = 0x252B36;
        constexpr std::uint32_t kText = 0xEEF1F5, kDim = 0x7D8796, kFaint = 0x4A5260, kInk = 0x0E1014;
        constexpr float kRadius = 18.0f, kCard = 14.0f, kControl = 10.0f;

        // Segoe Fluent Icons (merged into the body font).
        constexpr const char* kIcoPlay = "\xEE\x9D\xA8";      // U+E768
        constexpr const char* kIcoPeople = "\xEE\x9C\x96";    // U+E716
        constexpr const char* kIcoGroup = "\xEE\xA4\x82";     // U+E902
        constexpr const char* kIcoSettings = "\xEE\x9C\x93";  // U+E713
        constexpr const char* kIcoCopy = "\xEE\xA3\x88";      // U+E8C8
        constexpr const char* kIcoInvite = "\xEE\xA3\xBA";    // U+E8FA
        constexpr const char* kIcoClose = "\xEE\x9C\x91";     // U+E711
        constexpr const char* kIcoRefresh = "\xEE\x9C\xAC";   // U+E72C
        constexpr const char* kIcoGlobe = "\xEE\x9D\xB4";     // U+E774
        constexpr const char* kIcoCrown = "\xEE\xB4\xA7";     // U+ED27 (badge)

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

        // ---- UI-local state (render thread) ------------------------------------------------------
        struct Settings
        {
            bool partyFrames = true;
            bool overheadNames = true;
            bool overheadDistance = true;
            int accent = 0;  // 0 theme amber, 1 cyan, 2 violet, 3 green
        };
        struct Ui
        {
            char hostName[65] = "CoSync";
            int hostPort = 37015;
            int hostMax = 8;
            char hostPassword[65] = "";
            char joinCode[96] = "";
            char joinPassword[65] = "";
            char displayName[33] = "";
            int page = 0;  // 0 Play, 1 Lobby, 2 Group, 3 Settings
            std::string toast;
            bool toastGood = false;
            std::chrono::steady_clock::time_point toastAt{};
            std::string lastSeenError;
            Settings settings;
        } u;

        std::uint32_t Accent(const ui::Theme& t)
        {
            constexpr std::uint32_t options[] = { 0, 0x4FB3D9, 0x9B7BFF, 0x5CC98A };
            return u.settings.accent == 0 ? t.accent : options[u.settings.accent];
        }

        void Toast(std::string text, bool good = false)
        {
            u.toast = std::move(text);
            u.toastGood = good;
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

        std::string WhereText(const std::string& place, std::uint8_t kind, float distance)
        {
            if (distance >= 0.0f) return std::format("{:.0f} m", distance);
            if (kind == 2) return place.empty() ? std::string("In space") : std::format("In orbit · {}", place);
            if (!place.empty()) return place;
            return "Elsewhere";
        }

        void Style(const ui::Theme& t)
        {
            auto& s = ImGui::GetStyle();
            s.WindowRounding = kRadius;
            s.ChildRounding = kCard;
            s.FrameRounding = s.PopupRounding = s.GrabRounding = kControl;
            s.WindowBorderSize = 1.0f;
            s.ChildBorderSize = 0.0f;
            s.FrameBorderSize = 0.0f;
            s.WindowPadding = { 0, 0 };
            s.FramePadding = { 14, 11 };
            s.ItemSpacing = { 12, 12 };
            s.GrabMinSize = 14;
            s.ScrollbarSize = 6;
            s.ScrollbarRounding = 99;
            auto* c = s.Colors;
            c[ImGuiCol_WindowBg] = Col(kBg, 0.98f);
            c[ImGuiCol_ChildBg] = Col(kSurface);
            c[ImGuiCol_Border] = Col(kStroke);
            c[ImGuiCol_Text] = Col(kText);
            c[ImGuiCol_TextDisabled] = Col(kDim);
            c[ImGuiCol_FrameBg] = Col(kSurface2);
            c[ImGuiCol_FrameBgHovered] = Col(kHover);
            c[ImGuiCol_FrameBgActive] = Col(0x252C3A);
            c[ImGuiCol_SliderGrab] = Col(Accent(t));
            c[ImGuiCol_SliderGrabActive] = Col(Accent(t));
            c[ImGuiCol_CheckMark] = Col(kInk);
            c[ImGuiCol_TextSelectedBg] = Col(Accent(t), 0.35f);
            c[ImGuiCol_ScrollbarBg] = Col(0, 0.0f);
            c[ImGuiCol_ScrollbarGrab] = Col(kStroke);
            c[ImGuiCol_NavCursor] = Col(0, 0.0f);
        }

        // ---- Controls ----------------------------------------------------------------------------
        bool Button(const char* label, float width, std::uint32_t fill, std::uint32_t text, bool enabled = true,
                    float height = 46.0f, float size = 16.0f)
        {
            ImGui::BeginDisabled(!enabled);
            ImGui::PushStyleColor(ImGuiCol_Button, Col(fill));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Col(fill, 0.86f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, Col(fill, 0.72f));
            ImGui::PushStyleColor(ImGuiCol_Text, Col(text));
            Font(g_semibold, size);
            const bool clicked = ImGui::Button(label, { width, height });
            PopFont();
            ImGui::PopStyleColor(4);
            ImGui::EndDisabled();
            return clicked;
        }
        bool Primary(const char* label, float width, const ui::Theme& t, bool enabled = true, float height = 46.0f)
        {
            return Button(label, width, Accent(t), kInk, enabled, height);
        }
        bool Secondary(const char* label, float width, std::uint32_t text = kText, float height = 46.0f)
        {
            return Button(label, width, kSurface2, text, true, height);
        }
        bool Icon(const char* id, const char* glyph, std::uint32_t text, float size = 38.0f, bool enabled = true)
        {
            ImGui::PushID(id);
            const bool clicked = Button(glyph, size, kSurface2, text, enabled, size, 15.0f);
            ImGui::PopID();
            return clicked;
        }
        void Field(const char* id, const char* hint, char* buf, std::size_t size, ImGuiInputTextFlags flags = 0,
                   float width = -1.0f)
        {
            ImGui::SetNextItemWidth(width);
            Font(g_body, 16);
            ImGui::InputTextWithHint(id, hint, buf, size, flags);
            PopFont();
        }
        void Caption(const char* text, std::uint32_t colour = kDim)
        {
            Font(g_semibold, 12);
            ImGui::PushStyleColor(ImGuiCol_Text, Col(colour));
            ImGui::TextUnformatted(text);
            ImGui::PopStyleColor();
            PopFont();
        }
        void Chip(const char* text, std::uint32_t colour)
        {
            Font(g_semibold, 11);
            const auto size = ImGui::CalcTextSize(text);
            const auto pos = ImGui::GetCursorScreenPos();
            const ImVec2 pad{ 8, 3 };
            auto* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(pos, { pos.x + size.x + pad.x * 2, pos.y + size.y + pad.y * 2 }, U32(colour, 0.15f), 999);
            dl->AddText({ pos.x + pad.x, pos.y + pad.y }, U32(colour), text);
            ImGui::Dummy({ size.x + pad.x * 2, size.y + pad.y * 2 });
            PopFont();
        }
        void Dot(ImDrawList* dl, ImVec2 c, std::uint32_t colour, float r = 4.0f)
        {
            dl->AddCircleFilled(c, r * 2.4f, U32(colour, 0.16f));
            dl->AddCircleFilled(c, r, U32(colour));
        }
        void Bar(ImDrawList* dl, ImVec2 a, float w, float h, float fraction, std::uint32_t colour)
        {
            dl->AddRectFilled(a, { a.x + w, a.y + h }, U32(0x000000, 0.35f), 99);
            if (fraction > 0.0f) dl->AddRectFilled(a, { a.x + w * std::clamp(fraction, 0.0f, 1.0f), a.y + h }, U32(colour), 99);
        }
        std::uint32_t HealthColour(int pct, const ui::Theme& t)
        {
            return pct < 0 ? kFaint : pct <= 25 ? t.bad : pct <= 50 ? t.warn : t.good;
        }
        void Avatar(ImDrawList* dl, ImVec2 c, float r, const std::string& name, std::uint32_t tint)
        {
            dl->AddCircleFilled(c, r, U32(tint, 0.18f));
            dl->AddCircle(c, r, U32(tint, 0.45f), 0, 1.5f);
            Font(g_heading, r * 0.95f);
            const std::string initial = name.empty() ? "?" : name.substr(0, 1);
            const auto is = ImGui::CalcTextSize(initial.c_str());
            dl->AddText({ c.x - is.x * 0.5f, c.y - is.y * 0.5f }, U32(tint), initial.c_str());
            PopFont();
        }
        // A card with padding; content until EndCard.
        bool BeginCard(const char* id, ImVec2 size)
        {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 22, 20 });
            const bool open = ImGui::BeginChild(id, size, ImGuiChildFlags_AlwaysUseWindowPadding);
            ImGui::PopStyleVar();
            return open;
        }
        void EndCard() { ImGui::EndChild(); }
        void CardTitle(const char* text)
        {
            Font(g_heading, 22);
            ImGui::TextUnformatted(text);
            PopFont();
            ImGui::Dummy({ 0, 2 });
        }
        void PinToBottom(float height)
        {
            ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), ImGui::GetWindowHeight() - height - 20));
        }

        // ---- Codes -------------------------------------------------------------------------------
        void CodeRow(const char* id, const char* tag, const std::string& code, const ui::Theme& t,
                     const std::function<void(ui::UiCommand)>& emit, bool invite, bool inviteEnabled)
        {
            ImGui::PushID(id);
            const bool have = !code.empty();
            const float avail = ImGui::GetContentRegionAvail().x;
            const float inviteW = invite ? 140.0f : 0.0f;
            const float codeW = avail - 46 - 8 - (invite ? inviteW + 10 : 0);
            const auto p = ImGui::GetCursorScreenPos();
            auto* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p, { p.x + codeW, p.y + 46 }, U32(kSurface2), kControl);
            Font(g_semibold, 10.5f);
            const auto tagSize = ImGui::CalcTextSize(tag);
            dl->AddRectFilled({ p.x + 10, p.y + 13 }, { p.x + 26 + tagSize.x, p.y + 33 }, U32(t.accentAlt, 0.16f), 999);
            dl->AddText({ p.x + 18, p.y + 23 - tagSize.y * 0.5f }, U32(t.accentAlt), tag);
            PopFont();
            Font(g_mono, 15.5f);
            const char* text = have ? code.c_str() : "-";
            const auto ts = ImGui::CalcTextSize(text);
            dl->AddText({ p.x + 38 + tagSize.x, p.y + 23 - ts.y * 0.5f }, U32(have ? kText : kFaint), text);
            PopFont();
            ImGui::Dummy({ codeW, 46 });
            ImGui::SameLine(0, 8);
            if (Icon("copy", kIcoCopy, kText, 46, have)) {
                ImGui::SetClipboardText(code.c_str());
                Toast("Copied", true);
            }
            if (invite) {
                ImGui::SameLine(0, 10);
                const std::string label = std::string(kIcoInvite) + "  Invite";
                if (Button(label.c_str(), inviteW, Accent(t), kInk, have && inviteEnabled)) Emit("invite", {}, emit);
            }
            ImGui::PopID();
        }
        void Codes(const ui::UiSnapshot& s, const ui::Theme& t, const std::function<void(ui::UiCommand)>& emit)
        {
            const bool steamCode = s.joinCode.starts_with("steam:");
            if (steamCode) CodeRow("steam", "STEAM", s.joinCode, t, emit, true, s.steamAvailable);
            CodeRow("lan", "LAN", s.lanCode.empty() && !steamCode ? s.joinCode : s.lanCode, t, emit, !steamCode, false);
        }

        // ---- Sidebar -----------------------------------------------------------------------------
        bool Sidebar(const ui::UiSnapshot& s, const ui::Theme& t, float height)
        {
            bool close = false;
            ImGui::PushStyleColor(ImGuiCol_ChildBg, Col(kSide));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 22, 26 });
            ImGui::BeginChild("side", { 232, height }, ImGuiChildFlags_AlwaysUseWindowPadding);
            ImGui::PopStyleVar();
            Font(g_heading, 30);
            ImGui::TextUnformatted("COSYNC");
            PopFont();
            Font(g_semibold, 11);
            ImGui::TextColored(Col(Accent(t)), "MULTIPLAYER");
            PopFont();
            ImGui::Dummy({ 0, 22 });
            const char* icons[] = { kIcoPlay, kIcoPeople, kIcoGroup, kIcoSettings };
            const char* names[] = { "Play", "Lobby", "Group", "Settings" };
            for (int i = 0; i < 4; ++i) {
                const auto p = ImGui::GetCursorScreenPos();
                const float w = ImGui::GetContentRegionAvail().x, h = 44;
                auto* dl = ImGui::GetWindowDrawList();
                const bool active = u.page == i;
                const bool hovered = ImGui::IsMouseHoveringRect(p, { p.x + w, p.y + h });
                if (active || hovered) dl->AddRectFilled(p, { p.x + w, p.y + h }, U32(active ? kSurface2 : kSurface), kControl);
                if (active) dl->AddRectFilled({ p.x, p.y + 12 }, { p.x + 3, p.y + h - 12 }, U32(Accent(t)), 99);
                Font(g_body, 16);
                dl->AddText({ p.x + 16, p.y + 13 }, U32(active ? Accent(t) : kDim), icons[i]);
                PopFont();
                Font(g_semibold, 15);
                dl->AddText({ p.x + 46, p.y + 12 }, U32(active ? kText : kDim), names[i]);
                PopFont();
                // Badge: pending group invites.
                if (i == 2 && !s.invites.empty()) Dot(dl, { p.x + w - 16, p.y + h * 0.5f }, Accent(t), 4.5f);
                ImGui::PushID(i);
                if (ImGui::InvisibleButton("nav", { w, h })) u.page = i;
                ImGui::PopID();
                ImGui::Dummy({ 0, 0 });
            }
            // Bottom: connection status.
            const bool online = s.state == net::ClientState::InSession;
            const bool busy = Connected(s.state) && !online;
            ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 86);
            const auto p = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x;
            auto* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p, { p.x + w, p.y + 58 }, U32(kSurface), kCard);
            Dot(dl, { p.x + 20, p.y + 29 }, online ? t.good : busy ? t.warn : kFaint, 4.5f);
            Font(g_semibold, 14);
            dl->AddText({ p.x + 38, p.y + 11 }, U32(kText), online ? (s.hosting ? "Hosting" : "Online") : busy ? "Connecting" : "Offline");
            PopFont();
            Font(g_body, 12.5f);
            const std::string sub = online ? std::format("{} players · {} ms", s.players.size(), s.rttMs) : std::string("Not in a session");
            dl->AddText({ p.x + 38, p.y + 31 }, U32(kDim), sub.c_str());
            PopFont();
            ImGui::Dummy({ w, 58 });  // ImGui: an item must follow a cursor move
            ImGui::EndChild();
            ImGui::PopStyleColor();
            return close;
        }

        void PageHeader(const char* title, const char* subtitle, bool& close)
        {
            Font(g_heading, 34);
            ImGui::TextUnformatted(title);
            PopFont();
            if (subtitle && *subtitle) {
                Font(g_body, 15);
                ImGui::TextColored(Col(kDim), "%s", subtitle);
                PopFont();
            }
            // Close, top-right.
            const float right = ImGui::GetWindowWidth() - 36 - 28;
            const auto keep = ImGui::GetCursorPos();
            ImGui::SetCursorPos({ right, 26 });
            if (Icon("close", kIcoClose, kDim, 36)) close = true;
            ImGui::SetCursorPos(keep);
            ImGui::Dummy({ 0, 10 });
        }

        // ---- Pages -------------------------------------------------------------------------------
        void Nearby(const ui::UiSnapshot& s, const std::function<void(ui::UiCommand)>& emit)
        {
            static std::chrono::steady_clock::time_point next{};
            const auto now = std::chrono::steady_clock::now();
            if (now >= next) {
                next = now + std::chrono::seconds(5);
                emit(ui::cmd::RefreshServers{});
            }
            Caption("ON YOUR NETWORK");
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 6);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 8);
            if (Icon("refresh", kIcoRefresh, kDim, 28)) emit(ui::cmd::RefreshServers{});
            if (s.servers.empty()) {
                Font(g_body, 14);
                ImGui::TextColored(Col(kFaint), "No games found");
                PopFont();
                return;
            }
            for (std::size_t i = 0; i < s.servers.size() && i < 4; ++i) {
                const auto& sv = s.servers[i];
                ImGui::PushID(static_cast<int>(i));
                const auto p = ImGui::GetCursorScreenPos();
                const float w = ImGui::GetContentRegionAvail().x, h = 46;
                auto* dl = ImGui::GetWindowDrawList();
                const bool hovered = ImGui::IsMouseHoveringRect(p, { p.x + w, p.y + h });
                dl->AddRectFilled(p, { p.x + w, p.y + h }, U32(hovered ? kHover : kSurface2), kControl);
                Font(g_body, 14);
                dl->AddText({ p.x + 14, p.y + 15 }, U32(kDim), kIcoGlobe);
                PopFont();
                Font(g_semibold, 15);
                dl->AddText({ p.x + 40, p.y + 13 }, U32(kText), sv.name.c_str());
                PopFont();
                Font(g_body, 13);
                const std::string meta = std::format("{}/{}   {} ms", sv.players, sv.maxPlayers, sv.pingMs);
                const auto ms = ImGui::CalcTextSize(meta.c_str());
                dl->AddText({ p.x + w - ms.x - 14, p.y + 23 - ms.y * 0.5f }, U32(kDim), meta.c_str());
                PopFont();
                if (ImGui::InvisibleButton("srv", { w, h })) {
                    std::snprintf(u.joinCode, sizeof(u.joinCode), "%s", sv.address.c_str());
                    Emit("join", { { "address", sv.address }, { "password", u.joinPassword } }, emit);
                }
                ImGui::Dummy({ 0, 0 });
                ImGui::PopID();
            }
        }

        void PagePlay(const ui::UiSnapshot& s, const ui::Theme& t, const std::function<void(ui::UiCommand)>& emit)
        {
            const float avail = ImGui::GetContentRegionAvail().y;
            if (Connected(s.state)) {
                if (BeginCard("session", { 0, avail })) {
                    Font(g_heading, 26);
                    ImGui::TextUnformatted(s.hosting ? "You're hosting" : "You're connected");
                    PopFont();
                    Font(g_body, 15);
                    ImGui::TextColored(Col(kDim), "%zu %s  ·  %u ms", s.players.size(),
                                       s.players.size() == 1 ? "player" : "players", s.rttMs);
                    PopFont();
                    ImGui::Dummy({ 0, 14 });
                    Caption("JOIN CODES");
                    Codes(s, t, emit);
                    PinToBottom(46);
                    if (Secondary("Leave session", 190, t.bad)) Emit("leave", {}, emit);
                }
                EndCard();
                return;
            }
            const float half = (ImGui::GetContentRegionAvail().x - 16) * 0.5f;
            if (BeginCard("host", { half, avail })) {
                CardTitle("Host");
                Field("##name", "Session name", u.hostName, sizeof(u.hostName));
                const float col = (ImGui::GetContentRegionAvail().x - 12) * 0.5f;
                Font(g_body, 16);
                ImGui::SetNextItemWidth(col);
                ImGui::InputInt("##port", &u.hostPort, 0);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(col);
                ImGui::SliderInt("##max", &u.hostMax, 2, 16, "%d players");
                PopFont();
                Field("##hpw", "Password", u.hostPassword, sizeof(u.hostPassword), ImGuiInputTextFlags_Password);
                PinToBottom(46);
                if (Primary("Start hosting", -1, t)) {
                    Emit("host",
                         { { "name", u.hostName }, { "port", std::to_string(u.hostPort) },
                           { "maxPlayers", std::to_string(u.hostMax) }, { "password", u.hostPassword } },
                         emit);
                }
            }
            EndCard();
            ImGui::SameLine(0, 16);
            if (BeginCard("join", { half, avail })) {
                CardTitle("Join");
                Field("##code", "steam:… or ip:port", u.joinCode, sizeof(u.joinCode));
                Field("##jpw", "Password", u.joinPassword, sizeof(u.joinPassword), ImGuiInputTextFlags_Password);
                if (Button("Join", -1, t.accentAlt, kInk)) {
                    Emit("join", { { "address", u.joinCode }, { "password", u.joinPassword } }, emit);
                }
                ImGui::Dummy({ 0, 6 });
                Nearby(s, emit);
            }
            EndCard();
        }

        // One player row: avatar, name + chips, vitals, place, actions.
        void PlayerRowView(const ui::PlayerRow& pl, const ui::UiSnapshot& s, const ui::Theme& t,
                           const std::function<void(ui::UiCommand)>& emit, bool groupActions)
        {
            ImGui::PushID(static_cast<int>(pl.id));
            const auto p = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x, h = 70;
            auto* dl = ImGui::GetWindowDrawList();
            const bool hovered = ImGui::IsMouseHoveringRect(p, { p.x + w, p.y + h });
            dl->AddRectFilled(p, { p.x + w, p.y + h }, U32(hovered ? kSurface2 : kSurface), kCard);
            Avatar(dl, { p.x + 38, p.y + h * 0.5f }, 19, pl.name, pl.isLocal ? t.accentAlt : Accent(t));
            // Name + chips
            ImGui::SetCursorScreenPos({ p.x + 72, p.y + 13 });
            Font(g_semibold, 16);
            ImGui::TextUnformatted(pl.name.c_str());
            PopFont();
            ImGui::SameLine(0, 8);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2);
            if (pl.isLocal) {
                Chip("YOU", t.accentAlt);
                ImGui::SameLine(0, 6);
            }
            if (pl.isHost) {
                Chip("HOST", Accent(t));
                ImGui::SameLine(0, 6);
            }
            if (pl.inMyGroup && !pl.isLocal) Chip("GROUP", t.good);
            // Second line: level · place, health bar
            const std::string line = std::format("{}{}", pl.level > 0 ? std::format("Lv {}   ", pl.level) : std::string(),
                                                 pl.isLocal ? std::string() : WhereText(pl.place, pl.placeKind, pl.distance));
            Font(g_body, 13);
            dl->AddText({ p.x + 72, p.y + 42 }, U32(kDim), line.c_str());
            PopFont();
            const float barW = 120;
            const float barX = p.x + w - barW - (groupActions || s.hosting ? 116 : 24);
            if (pl.healthPercent >= 0) {
                Bar(dl, { barX, p.y + h * 0.5f - 3 }, barW, 6, pl.healthPercent / 100.0f, HealthColour(pl.healthPercent, t));
            }
            // Actions
            float ax = p.x + w - 16;
            if (s.hosting && !pl.isLocal) {
                ax -= 38;
                ImGui::SetCursorScreenPos({ ax, p.y + 16 });
                if (Icon("kick", kIcoClose, t.bad, 38)) Emit("kick", { { "player", std::to_string(pl.id) } }, emit);
            }
            if (groupActions && !pl.isLocal) {
                const bool leader = s.myGroup != 0 && s.myGroupLeader == s.localPlayer;
                const bool invited = std::ranges::find(s.myGroupInvited, pl.id) != s.myGroupInvited.end();
                if (leader && !pl.inMyGroup) {
                    ax -= 46;
                    ImGui::SetCursorScreenPos({ ax, p.y + 16 });
                    if (Icon("ginv", kIcoInvite, invited ? kFaint : Accent(t), 38, !invited)) {
                        Emit("groupInvite", { { "player", std::to_string(pl.id) } }, emit);
                        Toast("Invite sent", true);
                    }
                } else if (leader && pl.inMyGroup) {
                    ax -= 46;
                    ImGui::SetCursorScreenPos({ ax, p.y + 16 });
                    if (Icon("gkick", kIcoClose, t.warn, 38)) Emit("groupKick", { { "player", std::to_string(pl.id) } }, emit);
                }
            }
            ImGui::SetCursorScreenPos({ p.x, p.y + h + 8 });
            ImGui::Dummy({ 0, 0 });
            ImGui::PopID();
        }

        void PageLobby(const ui::UiSnapshot& s, const ui::Theme& t, const std::function<void(ui::UiCommand)>& emit)
        {
            const float footer = Connected(s.state) ? 46.0f * 2 + 12 + 30 : 0.0f;
            ImGui::BeginChild("players", { 0, ImGui::GetContentRegionAvail().y - footer }, ImGuiChildFlags_None,
                              ImGuiWindowFlags_NoBackground);
            if (s.players.empty()) {
                Font(g_body, 15);
                ImGui::TextColored(Col(kFaint), "Not in a session");
                PopFont();
            }
            for (const auto& pl : s.players) PlayerRowView(pl, s, t, emit, true);
            ImGui::EndChild();
            if (footer > 0) {
                Caption("JOIN CODES");
                Codes(s, t, emit);
            }
        }

        void PageGroup(const ui::UiSnapshot& s, const ui::Theme& t, const std::function<void(ui::UiCommand)>& emit)
        {
            if (!Connected(s.state)) {
                Font(g_body, 15);
                ImGui::TextColored(Col(kFaint), "Join or host a session first");
                PopFont();
                return;
            }
            // Pending invites.
            for (const auto& inv : s.invites) {
                ImGui::PushID(static_cast<int>(inv.group));
                const auto p = ImGui::GetCursorScreenPos();
                const float w = ImGui::GetContentRegionAvail().x, h = 76;
                auto* dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(p, { p.x + w, p.y + h }, U32(Accent(t), 0.10f), kCard);
                dl->AddRect(p, { p.x + w, p.y + h }, U32(Accent(t), 0.45f), kCard, 0, 1.2f);
                Avatar(dl, { p.x + 40, p.y + h * 0.5f }, 20, inv.leaderName, Accent(t));
                Font(g_semibold, 16);
                const std::string title = std::format("{} invited you", inv.leaderName);
                dl->AddText({ p.x + 74, p.y + 18 }, U32(kText), title.c_str());
                PopFont();
                Font(g_body, 13);
                dl->AddText({ p.x + 74, p.y + 42 }, U32(kDim), "Share markers, health and location");
                PopFont();
                ImGui::SetCursorScreenPos({ p.x + w - 232, p.y + 18 });
                if (Primary("Accept", 110, t, true, 40)) Emit("groupAccept", { { "group", std::to_string(inv.group) } }, emit);
                ImGui::SameLine(0, 8);
                if (Secondary("Decline", 98, kDim, 40)) Emit("groupDecline", { { "group", std::to_string(inv.group) } }, emit);
                ImGui::SetCursorScreenPos({ p.x, p.y + h + 12 });
                ImGui::Dummy({ 0, 0 });
                ImGui::PopID();
            }
            if (s.myGroup == 0) {
                const float avail = ImGui::GetContentRegionAvail().y;
                if (BeginCard("nogroup", { 0, avail })) {
                    CardTitle("No group");
                    Font(g_body, 15);
                    ImGui::TextColored(Col(kDim), "Group members see each other's marker, health, level and planet.");
                    PopFont();
                    PinToBottom(46);
                    if (Primary("Create group", 200, t)) Emit("groupCreate", {}, emit);
                }
                EndCard();
                return;
            }
            const bool leader = s.myGroupLeader == s.localPlayer;
            Caption(leader ? "YOUR GROUP  ·  YOU LEAD" : "YOUR GROUP");
            ImGui::BeginChild("members", { 0, ImGui::GetContentRegionAvail().y - 62 }, ImGuiChildFlags_None,
                              ImGuiWindowFlags_NoBackground);
            for (const auto& pl : s.players) {
                if (pl.inMyGroup) PlayerRowView(pl, s, t, emit, true);
            }
            if (leader) {
                bool header = false;
                for (const auto& pl : s.players) {
                    if (pl.inMyGroup || pl.isLocal) continue;
                    if (!header) {
                        ImGui::Dummy({ 0, 6 });
                        Caption("INVITE");
                        header = true;
                    }
                    PlayerRowView(pl, s, t, emit, true);
                }
            }
            ImGui::EndChild();
            ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 46 - 28);
            if (Secondary("Leave group", 170, t.bad)) Emit("groupLeave", {}, emit);
        }

        void PageSettings(const ui::UiSnapshot& s, const ui::Theme& t, const std::function<void(ui::UiCommand)>& emit)
        {
            const float half = (ImGui::GetContentRegionAvail().x - 16) * 0.5f;
            const float avail = ImGui::GetContentRegionAvail().y;
            if (BeginCard("profile", { half, avail })) {
                CardTitle("Profile");
                if (!u.displayName[0] && !s.displayName.empty()) {
                    std::snprintf(u.displayName, sizeof(u.displayName), "%s", s.displayName.c_str());
                }
                Field("##dn", "Display name", u.displayName, sizeof(u.displayName));
                if (Secondary("Save name", 150)) {
                    Emit("setName", { { "name", u.displayName } }, emit);
                    Toast("Applies to your next session", true);
                }
                ImGui::Dummy({ 0, 10 });
                Caption("ACCENT");
                const std::uint32_t swatches[] = { t.accent, 0x4FB3D9, 0x9B7BFF, 0x5CC98A };
                for (int i = 0; i < 4; ++i) {
                    if (i) ImGui::SameLine(0, 10);
                    const auto p = ImGui::GetCursorScreenPos();
                    auto* dl = ImGui::GetWindowDrawList();
                    dl->AddCircleFilled({ p.x + 16, p.y + 16 }, 14, U32(swatches[i]));
                    if (u.settings.accent == i) dl->AddCircle({ p.x + 16, p.y + 16 }, 18, U32(kText), 0, 2.0f);
                    ImGui::PushID(i);
                    if (ImGui::InvisibleButton("sw", { 32, 32 })) u.settings.accent = i;
                    ImGui::PopID();
                }
            }
            EndCard();
            ImGui::SameLine(0, 16);
            if (BeginCard("hud", { half, avail })) {
                CardTitle("HUD");
                auto toggle = [](const char* label, bool& v) {
                    Font(g_body, 16);
                    ImGui::Checkbox(label, &v);
                    PopFont();
                };
                toggle("Party frames", u.settings.partyFrames);
                toggle("Names over players", u.settings.overheadNames);
                toggle("Distance", u.settings.overheadDistance);
                ImGui::Dummy({ 0, 8 });
                Font(g_body, 13.5f);
                ImGui::TextColored(Col(kFaint), "Only group members on the same planet show overhead.");
                PopFont();
            }
            EndCard();
        }

        void ToastLayer(const ui::UiSnapshot& s, const ui::Theme& t)
        {
            if (s.lastError != u.lastSeenError) {
                u.lastSeenError = s.lastError;
                if (!s.lastError.empty()) Toast(s.lastError);
            }
            if (u.toast.empty()) return;
            const float age = std::chrono::duration<float>(std::chrono::steady_clock::now() - u.toastAt).count();
            if (age > 3.5f) {
                u.toast.clear();
                return;
            }
            const float alpha = std::clamp(3.5f - age, 0.0f, 1.0f);
            Font(g_semibold, 15);
            const auto ts = ImGui::CalcTextSize(u.toast.c_str());
            const auto wp = ImGui::GetWindowPos();
            const auto ws = ImGui::GetWindowSize();
            const ImVec2 a{ wp.x + 232 + (ws.x - 232 - ts.x) * 0.5f - 20, wp.y + ws.y - 66 };
            auto* dl = ImGui::GetForegroundDrawList();
            dl->AddRectFilled(a, { a.x + ts.x + 40, a.y + 40 }, U32(u.toastGood ? 0x15302A : 0x3A1A1C, 0.97f * alpha), 999);
            dl->AddText({ a.x + 20, a.y + 20 - ts.y * 0.5f }, U32(u.toastGood ? t.good : 0xFF8A80, alpha), u.toast.c_str());
            PopFont();
        }
    }

    void LoadFonts()
    {
        auto& io = ImGui::GetIO();
        auto exists = [](const char* path) { return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES; };
        auto load = [&](const char* path) -> ImFont* { return exists(path) ? io.Fonts->AddFontFromFileTTF(path, 16.0f) : nullptr; };
        const char* icons = exists("C:\\Windows\\Fonts\\SegoeIcons.ttf") ? "C:\\Windows\\Fonts\\SegoeIcons.ttf"
                            : exists("C:\\Windows\\Fonts\\segmdl2.ttf") ? "C:\\Windows\\Fonts\\segmdl2.ttf"
                                                                         : nullptr;
        // Icon glyphs merged into the body and semibold fonts (buttons draw with semibold).
        auto mergeIcons = [&] {
            if (!icons) return;
            ImFontConfig merge;
            merge.MergeMode = true;
            merge.GlyphOffset = { 0, 2 };
            io.Fonts->AddFontFromFileTTF(icons, 16.0f, &merge);
        };
        g_body = load("C:\\Windows\\Fonts\\segoeui.ttf");
        if (g_body) mergeIcons();
        g_semibold = load("C:\\Windows\\Fonts\\seguisb.ttf");
        if (g_semibold) mergeIcons();
        g_heading = load("C:\\Windows\\Fonts\\bahnschrift.ttf");
        g_mono = load("C:\\Windows\\Fonts\\consola.ttf");
        if (!g_body) g_body = io.Fonts->AddFontDefault();
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
        const ImVec2 size{ std::min(1080.0f, io.DisplaySize.x * 0.88f), std::min(680.0f, io.DisplaySize.y * 0.88f) };
        const ImVec2 pos{ (io.DisplaySize.x - size.x) * 0.5f, (io.DisplaySize.y - size.y) * 0.5f };
        // Scrim + soft shadow: the game behind is inactive while this is open.
        auto* bg = ImGui::GetBackgroundDrawList();
        bg->AddRectFilled({ 0, 0 }, io.DisplaySize, U32(0x000000, 0.45f));
        for (int i = 1; i <= 7; ++i) {
            const float g = i * 6.0f;
            bg->AddRectFilled({ pos.x - g, pos.y - g + 12 }, { pos.x + size.x + g, pos.y + size.y + g + 12 },
                              U32(0x000000, 0.055f), kRadius + g);
        }
        ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(size, ImGuiCond_Always);
        ImGui::Begin("##cosync", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoScrollbar);
        bool close = Sidebar(s, t, size.y);
        ImGui::SameLine(0, 0);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Col(0, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 32, 28 });
        ImGui::BeginChild("content", { 0, size.y }, ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleVar();
        const char* titles[] = { "Play", "Lobby", "Group", "Settings" };
        const std::string subtitle = u.page == 1   ? std::format("{} in session", s.players.size())
                                     : u.page == 2 ? std::string(s.myGroup ? "Shared HUD" : "")
                                                   : std::string();
        PageHeader(titles[u.page], subtitle.c_str(), close);
        switch (u.page) {
        case 0: PagePlay(s, t, emit); break;
        case 1: PageLobby(s, t, emit); break;
        case 2: PageGroup(s, t, emit); break;
        default: PageSettings(s, t, emit); break;
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ToastLayer(s, t);
        ImGui::End();
        return close;
    }

    bool DrawLauncher(const ui::UiSnapshot& s)
    {
        const auto& t = s.theme;
        Style(t);
        const auto& io = ImGui::GetIO();
        const ImVec2 size{ 250, 64 };
        const ImVec2 pos{ io.DisplaySize.x - size.x - 40, 40 };
        ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(size, ImGuiCond_Always);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, Col(kBg, 0.92f));
        ImGui::Begin("##launcher", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        auto* dl = ImGui::GetWindowDrawList();
        const bool hovered = ImGui::IsWindowHovered();
        if (hovered) dl->AddRectFilled(pos, { pos.x + size.x, pos.y + size.y }, U32(kSurface2), kRadius);
        dl->AddRectFilled({ pos.x + 14, pos.y + 14 }, { pos.x + 50, pos.y + 50 }, U32(Accent(t)), 12);
        Font(g_body, 18);
        dl->AddText({ pos.x + 23, pos.y + 22 }, U32(kInk), kIcoPeople);
        PopFont();
        Font(g_heading, 20);
        dl->AddText({ pos.x + 64, pos.y + 12 }, U32(kText), "MULTIPLAYER");
        PopFont();
        Font(g_body, 12.5f);
        const std::string sub = s.state == net::ClientState::InSession ? std::format("{} players online", s.players.size())
                                                                       : std::string("Host or join");
        dl->AddText({ pos.x + 64, pos.y + 36 }, U32(kDim), sub.c_str());
        PopFont();
        ImGui::SetCursorScreenPos(pos);
        const bool clicked = ImGui::InvisibleButton("open", size);
        ImGui::End();
        ImGui::PopStyleColor();
        return clicked;
    }

    void DrawHud(const overlay::HudState& hud, const ui::Theme& t)
    {
        const auto& io = ImGui::GetIO();
        auto* dl = ImGui::GetBackgroundDrawList();
        // Overhead markers: icon, name, distance (same planet / area only).
        if (u.settings.overheadNames) {
            for (const auto& m : hud.members) {
                if (!m.marker) continue;
                // NDC from the bridge (both axes scaled by the horizontal FOV): pixels.
                const ImVec2 c{ io.DisplaySize.x * 0.5f * (1.0f + m.x), io.DisplaySize.y * 0.5f - m.y * io.DisplaySize.x * 0.5f };
                if (c.x < 0 || c.y < 0 || c.x > io.DisplaySize.x || c.y > io.DisplaySize.y) continue;
                // Diamond icon.
                const float r = 7.0f;
                dl->AddQuadFilled({ c.x, c.y - r }, { c.x + r, c.y }, { c.x, c.y + r }, { c.x - r, c.y }, U32(Accent(t)));
                dl->AddQuad({ c.x, c.y - r - 3 }, { c.x + r + 3, c.y }, { c.x, c.y + r + 3 }, { c.x - r - 3, c.y }, U32(Accent(t), 0.4f), 1.5f);
                Font(g_semibold, 15);
                const auto ns = ImGui::CalcTextSize(m.name.c_str());
                const ImVec2 a{ c.x - ns.x * 0.5f - 10, c.y - r - 34 };
                dl->AddRectFilled(a, { a.x + ns.x + 20, a.y + 24 }, U32(kBg, 0.72f), 99);
                dl->AddText({ a.x + 10, a.y + 12 - ns.y * 0.5f }, U32(kText), m.name.c_str());
                PopFont();
                if (u.settings.overheadDistance && m.distance >= 0.0f) {
                    Font(g_semibold, 12.5f);
                    const std::string d = std::format("{:.0f} m", m.distance);
                    const auto ds = ImGui::CalcTextSize(d.c_str());
                    dl->AddText({ c.x - ds.x * 0.5f, c.y + r + 6 }, U32(Accent(t)), d.c_str());
                    PopFont();
                }
            }
        }
        // Party frames, stacked above the player's own health (bottom-left).
        if (!u.settings.partyFrames || hud.members.empty()) return;
        const float w = 280, h = 62, gap = 8;
        float y = io.DisplaySize.y * 0.80f - h;
        for (const auto& m : hud.members) {
            const ImVec2 a{ 40, y };
            dl->AddRectFilled(a, { a.x + w, a.y + h }, U32(kBg, 0.78f), 14);
            dl->AddRectFilled({ a.x, a.y + 14 }, { a.x + 3, a.y + h - 14 }, U32(Accent(t)), 99);
            Avatar(dl, { a.x + 30, a.y + h * 0.5f }, 15, m.name, Accent(t));
            Font(g_semibold, 15);
            dl->AddText({ a.x + 54, a.y + 9 }, U32(kText), m.name.c_str());
            PopFont();
            if (m.level > 0) {
                Font(g_semibold, 12);
                const std::string lv = std::format("LV {}", m.level);
                const auto ls = ImGui::CalcTextSize(lv.c_str());
                dl->AddText({ a.x + w - ls.x - 14, a.y + 11 }, U32(kDim), lv.c_str());
                PopFont();
            }
            Bar(dl, { a.x + 54, a.y + 32 }, w - 54 - 14, 6, m.healthPercent < 0 ? 0.0f : m.healthPercent / 100.0f,
                HealthColour(m.healthPercent, t));
            Font(g_body, 12);
            const std::string where = WhereText(m.place, m.placeKind, m.distance);
            dl->AddText({ a.x + 54, a.y + 42 }, U32(kDim), where.c_str());
            PopFont();
            y -= h + gap;
        }
    }
}
