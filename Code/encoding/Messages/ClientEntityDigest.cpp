#include <Messages/ClientEntityDigest.h>

#include <algorithm>

void ClientEntityDigest::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    using namespace Session::EntityDigest;

    Serialization::WriteVarInt(aWriter, Sequence);

    const size_t count = std::min(Entries.size(), kMaxEntries);
    Serialization::WriteBool(aWriter, Truncated || Entries.size() > kMaxEntries);
    Serialization::WriteVarInt(aWriter, count);
    for (size_t i = 0; i < count; ++i)
    {
        const Facts& facts = Entries[i];
        Serialization::WriteVarInt(aWriter, facts.ServerId);
        aWriter.WriteBits(facts.Flags & kKnownFlags, 8);
        aWriter.WriteBits(facts.HealthPct, 8);
        if (facts.Has(kFlagHasEquipment))
            aWriter.WriteBits(facts.EquipmentHash, 32);
        if (facts.Has(kFlagHasInventory))
        {
            aWriter.WriteBits(facts.InventoryHash, 32);
            aWriter.WriteBits(facts.InventoryKinds, 16);
            Serialization::WriteVarInt(aWriter, facts.InventoryItems);
        }
    }
}

void ClientEntityDigest::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    using namespace Session::EntityDigest;

    ClientMessage::DeserializeRaw(aReader);

    Sequence = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    Truncated = Serialization::ReadBool(aReader);

    // A count beyond the cap is not something this build writes: the rows that fit are read and the
    // report is treated as truncated, so nothing is concluded from what is not in it.
    const uint64_t declared = Serialization::ReadVarInt(aReader);
    const size_t count = static_cast<size_t>(std::min<uint64_t>(declared, kMaxEntries));
    if (declared > kMaxEntries)
        Truncated = true;

    Entries.clear();
    Entries.reserve(count);
    for (size_t i = 0; i < count; ++i)
    {
        // A short packet reads as zeroes; stop instead of inventing rows for server id 0.
        if (aReader.Eof())
        {
            Truncated = true;
            break;
        }

        Facts facts{};
        facts.ServerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;

        uint64_t raw = 0;
        aReader.ReadBits(raw, 8);
        facts.Flags = static_cast<uint8_t>(raw & kKnownFlags);

        raw = 0;
        aReader.ReadBits(raw, 8);
        const uint8_t health = static_cast<uint8_t>(raw & 0xFF);
        facts.HealthPct = health <= 100 ? health : kNoHealth;

        if (facts.Has(kFlagHasEquipment))
        {
            raw = 0;
            aReader.ReadBits(raw, 32);
            facts.EquipmentHash = static_cast<uint32_t>(raw & 0xFFFFFFFF);
        }
        if (facts.Has(kFlagHasInventory))
        {
            raw = 0;
            aReader.ReadBits(raw, 32);
            facts.InventoryHash = static_cast<uint32_t>(raw & 0xFFFFFFFF);
            raw = 0;
            aReader.ReadBits(raw, 16);
            facts.InventoryKinds = static_cast<uint16_t>(raw & 0xFFFF);
            facts.InventoryItems = static_cast<uint32_t>(Serialization::ReadVarInt(aReader) & 0xFFFFFFFF);
        }
        Entries.push_back(facts);
    }
}

bool ClientEntityDigest::operator==(const ClientEntityDigest& acRhs) const noexcept
{
    if (GetOpcode() != acRhs.GetOpcode() || Sequence != acRhs.Sequence || Truncated != acRhs.Truncated ||
        Entries.size() != acRhs.Entries.size())
        return false;
    for (size_t i = 0; i < Entries.size(); ++i)
    {
        const auto& a = Entries[i];
        const auto& b = acRhs.Entries[i];
        if (a.ServerId != b.ServerId || a.Flags != b.Flags || a.HealthPct != b.HealthPct || a.EquipmentHash != b.EquipmentHash ||
            a.InventoryHash != b.InventoryHash || a.InventoryKinds != b.InventoryKinds || a.InventoryItems != b.InventoryItems)
            return false;
    }
    return true;
}
