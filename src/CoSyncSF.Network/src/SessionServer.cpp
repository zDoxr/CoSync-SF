#include "cosync/net/SessionServer.h"

#include "cosync/shared/GameRuntime.h"
#include "cosync/shared/Log.h"

#include <algorithm>
#include <cstring>
#include <format>

namespace cosync::net
{
    namespace
    {
        bool ConstantTimeEquals(std::string_view a, std::string_view b)
        {
            const std::size_t n = std::max(a.size(), b.size());
            std::uint8_t diff = a.size() == b.size() ? 0 : 1;
            for (std::size_t i = 0; i < n; ++i) {
                const auto ca = i < a.size() ? static_cast<std::uint8_t>(a[i]) : 0;
                const auto cb = i < b.size() ? static_cast<std::uint8_t>(b[i]) : 0;
                diff |= static_cast<std::uint8_t>(ca ^ cb);
            }
            return diff == 0;
        }

        bool EqualsIgnoreAsciiCase(std::string_view a, std::string_view b)
        {
            return std::ranges::equal(a, b, [](char x, char y) {
                const auto lx = (x >= 'A' && x <= 'Z') ? static_cast<char>(x - 'A' + 'a') : x;
                const auto ly = (y >= 'A' && y <= 'Z') ? static_cast<char>(y - 'A' + 'a') : y;
                return lx == ly;
            });
        }
    }

    SessionServer::SessionServer(ITransport& transport, ServerConfig config, Clock clock) :
        _transport(transport), _config(std::move(config)), _clock(std::move(clock))
    {
        _config.maxPlayers = std::clamp<std::uint16_t>(_config.maxPlayers, 1, proto::kMaxPlayersLimit);
        std::uint8_t bytes[sizeof(SessionId)]{};
        if (!FillRandom(bytes)) {
            log::Warn("secure RNG unavailable for session id; falling back to clock");
            const auto t = _clock();
            std::memcpy(bytes, &t, sizeof bytes);
        }
        std::memcpy(&_sessionId, bytes, sizeof _sessionId);
    }

    SessionServer::~SessionServer()
    {
        if (_running) {
            Stop();
        }
    }

    bool SessionServer::Start(std::uint16_t port, std::string& error)
    {
        if (_running) {
            error = "already running";
            return false;
        }
        if (!_transport.Listen(port, error)) {
            return false;
        }
        _running = true;
        log::Info("session {:016x} '{}' listening on port {} (max {} players, protocol {}.{}, game runtime {})",
                  _sessionId, _config.serverName, port, _config.maxPlayers, proto::kProtocolMajor,
                  proto::kProtocolMinor,
                  _config.requiredGameRuntime ? game::FormatRuntimeVersion(_config.requiredGameRuntime) : "any");
        return true;
    }

    void SessionServer::Update()
    {
        _events.clear();
        _transport.Poll(_events);
        for (auto& ev : _events) {
            switch (ev.type) {
            case TransportEventType::Connected: OnConnected(ev.connection); break;
            case TransportEventType::Disconnected: OnDisconnected(ev); break;
            case TransportEventType::Message: OnMessage(ev.connection, ev.data); break;
            }
        }
        CheckTimers();
    }

    void SessionServer::Stop()
    {
        if (!_running) {
            return;
        }
        _running = false;
        const proto::Disconnect bye{ proto::DisconnectReason::ServerShutdown, "server shutting down" };
        std::vector<ConnectionId> ids;
        for (const auto& [id, conn] : _connections) {
            ids.push_back(id);
        }
        for (auto id : ids) {
            auto& conn = _connections.at(id);
            if (conn.player != kInvalidPlayerId) {
                Send(id, bye);
            } else {
                Send(id, proto::Reject{ proto::RejectReason::ServerShuttingDown });
            }
            _transport.Close(id, CloseCode::ServerShutdown, "server shutting down", true);
        }
        _connections.clear();
        _players.clear();
        log::Info("session {:016x} stopped", _sessionId);
    }

    bool SessionServer::Kick(PlayerId player, std::string_view reason)
    {
        const auto it = _players.find(player);
        if (it == _players.end()) {
            return false;
        }
        if (it->second.connection != kInvalidConnection) {
            const auto conn = it->second.connection;
            Send(conn, proto::Disconnect{ proto::DisconnectReason::Kicked, std::string(reason.substr(0, proto::kMaxDetailBytes)) });
            DropConnection(conn, CloseCode::Normal, "kicked", proto::LeaveReason::Kicked);
        } else {
            RemovePlayer(player, proto::LeaveReason::Kicked);
        }
        return true;
    }

    std::vector<PlayerInfo> SessionServer::Players() const
    {
        std::vector<PlayerInfo> out;
        for (const auto& [id, p] : _players) {
            out.push_back({ id, p.name, p.connection != kInvalidConnection, p.gameRuntime });
        }
        std::ranges::sort(out, {}, &PlayerInfo::id);
        return out;
    }

    std::size_t SessionServer::ConnectedPlayerCount() const
    {
        return static_cast<std::size_t>(std::ranges::count_if(
            _players, [](const auto& kv) { return kv.second.connection != kInvalidConnection; }));
    }

    std::size_t SessionServer::PendingConnectionCount() const
    {
        return static_cast<std::size_t>(
            std::ranges::count_if(_connections, [](const auto& kv) { return kv.second.player == kInvalidPlayerId; }));
    }

    void SessionServer::OnConnected(ConnectionId id)
    {
        if (!_running) {
            _transport.Close(id, CloseCode::ServerShutdown, "server not running", false);
            return;
        }
        if (PendingConnectionCount() >= _config.maxPendingConnections) {
            log::Warn("refusing {}: too many pending handshakes", _transport.Describe(id));
            _transport.Close(id, CloseCode::RateLimited, "too many pending connections", false);
            return;
        }
        const auto now = _clock();
        _connections[id] = Connection{ id, now, now, kInvalidPlayerId,
                                       TokenBucket(_config.maxMessagesPerSecond, _config.messageBurst, now) };
        log::Debug("connection {} opened", _transport.Describe(id));
    }

    void SessionServer::OnDisconnected(const TransportEvent& ev)
    {
        const auto it = _connections.find(ev.connection);
        if (it == _connections.end()) {
            return;  // already handled (we closed it ourselves)
        }
        const auto playerId = it->second.player;
        _connections.erase(it);
        if (playerId == kInvalidPlayerId) {
            return;
        }
        auto pit = _players.find(playerId);
        if (pit == _players.end() || pit->second.connection != ev.connection) {
            return;
        }
        log::Info("player {} '{}' lost connection ({}: {})", playerId, pit->second.name, ev.closeCode, ev.closeReason);
        ReservePlayer(pit->second);
    }

    void SessionServer::OnMessage(ConnectionId id, std::span<const std::uint8_t> data)
    {
        const auto cit = _connections.find(id);
        if (cit == _connections.end()) {
            return;
        }
        auto& conn = cit->second;
        const auto now = _clock();
        ++_stats.messagesReceived;

        if (!conn.bucket.TryTake(now)) {
            ++_stats.rateLimitDisconnects;
            log::Warn("{} exceeded message rate limit", _transport.Describe(id));
            if (conn.player == kInvalidPlayerId) {
                RejectAndClose(id, proto::RejectReason::RateLimited, "message rate limit exceeded");
            } else {
                DropConnection(id, CloseCode::RateLimited, "message rate limit exceeded", proto::LeaveReason::Kicked);
            }
            return;
        }
        conn.lastReceived = now;

        auto decoded = proto::Decode(data);
        if (!decoded.Ok()) {
            ++_stats.decodeFailures;
            ++_stats.protocolViolations;
            const auto detail = std::format("malformed message ({})", proto::ToString(decoded.error));
            log::Warn("{}: {}", _transport.Describe(id), detail);
            if (conn.player == kInvalidPlayerId) {
                RejectAndClose(id, proto::RejectReason::ProtocolViolation, detail);
            } else {
                DropConnection(id, CloseCode::ProtocolViolation, detail, proto::LeaveReason::ProtocolViolation);
            }
            return;
        }

        auto& msg = *decoded.message;
        const auto type = proto::TypeOf(msg);

        if (conn.player == kInvalidPlayerId) {
            if (type != proto::MessageType::Hello) {
                ++_stats.protocolViolations;
                RejectAndClose(id, proto::RejectReason::ProtocolViolation,
                               std::format("expected Hello, got {}", proto::ToString(type)));
                return;
            }
            OnHello(conn, std::get<proto::Hello>(msg));
            return;
        }

        auto& player = _players.at(conn.player);
        switch (type) {
        case proto::MessageType::Heartbeat: {
            const auto& hb = std::get<proto::Heartbeat>(msg);
            Send(id, proto::HeartbeatAck{ hb.sequence, hb.sendTimeMs });
            break;
        }
        case proto::MessageType::PlayerTransform:
            OnTransform(conn, player, std::get<proto::PlayerTransform>(msg));
            break;
        case proto::MessageType::GroupRequest: {
            std::string why;
            if (_groups.Apply(player.id, std::get<proto::GroupRequest>(msg), why)) {
                auto state = _groups.State();
                for (const auto& [pid, p] : _players) {
                    if (p.connection != kInvalidConnection) Send(p.connection, state);
                }
            } else {
                log::Info("group request from player {} refused: {}", player.id, why);
            }
            break;
        }
        case proto::MessageType::NpcCombat: {
            auto m = std::get<proto::NpcCombat>(msg);
            if (m.playerId != kInvalidPlayerId && m.playerId != player.id) {
                ++_stats.protocolViolations;
                DropConnection(id, CloseCode::ProtocolViolation, "NpcCombat for another player",
                               proto::LeaveReason::ProtocolViolation);
                break;
            }
            m.playerId = player.id;  // server stamps the reporter
            Broadcast(m, player.id, Delivery::Reliable);
            break;
        }
        case proto::MessageType::NpcStates:
        case proto::MessageType::NpcDeath:
        case proto::MessageType::NpcDamage:
        case proto::MessageType::NpcEvents:
        case proto::MessageType::NpcOutfits:
            OnNpcMessage(conn, player, msg);
            break;
        case proto::MessageType::WorldEvents:
        case proto::MessageType::WorldClock:
            OnWorldMessage(conn, player, msg);
            break;
        case proto::MessageType::Disconnect:
            log::Info("player {} '{}' left", player.id, player.name);
            DropConnection(id, CloseCode::Normal, "client quit", proto::LeaveReason::Quit);
            break;
        default:
            // Hello after registration, or a server-to-client message type.
            ++_stats.protocolViolations;
            log::Warn("player {} sent unexpected {}", player.id, proto::ToString(type));
            DropConnection(id, CloseCode::ProtocolViolation,
                           std::format("unexpected {}", proto::ToString(type)), proto::LeaveReason::ProtocolViolation);
            break;
        }
    }

    void SessionServer::OnHello(Connection& conn, const proto::Hello& hello)
    {
        const auto id = conn.id;
        if (hello.protocolMajor != proto::kProtocolMajor) {
            RejectAndClose(id, proto::RejectReason::ProtocolVersionMismatch,
                           std::format("server speaks protocol {}.{}, client {}.{}", proto::kProtocolMajor,
                                       proto::kProtocolMinor, hello.protocolMajor, hello.protocolMinor));
            return;
        }
        if (_config.requiredGameRuntime != 0 && hello.gameRuntime != _config.requiredGameRuntime) {
            RejectAndClose(id, proto::RejectReason::GameVersionMismatch,
                           std::format("server requires Starfield {}, client runs {}",
                                       game::FormatRuntimeVersion(_config.requiredGameRuntime),
                                       game::FormatRuntimeVersion(hello.gameRuntime)));
            return;
        }
        if (!ConstantTimeEquals(hello.password, _config.password)) {
            RejectAndClose(id, proto::RejectReason::BadPassword, "incorrect session password");
            return;
        }

        if (!hello.resumeToken.IsZero()) {
            if (auto* player = FindByToken(hello.resumeToken)) {
                const bool wasReserved = player->connection == kInvalidConnection;
                if (!wasReserved) {
                    // The old connection is half-open; the new one supersedes it.
                    const auto old = player->connection;
                    _connections.at(old).player = kInvalidPlayerId;
                    _connections.erase(old);
                    _transport.Close(old, CloseCode::Replaced, "superseded by resumed connection", false);
                }
                player->gameRuntime = hello.gameRuntime;
                log::Info("player {} '{}' resumed from {}", player->id, player->name, _transport.Describe(id));
                Welcome(conn, *player, true);
                if (wasReserved) {
                    Broadcast(proto::PlayerJoined{ { player->id, player->name } }, player->id);
                }
                return;
            }
            // Unknown or expired token: fall through to a fresh join.
        }

        if (!proto::IsValidPlayerName(hello.playerName)) {
            RejectAndClose(id, proto::RejectReason::InvalidName, "name must be 1-32 printable UTF-8 bytes");
            return;
        }
        if (NameTaken(hello.playerName)) {
            RejectAndClose(id, proto::RejectReason::NameInUse, "that name is already in this session");
            return;
        }
        if (_players.size() >= _config.maxPlayers) {
            RejectAndClose(id, proto::RejectReason::ServerFull, "session is full");
            return;
        }

        Player player;
        player.id = _nextPlayerId++;
        player.name = hello.playerName;
        player.gameRuntime = hello.gameRuntime;
        auto& stored = _players.emplace(player.id, std::move(player)).first->second;
        log::Info("player {} '{}' joined from {} (build '{}', game {})", stored.id, stored.name, _transport.Describe(id),
                  hello.clientBuild, game::FormatRuntimeVersion(hello.gameRuntime));
        Welcome(conn, stored, false);
        Broadcast(proto::PlayerJoined{ { stored.id, stored.name } }, stored.id);
    }

    void SessionServer::Welcome(Connection& conn, Player& player, bool resumed)
    {
        player.connection = conn.id;
        player.reservedUntil = 0;
        player.token = NewToken();  // rotate on every (re)registration
        player.hasTransform = false;
        conn.player = player.id;

        proto::Welcome welcome;
        welcome.playerId = player.id;
        welcome.sessionId = _sessionId;
        welcome.resumeToken = player.token;
        welcome.serverName = _config.serverName.substr(0, proto::kMaxServerNameBytes);
        welcome.maxPlayers = _config.maxPlayers;
        welcome.heartbeatIntervalMs = _config.heartbeatIntervalMs;
        welcome.resumed = resumed;
        Send(conn.id, welcome);

        proto::SessionSnapshot snapshot;
        for (const auto& [id, other] : _players) {
            if (id != player.id && other.connection != kInvalidConnection) {
                snapshot.players.push_back({ id, other.name });
            }
        }
        std::ranges::sort(snapshot.players, {}, &proto::PlayerEntry::playerId);
        Send(conn.id, snapshot);
        Send(conn.id, _groups.State());

        ElectNpcAuthority();
        Send(conn.id, proto::NpcAuthority{ _npcAuthority });
        for (const auto npc : _deadNpcs) Send(conn.id, proto::NpcDeath{ _npcAuthority, npc });
        const auto history = _world.Snapshot();
        for (std::size_t at = 0; at < history.size(); at += proto::kMaxWorldEvents) {
            const auto end = std::min(at + proto::kMaxWorldEvents, history.size());
            Send(conn.id, proto::WorldEvents{ kInvalidPlayerId, { history.begin() + static_cast<std::ptrdiff_t>(at), history.begin() + static_cast<std::ptrdiff_t>(end) } });
        }
        if (_worldClock) Send(conn.id, *_worldClock);
    }

    void SessionServer::OnWorldMessage(Connection& conn, Player& player, proto::Message& msg)
    {
        const auto stamped = [&](auto& m) {
            if (m.playerId != kInvalidPlayerId && m.playerId != player.id) {
                ++_stats.protocolViolations;
                DropConnection(conn.id, CloseCode::ProtocolViolation, "world message for another player",
                               proto::LeaveReason::ProtocolViolation);
                return false;
            }
            m.playerId = player.id;
            return true;
        };
        if (auto* events = std::get_if<proto::WorldEvents>(&msg)) {
            if (!stamped(*events)) return;
            for (const auto& e : events->events) {
                if (!_world.Apply(e)) ++_stats.worldEventsNotRemembered;
            }
            Broadcast(*events, player.id);
        } else if (auto* clock = std::get_if<proto::WorldClock>(&msg)) {
            if (!stamped(*clock)) return;
            // The authority's alone: one clock, or two copies would keep correcting each other.
            if (player.id != _npcAuthority) {
                ++_stats.npcMessagesRefused;
                return;
            }
            _worldClock = *clock;
            Broadcast(*clock, player.id);
        }
    }

    void SessionServer::OnTransform(Connection&, Player& player, proto::PlayerTransform transform)
    {
        if (transform.playerId != kInvalidPlayerId && transform.playerId != player.id) {
            ++_stats.protocolViolations;
            log::Warn("player {} tried to send a transform for player {}", player.id, transform.playerId);
            DropConnection(player.connection, CloseCode::ProtocolViolation, "transform for another player",
                           proto::LeaveReason::ProtocolViolation);
            return;
        }
        if (player.hasTransform && !proto::SequenceNewer(transform.sequence, player.lastTransformSequence)) {
            ++_stats.staleTransformsDropped;
            return;
        }
        player.hasTransform = true;
        player.lastTransformSequence = transform.sequence;
        transform.playerId = player.id;  // server stamps the authenticated identity
        Broadcast(transform, player.id, Delivery::Unreliable);
        ++_stats.transformsRelayed;
    }

    void SessionServer::CheckTimers()
    {
        const auto now = _clock();
        std::vector<ConnectionId> handshakeExpired;
        std::vector<ConnectionId> idle;
        for (const auto& [id, conn] : _connections) {
            if (conn.player == kInvalidPlayerId) {
                if (now - conn.openedAt > _config.handshakeTimeoutMs) handshakeExpired.push_back(id);
            } else if (now - conn.lastReceived > _config.idleTimeoutMs) {
                idle.push_back(id);
            }
        }
        for (auto id : handshakeExpired) {
            RejectAndClose(id, proto::RejectReason::HandshakeTimeout, "no Hello received in time");
        }
        for (auto id : idle) {
            log::Info("{} timed out", _transport.Describe(id));
            DropConnection(id, CloseCode::TimedOut, "heartbeat timeout", std::nullopt);
        }

        std::vector<PlayerId> expired;
        for (const auto& [id, p] : _players) {
            if (p.connection == kInvalidConnection && now >= p.reservedUntil) expired.push_back(id);
        }
        for (auto id : expired) {
            log::Info("player {} '{}' resume window expired", id, _players.at(id).name);
            _players.erase(id);  // PlayerLeft(ConnectionLost) was already broadcast at drop time
        }
    }

    void SessionServer::Send(ConnectionId id, const proto::Message& msg, Delivery delivery)
    {
        const auto bytes = proto::Encode(msg);
        _transport.Send(id, bytes, delivery);
    }

    void SessionServer::Broadcast(const proto::Message& msg, PlayerId except, Delivery delivery)
    {
        const auto bytes = proto::Encode(msg);
        for (const auto& [id, p] : _players) {
            if (id != except && p.connection != kInvalidConnection) {
                _transport.Send(p.connection, bytes, delivery);
            }
        }
    }

    void SessionServer::RejectAndClose(ConnectionId id, proto::RejectReason reason, std::string_view detail)
    {
        ++_stats.rejections;
        log::Info("rejecting {}: {} ({})", _transport.Describe(id), proto::ToString(reason), detail);
        proto::Reject reject;
        reject.reason = reason;
        reject.detail = std::string(detail.substr(0, proto::kMaxDetailBytes));
        Send(id, reject);
        _connections.erase(id);
        _transport.Close(id, CloseCode::Rejected, proto::ToString(reason), true);
    }

    void SessionServer::DropConnection(ConnectionId id, CloseCode code, std::string_view detail,
                                       std::optional<proto::LeaveReason> leaveReason)
    {
        const auto it = _connections.find(id);
        if (it == _connections.end()) {
            return;
        }
        const auto playerId = it->second.player;
        _connections.erase(it);
        _transport.Close(id, code, detail, code == CloseCode::Normal);

        if (playerId == kInvalidPlayerId) {
            return;
        }
        if (leaveReason) {
            RemovePlayer(playerId, *leaveReason);
        } else if (auto pit = _players.find(playerId); pit != _players.end()) {
            ReservePlayer(pit->second);
        }
    }

    void SessionServer::RemovePlayer(PlayerId id, proto::LeaveReason reason)
    {
        const auto it = _players.find(id);
        if (it == _players.end()) {
            return;
        }
        const bool wasConnected = it->second.connection != kInvalidConnection;
        _players.erase(it);
        if (wasConnected) {
            Broadcast(proto::PlayerLeft{ id, reason }, id);
        }
        if (_groups.RemovePlayer(id)) Broadcast(_groups.State(), id);
        ElectNpcAuthority();
    }

    void SessionServer::ElectNpcAuthority()
    {
        if (const auto it = _players.find(_npcAuthority);
            it != _players.end() && it->second.connection != kInvalidConnection) {
            return;
        }
        PlayerId next = kInvalidPlayerId;
        for (const auto& [id, p] : _players) {
            if (p.connection == kInvalidConnection || p.gameRuntime == 0) continue;  // tool clients have no NPCs
            if (next == kInvalidPlayerId || id < next) next = id;
        }
        if (next == _npcAuthority) return;
        _npcAuthority = next;
        log::Info("NPC authority: player {}", next);
        Broadcast(proto::NpcAuthority{ next }, kInvalidPlayerId);
    }

    void SessionServer::OnNpcMessage(Connection& conn, Player& player, proto::Message& msg)
    {
        const auto stamped = [&](auto& m) {
            if (m.playerId != kInvalidPlayerId && m.playerId != player.id) {
                ++_stats.protocolViolations;
                DropConnection(conn.id, CloseCode::ProtocolViolation, "NPC message for another player",
                               proto::LeaveReason::ProtocolViolation);
                return false;
            }
            m.playerId = player.id;
            return true;
        };
        if (auto* damage = std::get_if<proto::NpcDamage>(&msg)) {
            if (!stamped(*damage)) return;
            const auto authority = _players.find(_npcAuthority);
            if (authority == _players.end() || authority->first == player.id ||
                authority->second.connection == kInvalidConnection) {
                ++_stats.npcMessagesRefused;
                return;
            }
            Send(authority->second.connection, *damage);
            return;
        }
        // State and death: the authority's alone. A player that just lost the role can still have
        // messages in flight, so this is not a violation.
        if (player.id != _npcAuthority) {
            ++_stats.npcMessagesRefused;
            return;
        }
        if (auto* states = std::get_if<proto::NpcStates>(&msg)) {
            if (stamped(*states)) Broadcast(*states, player.id, Delivery::Unreliable);
        } else if (auto* events = std::get_if<proto::NpcEvents>(&msg)) {
            if (stamped(*events)) Broadcast(*events, player.id);
        } else if (auto* outfits = std::get_if<proto::NpcOutfits>(&msg)) {
            if (stamped(*outfits)) Broadcast(*outfits, player.id, Delivery::Unreliable);
        } else if (auto* death = std::get_if<proto::NpcDeath>(&msg)) {
            if (!stamped(*death)) return;
            constexpr std::size_t kMaxRememberedDeaths = 4096;
            if (_deadNpcs.size() < kMaxRememberedDeaths) _deadNpcs.insert(death->npcFormId);
            Broadcast(*death, player.id);
        }
    }

    void SessionServer::ReservePlayer(Player& player)
    {
        const PlayerId id = player.id;  // copy: `player` dies if erased below
        player.connection = kInvalidConnection;
        player.reservedUntil = _clock() + _config.resumeWindowMs;
        Broadcast(proto::PlayerLeft{ id, proto::LeaveReason::ConnectionLost }, id);
        if (_groups.RemovePlayer(id)) Broadcast(_groups.State(), id);
        if (_config.resumeWindowMs == 0) {
            _players.erase(id);
        }
        ElectNpcAuthority();
    }

    SessionServer::Player* SessionServer::FindByToken(const ResumeToken& token)
    {
        // Check every player so timing does not reveal which slot matched.
        Player* found = nullptr;
        for (auto& [id, p] : _players) {
            if (!p.token.IsZero() && p.token.ConstantTimeEquals(token)) {
                found = &p;
            }
        }
        return found;
    }

    bool SessionServer::NameTaken(std::string_view name) const
    {
        return std::ranges::any_of(_players, [&](const auto& kv) { return EqualsIgnoreAsciiCase(kv.second.name, name); });
    }

    ResumeToken SessionServer::NewToken()
    {
        ResumeToken token;
        if (!FillRandom(token.bytes)) {
            // Without a secure RNG tokens would be guessable. A zero token is never
            // accepted for resume (OnHello ignores it), so this disables resume.
            log::Error("secure RNG failure; resume disabled for this player");
            token.bytes.fill(0);
        }
        return token;
    }
}
