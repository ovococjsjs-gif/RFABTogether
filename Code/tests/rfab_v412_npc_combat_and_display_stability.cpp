// v4.1.2 regressions:
//   1. Dormant persistent NPCs hydrated from rfab_checkpoints.bin carry an empty unseeded
//      Inventory{} alongside persisted CurrentHealth. Wholesale SetActorInventory must never
//      wipe a living non-player NPC's weapons, arrows, shields, spells, or shouts to empty,
//      while still clearing dead looted corpses and allowing pure-mage NPCs with 0 physical
//      items + equipped spells.
//   2. Delayed m_weaponDrawUpdates retries (t+0.5s / t+2.0s) must apply strictly to remote
//      living NPCs and never force SetWeaponDrawnEx(false) onto locally simulated combat AI.
//   3. Co-op config overlay transforms SSEDisplayTweaks.ini and SSEDisplayTweaks_Custom.ini
//      to borderless flip presentation (Fullscreen=false, Borderless=true) without touching
//      solo RFAB files.

#include <catch2/catch.hpp>

// Before any Structs/ header: those rely on TiltedPhoques being declared, and test suites on both
// Linux and Windows compile without the encoding precompiled header.
#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <Structs/Inventory.h>
#include <common/Compat/ConfigOverlay.h>
#include <common/CoopInventoryDiff.h>

TEST_CASE("v4.1.2: unseeded empty inventory never disarms a living non-player NPC", "[rfab][v412][combat][inventory]")
{
    Inventory emptyInv{};
    REQUIRE(CoopInventoryDiff::IsActorInventoryEmpty(emptyInv));

    SECTION("living world NPC rejects empty wholesale inventory (checkpoint hydration payload)")
    {
        CHECK_FALSE(CoopInventoryDiff::ShouldApplyWholesaleActorInventory(
            emptyInv, /*aIsPlayer=*/false, /*aIsDead=*/false));
    }

    SECTION("living bandit with weapon and arrows is accepted")
    {
        Inventory armedBandit{};
        Inventory::Entry ironSword{};
        ironSword.BaseId = GameId{0u, 0x00012EB7u};
        ironSword.Count = 1;
        ironSword.ExtraWorn = true;
        armedBandit.Entries.push_back(ironSword);

        Inventory::Entry ironArrows{};
        ironArrows.BaseId = GameId{0u, 0x0001397Du};
        ironArrows.Count = 24;
        armedBandit.Entries.push_back(ironArrows);

        CHECK_FALSE(CoopInventoryDiff::IsActorInventoryEmpty(armedBandit));
        CHECK(CoopInventoryDiff::ShouldApplyWholesaleActorInventory(
            armedBandit, /*aIsPlayer=*/false, /*aIsDead=*/false));
    }

    SECTION("living pure-mage or draugr with zero physical items but equipped spell/shout is accepted")
    {
        Inventory spellcaster{};
        spellcaster.CurrentMagicEquipment.RightHandSpell = GameId{0u, 0x00012FD0u};
        CHECK_FALSE(CoopInventoryDiff::IsActorInventoryEmpty(spellcaster));
        CHECK(CoopInventoryDiff::ShouldApplyWholesaleActorInventory(
            spellcaster, /*aIsPlayer=*/false, /*aIsDead=*/false));

        Inventory shouter{};
        shouter.CurrentMagicEquipment.Shout = GameId{0u, 0x00013E07u};
        CHECK_FALSE(CoopInventoryDiff::IsActorInventoryEmpty(shouter));
        CHECK(CoopInventoryDiff::ShouldApplyWholesaleActorInventory(
            shouter, /*aIsPlayer=*/false, /*aIsDead=*/false));
    }

    SECTION("zero-count or null-FormId entries still count as empty for living NPC protection")
    {
        Inventory degenerate{};
        Inventory::Entry zeroCount{};
        zeroCount.BaseId = GameId{0u, 0x00012EB7u};
        zeroCount.Count = 0;
        degenerate.Entries.push_back(zeroCount);

        Inventory::Entry nullId{};
        nullId.BaseId = GameId{};
        nullId.Count = 2;
        degenerate.Entries.push_back(nullId);

        CHECK(CoopInventoryDiff::IsActorInventoryEmpty(degenerate));
        CHECK_FALSE(CoopInventoryDiff::ShouldApplyWholesaleActorInventory(
            degenerate, /*aIsPlayer=*/false, /*aIsDead=*/false));
    }

    SECTION("dead body accepts empty wholesale inventory when fully looted")
    {
        CHECK(CoopInventoryDiff::ShouldApplyWholesaleActorInventory(
            emptyInv, /*aIsPlayer=*/false, /*aIsDead=*/true));
    }

    SECTION("player clone accepts empty wholesale inventory")
    {
        CHECK(CoopInventoryDiff::ShouldApplyWholesaleActorInventory(
            emptyInv, /*aIsPlayer=*/true, /*aIsDead=*/false));
    }
}

TEST_CASE("v4.1.2: cached remote weapon-draw retries never sheathe locally simulated NPCs", "[rfab][v412][combat][weapondraw]")
{
    // Remote living NPC and remote living player clone both apply delayed weapon-draw sync.
    CHECK(CoopInventoryDiff::ShouldApplyCachedRemoteWeaponDraw(
        /*aIsLocalPlayer=*/false, /*aIsRemote=*/true, /*aIsDead=*/false));

    // Locally owned NPCs, dead actors, and the local player (0x14) never apply cached remote weapon-draw overrides.
    CHECK_FALSE(CoopInventoryDiff::ShouldApplyCachedRemoteWeaponDraw(
        /*aIsLocalPlayer=*/false, /*aIsRemote=*/false, /*aIsDead=*/false));
    CHECK_FALSE(CoopInventoryDiff::ShouldApplyCachedRemoteWeaponDraw(
        /*aIsLocalPlayer=*/false, /*aIsRemote=*/true, /*aIsDead=*/true));
    CHECK_FALSE(CoopInventoryDiff::ShouldApplyCachedRemoteWeaponDraw(
        /*aIsLocalPlayer=*/true, /*aIsRemote=*/false, /*aIsDead=*/false));
    CHECK_FALSE(CoopInventoryDiff::ShouldApplyCachedRemoteWeaponDraw(
        /*aIsLocalPlayer=*/true, /*aIsRemote=*/true, /*aIsDead=*/false));
}

TEST_CASE("v4.1.2: SSEDisplayTweaks overlay forces borderless flip presentation in co-op only", "[rfab][v412][compat][display]")
{
    const Compat::Rule* pMainRule = Compat::FindRule("sse-display-tweaks");
    const Compat::Rule* pCustomRule = Compat::FindRule("sse-display-tweaks-custom");
    REQUIRE(pMainRule != nullptr);
    REQUIRE(pCustomRule != nullptr);

    SECTION("stock RFAB SSEDisplayTweaks.ini with commented Fullscreen/Borderless")
    {
        const std::string stockIni =
            "[Render]\r\n"
            "#Fullscreen=false\r\n"
            "#Borderless=false\r\n"
            "FramerateLimit=240\r\n"
            "LoadingScreenFramerateLimit=60\r\n"
            "[HAVOK]\r\n"
            "DynamicMaxTimeScaling=true\r\n";

        const std::string once = pMainRule->Transform(stockIni);
        const std::string twice = pMainRule->Transform(once);
        REQUIRE(twice == once);

        const auto changes = Compat::DiffKeys(stockIni, once);
        REQUIRE(changes.size() == 2);
        CHECK(once.find("Fullscreen = false\r\n") != std::string::npos);
        CHECK(once.find("Borderless = true\r\n") != std::string::npos);
        CHECK(once.find("FramerateLimit=240\r\n") != std::string::npos);
        CHECK(once.find("DynamicMaxTimeScaling=true\r\n") != std::string::npos);
    }

    SECTION("explicit Exclusive Fullscreen in SSEDisplayTweaks_Custom.ini is rewritten cleanly")
    {
        const std::string customExclusive =
            "[Render]\n"
            "Fullscreen = true\n"
            "Borderless = false\n"
            "SwapEffect = auto\n";

        const std::string out = pCustomRule->Transform(customExclusive);
        REQUIRE(pCustomRule->Transform(out) == out);
        CHECK(out.find("Fullscreen = false\n") != std::string::npos);
        CHECK(out.find("Borderless = true\n") != std::string::npos);
        CHECK(out.find("SwapEffect = auto\n") != std::string::npos);
    }
}
