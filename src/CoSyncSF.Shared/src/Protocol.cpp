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
            if (m.status) {
                w.U8(4);
                w.U8(m.status->healthPercent);
                w.U16(m.status->level);
                w.U8(static_cast<std::uint8_t>(m.status->kind));
                w.String(m.status->place);
            }
        }

        void WriteIds(ByteWriter& w, const std::vector<PlayerId>& ids)
        {
            w.U8(static_cast<std::uint8_t>(ids.size()));
            for (auto id : ids) w.U32(id);
        }
        std::vector<PlayerId> ReadIds(ByteReader& r, std::size_t max)
        {
            const std::size_t n = r.U8();
            if (n > max) {
                r.Fail();
                return {};
            }
            std::vector<PlayerId> ids;
            for (std::size_t i = 0; i < n && r.Ok(); ++i) ids.push_back(r.U32());
            return ids;
        }

        void Encode(ByteWriter& w, const GroupRequest& m)
        {
            w.U8(static_cast<std::uint8_t>(m.action));
            w.U32(m.target);
            w.U32(m.groupId);
        }

        void Encode(ByteWriter& w, const GroupState& m)
        {
            w.U32(m.version);
            w.U8(static_cast<std::uint8_t>(m.groups.size()));
            for (const auto& g : m.groups) {
                w.U32(g.id);
                w.U32(g.leader);
                WriteIds(w, g.members);
                WriteIds(w, g.invited);
            }
        }

        void Encode(ByteWriter& w, const NpcCombat& m)
        {
            w.U32(m.playerId);
            w.U32(m.npcFormId);
            w.U32(m.targetPlayer);
        }

        void Encode(ByteWriter& w, const NpcAuthority& m) { w.U32(m.authority); }

        void Encode(ByteWriter& w, const NpcStates& m)
        {
            w.U32(m.playerId);
            w.U32(m.sequence);
            w.U32(m.locationId);
            const auto count = std::min(m.npcs.size(), kMaxNpcStates);
            w.U8(static_cast<std::uint8_t>(count));
            for (std::size_t i = 0; i < count; ++i) {
                const auto& n = m.npcs[i];
                w.U32(n.formId);
                w.U32(n.baseId);
                WriteVec3(w, n.position);
                w.F32(n.yaw);
                w.U8(n.healthPercent);
                w.U8(n.flags);
                if ((n.flags & kNpcCrowdTarget) != 0) WriteVec3(w, n.crowdTarget);
            }
        }

        void Encode(ByteWriter& w, const NpcDeath& m)
        {
            w.U32(m.playerId);
            w.U32(m.npcFormId);
        }

        void Encode(ByteWriter& w, const NpcDamage& m)
        {
            w.U32(m.playerId);
            w.U32(m.npcFormId);
            w.F32(m.amount);
        }

        void Encode(ByteWriter& w, const NpcEvents& m)
        {
            w.U32(m.playerId);
            const auto count = std::min(m.events.size(), kMaxNpcEvents);
            w.U8(static_cast<std::uint8_t>(count));
            for (std::size_t i = 0; i < count; ++i) {
                w.U32(m.events[i].formId);
                w.String(m.events[i].name);
            }
        }

        void Encode(ByteWriter& w, const NpcOutfits& m)
        {
            w.U32(m.playerId);
            const auto count = std::min(m.outfits.size(), kMaxNpcOutfits);
            w.U8(static_cast<std::uint8_t>(count));
            for (std::size_t i = 0; i < count; ++i) {
                const auto& o = m.outfits[i];
                w.U32(o.formId);
                w.U32(o.look);
                const auto items = std::min(o.items.size(), kMaxNpcOutfitItems);
                w.U8(static_cast<std::uint8_t>(items));
                for (std::size_t j = 0; j < items; ++j) w.U32(o.items[j]);
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
                    } else if (tag == 4 && !m.status) {  // 1.5 status
                        PlayerStatus st;
                        st.healthPercent = r.U8();
                        st.level = r.U16();
                        const auto kind = r.U8();
                        st.place = ReadText(r, kMaxPlaceBytes);
                        if (st.healthPercent > 100 || kind > 2) r.Fail();
                        st.kind = static_cast<PlaceKind>(kind);
                        m.status = std::move(st);
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
            case MessageType::GroupRequest: {
                GroupRequest m;
                const auto action = r.U8();
                m.target = r.U32();
                m.groupId = r.U32();
                if (action < 1 || action > 6) r.Fail();
                m.action = static_cast<GroupAction>(action);
                return m;
            }
            case MessageType::GroupState: {
                GroupState m;
                m.version = r.U32();
                const std::size_t n = r.U8();
                if (n > kMaxGroups) {
                    r.Fail();
                    return m;
                }
                for (std::size_t i = 0; i < n && r.Ok(); ++i) {
                    GroupInfo g;
                    g.id = r.U32();
                    g.leader = r.U32();
                    g.members = ReadIds(r, kMaxGroupMembers);
                    g.invited = ReadIds(r, 64);
                    m.groups.push_back(std::move(g));
                }
                return m;
            }
            case MessageType::NpcAuthority: {
                NpcAuthority m;
                m.authority = r.U32();
                return m;
            }
            case MessageType::NpcStates: {
                NpcStates m;
                m.playerId = r.U32();
                m.sequence = r.U32();
                m.locationId = r.U32();
                const std::size_t count = r.U8();
                if (count > kMaxNpcStates) r.Fail();
                for (std::size_t i = 0; r.Ok() && i < count; ++i) {
                    NpcState n;
                    n.formId = r.U32();
                    n.baseId = r.U32();
                    n.position = ReadVec3(r);
                    n.yaw = r.F32();
                    n.healthPercent = r.U8();
                    n.flags = r.U8();
                    if ((n.flags & kNpcCrowdTarget) != 0) n.crowdTarget = ReadVec3(r);
                    if (r.Ok() && !IsValidNpcState(n)) r.Fail();
                    m.npcs.push_back(n);
                }
                return m;
            }
            case MessageType::NpcDeath: {
                NpcDeath m;
                m.playerId = r.U32();
                m.npcFormId = r.U32();
                if (r.Ok() && !IsReplicatedNpcId(m.npcFormId)) r.Fail();
                return m;
            }
            case MessageType::NpcDamage: {
                NpcDamage m;
                m.playerId = r.U32();
                m.npcFormId = r.U32();
                m.amount = r.F32();
                if (r.Ok() && (!IsReplicatedNpcId(m.npcFormId) || !std::isfinite(m.amount) || m.amount <= 0.0f ||
                               m.amount > kMaxNpcDamage)) {
                    r.Fail();
                }
                return m;
            }
            case MessageType::NpcEvents: {
                NpcEvents m;
                m.playerId = r.U32();
                const std::size_t count = r.U8();
                if (count > kMaxNpcEvents) r.Fail();
                for (std::size_t i = 0; r.Ok() && i < count; ++i) {
                    NpcEvent e;
                    e.formId = r.U32();
                    e.name = r.String(kMaxAnimEventNameBytes);
                    if (r.Ok() && (!IsReplicatedNpcId(e.formId) || e.name.empty())) r.Fail();
                    m.events.push_back(std::move(e));
                }
                return m;
            }
            case MessageType::NpcOutfits: {
                NpcOutfits m;
                m.playerId = r.U32();
                const std::size_t count = r.U8();
                if (count > kMaxNpcOutfits) r.Fail();
                for (std::size_t i = 0; r.Ok() && i < count; ++i) {
                    NpcOutfit o;
                    o.formId = r.U32();
                    o.look = r.U32();
                    const std::size_t items = r.U8();
                    if (!IsReplicatedNpcId(o.formId) || items > kMaxNpcOutfitItems) r.Fail();
                    for (std::size_t j = 0; r.Ok() && j < items; ++j) {
                        o.items.push_back(r.U32());
                        if (o.items.back() == 0) r.Fail();
                    }
                    m.outfits.push_back(std::move(o));
                }
                return m;
            }
            case MessageType::NpcCombat: {
                NpcCombat m;
                m.playerId = r.U32();
                m.npcFormId = r.U32();
                m.targetPlayer = r.U32();
                if (r.Ok() && (m.npcFormId == 0 || m.targetPlayer == kInvalidPlayerId)) r.Fail();
                return m;
            }
            }
            return std::nullopt;
        }

        bool IsKnownType(std::uint16_t t)
        {
            return t >= static_cast<std::uint16_t>(MessageType::Hello) &&
                   t <= static_cast<std::uint16_t>(MessageType::NpcOutfits);
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
                else if constexpr (std::is_same_v<T, NpcCombat>) return MessageType::NpcCombat;
                else if constexpr (std::is_same_v<T, GroupRequest>) return MessageType::GroupRequest;
                else if constexpr (std::is_same_v<T, GroupState>) return MessageType::GroupState;
                else if constexpr (std::is_same_v<T, NpcAuthority>) return MessageType::NpcAuthority;
                else if constexpr (std::is_same_v<T, NpcStates>) return MessageType::NpcStates;
                else if constexpr (std::is_same_v<T, NpcDeath>) return MessageType::NpcDeath;
                else if constexpr (std::is_same_v<T, NpcDamage>) return MessageType::NpcDamage;
                else if constexpr (std::is_same_v<T, NpcEvents>) return MessageType::NpcEvents;
                else if constexpr (std::is_same_v<T, NpcOutfits>) return MessageType::NpcOutfits;
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

    bool IsValidNpcState(const NpcState& s) noexcept
    {
        return IsReplicatedNpcId(s.formId) && IsFiniteBounded(s.position, kMaxCoordinate) && std::isfinite(s.yaw) &&
               s.healthPercent <= 100 && (s.flags & ~kNpcKnownFlags) == 0 && IsFiniteBounded(s.crowdTarget, kMaxCoordinate);
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
               (!t.status || (t.status->healthPercent <= 100 && IsValidText(t.status->place, kMaxPlaceBytes))) &&
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
        case MessageType::NpcCombat: return "NpcCombat";
        case MessageType::GroupRequest: return "GroupRequest";
        case MessageType::GroupState: return "GroupState";
        case MessageType::NpcAuthority: return "NpcAuthority";
        case MessageType::NpcStates: return "NpcStates";
        case MessageType::NpcDeath: return "NpcDeath";
        case MessageType::NpcDamage: return "NpcDamage";
        case MessageType::NpcEvents: return "NpcEvents";
        case MessageType::NpcOutfits: return "NpcOutfits";
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
