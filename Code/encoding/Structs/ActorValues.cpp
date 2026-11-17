#include <Structs/ActorValues.h>
#include <TiltedCore/Serialization.hpp>
#include <common/CoopPacketSafety.h>

using TiltedPhoques::Serialization;

bool ActorValues::operator==(const ActorValues& acRhs) const noexcept
{
    return ActorValuesList == acRhs.ActorValuesList;
}

bool ActorValues::operator!=(const ActorValues& acRhs) const noexcept
{
    return !this->operator==(acRhs);
}

void ActorValues::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, ActorValuesList.size());
    for (auto& value : ActorValuesList)
    {
        Serialization::WriteVarInt(aWriter, value.first);
        Serialization::WriteFloat(aWriter, value.second);
    }

    Serialization::WriteVarInt(aWriter, ActorMaxValuesList.size());
    for (auto& value : ActorMaxValuesList)
    {
        Serialization::WriteVarInt(aWriter, value.first);
        Serialization::WriteFloat(aWriter, value.second);
    }
}

void ActorValues::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    const size_t count = CoopPacketSafety::ClampCollectionCount(
        Serialization::ReadVarInt(aReader),
        CoopPacketSafety::kMaxActorValues);
    for (size_t i = 0; i < count && !aReader.Eof(); i++)
    {
        const uint32_t key = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
        const float value = Serialization::ReadFloat(aReader);
        ActorValuesList.insert({key, value});
    }

    const size_t maxCount = CoopPacketSafety::ClampCollectionCount(
        Serialization::ReadVarInt(aReader),
        CoopPacketSafety::kMaxActorValues);
    for (size_t i = 0; i < maxCount && !aReader.Eof(); i++)
    {
        const uint32_t key = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
        const float value = Serialization::ReadFloat(aReader);
        ActorMaxValuesList.insert({key, value});
    }
}
