#pragma once

#include "AnimEvents.h"
#include "AnimProbe.h"
#include "Avatars.h"
#include "NpcReplication.h"
#include "DialogueSync.h"
#include "ShipSync.h"
#include "WorldSync.h"
#include "cosync/net/LanDiscovery.h"
#include "cosync/net/NetworkWorker.h"
#include "cosync/ui/UiModel.h"
#include "cosync/shared/Interpolation.h"

#include <atomic>
#include <memory>
#include <chrono>
#include <future>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace cosync::plugin
{
    // Game-thread side of the plugin. Tick() runs once per frame on Starfield's
    // main thread (SFSE permanent task) and is the ONLY caller of engine code
    // (GameState.cpp). It samples the local player's transform for the network
    // thread, consumes session events, and keeps an interpolated pose for every
    // remote player.
    class GameBridge
    {
    public:
        static GameBridge& Get();

        // Hand over the worker (owned elsewhere, outlives the bridge's use of it).
        void Attach(net::NetworkWorker* worker) noexcept { _worker.store(worker); }
        // Main thread: despawns every proxy and forgets the worker (before it is stopped).
        void Detach();
        // Reading the player needs the Address Library; disabled if it is missing.
        void EnableLocalPlayerReads(bool enable) noexcept { _readLocalPlayer = enable; }
        // Experimental: spawn and move a stand-in actor per remote player in the same cell.
        void EnableAvatars(bool enable) noexcept { _avatarsEnabled = enable; }
        // Experimental: replicate NPCs (NpcReplication.h).
        void EnableMenuPatch(bool enable) { _menuPatch = enable; }
    void EnableNpcSync(bool enable) noexcept { _npcSyncEnabled = enable; }
        void EnableWorldSync(bool enable);
        void SetAvatarScale(float scale) noexcept { _avatarScale = scale; }
        struct AvatarOptions
        {
            int freezeMode = 0;        // 0 never, 1 at spawn, 2 after 3D loaded + freezeDelay
            float freezeDelay = 3.0f;  // seconds
            bool move = true;
            int animDiag = 0;         // 0 off, 1 transitions, 2 transitions + floats at 2 Hz
            bool animate = false;
            bool nearbyBase = false;  // false: player base 00000007
            bool path = false;        // move by engine pathing instead of teleporting
            bool motion = false;      // experiment: motion-driven movement controller, then teleport
            bool mirror = false;      // experiment: proxies replay the local player's IMovementPlayerControls data
            bool controls = false;    // proxies replay the remote player's IMovementPlayerControls data (network)
        };
        void SetAvatarOptions(const AvatarOptions& o) noexcept { _opt = o; }
        int _fakeHealth = -1;  // '@fakehealth': sent instead of the real health, -1 = off
        void Tick();

    private:
        struct Avatar
        {
            std::shared_ptr<std::atomic<std::uint32_t>> pending;  // spawn in flight
            std::uint32_t refId = 0;
            std::uint32_t attempts = 0;
            std::chrono::steady_clock::time_point nextAttempt{};
            std::chrono::steady_clock::time_point nextMove{};
            std::chrono::steady_clock::time_point requestedAt{};
            std::chrono::steady_clock::time_point nextProbe{};  // engine-state validation log
            std::uint32_t probes = 0;
            std::chrono::steady_clock::time_point loaded3DAt{};  // first probe that saw 3D
            bool frozen = false;
            animprobe::Snapshot probe;                   // last native animation probe (AvatarAnimDiag)
            bool probed = false;
            std::chrono::steady_clock::time_point nextFloatLog{};
            bool motionApplied = false;                  // AvatarMoveMode=motion / mirror
            std::chrono::steady_clock::time_point nextStateFix{};  // mirror: re-enter state 1 if the AI leaves it
            std::uint32_t stateFixes = 0;
            std::uint32_t snaps = 0;  // controls mode: drift corrections
            std::chrono::steady_clock::time_point nextStopCombat{};
            std::uint32_t combatStops = 0;
            bool shownDead = false;  // the remote player is at 0 health, and the proxy was killed
            std::chrono::steady_clock::time_point deathCheckAt{};
            std::chrono::steady_clock::time_point deathVerifyAt{};
            std::chrono::steady_clock::time_point nextLookLog{};
            std::chrono::steady_clock::time_point lastTurnAt{};
            float bodyYaw = 0.0f;
            bool haveBodyYaw = false;
            float sentPitch = 1000.0f;  // last fCameraPitch written to the proxy graph
            std::chrono::steady_clock::time_point nextPitchWrite{};
            std::chrono::steady_clock::time_point bigErrorSince{};
            std::chrono::steady_clock::time_point nextMoveLog{};
            std::chrono::steady_clock::time_point smallErrorSince{};
            std::uint32_t appliedWeapon = 0;   // weapon equipped on the proxy by us
            int appliedDrawn = -1;
            std::chrono::steady_clock::time_point nextWeaponApply{};
            std::shared_ptr<avatars::WeaponQuery> proxyWeaponQuery;  // read back the proxy's real state
            std::chrono::steady_clock::time_point nextProxyWeaponCheck{};
            std::uint32_t weaponReasserts = 0;
            std::uint32_t lastPathResult = 0;            // path mode: last logged PathToReference result
            Vec3 lastPos{};                              // for velocity (AvatarAnimate)
            std::chrono::steady_clock::time_point lastPosAt{};
            float speed = 0.0f;                          // smoothed horizontal speed, units/s
            float sentSpeed = -1.0f;                     // last values written to the graph
            float sentDirection = -1.0f;
            std::chrono::steady_clock::time_point nextAnimWrite{};
            // Path mode: invisible target marker + latent PathToReference.
            std::uint32_t markerRef = 0;
            std::shared_ptr<std::atomic<std::uint32_t>> markerPending;
            std::shared_ptr<std::atomic<std::uint32_t>> pathDone;  // in flight while value is 0
            std::chrono::steady_clock::time_point pathStartedAt{};
            float pathWalkRun = -1.0f;
            bool pathIdleSnapped = false;
            float velX = 0.0f, velY = 0.0f;              // remote velocity, units/s
            Vec3 pathTarget{};
            std::uint64_t pathCalls = 0;
            std::chrono::steady_clock::time_point nextSnapAt{};
            std::string baseDetail;
        };

        struct RemotePlayer
        {
            std::string name;
            Avatar avatar;
            TransformInterpolator interpolator;
            std::optional<InterpolatedPose> pose;  // this frame's render pose
            std::uint64_t samplesReceived = 0;
            // Newest engine movement input received from this player (AvatarMoveMode=controls).
            std::optional<proto::PlayerControls> controls;
            std::uint32_t controlsSeq = 0;
            std::chrono::steady_clock::time_point controlsAt{};
            std::uint64_t controlsReceived = 0;
            float latestYaw = 0.0f;  // newest received heading (radians)
            float latestPitch = 0.0f;  // newest received look pitch (fCameraPitch, radians)
            Vec3 latestPos{};
            std::uint64_t latestReceivedMs = 0;
            float velX = 0.0f, velY = 0.0f, velZ = 0.0f;  // remote velocity from the newest samples
            std::uint32_t latestSeq = 0;
            bool haveLatest = false;
            std::optional<proto::WeaponState> weapon;  // newest received weapon state
            std::optional<proto::PlayerStatus> status;  // newest received vitals / whereabouts
            // Graph input events from this player not yet replayed on the proxy (controls mode).
            std::vector<std::string> pendingReplays;
            std::uint16_t lastEventSeq = 0;
            bool anyEventSeen = false;
        };

        void RebuildFromRoster(net::NetworkWorker& worker);
        void SampleLocalPlayer(net::NetworkWorker& worker);
        void UpdateRemotePlayers(net::NetworkWorker& worker);
        void LogSummary(net::NetworkWorker& worker);
        void UpdateAvatar(PlayerId id, RemotePlayer& remote);
        void ReleaseAvatar(PlayerId id, Avatar& avatar, const char* why);
        void RemoveRemote(PlayerId id);
        void RemoveAllRemotes();
        void ReapOrphans();
        void CheckDespawns();
        void UpdateVmState();
        void UpdateTemplate();
        [[nodiscard]] bool TemplateDecided() const noexcept;

        std::atomic<net::NetworkWorker*> _worker{ nullptr };
        bool _readLocalPlayer = false;
        bool _inWorld = false;
        bool _avatarsEnabled = false;
        bool _npcSyncEnabled = false;
        bool _menuPatch = true;
        npcsync::Replication _npcs;
        bool _worldSyncEnabled = false;
        worldsync::Replication _world;
        ships::Replication _ships;
        dialogue::Replication _dialogue;
        float _avatarScale = 1.0f;
        AvatarOptions _opt;
        // Animation diagnostics (AvatarAnimDiag).
        animprobe::Snapshot _playerProbe;
        bool _playerProbed = false;
        bool _probeSelfChecked = false;
        std::chrono::steady_clock::time_point _nextPlayerFloatLog{};
        std::chrono::steady_clock::time_point _nextAnimDiag{};
        std::chrono::steady_clock::time_point _nextControlsLog{};
        std::uint64_t _loggedControlsSeq = 0;
        std::uint64_t _sentControlsSeq = 0;
        // Local player's graph input events this frame (AnimEvents) and the outgoing queue.
        std::vector<animevents::CapturedEvent> _frameEvents;
        std::uint64_t _frameEventsDropped = 0;
        struct OutgoingEvent
        {
            std::uint16_t sequence;
            std::string name;
            int sendsLeft;
        };
        std::deque<OutgoingEvent> _outEvents;
        std::uint16_t _nextEventSeq = 0;
        // Local player's weapon (Papyrus query every 500 ms in controls mode).
        std::shared_ptr<avatars::WeaponQuery> _weaponQuery;
        std::optional<proto::WeaponState> _localWeapon;
        std::chrono::steady_clock::time_point _nextWeaponQuery{};
        void ApplyWeapon(PlayerId id, RemotePlayer& remote);
        void DrainAnimEvents();
        void LogNpcEnumeration();
        // In-game UI (Overlay/MetroUi): feed the model, publish snapshots, execute commands.
        void UpdateUi();
        ui::UiModel _ui;
        std::uint64_t _publishedUiVersion = 0;
        std::chrono::steady_clock::time_point _nextUiUpdate{};
        std::chrono::steady_clock::time_point _nextSteamCheck{};
        std::chrono::steady_clock::time_point _nextIpTry{};
        std::string _localIp;
        std::string _joinCode;
        std::future<std::vector<net::discovery::DiscoveredServer>> _discovery;  // LAN browse in flight
        bool _steamAvailable = false;
        // Player status (health, level, planet) and groups: party frames + overhead markers.
        void UpdateLocalStatus();
        void PublishHud();
        [[nodiscard]] const proto::GroupInfo* MyGroup() const;
        [[nodiscard]] float DistanceTo(const RemotePlayer& r) const;  // meters, -1 if not comparable
        [[nodiscard]] bool GameMenuOpen();
        std::shared_ptr<avatars::StatusQuery> _statusQuery;
        std::optional<proto::PlayerStatus> _localStatus;
        std::chrono::steady_clock::time_point _nextStatusQuery{};
        proto::GroupState _groupState;
        PlayerId _localPlayerId = kInvalidPlayerId;
        std::chrono::steady_clock::time_point _nextMenuCheck{};
        bool _menuOpen = false;
        // Hostility sync (docs/NPC_SYNC_ARCHITECTURE.md): report NPCs that enter combat against a
        // player, apply reports from others with StartCombat.
        void UpdateNpcCombat(net::NetworkWorker& worker);
        void ApplyNpcCombat(const proto::NpcCombat& c, net::NetworkWorker& worker);
        std::chrono::steady_clock::time_point _nextCombatScan{};
        std::map<std::uint32_t, std::shared_ptr<std::atomic<std::uint32_t>>> _combatQueries;  // npc -> target
        std::map<std::pair<std::uint32_t, PlayerId>, std::chrono::steady_clock::time_point> _combatKnown;
        std::chrono::steady_clock::time_point _nextNpcLog{};
        std::size_t _lastNpcCount = 0;
        std::uint32_t _npcLogs = 0;
        std::chrono::steady_clock::time_point _sentControlsAt{};
        Vec3 _lastPlayerPos{};
        std::chrono::steady_clock::time_point _lastPlayerPosAt{};
        float _playerSpeed = 0.0f;  // measured horizontal speed, units/s
        std::uint64_t _animEventsDropped = 0;
        void UpdateAnimDiag();
        void DriveLocomotion(Avatar& avatar, const InterpolatedPose& pose);
        void DrivePath(PlayerId id, RemotePlayer& remote);
        void DriveControls(PlayerId id, RemotePlayer& remote);
        bool _vmReady = false;
        bool _vmSettled = false;
        std::chrono::steady_clock::time_point _vmReadySince{};
        // Nearby NPC whose data-file base the avatars copy (0 = not found yet / player base).
        std::uint32_t _templateActor = 0;
        std::uint32_t _templateBase = 0;
        std::uint32_t _templateTries = 0;
        std::shared_ptr<std::atomic<std::uint32_t>> _templateQuery;
        std::chrono::steady_clock::time_point _templateQueriedAt{};
        std::chrono::steady_clock::time_point _nextTemplateTry{};
        // Spawns that were still in flight when their avatar was released.
        std::vector<std::shared_ptr<std::atomic<std::uint32_t>>> _orphans;
        // Despawned refs to re-check (Disable/Delete run asynchronously on the VM).
        std::vector<std::pair<std::uint32_t, std::chrono::steady_clock::time_point>> _despawnChecks;
        std::uint32_t _localLocationId = 0;
        std::string _localLocationName;
        std::chrono::steady_clock::time_point _nextSample{};
        std::chrono::steady_clock::time_point _nextLocalLog{};
        std::chrono::steady_clock::time_point _nextSummary{};
        std::vector<net::ClientEvent> _events;
        std::vector<net::RemoteSample> _samples;
        std::map<PlayerId, RemotePlayer> _remotes;
    };
}
