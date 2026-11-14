// Work plan 5.4 / defect M23: an unlock the server heard about before it knew the object.
//
// A client registers the doors and containers of the cell its player stands in. A lock picked
// across an exterior cell border reaches the server for an object it has no record of; it was
// relayed to the players in range and forgotten. The memory below is what the server keeps now,
// and this suite pins what it keeps, what it refuses to keep, and when it lets go.

#include <catch2/catch.hpp>

#include <World/UnlockMemory.h>

using WorldObjects::ResolveRegistrationLock;
using WorldObjects::UnlockMemory;

namespace
{
constexpr uint64_t kHouseDoor = (uint64_t{0} << 32) | 0x000165A8u;
constexpr uint64_t kCampChest = (uint64_t{2} << 32) | 0x0009ABCDu;
constexpr uint8_t kAdept = 50;
constexpr uint8_t kMaster = 100;
} // namespace

TEST_CASE("5.4: an unlock of an object nobody registered is kept for the registration", "[rfab][world][locks]")
{
    UnlockMemory<> memory;

    SECTION("nothing is remembered about an object nobody touched")
    {
        CHECK(memory.Size() == 0);
        CHECK_FALSE(memory.Remembers(kHouseDoor));
        CHECK_FALSE(memory.Take(kHouseDoor).has_value());
    }

    SECTION("an unlock is remembered with its lock level, per object")
    {
        CHECK(memory.Note(kHouseDoor, false, kAdept));
        CHECK(memory.Note(kCampChest, false, kMaster));
        CHECK(memory.Size() == 2);

        const auto door = memory.Take(kHouseDoor);
        REQUIRE(door.has_value());
        CHECK(*door == kAdept);

        // Taking one leaves the other.
        CHECK(memory.Size() == 1);
        CHECK(memory.Remembers(kCampChest));
    }

    SECTION("the registration takes the unlock once: the registered object carries it from there")
    {
        REQUIRE(memory.Note(kHouseDoor, false, kAdept));
        REQUIRE(memory.Take(kHouseDoor).has_value());
        CHECK_FALSE(memory.Take(kHouseDoor).has_value());
        CHECK(memory.Size() == 0);
    }

    SECTION("the same unlock reported twice is one record")
    {
        CHECK(memory.Note(kHouseDoor, false, kAdept));
        CHECK_FALSE(memory.Note(kHouseDoor, false, kAdept)); // nothing changed
        CHECK(memory.Size() == 1);
    }

    SECTION("an object without an id is never remembered")
    {
        CHECK_FALSE(memory.Note(0, false, kAdept));
        CHECK(memory.Size() == 0);
    }
}

TEST_CASE("5.4: a lock report is never remembered, it only cancels an unlock", "[rfab][world][locks]")
{
    UnlockMemory<> memory;

    SECTION("'locked' for an unknown object leaves the memory empty")
    {
        // An arena barrier closing behind the party. Remembering it could shut out a player who
        // arrives after a script (which reports nothing) opened the barrier again.
        CHECK_FALSE(memory.Note(kHouseDoor, true, kAdept));
        CHECK(memory.Size() == 0);
        CHECK_FALSE(memory.Take(kHouseDoor).has_value());
    }

    SECTION("unlocked, then locked again: the registrant's own game decides, as it did before")
    {
        REQUIRE(memory.Note(kHouseDoor, false, kAdept));
        CHECK(memory.Note(kHouseDoor, true, kAdept));
        CHECK(memory.Size() == 0);
        CHECK_FALSE(memory.Take(kHouseDoor).has_value());
    }

    SECTION("locked, then unlocked: the unlock stands")
    {
        (void)memory.Note(kHouseDoor, true, kAdept);
        CHECK(memory.Note(kHouseDoor, false, kAdept));
        CHECK(memory.Remembers(kHouseDoor));
    }
}

TEST_CASE("5.4: the memory is bounded and a rollback empties it", "[rfab][world][locks]")
{
    SECTION("more unlocks than it holds: old ones give way, new ones are kept, the size stays put")
    {
        UnlockMemory<4> memory;
        for (uint64_t key = 1; key <= 4; ++key)
            REQUIRE(memory.Note(key, false, kAdept));
        CHECK(memory.Size() == 4);

        CHECK(memory.Note(5, false, kAdept));
        CHECK(memory.Size() == 4);
        CHECK(memory.Remembers(5));
        CHECK_FALSE(memory.Remembers(1)); // the oldest record gave way
        CHECK(memory.Remembers(2));
        CHECK(memory.Remembers(3));
        CHECK(memory.Remembers(4));
    }

    SECTION("a slot freed by a registration is reused before anything is evicted")
    {
        UnlockMemory<4> memory;
        for (uint64_t key = 1; key <= 4; ++key)
            REQUIRE(memory.Note(key, false, kAdept));
        REQUIRE(memory.Take(3).has_value());

        CHECK(memory.Note(5, false, kAdept));
        CHECK(memory.Size() == 4);
        for (const uint64_t key : {uint64_t{1}, uint64_t{2}, uint64_t{4}, uint64_t{5}})
            CHECK(memory.Remembers(key));
    }

    SECTION("a rollback forgets every unlock: in the restored world they never happened")
    {
        UnlockMemory<> memory;
        REQUIRE(memory.Note(kHouseDoor, false, kAdept));
        REQUIRE(memory.Note(kCampChest, false, kMaster));
        memory.Clear();
        CHECK(memory.Size() == 0);
        CHECK_FALSE(memory.Take(kHouseDoor).has_value());
        CHECK_FALSE(memory.Take(kCampChest).has_value());

        // And it works again afterwards.
        CHECK(memory.Note(kHouseDoor, false, kAdept));
        CHECK(memory.Size() == 1);
    }
}

TEST_CASE("5.4: what the first registration of an object does with a remembered unlock", "[rfab][world][locks]")
{
    SECTION("no unlock remembered: the registrant's view becomes the server's, nobody is told anything")
    {
        const auto locked = ResolveRegistrationLock(true, kAdept, std::nullopt);
        CHECK(locked.IsLocked);
        CHECK(locked.LockLevel == kAdept);
        CHECK_FALSE(locked.TellRegistrant);

        const auto open = ResolveRegistrationLock(false, 0, std::nullopt);
        CHECK_FALSE(open.IsLocked);
        CHECK_FALSE(open.TellRegistrant);
    }

    SECTION("a party member opened it earlier and the registrant still sees it locked: unlocked, and told")
    {
        const auto result = ResolveRegistrationLock(true, kAdept, kAdept);
        CHECK_FALSE(result.IsLocked);
        CHECK(result.LockLevel == kAdept);
        CHECK(result.TellRegistrant);
    }

    SECTION("the registrant already sees it unlocked: nothing to tell")
    {
        const auto result = ResolveRegistrationLock(false, 0, kAdept);
        CHECK_FALSE(result.IsLocked);
        CHECK_FALSE(result.TellRegistrant);
    }
}
