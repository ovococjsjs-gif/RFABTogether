#pragma once

/**
 * ActorValue replication policy & canonical Skyrim SE indices.
 *
 * Keeps server and client strictly aligned on:
 *  - kInfamy = 61 (NOT 37, which is kVoicePoints!) — used locally by RFAB_XP_Alias.psc
 *    as a duplicate-XP suppression sentinel and must never cross the wire.
 *  - Synced defense / resistance AVs (kDamageResist 39..kMagicResist 44, kAbsorbChance 83)
 *    are reconciled on remote actors via ForceMode::TEMPORARY delta against current effective
 *    value instead of ForceMode::PERMANENT, preventing permanent base/mod compounding across
 *    ownership handoffs and .ess checkpoint saves.
 */

#include <cmath>
#include <cstdint>

namespace Authority
{

inline constexpr uint32_t kAvHealth = 24u;
inline constexpr uint32_t kAvMagicka = 25u;
inline constexpr uint32_t kAvStamina = 26u;
inline constexpr uint32_t kAvSpeedMult = 30u;
inline constexpr uint32_t kAvVoicePoints = 37u;
inline constexpr uint32_t kAvDamageResist = 39u;
inline constexpr uint32_t kAvPoisonResist = 40u;
inline constexpr uint32_t kAvFireResist = 41u;
inline constexpr uint32_t kAvElectricResist = 42u;
inline constexpr uint32_t kAvFrostResist = 43u;
inline constexpr uint32_t kAvMagicResist = 44u;
inline constexpr uint32_t kAvDiseaseResist = 45u;
inline constexpr uint32_t kAvInfamy = 61u;
inline constexpr uint32_t kAvAbsorbChance = 83u;
inline constexpr uint32_t kAvWeaponSpeedMult = 85u;
inline constexpr uint32_t kAvLeftWeaponSpeedMult = 132u;
inline constexpr uint32_t kAvDragonSouls = 133u;
inline constexpr uint32_t kAvCount = 164u;

enum class RemoteActorValueApplyMode : uint8_t
{
    kSkip = 0,
    kHealthSpecial = 1,
    kDamageDelta = 2,
    kTemporaryTotalDelta = 3,
    kPermanentDelta = 4,
};

[[nodiscard]] constexpr bool IsLocallyDerivedOrNonReplicatedAv(uint32_t aAvId) noexcept
{
    return aAvId == kAvDragonSouls ||
           aAvId == kAvSpeedMult ||
           aAvId == kAvInfamy ||
           aAvId == kAvWeaponSpeedMult ||
           aAvId == kAvLeftWeaponSpeedMult;
}

[[nodiscard]] constexpr bool IsSyncedDefenseResistanceAv(uint32_t aAvId) noexcept
{
    return (aAvId >= kAvDamageResist && aAvId <= kAvMagicResist) || aAvId == kAvAbsorbChance;
}

[[nodiscard]] constexpr bool ShouldAcceptServerActorValue(uint32_t aAvId) noexcept
{
    return aAvId < kAvCount && aAvId != kAvInfamy;
}

[[nodiscard]] constexpr RemoteActorValueApplyMode ClassifyRemoteActorValueApplyMode(
    uint32_t aAvId,
    bool aIsMaxValuePacket) noexcept
{
    if (aAvId >= kAvCount || IsLocallyDerivedOrNonReplicatedAv(aAvId))
        return RemoteActorValueApplyMode::kSkip;

    if (aAvId == kAvHealth)
        return RemoteActorValueApplyMode::kHealthSpecial;

    if (IsSyncedDefenseResistanceAv(aAvId))
    {
        // Authoritative effective total (Base + Permanent + Temporary) arrives on the current-value
        // channel and is applied via ForceMode::TEMPORARY delta. Never overwrite it from the
        // permanent/max-value channel, which would strip active temporary buffs or compound base values.
        return aIsMaxValuePacket ? RemoteActorValueApplyMode::kSkip
                                 : RemoteActorValueApplyMode::kTemporaryTotalDelta;
    }

    return aIsMaxValuePacket ? RemoteActorValueApplyMode::kPermanentDelta
                             : RemoteActorValueApplyMode::kDamageDelta;
}

[[nodiscard]] inline float ComputeTemporaryTotalDelta(
    float aCurrentEffectiveValue,
    float aTargetEffectiveValue) noexcept
{
    if (!std::isfinite(aCurrentEffectiveValue) || !std::isfinite(aTargetEffectiveValue))
        return 0.0f;

    const float delta = aTargetEffectiveValue - aCurrentEffectiveValue;
    return std::fabs(delta) > 1e-4f ? delta : 0.0f;
}

} // namespace Authority
