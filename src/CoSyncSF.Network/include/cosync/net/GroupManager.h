#pragma once

#include "cosync/shared/Protocol.h"

#include <string>

namespace cosync::net
{
    // Server-side group rules (pure logic; owned by SessionServer). Groups decide who shares party
    // frames and overhead markers.
    //   Create  - only when not in a group; the creator leads.
    //   Invite  - leader only; target not in a group and not already invited; group not full.
    //   Accept  - only with a pending invite; joins and drops the player's other invites.
    //   Decline - removes the invite.
    //   Leave   - the next member leads if the leader leaves; empty groups are deleted.
    //   Kick    - leader only.
    class GroupManager
    {
    public:
        // Applies a request from `actor`. True if the state changed; otherwise `error` says why.
        bool Apply(PlayerId actor, const proto::GroupRequest& request, std::string& error);
        // A player left the session: remove them everywhere. True if the state changed.
        bool RemovePlayer(PlayerId player);
        [[nodiscard]] proto::GroupState State() const;
        [[nodiscard]] const proto::GroupInfo* GroupOf(PlayerId player) const;

    private:
        proto::GroupInfo* Find(std::uint32_t id);
        proto::GroupInfo* MemberGroup(PlayerId player);
        void Leave(PlayerId player);
        std::vector<proto::GroupInfo> _groups;
        std::uint32_t _nextId = 1;
        std::uint32_t _version = 0;
    };
}
