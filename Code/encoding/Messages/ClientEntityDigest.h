#pragma once

#include "Message.h"

#include <common/Session/EntityDigest.h>

/**
 * Client -> server, every few seconds: what this client believes about each character it knows by
 * server id (work plan 3.3, see common/Session/EntityDigest.h). The server compares the rows with
 * its own state for this observer, logs every confirmed difference and may resend the character.
 * There is no answer message: a repair is an ordinary CharacterSpawnRequest / NotifyRemoveCharacter.
 */
struct ClientEntityDigest final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kClientEntityDigest;

    ClientEntityDigest()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const ClientEntityDigest& acRhs) const noexcept;

    uint32_t Sequence{0};
    // The client knew more characters than fit: absence from Entries proves nothing.
    bool Truncated{false};
    TiltedPhoques::Vector<Session::EntityDigest::Facts> Entries{};
};
