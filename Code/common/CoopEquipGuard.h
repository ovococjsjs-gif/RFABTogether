#pragma once

#include <cstdint>

namespace CoopEquipGuard
{

struct UnequipHookDecision
{
    bool AllowEngineCall{true};
    bool EmitNetworkEvent{false};
    bool WasLoopBroken{false};
};

// Defense-in-depth per-thread retry-loop breaker for EquipManager::DoUnEquip (SE 37979).
//
// Skyrim's native fast-travel / cell-unload / outfit replacement loops call DoUnEquip repeatedly
// until ExtraWorn / ExtraWornLeft is cleared from ExtraContainerChanges. If a hook returns nullptr
// (unoverridden remote actor) or fails to clear ExtraWorn on a Low-process actor, the engine retries
// the exact (actorFormId, itemFormId) pair hundreds of thousands of times on a BSJobs/Papyrus worker
// thread while WinMain waits at the BSJobs frame barrier.
//
// Used as a thread_local instance inside UnEquipHook:
//   - Preserves normal remote protection (first 2 attempts return nullptr).
//   - On >= 3 consecutive identical attempts within 250ms, lets RealUnEquip execute silently on a
//     remote actor so ExtraWorn clears and the engine loop terminates cleanly, and suppresses further
//     EquipmentChangeEvent posts on a local actor so Session::Lane::kSimulation can never overflow.
class UnequipRetryGuard
{
public:
    static constexpr uint64_t kBurstResetWindowMs = 250;
    static constexpr uint32_t kMaxNormalAttemptsPerBurst = 2;

    [[nodiscard]] constexpr UnequipHookDecision Evaluate(
        uint32_t aActorFormId,
        uint32_t aItemFormId,
        uint64_t aNowSteadyMs,
        bool aIsRemote,
        bool aIsLocal,
        bool aScopedRemoteOverride,
        bool aScopedUnequipOverride,
        bool aQueueEquip) noexcept
    {
        if (aActorFormId == 0 || aItemFormId == 0)
        {
            Reset();
            return UnequipHookDecision{true, false, false};
        }

        if (aActorFormId == m_lastActorFormId &&
            aItemFormId == m_lastItemFormId &&
            aNowSteadyMs >= m_lastAttemptMs &&
            (aNowSteadyMs - m_lastAttemptMs) <= kBurstResetWindowMs)
        {
            ++m_consecutiveCount;
        }
        else
        {
            m_lastActorFormId = aActorFormId;
            m_lastItemFormId = aItemFormId;
            m_consecutiveCount = 1;
        }
        m_lastAttemptMs = aNowSteadyMs;

        if (aIsRemote)
        {
            if (aScopedRemoteOverride)
                return UnequipHookDecision{true, false, false};

            if (m_consecutiveCount <= kMaxNormalAttemptsPerBurst)
                return UnequipHookDecision{false, false, false};

            return UnequipHookDecision{
                true,
                false,
                m_consecutiveCount == (kMaxNormalAttemptsPerBurst + 1)};
        }

        if (aIsLocal)
        {
            const bool wantEvent = !aScopedUnequipOverride && !aQueueEquip;
            if (!wantEvent)
                return UnequipHookDecision{true, false, false};

            if (m_consecutiveCount <= kMaxNormalAttemptsPerBurst)
                return UnequipHookDecision{true, true, false};

            return UnequipHookDecision{
                true,
                false,
                m_consecutiveCount == (kMaxNormalAttemptsPerBurst + 1)};
        }

        return UnequipHookDecision{true, false, false};
    }

    constexpr void Reset() noexcept
    {
        m_lastActorFormId = 0;
        m_lastItemFormId = 0;
        m_lastAttemptMs = 0;
        m_consecutiveCount = 0;
    }

    [[nodiscard]] constexpr uint32_t GetConsecutiveCount() const noexcept { return m_consecutiveCount; }

private:
    uint32_t m_lastActorFormId{0};
    uint32_t m_lastItemFormId{0};
    uint64_t m_lastAttemptMs{0};
    uint32_t m_consecutiveCount{0};
};

// Decides whether an owner client receiving AssignCharacterResponse(Owner == true) must immediately
// push its live BuildActorData(pActor) back to the server via RequestOwnershipClaim:
//   1. Newly created server entity (!aServerProvidedExistingState): push whenever an item/equip hook
//      marked DirtyInventory during the RTT OR Skyrim's InitOutfitItems / InitLeveledItems silently
//      populated ExtraContainerChanges without calling Actor::AddInventoryItem (aLiveInventoryDiffersFromServer).
//   2. Existing dormant/checkpoint server entity (aServerProvidedExistingState) whose server inventory
//      is still empty (aServerInventoryEmpty) on a living non-player NPC: ShouldApplyWholesaleActorInventory
//      kept the live NPC's real weapons locally, so seed the server's empty InventoryComponent immediately
//      so future leader switches and reconnects never receive an empty or weaponless payload.
[[nodiscard]] inline constexpr bool ShouldSyncPostAssignmentInventory(
    bool aServerProvidedExistingState,
    bool aDirtyInventoryDuringWait,
    bool aLiveInventoryDiffersFromServer,
    bool aIsPlayer,
    bool aIsDead,
    bool aServerInventoryEmpty,
    bool aLiveInventoryNonEmpty) noexcept
{
    if (!aServerProvidedExistingState)
        return aDirtyInventoryDuringWait || aLiveInventoryDiffersFromServer;

    if (!aIsPlayer && !aIsDead && aServerInventoryEmpty && aLiveInventoryNonEmpty)
        return true;

    return false;
}

} // namespace CoopEquipGuard
