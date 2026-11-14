#pragma once

// What the party leader asks the server for when he tries to load a save (work plan 4.4).
//
// In a coop session no player loads a save alone: the engine's load is intercepted, and the leader's
// attempt becomes a request to the server, which rolls the shared world back and tells everybody
// which slot to load. There are three things he can ask for:
//
//   F9, or any load that did not come from the list in the Esc menu   the newest party checkpoint
//   Shift + F9                                                        the checkpoint BEFORE the newest
//   a coop slot picked in Esc -> Load                                 exactly that slot
//
// "Before the newest" is the way out when the newest checkpoint was saved in a situation nobody
// survives. The server has implemented that rollback since 4.0 (SaveSnapshotService::
// TriggerRollbackToPrevious, it also drops the newer checkpoint), but until 4.1.1 no key, menu or
// command of the client ever asked for it (defect M15).
//
// The decision is here, away from the engine, so that the Linux suite can check every combination.

#include <cstdint>

namespace Save
{
enum class LoadRequestKind : uint8_t
{
    kLatestCheckpoint = 0,
    kPreviousCheckpoint = 1,
    kSpecificSlot = 2,
};

struct LoadAttempt
{
    bool HasSlotName{false};        // the engine named a file (the quickload path may not)
    bool QuickloadKeyDown{false};   // F9 is held right now
    bool ShiftDown{false};          // Shift is held right now
    bool LoadMenuOpen{false};       // the Esc menu (Journal) is open: the slot was picked from its list
    bool SlotIsCoopSave{false};     // the file is one of the coop's own slots (RFAB_MP_...)
    bool SlotIsPreJoinBackup{false};  // ... namely the private backup written before joining
};

// A quickload: F9, the death reload, a script - anything but a click in the load list.
[[nodiscard]] constexpr bool IsQuickload(const LoadAttempt& acAttempt) noexcept
{
    return !acAttempt.HasSlotName || acAttempt.QuickloadKeyDown || !acAttempt.LoadMenuOpen;
}

[[nodiscard]] constexpr LoadRequestKind ClassifyLeaderLoad(const LoadAttempt& acAttempt) noexcept
{
    // Both keys, held together, and nothing else: the death reload and scripted loads arrive without
    // F9, so a player who happens to hold Shift when he dies still gets the newest checkpoint.
    if (acAttempt.QuickloadKeyDown && acAttempt.ShiftDown)
        return LoadRequestKind::kPreviousCheckpoint;

    // A slot is loaded by name only when the leader clicked it in the list, it is a coop slot, and it
    // is not the pre-join backup (that file is private to each player and is not a party checkpoint).
    if (!IsQuickload(acAttempt) && acAttempt.SlotIsCoopSave && !acAttempt.SlotIsPreJoinBackup)
        return LoadRequestKind::kSpecificSlot;

    return LoadRequestKind::kLatestCheckpoint;
}
} // namespace Save
