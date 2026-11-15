#include <Structs/Inventory.h>
#include <TiltedCore/Serialization.hpp>
#include <common/CoopPacketSafety.h>
#include <algorithm>

using TiltedPhoques::Serialization;

void Inventory::EffectItem::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteFloat(aWriter, Magnitude);
    Serialization::WriteVarInt(aWriter, Area);
    Serialization::WriteVarInt(aWriter, Duration);
    Serialization::WriteFloat(aWriter, RawCost);
    EffectId.Serialize(aWriter);
}

void Inventory::EffectItem::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    Magnitude = Serialization::ReadFloat(aReader);
    Area = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    Duration = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    RawCost = Serialization::ReadFloat(aReader);
    EffectId.Deserialize(aReader);
}

void Inventory::Entry::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    BaseId.Serialize(aWriter);
    Serialization::WriteVarInt(aWriter, Count);

    Serialization::WriteFloat(aWriter, ExtraCharge);

    ExtraEnchantId.Serialize(aWriter);
    Serialization::WriteVarInt(aWriter, ExtraEnchantCharge);
    Serialization::WriteVarInt(aWriter, EnchantData.Effects.size());
    for (const EffectItem& effect : EnchantData.Effects)
    {
        effect.Serialize(aWriter);
    }

    Serialization::WriteFloat(aWriter, ExtraHealth);

    ExtraPoisonId.Serialize(aWriter);
    Serialization::WriteVarInt(aWriter, ExtraPoisonCount);

    Serialization::WriteVarInt(aWriter, ExtraSoulLevel);

    Serialization::WriteBool(aWriter, EnchantData.IsWeapon);
    Serialization::WriteBool(aWriter, ExtraEnchantRemoveUnequip);
    Serialization::WriteBool(aWriter, ExtraWorn);
    Serialization::WriteBool(aWriter, ExtraWornLeft);
    Serialization::WriteBool(aWriter, IsQuestItem);
}

void Inventory::Entry::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    BaseId.Deserialize(aReader);
    Count = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;

    ExtraCharge = Serialization::ReadFloat(aReader);

    ExtraEnchantId.Deserialize(aReader);
    ExtraEnchantCharge = Serialization::ReadVarInt(aReader) & 0xFFFF;
    const size_t effectCount = CoopPacketSafety::ClampCollectionCount(
        Serialization::ReadVarInt(aReader),
        CoopPacketSafety::kMaxEnchantEffects);
    EnchantData.Effects.clear();
    EnchantData.Effects.reserve(effectCount);
    for (size_t i = 0; i < effectCount && !aReader.Eof(); i++)
    {
        EffectItem effect{};
        effect.Deserialize(aReader);
        EnchantData.Effects.push_back(effect);
    }

    ExtraHealth = Serialization::ReadFloat(aReader);

    ExtraPoisonId.Deserialize(aReader);
    ExtraPoisonCount = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;

    ExtraSoulLevel = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;

    EnchantData.IsWeapon = Serialization::ReadBool(aReader);
    ExtraEnchantRemoveUnequip = Serialization::ReadBool(aReader);
    ExtraWorn = Serialization::ReadBool(aReader);
    ExtraWornLeft = Serialization::ReadBool(aReader);
    IsQuestItem = Serialization::ReadBool(aReader);
}

bool Inventory::operator==(const Inventory& acRhs) const noexcept
{
    return Entries == acRhs.Entries && CurrentMagicEquipment == acRhs.CurrentMagicEquipment;
}

bool Inventory::operator!=(const Inventory& acRhs) const noexcept
{
    return !this->operator==(acRhs);
}

bool Inventory::Entry::operator==(const Inventory::Entry& acRhs) const noexcept
{
    return BaseId == acRhs.BaseId && Count == acRhs.Count && IsExtraDataEquals(acRhs);
}

bool Inventory::Entry::operator!=(const Inventory::Entry& acRhs) const noexcept
{
    return !this->operator==(acRhs);
}

void Inventory::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    const size_t entryCount = std::min(Entries.size(), CoopPacketSafety::kMaxInventoryEntries);
    Serialization::WriteVarInt(aWriter, entryCount);
    for (size_t i = 0; i < entryCount; ++i)
    {
        Entries[i].Serialize(aWriter);
    }

    CurrentMagicEquipment.Serialize(aWriter);
}

void Inventory::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    const size_t count = CoopPacketSafety::ClampCollectionCount(
        Serialization::ReadVarInt(aReader),
        CoopPacketSafety::kMaxInventoryEntries);
    Entries.clear();
    Entries.reserve(std::min<size_t>(count, 256));
    for (size_t i = 0; i < count && !aReader.Eof(); i++)
    {
        Entry entry{};
        entry.Deserialize(aReader);
        Entries.push_back(std::move(entry));
    }

    if (!aReader.Eof())
        CurrentMagicEquipment.Deserialize(aReader);
}

std::optional<Inventory::Entry> Inventory::GetEntryById(GameId& aItemId) const noexcept
{
    auto entry = std::find_if(Entries.begin(), Entries.end(), [aItemId](auto entry) { return entry.BaseId == aItemId; });
    if (entry == Entries.end())
        return std::nullopt;

    return {*entry};
}

int32_t Inventory::GetEntryCountById(GameId& aItemId) const noexcept
{
    auto entry = GetEntryById(aItemId);
    if (!entry)
        return 0;

    return entry->Count;
}

void Inventory::AddOrRemoveEntry(const Entry& acEntry) noexcept
{
    if (acEntry.Count == 0)
        return;

    if (acEntry.Count > 0)
    {
        auto duplicate = std::find_if(Entries.begin(), Entries.end(), [acEntry](Entry& entry) { return entry.CanBeMerged(acEntry); });
        if (duplicate != Entries.end())
        {
            duplicate->Count += acEntry.Count;
        }
        else
        {
            Entries.push_back(acEntry);
        }
        return;
    }

    // acEntry.Count < 0: remove items without ever pushing phantom negative-count entries into Entries.
    int32_t remainingToRemove = -acEntry.Count;

    // 1. Prefer exact extra-data match first
    auto duplicate = std::find_if(Entries.begin(), Entries.end(), [acEntry](Entry& entry) { return entry.CanBeMerged(acEntry); });
    if (duplicate != Entries.end())
    {
        if (duplicate->Count > remainingToRemove)
        {
            duplicate->Count -= remainingToRemove;
            return;
        }

        remainingToRemove -= std::max(0, duplicate->Count);
        Entries.erase(duplicate);
    }

    // 2. Fallback: match by BaseId (handles looting worn corpse gear where ExtraWorn became false at removal time,
    // or items whose ExtraCharge/ExtraHealth changed slightly).
    while (remainingToRemove > 0)
    {
        auto fallback = std::find_if(
            Entries.begin(),
            Entries.end(),
            [&acEntry](const Entry& entry) { return entry.BaseId == acEntry.BaseId && entry.Count > 0; });

        if (fallback == Entries.end())
            break;

        if (fallback->Count > remainingToRemove)
        {
            fallback->Count -= remainingToRemove;
            break;
        }

        remainingToRemove -= fallback->Count;
        Entries.erase(fallback);
    }
}

void Inventory::UpdateEquipment(const Inventory& acNewInventory) noexcept
{
    for (auto& existing : Entries)
    {
        existing.ExtraWorn = false;
        existing.ExtraWornLeft = false;
    }

    for (const auto& newEntry : acNewInventory.Entries)
    {
        if (!newEntry.IsWorn() || newEntry.BaseId == GameId{})
            continue;

        // 1. Prefer an entry with the same BaseId that has not yet been marked worn in this pass
        //    (handles two separate Count=1 entries for dual-wielded identical 1H weapons).
        auto entry = std::find_if(
            Entries.begin(),
            Entries.end(),
            [&newEntry](const Entry& aEntry) {
                return aEntry.BaseId == newEntry.BaseId && aEntry.Count > 0 && !aEntry.IsWorn();
            });

        // 2. Otherwise match a stacked entry (Count >= 2) or idempotent duplicate for the other hand.
        if (entry == Entries.end())
        {
            entry = std::find_if(
                Entries.begin(),
                Entries.end(),
                [&newEntry](const Entry& aEntry) {
                    if (aEntry.BaseId != newEntry.BaseId || aEntry.Count <= 0)
                        return false;
                    if (aEntry.Count >= 2)
                        return (newEntry.ExtraWorn && !aEntry.ExtraWorn) ||
                               (newEntry.ExtraWornLeft && !aEntry.ExtraWornLeft);
                    return (newEntry.ExtraWorn == aEntry.ExtraWorn) &&
                           (newEntry.ExtraWornLeft == aEntry.ExtraWornLeft);
                });
        }

        // 3. Skyrim's InitOutfitItems / InitLeveledItems populates NPC weapons, bows, arrows, and
        //    armor directly into ExtraContainerChanges without calling Actor::AddInventoryItem (SE 36525).
        //    Never drop a newly equipped weapon/armor item when Entries did not have it yet — upsert it
        //    so subsequent ownership transfers, leader switches, and reconnects keep the NPC armed!
        if (entry == Entries.end())
        {
            Entry inserted = newEntry;
            if (inserted.Count <= 0)
                inserted.Count = 1;
            Entries.push_back(std::move(inserted));
            continue;
        }

        if (newEntry.ExtraWorn)
            entry->ExtraWorn = true;
        if (newEntry.ExtraWornLeft)
            entry->ExtraWornLeft = true;
        if (newEntry.Count > entry->Count)
            entry->Count = newEntry.Count;
    }

    CurrentMagicEquipment = acNewInventory.CurrentMagicEquipment;
}

void Inventory::RemoveByFilter(std::function<bool(const Entry&)> aFilter) noexcept
{
    Entries.erase(std::remove_if(Entries.begin(), Entries.end(), aFilter), Entries.end());
}

bool Inventory::ContainsQuestItems() const noexcept
{
    for (const auto& entry : Entries)
    {
        if (entry.IsQuestItem)
            return true;
    }
    return false;
}