// v4.1.3 regressions:
//   1. Carriage / fast-travel freeze & crash (RFAB_CarriagesTravel::OnAcceptTravel -> Game.FastTravel):
//      - HookUnequipObject (SE 36950) must NOT return early when !currentProcess || !middleProcess,
//        otherwise Low-process actors keep ExtraWorn set and Skyrim's native unequip retry loop spins
//        infinitely on the BSJobs worker thread while overflowing Session::Lane::kSimulation.
//      - CoopEquipGuard::UnequipRetryGuard breaks repeated tight (actorId, itemId) unequip bursts on
//        both remote and local actors without harming normal equipment protection.
//      - Session::MainThreadQueue::CanPost / TryRejectIfFull rejects full lanes before constructing
//        heap-backed std::function<void()> closures while preserving exact overflow stats.
//   2. Enemies non-reactive or running around unarmed across party leader changes & reconnections:
//      - Inventory::UpdateEquipment upserts newly equipped NPC weapons/bows/arrows/armor populated by
//        Skyrim's InitOutfitItems / InitLeveledItems (which bypass Actor::AddInventoryItem), handles
//        dual-wielded identical 1H weapons, and Inventory::operator== compares CurrentMagicEquipment.
//      - RemoteLifecycle::DecideGrantHandling defers TakeOwnership while an actor's 3D is not ready so
//        WaitingFor3D weapons are never discarded or overwritten on the server with empty gear.
//      - CoopEquipGuard::ShouldSyncPostAssignmentInventory pushes silent engine weapon init on new
//        assignments AND seeds empty dormant checkpoint inventories from live NPC gear.
//      - PartyService::OnPartyInfo notifies CharacterService::OnPartyLeadershipChanged on leader switch.

#include <catch2/catch.hpp>

// Before any Structs/ header: those rely on TiltedPhoques being declared, and test suites on both
// Linux and Windows compile without the encoding precompiled header.
#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <Structs/Inventory.h>
#include <common/CoopEquipGuard.h>
#include <common/CoopInventoryDiff.h>
#include <common/RemoteLifecycle.h>
#include <common/Session/MainThreadQueue.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace
{
std::filesystem::path FindRepoRootV413()
{
    namespace fs = std::filesystem;
    fs::path cur = fs::current_path();
    for (int i = 0; i < 8; ++i)
    {
        if (fs::exists(cur / "Code" / "common" / "CoopEquipGuard.h"))
            return cur;
        if (!cur.has_parent_path() || cur.parent_path() == cur)
            break;
        cur = cur.parent_path();
    }
    return {};
}

std::string ReadSourceV413(const std::filesystem::path& acPath)
{
    std::ifstream in(acPath, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
} // namespace

TEST_CASE("v4.1.3: carriage fast-travel unequip retry breaker protects both remote and local actors", "[rfab][v413][carriage][equip]")
{
    CoopEquipGuard::UnequipRetryGuard guard{};

    SECTION("remote actor blocks normal AI unequip attempts (<= 2) then breaks infinite engine retry loop")
    {
        const auto d1 = guard.Evaluate(0x0001A692u, 0x00013986u, 1000u, /*remote=*/true, /*local=*/false, false, false, false);
        CHECK_FALSE(d1.AllowEngineCall);
        CHECK_FALSE(d1.EmitNetworkEvent);
        CHECK_FALSE(d1.WasLoopBroken);

        const auto d2 = guard.Evaluate(0x0001A692u, 0x00013986u, 1001u, /*remote=*/true, /*local=*/false, false, false, false);
        CHECK_FALSE(d2.AllowEngineCall);
        CHECK_FALSE(d2.EmitNetworkEvent);
        CHECK_FALSE(d2.WasLoopBroken);

        // 3rd immediate retry on the same thread within 250ms: allow RealUnEquip silently so ExtraWorn clears!
        const auto d3 = guard.Evaluate(0x0001A692u, 0x00013986u, 1002u, /*remote=*/true, /*local=*/false, false, false, false);
        CHECK(d3.AllowEngineCall);
        CHECK_FALSE(d3.EmitNetworkEvent);
        CHECK(d3.WasLoopBroken);

        // Subsequent retries in the same burst stay silent without spamming warnings.
        const auto d4 = guard.Evaluate(0x0001A692u, 0x00013986u, 1003u, /*remote=*/true, /*local=*/false, false, false, false);
        CHECK(d4.AllowEngineCall);
        CHECK_FALSE(d4.EmitNetworkEvent);
        CHECK_FALSE(d4.WasLoopBroken);

        // After > 250ms burst window elapses, normal remote protection resumes cleanly.
        const auto afterWindow = guard.Evaluate(0x0001A692u, 0x00013986u, 1300u, /*remote=*/true, /*local=*/false, false, false, false);
        CHECK_FALSE(afterWindow.AllowEngineCall);
        CHECK_FALSE(afterWindow.EmitNetworkEvent);
    }

    SECTION("local actor emits at most 2 network events per tight retry burst so simulation lane never floods")
    {
        const auto l1 = guard.Evaluate(0x000B6FECu, 0x00013986u, 2000u, /*remote=*/false, /*local=*/true, false, false, false);
        CHECK(l1.AllowEngineCall);
        CHECK(l1.EmitNetworkEvent);
        CHECK_FALSE(l1.WasLoopBroken);

        const auto l2 = guard.Evaluate(0x000B6FECu, 0x00013986u, 2000u, /*remote=*/false, /*local=*/true, false, false, false);
        CHECK(l2.AllowEngineCall);
        CHECK(l2.EmitNetworkEvent);
        CHECK_FALSE(l2.WasLoopBroken);

        const auto l3 = guard.Evaluate(0x000B6FECu, 0x00013986u, 2001u, /*remote=*/false, /*local=*/true, false, false, false);
        CHECK(l3.AllowEngineCall);
        CHECK_FALSE(l3.EmitNetworkEvent);
        CHECK(l3.WasLoopBroken);

        // Equipping/unequipping a DIFFERENT item on the same frame resets the counter immediately.
        const auto nextItem = guard.Evaluate(0x000B6FECu, 0x0001397Du, 2001u, /*remote=*/false, /*local=*/true, false, false, false);
        CHECK(nextItem.AllowEngineCall);
        CHECK(nextItem.EmitNetworkEvent);
        CHECK_FALSE(nextItem.WasLoopBroken);
    }
}

TEST_CASE("v4.1.3: MainThreadQueue fast-path full check rejects without closure allocation and keeps exact stats", "[rfab][v413][mainthread]")
{
    Session::MainThreadQueue queue{/*aSimulationCapacity=*/2, /*aAlwaysCapacity=*/2};
    CHECK(queue.CanPost(Session::Lane::kSimulation));
    CHECK_FALSE(queue.TryRejectIfFull(Session::Lane::kSimulation, false));

    int executed = 0;
    REQUIRE(queue.Post(Session::Lane::kSimulation, Session::Token{1, 1}, false, [&]() { ++executed; }));
    REQUIRE(queue.Post(Session::Lane::kSimulation, Session::Token{1, 1}, false, [&]() { ++executed; }));

    CHECK_FALSE(queue.CanPost(Session::Lane::kSimulation));
    CHECK(queue.TryRejectIfFull(Session::Lane::kSimulation, /*aDuringWorldLoad=*/true));

    const auto stats = queue.GetStats(Session::Lane::kSimulation);
    CHECK(stats.Posted == 3);
    CHECK(stats.DroppedOverflow == 1);
    CHECK(stats.PostedDuringWorldLoad == 1);

    CHECK(queue.Drain(Session::Lane::kSimulation, [](Session::Token) noexcept { return true; }) == 2);
    CHECK(executed == 2);
    CHECK(queue.CanPost(Session::Lane::kSimulation));
}

TEST_CASE("v4.1.3: Inventory::UpdateEquipment upserts engine-generated NPC weapons and compares spells", "[rfab][v413][combat][inventory]")
{
    const GameId kGold{0u, 0x0000000Fu};
    const GameId kSteelGreatsword{7u, 0x000160DFu};
    const GameId kHuntingBow{0u, 0x00013985u};
    const GameId kIronArrow{0u, 0x0001397Du};
    const GameId kIronDagger{0u, 0x0001397Eu};
    const GameId kIceSpike{0u, 0x0002B96Cu};

    SECTION("bandit spawned with only gold keeps leveled weapon + arrows after UpdateEquipment")
    {
        Inventory serverInv{};
        Inventory::Entry gold{};
        gold.BaseId = kGold;
        gold.Count = 12;
        serverInv.Entries.push_back(gold);

        // Skyrim's InitOutfitItems / InitLeveledItems equips weapon + arrows without AddInventoryItem:
        Inventory equippedSnapshot{};
        Inventory::Entry sword{};
        sword.BaseId = kSteelGreatsword;
        sword.Count = 1;
        sword.ExtraWorn = true;
        equippedSnapshot.Entries.push_back(sword);

        Inventory::Entry arrows{};
        arrows.BaseId = kIronArrow;
        arrows.Count = 25;
        arrows.ExtraWorn = true;
        equippedSnapshot.Entries.push_back(arrows);

        serverInv.UpdateEquipment(equippedSnapshot);

        GameId goldLookup = kGold;
        GameId swordLookup = kSteelGreatsword;
        GameId arrowLookup = kIronArrow;
        CHECK(serverInv.GetEntryCountById(goldLookup) == 12);
        CHECK(serverInv.GetEntryCountById(swordLookup) == 1);
        CHECK(serverInv.GetEntryCountById(arrowLookup) == 25);

        const auto swordOpt = serverInv.GetEntryById(swordLookup);
        REQUIRE(swordOpt.has_value());
        CHECK(swordOpt->ExtraWorn);
        CHECK_FALSE(swordOpt->ExtraWornLeft);

        // And when applied differentially to a peer/reconnecting bandit who had the sword + bow,
        // the sword stays equipped instead of being stripped!
        Inventory peerCurrent = serverInv;
        Inventory::Entry bow{};
        bow.BaseId = kHuntingBow;
        bow.Count = 1;
        peerCurrent.Entries.push_back(bow);
        const auto delta = CoopInventoryDiff::ComputeInventoryDelta<Inventory::Entry>(
            peerCurrent.Entries, serverInv.Entries, false);
        bool removedSword = false;
        for (const auto& rem : delta.ItemsToRemove)
            if (rem.BaseId == kSteelGreatsword)
                removedSword = true;
        CHECK_FALSE(removedSword);
    }

    SECTION("dual-wielding two identical 1H daggers preserves both ExtraWorn and ExtraWornLeft")
    {
        Inventory serverInv{};
        Inventory::Entry d1{};
        d1.BaseId = kIronDagger;
        d1.Count = 1;
        Inventory::Entry d2{};
        d2.BaseId = kIronDagger;
        d2.Count = 1;
        serverInv.Entries.push_back(d1);
        serverInv.Entries.push_back(d2);

        Inventory dualWield{};
        d1.ExtraWorn = true;
        d2.ExtraWornLeft = true;
        dualWield.Entries.push_back(d1);
        dualWield.Entries.push_back(d2);

        serverInv.UpdateEquipment(dualWield);
        REQUIRE(serverInv.Entries.size() == 2);
        CHECK(serverInv.Entries[0].ExtraWorn);
        CHECK_FALSE(serverInv.Entries[0].ExtraWornLeft);
        CHECK_FALSE(serverInv.Entries[1].ExtraWorn);
        CHECK(serverInv.Entries[1].ExtraWornLeft);
    }

    SECTION("Inventory::operator== detects spell/shout differences when physical Entries match")
    {
        Inventory a{};
        Inventory b{};
        CHECK(a == b);

        b.CurrentMagicEquipment.RightHandSpell = kIceSpike;
        CHECK(a != b);
    }
}

TEST_CASE("v4.1.3: ownership grant defers until 3D is ready and post-assignment sync catches silent gear", "[rfab][v413][ownership][sync]")
{
    SECTION("DecideGrantHandling defers remote actor whose 3D has not loaded yet")
    {
        CHECK(RemoteLifecycle::DecideGrantHandling(
                  /*aEntityExists=*/true,
                  /*aActorIsRemoteHere=*/true,
                  /*aActorIsLocalHere=*/false,
                  /*aIsQueuedForMaterialization=*/true,
                  /*aIsWaitingFor3DNotReady=*/true) == RemoteLifecycle::GrantAction::Defer);

        CHECK(RemoteLifecycle::DecideGrantHandling(
                  /*aEntityExists=*/true,
                  /*aActorIsRemoteHere=*/true,
                  /*aActorIsLocalHere=*/false,
                  /*aIsQueuedForMaterialization=*/true,
                  /*aIsWaitingFor3DNotReady=*/false) == RemoteLifecycle::GrantAction::TakeOver);
    }

    SECTION("ShouldSyncPostAssignmentInventory covers new silent outfits and unseeded dormant records")
    {
        // New entity: silent InitOutfitItems changed inventory without setting DirtyInventory.
        CHECK(CoopEquipGuard::ShouldSyncPostAssignmentInventory(
            /*existing=*/false, /*dirty=*/false, /*differs=*/true,
            /*player=*/false, /*dead=*/false, /*serverEmpty=*/true, /*liveNonEmpty=*/true));

        // Existing checkpoint dormant entity: server inventory is empty, live NPC has real weapons.
        CHECK(CoopEquipGuard::ShouldSyncPostAssignmentInventory(
            /*existing=*/true, /*dirty=*/false, /*differs=*/true,
            /*player=*/false, /*dead=*/false, /*serverEmpty=*/true, /*liveNonEmpty=*/true));

        // Existing already-seeded server entity: server is authoritative, do not overwrite upwards.
        CHECK_FALSE(CoopEquipGuard::ShouldSyncPostAssignmentInventory(
            /*existing=*/true, /*dirty=*/true, /*differs=*/true,
            /*player=*/false, /*dead=*/false, /*serverEmpty=*/false, /*liveNonEmpty=*/true));
    }
}

TEST_CASE("v4.1.3: client and server source wiring guards carriage travel and leader-switch sync", "[rfab][v413][audit]")
{
    const auto root = FindRepoRootV413();
    if (root.empty())
    {
        WARN("repository root not found from test working directory; skipping source audit");
        return;
    }

    const std::string actorCpp = ReadSourceV413(root / "Code" / "client" / "Games" / "Skyrim" / "Actor.cpp");
    const std::string charCpp = ReadSourceV413(root / "Code" / "client" / "Services" / "Generic" / "CharacterService.cpp");
    const std::string partyClientCpp = ReadSourceV413(root / "Code" / "client" / "Services" / "Generic" / "PartyService.cpp");
    const std::string partyServerCpp = ReadSourceV413(root / "Code" / "server" / "Services" / "PartyService.cpp");

    REQUIRE_FALSE(actorCpp.empty());
    REQUIRE_FALSE(charCpp.empty());
    REQUIRE_FALSE(partyClientCpp.empty());
    REQUIRE_FALSE(partyServerCpp.empty());

    // 1. HookUnequipObject must NOT reject Low-process (!middleProcess) actors during fast travel.
    const size_t hookUnequipPos = actorCpp.find("HookUnequipObject, Actor");
    REQUIRE(hookUnequipPos != std::string::npos);
    const size_t hookUnequipEnd = actorCpp.find("RealUnequipObject", hookUnequipPos);
    REQUIRE(hookUnequipEnd != std::string::npos);
    const std::string hookUnequipBody = actorCpp.substr(hookUnequipPos, hookUnequipEnd - hookUnequipPos);
    CHECK(hookUnequipBody.find("apThis->currentProcess->middleProcess") == std::string::npos);

    // 2. Party leadership updates notify CharacterService on client and ReconcileAllEntitiesForObserver on server.
    CHECK(partyClientCpp.find("GetCharacterService().OnPartyLeadershipChanged(") != std::string::npos);
    CHECK(partyServerCpp.find("GetCharacterService().ReconcileAllEntitiesForObserver(") != std::string::npos);

    // 3. OnAssignCharacter remote branch uses ShouldApplyWholesaleActorInventory before SetActorInventory.
    CHECK(charCpp.find("shouldApplyRemoteInv") != std::string::npos);
}
