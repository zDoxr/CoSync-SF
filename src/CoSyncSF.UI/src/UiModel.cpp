#include "cosync/ui/UiModel.h"

#include "cosync/net/Transport.h"
#include "cosync/shared/Protocol.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>

namespace cosync::ui
{
    namespace
    {
        std::string_view Arg(const IntentArgs& args, std::string_view key)
        {
            const auto it = args.find(key);
            return it == args.end() ? std::string_view{} : std::string_view{ it->second };
        }

        template <class T>
        bool ParseUnsigned(std::string_view text, T& out)
        {
            unsigned long long v = 0;
            const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
            if (ec != std::errc{} || ptr != text.data() + text.size() || v > std::numeric_limits<T>::max()) {
                return false;
            }
            out = static_cast<T>(v);
            return true;
        }

        // The transport only accepts numeric addresses (README "Known limitations").
        bool IsNumericHost(std::string_view host)
        {
            if (host.find(':') != std::string_view::npos) {  // IPv6: hex digits, ':' and '.' only
                return std::ranges::all_of(host, [](char c) {
                    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == ':' ||
                           c == '.';
                });
            }
            int parts = 0;
            while (true) {  // IPv4: exactly four decimal parts 0-255
                const auto dot = host.find('.');
                const auto part = host.substr(0, dot);
                unsigned v = 0;
                const auto [ptr, ec] = std::from_chars(part.data(), part.data() + part.size(), v);
                if (part.empty() || part.size() > 3 || ec != std::errc{} || ptr != part.data() + part.size() || v > 255) {
                    return false;
                }
                ++parts;
                if (dot == std::string_view::npos) break;
                host.remove_prefix(dot + 1);
            }
            return parts == 4;
        }

        std::string_view Trim(std::string_view s)
        {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
            return s;
        }
    }

    std::string_view ToString(Tab tab) noexcept
    {
        switch (tab) {
        case Tab::Session: return "session";
        case Tab::Browser: return "browser";
        case Tab::Players: return "players";
        case Tab::Diagnostics: return "diagnostics";
        case Tab::Settings: return "settings";
        }
        return "session";
    }

    std::optional<UiCommand> ParseIntent(std::string_view intent, const IntentArgs& args, std::string& error)
    {
        error.clear();
        const auto password = Arg(args, "password");
        if (!proto::IsValidText(password, proto::kMaxPasswordBytes)) {
            error = "Password is too long or not valid text.";
            return std::nullopt;
        }

        if (intent == "host") {
            cmd::HostGame c;
            c.serverName = std::string(Arg(args, "name"));
            if (c.serverName.empty()) c.serverName = "CoSync-SF Session";
            if (!proto::IsValidText(c.serverName, proto::kMaxServerNameBytes)) {
                error = "Server name must be 1-64 bytes of valid text.";
                return std::nullopt;
            }
            if (const auto port = Arg(args, "port"); !port.empty() && (!ParseUnsigned(port, c.port) || c.port == 0)) {
                error = "Port must be a number from 1 to 65535.";
                return std::nullopt;
            }
            if (const auto max = Arg(args, "maxPlayers"); !max.empty() &&
                (!ParseUnsigned(max, c.maxPlayers) || c.maxPlayers < 2 || c.maxPlayers > proto::kMaxPlayersLimit)) {
                error = std::format("Max players must be from 2 to {}.", proto::kMaxPlayersLimit);
                return std::nullopt;
            }
            c.password = std::string(password);
            return c;
        }
        if (intent == "join") {
            const auto address = Trim(Arg(args, "address"));
            const auto endpoint = net::Endpoint::Parse(address, 37015);
            const bool steamPeer = endpoint && endpoint->host.starts_with("steam:");
            if (!endpoint || (!steamPeer && !IsNumericHost(endpoint->host))) {
                error = "Join code must be steam:<SteamID> or a numeric IP with optional :port (e.g. 192.168.1.20:37015).";
                return std::nullopt;
            }
            return cmd::JoinGame{ endpoint->ToString(), std::string(password) };
        }
        if (intent == "leave") return cmd::LeaveGame{};
        if (intent == "refreshServers") return cmd::RefreshServers{};
        if (intent == "invite") return cmd::InviteFriends{};
        {
            using A = proto::GroupAction;
            const std::pair<std::string_view, A> groupIntents[] = {
                { "groupCreate", A::Create }, { "groupInvite", A::Invite }, { "groupAccept", A::Accept },
                { "groupDecline", A::Decline }, { "groupLeave", A::Leave }, { "groupKick", A::Kick },
            };
            for (const auto& [name, action] : groupIntents) {
                if (intent != name) continue;
                cmd::Group c{ action };
                if (action == A::Invite || action == A::Kick) {
                    if (!ParseUnsigned(Arg(args, "player"), c.target) || c.target == kInvalidPlayerId) {
                        error = "Unknown player.";
                        return std::nullopt;
                    }
                }
                if (action == A::Accept || action == A::Decline) {
                    if (!ParseUnsigned(Arg(args, "group"), c.group) || c.group == 0) {
                        error = "Unknown group.";
                        return std::nullopt;
                    }
                }
                return c;
            }
        }
        if (intent == "copyJoinCode") return cmd::CopyJoinCode{};
        if (intent == "kick") {
            cmd::KickPlayer c;
            if (!ParseUnsigned(Arg(args, "player"), c.player) || c.player == kInvalidPlayerId) {
                error = "Unknown player.";
                return std::nullopt;
            }
            return c;
        }
        if (intent == "setName") {
            const auto name = Trim(Arg(args, "name"));
            if (!proto::IsValidPlayerName(name)) {
                error = "Display name must be 1-32 bytes of valid text.";
                return std::nullopt;
            }
            return cmd::SetDisplayName{ std::string(name) };
        }
        if (intent == "selectTab") {
            const auto t = Arg(args, "tab");
            for (const auto tab : { Tab::Session, Tab::Browser, Tab::Players, Tab::Diagnostics, Tab::Settings }) {
                if (t == ToString(tab)) return cmd::SelectTab{ tab };
            }
            error = "Unknown tab.";
            return std::nullopt;
        }
        if (intent == "setTheme") {
            const auto name = Trim(Arg(args, "name"));
            if (name.empty() || !proto::IsValidText(name, 64)) {
                error = "Unknown theme.";
                return std::nullopt;
            }
            return cmd::SetTheme{ std::string(name) };
        }
        error = "Unknown action.";
        return std::nullopt;
    }

    Theme ParseTheme(std::string_view text, std::string& warnings)
    {
        Theme t;
        warnings.clear();
        auto warn = [&](std::size_t line, std::string_view what) {
            warnings += std::format("line {}: {}\n", line, what);
        };
        std::size_t lineNo = 0;
        while (!text.empty()) {
            ++lineNo;
            const auto nl = text.find('\n');
            auto line = Trim(text.substr(0, nl));
            text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
            if (line.empty() || line.front() == '#' || line.front() == ';') continue;
            const auto eq = line.find('=');
            if (eq == std::string_view::npos) {
                warn(lineNo, "expected key = value");
                continue;
            }
            const auto key = Trim(line.substr(0, eq));
            const auto value = Trim(line.substr(eq + 1));

            auto colour = [&](std::uint32_t& out) {
                if (value.size() != 7 || value.front() != '#') {
                    warn(lineNo, "colour must be #RRGGBB");
                    return;
                }
                std::uint32_t v = 0;
                const auto [ptr, ec] = std::from_chars(value.data() + 1, value.data() + 7, v, 16);
                if (ec != std::errc{} || ptr != value.data() + 7) {
                    warn(lineNo, "colour must be #RRGGBB");
                    return;
                }
                out = v;
            };
            auto number = [&](float& out, float lo, float hi) {
                float v = 0;
                const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), v);
                if (ec != std::errc{} || ptr != value.data() + value.size() || !std::isfinite(v) || v < lo || v > hi) {
                    warn(lineNo, std::format("number out of range [{}, {}]", lo, hi));
                    return;
                }
                out = v;
            };

            if (key == "name") {
                if (value.empty() || !proto::IsValidText(value, 64)) warn(lineNo, "invalid name");
                else t.name = std::string(value);
            } else if (key == "background") colour(t.background);
            else if (key == "panel") colour(t.panel);
            else if (key == "panelAlt") colour(t.panelAlt);
            else if (key == "text") colour(t.text);
            else if (key == "textDim") colour(t.textDim);
            else if (key == "accent") colour(t.accent);
            else if (key == "accentAlt") colour(t.accentAlt);
            else if (key == "good") colour(t.good);
            else if (key == "warn") colour(t.warn);
            else if (key == "bad") colour(t.bad);
            else if (key == "panelOpacity") number(t.panelOpacity, 0.0f, 1.0f);
            else if (key == "cornerRadius") number(t.cornerRadius, 0.0f, 32.0f);
            else if (key == "spacing") number(t.spacing, 2.0f, 64.0f);
            else if (key == "fontScale") number(t.fontScale, 0.5f, 3.0f);
            else if (key == "transitionMs") {
                std::uint32_t v = 0;
                if (!ParseUnsigned(value, v) || v > 2000) warn(lineNo, "transitionMs must be 0-2000");
                else t.transitionMs = v;
            } else {
                warn(lineNo, std::format("unknown key '{}'", key));
            }
        }
        return t;
    }

    // ---- JSON -----------------------------------------------------------------
    namespace
    {
        void Str(std::string& o, std::string_view s)
        {
            o += '"';
            for (const char ch : s) {
                const auto c = static_cast<unsigned char>(ch);
                switch (c) {
                case '"': o += "\\\""; break;
                case '\\': o += "\\\\"; break;
                case '\n': o += "\\n"; break;
                case '\r': o += "\\r"; break;
                case '\t': o += "\\t"; break;
                default:
                    if (c < 0x20) o += std::format("\\u{:04x}", c);
                    else o += ch;  // UTF-8 passes through (inputs are validated text)
                }
            }
            o += '"';
        }
        void Key(std::string& o, std::string_view k)
        {
            if (!o.empty() && o.back() != '{' && o.back() != '[') o += ',';
            Str(o, k);
            o += ':';
        }
        void Colour(std::string& o, std::string_view k, std::uint32_t c)
        {
            Key(o, k);
            Str(o, std::format("#{:06X}", c & 0xFFFFFF));
        }
        std::string_view StateName(net::ClientState s) { return net::ToString(s); }
    }

    std::string ToJson(const UiSnapshot& s)
    {
        std::string o = "{";
        Key(o, "version"); o += std::to_string(s.version);
        Key(o, "tab"); Str(o, ToString(s.tab));
        Key(o, "connection"); o += '{';
        Key(o, "state"); Str(o, StateName(s.state));
        Key(o, "detail"); Str(o, s.stateDetail);
        Key(o, "hosting"); o += s.hosting ? "true" : "false";
        Key(o, "serverName"); Str(o, s.serverName);
        Key(o, "address"); Str(o, s.address);
        Key(o, "localPlayer"); o += std::to_string(s.localPlayer);
        Key(o, "rttMs"); o += std::to_string(s.rttMs);
        Key(o, "droppedEvents"); o += std::to_string(s.droppedEvents);
        Key(o, "protocol"); Str(o, s.protocolVersion);
        Key(o, "game"); Str(o, s.gameVersion);
        o += '}';
        Key(o, "players"); o += '[';
        for (const auto& p : s.players) {
            if (o.back() != '[') o += ',';
            o += '{';
            Key(o, "id"); o += std::to_string(p.id);
            Key(o, "name"); Str(o, p.name);
            Key(o, "local"); o += p.isLocal ? "true" : "false";
            Key(o, "host"); o += p.isHost ? "true" : "false";
            Key(o, "sameLocation"); o += p.sameLocation ? "true" : "false";
            Key(o, "proxy"); Str(o, p.proxy);
            o += '}';
        }
        o += ']';
        Key(o, "servers"); o += '[';
        for (const auto& sv : s.servers) {
            if (o.back() != '[') o += ',';
            o += '{';
            Key(o, "name"); Str(o, sv.name);
            Key(o, "address"); Str(o, sv.address);
            Key(o, "players"); o += std::to_string(sv.players);
            Key(o, "maxPlayers"); o += std::to_string(sv.maxPlayers);
            Key(o, "pingMs"); o += std::to_string(sv.pingMs);
            Key(o, "passworded"); o += sv.passworded ? "true" : "false";
            Key(o, "game"); Str(o, sv.gameVersion);
            o += '}';
        }
        o += ']';
        Key(o, "diagnostics"); o += '[';
        for (const auto& d : s.diagnostics) {
            if (o.back() != '[') o += ',';
            Str(o, d);
        }
        o += ']';
        Key(o, "displayName"); Str(o, s.displayName);
        Key(o, "lastError"); Str(o, s.lastError);
        Key(o, "joinCode"); Str(o, s.joinCode);
        Key(o, "steamAvailable"); o += s.steamAvailable ? "true" : "false";
        Key(o, "lanCode"); Str(o, s.lanCode);
        const auto& t = s.theme;
        Key(o, "theme"); o += '{';
        Key(o, "name"); Str(o, t.name);
        Colour(o, "background", t.background);
        Colour(o, "panel", t.panel);
        Colour(o, "panelAlt", t.panelAlt);
        Colour(o, "text", t.text);
        Colour(o, "textDim", t.textDim);
        Colour(o, "accent", t.accent);
        Colour(o, "accentAlt", t.accentAlt);
        Colour(o, "good", t.good);
        Colour(o, "warn", t.warn);
        Colour(o, "bad", t.bad);
        Key(o, "panelOpacity"); o += std::format("{:.3f}", t.panelOpacity);
        Key(o, "cornerRadius"); o += std::format("{:.2f}", t.cornerRadius);
        Key(o, "spacing"); o += std::format("{:.2f}", t.spacing);
        Key(o, "fontScale"); o += std::format("{:.3f}", t.fontScale);
        Key(o, "transitionMs"); o += std::to_string(t.transitionMs);
        o += '}';
        o += '}';
        return o;
    }

    // ---- Model ----------------------------------------------------------------
    void UiModel::SetConnection(net::ClientState state, std::string detail, bool hosting, std::string serverName,
                                std::string address, PlayerId localPlayer, std::uint32_t rttMs, std::uint64_t dropped)
    {
        auto& s = _snapshot;
        if (s.state == state && s.stateDetail == detail && s.hosting == hosting && s.serverName == serverName &&
            s.address == address && s.localPlayer == localPlayer && s.rttMs == rttMs && s.droppedEvents == dropped) {
            return;
        }
        s.state = state;
        s.stateDetail = std::move(detail);
        s.hosting = hosting;
        s.serverName = std::move(serverName);
        s.address = std::move(address);
        s.localPlayer = localPlayer;
        s.rttMs = rttMs;
        s.droppedEvents = dropped;
        Touch();
    }

    void UiModel::SetVersions(std::string protocolVersion, std::string gameVersion)
    {
        if (_snapshot.protocolVersion == protocolVersion && _snapshot.gameVersion == gameVersion) return;
        _snapshot.protocolVersion = std::move(protocolVersion);
        _snapshot.gameVersion = std::move(gameVersion);
        Touch();
    }

    void UiModel::SetPlayers(std::vector<PlayerRow> players)
    {
        std::ranges::sort(players, {}, &PlayerRow::id);
        if (_snapshot.players == players) return;
        _snapshot.players = std::move(players);
        Touch();
    }

    void UiModel::SetServers(std::vector<ServerRow> servers)
    {
        if (_snapshot.servers == servers) return;
        _snapshot.servers = std::move(servers);
        Touch();
    }

    void UiModel::AddDiagnostic(std::string line)
    {
        auto& d = _snapshot.diagnostics;
        d.push_back(std::move(line));
        if (d.size() > kMaxDiagnostics) d.erase(d.begin(), d.begin() + static_cast<std::ptrdiff_t>(d.size() - kMaxDiagnostics));
        Touch();
    }

    void UiModel::SetDisplayName(std::string name)
    {
        if (_snapshot.displayName == name) return;
        _snapshot.displayName = std::move(name);
        Touch();
    }

    void UiModel::SetTheme(Theme theme)
    {
        if (_snapshot.theme == theme) return;
        _snapshot.theme = std::move(theme);
        Touch();
    }

    void UiModel::SetError(std::string error)
    {
        if (_snapshot.lastError == error) return;
        _snapshot.lastError = std::move(error);
        Touch();
    }

    void UiModel::SetJoinCode(std::string code, bool steamAvailable)
    {
        if (_snapshot.joinCode == code && _snapshot.steamAvailable == steamAvailable) return;
        _snapshot.joinCode = std::move(code);
        _snapshot.steamAvailable = steamAvailable;
        Touch();
    }

    void UiModel::SetGroup(std::uint32_t group, PlayerId leader, std::vector<PlayerId> invited,
                           std::vector<UiSnapshot::GroupInvite> invites)
    {
        auto& s = _snapshot;
        if (s.myGroup == group && s.myGroupLeader == leader && s.myGroupInvited == invited && s.invites == invites) return;
        s.myGroup = group;
        s.myGroupLeader = leader;
        s.myGroupInvited = std::move(invited);
        s.invites = std::move(invites);
        Touch();
    }

    void UiModel::SetLanCode(std::string code)
    {
        if (_snapshot.lanCode == code) return;
        _snapshot.lanCode = std::move(code);
        Touch();
    }

    bool UiModel::ApplyLocal(const UiCommand& command)
    {
        if (const auto* sel = std::get_if<cmd::SelectTab>(&command)) {
            if (_snapshot.tab != sel->tab) {
                _snapshot.tab = sel->tab;
                Touch();
            }
            return true;
        }
        return false;
    }
}
