#include "cosync/shared/Protocol.h"

#include "cosync/shared/ByteStream.h"

#include <algorithm>
#include <cmath>
#include <type_traits>

namespace cosync::proto
{
    namespace
    {
        template <class>
        inline constexpr bool kAlwaysFalse = false;

        void WriteToken(ByteWriter& w, const ResumeToken& t) { w.Bytes(t.bytes); }
        void ReadToken(ByteReader& r, ResumeToken& t) { r.Bytes(t.bytes); }

        void WriteVec3(ByteWriter& w, const Vec3& v)
        {
            w.F32(v.x);
            w.F32(v.y);
            w.F32(v.z);
        }

        Vec3 ReadVec3(ByteReader& r)
        {
            Vec3 v;
            v.x = r.F32();
            v.y = r.F32();
            v.z = r.F32();
            return v;
        }

        // Reads a validated text field; marks the reader failed on invalid content.
        std::string ReadText(ByteReader& r, std::size_t maxBytes)
        {
            auto s = r.String(maxBytes);
            if (r.Ok() && !IsValidText(s, maxBytes)) {
                r.Fail();
            }
            return s;
        }

        void Encode(ByteWriter& w, const Hello& m)
        {
            // magic/major/minor are frozen for all protocol versions so any server
            // can read them and answer with a version-mismatch Reject.
            w.U32(m.magic);
            w.U16(m.protocolMajor);
            w.U16(m.protocolMinor);
            w.U32(m.gameRuntime);
            w.String(m.clientBuild);
            w.String(m.playerName);
            w.String(m.password);
            WriteToken(w, m.resumeToken);
        }

        void Encode(ByteWriter& w, const Welcome& m)
        {
            w.U16(m.protocolMajor);
            w.U16(m.protocolMinor);
            w.U32(m.playerId);
            w.U64(m.sessionId);
            WriteToken(w, m.resumeToken);
            w.String(m.serverName);
            w.U16(m.maxPlayers);
            w.U16(m.heartbeatIntervalMs);
            w.Bool(m.resumed);
        }

        void Encode(ByteWriter& w, const Reject& m)
        {
            // Layout frozen across protocol versions (see Hello).
            w.U16(static_cast<std::uint16_t>(m.reason));
            w.U16(m.serverProtocolMajor);
            w.U16(m.serverProtocolMinor);
            w.String(m.detail);
        }

        void Encode(ByteWriter& w, const PlayerEntry& m)
        {
            w.U32(m.playerId);
            w.String(m.name);
        }

        void Encode(ByteWriter& w, const SessionSnapshot& m)
        {
            w.U16(static_cast<std::uint16_t>(m.players.size()));
            for (const auto& p : m.players) {
                Encode(w, p);
            }
        }

        void Encode(ByteWriter& w, const PlayerJoined& m) { Encode(w, m.player); }

        void Encode(ByteWriter& w, const PlayerLeft& m)
        {
            w.U32(m.playerId);
            w.U8(static_cast<std::uint8_t>(m.reason));
        }

        void Encode(ByteWriter& w, const Heartbeat& m)
        {
            w.U32(m.sequence);
            w.U64(m.sendTimeMs);
        }

        void Encode(ByteWriter& w, const HeartbeatAck& m)
        {
            w.U32(m.sequence);
            w.U64(m.echoTimeMs);
        }

        void Encode(ByteWriter& w, const Disconnect& m)
        {
            w.U16(static_cast<std::uint16_t>(m.reason));
            w.String(m.detail);
        }

        void Encode(ByteWriter& w, const PlayerTransform& m)
        {
            w.U32(m.playerId);
            w.U32(m.sequence);
            w.U32(m.locationId);
            WriteVec3(w, m.position);
            WriteVec3(w, m.rotation);
            if (m.controls) {
                w.U8(1);
                for (float v : m.controls->values) w.F32(v);
                w.U8(m.controls->flag);
            }
            if (!m.events.empty()) {
                w.U8(2);
                w.U8(static_cast<std::uint8_t>(m.events.size()));
                for (const auto& e : m.events) {
                    w.U16(e.sequence);
                    w.String(e.name);
                }
            }
            if (m.weapon) {
                w.U8(3);
                w.U32(m.weapon->formId);
                w.Bool(m.weapon->drawn);
            }
        }

        bool ValidReject(std::uint16_t v) { return v >= 1 && v <= static_cast<std::uint16_t>(RejectReason::RateLimited); }
        bool ValidLeave(std::uint8_t v) { return v >= 1 && v <= static_cast<std::uint8_t>(LeaveReason::ServerShutdown); }
        bool ValidDisconnect(std::uint16_t v) { return v >= 1 && v <= static_cast<std::uint16_t>(DisconnectReason::Kicked); }

        PlayerEntry ReadEntry(ByteReader& r)
        {
            PlayerEntry e;
            e.playerId = r.U32();
            e.name = r.String(kMaxPlayerNameBytes);
            if (r.Ok() && (e.playerId == kInvalidPlayerId || !IsValidPlayerName(e.name))) {
                r.Fail();
            }
            return e;
        }

        // Returns the message, or sets r to failed. `exactEnd` is cleared for
        // messages that intentionally ignore trailing data (future Hello layouts).
        std::optional<Message> DecodePayload(MessageType type, ByteReader& r, bool& exactEnd)
        {
            switch (type) {
            case MessageType::Hello: {
                Hello m;
                m.magic = r.U32();
                m.protocolMajor = r.U16();
                m.protocolMinor = r.U16();
                if (!r.Ok() || m.magic != kMagic) {
                    r.Fail();
                    return std::nullopt;
                }
                if (m.protocolMajor != kProtocolMajor) {
                    // Different major: the rest of the layout is unknown to us. Return
                    // the frozen prefix so the server can reply with a version Reject.
                    exactEnd = false;
                    return m;
                }
                m.gameRuntime = r.U32();
                m.clientBuild = ReadText(r, kMaxBuildBytes);
                m.playerName = r.String(kMaxPlayerNameBytes);  // semantic check is the server's job
                m.password = r.String(kMaxPasswordBytes);
                ReadToken(r, m.resumeToken);
                if (m.protocolMinor > kProtocolMinor) {
                    exactEnd = false;  // newer minor may append fields
                }
                return m;
            }
            case MessageType::Welcome: {
                Welcome m;
                m.protocolMajor = r.U16();
                m.protocolMinor = r.U16();
                m.playerId = r.U32();
                m.sessionId = r.U64();
                ReadToken(r, m.resumeToken);
                m.serverName = ReadText(r, kMaxServerNameBytes);
                m.maxPlayers = r.U16();
                m.heartbeatIntervalMs = r.U16();
                m.resumed = r.Bool();
                if (r.Ok() && (m.playerId == kInvalidPlayerId || m.maxPlayers == 0 || m.maxPlayers > kMaxPlayersLimit ||
                               m.heartbeatIntervalMs == 0)) {
                    r.Fail();
                }
                return m;
            }
            case MessageType::Reject: {
                Reject m;
                const auto reason = r.U16();
                m.serverProtocolMajor = r.U16();
                m.serverProtocolMinor = r.U16();
                m.detail = ReadText(r, kMaxDetailBytes);
                if (r.Ok() && !ValidReject(reason)) {
                    r.Fail();
                }
                m.reason = static_cast<RejectReason>(reason);
                return m;
            }
            case MessageType::SessionSnapshot: {
                SessionSnapshot m;
                const auto count = r.U16();
                if (!r.Ok() || count > kMaxPlayersLimit) {
                    r.Fail();
                    return std::nullopt;
                }
                m.players.reserve(count);
                for (std::uint16_t i = 0; i < count && r.Ok(); ++i) {
                    m.players.push_back(ReadEntry(r));
                }
                return m;
            }
            case MessageType::PlayerJoined:
                return PlayerJoined{ ReadEntry(r) };
            case MessageType::PlayerLeft: {
                PlayerLeft m;
                m.playerId = r.U32();
                const auto reason = r.U8();
                if (r.Ok() && (m.playerId == kInvalidPlayerId || !ValidLeave(reason))) {
                    r.Fail();
                }
                m.reason = static_cast<LeaveReason>(reason);
                return m;
            }
            case MessageType::Heartbeat: {
                Heartbeat m;
                m.sequence = r.U32();
                m.sendTimeMs = r.U64();
                return m;
            }
            case MessageType::HeartbeatAck: {
                HeartbeatAck m;
                m.sequence = r.U32();
                m.echoTimeMs = r.U64();
                return m;
            }
            case MessageType::Disconnect: {
                Disconnect m;
                const auto reason = r.U16();
                m.detail = ReadText(r, kMaxDetailBytes);
                if (r.Ok() && !ValidDisconnect(reason)) {
                    r.Fail();
                }
                m.reason = static_cast<DisconnectReason>(reason);
                return m;
            }
            case MessageType::PlayerTransform: {
                PlayerTransform m;
                m.playerId = r.U32();
                m.sequence = r.U32();
                m.locationId = r.U32();
                m.position = ReadVec3(r);
                m.rotation = ReadVec3(r);
                bool seenEvents = false;
                while (r.Ok() && r.Remaining() > 0) {  // tagged sections (1.1+)
                    const auto tag = r.U8();
                    if (tag == 1 && !m.controls) {
                        PlayerControls c;
                        for (float& v : c.values) v = r.F32();
                        c.flag = r.U8();
                        if (r.Ok() && !IsValidControls(c)) r.Fail();
                        m.controls = c;
                    } else if (tag == 2 && !seenEvents) {
                        seenEvents = true;
                        const std::size_t count = r.U8();
                        if (count == 0 || count > kMaxAnimEventsPerTransform) {
                            r.Fail();
                            break;
                        }
                        for (std::size_t i = 0; i < count && r.Ok(); ++i) {
                            AnimEvent e;
                            e.sequence = r.U16();
                            e.name = ReadText(r, kMaxAnimEventNameBytes);
                            m.events.push_back(std::move(e));
                        }
                    } else if (tag == 3 && !m.weapon) {  // 1.3 weapon
                        WeaponState ws;
                        ws.formId = r.U32();
                        ws.drawn = r.Bool();
                        m.weapon = ws;
                    } else {
                        r.Fail();  // unknown or repeated section
                    }
                }
                if (r.Ok() && !IsValidTransform(m)) {
                    r.Fail();
                }
                return m;
            }
            }
            return std::nullopt;
        }

        bool IsKnownType(std::uint16_t t)
        {
            return t >= static_cast<std::uint16_t>(MessageType::Hello) &&
                   t <= static_cast<std::uint16_t>(MessageType::PlayerTransform);
        }

        bool IsFiniteBounded(const Vec3& v, float bound)
        {
            for (float f : { v.x, v.y, v.z }) {
                if (!std::isfinite(f) || std::fabs(f) > bound) {
                    return false;
                }
            }
            return true;
        }
    }

    MessageType TypeOf(const Message& message) noexcept
    {
        return std::visit(
            [](const auto& m) -> MessageType {
                using T = std::decay_t<decltype(m)>;
                if constexpr (std::is_same_v<T, Hello>) return MessageType::Hello;
                else if constexpr (std::is_same_v<T, Welcome>) return MessageType::Welcome;
                else if constexpr (std::is_same_v<T, Reject>) return MessageType::Reject;
                else if constexpr (std::is_same_v<T, SessionSnapshot>) return MessageType::SessionSnapshot;
                else if constexpr (std::is_same_v<T, PlayerJoined>) return MessageType::PlayerJoined;
                else if constexpr (std::is_same_v<T, PlayerLeft>) return MessageType::PlayerLeft;
                else if constexpr (std::is_same_v<T, Heartbeat>) return MessageType::Heartbeat;
                else if constexpr (std::is_same_v<T, HeartbeatAck>) return MessageType::HeartbeatAck;
                else if constexpr (std::is_same_v<T, Disconnect>) return MessageType::Disconnect;
                else if constexpr (std::is_same_v<T, PlayerTransform>) return MessageType::PlayerTransform;
                else static_assert(kAlwaysFalse<T>, "unhandled message type");
            },
            message);
    }

    std::vector<std::uint8_t> Encode(const Message& message)
    {
        ByteWriter w;
        w.U16(static_cast<std::uint16_t>(TypeOf(message)));
        w.U16(0);  // flags, reserved
        std::visit([&w](const auto& m) { Encode(w, m); }, message);
        return w.Take();
    }

    DecodeResult Decode(std::span<const std::uint8_t> bytes)
    {
        if (bytes.empty()) {
            return { std::nullopt, DecodeError::Empty };
        }
        if (bytes.size() > kMaxMessageBytes) {
            return { std::nullopt, DecodeError::TooLarge };
        }

        ByteReader r(bytes);
        const auto type = r.U16();
        const auto flags = r.U16();
        if (!r.Ok()) {
            return { std::nullopt, DecodeError::Truncated };
        }
        if (!IsKnownType(type)) {
            return { std::nullopt, DecodeError::UnknownType };
        }
        if (flags != 0) {
            return { std::nullopt, DecodeError::BadFlags };
        }

        bool exactEnd = true;
        auto msg = DecodePayload(static_cast<MessageType>(type), r, exactEnd);
        if (!r.Ok() || !msg) {
            return { std::nullopt, r.Overrun() ? DecodeError::Truncated : DecodeError::InvalidField };
        }
        if (exactEnd && !r.AtEnd()) {
            return { std::nullopt, DecodeError::TrailingBytes };
        }
        return { std::move(msg), DecodeError::None };
    }

    bool IsValidText(std::string_view s, std::size_t maxBytes) noexcept
    {
        if (s.size() > maxBytes) {
            return false;
        }
        // Strict UTF-8: no overlongs, no surrogates, max U+10FFFF, no C0/C1 controls or DEL.
        std::size_t i = 0;
        while (i < s.size()) {
            const auto c = static_cast<unsigned char>(s[i]);
            std::uint32_t cp = 0;
            std::size_t len = 0;
            if (c < 0x80) {
                cp = c;
                len = 1;
            } else if ((c & 0xE0) == 0xC0) {
                cp = c & 0x1F;
                len = 2;
            } else if ((c & 0xF0) == 0xE0) {
                cp = c & 0x0F;
                len = 3;
            } else if ((c & 0xF8) == 0xF0) {
                cp = c & 0x07;
                len = 4;
            } else {
                return false;
            }
            if (i + len > s.size()) {
                return false;
            }
            for (std::size_t k = 1; k < len; ++k) {
                const auto cc = static_cast<unsigned char>(s[i + k]);
                if ((cc & 0xC0) != 0x80) {
                    return false;
                }
                cp = (cp << 6) | (cc & 0x3F);
            }
            const bool overlong = (len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000);
            if (overlong || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
                return false;
            }
            if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F)) {
                return false;
            }
            i += len;
        }
        return true;
    }

    bool IsValidPlayerName(std::string_view name) noexcept
    {
        if (name.empty() || name.size() > kMaxPlayerNameBytes || !IsValidText(name, kMaxPlayerNameBytes)) {
            return false;
        }
        return name.front() != ' ' && name.back() != ' ';
    }

    bool IsValidControls(const PlayerControls& c) noexcept
    {
        if (c.flag > 1) return false;
        for (float v : c.values) {
            if (!std::isfinite(v) || std::fabs(v) > kMaxControlValue) return false;
        }
        return true;
    }

    bool IsValidTransform(const PlayerTransform& t) noexcept
    {
        constexpr float kMaxRotation = 1000.0f;  // radians; generous, only rejects garbage
        return IsFiniteBounded(t.position, kMaxCoordinate) && IsFiniteBounded(t.rotation, kMaxRotation) &&
               (!t.controls || IsValidControls(*t.controls)) && t.events.size() <= kMaxAnimEventsPerTransform &&
               std::ranges::all_of(t.events, [](const AnimEvent& e) {
                   return !e.name.empty() && IsValidText(e.name, kMaxAnimEventNameBytes);
               });
    }

    std::string_view ToString(MessageType type) noexcept
    {
        switch (type) {
        case MessageType::Hello: return "Hello";
        case MessageType::Welcome: return "Welcome";
        case MessageType::Reject: return "Reject";
        case MessageType::SessionSnapshot: return "SessionSnapshot";
        case MessageType::PlayerJoined: return "PlayerJoined";
        case MessageType::PlayerLeft: return "PlayerLeft";
        case MessageType::Heartbeat: return "Heartbeat";
        case MessageType::HeartbeatAck: return "HeartbeatAck";
        case MessageType::Disconnect: return "Disconnect";
        case MessageType::PlayerTransform: return "PlayerTransform";
        }
        return "Unknown";
    }

    std::string_view ToString(DecodeError error) noexcept
    {
        switch (error) {
        case DecodeError::None: return "none";
        case DecodeError::Empty: return "empty";
        case DecodeError::TooLarge: return "too large";
        case DecodeError::Truncated: return "truncated";
        case DecodeError::TrailingBytes: return "trailing bytes";
        case DecodeError::UnknownType: return "unknown type";
        case DecodeError::BadFlags: return "bad flags";
        case DecodeError::InvalidField: return "invalid field";
        }
        return "unknown";
    }

    std::string_view ToString(RejectReason reason) noexcept
    {
        switch (reason) {
        case RejectReason::ProtocolVersionMismatch: return "protocol version mismatch";
        case RejectReason::GameVersionMismatch: return "game version mismatch";
        case RejectReason::ServerFull: return "server full";
        case RejectReason::InvalidName: return "invalid name";
        case RejectReason::NameInUse: return "name in use";
        case RejectReason::BadPassword: return "bad password";
        case RejectReason::ProtocolViolation: return "protocol violation";
        case RejectReason::HandshakeTimeout: return "handshake timeout";
        case RejectReason::ServerShuttingDown: return "server shutting down";
        case RejectReason::RateLimited: return "rate limited";
        }
        return "unknown";
    }

    std::string_view ToString(LeaveReason reason) noexcept
    {
        switch (reason) {
        case LeaveReason::Quit: return "quit";
        case LeaveReason::ConnectionLost: return "connection lost";
        case LeaveReason::TimedOut: return "timed out";
        case LeaveReason::Kicked: return "kicked";
        case LeaveReason::ProtocolViolation: return "protocol violation";
        case LeaveReason::ServerShutdown: return "server shutdown";
        }
        return "unknown";
    }
}
