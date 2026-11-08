#include "DialogueSync.h"

#include "Avatars.h"
#include "NpcSync.h"

#include "cosync/shared/Log.h"

#include <RE/Starfield.h>

#include <format>
#include <mutex>
#include <vector>

namespace cosync::plugin::dialogue
{
    namespace
    {
        struct Captured
        {
            std::string speaker;
            std::string text;
            bool isPlayer = false;
        };

        std::mutex g_lock;
        std::vector<Captured> g_lines;

        // On whatever thread shows the subtitle: the strings are only valid during the call.
        class SubtitleSink final : public RE::BSTEventSink<RE::ShowSubtitleEvent::Event>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RE::ShowSubtitleEvent::Event& e, RE::BSTEventSource<RE::ShowSubtitleEvent::Event>*) override
            {
                if (!e.subtitleText || !*e.subtitleText) return RE::BSEventNotifyControl::kContinue;
                std::scoped_lock lock(g_lock);
                if (g_lines.size() < 64) g_lines.push_back({ e.speakerName ? e.speakerName : "", e.subtitleText, e.isPlayer });
                return RE::BSEventNotifyControl::kContinue;
            }
        };
        SubtitleSink g_sink;

        // A line belongs to the local player's conversation when its speaker is in dialogue with the
        // player (Actor+892 bit 8, ObjectReference.IsInDialogueWithPlayer 0x20761F0), not to the ambient
        // chatter every copy already shows for itself. The menu test (UI "DialogueMenu") stayed false
        // through a whole greeting on 2026-11-07, so the speaker decides.
        bool InConversation(const std::string& speaker)
        {
            const auto e = npcsync::EnumerateHighActors();
            if (!e.ok) return false;
            for (const auto& a : e.actors) {
                auto* form = RE::TESForm::LookupByID(a.formId);
                auto* actor = form ? form->As<RE::Actor>() : nullptr;
                if (!actor) continue;
                if ((*reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const std::byte*>(actor) + 892) & 8u) == 0) continue;
                const auto base = actor->GetBaseObject();
                auto* full = base ? starfield_cast<RE::TESFullName*>(base.get()) : nullptr;
                const char* name = full ? full->GetFullName() : nullptr;
                if (speaker.empty() || (name && speaker == name)) return true;
            }
            return false;
        }
    }

    void Replication::Tick(net::NetworkWorker& worker, bool inSession)
    {
        if (!_sinkInstalled) {
            _sinkInstalled = true;
            RE::ShowSubtitleEvent::Event::GetEventSource()->RegisterSink(&g_sink);
            log::Info("[dialogue] watching subtitles");
        }
        std::vector<Captured> lines;
        {
            std::scoped_lock lock(g_lock);
            lines.swap(g_lines);
        }
        if (!inSession || lines.empty()) return;
        for (const auto& l : lines) {
            // The player's own lines have the player as speaker; any conversation going on counts.
            const bool ours = InConversation(l.isPlayer ? std::string() : l.speaker);
            if (++_seen <= 20) log::Info("[dialogue] subtitle '{}': {} (player {}, conversation {})", l.speaker, l.text, l.isPlayer, ours);
            if (!ours) continue;
            const auto key = l.speaker + "\n" + l.text;
            if (key == _lastSent) continue;  // the game raises the same subtitle more than once
            _lastSent = key;
            worker.SubmitDialogueLine({ kInvalidPlayerId, l.speaker.substr(0, proto::kMaxSpeakerBytes), l.text.substr(0, proto::kMaxLineBytes), l.isPlayer });
            if (++_sent <= 50) log::Info("[dialogue] sent '{}': {}", l.speaker, l.text);
        }
    }

    void Replication::OnLine(const proto::DialogueLine& line)
    {
        const auto who = line.isPlayer ? std::format("Player {}", line.playerId) : (line.speaker.empty() ? std::string("Someone") : line.speaker);
        avatars::Notify(std::format("{}: {}", who, line.text));
        if (++_shown <= 50) log::Info("[dialogue] player {}'s conversation, '{}': {}", line.playerId, who, line.text);
    }
}
