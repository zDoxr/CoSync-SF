#pragma once

#include "CrowdSync.h"
#include "NpcSync.h"

#include "cosync/net/NetworkWorker.h"
#include "cosync/shared/NpcReplication.h"
#include "cosync/shared/Types.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace RE
{
    class Actor;
}

// NPC replication, game side (docs/NPC_SYNC_ARCHITECTURE.md).
//
// The authority publishes the NPCs its engine runs; its AI, combat and death are the real ones.
// Every other client finds the same references locally and takes them over: the actor keeps
// updating (RE 12.2: its AI bit gates the whole per-tick update, 3D included), and its movement
// goes through the same IMovementPlayerControls replay as a remote player's proxy.
//
// EXPERIMENTAL, [Experimental] NpcSync=1. See docs/NPC_SYNC_STATUS.md for what has been tested.
// MAIN THREAD ONLY.
namespace cosync::plugin::npcsync
{
    inline constexpr std::size_t kReplicationSlots = 256;

    void KeepPlayerAlive(float belowPercent);

    class Replication
    {
    public:
        // A runtime NPC under the NPC authority's number, from this copy's, and back. 0 when the NPC
        // is not paired. On the authority both are the same number.
        [[nodiscard]] std::uint32_t SharedNpcId(std::uint32_t localId) const;
        [[nodiscard]] std::uint32_t LocalNpcId(std::uint32_t sharedId) const;

        struct Context
        {
            bool inWorld = false;
            bool vmSettled = false;
            std::uint32_t locationId = 0;
            PlayerId self = kInvalidPlayerId;
            std::vector<std::uint32_t> proxies;  // this client's stand-ins for remote players: never NPCs
        };

        void Tick(net::NetworkWorker& worker, const Context& ctx);
        void OnAuthority(PlayerId authority);
        void OnDeath(std::uint32_t formId);
        void OnDamage(const proto::NpcDamage& damage);
        void OnEvents(const proto::NpcEvents& events);
        void OnOutfits(const proto::NpcOutfits& outfits);
        // Hooks the graph. Plugin load.
        static void Install(std::string& detail);
        // Gives every NPC back to its local AI.
        void ReleaseAll(const char* why);
        // The session ended: also forgets who owned the NPCs and which ones died.
        void Reset(const char* why);

    private:
        using Clock = std::chrono::steady_clock;
        static constexpr std::size_t kSlots = kReplicationSlots;

        struct Tracked
        {
            int slot = -1;  // >= 0 while taken over
            Clock::time_point heldSince{};  // when it was taken over
            Vec3 position{};
            float velX = 0.0f, velY = 0.0f, velZ = 0.0f;
            float yaw = 0.0f;
            std::uint8_t healthPercent = 100;
            std::uint8_t flags = 0;
            bool fresh = false;           // a state arrived this frame
            Clock::time_point nextLoadCheck{};
            Clock::time_point receivedAt{};
            Clock::time_point nextAttempt{};
            Clock::time_point bigErrorSince{};
            Clock::time_point nextSnap{};
            Clock::time_point nextDraw{};
            Clock::time_point nextOutfitFix{};
            std::vector<std::uint32_t> outfit;  // what the authority's NPC wears and holds
            bool haveOutfit = false;
            bool outfitDiffers = false;
            bool lookChecked = false;
            bool lookDiffers = false;
            std::uint32_t freeSnaps = 0;
            std::uint32_t outfitTries = 0;
            std::uint32_t snaps = 0;
            std::uint32_t lostControl = 0;
            bool hasCrowdTarget = false;       // the authority's CrowdManager has a target for it
            Vec3 crowdTarget{};
            std::uint32_t crowdHandle = 0;     // that target here, 0 if there is none
            std::uint32_t crowdGeneration = 0;
            npc::HealthTracker health;
        };

        void TickAuthority(net::NetworkWorker& worker, const Context& ctx);
        void TickReplica(net::NetworkWorker& worker, const Context& ctx);
        void Absorb(const proto::NpcStates& states);
        void UpdateOne(std::uint32_t formId, Tracked& t, net::NetworkWorker& worker);
        bool TakeOver(std::uint32_t formId, Tracked& t);
        void Drive(std::uint32_t formId, Tracked& t, Clock::time_point now);
        void Release(std::uint32_t formId, Tracked& t, const char* why);
        void ApplyPendingDeaths();
        void LogSummary(const Context& ctx);
        void ScanLists();
        void PublishActions(net::NetworkWorker& worker);
        void PublishOutfits(net::NetworkWorker& worker, const std::vector<proto::NpcState>& states);
        void FixOutfit(std::uint32_t formId, Tracked& t, RE::Actor* actor, Clock::time_point now);
        void FollowWhileFree(std::uint32_t formId, Tracked& t, RE::Actor* actor, Clock::time_point now);
        void PairRuntimeNpcs(Clock::time_point now);
        void RefreshCrowd(Clock::time_point now);
        [[nodiscard]] bool FollowsCrowd(std::uint32_t formId, Tracked& t);
        [[nodiscard]] std::uint32_t LocalId(std::uint32_t authorityId) const;
        [[nodiscard]] bool IsProxy(std::uint32_t formId) const;

        PlayerId _authority = kInvalidPlayerId;
        bool _isAuthority = false;
        std::map<std::uint32_t, Tracked> _tracked;
        std::array<bool, kSlots> _slotUsed{};
        std::set<std::uint32_t> _dead;          // every death heard of or announced this session
        std::set<std::uint32_t> _killsApplied;  // deaths applied to the local copy
        Clock::time_point _nextPublish{};
        Clock::time_point _nextDeathCheck{};
        Clock::time_point _nextPurge{};
        Clock::time_point _nextLog{};
        std::uint64_t _published = 0;
        std::uint64_t _received = 0;
        std::uint64_t _otherLocation = 0;   // parts from a location that is not this client's
        std::uint32_t _listScans = 0;
        Clock::time_point _nextListScan{};
        // The authority's runtime NPCs not yet paired with one here.
        struct Unmatched
        {
            std::uint32_t base = 0;
            std::uint32_t look = 0;
            std::vector<std::uint32_t> worn;
            Vec3 position{};
            Clock::time_point seen{};
        };
        std::map<std::uint32_t, Unmatched> _unmatched;
        std::map<std::uint32_t, std::uint32_t> _idMap;  // authority's runtime ID -> the same NPC's ID here
        std::set<std::uint32_t> _sameRuntimeId;         // runtime IDs that are the same NPC on both machines
        // Crowd actors this copy spawned that the authority has no match for (issue #2): first seen
        // unpaired, and those hidden for it.
        std::map<std::uint32_t, Clock::time_point> _unpairedSince;
        std::set<std::uint32_t> _hiddenLocal;
        Clock::time_point _nextHideCheck{};
        Clock::time_point _lastAbsorb{};
        void HideLocalOnlyCrowd(Clock::time_point now);

        // The authority's walk-ins, placed here from their template (issue #2).
        struct StandIn
        {
            std::shared_ptr<std::atomic<std::uint32_t>> result;
            std::uint32_t local = 0;
            Vec3 position{};
            Clock::time_point requested{};
        };
        std::map<std::uint32_t, std::uint32_t> _templates;  // authority's runtime ID -> its data-file template
        std::map<std::uint32_t, StandIn> _standIns;         // authority's runtime ID -> what was placed for it
        std::map<std::uint32_t, Clock::time_point> _unmatchedSince;  // when an authority NPC was first unmatched
        std::uint64_t _standInsPlaced = 0;
        std::uint64_t _standInsRemoved = 0;
        Clock::time_point _nextStandInLog{};
        void PlaceStandIns(Clock::time_point now);
        std::uint64_t _tallySameId = 0, _tallyPaired = 0, _tallyUnmatched = 0, _tallyProxy = 0;
        Clock::time_point _nextPairing{};
        std::uint64_t _pairs = 0;
        std::size_t _lastPublishedRuntime = 0;
        std::size_t _lastEnumerated = 0;
        std::size_t _lastCrowd = 0;
        std::set<std::uint32_t> _localCrowd;           // this copy's crowd actors
        std::vector<npc::CrowdTarget> _crowdTargets;   // this copy's crowd targets
        std::uint32_t _crowdGeneration = 0;
        std::vector<crowd::Order> _crowdOrders;
        Clock::time_point _nextCrowdRefresh{};
        std::uint64_t _crowdNoTarget = 0;
        std::vector<ActorInfo> _extraActors;     // loaded, but not in the main list
        Clock::time_point _nextExtraScan{};
        std::uint64_t _eventsPublished = 0;
        std::uint64_t _eventsApplied = 0;
        std::uint64_t _outfitsPublished = 0;
        std::uint64_t _outfitFixes = 0;
        Clock::time_point _nextOutfits{};
        Clock::time_point _nextOutfitWork{};
        std::vector<std::uint32_t> _proxies;
        std::map<std::uint32_t, std::uint32_t> _baseDiffers;  // same reference, other base: id -> authority's base
        std::map<std::string, std::uint32_t> _eventNames;  // diagnostics: what NPC graphs receive
        std::set<std::uint32_t> _busyLogged;
        std::set<std::uint32_t> _lookLogged;
        Clock::time_point _nextUnmatchedLog{};
        Clock::time_point _nextBlockedLog{};
        Clock::time_point _nextNearLog{};
        Clock::time_point _nextCrowdLog{};
        std::set<std::uint32_t> _crowdLogged;
        std::set<std::uint32_t> _invalidLogged;
        std::uint64_t _skippedBusy = 0;     // controller not in the plain AI state
        std::uint64_t _damageForwarded = 0;
        std::size_t _lastPublishedCount = 0;
    };
}
