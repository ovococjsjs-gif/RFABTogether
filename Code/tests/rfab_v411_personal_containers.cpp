// Work plan 6.4 / defect M27: which containers are one player's own.
//
// A container registered on the server is shared, with the loot rules: the first player to
// register it decides its contents, and a take the server's copy cannot cover is taken back from
// the taker. The client registered every container a player touched, a merchant's chest and a
// storage chest opened by a script from another place included. Both are filled by each player's
// own game. Server harness scene 20 shows what the loot rules then do to a trade.
//
// The suite pins the rule the client asks before it registers a container, and its switch.
//
// The same work package closes the rest of defect M11: outdoors the containers of the cells around
// the player are registered before he can loot them across a cell border. The engine calls of that
// scan run only in the game; the suite pins which cells and which containers it asks for, and its
// switch.

#include <catch2/catch.hpp>

#include <RfabModpackCompat.h>
#include <Session/Features.h>

#include <limits>
#include <set>
#include <utility>

using RfabCompat::ContainerSyncFacts;
using RfabCompat::IsPersonalContainer;

namespace
{
constexpr ContainerSyncFacts Facts(bool aMerchant, bool aWherePlayerIs) noexcept
{
    ContainerSyncFacts facts{};
    facts.IsMerchantContainer = aMerchant;
    facts.IsWherePlayerIs = aWherePlayerIs;
    return facts;
}
} // namespace

TEST_CASE("6.4: a merchant's chest and a remotely opened container are the player's own", "[rfab][loot][merchant][m27]")
{
    SECTION("a chest of loot next to the player is shared")
    {
        CHECK_FALSE(IsPersonalContainer(Facts(false, true), true));
        // What a default-constructed description means: an ordinary container next to the player.
        CHECK_FALSE(IsPersonalContainer(ContainerSyncFacts{}, true));
    }

    SECTION("a merchant's chest is personal, wherever it stands")
    {
        CHECK(IsPersonalContainer(Facts(true, true), true));   // under the shop floor
        CHECK(IsPersonalContainer(Facts(true, false), true));  // in a holding cell
    }

    SECTION("a container that is not where the player is, is personal: only a script can have opened it")
    {
        CHECK(IsPersonalContainer(Facts(false, false), true));
    }

    SECTION("with the switch off every container is shared, as in 4.1.0")
    {
        CHECK_FALSE(IsPersonalContainer(Facts(false, true), false));
        CHECK_FALSE(IsPersonalContainer(Facts(true, true), false));
        CHECK_FALSE(IsPersonalContainer(Facts(true, false), false));
        CHECK_FALSE(IsPersonalContainer(Facts(false, false), false));
    }
}

TEST_CASE("6.4: the switch is a feature bit of its own", "[rfab][loot][merchant][m27][features]")
{
    using Session::Feature;

    CHECK((Session::kAllKnownFeatureBits & Session::Bit(Feature::kPersonalMerchantStock)) != 0);
    CHECK(Session::NameOf(Feature::kPersonalMerchantStock) == "PersonalMerchantStock");

    CHECK(Session::IsEnabledIn(0u, Feature::kPersonalMerchantStock));
    const uint32_t mask = Session::WithFeature(0u, Feature::kPersonalMerchantStock, false);
    CHECK_FALSE(Session::IsEnabledIn(mask, Feature::kPersonalMerchantStock));
    // Switching it off does not switch off server-owned dropped items or anything else.
    CHECK(Session::IsEnabledIn(mask, Feature::kServerOwnedWorldItems));
    CHECK(Session::IsEnabledIn(mask, Feature::kQuestXpSingleAward));
}

TEST_CASE("6.4: outdoors the containers within reach of the player's cell are registered ahead", "[rfab][loot][m11][neighbours]")
{
    SECTION("the eight cells around the player's cell, each once, never his own")
    {
        std::set<std::pair<int32_t, int32_t>> seen;
        for (const RfabCompat::CellOffset& offset : RfabCompat::kNeighbourCellOffsets)
        {
            CHECK((offset.X != 0 || offset.Y != 0));
            CHECK((offset.X >= -1 && offset.X <= 1));
            CHECK((offset.Y >= -1 && offset.Y <= 1));
            seen.insert({offset.X, offset.Y});
        }
        CHECK(seen.size() == 8);
    }

    SECTION("only outdoors, and only with the switch on")
    {
        CHECK(RfabCompat::ShouldScanNeighbourCells(true, true));
        // Indoors a cell is entered through a door, and the scan of the cell runs before any take.
        CHECK_FALSE(RfabCompat::ShouldScanNeighbourCells(true, false));
        // Off = 4.1.0: the first take registers the chest.
        CHECK_FALSE(RfabCompat::ShouldScanNeighbourCells(false, true));
        CHECK_FALSE(RfabCompat::ShouldScanNeighbourCells(false, false));
    }

    SECTION("within reach: a quarter of a cell around the player's cell")
    {
        // The cell (0, 0) covers 0..4096 on both axes.
        CHECK(RfabCompat::kExteriorCellSizeUnits == 4096.0f);
        CHECK(RfabCompat::kNeighbourContainerReachUnits == 1024.0f);

        // A chest two metres across the western border: the case the scan exists for.
        CHECK(RfabCompat::IsWithinReachOfCell(-140.0f, 2000.0f, 0, 0));
        // The limit itself, on each side.
        CHECK(RfabCompat::IsWithinReachOfCell(-1024.0f, 2000.0f, 0, 0));
        CHECK(RfabCompat::IsWithinReachOfCell(4096.0f + 1024.0f, 2000.0f, 0, 0));
        CHECK(RfabCompat::IsWithinReachOfCell(2000.0f, -1024.0f, 0, 0));
        CHECK(RfabCompat::IsWithinReachOfCell(2000.0f, 4096.0f + 1024.0f, 0, 0));
        // One unit further is the neighbour's own business.
        CHECK_FALSE(RfabCompat::IsWithinReachOfCell(-1025.0f, 2000.0f, 0, 0));
        CHECK_FALSE(RfabCompat::IsWithinReachOfCell(4096.0f + 1025.0f, 2000.0f, 0, 0));
        CHECK_FALSE(RfabCompat::IsWithinReachOfCell(2000.0f, -1025.0f, 0, 0));
        CHECK_FALSE(RfabCompat::IsWithinReachOfCell(2000.0f, 4096.0f + 1025.0f, 0, 0));
        // The middle of a neighbouring cell is out of reach.
        CHECK_FALSE(RfabCompat::IsWithinReachOfCell(-2048.0f, 2048.0f, 0, 0));

        // Across a corner the distance is measured to the corner, not along each axis.
        CHECK(RfabCompat::IsWithinReachOfCell(4096.0f + 700.0f, 4096.0f + 700.0f, 0, 0));        // 990 units
        CHECK_FALSE(RfabCompat::IsWithinReachOfCell(4096.0f + 800.0f, 4096.0f + 800.0f, 0, 0)); // 1131 units
        CHECK_FALSE(RfabCompat::IsWithinReachOfCell(-800.0f, -800.0f, 0, 0));

        // The player's own cell is within reach of itself (its containers go with the cell's scan).
        CHECK(RfabCompat::IsWithinReachOfCell(2000.0f, 2000.0f, 0, 0));

        // Cells with negative coordinates: (-3, 5) covers x -12288..-8192, y 20480..24576.
        CHECK(RfabCompat::IsWithinReachOfCell(-8192.0f + 1000.0f, 22000.0f, -3, 5));
        CHECK(RfabCompat::IsWithinReachOfCell(-12288.0f - 1000.0f, 22000.0f, -3, 5));
        CHECK_FALSE(RfabCompat::IsWithinReachOfCell(-12288.0f - 1100.0f, 22000.0f, -3, 5));
        CHECK_FALSE(RfabCompat::IsWithinReachOfCell(-10000.0f, 20480.0f - 1100.0f, -3, 5));

        // A reach of its own.
        CHECK(RfabCompat::IsWithinReachOfCell(-10.0f, 2000.0f, 0, 0, 10.0f));
        CHECK_FALSE(RfabCompat::IsWithinReachOfCell(-11.0f, 2000.0f, 0, 0, 10.0f));

        // A reference without a position is never sent ahead.
        const float notANumber = std::numeric_limits<float>::quiet_NaN();
        CHECK_FALSE(RfabCompat::IsWithinReachOfCell(notANumber, 2000.0f, 0, 0));
        CHECK_FALSE(RfabCompat::IsWithinReachOfCell(2000.0f, notANumber, 0, 0));
    }

    SECTION("a container this client already holds, a personal one, or one out of reach is not sent")
    {
        CHECK(RfabCompat::ShouldRegisterNeighbourContainer(false, false, true));
        CHECK_FALSE(RfabCompat::ShouldRegisterNeighbourContainer(true, false, true));
        CHECK_FALSE(RfabCompat::ShouldRegisterNeighbourContainer(false, true, true));
        CHECK_FALSE(RfabCompat::ShouldRegisterNeighbourContainer(false, false, false));
        CHECK_FALSE(RfabCompat::ShouldRegisterNeighbourContainer(true, true, true));
        CHECK_FALSE(RfabCompat::ShouldRegisterNeighbourContainer(true, true, false));
    }

    SECTION("the switch is a feature bit of its own")
    {
        using Session::Feature;

        CHECK((Session::kAllKnownFeatureBits & Session::Bit(Feature::kNeighbourCellContainers)) != 0);
        CHECK(Session::NameOf(Feature::kNeighbourCellContainers) == "NeighbourCellContainers");

        CHECK(Session::IsEnabledIn(0u, Feature::kNeighbourCellContainers));
        const uint32_t mask = Session::WithFeature(0u, Feature::kNeighbourCellContainers, false);
        CHECK_FALSE(Session::IsEnabledIn(mask, Feature::kNeighbourCellContainers));
        // Switching it off leaves merchants' chests personal.
        CHECK(Session::IsEnabledIn(mask, Feature::kPersonalMerchantStock));
    }
}
