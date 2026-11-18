/**
 * @file tests/unit/test_token_user_session.cpp
 * @brief Which Windows session's user may read the local host credential (src/confighttp/token_user_session.h), over a fake WTS layer.
 */
#include "src/confighttp/token_user_session.h"

#include <gtest/gtest.h>

namespace tus = confighttp::token_user_session;

namespace {
  tus::wts_view_t
  view(std::uint32_t console, std::vector<tus::session_t> sessions) {
    return { [console] { return console; }, [sessions] { return sessions; } };
  }
}  // namespace

TEST(TokenUserSession, ConsoleUserIsPreferred) {
  // A signed-in console user wins even when another user is active over RDP.
  EXPECT_EQ(tus::pick(view(1, { { 0, false, false }, { 1, true, true }, { 3, true, true } })), 1u);
}

TEST(TokenUserSession, LoneRdpUserGetsItWhenTheConsoleHasNone) {
  // Live case 2026-11-18: mahle on rdp-tcp#0 (session 2), the console (session 5) at the sign-in screen.
  EXPECT_EQ(tus::pick(view(5, { { 0, false, false }, { 2, true, true }, { 5, true, false } })), 2u);
}

TEST(TokenUserSession, NoConsoleSessionStillFindsTheActiveUser) {
  EXPECT_EQ(tus::pick(view(tus::no_session, { { 0, false, false }, { 2, true, true } })), 2u);
}

TEST(TokenUserSession, TwoActiveUsersWithoutAConsoleUserGrantNobody) {
  // Never more than one user: an ambiguous choice leaves only SYSTEM and Administrators.
  EXPECT_FALSE(tus::pick(view(5, { { 2, true, true }, { 3, true, true }, { 5, true, false } })).has_value());
}

TEST(TokenUserSession, DisconnectedAndServiceSessionsDoNotCount) {
  // A disconnected RDP session (the user left) and session 0 are not interactive.
  EXPECT_FALSE(tus::pick(view(5, { { 0, true, true }, { 2, false, true }, { 5, true, false } })).has_value());
  EXPECT_EQ(tus::pick(view(5, { { 2, false, true }, { 3, true, true }, { 5, true, false } })), 3u);
}

TEST(TokenUserSession, NobodySignedInGrantsNobody) {
  EXPECT_FALSE(tus::pick(view(1, { { 0, false, false }, { 1, true, false } })).has_value());
  EXPECT_FALSE(tus::pick(view(tus::no_session, {})).has_value());
}

TEST(TokenUserSession, AnUnreadableSessionFailsClosed) {
  // If the user of any session cannot be read, the choice cannot be trusted: a signed-in console user or a second active user
  // could be hidden behind the failure.
  EXPECT_FALSE(tus::pick(view(5, { { 2, true, true }, { 5, true, false, false } })).has_value());
  EXPECT_FALSE(tus::pick(view(5, { { 2, true, true }, { 3, true, false, false }, { 5, true, false } })).has_value());
}

// Upstream #1328 chooses the session (platf::retrieve_interactive_users_token: the console user, else the first active RDP
// session, else the first active user). The credential goes to it only when that is also the single session pick() allows.
TEST(TokenUserSession, UpstreamChoiceIsGrantedWhenItIsTheOnlyOne) {
  EXPECT_EQ(tus::grant(view(5, { { 2, true, true }, { 5, true, false } }), 2u), 2u);
  EXPECT_EQ(tus::grant(view(1, { { 1, true, true }, { 3, true, true } }), 1u), 1u);
}

TEST(TokenUserSession, UpstreamFirstRdpPickAmongTwoUsersGrantsNobody) {
  // Upstream would take the first active RDP session; with two signed-in users that is a guess, so nobody gets it.
  EXPECT_FALSE(tus::grant(view(5, { { 2, true, true }, { 3, true, true }, { 5, true, false } }), 2u).has_value());
}

TEST(TokenUserSession, DisagreementOrNoUpstreamSessionGrantsNobody) {
  EXPECT_FALSE(tus::grant(view(5, { { 2, true, true }, { 5, true, false } }), 3u).has_value());
  EXPECT_FALSE(tus::grant(view(5, { { 2, true, true }, { 5, true, false } }), std::nullopt).has_value());
}
