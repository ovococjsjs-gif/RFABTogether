#pragma once

/**
 * NPC watch: do the enemies around the players behave like enemies?
 *
 * Playtest 2026-11-15: "enemies do not fight, they walk around with their weapons sheathed". The
 * logs of that session could not say why, because nothing in the client records what an NPC is
 * doing. There are three different failures behind the same picture, and each needs a different
 * fix, so the client now counts them apart (Code/client/Services/Generic/CombatService.cpp):
 *
 *   own, hostile, idle       this client simulates the NPC, the NPC is hostile to a player who
 *                            stands next to it, and it does not fight: the game's own AI does not
 *                            start the fight (detection, aggression, a combat state we reset).
 *   remote, hostile, idle    another client simulates the NPC; here it is shown next to a player
 *                            with its weapon sheathed: either its owner has the first failure, or
 *                            the fight is not transmitted. The owner's log tells which.
 *   unowned, hostile         nobody was given the NPC: every client runs its own copy and the
 *                            players see different fights.
 *   not hostile              the NPC does not consider any player an enemy at all (factions).
 *                            Only counted: most NPCs near players are legitimately peaceful.
 *
 * Both players' logs together name the failure. This header is the counting and the pacing of the
 * log lines, tested in Code/tests/rfab_v412_hang_watch.cpp.
 */

#include <cstdint>

namespace Diag
{

enum class NpcOwner : uint8_t
{
    kHere = 0,      // this client simulates it
    kElsewhere = 1, // another client simulates it, this one shows it
    kNobody = 2,    // the server has not given it to anyone (yet)
};

struct NpcObservation
{
    NpcOwner Owner{NpcOwner::kNobody};
    bool HostileToAPlayer{false};
    bool InCombat{false};
    bool WeaponDrawn{false};
    // A player who is not sneaking stands within reach: an enemy would be fighting by now.
    bool PlayerInPlainSightRange{false};
};

struct NpcTally
{
    uint32_t Total{0};
    uint32_t OwnedHere{0};
    uint32_t OwnedHereInCombat{0};
    uint32_t OwnedElsewhere{0};
    uint32_t OwnedElsewhereArmed{0};
    uint32_t Unowned{0};
    uint32_t Hostile{0};

    uint32_t IdleHostileHere{0};
    uint32_t IdleHostileElsewhere{0};
    uint32_t HostileUnowned{0};

    // Returns true when this NPC is one of the three failures (the caller may log it as an example).
    constexpr bool Add(const NpcObservation& acNpc) noexcept
    {
        ++Total;
        if (acNpc.HostileToAPlayer)
            ++Hostile;

        switch (acNpc.Owner)
        {
        case NpcOwner::kHere:
            ++OwnedHere;
            if (acNpc.InCombat)
                ++OwnedHereInCombat;
            if (acNpc.HostileToAPlayer && acNpc.PlayerInPlainSightRange && !acNpc.InCombat)
            {
                ++IdleHostileHere;
                return true;
            }
            return false;
        case NpcOwner::kElsewhere:
            ++OwnedElsewhere;
            if (acNpc.WeaponDrawn)
                ++OwnedElsewhereArmed;
            // A remote actor has no combat state of its own here: its drawn weapon is what shows.
            if (acNpc.HostileToAPlayer && acNpc.PlayerInPlainSightRange && !acNpc.WeaponDrawn)
            {
                ++IdleHostileElsewhere;
                return true;
            }
            return false;
        case NpcOwner::kNobody:
            ++Unowned;
            if (acNpc.HostileToAPlayer && acNpc.PlayerInPlainSightRange)
            {
                ++HostileUnowned;
                return true;
            }
            return false;
        }
        return false;
    }

    [[nodiscard]] constexpr uint32_t Anomalies() const noexcept { return IdleHostileHere + IdleHostileElsewhere + HostileUnowned; }
};

enum class NpcReport : uint8_t
{
    kNone = 0,
    kSummary = 1,  // the regular line: how many NPCs, who owns them
    kAnomaly = 2,  // enemies that stand idle next to a player, long enough to be no coincidence
};

struct NpcReportPolicy
{
    uint64_t SummaryEveryMs{60'000};
    // An enemy needs a moment to notice a player; only an anomaly that lasts is reported.
    uint64_t AnomalyAfterMs{8'000};
    uint64_t AnomalyRepeatMs{30'000};
};

class NpcReportPacer
{
public:
    [[nodiscard]] constexpr NpcReport Evaluate(uint64_t aNowMs, uint32_t aTotal, uint32_t aAnomalies, const NpcReportPolicy& acPolicy = NpcReportPolicy{}) noexcept
    {
        if (aAnomalies == 0)
        {
            m_anomalySinceMs = 0;
        }
        else
        {
            if (m_anomalySinceMs == 0)
                m_anomalySinceMs = aNowMs == 0 ? 1 : aNowMs;

            const bool lasted = aNowMs - m_anomalySinceMs >= acPolicy.AnomalyAfterMs;
            const bool due = m_lastAnomalyReportMs == 0 || aNowMs - m_lastAnomalyReportMs >= acPolicy.AnomalyRepeatMs;
            if (lasted && due)
            {
                m_lastAnomalyReportMs = aNowMs == 0 ? 1 : aNowMs;
                m_lastSummaryMs = m_lastAnomalyReportMs;  // an anomaly line carries the summary too
                return NpcReport::kAnomaly;
            }
        }

        // Nothing around: nothing to say.
        if (aTotal == 0)
            return NpcReport::kNone;

        if (m_lastSummaryMs == 0 || aNowMs - m_lastSummaryMs >= acPolicy.SummaryEveryMs)
        {
            m_lastSummaryMs = aNowMs == 0 ? 1 : aNowMs;
            return NpcReport::kSummary;
        }
        return NpcReport::kNone;
    }

    constexpr void Reset() noexcept
    {
        m_anomalySinceMs = 0;
        m_lastAnomalyReportMs = 0;
        m_lastSummaryMs = 0;
    }

private:
    uint64_t m_anomalySinceMs{0};
    uint64_t m_lastAnomalyReportMs{0};
    uint64_t m_lastSummaryMs{0};
};

} // namespace Diag
