// Work plan 4.4 / defect M15: what the party leader asks the server for when he tries to load.
// The server could roll back to the checkpoint before the newest one since 4.0; no client input
// ever asked for it. Shift+F9 does now, and this suite pins which input means what.

#include <catch2/catch.hpp>

#include <common/Save/LoadRequest.h>

using Save::ClassifyLeaderLoad;
using Save::IsQuickload;
using Save::LoadAttempt;
using Save::LoadRequestKind;

namespace
{
LoadAttempt Clicked(bool aCoopSlot, bool aPreJoin = false)
{
    LoadAttempt attempt{};
    attempt.HasSlotName = true;
    attempt.LoadMenuOpen = true;
    attempt.SlotIsCoopSave = aCoopSlot;
    attempt.SlotIsPreJoinBackup = aPreJoin;
    return attempt;
}

LoadAttempt F9(bool aShift)
{
    LoadAttempt attempt{};
    attempt.HasSlotName = true;  // the engine names its quicksave file
    attempt.QuickloadKeyDown = true;
    attempt.ShiftDown = aShift;
    return attempt;
}
} // namespace

TEST_CASE("v4.1.1 load request: F9 is the newest checkpoint, Shift+F9 the one before", "[rfab][v411][saves]")
{
    CHECK(ClassifyLeaderLoad(F9(false)) == LoadRequestKind::kLatestCheckpoint);
    CHECK(ClassifyLeaderLoad(F9(true)) == LoadRequestKind::kPreviousCheckpoint);

    SECTION("it does not matter which file the engine's quickload named")
    {
        LoadAttempt attempt = F9(true);
        attempt.SlotIsCoopSave = true;
        CHECK(ClassifyLeaderLoad(attempt) == LoadRequestKind::kPreviousCheckpoint);
        attempt.SlotIsPreJoinBackup = true;
        CHECK(ClassifyLeaderLoad(attempt) == LoadRequestKind::kPreviousCheckpoint);
        attempt.HasSlotName = false;
        CHECK(ClassifyLeaderLoad(attempt) == LoadRequestKind::kPreviousCheckpoint);
    }

    SECTION("F9 with the Esc menu open is still a quickload")
    {
        LoadAttempt attempt = F9(false);
        attempt.LoadMenuOpen = true;
        attempt.SlotIsCoopSave = true;
        CHECK(IsQuickload(attempt));
        CHECK(ClassifyLeaderLoad(attempt) == LoadRequestKind::kLatestCheckpoint);
    }
}

TEST_CASE("v4.1.1 load request: Shift alone never means the previous checkpoint", "[rfab][v411][saves]")
{
    // The reload after death and loads started by scripts arrive without F9. A player who was
    // holding Shift when he died must get the newest checkpoint like everybody else.
    LoadAttempt deathReload{};
    deathReload.ShiftDown = true;
    CHECK(IsQuickload(deathReload));
    CHECK(ClassifyLeaderLoad(deathReload) == LoadRequestKind::kLatestCheckpoint);

    LoadAttempt named = deathReload;
    named.HasSlotName = true;
    named.SlotIsCoopSave = true;
    CHECK(ClassifyLeaderLoad(named) == LoadRequestKind::kLatestCheckpoint);

    // Shift held while clicking a slot in the list is a click on that slot.
    LoadAttempt clicked = Clicked(true);
    clicked.ShiftDown = true;
    CHECK(ClassifyLeaderLoad(clicked) == LoadRequestKind::kSpecificSlot);
}

TEST_CASE("v4.1.1 load request: a slot is loaded by name only when it was clicked and is a party checkpoint", "[rfab][v411][saves]")
{
    CHECK(ClassifyLeaderLoad(Clicked(true)) == LoadRequestKind::kSpecificSlot);

    SECTION("the player's own saves are not party checkpoints")
    {
        CHECK(ClassifyLeaderLoad(Clicked(false)) == LoadRequestKind::kLatestCheckpoint);
    }

    SECTION("neither is the private backup written before joining")
    {
        CHECK(ClassifyLeaderLoad(Clicked(true, true)) == LoadRequestKind::kLatestCheckpoint);
    }

    SECTION("a named load with no menu open did not come from the list")
    {
        LoadAttempt attempt = Clicked(true);
        attempt.LoadMenuOpen = false;
        CHECK(IsQuickload(attempt));
        CHECK(ClassifyLeaderLoad(attempt) == LoadRequestKind::kLatestCheckpoint);
    }

    SECTION("a load without a file name is a quickload even with the menu open")
    {
        LoadAttempt attempt{};
        attempt.LoadMenuOpen = true;
        attempt.SlotIsCoopSave = true;
        CHECK(IsQuickload(attempt));
        CHECK(ClassifyLeaderLoad(attempt) == LoadRequestKind::kLatestCheckpoint);
    }
}

TEST_CASE("v4.1.1 load request: every combination gives exactly one of the three answers, as 4.1.0 did without Shift",
          "[rfab][v411][saves]")
{
    for (unsigned bits = 0; bits < 64; ++bits)
    {
        LoadAttempt attempt{};
        attempt.HasSlotName = (bits & 1u) != 0;
        attempt.QuickloadKeyDown = (bits & 2u) != 0;
        attempt.ShiftDown = (bits & 4u) != 0;
        attempt.LoadMenuOpen = (bits & 8u) != 0;
        attempt.SlotIsCoopSave = (bits & 16u) != 0;
        attempt.SlotIsPreJoinBackup = (bits & 32u) != 0;

        const LoadRequestKind kind = ClassifyLeaderLoad(attempt);

        // The 4.1.0 rule, written out the way CoordinatedSaveService had it.
        const bool quickload = !attempt.HasSlotName || attempt.QuickloadKeyDown || !attempt.LoadMenuOpen;
        const bool specific = !quickload && !attempt.SlotIsPreJoinBackup && attempt.SlotIsCoopSave;
        const LoadRequestKind before = specific ? LoadRequestKind::kSpecificSlot : LoadRequestKind::kLatestCheckpoint;

        INFO("bits " << bits);
        if (attempt.QuickloadKeyDown && attempt.ShiftDown)
            CHECK(kind == LoadRequestKind::kPreviousCheckpoint);
        else
            CHECK(kind == before);
    }
}
