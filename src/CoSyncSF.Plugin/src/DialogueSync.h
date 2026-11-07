#pragma once

#include "cosync/net/NetworkWorker.h"

#include <chrono>
#include <cstdint>
#include <string>

// Conversations (issue #16), first step: what is said in one player's conversation reaches the
// players near them as text, so they can follow it. The lines are read from the game's own
// subtitles (ShowSubtitleEvent). Speaking them aloud on the other copies needs the line's form and
// is not done. Main thread only, except the event sink, which only copies strings.
namespace cosync::plugin::dialogue
{
    class Replication
    {
    public:
        void Tick(net::NetworkWorker& worker, bool inSession);
        void OnLine(const proto::DialogueLine& line);

    private:
        bool _sinkInstalled = false;
        std::string _lastSent;
        std::uint64_t _sent = 0;
        std::uint64_t _seen = 0;
        std::uint64_t _shown = 0;
    };
}
