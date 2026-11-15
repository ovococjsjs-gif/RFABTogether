// Work plan 6.4, defect M28: a change made while a registration is unanswered.
//
// A client registers a container with the server by sending its list: when the player enters the
// cell (the cell scan), when he activates a container the client holds no id for, and when he
// takes from or puts into such a container (the change is queued and the list is sent as it is
// after the change). The answer "you are first" means the server built its copy from that list,
// and 4.1.0 then dropped every queued change: "the list already has it".
//
// That is true only of the change that caused the registration. A change made between a request
// and its answer registered the container a second time and was queued; the first answer dropped
// it, although the list the server's copy was built from had been read before the change, and
// the second answer ("not first") wrote that list over the chest. The item was in the player's
// pack and back in the chest for everybody.
//
// The client side runs only in the game. This suite pins the parts it is built from: the mark a
// queued change carries, the rule that decides at the answer, the record of unanswered
// registrations, and the switch. The server's side of both sequences is harness scene 22.

#include <catch2/catch.hpp>

#include <CoopLootLedger.h>
#include <Session/Features.h>

#include <cstdint>

namespace
{
struct Item
{
    uint32_t Id{0};
    int32_t Count{0};
};

using Queue = CoopLootLedger::PendingContainerLootQueue<Item>;

constexpr uint32_t kChest = 0x000A1001u;
constexpr uint32_t kOtherChest = 0x000A1002u;
constexpr uint32_t kSword = 0x000139B9u;
constexpr uint32_t kAxe = 0x000139B3u;
constexpr uint32_t kGold = 0x0000000Fu;
} // namespace

TEST_CASE("M28: which queued changes are sent when the server answers a registration", "[rfab][loot][m28]")
{
    SECTION("not first: another player's list stands, every queued change is arbitrated (as in 4.1.0)")
    {
        CHECK(CoopLootLedger::ShouldReplayPendingDelta(false, true));
        CHECK(CoopLootLedger::ShouldReplayPendingDelta(false, false));
    }

    SECTION("first, and the list the server was sent already has the change: dropped (as in 4.1.0)")
    {
        CHECK_FALSE(CoopLootLedger::ShouldReplayPendingDelta(true, true));
    }

    SECTION("first, but the list was read before the change: sent, or the server's copy keeps the item")
    {
        CHECK(CoopLootLedger::ShouldReplayPendingDelta(true, false));
    }
}

TEST_CASE("M28: a change is in the registered list only if it was counted before the list was read", "[rfab][loot][m28]")
{
    // The registration read the list when the counter stood at 10.
    CHECK(CoopLootLedger::IsChangeReflectedInRegistration(9, 10));
    // The tenth change was applied before the counter was read.
    CHECK(CoopLootLedger::IsChangeReflectedInRegistration(10, 10));
    // The eleventh came after.
    CHECK_FALSE(CoopLootLedger::IsChangeReflectedInRegistration(11, 10));
    CHECK_FALSE(CoopLootLedger::IsChangeReflectedInRegistration(1, 0));
}

TEST_CASE("M28: how long a registration counts as unanswered", "[rfab][loot][m28]")
{
    CHECK(CoopLootLedger::kRegistrationWindowMs == 3000ULL);
    // Shorter than a queued change lives: a change queued under a registration is still there at the answer.
    CHECK(CoopLootLedger::kRegistrationWindowMs < Queue::kDefaultTtlMs);

    CHECK(CoopLootLedger::IsRegistrationInFlight(1000, 1000));
    CHECK(CoopLootLedger::IsRegistrationInFlight(1000, 1150));
    CHECK(CoopLootLedger::IsRegistrationInFlight(1000, 4000));
    // After the window a change registers the container itself, as in 4.1.0.
    CHECK_FALSE(CoopLootLedger::IsRegistrationInFlight(1000, 4001));
    // A clock that reads earlier than the send time proves nothing.
    CHECK_FALSE(CoopLootLedger::IsRegistrationInFlight(1000, 999));
    CHECK(CoopLootLedger::IsRegistrationInFlight(1000, 1500, 500));
    CHECK_FALSE(CoopLootLedger::IsRegistrationInFlight(1000, 1501, 500));
}

TEST_CASE("M28: the record of unanswered registrations", "[rfab][loot][m28]")
{
    CoopLootLedger::RegistrationTracker tracker;

    SECTION("a container that was sent is in flight until the answer, with the counter its list was read at")
    {
        CHECK_FALSE(tracker.Find(kChest, 1000).InFlight);

        tracker.Note(kChest, 1000, 10);
        const auto found = tracker.Find(kChest, 1100);
        CHECK(found.InFlight);
        CHECK(found.Sequence == 10);
        CHECK_FALSE(tracker.Find(kOtherChest, 1100).InFlight);

        tracker.Erase(kChest);
        CHECK_FALSE(tracker.Find(kChest, 1100).InFlight);
        CHECK(tracker.Size() == 0);
    }

    SECTION("an answer that never comes stops counting after the window")
    {
        tracker.Note(kChest, 1000, 10);
        CHECK(tracker.Find(kChest, 4000).InFlight);
        CHECK_FALSE(tracker.Find(kChest, 4001).InFlight);
        // The record itself stays until it is pruned or the container is sent again.
        CHECK(tracker.Size() == 1);
        tracker.Prune(4001);
        CHECK(tracker.Size() == 0);
    }

    SECTION("sent again while the first request is unanswered: the first list is the one the server may build from")
    {
        tracker.Note(kChest, 1000, 10);
        tracker.Note(kChest, 1200, 42);
        const auto found = tracker.Find(kChest, 1300);
        CHECK(found.InFlight);
        CHECK(found.Sequence == 10);
        // The window runs from the first request, too.
        CHECK(tracker.Find(kChest, 4000).InFlight);
        CHECK_FALSE(tracker.Find(kChest, 4001).InFlight);
        CHECK(tracker.Size() == 1);
    }

    SECTION("sent again after the first request is overdue: the new request counts")
    {
        tracker.Note(kChest, 1000, 10);
        tracker.Note(kChest, 4500, 42);
        const auto found = tracker.Find(kChest, 6000);
        CHECK(found.InFlight);
        CHECK(found.Sequence == 42);
        CHECK(tracker.Size() == 1);
    }

    SECTION("form id 0 is nothing")
    {
        tracker.Note(0, 1000, 10);
        CHECK(tracker.Size() == 0);
        CHECK_FALSE(tracker.Find(0, 1000).InFlight);
    }

    SECTION("a new world starts empty")
    {
        tracker.Note(kChest, 1000, 10);
        tracker.Note(kOtherChest, 1000, 10);
        tracker.Clear();
        CHECK(tracker.Size() == 0);
        CHECK_FALSE(tracker.Find(kChest, 1000).InFlight);
    }

    SECTION("the record is bounded: stale entries make room, fresh ones are never pushed out")
    {
        const size_t limit = CoopLootLedger::RegistrationTracker::kMaxTracked;
        for (uint32_t i = 0; i < limit; ++i)
            tracker.Note(0x1000u + i, 1000, 1);
        CHECK(tracker.Size() == limit);

        // All still fresh: one more is not recorded (it falls back to 4.1.0's path).
        tracker.Note(0x900000u, 2000, 2);
        CHECK(tracker.Size() == limit);
        CHECK_FALSE(tracker.Find(0x900000u, 2000).InFlight);

        // Later the old ones are stale: the newcomer prunes them and is recorded.
        tracker.Note(0x900001u, 5500, 3);
        CHECK(tracker.Find(0x900001u, 5500).InFlight);
        CHECK(tracker.Size() == 1);
    }
}

TEST_CASE("M28: a queued change carries whether the registration's list has it", "[rfab][loot][m28]")
{
    Queue queue;

    // 4.1.0's call: the registration the take itself sends reads the list after the take.
    REQUIRE(queue.Enqueue(kChest, Item{kGold, -50}, false, true, 1000));
    // A take counted after the list was read.
    REQUIRE(queue.Enqueue(kChest, Item{kSword, -1}, false, true, 1010, Queue::kDefaultTtlMs, false));
    // Another container's change is not touched by this container's answer.
    REQUIRE(queue.Enqueue(kOtherChest, Item{kAxe, -1}, false, true, 1020, Queue::kDefaultTtlMs, false));

    const auto pending = queue.TakeForContainer(kChest, 1100);
    REQUIRE(pending.size() == 2);
    CHECK(pending[0].Item.Id == kGold);
    CHECK(pending[0].ReflectedInRegistration);
    CHECK(pending[1].Item.Id == kSword);
    CHECK_FALSE(pending[1].ReflectedInRegistration);
    CHECK(queue.Size() == 1);
}

TEST_CASE("M28: a change between a registration and its answer, step by step", "[rfab][loot][m28]")
{
    CoopLootLedger::RegistrationTracker sent;
    Queue queue;

    // ObjectService: the cell scan, or the activation of a container the client holds no id for,
    // reads the counter, then the list, and sends the list.
    const auto registerList = [&](uint32_t aChest, uint64_t aCounterNow, uint64_t aNowMs) { sent.Note(aChest, aNowMs, aCounterNow); };

    // ObjectService::RequestContainerSyncForLoot: a change to a container the client holds no id
    // for, dispatched one frame after the hook counted it. aCounterNow is the counter at dispatch.
    // Returns true when it sends a registration of its own.
    const auto onChange = [&](uint32_t aChest, const Item& acItem, uint64_t aSequence, uint64_t aCounterNow, uint64_t aNowMs,
                              bool aSwitchOn) {
        if (aSwitchOn)
        {
            const auto found = sent.Find(aChest, aNowMs);
            if (found.InFlight)
            {
                const bool reflected = CoopLootLedger::IsChangeReflectedInRegistration(aSequence, found.Sequence);
                queue.Enqueue(aChest, acItem, false, true, aNowMs, Queue::kDefaultTtlMs, reflected);
                return false;
            }
            sent.Note(aChest, aNowMs, aCounterNow);
        }
        queue.Enqueue(aChest, acItem, false, true, aNowMs);
        return true;
    };

    // ObjectService::OnAssignObjectsResponse for an answer "you are first": what is sent on.
    const auto sentOnFirstAnswer = [&](uint32_t aChest, uint64_t aNowMs) {
        sent.Erase(aChest);
        int32_t total = 0;
        for (const auto& pending : queue.TakeForContainer(aChest, aNowMs))
            if (CoopLootLedger::ShouldReplayPendingDelta(true, pending.ReflectedInRegistration))
                total += pending.Item.Count;
        return total;
    };

    SECTION("a take after the cell scan read the list: no second registration, the take is sent at the answer")
    {
        registerList(kChest, 10, 1000); // the scan reads the chest: a sword and 50 gold
        CHECK_FALSE(onChange(kChest, Item{kSword, -1}, 11, 11, 1040, true));
        CHECK(sentOnFirstAnswer(kChest, 1120) == -1);
        CHECK(queue.Empty());
    }

    SECTION("a take before the scan read the list: the list has it, nothing is sent twice")
    {
        // The hook counted the take as change 10; the scan then read the counter (10) and the list.
        registerList(kChest, 10, 1000);
        CHECK_FALSE(onChange(kChest, Item{kSword, -1}, 10, 10, 1001, true));
        CHECK(sentOnFirstAnswer(kChest, 1120) == 0);
    }

    SECTION("one take on each side of the scan: only the later one is sent")
    {
        registerList(kChest, 10, 1000);
        CHECK_FALSE(onChange(kChest, Item{kGold, -20}, 10, 10, 1001, true));
        CHECK_FALSE(onChange(kChest, Item{kGold, -30}, 11, 11, 1050, true));
        CHECK(sentOnFirstAnswer(kChest, 1120) == -30);
    }

    SECTION("an opened container: the activation sent its list, a take before the answer is sent at the answer")
    {
        registerList(kChest, 20, 2000); // the player opens a chest the client had not registered
        CHECK_FALSE(onChange(kChest, Item{kAxe, -1}, 21, 21, 2400, true));
        CHECK(sentOnFirstAnswer(kChest, 2600) == -1);
    }

    SECTION("two takes in a row from a container nobody registered: the second is sent at the answer")
    {
        // The first take registers the container; its list is read after it.
        CHECK(onChange(kChest, Item{kSword, -1}, 11, 11, 1000, true));
        // The second comes two frames later, before the answer.
        CHECK_FALSE(onChange(kChest, Item{kAxe, -1}, 12, 12, 1040, true));
        CHECK(sentOnFirstAnswer(kChest, 1120) == -1);
    }

    SECTION("a take-all in one frame: every take is in the list the first of them sends")
    {
        // Three takes are counted (11, 12, 13) and applied before any of them is dispatched.
        CHECK(onChange(kChest, Item{kSword, -1}, 11, 13, 1000, true));
        CHECK_FALSE(onChange(kChest, Item{kAxe, -1}, 12, 13, 1000, true));
        CHECK_FALSE(onChange(kChest, Item{kGold, -50}, 13, 13, 1000, true));
        CHECK(sentOnFirstAnswer(kChest, 1120) == 0);
    }

    SECTION("a single take from a container nobody registered: registered by the take, dropped at the answer, as in 4.1.0")
    {
        CHECK(onChange(kChest, Item{kSword, -1}, 11, 11, 1040, true));
        CHECK(sentOnFirstAnswer(kChest, 1120) == 0);

        // Once a registration was answered, the next change to an unknown container sends its own again ...
        CHECK(onChange(kChest, Item{kSword, -1}, 21, 21, 2100, true));
        CHECK(sentOnFirstAnswer(kChest, 2200) == 0);
        // ... and so does a change that finds a registration overdue.
        registerList(kOtherChest, 20, 2000);
        CHECK(onChange(kOtherChest, Item{kSword, -1}, 21, 21, 5001, true));
    }

    SECTION("switch off: 4.1.0 - a second registration, and the take is dropped by the first answer")
    {
        registerList(kChest, 10, 1000);
        CHECK(onChange(kChest, Item{kSword, -1}, 11, 11, 1040, false));
        CHECK(sentOnFirstAnswer(kChest, 1120) == 0);
    }
}

TEST_CASE("M28: the switch is a feature bit of its own", "[rfab][loot][m28]")
{
    using Session::Feature;

    CHECK((Session::kAllKnownFeatureBits & Session::Bit(Feature::kEarlyTakeReplay)) != 0);
    CHECK(Session::NameOf(Feature::kEarlyTakeReplay) == "EarlyTakeReplay");

    CHECK(Session::IsEnabledIn(0u, Feature::kEarlyTakeReplay));
    const uint32_t mask = Session::WithFeature(0u, Feature::kEarlyTakeReplay, false);
    CHECK_FALSE(Session::IsEnabledIn(mask, Feature::kEarlyTakeReplay));
    // It does not switch off the registration ahead, nor the other way round.
    CHECK(Session::IsEnabledIn(mask, Feature::kNeighbourCellContainers));
    const uint32_t other = Session::WithFeature(0u, Feature::kNeighbourCellContainers, false);
    CHECK(Session::IsEnabledIn(other, Feature::kEarlyTakeReplay));
}
