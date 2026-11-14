#pragma once

/**
 * Desync detector (work plan, package 3.3).
 *
 * WHY THIS EXISTS
 * The self-healing contract (Session/Resync.h) repairs a loss the CLIENT notices: an unknown id in
 * a movement frame, a spawn dropped while loading. A client cannot notice what it never hears
 * about: an actor that stands still and was never spawned here, an actor the server removed whose
 * removal was lost, an actor that is dead on the server and walks around on this screen. Those
 * stayed wrong for the rest of the session, and the only trace was a player saying "he sees a
 * bandit and I do not".
 *
 * CONTRACT
 *  - Every few seconds the client sends `ClientEntityDigest`: one `Facts` row per character it
 *    knows by server id (remote copies and the ones it simulates itself).
 *  - The server builds the same rows from its own state for that observer and gives both lists to
 *    `Comparer`. A difference becomes a `Finding` only when a LATER report (at least
 *    `ConfirmAfterMs` after the first) still shows it: a spawn, a removal or a death that was in
 *    flight when the report was written is over by then.
 *  - Every confirmed finding is logged with the entity and the field ("[RFAB Desync] ..."). That
 *    line is the product: it is what one playtest brings back.
 *  - A finding may also be repaired by the resend the server already knows how to build
 *    (`CharacterService::ResyncEntityForObserver`). Repair is bounded per entity, and only offered
 *    for presence and life state (see `IsRepairable`).
 *
 * What is compared, and for whom:
 *    presence   - every character the server believes this client has, and every one it reports;
 *    owner      - "the reporter simulates it" against the server's owner;
 *    life state - for characters the reporter does not simulate and has a live actor for;
 *    health     - same, as a percentage of the maximum, beyond `HealthTolerancePct`; logged only;
 *    equipment  - for player characters only (NPC outfits are equipped by the engine on every
 *                 client and differ harmlessly); logged, never repaired.
 * When the reporter simulates a character itself it IS the source of its life state and health, so
 * those are not compared for it.
 *
 * THREADING: `Comparer` is single-threaded (server tick), one instance per observer.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace Session::EntityDigest
{

// A report larger than this is cut and marked truncated: the server then cannot tell "missing"
// from "did not fit" and skips the presence check for that report.
inline constexpr size_t kMaxEntries = 192;
inline constexpr uint8_t kNoHealth = 0xFF;

enum Flag : uint8_t
{
    kFlagDead = 1u << 0,
    kFlagOwnedByReporter = 1u << 1,  // client: it simulates the character; server: the observer owns it
    kFlagLive = 1u << 2,             // client: a live actor exists here, life state and health are real
    kFlagHasEquipment = 1u << 3,     // EquipmentHash is meaningful (player characters only)
    kFlagPlayer = 1u << 4,           // a player character
};
inline constexpr uint8_t kKnownFlags = 0x1F;

struct Facts
{
    uint32_t ServerId{0};
    uint8_t Flags{0};
    uint8_t HealthPct{kNoHealth};  // 0..100, kNoHealth when unknown
    uint32_t EquipmentHash{0};

    [[nodiscard]] constexpr bool Has(Flag aFlag) const noexcept { return (Flags & aFlag) != 0; }
};

enum class Field : uint8_t
{
    kMissingOnClient = 0,  // the server believes the client has the character; the client does not list it
    kUnknownToServer = 1,  // the client lists a character the server does not expect it to have
    kOwner = 2,
    kLifeState = 3,
    kHealth = 4,
    kEquipment = 5,
};
inline constexpr uint8_t kFieldCount = 6;

[[nodiscard]] constexpr const char* ToString(Field aField) noexcept
{
    switch (aField)
    {
    case Field::kMissingOnClient: return "missing-on-client";
    case Field::kUnknownToServer: return "unknown-to-server";
    case Field::kOwner: return "owner";
    case Field::kLifeState: return "life-state";
    case Field::kHealth: return "health";
    case Field::kEquipment: return "equipment";
    }
    return "?";
}

// A resend of the character (or its removal) fixes presence and life state. The rest is only said:
//  - an owner disagreement is not fixed by a snapshot (the client would build a second copy next
//    to the one it simulates);
//  - health has its own authority and its own corrections (Authority/Health.h), and a resend in
//    the middle of a fight moves the actor;
//  - a player's equipment is compared before anyone has seen the comparison work in the game.
[[nodiscard]] constexpr bool IsRepairable(Field aField) noexcept
{
    return aField == Field::kMissingOnClient || aField == Field::kUnknownToServer || aField == Field::kLifeState;
}

// Health as the digest carries it. Both ends call this with their own current / maximum values.
[[nodiscard]] constexpr uint8_t HealthPercent(float aCurrent, float aMaximum) noexcept
{
    if (!(aMaximum > 0.0f) || !(aCurrent == aCurrent))  // also rejects NaN
        return kNoHealth;
    const float ratio = aCurrent / aMaximum;
    if (!(ratio > 0.0f))
        return 0;
    if (ratio >= 1.0f)
        return 100;
    return static_cast<uint8_t>(ratio * 100.0f + 0.5f);
}

// Hash of the SET of worn items: `Add` once per worn inventory entry with its (mod id, base id),
// in any order; the same item listed twice (dual wielding, a stack split by tempering) counts once,
// because the two ends do not split stacks the same way. Never returns 0, which means "not reported".
class EquipmentHasher
{
public:
    void Add(uint32_t aModId, uint32_t aBaseId) { m_keys.push_back((static_cast<uint64_t>(aModId) << 32) | aBaseId); }

    [[nodiscard]] uint32_t Finish()
    {
        std::sort(m_keys.begin(), m_keys.end());
        m_keys.erase(std::unique(m_keys.begin(), m_keys.end()), m_keys.end());

        uint64_t hash = 14695981039346656037ULL ^ m_keys.size();
        for (const uint64_t key : m_keys)
        {
            for (int shift = 0; shift < 64; shift += 8)
            {
                hash ^= (key >> shift) & 0xFFULL;
                hash *= 1099511628211ULL;
            }
        }
        const uint32_t folded = static_cast<uint32_t>(hash ^ (hash >> 32));
        return folded == 0 ? 1u : folded;
    }

    [[nodiscard]] size_t Count() const noexcept { return m_keys.size(); }

private:
    std::vector<uint64_t> m_keys{};
};

struct Policy
{
    // A difference counts once a later report, at least this long after the first, repeats it.
    uint64_t ConfirmAfterMs{4000};
    // The same (entity, field) is written to the log at most this often.
    uint64_t LogRepeatMs{60000};
    uint8_t HealthTolerancePct{25};
    // A character is resent at most this many times per window; after that the difference is
    // something a resend does not fix, and repeating it would only make the actor flicker.
    uint32_t MaxRepairsPerWindow{2};
    uint64_t RepairWindowMs{120000};
    size_t MaxTracked{512};
};

struct Finding
{
    uint32_t ServerId{0};
    Field What{Field::kMissingOnClient};
    uint32_t ClientValue{0};
    uint32_t ServerValue{0};
    uint64_t AgeMs{0};      // how long the difference has been reported
    bool Log{false};        // write it now
    bool Repair{false};     // a resend is allowed now (the caller still applies its own switch and budget)
    bool GaveUp{false};     // repairs for this entity are exhausted for the window; said once
    bool Healed{false};     // the difference was resent and is gone from this report
};

struct Stats
{
    uint64_t Reports{0};
    uint64_t Suspected{0};
    uint64_t Cleared{0};  // suspicions that were gone in the next report (in-flight state, not a desync)
    std::array<uint64_t, kFieldCount> Confirmed{};
    uint64_t RepairsOffered{0};
    uint64_t Healed{0};   // differences that were gone in the report after a resend
    uint64_t GaveUp{0};
};

class Comparer
{
public:
    explicit Comparer(Policy aPolicy = {}) noexcept
        : m_policy(aPolicy)
    {
    }

    /**
     * `acClient` is the report, `acServer` what the server holds for the same observer. Neither
     * needs to be sorted; a server id listed twice counts once (the first row wins).
     * `aClientTruncated`: the report did not fit, so absence from it proves nothing.
     * Confirmed findings are appended to `aOut`.
     */
    void Compare(const std::vector<Facts>& acClient, const std::vector<Facts>& acServer, bool aClientTruncated,
                 uint64_t aNowMs, std::vector<Finding>& aOut)
    {
        ++m_stats.Reports;
        ++m_generation;

        m_clientIndex.clear();
        for (const Facts& facts : acClient)
            m_clientIndex.try_emplace(facts.ServerId, facts);

        m_serverIndex.clear();
        for (const Facts& facts : acServer)
            m_serverIndex.try_emplace(facts.ServerId, facts);

        for (const auto& [serverId, server] : m_serverIndex)
        {
            const auto clientIt = m_clientIndex.find(serverId);
            if (clientIt == m_clientIndex.end())
            {
                if (!aClientTruncated)
                    Observe(serverId, Field::kMissingOnClient, 0, server.Flags, aNowMs, aOut);
                continue;
            }
            const Facts& client = clientIt->second;

            const bool clientOwns = client.Has(kFlagOwnedByReporter);
            const bool serverSaysOwns = server.Has(kFlagOwnedByReporter);
            if (clientOwns != serverSaysOwns)
            {
                Observe(serverId, Field::kOwner, clientOwns ? 1u : 0u, serverSaysOwns ? 1u : 0u, aNowMs, aOut);
                continue;  // the two ends do not agree on who the source is: nothing else is comparable
            }
            if (clientOwns)
                continue;  // the reporter is the source of this character's state

            if (!client.Has(kFlagLive))
                continue;  // known here, but no actor: nothing on this screen can be wrong

            const bool clientDead = client.Has(kFlagDead);
            const bool serverDead = server.Has(kFlagDead);
            if (clientDead != serverDead)
            {
                Observe(serverId, Field::kLifeState, clientDead ? 1u : 0u, serverDead ? 1u : 0u, aNowMs, aOut);
                continue;  // health of a body is not worth a second line
            }

            if (!clientDead && client.HealthPct != kNoHealth && server.HealthPct != kNoHealth)
            {
                const int difference = static_cast<int>(client.HealthPct) - static_cast<int>(server.HealthPct);
                if (difference > m_policy.HealthTolerancePct || -difference > m_policy.HealthTolerancePct)
                    Observe(serverId, Field::kHealth, client.HealthPct, server.HealthPct, aNowMs, aOut);
            }

            if (client.Has(kFlagHasEquipment) && server.Has(kFlagHasEquipment) && client.EquipmentHash != server.EquipmentHash)
                Observe(serverId, Field::kEquipment, client.EquipmentHash, server.EquipmentHash, aNowMs, aOut);
        }

        for (const auto& [serverId, client] : m_clientIndex)
        {
            if (m_serverIndex.find(serverId) == m_serverIndex.end())
                Observe(serverId, Field::kUnknownToServer, client.Flags, 0, aNowMs, aOut);
        }

        // Whatever was suspected and is not in this report any more was state in flight.
        for (auto it = m_tracked.begin(); it != m_tracked.end();)
        {
            Tracked& tracked = it->second;
            if (tracked.Generation == m_generation)
            {
                ++it;
                continue;
            }
            if (tracked.Active && tracked.Repairs != 0)
            {
                Finding healed{};
                healed.ServerId = static_cast<uint32_t>(it->first >> 8);
                healed.What = static_cast<Field>(it->first & 0xFF);
                healed.AgeMs = aNowMs - tracked.FirstSeenMs;
                healed.Log = true;
                healed.Healed = true;
                aOut.push_back(healed);
                ++m_stats.Healed;
            }
            else if (tracked.Active && !tracked.Confirmed)
            {
                ++m_stats.Cleared;
            }
            // The repair count outlives the difference for the rest of its window, or a difference
            // that comes back after every resend would never run out of resends.
            if (tracked.Repairs != 0 && aNowMs < tracked.RepairWindowStartMs + m_policy.RepairWindowMs)
            {
                tracked.Active = false;
                tracked.Confirmed = false;
                ++it;
                continue;
            }
            it = m_tracked.erase(it);
        }
    }

    // The caller did resend the character for this finding.
    void NoteRepaired(uint32_t aServerId, Field aField, uint64_t aNowMs) noexcept
    {
        const auto it = m_tracked.find(KeyOf(aServerId, aField));
        if (it == m_tracked.end())
            return;
        Tracked& tracked = it->second;
        if (tracked.Repairs == 0 || aNowMs >= tracked.RepairWindowStartMs + m_policy.RepairWindowMs)
        {
            tracked.RepairWindowStartMs = aNowMs;
            tracked.Repairs = 0;
            tracked.SaidGaveUp = false;
        }
        ++tracked.Repairs;
        // The next confirmation needs a fresh period: the resend has to arrive and be applied.
        tracked.FirstSeenMs = aNowMs;
        tracked.Confirmed = false;
    }

    void Reset() noexcept { m_tracked.clear(); }

    [[nodiscard]] const Stats& GetStats() const noexcept { return m_stats; }
    [[nodiscard]] size_t TrackedCount() const noexcept { return m_tracked.size(); }

private:
    struct Tracked
    {
        uint64_t FirstSeenMs{0};
        uint64_t LastLoggedMs{0};
        uint64_t RepairWindowStartMs{0};
        uint64_t Generation{0};
        uint32_t Repairs{0};
        bool Active{true};
        bool Confirmed{false};
        bool Logged{false};
        bool SaidGaveUp{false};
    };

    [[nodiscard]] static constexpr uint64_t KeyOf(uint32_t aServerId, Field aField) noexcept
    {
        return (static_cast<uint64_t>(aServerId) << 8) | static_cast<uint8_t>(aField);
    }

    void Observe(uint32_t aServerId, Field aField, uint32_t aClientValue, uint32_t aServerValue, uint64_t aNowMs,
                 std::vector<Finding>& aOut)
    {
        const uint64_t key = KeyOf(aServerId, aField);
        auto it = m_tracked.find(key);
        if (it == m_tracked.end())
        {
            if (m_tracked.size() >= m_policy.MaxTracked)
                return;  // bounded: with this many open differences one more row adds nothing
            Tracked fresh{};
            fresh.FirstSeenMs = aNowMs;
            fresh.Generation = m_generation;
            m_tracked.emplace(key, fresh);
            ++m_stats.Suspected;
            return;
        }

        Tracked& tracked = it->second;
        tracked.Generation = m_generation;
        if (!tracked.Active)
        {
            // It was gone and is back: a new suspicion, with the old repair count.
            tracked.Active = true;
            tracked.FirstSeenMs = aNowMs;
            ++m_stats.Suspected;
            return;
        }
        if (aNowMs < tracked.FirstSeenMs + m_policy.ConfirmAfterMs)
            return;

        Finding finding{};
        finding.ServerId = aServerId;
        finding.What = aField;
        finding.ClientValue = aClientValue;
        finding.ServerValue = aServerValue;
        finding.AgeMs = aNowMs - tracked.FirstSeenMs;

        if (!tracked.Confirmed)
        {
            tracked.Confirmed = true;
            ++m_stats.Confirmed[static_cast<uint8_t>(aField) < kFieldCount ? static_cast<uint8_t>(aField) : 0];
        }

        if (!tracked.Logged || aNowMs >= tracked.LastLoggedMs + m_policy.LogRepeatMs)
        {
            tracked.Logged = true;
            tracked.LastLoggedMs = aNowMs;
            finding.Log = true;
        }

        if (IsRepairable(aField))
        {
            const bool windowOver = tracked.Repairs != 0 && aNowMs >= tracked.RepairWindowStartMs + m_policy.RepairWindowMs;
            const uint32_t repairs = windowOver ? 0u : tracked.Repairs;
            if (repairs < m_policy.MaxRepairsPerWindow)
            {
                finding.Repair = true;
                ++m_stats.RepairsOffered;
            }
            else if (!tracked.SaidGaveUp)
            {
                tracked.SaidGaveUp = true;
                finding.GaveUp = true;
                finding.Log = true;
                ++m_stats.GaveUp;
            }
        }

        if (finding.Log || finding.Repair)
            aOut.push_back(finding);
    }

    Policy m_policy;
    Stats m_stats{};
    uint64_t m_generation{0};
    std::unordered_map<uint64_t, Tracked> m_tracked{};
    std::unordered_map<uint32_t, Facts> m_clientIndex{};
    std::unordered_map<uint32_t, Facts> m_serverIndex{};
};

} // namespace Session::EntityDigest
