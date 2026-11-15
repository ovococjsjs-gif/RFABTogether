#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

namespace CoopLootLedger
{
struct LootSlotKey
{
    uint32_t ContainerServerId{0};
    uint32_t ModId{0};
    uint32_t BaseId{0};

    bool operator==(const LootSlotKey& acRhs) const noexcept
    {
        return ContainerServerId == acRhs.ContainerServerId &&
               ModId == acRhs.ModId &&
               BaseId == acRhs.BaseId;
    }
};

struct LootSlotKeyHash
{
    size_t operator()(const LootSlotKey& acKey) const noexcept
    {
        uint64_t h = static_cast<uint64_t>(acKey.ContainerServerId) * 0x9E3779B97F4A7C15ULL;
        h ^= (static_cast<uint64_t>(acKey.ModId) << 32) | static_cast<uint64_t>(acKey.BaseId);
        h ^= (h >> 30);
        h *= 0xBF58476D1CE4E5B9ULL;
        h ^= (h >> 27);
        return static_cast<size_t>(h);
    }
};

struct ClaimResolution
{
    int32_t GrantedTake{0};
    int32_t OverdrawnRollback{0};
};

/**
 * A body changes hands: may an item from the new simulator's list enter the server's list?
 *
 * The player who takes over the simulation of an NPC sends the NPC as his game shows it, items
 * included, and the server used to replace its own list with that one. For a living NPC that is
 * the owner's snapshot. For a body it undid the loot ledger: a take by another player that reached
 * the server between the grant and the new simulator's confirmation was overwritten by a list
 * built before the take, the item was listed again, sent to every player in range, and could be
 * taken a second time (work plan 5.3, defect M24).
 *
 * For a body the server already holds as dead its own list stands. From the new simulator's list
 * only what the server has never listed and nobody has taken is added - an item his game put on
 * the body that the server has not heard of yet.
 *
 *   aHeldCount   how many of the item the server lists on the body now
 *   aIsDepleted  the last one was taken and nothing was put back since (ContainerLootLedger)
 */
[[nodiscard]] constexpr bool ShouldAcceptClaimedBodyItem(int32_t aHeldCount, bool aIsDepleted) noexcept
{
    // Listed: the server's count stands, whatever the claim says. A higher count in the claim is a
    // take the claimer has not seen yet; a lower one is his own take the server has not seen yet.
    if (aHeldCount > 0)
        return false;
    // Taken to the last one: it stays taken.
    return !aIsDepleted;
}

/**
 * SkyMP-grade server-authoritative container & corpse anti-duplication ledger.
 * Depletion claims persist for the lifetime of the container/corpse (not a short 15s window) so
 * late-arriving peers cannot re-loot dynamic RFAB boss tokens / death drops minutes later, while
 * supporting exact checkpoint snapshot capture/restore, explicit deposits, and container respawns.
 */
class ContainerLootLedger
{
public:
    // 0 means container-lifetime persistence (no time-based expiry); finite >0 honors an explicit window.
    static constexpr uint64_t kPersistentDepletionWindowMs = 0ULL;
    static constexpr uint64_t kDefaultConflictWindowMs = kPersistentDepletionWindowMs;
    static constexpr size_t kMaxTrackedDepletions = 32768;

    struct SnapshotEntry
    {
        uint32_t ContainerServerId{0};
        uint32_t ModId{0};
        uint32_t BaseId{0};
        uint32_t ClaimantPlayerId{0};
        uint64_t DepletedAtMs{0};
    };

    using Snapshot = std::vector<SnapshotEntry>;

    void Clear() noexcept
    {
        m_depletions.clear();
        m_fifoOrder.clear();
        m_nextSequence = 1;
    }

    [[nodiscard]] size_t Size() const noexcept
    {
        return m_depletions.size();
    }

    void RecordDeposit(uint32_t aContainerServerId, uint32_t aModId, uint32_t aBaseId) noexcept
    {
        if (aContainerServerId == 0 || (aModId == 0 && aBaseId == 0))
            return;

        const LootSlotKey key{aContainerServerId, aModId, aBaseId};
        m_depletions.erase(key);
    }

    template <class TGameId>
        requires requires(const TGameId& id) { id.ModId; id.BaseId; }
    void RecordDeposit(uint32_t aContainerServerId, const TGameId& acItemId) noexcept
    {
        if (!acItemId)
            return;
        RecordDeposit(aContainerServerId, acItemId.ModId, acItemId.BaseId);
    }

    // Was the last one of this item taken from the container, with nothing put back since?
    [[nodiscard]] bool IsDepleted(uint32_t aContainerServerId, uint32_t aModId, uint32_t aBaseId) const noexcept
    {
        if (aContainerServerId == 0 || (aModId == 0 && aBaseId == 0))
            return false;
        return m_depletions.find(LootSlotKey{aContainerServerId, aModId, aBaseId}) != m_depletions.end();
    }

    template <class TGameId>
        requires requires(const TGameId& id) { id.ModId; id.BaseId; }
    [[nodiscard]] bool IsDepleted(uint32_t aContainerServerId, const TGameId& acItemId) const noexcept
    {
        return IsDepleted(aContainerServerId, acItemId.ModId, acItemId.BaseId);
    }

    void ForgetContainer(uint32_t aContainerServerId) noexcept
    {
        if (aContainerServerId == 0 || m_depletions.empty())
            return;

        for (auto it = m_depletions.begin(); it != m_depletions.end();)
        {
            if (it->first.ContainerServerId == aContainerServerId)
                it = m_depletions.erase(it);
            else
                ++it;
        }
    }

    [[nodiscard]] Snapshot CaptureSnapshot() const
    {
        Snapshot out;
        out.reserve(m_depletions.size());
        for (const auto& fifoItem : m_fifoOrder)
        {
            const auto it = m_depletions.find(fifoItem.Key);
            if (it == m_depletions.end() || it->second.Sequence != fifoItem.Sequence)
                continue;
            out.push_back(SnapshotEntry{
                it->first.ContainerServerId,
                it->first.ModId,
                it->first.BaseId,
                it->second.ClaimantPlayerId,
                it->second.DepletedAtMs});
        }
        return out;
    }

    // Disk checkpoints can outlive the process that assigned player IDs. In that case the
    // claimant is unknown, not a player in the new process: zero cannot match a valid player ID.
    // In-memory rollback preserves claimant IDs and the existing same-player behavior.
    void RestoreSnapshot(const Snapshot& acSnapshot, bool aForgetClaimants = false)
    {
        Clear();
        for (const auto& entry : acSnapshot)
        {
            if (entry.ContainerServerId == 0 || (entry.ModId == 0 && entry.BaseId == 0))
                continue;
            RecordDepletion(
                LootSlotKey{entry.ContainerServerId, entry.ModId, entry.BaseId},
                aForgetClaimants ? 0u : entry.ClaimantPlayerId,
                entry.DepletedAtMs);
        }
    }

    [[nodiscard]] ClaimResolution EvaluateWithdrawal(
        uint32_t aContainerServerId,
        uint32_t aModId,
        uint32_t aBaseId,
        uint32_t aClaimantPlayerId,
        int32_t aAvailableBefore,
        int32_t aRequestedTake,
        bool aIsQuestItem,
        uint64_t aNowMs,
        uint64_t aConflictWindowMs = kDefaultConflictWindowMs) noexcept
    {
        ClaimResolution res{};
        if (aRequestedTake <= 0)
            return res;

        // Shared co-op keys, dragon claws, and quest items intentionally replicate across party members.
        if (aIsQuestItem || aContainerServerId == 0 || (aModId == 0 && aBaseId == 0) || aClaimantPlayerId == 0)
        {
            res.GrantedTake = aRequestedTake;
            return res;
        }

        if (aConflictWindowMs > 0)
            PruneExpired(aNowMs, aConflictWindowMs);

        const int32_t safeAvailable = std::max(0, aAvailableBefore);
        const LootSlotKey key{aContainerServerId, aModId, aBaseId};

        // Case 1: Server container ledger explicitly holds enough items to cover the withdrawal.
        if (safeAvailable >= aRequestedTake)
        {
            res.GrantedTake = aRequestedTake;
            if (safeAvailable == aRequestedTake)
                RecordDepletion(key, aClaimantPlayerId, aNowMs);
            return res;
        }

        // Case 2: Requested more than currently tracked in `InventoryComponent`.
        const auto it = m_depletions.find(key);
        const bool withinWindow =
            (aConflictWindowMs == 0) ||
            (it != m_depletions.end() && aNowMs >= it->second.DepletedAtMs && (aNowMs - it->second.DepletedAtMs) <= aConflictWindowMs);
        const bool depletedByPeer =
            (it != m_depletions.end()) &&
            (it->second.ClaimantPlayerId != aClaimantPlayerId) &&
            withinWindow;

        if (safeAvailable > 0 || depletedByPeer)
        {
            // Simultaneous QuickLoot / ContainerMenu race OR late-peer re-loot of an already-depleted slot!
            res.GrantedTake = safeAvailable;
            res.OverdrawnRollback = aRequestedTake - safeAvailable;
            if (safeAvailable > 0)
                RecordDepletion(key, aClaimantPlayerId, aNowMs);
            return res;
        }

        // Case 3: `safeAvailable == 0` with no prior peer depletion — dynamic/script-spawned loot not present in initial snapshot.
        // Allow the first player's claim AND stamp the persistent depletion marker so no other peer can dupe it later!
        res.GrantedTake = aRequestedTake;
        res.OverdrawnRollback = 0;
        RecordDepletion(key, aClaimantPlayerId, aNowMs);
        return res;
    }

    template <class TGameId>
        requires requires(const TGameId& id) { id.ModId; id.BaseId; }
    [[nodiscard]] ClaimResolution EvaluateWithdrawal(
        uint32_t aContainerServerId,
        const TGameId& acItemId,
        uint32_t aClaimantPlayerId,
        int32_t aAvailableBefore,
        int32_t aRequestedTake,
        bool aIsQuestItem,
        uint64_t aNowMs,
        uint64_t aConflictWindowMs = kDefaultConflictWindowMs) noexcept
    {
        if (!acItemId)
            return ClaimResolution{aRequestedTake > 0 ? aRequestedTake : 0, 0};
        return EvaluateWithdrawal(
            aContainerServerId,
            acItemId.ModId,
            acItemId.BaseId,
            aClaimantPlayerId,
            aAvailableBefore,
            aRequestedTake,
            aIsQuestItem,
            aNowMs,
            aConflictWindowMs);
    }

private:
    struct DepletionEntry
    {
        uint32_t ClaimantPlayerId{0};
        uint64_t DepletedAtMs{0};
        uint64_t Sequence{0};
    };

    struct FifoItem
    {
        LootSlotKey Key{};
        uint64_t Sequence{0};
    };

    void EvictDeadFrontEntries() noexcept
    {
        while (!m_fifoOrder.empty())
        {
            const auto& front = m_fifoOrder.front();
            const auto it = m_depletions.find(front.Key);
            if (it == m_depletions.end() || it->second.Sequence != front.Sequence)
            {
                m_fifoOrder.pop_front();
                continue;
            }
            break;
        }
    }

    void RecordDepletion(const LootSlotKey& acKey, uint32_t aPlayerId, uint64_t aNowMs)
    {
        const uint64_t seq = m_nextSequence++;
        auto it = m_depletions.find(acKey);
        if (it != m_depletions.end())
        {
            it->second.ClaimantPlayerId = aPlayerId;
            it->second.DepletedAtMs = aNowMs;
            it->second.Sequence = seq;
            m_fifoOrder.push_back(FifoItem{acKey, seq});
            if (m_fifoOrder.size() > kMaxTrackedDepletions * 2)
                EvictDeadFrontEntries();
            return;
        }

        while (m_depletions.size() >= kMaxTrackedDepletions)
        {
            EvictDeadFrontEntries();
            if (m_fifoOrder.empty())
                break;
            m_depletions.erase(m_fifoOrder.front().Key);
            m_fifoOrder.pop_front();
        }

        m_depletions.emplace(acKey, DepletionEntry{aPlayerId, aNowMs, seq});
        m_fifoOrder.push_back(FifoItem{acKey, seq});
    }

    void PruneExpired(uint64_t aNowMs, uint64_t aWindowMs) noexcept
    {
        while (!m_fifoOrder.empty())
        {
            const auto& front = m_fifoOrder.front();
            const auto it = m_depletions.find(front.Key);
            if (it == m_depletions.end() || it->second.Sequence != front.Sequence)
            {
                m_fifoOrder.pop_front();
                continue;
            }

            if (aNowMs >= it->second.DepletedAtMs && (aNowMs - it->second.DepletedAtMs) > aWindowMs)
            {
                m_depletions.erase(it);
                m_fifoOrder.pop_front();
                continue;
            }
            break;
        }
    }

    std::unordered_map<LootSlotKey, DepletionEntry, LootSlotKeyHash> m_depletions{};
    std::deque<FifoItem> m_fifoOrder{};
    uint64_t m_nextSequence{1};
};

/**
 * Bounded, TTL-expiring buffer for container loot deltas fired by QuickLootIE (`LootMenu`) or
 * RFAB AutoLoot (`RFAB_MCM_Settings.json`) BEFORE `AssignObjectsResponse` has assigned a server id
 * to a newly streamed container (since QuickLootIE/AutoLoot call RemoveItem without calling Activate).
 */
template <class TItemEntry>
class PendingContainerLootQueue
{
public:
    static constexpr size_t kMaxPendingDeltas = 256;
    static constexpr uint64_t kDefaultTtlMs = 8000ULL;

    struct PendingDelta
    {
        uint32_t ContainerFormId{0};
        TItemEntry Item{};
        bool Drop{false};
        bool UpdateClients{true};
        uint64_t QueuedAtMs{0};
        // false: the list the server was sent for this container was read BEFORE this change, so
        // the server's copy does not have it (defect M28, ShouldReplayPendingDelta).
        bool ReflectedInRegistration{true};
    };

    void Clear() noexcept { m_entries.clear(); }
    [[nodiscard]] size_t Size() const noexcept { return m_entries.size(); }
    [[nodiscard]] bool Empty() const noexcept { return m_entries.empty(); }

    void PruneExpired(uint64_t aNowMs, uint64_t aTtlMs = kDefaultTtlMs) noexcept
    {
        while (!m_entries.empty())
        {
            const uint64_t queuedAt = m_entries.front().QueuedAtMs;
            if (aNowMs >= queuedAt && (aNowMs - queuedAt) > aTtlMs)
                m_entries.pop_front();
            else
                break;
        }
    }

    bool Enqueue(
        uint32_t aContainerFormId,
        const TItemEntry& acItem,
        bool aDrop,
        bool aUpdateClients,
        uint64_t aNowMs,
        uint64_t aTtlMs = kDefaultTtlMs,
        bool aReflectedInRegistration = true)
    {
        if (aContainerFormId == 0)
            return false;

        PruneExpired(aNowMs, aTtlMs);
        while (m_entries.size() >= kMaxPendingDeltas)
            m_entries.pop_front();

        m_entries.push_back(PendingDelta{aContainerFormId, acItem, aDrop, aUpdateClients, aNowMs, aReflectedInRegistration});
        return true;
    }

    [[nodiscard]] std::vector<PendingDelta> TakeForContainer(
        uint32_t aContainerFormId,
        uint64_t aNowMs,
        uint64_t aTtlMs = kDefaultTtlMs)
    {
        PruneExpired(aNowMs, aTtlMs);
        std::vector<PendingDelta> out;
        if (aContainerFormId == 0 || m_entries.empty())
            return out;

        std::deque<PendingDelta> remaining;
        while (!m_entries.empty())
        {
            PendingDelta entry = std::move(m_entries.front());
            m_entries.pop_front();
            if (entry.ContainerFormId == aContainerFormId)
                out.push_back(std::move(entry));
            else
                remaining.push_back(std::move(entry));
        }
        m_entries = std::move(remaining);
        return out;
    }

private:
    std::deque<PendingDelta> m_entries{};
};

// ---------------------------------------------------------------------------------------------
// A change made while a registration is unanswered (work plan 6.4, defect M28).
//
// The client registers a container with the server in three ways, each sending the container's
// list as the client's game holds it at that moment:
//   - the cell scan, when the player enters a cell, for every container of the cell;
//   - an activation of a container the client holds no server id for;
//   - a take or a put (QuickLootIE's window) in such a container: the change is queued and the
//     list is sent as it is AFTER the change.
// The answer says whether the server built its copy from that list ("you are first") or already
// had one ("not first": the server's list replaces the local one, and queued changes are sent for
// the server to arbitrate). 4.1.0 dropped the queued changes on "you are first", on the ground
// that the list the server was sent already had them.
//
// That holds only for the change that caused the registration. Any change made between a request
// and its answer finds the client still without an id, registers the container a second time and
// is queued. The first answer says "you are first" and drops it, although the list the server
// built its copy from was read BEFORE the change. The second answer says "not first" and writes
// that list over the chest. The item is in the player's pack, back in the chest for everybody,
// and the server will grant it again. A put is lost the same way. It happens when a player
//   - takes an item right after crossing a cell border outdoors (the scan is unanswered);
//   - opens a container the client had not registered and takes an item before the answer;
//   - takes two items in a row from such a container, the second before the first is answered.
//
// The fix: every local inventory change carries a number from one counter, and every registration
// notes the counter when it reads the list. A change with a higher number is not in that list.
// While a registration of a container is unanswered, a change is only queued - no second
// registration - marked with whether the list has it; on "you are first" the changes the list
// lacks are sent instead of dropped.

// How long a registration counts as unanswered. Far above any round trip, below the 8 s a queued
// change lives; after it a change registers the container itself, as in 4.1.0.
inline constexpr uint64_t kRegistrationWindowMs = 3000ULL;

[[nodiscard]] constexpr bool IsRegistrationInFlight(
    uint64_t aSentAtMs, uint64_t aNowMs, uint64_t aWindowMs = kRegistrationWindowMs) noexcept
{
    return aNowMs >= aSentAtMs && (aNowMs - aSentAtMs) <= aWindowMs;
}

// aChangeSequence: the counter value the change was given, just before the game applied it.
// aListSequence: the counter value when the registration read the container's list.
[[nodiscard]] constexpr bool IsChangeReflectedInRegistration(uint64_t aChangeSequence, uint64_t aListSequence) noexcept
{
    return aChangeSequence <= aListSequence;
}

// The answer to a registration arrived; is a queued change sent to the server?
[[nodiscard]] constexpr bool ShouldReplayPendingDelta(bool aIsSenderFirst, bool aReflectedInRegistration) noexcept
{
    if (!aIsSenderFirst)
        return true; // another player's list stands: the server arbitrates the change against it
    // The server's copy was built from this client's list. A change the list already has would be
    // applied twice; a change made after the list was read is missing from the copy.
    return !aReflectedInRegistration;
}

// The containers this client has sent a registration for whose answer has not arrived.
class RegistrationTracker
{
public:
    static constexpr size_t kMaxTracked = 4096;

    struct Lookup
    {
        bool InFlight{false};
        uint64_t Sequence{0};
    };

    void Clear() noexcept { m_sent.clear(); }
    [[nodiscard]] size_t Size() const noexcept { return m_sent.size(); }

    // A registration read this container's list when the change counter stood at aSequence, and
    // sent it. While an earlier registration of the container is unanswered, that one stands: if
    // the server builds its copy from this client's list at all, it is from the earlier list.
    void Note(uint32_t aContainerFormId, uint64_t aNowMs, uint64_t aSequence, uint64_t aWindowMs = kRegistrationWindowMs)
    {
        if (aContainerFormId == 0)
            return;

        const auto it = m_sent.find(aContainerFormId);
        if (it != m_sent.end())
        {
            if (!IsRegistrationInFlight(it->second.SentAtMs, aNowMs, aWindowMs))
                it->second = Sent{aNowMs, aSequence};
            return;
        }

        if (m_sent.size() >= kMaxTracked)
        {
            Prune(aNowMs, aWindowMs);
            if (m_sent.size() >= kMaxTracked)
                return; // more unanswered containers than any scan sends: leave this one to 4.1.0's path
        }

        m_sent.emplace(aContainerFormId, Sent{aNowMs, aSequence});
    }

    // The server answered for this container.
    void Erase(uint32_t aContainerFormId) noexcept { m_sent.erase(aContainerFormId); }

    [[nodiscard]] Lookup Find(
        uint32_t aContainerFormId, uint64_t aNowMs, uint64_t aWindowMs = kRegistrationWindowMs) const noexcept
    {
        const auto it = m_sent.find(aContainerFormId);
        if (it == m_sent.end() || !IsRegistrationInFlight(it->second.SentAtMs, aNowMs, aWindowMs))
            return Lookup{};
        return Lookup{true, it->second.Sequence};
    }

    void Prune(uint64_t aNowMs, uint64_t aWindowMs = kRegistrationWindowMs) noexcept
    {
        for (auto it = m_sent.begin(); it != m_sent.end();)
        {
            if (IsRegistrationInFlight(it->second.SentAtMs, aNowMs, aWindowMs))
                ++it;
            else
                it = m_sent.erase(it);
        }
    }

private:
    struct Sent
    {
        uint64_t SentAtMs{0};
        uint64_t Sequence{0};
    };

    std::unordered_map<uint32_t, Sent> m_sent{};
};

} // namespace CoopLootLedger
