#include <catch2/catch.hpp>

#include <common/Authority/Lease.h>
#include <common/Compat/ConfigOverlay.h>
#include <common/Compat/MenuPausePolicy.h>
#include <common/CoopCraftedItems.h>
#include <common/CoopLootLedger.h>
#include <common/RfabCoopProgression.h>
#include <common/RfabModpackCompat.h>
#include <common/RfabWorldImmersion.h>
#include <Structs/Inventory.h>
#include <TiltedCore/Buffer.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
void WriteValidSseEss(const std::filesystem::path& acPath)
{
    std::ofstream ofs(acPath, std::ios::binary | std::ios::trunc);
    ofs.write("TESV_SAVEGAME", 13);

    auto writeU32 = [&](uint32_t v)
    {
        uint8_t b[4]{
            static_cast<uint8_t>(v & 0xFFu),
            static_cast<uint8_t>((v >> 8) & 0xFFu),
            static_cast<uint8_t>((v >> 16) & 0xFFu),
            static_cast<uint8_t>((v >> 24) & 0xFFu),
        };
        ofs.write(reinterpret_cast<const char*>(b), 4);
    };
    auto writeU16 = [&](uint16_t v)
    {
        uint8_t b[2]{static_cast<uint8_t>(v & 0xFFu), static_cast<uint8_t>((v >> 8) & 0xFFu)};
        ofs.write(reinterpret_cast<const char*>(b), 2);
    };

    std::vector<uint8_t> hdr;
    auto pushU32 = [&](uint32_t v)
    {
        hdr.push_back(static_cast<uint8_t>(v & 0xFFu));
        hdr.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
        hdr.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
        hdr.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
    };
    auto pushU16 = [&](uint16_t v)
    {
        hdr.push_back(static_cast<uint8_t>(v & 0xFFu));
        hdr.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    };
    auto pushStr = [&](const char* s)
    {
        const uint16_t len = static_cast<uint16_t>(std::strlen(s));
        pushU16(len);
        hdr.insert(hdr.end(), s, s + len);
    };

    pushU32(12u);       // SSE version 12
    pushU32(1u);        // saveNumber
    pushStr("Dovah");   // playerName
    pushU32(10u);       // playerLevel
    pushStr("Whiterun");// playerLocation
    pushStr("001.00");  // gameDate
    pushStr("Nord");    // playerRace
    pushU16(0u);        // playerSex
    pushU32(0u);        // curExp
    pushU32(0u);        // lvlUpExp
    pushU32(0u);        // filetime lo
    pushU32(0u);        // filetime hi
    pushU32(2u);        // screenshot width
    pushU32(2u);        // screenshot height
    pushU16(0u);        // uncompressed

    writeU32(static_cast<uint32_t>(hdr.size()));
    ofs.write(reinterpret_cast<const char*>(hdr.data()), static_cast<std::streamsize>(hdr.size()));

    // 2x2 RGBA screenshot = 16 bytes + 256 bytes body (>= 256-byte minimum valid SSE .ess size)
    std::vector<uint8_t> body(16 + 256, 0x5Au);
    ofs.write(reinterpret_cast<const char*>(body.data()), static_cast<std::streamsize>(body.size()));
    (void)writeU16;
}

void WriteValidSkseCosave(const std::filesystem::path& acPath, uint32_t aNumPlugins = 3)
{
    std::ofstream ofs(acPath, std::ios::binary | std::ios::trunc);
    ofs.write("SKSE", 4);
    auto writeU32 = [&](uint32_t v)
    {
        uint8_t b[4]{
            static_cast<uint8_t>(v & 0xFFu),
            static_cast<uint8_t>((v >> 8) & 0xFFu),
            static_cast<uint8_t>((v >> 16) & 0xFFu),
            static_cast<uint8_t>((v >> 24) & 0xFFu),
        };
        ofs.write(reinterpret_cast<const char*>(b), 4);
    };
    writeU32(1u);          // formatVersion
    writeU32(0x02001400u); // skse 2.0.20
    writeU32(0x01050610u); // runtime 1.5.97
    writeU32(aNumPlugins);
    std::vector<uint8_t> pluginRecords(static_cast<size_t>(aNumPlugins) * 16u, 0x11u);
    ofs.write(reinterpret_cast<const char*>(pluginRecords.data()), static_cast<std::streamsize>(pluginRecords.size()));
}
} // namespace

TEST_CASE("3A.1 RFAB.ini overlay forces bAlwaysActive=true and MenuPausePolicy recognizes QuickLootIE/WheelMenu", "[rfab][phase3][overlay]")
{
    const Compat::Rule* pRule = Compat::FindRule("rfab-ini");
    REQUIRE(pRule != nullptr);
    REQUIRE(pRule->Kind == Compat::RuleKind::kOverlay);
    REQUIRE(pRule->DataRelativePath == "RFAB.ini");
    REQUIRE(pRule->IsManagedKey("bAlwaysActive"));
    REQUIRE_FALSE(pRule->IsManagedKey("bShowCustomHud"));

    const std::string stockRfabIni =
        "[General]\r\n"
        "bAlwaysActive=false\r\n"
        "bKeepOtherSetting=true\r\n";

    const std::string once = pRule->Transform(stockRfabIni);
    const std::string twice = pRule->Transform(once);
    REQUIRE(once == twice);
    REQUIRE(once.find("bAlwaysActive") != std::string::npos);
    REQUIRE(once.find("true") != std::string::npos);
    REQUIRE(once.find("bKeepOtherSetting=true") != std::string::npos);

    const auto diffs = Compat::DiffKeys(stockRfabIni, once);
    REQUIRE(diffs.size() == 1);
    REQUIRE(diffs[0].Key == "[General] bAlwaysActive");
    REQUIRE(diffs[0].From == "false");
    REQUIRE(diffs[0].To == "true");

    REQUIRE(Compat::IsInteractiveLootMenu("ContainerMenu"));
    REQUIRE(Compat::IsInteractiveLootMenu("LootMenu"));
    REQUIRE_FALSE(Compat::IsInteractiveLootMenu("InventoryMenu"));

    REQUIRE(Compat::IsNonPausingRfabOverlayMenu("LootMenu"));
    REQUIRE(Compat::IsNonPausingRfabOverlayMenu("WheelMenu"));
    REQUIRE(Compat::IsNonPausingRfabOverlayMenu("RFAB WheelMenu"));
    REQUIRE(Compat::IsNonPausingRfabOverlayMenu("TrueHUD"));
    REQUIRE_FALSE(Compat::IsNonPausingRfabOverlayMenu("StatsMenu"));

    REQUIRE(Compat::IsPauseRequiredRfabMenu("StatsMenu"));
    REQUIRE(Compat::IsPauseRequiredRfabMenu("CustomMenu"));
}

TEST_CASE("3A.2 PendingContainerLootQueue buffers QuickLootIE/AutoLoot deltas safely across registration", "[rfab][phase3][loot]")
{
    CoopLootLedger::PendingContainerLootQueue<Inventory::Entry> queue;
    Inventory::Entry gold{};
    gold.BaseId = GameId{0u, 0x0000000Fu};
    gold.Count = -150;

    Inventory::Entry potion{};
    potion.BaseId = GameId{0u, 0x0003EADEu};
    potion.Count = -2;

    REQUIRE(queue.Enqueue(0x00099111u, gold, false, true, 1000ULL));
    REQUIRE(queue.Enqueue(0x00099222u, potion, false, true, 1200ULL));
    REQUIRE(queue.Enqueue(0x00099111u, potion, false, true, 1300ULL));
    REQUIRE(queue.Size() == 3);

    const auto chest1 = queue.TakeForContainer(0x00099111u, 2000ULL);
    REQUIRE(chest1.size() == 2);
    REQUIRE(chest1[0].Item.BaseId == gold.BaseId);
    REQUIRE(chest1[0].Item.Count == -150);
    REQUIRE(chest1[1].Item.BaseId == potion.BaseId);
    REQUIRE(queue.Size() == 1);

    // TTL expiration drops stale pending deltas cleanly.
    const auto expired = queue.TakeForContainer(0x00099222u, 20000ULL);
    REQUIRE(expired.empty());
    REQUIRE(queue.Empty());
}

TEST_CASE("3A.3 Atomic .ess + .skse co-save verification catches corrupt or truncated co-saves", "[rfab][phase3][saveload]")
{
    const auto tempDir = std::filesystem::temp_directory_path() / "rfab_v411_phase3_cosave_test";
    std::error_code ec;
    std::filesystem::remove_all(tempDir, ec);
    std::filesystem::create_directories(tempDir, ec);

    const auto essPath = tempDir / "RFAB_MP_Epoch_42.ess";
    const auto sksePath = RfabCompat::Detail::DeriveSkseCosavePath(essPath);
    REQUIRE(sksePath.filename() == "RFAB_MP_Epoch_42.skse");

    // 1. Missing .ess -> kMissingEss
    REQUIRE(RfabCompat::Detail::ValidateSaveSlotPairOnDisk(essPath, false) ==
            RfabCompat::Detail::SavePairValidationResult::kMissingEss);

    // 2. Valid .ess without .skse -> kOk when optional, kMissingSkse when required
    WriteValidSseEss(essPath);
    REQUIRE(RfabCompat::Detail::ValidateSaveSlotPairOnDisk(essPath, false) ==
            RfabCompat::Detail::SavePairValidationResult::kOk);
    REQUIRE(RfabCompat::Detail::ValidateSaveSlotPairOnDisk(essPath, true) ==
            RfabCompat::Detail::SavePairValidationResult::kMissingSkse);

    // 3. Valid .ess + valid .skse -> kOk
    WriteValidSkseCosave(sksePath, 4u);
    REQUIRE(RfabCompat::Detail::IsValidSkseCosaveOnDisk(sksePath));
    REQUIRE(RfabCompat::Detail::ValidateSaveSlotPairOnDisk(essPath, true) ==
            RfabCompat::Detail::SavePairValidationResult::kOk);

    // 4. Valid .ess + truncated 0-byte .skse -> kCorruptSkse even when optional!
    {
        std::ofstream truncSkse(sksePath, std::ios::binary | std::ios::trunc);
    }
    REQUIRE_FALSE(RfabCompat::Detail::IsValidSkseCosaveOnDisk(sksePath));
    REQUIRE(RfabCompat::Detail::ValidateSaveSlotPairOnDisk(essPath, false) ==
            RfabCompat::Detail::SavePairValidationResult::kCorruptSkse);

    // 5. Valid .ess + bad-magic .skse -> kCorruptSkse, and QuarantineCorruptEssFilesInDirectory quarantines it!
    {
        std::ofstream badSkse(sksePath, std::ios::binary | std::ios::trunc);
        badSkse.write("BAD!012345678901234567890123456789", 34);
    }
    REQUIRE(RfabCompat::Detail::ValidateSaveSlotPairOnDisk(essPath, false) ==
            RfabCompat::Detail::SavePairValidationResult::kCorruptSkse);
    const size_t moved = RfabCompat::Detail::QuarantineCorruptEssFilesInDirectory(tempDir);
    REQUIRE(moved == 1);
    REQUIRE_FALSE(std::filesystem::exists(sksePath, ec));
    REQUIRE(std::filesystem::exists(essPath, ec));

    std::filesystem::remove_all(tempDir, ec);
}

TEST_CASE("3A.4 Crafted Enchantment structural identity and Tempered float tolerance work across clients", "[rfab][phase3][crafted]")
{
    Inventory::Entry swordClientA{};
    swordClientA.BaseId = GameId{0u, 0x000139B9u}; // Daedric Sword
    swordClientA.Count = 1;
    swordClientA.ExtraHealth = 1.600000f;
    swordClientA.ExtraEnchantId = GameId{0xFFFFFFFFu, 0x00123456u}; // Client A's local 0xFF form ID
    swordClientA.ExtraEnchantCharge = 1500;
    swordClientA.EnchantData.IsWeapon = true;
    swordClientA.EnchantData.Effects.push_back(Inventory::EffectItem{25.0f, 0, 1, 10.0f, GameId{0u, 0x0004605Au}});
    swordClientA.EnchantData.Effects.push_back(Inventory::EffectItem{15.0f, 0, 5, 8.0f, GameId{0u, 0x0004605Bu}});

    // Client B has the EXACT SAME double-enchanted & tempered sword, but with a DIFFERENT local 0xFF id,
    // reversed effect ordering, and tiny wire-quantization float drift on ExtraHealth!
    Inventory::Entry swordClientB = swordClientA;
    swordClientB.ExtraHealth = 1.6000004f;
    swordClientB.ExtraEnchantId = GameId{0xFFFFFFFFu, 0x00ABCDEFu};
    std::swap(swordClientB.EnchantData.Effects[0], swordClientB.EnchantData.Effects[1]);

    REQUIRE(swordClientA.IsExtraDataEquals(swordClientB));
    REQUIRE(swordClientA.CanBeMerged(swordClientB));
    REQUIRE(CoopCraftedItems::ClassifyCraftedItem(swordClientA) == CoopCraftedItems::CraftedItemKind::kTemperedAndEnchanted);

    // Normalizing stamps identical deterministic non-zero fingerprint BaseId on both clients!
    CoopCraftedItems::NormalizeCraftedEntry(swordClientA);
    CoopCraftedItems::NormalizeCraftedEntry(swordClientB);
    REQUIRE(swordClientA.ExtraEnchantId.ModId == 0xFFFFFFFFu);
    REQUIRE(swordClientA.ExtraEnchantId.BaseId != 0u);
    REQUIRE(swordClientA.ExtraEnchantId == swordClientB.ExtraEnchantId);

    // Different enchantment magnitude MUST NOT match or collide!
    Inventory::Entry swordDifferent = swordClientA;
    swordDifferent.EnchantData.Effects[0].Magnitude = 40.0f;
    CoopCraftedItems::NormalizeCraftedEntry(swordDifferent);
    REQUIRE_FALSE(swordClientA.IsExtraDataEquals(swordDifferent));
    REQUIRE(swordClientA.ExtraEnchantId != swordDifferent.ExtraEnchantId);

    // Wire round-trip preserves tempered + double-enchanted item cleanly.
    TiltedPhoques::Buffer buf(1024);
    TiltedPhoques::Buffer::Writer writer(&buf);
    swordClientA.Serialize(writer);
    TiltedPhoques::Buffer::Reader reader(&buf);
    Inventory::Entry decoded{};
    decoded.Deserialize(reader);
    REQUIRE(decoded == swordClientA);
    REQUIRE(decoded.EnchantData.Effects.size() == 2);
}

TEST_CASE("3B.1 Boss Lease Lock prevents mid-phase ownership steals and Ally Protection blocks knockback/ragdoll", "[rfab][phase3][boss][ally]")
{
    const auto bossPolicy = Authority::BossEncounterLeasePolicy();
    REQUIRE(bossPolicy.MinHoldMs == 5000ull);
    REQUIRE(bossPolicy.LeaseDurationMs == 15000ull);

    Authority::Lease activeLease{};
    activeLease.OwnerId = 1;
    activeLease.Epoch = 3;
    activeLease.GrantedAtMs = 10000;
    activeLease.ExpiresAtMs = 10000 + bossPolicy.LeaseDurationMs;

    Authority::Subject bossSubject{};
    bossSubject.IsBossEncounterLocked = true;

    Authority::OwnerState ownerSimulating{true, true};
    Authority::Candidate calmLeader{2, false, true, true};

    // Even a calm party leader after MinHoldMs CANNOT steal an active locked boss phase while owner simulates!
    const auto denied = Authority::Claim(activeLease, bossSubject, ownerSimulating, calmLeader, 16000, bossPolicy);
    REQUIRE(denied.Result == Authority::Outcome::kDeniedBossEncounterLocked);
    REQUIRE_FALSE(denied.Changed);
    REQUIRE(denied.Next.OwnerId == 1);

    // When the owner's lease expires (stalled/paused > 15s), handoff succeeds cleanly!
    const auto expiredTakeover = Authority::Claim(activeLease, bossSubject, ownerSimulating, calmLeader, 26000, bossPolicy);
    REQUIRE(expiredTakeover.Result == Authority::Outcome::kGranted);
    REQUIRE(expiredTakeover.Changed);
    REQUIRE(expiredTakeover.Next.OwnerId == 2);

    // When owner leaves range, handoff succeeds immediately!
    Authority::OwnerState ownerLeft{true, false};
    const auto oobTakeover = Authority::Claim(activeLease, bossSubject, ownerLeft, calmLeader, 11000, bossPolicy);
    REQUIRE(oobTakeover.Result == Authority::Outcome::kGranted);
    REQUIRE(oobTakeover.Next.OwnerId == 2);

    // Ally knockback/ragdoll/crowd-control policy checks
    const auto protectedAlly = RfabCompat::EvaluateCoopAllyActionPolicy(true, true, false);
    REQUIRE(protectedAlly.BlockHarmfulAction);
    REQUIRE(protectedAlly.BlockKnockbackAndRagdoll);

    const auto healAlly = RfabCompat::EvaluateCoopAllyActionPolicy(true, true, true);
    REQUIRE_FALSE(healAlly.BlockHarmfulAction);
    REQUIRE_FALSE(healAlly.BlockKnockbackAndRagdoll);
    REQUIRE(healAlly.AllowBeneficialMagic);

    REQUIRE(RfabCompat::IsKnockbackOrCrowdControlEffect(30)); // kConcussion (Unrelenting Force)
    REQUIRE(RfabCompat::IsKnockbackOrCrowdControlEffect(33)); // kStagger (Requiem bullrush)
    REQUIRE(RfabCompat::IsKnockbackOrCrowdControlEffect(9));  // kDisarm
    REQUIRE(RfabCompat::IsKnockbackOrCrowdControlEffect(21)); // kParalysis
    REQUIRE(RfabCompat::IsKnockbackOrCrowdControlEffect(42)); // kBanish
    REQUIRE(RfabCompat::IsKnockbackOrCrowdControlEffect(1, "RFAB_Script_PushActorAway"));
    REQUIRE_FALSE(RfabCompat::IsKnockbackOrCrowdControlEffect(0, "RestoreHealth"));
}

TEST_CASE("3B.2 Shared Dragon Souls, Word Walls, Difficulty/Cursed Handshake, Quests.ini Dedupe & Late-Joiner Catch-Up", "[rfab][phase3][progression]")
{
    // 1. Shared Dragon Souls Ledger
    RfabCoopProgression::DragonSoulLedger soulLedger;
    const std::string dragonKey = RfabCoopProgression::DragonSoulLedger::MakeDragonKey(GameId{0u, 0x0001CA03u}, 99u);
    REQUIRE(dragonKey == "dragon:00000000:0001CA03");

    const auto firstAward = soulLedger.RecordDragonKill(dragonKey, 1u, {1u, 2u, 3u}, false);
    REQUIRE(firstAward.IsNewDragonKill);
    REQUIRE(firstAward.NewlyAwardedPlayerIds == std::vector<uint32_t>{2u, 3u});
    REQUIRE(soulLedger.HasPlayerReceivedSoul(dragonKey, 1u));
    REQUIRE(soulLedger.HasPlayerReceivedSoul(dragonKey, 2u));
    REQUIRE(soulLedger.HasPlayerReceivedSoul(dragonKey, 3u));

    // Duplicate report from peer 2 awards 0 extra souls!
    const auto dupAward = soulLedger.RecordDragonKill(dragonKey, 2u, {1u, 2u, 3u}, false);
    REQUIRE_FALSE(dupAward.IsNewDragonKill);
    REQUIRE(dupAward.NewlyAwardedPlayerIds.empty());

    // 2. Shared Word Wall Ledger
    RfabCoopProgression::WordWallLedger wordLedger;
    const GameId fusWord{0u, 0x00013E22u};
    REQUIRE(wordLedger.RecordWordLearned(fusWord));
    REQUIRE_FALSE(wordLedger.RecordWordLearned(fusWord));
    REQUIRE(wordLedger.Size() == 1);

    // 3. RFAB_StartMenu Difficulty & Cursed Handshake
    const auto wanderer = RfabCoopProgression::InferDifficultyFromGmst(2.0f, 0.5f);
    const auto dovahkiin = RfabCoopProgression::InferDifficultyFromGmst(1.0f, 1.0f);
    const auto legendCursed = RfabCoopProgression::InferDifficultyFromGmst(0.5f, 2.0f);
    REQUIRE(wanderer.Tier == RfabCoopProgression::RfabDifficultyTier::kWanderer);
    REQUIRE_FALSE(wanderer.IsCursed);
    REQUIRE(dovahkiin.Tier == RfabCoopProgression::RfabDifficultyTier::kDovahkiin);
    REQUIRE(legendCursed.Tier == RfabCoopProgression::RfabDifficultyTier::kLegend);
    REQUIRE(legendCursed.IsCursed);

    const int32_t packed = RfabCoopProgression::PackDifficultyHandshake(legendCursed.Tier, legendCursed.IsCursed);
    const auto unpacked = RfabCoopProgression::UnpackDifficultyHandshake(packed, 2.0f);
    REQUIRE(unpacked.Tier == RfabCoopProgression::RfabDifficultyTier::kLegend);
    REQUIRE(unpacked.IsCursed);

    REQUIRE(RfabCoopProgression::EvaluateDifficultyHandshake(legendCursed, unpacked) ==
            RfabCoopProgression::DifficultyParityOutcome::kMatch);
    REQUIRE(RfabCoopProgression::EvaluateDifficultyHandshake(legendCursed, dovahkiin) ==
            RfabCoopProgression::DifficultyParityOutcome::kBothMismatch);
    const std::string notice = RfabCoopProgression::FormatDifficultyMismatchNotice(legendCursed, dovahkiin);
    REQUIRE(notice.find("Legend+Cursed") != std::string::npos);
    REQUIRE(notice.find("Dovahkiin") != std::string::npos);

    // 4. Quests.ini Canonical UTF-8/Cyrillic Dedupe Key
    const std::string k1 = RfabCoopProgression::MakeCanonicalQuestsIniSourceKey(GameId{}, 200, "  Ветреный   пик  ");
    const std::string k2 = RfabCoopProgression::MakeCanonicalQuestsIniSourceKey(GameId{}, 200, "Ветреный пик");
    const std::string k3 = RfabCoopProgression::MakeCanonicalQuestsIniSourceKey(GameId{}, 200, "Дракон в небе");
    REQUIRE(!k1.empty());
    REQUIRE(k1 == k2);
    REQUIRE(k1 != k3);

    // 5. Late-Joiner Catch-Up XP
    RfabCoopProgression::LateJoinCatchUpTracker catchUp;
    catchUp.MarkAlreadySyncedAtSessionStart(1u);
    REQUIRE_FALSE(catchUp.EvaluateAndMark(1u, 12000, true).ShouldGrant);

    const auto grant = catchUp.EvaluateAndMark(2u, 40000, true, 25000);
    REQUIRE(grant.ShouldGrant);
    REQUIRE(grant.GrantXpDelta == 25000); // Bounded!
    // Reconnecting peer never double-receives catch-up XP!
    REQUIRE_FALSE(catchUp.EvaluateAndMark(2u, 40000, true, 25000).ShouldGrant);
    // Disabled setting grants nothing
    REQUIRE_FALSE(catchUp.EvaluateAndMark(3u, 15000, false, 25000).ShouldGrant);
}

TEST_CASE("3C RFAB World Immersion: Mount Ownership, Personal Blessings/CrimeGold, and TrueHUD Synergy", "[rfab][phase3][immersion]")
{
    // 1. MountOwnershipGuard
    RfabWorldImmersion::MountOwnershipGuard mountGuard;
    REQUIRE(mountGuard.TryAcquireRide(500u, 1u, 10u));
    // Peer 2 cannot hijack horse 500 while Player 1 is riding it!
    REQUIRE_FALSE(mountGuard.TryAcquireRide(500u, 2u, 20u));
    REQUIRE_FALSE(mountGuard.CanTransferMountOwnership(500u, 2u));
    REQUIRE_FALSE(mountGuard.CanMutateMountSaddlebag(500u, 2u, 1u));
    REQUIRE(mountGuard.CanMutateMountSaddlebag(500u, 1u, 1u));

    // Player 1 disconnects -> horse 500 is immediately released and mountable by Player 2!
    mountGuard.ReleaseAllForPlayer(1u);
    REQUIRE(mountGuard.CanTransferMountOwnership(500u, 2u));
    REQUIRE(mountGuard.TryAcquireRide(500u, 2u, 20u));
    REQUIRE(mountGuard.GetActiveRiderPlayerId(500u) == 2u);

    // 2. PersonalBlessingCrimePolicy
    REQUIRE(RfabWorldImmersion::IsPersonalCrimeOrBlessingGlobal("RFAB_SaveBlessing"));
    REQUIRE(RfabWorldImmersion::IsPersonalCrimeOrBlessingGlobal("CrimeGoldWhiterun"));
    REQUIRE(RfabWorldImmersion::IsPersonalCrimeOrBlessingGlobal("REQ_Blessing_Arkay"));
    REQUIRE_FALSE(RfabWorldImmersion::IsPersonalCrimeOrBlessingGlobal("RFAB_Boss_UmbraPhase"));
    REQUIRE(RfabWorldImmersion::IsPersonalShrineOrDoomstoneActivator("RFAB_Shrine_Akatosh"));
    REQUIRE(RfabWorldImmersion::IsPersonalShrineOrDoomstoneActivator("RFAB_BA_UniversalAltar"));
    REQUIRE(RfabWorldImmersion::IsPersonalShrineOrDoomstoneActivator("DoomstoneWarrior"));

    // 3. TrueHUD Synergy
    REQUIRE(RfabWorldImmersion::IsTrueHudResourceBarActorValue(24u)); // Health
    REQUIRE(RfabWorldImmersion::IsTrueHudResourceBarActorValue(25u)); // Magicka
    REQUIRE(RfabWorldImmersion::IsTrueHudResourceBarActorValue(26u)); // Stamina
    REQUIRE(RfabWorldImmersion::IsTrueHudResourceBarActorValue(82u)); // WardPower
    REQUIRE(RfabWorldImmersion::IsTrueHudBossBarEligible(true, false, 400.0f, false));
    REQUIRE(RfabWorldImmersion::IsTrueHudBossBarEligible(false, true, 500.0f, false));
    REQUIRE_FALSE(RfabWorldImmersion::IsTrueHudBossBarEligible(false, false, 1200.0f, true));
}
