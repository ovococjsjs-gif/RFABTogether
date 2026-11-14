#pragma once

/**
 * Pure, deterministic RFAB world immersion & multiplayer isolation policies (Phase 3 Group C):
 *  1. MountOwnershipGuard: tracks active mount riders and enforces saddlebag ownership isolation
 *     so peers cannot hijack an actively ridden RFAB horse mid-gallop or duplicate saddlebag loot,
 *     and automatically frees mounts on dismount or rider disconnect.
 *  2. PersonalBlessingCrimePolicy: isolates personal Divine Shrine blessings, Requiem SaveBlessing
 *     flags, and CrimeGold / bounty globals so one player's theft or shrine visit never strips a
 *     teammate's Divine Blessing or corrupts their crime state.
 *  3. TrueHudSynergyPolicy: defines TrueHUD.esl boss-bar eligibility and floating-bar actor value
 *     consistency rules so remote enemy bars and boss bars stay rock-solid across clients.
 */

#include <common/Authority/ActorValues.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace RfabWorldImmersion
{

// ---------------------------------------------------------------------------
// 1. RFAB Mount Ownership & Saddlebag Anti-Dupe Guard
// ---------------------------------------------------------------------------

class MountOwnershipGuard
{
public:
    struct ActiveRide
    {
        uint32_t RiderPlayerId{0};
        uint32_t RiderServerEntityId{0};
    };

    void Clear() noexcept
    {
        m_activeRiderByMountId.clear();
        m_activeMountByPlayerId.clear();
    }

    [[nodiscard]] uint32_t GetActiveRiderPlayerId(uint32_t aMountServerId) const noexcept
    {
        const auto it = m_activeRiderByMountId.find(aMountServerId);
        return it != m_activeRiderByMountId.end() ? it->second.RiderPlayerId : 0u;
    }

    [[nodiscard]] uint32_t GetMountedHorseForPlayer(uint32_t aPlayerId) const noexcept
    {
        const auto it = m_activeMountByPlayerId.find(aPlayerId);
        return it != m_activeMountByPlayerId.end() ? it->second : 0u;
    }

    // Attempts to bind `aMountServerId` to `aRiderPlayerId`.
    // Rejects hijacking when a DIFFERENT player is currently mounted on the exact same horse!
    // Automatically releases any previous mount the same player was riding.
    bool TryAcquireRide(uint32_t aMountServerId, uint32_t aRiderPlayerId, uint32_t aRiderServerEntityId = 0) noexcept
    {
        if (aMountServerId == 0 || aRiderPlayerId == 0)
            return false;

        if (const auto it = m_activeRiderByMountId.find(aMountServerId);
            it != m_activeRiderByMountId.end() && it->second.RiderPlayerId != aRiderPlayerId)
        {
            return false;
        }

        // Release any previous mount this rider was on.
        if (const auto prevIt = m_activeMountByPlayerId.find(aRiderPlayerId);
            prevIt != m_activeMountByPlayerId.end() && prevIt->second != aMountServerId)
        {
            m_activeRiderByMountId.erase(prevIt->second);
        }

        m_activeRiderByMountId[aMountServerId] = ActiveRide{aRiderPlayerId, aRiderServerEntityId};
        m_activeMountByPlayerId[aRiderPlayerId] = aMountServerId;
        return true;
    }

    void ReleaseRide(uint32_t aMountServerId, uint32_t aRiderPlayerId) noexcept
    {
        const auto it = m_activeRiderByMountId.find(aMountServerId);
        if (it == m_activeRiderByMountId.end())
            return;
        if (aRiderPlayerId != 0 && it->second.RiderPlayerId != aRiderPlayerId)
            return;

        m_activeMountByPlayerId.erase(it->second.RiderPlayerId);
        m_activeRiderByMountId.erase(it);
    }

    void ReleaseMountEntity(uint32_t aMountServerId) noexcept
    {
        ReleaseRide(aMountServerId, 0);
    }

    void ReleaseAllForPlayer(uint32_t aPlayerId) noexcept
    {
        const auto it = m_activeMountByPlayerId.find(aPlayerId);
        if (it != m_activeMountByPlayerId.end())
        {
            m_activeRiderByMountId.erase(it->second);
            m_activeMountByPlayerId.erase(it);
        }
    }

    // Voluntary mount ownership transfer is blocked only while a DIFFERENT player is actively riding it.
    [[nodiscard]] bool CanTransferMountOwnership(uint32_t aMountServerId, uint32_t aChallengerPlayerId) const noexcept
    {
        const uint32_t activeRider = GetActiveRiderPlayerId(aMountServerId);
        return activeRider == 0 || activeRider == aChallengerPlayerId;
    }

    // RFAB horse saddlebag access guard: prevents a bystander from mutating a mount's saddlebag
    // inventory while another player is actively riding that horse (eliminates mid-ride theft/dupes).
    [[nodiscard]] bool CanMutateMountSaddlebag(
        uint32_t aMountServerId,
        uint32_t aMutatorPlayerId,
        uint32_t aMountOwnerPlayerId) const noexcept
    {
        if (aMutatorPlayerId == 0)
            return false;
        const uint32_t activeRider = GetActiveRiderPlayerId(aMountServerId);
        if (activeRider != 0)
            return activeRider == aMutatorPlayerId;
        // When nobody is actively riding the horse, the current mount owner (or unowned horse) may access it.
        return aMountOwnerPlayerId == 0 || aMountOwnerPlayerId == aMutatorPlayerId;
    }

private:
    std::unordered_map<uint32_t, ActiveRide> m_activeRiderByMountId{};
    std::unordered_map<uint32_t, uint32_t> m_activeMountByPlayerId{};
};

// ---------------------------------------------------------------------------
// 2. RFAB Personal Blessings & CrimeGold Isolation Policy
// ---------------------------------------------------------------------------

namespace Detail
{
[[nodiscard]] inline std::string ToLowerAscii(std::string_view aInput)
{
    std::string out;
    out.reserve(aInput.size());
    for (unsigned char ch : aInput)
        out.push_back((ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : static_cast<char>(ch));
    return out;
}
} // namespace Detail

// Returns true for TESGlobals that track per-character crime / bounty / Divine Blessing /
// RFAB SaveBlessing state and MUST NEVER be replicated globally across co-op teammates.
[[nodiscard]] inline bool IsPersonalCrimeOrBlessingGlobal(std::string_view aEditorId) noexcept
{
    if (aEditorId.empty())
        return false;

    const std::string lower = Detail::ToLowerAscii(aEditorId);

    static constexpr std::string_view kPersonalTokens[] = {
        "crimegold",
        "stolenitems",
        "saveblessing",
        "rfab_blessing",
        "req_blessing",
        "rfab_shrine",
        "rfab_crime",
        "req_crime",
        "playerbounty",
        "murdercount",
        "assaultcount",
        "trespasscount",
        "daedricblessing",
        "divineblessing",
    };

    for (const std::string_view token : kPersonalTokens)
    {
        if (lower.find(token) != std::string::npos)
            return true;
    }
    return false;
}

// Personal Shrine / Altar / Standing Stone activators in RFAB & vanilla Skyrim whose blessing
// must apply strictly to the activating player without overwriting party members' blessings.
[[nodiscard]] inline bool IsPersonalShrineOrDoomstoneActivator(std::string_view aEditorId) noexcept
{
    if (aEditorId.empty())
        return false;

    const std::string lower = Detail::ToLowerAscii(aEditorId);
    return lower.rfind("rfab_shrine_", 0) == 0 ||
           lower == "rfab_ba_universalaltar" ||
           lower.find("templeblessing") != std::string::npos ||
           lower.find("shrineof") != std::string::npos ||
           lower.find("doomstone") != std::string::npos ||
           lower.find("standingstone") != std::string::npos;
}

// ---------------------------------------------------------------------------
// 3. TrueHUD.esl Synergy Policy
// ---------------------------------------------------------------------------

// TrueHUD floating resource bars display Health (24), Magicka (25), Stamina (26), and WardPower (82).
[[nodiscard]] constexpr bool IsTrueHudResourceBarActorValue(uint32_t aActorValueId) noexcept
{
    return aActorValueId == Authority::kAvHealth ||
           aActorValueId == Authority::kAvMagicka ||
           aActorValueId == Authority::kAvStamina ||
           aActorValueId == 82u; // kWardPower
}

// Determines whether a creature should receive TrueHUD boss-bar treatment in co-op parity
// with RFAB's BossList.json / Dragon / high-HP boss thresholds.
[[nodiscard]] inline bool IsTrueHudBossBarEligible(
    bool aIsDragon,
    bool aIsKnownRfabBoss,
    float aMaxHealth,
    bool aIsPlayer) noexcept
{
    if (aIsPlayer)
        return false;
    if (aIsDragon || aIsKnownRfabBoss)
        return true;
    return std::isfinite(aMaxHealth) && aMaxHealth >= 800.0f;
}

} // namespace RfabWorldImmersion
