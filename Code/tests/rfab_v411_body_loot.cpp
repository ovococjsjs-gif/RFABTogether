// Work plan 5.3 / defect M24: what a body keeps when its simulation changes hands.
//
// The player who takes an NPC over sends the NPC as his game shows it, items included, and the
// server replaced its own list with that one. For a body that undid the loot ledger: a take by
// another player that reached the server between the grant and the new simulator's confirmation
// was overwritten by a list built before the take. The item was listed again, sent to every
// player in range, and could be taken a second time.
//
// The suite pins the rule that replaced "the claimer's list wins" and the ledger query it uses.

#include <catch2/catch.hpp>

#include <CoopLootLedger.h>

using CoopLootLedger::ContainerLootLedger;
using CoopLootLedger::ShouldAcceptClaimedBodyItem;

namespace
{
constexpr uint32_t kBody = 0x80001234u;
constexpr uint32_t kOtherBody = 0x80005678u;
constexpr uint32_t kSkyrim = 0u;
constexpr uint32_t kRing = 0x000877C9u;
constexpr uint32_t kGold = 0x0000000Fu;
constexpr uint32_t kHost = 1u;
constexpr uint32_t kGuest = 2u;
constexpr uint64_t kNow = 10'000u;
} // namespace

TEST_CASE("5.3: an item of the new simulator's list enters a body's list only if it was never there", "[rfab][loot][ownership]")
{
    SECTION("the server lists the item: its count stands")
    {
        // 2 gold left of 7; the claim, built before the take, says 7.
        CHECK_FALSE(ShouldAcceptClaimedBodyItem(2, false));
        // The marker is irrelevant while something is listed (a deposit after a depletion).
        CHECK_FALSE(ShouldAcceptClaimedBodyItem(1, true));
    }

    SECTION("the last one was taken: it stays taken")
    {
        CHECK_FALSE(ShouldAcceptClaimedBodyItem(0, true));
    }

    SECTION("never listed, never taken: an item the new simulator's game put on the body")
    {
        CHECK(ShouldAcceptClaimedBodyItem(0, false));
    }

    SECTION("a negative count from a damaged list is treated as nothing listed")
    {
        CHECK(ShouldAcceptClaimedBodyItem(-3, false));
        CHECK_FALSE(ShouldAcceptClaimedBodyItem(-3, true));
    }
}

TEST_CASE("5.3: the ledger answers whether the last one of an item was taken", "[rfab][loot][ownership]")
{
    ContainerLootLedger ledger;

    SECTION("nothing was taken yet")
    {
        CHECK_FALSE(ledger.IsDepleted(kBody, kSkyrim, kRing));
    }

    SECTION("taking the last one marks the slot, for that body and that item only")
    {
        const auto take = ledger.EvaluateWithdrawal(kBody, kSkyrim, kRing, kHost, 1, 1, false, kNow);
        REQUIRE(take.GrantedTake == 1);
        CHECK(ledger.IsDepleted(kBody, kSkyrim, kRing));
        CHECK_FALSE(ledger.IsDepleted(kBody, kSkyrim, kGold));
        CHECK_FALSE(ledger.IsDepleted(kOtherBody, kSkyrim, kRing));
    }

    SECTION("taking part of a stack does not")
    {
        const auto take = ledger.EvaluateWithdrawal(kBody, kSkyrim, kGold, kHost, 7, 5, false, kNow);
        REQUIRE(take.GrantedTake == 5);
        CHECK_FALSE(ledger.IsDepleted(kBody, kSkyrim, kGold));
    }

    SECTION("an item the server never listed, granted once, is marked as well")
    {
        const auto take = ledger.EvaluateWithdrawal(kBody, kSkyrim, kRing, kGuest, 0, 1, false, kNow);
        REQUIRE(take.GrantedTake == 1);
        CHECK(ledger.IsDepleted(kBody, kSkyrim, kRing));
    }

    SECTION("putting the item back clears the mark")
    {
        (void)ledger.EvaluateWithdrawal(kBody, kSkyrim, kRing, kHost, 1, 1, false, kNow);
        REQUIRE(ledger.IsDepleted(kBody, kSkyrim, kRing));
        ledger.RecordDeposit(kBody, kSkyrim, kRing);
        CHECK_FALSE(ledger.IsDepleted(kBody, kSkyrim, kRing));
    }

    SECTION("a quest item is never marked: every player of the party may take it")
    {
        const auto take = ledger.EvaluateWithdrawal(kBody, kSkyrim, kRing, kHost, 1, 1, true, kNow);
        REQUIRE(take.GrantedTake == 1);
        CHECK_FALSE(ledger.IsDepleted(kBody, kSkyrim, kRing));
    }

    SECTION("forgetting the body forgets its marks")
    {
        (void)ledger.EvaluateWithdrawal(kBody, kSkyrim, kRing, kHost, 1, 1, false, kNow);
        (void)ledger.EvaluateWithdrawal(kOtherBody, kSkyrim, kRing, kHost, 1, 1, false, kNow);
        ledger.ForgetContainer(kBody);
        CHECK_FALSE(ledger.IsDepleted(kBody, kSkyrim, kRing));
        CHECK(ledger.IsDepleted(kOtherBody, kSkyrim, kRing));
    }

    SECTION("no body and no item are never 'depleted'")
    {
        CHECK_FALSE(ledger.IsDepleted(0, kSkyrim, kRing));
        CHECK_FALSE(ledger.IsDepleted(kBody, 0, 0));
    }
}

TEST_CASE("5.3: the race the rule closes, step by step", "[rfab][loot][ownership]")
{
    // The body holds a ring and 7 gold. It is handed to the guest; before the guest's confirmation
    // arrives the host takes the ring and 5 gold. The confirmation lists a ring, 7 gold and a
    // dagger only the guest's game has.
    ContainerLootLedger ledger;
    int32_t ringsHeld = 1;
    int32_t goldHeld = 7;
    constexpr uint32_t kDagger = 0x0001397Eu;

    const auto ringTake = ledger.EvaluateWithdrawal(kBody, kSkyrim, kRing, kHost, ringsHeld, 1, false, kNow);
    ringsHeld -= ringTake.GrantedTake;
    const auto goldTake = ledger.EvaluateWithdrawal(kBody, kSkyrim, kGold, kHost, goldHeld, 5, false, kNow);
    goldHeld -= goldTake.GrantedTake;
    REQUIRE(ringsHeld == 0);
    REQUIRE(goldHeld == 2);

    CHECK_FALSE(ShouldAcceptClaimedBodyItem(ringsHeld, ledger.IsDepleted(kBody, kSkyrim, kRing))); // the ring stays taken
    CHECK_FALSE(ShouldAcceptClaimedBodyItem(goldHeld, ledger.IsDepleted(kBody, kSkyrim, kGold)));  // 2 gold, not 7
    CHECK(ShouldAcceptClaimedBodyItem(0, ledger.IsDepleted(kBody, kSkyrim, kDagger)));             // the dagger is new

    // The guest, whose copy still shows the ring, clicks it: refused, as before this change.
    const auto late = ledger.EvaluateWithdrawal(kBody, kSkyrim, kRing, kGuest, ringsHeld, 1, false, kNow + 50);
    CHECK(late.GrantedTake == 0);
    CHECK(late.OverdrawnRollback == 1);
}
