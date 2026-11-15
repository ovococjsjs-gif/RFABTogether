// Work plan 6.4 / defect M27: which containers are one player's own.
//
// A container registered on the server is shared, with the loot rules: the first player to
// register it decides its contents, and a take the server's copy cannot cover is taken back from
// the taker. The client registered every container a player touched, a merchant's chest and a
// storage chest opened by a script from another place included. Both are filled by each player's
// own game. Server harness scene 20 shows what the loot rules then do to a trade.
//
// The suite pins the rule the client asks before it registers a container, and its switch.

#include <catch2/catch.hpp>

#include <RfabModpackCompat.h>
#include <Session/Features.h>

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
