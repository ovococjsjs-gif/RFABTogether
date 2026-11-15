// Work plan 6.1 / defect M25: a quest's RFAB XP is paid once, in every arrival order.
//
// The modpack pays quest XP from the quest's own stage script (`XP.OnQuestComplete(self)` in the
// final stage of the quest). Quest stages are synced, so the script runs, and pays, on every
// client. The party's shared XP wallet then shares the award of the player who handed the quest
// in, and the other player would be paid twice. The tracker cancels that second payment by the
// order in which three things reach the other player's client:
//
//   S  the synced quest stage is applied,
//   L  the local quest script's award is seen by the wallet poll,
//   R  the partner's copy of the award arrives.
//
// 4.1.0 handled S, L, R and R, S, L. It did not handle S, R, L: R found nothing local to be
// absorbed by, was paid, and L was paid on top of it. R and L leave the two clients within the
// same fraction of a second, so this order is as likely as S, L, R.
//
// `Client` below does what the game client does around the tracker
// (CharacterService::OnNotifySyncRfabExperience and RunRfabExperienceUpdates), so the cases state
// the result the player sees: the XP in the wallet.

#include <catch2/catch.hpp>

#include <CoopExperienceLedger.h>
#include <RfabModpackCompat.h>
#include <Session/Features.h>

#include <algorithm>

using namespace CoopExperienceLedger;

namespace
{
constexpr uint64_t kT0 = 10'000;
constexpr int32_t kStartWallet = 1'000;

struct Client
{
    ClientRfabXpTracker Tracker{};
    int32_t Wallet{kStartWallet};
    float Multiplier{1.0f};
    uint32_t Broadcast{0};

    explicit Client(bool aSingleAward)
    {
        Tracker.SetNetRemoteAwardsInsideQuestWindow(aSingleAward);
        // The first poll only takes the wallet as the baseline.
        (void)Tracker.EvaluateLocalWalletPoll(Wallet, true, false, Multiplier, kT0 - 5'000);
    }

    // The synced quest stage is applied on this client.
    void Stage(uint64_t aNowMs) { Tracker.OnRemoteQuestStageApplied(aNowMs); }

    // The local quest script (or any other local source) pays into the wallet.
    void Script(int32_t aAmount) { Wallet += aAmount; }

    // The partner's award arrives. Returns what was paid.
    int32_t Remote(uint32_t aBaseXp, uint64_t aNowMs)
    {
        const int32_t delta = Tracker.EvaluateIncomingRemoteXp(aBaseXp, Multiplier, aNowMs);
        if (delta <= 0)
            return 0;
        const int32_t before = Wallet;
        Wallet += delta;
        Tracker.CommitAppliedRemoteXp(before, delta, aNowMs);
        return delta;
    }

    // The wallet poll. Returns what was taken back.
    int32_t Poll(uint64_t aNowMs)
    {
        const auto decision = Tracker.EvaluateLocalWalletPoll(Wallet, true, false, Multiplier, aNowMs);
        if (decision.RefundToApply > 0)
        {
            Wallet = std::max(0, Wallet - decision.RefundToApply);
            Tracker.CommitLocalRefundApplied(decision.RefundToApply, Wallet);
        }
        Broadcast += decision.BroadcastBaseXp;
        return decision.RefundToApply;
    }

    [[nodiscard]] int32_t Gained() const { return Wallet - kStartWallet; }
};
} // namespace

TEST_CASE("6.1: the partner's quest award that arrives before the local one is taken back", "[rfab][xp][quest][m25]")
{
    Client client{true};

    client.Stage(kT0);
    CHECK(client.Remote(100, kT0 + 150) == 100);
    CHECK(client.Tracker.GetRemoteAwardsRememberedInWindow() == 1);

    client.Script(100);
    CHECK(client.Poll(kT0 + 400) == 100);

    CHECK(client.Gained() == 100);
    CHECK(client.Broadcast == 0);
    CHECK(client.Tracker.GetRemoteAwardsRememberedInWindow() == 0);
    // The local award is settled: a later packet in the same window is not absorbed by it.
    CHECK(client.Tracker.GetLocalQuestXpGainedInWindow() == 0);
    CHECK(client.Remote(40, kT0 + 900) == 40);
}

TEST_CASE("6.1: with the switch off the same order pays twice, as 4.1.0 did", "[rfab][xp][quest][m25]")
{
    Client client{false};

    client.Stage(kT0);
    CHECK(client.Remote(100, kT0 + 150) == 100);
    CHECK(client.Tracker.GetRemoteAwardsRememberedInWindow() == 0);

    client.Script(100);
    CHECK(client.Poll(kT0 + 400) == 0);
    CHECK(client.Gained() == 200);
}

TEST_CASE("6.1: all three arrival orders leave the same XP in the wallet", "[rfab][xp][quest][m25]")
{
    SECTION("stage, local award, partner's award")
    {
        Client client{true};
        client.Stage(kT0);
        client.Script(100);
        CHECK(client.Poll(kT0 + 150) == 0);
        CHECK(client.Remote(100, kT0 + 400) == 0);
        CHECK(client.Gained() == 100);
        CHECK(client.Broadcast == 0);
    }

    SECTION("partner's award, stage, local award")
    {
        Client client{true};
        CHECK(client.Remote(100, kT0 - 200) == 100);
        client.Stage(kT0);
        client.Script(100);
        CHECK(client.Poll(kT0 + 250) == 100);
        CHECK(client.Gained() == 100);
        CHECK(client.Broadcast == 0);
    }

    SECTION("stage, partner's award, local award")
    {
        Client client{true};
        client.Stage(kT0);
        CHECK(client.Remote(100, kT0 + 150) == 100);
        client.Script(100);
        CHECK(client.Poll(kT0 + 400) == 100);
        CHECK(client.Gained() == 100);
        CHECK(client.Broadcast == 0);
    }
}

TEST_CASE("6.1: a second update of the same quest does not count the remembered award again", "[rfab][xp][quest][m25]")
{
    // A finished quest usually sends two updates in a row: the final stage, then "stopped".
    Client client{true};

    client.Stage(kT0);
    CHECK(client.Remote(100, kT0 + 150) == 100);
    client.Stage(kT0 + 300);

    // The look-back of the second update must find nothing: the award is already remembered.
    CHECK(client.Tracker.GetCreditToRefundOnLocalQuestXp() == 0);
    CHECK(client.Tracker.GetRemoteAwardsRememberedInWindow() == 1);

    client.Script(100);
    CHECK(client.Poll(kT0 + 500) == 100);
    CHECK(client.Gained() == 100);
    CHECK(client.Tracker.GetCreditToRefundOnLocalQuestXp() == 0);
    CHECK(client.Tracker.GetRemoteAwardsRememberedInWindow() == 0);
}

TEST_CASE("6.1: an award of a different size is not taken for the quest award", "[rfab][xp][quest][m25]")
{
    // A stage that pays nothing, the partner's kill and this player's own kill in the same seconds.
    Client client{true};

    client.Stage(kT0);
    CHECK(client.Remote(40, kT0 + 500) == 40);
    client.Script(150);
    CHECK(client.Poll(kT0 + 900) == 0);
    CHECK(client.Gained() == 190);

    // When the window is over the remembered award is forgotten, and a local award of that
    // size is ordinary XP again: kept and shared.
    const uint64_t later = kT0 + kQuestXpWindowDurationMs + 1'000;
    CHECK(client.Poll(later) == 0);
    CHECK(client.Tracker.GetRemoteAwardsRememberedInWindow() == 0);
    client.Script(40);
    CHECK(client.Poll(later + 250) == 0);
    CHECK(client.Gained() == 230);
    CHECK(client.Broadcast == 40);
}

TEST_CASE("6.1: the same quest award rounded by two personal multipliers is still recognised", "[rfab][xp][quest][m25]")
{
    SECTION("what counts as the same award")
    {
        CHECK(IsSameQuestAward(100, 100));
        CHECK(IsSameQuestAward(120, 125));
        CHECK(IsSameQuestAward(125, 120));
        CHECK(IsSameQuestAward(1'000, 1'020));
        CHECK_FALSE(IsSameQuestAward(1'000, 1'025));
        CHECK_FALSE(IsSameQuestAward(100, 110));
        CHECK_FALSE(IsSameQuestAward(20, 30));
        CHECK_FALSE(IsSameQuestAward(0, 5));
        CHECK_FALSE(IsSameQuestAward(5, 0));
        CHECK_FALSE(IsSameQuestAward(-5, -5));
    }

    SECTION("the player ends with what his own quest script paid")
    {
        // The partner's copy came out 5 XP higher after the two roundings.
        Client client{true};
        client.Stage(kT0);
        CHECK(client.Remote(125, kT0 + 150) == 125);
        client.Script(120);
        CHECK(client.Poll(kT0 + 400) == 125);
        CHECK(client.Gained() == 120);
    }

    SECTION("with a personal multiplier on the receiving side")
    {
        // 500 base at 1.15: the partner's copy is scaled to 575, the local script pays 575.
        Client client{true};
        client.Multiplier = 1.15f;
        client.Stage(kT0);
        CHECK(client.Remote(500, kT0 + 150) == 575);
        client.Script(575);
        CHECK(client.Poll(kT0 + 400) == 575);
        CHECK(client.Gained() == 575);
    }
}

TEST_CASE("6.1: remembered awards are few and short-lived", "[rfab][xp][quest][m25]")
{
    SECTION("the oldest of more than four is dropped")
    {
        Client client{true};
        client.Stage(kT0);
        const uint32_t awards[] = {100, 200, 300, 400, 500};
        uint64_t now = kT0 + 100;
        for (const uint32_t award : awards)
            CHECK(client.Remote(award, now += 50) == static_cast<int32_t>(award));
        CHECK(client.Tracker.GetRemoteAwardsRememberedInWindow() == kMaxRemoteAwardsRememberedInQuestWindow);

        // 100 was the oldest and is gone; 500 is still there.
        client.Script(100);
        CHECK(client.Poll(kT0 + 600) == 0);
        client.Script(500);
        CHECK(client.Poll(kT0 + 850) == 500);
    }

    SECTION("an award older than one window is not matched, even if later updates kept the window open")
    {
        Client client{true};
        client.Stage(kT0);
        CHECK(client.Remote(100, kT0 + 100) == 100);
        client.Stage(kT0 + 3'900); // another quest update: the window now ends at kT0 + 7'900

        client.Script(100);
        CHECK(client.Poll(kT0 + 4'500) == 0);
        CHECK(client.Gained() == 200);
    }

    SECTION("a disconnect forgets them")
    {
        Client client{true};
        client.Stage(kT0);
        CHECK(client.Remote(100, kT0 + 100) == 100);
        client.Tracker.Reset();
        CHECK(client.Tracker.GetRemoteAwardsRememberedInWindow() == 0);
    }

    SECTION("turning the switch off forgets them")
    {
        Client client{true};
        client.Stage(kT0);
        CHECK(client.Remote(100, kT0 + 100) == 100);
        client.Tracker.SetNetRemoteAwardsInsideQuestWindow(false);
        CHECK(client.Tracker.GetRemoteAwardsRememberedInWindow() == 0);
        client.Script(100);
        CHECK(client.Poll(kT0 + 400) == 0);
        CHECK(client.Gained() == 200);
    }
}

TEST_CASE("6.1: the switch is a feature bit of its own", "[rfab][xp][quest][m25][features]")
{
    using Session::Feature;

    CHECK((Session::kAllKnownFeatureBits & Session::Bit(Feature::kQuestXpSingleAward)) != 0);
    CHECK(Session::NameOf(Feature::kQuestXpSingleAward) == "QuestXpSingleAward");

    // Nothing disabled: on. Only this bit disabled: off, and the shared wallet itself stays on.
    CHECK(Session::IsEnabledIn(0u, Feature::kQuestXpSingleAward));
    const uint32_t mask = Session::WithFeature(0u, Feature::kQuestXpSingleAward, false);
    CHECK_FALSE(Session::IsEnabledIn(mask, Feature::kQuestXpSingleAward));
    CHECK(Session::IsEnabledIn(mask, Feature::kSharedRfabXp));
}

TEST_CASE("6.1: the origin quest of the start menu is personal and never synced", "[rfab][quest][origin]")
{
    // RFAB_Origin.Choose sets a stage of RFAB_Origin_Main for the chosen origin, and the stage's
    // script pays that origin's bonus. Synced, one player's origin would pay the other players.
    CHECK(RfabCompat::IsQuestNonSyncableByEditorIdOrBaseId("RFAB_Origin_Main", 0));
    CHECK(RfabCompat::IsQuestNonSyncableByEditorIdOrBaseId("rfab_origin_main", 0x0D000800u));
    CHECK_FALSE(RfabCompat::IsDungeonOrStoryQuestEditorId("RFAB_Origin_Main"));

    // The modpack's story quests keep syncing.
    CHECK_FALSE(RfabCompat::IsQuestNonSyncableByEditorIdOrBaseId("RFAB_Quest_Cartographer", 0));
    CHECK(RfabCompat::IsDungeonOrStoryQuestEditorId("RFAB_Quest_Cartographer"));
}
