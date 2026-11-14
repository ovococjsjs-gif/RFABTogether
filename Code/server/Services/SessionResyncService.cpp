#include <Services/SessionResyncService.h>

#include <Events/PlayerLeaveEvent.h>
#include <Game/Player.h>
#include <GameServer.h>
#include <Components.h>
#include <Messages/ClientEntityDigest.h>
#include <Messages/NotifyResyncComplete.h>
#include <Messages/RequestResync.h>
#include <Services/CharacterService.h>
#include <Services/SaveSnapshotService.h>
#include <Services/StringCacheService.h>
#include <Services/WorldItemService.h>
#include <Setting.h>
#include <World.h>

#include <chrono>
#include <cstdio>
#include <unordered_set>
#include <vector>

extern Console::Setting<bool> bEnableDesyncAutoRepair;

namespace
{
uint64_t ResyncNowMs() noexcept
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
} // namespace

SessionResyncService::SessionResyncService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
    , m_requestConnection(aDispatcher.sink<PacketEvent<RequestResync>>().connect<&SessionResyncService::OnRequestResync>(this))
    , m_digestConnection(aDispatcher.sink<PacketEvent<ClientEntityDigest>>().connect<&SessionResyncService::OnEntityDigest>(this))
    , m_playerLeaveConnection(aDispatcher.sink<PlayerLeaveEvent>().connect<&SessionResyncService::OnPlayerLeave>(this))
{
}

void SessionResyncService::OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept
{
    if (acEvent.pPlayer)
    {
        m_budgetByPlayerId.erase(acEvent.pPlayer->GetId());
        m_digestByPlayerId.erase(acEvent.pPlayer->GetId());
    }
}

Session::EntityDigest::Facts SessionResyncService::BuildServerFacts(entt::entity aEntity, const Player* apObserver) const noexcept
{
    using namespace Session::EntityDigest;

    Facts facts{};
    facts.ServerId = World::ToInteger(aEntity);

    const auto& charComp = m_world.get<CharacterComponent>(aEntity);
    if (m_world.get<OwnerComponent>(aEntity).GetOwner() == apObserver)
        facts.Flags |= kFlagOwnedByReporter;
    if (charComp.IsDead())
        facts.Flags |= kFlagDead;
    if (charComp.IsPlayer())
        facts.Flags |= kFlagPlayer;

    if (const auto* pValues = m_world.try_get<ActorValuesComponent>(aEntity))
    {
        float health = 0.0f;
        float maxHealth = 0.0f;
        if (pValues->TryGetHealth(health) && pValues->TryGetMaxHealth(maxHealth))
            facts.HealthPct = HealthPercent(health, maxHealth);
    }

    // Player characters only: an NPC's outfit is put on by the engine of every client.
    if (charComp.IsPlayer())
    {
        if (const auto* pInventory = m_world.try_get<InventoryComponent>(aEntity))
        {
            EquipmentHasher hasher;
            for (const auto& entry : pInventory->Content.Entries)
            {
                if (entry.IsWorn())
                    hasher.Add(entry.BaseId.ModId, entry.BaseId.BaseId);
            }
            facts.EquipmentHash = hasher.Finish();
            facts.Flags |= kFlagHasEquipment;

            // The observer's own player: what the server believes that player carries. This copy is
            // what a rollback restores from, so it had better be the inventory the player has.
            if (apObserver && apObserver->GetCharacter().has_value() && *apObserver->GetCharacter() == aEntity)
            {
                InventoryHasher inventory;
                for (const auto& entry : pInventory->Content.Entries)
                    inventory.Add(entry.BaseId.ModId, entry.BaseId.BaseId, entry.Count);
                const InventoryHasher::Summary summary = inventory.Finish();
                facts.InventoryHash = summary.Hash;
                facts.InventoryKinds = summary.Kinds;
                facts.InventoryItems = summary.Items;
                facts.Flags |= kFlagHasInventory;
            }
        }
    }
    return facts;
}

void SessionResyncService::OnEntityDigest(const PacketEvent<ClientEntityDigest>& acMessage) noexcept
{
    using namespace Session::EntityDigest;

    Player* pPlayer = acMessage.pPlayer;
    if (!pPlayer || !IsCoopFeatureEnabled(Session::Feature::kDesyncDetector))
        return;

    // A reload barrier replaces the world both lists describe; whatever was suspected before it is
    // about the old one.
    if (m_world.GetSaveSnapshotService().GetCoordinator().HasActiveReloadBarrier())
    {
        m_digestByPlayerId.erase(pPlayer->GetId());
        return;
    }

    const ClientEntityDigest& report = acMessage.Packet;
    const uint64_t nowMs = ResyncNowMs() + m_clockSkewMs;
    const uint32_t observerId = pPlayer->GetId();

    std::vector<Facts> clientFacts(report.Entries.begin(), report.Entries.end());
    std::unordered_set<uint32_t> reportedIds;
    reportedIds.reserve(clientFacts.size());
    for (const Facts& facts : clientFacts)
        reportedIds.insert(facts.ServerId);

    // What the server expects this client to have: the characters it marked as delivered, and the
    // ones the client simulates. Another player's character may or may not be there (a client keeps
    // it after it leaves the range and never gets one it has not met): it is compared only when the
    // client lists it.
    const auto& registry = m_world.GetCharacterService().GetInterestRegistry();
    std::vector<Facts> serverFacts;
    auto characterView = m_world.view<CharacterComponent, CellIdComponent, OwnerComponent>();
    for (auto entity : characterView)
    {
        const uint32_t serverId = World::ToInteger(entity);
        const bool owned = characterView.get<OwnerComponent>(entity).GetOwner() == pPlayer;
        const bool delivered = registry.IsMarkedInRange(observerId, serverId);
        if (!owned && !delivered)
        {
            if (!characterView.get<CharacterComponent>(entity).IsPlayer() || reportedIds.find(serverId) == reportedIds.end())
                continue;
        }
        serverFacts.push_back(BuildServerFacts(entity, pPlayer));
    }

    auto& comparer = m_digestByPlayerId.try_emplace(observerId).first->second;
    std::vector<Finding> findings;
    comparer.Compare(clientFacts, serverFacts, report.Truncated, nowMs, findings);
    ++m_desyncTotals.Reports;

    // A report can differ in every row (a client that lost its world). The first lines name the
    // characters; the rest is a count.
    constexpr uint32_t kMaxLinesPerReport = 12;
    uint32_t linesWritten = 0;
    uint32_t linesSuppressed = 0;

    for (const Finding& finding : findings)
    {
        if (finding.Healed)
        {
            ++m_desyncTotals.Healed;
            spdlog::info("[RFAB Desync] player {} entity {:X} {}: gone after the resend", observerId, finding.ServerId,
                         ToString(finding.What));
            continue;
        }

        if (finding.Log)
        {
            ++m_desyncTotals.Confirmed;
            if (linesWritten >= kMaxLinesPerReport)
            {
                ++linesSuppressed;
            }
            else
            {
                ++linesWritten;

                // Who it is, for whoever reads the log: the reference, and who simulates it.
                const auto entity = static_cast<entt::entity>(finding.ServerId);
                const bool exists = m_world.valid(entity) && m_world.all_of<CharacterComponent, OwnerComponent>(entity);
                uint32_t formMod = 0;
                uint32_t formBase = 0;
                bool isPlayerCharacter = false;
                uint32_t ownerId = 0;
                if (exists)
                {
                    if (const auto* pForm = m_world.try_get<FormIdComponent>(entity))
                    {
                        formMod = pForm->Id.ModId;
                        formBase = pForm->Id.BaseId;
                    }
                    isPlayerCharacter = m_world.get<CharacterComponent>(entity).IsPlayer();
                    if (const Player* pOwner = m_world.get<OwnerComponent>(entity).GetOwner())
                        ownerId = pOwner->GetId();
                }

                char what[160]{};
                switch (finding.What)
                {
                case Field::kMissingOnClient:
                    std::snprintf(what, sizeof(what), "the server delivered it, the client does not have it");
                    break;
                case Field::kUnknownToServer:
                    std::snprintf(what, sizeof(what), "%s",
                                  exists ? "the client still has it, the server took it out of that client's range"
                                         : "the client still has it, the server has no such character");
                    break;
                case Field::kOwner:
                    std::snprintf(what, sizeof(what), "client says it %s it, server says it %s",
                                  finding.ClientValue ? "simulates" : "does not simulate", finding.ServerValue ? "does" : "does not");
                    break;
                case Field::kLifeState:
                    std::snprintf(what, sizeof(what), "client=%s server=%s", finding.ClientValue ? "dead" : "alive",
                                  finding.ServerValue ? "dead" : "alive");
                    break;
                case Field::kHealth:
                    std::snprintf(what, sizeof(what), "client=%u%% server=%u%%", finding.ClientValue, finding.ServerValue);
                    break;
                case Field::kEquipment:
                    std::snprintf(what, sizeof(what), "worn items differ (client hash %08X, server hash %08X)", finding.ClientValue,
                                  finding.ServerValue);
                    break;
                case Field::kInventory:
                {
                    // Both ends counted what they have; the numbers say how far apart the copies are.
                    uint32_t clientKinds = 0, clientItems = 0, serverKinds = 0, serverItems = 0;
                    for (const Facts& facts : clientFacts)
                    {
                        if (facts.ServerId == finding.ServerId)
                        {
                            clientKinds = facts.InventoryKinds;
                            clientItems = facts.InventoryItems;
                            break;
                        }
                    }
                    for (const Facts& facts : serverFacts)
                    {
                        if (facts.ServerId == finding.ServerId)
                        {
                            serverKinds = facts.InventoryKinds;
                            serverItems = facts.InventoryItems;
                            break;
                        }
                    }
                    std::snprintf(what, sizeof(what),
                                  "the player carries %u kinds / %u items, the server's copy has %u kinds / %u items%s",
                                  clientKinds, clientItems, serverKinds, serverItems,
                                  (clientKinds == serverKinds && clientItems == serverItems) ? " (same counts, different items)" : "");
                    break;
                }
                }

                spdlog::warn("[RFAB Desync] player {} entity {:X} ({}form {:X}:{:X}, simulated by player {}) {}: {}, for {} ms{}",
                             observerId, finding.ServerId, isPlayerCharacter ? "player character, " : "", formMod, formBase, ownerId,
                             ToString(finding.What), static_cast<const char*>(what), finding.AgeMs,
                             finding.GaveUp ? " - resent twice and still different, not resending again" : "");
            }
        }

        if (!finding.Repair || !bEnableDesyncAutoRepair)
            continue;

        auto& budget = m_budgetByPlayerId.try_emplace(observerId).first->second;
        if (!budget.Admit(Session::Resync::Scope::kEntity, nowMs))
            continue;  // offered again with the next report

        uint32_t snapshots = 0;
        const auto outcome = m_world.GetCharacterService().ResyncEntityForObserver(pPlayer, finding.ServerId, snapshots);
        comparer.NoteRepaired(finding.ServerId, finding.What, nowMs);
        ++m_desyncTotals.Repaired;
        spdlog::info("[RFAB Desync] player {} entity {:X} {}: resent ({})", observerId, finding.ServerId, ToString(finding.What),
                     Session::Resync::ToString(outcome));
    }

    if (linesSuppressed != 0)
        spdlog::warn("[RFAB Desync] player {}: {} more difference(s) in this report are not listed", observerId, linesSuppressed);
}

void SessionResyncService::OnRequestResync(const PacketEvent<RequestResync>& acMessage) noexcept
{
    using Session::Resync::Outcome;
    using Session::Resync::Scope;

    Player* pPlayer = acMessage.pPlayer;
    if (!pPlayer)
        return;

    const RequestResync& request = acMessage.Packet;

    NotifyResyncComplete complete{};
    complete.RequestId = request.RequestId;
    complete.RequestScope = request.RequestScope;
    complete.ServerId = request.ServerId;

    const auto finish = [&](Outcome aOutcome, uint32_t aSnapshots) {
        complete.Result = aOutcome;
        complete.SnapshotsSent = aSnapshots;
        pPlayer->Send(complete);
    };

    if (!request.RawScopeKnown || (request.RequestScope == Scope::kEntity && request.ServerId == Session::Resync::kNoServerId))
        return finish(Outcome::kInvalid, 0);

    if (!IsCoopFeatureEnabled(Session::Feature::kSelfHealingResync))
        return finish(Outcome::kDisabled, 0);

    // A world reload barrier rebuilds every observer itself (ResetWorldEntitiesForCheckpointReload +
    // ForceResyncObserverAfterRollback); answering now would send pre-reload state. The client
    // re-arms throttled requests and asks again after its cooldown. The string table and the dropped
    // items are session state the barrier does not rebuild (the rollback already restored and
    // broadcast the items): they are answered at once.
    const auto& saveCoordinator = m_world.GetSaveSnapshotService().GetCoordinator();
    const bool rebuiltByBarrier = request.RequestScope == Scope::kEntity || request.RequestScope == Scope::kAllEntities;
    if (rebuiltByBarrier && saveCoordinator.HasActiveReloadBarrier())
        return finish(Outcome::kThrottled, 0);

    const uint64_t nowMs = ResyncNowMs();
    auto& budget = m_budgetByPlayerId.try_emplace(pPlayer->GetId()).first->second;
    if (!budget.Admit(request.RequestScope, nowMs))
        return finish(Outcome::kThrottled, 0);

    ++m_requestsServed;
    switch (request.RequestScope)
    {
    case Scope::kEntity:
    {
        uint32_t snapshots = 0;
        const Outcome outcome = m_world.GetCharacterService().ResyncEntityForObserver(pPlayer, request.ServerId, snapshots);
        spdlog::info("[RFAB Resync] player {} entity {:X} ({}): {}", pPlayer->GetId(), request.ServerId,
                     Session::Resync::ToString(request.GapReason), Session::Resync::ToString(outcome));
        return finish(outcome, snapshots);
    }
    case Scope::kAllEntities:
    {
        const uint32_t snapshots = m_world.GetCharacterService().ResyncAllEntitiesForObserver(pPlayer);
        spdlog::info("[RFAB Resync] player {} whole session ({}): {} entities in range resent", pPlayer->GetId(),
                     Session::Resync::ToString(request.GapReason), snapshots);
        return finish(Outcome::kApplied, snapshots);
    }
    case Scope::kStringCache:
    {
        const uint32_t strings = m_world.ctx().at<StringCacheService>().ResendAllTo(pPlayer);
        spdlog::info("[RFAB Resync] player {} string table ({}): {} strings resent", pPlayer->GetId(),
                     Session::Resync::ToString(request.GapReason), strings);
        return finish(Outcome::kApplied, strings);
    }
    case Scope::kWorldItems:
    {
        // Answered even with Feature::kServerOwnedWorldItems off: items registered before a live
        // switch-off still exist and are still arbitrated.
        const uint32_t items = m_world.ctx().at<WorldItemService>().SendSnapshotTo(pPlayer);
        spdlog::info("[RFAB Resync] player {} dropped items ({}): snapshot with {} items", pPlayer->GetId(),
                     Session::Resync::ToString(request.GapReason), items);
        return finish(Outcome::kApplied, items);
    }
    }

    finish(Outcome::kInvalid, 0);
}
