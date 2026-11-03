#include "cosync/net/GroupManager.h"

#include <algorithm>

namespace cosync::net
{
    namespace
    {
        bool Contains(const std::vector<PlayerId>& v, PlayerId id) { return std::ranges::find(v, id) != v.end(); }
        void Erase(std::vector<PlayerId>& v, PlayerId id) { std::erase(v, id); }
    }

    proto::GroupInfo* GroupManager::Find(std::uint32_t id)
    {
        const auto it = std::ranges::find(_groups, id, &proto::GroupInfo::id);
        return it == _groups.end() ? nullptr : &*it;
    }

    proto::GroupInfo* GroupManager::MemberGroup(PlayerId player)
    {
        const auto it = std::ranges::find_if(_groups, [&](const auto& g) { return Contains(g.members, player); });
        return it == _groups.end() ? nullptr : &*it;
    }

    const proto::GroupInfo* GroupManager::GroupOf(PlayerId player) const
    {
        return const_cast<GroupManager*>(this)->MemberGroup(player);
    }

    void GroupManager::Leave(PlayerId player)
    {
        auto* g = MemberGroup(player);
        if (!g) return;
        Erase(g->members, player);
        if (g->members.empty()) {
            const auto id = g->id;
            std::erase_if(_groups, [id](const auto& x) { return x.id == id; });
            return;
        }
        if (g->leader == player) g->leader = g->members.front();
    }

    bool GroupManager::Apply(PlayerId actor, const proto::GroupRequest& r, std::string& error)
    {
        using A = proto::GroupAction;
        auto* mine = MemberGroup(actor);
        switch (r.action) {
        case A::Create:
            if (mine) {
                error = "already in a group";
                return false;
            }
            if (_groups.size() >= proto::kMaxGroups) {
                error = "too many groups";
                return false;
            }
            for (auto& g : _groups) Erase(g.invited, actor);
            _groups.push_back({ _nextId++, actor, { actor }, {} });
            break;
        case A::Invite:
            if (!mine || mine->leader != actor) {
                error = "only the group leader can invite";
                return false;
            }
            if (r.target == actor || r.target == kInvalidPlayerId || MemberGroup(r.target)) {
                error = "that player cannot be invited";
                return false;
            }
            if (Contains(mine->invited, r.target)) {
                error = "already invited";
                return false;
            }
            if (mine->members.size() + mine->invited.size() >= proto::kMaxGroupMembers) {
                error = "the group is full";
                return false;
            }
            mine->invited.push_back(r.target);
            break;
        case A::Accept: {
            auto* g = Find(r.groupId);
            if (!g || !Contains(g->invited, actor)) {
                error = "no such invite";
                return false;
            }
            if (mine) Leave(actor);
            g = Find(r.groupId);  // Leave may have erased another group (pointers into _groups move)
            if (!g) {
                error = "the group no longer exists";
                return false;
            }
            Erase(g->invited, actor);
            g->members.push_back(actor);
            for (auto& other : _groups) Erase(other.invited, actor);
            break;
        }
        case A::Decline: {
            auto* g = Find(r.groupId);
            if (!g || !Contains(g->invited, actor)) {
                error = "no such invite";
                return false;
            }
            Erase(g->invited, actor);
            break;
        }
        case A::Leave:
            if (!mine) {
                error = "not in a group";
                return false;
            }
            Leave(actor);
            break;
        case A::Kick:
            if (!mine || mine->leader != actor) {
                error = "only the group leader can remove members";
                return false;
            }
            if (r.target == actor || !Contains(mine->members, r.target)) {
                error = "that player is not in your group";
                return false;
            }
            Erase(mine->members, r.target);
            break;
        }
        ++_version;
        return true;
    }

    bool GroupManager::RemovePlayer(PlayerId player)
    {
        bool changed = MemberGroup(player) != nullptr;
        Leave(player);
        for (auto& g : _groups) {
            if (Contains(g.invited, player)) {
                Erase(g.invited, player);
                changed = true;
            }
        }
        if (changed) ++_version;
        return changed;
    }

    proto::GroupState GroupManager::State() const { return { _version, _groups }; }
}
