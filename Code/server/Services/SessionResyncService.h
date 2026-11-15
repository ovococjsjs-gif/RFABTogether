#pragma once

#include <Events/PacketEvent.h>
#include <common/Session/EntityDigest.h>
#include <common/Session/Resync.h>

#include <cstdint>
#include <unordered_map>

struct World;
struct PlayerLeaveEvent;
struct RequestResync;
struct ClientEntityDigest;
struct Player;

/**
 * Server half of the self-healing contract (architecture step 1, common/Session/Resync.h).
 *
 * Answers RequestResync with authoritative snapshots built by the services that own the state
 * (CharacterService for characters, StringCacheService for the string table) and terminates
 * every answer with NotifyResyncComplete on the same reliable ordered channel. Every request is
 * admitted through a per-player ServerBudget, independent of anything the client claims.
 */
struct SessionResyncService
{
    SessionResyncService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~SessionResyncService() noexcept = default;

    TP_NOCOPYMOVE(SessionResyncService);

    // Desync detector totals over every observer since the server started (console, tests).
    struct DesyncTotals
    {
        uint64_t Reports{0};
        uint64_t Confirmed{0};
        uint64_t Repaired{0};
        uint64_t Healed{0};
    };
    [[nodiscard]] const DesyncTotals& GetDesyncTotals() const noexcept { return m_desyncTotals; }

    // Moves this service's clock forward. The server harness uses it to put seconds between two
    // reports without sleeping; nothing in the server calls it.
    void SkewClockForTesting(uint64_t aMs) noexcept { m_clockSkewMs += aMs; }

private:
    void OnRequestResync(const PacketEvent<RequestResync>& acMessage) noexcept;
    // Desync detector (common/Session/EntityDigest.h): compares the client's report with the
    // server's state for that observer, logs confirmed differences, optionally resends.
    void OnEntityDigest(const PacketEvent<ClientEntityDigest>& acMessage) noexcept;
    void OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept;

    [[nodiscard]] Session::EntityDigest::Facts BuildServerFacts(entt::entity aEntity, const Player* apObserver) const noexcept;

    World& m_world;
    std::unordered_map<uint32_t, Session::Resync::ServerBudget> m_budgetByPlayerId{};
    std::unordered_map<uint32_t, Session::EntityDigest::Comparer> m_digestByPlayerId{};
    DesyncTotals m_desyncTotals{};
    uint64_t m_clockSkewMs{0};
    uint64_t m_requestsServed{0};

    entt::scoped_connection m_requestConnection;
    entt::scoped_connection m_digestConnection;
    entt::scoped_connection m_playerLeaveConnection;
};
