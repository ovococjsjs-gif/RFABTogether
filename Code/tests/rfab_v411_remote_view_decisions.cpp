// Work plan 4.2: the decisions the client makes about a remote character between engine calls -
// is it present here, may it be created this frame, may its binding survive a 3D rebuild - were
// written inline in CharacterService.cpp. They live in common/CoopWorldView.h now; this suite pins
// them, including that each answers exactly what the inline code answered.

#include <catch2/catch.hpp>

#include <common/CoopWorldView.h>

using CoopWorldView::HasStaleFormBinding;
using CoopWorldView::IsRemoteViewMaterialized;
using CoopWorldView::LiveViewFacts;
using CoopWorldView::MaterializeBudget;
using CoopWorldView::MayKeepViewOnActorRemoved;

namespace
{
LiveViewFacts Facts(bool aBinding, bool aFound, bool aNetwork, bool aDragon, bool aCellLoaded)
{
    LiveViewFacts facts{};
    facts.HasFormBinding = aBinding;
    facts.ActorFound = aFound;
    facts.CreatedByNetwork = aNetwork;
    facts.IsDragon = aDragon;
    facts.ParentCellAttachedAndLoaded = aCellLoaded;
    return facts;
}
} // namespace

TEST_CASE("v4.1.1 remote view: a character is present only while its actor can be touched", "[rfab][v411][worldview]")
{
    SECTION("no binding, or a binding whose actor the engine no longer knows: absent")
    {
        CHECK_FALSE(IsRemoteViewMaterialized(Facts(false, false, false, false, false)));
        CHECK_FALSE(IsRemoteViewMaterialized(Facts(true, false, true, false, true)));
        CHECK_FALSE(IsRemoteViewMaterialized(Facts(false, true, true, false, true)));
    }

    SECTION("an actor placed by a plugin counts only while its cell is attached and loaded")
    {
        // A bandit of Skyrim.esm stays findable after its cell went to the cell buffer.
        CHECK(IsRemoteViewMaterialized(Facts(true, true, false, false, true)));
        CHECK_FALSE(IsRemoteViewMaterialized(Facts(true, true, false, false, false)));
    }

    SECTION("an actor this client created for the network does not depend on that")
    {
        CHECK(IsRemoteViewMaterialized(Facts(true, true, true, false, false)));
        CHECK(IsRemoteViewMaterialized(Facts(true, true, true, false, true)));
    }

    SECTION("neither does a dragon, which is kept across cells")
    {
        CHECK(IsRemoteViewMaterialized(Facts(true, true, false, true, false)));
    }
}

TEST_CASE("v4.1.1 remote view: a binding that leads nowhere is stale", "[rfab][v411][worldview]")
{
    CHECK(HasStaleFormBinding(Facts(true, false, false, false, false)));   // the form is gone
    CHECK(HasStaleFormBinding(Facts(true, true, false, false, false)));    // the cell was unloaded under it
    CHECK_FALSE(HasStaleFormBinding(Facts(true, true, false, false, true)));
    CHECK_FALSE(HasStaleFormBinding(Facts(true, true, true, false, false)));
    CHECK_FALSE(HasStaleFormBinding(Facts(false, false, false, false, false)));  // nothing to drop
}

TEST_CASE("v4.1.1 remote view: both answers equal the inline code they replaced, for every input", "[rfab][v411][worldview]")
{
    for (unsigned bits = 0; bits < 32; ++bits)
    {
        const bool binding = (bits & 1u) != 0;
        const bool found = (bits & 2u) != 0;
        const bool network = (bits & 4u) != 0;
        const bool dragon = (bits & 8u) != 0;
        const bool cellLoaded = (bits & 16u) != 0;

        // CharacterService::ReconcileRemoteWorldViews as it was in 4.1.0.
        const bool requiresLoadedParentCell = found && !network && !dragon;
        const bool cellActuallyLoaded = !requiresLoadedParentCell || cellLoaded;
        const bool wasMaterialized = binding && found && cellActuallyLoaded;
        const bool wasStale = binding && !wasMaterialized;

        INFO("bits " << bits);
        CHECK(IsRemoteViewMaterialized(Facts(binding, found, network, dragon, cellLoaded)) == wasMaterialized);
        CHECK(HasStaleFormBinding(Facts(binding, found, network, dragon, cellLoaded)) == wasStale);
    }
}

TEST_CASE("v4.1.1 remote view: ordinary characters are created a few per frame, important ones at once", "[rfab][v411][worldview]")
{
    SECTION("who is important")
    {
        CHECK(MaterializeBudget::IsHighPriority(true, false, false));   // another player
        CHECK(MaterializeBudget::IsHighPriority(false, true, false));   // a dragon
        CHECK(MaterializeBudget::IsHighPriority(false, false, true));   // ownership is being handed to us
        CHECK_FALSE(MaterializeBudget::IsHighPriority(false, false, false));
    }

    SECTION("a cell with forty NPCs is spread over ten frames")
    {
        int remaining = 40;
        int frames = 0;
        while (remaining > 0 && frames < 100)
        {
            MaterializeBudget budget;  // a new one every frame, as the reconcile loop does
            int createdThisFrame = 0;
            for (int i = 0; i < remaining; ++i)
            {
                if (!budget.CanCreate(false))
                    break;
                budget.NoteCreated(false);
                ++createdThisFrame;
            }
            CHECK(createdThisFrame <= static_cast<int>(MaterializeBudget::kMaxOrdinaryPerFrame));
            remaining -= createdThisFrame;
            ++frames;
        }
        CHECK(remaining == 0);
        CHECK(frames == 10);
    }

    SECTION("important characters never wait and never use up the others' share")
    {
        MaterializeBudget budget;
        for (int i = 0; i < 20; ++i)
        {
            REQUIRE(budget.CanCreate(true));
            budget.NoteCreated(true);
        }
        CHECK(budget.OrdinaryCreated() == 0);
        CHECK(budget.CanCreate(false));

        for (uint32_t i = 0; i < MaterializeBudget::kMaxOrdinaryPerFrame; ++i)
        {
            REQUIRE(budget.CanCreate(false));
            budget.NoteCreated(false);
        }
        CHECK_FALSE(budget.CanCreate(false));
        CHECK(budget.CanCreate(true));  // a player arriving in the same frame is still created
    }

    SECTION("a creation that failed is not counted")
    {
        MaterializeBudget budget;
        for (int attempt = 0; attempt < 50; ++attempt)
            CHECK(budget.CanCreate(false));  // CanCreate alone takes nothing
        CHECK(budget.OrdinaryCreated() == 0);
    }
}

TEST_CASE("v4.1.1 remote view: when the binding may survive the actor leaving the active lists", "[rfab][v411][worldview]")
{
    // A network-created actor the engine still knows, outside a save load and a purge.
    CHECK(MayKeepViewOnActorRemoved(true, true, false, false));

    CHECK_FALSE(MayKeepViewOnActorRemoved(false, true, false, false));  // a plugin-placed actor: its cell decides
    CHECK_FALSE(MayKeepViewOnActorRemoved(true, false, false, false));  // the engine deleted it
    CHECK_FALSE(MayKeepViewOnActorRemoved(true, true, true, false));    // the session's actors are being purged
    CHECK_FALSE(MayKeepViewOnActorRemoved(true, true, false, true));    // a save is being loaded

    // Exactly the condition CharacterService::OnActorRemoved had inline.
    for (unsigned bits = 0; bits < 16; ++bits)
    {
        const bool network = (bits & 1u) != 0;
        const bool found = (bits & 2u) != 0;
        const bool purge = (bits & 4u) != 0;
        const bool loading = (bits & 8u) != 0;
        INFO("bits " << bits);
        CHECK(MayKeepViewOnActorRemoved(network, found, purge, loading) == (network && found && !purge && !loading));
    }
}
