#include <catch2/catch.hpp>

#include <Authority/ActorValues.h>
#include <Authority/Health.h>
#include <RfabModpackCompat.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

namespace
{
std::filesystem::path Utf8SubPath(const std::string& acUtf8)
{
    std::u8string u8(reinterpret_cast<const char8_t*>(acUtf8.data()), acUtf8.size());
    return std::filesystem::path(u8);
}

void WriteDummyValidEss(const std::filesystem::path& acPath)
{
    std::filesystem::create_directories(acPath.parent_path());
    std::ofstream out(acPath, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());

    auto putU16 = [](std::string& dst, uint16_t v) {
        dst.push_back(static_cast<char>(v & 0xFFu));
        dst.push_back(static_cast<char>((v >> 8) & 0xFFu));
    };
    auto putU32 = [](std::string& dst, uint32_t v) {
        dst.push_back(static_cast<char>(v & 0xFFu));
        dst.push_back(static_cast<char>((v >> 8) & 0xFFu));
        dst.push_back(static_cast<char>((v >> 16) & 0xFFu));
        dst.push_back(static_cast<char>((v >> 24) & 0xFFu));
    };
    auto putWStr = [&](std::string& dst, std::string_view s) {
        putU16(dst, static_cast<uint16_t>(s.size()));
        dst.append(s.data(), s.size());
    };

    std::string header;
    putU32(header, 12u); // Skyrim SE save version
    putU32(header, 1u);  // saveNumber
    putWStr(header, "Dovahkiin");
    putU32(header, 1u); // playerLevel
    putWStr(header, "Whiterun");
    putWStr(header, "000.00.01");
    putWStr(header, "NordRace");
    putU16(header, 0u);  // sex
    putU32(header, 0u);  // curExp
    putU32(header, 100u); // lvlExp
    putU32(header, 0u);  // filetime lo
    putU32(header, 0u);  // filetime hi
    putU32(header, 1u);  // screenshot width
    putU32(header, 1u);  // screenshot height
    putU16(header, 0u);  // compressionType (uncompressed)

    std::string payload("TESV_SAVEGAME", 13);
    putU32(payload, static_cast<uint32_t>(header.size()));
    payload.append(header);
    payload.append(4u, '\0');   // 1x1 RGBA screenshot
    payload.append(256u, '\0'); // formVersion + plugin table + body padding
    out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    out.flush();
    REQUIRE(out.good());
}
} // namespace

TEST_CASE("P1.2 ActorValues constants & remote apply mode prevent armor/resist compounding", "[rfab][v411][p12][av]")
{
    CHECK(Authority::kAvHealth == 24u);
    CHECK(Authority::kAvVoicePoints == 37u);
    CHECK(Authority::kAvDamageResist == 39u);
    CHECK(Authority::kAvMagicResist == 44u);
    CHECK(Authority::kAvInfamy == 61u);
    CHECK(Authority::kAvInfamy != Authority::kAvVoicePoints);
    CHECK(Authority::kAvAbsorbChance == 83u);
    CHECK(Authority::kAvCount == 164u);

    for (uint32_t av = Authority::kAvDamageResist; av <= Authority::kAvMagicResist; ++av)
    {
        CHECK(Authority::IsSyncedDefenseResistanceAv(av));
        CHECK(Authority::ShouldAcceptServerActorValue(av));
        CHECK(Authority::ClassifyRemoteActorValueApplyMode(av, false) ==
              Authority::RemoteActorValueApplyMode::kTemporaryTotalDelta);
        CHECK(Authority::ClassifyRemoteActorValueApplyMode(av, true) ==
              Authority::RemoteActorValueApplyMode::kSkip);
    }

    CHECK(Authority::IsSyncedDefenseResistanceAv(Authority::kAvAbsorbChance));
    CHECK(Authority::ShouldAcceptServerActorValue(Authority::kAvAbsorbChance));
    CHECK(Authority::ClassifyRemoteActorValueApplyMode(Authority::kAvAbsorbChance, false) ==
          Authority::RemoteActorValueApplyMode::kTemporaryTotalDelta);
    CHECK(Authority::ClassifyRemoteActorValueApplyMode(Authority::kAvAbsorbChance, true) ==
          Authority::RemoteActorValueApplyMode::kSkip);

    CHECK_FALSE(Authority::IsSyncedDefenseResistanceAv(Authority::kAvHealth));
    CHECK(Authority::ShouldAcceptServerActorValue(Authority::kAvHealth));
    CHECK(Authority::ClassifyRemoteActorValueApplyMode(Authority::kAvHealth, false) ==
          Authority::RemoteActorValueApplyMode::kHealthSpecial);
    CHECK(Authority::ClassifyRemoteActorValueApplyMode(Authority::kAvHealth, true) ==
          Authority::RemoteActorValueApplyMode::kHealthSpecial);

    // kAvMagicka uses normal DamageDelta / PermanentDelta channels.
    CHECK(Authority::ClassifyRemoteActorValueApplyMode(Authority::kAvMagicka, false) ==
          Authority::RemoteActorValueApplyMode::kDamageDelta);
    CHECK(Authority::ClassifyRemoteActorValueApplyMode(Authority::kAvMagicka, true) ==
          Authority::RemoteActorValueApplyMode::kPermanentDelta);

    // kAvInfamy (61) is stripped on server and skipped on remote apply; kAvVoicePoints (37) is accepted!
    CHECK_FALSE(Authority::ShouldAcceptServerActorValue(Authority::kAvInfamy));
    CHECK(Authority::ShouldAcceptServerActorValue(Authority::kAvVoicePoints));
    CHECK(Authority::ClassifyRemoteActorValueApplyMode(Authority::kAvInfamy, false) ==
          Authority::RemoteActorValueApplyMode::kSkip);
    CHECK(Authority::ClassifyRemoteActorValueApplyMode(Authority::kAvInfamy, true) ==
          Authority::RemoteActorValueApplyMode::kSkip);
    CHECK(Authority::ClassifyRemoteActorValueApplyMode(Authority::kAvVoicePoints, false) ==
          Authority::RemoteActorValueApplyMode::kDamageDelta);

    CHECK_FALSE(Authority::ShouldAcceptServerActorValue(Authority::kAvCount));
    CHECK(Authority::ClassifyRemoteActorValueApplyMode(Authority::kAvCount, false) ==
          Authority::RemoteActorValueApplyMode::kSkip);
    CHECK(Authority::ClassifyRemoteActorValueApplyMode(999u, true) ==
          Authority::RemoteActorValueApplyMode::kSkip);

    // Simulate repeated remote armor syncs: effective armor = base + equip + tempMod.
    // Must converge to 450 without compounding to 900, 1350, ... across repeated updates.
    float baseArmor = 0.0f;
    float equipArmor = 300.0f;
    float tempMod = 0.0f;
    for (int i = 0; i < 50; ++i)
    {
        const float currentTotal = baseArmor + equipArmor + tempMod;
        const float delta = Authority::ComputeTemporaryTotalDelta(currentTotal, 450.0f);
        tempMod += delta;
        CHECK(baseArmor + equipArmor + tempMod == Approx(450.0f).margin(1e-3f));
    }
    // Now target unequipped armor -> drops cleanly back to 0.
    tempMod += Authority::ComputeTemporaryTotalDelta(baseArmor + equipArmor + tempMod, 0.0f);
    CHECK(baseArmor + equipArmor + tempMod == Approx(0.0f).margin(1e-3f));

    // Non-finite values are rejected as zero delta.
    CHECK(Authority::ComputeTemporaryTotalDelta(100.0f, std::numeric_limits<float>::quiet_NaN()) == 0.0f);
    CHECK(Authority::ComputeTemporaryTotalDelta(std::numeric_limits<float>::infinity(), 100.0f) == 0.0f);
}

TEST_CASE("P1.3 UTF-8 and CP1251 save slot resolution handles Cyrillic Skyrim save names", "[rfab][v411][p13][save]")
{
    using namespace RfabCompat::Detail;

    CHECK(IsValidUtf8("RFAB_MP_S1234_Auto_0"));
    CHECK(IsValidUtf8("\xD0\xA1\xD0\xBE\xD1\x85\xD1\x80\xD0\xB0\xD0\xBD\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB5")); // "Сохранение"
    CHECK_FALSE(IsValidUtf8("\xD1\xEE\xF5\xF0\xE0\xED\xE5\xED\xE8\xE5")); // CP1251 "Сохранение"
    CHECK_FALSE(IsValidUtf8("\xC0\xAF"));                                 // overlong '/'
    CHECK_FALSE(IsValidUtf8("\xED\xA0\x80"));                             // UTF-16 high surrogate

    // CP1251: "Сохранение Ёлка - Ярослав"
    const std::string cp1251Sample = "\xD1\xEE\xF5\xF0\xE0\xED\xE5\xED\xE8\xE5 \xA8\xEB\xEA\xE0 - \xDF\xF0\xEE\xF1\xEB\xE0\xE2";
    const std::string expectedUtf8 =
        "\xD0\xA1\xD0\xBE\xD1\x85\xD1\x80\xD0\xB0\xD0\xBD\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB5 "
        "\xD0\x81\xD0\xBB\xD0\xBA\xD0\xB0 - "
        "\xD0\xAF\xD1\x80\xD0\xBE\xD1\x81\xD0\xBB\xD0\xB0\xD0\xB2";
    CHECK(ConvertCp1251ToUtf8(cp1251Sample) == expectedUtf8);
    CHECK(IsValidUtf8(ConvertCp1251ToUtf8(cp1251Sample)));

    // Case-folding ASCII + Cyrillic (including Ё/ё and А..Я/а..я).
    const std::string upperCyrillic =
        "\xD0\xA1\xD0\x9E\xD0\xA5\xD0\xA0\xD0\x90\xD0\x9D\xD0\x95\xD0\x9D\xD0\x98\xD0\x95 "
        "\xD0\x81\xD0\x9B\xD0\x9A\xD0\x90 - \xD0\xAF\xD0\xA0\xD0\x9E\xD0\xA1\xD0\x9B\xD0\x90\xD0\x92.ESS";
    const std::string lowerCyrillic =
        "\xD1\x81\xD0\xBE\xD1\x85\xD1\x80\xD0\xB0\xD0\xBD\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB5 "
        "\xD1\x91\xD0\xBB\xD0\xBA\xD0\xB0 - \xD1\x8F\xD1\x80\xD0\xBE\xD1\x81\xD0\xBB\xD0\xB0\xD0\xB2";
    CHECK(FoldSaveStemUtf8Lower(upperCyrillic) == lowerCyrillic);
    CHECK(FoldSaveStemUtf8Lower(expectedUtf8) == lowerCyrillic);

    // Real filesystem round-trip in a unique temp directory.
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path tempDir =
        std::filesystem::temp_directory_path() / ("rfab_p13_save_test_" + std::to_string(nonce));
    std::error_code ec;
    std::filesystem::remove_all(tempDir, ec);
    std::filesystem::create_directories(tempDir, ec);
    REQUIRE_FALSE(ec);

    const std::filesystem::path rootSave = tempDir / Utf8SubPath(expectedUtf8 + ".ess");
    const std::string subSaveStemUtf8 = "\xD0\x90\xD0\xB2\xD1\x82\xD0\xBE_\xD0\x81\xD0\xBB\xD0\xBA\xD0\xB0_01";
    const std::filesystem::path subSave = tempDir / "CharProfile" / Utf8SubPath(subSaveStemUtf8 + ".ess");
    WriteDummyValidEss(rootSave);
    WriteDummyValidEss(subSave);

    // What matters is that the lookup names the same FILE. NTFS is case-insensitive, so a lookup
    // may come back spelled the way it was asked for; comparing paths with == failed on Windows for
    // that reason alone. (And a failed == makes Catch print both paths, which throws on a system
    // whose ANSI code page has no Cyrillic - the first Windows run of this suite.)
    const auto sameFile = [](const std::filesystem::path& acFound, const std::filesystem::path& acExpected) {
        std::error_code compareEc;
        return !acFound.empty() && std::filesystem::equivalent(acFound, acExpected, compareEc) && !compareEc;
    };
    CHECK(sameFile(ResolveExistingEssSlotPath(tempDir, expectedUtf8), rootSave));
    CHECK(sameFile(ResolveExistingEssSlotPath(tempDir, "Saves\\" + expectedUtf8 + ".ESS"), rootSave));
    CHECK(sameFile(ResolveExistingEssSlotPath(tempDir, upperCyrillic), rootSave));
    CHECK(sameFile(ResolveExistingEssSlotPath(tempDir, cp1251Sample), rootSave));
    CHECK(sameFile(ResolveExistingEssSlotPath(tempDir, "nonmatching_raw", expectedUtf8), rootSave));
    CHECK(sameFile(ResolveExistingEssSlotPath(tempDir, subSaveStemUtf8), subSave));
    CHECK_FALSE(sameFile(ResolveExistingEssSlotPath(tempDir, subSaveStemUtf8), rootSave));
    CHECK(IsValidEssFileOnDisk(ResolveExistingEssSlotPath(tempDir, cp1251Sample)));
    CHECK(ResolveExistingEssSlotPath(tempDir, "DoesNotExist_Slot").empty());

    std::filesystem::remove_all(tempDir, ec);
}

TEST_CASE("P2.2 enemy health scaling skips peaceful town NPCs while preserving hostile/boss scaling", "[rfab][v411][p22][scaling]")
{
    // 5-arg call preserves backward compatibility (defaults aIsHostileOrBoss = true).
    CHECK(Authority::ShouldScaleIncomingEnemyDamage(false, false, false, false, false));

    // Peaceful town NPC / guard / merchant (aIsHostileOrBoss = false) must NOT be scaled.
    CHECK_FALSE(Authority::ShouldScaleIncomingEnemyDamage(false, false, false, false, false, false));

    // Hostile NPC or boss (aIsHostileOrBoss = true) scales unless player/teammate/summon/commanded/mount.
    CHECK(Authority::ShouldScaleIncomingEnemyDamage(false, false, false, false, false, true));
    CHECK_FALSE(Authority::ShouldScaleIncomingEnemyDamage(true, false, false, false, false, true));  // player
    CHECK_FALSE(Authority::ShouldScaleIncomingEnemyDamage(false, true, false, false, false, true));  // teammate
    CHECK_FALSE(Authority::ShouldScaleIncomingEnemyDamage(false, false, true, false, false, true));  // summon
    CHECK_FALSE(Authority::ShouldScaleIncomingEnemyDamage(false, false, false, true, false, true));  // commanded
    CHECK_FALSE(Authority::ShouldScaleIncomingEnemyDamage(false, false, false, false, true, true));  // mount
}
