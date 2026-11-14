#include <common/RfabModpackCompat.h>
#include <common/Compat/ConfigOverlay.h>
#include <common/CoopOwnership.h>
#include <common/CoopFlightDigest.h>
#ifndef RFAB_STANDALONE_NO_ESLOADER
#include <es_loader/stdafx.h>
#include <es_loader/ESLoader.h>
#include <es_loader/TESFile.h>
#endif

#include <catch2/catch.hpp>
#include <common/CoopOwnership.h>
#include <Structs/AnimationGraphDescriptorManager.h>
#include <Structs/Skyrim/AnimationGraphDescriptor_DraugrBehavior.h>
#include <Structs/Skyrim/AnimationGraphDescriptor_Falmer_Master_Behavior.h>
#include <Structs/Skyrim/AnimationGraphDescriptor_Master_Behavior.h>
#include <atomic>
#include <thread>
#include <vector>

TEST_CASE("RFAB Modpack Self-Healers, Endpoint Normalization & ESLoader BOM/Overflow Safety", "[rfab][hardening]")
{
    REQUIRE(RfabCompat::NormalizeServerEndpoint("") == "127.0.0.1:10578");
    REQUIRE(RfabCompat::NormalizeServerEndpoint("26.160.34.131") == "26.160.34.131:10578");
    REQUIRE(RfabCompat::NormalizeServerEndpoint("26.160.34.131:10578:10578") == "26.160.34.131:10578");
    REQUIRE(RfabCompat::AreBuildVersionsProtocolCompatible("v1.0.0-6-g383738a6", "v1.0.0-7-gc0b262aa"));
    REQUIRE(RfabCompat::AreBuildVersionsProtocolCompatible("v1.0.0", "v1.0.9-12-gabcdef0"));
    REQUIRE_FALSE(RfabCompat::AreBuildVersionsProtocolCompatible("v1.0.0", "v1.1.0"));
    REQUIRE_FALSE(RfabCompat::AreBuildVersionsProtocolCompatible("383738a6", "c0b262aa"));

    std::string partialOverride = "[Patches]\r\nRegularQuicksaves = false\r\n\r\n[SaveGameMaxSize]\r\nSaveGameMaxSize = true\r\n";
    REQUIRE(RfabCompat::PatchEngineFixesTomlContent(partialOverride));
    REQUIRE(partialOverride.find("MemoryManager = false") != std::string::npos);
    REQUIRE(partialOverride.find("ScaleformAllocator = false") != std::string::npos);
    REQUIRE(partialOverride.find("RegularQuicksaves = true") != std::string::npos);
    REQUIRE(partialOverride.find("MaxStdio = 8192") != std::string::npos);
    REQUIRE(partialOverride.find("[Experimental]") != std::string::npos);
    REQUIRE(partialOverride.find("SaveGameMaxSize = false") != std::string::npos);
    REQUIRE(partialOverride.find("[SaveGameMaxSize]") == std::string::npos);
    REQUIRE_FALSE(RfabCompat::PatchEngineFixesTomlContent(partialOverride));

    std::string soulsIni = "[SLOWMOTION]\r\nfSlowMotionMultiplier = 0.250000\r\nbEnableSlowMotionInventoryMenu = true\r\n";
    REQUIRE(RfabCompat::PatchSkyrimSoulsReIniContent(soulsIni));
    REQUIRE(soulsIni.find("fSlowMotionMultiplier = 1.000000") != std::string::npos);
    REQUIRE(soulsIni.find("bEnableSlowMotionInventoryMenu = false") != std::string::npos);

    TiltedPhoques::Map<TiltedPhoques::String, uint8_t> masters;
    ESLoader::TESFile missingPlugin(masters);
    REQUIRE_FALSE(missingPlugin.LoadFile("/tmp/non_existent_plugin_for_noexcept_test.esp"));

    const auto loDir = std::filesystem::temp_directory_path() / "rfab_esloader_600_test";
    std::filesystem::remove_all(loDir);
    std::filesystem::create_directories(loDir / "Data");
    {
        std::ofstream lo(loDir / "Data" / "loadorder.txt", std::ios::binary);
        const char bom[3] = {static_cast<char>(0xEF), static_cast<char>(0xBB), static_cast<char>(0xBF)};
        lo.write(bom, 3);
        lo << "Skyrim.esm\r\n*Update.esm\r\n";
        for (int i = 0; i < 300; ++i)
            lo << "RfabMod_" << i << ".esp\r\n";
        lo << "SkyrimTogether.esp\r\n";
    }
    {
        struct ScopedCurrentPath
        {
            std::filesystem::path Prev;
            ~ScopedCurrentPath() noexcept
            {
                std::error_code ec;
                if (!Prev.empty())
                    std::filesystem::current_path(Prev, ec);
            }
        } cwdGuard{std::filesystem::current_path()};

        std::filesystem::current_path(loDir);
        ESLoader::ESLoader loader;
        (void)loader.BuildRecordCollection();
        const auto& order = loader.GetLoadOrder();
        REQUIRE(order.size() == 303);
        REQUIRE(order.front().m_filename == "Skyrim.esm");
        REQUIRE(order.front().m_standardId == 0x00);
        REQUIRE(order[260].m_isLite == true);
        REQUIRE(order.back().m_filename == "SkyrimTogether.esp");
    }
    std::filesystem::remove_all(loDir);

    // RFAB «Единство в битве (Кооп)» (Co-Op Ally Friendly-Fire Protection) matrix:
    // 1. Non-party actors, enemies, solo NPC followers, and personal summons are untouched
    //    (preserving RFAB's Speechcraft 50 «Лидерство» perk).
    {
        const auto soloHarm = RfabCompat::EvaluateCoopAllyActionPolicy(false, true, false);
        REQUIRE_FALSE(soloHarm.BlockHarmfulAction);
        REQUIRE_FALSE(soloHarm.AllowPartySparring);
        REQUIRE_FALSE(soloHarm.AllowBeneficialMagic);

        const auto soloHeal = RfabCompat::EvaluateCoopAllyActionPolicy(false, true, true);
        REQUIRE_FALSE(soloHeal.BlockHarmfulAction);
        REQUIRE_FALSE(soloHeal.AllowPartySparring);
        REQUIRE_FALSE(soloHeal.AllowBeneficialMagic);
    }
    // 2. Same co-op party with AllyProtection ENABLED (default):
    //    - 100% of physical attacks, hostile spells, shouts, poisons, cloaks, and debuffs are blocked.
    //    - 100% of heals & buffs (including RFAB_List_NoAllyDamage_AllowedSpells) still work!
    {
        const auto allyHarm = RfabCompat::EvaluateCoopAllyActionPolicy(true, true, false);
        REQUIRE(allyHarm.BlockHarmfulAction);
        REQUIRE_FALSE(allyHarm.AllowPartySparring);
        REQUIRE_FALSE(allyHarm.AllowBeneficialMagic);

        const auto allyHealOrBuff = RfabCompat::EvaluateCoopAllyActionPolicy(true, true, true);
        REQUIRE_FALSE(allyHealOrBuff.BlockHarmfulAction);
        REQUIRE_FALSE(allyHealOrBuff.AllowPartySparring);
        REQUIRE(allyHealOrBuff.AllowBeneficialMagic);
    }
    // 3. Same co-op party with AllyProtection DISABLED via Right-Ctrl overlay toggle:
    //    - Party members can spar / test builds without editing STServer.ini, while heals & buffs still work!
    {
        const auto sparHarm = RfabCompat::EvaluateCoopAllyActionPolicy(true, false, false);
        REQUIRE_FALSE(sparHarm.BlockHarmfulAction);
        REQUIRE(sparHarm.AllowPartySparring);
        REQUIRE_FALSE(sparHarm.AllowBeneficialMagic);

        const auto sparHeal = RfabCompat::EvaluateCoopAllyActionPolicy(true, false, true);
        REQUIRE_FALSE(sparHeal.BlockHarmfulAction);
        REQUIRE(sparHeal.AllowPartySparring);
        REQUIRE(sparHeal.AllowBeneficialMagic);
    }

    // 4. Exact RFAB Respect Edition 30-plugin loadorder.txt verification (RFAB.esp at 0x07, SkyrimTogether.esp at 0x10,
    //    SkyrimTogetherQuestPatches.esp at FE:003, SkyrimOutfitSystem.esp at 0x17):
    {
        const auto respectDir = std::filesystem::temp_directory_path() / "rfab_respect_edition_lo_test";
        std::filesystem::remove_all(respectDir);
        std::filesystem::create_directories(respectDir / "Data");

        auto writeMinPlugin = [&](const std::string& fileName, uint32_t tes4Flags)
        {
            std::ofstream out(respectDir / "Data" / fileName, std::ios::binary);
            std::vector<uint8_t> rec(24 + 18, 0);
            std::memcpy(rec.data(), "TES4", 4);
            const uint32_t dataSize = 18;
            std::memcpy(rec.data() + 4, &dataSize, 4);
            std::memcpy(rec.data() + 8, &tes4Flags, 4);
            std::memcpy(rec.data() + 24, "HEDR", 4);
            const uint16_t hedrSize = 12;
            std::memcpy(rec.data() + 28, &hedrSize, 2);
            const float ver = 1.70f;
            std::memcpy(rec.data() + 30, &ver, 4);
            out.write(reinterpret_cast<const char*>(rec.data()), static_cast<std::streamsize>(rec.size()));
        };

        const std::vector<std::pair<std::string, uint32_t>> respectPlugins = {
            {"Skyrim.esm", 0x001u},
            {"Update.esm", 0x001u},
            {"Dawnguard.esm", 0x001u},
            {"HearthFires.esm", 0x001u},
            {"Dragonborn.esm", 0x001u},
            {"Unofficial Skyrim Special Edition Patch.esp", 0x001u},
            {"Skyrim Project Optimization - Full Version.esm", 0x001u},
            {"RFAB.esp", 0x001u}, // ESM-flagged at 0x07 in RFAB Respect Edition!
            {"SkyUI_SE.esp", 0x000u},
            {"SkyHUD.esp", 0x000u},
            {"UIExtensions.esp", 0x000u},
            {"Extended UI.esp", 0x000u},
            {"RaceMenu.esp", 0x000u},
            {"RaceMenuPlugin.esp", 0x000u},
            {"Skyrim Mixed Enhanced Physics - Force and Projectiles.esp", 0x000u},
            {"AHZmoreHUDInventory.esp", 0x200u},
            {"BetterContainerControls.esp", 0x000u},
            {"MCMHelper.esp", 0x200u},
            {"TrueHUD.esl", 0x201u},
            {"SkyrimTogether.esp", 0x000u},
            {"SkyrimTogetherQuestPatches.esp", 0x200u},
            {"UntarnishedUI_Blur.esp", 0x200u},
            {"RFAB WheelMenu.esp", 0x000u},
            {"RFABLoadScreens.esp", 0x000u},
            {"QuickLootIE.esp", 0x200u},
            {"AddItemMenuSE.esp", 0x000u},
            {"RFAB_PerksMenu.esp", 0x000u},
            {"RFAB HF for Gold.esp", 0x000u},
            {"icepenguinworldmapclassic.esp", 0x000u},
            {"SkyrimOutfitSystem.esp", 0x000u},
        };

        {
            std::ofstream lo(respectDir / "Data" / "loadorder.txt", std::ios::binary);
            for (const auto& [name, flags] : respectPlugins)
            {
                writeMinPlugin(name, flags);
                lo << name << "\r\n";
            }
        }

        {
            const auto oldCwd = std::filesystem::current_path();
            std::filesystem::current_path(respectDir);
            ESLoader::ESLoader loader;
            (void)loader.BuildRecordCollection();
            const auto& order = loader.GetLoadOrder();
            REQUIRE(order.size() == 30);
            REQUIRE(order[7].m_filename == "RFAB.esp");
            REQUIRE(order[7].m_standardId == 0x07);
            REQUIRE_FALSE(order[7].m_isLite);
            REQUIRE(order[19].m_filename == "SkyrimTogether.esp");
            REQUIRE(order[19].m_standardId == 0x10);
            REQUIRE_FALSE(order[19].m_isLite);
            REQUIRE(order[20].m_filename == "SkyrimTogetherQuestPatches.esp");
            REQUIRE(order[20].m_isLite);
            REQUIRE(order[20].m_liteId == 0x003);
            REQUIRE(order[29].m_filename == "SkyrimOutfitSystem.esp");
            REQUIRE(order[29].m_standardId == 0x17);
            std::filesystem::current_path(oldCwd);
        }
        std::filesystem::remove_all(respectDir);
    }

    // Live multi-threaded stress test on AnimationGraphDescriptorManager:
    // Verifies that concurrent BehaviorVar::Patch registrations on worker threads never race or
    // invalidate raw const AnimationGraphDescriptor* pointers across hash-table bucket rehashes.
    {
        auto& mgr = AnimationGraphDescriptorManager::Get();
        constexpr uint64_t kAnchorKey = 0x52464142'00000001ull;
        {
            const uint32_t bools[] = {3u, 7u, 11u};
            const uint32_t floats[] = {19u, 23u};
            const uint32_t ints[] = {29u};
            AnimationGraphDescriptor anchor(bools, floats, ints);
            AnimationGraphDescriptorManager::Builder(mgr, kAnchorKey, std::move(anchor));
        }

        const AnimationGraphDescriptor* const pPinnedAnchor = mgr.GetDescriptor(kAnchorKey);
        REQUIRE(pPinnedAnchor != nullptr);
        REQUIRE(pPinnedAnchor->BooleanLookUpTable.size() == 3);
        REQUIRE(pPinnedAnchor->FloatLookupTable.size() == 2);
        REQUIRE(pPinnedAnchor->IntegerLookupTable.size() == 1);

        std::atomic<bool> stopReaders{false};
        std::atomic<bool> pointerMismatch{false};
        std::vector<std::thread> readers;
        for (int r = 0; r < 4; ++r)
        {
            readers.emplace_back([&]() {
                while (!stopReaders.load(std::memory_order_acquire))
                {
                    const AnimationGraphDescriptor* pFound = mgr.GetDescriptor(kAnchorKey);
                    if (pFound != pPinnedAnchor ||
                        pFound->BooleanLookUpTable.size() != 3 ||
                        pFound->BooleanLookUpTable[2] != 11u ||
                        pFound->FloatLookupTable[1] != 23u ||
                        pFound->IntegerLookupTable[0] != 29u)
                    {
                        pointerMismatch.store(true, std::memory_order_release);
                        break;
                    }
                }
            });
        }

        std::vector<std::thread> writers;
        for (int w = 0; w < 2; ++w)
        {
            writers.emplace_back([&, w]() {
                for (uint64_t i = 0; i < 1000; ++i)
                {
                    const uint64_t dynKey = 0x52464142'10000000ull + (static_cast<uint64_t>(w) << 20) + i;
                    const uint32_t bools[] = {static_cast<uint32_t>(i), 42u};
                    const uint32_t floats[] = {100u};
                    const uint32_t ints[] = {200u};
                    AnimationGraphDescriptor desc(bools, floats, ints);
                    AnimationGraphDescriptorManager::Builder(mgr, dynKey, std::move(desc));
                }
            });
        }

        for (auto& t : writers)
            t.join();
        stopReaders.store(true, std::memory_order_release);
        for (auto& t : readers)
            t.join();

        REQUIRE_FALSE(pointerMismatch.load(std::memory_order_acquire));
        REQUIRE(mgr.GetDescriptor(kAnchorKey) == pPinnedAnchor);
        REQUIRE(mgr.GetDescriptors().count(kAnchorKey) == 1);
    }

    // Playtest Combat & Downed Ownership Regression Checks:
    // 1. Verify Master_Behavior, DraugrBehavior, and Falmer_Master_Behavior register synced weapon-speed and combat state variables
    //    within the 64-variable per-type wire limit.
    {
        auto& mgr = AnimationGraphDescriptorManager::Get();
        AnimationGraphDescriptor_Master_Behavior initMaster(mgr);
        AnimationGraphDescriptor_DraugrBehavior initDraugr(mgr);
        AnimationGraphDescriptor_Falmer_Master_Behavior initFalmer(mgr);

        const auto* pMaster = mgr.GetDescriptor(AnimationGraphDescriptor_Master_Behavior::m_key);
        REQUIRE(pMaster != nullptr);
        REQUIRE(pMaster->FloatLookupTable.size() == 16);
        REQUIRE(pMaster->FloatLookupTable.size() <= 64);

        const auto* pDraugr = mgr.GetDescriptor(1556693752012287718ull);
        REQUIRE(pDraugr != nullptr);
        REQUIRE(pDraugr->BooleanLookUpTable.size() == 15);
        REQUIRE(pDraugr->BooleanLookUpTable.size() <= 64);

        const auto* pFalmer = mgr.GetDescriptor(3009402738794250552ull);
        REQUIRE(pFalmer != nullptr);
        REQUIRE(pFalmer->BooleanLookUpTable.size() == 12);
        REQUIRE(pFalmer->IntegerLookupTable.size() == 8);
        REQUIRE(pFalmer->IntegerLookupTable.size() <= 64);
    }

    // 3. Stage 4 (v1.2.0): Co-Op Party Tactical HUD & 3D Ping System audit
    {
        // Round-trip serialization and delimiter injection sanitization
        RfabCompat::TacticalPingPayload src{};
        src.Kind = RfabCompat::TacticalPingKind::EnemyBoss;
        src.SenderPlayerId = 7u;
        src.TargetServerId = 4201u;
        src.WorldSpaceBaseId = 0x3Cu;
        src.CellBaseId = 0x1042u;
        src.PosX = -14250.5f;
        src.PosY = 8920.25f;
        src.PosZ = 1340.75f;
        src.SenderName = "Dragonborn|Injected\nName";
        src.TargetLabel = "Драконий жрец|Морваскар";

        const std::string encoded = RfabCompat::SerializeTacticalPingPayload(src);
        RfabCompat::TacticalPingPayload decoded{};
        REQUIRE(RfabCompat::DeserializeTacticalPingPayload(encoded, decoded));
        REQUIRE(decoded.Kind == RfabCompat::TacticalPingKind::EnemyBoss);
        REQUIRE(decoded.SenderPlayerId == 7u);
        REQUIRE(decoded.TargetServerId == 4201u);
        REQUIRE(decoded.WorldSpaceBaseId == 0x3Cu);
        REQUIRE(decoded.CellBaseId == 0x1042u);
        REQUIRE(std::fabs(decoded.PosX - src.PosX) < 0.05f);
        REQUIRE(std::fabs(decoded.PosY - src.PosY) < 0.05f);
        REQUIRE(std::fabs(decoded.PosZ - src.PosZ) < 0.05f);
        REQUIRE(decoded.SenderName == "Dragonborn Injected Name");
        REQUIRE(decoded.TargetLabel == "Драконий жрец Морваскар");

        // Contextual crosshair ping classification
        REQUIRE(RfabCompat::ClassifyCrosshairTacticalPingKind(true, false, false, true, true, false, 62) ==
                RfabCompat::TacticalPingKind::Danger);
        REQUIRE(RfabCompat::ClassifyCrosshairTacticalPingKind(false, true, true, false, false, false, 62) ==
                RfabCompat::TacticalPingKind::AssistAlly);
        REQUIRE(RfabCompat::ClassifyCrosshairTacticalPingKind(false, false, false, true, true, false, 62) ==
                RfabCompat::TacticalPingKind::EnemyBoss);
        REQUIRE(RfabCompat::ClassifyCrosshairTacticalPingKind(false, false, false, false, true, false, 62) ==
                RfabCompat::TacticalPingKind::Enemy);
        REQUIRE(RfabCompat::ClassifyCrosshairTacticalPingKind(false, false, false, false, false, true, 62) ==
                RfabCompat::TacticalPingKind::Loot);
        REQUIRE(RfabCompat::ClassifyCrosshairTacticalPingKind(false, false, false, false, false, false, 28) ==
                RfabCompat::TacticalPingKind::Loot); // Container
        REQUIRE(RfabCompat::ClassifyCrosshairTacticalPingKind(false, false, false, false, false, false, 24) ==
                RfabCompat::TacticalPingKind::Objective); // Activator / Lever

        // 3D-to-2D screen placement & behind-camera (aDepthZ < 0) inversion guard
        const auto onScreen = RfabCompat::ComputeTacticalMarkerScreenPlacement(0.5f, 0.5f, 0.85f, 1920.0f, 1080.0f, 48.0f);
        REQUIRE(onScreen.IsOnScreen);
        REQUIRE_FALSE(onScreen.IsClampedToEdge);
        REQUIRE(std::fabs(onScreen.ScreenX - 960.0f) < 0.5f);
        REQUIRE(std::fabs(onScreen.ScreenY - 540.0f) < 0.5f);

        // Target behind camera on the right (perspective divide by -W mirrored normX to < 0.5; ComputeTacticalMarkerScreenPlacement must flip back to right edge!)
        const auto behindRight = RfabCompat::ComputeTacticalMarkerScreenPlacement(0.2f, 0.5f, -0.5f, 1920.0f, 1080.0f, 48.0f);
        REQUIRE_FALSE(behindRight.IsOnScreen);
        REQUIRE(behindRight.IsClampedToEdge);
        REQUIRE(behindRight.DirX > 0.9f);
        REQUIRE(std::fabs(behindRight.ScreenX - (1920.0f - 48.0f)) < 1.0f);

        // Chat command parser & HUD mode cycling
        RfabCompat::TacticalChatCommand cmd{};
        REQUIRE(RfabCompat::ParseTacticalChatCommand("/ping boss Драконий жрец", cmd));
        REQUIRE(cmd.Kind == RfabCompat::TacticalChatCommand::Action::TriggerExplicitPing);
        REQUIRE(cmd.PingKind == RfabCompat::TacticalPingKind::EnemyBoss);
        REQUIRE(cmd.CustomLabel == "Драконий жрец");

        REQUIRE(RfabCompat::ParseTacticalChatCommand("/ping clear", cmd));
        REQUIRE(cmd.Kind == RfabCompat::TacticalChatCommand::Action::ClearPings);

        REQUIRE(RfabCompat::ParseTacticalChatCommand("/hud compact", cmd));
        REQUIRE(cmd.Kind == RfabCompat::TacticalChatCommand::Action::SetHudMode);
        REQUIRE(cmd.HudMode == RfabCompat::TacticalHudMode::Compact);

        REQUIRE(RfabCompat::CycleTacticalHudMode(RfabCompat::TacticalHudMode::Full) == RfabCompat::TacticalHudMode::Compact);
        REQUIRE(RfabCompat::CycleTacticalHudMode(RfabCompat::TacticalHudMode::Compact) == RfabCompat::TacticalHudMode::MarkersOnly);
        REQUIRE(RfabCompat::CycleTacticalHudMode(RfabCompat::TacticalHudMode::MarkersOnly) == RfabCompat::TacticalHudMode::Hidden);
        REQUIRE(RfabCompat::CycleTacticalHudMode(RfabCompat::TacticalHudMode::Hidden) == RfabCompat::TacticalHudMode::Full);

        std::string rfabAdapterCpp;
        std::string rfabProgServerCpp;
        std::string overlayClientCpp;
        REQUIRE(RfabCompat::Detail::ReadTextFile("Code/client/Services/RfabAdapterService.cpp", rfabAdapterCpp));
        REQUIRE(RfabCompat::Detail::ReadTextFile("Code/server/Services/RfabProgressionService.cpp", rfabProgServerCpp));
        REQUIRE(RfabCompat::Detail::ReadTextFile("Code/client/Services/Generic/OverlayClient.cpp", overlayClientCpp));
        REQUIRE(rfabAdapterCpp.find("OnDrawTacticalHud") != std::string::npos);
        REQUIRE(rfabAdapterCpp.find("TacticalPingSync") != std::string::npos);
        REQUIRE(rfabAdapterCpp.find("ComputeTacticalMarkerScreenPlacement") != std::string::npos);
        REQUIRE(rfabProgServerCpp.find("TacticalPingSync") != std::string::npos);
        REQUIRE(overlayClientCpp.find("ParseTacticalChatCommand") != std::string::npos);
    }

    // 19. Stage 5: World Containers, Locks, Shared Keys/Claws, Projectiles/Aggro & Snapshot Rollback
    {
        // Shared dungeon keys & Dragon Claws vs excluded items
        REQUIRE(RfabCompat::IsSharedCoopKeyOrClaw("BleakFallsBarrowKey", 45, 0x000A9636, false));
        REQUIRE_FALSE(RfabCompat::IsSharedCoopKeyOrClaw("WhiterunBreezehomeKey", 45, 0x00093B08, false));
        REQUIRE(RfabCompat::IsSharedCoopKeyOrClaw("WhiterunBreezehomeKey", 45, 0x00093B08, true));

        REQUIRE(RfabCompat::IsSharedCoopKeyOrClaw("MS13GoldenClaw", 32, 0x000399E7, false));
        REQUIRE(RfabCompat::IsSharedCoopKeyOrClaw("dunFolgunthurIvoryDragonClaw", 32, 0x000AB7BB, false));
        REQUIRE(RfabCompat::IsSharedCoopKeyOrClaw("MQ103Dragonstone", 32, 0x000DF202, false));
        REQUIRE(RfabCompat::IsSharedCoopKeyOrClaw("DA04AttunementSphere", 32, 0x0003532C, false));
        REQUIRE(RfabCompat::IsSharedCoopKeyOrClaw("MQ205ElderScrollDragon", 27, 0x0002D513, false));

        REQUIRE(RfabCompat::IsExcludedFromQuestItemReplication("Gold001", 32, 0x0000000F));
        REQUIRE(RfabCompat::IsExcludedFromQuestItemReplication("Lockpick", 32, 0x0000000A));
        REQUIRE(RfabCompat::IsExcludedFromQuestItemReplication("RFAB_XPBook_Small", 27, 0x07123456));
        REQUIRE(RfabCompat::IsExcludedFromQuestItemReplication("PotionRestoreHealth01", 46, 0x0003EADD));
        REQUIRE_FALSE(RfabCompat::IsSharedCoopKeyOrClaw("RFAB_XPBook_Small", 27, 0x07123456, false));

        // Co-Op Smart Threat / Aggro Switching policy
        RfabCompat::CoopAggroDecisionInput aggro{};
        aggro.IsHitteeValidNpc = true;
        aggro.IsHitteeRemote = false;
        aggro.IsHitteeDead = false;
        aggro.IsHitterPlayer = true;
        aggro.IsHitterDowned = false;
        aggro.HasActiveTargetLock = false;
        aggro.IsHitteeInCombat = true;
        aggro.HasValidCurrentTarget = true;
        aggro.IsCurrentTargetDeadOrDowned = false;
        aggro.DistToCurrentTarget = 950.0f; // Host Archer/Mage at range
        aggro.DistToAttacker = 180.0f;      // Co-Op Tank in melee range
        REQUIRE(RfabCompat::ShouldSwitchCoopCombatTarget(aggro));

        // Respect active target lock when current target is alive and standing
        aggro.HasActiveTargetLock = true;
        REQUIRE_FALSE(RfabCompat::ShouldSwitchCoopCombatTarget(aggro));

        // Break target lock immediately if current target goes downed
        aggro.IsCurrentTargetDeadOrDowned = true;
        REQUIRE(RfabCompat::ShouldSwitchCoopCombatTarget(aggro));

        // Never switch aggro onto a downed attacker
        aggro.IsHitterDowned = true;
        REQUIRE_FALSE(RfabCompat::ShouldSwitchCoopCombatTarget(aggro));

        // Projectile parameter safety
        REQUIRE(RfabCompat::AreProjectileLaunchParamsSafe(1024.0f, -2048.0f, 512.0f, 0.1f, 0.0f, 1.57f, 1.0f, 1.0f));
        REQUIRE_FALSE(RfabCompat::AreProjectileLaunchParamsSafe(
            std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f));
        REQUIRE_FALSE(RfabCompat::AreProjectileLaunchParamsSafe(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 1.0f));

        // Static source audit for Stage 5 fixes
        std::string invStructCpp;
        std::string refrCpp;
        std::string clientObjCpp;
        std::string serverObjCpp;
        std::string clientCombatCpp;
        std::string saveSnapCpp;
        REQUIRE(RfabCompat::Detail::ReadTextFile("Code/encoding/Structs/Inventory.cpp", invStructCpp));
        REQUIRE(RfabCompat::Detail::ReadTextFile("Code/client/Games/Skyrim/TESObjectREFR.cpp", refrCpp));
        REQUIRE(RfabCompat::Detail::ReadTextFile("Code/client/Services/Generic/ObjectService.cpp", clientObjCpp));
        REQUIRE(RfabCompat::Detail::ReadTextFile("Code/server/Services/ObjectService.cpp", serverObjCpp));
        REQUIRE(RfabCompat::Detail::ReadTextFile("Code/client/Services/Generic/CombatService.cpp", clientCombatCpp));
        REQUIRE(RfabCompat::Detail::ReadTextFile("Code/server/Services/SaveSnapshotService.cpp", saveSnapCpp));

        REQUIRE(invStructCpp.find("remainingToRemove") != std::string::npos);
        REQUIRE(refrCpp.find("RemoveItem(pObject, -arEntry.Count, ITEM_REMOVE_REASON::kRemove, nullptr, nullptr)") != std::string::npos);
        REQUIRE(refrCpp.find("IsSharedCoopKeyOrClaw") != std::string::npos);
        REQUIRE(refrCpp.find("wasLockedBefore") != std::string::npos);
        REQUIRE(clientObjCpp.find("if (pLock->IsLocked())") != std::string::npos);
        REQUIRE(clientObjCpp.find("if (pLock->flags & 0xFF)") == std::string::npos);
        REQUIRE(serverObjCpp.find("IsInRange(acMessage.pPlayer->GetCellComponent(), false)") != std::string::npos);
        REQUIRE(clientCombatCpp.find("ShouldSwitchCoopCombatTarget") != std::string::npos);
        REQUIRE(saveSnapCpp.find("TrackedObjects") != std::string::npos);
    }

    // 20. Stage 6: World Systems, Teleportation, Calendar/Weather, Beast Forms & Reconnect Polish
    {
        // Lossless Skyrim GameHour encode/decode across all 24 * 60 minute combinations (including 23:59!)
        for (int h = 0; h < 24; ++h)
        {
            for (int m = 0; m < 60; ++m)
            {
                const float gameHour = RfabCompat::EncodeSkyrimGameHour(h, m);
                REQUIRE(gameHour >= 0.0f);
                REQUIRE(gameHour < 24.0f);

                int decH = -1;
                int decM = -1;
                RfabCompat::DecodeSkyrimGameHour(gameHour, decH, decM);
                REQUIRE(decH == h);
                REQUIRE(decM == m);
            }
        }

        // Load-order independent Beast Form race recognition
        REQUIRE(RfabCompat::IsBeastFormRace("WerewolfBeastRace", 0x000CDD84u));
        REQUIRE(RfabCompat::IsBeastFormRace("DLC1VampireBeastRace", 0x0200283Au));
        REQUIRE(RfabCompat::IsBeastFormRace("DLC1VampireBeastRace", 0x0400283Au)); // Dawnguard at load index 04
        REQUIRE(RfabCompat::IsBeastFormRace("DLC2WerebearBeastRace", 0x03023FE9u));
        REQUIRE(RfabCompat::IsBeastFormRace("RFAB_CustomLichRace", 0x07123456u));
        REQUIRE_FALSE(RfabCompat::IsBeastFormRace("NordRace", 0x00013746u));
        REQUIRE_FALSE(RfabCompat::IsBeastFormRace("NordRaceVampire", 0x00088794u));

        // Safe Teleport Destination validation (blocks downed/loading, NaN/Inf, and temp 0xFFxxxxxx cells without WorldSpace)
        REQUIRE(RfabCompat::CanResolveTeleportDestination(true, false, false, 100.0f, 200.0f, 50.0f, false, false));
        REQUIRE(RfabCompat::CanResolveTeleportDestination(true, true, true, 12000.0f, -8000.0f, 350.0f, false, false));
        REQUIRE_FALSE(RfabCompat::CanResolveTeleportDestination(true, true, false, 12000.0f, -8000.0f, 350.0f, false, false));
        REQUIRE_FALSE(RfabCompat::CanResolveTeleportDestination(false, false, false, 0.0f, 0.0f, 0.0f, false, false));
        REQUIRE_FALSE(RfabCompat::CanResolveTeleportDestination(true, false, true, 100.0f, 200.0f, 50.0f, true, false));
        REQUIRE_FALSE(RfabCompat::CanResolveTeleportDestination(true, false, true, 100.0f, 200.0f, 50.0f, false, true));
        REQUIRE_FALSE(RfabCompat::CanResolveTeleportDestination(
            true, false, true, std::numeric_limits<float>::quiet_NaN(), 200.0f, 50.0f, false, false));

        // Static source audit for Stage 6 fixes
        std::string serverCalendarCpp;
        std::string serverCommandCpp;
        std::string clientOverlayCpp;
        std::string clientCommandCpp;
        std::string clientPlayerCpp;
        std::string clientWeatherCpp;
        std::string clientMapCpp;
        std::string coordSaveCpp;
        REQUIRE(RfabCompat::Detail::ReadTextFile("Code/server/Services/CalendarService.cpp", serverCalendarCpp));
        REQUIRE(RfabCompat::Detail::ReadTextFile("Code/server/Services/CommandService.cpp", serverCommandCpp));
        REQUIRE(RfabCompat::Detail::ReadTextFile("Code/client/Services/Generic/OverlayService.cpp", clientOverlayCpp));
        REQUIRE(RfabCompat::Detail::ReadTextFile("Code/client/Services/Generic/CommandService.cpp", clientCommandCpp));
        REQUIRE(RfabCompat::Detail::ReadTextFile("Code/client/Services/Generic/PlayerService.cpp", clientPlayerCpp));
        REQUIRE(RfabCompat::Detail::ReadTextFile("Code/client/Services/Generic/WeatherService.cpp", clientWeatherCpp));
        REQUIRE(RfabCompat::Detail::ReadTextFile("Code/client/Services/Generic/MapService.cpp", clientMapCpp));
        REQUIRE(RfabCompat::Detail::ReadTextFile("Code/client/Services/CoordinatedSaveService.cpp", coordSaveCpp));

        REQUIRE(serverCalendarCpp.find("EncodeSkyrimGameHour") != std::string::npos);
        REQUIRE(serverCalendarCpp.find("DecodeSkyrimGameHour") != std::string::npos);
        REQUIRE(serverCommandCpp.find("IsPlayerAuthorizedHost") != std::string::npos);
        REQUIRE(clientOverlayCpp.find("CanResolveTeleportDestination") != std::string::npos);
        REQUIRE(clientCommandCpp.find("CanResolveTeleportDestination") != std::string::npos);
        REQUIRE(clientPlayerCpp.find("IsBeastFormRace") != std::string::npos);
        REQUIRE(clientWeatherCpp.find("if (Sky* pSky = Sky::Get())") != std::string::npos);
        REQUIRE(clientWeatherCpp.find("CanMutateOutdoorSkyWeather") != std::string::npos);
        REQUIRE(clientWeatherCpp.find("BSFixedString(\"StatsMenu\")") != std::string::npos);
        REQUIRE(clientWeatherCpp.find("BSFixedString(\"CustomMenu\")") != std::string::npos);
        REQUIRE(clientMapCpp.find("if (!pLocation || (!Cast<TESWorldSpace>(pLocation) && !Cast<TESObjectCELL>(pLocation)))") != std::string::npos);
        REQUIRE(coordSaveCpp.find("!pPlayer->IsStillLoading()") != std::string::npos);

        // v1.2.0-hotfix.1 Skills-CTD, MO2 gamePath, Render-Thread Snapshot & Cross-Version RFAB tests:
        REQUIRE(std::filesystem::exists("docs/V1.2.0_RELEASE_BACKUP_MANIFEST.json"));

        std::string soulsStats = "[UNPAUSED_MENUS]\r\nbStatsMenu = true\r\n[SLOWMOTION]\r\nfSlowMotionMultiplier = 0.5\r\n";
        REQUIRE(RfabCompat::PatchSkyrimSoulsReIniContent(soulsStats));
        REQUIRE(soulsStats.find("bStatsMenu = false") != std::string::npos);
        REQUIRE(soulsStats.find("fSlowMotionMultiplier = 1.000000") != std::string::npos);

        REQUIRE(RfabCompat::DecodeMo2IniValueUtf8("@ByteArray(..\\\\)") == "..\\");
        REQUIRE(RfabCompat::DecodeMo2IniValueUtf8("@ByteArray(D:\\\\\\xd0\\x98\\xd0\\xb3\\xd1\\x80\\xd1\\x8b\\\\RFAB)") == "D:\\Игры\\RFAB");
        REQUIRE(RfabCompat::DecodeMo2IniValueUtf8("D:/\\x0418\\x0433\\x0440\\x044b/RFAB") == "D:/Игры/RFAB");

        std::string rfabAdapterCpp;
        REQUIRE(RfabCompat::Detail::ReadTextFile("Code/client/Services/RfabAdapterService.cpp", rfabAdapterCpp));
        const size_t drawPos = rfabAdapterCpp.find("void RfabAdapterService::OnDrawTacticalHud() noexcept");
        REQUIRE(drawPos != std::string::npos);
        const std::string drawBody = rfabAdapterCpp.substr(drawPos);
        REQUIRE(drawBody.find("m_tacticalHudSnapshotMutex") != std::string::npos);
        REQUIRE(drawBody.find("BSFixedString") == std::string::npos);
        REQUIRE(drawBody.find("GetMenuOpen") == std::string::npos);
        REQUIRE(drawBody.find("m_world.view") == std::string::npos);
        REQUIRE(drawBody.find("TESForm::GetById") == std::string::npos);
        REQUIRE(drawBody.find("WorldPtToScreenPt3") == std::string::npos);

        std::string fileMappingCpp;
        REQUIRE(RfabCompat::Detail::ReadTextFile("Code/immersive_launcher/stubs/FileMapping.cpp", fileMappingCpp));
        REQUIRE(fileMappingCpp.find("DllName->Length / sizeof(wchar_t)") != std::string::npos);

        // Live-test Fixes #1, #2, #3 regression guards:
        // 1) Save+Load mutual player visibility, DiscoveryService reset & GNS lingering-handle reconnect guard
        std::string discoveryCpp;
        std::string clientCharCpp;
        std::string serverCharCpp;
        std::string cellCpp;
        std::string skillsMenuCpp;
        std::string patchSubmodulesPy;
        REQUIRE(RfabCompat::Detail::ReadTextFile("Code/client/Services/Generic/DiscoveryService.cpp", discoveryCpp));
        REQUIRE(RfabCompat::Detail::ReadTextFile("Code/client/Services/Generic/CharacterService.cpp", clientCharCpp));
        REQUIRE(RfabCompat::Detail::ReadTextFile("Code/server/Services/CharacterService.cpp", serverCharCpp));
        REQUIRE(RfabCompat::Detail::ReadTextFile("Code/client/Games/Skyrim/Forms/TESObjectCELL.cpp", cellCpp));
        REQUIRE(RfabCompat::Detail::ReadTextFile("Code/client/Games/Skyrim/Interface/Menus/SkillsMenu.cpp", skillsMenuCpp));
        REQUIRE(RfabCompat::Detail::ReadTextFile("Tools/rfab/patch_submodules.py", patchSubmodulesPy));

        REQUIRE(discoveryCpp.find("void DiscoveryService::ResetLoadedForms() noexcept") != std::string::npos);
        REQUIRE(discoveryCpp.find("void DiscoveryService::ReconcileMissingWorldEntities() noexcept") != std::string::npos);
        // v4.1.0: Discovery resets its loaded forms as a lifecycle participant on every world-load begin.
        {
            const auto beginPos = discoveryCpp.find("void DiscoveryService::OnWorldLoadBegin(");
            const auto endPos = discoveryCpp.find("void DiscoveryService::OnWorldLoadEnd(");
            REQUIRE(beginPos != std::string::npos);
            REQUIRE(endPos != std::string::npos);
            REQUIRE(beginPos < endPos);
            REQUIRE(discoveryCpp.substr(beginPos, endPos - beginPos).find("ResetLoadedForms();") != std::string::npos);
            REQUIRE(coordSaveCpp.find("m_world.GetLifecycle().OnWorldLoadBegin(") != std::string::npos);
        }
        REQUIRE(coordSaveCpp.find("pPlayer->parentCell != nullptr") != std::string::npos);
        REQUIRE(coordSaveCpp.find("m_autoReconnectAttemptsLeft") != std::string::npos);
        REQUIRE(clientCharCpp.find("void CharacterService::EnsureLocalPlayerRegistered() noexcept") != std::string::npos);
        REQUIRE(serverCharCpp.find("ReconcileAllEntitiesForObserver(acMessage.pPlayer)") != std::string::npos);
        REQUIRE(serverCharCpp.find("m_interestRegistry.IsMarkedInRange(acMessage.pPlayer->GetId(), otherServerId)") != std::string::npos);
        REQUIRE(patchSubmodulesPy.find("apInfo->m_hConn != m_connection") != std::string::npos);

        // 2) SkyrimSE.exe+0x23D000 (SE_ID 17859) Teleport CTD guard: MoveActor never invokes TESWorldSpace::LoadCell
        REQUIRE(clientCharCpp.find("pWorldSpace->LoadCell") == std::string::npos);
        REQUIRE(clientCharCpp.find("GetCellFromCoordinates(coordinates.X, coordinates.Y, pWorldSpace, false)") != std::string::npos);
        REQUIRE(serverCharCpp.find("SendToPlayersInRange(notify, cEntity, acMessage.pPlayer)") != std::string::npos);
        REQUIRE(cellCpp.find("SafeGetWorldSpaceOfExteriorCell") != std::string::npos);
        REQUIRE(cellCpp.find("17859") != std::string::npos);

        // 3) Update-resilient RFAB.dll PerkPlacementMenuOpenCloseManager bounds scanner + trampoline builder
        REQUIRE(skillsMenuCpp.find("ApplyRfabDllPerkPlacementBoundsFixPostSkse") != std::string::npos);
        {
            // Exact byte slice of RFAB.dll PerkPlacement loop placed at an arbitrary shifted RVA (+0x1234):
            std::vector<uint8_t> fakeText(0x2000, 0xCC);
            const size_t baseOff = 0x1234;
            const uint8_t unpatchedLoop[] = {
                0x33, 0xDB,                                     // xor ebx, ebx
                0x48, 0x8B, 0x90, 0x18, 0x01, 0x00, 0x00,       // mov rdx, [rax+0x118]  (head = baseOff + 2)
                0x48, 0x85, 0xD2,                               // test rdx, rdx         (resume = head + 7)
                0x74, 0x1F,                                     // je skip (+0x1F)
                0x83, 0x7A, 0x20, 0x00,                         // cmp dword ptr [rdx+0x20], 0
                0x74, 0x19,                                     // je skip
                // 25 bytes of inner loop body (`lea rcx, [rdx+0x10]; call copy_ctor; ...`)
                0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
                0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
                0x90, 0x90, 0x90, 0x90, 0x90,
                0x48, 0xFF, 0xC3,                               // inc rbx               (skip = head + 0x2B)
                0x48, 0x8B, 0x44, 0xDE, 0x08,                   // mov rax, [rsi+rbx*8+8]
                0x48, 0x85, 0xC0,                               // test rax, rax
                0x75, 0xC8,                                     // jne head              (exit = skip + 13)
                0x48, 0x8B, 0x4F, 0x08                          // mov rcx, [rdi+8]
            };
            std::memcpy(fakeText.data() + baseOff, unpatchedLoop, sizeof(unpatchedLoop));

            const auto scan = RfabCompat::LocateRfabPerkPlacementLoop(fakeText.data(), fakeText.size());
            REQUIRE(scan.State == RfabCompat::RfabPerkLoopScanResult::Status::FoundUnpatched);
            REQUIRE(scan.LoopHeadOffset == baseOff + 2);
            REQUIRE(scan.ResumeOffset == baseOff + 9);
            REQUIRE(scan.SkipOffset == baseOff + 2 + 0x2B);
            REQUIRE(scan.ExitOffset == baseOff + 2 + 0x2B + 13);

            uint8_t cave[RfabCompat::kRfabPerkTrampolineSize]{};
            const uintptr_t moduleBase = 0x180000000ULL;
            const uintptr_t caveAddr = 0x180250000ULL;
            REQUIRE(RfabCompat::BuildRfabPerkPlacementTrampoline(
                caveAddr,
                moduleBase + scan.ResumeOffset,
                moduleBase + scan.SkipOffset,
                moduleBase + scan.ExitOffset,
                cave));

            auto readRel32Target = [&](size_t dispOffset, size_t nextIpOffset) -> uintptr_t
            {
                int32_t d32 = 0;
                std::memcpy(&d32, cave + dispOffset, sizeof(d32));
                return static_cast<uintptr_t>(static_cast<intptr_t>(caveAddr + nextIpOffset) + static_cast<intptr_t>(d32));
            };
            REQUIRE(readRel32Target(9,  13) == moduleBase + scan.ExitOffset);
            REQUIRE(readRel32Target(19, 23) == moduleBase + scan.SkipOffset);
            REQUIRE(readRel32Target(35, 39) == moduleBase + scan.SkipOffset);
            REQUIRE(readRel32Target(46, 50) == moduleBase + scan.SkipOffset);
            REQUIRE(readRel32Target(51, 55) == moduleBase + scan.ResumeOffset);

            // Simulate user's disk-patched RFAB.dll (`E9 <rel32> 90 90` at loopHead) -> must detect AlreadyPatched
            fakeText[scan.LoopHeadOffset] = 0xE9;
            fakeText[scan.LoopHeadOffset + 5] = 0x90;
            fakeText[scan.LoopHeadOffset + 6] = 0x90;
            const auto scanDiskPatched = RfabCompat::LocateRfabPerkPlacementLoop(fakeText.data(), fakeText.size());
            REQUIRE(scanDiskPatched.State == RfabCompat::RfabPerkLoopScanResult::Status::AlreadyPatched);
        }
    }
}
