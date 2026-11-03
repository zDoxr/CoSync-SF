#include "MetroUi.h"

#include "cosync/net/SessionClient.h"

#include <imgui.h>

#include <format>
#include <string>

namespace cosync::plugin::metroui
{
    namespace
    {
        // ---- Metro styling from the theme tokens ----
        ImVec4 Rgb(std::uint32_t rgb, float a = 1.0f)
        {
            return { ((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a };
        }

        void ApplyStyle(const ui::Theme& t)
        {
            auto& s = ImGui::GetStyle();
            s.WindowRounding = s.FrameRounding = s.ChildRounding = s.PopupRounding = s.TabRounding = 0.0f;
            s.ScrollbarRounding = s.GrabRounding = 0.0f;
            s.WindowBorderSize = s.ChildBorderSize = 0.0f;
            s.FrameBorderSize = 1.0f;
            s.WindowPadding = { t.spacing * 1.5f, t.spacing * 1.5f };
            s.FramePadding = { t.spacing * 0.75f, t.spacing * 0.5f };
            s.ItemSpacing = { t.spacing, t.spacing * 0.75f };
            auto* c = s.Colors;
            c[ImGuiCol_WindowBg] = Rgb(t.background, t.panelOpacity);
            c[ImGuiCol_ChildBg] = Rgb(t.panel, 1.0f);
            c[ImGuiCol_Text] = Rgb(t.text);
            c[ImGuiCol_TextDisabled] = Rgb(t.textDim);
            c[ImGuiCol_Border] = Rgb(t.panelAlt);
            c[ImGuiCol_FrameBg] = Rgb(t.panelAlt);
            c[ImGuiCol_FrameBgHovered] = Rgb(t.panelAlt, 0.85f);
            c[ImGuiCol_FrameBgActive] = Rgb(t.panelAlt, 0.7f);
            c[ImGuiCol_Button] = Rgb(t.accent);
            c[ImGuiCol_ButtonHovered] = Rgb(t.accent, 0.85f);
            c[ImGuiCol_ButtonActive] = Rgb(t.accent, 0.7f);
            c[ImGuiCol_Header] = Rgb(t.panelAlt);
            c[ImGuiCol_HeaderHovered] = Rgb(t.accentAlt, 0.35f);
            c[ImGuiCol_HeaderActive] = Rgb(t.accentAlt, 0.5f);
            c[ImGuiCol_TableHeaderBg] = Rgb(t.panelAlt);
            c[ImGuiCol_TableRowBg] = Rgb(t.panel, 0.0f);
            c[ImGuiCol_TableRowBgAlt] = Rgb(t.panelAlt, 0.35f);
            c[ImGuiCol_Separator] = Rgb(t.accent);
            ImGui::GetIO().FontGlobalScale = t.fontScale * 1.15f;
        }

        // Metro tile button: flat, square, label bottom-left like a Windows Phone tile.
        bool Tile(const char* label, ImVec2 size, std::uint32_t colour, bool enabled = true)
        {
            ImGui::BeginDisabled(!enabled);
            ImGui::PushStyleColor(ImGuiCol_Button, Rgb(colour));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Rgb(colour, 0.85f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, Rgb(colour, 0.7f));
            ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, { 0.08f, 0.85f });
            const bool clicked = ImGui::Button(label, size);
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(3);
            ImGui::EndDisabled();
            return clicked;
        }

        void SectionHeader(const char* text, const ui::Theme& t)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, Rgb(t.accent));
            ImGui::TextUnformatted(text);
            ImGui::PopStyleColor();
        }

        const char* StateText(net::ClientState s)
        {
            switch (s) {
            case net::ClientState::Idle: return "OFFLINE";
            case net::ClientState::Connecting: return "CONNECTING";
            case net::ClientState::Handshaking: return "HANDSHAKING";
            case net::ClientState::InSession: return "IN SESSION";
            case net::ClientState::Reconnecting: return "RECONNECTING";
            case net::ClientState::Disconnected: return "DISCONNECTED";
            }
            return "?";
        }

        // Form state (render thread only).
        struct Forms
        {
            char hostName[65] = "CoSync Session";
            int hostPort = 37015;
            int hostMax = 8;
            char hostPassword[65] = "";
            char joinAddress[64] = "";
            char joinPassword[65] = "";
            int tab = 0;  // 0 Multiplayer, 1 Lobby
            std::string localError;
        } f;

        void Emit(const char* intent, const ui::IntentArgs& args, const std::function<void(ui::UiCommand)>& emit)
        {
            std::string error;
            if (auto c = ui::ParseIntent(intent, args, error)) {
                f.localError.clear();
                emit(std::move(*c));
            } else {
                f.localError = error;
            }
        }

        void DrawMultiplayer(const ui::UiSnapshot& s, const std::function<void(ui::UiCommand)>& emit)
        {
            const auto& t = s.theme;
            const bool inSession = s.state == net::ClientState::InSession || s.state == net::ClientState::Connecting ||
                                   s.state == net::ClientState::Handshaking || s.state == net::ClientState::Reconnecting;
            // Status strip
            ImGui::BeginChild("status", { 0, 86 }, ImGuiChildFlags_None);
            SectionHeader("STATUS", t);
            const auto stateColour = s.state == net::ClientState::InSession ? t.good
                                     : inSession                             ? t.warn
                                                                             : t.textDim;
            ImGui::PushStyleColor(ImGuiCol_Text, Rgb(stateColour));
            ImGui::Text("%s", StateText(s.state));
            ImGui::PopStyleColor();
            ImGui::SameLine(220);
            ImGui::TextDisabled("%s  |  %zu player(s)  |  rtt %u ms  |  protocol %s  |  game %s",
                                s.hosting ? "HOSTING" : "CLIENT", s.players.size(), s.rttMs, s.protocolVersion.c_str(),
                                s.gameVersion.c_str());
            ImGui::EndChild();

            const float half = (ImGui::GetContentRegionAvail().x - t.spacing) * 0.5f;
            // Host
            ImGui::BeginChild("host", { half, 300 });
            SectionHeader("HOST A GAME", t);
            ImGui::InputText("Server name", f.hostName, sizeof(f.hostName));
            ImGui::InputInt("Port", &f.hostPort);
            ImGui::SliderInt("Max players", &f.hostMax, 2, 16);
            ImGui::InputText("Password", f.hostPassword, sizeof(f.hostPassword), ImGuiInputTextFlags_Password);
            if (Tile("HOST", { half - t.spacing * 3, 64 }, t.accent)) {
                Emit("host",
                     { { "name", f.hostName }, { "port", std::to_string(f.hostPort) },
                       { "maxPlayers", std::to_string(f.hostMax) }, { "password", f.hostPassword } },
                     emit);
            }
            ImGui::EndChild();
            ImGui::SameLine();
            // Join
            ImGui::BeginChild("join", { half, 300 });
            SectionHeader("JOIN A GAME", t);
            ImGui::InputTextWithHint("Join code", "ip:port", f.joinAddress, sizeof(f.joinAddress));
            ImGui::InputText("Password##join", f.joinPassword, sizeof(f.joinPassword), ImGuiInputTextFlags_Password);
            if (Tile("JOIN", { half - t.spacing * 3, 64 }, t.accentAlt)) {
                Emit("join", { { "address", f.joinAddress }, { "password", f.joinPassword } }, emit);
            }
            ImGui::EndChild();

            if (inSession && Tile("LEAVE SESSION", { 220, 52 }, t.bad)) Emit("leave", {}, emit);
        }

        void DrawLobby(const ui::UiSnapshot& s, const std::function<void(ui::UiCommand)>& emit)
        {
            const auto& t = s.theme;
            SectionHeader("PLAYERS", t);
            if (ImGui::BeginTable("players", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp,
                                  { 0, 300 })) {
                ImGui::TableSetupColumn("NAME", ImGuiTableColumnFlags_WidthStretch, 3.0f);
                ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthStretch, 0.6f);
                ImGui::TableSetupColumn("WHERE", ImGuiTableColumnFlags_WidthStretch, 1.4f);
                ImGui::TableSetupColumn("AVATAR", ImGuiTableColumnFlags_WidthStretch, 1.2f);
                ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableHeadersRow();
                for (const auto& p : s.players) {
                    ImGui::TableNextRow(ImGuiTableRowFlags_None, 40.0f);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(p.name.c_str());
                    if (p.isLocal) {
                        ImGui::SameLine();
                        ImGui::TextColored(Rgb(t.accentAlt), "YOU");
                    }
                    if (p.isHost) {
                        ImGui::SameLine();
                        ImGui::TextColored(Rgb(t.accent), "HOST");
                    }
                    ImGui::TableNextColumn();
                    ImGui::Text("%u", p.id);
                    ImGui::TableNextColumn();
                    ImGui::TextColored(Rgb(p.isLocal || p.sameLocation ? t.good : t.textDim), "%s",
                                       p.isLocal ? "-" : p.sameLocation ? "WITH YOU" : "ELSEWHERE");
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", p.isLocal ? "-" : p.proxy.c_str());
                    ImGui::TableNextColumn();
                    if (s.hosting && !p.isLocal) {
                        ImGui::PushID(static_cast<int>(p.id));
                        if (Tile("KICK", { 96, 32 }, t.bad)) Emit("kick", { { "player", std::to_string(p.id) } }, emit);
                        ImGui::PopID();
                    }
                }
                ImGui::EndTable();
            }

            ImGui::Spacing();
            SectionHeader("INVITE", t);
            ImGui::TextDisabled("Join code");
            ImGui::SameLine();
            ImGui::Text("%s", s.joinCode.empty() ? "(host a game to get a code)" : s.joinCode.c_str());
            const bool haveCode = !s.joinCode.empty();
            if (Tile("COPY JOIN CODE", { 220, 64 }, t.accentAlt, haveCode)) Emit("copyJoinCode", {}, emit);
            ImGui::SameLine();
            if (Tile("INVITE FRIENDS (STEAM)", { 280, 64 }, t.accent, haveCode && s.steamAvailable)) {
                Emit("invite", {}, emit);
            }
            if (!s.steamAvailable) ImGui::TextDisabled("Steam overlay not available");
        }
    }

    void Draw(const ui::UiSnapshot& s, const std::function<void(ui::UiCommand)>& emit)
    {
        const auto& t = s.theme;
        ApplyStyle(t);
        const auto& io = ImGui::GetIO();
        const ImVec2 size{ std::min(1100.0f, io.DisplaySize.x * 0.8f), std::min(720.0f, io.DisplaySize.y * 0.8f) };
        ImGui::SetNextWindowSize(size, ImGuiCond_Always);
        ImGui::SetNextWindowPos({ io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f }, ImGuiCond_Always, { 0.5f, 0.5f });
        ImGui::Begin("CoSync", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
        // Header: accent bar + title, Metro-style uppercase tabs.
        auto* draw = ImGui::GetWindowDrawList();
        const auto pos = ImGui::GetWindowPos();
        draw->AddRectFilled(pos, { pos.x + 8.0f, pos.y + size.y }, ImGui::GetColorU32(Rgb(t.accent)));
        ImGui::SetWindowFontScale(1.6f);
        ImGui::TextUnformatted("COSYNC");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::SameLine();
        ImGui::TextDisabled("  multiplayer for Starfield   (F7 to close)");
        ImGui::Spacing();
        const char* tabs[] = { "MULTIPLAYER", "LOBBY" };
        for (int i = 0; i < 2; ++i) {
            if (i) ImGui::SameLine(0, t.spacing * 2);
            ImGui::PushStyleColor(ImGuiCol_Text, Rgb(f.tab == i ? t.text : t.textDim));
            ImGui::SetWindowFontScale(1.3f);
            if (ImGui::Selectable(tabs[i], f.tab == i, 0, ImGui::CalcTextSize(tabs[i]))) f.tab = i;
            ImGui::SetWindowFontScale(1.0f);
            ImGui::PopStyleColor();
        }
        ImGui::Separator();
        ImGui::Spacing();
        if (f.tab == 0) DrawMultiplayer(s, emit);
        else DrawLobby(s, emit);

        const std::string& error = !f.localError.empty() ? f.localError : s.lastError;
        if (!error.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(Rgb(t.bad), "%s", error.c_str());
        }
        ImGui::End();
    }
}
