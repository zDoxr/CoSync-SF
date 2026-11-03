#include "cosync/net/GroupManager.h"

#include <gtest/gtest.h>

using namespace cosync;
using namespace cosync::net;
using proto::GroupAction;

namespace
{
    bool Do(GroupManager& g, PlayerId actor, GroupAction a, PlayerId target = 0, std::uint32_t group = 0)
    {
        std::string error;
        return g.Apply(actor, proto::GroupRequest{ a, target, group }, error);
    }
}

TEST(Groups, CreateInviteAcceptLeave)
{
    GroupManager g;
    ASSERT_TRUE(Do(g, 1, GroupAction::Create));
    const auto id = g.GroupOf(1)->id;
    EXPECT_FALSE(Do(g, 2, GroupAction::Invite, 3));  // not a leader
    ASSERT_TRUE(Do(g, 1, GroupAction::Invite, 2));
    EXPECT_FALSE(Do(g, 1, GroupAction::Invite, 2));  // already invited
    EXPECT_FALSE(Do(g, 3, GroupAction::Accept, 0, id));  // not invited
    ASSERT_TRUE(Do(g, 2, GroupAction::Accept, 0, id));
    EXPECT_EQ(g.GroupOf(2)->id, id);
    EXPECT_TRUE(g.GroupOf(2)->invited.empty());
    ASSERT_TRUE(Do(g, 1, GroupAction::Leave));  // leader leaves: 2 leads
    EXPECT_EQ(g.GroupOf(2)->leader, 2u);
    ASSERT_TRUE(Do(g, 2, GroupAction::Leave));  // empty: deleted
    EXPECT_TRUE(g.State().groups.empty());
}

TEST(Groups, DeclineKickAndDisconnect)
{
    GroupManager g;
    ASSERT_TRUE(Do(g, 1, GroupAction::Create));
    const auto id = g.GroupOf(1)->id;
    ASSERT_TRUE(Do(g, 1, GroupAction::Invite, 2));
    ASSERT_TRUE(Do(g, 1, GroupAction::Invite, 3));
    ASSERT_TRUE(Do(g, 2, GroupAction::Decline, 0, id));
    ASSERT_TRUE(Do(g, 3, GroupAction::Accept, 0, id));
    EXPECT_FALSE(Do(g, 3, GroupAction::Kick, 1));  // not the leader
    ASSERT_TRUE(Do(g, 1, GroupAction::Kick, 3));
    EXPECT_EQ(g.GroupOf(3), nullptr);
    ASSERT_TRUE(Do(g, 1, GroupAction::Invite, 4));
    EXPECT_TRUE(g.RemovePlayer(4));  // pending invitee disconnects
    EXPECT_TRUE(g.GroupOf(1)->invited.empty());
    EXPECT_TRUE(g.RemovePlayer(1));
    EXPECT_TRUE(g.State().groups.empty());
}

TEST(Groups, CapacityAndOneGroupPerPlayer)
{
    GroupManager g;
    ASSERT_TRUE(Do(g, 1, GroupAction::Create));
    EXPECT_FALSE(Do(g, 1, GroupAction::Create));
    for (PlayerId p = 2; p <= proto::kMaxGroupMembers; ++p) ASSERT_TRUE(Do(g, 1, GroupAction::Invite, p));
    EXPECT_FALSE(Do(g, 1, GroupAction::Invite, 99));  // members + invites at capacity
}
