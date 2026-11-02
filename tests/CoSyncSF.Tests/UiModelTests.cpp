#include "cosync/ui/UiModel.h"

#include <gtest/gtest.h>

using namespace cosync;
using namespace cosync::ui;

TEST(UiIntent, JoinNormalizesAddressAndDefaultsPort)
{
    std::string error;
    const auto c = ParseIntent("join", { { "address", " 192.168.1.20 " } }, error);
    ASSERT_TRUE(c) << error;
    const auto& join = std::get<cmd::JoinGame>(*c);
    EXPECT_EQ(join.address, "192.168.1.20:37015");
    EXPECT_TRUE(join.password.empty());
}

TEST(UiIntent, JoinRejectsHostnamesAndGarbage)
{
    std::string error;
    EXPECT_FALSE(ParseIntent("join", { { "address", "example.com:37015" } }, error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(ParseIntent("join", { { "address", "" } }, error));
    EXPECT_FALSE(ParseIntent("join", { { "address", "1.2.3.4:99999" } }, error));
}

TEST(UiIntent, HostValidatesPortAndPlayerCount)
{
    std::string error;
    auto c = ParseIntent("host", { { "name", "Crew" }, { "port", "40000" }, { "maxPlayers", "4" } }, error);
    ASSERT_TRUE(c) << error;
    const auto& host = std::get<cmd::HostGame>(*c);
    EXPECT_EQ(host.serverName, "Crew");
    EXPECT_EQ(host.port, 40000);
    EXPECT_EQ(host.maxPlayers, 4);

    EXPECT_FALSE(ParseIntent("host", { { "port", "0" } }, error));
    EXPECT_FALSE(ParseIntent("host", { { "port", "70000" } }, error));
    EXPECT_FALSE(ParseIntent("host", { { "maxPlayers", "1" } }, error));
    EXPECT_FALSE(ParseIntent("host", { { "maxPlayers", "abc" } }, error));
    EXPECT_FALSE(ParseIntent("host", { { "name", std::string(65, 'x') } }, error));

    c = ParseIntent("host", {}, error);  // defaults
    ASSERT_TRUE(c) << error;
    EXPECT_EQ(std::get<cmd::HostGame>(*c).port, 37015);
}

TEST(UiIntent, RejectsLongPasswordAndUnknownActions)
{
    std::string error;
    EXPECT_FALSE(ParseIntent("join", { { "address", "127.0.0.1" }, { "password", std::string(65, 'p') } }, error));
    EXPECT_FALSE(ParseIntent("selfDestruct", {}, error));
    EXPECT_EQ(error, "Unknown action.");
}

TEST(UiIntent, KickNameAndTab)
{
    std::string error;
    EXPECT_EQ(std::get<cmd::KickPlayer>(*ParseIntent("kick", { { "player", "3" } }, error)).player, 3u);
    EXPECT_FALSE(ParseIntent("kick", { { "player", "0" } }, error));
    EXPECT_FALSE(ParseIntent("kick", { { "player", "-1" } }, error));
    EXPECT_EQ(std::get<cmd::SetDisplayName>(*ParseIntent("setName", { { "name", " Nova " } }, error)).name, "Nova");
    EXPECT_FALSE(ParseIntent("setName", { { "name", std::string(33, 'n') } }, error));
    EXPECT_EQ(std::get<cmd::SelectTab>(*ParseIntent("selectTab", { { "tab", "players" } }, error)).tab, Tab::Players);
    EXPECT_FALSE(ParseIntent("selectTab", { { "tab", "nope" } }, error));
}

TEST(UiTheme, ParsesTokensAndReportsBadLines)
{
    std::string warnings;
    const auto t = ParseTheme("# comment\nname = Deep Space\naccent = #4FB3D9\nspacing = 16\n"
                              "panelOpacity = 1.5\nbogus = 1\ntransitionMs = 250\nnot a pair\n",
                              warnings);
    EXPECT_EQ(t.name, "Deep Space");
    EXPECT_EQ(t.accent, 0x4FB3D9u);
    EXPECT_FLOAT_EQ(t.spacing, 16.0f);
    EXPECT_FLOAT_EQ(t.panelOpacity, Theme{}.panelOpacity);  // out of range: default kept
    EXPECT_EQ(t.transitionMs, 250u);
    EXPECT_NE(warnings.find("line 5"), std::string::npos);
    EXPECT_NE(warnings.find("unknown key 'bogus'"), std::string::npos);
    EXPECT_NE(warnings.find("line 8"), std::string::npos);
}

TEST(UiModel, VersionChangesOnlyWhenContentChanges)
{
    UiModel m;
    const auto v0 = m.Snapshot().version;
    m.SetConnection(net::ClientState::InSession, "joined", true, "Crew", "127.0.0.1:37015", 1, 30, 0);
    const auto v1 = m.Snapshot().version;
    EXPECT_GT(v1, v0);
    m.SetConnection(net::ClientState::InSession, "joined", true, "Crew", "127.0.0.1:37015", 1, 30, 0);
    EXPECT_EQ(m.Snapshot().version, v1);

    m.SetPlayers({ { 2, "B" }, { 1, "A", true, true } });
    const auto v2 = m.Snapshot().version;
    EXPECT_GT(v2, v1);
    EXPECT_EQ(m.Snapshot().players.front().id, 1u);  // sorted by id
    m.SetPlayers({ { 1, "A", true, true }, { 2, "B" } });  // same set, other order
    EXPECT_EQ(m.Snapshot().version, v2);
}

TEST(UiModel, TabIsLocalSessionActionsAreNot)
{
    UiModel m;
    EXPECT_TRUE(m.ApplyLocal(cmd::SelectTab{ Tab::Diagnostics }));
    EXPECT_EQ(m.Snapshot().tab, Tab::Diagnostics);
    EXPECT_FALSE(m.ApplyLocal(cmd::LeaveGame{}));
    EXPECT_FALSE(m.ApplyLocal(cmd::JoinGame{ "127.0.0.1:37015", "" }));
}

TEST(UiModel, DiagnosticsAreBounded)
{
    UiModel m;
    for (int i = 0; i < 100; ++i) m.AddDiagnostic("line " + std::to_string(i));
    ASSERT_EQ(m.Snapshot().diagnostics.size(), UiModel::kMaxDiagnostics);
    EXPECT_EQ(m.Snapshot().diagnostics.back(), "line 99");
    EXPECT_EQ(m.Snapshot().diagnostics.front(), "line 36");
}

TEST(UiJson, EscapesAndContainsSections)
{
    UiModel m;
    m.SetPlayers({ { 1, "Quote\"Back\\slash\nNew", true, true, true, "valid" } });
    m.AddDiagnostic(std::string("ctrl\x01"));
    const auto json = ToJson(m.Snapshot());
    EXPECT_NE(json.find(R"("name":"Quote\"Back\\slash\nNew")"), std::string::npos) << json;
    EXPECT_NE(json.find(R"("ctrl\u0001")"), std::string::npos) << json;
    EXPECT_NE(json.find(R"("accent":"#E8A33D")"), std::string::npos) << json;
    EXPECT_NE(json.find(R"("state":")"), std::string::npos);
    EXPECT_EQ(json.front(), '{');
    EXPECT_EQ(json.back(), '}');
    // Balanced braces/brackets outside strings.
    int depth = 0;
    bool inString = false, escaped = false;
    for (const char c : json) {
        if (inString) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') inString = false;
            continue;
        }
        if (c == '"') inString = true;
        else if (c == '{' || c == '[') ++depth;
        else if (c == '}' || c == ']') --depth;
        EXPECT_GE(depth, 0);
    }
    EXPECT_EQ(depth, 0);
}
