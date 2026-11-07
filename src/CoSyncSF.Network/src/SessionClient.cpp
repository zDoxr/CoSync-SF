#include "cosync/net/SessionClient.h"

#include "cosync/shared/Log.h"

#include <format>

namespace cosync::net
{
    std::string_view ToString(ClientState state) noexcept
    {
        switch (state) {
        case ClientState::Idle: return "idle";
        case ClientState::Connecting: return "connecting";
        case ClientState::Handshaking: return "handshaking";
        case ClientState::InSession: return "in-session";
        case ClientState::Reconnecting: return "reconnecting";
        case ClientState::Disconnected: return "disconnected";
        }
        return "?";
    }

    SessionClient::SessionClient(ITransport& transport, ClientConfig config, Clock clock) :
        _transport(transport), _config(std::move(config)), _clock(std::move(clock))
    {}

    SessionClient::~SessionClient()
    {
        if (_state == ClientState::InSession || _state == ClientState::Handshaking || _state == ClientState::Connecting) {
            Disconnect();
        }
    }

    bool SessionClient::Connect(const Endpoint& endpoint, std::string& error)
    {
        if (_state != ClientState::Idle && _state != ClientState::Disconnected) {
            error = std::format("cannot connect while {}", ToString(_state));
            return false;
        }
        if (!proto::IsValidPlayerName(_config.playerName)) {
            error = "invalid player name";
            return false;
        }
        _endpoint = endpoint;
        _resumeToken = {};
        _reconnectAttempts = 0;
        _roster.clear();
        _lastRemoteSequence.clear();
        if (!StartConnection(error)) {
            return false;
        }
        _state = ClientState::Connecting;
        _reportConnecting = true;  // StateChanged is reported by the next Update()
        return true;
    }

    bool SessionClient::StartConnection(std::string& error)
    {
        _connection = _transport.Connect(_endpoint, error);
        if (_connection == kInvalidConnection) {
            return false;
        }
        _stateSince = _clock();
        return true;
    }

    void SessionClient::Update(std::vector<ClientEvent>& events)
    {
        if (_reportConnecting) {
            _reportConnecting = false;
            events.emplace_back(client_event::StateChanged{ ClientState::Connecting, _endpoint.ToString() });
        }

        _transportEvents.clear();
        _transport.Poll(_transportEvents);
        for (auto& ev : _transportEvents) {
            if (ev.connection != _connection || _connection == kInvalidConnection) {
                continue;  // stale events from a superseded connection
            }
            switch (ev.type) {
            case TransportEventType::Connected: {
                if (_state != ClientState::Connecting && _state != ClientState::Reconnecting) break;
                proto::Hello hello;
                hello.gameRuntime = _config.gameRuntime;
                hello.clientBuild = _config.clientBuild.substr(0, proto::kMaxBuildBytes);
                hello.playerName = _config.playerName;
                hello.password = _config.password;
                hello.resumeToken = _resumeToken;
                Send(hello);
                _lastReceived = _clock();
                SetState(ClientState::Handshaking, _endpoint.ToString(), events);
                break;
            }
            case TransportEventType::Disconnected:
                if (ev.closedLocally) break;
                _connection = kInvalidConnection;
                ConnectionLost(std::format("connection closed ({}: {})", ev.closeCode, ev.closeReason), events);
                break;
            case TransportEventType::Message:
                _lastReceived = _clock();
                OnMessage(ev.data, events);
                break;
            }
        }

        const auto now = _clock();
        switch (_state) {
        case ClientState::Connecting:
            if (now - _stateSince > _config.connectTimeoutMs) {
                CloseConnection(CloseCode::TimedOut, "connect timeout", false);
                ConnectionLost("connect timed out", events);
            }
            break;
        case ClientState::Handshaking:
            if (now - _stateSince > _config.handshakeTimeoutMs) {
                CloseConnection(CloseCode::TimedOut, "handshake timeout", false);
                ConnectionLost("handshake timed out", events);
            }
            break;
        case ClientState::InSession:
            if (now - _lastReceived > _config.serverTimeoutMs) {
                CloseConnection(CloseCode::TimedOut, "server timeout", false);
                ConnectionLost("server stopped responding", events);
            } else if (now >= _nextHeartbeat) {
                Send(proto::Heartbeat{ ++_heartbeatSequence, now });
                _nextHeartbeat = now + _heartbeatIntervalMs;
            }
            break;
        case ClientState::Reconnecting:
            if (_connection != kInvalidConnection && now - _stateSince > _config.connectTimeoutMs) {
                CloseConnection(CloseCode::TimedOut, "reconnect timeout", false);
                ConnectionLost("reconnect attempt timed out", events);
            } else if (_connection == kInvalidConnection && now >= _nextReconnect) {
                ++_reconnectAttempts;
                std::string error;
                if (StartConnection(error)) {
                    log::Info("reconnect attempt {}/{} to {}", _reconnectAttempts, _config.maxReconnectAttempts,
                              _endpoint.ToString());
                } else {
                    ConnectionLost(error, events);
                }
            }
            break;
        default:
            break;
        }
    }

    void SessionClient::OnMessage(std::span<const std::uint8_t> data, std::vector<ClientEvent>& events)
    {
        auto decoded = proto::Decode(data);
        if (!decoded.Ok()) {
            Fail(std::format("server sent malformed message ({})", proto::ToString(decoded.error)), events);
            return;
        }
        auto& msg = *decoded.message;

        if (_state == ClientState::Handshaking) {
            if (auto* welcome = std::get_if<proto::Welcome>(&msg)) {
                if (welcome->protocolMajor != proto::kProtocolMajor) {
                    Fail("server protocol mismatch", events);
                    return;
                }
                const bool sameSession = welcome->resumed && welcome->sessionId == _sessionId;
                _playerId = welcome->playerId;
                _sessionId = welcome->sessionId;
                _resumeToken = welcome->resumeToken;
                _heartbeatIntervalMs = welcome->heartbeatIntervalMs;
                _nextHeartbeat = _clock();
                _reconnectAttempts = 0;
                if (!sameSession) {
                    for (const auto& [id, name] : _roster) {
                        events.emplace_back(client_event::PeerLeft{ id, proto::LeaveReason::ConnectionLost });
                    }
                    _roster.clear();
                    _lastRemoteSequence.clear();
                }
                events.emplace_back(client_event::Welcomed{ *welcome });
                SetState(ClientState::InSession, welcome->resumed ? "resumed" : "joined", events);
                return;
            }
            if (auto* reject = std::get_if<proto::Reject>(&msg)) {
                events.emplace_back(client_event::Rejected{ *reject });
                _resumeToken = {};
                CloseConnection(CloseCode::Normal, "rejected", false);
                SetState(ClientState::Disconnected,
                         std::format("rejected: {} ({})", proto::ToString(reject->reason), reject->detail), events);
                return;
            }
            Fail(std::format("unexpected {} during handshake", proto::ToString(proto::TypeOf(msg))), events);
            return;
        }

        if (_state != ClientState::InSession) {
            return;
        }

        std::visit(
            [&](const auto& m) {
                using T = std::decay_t<decltype(m)>;
                if constexpr (std::is_same_v<T, proto::SessionSnapshot>) {
                    ApplySnapshot(m, events);
                } else if constexpr (std::is_same_v<T, proto::PlayerJoined>) {
                    if (m.player.playerId != _playerId) {
                        _roster[m.player.playerId] = m.player.name;
                        _lastRemoteSequence.erase(m.player.playerId);
                        events.emplace_back(client_event::PeerJoined{ m.player });
                    }
                } else if constexpr (std::is_same_v<T, proto::PlayerLeft>) {
                    if (_roster.erase(m.playerId) > 0) {
                        _lastRemoteSequence.erase(m.playerId);
                        events.emplace_back(client_event::PeerLeft{ m.playerId, m.reason });
                    }
                } else if constexpr (std::is_same_v<T, proto::HeartbeatAck>) {
                    const auto now = _clock();
                    if (m.echoTimeMs <= now) {
                        _lastRttMs = static_cast<std::uint32_t>(now - m.echoTimeMs);
                        events.emplace_back(client_event::RttSample{ _lastRttMs });
                    }
                } else if constexpr (std::is_same_v<T, proto::PlayerTransform>) {
                    if (!_roster.contains(m.playerId)) {
                        return;  // unknown or already-left player: stale update
                    }
                    auto [it, inserted] = _lastRemoteSequence.try_emplace(m.playerId, m.sequence);
                    if (!inserted) {
                        if (!proto::SequenceNewer(m.sequence, it->second)) return;
                        it->second = m.sequence;
                    }
                    events.emplace_back(client_event::RemoteTransform{ m });
                } else if constexpr (std::is_same_v<T, proto::GroupState>) {
                    events.emplace_back(client_event::Groups{ m });
                } else if constexpr (std::is_same_v<T, proto::NpcCombat>) {
                    if (_roster.contains(m.playerId)) events.emplace_back(client_event::NpcCombat{ m });
                } else if constexpr (std::is_same_v<T, proto::NpcAuthority>) {
                    if (m.authority != _npcAuthority) _haveNpcSequence = false;  // a new sender's numbering
                    _npcAuthority = m.authority;
                    events.emplace_back(client_event::NpcAuthority{ m.authority });
                } else if constexpr (std::is_same_v<T, proto::NpcStates>) {
                    if (m.playerId != _npcAuthority) return;  // from before an ownership change
                    if (_haveNpcSequence && !proto::SequenceNewer(m.sequence, _lastNpcSequence)) return;
                    _lastNpcSequence = m.sequence;
                    _haveNpcSequence = true;
                    events.emplace_back(client_event::NpcStates{ m });
                } else if constexpr (std::is_same_v<T, proto::NpcDeath>) {
                    events.emplace_back(client_event::NpcDeath{ m });
                } else if constexpr (std::is_same_v<T, proto::NpcDamage>) {
                    events.emplace_back(client_event::NpcDamage{ m });
                } else if constexpr (std::is_same_v<T, proto::NpcEvents>) {
                    if (m.playerId == _npcAuthority) events.emplace_back(client_event::NpcEvents{ m });
                } else if constexpr (std::is_same_v<T, proto::NpcOutfits>) {
                    if (m.playerId == _npcAuthority) events.emplace_back(client_event::NpcOutfits{ m });
                } else if constexpr (std::is_same_v<T, proto::Disconnect>) {
                    _resumeToken = {};
                    CloseConnection(CloseCode::Normal, "server closed session", false);
                    const auto why = m.reason == proto::DisconnectReason::Kicked ? "kicked" : "server shut down";
                    SetState(ClientState::Disconnected, m.detail.empty() ? std::string(why)
                                                                         : std::format("{}: {}", why, m.detail),
                             events);
                } else {
                    Fail(std::format("unexpected {} from server", proto::ToString(proto::TypeOf(m))), events);
                }
            },
            msg);
    }

    void SessionClient::ApplySnapshot(const proto::SessionSnapshot& snapshot, std::vector<ClientEvent>& events)
    {
        std::map<PlayerId, std::string> next;
        for (const auto& p : snapshot.players) {
            if (p.playerId != _playerId) next[p.playerId] = p.name;
        }
        for (const auto& [id, name] : _roster) {
            if (!next.contains(id)) {
                _lastRemoteSequence.erase(id);
                events.emplace_back(client_event::PeerLeft{ id, proto::LeaveReason::ConnectionLost });
            }
        }
        for (const auto& [id, name] : next) {
            if (!_roster.contains(id)) events.emplace_back(client_event::PeerJoined{ { id, name } });
        }
        _roster = std::move(next);
    }

    void SessionClient::ConnectionLost(std::string detail, std::vector<ClientEvent>& events)
    {
        const bool canResume = _config.autoReconnect && !_resumeToken.IsZero() &&
                               _reconnectAttempts < _config.maxReconnectAttempts;
        _connection = kInvalidConnection;
        if (canResume) {
            _nextReconnect = _clock() + _config.reconnectDelayMs;
            SetState(ClientState::Reconnecting, std::move(detail), events);
        } else {
            _resumeToken = {};
            SetState(ClientState::Disconnected, std::move(detail), events);
        }
    }

    void SessionClient::Fail(std::string detail, std::vector<ClientEvent>& events)
    {
        log::Warn("session client: {}", detail);
        _resumeToken = {};
        CloseConnection(CloseCode::ProtocolViolation, detail, false);
        SetState(ClientState::Disconnected, std::move(detail), events);
    }

    void SessionClient::Disconnect()
    {
        if (_state == ClientState::InSession) {
            Send(proto::Disconnect{ proto::DisconnectReason::Quit, {} });
        }
        CloseConnection(CloseCode::Normal, "client quit", true);
        _resumeToken = {};
        _state = ClientState::Disconnected;
        _stateSince = _clock();
    }

    bool SessionClient::SendGroupRequest(const proto::GroupRequest& request)
    {
        if (_state != ClientState::InSession) return false;
        Send(request, Delivery::Reliable);
        return true;
    }

    bool SessionClient::SendNpcStates(std::uint32_t locationId, std::vector<proto::NpcState> npcs)
    {
        if (_state != ClientState::InSession) return false;
        Send(proto::NpcStates{ kInvalidPlayerId, ++_npcStatesSequence, locationId, std::move(npcs) }, Delivery::Unreliable);
        return true;
    }

    bool SessionClient::SendNpcDeath(std::uint32_t npcFormId)
    {
        if (_state != ClientState::InSession || !proto::IsReplicatedNpcId(npcFormId)) return false;
        Send(proto::NpcDeath{ kInvalidPlayerId, npcFormId }, Delivery::Reliable);
        return true;
    }

    bool SessionClient::SendNpcDamage(std::uint32_t npcFormId, float amount)
    {
        if (_state != ClientState::InSession || !proto::IsReplicatedNpcId(npcFormId)) return false;
        Send(proto::NpcDamage{ kInvalidPlayerId, npcFormId, amount }, Delivery::Reliable);
        return true;
    }

    bool SessionClient::SendNpcEvents(std::vector<proto::NpcEvent> events)
    {
        if (_state != ClientState::InSession || events.empty()) return false;
        Send(proto::NpcEvents{ kInvalidPlayerId, std::move(events) }, Delivery::Reliable);
        return true;
    }

    bool SessionClient::SendNpcOutfits(std::vector<proto::NpcOutfit> outfits)
    {
        if (_state != ClientState::InSession || outfits.empty()) return false;
        // Full state on a timer: a lost message is replaced by the next one.
        Send(proto::NpcOutfits{ kInvalidPlayerId, std::move(outfits) }, Delivery::Unreliable);
        return true;
    }

    bool SessionClient::SendNpcCombat(std::uint32_t npcFormId, PlayerId targetPlayer)
    {
        if (_state != ClientState::InSession || npcFormId == 0 || targetPlayer == kInvalidPlayerId) return false;
        Send(proto::NpcCombat{ kInvalidPlayerId, npcFormId, targetPlayer }, Delivery::Reliable);
        return true;
    }

    bool SessionClient::SendTransform(std::uint32_t locationId, const Vec3& position, const Vec3& rotation,
                                      const std::optional<proto::PlayerControls>& controls,
                                      const std::vector<proto::AnimEvent>& events,
                                      const std::optional<proto::WeaponState>& weapon,
                                      const std::optional<proto::PlayerStatus>& status)
    {
        if (_state != ClientState::InSession) {
            return false;
        }
        proto::PlayerTransform t;
        t.sequence = ++_transformSequence;
        t.locationId = locationId;
        t.position = position;
        t.rotation = rotation;
        t.controls = controls;
        t.events = events;
        t.weapon = weapon;
        t.status = status;
        if (!proto::IsValidTransform(t)) {
            return false;
        }
        Send(t, Delivery::Unreliable);
        return true;
    }

    void SessionClient::SetState(ClientState state, std::string detail, std::vector<ClientEvent>& events)
    {
        _state = state;
        _stateSince = _clock();
        log::Info("session client: {} ({})", ToString(state), detail);
        events.emplace_back(client_event::StateChanged{ state, std::move(detail) });
    }

    void SessionClient::Send(const proto::Message& msg, Delivery delivery)
    {
        if (_connection != kInvalidConnection) {
            const auto bytes = proto::Encode(msg);
            _transport.Send(_connection, bytes, delivery);
        }
    }

    void SessionClient::CloseConnection(CloseCode code, std::string_view reason, bool linger)
    {
        if (_connection != kInvalidConnection) {
            _transport.Close(_connection, code, reason, linger);
            _connection = kInvalidConnection;
        }
    }
}
