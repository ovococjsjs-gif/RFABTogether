#pragma once

/**
 * Deterministic structural identity & normalization for RFAB crafted items:
 *  - Player-crafted Enchanted weapons/armor (dynamic 0xFFxxxxxx EnchantmentItem, ModId == 0xFFFFFFFF):
 *    compares and fingerprints by structural EnchantData (IsWeapon + effect list) instead of
 *    arbitrary client-local 0xFF form IDs, preventing collision/loss across trades, chests, and drops.
 *  - Tempered Smithing items (ExtraHealth > 1.0f, e.g., RFAB Honed/Legendary tiers):
 *    uses float-tolerant comparison (1e-3) so wire round-trips never break container/drop matching.
 *  - Crafted Alchemy potions & poisons (dynamic 0xFFxxxxxx BaseId / ExtraPoisonId):
 *    classified cleanly so client/server layers preserve metadata and never corrupt static stacks.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace CoopCraftedItems
{

inline constexpr uint32_t kTempModId = 0xFFFFFFFFu;
inline constexpr float kFloatMatchEpsilon = 1e-3f;
inline constexpr float kMaxExtraHealth = 100000.0f;
inline constexpr float kMaxExtraCharge = 100000.0f;

enum class CraftedItemKind : uint8_t
{
    kStandard = 0,
    kTempered = 1,
    kCraftedEnchanted = 2,
    kTemperedAndEnchanted = 3,
    kDynamicAlchemyOrTempBase = 4,
};

[[nodiscard]] inline bool AreFloatsClose(float aLhs, float aRhs, float aEps = kFloatMatchEpsilon) noexcept
{
    if (!std::isfinite(aLhs) && !std::isfinite(aRhs))
        return true;
    if (!std::isfinite(aLhs) || !std::isfinite(aRhs))
        return false;
    return std::fabs(aLhs - aRhs) <= aEps;
}

[[nodiscard]] inline float SanitizeCraftedFloat(float aValue, float aMaxAbs) noexcept
{
    if (!std::isfinite(aValue) || aValue <= 0.0f)
        return 0.0f;
    return std::min(aValue, aMaxAbs);
}

[[nodiscard]] inline uint32_t QuantizeFloatMilli(float aValue) noexcept
{
    if (!std::isfinite(aValue))
        return 0u;
    const double clamped = std::clamp(static_cast<double>(aValue), -1.0e6, 1.0e6);
    const int64_t rounded = static_cast<int64_t>(std::llround(clamped * 1000.0));
    return static_cast<uint32_t>(rounded);
}

struct EffectFingerprint
{
    uint32_t ModId{0};
    uint32_t BaseId{0};
    uint32_t MagMilli{0};
    int32_t Area{0};
    int32_t Duration{0};
    uint32_t CostMilli{0};

    bool operator==(const EffectFingerprint& acRhs) const noexcept
    {
        return ModId == acRhs.ModId &&
               BaseId == acRhs.BaseId &&
               MagMilli == acRhs.MagMilli &&
               Area == acRhs.Area &&
               Duration == acRhs.Duration &&
               CostMilli == acRhs.CostMilli;
    }

    bool operator<(const EffectFingerprint& acRhs) const noexcept
    {
        if (ModId != acRhs.ModId)
            return ModId < acRhs.ModId;
        if (BaseId != acRhs.BaseId)
            return BaseId < acRhs.BaseId;
        if (MagMilli != acRhs.MagMilli)
            return MagMilli < acRhs.MagMilli;
        if (Area != acRhs.Area)
            return Area < acRhs.Area;
        if (Duration != acRhs.Duration)
            return Duration < acRhs.Duration;
        return CostMilli < acRhs.CostMilli;
    }
};

template <class TEnchantData>
[[nodiscard]] inline std::vector<EffectFingerprint> BuildCanonicalEffectFingerprints(const TEnchantData& acData)
{
    std::vector<EffectFingerprint> out;
    out.reserve(acData.Effects.size());
    for (const auto& ef : acData.Effects)
    {
        EffectFingerprint fp{};
        fp.ModId = ef.EffectId.ModId;
        fp.BaseId = ef.EffectId.BaseId;
        fp.MagMilli = QuantizeFloatMilli(ef.Magnitude);
        fp.Area = ef.Area;
        fp.Duration = ef.Duration;
        fp.CostMilli = QuantizeFloatMilli(ef.RawCost);
        out.push_back(fp);
    }
    std::sort(out.begin(), out.end());
    return out;
}

template <class TEnchantData>
[[nodiscard]] inline uint32_t ComputeCraftedEnchantFingerprint(const TEnchantData& acData)
{
    const auto effects = BuildCanonicalEffectFingerprints(acData);
    uint64_t h = 14695981039346656037ULL;
    auto mixU32 = [&h](uint32_t v) noexcept
    {
        for (int shift = 0; shift < 32; shift += 8)
        {
            h ^= static_cast<uint64_t>((v >> shift) & 0xFFu);
            h *= 1099511628211ULL;
        }
    };

    mixU32(acData.IsWeapon ? 1u : 0u);
    mixU32(static_cast<uint32_t>(effects.size()));
    for (const auto& ef : effects)
    {
        mixU32(ef.ModId);
        mixU32(ef.BaseId);
        mixU32(ef.MagMilli);
        mixU32(static_cast<uint32_t>(ef.Area));
        mixU32(static_cast<uint32_t>(ef.Duration));
        mixU32(ef.CostMilli);
    }

    const uint32_t folded = static_cast<uint32_t>((h >> 32) ^ (h & 0xFFFFFFFFULL));
    // Keep within 24-bit local form range and guarantee non-zero so `ExtraEnchantId != 0` always holds.
    const uint32_t local24 = folded & 0x00FFFFFFu;
    return local24 != 0u ? local24 : 1u;
}

template <class TEntry>
[[nodiscard]] inline bool IsDynamicTempEnchantment(const TEntry& acEntry) noexcept
{
    return acEntry.ExtraEnchantId.ModId == kTempModId && !acEntry.EnchantData.Effects.empty();
}

template <class TEntry>
[[nodiscard]] inline bool AreEnchantmentsStructurallyEqual(const TEntry& acLhs, const TEntry& acRhs) noexcept
{
    const bool lhsDyn = IsDynamicTempEnchantment(acLhs);
    const bool rhsDyn = IsDynamicTempEnchantment(acRhs);
    if (lhsDyn != rhsDyn)
        return false;

    if (lhsDyn)
    {
        if (acLhs.EnchantData.IsWeapon != acRhs.EnchantData.IsWeapon)
            return false;
        if (acLhs.EnchantData.Effects.size() != acRhs.EnchantData.Effects.size())
            return false;
        return BuildCanonicalEffectFingerprints(acLhs.EnchantData) ==
               BuildCanonicalEffectFingerprints(acRhs.EnchantData);
    }

    return acLhs.ExtraEnchantId == acRhs.ExtraEnchantId;
}

template <class TEntry>
inline void NormalizeCraftedEntry(TEntry& aEntry) noexcept
{
    aEntry.ExtraHealth = SanitizeCraftedFloat(aEntry.ExtraHealth, kMaxExtraHealth);
    aEntry.ExtraCharge = SanitizeCraftedFloat(aEntry.ExtraCharge, kMaxExtraCharge);

    if (IsDynamicTempEnchantment(aEntry))
    {
        // Replace arbitrary client-local 0xFFxxxxxx BaseId with deterministic structural fingerprint
        // so identical player-crafted enchantments unify cleanly across server, containers, and peers
        // while distinct crafted enchantments never collide!
        aEntry.ExtraEnchantId.BaseId = ComputeCraftedEnchantFingerprint(aEntry.EnchantData);
    }
}

template <class TEntry>
[[nodiscard]] inline CraftedItemKind ClassifyCraftedItem(const TEntry& acEntry) noexcept
{
    if (acEntry.BaseId.ModId == kTempModId)
        return CraftedItemKind::kDynamicAlchemyOrTempBase;

    const bool tempered = std::isfinite(acEntry.ExtraHealth) && acEntry.ExtraHealth > 1.0f + kFloatMatchEpsilon;
    const bool craftedEnchant = IsDynamicTempEnchantment(acEntry);

    if (tempered && craftedEnchant)
        return CraftedItemKind::kTemperedAndEnchanted;
    if (craftedEnchant)
        return CraftedItemKind::kCraftedEnchanted;
    if (tempered)
        return CraftedItemKind::kTempered;
    return CraftedItemKind::kStandard;
}

} // namespace CoopCraftedItems
