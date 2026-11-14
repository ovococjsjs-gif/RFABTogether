#pragma once

/**
 * Pure, deterministic RFAB world immersion & multiplayer isolation policies (Phase 3 Group C):
 *  1. MountOwnershipGuard: remembers which player is riding which mount, so that the server can
 *     refuse a second rider, a takeover of the simulation or a saddlebag change while the ride
 *     lasts. Only the start of a ride is reported by clients; how a ride ends is described at
 *     IsRiderStillAtMount and IsMountExitAnimationEvent.
 *  2. PersonalBlessingCrimePolicy: isolates personal Divine Shrine blessings, Requiem SaveBlessing
 *     flags, and CrimeGold / bounty globals so one player's theft or shrine visit never strips a
 *     teammate's Divine Blessing or corrupts their crime state.
 *  3. TrueHudSynergyPolicy: defines TrueHUD.esl boss-bar eligibility and floating-bar actor value
 *     consistency rules so remote enemy bars and boss bars stay rock-solid across clients.
 */

#include <common/Authority/ActorValues.h>

#include <cmath>
#include <cstdint>
#include <optional>
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

    [[nodiscard]] std::optional<ActiveRide> GetActiveRide(uint32_t aMountServerId) const noexcept
    {
        const auto it = m_activeRiderByMountId.find(aMountServerId);
        if (it == m_activeRiderByMountId.end())
            return std::nullopt;
        return it->second;
    }

    // The server is told when a ride starts and never when it ends: no client sends a dismount.
    // A ride is therefore over as soon as the rider is further from the mount than someone
    // sitting on it can be. 1024 units is about 15 metres: far more than the lag between the two
    // position reports at a gallop, far less than "walked away from the horse".
    static constexpr float kMaxRiderToMountDistance = 1024.0f;

    [[nodiscard]] static bool IsRiderStillAtMount(float aDistanceSquared) noexcept
    {
        return std::isfinite(aDistanceSquared) &&
               aDistanceSquared <= kMaxRiderToMountDistance * kMaxRiderToMountDistance;
    }

    // The animation events a player's own graph sends when it gets off a horse or a dragon. The
    // server sees them in the rider's action stream, which is the earliest sign that a ride ended.
    [[nodiscard]] static bool IsMountExitAnimationEvent(std::string_view aEventName) noexcept
    {
        static constexpr std::string_view kExitEvents[] = {
            "horseexit", "horseexitout", "dragonmountexit", "dragonmountexitout"};
        for (const std::string_view exitEvent : kExitEvents)
        {
            if (aEventName.size() != exitEvent.size())
                continue;
            bool same = true;
            for (size_t i = 0; i < exitEvent.size() && same; ++i)
            {
                const char ch = aEventName[i];
                same = ((ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch) == exitEvent[i];
            }
            if (same)
                return true;
        }
        return false;
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
