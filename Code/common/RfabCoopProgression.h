#pragma once

/**
 * Pure, deterministic RFAB co-op progression & session-parity helpers (zero Skyrim engine deps):
 *  1. DragonSoulLedger: shares slain dragon souls 1-for-1 across all party members without double-grants.
 *  2. WordWallLedger: shares learned Words of Power (TESWordOfPower GameId) across party & late joiners.
 *  3. DifficultyHandshake: validates RFAB_StartMenu Difficulty (Странник / Довакин / Легенда) &
 *     Cursed (Проклятый) parity between Host and Guest with localized mismatch warnings.
 *  4. QuestsIniDedupe: canonical UTF-8 / Cyrillic-safe keys for RFAB Quests.ini XP deduplication.
 *  5. LateJoinerCatchUp: bounded, idempotent catch-up XP calculation for friends joining mid-session.
 */

#include <Structs/GameId.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace RfabCoopProgression
{

// ---------------------------------------------------------------------------
// 1. Shared Dragon Souls Ledger
// ---------------------------------------------------------------------------

class DragonSoulLedger
{
public:
    struct AwardResult
    {
        bool IsNewDragonKill{false};
        int32_t TotalPartyDragonKills{0};
        std::vector<uint32_t> NewlyAwardedPlayerIds{};
    };

    void Clear() noexcept
    {
        m_slainDragonKeys.clear();
        m_orderedDragonKeys.clear();
        m_awardedPlayersByDragon.clear();
    }

    [[nodiscard]] size_t GetSlainDragonCount() const noexcept { return m_orderedDragonKeys.size(); }
    [[nodiscard]] const std::vector<std::string>& GetOrderedDragonKeys() const noexcept { return m_orderedDragonKeys; }

    // Canonicalizes a dragon kill identity from either persistent FormId (ModId:BaseId) or server entity id.
    [[nodiscard]] static std::string MakeDragonKey(const GameId& acDragonFormId, uint32_t aFallbackServerId)
    {
        if (acDragonFormId && acDragonFormId.ModId != 0xFFFFFFFFu)
        {
            char buf[64]{};
            std::snprintf(buf, sizeof(buf), "dragon:%08X:%08X", acDragonFormId.ModId, acDragonFormId.BaseId);
            return std::string(buf);
        }
        if (aFallbackServerId != 0)
        {
            char buf[64]{};
            std::snprintf(buf, sizeof(buf), "dragon_srv:%08X", aFallbackServerId);
            return std::string(buf);
        }
        return {};
    }

    // Records a dragon death / soul absorption reported by `aReporterPlayerId` and returns the exact
    // subset of `acEligiblePeerIds` that have NOT yet received a Dragon Soul for this dragon.
    // Note: the killer (`aReporterPlayerId`) already absorbs the soul locally via Skyrim's native
    // MQKillDragonScript unless `aGrantToReporter == true`.
    AwardResult RecordDragonKill(
        const std::string& acDragonKey,
        uint32_t aReporterPlayerId,
        const std::vector<uint32_t>& acPartyMemberIds,
        bool aGrantToReporter = false)
    {
        AwardResult out{};
        if (acDragonKey.empty())
        {
            out.TotalPartyDragonKills = static_cast<int32_t>(m_orderedDragonKeys.size());
            return out;
        }

        const bool inserted = m_slainDragonKeys.insert(acDragonKey).second;
        if (inserted)
            m_orderedDragonKeys.push_back(acDragonKey);

        auto& awardedSet = m_awardedPlayersByDragon[acDragonKey];
        if (!aGrantToReporter && aReporterPlayerId != 0)
            awardedSet.insert(aReporterPlayerId);

        for (uint32_t pid : acPartyMemberIds)
        {
            if (pid == 0)
                continue;
            if (awardedSet.insert(pid).second)
                out.NewlyAwardedPlayerIds.push_back(pid);
        }

        out.IsNewDragonKill = inserted;
        out.TotalPartyDragonKills = static_cast<int32_t>(m_orderedDragonKeys.size());
        return out;
    }

    [[nodiscard]] bool HasPlayerReceivedSoul(const std::string& acDragonKey, uint32_t aPlayerId) const noexcept
    {
        const auto it = m_awardedPlayersByDragon.find(acDragonKey);
        return it != m_awardedPlayersByDragon.end() && it->second.contains(aPlayerId);
    }

    void RestoreDragonKeys(const std::vector<std::string>& acKeys)
    {
        Clear();
        for (const auto& key : acKeys)
        {
            if (!key.empty() && m_slainDragonKeys.insert(key).second)
                m_orderedDragonKeys.push_back(key);
        }
    }

private:
    std::unordered_set<std::string> m_slainDragonKeys{};
    std::vector<std::string> m_orderedDragonKeys{};
    std::unordered_map<std::string, std::unordered_set<uint32_t>> m_awardedPlayersByDragon{};
};

// ---------------------------------------------------------------------------
// 2. Shared Word Walls Ledger (TESWordOfPower GameId)
// ---------------------------------------------------------------------------

class WordWallLedger
{
public:
    void Clear() noexcept { m_learnedWords.clear(); }
    [[nodiscard]] size_t Size() const noexcept { return m_learnedWords.size(); }
    [[nodiscard]] const std::vector<GameId>& GetLearnedWords() const noexcept { return m_learnedWords; }

    bool RecordWordLearned(const GameId& acWordId)
    {
        if (!acWordId || acWordId.ModId == 0xFFFFFFFFu)
            return false;
        for (const auto& existing : m_learnedWords)
        {
            if (existing == acWordId)
                return false;
        }
        m_learnedWords.push_back(acWordId);
        return true;
    }

    void RestoreWords(const std::vector<GameId>& acWords)
    {
        Clear();
        for (const auto& w : acWords)
            (void)RecordWordLearned(w);
    }

private:
    std::vector<GameId> m_learnedWords{};
};

// ---------------------------------------------------------------------------
// 3. RFAB_StartMenu Difficulty & Cursed Handshake
// ---------------------------------------------------------------------------

enum class RfabDifficultyTier : uint8_t
{
    kWanderer = 0,  // Странник: 2.0x outgoing / 0.5x incoming
    kDovahkiin = 1, // Довакин:  1.0x outgoing / 1.0x incoming
    kLegend = 2,    // Легенда:  1.0x outgoing / 2.0x incoming
    kUnknown = 255,
};

struct DifficultyHandshakeState
{
    RfabDifficultyTier Tier{RfabDifficultyTier::kUnknown};
    bool IsCursed{false};
    float OutgoingMult{1.0f};
    float IncomingMult{1.0f};

    [[nodiscard]] bool IsValid() const noexcept { return Tier != RfabDifficultyTier::kUnknown; }
};

enum class DifficultyParityOutcome : uint8_t
{
    kMatch = 0,
    kHostNotReported = 1,
    kDifficultyMismatch = 2,
    kCursedMismatch = 3,
    kBothMismatch = 4,
};

// Infers RFAB StartMenu tier & Cursed flag directly from live GMST multipliers:
//   fDiffMultHPByPCVH (outgoing) and fDiffMultHPToPCVH (incoming).
// Matches docs/RFAB_DIFFICULTY_AND_VANILLA_OVERRIDE_AUDIT_2026_10_24.md verbatim:
//   - Wanderer: outgoing 2.0 (cursed 0.5), incoming 0.5
//   - Dovahkiin: outgoing 1.0 (cursed 0.5), incoming 1.0
//   - Legend:    outgoing 1.0 (cursed 0.5), incoming 2.0
[[nodiscard]] inline DifficultyHandshakeState InferDifficultyFromGmst(float aOutgoingMult, float aIncomingMult) noexcept
{
    DifficultyHandshakeState state{};
    if (!std::isfinite(aOutgoingMult) || !std::isfinite(aIncomingMult) || aOutgoingMult <= 0.0f || aIncomingMult <= 0.0f)
        return state;

    state.OutgoingMult = aOutgoingMult;
    state.IncomingMult = aIncomingMult;

    if (aIncomingMult <= 0.75f)
        state.Tier = RfabDifficultyTier::kWanderer;
    else if (aIncomingMult >= 1.5f)
        state.Tier = RfabDifficultyTier::kLegend;
    else
        state.Tier = RfabDifficultyTier::kDovahkiin;

    // Cursed (`Проклятый`) forces outgoing damage to 0.5x regardless of base tier.
    state.IsCursed = (aOutgoingMult <= 0.65f);
    return state;
}

// Encodes/decodes DifficultyHandshakeState into SyncRfabProgressionEvent's compact int32_t field.
[[nodiscard]] constexpr int32_t PackDifficultyHandshake(RfabDifficultyTier aTier, bool aIsCursed) noexcept
{
    const uint32_t tierBits = static_cast<uint32_t>(aTier) & 0xFFu;
    const uint32_t cursedBit = aIsCursed ? 0x100u : 0u;
    return static_cast<int32_t>(tierBits | cursedBit);
}

[[nodiscard]] inline DifficultyHandshakeState UnpackDifficultyHandshake(int32_t aPacked, float aIncomingMult = 1.0f) noexcept
{
    DifficultyHandshakeState out{};
    const uint32_t raw = static_cast<uint32_t>(aPacked);
    const uint8_t tierByte = static_cast<uint8_t>(raw & 0xFFu);
    out.Tier = (tierByte <= 2u) ? static_cast<RfabDifficultyTier>(tierByte) : RfabDifficultyTier::kUnknown;
    out.IsCursed = (raw & 0x100u) != 0u;
    out.IncomingMult = std::isfinite(aIncomingMult) && aIncomingMult > 0.0f ? aIncomingMult : 1.0f;
    out.OutgoingMult = out.IsCursed ? 0.5f : (out.Tier == RfabDifficultyTier::kWanderer ? 2.0f : 1.0f);
    return out;
}

[[nodiscard]] constexpr const char* DifficultyTierDisplayName(RfabDifficultyTier aTier) noexcept
{
    switch (aTier)
    {
    case RfabDifficultyTier::kWanderer: return "Wanderer";
    case RfabDifficultyTier::kDovahkiin: return "Dovahkiin";
    case RfabDifficultyTier::kLegend: return "Legend";
    default: return "Unknown";
    }
}

[[nodiscard]] inline DifficultyParityOutcome EvaluateDifficultyHandshake(
    const DifficultyHandshakeState& acHost,
    const DifficultyHandshakeState& acGuest) noexcept
{
    if (!acHost.IsValid())
        return DifficultyParityOutcome::kHostNotReported;
    if (!acGuest.IsValid())
        return DifficultyParityOutcome::kDifficultyMismatch;

    const bool diffMismatch = (acHost.Tier != acGuest.Tier);
    const bool cursedMismatch = (acHost.IsCursed != acGuest.IsCursed);

    if (diffMismatch && cursedMismatch)
        return DifficultyParityOutcome::kBothMismatch;
    if (diffMismatch)
        return DifficultyParityOutcome::kDifficultyMismatch;
    if (cursedMismatch)
        return DifficultyParityOutcome::kCursedMismatch;
    return DifficultyParityOutcome::kMatch;
}

[[nodiscard]] inline std::string FormatDifficultyMismatchNotice(
    const DifficultyHandshakeState& acHost,
    const DifficultyHandshakeState& acGuest)
{
    const auto outcome = EvaluateDifficultyHandshake(acHost, acGuest);
    if (outcome == DifficultyParityOutcome::kMatch || outcome == DifficultyParityOutcome::kHostNotReported)
        return {};

    std::string msg = "[RFAB Co-Op] Difficulty mismatch: Host=";
    msg += DifficultyTierDisplayName(acHost.Tier);
    if (acHost.IsCursed)
        msg += "+Cursed";
    msg += " vs You=";
    msg += DifficultyTierDisplayName(acGuest.Tier);
    if (acGuest.IsCursed)
        msg += "+Cursed";
    return msg;
}

// ---------------------------------------------------------------------------
// 4. Quests.ini Canonical Deduplication Key
// ---------------------------------------------------------------------------

// Normalizes quest names (including UTF-8 Cyrillic titles from RFAB `Quests.ini`) into a
// whitespace-collapsed, ASCII-lowercased canonical source key (`quest_ini:<hash64>`), so
// two players completing the same RFAB quest >4s apart still hit the exact same server XP key!
[[nodiscard]] inline std::string MakeCanonicalQuestsIniSourceKey(
    const GameId& acQuestId,
    uint16_t aStage,
    std::string_view aQuestEditorOrTitle = {})
{
    if (acQuestId && acQuestId.ModId != 0xFFFFFFFFu)
    {
        char buf[64]{};
        std::snprintf(buf, sizeof(buf), "quest:%08X:%08X:%u", acQuestId.ModId, acQuestId.BaseId, static_cast<unsigned>(aStage));
        return std::string(buf);
    }

    // Fallback: canonical FNV-1a 64-bit hash of trimmed, whitespace-collapsed title/editorId.
    std::string canonical;
    canonical.reserve(aQuestEditorOrTitle.size());
    bool prevSpace = true;
    for (unsigned char ch : aQuestEditorOrTitle)
    {
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n')
        {
            if (!prevSpace)
            {
                canonical.push_back(' ');
                prevSpace = true;
            }
        }
        else
        {
            if (ch >= 'A' && ch <= 'Z')
                ch = static_cast<unsigned char>(ch - 'A' + 'a');
            canonical.push_back(static_cast<char>(ch));
            prevSpace = false;
        }
    }
    while (!canonical.empty() && canonical.back() == ' ')
        canonical.pop_back();

    if (canonical.empty())
        return {};

    uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : canonical)
    {
        h ^= static_cast<uint64_t>(c);
        h *= 1099511628211ULL;
    }

    char buf[64]{};
    std::snprintf(buf, sizeof(buf), "quest_ini:%016llX:%u", static_cast<unsigned long long>(h), static_cast<unsigned>(aStage));
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// 5. Late-Joiner Catch-Up XP Policy
// ---------------------------------------------------------------------------

inline constexpr int32_t kDefaultMaxLateJoinCatchUpXp = 25000;

struct LateJoinCatchUpDecision
{
    bool ShouldGrant{false};
    int32_t GrantXpDelta{0};
};

class LateJoinCatchUpTracker
{
public:
    void Clear() noexcept { m_grantedPlayers.clear(); }

    // Evaluates bounded, one-time-per-session catch-up XP for a player joining an ongoing session
    // that already accumulated `aAuthoritativeSessionXp`. Players who were already credited (or
    // who joined at session start with 0 accumulated XP) never receive duplicate grants.
    LateJoinCatchUpDecision EvaluateAndMark(
        uint32_t aPlayerId,
        int64_t aAuthoritativeSessionXp,
        bool aEnabled,
        int32_t aMaxGrantXp = kDefaultMaxLateJoinCatchUpXp)
    {
        LateJoinCatchUpDecision out{};
        if (!aEnabled || aPlayerId == 0 || aAuthoritativeSessionXp <= 0 || aMaxGrantXp <= 0)
            return out;

        if (!m_grantedPlayers.insert(aPlayerId).second)
            return out;

        const int64_t bounded = std::min<int64_t>(aAuthoritativeSessionXp, static_cast<int64_t>(aMaxGrantXp));
        out.ShouldGrant = bounded > 0;
        out.GrantXpDelta = static_cast<int32_t>(bounded);
        return out;
    }

    void MarkAlreadySyncedAtSessionStart(uint32_t aPlayerId)
    {
        if (aPlayerId != 0)
            m_grantedPlayers.insert(aPlayerId);
    }

    [[nodiscard]] bool HasBeenEvaluated(uint32_t aPlayerId) const noexcept
    {
        return m_grantedPlayers.contains(aPlayerId);
    }

private:
    std::unordered_set<uint32_t> m_grantedPlayers{};
};

} // namespace RfabCoopProgression
