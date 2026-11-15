#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace CoopExperienceLedger
{
inline constexpr uint32_t kMaxRfabXpDelta = 1'000'000u;
inline constexpr uint64_t kQuestXpWindowDurationMs = 4000ULL;
inline constexpr uint64_t kPrematureRemoteXpLookbackMs = 3000ULL;
// How many of the partner's awards, applied while a quest window was open, are remembered.
inline constexpr size_t kMaxRemoteAwardsRememberedInQuestWindow = 4;

/**
 * Do two awards describe the same quest reward? Both players start from the same number in the
 * modpack's Quests.ini; what differs is each player's personal multiplier and the rounding to 5 XP
 * on the way (the sender divides and snaps, the receiver multiplies and floors). Two percent of the
 * larger award, and never less than 5 XP, covers that rounding. An award that differs by more is a
 * different award: a kill that happened to fall into the same seconds is not taken for the quest.
 */
[[nodiscard]] constexpr bool IsSameQuestAward(int32_t aLocalGain, int32_t aRemoteAward) noexcept
{
    if (aLocalGain <= 0 || aRemoteAward <= 0)
        return false;

    const int32_t larger = aLocalGain > aRemoteAward ? aLocalGain : aRemoteAward;
    const int32_t smaller = aLocalGain > aRemoteAward ? aRemoteAward : aLocalGain;
    const int32_t tolerance = larger / 50 > 5 ? larger / 50 : 5;
    return (larger - smaller) <= tolerance;
}

/**
 * Normalizes a local RFAB XP gain by dividing out the sender's personal RFAB_XP_Multiplier
 * (e.g. Adventurer's Ring / Standing Stone) and rounding Requiem's >= 5 awards to the nearest 5,
 * so each party peer scales from an exact 1x base award using their own personal multiplier.
 */
[[nodiscard]] inline int32_t NormalizeSenderBaseXp(int32_t aLocalDelta, float aSenderMultiplier) noexcept
{
    if (aLocalDelta <= 0)
        return 0;

    int32_t baseDelta = aLocalDelta;
    if (std::isfinite(aSenderMultiplier) && aSenderMultiplier > 0.01f && std::fabs(aSenderMultiplier - 1.0f) > 0.01f)
    {
        baseDelta = static_cast<int32_t>(std::lround(static_cast<double>(aLocalDelta) / static_cast<double>(aSenderMultiplier)));
        if (baseDelta >= 5)
            baseDelta = ((baseDelta + 2) / 5) * 5;
    }

    return baseDelta > 0 ? baseDelta : 0;
}

/**
 * Scales an incoming 1x base RFAB XP packet by the receiver's personal RFAB_XP_Multiplier,
 * snapping positive scaled grants to Requiem's 5-XP granularity with a minimum floor of 5 XP.
 */
[[nodiscard]] inline int32_t ScaleReceiverPersonalXp(uint32_t aRawBaseXp, float aReceiverMultiplier) noexcept
{
    if (aRawBaseXp == 0 || aRawBaseXp > kMaxRfabXpDelta)
        return 0;

    const int32_t rawIncoming = static_cast<int32_t>(aRawBaseXp);
    int32_t delta = rawIncoming;
    if (std::isfinite(aReceiverMultiplier) && aReceiverMultiplier > 0.0f && std::fabs(aReceiverMultiplier - 1.0f) > 0.01f)
    {
        const int32_t scaled = static_cast<int32_t>(static_cast<float>(rawIncoming) * aReceiverMultiplier);
        delta = scaled - (scaled % 5);
        if (delta <= 0 && rawIncoming > 0)
            delta = 5;
    }

    return delta > 0 ? delta : 0;
}

/**
 * Deterministic client-side ledger for bidirectional RFAB XP synchronization:
 * - Suppresses wallet baseline churn while CustomSkills / RFAB_Menu ("CustomMenu") is open.
 * - Deduplicates quest stage XP across the packet arrival orders. The modpack pays quest XP from
 *   the quest's stage fragment, so it is paid on every client that applies the stage, and the wallet
 *   sharing would pay it a second time. With S = the synced stage is applied here, L = the local
 *   script's award is seen by the wallet poll, R = the partner's copy of the award arrives:
 *   (A) S, L, R -> R is absorbed by the local award already seen.
 *   (B) R, S, L -> R was applied early; it is taken back when L arrives.
 *   (C) S, R, L -> R is applied (nothing local to absorb it yet) and remembered; when L arrives with
 *       the same amount, R is taken back. 4.1.0 did not have this order and paid both. It is as
 *       likely as (A): R and L leave the two clients within the same fraction of a second.
 *       Controlled by SetNetRemoteAwardsInsideQuestWindow, off by default in this class.
 */
class ClientRfabXpTracker
{
public:
    struct LocalPollResult
    {
        int32_t RefundToApply{0};
        uint32_t BroadcastBaseXp{0};
    };

    void Reset() noexcept
    {
        m_initialized = false;
        m_remoteQuestXpWindowUntilMs = 0;
        m_hasRemoteQuestXpWindow = false;
        m_localQuestXpGainedInWindow = 0;
        m_lastAppliedRemoteXpAmount = 0;
        m_lastAppliedRemoteXpTimeMs = 0;
        m_hasLastAppliedRemoteXp = false;
        m_creditToRefundOnLocalQuestXp = 0;
        m_remoteAwardCountInWindow = 0;
    }

    // Order (C) of the class comment. Off, the tracker behaves exactly as in 4.1.0.
    void SetNetRemoteAwardsInsideQuestWindow(bool aEnabled) noexcept
    {
        m_netRemoteAwardsInsideQuestWindow = aEnabled;
        if (!aEnabled)
            m_remoteAwardCountInWindow = 0;
    }

    void OnRemoteQuestStageApplied(uint64_t aNowMs) noexcept
    {
        m_remoteQuestXpWindowUntilMs = aNowMs + kQuestXpWindowDurationMs;
        m_hasRemoteQuestXpWindow = true;

        if (m_hasLastAppliedRemoteXp &&
            m_lastAppliedRemoteXpAmount > 0 &&
            aNowMs >= m_lastAppliedRemoteXpTimeMs &&
            (aNowMs - m_lastAppliedRemoteXpTimeMs) <= kPrematureRemoteXpLookbackMs)
        {
            m_creditToRefundOnLocalQuestXp += m_lastAppliedRemoteXpAmount;
            m_lastAppliedRemoteXpAmount = 0;
            m_hasLastAppliedRemoteXp = false;
        }
    }

    [[nodiscard]] int32_t EvaluateIncomingRemoteXp(
        uint32_t aRawExperience,
        float aReceiverMultiplier,
        uint64_t aNowMs) noexcept
    {
        int32_t delta = ScaleReceiverPersonalXp(aRawExperience, aReceiverMultiplier);
        if (delta <= 0)
            return 0;

        if (IsInQuestWindow(aNowMs) && m_localQuestXpGainedInWindow > 0)
        {
            const int32_t absorbed = std::min(delta, m_localQuestXpGainedInWindow);
            m_localQuestXpGainedInWindow -= absorbed;
            delta -= absorbed;
            if (delta <= 0)
                return 0;
        }

        return delta;
    }

    void CommitAppliedRemoteXp(int32_t aCurrentWalletBefore, int32_t aAppliedDelta, uint64_t aNowMs) noexcept
    {
        if (!m_initialized)
        {
            m_lastWallet = aCurrentWalletBefore;
            m_initialized = true;
        }

        m_lastWallet += aAppliedDelta;

        if (m_netRemoteAwardsInsideQuestWindow && aAppliedDelta > 0 && IsInQuestWindow(aNowMs))
        {
            // Order (C): the stage is already applied here and the partner's award came before
            // the local script paid. It is remembered for the local award, and for nothing else:
            // a second update of the same quest (a stage and then "stopped" is the usual pair)
            // must not count it once more through the look-back of order (B).
            RememberRemoteAwardInWindow(aAppliedDelta, aNowMs);
            m_lastAppliedRemoteXpAmount = 0;
            m_hasLastAppliedRemoteXp = false;
            return;
        }

        m_lastAppliedRemoteXpAmount = aAppliedDelta;
        m_lastAppliedRemoteXpTimeMs = aNowMs;
        m_hasLastAppliedRemoteXp = true;
    }

    void CommitAuthoritativeDelta(int32_t aNewWalletTotal, int32_t aAppliedDelta, uint64_t aNowMs) noexcept
    {
        m_lastWallet = aNewWalletTotal;
        m_initialized = true;
        m_lastAppliedRemoteXpAmount = aAppliedDelta;
        m_lastAppliedRemoteXpTimeMs = aNowMs;
        m_hasLastAppliedRemoteXp = true;
    }

    [[nodiscard]] LocalPollResult EvaluateLocalWalletPoll(
        int32_t aCurrentWallet,
        bool aIsConnected,
        bool aIsCustomSkillMenuOpen,
        float aSenderMultiplier,
        uint64_t aNowMs) noexcept
    {
        LocalPollResult out{};
        if (!aIsConnected)
        {
            m_initialized = false;
            return out;
        }

        if (!IsInQuestWindow(aNowMs))
        {
            m_localQuestXpGainedInWindow = 0;
            m_creditToRefundOnLocalQuestXp = 0;
            m_remoteAwardCountInWindow = 0;
        }

        if (!m_initialized)
        {
            m_lastWallet = aCurrentWallet;
            m_initialized = true;
            return out;
        }

        if (aIsCustomSkillMenuOpen)
        {
            m_lastWallet = aCurrentWallet;
            return out;
        }

        int32_t delta = aCurrentWallet - m_lastWallet;
        m_lastWallet = aCurrentWallet;

        if (delta <= 0)
            return out;

        if (IsInQuestWindow(aNowMs))
        {
            if (m_creditToRefundOnLocalQuestXp > 0)
            {
                const int32_t refund = std::min(delta, m_creditToRefundOnLocalQuestXp);
                out.RefundToApply = refund;
                delta -= refund;
            }

            if (delta > 0)
            {
                if (const int32_t remoteAward = TakeRemoteAwardMatching(delta, aNowMs); remoteAward > 0)
                {
                    // Order (C): the partner's copy of this award was applied first. Taking that
                    // copy back leaves exactly what the local quest script paid. The local award is
                    // settled by it and absorbs no later packet.
                    out.RefundToApply += remoteAward;
                    delta = 0;
                }
            }

            if (delta > 0)
                m_localQuestXpGainedInWindow += delta;

            return out;
        }

        const int32_t baseDelta = NormalizeSenderBaseXp(delta, aSenderMultiplier);
        if (baseDelta > 0)
        {
            out.BroadcastBaseXp = static_cast<uint32_t>(
                std::min<int32_t>(baseDelta, static_cast<int32_t>(kMaxRfabXpDelta)));
        }

        return out;
    }

    void CommitLocalRefundApplied(int32_t aRefundAmount, int32_t aAdjustedWalletTotal) noexcept
    {
        if (aRefundAmount <= 0)
            return;

        m_creditToRefundOnLocalQuestXp = std::max(0, m_creditToRefundOnLocalQuestXp - aRefundAmount);
        m_lastWallet = aAdjustedWalletTotal;
    }

    [[nodiscard]] bool IsInitialized() const noexcept { return m_initialized; }
    [[nodiscard]] int32_t GetLastWallet() const noexcept { return m_lastWallet; }
    [[nodiscard]] int32_t GetLocalQuestXpGainedInWindow() const noexcept { return m_localQuestXpGainedInWindow; }
    [[nodiscard]] int32_t GetCreditToRefundOnLocalQuestXp() const noexcept { return m_creditToRefundOnLocalQuestXp; }
    [[nodiscard]] size_t GetRemoteAwardsRememberedInWindow() const noexcept { return m_remoteAwardCountInWindow; }

private:
    struct RemoteAward
    {
        int32_t Amount{0};
        uint64_t AppliedAtMs{0};
    };

    [[nodiscard]] bool IsInQuestWindow(uint64_t aNowMs) const noexcept
    {
        return m_hasRemoteQuestXpWindow && aNowMs <= m_remoteQuestXpWindowUntilMs;
    }

    void RememberRemoteAwardInWindow(int32_t aAmount, uint64_t aNowMs) noexcept
    {
        if (m_remoteAwardCountInWindow == kMaxRemoteAwardsRememberedInQuestWindow)
        {
            // Full: the oldest award is the least likely to still meet its local twin.
            for (size_t i = 1; i < m_remoteAwardCountInWindow; ++i)
                m_remoteAwardsInWindow[i - 1] = m_remoteAwardsInWindow[i];
            --m_remoteAwardCountInWindow;
        }
        m_remoteAwardsInWindow[m_remoteAwardCountInWindow++] = RemoteAward{aAmount, aNowMs};
    }

    // Removes and returns the remembered award that is the same quest reward as `aLocalGain`,
    // or 0. An award older than one window is not a candidate even if later stage updates kept
    // the window open.
    [[nodiscard]] int32_t TakeRemoteAwardMatching(int32_t aLocalGain, uint64_t aNowMs) noexcept
    {
        for (size_t i = 0; i < m_remoteAwardCountInWindow; ++i)
        {
            const RemoteAward award = m_remoteAwardsInWindow[i];
            const bool recent = aNowMs >= award.AppliedAtMs && (aNowMs - award.AppliedAtMs) <= kQuestXpWindowDurationMs;
            if (!recent || !IsSameQuestAward(aLocalGain, award.Amount))
                continue;

            for (size_t j = i + 1; j < m_remoteAwardCountInWindow; ++j)
                m_remoteAwardsInWindow[j - 1] = m_remoteAwardsInWindow[j];
            --m_remoteAwardCountInWindow;
            return award.Amount;
        }
        return 0;
    }

    int32_t m_lastWallet{0};
    bool m_initialized{false};
    uint64_t m_remoteQuestXpWindowUntilMs{0};
    bool m_hasRemoteQuestXpWindow{false};
    int32_t m_localQuestXpGainedInWindow{0};
    int32_t m_lastAppliedRemoteXpAmount{0};
    uint64_t m_lastAppliedRemoteXpTimeMs{0};
    bool m_hasLastAppliedRemoteXp{false};
    int32_t m_creditToRefundOnLocalQuestXp{0};
    bool m_netRemoteAwardsInsideQuestWindow{false};
    RemoteAward m_remoteAwardsInWindow[kMaxRemoteAwardsRememberedInQuestWindow]{};
    size_t m_remoteAwardCountInWindow{0};
};
} // namespace CoopExperienceLedger
