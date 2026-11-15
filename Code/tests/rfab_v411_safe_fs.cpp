// Work plan 4.3 / defect M14: filesystem calls that cannot throw (common/Compat/SafeFs.h), and the
// save quarantine that used the throwing forms inside noexcept functions.
//
// The file names below are the ones that ended the game at start-up: a name that the machine's ANSI
// code page cannot hold makes path::string() throw. On the Windows build machine (code page 1252)
// the Cyrillic, CJK and emoji names here are exactly such names, so this suite reproduces the crash
// there; on Linux the same cases check the logic.

#include <catch2/catch.hpp>

#include <common/Compat/SafeFs.h>
#include <common/RfabModpackCompat.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using namespace Compat::SafeFs;

namespace
{
// Names as UTF-8 bytes, turned into paths without going through the ANSI code page.
fs::path Utf8Path(std::string_view aUtf8)
{
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(aUtf8.data()), aUtf8.size()));
}

constexpr std::string_view kCyrillic = "\xD0\xA1\xD0\xBE\xD1\x85\xD1\x80\xD0\xB0\xD0\xBD\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB5 12";  // "Сохранение 12"
constexpr std::string_view kCjk = "\xE4\xB8\xAD\xE6\x96\x87\xE5\xAD\x98\xE6\xA1\xA3";                                              // "中文存档"
constexpr std::string_view kEmoji = "save \xF0\x9F\x98\x80";                                                                        // "save 😀"

struct TempDir
{
    fs::path Path;

    explicit TempDir(const char* apName)
    {
        std::error_code ec;
        Path = fs::temp_directory_path(ec) / apName;
        fs::remove_all(Path, ec);
        fs::create_directories(Path, ec);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(Path, ec);
    }

    void Write(const fs::path& acRelative, std::string_view aContent) const
    {
        std::ofstream out(Path / acRelative, std::ios::binary);
        out.write(aContent.data(), static_cast<std::streamsize>(aContent.size()));
    }
};

void AppendU16(std::string& aOut, uint16_t aValue)
{
    aOut.push_back(static_cast<char>(aValue & 0xFFu));
    aOut.push_back(static_cast<char>((aValue >> 8) & 0xFFu));
}

void AppendU32(std::string& aOut, uint32_t aValue)
{
    for (int shift = 0; shift < 32; shift += 8)
        aOut.push_back(static_cast<char>((aValue >> shift) & 0xFFu));
}

void AppendStr16(std::string& aOut, std::string_view aText)
{
    AppendU16(aOut, static_cast<uint16_t>(aText.size()));
    aOut.append(aText);
}

// A save the validator accepts: Skyrim SE header (version 12) and enough body.
std::string ValidEssBytes()
{
    std::string header;
    AppendU32(header, 12u);  // version
    AppendU32(header, 1u);   // save number
    AppendStr16(header, "Dovah");
    AppendU32(header, 10u);  // level
    AppendStr16(header, "Whiterun");
    AppendStr16(header, "001.00");
    AppendStr16(header, "Nord");
    AppendU16(header, 0u);   // sex
    AppendU32(header, 0u);   // experience
    AppendU32(header, 0u);   // experience to level up
    AppendU32(header, 0u);   // file time, low
    AppendU32(header, 0u);   // file time, high
    AppendU32(header, 2u);   // screenshot width
    AppendU32(header, 2u);   // screenshot height
    AppendU16(header, 0u);   // not compressed

    std::string bytes = "TESV_SAVEGAME";
    AppendU32(bytes, static_cast<uint32_t>(header.size()));
    bytes += header;
    bytes.append(16 + 256, 'Z');  // 2x2 RGBA screenshot and a body
    return bytes;
}

std::string ValidSkseBytes()
{
    std::string bytes = "SKSE";
    AppendU32(bytes, 1u);           // format version
    AppendU32(bytes, 0x02001400u);  // SKSE 2.0.20
    AppendU32(bytes, 0x01050610u);  // runtime 1.5.97
    AppendU32(bytes, 2u);           // plugins
    bytes.append(2 * 16, '\x11');
    return bytes;
}

const std::string kDamaged = "this is not a save file, it only has the right extension";

bool ExistsNow(const fs::path& acPath)
{
    std::error_code ec;
    return fs::exists(acPath, ec) && !ec;
}
} // namespace

TEST_CASE("v4.1.1 safe fs: UTF-16 to UTF-8 never fails on the text itself", "[rfab][v411][safefs]")
{
    SECTION("ASCII, Cyrillic, CJK")
    {
        CHECK(Utf16ToUtf8Lossy(std::u16string_view(u"Save 1.ess")) == "Save 1.ess");
        CHECK(Utf16ToUtf8Lossy(std::u16string_view(u"\u0421\u043e\u0445")) == "\xD0\xA1\xD0\xBE\xD1\x85");
        CHECK(Utf16ToUtf8Lossy(std::u16string_view(u"\u4E2D\u6587")) == "\xE4\xB8\xAD\xE6\x96\x87");
        CHECK(Utf16ToUtf8Lossy(std::u16string_view(u"")) == "");
    }

    SECTION("a surrogate pair becomes one four-byte character")
    {
        const char16_t emoji[] = {0xD83D, 0xDE00, 0};
        CHECK(Utf16ToUtf8Lossy(std::u16string_view(emoji)) == "\xF0\x9F\x98\x80");
    }

    SECTION("an unpaired surrogate (NTFS allows it in a file name) becomes U+FFFD instead of an exception")
    {
        const char16_t highInMiddle[] = {u'a', 0xD83D, u'b', 0};
        const char16_t highAtEnd[] = {u'x', 0xD83D, 0};
        const char16_t lowAlone[] = {0xDE00, u'y', 0};
        const char16_t twoHighs[] = {0xD83D, 0xD83D, 0xDE00, 0};
        CHECK(Utf16ToUtf8Lossy(std::u16string_view(highInMiddle)) == "a\xEF\xBF\xBD" "b");
        CHECK(Utf16ToUtf8Lossy(std::u16string_view(highAtEnd)) == "x\xEF\xBF\xBD");
        CHECK(Utf16ToUtf8Lossy(std::u16string_view(lowAlone)) == "\xEF\xBF\xBD" "y");
        CHECK(Utf16ToUtf8Lossy(std::u16string_view(twoHighs)) == "\xEF\xBF\xBD\xF0\x9F\x98\x80");
    }

    SECTION("32-bit code units (wchar_t on Linux) pass through as code points")
    {
        const char32_t text[] = {U'a', 0x1F600, 0x110000, 0};
        CHECK(Utf16ToUtf8Lossy(std::u32string_view(text)) == "a\xF0\x9F\x98\x80\xEF\xBF\xBD");
    }
}

TEST_CASE("v4.1.1 safe fs: extension tests work on the native text", "[rfab][v411][safefs]")
{
    CHECK(TextEndsWithAsciiNoCase(std::string_view("Save 1.ESS"), ".ess"));
    CHECK(TextEndsWithAsciiNoCase(std::u16string_view(u"\u0421\u043e\u0445.Ess"), ".ess"));
    CHECK(TextEndsWithAsciiNoCase(std::wstring_view(L"\u4E2D\u6587.ess.TMP"), ".ess.tmp"));
    CHECK(TextEndsWithAsciiNoCase(std::string_view("anything"), ""));
    CHECK_FALSE(TextEndsWithAsciiNoCase(std::string_view("a.es"), ".ess"));
    CHECK_FALSE(TextEndsWithAsciiNoCase(std::string_view("ess"), ".ess"));
    CHECK_FALSE(TextEndsWithAsciiNoCase(std::string_view("save.esp"), ".ess"));
    // Only ASCII letters fold: a code unit that merely shares its low byte with a letter is different.
    CHECK_FALSE(TextEndsWithAsciiNoCase(std::u16string_view(u"save.e\u0173s"), ".ess"));

    CHECK(EndsWithAsciiNoCase(fs::path("dir") / "Quick.ESS", ".ess"));
    CHECK(EndsWithAsciiNoCase(Utf8Path(kCjk) += ".skse", ".SKSE"));
    CHECK_FALSE(EndsWithAsciiNoCase(fs::path("save.ess.bak"), ".ess"));
    CHECK_FALSE(EndsWithAsciiNoCase(fs::path(), ".ess"));
}

TEST_CASE("v4.1.1 safe fs: a path of any name can be written to a log", "[rfab][v411][safefs]")
{
    for (const std::string_view name : {kCyrillic, kCjk, kEmoji, std::string_view("plain")})
    {
        const fs::path path = fs::path("Saves") / Utf8Path(name);
        CHECK(FileNameUtf8Lossy(path) == name);
        const std::string whole = ToUtf8Lossy(path);
        REQUIRE(whole.size() > name.size());
        CHECK(whole.substr(whole.size() - name.size()) == name);
    }
    CHECK(ToUtf8Lossy(fs::path()) == "");
}

TEST_CASE("v4.1.1 safe fs: queries answer false instead of throwing", "[rfab][v411][safefs]")
{
    TempDir tmp("rfab_safefs_queries");
    tmp.Write("file.bin", "x");

    CHECK(Exists(tmp.Path));
    CHECK(IsDirectory(tmp.Path));
    CHECK_FALSE(IsRegularFile(tmp.Path));
    CHECK(IsRegularFile(tmp.Path / "file.bin"));
    CHECK_FALSE(IsDirectory(tmp.Path / "file.bin"));
    CHECK_FALSE(Exists(tmp.Path / "missing"));
    CHECK_FALSE(IsDirectory(tmp.Path / "missing" / "deeper"));
    CHECK_FALSE(Exists(fs::path()));

    CHECK(CreateDirectories(tmp.Path / "a" / "b" / Utf8Path(kCyrillic)));
    CHECK(CreateDirectories(tmp.Path / "a" / "b"));                 // already there
    CHECK_FALSE(CreateDirectories(tmp.Path / "file.bin" / "below"));  // a file is in the way

    const fs::path fallback = "fallback-directory";
    CHECK_FALSE(CurrentPathOr(fallback).empty());
    CHECK(CurrentPathOr(fallback) != fallback);  // the test process has a working directory
}

TEST_CASE("v4.1.1 safe fs: walking a directory", "[rfab][v411][safefs]")
{
    TempDir tmp("rfab_safefs_walk");
    tmp.Write("a.txt", "1");
    tmp.Write(Utf8Path(kCyrillic) += ".ess", "2");
    tmp.Write(Utf8Path(kCjk) += ".ess", "3");
    tmp.Write(Utf8Path(kEmoji) += ".skse", "4");
    REQUIRE(CreateDirectories(tmp.Path / "sub"));

    SECTION("every entry is visited once, whatever its name")
    {
        std::vector<std::string> names;
        size_t directories = 0;
        const bool ok = ForEachEntry(tmp.Path, [&](const fs::directory_entry& acEntry) {
            names.push_back(FileNameUtf8Lossy(acEntry.path()));
            if (IsDirectory(acEntry))
                ++directories;
        });
        CHECK(ok);
        std::sort(names.begin(), names.end());
        std::vector<std::string> expected{"a.txt", "sub", std::string(kCyrillic) + ".ess", std::string(kCjk) + ".ess",
                                          std::string(kEmoji) + ".skse"};
        std::sort(expected.begin(), expected.end());
        CHECK(names == expected);
        CHECK(directories == 1);
    }

    SECTION("a visitor that returns false stops the walk, and that is still a successful walk")
    {
        size_t visited = 0;
        CHECK(ForEachEntry(tmp.Path, [&](const fs::directory_entry&) {
            ++visited;
            return false;
        }));
        CHECK(visited == 1);
    }

    SECTION("a visitor that returns true keeps going")
    {
        size_t visited = 0;
        CHECK(ForEachEntry(tmp.Path, [&](const fs::directory_entry&) {
            ++visited;
            return true;
        }));
        CHECK(visited == 5);
    }

    SECTION("a directory that is not there, or is a file: false, nothing visited")
    {
        size_t visited = 0;
        CHECK_FALSE(ForEachEntry(tmp.Path / "missing", [&](const fs::directory_entry&) { ++visited; }));
        CHECK_FALSE(ForEachEntry(tmp.Path / "a.txt", [&](const fs::directory_entry&) { ++visited; }));
        CHECK_FALSE(ForEachEntry(fs::path(), [&](const fs::directory_entry&) { ++visited; }));
        CHECK(visited == 0);
    }

    SECTION("an exception from the visitor ends the walk; it does not leave a noexcept function")
    {
        size_t visited = 0;
        const bool ok = ForEachEntry(tmp.Path, [&](const fs::directory_entry&) {
            ++visited;
            throw std::bad_alloc();
        });
        CHECK_FALSE(ok);
        CHECK(visited == 1);
    }

    SECTION("an empty directory is a successful walk of nothing")
    {
        size_t visited = 0;
        CHECK(ForEachEntry(tmp.Path / "sub", [&](const fs::directory_entry&) { ++visited; }));
        CHECK(visited == 0);
    }
}

// ---- the save quarantine (work plan 4.4, defect M14) ----

TEST_CASE("v4.1.1 quarantine: the helpers agree on what a valid save is", "[rfab][v411][quarantine]")
{
    TempDir tmp("rfab_quarantine_helpers");
    tmp.Write("good.ess", ValidEssBytes());
    tmp.Write("good.skse", ValidSkseBytes());
    tmp.Write("bad.ess", kDamaged);
    tmp.Write("empty.ess", "");

    using namespace RfabCompat::Detail;
    CHECK(IsValidEssFileOnDisk(tmp.Path / "good.ess"));
    CHECK(IsValidSkseCosaveOnDisk(tmp.Path / "good.skse"));
    CHECK_FALSE(IsValidEssFileOnDisk(tmp.Path / "bad.ess"));
    CHECK_FALSE(IsValidEssFileOnDisk(tmp.Path / "empty.ess"));

    // "Can it be read at all" is a different question from "is it a save".
    CHECK(CanReadFileNow(tmp.Path / "bad.ess"));
    CHECK(CanReadFileNow(tmp.Path / "empty.ess"));
    CHECK_FALSE(CanReadFileNow(tmp.Path / "missing.ess"));
    CHECK_FALSE(CanReadFileNow(tmp.Path));  // a directory

    CHECK(IsCoopSaveFile(tmp.Path / "RFAB_MP_ab12_0007.ess"));
    CHECK(IsCoopSaveFile(tmp.Path / "rfab_mp_PreJoin_Backup.skse"));
    CHECK(IsCoopSaveFile(fs::path("RFAB_MP_x.ess")));
    CHECK_FALSE(IsCoopSaveFile(tmp.Path / "Save 12 RFAB_MP_.ess"));   // the prefix, not a substring
    CHECK_FALSE(IsCoopSaveFile(tmp.Path / "Quicksave0.ess"));
    CHECK_FALSE(IsCoopSaveFile(fs::path("RFAB_MP_folder") / "Autosave1.ess"));  // the file name, not a folder
    CHECK_FALSE(IsCoopSaveFile(fs::path()));
}

TEST_CASE("v4.1.1 quarantine: only the coop's own damaged saves are moved", "[rfab][v411][quarantine]")
{
    using namespace RfabCompat::Detail;
    TempDir tmp("rfab_quarantine_policy");
    const fs::path quarantine = tmp.Path / "Quarantined_Corrupt_v100";

    // The coop's slots.
    tmp.Write("RFAB_MP_ab12_0001.ess", ValidEssBytes());
    tmp.Write("RFAB_MP_ab12_0001.skse", ValidSkseBytes());
    tmp.Write("RFAB_MP_ab12_0002.ess", kDamaged);            // damaged: goes, with its co-save
    tmp.Write("RFAB_MP_ab12_0002.skse", ValidSkseBytes());
    tmp.Write("RFAB_MP_ab12_0003.ess", ValidEssBytes());
    tmp.Write("RFAB_MP_ab12_0003.skse", kDamaged);           // only the co-save is damaged: only it goes
    tmp.Write("rfab_mp_ab12_0004.ess.tmp", kDamaged);        // a save that was being written when the game died

    // The player's own saves: never moved, whatever state they are in.
    tmp.Write("Quicksave0.ess", ValidEssBytes());
    tmp.Write("Autosave1.ess", kDamaged);
    tmp.Write("Save 7 - Whiterun.ess", "");
    tmp.Write("Save 7 - Whiterun.skse", kDamaged);
    tmp.Write("notes.txt", kDamaged);                        // not a save at all

    const SaveQuarantineReport report = QuarantineDamagedCoopSavesInDirectory(tmp.Path);

    SECTION("the damaged coop files are in the quarantine folder")
    {
        CHECK(report.Moved == 3);  // 0002.ess, 0003.skse, 0004.ess.tmp (the companion of 0002 is not counted)
        CHECK(report.MoveFailed == 0);
        CHECK_FALSE(report.WalkFailed);
        CHECK(ExistsNow(quarantine / "RFAB_MP_ab12_0002.ess"));
        CHECK(ExistsNow(quarantine / "RFAB_MP_ab12_0002.skse"));
        CHECK(ExistsNow(quarantine / "RFAB_MP_ab12_0003.skse"));
        CHECK(ExistsNow(quarantine / "rfab_mp_ab12_0004.ess.tmp"));
        CHECK_FALSE(ExistsNow(tmp.Path / "RFAB_MP_ab12_0002.ess"));
        CHECK_FALSE(ExistsNow(tmp.Path / "RFAB_MP_ab12_0002.skse"));
        CHECK_FALSE(ExistsNow(tmp.Path / "RFAB_MP_ab12_0003.skse"));
    }

    SECTION("healthy coop files stay")
    {
        CHECK(ExistsNow(tmp.Path / "RFAB_MP_ab12_0001.ess"));
        CHECK(ExistsNow(tmp.Path / "RFAB_MP_ab12_0001.skse"));
        CHECK(ExistsNow(tmp.Path / "RFAB_MP_ab12_0003.ess"));
    }

    SECTION("the player's saves are exactly where they were, and the damaged ones are named")
    {
        CHECK(ExistsNow(tmp.Path / "Quicksave0.ess"));
        CHECK(ExistsNow(tmp.Path / "Autosave1.ess"));
        CHECK(ExistsNow(tmp.Path / "Save 7 - Whiterun.ess"));
        CHECK(ExistsNow(tmp.Path / "Save 7 - Whiterun.skse"));
        CHECK(ExistsNow(tmp.Path / "notes.txt"));
        CHECK_FALSE(ExistsNow(quarantine / "Autosave1.ess"));

        CHECK(report.ForeignDamaged == 3);
        std::vector<std::string> names = report.ForeignNamesUtf8;
        std::sort(names.begin(), names.end());
        CHECK(names == std::vector<std::string>{"Autosave1.ess", "Save 7 - Whiterun.ess", "Save 7 - Whiterun.skse"});
        CHECK(report.Unreadable == 0);
    }

    SECTION("a second run finds nothing more to do")
    {
        const SaveQuarantineReport again = QuarantineDamagedCoopSavesInDirectory(tmp.Path);
        CHECK(again.Moved == 0);
        CHECK(again.ForeignDamaged == 3);  // still there, still reported, still untouched
        CHECK(ExistsNow(tmp.Path / "Autosave1.ess"));
    }

    SECTION("the count the client logs is the number of moved files")
    {
        TempDir other("rfab_quarantine_policy_count");
        other.Write("RFAB_MP_x.ess", kDamaged);
        other.Write("Mine.ess", kDamaged);
        CHECK(QuarantineCorruptEssFilesInDirectory(other.Path) == 1);
        CHECK(ExistsNow(other.Path / "Mine.ess"));
    }
}

TEST_CASE("v4.1.1 quarantine: names the machine's code page cannot hold (M14)", "[rfab][v411][quarantine]")
{
    // Before 4.1.1 the scan called path::string() on every file name inside a noexcept function.
    // On a machine whose ANSI code page cannot hold one of these names that was std::terminate
    // during start-up; the Windows build machine is such a machine for all three.
    using namespace RfabCompat::Detail;
    TempDir tmp("rfab_quarantine_names");

    const fs::path cyrillicSave = Utf8Path(kCyrillic) += ".ess";
    const fs::path cjkSave = Utf8Path(kCjk) += ".ess";
    const fs::path emojiCosave = Utf8Path(kEmoji) += ".skse";
    const fs::path coopCyrillic = Utf8Path(std::string("RFAB_MP_") + std::string(kCyrillic)) += ".ess";
    const fs::path coopCjkCosave = Utf8Path(std::string("RFAB_MP_") + std::string(kCjk)) += ".skse";

    tmp.Write(cyrillicSave, ValidEssBytes());
    tmp.Write(cjkSave, kDamaged);
    tmp.Write(emojiCosave, kDamaged);
    tmp.Write(coopCyrillic, kDamaged);
    tmp.Write(coopCjkCosave, kDamaged);
    REQUIRE(ExistsNow(tmp.Path / cjkSave));  // the file system took the names

    const SaveQuarantineReport report = QuarantineDamagedCoopSavesInDirectory(tmp.Path);

    CHECK(report.Moved == 2);
    CHECK(ExistsNow(tmp.Path / "Quarantined_Corrupt_v100" / coopCyrillic));
    CHECK(ExistsNow(tmp.Path / "Quarantined_Corrupt_v100" / coopCjkCosave));

    CHECK(ExistsNow(tmp.Path / cyrillicSave));
    CHECK(ExistsNow(tmp.Path / cjkSave));
    CHECK(ExistsNow(tmp.Path / emojiCosave));
    CHECK(report.ForeignDamaged == 2);

    std::vector<std::string> names = report.ForeignNamesUtf8;
    std::sort(names.begin(), names.end());
    std::vector<std::string> expected{std::string(kCjk) + ".ess", std::string(kEmoji) + ".skse"};
    std::sort(expected.begin(), expected.end());
    CHECK(names == expected);  // readable in the log, as UTF-8

    // The slot lookup walks the same folder with the same names.
    CHECK(ResolveExistingEssSlotPath(tmp.Path, std::string(kCyrillic)) == tmp.Path / cyrillicSave);
    CHECK(ResolveExistingEssSlotPath(tmp.Path, "no such slot").empty());
}

TEST_CASE("v4.1.1 quarantine: folders that are not there, and the report for the log", "[rfab][v411][quarantine]")
{
    using namespace RfabCompat::Detail;

    SECTION("a missing folder, a file instead of a folder, an empty path")
    {
        TempDir tmp("rfab_quarantine_missing");
        tmp.Write("file.ess", kDamaged);
        for (const fs::path& path : {tmp.Path / "missing", tmp.Path / "file.ess", fs::path()})
        {
            const SaveQuarantineReport report = QuarantineDamagedCoopSavesInDirectory(path);
            CHECK(report.Moved == 0);
            CHECK(report.ForeignDamaged == 0);
            CHECK_FALSE(report.WalkFailed);
        }
        CHECK(ExistsNow(tmp.Path / "file.ess"));
        CHECK_FALSE(ExistsNow(tmp.Path / "Quarantined_Corrupt_v100"));
    }

    SECTION("nothing damaged: the quarantine folder is not even created")
    {
        TempDir tmp("rfab_quarantine_clean");
        tmp.Write("RFAB_MP_a.ess", ValidEssBytes());
        tmp.Write("Mine.ess", ValidEssBytes());
        const SaveQuarantineReport report = QuarantineDamagedCoopSavesInDirectory(tmp.Path);
        CHECK(report.Moved == 0);
        CHECK(report.ForeignDamaged == 0);
        CHECK_FALSE(ExistsNow(tmp.Path / "Quarantined_Corrupt_v100"));
    }

    SECTION("the names in the log are capped, the count is not")
    {
        TempDir tmp("rfab_quarantine_many");
        for (int i = 0; i < 20; ++i)
            tmp.Write("Save " + std::to_string(i) + ".ess", kDamaged);
        const SaveQuarantineReport report = QuarantineDamagedCoopSavesInDirectory(tmp.Path);
        CHECK(report.ForeignDamaged == 20);
        CHECK(report.ForeignNamesUtf8.size() == SaveQuarantineReport::kMaxReportedNames);
        const std::string text = report.DescribeForeign();
        CHECK(text.find("and 12 more") != std::string::npos);
        CHECK(text.find("Save ") != std::string::npos);
    }

    SECTION("reports of several folders add up")
    {
        SaveQuarantineReport total;
        SaveQuarantineReport a;
        a.Moved = 2;
        a.ForeignDamaged = 1;
        a.ForeignNamesUtf8 = {"a.ess"};
        SaveQuarantineReport b;
        b.Moved = 1;
        b.MoveFailed = 1;
        b.Unreadable = 3;
        b.WalkFailed = true;
        b.ForeignDamaged = 9;
        b.ForeignNamesUtf8 = {"b1.ess", "b2.ess", "b3.ess", "b4.ess", "b5.ess", "b6.ess", "b7.ess", "b8.ess"};
        total.Add(a);
        total.Add(b);

        CHECK(total.Moved == 3);
        CHECK(total.MoveFailed == 1);
        CHECK(total.Unreadable == 3);
        CHECK(total.WalkFailed);
        CHECK(total.ForeignDamaged == 10);
        CHECK(total.ForeignNamesUtf8.size() == SaveQuarantineReport::kMaxReportedNames);
        CHECK(total.DescribeForeign() == "a.ess, b1.ess, b2.ess, b3.ess, b4.ess, b5.ess, b6.ess, b7.ess and 2 more");
        CHECK(SaveQuarantineReport{}.DescribeForeign().empty());
    }
}
