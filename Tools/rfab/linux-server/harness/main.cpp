// The server, in process, driven through the same dispatcher a connection drives.
//
// This is the first slice of the integration harness the project was missing: `Code/server`
// now compiles and runs on a plain Linux box, so a server-side defect can be observed in
// seconds instead of after a playtest. Nothing here mocks the server - the handlers below
// are the shipping ones (`ActorValueService::OnHealthChangeBroadcast`, `OnDeathStateChange`,
// `OnActorValueChanges`); only the socket layer is absent.
//
// The scenarios are the ones players report as "rollbacks":
//   1. a hit from the owner lands and moves the health revision;
//   2. a killing blow fixes health at zero and moves it again;
//   3. an absolute reading sampled *before* the death is refused, and the sender is told
//      the truth - this is the defect that let dead NPCs stand back up;
//   4. a reading sampled after the death is accepted as already current (no correction
//      storm, no oscillation);
//   5. a claim carries the claimer's whole actor data - and health is still written through
//      the one writer that owns its revision, so a claim cannot move health silently.
//
// And the save barrier, which decides whether an hour of play was persisted:
//
//   6. a checkpoint is recorded only when every connected player reports its file written;
//   7. one refusal voids the barrier - no epoch is recorded, and the flight ring says "aborted";
//   8. an ack that arrives after the barrier is gone cannot commit anything;
//   9. a duplicate ack does not commit twice;
//  10. a player who leaves is no longer waited for;
//  11. the checkpoint file survives its own round trip and corruption is refused;
//  12. a rollback restores health and moves the revision;
//  13. a fresh server rebuilds actors, objects, progression, loot and calendar from the
//      checkpoint; the first join cannot overwrite the restored calendar;
//  14. after an actual process restart, a reused player ID cannot reclaim depleted loot,
//      including after rollback to either the latest or an older on-disk checkpoint.
//  15. v4.1.0 self-healing resync: a client that lost state asks, the server resends the
//      authoritative snapshot and terminates with NotifyResyncComplete - after the snapshots,
//      to the requester only, within a per-player budget, and never when switched off.
//
// The coordinator's own rules run in the fast loop (Code/tests/rfab_save_barrier.cpp). What
// was never observed is the wiring: who is told, and what the server records when a player
// refuses or answers too late.

#include <spdlog/sinks/stdout_color_sinks.h>

#include <ConsoleRegistry.h>
#include <Components.h>
#include <GameServer.h>
#include <World.h>

#include <Events/PacketEvent.h>
#include <Events/PlayerLeaveEvent.h>
#include <Events/PlayerJoinEvent.h>
#include <Messages/ActivateRequest.h>
#include <Messages/AssignCharacterRequest.h>
#include <Messages/AssignCharacterResponse.h>
#include <Messages/AssignObjectsResponse.h>
#include <Messages/AssignObjectsRequest.h>
#include <Messages/CharacterSpawnRequest.h>
#include <Messages/ClientEntityDigest.h>
#include <Messages/ClientMessageFactory.h>
#include <Messages/ClientReferencesMoveRequest.h>
#include <Messages/CoordinatedSaveAck.h>
#include <Messages/NotifyPlayerCellChanged.h>
#include <Messages/EnterInteriorCellRequest.h>
#include <Messages/ServerReferencesMoveRequest.h>
#include <Messages/NotifyActivate.h>
#include <Messages/LockChangeRequest.h>
#include <Messages/NotifyLockChange.h>
#include <Messages/ScriptAnimationRequest.h>
#include <Messages/NotifyScriptAnimation.h>
#include <Messages/RequestQuestUpdate.h>
#include <Messages/NotifyQuestUpdate.h>
#include <Messages/MountRequest.h>
#include <Messages/DrawWeaponRequest.h>
#include <Messages/NotifyDrawWeapon.h>
#include <Messages/NotifyActorValueChanges.h>
#include <Messages/NotifyDeathStateChange.h>
#include <Messages/NotifyExecuteLoad.h>
#include <Messages/NotifyEquipmentChanges.h>
#include <Messages/NotifyHealthChangeBroadcast.h>
#include <Messages/NotifyMount.h>
#include <Messages/NotifyOwnershipTransfer.h>
#include <Messages/NotifyRelinquishControl.h>
#include <Messages/NotifyRemoveCharacter.h>
#include <Messages/NotifyResyncComplete.h>
#include <Messages/NotifyRfabProgressionEvent.h>
#include <Messages/NotifySpawnData.h>
#include <Messages/RequestActorValueChanges.h>
#include <Messages/RequestCoordinatedSaveLoad.h>
#include <Messages/RequestDeathStateChange.h>
#include <Messages/RequestEquipmentChanges.h>
#include <Messages/RequestHealthChangeBroadcast.h>
#include <Messages/RequestInventoryChanges.h>
#include <Messages/RequestOwnershipClaim.h>
#include <Messages/RequestOwnershipTransfer.h>
#include <Messages/RequestResync.h>
#include <Messages/SyncRfabProgressionEvent.h>
#include <Messages/NotifySyncRfabExperience.h>
#include <Messages/SyncRfabExperienceRequest.h>
#include <Messages/NotifyInventoryChanges.h>
#include <Messages/NotifyWorldItemDespawn.h>
#include <Messages/NotifyWorldItemSnapshot.h>
#include <Messages/NotifyWorldItemSpawn.h>
#include <Messages/RequestWorldItemDrop.h>
#include <Messages/RequestWorldItemPickup.h>
#include <Messages/ServerMessageFactory.h>

#include <Services/CharacterService.h>
#include <Services/PartyService.h>
#include <Services/SaveSnapshotService.h>
#include <Services/SessionResyncService.h>
#include <Services/WorldItemService.h>

#include "TransportSpy.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

// v4.1.0: the self-healing switch lives in GameServer.cpp; the harness flips it like an operator would.
extern Console::Setting<bool> bEnableSelfHealingResync;
extern Console::Setting<bool> bEnableServerOwnedWorldItems;
extern Console::Setting<bool> bEnableLateJoinCatchUpXp;
extern Console::Setting<bool> bEnableSharedDragonSouls;
extern Console::Setting<bool> bEnableMountRideGuard;
extern Console::Setting<bool> bEnableDesyncDetector;
extern Console::Setting<bool> bEnableDesyncAutoRepair;
extern Console::Setting<bool> bEnableBossLeaseRenewal;
extern Console::Setting<bool> bEnableUnlockMemory;
extern Console::Setting<bool> bEnableBodyLootAuthority;
extern Console::Setting<bool> bEnableSummonDismissal;

namespace
{
int g_failures = 0;

// Never let the harness delete or overwrite a real server's Data/rfab_checkpoints.bin.
class TemporaryWorkingDirectory
{
public:
    TemporaryWorkingDirectory() : m_original(std::filesystem::current_path())
    {
        const auto seed = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned i = 0; i < 100; ++i)
        {
            m_path = std::filesystem::temp_directory_path() /
                     ("rfab-server-harness-" + std::to_string(seed) + "-" + std::to_string(i));
            std::error_code ec;
            if (std::filesystem::create_directory(m_path, ec))
            {
                std::filesystem::current_path(m_path);
                return;
            }
            if (ec)
                throw std::filesystem::filesystem_error("create harness directory", m_path, ec);
        }
        throw std::runtime_error("could not create a unique harness directory");
    }

    ~TemporaryWorkingDirectory()
    {
        std::error_code ec;
        std::filesystem::current_path(m_original, ec);
        if (!ec)
            std::filesystem::remove_all(m_path, ec);
    }

private:
    std::filesystem::path m_original;
    std::filesystem::path m_path;
};

void Check(bool aCondition, const char* acWhat)
{
    if (aCondition)
    {
        std::printf("  ok   %s\n", acWhat);
        return;
    }
    std::printf("  FAIL %s\n", acWhat);
    ++g_failures;
}

template <class T> void Deliver(World& aWorld, T aPacket, Player* apSender)
{
    aWorld.GetDispatcher().enqueue<PacketEvent<T>>(&aPacket, apSender);
    aWorld.GetDispatcher().update();
}

// Serializes a client packet onto a real TiltedPhoques::Buffer, extracts it through
// ClientMessageFactory::Extract (exercising the real wire bitstream parser), and
// dispatches the decoded message into the shipping server World.
template <class T> bool DeliverWire(World& aWorld, const T& acOutgoing, Player* apSender)
{
    TiltedPhoques::Buffer raw(65536);
    TiltedPhoques::Buffer::Writer writer(&raw);
    acOutgoing.Serialize(writer);

    TiltedPhoques::Buffer::Reader reader(&raw);
    const ClientMessageFactory factory;
    auto pExtracted = factory.Extract(reader);
    if (!pExtracted || pExtracted->GetOpcode() != acOutgoing.GetOpcode())
        return false;

    auto* pTyped = static_cast<T*>(pExtracted.get());
    aWorld.GetDispatcher().enqueue<PacketEvent<T>>(pTyped, apSender);
    aWorld.GetDispatcher().update();
    return true;
}

// Extracts all server packets sent to `aConn` (or broadcast via k_HSteamNetConnection_Invalid)
// from Harness::Outbox() using the real ServerMessageFactory::Extract bitstream decoder.
template <class TServerMsg>
std::vector<TServerMsg> DecodeOutboxMessagesFor(TiltedPhoques::ConnectionId_t aConn)
{
    std::vector<TServerMsg> decoded;
    const ServerMessageFactory factory;
    for (const auto& pkt : Harness::Outbox())
    {
        if (pkt.Connection != aConn && pkt.Connection != k_HSteamNetConnection_Invalid)
            continue;
        if (pkt.Bytes.empty())
            continue;

        // Pad with 16 zero bytes so TiltedCore's 64-bit word bit-reader never reads past allocation.
        TiltedPhoques::Buffer buf(pkt.Bytes.size() + 16);
        std::memset(buf.GetWriteData(), 0, pkt.Bytes.size() + 16);
        std::memcpy(buf.GetWriteData(), pkt.Bytes.data(), pkt.Bytes.size());
        TiltedPhoques::Buffer::Reader reader(&buf);
        auto pMsg = factory.Extract(reader);
        if (pMsg && pMsg->GetOpcode() == TServerMsg::Opcode)
            decoded.push_back(*static_cast<const TServerMsg*>(pMsg.get()));
    }
    return decoded;
}

// Every server opcode sent to `aConn`, in send order. The reliable ordered channel delivers in this
// order, so "the terminator comes after the snapshots" is an ordering property of the outbox.
std::vector<uint32_t> OutboxOpcodesFor(TiltedPhoques::ConnectionId_t aConn)
{
    std::vector<uint32_t> opcodes;
    const ServerMessageFactory factory;
    for (const auto& pkt : Harness::Outbox())
    {
        if ((pkt.Connection != aConn && pkt.Connection != k_HSteamNetConnection_Invalid) || pkt.Bytes.empty())
            continue;
        TiltedPhoques::Buffer buf(pkt.Bytes.size() + 16);
        std::memset(buf.GetWriteData(), 0, pkt.Bytes.size() + 16);
        std::memcpy(buf.GetWriteData(), pkt.Bytes.data(), pkt.Bytes.size());
        TiltedPhoques::Buffer::Reader reader(&buf);
        if (auto pMsg = factory.Extract(reader))
            opcodes.push_back(static_cast<uint32_t>(pMsg->GetOpcode()));
    }
    return opcodes;
}

// A connection that ends, the way the transport reports it: GameServer::OnDisconnection, the code
// that runs when a player's game closes or the link drops. It is protected in the transport's base
// class; this reaches it without changing the server. A scene that only fires PlayerLeaveEvent and
// removes the player skips what the real path does with the entities that player simulated.
struct TransportAccess : TiltedPhoques::Server
{
    using TiltedPhoques::Server::OnDisconnection;
};

void Disconnect(GameServer& aServer, TiltedPhoques::ConnectionId_t aConnectionId)
{
    TiltedPhoques::Server& transport = aServer;
    (transport.*(&TransportAccess::OnDisconnection))(aConnectionId, TiltedPhoques::Server::EDisconnectReason::Quit);
}

template <class TMap>
bool HasApproxValue(const TMap& acMap, uint32_t aKey, float aExpected, float aEps = 0.01f)
{
    const auto it = acMap.find(aKey);
    return it != acMap.end() && std::fabs(it->second - aExpected) < aEps;
}

// How many save events of one kind the server recorded for one epoch. The flight ring is the
// shipping record of what the save machinery did - it is what an operator reads after a lost
// session, so the harness reads it too instead of trusting private fields.
size_t CountSaveEvents(const SaveSnapshotCoordinator& acCoordinator, CoopFlight::FlightEventKind aKind,
                       uint64_t aEpochId)
{
    size_t count = 0;
    for (const auto& record : acCoordinator.GetFlightRing().SnapshotRecent())
    {
        if (record.Kind == aKind && record.AuxValue == aEpochId)
            ++count;
    }
    return count;
}

std::vector<uint8_t> ReadAllBytes(const std::filesystem::path& acPath)
{
    std::ifstream in(acPath, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void WriteAllBytes(const std::filesystem::path& acPath, const std::vector<uint8_t>& acBytes)
{
    std::ofstream out(acPath, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(acBytes.data()), static_cast<std::streamsize>(acBytes.size()));
}

size_t CountCharactersByFormId(World& aWorld, const GameId& acFormId)
{
    size_t count = 0;
    auto view = aWorld.view<FormIdComponent, CharacterComponent>();
    for (const entt::entity entity : view)
    {
        if (view.get<FormIdComponent>(entity).Id == acFormId)
            ++count;
    }
    return count;
}

float HealthOf(const ActorValuesComponent& acValues)
{
    float hp = -1.0f;
    return acValues.TryGetHealth(hp) ? hp : -1.0f;
}
// Executed with exec in a separate process: unlike a second World in this process, the static
// Player ID generator starts at 1. The parent has left a checkpoint whose claimant was player 1.
int CheckRestartedLoot()
{
    Console::ConsoleRegistry registry{"ConOut"};
    GameServer server{registry};
    World& world = server.GetWorld();
    Player* pPlayer = world.GetPlayerManager().Create(3);
    Check(pPlayer && pPlayer->GetId() == 1, "a new process reuses the former claimant's player ID");
    Check(world.GetSaveSnapshotService().GetSnapshotCount() >= 2, "the new process loads checkpoint history");
    if (!pPlayer)
        return 1;

    const GameId containerId{0u, 0x00054321u};
    const GameId itemId{0u, 0x00023456u};
    const entt::entity playerEntity = world.create();
    world.emplace<InventoryComponent>(playerEntity);
    pPlayer->SetCharacter(playerEntity);
    const auto attempt = [&]() {
        entt::entity container = entt::null;
        auto objects = world.view<FormIdComponent, ObjectComponent, InventoryComponent>();
        for (const auto entity : objects)
            if (objects.get<FormIdComponent>(entity).Id == containerId)
                container = entity;
        if (container == entt::null)
            return false;
        auto& inventory = world.get<InventoryComponent>(playerEntity).Content;
        inventory = Inventory{};
        Inventory::Entry item{};
        item.BaseId = itemId;
        item.Count = 1;
        inventory.AddOrRemoveEntry(item); // the client already awarded itself the item
        RequestInventoryChanges withdrawal{};
        withdrawal.ServerId = World::ToInteger(container);
        withdrawal.Item = item;
        withdrawal.Item.Count = -1;
        withdrawal.UpdateClients = false;
        Deliver(world, withdrawal, pPlayer);
        GameId lookup = itemId;
        return inventory.GetEntryCountById(lookup) == 0;
    };
    Check(attempt(), "reused player ID cannot withdraw previously depleted loot after restart");
    Check(world.GetSaveSnapshotService().TriggerRollbackToLatest(false), "the persisted checkpoint can be rolled back");
    Check(attempt(), "rollback to a persisted checkpoint still rejects the reused player ID");
    const uint64_t latestEpoch = world.GetSaveSnapshotService().GetLastCommittedEpoch();
    Check(world.GetSaveSnapshotService().TriggerRollbackToPrevious(), "an older persisted checkpoint can be rolled back");
    Check(world.GetSaveSnapshotService().GetLastCommittedEpoch() < latestEpoch, "rollback selected an older epoch");
    Check(attempt(), "rollback to an older persisted checkpoint also rejects the reused player ID");
    return g_failures == 0 ? 0 : 1;
}
} // namespace

int main(int argc, char** argv)
{
    // The console registry asserts on a logger named by its caller; the server logs through
    // "ConOut" as well.
    auto pConsoleLogger = spdlog::stdout_color_mt("ConOut");
    pConsoleLogger->set_level(spdlog::level::warn);
    spdlog::set_default_logger(pConsoleLogger);

    if (argc == 2 && std::string(argv[1]) == "--check-restarted-loot")
        return CheckRestartedLoot();

    TemporaryWorkingDirectory isolatedDirectory;
    Console::ConsoleRegistry registry{"ConOut"};

    // The service restores persisted checkpoints from `cwd/Data` on construction. A file left by
    // a previous run would decide the starting epoch and session tag, so the scene would depend on
    // whatever ran before it: remove it before the server exists.
    std::error_code removeEc;
    std::filesystem::remove(std::filesystem::current_path() / "Data" / "rfab_checkpoints.bin", removeEc);

    const GameId restartActorFormId{0u, 0x00012345u};
    const GameId restartContainerId{0u, 0x00054321u};
    const GameId depletedItemId{0u, 0x00023456u};
    const GameId remainingItemId{0u, 0x00034567u};
    const GameId restartWorldItemId{0u, 0x00045678u};
    uint64_t restartEpoch = 0;
    {
    GameServer server{registry};

    World& world = server.GetWorld();
    const GameId kCell{0x0000003Cu, 0x00000000u}; // Whiterun, interior-free exterior cell
    Player* pOwner = world.GetPlayerManager().Create(1);
    Player* pAlly = world.GetPlayerManager().Create(2);
    if (pOwner == nullptr || pAlly == nullptr)
    {
        std::printf("FAIL could not create players\n");
        return 1;
    }

    // With no checkpoint, the first host still seeds the calendar.
    Check(world.GetSaveSnapshotService().GetSnapshotCount() == 0, "a new server has no checkpoint");
    TimeModel initialHostTime{};
    initialHostTime.Time = 2.0f;
    initialHostTime.Day = 2;
    initialHostTime.Month = 1;
    initialHostTime.Year = 100;
    world.GetDispatcher().trigger(PlayerJoinEvent{pOwner, {}, {}, DateTime{initialHostTime}});
    Check(world.GetCalendarService().GetDate() == CalendarService::TDate{2, 1, 100},
          "a new server seeds its date from the first player");
    Check(world.GetCalendarService().GetTime() == CalendarService::TTime{2, 0},
          "a new server seeds its time from the first player");

    // Both peers stand in the same cell as the NPC: range filtering is what decides
    // whether a report is even considered, and a player without a cell is nowhere.
    pOwner->SetCellComponent(CellIdComponent{kCell});
    pAlly->SetCellComponent(CellIdComponent{kCell});

    // An NPC in the world, simulated by pOwner.
    const entt::entity actor = world.create();
    world.emplace<FormIdComponent>(actor, 0x00012345u, 0u);
    world.emplace<CharacterComponent>(actor);
    world.emplace<OwnerComponent>(actor, pOwner);
    // A cell is what `SendToPlayersInRange` filters on; without it every broadcast is
    // dropped and the outbox assertions would test nothing.
    world.emplace<CellIdComponent>(actor, kCell);
    auto& values = world.emplace<ActorValuesComponent>(actor);
    values.CurrentActorValues.ActorMaxValuesList[24] = 100.0f;
    values.SetHealth(50.0f, 100.0f);

    const auto actorId = static_cast<uint32_t>(actor);
    const uint32_t revisionAfterSetup = values.HealthRevision;

    std::printf("health writes reachable from the wire\n");

    // 1. A hit from the owner.
    {
        RequestHealthChangeBroadcast hit{};
        hit.Id = actorId;
        hit.DeltaHealth = -20.0f;
        hit.HitId = 1;
        Deliver(world, hit, pOwner);

        Check(std::fabs(HealthOf(values) - 30.0f) < 0.01f, "an owner's hit lands on the server");
        Check(values.HealthRevision == revisionAfterSetup + 1, "a hit moves the health revision");
    }

    const uint32_t revisionAfterHit = values.HealthRevision;

    // 2. The killing blow.
    {
        RequestDeathStateChange death{};
        death.Id = actorId;
        death.IsDead = true;
        Deliver(world, death, pOwner);

        const auto& character = world.get<CharacterComponent>(actor);
        Check(character.IsDead(), "the killing blow is recorded");
        Check(std::fabs(HealthOf(values)) < 0.01f, "a dead actor has zero health");
        Check(values.HealthRevision == revisionAfterHit + 1, "death moves the health revision");
    }

    const uint32_t revisionAtDeath = values.HealthRevision;

    // 3. The reading that used to resurrect the corpse: sampled before the death, arriving after.
    {
        Harness::ClearOutbox();

        RequestActorValueChanges stale{};
        stale.Id = actorId;
        stale.HealthRevision = revisionAfterHit; // what the owner had applied when it read
        stale.Values[24] = 30.0f;
        Deliver(world, stale, pOwner);

        Check(std::fabs(HealthOf(values)) < 0.01f, "a pre-death reading cannot restore a corpse's health");
        Check(values.HealthRevision == revisionAtDeath, "a refused reading does not move the revision");
        Check(Harness::OutboxSize() > 0, "the sender is answered with the truth instead of silence");
    }

    // 4. A reading taken after the death is current, not stale.
    {
        RequestActorValueChanges fresh{};
        fresh.Id = actorId;
        fresh.HealthRevision = revisionAtDeath;
        fresh.Values[24] = 0.0f;
        Deliver(world, fresh, pOwner);

        Check(std::fabs(HealthOf(values)) < 0.01f, "a post-death reading agrees with the server");
        Check(values.HealthRevision == revisionAtDeath, "agreement does not churn the revision");
    }

    // 5. The claim path. `RequestOwnershipClaim` carries the claimer's whole actor data, and
    //    the server applies it wholesale - but health is the one number a revision describes,
    //    so it has to go through the single writer. Written straight into the map it changed
    //    health without moving the revision, and a reading sampled before the write still
    //    carried the current revision: the corpse case of Authority/Health.h §8E, still open
    //    on this path because the snapshot bypassed `SetHealth`.
    {
        // Unowned, so the claim is eligible (a fresh owner in range is protected by the hold
        // window), and unwounded, so a claimer reporting higher max health is believed - the
        // RFAB case where Fortify-Health armor lands on load and the peer's max grows.
        // Alive again: the scenarios above left it dead, and a claim cannot resurrect a corpse -
        // that is the protection 2. put there, not something to work around.
        world.get<CharacterComponent>(actor).SetDead(false);
        world.get<OwnerComponent>(actor).SetOwner(nullptr, 0);
        values.SetHealth(100.0f, 100.0f);
        const uint32_t revisionBeforeClaim = values.HealthRevision;

        ActorData claimerData{};
        claimerData.InitialActorValues.ActorValuesList[ActorValuesComponent::kHealthActorValueIndex] = 150.0f;
        claimerData.InitialActorValues.ActorMaxValuesList[ActorValuesComponent::kHealthActorValueIndex] = 150.0f;
        claimerData.IsDead = false;

        RequestOwnershipClaim claim{};
        claim.ServerId = actorId;
        claim.NewActorData = claimerData;
        Deliver(world, claim, pAlly);

        Check(world.get<OwnerComponent>(actor).GetOwner() == pAlly, "the claim is granted");
        Check(std::fabs(HealthOf(values) - 150.0f) < 0.01f, "a claim's actor data is applied");
        Check(values.HealthRevision == revisionBeforeClaim + 1, "a claim that moves health moves the revision");
    }

    // 6. And the reason the revision has to move: a claim reporting the actor dead must zero
    //    its health, and a reading sampled before that write must not revive it.
    {
        world.get<OwnerComponent>(actor).SetOwner(nullptr, 0);
        const uint32_t revisionBeforeCorpse = values.HealthRevision;

        ActorData corpseData{};
        corpseData.InitialActorValues.ActorValuesList[ActorValuesComponent::kHealthActorValueIndex] = 50.0f;
        corpseData.InitialActorValues.ActorMaxValuesList[ActorValuesComponent::kHealthActorValueIndex] = 150.0f;
        corpseData.IsDead = true;

        RequestOwnershipClaim claim{};
        claim.ServerId = actorId;
        claim.NewActorData = corpseData;
        Deliver(world, claim, pAlly);

        Check(std::fabs(HealthOf(values)) < 0.01f, "a claim that reports the actor dead leaves no health");
        Check(values.HealthRevision == revisionBeforeCorpse + 1, "the dead claim moves the revision once");
        Check(Authority::ClassifyHealthSnapshot(HealthOf(values), 50.0f, values.HealthRevision,
                                                revisionBeforeCorpse) == Authority::SnapshotVerdict::kStaleIncrease,
              "a reading sampled before the dead claim cannot revive the corpse");
    }

    std::printf("the save barrier\n");

    auto MakeAck = [](uint64_t aEpochId, const std::string& acSlot, bool aSuccess, const char* acReason) {
        CoordinatedSaveAck ack{};
        ack.EpochId = aEpochId;
        ack.SlotName = String(acSlot.c_str());
        ack.Success = aSuccess;
        ack.Reason = String(acReason);
        return ack;
    };

    // 6. Everyone reports its file written -> the epoch is recorded, once.
    {
        auto& saves = world.GetSaveSnapshotService();
        auto& coordinator = saves.GetCoordinator();
        const uint64_t epochBefore = coordinator.GetLastCommittedEpoch();

        Check(saves.TriggerCoordinatedSave(true, nullptr), "a save starts a barrier");
        const auto pending = coordinator.GetPendingBarrier();
        Check(pending.has_value(), "the barrier is armed");
        Check(pending.has_value() && pending->ExpectedPlayerIds.size() == 2, "every connected player is expected");
        Check(coordinator.GetLastCommittedEpoch() == epochBefore, "nothing is committed while players are saving");

        if (pending.has_value())
        {
            Deliver(world, MakeAck(pending->EpochId, pending->SlotName, true, ""), pOwner);
            Check(coordinator.HasPendingBarrier(), "one ack is not enough");

            Deliver(world, MakeAck(pending->EpochId, pending->SlotName, true, ""), pAlly);
            Check(!coordinator.HasPendingBarrier(), "the last ack closes the barrier");
            Check(coordinator.GetLastCommittedEpoch() == pending->EpochId, "the committed epoch is the barrier's epoch");
            Check(CountSaveEvents(coordinator, CoopFlight::FlightEventKind::kSaveCommitted, pending->EpochId) == 1,
                  "the commit is recorded once in the flight ring");
            Check(CountSaveEvents(coordinator, CoopFlight::FlightEventKind::kSaveAborted, pending->EpochId) == 0,
                  "a committed save is not also recorded as aborted");
        }
    }

    // 7. One refusal voids the barrier: nothing is recorded, and the abort is visible afterwards.
    {
        auto& saves = world.GetSaveSnapshotService();
        auto& coordinator = saves.GetCoordinator();
        const uint64_t epochBefore = coordinator.GetLastCommittedEpoch();

        Check(saves.TriggerCoordinatedSave(false, nullptr), "a manual save starts a barrier");
        const auto pending = coordinator.GetPendingBarrier();
        Check(pending.has_value(), "the manual barrier is armed");

        if (pending.has_value())
        {
            Deliver(world, MakeAck(pending->EpochId, pending->SlotName, true, ""), pOwner);
            Deliver(world, MakeAck(pending->EpochId, pending->SlotName, false, "no disk space"), pAlly);

            Check(!coordinator.HasPendingBarrier(), "a refusal closes the barrier");
            Check(coordinator.GetLastCommittedEpoch() == epochBefore, "a refused save records no epoch");
            Check(CountSaveEvents(coordinator, CoopFlight::FlightEventKind::kSaveAborted, pending->EpochId) == 1,
                  "the abort is recorded in the flight ring");
            Check(CountSaveEvents(coordinator, CoopFlight::FlightEventKind::kSaveCommitted, pending->EpochId) == 0,
                  "a refused save is never recorded as committed");

            // 8. The refusing client finishes writing anyway and answers late. The barrier is gone,
            //    so the ack can only be ignored - it must not turn the abort into a commit.
            Deliver(world, MakeAck(pending->EpochId, pending->SlotName, true, ""), pAlly);
            Check(coordinator.GetLastCommittedEpoch() == epochBefore, "a late ack does not commit a voided barrier");
            Check(CountSaveEvents(coordinator, CoopFlight::FlightEventKind::kSaveCommitted, pending->EpochId) == 0,
                  "a late ack does not commit anything either");

            // 9. A duplicate of an ack that already counted changes nothing.
            const uint64_t committedEpoch = coordinator.GetLastCommittedEpoch();
            Deliver(world, MakeAck(pending->EpochId, pending->SlotName, true, ""), pOwner);
            Check(coordinator.GetLastCommittedEpoch() == committedEpoch, "a duplicate ack does not move the epoch");
            Check(CountSaveEvents(coordinator, CoopFlight::FlightEventKind::kSaveCommitted, pending->EpochId) == 0,
                  "a duplicate ack does not commit twice");
        }
    }

    // 10. A player who leaves while its peers are saving is no longer waited for: the barrier
    //     resolves against the players that are still there instead of timing out.
    {
        auto& saves = world.GetSaveSnapshotService();
        auto& coordinator = saves.GetCoordinator();

        Check(saves.TriggerCoordinatedSave(true, nullptr), "a save starts a barrier with three scenarios behind it");
        const auto pending = coordinator.GetPendingBarrier();
        if (pending.has_value())
        {
            Deliver(world, MakeAck(pending->EpochId, pending->SlotName, true, ""), pOwner);
            Check(coordinator.HasPendingBarrier(), "the save still waits for the other player");

            world.GetDispatcher().trigger(PlayerLeaveEvent{pAlly});
            world.GetDispatcher().update();

            Check(!coordinator.HasPendingBarrier(), "the player who left is no longer waited for");
            Check(coordinator.GetLastCommittedEpoch() == pending->EpochId,
                  "the remaining acks are enough to record the checkpoint");
        }
    }

    std::printf("the checkpoint file\n");

    // 11. A rollback is only as good as the file it reads. The format is a v2.0 body plus two
    //     trailing extension blocks (NPC spatial state, v2.0.3 identity/ledger state), each behind
    //     its own magic, and a reader that falls out of step by a single field restores a
    //     different world without complaining. Nothing exercised that path end to end.
    {
        auto& saves = world.GetSaveSnapshotService();
        const auto dataDir = std::filesystem::current_path() / "Data";
        std::error_code ec;
        std::filesystem::create_directories(dataDir, ec);
        const auto firstPath = dataDir / "rfab_harness_roundtrip_a.bin";
        const auto secondPath = dataDir / "rfab_harness_roundtrip_b.bin";

        // The same state must produce the same bytes: a checkpoint that reorders its own contents
        // between two writes cannot be compared, deduplicated or trusted.
        Check(saves.SaveCheckpointsToDisk(firstPath), "a checkpoint file is written");
        Check(saves.SaveCheckpointsToDisk(secondPath), "the same state writes a second file");
        Check(ReadAllBytes(firstPath) == ReadAllBytes(secondPath), "the same state writes identical bytes twice");

        const size_t snapshotCount = saves.GetSnapshotCount();
        const uint64_t committedEpoch = saves.GetLastCommittedEpoch();
        const std::string committedSlot = saves.GetLastCommittedSlot();

        Check(saves.LoadCheckpointsFromDisk(firstPath), "the file reads back");
        Check(saves.GetSnapshotCount() == snapshotCount, "the snapshot count survives the round trip");
        Check(saves.GetLastCommittedEpoch() == committedEpoch, "the committed epoch survives the round trip");
        Check(saves.GetLastCommittedSlot() == committedSlot, "the committed slot survives the round trip");

        Check(saves.SaveCheckpointsToDisk(secondPath), "the loaded state writes again");
        Check(ReadAllBytes(firstPath) == ReadAllBytes(secondPath),
              "load-then-save reproduces the file byte for byte");

        // A single flipped byte must be caught by the envelope hash, and a refused file must leave
        // the loaded state alone rather than half-applying it.
        auto corrupted = ReadAllBytes(firstPath);
        if (!corrupted.empty())
        {
            corrupted[corrupted.size() / 2] ^= 0xFF;
            WriteAllBytes(secondPath, corrupted);

            Check(!saves.LoadCheckpointsFromDisk(secondPath), "a corrupted checkpoint file is refused");
            Check(saves.GetLastCommittedEpoch() == committedEpoch, "a refused file leaves the state alone");
            Check(saves.GetSnapshotCount() == snapshotCount, "a refused file does not replace the history");
        }

        std::filesystem::remove(firstPath, ec);
        std::filesystem::remove(secondPath, ec);
    }

    std::printf("the rollback path\n");

    // 12. A rollback is an absolute write of the checkpoint's health, like a death or a transfer -
    //     so it has to move the revision. Written straight into the map it does not, and a reading
    //     that was in flight before the rollback still carries the current revision: the restore is
    //     undone, and a boss the party wiped comes back at whatever the last straggler saw.
    {
        auto& saves = world.GetSaveSnapshotService();
        auto& coordinator = saves.GetCoordinator();

        world.get<CharacterComponent>(actor).SetDead(false);
        values.SetHealth(40.0f, 100.0f);
        Check(std::fabs(HealthOf(values) - 40.0f) < 0.01f, "the actor is alive at 40 of 100 before the checkpoint");

        Check(saves.TriggerCoordinatedSave(true, nullptr), "a save starts for the checkpoint");
        const auto pending = coordinator.GetPendingBarrier();
        if (pending.has_value())
        {
            Deliver(world, MakeAck(pending->EpochId, pending->SlotName, true, ""), pOwner);
            Deliver(world, MakeAck(pending->EpochId, pending->SlotName, true, ""), pAlly);
        }
        Check(saves.GetSnapshotCount() > 0, "the checkpoint is recorded before the rollback");

        // After the checkpoint: the fight is lost, the actor is dead.
        values.SetHealth(0.0f, 100.0f);
        world.get<CharacterComponent>(actor).SetDead(true);
        const uint32_t revisionAtDeath = values.HealthRevision;
        Check(std::fabs(HealthOf(values)) < 0.01f, "the actor is dead before the rollback");

        Check(saves.TriggerRollbackToLatest(false), "the rollback runs");

        Check(world.valid(actor), "the checkpoint's actor survives the rollback");
        if (world.valid(actor))
        {
            auto* pChar = world.try_get<CharacterComponent>(actor);
            auto* pAv = world.try_get<ActorValuesComponent>(actor);
            Check(pChar != nullptr && !pChar->IsDead(), "the rollback brings the actor back alive");
            if (pAv != nullptr)
            {
                float restoredHp = -1.0f;
                (void)pAv->TryGetHealth(restoredHp);
                Check(std::fabs(restoredHp - 40.0f) < 0.01f, "the rollback restores the checkpoint's health");
                Check(pAv->HealthRevision != revisionAtDeath, "the restore moves the health revision");
                Check(Authority::ClassifyHealthSnapshot(restoredHp, 55.0f, pAv->HealthRevision, revisionAtDeath) ==
                          Authority::SnapshotVerdict::kStaleIncrease,
                      "a reading sampled before the rollback cannot undo the restore");
            }
        }
    }

    std::printf("the restart path\n");
    {
        auto& saves = world.GetSaveSnapshotService();
        auto& coordinator = saves.GetCoordinator();
        auto& calendar = world.GetCalendarService();
        auto& ledger = world.GetRfabProgressionService().GetLedger();
        // Persistent actors need spatial state to be recreated after startup.
        auto& movement = world.get_or_emplace<MovementComponent>(actor);
        movement.Position = {123.0f, 456.0f, 789.0f};
        world.get<CharacterComponent>(actor).SetDead(false);
        values.SetHealth(40.0f, 100.0f);
        Check(calendar.SetDate(12, 4, 205), "the checkpoint date is set");
        Check(calendar.SetTime(13, 17, 33.0f), "the checkpoint time is set");
        Check(ledger.TryGrantXp("restart-test", 123).Accepted, "the checkpoint progression is set");
        // A creature is in an invulnerable phase when the checkpoint is taken (M18).
        Check(ledger.UpdateBossGhostState(World::ToInteger(actor), true), "an invulnerable phase is in progress at the checkpoint");
        Check(pOwner->GetId() == 1, "the checkpoint claimant is player ID 1");

        LockData lock{};
        lock.IsLocked = true;
        lock.LockLevel = 42;
        Inventory contents{};
        Inventory::Entry depleted{};
        depleted.BaseId = depletedItemId;
        depleted.Count = 3;
        contents.AddOrRemoveEntry(depleted);
        Inventory::Entry remaining{};
        remaining.BaseId = remainingItemId;
        remaining.Count = 7;
        contents.AddOrRemoveEntry(remaining);
        world.GetObjectService().UpsertRestoredObject(restartContainerId, kCell, {}, {}, lock, contents);
        entt::entity container = entt::null;
        auto objects = world.view<FormIdComponent, ObjectComponent, InventoryComponent>();
        for (const auto entity : objects)
            if (objects.get<FormIdComponent>(entity).Id == restartContainerId)
                container = entity;
        Check(container != entt::null, "the checkpoint container exists");
        if (container != entt::null)
        {
            RequestInventoryChanges withdrawal{};
            withdrawal.ServerId = World::ToInteger(container);
            withdrawal.Item = depleted;
            withdrawal.Item.Count = -3;
            withdrawal.UpdateClients = false;
            Deliver(world, withdrawal, pOwner);
            const auto depletion = world.GetInventoryService().CaptureLootLedgerSnapshot();
            Check(depletion.size() == 1, "a real withdrawal records the depletion");
        }

        // A dropped item the checkpoint must carry across the restart (TLV kWorldItems).
        {
            auto& actorInventory = world.get_or_emplace<InventoryComponent>(actor).Content;
            Inventory::Entry carried{};
            carried.BaseId = restartWorldItemId;
            carried.Count = 4;
            carried.ExtraHealth = 1.5f;
            actorInventory.AddOrRemoveEntry(carried);

            // The claim scene above released the actor; a drop is only accepted from the actor's owner.
            world.get<OwnerComponent>(actor).SetOwner(pOwner, 0);
            RequestWorldItemDrop drop{};
            drop.Cookie = 31u;
            drop.ActorServerId = actorId;
            drop.Item = carried;
            drop.CellId = kCell;
            drop.Position = {11.0f, -22.5f, 33.25f};
            drop.Rotation = {0.0f, 0.0f, 1.5f};
            Check(DeliverWire(world, drop, pOwner), "the restart checkpoint's dropped item round-trips");
            GameId lookup = restartWorldItemId;
            Check(world.ctx().at<WorldItemService>().GetRegistry().Size() == 1 && actorInventory.GetEntryCountById(lookup) == 0,
                  "the drop moved the stack from the inventory into the registry");
            world.get<OwnerComponent>(actor).SetOwner(nullptr, 0);
        }

        Check(saves.TriggerCoordinatedSave(true, nullptr), "a save starts for the restart checkpoint");
        const auto pending = coordinator.GetPendingBarrier();
        Check(pending.has_value(), "the restart checkpoint has a pending barrier");
        if (pending.has_value())
        {
            Deliver(world, MakeAck(pending->EpochId, pending->SlotName, true, ""), pOwner);
            Deliver(world, MakeAck(pending->EpochId, pending->SlotName, true, ""), pAlly);
            Check(coordinator.GetLastCommittedEpoch() == pending->EpochId, "the restart checkpoint commits");
            restartEpoch = pending->EpochId;
        }
        // Keep two consecutive checkpoints containing this depletion, to exercise older history.
        Check(saves.TriggerCoordinatedSave(true, nullptr), "a second restart checkpoint starts");
        const auto secondPending = coordinator.GetPendingBarrier();
        Check(secondPending.has_value(), "the second checkpoint has a pending barrier");
        if (secondPending)
        {
            Deliver(world, MakeAck(secondPending->EpochId, secondPending->SlotName, true, ""), pOwner);
            Deliver(world, MakeAck(secondPending->EpochId, secondPending->SlotName, true, ""), pAlly);
            Check(coordinator.GetLastCommittedEpoch() == secondPending->EpochId, "the second checkpoint commits");
            restartEpoch = secondPending->EpochId;
        }
        Check(saves.TriggerRollbackToLatest(false), "an in-process checkpoint can be rolled back");
        const auto sameProcessLedger = world.GetInventoryService().CaptureLootLedgerSnapshot();
        Check(sameProcessLedger.size() == 1 && sameProcessLedger[0].ClaimantPlayerId == pOwner->GetId(),
              "an in-process rollback keeps the current process's claimant ID");
        Check(saves.SaveCheckpointsToDisk(SaveSnapshotService::GetDefaultCheckpointStorePath()),
              "the restart checkpoint is written to the default path");
    }
    } // Destroy the first server and its world before constructing the next one.

    std::printf("the real process restart path\n");
    const auto storePath = SaveSnapshotService::GetDefaultCheckpointStorePath();
    const auto originalCheckpoint = ReadAllBytes(storePath);
    Check(!originalCheckpoint.empty(), "the child receives a nonempty checkpoint");
    std::fflush(nullptr);
    const pid_t child = fork();
    if (child == 0)
    {
        execl("/proc/self/exe", "servertests", "--check-restarted-loot", static_cast<char*>(nullptr));
        _exit(127);
    }
    int childStatus = 0;
    const bool childWaited = child > 0 && waitpid(child, &childStatus, 0) == child;
    Check(childWaited && WIFEXITED(childStatus) && WEXITSTATUS(childStatus) == 0,
          "the checkpoint survives a fresh process and a reused player ID");
    // Child rollbacks rewrote the store; restore the original file for the independent tests below.
    if (!originalCheckpoint.empty())
        WriteAllBytes(storePath, originalCheckpoint);

    {
        Console::ConsoleRegistry restartRegistry{"ConOut"};
        GameServer restarted{restartRegistry};
        World& restored = restarted.GetWorld();
        const auto& saves = restored.GetSaveSnapshotService();
        Check(restartEpoch != 0 && saves.GetLastCommittedEpoch() == restartEpoch,
              "a fresh server reads the committed epoch in its constructor");
        const auto rewrittenPath = std::filesystem::current_path() / "Data" / "checkpoint-rewritten.bin";
        Check(saves.SaveCheckpointsToDisk(rewrittenPath), "the loaded checkpoint can be written again");
        Check(ReadAllBytes(rewrittenPath) == originalCheckpoint,
              "forgetting runtime claimant IDs does not alter the checkpoint file");
        Check(CountCharactersByFormId(restored, restartActorFormId) == 1,
              "a fresh server rebuilds the persistent actor exactly once");
        auto actors = restored.view<FormIdComponent, CharacterComponent>();
        for (const entt::entity entity : actors)
        {
            if (actors.get<FormIdComponent>(entity).Id != restartActorFormId)
                continue;
            auto* pAv = restored.try_get<ActorValuesComponent>(entity);
            float hp = -1.0f;
            Check(pAv != nullptr && pAv->TryGetHealth(hp) && std::fabs(hp - 40.0f) < 0.01f,
                  "the rebuilt actor carries the checkpoint's health");
            Check(!actors.get<CharacterComponent>(entity).IsDead(), "the rebuilt actor is alive");
            Check(restored.all_of<OwnerComponent>(entity) && restored.get<OwnerComponent>(entity).GetOwner() == nullptr,
                  "the rebuilt actor starts without an owner");
            const auto* pMov = restored.try_get<MovementComponent>(entity);
            Check(pMov && std::fabs(pMov->Position.x - 123.0f) < 0.01f &&
                      std::fabs(pMov->Position.y - 456.0f) < 0.01f &&
                      std::fabs(pMov->Position.z - 789.0f) < 0.01f,
                  "the rebuilt actor carries the checkpoint's position");
        }
        auto restoredObjects = restored.view<FormIdComponent, ObjectComponent, InventoryComponent>();
        size_t containerCount = 0;
        entt::entity restoredContainer = entt::null;
        for (const auto entity : restoredObjects)
        {
            if (restoredObjects.get<FormIdComponent>(entity).Id == restartContainerId)
            {
                ++containerCount;
                restoredContainer = entity;
            }
        }
        Check(containerCount == 1, "the fresh server rebuilds the checkpoint container exactly once");
        if (restoredContainer != entt::null)
        {
            const auto& obj = restoredObjects.get<ObjectComponent>(restoredContainer);
            const auto& inventory = restoredObjects.get<InventoryComponent>(restoredContainer).Content;
            GameId lookupDepleted = depletedItemId;
            GameId lookupRemaining = remainingItemId;
            Check(obj.CurrentLockData.IsLocked && obj.CurrentLockData.LockLevel == 42,
                  "the restored container keeps its lock");
            Check(inventory.GetEntryCountById(lookupDepleted) == 0 &&
                      inventory.GetEntryCountById(lookupRemaining) == 7,
                  "the restored container keeps its checkpoint inventory");
            const auto depletion = restored.GetInventoryService().CaptureLootLedgerSnapshot();
            const auto stableKey = restored.GetInventoryService().ResolveLootContainerKey(restoredContainer, 0);
            Check(depletion.size() == 1 && depletion[0].ContainerServerId == stableKey &&
                      depletion[0].ModId == depletedItemId.ModId && depletion[0].BaseId == depletedItemId.BaseId &&
                      depletion[0].ClaimantPlayerId == 0,
                  "the depletion keeps its stable key but its old claimant is unknown");
        }
        {
            const auto& worldItems = restored.ctx().at<WorldItemService>().GetRegistry();
            const auto records = worldItems.Export();
            Check(records.size() == 1, "the fresh server restores the dropped item");
            if (records.size() == 1)
            {
                const auto& record = records[0];
                Check(record.Count == 4 && record.Payload.Item.BaseId == restartWorldItemId &&
                          std::fabs(record.Payload.Item.ExtraHealth - 1.5f) < 0.001f && record.Payload.CellId == GameId{0x0000003Cu, 0u} &&
                          record.Payload.Position == glm::vec3(11.0f, -22.5f, 33.25f) && std::fabs(record.Payload.Rotation.z - 1.5f) < 1e-6f,
                      "the restored dropped item keeps its stack, extra data, cell and exact position");
                Check(record.DropperCookie == 0, "a restored item carries no cookie a new client could adopt");
                Check(worldItems.GetRevision() != 0, "the restored registry starts at a nonzero revision");
            }
        }
        const auto progress = restored.GetRfabProgressionService().GetLedger().CaptureSnapshot();
        Check(progress.GrantedXpBySource.count("restart-test") == 1,
              "the fresh server restores the progression ledger");
        Check(progress.BossGhostStates.empty(),
              "an invulnerable phase is not restored into a new process, where its server id belongs to another creature");
        Check(restored.GetCalendarService().GetDate() == CalendarService::TDate{12, 4, 205},
              "the fresh server restores the checkpoint date");
        Check(restored.GetCalendarService().GetTime() == CalendarService::TTime{13, 17},
              "the fresh server restores the checkpoint time");
        // GameServer::UpdateTimeScale deliberately reapplies its configured scale at startup.
        Check(std::fabs(restored.GetCalendarService().GetTimeScale() - 20.0f) < 0.01f,
              "the configured time scale still takes precedence at server startup");
        Check(!restored.GetCalendarService().RestoreCheckpoint(99, 17, 20.0f, 12, 4, 205),
              "an invalid checkpoint calendar is rejected");
        Check(restored.GetCalendarService().GetTime() == CalendarService::TTime{13, 17} &&
                  restored.GetCalendarService().GetDate() == CalendarService::TDate{12, 4, 205},
              "a rejected calendar leaves the existing state untouched");
        Player* pHost = restored.GetPlayerManager().Create(3);
        Check(pHost != nullptr, "a player can join the restarted server");
        if (pHost)
        {
            TimeModel hostTime{};
            hostTime.Time = 2.0f;
            hostTime.Day = 2;
            hostTime.Month = 1;
            hostTime.Year = 100;
            Harness::ClearOutbox();
            restored.GetDispatcher().trigger(PlayerJoinEvent{pHost, {}, {}, DateTime{hostTime}});
            {
                const auto snapshots = DecodeOutboxMessagesFor<NotifyWorldItemSnapshot>(3);
                Check(snapshots.size() == 1 && snapshots[0].Items.size() == 1 && snapshots[0].Items[0].Item.Count == 4 &&
                          snapshots[0].Items[0].Item.BaseId == restartWorldItemId,
                      "a player joining the restarted server receives the restored dropped item");
            }
            Check(restored.GetCalendarService().GetDate() == CalendarService::TDate{12, 4, 205},
                  "the first player does not overwrite the restored date");
            Check(restored.GetCalendarService().GetTime() == CalendarService::TTime{13, 17},
                  "the first player does not overwrite the restored time");
            if (restoredContainer != entt::null)
            {
                // Model a client that already awarded itself a duplicated item. The server must
                // use the persisted depletion to remove it again through the normal packet path.
                const entt::entity playerEntity = restored.create();
                auto& playerInventory = restored.emplace<InventoryComponent>(playerEntity).Content;
                Inventory::Entry duplicate{};
                duplicate.BaseId = depletedItemId;
                duplicate.Count = 1;
                playerInventory.AddOrRemoveEntry(duplicate);
                pHost->SetCharacter(playerEntity);
                RequestInventoryChanges lateWithdrawal{};
                lateWithdrawal.ServerId = World::ToInteger(restoredContainer);
                lateWithdrawal.Item = duplicate;
                lateWithdrawal.Item.Count = -1;
                lateWithdrawal.UpdateClients = false;
                Deliver(restored, lateWithdrawal, pHost);
                GameId lookup = depletedItemId;
                Check(playerInventory.GetEntryCountById(lookup) == 0,
                      "the restored depletion rejects a late peer's duplicate withdrawal");
            }
        }
    }

    // Leave no checkpoint history behind: the scenes above wrote one, and the next run would
    // otherwise restore it before the first assertion.
    std::filesystem::remove(std::filesystem::current_path() / "Data" / "rfab_checkpoints.bin", removeEc);

    // =========================================================================
    // v4.0.0 Full Wire-Decoded Multi-Peer Protocol & Authority Integration Suite
    // =========================================================================
    std::printf("v4.0.0 wire-decoded multi-peer protocol & authority\n");
    {
        GameServer server{registry};
        World& world = server.GetWorld();

        constexpr TiltedPhoques::ConnectionId_t kHostConn = 101;
        constexpr TiltedPhoques::ConnectionId_t kGuest2Conn = 102;
        constexpr TiltedPhoques::ConnectionId_t kGuest3Conn = 103;
        const GameId kDungeonCell{0u, 0x0009ABCDu};
        const GameId kFarCell{0u, 0x0009FFFEu};
        const GameId kPlacedRefId{1u, 0x000E5F72u};
        const GameId kRestlessDraugrBase{2u, 0x00023BC0u};
        const GameId kDraugrScourgeBase{2u, 0x00023BC1u};

        Player* pHost = world.GetPlayerManager().Create(kHostConn);
        Player* pGuest2 = world.GetPlayerManager().Create(kGuest2Conn);
        Player* pGuest3 = world.GetPlayerManager().Create(kGuest3Conn);
        Check(pHost && pGuest2 && pGuest3, "three co-op peers connect to the server");

        TimeModel t0{};
        t0.Time = 12.0f;
        t0.Day = 5;
        t0.Month = 3;
        t0.Year = 201;
        world.GetDispatcher().trigger(PlayerJoinEvent{pHost, {}, {}, DateTime{t0}});
        world.GetDispatcher().trigger(PlayerJoinEvent{pGuest2, {}, {}, DateTime{t0}});
        world.GetDispatcher().trigger(PlayerJoinEvent{pGuest3, {}, {}, DateTime{t0}});

        Check(world.GetPartyService().IsPlayerLeader(pHost) &&
                  world.GetPartyService().IsPlayerInParty(pGuest2) &&
                  world.GetPartyService().IsPlayerInParty(pGuest3),
              "auto-party binds Host as party leader and both guests as party members");

        pHost->SetCellComponent(CellIdComponent{kFarCell});
        pGuest2->SetCellComponent(CellIdComponent{kDungeonCell});
        pGuest3->SetCellComponent(CellIdComponent{kFarCell});

        // 1. Guest2 streams the dungeon cell first and assigns a placed TESLevCharacter
        //    rolled locally as Restless Draugr (250/250 HP). Everything below is verified
        //    strictly from ServerMessageFactory-decoded wire bytes — zero server RAM peeking!
        Harness::ClearOutbox();
        AssignCharacterRequest g2Assign{};
        g2Assign.Cookie = 11u;
        g2Assign.ReferenceId = kPlacedRefId;
        g2Assign.FormId = kRestlessDraugrBase;
        g2Assign.CellId = kDungeonCell;
        g2Assign.Position = {100.0f, 200.0f, 0.0f};
        g2Assign.CurrentActorData.BaseId = kRestlessDraugrBase;
        g2Assign.CurrentActorData.InitialActorValues.ActorValuesList[24] = 250.0f;
        g2Assign.CurrentActorData.InitialActorValues.ActorMaxValuesList[24] = 250.0f;
        Check(DeliverWire(world, g2Assign, pGuest2), "Guest2 AssignCharacterRequest round-trips over the wire");

        const auto g2AssignResps = DecodeOutboxMessagesFor<AssignCharacterResponse>(kGuest2Conn);
        Check(g2AssignResps.size() == 1 && g2AssignResps[0].Owner && g2AssignResps[0].OwnershipEpoch >= 1 &&
                  g2AssignResps[0].HealthRevision >= 1,
              "AssignCharacterResponse delivers initial OwnershipEpoch and non-zero HealthRevision on the wire");

        const uint32_t npcServerId = g2AssignResps.empty() ? 0u : g2AssignResps[0].ServerId;
        uint64_t g2KnownEpoch = g2AssignResps.empty() ? 0ull : g2AssignResps[0].OwnershipEpoch;
        uint32_t g2KnownHealthRev = g2AssignResps.empty() ? 0u : g2AssignResps[0].HealthRevision;

        // 2. Guest2 (owner) lands a -40 HP hit -> server sends empty-Values NotifyActorValueChanges
        //    revision ack back to Guest2 so Guest2's next regen snapshot is NOT falsely rejected!
        Harness::ClearOutbox();
        RequestHealthChangeBroadcast g2Hit{};
        g2Hit.Id = npcServerId;
        g2Hit.DeltaHealth = -40.0f;
        g2Hit.HitId = 501u;
        Check(DeliverWire(world, g2Hit, pGuest2), "Guest2 RequestHealthChangeBroadcast round-trips over the wire");

        const auto g2HitAcks = DecodeOutboxMessagesFor<NotifyActorValueChanges>(kGuest2Conn);
        Check(g2HitAcks.size() == 1 && g2HitAcks[0].Id == npcServerId && g2HitAcks[0].Values.empty() &&
                  g2HitAcks[0].HealthRevision == g2KnownHealthRev + 1,
              "owner health delta emits an empty-Values NotifyActorValueChanges HealthRevision ack on the wire");
        if (!g2HitAcks.empty())
            g2KnownHealthRev = g2HitAcks[0].HealthRevision;

        // Guest3 enters the cell to observe Guest2's subsequent regen broadcast.
        pGuest3->SetCellComponent(CellIdComponent{kDungeonCell});
        world.GetCharacterService().ReconcileAllEntitiesForObserver(pGuest3);
        const auto g3InitialSpawns = DecodeOutboxMessagesFor<CharacterSpawnRequest>(kGuest3Conn);
        Check(!g3InitialSpawns.empty() && g3InitialSpawns.back().ServerId == npcServerId &&
                  g3InitialSpawns.back().BaseId == kRestlessDraugrBase &&
                  g3InitialSpawns.back().HealthRevision == g2KnownHealthRev,
              "CharacterSpawnRequest delivers BaseId and current HealthRevision to arriving observer");

        // Now Guest2 reports legitimate +15 HP Requiem troll/draugr regen (210 -> 225) echoing g2KnownHealthRev.
        Harness::ClearOutbox();
        RequestActorValueChanges g2Regen{};
        g2Regen.Id = npcServerId;
        g2Regen.HealthRevision = g2KnownHealthRev;
        g2Regen.Values[24] = 225.0f;
        Check(DeliverWire(world, g2Regen, pGuest2), "Guest2 regen RequestActorValueChanges round-trips over the wire");

        const auto g2RegenAcks = DecodeOutboxMessagesFor<NotifyActorValueChanges>(kGuest2Conn);
        const auto g3RegenBroadcasts = DecodeOutboxMessagesFor<NotifyActorValueChanges>(kGuest3Conn);
        Check(g2RegenAcks.size() == 1 && g2RegenAcks[0].Values.empty() &&
                  g2RegenAcks[0].HealthRevision == g2KnownHealthRev + 1,
              "legitimate owner regen echoing acked HealthRevision is accepted and acked (never kStaleIncrease)");
        Check(g3RegenBroadcasts.size() == 1 &&
                  HasApproxValue(g3RegenBroadcasts[0].Values, 24u, 225.0f) &&
                  g3RegenBroadcasts[0].HealthRevision == g2KnownHealthRev + 1,
              "accepted owner regen broadcasts updated HP=225 and new HealthRevision to observers");
        if (!g2RegenAcks.empty())
            g2KnownHealthRev = g2RegenAcks[0].HealthRevision;

        // 3. Party Leader (pHost) enters the cell where RFAB rolled Draugr Scourge for the SAME placed ref!
        //    Verify calm leader takeover preserves wounded HP=225 AND propagates Draugr Scourge BaseId to peers!
        pHost->SetCellComponent(CellIdComponent{kDungeonCell});
        Harness::ClearOutbox();
        AssignCharacterRequest hostAssign{};
        hostAssign.Cookie = 22u;
        hostAssign.ReferenceId = kPlacedRefId;
        hostAssign.FormId = kDraugrScourgeBase;
        hostAssign.CellId = kDungeonCell;
        hostAssign.Position = {100.0f, 200.0f, 0.0f};
        hostAssign.CurrentActorData.BaseId = kDraugrScourgeBase;
        hostAssign.CurrentActorData.InitialActorValues.ActorValuesList[24] = 400.0f;
        hostAssign.CurrentActorData.InitialActorValues.ActorMaxValuesList[24] = 400.0f;
        Check(DeliverWire(world, hostAssign, pHost), "Host AssignCharacterRequest round-trips over the wire");

        const auto hostAssignResps = DecodeOutboxMessagesFor<AssignCharacterResponse>(kHostConn);
        const auto g2Relinquishes = DecodeOutboxMessagesFor<NotifyRelinquishControl>(kGuest2Conn);
        const auto g2SpawnDatas = DecodeOutboxMessagesFor<NotifySpawnData>(kGuest2Conn);
        const auto g3SpawnDatas = DecodeOutboxMessagesFor<NotifySpawnData>(kGuest3Conn);

        Check(hostAssignResps.size() == 1 && hostAssignResps[0].Owner &&
                  hostAssignResps[0].OwnershipEpoch > g2KnownEpoch &&
                  hostAssignResps[0].HealthRevision >= g2KnownHealthRev &&
                  HasApproxValue(hostAssignResps[0].AllActorValues.ActorValuesList, 24u, 225.0f),
              "Party Leader calm takeover grants new epoch, carries HealthRevision, and preserves wounded HP=225");
        Check(g2Relinquishes.size() == 1 && g2Relinquishes[0].ServerId == npcServerId &&
                  g2Relinquishes[0].OwnershipEpoch == hostAssignResps[0].OwnershipEpoch,
              "previous owner receives NotifyRelinquishControl with matching authoritative epoch");
        Check(!g2SpawnDatas.empty() && g2SpawnDatas.back().NewActorData.BaseId == kDraugrScourgeBase &&
                  !g3SpawnDatas.empty() && g3SpawnDatas.back().NewActorData.BaseId == kDraugrScourgeBase &&
                  g2SpawnDatas.back().HealthRevision == hostAssignResps[0].HealthRevision,
              "TESLevCharacter static BaseId parity propagates to all peers via NotifySpawnData on takeover");

        uint64_t hostEpoch = hostAssignResps[0].OwnershipEpoch;
        uint32_t authHealthRev = hostAssignResps[0].HealthRevision;

        // 4. Pure server-granted RequestOwnershipClaim rules:
        //    (a) Out-of-range peer claim is rejected with NotifyRelinquishControl.
        //    (b) Current owner redundant claim (kAlreadyOwner) does NOT emit NotifyOwnershipTransfer (no loop!).
        pGuest3->SetCellComponent(CellIdComponent{kFarCell});
        Harness::ClearOutbox();
        RequestOwnershipClaim farClaim{};
        farClaim.ServerId = npcServerId;
        farClaim.NewActorData = hostAssign.CurrentActorData;
        farClaim.NewActorData.InitialActorValues.ActorValuesList[24] = 225.0f;
        Check(DeliverWire(world, farClaim, pGuest3), "out-of-range RequestOwnershipClaim round-trips over the wire");
        const auto farRejects = DecodeOutboxMessagesFor<NotifyRelinquishControl>(kGuest3Conn);
        const auto farGrants = DecodeOutboxMessagesFor<NotifyOwnershipTransfer>(kGuest3Conn);
        Check(farRejects.size() == 1 && farGrants.empty() && farRejects[0].OwnershipEpoch == hostEpoch,
              "out-of-range RequestOwnershipClaim is rejected with NotifyRelinquishControl at current epoch");

        Harness::ClearOutbox();
        RequestOwnershipClaim ownerSelfClaim{};
        ownerSelfClaim.ServerId = npcServerId;
        ownerSelfClaim.NewActorData = farClaim.NewActorData;
        Check(DeliverWire(world, ownerSelfClaim, pHost), "owner kAlreadyOwner RequestOwnershipClaim round-trips");
        const auto selfClaimGrants = DecodeOutboxMessagesFor<NotifyOwnershipTransfer>(kHostConn);
        Check(selfClaimGrants.empty(),
              "kAlreadyOwner RequestOwnershipClaim never echoes NotifyOwnershipTransfer (zero claim/grant loop)");

        // 5. Self-healing WorldNpcLeasePolicy expiry:
        //    - Unpaused idle keepalives renew Host's lease without churning epoch.
        //    - Solo menu pause (no active peer in range) NEVER revokes Host's lease.
        //    - Co-op menu pause while Guest2 is actively simulating hands off cleanly on the wire!
        pGuest2->SetCellComponent(CellIdComponent{kFarCell});
        const uint64_t baseMs = 5'000'000'000ull;
        world.GetCharacterService().NotePlayerSimulationActivity(pHost, baseMs);

        ClientReferencesMoveRequest idleKeepalive{};
        idleKeepalive.Tick = 100u;
        Check(DeliverWire(world, idleKeepalive, pHost), "5-byte empty unpaused keepalive round-trips over the wire");

        // Re-anchor deterministic timestamps for exact lease boundary checks:
        world.GetCharacterService().NotePlayerSimulationActivity(pHost, baseMs + 2000ull);
        Harness::ClearOutbox();
        world.GetCharacterService().SweepExpiredWorldNpcLeases(baseMs + 15000ull); // > 8000ms, Guest2/Guest3 out of range
        Check(DecodeOutboxMessagesFor<NotifyRelinquishControl>(kHostConn).empty(),
              "solo pause menu (>8s without keepalives, no active peer in range) never revokes world NPC lease");

        // Host closes solo pause menu at baseMs + 16000 -> RefreshUnrevoked restores lease without epoch bump.
        world.GetCharacterService().NotePlayerSimulationActivity(pHost, baseMs + 16000ull);
        Check(world.get<OwnerComponent>(static_cast<entt::entity>(npcServerId)).GetOwner() == pHost,
              "resuming simulation after solo pause keeps Host as owner without churn");

        // Now Guest2 enters the cell and actively simulates while Host pauses in StatsMenu for >8s!
        pGuest2->SetCellComponent(CellIdComponent{kDungeonCell});
        pGuest3->SetCellComponent(CellIdComponent{kDungeonCell});
        world.GetCharacterService().NotePlayerSimulationActivity(pGuest2, baseMs + 24500ull);
        world.GetCharacterService().NotePlayerSimulationActivity(pGuest3, baseMs + 24500ull);
        Harness::ClearOutbox();
        world.GetCharacterService().SweepExpiredWorldNpcLeases(baseMs + 25000ull); // Host last active at +16000 (9s ago!)

        const auto hostExpiredRelinquish = DecodeOutboxMessagesFor<NotifyRelinquishControl>(kHostConn);
        const auto g2ExpiredGrant = DecodeOutboxMessagesFor<NotifyOwnershipTransfer>(kGuest2Conn);
        Check(hostExpiredRelinquish.size() == 1 && g2ExpiredGrant.size() == 1 &&
                  g2ExpiredGrant[0].ServerId == npcServerId &&
                  g2ExpiredGrant[0].OwnershipEpoch == hostExpiredRelinquish[0].OwnershipEpoch &&
                  g2ExpiredGrant[0].OwnershipEpoch > hostEpoch &&
                  g2ExpiredGrant[0].HealthRevision >= authHealthRev,
              "co-op menu pause (>8s) hands expired world NPC lease to active peer with matching epoch & HealthRevision");

        const uint64_t g2HandedEpoch = g2ExpiredGrant.empty() ? 0ull : g2ExpiredGrant[0].OwnershipEpoch;

        // 6. 3-peer cascade DeclineList: Guest2 declines (streaming cell -> Position {0,0,0});
        //    Host is already in DeclineList and Guest2 is added -> server must pick Guest3 (never bounce to Host!).
        Harness::ClearOutbox();
        RequestOwnershipTransfer g2Decline{};
        g2Decline.ServerId = npcServerId;
        g2Decline.OwnershipEpoch = g2HandedEpoch;
        Check(DeliverWire(world, g2Decline, pGuest2), "Guest2 DeclineGrant RequestOwnershipTransfer round-trips");

        const auto g3CascadeGrant = DecodeOutboxMessagesFor<NotifyOwnershipTransfer>(kGuest3Conn);
        const auto hostCascadeGrant = DecodeOutboxMessagesFor<NotifyOwnershipTransfer>(kHostConn);
        Check(g3CascadeGrant.size() == 1 && hostCascadeGrant.empty() &&
                  g3CascadeGrant[0].OwnershipEpoch > g2HandedEpoch,
              "3-peer cascade decline skips both declined peers and grants Guest3 without ping-pong");

        const uint32_t revBeforeKill = g3CascadeGrant.empty() ? authHealthRev : g3CascadeGrant[0].HealthRevision;

        // 7. Wire NotifyDeathStateChange carries HealthRevision and blocks stale pre-death snapshots.
        Harness::ClearOutbox();
        RequestDeathStateChange g2Kill{};
        g2Kill.Id = npcServerId;
        g2Kill.IsDead = true;
        Check(DeliverWire(world, g2Kill, pGuest2), "RequestDeathStateChange round-trips over the wire");

        const auto g3DeathNotifies = DecodeOutboxMessagesFor<NotifyDeathStateChange>(kGuest3Conn);
        const auto g2KillRevAcks = DecodeOutboxMessagesFor<NotifyActorValueChanges>(kGuest2Conn);
        Check(g3DeathNotifies.size() == 1 && g3DeathNotifies[0].IsDead &&
                  g3DeathNotifies[0].HealthRevision > revBeforeKill &&
                  g2KillRevAcks.size() == 1 &&
                  g2KillRevAcks[0].HealthRevision == g3DeathNotifies[0].HealthRevision,
              "NotifyDeathStateChange delivers bumped HealthRevision to peers and acks killer on the wire");

        const uint32_t deadWireRev = g3DeathNotifies.empty() ? 0u : g3DeathNotifies[0].HealthRevision;

        // Guest3 (current owner) sends an in-flight pre-death health reading echoing revBeforeKill -> refused & corrected!
        Harness::ClearOutbox();
        RequestActorValueChanges staleOwnerSnap{};
        staleOwnerSnap.Id = npcServerId;
        staleOwnerSnap.HealthRevision = revBeforeKill;
        staleOwnerSnap.Values[24] = 225.0f;
        Check(DeliverWire(world, staleOwnerSnap, pGuest3), "stale owner snapshot round-trips over the wire");

        const auto g3Corrections = DecodeOutboxMessagesFor<NotifyActorValueChanges>(kGuest3Conn);
        Check(g3Corrections.size() == 1 &&
                  HasApproxValue(g3Corrections[0].Values, 24u, 0.0f) &&
                  g3Corrections[0].HealthRevision == deadWireRev,
              "stale pre-death snapshot is refused and answered on the wire with HP=0 and dead HealthRevision");

        std::filesystem::remove(std::filesystem::current_path() / "Data" / "rfab_checkpoints.bin", removeEc);
    }


    // =========================================================================
    // v4.1.0 Self-healing resync: RequestResync -> snapshots -> NotifyResyncComplete
    // =========================================================================
    std::printf("v4.1.0 self-healing resync over the wire\n");
    {
        using Session::Resync::Outcome;
        using Session::Resync::Reason;
        using Session::Resync::Scope;

        GameServer server{registry};
        World& world = server.GetWorld();

        constexpr TiltedPhoques::ConnectionId_t kOwnerConn = 201;
        constexpr TiltedPhoques::ConnectionId_t kObserverConn = 202;
        const GameId kCell{0u, 0x0009ABCDu};
        const GameId kRefId{1u, 0x000E5F80u};
        const GameId kBaseId{2u, 0x00023BC0u};

        Player* pOwner = world.GetPlayerManager().Create(kOwnerConn);
        Player* pObserver = world.GetPlayerManager().Create(kObserverConn);
        Check(pOwner && pObserver, "two peers connect for the resync scene");

        TimeModel t0{};
        t0.Time = 12.0f;
        t0.Day = 5;
        t0.Month = 3;
        t0.Year = 201;
        world.GetDispatcher().trigger(PlayerJoinEvent{pOwner, {}, {}, DateTime{t0}});
        world.GetDispatcher().trigger(PlayerJoinEvent{pObserver, {}, {}, DateTime{t0}});
        pOwner->SetCellComponent(CellIdComponent{kCell});
        pObserver->SetCellComponent(CellIdComponent{kCell});

        Harness::ClearOutbox();
        AssignCharacterRequest assign{};
        assign.Cookie = 21u;
        assign.ReferenceId = kRefId;
        assign.FormId = kBaseId;
        assign.CellId = kCell;
        assign.Position = {10.0f, 20.0f, 0.0f};
        assign.CurrentActorData.BaseId = kBaseId;
        assign.CurrentActorData.InitialActorValues.ActorValuesList[24] = 120.0f;
        assign.CurrentActorData.InitialActorValues.ActorMaxValuesList[24] = 120.0f;
        Check(DeliverWire(world, assign, pOwner), "owner assigns the NPC the observer will lose");
        const auto assignResps = DecodeOutboxMessagesFor<AssignCharacterResponse>(kOwnerConn);
        // The first character of a fresh server is entt entity 0 - exactly the id an "0 means none"
        // shortcut would refuse. The scene keeps it on purpose.
        Check(assignResps.size() == 1, "the NPC is assigned a server id");
        const uint32_t npcServerId = assignResps.empty() ? Session::Resync::kNoServerId : assignResps[0].ServerId;

        const auto requestFrom = [&](Player* apFrom, uint32_t aRequestId, Scope aScope, uint32_t aServerId, Reason aReason) {
            Harness::ClearOutbox();
            RequestResync request{};
            request.RequestId = aRequestId;
            request.RequestScope = aScope;
            request.ServerId = aServerId;
            request.GapReason = aReason;
            return DeliverWire(world, request, apFrom);
        };
        const auto onlyComplete = [](TiltedPhoques::ConnectionId_t aConn) {
            const auto completes = DecodeOutboxMessagesFor<NotifyResyncComplete>(aConn);
            return completes.size() == 1 ? completes[0] : NotifyResyncComplete{};
        };
        const auto terminatorIsLast = [](TiltedPhoques::ConnectionId_t aConn) {
            const auto opcodes = OutboxOpcodesFor(aConn);
            return !opcodes.empty() && opcodes.back() == static_cast<uint32_t>(kNotifyResyncComplete) &&
                   std::count(opcodes.begin(), opcodes.end(), static_cast<uint32_t>(kNotifyResyncComplete)) == 1;
        };

        // 1. The observer saw movement for an entity it never spawned (spawn dropped during its load).
        Check(requestFrom(pObserver, 7u, Scope::kEntity, npcServerId, Reason::kUnknownEntityInMovement),
              "RequestResync(entity) round-trips over the wire");
        {
            const auto spawns = DecodeOutboxMessagesFor<CharacterSpawnRequest>(kObserverConn);
            const auto complete = onlyComplete(kObserverConn);
            Check(spawns.size() == 1 && spawns[0].ServerId == npcServerId && spawns[0].BaseId == kBaseId,
                  "entity resync resends exactly one authoritative CharacterSpawnRequest");
            Check(complete.RequestId == 7u && complete.RequestScope == Scope::kEntity &&
                      complete.ServerId == npcServerId && complete.Result == Outcome::kApplied &&
                      complete.SnapshotsSent == 1u,
                  "NotifyResyncComplete echoes the request and reports kApplied with one snapshot");
            Check(terminatorIsLast(kObserverConn), "NotifyResyncComplete is sent once and after every snapshot");
            Check(DecodeOutboxMessagesFor<CharacterSpawnRequest>(kOwnerConn).empty() &&
                      DecodeOutboxMessagesFor<NotifyResyncComplete>(kOwnerConn).empty(),
                  "an entity resync is answered to the requester only");
        }

        // 2. The owner asking for its own NPC cannot be fixed by a resend: no spawn, explicit outcome.
        Check(requestFrom(pOwner, 8u, Scope::kEntity, npcServerId, Reason::kUnknownEntityInMovement),
              "owner RequestResync round-trips over the wire");
        Check(DecodeOutboxMessagesFor<CharacterSpawnRequest>(kOwnerConn).empty() &&
                  onlyComplete(kOwnerConn).Result == Outcome::kOwnedByRequester,
              "a request for an entity the requester owns answers kOwnedByRequester without spawning a duplicate");

        // 3. An id the server no longer knows: the client is told to remove it.
        constexpr uint32_t kGoneId = 0x00FFFF01u;
        Check(requestFrom(pObserver, 9u, Scope::kEntity, kGoneId, Reason::kUnknownEntityInMovement),
              "RequestResync for a vanished entity round-trips");
        {
            const auto removals = DecodeOutboxMessagesFor<NotifyRemoveCharacter>(kObserverConn);
            Check(removals.size() == 1 && removals[0].ServerId == kGoneId &&
                      onlyComplete(kObserverConn).Result == Outcome::kEntityGone && terminatorIsLast(kObserverConn),
                  "a vanished entity answers NotifyRemoveCharacter then kEntityGone");
        }

        // 4. Malformed requests are refused explicitly, never guessed.
        Check(requestFrom(pObserver, 10u, Scope::kEntity, Session::Resync::kNoServerId, Reason::kManual),
              "entity RequestResync without an id round-trips");
        Check(onlyComplete(kObserverConn).Result == Outcome::kInvalid && OutboxOpcodesFor(kObserverConn).size() == 1,
              "entity scope without a server id answers kInvalid and nothing else");
        Check(requestFrom(pObserver, 11u, static_cast<Scope>(0x7Fu), npcServerId, Reason::kManual),
              "RequestResync with an unknown scope byte round-trips");
        Check(onlyComplete(kObserverConn).Result == Outcome::kInvalid && OutboxOpcodesFor(kObserverConn).size() == 1,
              "an unknown scope byte answers kInvalid and nothing else");

        // 5. Whole-session rebuild resends the NPC, and a second one inside the interval is throttled.
        Check(requestFrom(pObserver, 12u, Scope::kAllEntities, Session::Resync::kNoServerId, Reason::kWorldPacketsDiscarded),
              "RequestResync(all entities) round-trips");
        {
            const auto spawns = DecodeOutboxMessagesFor<CharacterSpawnRequest>(kObserverConn);
            const auto complete = onlyComplete(kObserverConn);
            const bool npcResent = std::any_of(spawns.begin(), spawns.end(),
                                               [&](const CharacterSpawnRequest& s) { return s.ServerId == npcServerId; });
            Check(npcResent && complete.Result == Outcome::kApplied && complete.SnapshotsSent >= 1u &&
                      terminatorIsLast(kObserverConn),
                  "whole-session resync resends the in-range NPC and terminates with kApplied");
        }
        Check(requestFrom(pObserver, 13u, Scope::kAllEntities, Session::Resync::kNoServerId, Reason::kWorldPacketsDiscarded),
              "a second whole-session request round-trips");
        Check(onlyComplete(kObserverConn).Result == Outcome::kThrottled &&
                  DecodeOutboxMessagesFor<CharacterSpawnRequest>(kObserverConn).empty(),
              "a whole-session rebuild inside the server interval is throttled and costs nothing");

        // 6. String table.
        Check(requestFrom(pObserver, 14u, Scope::kStringCache, Session::Resync::kNoServerId, Reason::kStringCacheRejected),
              "RequestResync(string cache) round-trips");
        Check(onlyComplete(kObserverConn).Result == Outcome::kApplied && terminatorIsLast(kObserverConn),
              "string table resync terminates with kApplied");

        // 7. A client looping on single-entity requests is bounded by the burst budget.
        {
            uint32_t applied = 0;
            uint32_t throttled = 0;
            for (uint32_t i = 0; i < 64; ++i)
            {
                requestFrom(pObserver, 100u + i, Scope::kEntity, npcServerId, Reason::kUnknownEntityInMovement);
                const auto result = onlyComplete(kObserverConn).Result;
                applied += result == Outcome::kApplied ? 1u : 0u;
                throttled += result == Outcome::kThrottled ? 1u : 0u;
            }
            // One token was spent by scene 1, one by scene 3; refill is 100 ms, the loop takes far less.
            Check(applied <= Session::Resync::ServerPolicy{}.EntityBurst && throttled >= 64u - Session::Resync::ServerPolicy{}.EntityBurst &&
                      applied + throttled == 64u,
                  "a request storm is bounded by the per-player entity budget and every request is still answered");
        }

        // 8. The operator switch: answered, never served.
        bEnableSelfHealingResync = false;
        Check(requestFrom(pOwner, 200u, Scope::kStringCache, Session::Resync::kNoServerId, Reason::kManual), "RequestResync with resync disabled round-trips");
        Check(onlyComplete(kOwnerConn).Result == Outcome::kDisabled && OutboxOpcodesFor(kOwnerConn).size() == 1,
              "with Gameplay:bEnableSelfHealingResync off the server answers kDisabled and resends nothing");
        Check(!Session::IsEnabledIn(GetDisabledCoopFeatureMask(), Session::Feature::kSelfHealingResync),
              "the disabled switch is folded into the feature mask clients receive");
        bEnableSelfHealingResync = true;

        // 9. A player who leaves takes their budget with them; a reconnect starts clean.
        world.GetDispatcher().trigger(PlayerLeaveEvent{pObserver});
        Check(requestFrom(pOwner, 201u, Scope::kAllEntities, Session::Resync::kNoServerId, Reason::kManual), "whole-session request from the remaining peer");
        Check(onlyComplete(kOwnerConn).Result == Outcome::kApplied, "another player's budget is independent");

        std::filesystem::remove(std::filesystem::current_path() / "Data" / "rfab_checkpoints.bin", removeEc);
    }

    // =========================================================================
    // Step 4: server-owned dropped items (common/Session/WorldItems.h, WorldItemService)
    // =========================================================================
    std::printf("v4.1.0 server-owned dropped items\n");
    {
        GameServer server{registry};
        World& world = server.GetWorld();
        auto& worldItems = world.ctx().at<WorldItemService>();

        constexpr TiltedPhoques::ConnectionId_t kAConn = 301;
        constexpr TiltedPhoques::ConnectionId_t kBConn = 302;
        constexpr TiltedPhoques::ConnectionId_t kCConn = 303;
        const GameId kCell{0u, 0x0009ABCDu};
        const GameId kSword{2u, 0x00012EB7u};
        const GameId kArrows{2u, 0x0001397Du};
        const GameId kGold{0u, 0x0000000Fu};

        Player* pA = world.GetPlayerManager().Create(kAConn);
        Player* pB = world.GetPlayerManager().Create(kBConn);
        Check(pA && pB, "two players connect for the dropped-item scene");

        TimeModel t0{};
        t0.Time = 12.0f;
        t0.Day = 5;
        t0.Month = 3;
        t0.Year = 201;
        Harness::ClearOutbox();
        world.GetDispatcher().trigger(PlayerJoinEvent{pA, {}, {}, DateTime{t0}});
        world.GetDispatcher().trigger(PlayerJoinEvent{pB, {}, {}, DateTime{t0}});
        pA->SetCellComponent(CellIdComponent{kCell});
        pB->SetCellComponent(CellIdComponent{kCell});
        Check(DecodeOutboxMessagesFor<NotifyWorldItemSnapshot>(kAConn).size() == 1,
              "a joining player always gets the dropped-item snapshot (empty here)");

        // Each player simulates one actor with an inventory.
        const auto makeActor = [&](Player* apOwner, uint32_t aFormId) {
            const entt::entity entity = world.create();
            world.emplace<FormIdComponent>(entity, aFormId, 0u);
            world.emplace<CharacterComponent>(entity);
            world.emplace<OwnerComponent>(entity, apOwner);
            world.emplace<CellIdComponent>(entity, kCell);
            world.emplace<InventoryComponent>(entity);
            return entity;
        };
        const entt::entity actorA = makeActor(pA, 0x00011111u);
        const entt::entity actorB = makeActor(pB, 0x00022222u);
        const uint32_t actorAId = World::ToInteger(actorA);
        const uint32_t actorBId = World::ToInteger(actorB);

        const auto countOf = [&](entt::entity aActor, const GameId& acItem) {
            GameId lookup = acItem;
            return world.get<InventoryComponent>(aActor).Content.GetEntryCountById(lookup);
        };
        const auto give = [&](entt::entity aActor, const GameId& acItem, int32_t aCount) {
            Inventory::Entry entry{};
            entry.BaseId = acItem;
            entry.Count = aCount;
            world.get<InventoryComponent>(aActor).Content.AddOrRemoveEntry(entry);
        };

        // A non-owner can pickpocket/loot an owned NPC only from the same loaded cell range.
        // Even if the engine marks the delta as local-only, observers must receive the mutation.
        const GameId kPickpocketGold{0u, 0x0000000Fu};
        const entt::entity ownedNpc = makeActor(pA, 0x00033333u);
        give(ownedNpc, kPickpocketGold, 3);
        Inventory::Entry stolenGold{};
        stolenGold.BaseId = kPickpocketGold;
        stolenGold.Count = -1;
        pB->SetCellComponent(CellIdComponent{GameId{0u, 0x0009ABCEu}});
        Harness::ClearOutbox();
        Check(!world.GetInventoryService().ApplyInventoryDelta(pB, World::ToInteger(ownedNpc), stolenGold, false, false, false),
              "out-of-range non-owner inventory interaction on an owned NPC is rejected");
        Check(countOf(ownedNpc, kPickpocketGold) == 3, "rejected remote NPC interaction leaves authoritative inventory unchanged");
        pB->SetCellComponent(CellIdComponent{kCell});
        Harness::ClearOutbox();
        Check(world.GetInventoryService().ApplyInventoryDelta(pB, World::ToInteger(ownedNpc), stolenGold, false, false, false),
              "in-range pickpocket delta from a non-owner is accepted");
        Check(countOf(ownedNpc, kPickpocketGold) == 2, "accepted pickpocket delta updates authoritative inventory");
        Check(!DecodeOutboxMessagesFor<NotifyInventoryChanges>(kAConn).empty(),
              "remote NPC inventory delta is broadcast despite UpdateClients=false");
        pB->SetCellComponent(CellIdComponent{kCell});
        const auto drop = [&](Player* apFrom, uint32_t aActorId, uint32_t aCookie, const GameId& acItem, int32_t aCount) {
            Harness::ClearOutbox();
            RequestWorldItemDrop request{};
            request.Cookie = aCookie;
            request.ActorServerId = aActorId;
            request.Item.BaseId = acItem;
            request.Item.Count = aCount;
            request.CellId = kCell;
            request.Position = {100.0f, 200.0f, 300.0f};
            return DeliverWire(world, request, apFrom);
        };
        const auto pickup = [&](Player* apFrom, uint32_t aActorId, uint32_t aItemId, const GameId& acItem, int32_t aCount, bool aClear = true) {
            if (aClear)
                Harness::ClearOutbox();
            RequestWorldItemPickup request{};
            request.ItemId = aItemId;
            request.PickerServerId = aActorId;
            request.Item.BaseId = acItem;
            request.Item.Count = aCount;
            return DeliverWire(world, request, apFrom);
        };
        const auto inventoryNotes = [](TiltedPhoques::ConnectionId_t aConn) { return DecodeOutboxMessagesFor<NotifyInventoryChanges>(aConn); };

        // 1. A drop moves the stack out of the inventory into the registry; everybody, the dropper
        //    included, gets the spawn with the dropper's cookie. Nobody gets a legacy Drop replay.
        give(actorA, kSword, 5);
        Check(drop(pA, actorAId, 77u, kSword, 5), "RequestWorldItemDrop round-trips over the wire");
        uint32_t swordId = WorldItems::kNoItem;
        {
            const auto spawnsA = DecodeOutboxMessagesFor<NotifyWorldItemSpawn>(kAConn);
            const auto spawnsB = DecodeOutboxMessagesFor<NotifyWorldItemSpawn>(kBConn);
            Check(spawnsA.size() == 1 && spawnsB.size() == 1, "the spawn goes to every player, the dropper included");
            if (!spawnsB.empty())
            {
                swordId = spawnsB[0].State.Id;
                Check(swordId != WorldItems::kNoItem && spawnsB[0].State.Item.Count == 5 && spawnsB[0].State.DropperCookie == 77u &&
                          spawnsB[0].State.DropperPlayerId == pA->GetId() && spawnsB[0].Revision == worldItems.GetRegistry().GetRevision(),
                      "the spawn carries the id, the stack, the dropper's cookie and the registry revision");
            }
            Check(countOf(actorA, kSword) == 0, "the dropped stack left the dropper's inventory on the server");
            const auto notesB = inventoryNotes(kBConn);
            Check(std::none_of(notesB.begin(), notesB.end(), [](const NotifyInventoryChanges& n) { return n.Drop; }),
                  "a registered drop is not also replayed the legacy way");
        }

        // 2. Two players race for it: the first claim processed wins, the loser is rolled back.
        Check(pickup(pB, actorBId, swordId, kSword, 5), "B's claim round-trips");
        Check(pickup(pA, actorAId, swordId, kSword, 5, /*clear*/ false), "A's late claim round-trips");
        {
            Check(countOf(actorB, kSword) == 5 && countOf(actorA, kSword) == 0, "only the first claimer gets the item");
            const auto despawnsB = DecodeOutboxMessagesFor<NotifyWorldItemDespawn>(kBConn);
            const auto despawnsA = DecodeOutboxMessagesFor<NotifyWorldItemDespawn>(kAConn);
            const bool bSawPickedUp = std::any_of(despawnsB.begin(), despawnsB.end(), [&](const NotifyWorldItemDespawn& d) {
                return d.ItemId == swordId && d.Reason == WorldItems::DespawnReason::kPickedUp && d.PickerPlayerId == pB->GetId();
            });
            const bool aSawRejected = std::any_of(despawnsA.begin(), despawnsA.end(), [&](const NotifyWorldItemDespawn& d) {
                return d.ItemId == swordId && d.Reason == WorldItems::DespawnReason::kClaimRejected;
            });
            const bool bSawRejected = std::any_of(despawnsB.begin(), despawnsB.end(), [&](const NotifyWorldItemDespawn& d) {
                return d.Reason == WorldItems::DespawnReason::kClaimRejected;
            });
            Check(bSawPickedUp && aSawRejected && !bSawRejected, "everybody sees the pickup; only the loser hears its claim was rejected");
            const auto notesA = inventoryNotes(kAConn);
            const bool rolledBack = std::any_of(notesA.begin(), notesA.end(), [&](const NotifyInventoryChanges& n) {
                return n.ServerId == actorAId && n.Item.BaseId == kSword && n.Item.Count == -5 && !n.Drop;
            });
            Check(rolledBack, "the loser's optimistic pickup is taken back from its own actor");
            Check(worldItems.GetRegistry().Size() == 0, "a whole-stack pickup removes the item");
        }

        // 3. A partial pickup leaves the remainder, announced as an upsert.
        give(actorA, kArrows, 10);
        Check(drop(pA, actorAId, 78u, kArrows, 10), "a stack of ten is dropped");
        const auto arrowsSpawn = DecodeOutboxMessagesFor<NotifyWorldItemSpawn>(kBConn);
        const uint32_t arrowsId = arrowsSpawn.empty() ? WorldItems::kNoItem : arrowsSpawn[0].State.Id;
        Check(pickup(pB, actorBId, arrowsId, kArrows, 4), "a partial pickup round-trips");
        {
            const auto upserts = DecodeOutboxMessagesFor<NotifyWorldItemSpawn>(kAConn);
            Check(countOf(actorB, kArrows) == 4 && upserts.size() == 1 && upserts[0].State.Id == arrowsId && upserts[0].State.Item.Count == 6,
                  "the picker gets four and everybody sees the six that remain");
        }

        // 4. A claim whose item does not match the registry is undone and answered with the truth.
        Check(pickup(pB, actorBId, arrowsId, kSword, 6), "a claim naming the wrong base form round-trips");
        {
            const auto notesB = inventoryNotes(kBConn);
            const bool undone = std::any_of(notesB.begin(), notesB.end(), [&](const NotifyInventoryChanges& n) {
                return n.ServerId == actorBId && n.Item.BaseId == kSword && n.Item.Count == -6;
            });
            Check(undone && DecodeOutboxMessagesFor<NotifyWorldItemSnapshot>(kBConn).size() == 1 &&
                      worldItems.GetRegistry().Find(arrowsId) && worldItems.GetRegistry().Find(arrowsId)->Count == 6,
                  "a mismatched claim takes nothing, is rolled back and gets the snapshot");
        }

        // 5. Nobody drops for an actor they do not control.
        give(actorB, kGold, 100);
        const uint32_t revisionBefore = worldItems.GetRegistry().GetRevision();
        Check(drop(pA, actorBId, 79u, kGold, 100), "a drop for somebody else's actor round-trips");
        Check(countOf(actorB, kGold) == 100 && worldItems.GetRegistry().GetRevision() == revisionBefore &&
                  DecodeOutboxMessagesFor<NotifyWorldItemSpawn>(kBConn).empty(),
              "a drop for an actor the sender does not control changes nothing");

        // 6. A resync answers the whole registry, then the terminator.
        {
            Harness::ClearOutbox();
            RequestResync request{};
            request.RequestId = 500u;
            request.RequestScope = Session::Resync::Scope::kWorldItems;
            request.ServerId = Session::Resync::kNoServerId;
            request.GapReason = Session::Resync::Reason::kWorldItemRevisionGap;
            Check(DeliverWire(world, request, pB), "RequestResync(world items) round-trips");
            const auto snapshots = DecodeOutboxMessagesFor<NotifyWorldItemSnapshot>(kBConn);
            const auto completes = DecodeOutboxMessagesFor<NotifyResyncComplete>(kBConn);
            const auto opcodes = OutboxOpcodesFor(kBConn);
            Check(snapshots.size() == 1 && snapshots[0].Items.size() == 1 && snapshots[0].Revision == worldItems.GetRegistry().GetRevision() &&
                      completes.size() == 1 && completes[0].Result == Session::Resync::Outcome::kApplied && !opcodes.empty() &&
                      opcodes.back() == static_cast<uint32_t>(kNotifyResyncComplete),
                  "a world-item resync sends the snapshot, then kApplied");
        }

        // 7. Switched off live: new drops take the legacy relay, existing items are still arbitrated.
        bEnableServerOwnedWorldItems = false;
        Check(!Session::IsEnabledIn(GetDisabledCoopFeatureMask(), Session::Feature::kServerOwnedWorldItems),
              "the switch is folded into the feature mask clients receive");
        give(actorA, kGold, 50);
        Check(drop(pA, actorAId, 80u, kGold, 50), "a drop with the feature off round-trips");
        {
            const auto notesB = inventoryNotes(kBConn);
            const bool legacy = std::any_of(notesB.begin(), notesB.end(), [&](const NotifyInventoryChanges& n) {
                return n.ServerId == actorAId && n.Item.BaseId == kGold && n.Item.Count == -50 && n.Drop;
            });
            Check(legacy && countOf(actorA, kGold) == 0 && DecodeOutboxMessagesFor<NotifyWorldItemSpawn>(kBConn).empty() &&
                      worldItems.GetRegistry().Size() == 1,
                  "with the feature off a drop is the legacy replay and is not registered");
        }
        Check(pickup(pA, actorAId, arrowsId, kArrows, 6), "a claim on an existing item with the feature off round-trips");
        Check(countOf(actorA, kArrows) == 6 && worldItems.GetRegistry().Size() == 0, "an item that exists is still arbitrated");
        bEnableServerOwnedWorldItems = true;

        // 8. Bounds: the per-player drop budget refuses a flood; refused drops fall back to the legacy
        //    relay instead of losing the item.
        {
            give(actorA, kGold, 1000);
            uint32_t registered = 0;
            uint32_t legacy = 0;
            for (uint32_t i = 0; i < 64; ++i)
            {
                drop(pA, actorAId, 1000u + i, kGold, 1);
                registered += DecodeOutboxMessagesFor<NotifyWorldItemSpawn>(kBConn).size() == 1 ? 1u : 0u;
                const auto notesB = inventoryNotes(kBConn);
                legacy += std::any_of(notesB.begin(), notesB.end(), [](const NotifyInventoryChanges& n) { return n.Drop; }) ? 1u : 0u;
            }
            Check(registered <= worldItems.GetRegistry().GetPolicy().DropBurst && registered + legacy == 64u &&
                      countOf(actorA, kGold) == 1000 - 64,
                  "a drop flood is bounded by the per-player budget and every drop still leaves the inventory");
        }

        // 9. A late joiner sees what is on the ground; a checkpoint restore is broadcast.
        Player* pC = world.GetPlayerManager().Create(kCConn);
        Harness::ClearOutbox();
        world.GetDispatcher().trigger(PlayerJoinEvent{pC, {}, {}, DateTime{t0}});
        {
            const auto snapshots = DecodeOutboxMessagesFor<NotifyWorldItemSnapshot>(kCConn);
            Check(snapshots.size() == 1 && snapshots[0].Items.size() == worldItems.GetRegistry().Size() && !snapshots[0].Items.empty(),
                  "a late joiner receives every dropped item");
        }
        const auto saved = worldItems.CaptureCheckpoint();
        Harness::ClearOutbox();
        Check(worldItems.ClearAll() == saved.size() && worldItems.GetRegistry().Size() == 0, "ClearAll empties the registry");
        Check(DecodeOutboxMessagesFor<NotifyWorldItemSnapshot>(kBConn).size() == 1, "ClearAll tells every player");
        const uint32_t revisionAfterClear = worldItems.GetRegistry().GetRevision();
        Check(worldItems.RestoreCheckpoint(saved) == saved.size(), "a checkpoint restores every item");
        Check(WorldItems::IsNewerRevision(worldItems.GetRegistry().GetRevision(), revisionAfterClear),
              "a restore moves the revision forward, so no client mistakes it for an old delta");

        // 10. A player who leaves keeps its items in the world.
        const size_t before = worldItems.GetRegistry().Size();
        world.GetDispatcher().trigger(PlayerLeaveEvent{pA});
        Check(worldItems.GetRegistry().Size() == before, "items dropped by a player who left stay in the world");

        std::filesystem::remove(std::filesystem::current_path() / "Data" / "rfab_checkpoints.bin", removeEc);
    }

    // Scene 7 (v4.1.1 Phase 1 stabilization): Server::World entity-0 sentinel + kAvInfamy == 61 + ghost boss XP.
    std::printf("v4.1.1 Phase 1 stabilization invariants\n");
    {
        GameServer stabServer{registry};
        World& world = stabServer.GetWorld();
        constexpr ConnectionId_t kStabConn = 9901;

        Check(!world.valid(static_cast<entt::entity>(0)), "entity 0 sentinel is never reported valid()");
        world.destroy(static_cast<entt::entity>(0));
        Check(!world.valid(static_cast<entt::entity>(0)), "destroying entity 0 sentinel is a safe no-op");

        const entt::entity firstCreated = world.create();
        Check(World::ToInteger(firstCreated) != 0u, "first live entity allocated on fresh Server::World has non-zero ServerId");
        world.destroy(firstCreated);

        Player* pPlayer = world.GetPlayerManager().Create(kStabConn);
        const CellIdComponent kStabCell{GameId{0u, 0x000165A8u}, GameId{0u, 0x0000003Cu}, GridCellCoords{0, 0}};
        pPlayer->SetCellComponent(kStabCell);
        const entt::entity playerEntity = world.create();
        pPlayer->SetCharacter(playerEntity);
        const uint32_t playerServerId = World::ToInteger(playerEntity);
        Check(playerServerId != 0u, "assigned player character never gets ServerId == 0");

        world.emplace<OwnerComponent>(playerEntity, pPlayer);
        world.emplace<CellIdComponent>(playerEntity, kStabCell);
        world.emplace<ActorValuesComponent>(playerEntity);

        // Verify kAvInfamy (61) is stripped on server while kAvVoicePoints (37), kAvDamageResist (39),
        // and kAvAbsorbChance (83) are accepted cleanly!
        {
            RequestActorValueChanges avReq{};
            avReq.Id = playerServerId;
            avReq.Values[61u] = 500.0f; // kAvInfamy -> must be dropped (RFAB_XP_Alias local sentinel)
            avReq.Values[37u] = 100.0f; // kAvVoicePoints -> must be accepted (was wrongly dropped as 37)
            avReq.Values[39u] = 420.0f; // kAvDamageResist
            avReq.Values[83u] = 25.0f;  // kAvAbsorbChance
            Check(DeliverWire(world, avReq, pPlayer), "P1.2 ActorValueChanges with Infamy/VoicePoints/Armor/Absorb round-trips");
            const auto& stored = world.get<ActorValuesComponent>(playerEntity).CurrentActorValues;
            Check(stored.ActorValuesList.count(61u) == 0,
                  "kAvInfamy (61) is filtered out on server so RFAB_XP_Alias sentinel never leaks");
            Check(stored.ActorValuesList.count(37u) == 1 && stored.ActorValuesList.at(37u) == 100.0f,
                  "kAvVoicePoints (37) is no longer mistakenly blocked as kAvInfamy");
            Check(stored.ActorValuesList.count(39u) == 1 && stored.ActorValuesList.at(39u) == 420.0f,
                  "kAvDamageResist (39) persists on server ActorValuesComponent");
            Check(stored.ActorValuesList.count(83u) == 1 && stored.ActorValuesList.at(83u) == 25.0f,
                  "kAvAbsorbChance (83) persists on server ActorValuesComponent");
        }

        // Verify boss_ghost on valid boss entity (and rejection of entity 0 sentinel).
        {
            const entt::entity bossEntity = world.create();
            const uint32_t bossServerId = World::ToInteger(bossEntity);
            Check(bossServerId != 0u, "boss entity never gets ServerId == 0");
            world.emplace<OwnerComponent>(bossEntity, pPlayer);
            world.emplace<CellIdComponent>(bossEntity, kStabCell);

            SyncRfabProgressionEvent bossGhostZero{};
            bossGhostZero.Kind = SyncRfabProgressionEvent::EventKind::BossEncounterSync;
            bossGhostZero.EpochId = world.GetSaveSnapshotService().GetCurrentWorldEpoch();
            bossGhostZero.SourceKey = "boss_ghost";
            bossGhostZero.XpDelta = 0;
            bossGhostZero.GlobalValue = 1.0f;
            Check(DeliverWire(world, bossGhostZero, pPlayer), "P1.1 boss_ghost with ServerId == 0 is handled safely");

            SyncRfabProgressionEvent bossGhostLive{};
            bossGhostLive.Kind = SyncRfabProgressionEvent::EventKind::BossEncounterSync;
            bossGhostLive.EpochId = world.GetSaveSnapshotService().GetCurrentWorldEpoch();
            bossGhostLive.SourceKey = "boss_ghost";
            bossGhostLive.XpDelta = static_cast<int32_t>(bossServerId);
            bossGhostLive.GlobalValue = 1.0f;
            Check(DeliverWire(world, bossGhostLive, pPlayer), "P1.1 boss_ghost with valid non-zero bossServerId delivers cleanly");
            world.destroy(bossEntity);
        }
    }

    // Scene 8: Phase 3 RFAB Deep Adaptation end-to-end server verification
    // (Boss Lease Lock, Shared Dragon Souls, Word Walls, Difficulty/Cursed Handshake,
    //  Personal Blessing/CrimeGold Isolation, and Mount Hijack / Saddlebag Anti-Dupe).
    {
        std::printf("[scene 8] Phase 3 RFAB Deep Adaptation (Boss Lock, Souls, WordWalls, Difficulty, Blessings, Mounts)\n");
        Harness::ClearOutbox();
        GameServer server{registry};
        World& world = server.GetWorld();

        constexpr TiltedPhoques::ConnectionId_t kHostConn = 0x8001u;
        constexpr TiltedPhoques::ConnectionId_t kGuestConn = 0x8002u;
        Player* pHost = world.GetPlayerManager().Create(kHostConn);
        Player* pGuest = world.GetPlayerManager().Create(kGuestConn);
        pHost->SetUsername("HostP3");
        pGuest->SetUsername("GuestP3");

        const CellIdComponent kCell{GameId{0u, 0x000165A8u}, GameId{0u, 0x0000003Cu}, GridCellCoords{0, 0}};
        pHost->SetCellComponent(kCell);
        pGuest->SetCellComponent(kCell);

        const entt::entity hostChar = world.create();
        pHost->SetCharacter(hostChar);
        world.emplace<OwnerComponent>(hostChar, pHost);
        world.emplace<CellIdComponent>(hostChar, kCell);
        auto& hostComp = world.emplace<CharacterComponent>(hostChar);
        hostComp.SetPlayer(true);

        const entt::entity guestChar = world.create();
        pGuest->SetCharacter(guestChar);
        world.emplace<OwnerComponent>(guestChar, pGuest);
        world.emplace<CellIdComponent>(guestChar, kCell);
        auto& guestComp = world.emplace<CharacterComponent>(guestChar);
        guestComp.SetPlayer(true);

        const uint64_t epoch = world.GetSaveSnapshotService().GetCurrentWorldEpoch();

        // 8.1 Boss encounter lock (M7): it is the owner's statement, it lasts as long as the fight,
        //     and nothing about it outlives the owner, the boss or the server process.
        {
            const entt::entity boss = world.create();
            const uint32_t bossId = World::ToInteger(boss);
            world.emplace<OwnerComponent>(boss, pHost);
            world.emplace<CellIdComponent>(boss, kCell);
            world.emplace<CharacterComponent>(boss);
            const auto& ledger = world.GetRfabProgressionService().GetLedger();

            // What the owner's client sends when a boss it simulates enters or leaves combat, and
            // when the boss turns invulnerable for a phase.
            const auto report = [&](Player* apSender, const char* acWhat, bool aOn) {
                SyncRfabProgressionEvent packet{};
                packet.Kind = SyncRfabProgressionEvent::EventKind::BossEncounterSync;
                packet.EpochId = epoch;
                packet.SourceKey = acWhat;
                packet.XpDelta = static_cast<int32_t>(bossId);
                packet.GlobalValue = aOn ? 1.0f : 0.0f;
                return DeliverWire(world, packet, apSender);
            };

            Check(report(pGuest, "boss_lock", true) && !ledger.IsBossEncounterLocked(bossId, pGuest->GetId()) &&
                      !ledger.IsBossEncounterLocked(bossId, pHost->GetId()),
                  "a player who does not simulate the boss cannot lock it");

            Check(report(pHost, "boss_lock", true) && ledger.IsBossEncounterLocked(bossId, pHost->GetId()),
                  "the simulating player locks the boss when it enters combat");

            Harness::ClearOutbox();
            RequestOwnershipClaim steal{};
            steal.ServerId = bossId;
            Check(DeliverWire(world, steal, pGuest), "guest claim on locked boss processed");
            Check(world.get<OwnerComponent>(boss).GetOwner() == pHost,
                  "locked boss stays owned by simulating Host");
            Check(!DecodeOutboxMessagesFor<NotifyRelinquishControl>(kGuestConn).empty(),
                  "guest receives NotifyRelinquishControl rejecting locked boss claim");

            Check(!ledger.IsBossEncounterLocked(bossId, pGuest->GetId()),
                  "the lock is void once another player simulates the boss");

            Check(report(pHost, "boss_lock", false) && !ledger.IsBossEncounterLocked(bossId, pHost->GetId()),
                  "the lock is given back when the boss leaves combat, not only when it dies");

            // An invulnerable phase holds the boss by itself and lets go when it ends. It used to
            // leave a lock behind that only the boss's death removed.
            Check(report(pHost, "boss_ghost", true) && ledger.IsBossEncounterLocked(bossId, pHost->GetId()) &&
                      ledger.IsBossEncounterLocked(bossId, pGuest->GetId()),
                  "an invulnerable phase holds the boss whoever simulates it");
            Check(report(pHost, "boss_ghost", false) && !ledger.IsBossEncounterLocked(bossId, pHost->GetId()),
                  "the end of the invulnerable phase leaves no lock behind");

            // The boss is gone: nothing keyed by its server id stays.
            Check(report(pHost, "boss_ghost", true) && report(pHost, "boss_lock", true) && ledger.GetBossGhostStates().count(bossId) == 1,
                  "a phase and a lock are recorded for the living boss");
            world.destroy(boss);
            Check(ledger.GetBossGhostStates().count(bossId) == 0 && !ledger.IsBossEncounterLocked(bossId, pHost->GetId()),
                  "destroying the boss entity removes its phase and its lock");
        }

        // 8.1b Boss lease (work plan 5.1). The player who simulates a locked boss goes silent - a
        //      menu that pauses his game - while the other player keeps fighting next to him. The
        //      boss window is 15 s. Before 4.1.1 the owner's own packets renewed the lease with the
        //      ordinary 8 s window, so the boss changed hands in the middle of the fight (M22).
        {
            auto& characters = world.GetCharacterService();
            const auto& ledger = world.GetRfabProgressionService().GetLedger();

            const auto makeLockedBoss = [&]() {
                const entt::entity boss = world.create();
                world.emplace<OwnerComponent>(boss, pHost);
                world.emplace<CellIdComponent>(boss, kCell);
                world.emplace<CharacterComponent>(boss);

                SyncRfabProgressionEvent packet{};
                packet.Kind = SyncRfabProgressionEvent::EventKind::BossEncounterSync;
                packet.EpochId = epoch;
                packet.SourceKey = "boss_lock";
                packet.XpDelta = static_cast<int32_t>(World::ToInteger(boss));
                packet.GlobalValue = 1.0f;
                const bool delivered = DeliverWire(world, packet, pHost);
                Check(delivered && ledger.IsBossEncounterLocked(World::ToInteger(boss), pHost->GetId()),
                      "the host locks the boss it simulates");
                return boss;
            };
            const auto ownerOf = [&](entt::entity aBoss) { return world.get<OwnerComponent>(aBoss).GetOwner(); };
            const auto countFor = [](const auto& acMessages, entt::entity aBoss) {
                size_t count = 0;
                for (const auto& message : acMessages)
                    count += message.ServerId == World::ToInteger(aBoss) ? 1u : 0u;
                return count;
            };

            Check(static_cast<bool>(bEnableBossLeaseRenewal), "Gameplay:bEnableBossLeaseRenewal is on by default");

            // Switch on: the owner's last packet is at t0, the guest keeps sending.
            const entt::entity bossOn = makeLockedBoss();
            const uint64_t t0 = 7'000'000'000ull;
            characters.NotePlayerSimulationActivity(pHost, t0);
            characters.NotePlayerSimulationActivity(pGuest, t0 + 9'500ull);
            Harness::ClearOutbox();
            characters.SweepExpiredWorldNpcLeases(t0 + 10'000ull);
            Check(ownerOf(bossOn) == pHost &&
                      countFor(DecodeOutboxMessagesFor<NotifyRelinquishControl>(kHostConn), bossOn) == 0,
                  "a locked boss stays with its owner through 10 s of silence");

            characters.NotePlayerSimulationActivity(pGuest, t0 + 15'500ull);
            Harness::ClearOutbox();
            characters.SweepExpiredWorldNpcLeases(t0 + 16'000ull);
            const auto lapsedRelinquish = DecodeOutboxMessagesFor<NotifyRelinquishControl>(kHostConn);
            const auto lapsedGrant = DecodeOutboxMessagesFor<NotifyOwnershipTransfer>(kGuestConn);
            Check(ownerOf(bossOn) == pGuest && countFor(lapsedRelinquish, bossOn) == 1 && countFor(lapsedGrant, bossOn) == 1,
                  "after 15 s of silence the boss goes to the player who is still fighting, told to both");
            Check(!ledger.IsBossEncounterLocked(World::ToInteger(bossOn), pGuest->GetId()),
                  "the previous owner's lock does not follow the boss to its new owner");

            // Switch off: the code as it was. The same ten seconds are enough to lose the boss.
            bEnableBossLeaseRenewal = false;
            const entt::entity bossOff = makeLockedBoss();
            const uint64_t t1 = t0 + 100'000ull;
            characters.NotePlayerSimulationActivity(pHost, t1);
            characters.NotePlayerSimulationActivity(pGuest, t1 + 9'500ull);
            Harness::ClearOutbox();
            characters.SweepExpiredWorldNpcLeases(t1 + 10'000ull);
            Check(ownerOf(bossOff) == pGuest &&
                      countFor(DecodeOutboxMessagesFor<NotifyRelinquishControl>(kHostConn), bossOff) == 1,
                  "with Gameplay:bEnableBossLeaseRenewal off a locked boss changes hands after 8 s, as it did before 4.1.1");
            bEnableBossLeaseRenewal = true;

            world.destroy(bossOn);
            world.destroy(bossOff);
        }

        // 8.2 Shared dragon souls have their own scene (10). Shared Words of Power were removed: no
        //     client ever sent them (M4).

        // 8.3 RFAB_StartMenu Difficulty & Cursed Handshake mismatch detection
        {
            Harness::ClearOutbox();
            SyncRfabProgressionEvent hostDiff{};
            hostDiff.Kind = SyncRfabProgressionEvent::EventKind::DifficultyHandshakeSync;
            hostDiff.EpochId = epoch;
            hostDiff.XpDelta = RfabCoopProgression::PackDifficultyHandshake(
                RfabCoopProgression::RfabDifficultyTier::kLegend, true); // Legend + Cursed
            hostDiff.GlobalValue = 2.0f;
            Check(DeliverWire(world, hostDiff, pHost), "Host DifficultyHandshakeSync delivered");

            Harness::ClearOutbox();
            SyncRfabProgressionEvent guestDiff{};
            guestDiff.Kind = SyncRfabProgressionEvent::EventKind::DifficultyHandshakeSync;
            guestDiff.EpochId = epoch;
            guestDiff.XpDelta = RfabCoopProgression::PackDifficultyHandshake(
                RfabCoopProgression::RfabDifficultyTier::kDovahkiin, false); // Dovahkiin, not cursed
            guestDiff.GlobalValue = 1.0f;
            Check(DeliverWire(world, guestDiff, pGuest), "Guest DifficultyHandshakeSync delivered");

            const auto warnings = DecodeOutboxMessagesFor<NotifyRfabProgressionEvent>(kGuestConn);
            Check(warnings.size() == 1 && warnings[0].EditorId == "mismatch",
                  "Guest with mismatched RFAB Difficulty/Cursed receives mismatch warning");
        }

        // 8.4 Personal Blessing & CrimeGold Global isolation
        {
            Harness::ClearOutbox();
            SyncRfabProgressionEvent crimeGlobal{};
            crimeGlobal.Kind = SyncRfabProgressionEvent::EventKind::GlobalValueSync;
            crimeGlobal.EpochId = epoch;
            crimeGlobal.EditorId = "RFAB_SaveBlessing";
            crimeGlobal.GlobalValue = 1.0f;
            Check(DeliverWire(world, crimeGlobal, pHost), "personal SaveBlessing global packet processed");
            Check(DecodeOutboxMessagesFor<NotifyRfabProgressionEvent>(kGuestConn).empty(),
                  "personal SaveBlessing/CrimeGold global is NEVER replicated to teammates");
        }

        // 8.5 The mount ride guard has its own scene (11), driven by the packets a client really sends.

        // 8.6 EncounterCoordinator edge-case hardening (duplicate revive anti-reset + last-player disconnect cleanup)
        {
            EncounterCoordinator coord{};
            coord.UpdatePlayerCombatReport(1, true, 1, 5.0f, false, 0.f, 0.f, 0.f, 1000ULL);
            coord.UpdatePlayerCombatReport(2, true, 1, 5.0f, false, 1.f, 0.f, 0.f, 1000ULL);
            coord.UpdatePlayerCombatReport(3, true, 1, 5.0f, false, 1.5f, 0.f, 0.f, 1000ULL);

            Check(coord.SetPlayerDowned(1, true, 0.f, 0.f, 0.f, 1, 1100ULL), "Player 1 downed");
            Check(coord.TryStartAllyRevive(2, 1, 2000ULL), "Ally 2 starts reviving Player 1");
            // Ally 3 pressing E at 4500ms MUST NOT reset Ally 2's 4000ms timer!
            Check(!coord.TryStartAllyRevive(3, 1, 4500ULL), "Second ally cannot reset an active in-progress revive channel");
            const auto tickDone = coord.Tick(6100ULL);
            Check(tickDone.CompletedAllyRevives.size() == 1 && tickDone.CompletedAllyRevives[0].first == 2u,
                  "Ally 2's revive completes on original schedule without timer reset");

            // All players leave -> encounter state fully resets cleanly
            (void)coord.RemovePlayer(1);
            (void)coord.RemovePlayer(2);
            (void)coord.RemovePlayer(3);
            Check(!coord.IsEncounterActiveWithLivingParticipants(),
                  "RemovePlayer clears active encounter state when last participant disconnects");
        }
    }

    // Scene 9: catch-up XP is counted per persistent player identity, never per connection (M1).
    // Every step goes through the real join path and the real XP packet; the amounts are read from
    // what the server actually sent to the joining connection.
    {
        std::printf("[scene 9] late-join catch-up XP: per identity, never per connection\n");
        const auto storePath = SaveSnapshotService::GetDefaultCheckpointStorePath();
        std::filesystem::remove(storePath, removeEc);

        const auto joinAs = [](World& aWorld, TiltedPhoques::ConnectionId_t aConn, const char* acName) {
            Player* pPlayer = aWorld.GetPlayerManager().Create(aConn);
            if (acName != nullptr)
                pPlayer->SetUsername(acName);
            TimeModel joinTime{};
            joinTime.Time = 8.0f;
            joinTime.Day = 1;
            joinTime.Month = 1;
            joinTime.Year = 201;
            Harness::ClearOutbox();
            aWorld.GetDispatcher().trigger(PlayerJoinEvent{pPlayer, {}, {}, DateTime{joinTime}});
            return pPlayer;
        };
        const auto leave = [](World& aWorld, Player* apPlayer) {
            aWorld.GetDispatcher().trigger(PlayerLeaveEvent{apPlayer});
            aWorld.GetPlayerManager().Remove(apPlayer);
        };
        // The packet a client really sends when its RFAB XP wallet grows: the relay to the party.
        // (No client sends a keyed LedgerXpGrant; the first version of this scene used one and
        // proved the accounting on a path nothing feeds.)
        const auto earn = [](World& aWorld, Player* apPlayer, uint32_t aXp) {
            SyncRfabExperienceRequest relay{};
            relay.Experience = aXp;
            return DeliverWire(aWorld, relay, apPlayer);
        };
        // Catch-up XP the server sent to this connection since the outbox was last cleared.
        const auto catchUpSentTo = [](TiltedPhoques::ConnectionId_t aConn) {
            int64_t total = 0;
            for (const auto& message : DecodeOutboxMessagesFor<NotifyRfabProgressionEvent>(aConn))
                if (message.Kind == NotifyRfabProgressionEvent::EventKind::LateJoinCatchUpXp)
                    total += message.XpDelta;
            return total;
        };

        bEnableLateJoinCatchUpXp = true;
        {
            GameServer server{registry};
            World& world = server.GetWorld();
            auto& ledger = world.GetRfabProgressionService().GetLedger();

            Player* pHost = joinAs(world, 0x9001u, "HostS9");
            Check(catchUpSentTo(0x9001u) == 0, "the first player of a new world is owed nothing");
            Player* pGuest = joinAs(world, 0x9002u, "GuestS9");
            Check(catchUpSentTo(0x9002u) == 0, "a player who joins before any XP is earned is owed nothing");

            Harness::ClearOutbox();
            Check(earn(world, pHost, 1000) && ledger.GetTotalSharedXp() == 1000, "the party earns 1000 XP while both are connected");
            {
                const auto relayed = DecodeOutboxMessagesFor<NotifySyncRfabExperience>(0x9002u);
                Check(relayed.size() == 1 && relayed[0].Experience == 1000, "the guest is sent the 1000 XP by the ordinary relay");
            }

            // The defect: Player::GetId() is new on every connection, so this paid 1000 again.
            leave(world, pGuest);
            pGuest = joinAs(world, 0x9003u, "GuestS9");
            Check(catchUpSentTo(0x9003u) == 0, "a reconnect pays nothing to a player who was there when the XP was earned");

            leave(world, pGuest);
            Check(earn(world, pHost, 500) && ledger.GetTotalSharedXp() == 1500, "the host earns 500 XP while the guest is away");
            pGuest = joinAs(world, 0x9004u, "  guests9 ");
            Check(catchUpSentTo(0x9004u) == 500, "the returning guest is paid exactly what was earned while away (name compared without case and padding)");
            leave(world, pGuest);
            pGuest = joinAs(world, 0x9005u, "GuestS9");
            Check(catchUpSentTo(0x9005u) == 0, "and is not paid for it a second time");

            Player* pLate = joinAs(world, 0x9006u, "LateS9");
            Check(catchUpSentTo(0x9006u) == 1500, "a friend who joins the campaign late is paid the party total");
            Harness::ClearOutbox();
            world.GetDispatcher().trigger(PlayerJoinEvent{pLate});
            Check(catchUpSentTo(0x9006u) == 0, "a second join event for the same connection pays nothing");

            joinAs(world, 0x9007u, nullptr);
            Check(catchUpSentTo(0x9007u) == 0, "a connection without a name or Discord id has no persistent identity and is owed nothing");

            // A checkpoint carries the credits together with the total.
            auto& saves = world.GetSaveSnapshotService();
            auto& coordinator = saves.GetCoordinator();
            Check(saves.TriggerCoordinatedSave(true, nullptr), "a checkpoint starts");
            if (const auto pending = coordinator.GetPendingBarrier(); pending.has_value())
            {
                std::vector<Player*> connected;
                world.GetPlayerManager().ForEach([&](Player* apPlayer) { connected.push_back(apPlayer); });
                for (Player* pPlayer : connected)
                {
                    CoordinatedSaveAck ack{};
                    ack.EpochId = pending->EpochId;
                    ack.SlotName = String(pending->SlotName.c_str());
                    ack.Success = true;
                    Deliver(world, ack, pPlayer);
                }
            }
            Check(!coordinator.HasPendingBarrier() && saves.GetSnapshotCount() == 1, "the checkpoint is committed");
            Check(saves.SaveCheckpointsToDisk(storePath), "the checkpoint is written");
        }

        // The other half of the defect: after a restart the tracker was empty, so every player who
        // had been there all along was paid up to the cap on the first join.
        {
            GameServer restarted{registry};
            World& world = restarted.GetWorld();
            const auto& ledger = world.GetRfabProgressionService().GetLedger();
            Check(ledger.GetTotalSharedXp() == 1500, "the restarted server restores the party total");
            Check(ledger.GetXpCredits().GetCreditedXp("name:hosts9") == 1500 && ledger.GetXpCredits().GetCreditedXp("name:guests9") == 1500,
                  "the restarted server restores who already holds the shared XP");

            joinAs(world, 0x9101u, "HostS9");
            Check(catchUpSentTo(0x9101u) == 0, "a server restart pays nothing to a player who was there all along");
            joinAs(world, 0x9102u, "NewAfterRestartS9");
            Check(catchUpSentTo(0x9102u) == 1500, "a new friend joining after the restart is paid the party total once");

            // Switched off, nothing is paid - and switching it on later does not pay for that time.
            bEnableLateJoinCatchUpXp = false;
            Player* pOff = joinAs(world, 0x9103u, "OffS9");
            Check(catchUpSentTo(0x9103u) == 0, "with Gameplay:bEnableLateJoinCatchUpXp off a late joiner is paid nothing");
            leave(world, pOff);
            bEnableLateJoinCatchUpXp = true;
            joinAs(world, 0x9104u, "OffS9");
            Check(catchUpSentTo(0x9104u) == 0, "switching the setting on later does not pay for the time it was off");
        }

        bEnableLateJoinCatchUpXp = false;
        std::filesystem::remove(storePath, removeEc);
    }

    // Scene 10: a dragon's soul is shared with the party members who were not there, once per
    // death (M2). The trigger is the death the server itself accepts; nothing a client says about
    // souls is trusted. Amounts are read from the packets sent to each connection.
    {
        std::printf("[scene 10] shared dragon soul: absent party members only, once per death\n");
        std::filesystem::remove(SaveSnapshotService::GetDefaultCheckpointStorePath(), removeEc);

        GameServer server{registry};
        World& world = server.GetWorld();

        constexpr TiltedPhoques::ConnectionId_t kKillerConn = 0xA001u;
        constexpr TiltedPhoques::ConnectionId_t kNearConn = 0xA002u;
        constexpr TiltedPhoques::ConnectionId_t kFarConn = 0xA003u;
        const GameId kLairCell{0u, 0x000A1001u};
        const GameId kTownCell{0u, 0x000A1002u};
        const GameId kDragonBase{0u, 0x000F811Au};

        Player* pKiller = world.GetPlayerManager().Create(kKillerConn);
        Player* pNear = world.GetPlayerManager().Create(kNearConn);
        Player* pFar = world.GetPlayerManager().Create(kFarConn);
        TimeModel t0{};
        t0.Time = 9.0f;
        t0.Day = 2;
        t0.Month = 2;
        t0.Year = 201;
        world.GetDispatcher().trigger(PlayerJoinEvent{pKiller, {}, {}, DateTime{t0}});
        world.GetDispatcher().trigger(PlayerJoinEvent{pNear, {}, {}, DateTime{t0}});
        world.GetDispatcher().trigger(PlayerJoinEvent{pFar, {}, {}, DateTime{t0}});
        Check(world.GetPartyService().IsPlayerInParty(pNear) && world.GetPartyService().IsPlayerInParty(pFar),
              "the three players are one party");

        pKiller->SetCellComponent(CellIdComponent{kLairCell});
        pNear->SetCellComponent(CellIdComponent{kLairCell});
        pFar->SetCellComponent(CellIdComponent{kTownCell});

        uint32_t nextCookie = 500u;
        const auto spawn = [&](uint32_t aRefId, bool aIsDragon, bool aAlreadyDead) {
            Harness::ClearOutbox();
            AssignCharacterRequest assign{};
            assign.Cookie = nextCookie++;
            assign.ReferenceId = GameId{0u, aRefId};
            assign.FormId = kDragonBase;
            assign.CellId = kLairCell;
            assign.Position = {10.0f, 20.0f, 30.0f};
            assign.CurrentActorData.BaseId = kDragonBase;
            assign.CurrentActorData.InitialActorValues.ActorValuesList[24] = 3000.0f;
            assign.CurrentActorData.InitialActorValues.ActorMaxValuesList[24] = 3000.0f;
            assign.CurrentActorData.IsDead = aAlreadyDead;
            assign.IsDragon = aIsDragon;
            (void)DeliverWire(world, assign, pKiller);
            const auto responses = DecodeOutboxMessagesFor<AssignCharacterResponse>(kKillerConn);
            return responses.size() == 1 ? responses[0].ServerId : 0xFFFFFFFFu;
        };
        const auto reportDeath = [&](uint32_t aServerId, Player* apBy, bool aIsDead) {
            Harness::ClearOutbox();
            RequestDeathStateChange death{};
            death.Id = aServerId;
            death.IsDead = aIsDead;
            return DeliverWire(world, death, apBy);
        };
        // Souls the server granted to this connection since the outbox was last cleared.
        const auto soulsSentTo = [](TiltedPhoques::ConnectionId_t aConn) {
            int64_t total = 0;
            for (const auto& message : DecodeOutboxMessagesFor<NotifyRfabProgressionEvent>(aConn))
                if (message.Kind == NotifyRfabProgressionEvent::EventKind::DragonSoulSync)
                    total += message.XpDelta;
            return total;
        };
        const auto nobodyWasSentASoul = [&] {
            return soulsSentTo(kKillerConn) == 0 && soulsSentTo(kNearConn) == 0 && soulsSentTo(kFarConn) == 0;
        };

        const uint32_t firstDragon = spawn(0x000A2001u, true, false);
        Check(firstDragon != 0xFFFFFFFFu, "the killer's client registers a living dragon");
        Check(reportDeath(firstDragon, pKiller, true) && nobodyWasSentASoul(),
              "with Gameplay:bEnableSharedDragonSouls off (the default) a dragon's death sends no souls");

        bEnableSharedDragonSouls = true;

        Check(reportDeath(firstDragon, pNear, true) && nobodyWasSentASoul(),
              "a second death packet for a dragon that is already dead sends no souls");

        // What a 4.1.0 client sends for every dead dragon it simulates, corpses included.
        {
            Harness::ClearOutbox();
            SyncRfabProgressionEvent clientReport{};
            clientReport.Kind = SyncRfabProgressionEvent::EventKind::DragonSoulSync;
            clientReport.EpochId = world.GetSaveSnapshotService().GetCurrentWorldEpoch();
            clientReport.BookId = GameId{0u, 0x000A2001u};
            clientReport.XpDelta = static_cast<int32_t>(firstDragon);
            Check(DeliverWire(world, clientReport, pKiller) && nobodyWasSentASoul(),
                  "a client's own report about a dead dragon sends no souls");
            clientReport.SourceKey = "dragon:made-up-key";
            Check(DeliverWire(world, clientReport, pNear) && nobodyWasSentASoul(),
                  "a client cannot invent a dragon by sending a key of its own");
        }

        const uint32_t secondDragon = spawn(0x000A2002u, true, false);
        Check(reportDeath(secondDragon, pKiller, true), "the second dragon dies");
        {
            const auto farGrants = DecodeOutboxMessagesFor<NotifyRfabProgressionEvent>(kFarConn);
            int64_t grantCount = 0;
            for (const auto& message : farGrants)
                if (message.Kind == NotifyRfabProgressionEvent::EventKind::DragonSoulSync)
                    ++grantCount;
            Check(grantCount == 1 && soulsSentTo(kFarConn) == 1, "the party member in another cell is sent exactly one soul, as +1");
        }
        Check(soulsSentTo(kKillerConn) == 0 && soulsSentTo(kNearConn) == 0,
              "the killer and the member standing at the dragon are sent nothing: they absorb the soul the native way");

        const uint32_t corpse = spawn(0x000A2003u, true, true);
        Check(corpse != 0xFFFFFFFFu && reportDeath(corpse, pKiller, true) && nobodyWasSentASoul(),
              "a dragon that is registered already dead (a corpse someone walked up to) sends no souls");

        const uint32_t ordinaryEnemy = spawn(0x000A2004u, false, false);
        Check(ordinaryEnemy != 0xFFFFFFFFu && reportDeath(ordinaryEnemy, pKiller, true) && nobodyWasSentASoul(),
              "an ordinary enemy's death sends no souls");

        // The owner brings the dragon back (a rollback does this) and it dies again: a new death.
        Check(reportDeath(secondDragon, pKiller, false) && nobodyWasSentASoul(), "resurrecting a dragon sends no souls");
        Check(reportDeath(secondDragon, pKiller, true) && soulsSentTo(kFarConn) == 1,
              "a dragon that came back to life and died again counts as one more death");

        // Everyone at the dragon: nothing to share.
        pFar->SetCellComponent(CellIdComponent{kLairCell});
        const uint32_t thirdDragon = spawn(0x000A2005u, true, false);
        Check(reportDeath(thirdDragon, pKiller, true) && nobodyWasSentASoul(),
              "when the whole party is at the dragon nobody is sent a soul");

        bEnableSharedDragonSouls = false;
        std::filesystem::remove(SaveSnapshotService::GetDefaultCheckpointStorePath(), removeEc);
    }

    // Scene 11: the mount ride guard (M3). Clients report the start of a ride and never its end,
    // so the scene ends rides the three ways they really end: the rider's dismount animation, the
    // rider walking away, the rider leaving the server. Every step is a packet a client sends.
    {
        std::printf("[scene 11] mount ride guard: a ride ends without a dismount packet\n");
        std::filesystem::remove(SaveSnapshotService::GetDefaultCheckpointStorePath(), removeEc);

        GameServer server{registry};
        World& world = server.GetWorld();

        constexpr TiltedPhoques::ConnectionId_t kHostConn = 0xB001u;
        constexpr TiltedPhoques::ConnectionId_t kGuestConn = 0xB002u;
        const GameId kStableCell{0u, 0x000B1001u};
        const GameId kHorseBase{0u, 0x000109E3u};
        const GameId kApple{0u, 0x00064B2Eu};

        Player* pHost = world.GetPlayerManager().Create(kHostConn);
        Player* pGuest = world.GetPlayerManager().Create(kGuestConn);
        TimeModel t0{};
        t0.Time = 10.0f;
        t0.Day = 3;
        t0.Month = 3;
        t0.Year = 201;
        world.GetDispatcher().trigger(PlayerJoinEvent{pHost, {}, {}, DateTime{t0}});
        world.GetDispatcher().trigger(PlayerJoinEvent{pGuest, {}, {}, DateTime{t0}});
        pHost->SetCellComponent(CellIdComponent{kStableCell});
        pGuest->SetCellComponent(CellIdComponent{kStableCell});

        uint32_t nextCookie = 700u;
        const GameId kPlayerRef{0u, 0x14u};
        const GameId kGuardBase{0u, 0x000B4001u};
        const auto assign = [&](Player* apOwner, TiltedPhoques::ConnectionId_t aConn, const GameId& acRefId, bool aIsMount, const glm::vec3& acPosition) {
            Harness::ClearOutbox();
            AssignCharacterRequest request{};
            request.Cookie = nextCookie++;
            request.ReferenceId = acRefId;
            // A client sends no base form for its own character (the server refuses one).
            if (acRefId != kPlayerRef)
                request.FormId = aIsMount ? kHorseBase : kGuardBase;
            request.CellId = kStableCell;
            request.Position = acPosition;
            request.CurrentActorData.BaseId = request.FormId;
            request.CurrentActorData.InitialActorValues.ActorValuesList[24] = 300.0f;
            request.CurrentActorData.InitialActorValues.ActorMaxValuesList[24] = 300.0f;
            if (aIsMount)
            {
                Inventory::Entry apples{};
                apples.BaseId = kApple;
                apples.Count = 5;
                request.CurrentActorData.InitialInventory.AddOrRemoveEntry(apples);
            }
            request.IsMount = aIsMount;
            (void)DeliverWire(world, request, apOwner);
            const auto responses = DecodeOutboxMessagesFor<AssignCharacterResponse>(aConn);
            return responses.size() == 1 ? responses[0].ServerId : 0xFFFFFFFFu;
        };
        const auto requestMount = [&](Player* apRider, uint32_t aRiderId, uint32_t aMountId) {
            Harness::ClearOutbox();
            MountRequest request{};
            request.RiderId = aRiderId;
            request.MountId = aMountId;
            return DeliverWire(world, request, apRider);
        };
        // The other player is told about a mount only when the server accepted the request.
        const auto wasToldAboutMount = [](TiltedPhoques::ConnectionId_t aConn, uint32_t aRiderId, uint32_t aMountId) {
            for (const auto& notify : DecodeOutboxMessagesFor<NotifyMount>(aConn))
                if (notify.RiderId == aRiderId && notify.MountId == aMountId)
                    return true;
            return false;
        };
        const auto wasGrantedSimulation = [](TiltedPhoques::ConnectionId_t aConn, uint32_t aServerId) {
            for (const auto& grant : DecodeOutboxMessagesFor<NotifyOwnershipTransfer>(aConn))
                if (grant.ServerId == aServerId)
                    return true;
            return false;
        };
        // One movement report for an entity the sender simulates, optionally with an animation event.
        const auto reportMovement = [&](Player* apSender, uint32_t aServerId, const glm::vec3& acPosition, const char* acAnimationEvent) {
            ClientReferencesMoveRequest request{};
            request.Tick = 1000u;
            ReferenceUpdate update{};
            update.UpdatedMovement.CellId = kStableCell;
            update.UpdatedMovement.Position = acPosition;
            if (acAnimationEvent != nullptr)
            {
                ActionEvent action{};
                action.ActorId = aServerId;
                action.EventName = acAnimationEvent;
                update.ActionEvents.push_back(action);
            }
            request.Updates[aServerId] = update;
            Harness::ClearOutbox();
            Deliver(world, request, apSender);
        };
        const auto takeApple = [&](Player* apTaker, uint32_t aHorseId) {
            Harness::ClearOutbox();
            RequestInventoryChanges request{};
            request.ServerId = aHorseId;
            request.Item.BaseId = kApple;
            request.Item.Count = -1;
            request.UpdateClients = true;
            return DeliverWire(world, request, apTaker);
        };
        const auto applesLeft = [&](uint32_t aHorseId) {
            GameId apple = kApple;
            return world.get<InventoryComponent>(static_cast<entt::entity>(aHorseId)).Content.GetEntryCountById(apple);
        };

        const glm::vec3 kAtStable{100.0f, 100.0f, 0.0f};
        const uint32_t hostChar = assign(pHost, kHostConn, kPlayerRef, false, kAtStable);
        const uint32_t guestChar = assign(pGuest, kGuestConn, kPlayerRef, false, kAtStable);
        const uint32_t horseA = assign(pHost, kHostConn, GameId{0u, 0x000B2001u}, true, kAtStable);
        const uint32_t horseB = assign(pHost, kHostConn, GameId{0u, 0x000B2002u}, true, kAtStable);
        Check(hostChar != 0xFFFFFFFFu && guestChar != 0xFFFFFFFFu && horseA != 0xFFFFFFFFu && horseB != 0xFFFFFFFFu,
              "two player characters and two horses are registered");

        // --- Off (the default): the behaviour of 4.1.0, whoever mounts takes the horse over.
        Check(requestMount(pHost, hostChar, horseA) && wasToldAboutMount(kGuestConn, hostChar, horseA),
              "the host mounts a horse and the guest is told");
        Check(requestMount(pGuest, guestChar, horseA) && wasToldAboutMount(kHostConn, guestChar, horseA) &&
                  wasGrantedSimulation(kGuestConn, horseA),
              "with Gameplay:bEnableMountRideGuard off (the default) a second mount request is honoured as in 4.1.0");
        Check(takeApple(pHost, horseA) && applesLeft(horseA) == 4,
              "with the guard off a player who does not simulate the horse can still use its saddlebag");

        // --- On.
        bEnableMountRideGuard = true;
        reportMovement(pGuest, guestChar, kAtStable, "HorseExit");
        Check(requestMount(pHost, hostChar, horseA) && wasToldAboutMount(kGuestConn, hostChar, horseA) &&
                  wasGrantedSimulation(kHostConn, horseA),
              "with the guard on the host mounts horse A");
        Check(requestMount(pGuest, guestChar, horseA) && !wasToldAboutMount(kHostConn, guestChar, horseA) &&
                  !wasGrantedSimulation(kGuestConn, horseA),
              "the guest cannot mount the horse the host is riding");
        Check(takeApple(pGuest, horseA) && applesLeft(horseA) == 4, "the guest cannot change the saddlebag of the horse the host is riding");
        Check(takeApple(pHost, horseA) && applesLeft(horseA) == 3, "the rider can use the saddlebag of the horse they ride");

        // 1. The rider's own animation: the only dismount report a client produces.
        reportMovement(pHost, hostChar, kAtStable, "HorseExit");
        Check(takeApple(pGuest, horseA) && applesLeft(horseA) == 2, "after the host's dismount animation the saddlebag is open to the guest");
        Check(requestMount(pGuest, guestChar, horseA) && wasToldAboutMount(kHostConn, guestChar, horseA) &&
                  wasGrantedSimulation(kGuestConn, horseA),
              "after the host's dismount animation the guest can mount the horse");

        // 2. No animation event arrives (lost, or the game used another one): the rider walks away.
        reportMovement(pGuest, guestChar, kAtStable, "HorseExit");
        Check(requestMount(pHost, hostChar, horseB), "the host mounts horse B");
        reportMovement(pHost, hostChar, {100.0f + 400.0f, 100.0f, 0.0f}, nullptr);
        Check(requestMount(pGuest, guestChar, horseB) && !wasToldAboutMount(kHostConn, guestChar, horseB),
              "a rider whose position report lags a little behind the horse is still riding");
        reportMovement(pHost, hostChar, {100.0f + 5000.0f, 100.0f, 0.0f}, nullptr);
        Check(requestMount(pGuest, guestChar, horseB) && wasToldAboutMount(kHostConn, guestChar, horseB),
              "once the rider is far from the horse the ride is over, without any dismount report");

        // 3. An NPC the host simulates mounts a horse: no ride is recorded under the host's name.
        reportMovement(pGuest, guestChar, kAtStable, "HorseExit");
        reportMovement(pHost, hostChar, kAtStable, nullptr);
        const uint32_t hostNpc = assign(pHost, kHostConn, GameId{0u, 0x000B3001u}, false, kAtStable);
        Check(requestMount(pHost, hostChar, horseA), "the host mounts horse A again");
        Check(hostNpc != 0xFFFFFFFFu && requestMount(pHost, hostNpc, horseB) && wasToldAboutMount(kGuestConn, hostNpc, horseB),
              "an NPC simulated by the host mounts horse B");
        Check(requestMount(pGuest, guestChar, horseA) && !wasToldAboutMount(kHostConn, guestChar, horseA),
              "the NPC's mount did not overwrite the host's own ride: horse A is still the host's");

        // 4. The rider leaves the server.
        world.GetDispatcher().trigger(PlayerLeaveEvent{pHost});
        Check(requestMount(pGuest, guestChar, horseA) && world.GetCharacterService().GetMountGuard().GetActiveRiderPlayerId(horseA) == pGuest->GetId(),
              "after the rider left the server the guest can mount the horse");

        bEnableMountRideGuard = false;
        std::filesystem::remove(SaveSnapshotService::GetDefaultCheckpointStorePath(), removeEc);
    }

    // Scene 12: a container that is registered only when someone takes from it (M11). QuickLootIE and
    // the auto-loot cloak take items without activating the container, and a container outside the
    // player's own cell is not registered in advance. Each client rolls its own levelled contents,
    // so the second player may be looking at items the server has never heard of. The rules:
    //  - an item the server lists is taken by exactly one player, whatever the order of packets;
    //  - an item the server does not list is granted ONCE (CoopLootLedger case 3: a script may have
    //    put it there on that client only, and refusing would eat quest rewards), never twice.
    // The second rule is what is left of M11: the second player keeps the one item of its own roll
    // that it took before the server's contents reached it.
    {
        std::printf("[scene 12] loot from a container registered on the first take\n");
        std::filesystem::remove(SaveSnapshotService::GetDefaultCheckpointStorePath(), removeEc);

        GameServer server{registry};
        World& world = server.GetWorld();

        constexpr TiltedPhoques::ConnectionId_t kFirstConn = 0xC001u;
        constexpr TiltedPhoques::ConnectionId_t kSecondConn = 0xC002u;
        const GameId kRuinCell{0u, 0x000C1001u};
        const GameId kChest{0u, 0x000C2001u};
        const GameId kGold{0u, 0x0000000Fu};
        const GameId kSword{0u, 0x000139B9u};
        const GameId kAxe{0u, 0x000139B3u};

        Player* pFirst = world.GetPlayerManager().Create(kFirstConn);
        Player* pSecond = world.GetPlayerManager().Create(kSecondConn);
        TimeModel t0{};
        t0.Time = 11.0f;
        t0.Day = 4;
        t0.Month = 4;
        t0.Year = 201;
        world.GetDispatcher().trigger(PlayerJoinEvent{pFirst, {}, {}, DateTime{t0}});
        world.GetDispatcher().trigger(PlayerJoinEvent{pSecond, {}, {}, DateTime{t0}});
        pFirst->SetCellComponent(CellIdComponent{kRuinCell});
        pSecond->SetCellComponent(CellIdComponent{kRuinCell});

        const auto registerPlayerCharacter = [&](Player* apPlayer, TiltedPhoques::ConnectionId_t aConn, uint32_t aCookie) {
            Harness::ClearOutbox();
            AssignCharacterRequest request{};
            request.Cookie = aCookie;
            request.ReferenceId = GameId{0u, 0x14u};
            request.CellId = kRuinCell;
            request.CurrentActorData.InitialActorValues.ActorValuesList[24] = 100.0f;
            request.CurrentActorData.InitialActorValues.ActorMaxValuesList[24] = 100.0f;
            (void)DeliverWire(world, request, apPlayer);
            const auto responses = DecodeOutboxMessagesFor<AssignCharacterResponse>(aConn);
            return responses.size() == 1 ? responses[0].ServerId : 0xFFFFFFFFu;
        };
        const auto stack = [](const GameId& acItem, int32_t aCount) {
            Inventory::Entry entry{};
            entry.BaseId = acItem;
            entry.Count = aCount;
            return entry;
        };
        // What the client sends for a container it has not registered: the contents as it sees them now.
        const auto registerChest = [&](Player* apPlayer, TiltedPhoques::ConnectionId_t aConn, std::initializer_list<Inventory::Entry> aLocalContents) {
            Harness::ClearOutbox();
            AssignObjectsRequest request{};
            ObjectData chest{};
            chest.Id = kChest;
            chest.CellId = kRuinCell;
            for (const auto& entry : aLocalContents)
                chest.CurrentInventory.AddOrRemoveEntry(entry);
            request.Objects.push_back(chest);
            (void)DeliverWire(world, request, apPlayer);
            const auto responses = DecodeOutboxMessagesFor<AssignObjectsResponse>(aConn);
            return (responses.size() == 1 && responses[0].Objects.size() == 1) ? responses[0].Objects[0] : ObjectData{};
        };
        const auto take = [&](Player* apPlayer, uint32_t aChestServerId, const GameId& acItem, int32_t aCount) {
            Harness::ClearOutbox();
            RequestInventoryChanges request{};
            request.ServerId = aChestServerId;
            request.Item = stack(acItem, -aCount);
            request.UpdateClients = true;
            return DeliverWire(world, request, apPlayer);
        };
        // Net change the server told this connection to apply to the given entity, for one item.
        const auto toldToChange = [](TiltedPhoques::ConnectionId_t aConn, uint32_t aEntityServerId, const GameId& acItem) {
            int32_t total = 0;
            for (const auto& notify : DecodeOutboxMessagesFor<NotifyInventoryChanges>(aConn))
                if (notify.ServerId == aEntityServerId && notify.Item.BaseId == acItem)
                    total += notify.Item.Count;
            return total;
        };
        const auto countOf = [](const Inventory& acInventory, const GameId& acItem) {
            GameId lookup = acItem;
            return acInventory.GetEntryCountById(lookup);
        };

        const uint32_t firstChar = registerPlayerCharacter(pFirst, kFirstConn, 900u);
        const uint32_t secondChar = registerPlayerCharacter(pSecond, kSecondConn, 901u);
        Check(firstChar != 0xFFFFFFFFu && secondChar != 0xFFFFFFFFu, "both player characters are registered");

        // The first player's game rolled 10 gold and a sword. It takes the gold through the loot
        // window, so the container reaches the server already without it.
        const ObjectData asFirst = registerChest(pFirst, kFirstConn, {stack(kSword, 1)});
        Check(asFirst.IsSenderFirst && asFirst.ServerId != 0, "the first taker's view of the chest becomes the server's");
        const uint32_t chestId = asFirst.ServerId;

        // The second player's game rolled an axe instead of the sword. It took the axe before the
        // chest was known to it, and registers the chest as it sees it now: empty.
        const ObjectData asSecond = registerChest(pSecond, kSecondConn, {});
        Check(!asSecond.IsSenderFirst && asSecond.ServerId == chestId, "the second taker is told the chest already exists");
        Check(countOf(asSecond.CurrentInventory, kSword) == 1 && countOf(asSecond.CurrentInventory, kAxe) == 0,
              "and receives the server's contents (a sword), not its own roll (an axe)");

        // Its queued take of the axe now reaches the server.
        Check(take(pSecond, chestId, kAxe, 1), "the queued take of an item only the second client rolled is processed");
        Check(toldToChange(kSecondConn, secondChar, kAxe) == 0,
              "an item the server never listed is granted once: the second player keeps the axe of its own roll");
        Check(toldToChange(kFirstConn, chestId, kSword) == 0, "the server's own contents are untouched by it");
        Check(take(pFirst, chestId, kAxe, 1) && toldToChange(kFirstConn, firstChar, kAxe) == -1,
              "but not twice: another player claiming the same unlisted item is told to give it back");

        // Both players go for the single sword; the packets arrive one after the other.
        Check(take(pSecond, chestId, kSword, 1) && toldToChange(kFirstConn, chestId, kSword) == -1 &&
                  toldToChange(kSecondConn, secondChar, kSword) == 0,
              "the take that arrives first is granted and the other player sees the sword leave the chest");
        Check(take(pFirst, chestId, kSword, 1) && toldToChange(kFirstConn, firstChar, kSword) == -1,
              "the take that arrives second is refused: that player is told to give the sword back");
        Check(toldToChange(kSecondConn, chestId, kSword) == 0, "and the winner is not told about a second sword leaving the chest");

        // A third registration (a player who walks in later) gets the chest as it is now.
        const ObjectData later = registerChest(pFirst, kFirstConn, {stack(kSword, 1), stack(kGold, 10)});
        Check(!later.IsSenderFirst && countOf(later.CurrentInventory, kSword) == 0 && countOf(later.CurrentInventory, kGold) == 0,
              "a later registration cannot put taken items back into the chest");

        std::filesystem::remove(SaveSnapshotService::GetDefaultCheckpointStorePath(), removeEc);
    }

    // Scene 13: the desync detector (work plan 3.3). A client reports what it believes about every
    // character; the server compares the report with its own state for that observer. Every report
    // below is the packet a client sends, through the wire encoding.
    {
        std::printf("[scene 13] desync detector: reports, confirmation, bounded resend\n");
        std::filesystem::remove(SaveSnapshotService::GetDefaultCheckpointStorePath(), removeEc);

        GameServer server{registry};
        World& world = server.GetWorld();
        auto& resync = world.ctx().at<SessionResyncService>();

        constexpr TiltedPhoques::ConnectionId_t kHostConn = 0xD001u;
        constexpr TiltedPhoques::ConnectionId_t kGuestConn = 0xD002u;
        const GameId kCell{0u, 0x000D1001u};
        const GameId kPlayerRef{0u, 0x14u};
        const GameId kGuardBase{0u, 0x000D4001u};

        Player* pHost = world.GetPlayerManager().Create(kHostConn);
        Player* pGuest = world.GetPlayerManager().Create(kGuestConn);
        TimeModel t0{};
        t0.Time = 10.0f;
        t0.Day = 3;
        t0.Month = 3;
        t0.Year = 201;
        world.GetDispatcher().trigger(PlayerJoinEvent{pHost, {}, {}, DateTime{t0}});
        world.GetDispatcher().trigger(PlayerJoinEvent{pGuest, {}, {}, DateTime{t0}});
        pHost->SetCellComponent(CellIdComponent{kCell});
        pGuest->SetCellComponent(CellIdComponent{kCell});

        uint32_t nextCookie = 900u;
        const auto assign = [&](Player* apOwner, TiltedPhoques::ConnectionId_t aConn, const GameId& acRefId) {
            Harness::ClearOutbox();
            AssignCharacterRequest request{};
            request.Cookie = nextCookie++;
            request.ReferenceId = acRefId;
            if (acRefId != kPlayerRef)
                request.FormId = kGuardBase;
            request.CellId = kCell;
            request.Position = glm::vec3{100.0f, 100.0f, 0.0f};
            request.CurrentActorData.BaseId = request.FormId;
            request.CurrentActorData.InitialActorValues.ActorValuesList[24] = 300.0f;
            request.CurrentActorData.InitialActorValues.ActorMaxValuesList[24] = 300.0f;
            (void)DeliverWire(world, request, apOwner);
            const auto responses = DecodeOutboxMessagesFor<AssignCharacterResponse>(aConn);
            return responses.size() == 1 ? responses[0].ServerId : 0xFFFFFFFFu;
        };

        const uint32_t hostChar = assign(pHost, kHostConn, kPlayerRef);
        const uint32_t guestChar = assign(pGuest, kGuestConn, kPlayerRef);
        Harness::ClearOutbox();
        const uint32_t guard = assign(pHost, kHostConn, GameId{0u, 0x000D2001u});
        Check(hostChar != 0xFFFFFFFFu && guestChar != 0xFFFFFFFFu && guard != 0xFFFFFFFFu,
              "two player characters and a guard simulated by the host are registered");
        Check(world.GetCharacterService().GetInterestRegistry().IsMarkedInRange(pGuest->GetId(), guard),
              "the server considers the guard delivered to the guest");

        using Session::EntityDigest::Facts;
        const auto row = [](uint32_t aServerId, uint8_t aFlags, uint8_t aHealth = Session::EntityDigest::kNoHealth) {
            Facts facts{};
            facts.ServerId = aServerId;
            facts.Flags = aFlags;
            facts.HealthPct = aHealth;
            return facts;
        };
        using Session::EntityDigest::kFlagDead;
        using Session::EntityDigest::kFlagLive;
        using Session::EntityDigest::kFlagOwnedByReporter;

        uint32_t sequence = 0;
        // One report from the guest, five seconds after the previous one.
        const auto report = [&](std::initializer_list<Facts> aRows) {
            resync.SkewClockForTesting(5000);
            Harness::ClearOutbox();
            ClientEntityDigest digest{};
            digest.Sequence = ++sequence;
            for (const Facts& facts : aRows)
                digest.Entries.push_back(facts);
            return DeliverWire(world, digest, pGuest);
        };
        const auto resentToGuest = [&](uint32_t aServerId) {
            for (const auto& spawn : DecodeOutboxMessagesFor<CharacterSpawnRequest>(kGuestConn))
                if (spawn.ServerId == aServerId)
                    return true;
            return false;
        };
        const auto removedForGuest = [&](uint32_t aServerId) {
            for (const auto& removal : DecodeOutboxMessagesFor<NotifyRemoveCharacter>(kGuestConn))
                if (removal.ServerId == aServerId)
                    return true;
            return false;
        };
        const auto totals = [&]() { return resync.GetDesyncTotals(); };

        const Facts mine = row(guestChar, kFlagOwnedByReporter);
        const Facts hostSeen = row(hostChar, kFlagLive, 100);
        const Facts guardAlive = row(guard, kFlagLive, 100);
        const Facts guardDead = row(guard, kFlagLive | kFlagDead, 0);

        // --- In sync: nothing is said, however often it is reported.
        Check(report({mine, hostSeen, guardAlive}) && report({mine, hostSeen, guardAlive}) && report({mine, hostSeen, guardAlive}),
              "the guest's reports are accepted");
        Check(totals().Reports == 3 && totals().Confirmed == 0 && totals().Repaired == 0,
              "a client that agrees with the server produces no finding");

        // --- The guest never got the guard. Seen once it is a suspicion; seen again it is a finding.
        (void)report({mine, hostSeen});
        Check(totals().Confirmed == 0, "a difference reported once is not a finding yet (the spawn may be in flight)");
        (void)report({mine, hostSeen});
        Check(totals().Confirmed == 1, "the same difference in the next report is confirmed and logged");
        Check(!resentToGuest(guard) && totals().Repaired == 0,
              "with Gameplay:bEnableDesyncAutoRepair off (the default) the detector only writes to the log");

        bEnableDesyncAutoRepair = true;
        (void)report({mine, hostSeen});
        Check(resentToGuest(guard) && totals().Repaired == 1, "switched on, the missing guard is resent to that client");
        (void)report({mine, hostSeen, guardAlive});
        Check(totals().Healed == 1 && !resentToGuest(guard), "the next report has the guard: the repair is recorded, nothing more is sent");

        // --- The host kills the guard; the guest's screen still shows it alive.
        RequestDeathStateChange death{};
        death.Id = guard;
        death.IsDead = true;
        Check(DeliverWire(world, death, pHost) && world.get<CharacterComponent>(static_cast<entt::entity>(guard)).IsDead(),
              "the host reports the guard's death");
        const uint64_t confirmedBeforeDeath = totals().Confirmed;
        (void)report({mine, hostSeen, guardAlive});
        Check(!resentToGuest(guard), "one report with the guard alive may be the death still in flight");
        (void)report({mine, hostSeen, guardAlive});
        bool resentAsDead = false;
        for (const auto& spawn : DecodeOutboxMessagesFor<CharacterSpawnRequest>(kGuestConn))
            resentAsDead = resentAsDead || (spawn.ServerId == guard && spawn.IsDead);
        Check(totals().Confirmed == confirmedBeforeDeath + 1 && resentAsDead,
              "the second one is a life-state desync: logged, and the guard is resent as dead");
        (void)report({mine, hostSeen, guardDead});
        Check(totals().Healed == 2, "the guest agrees after the resend");

        // --- A resend that changes nothing is not repeated forever.
        const uint64_t repairedBefore = totals().Repaired;
        for (int i = 0; i < 12; ++i)
            (void)report({mine, hostSeen, guardAlive});
        Check(totals().Repaired == repairedBefore + 1,
              "a client that keeps showing the guard alive gets one more resend in the window, not one per report");

        // --- A character the guest still shows and the server does not have at all.
        const uint32_t kGhost = 0x000ABCDEu;
        (void)report({mine, hostSeen, guardDead, row(kGhost, kFlagLive, 100)});
        Check(!removedForGuest(kGhost), "an unknown character reported once may be a removal in flight");
        (void)report({mine, hostSeen, guardDead, row(kGhost, kFlagLive, 100)});
        Check(removedForGuest(kGhost), "reported again, the guest is told to remove it");

        // --- Who simulates it: said, never "repaired" with a second copy.
        const uint64_t confirmedBeforeOwner = totals().Confirmed;
        (void)report({mine, hostSeen, row(guard, kFlagOwnedByReporter)});
        (void)report({mine, hostSeen, row(guard, kFlagOwnedByReporter)});
        Check(totals().Confirmed == confirmedBeforeOwner + 1 && !resentToGuest(guard),
              "a guest that believes it simulates the host's guard is logged and not sent a copy of it");

        // --- Whatever a client reports, the server's work for it is bounded.
        ClientEntityDigest flood{};
        for (uint32_t id = 0; id < 400; ++id)
            flood.Entries.push_back(row(0x00F00000u + id, kFlagLive, 100));
        resync.SkewClockForTesting(5000);
        (void)DeliverWire(world, flood, pGuest);
        resync.SkewClockForTesting(5000);
        Harness::ClearOutbox();
        (void)DeliverWire(world, flood, pGuest);
        const size_t removalsForFlood = DecodeOutboxMessagesFor<NotifyRemoveCharacter>(kGuestConn).size();
        Check(removalsForFlood > 0 && removalsForFlood <= 24,
              "a report full of unknown characters costs the server at most its per-player resend budget");

        // --- The guest's own inventory against the server's copy of it (work plan 5.3). The copy is
        //     what a rollback restores from. The server can only say that it is wrong: it never hands
        //     out or takes away items, and it never resends a character for it.
        {
            using Session::EntityDigest::InventoryHasher;
            using Session::EntityDigest::kFlagHasInventory;
            const GameId kGold{0u, 0x0000000Fu};
            const GameId kSword{0u, 0x000139B9u};

            // What the guest's game holds, as that client would sum it up.
            const auto carrying = [&](std::initializer_list<std::pair<GameId, int32_t>> aItems) {
                InventoryHasher hasher;
                for (const auto& [id, count] : aItems)
                    hasher.Add(id.ModId, id.BaseId, count);
                const InventoryHasher::Summary summary = hasher.Finish();
                Facts facts = row(guestChar, kFlagOwnedByReporter | kFlagHasInventory);
                facts.InventoryHash = summary.Hash;
                facts.InventoryKinds = summary.Kinds;
                facts.InventoryItems = summary.Items;
                return facts;
            };
            // The packet the guest's client sends when its player gains or loses an item.
            const auto guestItemDelta = [&](const GameId& acItem, int32_t aCount) {
                RequestInventoryChanges request{};
                request.ServerId = guestChar;
                request.Item.BaseId = acItem;
                request.Item.Count = aCount;
                request.UpdateClients = true;
                return DeliverWire(world, request, pGuest);
            };
            const auto sentToGuestAsRepair = [&]() {
                return !DecodeOutboxMessagesFor<CharacterSpawnRequest>(kGuestConn).empty() ||
                       !DecodeOutboxMessagesFor<NotifyRemoveCharacter>(kGuestConn).empty();
            };

            // The rest of this client's view agrees with the server from here on (the guard is dead on
            // both sides), so every finding below is about the inventory. Two reports close whatever
            // the checks above left open.
            (void)report({mine, hostSeen, guardDead});
            (void)report({mine, hostSeen, guardDead});

            bEnableDesyncAutoRepair = true;  // even with repairs on, an inventory is never "repaired"
            const uint64_t confirmedBefore = totals().Confirmed;
            const uint64_t repairedBeforeInventory = totals().Repaired;

            Check(guestItemDelta(kGold, 250) && guestItemDelta(kSword, 1), "the guest's client reports 250 gold and a sword gained");
            (void)report({carrying({{kGold, 250}, {kSword, 1}}), hostSeen, guardDead});
            (void)report({carrying({{kGold, 250}, {kSword, 1}}), hostSeen, guardDead});
            Check(totals().Confirmed == confirmedBefore, "an inventory the server's copy agrees with produces no finding");

            // The player picks up 30 more gold; the digest leaves before the item packet does.
            (void)report({carrying({{kGold, 280}, {kSword, 1}}), hostSeen, guardDead});
            Check(guestItemDelta(kGold, 30), "the item packet arrives after the digest that already counted it");
            (void)report({carrying({{kGold, 280}, {kSword, 1}}), hostSeen, guardDead});
            Check(totals().Confirmed == confirmedBefore, "a pickup that was still in flight is not reported as a desync");

            // A change the client never told the server about (a script, a hook that did not fire):
            // the player has 5 more gold than the server's copy, report after report.
            (void)report({carrying({{kGold, 285}, {kSword, 1}}), hostSeen, guardDead});
            Check(totals().Confirmed == confirmedBefore, "seen once it is only a suspicion");
            (void)report({carrying({{kGold, 285}, {kSword, 1}}), hostSeen, guardDead});
            Check(totals().Confirmed == confirmedBefore + 1, "seen again it is a confirmed inventory difference, written to the log");
            Check(!sentToGuestAsRepair() && totals().Repaired == repairedBeforeInventory,
                  "nothing is resent for it, with auto-repair on: the server cannot make a player's inventory right");

            // The copy catches up (the client reports the missing gold): the difference is gone.
            Check(guestItemDelta(kGold, 5), "the missing gold is reported after all");
            const uint64_t confirmedAfter = totals().Confirmed;
            (void)report({carrying({{kGold, 285}, {kSword, 1}}), hostSeen, guardDead});
            (void)report({carrying({{kGold, 285}, {kSword, 1}}), hostSeen, guardDead});
            Check(totals().Confirmed == confirmedAfter, "once the copy agrees again nothing more is said");
            bEnableDesyncAutoRepair = false;
        }

        // --- The detector's own switch.
        bEnableDesyncDetector = false;
        const uint64_t reportsBefore = totals().Reports;
        (void)report({mine, hostSeen});
        Check(totals().Reports == reportsBefore, "with Gameplay:bEnableDesyncDetector off reports are ignored");
        bEnableDesyncDetector = true;
        bEnableDesyncAutoRepair = false;
    }

    // Scene 14: the party leader asks for a rollback (work plan 4.4, defect M15). A client can send
    // three such requests. The one for the checkpoint BEFORE the newest was implemented on the server
    // since 4.0 and had no sender until 4.1.1 (Shift+F9, common/Save/LoadRequest.h); this is the first
    // time that server path runs anywhere. Every request below goes through the wire encoding.
    {
        std::printf("[scene 14] rollback requests: newest, previous, by name; only from the leader\n");
        std::filesystem::remove(SaveSnapshotService::GetDefaultCheckpointStorePath(), removeEc);

        GameServer server{registry};
        World& world = server.GetWorld();
        auto& saves = world.GetSaveSnapshotService();
        auto& coordinator = saves.GetCoordinator();

        constexpr TiltedPhoques::ConnectionId_t kHostConn = 0xE001u;
        constexpr TiltedPhoques::ConnectionId_t kGuestConn = 0xE002u;
        Player* pHost = world.GetPlayerManager().Create(kHostConn);
        Player* pGuest = world.GetPlayerManager().Create(kGuestConn);
        TimeModel t0{};
        t0.Time = 9.0f;
        t0.Day = 4;
        t0.Month = 4;
        t0.Year = 201;
        world.GetDispatcher().trigger(PlayerJoinEvent{pHost, {}, {}, DateTime{t0}});
        world.GetDispatcher().trigger(PlayerJoinEvent{pGuest, {}, {}, DateTime{t0}});

        const auto ack = [](uint64_t aEpochId, const std::string& acSlot) {
            CoordinatedSaveAck message{};
            message.EpochId = aEpochId;
            message.SlotName = String(acSlot.c_str());
            message.Success = true;
            return message;
        };

        // A committed checkpoint: the server starts the barrier, both clients report their file written.
        const auto checkpoint = [&]() -> std::string {
            if (!saves.TriggerCoordinatedSave(false, nullptr))
                return {};
            const auto pending = coordinator.GetPendingBarrier();
            if (!pending.has_value())
                return {};
            const std::string slot = pending->SlotName;
            (void)DeliverWire(world, ack(pending->EpochId, slot), pHost);
            (void)DeliverWire(world, ack(pending->EpochId, slot), pGuest);
            return slot;
        };

        const std::string first = checkpoint();
        const std::string second = checkpoint();
        const std::string third = checkpoint();
        Check(!first.empty() && !second.empty() && !third.empty() && first != second && second != third && first != third,
              "three checkpoints are committed, each in its own slot");
        Check(saves.GetSnapshotCount() == 3, "the server keeps all three");

        const auto request = [&](Player* apFrom, RequestCoordinatedSaveLoad::ActionType aAction, const std::string& acSlot = {}) {
            Harness::ClearOutbox();
            RequestCoordinatedSaveLoad message{};
            message.Action = aAction;
            message.SlotName = String(acSlot.c_str());
            (void)DeliverWire(world, message, apFrom);
        };
        // The slot a player was told to load by the last request ("" when he was told nothing).
        const auto toldToLoad = [](TiltedPhoques::ConnectionId_t aConn) -> std::string {
            const auto loads = DecodeOutboxMessagesFor<NotifyExecuteLoad>(aConn);
            if (loads.empty())
                return {};
            if (loads.size() > 1)
                return "<" + std::to_string(loads.size()) + " load orders>";
            return std::string(loads[0].SlotName.c_str(), loads[0].SlotName.size());
        };
        // Both clients finished loading: the reload barrier of the last rollback is released.
        const auto everyoneReloaded = [&](const std::string& acSlot) {
            const auto* pReload = coordinator.GetReloadBarrier();
            if (!pReload)
                return;
            const uint64_t encoded = CoopCheckpoint::EncodeReloadBarrierEpoch(pReload->EpochId);
            (void)DeliverWire(world, ack(encoded, acSlot), pHost);
            (void)DeliverWire(world, ack(encoded, acSlot), pGuest);
        };

        // --- Only the leader rolls the party back.
        request(pGuest, RequestCoordinatedSaveLoad::ActionType::LoadPreviousCheckpoint);
        Check(toldToLoad(kHostConn).empty() && toldToLoad(kGuestConn).empty(),
              "a rollback request from a player who is not the leader loads nothing");
        Check(saves.GetSnapshotCount() == 3, "and drops no checkpoint");

        // --- The checkpoint before the newest.
        request(pHost, RequestCoordinatedSaveLoad::ActionType::LoadPreviousCheckpoint);
        Check(toldToLoad(kHostConn) == second, "the previous-checkpoint request tells the leader to load the checkpoint before the newest");
        Check(toldToLoad(kGuestConn) == second, "and tells every other player the same slot");
        Check(saves.GetSnapshotCount() == 2, "the newest checkpoint is dropped: it was the reason to go back");
        Check(coordinator.GetReloadBarrier() != nullptr, "the party is held together until everyone has loaded");
        everyoneReloaded(second);
        Check(coordinator.GetReloadBarrier() == nullptr, "the hold is released when both clients report the load done");

        // --- F9 after that: the newest is now the one just loaded, the dropped one cannot come back.
        request(pHost, RequestCoordinatedSaveLoad::ActionType::LoadLatestCheckpoint);
        Check(toldToLoad(kHostConn) == second && toldToLoad(kGuestConn) == second,
              "a quickload after going back loads the same checkpoint, not the dropped one");
        Check(saves.GetSnapshotCount() == 2, "and drops nothing");
        everyoneReloaded(second);

        // --- A slot picked in the load menu.
        request(pHost, RequestCoordinatedSaveLoad::ActionType::LoadSpecificSlot, first);
        Check(toldToLoad(kHostConn) == first && toldToLoad(kGuestConn) == first, "a slot picked by name is loaded by everybody");
        Check(saves.GetSnapshotCount() == 1, "the checkpoints after it are dropped");
        everyoneReloaded(first);

        // --- Nothing before the only checkpoint: going back means loading it.
        request(pHost, RequestCoordinatedSaveLoad::ActionType::LoadPreviousCheckpoint);
        Check(toldToLoad(kHostConn) == first && toldToLoad(kGuestConn) == first,
              "with one checkpoint left the previous-checkpoint request loads that one");
        Check(saves.GetSnapshotCount() == 1, "and keeps it");
        everyoneReloaded(first);

        // --- A slot name the server never committed: the newest checkpoint, never an arbitrary file.
        request(pHost, RequestCoordinatedSaveLoad::ActionType::LoadSpecificSlot, "Quicksave0_of_somebody");
        Check(toldToLoad(kHostConn) == first && toldToLoad(kGuestConn) == first,
              "a slot name that is not a party checkpoint loads the newest checkpoint instead");
    }

    // Scene 15: the player who simulates the enemies of a fight disconnects in the middle of it
    // (work plan 5.1 / 5.2). The other player in the cell must become their simulator without the
    // fight changing: a wounded enemy keeps its wounds, a dead one stays dead, and the new simulator
    // is told. Every request goes through the wire encoding.
    {
        std::printf("[scene 15] the simulating player leaves in the middle of a fight\n");
        std::filesystem::remove(SaveSnapshotService::GetDefaultCheckpointStorePath(), removeEc);

        GameServer server{registry};
        World& world = server.GetWorld();

        constexpr TiltedPhoques::ConnectionId_t kHostConn = 0xF001u;
        constexpr TiltedPhoques::ConnectionId_t kGuestConn = 0xF002u;
        const GameId kCell{0u, 0x000F1001u};
        const GameId kPlayerRef{0u, 0x14u};
        const GameId kBanditBase{0u, 0x000F4001u};
        constexpr uint32_t kHealthIndex = ActorValuesComponent::kHealthActorValueIndex;

        Player* pHost = world.GetPlayerManager().Create(kHostConn);
        Player* pGuest = world.GetPlayerManager().Create(kGuestConn);
        TimeModel t0{};
        t0.Time = 12.0f;
        t0.Day = 5;
        t0.Month = 5;
        t0.Year = 201;
        world.GetDispatcher().trigger(PlayerJoinEvent{pHost, {}, {}, DateTime{t0}});
        world.GetDispatcher().trigger(PlayerJoinEvent{pGuest, {}, {}, DateTime{t0}});
        pHost->SetCellComponent(CellIdComponent{kCell});
        pGuest->SetCellComponent(CellIdComponent{kCell});

        uint32_t nextCookie = 1500u;
        const auto assign = [&](Player* apOwner, TiltedPhoques::ConnectionId_t aConn, const GameId& acRefId) {
            Harness::ClearOutbox();
            AssignCharacterRequest request{};
            request.Cookie = nextCookie++;
            request.ReferenceId = acRefId;
            if (acRefId != kPlayerRef)
                request.FormId = kBanditBase;
            request.CellId = kCell;
            request.Position = glm::vec3{200.0f, 200.0f, 0.0f};
            request.CurrentActorData.BaseId = request.FormId;
            request.CurrentActorData.InitialActorValues.ActorValuesList[kHealthIndex] = 300.0f;
            request.CurrentActorData.InitialActorValues.ActorMaxValuesList[kHealthIndex] = 300.0f;
            (void)DeliverWire(world, request, apOwner);
            const auto responses = DecodeOutboxMessagesFor<AssignCharacterResponse>(aConn);
            return responses.size() == 1 ? responses[0].ServerId : 0xFFFFFFFFu;
        };

        const uint32_t hostChar = assign(pHost, kHostConn, kPlayerRef);
        const uint32_t guestChar = assign(pGuest, kGuestConn, kPlayerRef);
        const uint32_t wounded = assign(pHost, kHostConn, GameId{0u, 0x000F2001u});
        const uint32_t corpse = assign(pHost, kHostConn, GameId{0u, 0x000F2002u});
        const uint32_t untouched = assign(pHost, kHostConn, GameId{0u, 0x000F2003u});
        Check(hostChar != 0xFFFFFFFFu && guestChar != 0xFFFFFFFFu && wounded != 0xFFFFFFFFu && corpse != 0xFFFFFFFFu &&
                  untouched != 0xFFFFFFFFu,
              "two players and three bandits simulated by the host are registered");

        const auto entityOf = [](uint32_t aServerId) { return static_cast<entt::entity>(aServerId); };
        const auto ownerOf = [&](uint32_t aServerId) { return world.get<OwnerComponent>(entityOf(aServerId)).GetOwner(); };
        const auto healthOf = [&](uint32_t aServerId) {
            float health = -1.0f;
            (void)world.get<ActorValuesComponent>(entityOf(aServerId)).TryGetHealth(health);
            return health;
        };
        Check(ownerOf(wounded) == pHost && ownerOf(corpse) == pHost && ownerOf(untouched) == pHost, "the host simulates all three");

        // The fight: the host's client reports one bandit down to 120 of 300 and another one dead.
        RequestActorValueChanges hit{};
        hit.Id = wounded;
        hit.Values[kHealthIndex] = 120.0f;
        // The revision the host's client holds for this actor (it came with the assignment).
        hit.HealthRevision = world.get<ActorValuesComponent>(entityOf(wounded)).HealthRevision;
        Check(DeliverWire(world, hit, pHost) && std::fabs(healthOf(wounded) - 120.0f) < 0.01f, "the host wounds a bandit to 120 of 300");
        RequestDeathStateChange death{};
        death.Id = corpse;
        death.IsDead = true;
        Check(DeliverWire(world, death, pHost) && world.get<CharacterComponent>(entityOf(corpse)).IsDead(), "the host kills another bandit");

        const uint64_t woundedEpochBefore = world.get<OwnerComponent>(entityOf(wounded)).GetLease().Epoch;
        const uint32_t woundedRevisionBefore = world.get<ActorValuesComponent>(entityOf(wounded)).HealthRevision;

        // The host's connection drops: the transport reports it, as it does when a game closes.
        Harness::ClearOutbox();
        Disconnect(server, kHostConn);
        pHost = nullptr;
        Check(world.GetPlayerManager().GetByConnectionId(kHostConn) == nullptr, "the host is gone from the server");

        Check(world.valid(entityOf(wounded)) && world.valid(entityOf(corpse)) && world.valid(entityOf(untouched)),
              "the bandits outlive the player who simulated them");
        Check(ownerOf(wounded) == pGuest && ownerOf(untouched) == pGuest, "the guest, who stands in the same cell, simulates the living bandits now");

        const auto transfers = DecodeOutboxMessagesFor<NotifyOwnershipTransfer>(kGuestConn);
        const auto transferOf = [&](uint32_t aServerId) -> const NotifyOwnershipTransfer* {
            for (const auto& transfer : transfers)
                if (transfer.ServerId == aServerId)
                    return &transfer;
            return nullptr;
        };
        const NotifyOwnershipTransfer* pWoundedTransfer = transferOf(wounded);
        Check(pWoundedTransfer != nullptr && transferOf(untouched) != nullptr, "the guest is told about each bandit it took over");
        Check(pWoundedTransfer != nullptr && pWoundedTransfer->OwnershipEpoch > woundedEpochBefore &&
                  world.get<OwnerComponent>(entityOf(wounded)).GetLease().Epoch == pWoundedTransfer->OwnershipEpoch,
              "with a newer ownership epoch than the host had, the one the server holds");
        Check(pWoundedTransfer != nullptr && pWoundedTransfer->HealthRevision == world.get<ActorValuesComponent>(entityOf(wounded)).HealthRevision,
              "and with the health revision its own readings must carry from now on");

        Check(std::fabs(healthOf(wounded) - 120.0f) < 0.01f, "the wounded bandit is still at 120: the handover did not heal it");
        Check(world.get<ActorValuesComponent>(entityOf(wounded)).HealthRevision >= woundedRevisionBefore,
              "its health revision did not go backwards");
        Check(std::fabs(healthOf(untouched) - 300.0f) < 0.01f, "the untouched bandit is still at full health");
        Check(world.get<CharacterComponent>(entityOf(corpse)).IsDead(), "the dead bandit is still dead");
        Check(!world.valid(entityOf(hostChar)) || !world.all_of<CharacterComponent>(entityOf(hostChar)) || ownerOf(hostChar) != pGuest,
              "the departed player's own character is not handed to anybody");

        // The new simulator goes on with the fight: its readings are accepted, with the new epoch.
        RequestActorValueChanges secondHit{};
        secondHit.Id = wounded;
        secondHit.Values[kHealthIndex] = 60.0f;
        // The revision the handover told the guest's client.
        secondHit.HealthRevision = pWoundedTransfer != nullptr ? pWoundedTransfer->HealthRevision : 0u;
        Check(DeliverWire(world, secondHit, pGuest) && std::fabs(healthOf(wounded) - 60.0f) < 0.01f,
              "the guest's next hit on the bandit it now simulates is applied");

        // The body is the guest's to keep as well: it is told, and the body is still a body.
        Check(ownerOf(corpse) == pGuest || ownerOf(corpse) == nullptr, "the dead bandit is not left with the departed player");
        Check(world.get<CharacterComponent>(entityOf(corpse)).IsDead(), "and it is still dead after the guest's hit on the other one");

        // The guest leaves too: nobody is left to simulate, and placed NPCs wait as they are.
        Harness::ClearOutbox();
        Disconnect(server, kGuestConn);
        Check(world.valid(entityOf(wounded)) && ownerOf(wounded) == nullptr, "with nobody left the wounded bandit stays in the world without a simulator");
        Check(world.valid(entityOf(wounded)) && std::fabs(healthOf(wounded) - 60.0f) < 0.01f, "at the health the fight left it with");
        Check(world.valid(entityOf(corpse)) && world.get<CharacterComponent>(entityOf(corpse)).IsDead(), "and the dead one remains dead for whoever comes next");
    }

    // Scene 16: one fight, two players, and the enemies one of them simulates (work plan 5.2).
    // What reaches the server during such a fight comes from two games that disagree for a moment
    // about everything: who hit first, whether the enemy is still alive, who simulates it. The
    // scene sends those packets in the orders a real session produces and pins what the server
    // makes of them: the kill by the player who does not simulate the target, the packets of the
    // simulating player that were already on their way, a stagger, a drawn weapon, and a handover
    // in the middle of the fight followed by the previous simulator's late packets.
    {
        std::printf("[scene 16] one fight seen by two players: kill, stagger, late packets, handover\n");
        std::filesystem::remove(SaveSnapshotService::GetDefaultCheckpointStorePath(), removeEc);

        GameServer server{registry};
        World& world = server.GetWorld();

        constexpr TiltedPhoques::ConnectionId_t kHostConn = 0xF101u;
        constexpr TiltedPhoques::ConnectionId_t kGuestConn = 0xF102u;
        const GameId kCell{0u, 0x00101001u};
        const GameId kFarCell{0u, 0x00101999u};
        const GameId kPlayerRef{0u, 0x14u};
        const GameId kBanditBase{0u, 0x00104001u};
        constexpr uint32_t kHealthIndex = ActorValuesComponent::kHealthActorValueIndex;

        Player* pHost = world.GetPlayerManager().Create(kHostConn);
        Player* pGuest = world.GetPlayerManager().Create(kGuestConn);
        TimeModel t0{};
        t0.Time = 12.0f;
        t0.Day = 5;
        t0.Month = 5;
        t0.Year = 201;
        world.GetDispatcher().trigger(PlayerJoinEvent{pHost, {}, {}, DateTime{t0}});
        world.GetDispatcher().trigger(PlayerJoinEvent{pGuest, {}, {}, DateTime{t0}});
        pHost->SetCellComponent(CellIdComponent{kCell});
        pGuest->SetCellComponent(CellIdComponent{kCell});

        uint32_t nextCookie = 1600u;
        const auto assign = [&](Player* apOwner, TiltedPhoques::ConnectionId_t aConn, const GameId& acRefId) {
            Harness::ClearOutbox();
            AssignCharacterRequest request{};
            request.Cookie = nextCookie++;
            request.ReferenceId = acRefId;
            if (acRefId != kPlayerRef)
                request.FormId = kBanditBase;
            request.CellId = kCell;
            request.Position = glm::vec3{200.0f, 200.0f, 0.0f};
            request.CurrentActorData.BaseId = request.FormId;
            request.CurrentActorData.InitialActorValues.ActorValuesList[kHealthIndex] = 300.0f;
            request.CurrentActorData.InitialActorValues.ActorMaxValuesList[kHealthIndex] = 300.0f;
            (void)DeliverWire(world, request, apOwner);
            const auto responses = DecodeOutboxMessagesFor<AssignCharacterResponse>(aConn);
            return responses.size() == 1 ? responses[0].ServerId : 0xFFFFFFFFu;
        };

        const uint32_t hostChar = assign(pHost, kHostConn, kPlayerRef);
        const uint32_t guestChar = assign(pGuest, kGuestConn, kPlayerRef);
        const uint32_t victim = assign(pHost, kHostConn, GameId{0u, 0x00102001u});
        const uint32_t brawler = assign(pHost, kHostConn, GameId{0u, 0x00102002u});
        Check(hostChar != 0xFFFFFFFFu && guestChar != 0xFFFFFFFFu && victim != 0xFFFFFFFFu && brawler != 0xFFFFFFFFu,
              "two players and two bandits simulated by the host are registered");

        const auto entityOf = [](uint32_t aServerId) { return static_cast<entt::entity>(aServerId); };
        const auto ownerOf = [&](uint32_t aServerId) { return world.get<OwnerComponent>(entityOf(aServerId)).GetOwner(); };
        const auto epochOf = [&](uint32_t aServerId) { return world.get<OwnerComponent>(entityOf(aServerId)).GetLease().Epoch; };
        const auto revisionOf = [&](uint32_t aServerId) { return world.get<ActorValuesComponent>(entityOf(aServerId)).HealthRevision; };
        const auto isDead = [&](uint32_t aServerId) { return world.get<CharacterComponent>(entityOf(aServerId)).IsDead(); };
        const auto healthOf = [&](uint32_t aServerId) {
            float health = -1.0f;
            (void)world.get<ActorValuesComponent>(entityOf(aServerId)).TryGetHealth(health);
            return health;
        };
        const auto near = [](float aLhs, float aRhs, float aTolerance = 0.01f) { return std::fabs(aLhs - aRhs) < aTolerance; };

        uint32_t nextHitId = 16000u;
        const auto swing = [&](uint32_t aTarget, float aDelta, float aStagger) {
            RequestHealthChangeBroadcast hit{};
            hit.Id = aTarget;
            hit.DeltaHealth = aDelta;
            hit.StaggerMagnitude = aStagger;
            hit.StaggerDirection = aStagger > 0.0f ? 0.5f : 0.0f;
            hit.HitId = nextHitId++;
            return hit;
        };
        const auto reading = [&](uint32_t aTarget, float aHealth, uint32_t aRevision) {
            RequestActorValueChanges snapshot{};
            snapshot.Id = aTarget;
            snapshot.Values[kHealthIndex] = aHealth;
            snapshot.HealthRevision = aRevision;
            return snapshot;
        };

        // --- 1. A power attack by the player who does not simulate the bandit -------------------
        Harness::ClearOutbox();
        const uint32_t revisionBeforeHit = revisionOf(victim);
        const RequestHealthChangeBroadcast guestHit = swing(victim, -100.0f, 0.6f);
        Check(DeliverWire(world, guestHit, pGuest) && near(healthOf(victim), 200.0f),
              "a hit by the player who does not simulate the bandit is applied: 300 -> 200");
        const auto hostSeesHit = DecodeOutboxMessagesFor<NotifyHealthChangeBroadcast>(kHostConn);
        Check(hostSeesHit.size() == 1 && hostSeesHit[0].Id == victim && near(hostSeesHit[0].DeltaHealth, -100.0f) &&
                  near(hostSeesHit[0].StaggerMagnitude, 0.6f, 0.02f) && revisionOf(victim) != revisionBeforeHit &&
                  hostSeesHit[0].HealthRevision == revisionOf(victim),
              "the simulating player gets the damage, the stagger order and the new health revision");
        Check(DecodeOutboxMessagesFor<NotifyHealthChangeBroadcast>(kGuestConn).empty(), "the attacker is not sent his own hit back");
        const auto guestAcks = DecodeOutboxMessagesFor<NotifyActorValueChanges>(kGuestConn);
        Check(!guestAcks.empty() && guestAcks.back().Id == victim && guestAcks.back().HealthRevision == revisionOf(victim),
              "the attacker is told the health revision his hit produced");

        Check(DeliverWire(world, guestHit, pGuest) && near(healthOf(victim), 200.0f),
              "the same swing delivered twice costs the bandit once");

        // --- 2. The simulating player's own hit --------------------------------------------------
        Harness::ClearOutbox();
        Check(DeliverWire(world, swing(victim, -50.0f, 0.9f), pHost) && near(healthOf(victim), 150.0f),
              "the simulating player's hit is applied: 200 -> 150");
        const auto guestSeesHit = DecodeOutboxMessagesFor<NotifyHealthChangeBroadcast>(kGuestConn);
        Check(guestSeesHit.size() == 1 && near(guestSeesHit[0].DeltaHealth, -50.0f) && guestSeesHit[0].StaggerMagnitude == 0.0f,
              "it travels without a stagger order: his game staggered the bandit already and the animation carries it");

        // --- 3. The guest kills the bandit; the host's packets are already on their way ----------
        const uint32_t hostRevisionBeforeKill = revisionOf(victim); // what the host's client holds
        Harness::ClearOutbox();
        Check(DeliverWire(world, swing(victim, -150.0f, 0.0f), pGuest) && near(healthOf(victim), 0.0f),
              "the guest's killing blow takes the bandit to 0");
        RequestDeathStateChange death{};
        death.Id = victim;
        death.IsDead = true;
        Check(DeliverWire(world, death, pGuest) && isDead(victim), "and the guest's client reports the death");
        const auto hostSeesDeath = DecodeOutboxMessagesFor<NotifyDeathStateChange>(kHostConn);
        Check(hostSeesDeath.size() == 1 && hostSeesDeath[0].Id == victim && hostSeesDeath[0].IsDead &&
                  hostSeesDeath[0].HealthRevision == revisionOf(victim),
              "the simulating player is told the bandit died, with the health revision of the death");
        Check(revisionOf(victim) != hostRevisionBeforeKill, "the kill moved the health revision past what the host's client holds");

        Harness::ClearOutbox();
        Check(DeliverWire(world, reading(victim, 150.0f, hostRevisionBeforeKill), pHost) && near(healthOf(victim), 0.0f) && isDead(victim),
              "a health reading the simulating player sampled before the kill does not give the body its health back");
        const auto corrections = DecodeOutboxMessagesFor<NotifyActorValueChanges>(kHostConn);
        Check(!corrections.empty() && corrections.back().Id == victim && corrections.back().HealthRevision == revisionOf(victim) &&
                  corrections.back().Values.count(kHealthIndex) == 1 && near(corrections.back().Values.at(kHealthIndex), 0.0f),
              "he is answered with the truth: health 0 and the current revision");
        Check(DecodeOutboxMessagesFor<NotifyActorValueChanges>(kGuestConn).empty(), "and the stale reading is not relayed to the other player");

        Harness::ClearOutbox();
        Check(DeliverWire(world, swing(victim, 5.0f, 0.0f), pHost) && near(healthOf(victim), 0.0f),
              "a regeneration tick that was on its way does not heal a corpse");
        Check(DeliverWire(world, swing(victim, -40.0f, 0.5f), pGuest) && near(healthOf(victim), 0.0f) &&
                  DecodeOutboxMessagesFor<NotifyHealthChangeBroadcast>(kHostConn).empty() &&
                  DecodeOutboxMessagesFor<NotifyHealthChangeBroadcast>(kGuestConn).empty(),
              "a second arrow that lands on the body is not relayed to anybody as damage");
        Check(DeliverWire(world, death, pHost) && isDead(victim) && DecodeOutboxMessagesFor<NotifyDeathStateChange>(kGuestConn).empty(),
              "the host's client noticing the death a moment later changes nothing and is not broadcast again");

        // --- 3b. The body (work plan 5.3): the server is the one writer of what is on it ----------
        // Each game rolls its own death items, so the two players see different things on the same
        // body. What the simulating player's game put there is what the server lists.
        {
            const GameId kSword{0u, 0x00012EB7u};
            const GameId kGold{0u, 0x0000000Fu};
            const GameId kAxe{0u, 0x00013790u};
            const auto stack = [](const GameId& acItem, int32_t aCount) {
                Inventory::Entry entry{};
                entry.BaseId = acItem;
                entry.Count = aCount;
                return entry;
            };
            const auto change = [&](Player* apPlayer, uint32_t aEntity, const GameId& acItem, int32_t aCount) {
                Harness::ClearOutbox();
                RequestInventoryChanges request{};
                request.ServerId = aEntity;
                request.Item = stack(acItem, aCount);
                request.UpdateClients = true;
                return DeliverWire(world, request, apPlayer);
            };
            const auto toldToChange = [](TiltedPhoques::ConnectionId_t aConn, uint32_t aEntityServerId, const GameId& acItem) {
                int32_t total = 0;
                for (const auto& notify : DecodeOutboxMessagesFor<NotifyInventoryChanges>(aConn))
                    if (notify.ServerId == aEntityServerId && notify.Item.BaseId == acItem)
                        total += notify.Item.Count;
                return total;
            };
            const auto onBody = [&](const GameId& acItem) {
                GameId lookup = acItem;
                return world.get<InventoryComponent>(entityOf(victim)).Content.GetEntryCountById(lookup);
            };

            Check(change(pHost, victim, kSword, 1) && toldToChange(kGuestConn, victim, kSword) == 1 && onBody(kSword) == 1,
                  "the death item the simulating player's game rolled is listed by the server and shown to the guest");
            Check(change(pHost, victim, kGold, 25) && onBody(kGold) == 25, "so is the gold");

            Check(change(pGuest, victim, kSword, -1) && onBody(kSword) == 0 && toldToChange(kHostConn, victim, kSword) == -1 &&
                      toldToChange(kGuestConn, guestChar, kSword) == 0,
                  "the take that arrives first is granted, and the other player sees the sword leave the body");
            Check(change(pHost, victim, kSword, -1) && toldToChange(kHostConn, hostChar, kSword) == -1 && onBody(kSword) == 0,
                  "the take that arrives second is refused: that player is told to give the sword back");

            Check(change(pGuest, victim, kGold, -25) && onBody(kGold) == 0, "the guest takes all 25 gold");
            Check(change(pHost, victim, kGold, -10) && toldToChange(kHostConn, hostChar, kGold) == -10,
                  "gold that is no longer there is taken back from the player who was late, to the coin");

            Check(change(pGuest, victim, kAxe, -1) && toldToChange(kGuestConn, guestChar, kAxe) == 0,
                  "an item only the guest's game rolled is granted once (the same rule as for chests)");
            Check(change(pHost, victim, kAxe, -1) && toldToChange(kHostConn, hostChar, kAxe) == -1,
                  "and not a second time to the other player");

            Check(change(pHost, victim, kGold, 7) && onBody(kGold) == 7, "the host puts 7 gold back on the body");
            pGuest->SetCellComponent(CellIdComponent{kFarCell});
            Check(change(pGuest, victim, kGold, -7) && onBody(kGold) == 7 && toldToChange(kHostConn, victim, kGold) == 0,
                  "a player who is not in the cell cannot take from the body");
            pGuest->SetCellComponent(CellIdComponent{kCell});

            // --- M24: the body changes hands while it is being looted ---------------------------
            // The host hands the body over and the server grants it to the guest. Before the
            // guest's confirmation arrives, the host - now just a player standing next to the body
            // - takes the ring and 5 of the 7 gold. The confirmation was built before those takes
            // reached the guest: it lists the ring, 7 gold, and a dagger only the guest's game has.
            const GameId kRing{0u, 0x000877C9u};
            const GameId kDagger{0u, 0x0001397Eu};
            const auto listed = [](const Inventory& acInventory, const GameId& acItem) {
                GameId lookup = acItem;
                return acInventory.GetEntryCountById(lookup);
            };
            Check(change(pHost, victim, kRing, 1) && onBody(kRing) == 1 && onBody(kGold) == 7,
                  "the body holds a ring and 7 gold, and both players' games show them");

            RequestOwnershipTransfer handBody{};
            handBody.ServerId = victim;
            Check(DeliverWire(world, handBody, pHost) && ownerOf(victim) == pGuest, "the host hands the body over: the guest simulates it now");
            Check(change(pHost, victim, kRing, -1) && onBody(kRing) == 0 && change(pHost, victim, kGold, -5) && onBody(kGold) == 2,
                  "before the guest's confirmation arrives the host takes the ring and 5 of the 7 gold");

            RequestOwnershipClaim bodyClaim{};
            bodyClaim.ServerId = victim;
            bodyClaim.NewActorData.BaseId = kBanditBase;
            bodyClaim.NewActorData.IsDead = true;
            bodyClaim.NewActorData.InitialActorValues.ActorValuesList[kHealthIndex] = 0.0f;
            bodyClaim.NewActorData.InitialActorValues.ActorMaxValuesList[kHealthIndex] = 300.0f;
            bodyClaim.NewActorData.InitialInventory.AddOrRemoveEntry(stack(kRing, 1));
            bodyClaim.NewActorData.InitialInventory.AddOrRemoveEntry(stack(kGold, 7));
            bodyClaim.NewActorData.InitialInventory.AddOrRemoveEntry(stack(kDagger, 1));
            Harness::ClearOutbox();
            Check(DeliverWire(world, bodyClaim, pGuest) && ownerOf(victim) == pGuest && isDead(victim),
                  "the guest's confirmation arrives with the list his game showed a moment earlier");
            Check(onBody(kRing) == 0, "the ring the host took does not come back with that list");
            Check(onBody(kGold) == 2, "the gold is what the server counted (2), not what the list says (7)");
            Check(onBody(kDagger) == 1, "an item the server never listed and nobody took is accepted from the new simulator");
            const auto hostBodyUpdates = DecodeOutboxMessagesFor<NotifySpawnData>(kHostConn);
            Check(hostBodyUpdates.size() == 1 && hostBodyUpdates[0].Id == victim &&
                      listed(hostBodyUpdates[0].NewActorData.InitialInventory, kRing) == 0 &&
                      listed(hostBodyUpdates[0].NewActorData.InitialInventory, kGold) == 2 &&
                      listed(hostBodyUpdates[0].NewActorData.InitialInventory, kDagger) == 1,
                  "the host's game is sent the server's list, so the ring does not reappear on the body in front of him");
            Check(change(pGuest, victim, kRing, -1) && toldToChange(kGuestConn, guestChar, kRing) == -1 && onBody(kRing) == 0,
                  "the guest clicking the ring his copy still shows is told to give it back");

            // The switch off: the list of the player who simulates the body replaces the server's.
            bEnableBodyLootAuthority = false;
            Check(DeliverWire(world, bodyClaim, pGuest) && onBody(kRing) == 1 && onBody(kGold) == 7,
                  "switch off: the same confirmation lists the ring and all 7 gold again (4.1.0)");
            bEnableBodyLootAuthority = true;
        }

        // --- 4. Handover in the middle of the fight ---------------------------------------------
        // The host fights the second bandit: it draws its weapon and is wounded to 120 of 300.
        DrawWeaponRequest draw{};
        draw.Id = brawler;
        draw.IsWeaponDrawn = true;
        Harness::ClearOutbox();
        Check(DeliverWire(world, draw, pHost) && world.get<CharacterComponent>(entityOf(brawler)).IsWeaponDrawn(),
              "the bandit draws its weapon in the host's game");
        const auto guestSeesDraw = DecodeOutboxMessagesFor<NotifyDrawWeapon>(kGuestConn);
        Check(guestSeesDraw.size() == 1 && guestSeesDraw[0].Id == brawler && guestSeesDraw[0].IsWeaponDrawn, "the guest is told");
        Check(DeliverWire(world, reading(brawler, 120.0f, revisionOf(brawler)), pHost) && near(healthOf(brawler), 120.0f),
              "the host wounds it to 120 of 300");

        // The host walks out of the cell. Its client lets the bandit go, with the bandit's position
        // and the ownership epoch it holds.
        const uint64_t hostEpoch = epochOf(brawler);
        const uint32_t hostRevisionAtHandoff = revisionOf(brawler);
        pHost->SetCellComponent(CellIdComponent{kFarCell});
        RequestOwnershipTransfer handoff{};
        handoff.ServerId = brawler;
        handoff.CellId = kCell;
        handoff.Position = glm::vec3{210.0f, 205.0f, 0.0f};
        handoff.OwnershipEpoch = hostEpoch;
        Harness::ClearOutbox();
        Check(DeliverWire(world, handoff, pHost) && ownerOf(brawler) == pGuest, "the bandit goes to the guest, who is still in the cell");
        const auto grants = DecodeOutboxMessagesFor<NotifyOwnershipTransfer>(kGuestConn);
        Check(grants.size() == 1 && grants[0].ServerId == brawler && grants[0].OwnershipEpoch > hostEpoch &&
                  grants[0].OwnershipEpoch == epochOf(brawler) && grants[0].HealthRevision == revisionOf(brawler),
              "the guest is told, with a newer ownership epoch and the health revision to quote from now on");
        Check(near(healthOf(brawler), 120.0f) && !isDead(brawler), "the bandit keeps its wounds through the handover");
        Check(world.get<CharacterComponent>(entityOf(brawler)).IsWeaponDrawn(), "and its drawn weapon: for the new simulator it is still in a fight");

        // What the guest's client does with the grant: it takes the bandit over and confirms with
        // the bandit as its own game shows it. Its copy missed the wounds: 300 of 300.
        RequestOwnershipClaim confirm{};
        confirm.ServerId = brawler;
        confirm.NewActorData.BaseId = kBanditBase;
        confirm.NewActorData.InitialActorValues.ActorValuesList[kHealthIndex] = 300.0f;
        confirm.NewActorData.InitialActorValues.ActorMaxValuesList[kHealthIndex] = 300.0f;
        confirm.NewActorData.IsWeaponDrawn = true;
        Harness::ClearOutbox();
        Check(DeliverWire(world, confirm, pGuest) && ownerOf(brawler) == pGuest && !grants.empty() &&
                  epochOf(brawler) == grants[0].OwnershipEpoch,
              "the new simulator's confirmation keeps the epoch it was granted");
        Check(near(healthOf(brawler), 120.0f) && !isDead(brawler), "its copy of the bandit at full health does not heal the bandit");
        const auto confirmAnswers = DecodeOutboxMessagesFor<NotifySpawnData>(kGuestConn);
        Check(confirmAnswers.size() == 1 && confirmAnswers[0].Id == brawler &&
                  confirmAnswers[0].NewActorData.InitialActorValues.ActorValuesList.count(kHealthIndex) == 1 &&
                  near(confirmAnswers[0].NewActorData.InitialActorValues.ActorValuesList.at(kHealthIndex), 120.0f) &&
                  confirmAnswers[0].HealthRevision == revisionOf(brawler),
              "the new simulator is told the real health (120) and the revision that goes with it");
        Check(DecodeOutboxMessagesFor<NotifyOwnershipTransfer>(kGuestConn).empty(), "and is not granted the bandit a second time");
        // The health revision the guest's client holds from here on.
        const uint32_t guestRevision = !confirmAnswers.empty() ? confirmAnswers[0].HealthRevision
                                                               : (grants.empty() ? 0u : grants[0].HealthRevision);

        // Packets the host's client had sent before it learned that it let go.
        Harness::ClearOutbox();
        Check(DeliverWire(world, reading(brawler, 300.0f, hostRevisionAtHandoff), pHost) && near(healthOf(brawler), 120.0f),
              "a health reading from the previous simulator is ignored: the bandit is not healed");
        draw.IsWeaponDrawn = false;
        Check(DeliverWire(world, draw, pHost) && world.get<CharacterComponent>(entityOf(brawler)).IsWeaponDrawn() &&
                  DecodeOutboxMessagesFor<NotifyDrawWeapon>(kGuestConn).empty(),
              "so is its 'weapon sheathed': the bandit does not leave the fight in the guest's game");
        Check(DeliverWire(world, handoff, pHost) && ownerOf(brawler) == pGuest && epochOf(brawler) == grants[0].OwnershipEpoch,
              "a repeated handoff quoting the old epoch does not take the bandit from the guest");
        const auto refusals = DecodeOutboxMessagesFor<NotifyRelinquishControl>(kHostConn);
        Check(!refusals.empty() && refusals.back().ServerId == brawler && refusals.back().OwnershipEpoch == epochOf(brawler),
              "the previous simulator is answered with the epoch the server holds");
        RequestEquipmentChanges lateEquip{};
        lateEquip.ServerId = brawler;
        lateEquip.ItemId = GameId{0u, 0x00012EB7u};
        lateEquip.Count = 1;
        lateEquip.Unequip = true;
        Check(DeliverWire(world, lateEquip, pHost) && DecodeOutboxMessagesFor<NotifyEquipmentChanges>(kGuestConn).empty(),
              "nor does its 'weapon unequipped' reach the guest's game");
        RequestDeathStateChange farDeath{};
        farDeath.Id = brawler;
        farDeath.IsDead = true;
        Check(DeliverWire(world, farDeath, pHost) && !isDead(brawler), "a death report from a player who left the cell is ignored");

        // The fight goes on with the new simulator...
        Check(DeliverWire(world, reading(brawler, 60.0f, guestRevision), pGuest) && near(healthOf(brawler), 60.0f),
              "the new simulator's health reading is applied: 120 -> 60");

        // ...and the host comes back and joins it as the player who does not simulate the bandit.
        pHost->SetCellComponent(CellIdComponent{kCell});
        Harness::ClearOutbox();
        Check(DeliverWire(world, swing(brawler, -30.0f, 0.4f), pHost) && near(healthOf(brawler), 30.0f),
              "back in the cell, the previous simulator's hit counts like anybody's: 60 -> 30");
        const auto guestOrders = DecodeOutboxMessagesFor<NotifyHealthChangeBroadcast>(kGuestConn);
        Check(guestOrders.size() == 1 && near(guestOrders[0].StaggerMagnitude, 0.4f, 0.02f),
              "and now it is the guest's game that is ordered to stagger the bandit");
        Check(ownerOf(brawler) == pGuest, "hitting the bandit does not move its simulation back to the host");
    }

    // =========================================================================
    // Scene 17: doors, chests, levers and quest stages seen by two players (work plan 5.4).
    // =========================================================================
    //
    // The harness never sent a lock change, an activation, a script animation or a quest stage:
    // this part of the server ran only in the game. The scene pins who is told what, and covers
    // defect M23 - an unlock that reaches the server before anybody has registered the object.
    {
        std::printf("[scene 17] doors, chests, levers and quest stages for two players\n");
        std::filesystem::remove(SaveSnapshotService::GetDefaultCheckpointStorePath(), removeEc);

        GameServer server{registry};
        World& world = server.GetWorld();

        constexpr TiltedPhoques::ConnectionId_t kHostConn = 0xF201u;
        constexpr TiltedPhoques::ConnectionId_t kGuestConn = 0xF202u;
        constexpr TiltedPhoques::ConnectionId_t kLateConn = 0xF203u;
        const GameId kPlayerRef{0u, 0x14u};
        const GameId kTamriel{0u, 0x0000003Cu};
        // Two neighbouring exterior cells and one far away (outside the 5x5 loaded grid).
        const GameId kCellA{0u, 0x00009001u};
        const GameId kCellB{0u, 0x00009002u};
        const GameId kCellFar{0u, 0x00009999u};
        const CellIdComponent inA{kCellA, kTamriel, GridCellCoords{10, 10}};
        const CellIdComponent inB{kCellB, kTamriel, GridCellCoords{11, 10}};
        const CellIdComponent farAway{kCellFar, kTamriel, GridCellCoords{40, 40}};
        constexpr uint32_t kHealthIndex = ActorValuesComponent::kHealthActorValueIndex;

        Player* pHost = world.GetPlayerManager().Create(kHostConn);
        Player* pGuest = world.GetPlayerManager().Create(kGuestConn);
        TimeModel t0{};
        t0.Time = 12.0f;
        t0.Day = 5;
        t0.Month = 5;
        t0.Year = 201;
        world.GetDispatcher().trigger(PlayerJoinEvent{pHost, {}, {}, DateTime{t0}});
        world.GetDispatcher().trigger(PlayerJoinEvent{pGuest, {}, {}, DateTime{t0}});
        pHost->SetCellComponent(inA);
        pGuest->SetCellComponent(inA);

        uint32_t nextCookie = 1700u;
        const auto enterWorld = [&](Player* apPlayer, TiltedPhoques::ConnectionId_t aConn, const QuestLog& acQuests) {
            Harness::ClearOutbox();
            AssignCharacterRequest request{};
            request.Cookie = nextCookie++;
            request.ReferenceId = kPlayerRef;
            request.CellId = kCellA;
            request.WorldSpaceId = kTamriel;
            request.Position = glm::vec3{10.0f * 4096.0f + 200.0f, 10.0f * 4096.0f + 200.0f, 0.0f};
            request.QuestContent = acQuests;
            request.CurrentActorData.InitialActorValues.ActorValuesList[kHealthIndex] = 300.0f;
            request.CurrentActorData.InitialActorValues.ActorMaxValuesList[kHealthIndex] = 300.0f;
            (void)DeliverWire(world, request, apPlayer);
            const auto responses = DecodeOutboxMessagesFor<AssignCharacterResponse>(aConn);
            return responses.size() == 1 ? responses[0].ServerId : 0xFFFFFFFFu;
        };
        const uint32_t hostChar = enterWorld(pHost, kHostConn, QuestLog{});
        const uint32_t guestChar = enterWorld(pGuest, kGuestConn, QuestLog{});
        Check(hostChar != 0xFFFFFFFFu && guestChar != 0xFFFFFFFFu && hostChar != guestChar, "two players are in the world");

        // ---- what a client sends ------------------------------------------------------------
        const auto object = [&](const GameId& acId, const CellIdComponent& acWhere, bool aLocked, uint8_t aLevel) {
            ObjectData data{};
            data.Id = acId;
            data.CellId = acWhere.Cell;
            data.WorldSpaceId = acWhere.WorldSpaceId;
            data.CurrentCoords = acWhere.CenterCoords;
            data.CurrentLockData.IsLocked = aLocked;
            data.CurrentLockData.LockLevel = aLevel;
            return data;
        };
        // A client registers the doors and containers of the cell its player has just entered.
        const auto registerObjects = [&](Player* apPlayer, TiltedPhoques::ConnectionId_t aConn, const std::vector<ObjectData>& acObjects) {
            Harness::ClearOutbox();
            AssignObjectsRequest request{};
            for (const ObjectData& data : acObjects)
                request.Objects.push_back(data);
            (void)DeliverWire(world, request, apPlayer);
            std::vector<ObjectData> answered;
            for (const auto& response : DecodeOutboxMessagesFor<AssignObjectsResponse>(aConn))
            {
                for (const ObjectData& data : response.Objects)
                    answered.push_back(data);
            }
            return answered;
        };
        const auto lockChange = [](const GameId& acId, const GameId& acCell, bool aLocked, uint8_t aLevel) {
            LockChangeRequest request{};
            request.Id = acId;
            request.CellId = acCell;
            request.IsLocked = aLocked;
            request.LockLevel = aLevel;
            return request;
        };
        // What the server holds for an object: nothing until somebody registers it.
        const auto lockOf = [&](const GameId& acId) -> std::optional<LockData> {
            const auto view = world.view<FormIdComponent, ObjectComponent>();
            for (const entt::entity entity : view)
            {
                if (view.get<FormIdComponent>(entity).Id == acId)
                    return view.get<ObjectComponent>(entity).CurrentLockData;
            }
            return std::nullopt;
        };
        const auto isUnlocked = [&](const GameId& acId) {
            const auto lock = lockOf(acId);
            return lock.has_value() && !lock->IsLocked;
        };
        const auto isLocked = [&](const GameId& acId) {
            const auto lock = lockOf(acId);
            return lock.has_value() && lock->IsLocked;
        };
        const auto unlockNoticesFor = [&](TiltedPhoques::ConnectionId_t aConn, const GameId& acId) {
            size_t count = 0;
            for (const auto& notice : DecodeOutboxMessagesFor<NotifyLockChange>(aConn))
            {
                if (notice.Id == acId && !notice.IsLocked)
                    ++count;
            }
            return count;
        };

        const GameId kLockpick{0u, 0x0000000Au};
        const GameId kGold{0u, 0x0000000Fu};
        const auto stack = [](const GameId& acItem, int32_t aCount) {
            Inventory::Entry entry{};
            entry.BaseId = acItem;
            entry.Count = aCount;
            return entry;
        };
        const auto countIn = [](const Inventory& acInventory, const GameId& acItem) {
            GameId lookup = acItem;
            return acInventory.GetEntryCountById(lookup);
        };

        // --- 1. Registration: the first player's game decides what the object is ----------------
        const GameId kGate{0u, 0x000A0001u};
        const GameId kChest{0u, 0x000A0002u};
        ObjectData hostChest = object(kChest, inA, true, 25);
        hostChest.CurrentInventory.AddOrRemoveEntry(stack(kLockpick, 3));
        const auto hostRegistered = registerObjects(pHost, kHostConn, {object(kGate, inA, true, 50), hostChest});
        Check(hostRegistered.size() == 2 && hostRegistered[0].IsSenderFirst && hostRegistered[1].IsSenderFirst &&
                  hostRegistered[0].ServerId != hostRegistered[1].ServerId,
              "the first player to enter a cell registers its gate and its chest as his game shows them");
        Check(isLocked(kGate) && isLocked(kChest), "and the server holds both as locked");

        // The guest's game rolled other chest contents: 40 gold instead of 3 lockpicks.
        ObjectData guestChest = object(kChest, inA, true, 25);
        guestChest.CurrentInventory.AddOrRemoveEntry(stack(kGold, 40));
        const auto guestRegistered = registerObjects(pGuest, kGuestConn, {object(kGate, inA, true, 50), guestChest});
        Check(guestRegistered.size() == 2 && !guestRegistered[0].IsSenderFirst && !guestRegistered[1].IsSenderFirst &&
                  hostRegistered.size() == 2 && guestRegistered[0].ServerId == hostRegistered[0].ServerId &&
                  guestRegistered[1].ServerId == hostRegistered[1].ServerId,
              "the second player gets the same objects back, not objects of his own");
        Check(guestRegistered.size() == 2 && guestRegistered[1].CurrentLockData.IsLocked &&
                  countIn(guestRegistered[1].CurrentInventory, kLockpick) == 3 && countIn(guestRegistered[1].CurrentInventory, kGold) == 0,
              "with the first player's chest contents: 3 lockpicks, not the 40 gold his own game rolled");

        // --- 2. A lock picked in the cell both players stand in ---------------------------------
        Harness::ClearOutbox();
        Check(DeliverWire(world, lockChange(kGate, kCellA, false, 50), pHost) && isUnlocked(kGate),
              "the host picks the gate's lock: the server holds it as unlocked");
        Check(unlockNoticesFor(kGuestConn, kGate) == 1, "the guest standing next to him is told once");
        Check(DecodeOutboxMessagesFor<NotifyLockChange>(kHostConn).empty(), "the host is not told about his own lockpicking");
        // The guest's game unlocks the gate and, as for any lock change, reports it.
        Check(DeliverWire(world, lockChange(kGate, kCellA, false, 50), pGuest) && isUnlocked(kGate),
              "the guest's game reporting the same unlock changes nothing");

        // The guest walks away; the host locks the gate again with its key.
        pGuest->SetCellComponent(farAway);
        Harness::ClearOutbox();
        Check(DeliverWire(world, lockChange(kGate, kCellA, true, 50), pHost) && isLocked(kGate), "the host locks the gate again with its key");
        Check(DecodeOutboxMessagesFor<NotifyLockChange>(kGuestConn).empty(), "a player far away is not told");
        // He comes back: his game still shows the gate unlocked, and registers it that way.
        pGuest->SetCellComponent(inA);
        const auto guestBack = registerObjects(pGuest, kGuestConn, {object(kGate, inA, false, 50)});
        Check(guestBack.size() == 1 && !guestBack[0].IsSenderFirst && guestBack[0].CurrentLockData.IsLocked &&
                  guestBack[0].CurrentLockData.LockLevel == 50 && isLocked(kGate),
              "coming back he is told the gate is locked: what the server holds wins over what his game remembers");

        // --- 3. M23: a lock picked across a cell border, before anybody registered the object ---
        // The host stands in cell A and picks the lock of a house door that belongs to cell B.
        // Nobody has been in cell B, so the server has no record of the door. The guest is away.
        const GameId kHouseDoor{0u, 0x000B0001u};
        pGuest->SetCellComponent(farAway);
        Harness::ClearOutbox();
        Check(DeliverWire(world, lockChange(kHouseDoor, kCellB, false, 75), pHost) && !lockOf(kHouseDoor).has_value(),
              "an unlock of a door nobody registered creates no object on the server");
        Check(DecodeOutboxMessagesFor<NotifyLockChange>(kGuestConn).empty(), "and the guest, far away, hears nothing of it");

        // The guest is the first to walk into cell B. His game shows the door locked.
        pGuest->SetCellComponent(inB);
        const auto guestHouse = registerObjects(pGuest, kGuestConn, {object(kHouseDoor, inB, true, 75)});
        Check(guestHouse.size() == 1 && guestHouse[0].IsSenderFirst, "the guest registers the house door first");
        Check(isUnlocked(kHouseDoor), "the server holds it as unlocked: the host picked that lock");
        const auto guestUnlocks = DecodeOutboxMessagesFor<NotifyLockChange>(kGuestConn);
        Check(guestUnlocks.size() == 1 && guestUnlocks[0].Id == kHouseDoor && !guestUnlocks[0].IsLocked && guestUnlocks[0].LockLevel == 75,
              "and the guest is told to unlock it, once");
        Check(DecodeOutboxMessagesFor<NotifyLockChange>(kHostConn).empty(), "the host is not told again");
        // The guest's game unlocks the door and reports the change.
        Check(DeliverWire(world, lockChange(kHouseDoor, kCellB, false, 75), pGuest) && isUnlocked(kHouseDoor),
              "the guest's game reporting that unlock leaves the door unlocked");

        // The host walks in. In 4.1.0 he was answered "locked" and the door he picked locked again.
        pHost->SetCellComponent(inB);
        const auto hostHouse = registerObjects(pHost, kHostConn, {object(kHouseDoor, inB, false, 75)});
        Check(hostHouse.size() == 1 && !hostHouse[0].IsSenderFirst && !hostHouse[0].CurrentLockData.IsLocked,
              "the host, entering later, is answered 'unlocked': the door he picked does not lock again");
        Check(unlockNoticesFor(kHostConn, kHouseDoor) == 0, "and needs no extra unlock notice");

        // A second registration of the same door finds nothing left to remember.
        pGuest->SetCellComponent(farAway);
        pGuest->SetCellComponent(inB);
        const auto guestHouseAgain = registerObjects(pGuest, kGuestConn, {object(kHouseDoor, inB, false, 75)});
        Check(guestHouseAgain.size() == 1 && !guestHouseAgain[0].IsSenderFirst && unlockNoticesFor(kGuestConn, kHouseDoor) == 0,
              "registering the door again later brings no second unlock notice");

        // The player who picked the lock is also the first to register the door: nothing to tell.
        const GameId kBarnDoor{0u, 0x000B0002u};
        pHost->SetCellComponent(inA);
        pGuest->SetCellComponent(farAway);
        (void)DeliverWire(world, lockChange(kBarnDoor, kCellB, false, 25), pHost);
        pHost->SetCellComponent(inB);
        const auto hostBarn = registerObjects(pHost, kHostConn, {object(kBarnDoor, inB, false, 25)});
        Check(hostBarn.size() == 1 && hostBarn[0].IsSenderFirst && isUnlocked(kBarnDoor) && unlockNoticesFor(kHostConn, kBarnDoor) == 0,
              "the picker registering his own unlocked door first is told nothing");
        pGuest->SetCellComponent(inB);
        const auto guestBarn = registerObjects(pGuest, kGuestConn, {object(kBarnDoor, inB, true, 25)});
        Check(guestBarn.size() == 1 && !guestBarn[0].IsSenderFirst && !guestBarn[0].CurrentLockData.IsLocked,
              "and the guest, registering it locked afterwards, is answered 'unlocked'");

        // Only unlocks are kept. "Locked" for an unknown object is not: an arena gate closes behind
        // the party, a script opens it again without a report, and a player arriving later must
        // not be locked out by a stale record.
        const GameId kArenaGate{0u, 0x000B0003u};
        pHost->SetCellComponent(inA);
        pGuest->SetCellComponent(farAway);
        (void)DeliverWire(world, lockChange(kArenaGate, kCellB, true, 100), pHost);
        pGuest->SetCellComponent(inB);
        const auto guestArena = registerObjects(pGuest, kGuestConn, {object(kArenaGate, inB, false, 0)});
        Check(guestArena.size() == 1 && guestArena[0].IsSenderFirst && isUnlocked(kArenaGate) &&
                  DecodeOutboxMessagesFor<NotifyLockChange>(kGuestConn).empty(),
              "a 'locked' report for an unregistered gate is not kept: the first registration decides, as before");

        // Unlocked and locked again before anybody registered it: the unlock is cancelled.
        const GameId kShedDoor{0u, 0x000B0004u};
        pGuest->SetCellComponent(farAway);
        (void)DeliverWire(world, lockChange(kShedDoor, kCellB, false, 50), pHost);
        (void)DeliverWire(world, lockChange(kShedDoor, kCellB, true, 50), pHost);
        pGuest->SetCellComponent(inB);
        const auto guestShed = registerObjects(pGuest, kGuestConn, {object(kShedDoor, inB, true, 50)});
        Check(guestShed.size() == 1 && guestShed[0].IsSenderFirst && isLocked(kShedDoor) && unlockNoticesFor(kGuestConn, kShedDoor) == 0,
              "a door unlocked and locked again before its registration stays locked");

        // The switch off: as in 4.1.0 the unlock is forgotten and the first registration decides.
        const GameId kCellarDoor{0u, 0x000B0005u};
        bEnableUnlockMemory = false;
        pGuest->SetCellComponent(farAway);
        (void)DeliverWire(world, lockChange(kCellarDoor, kCellB, false, 50), pHost);
        pGuest->SetCellComponent(inB);
        const auto guestCellar = registerObjects(pGuest, kGuestConn, {object(kCellarDoor, inB, true, 50)});
        Check(guestCellar.size() == 1 && isLocked(kCellarDoor) && unlockNoticesFor(kGuestConn, kCellarDoor) == 0,
              "switch off: the guest's registration makes the picked door locked again (4.1.0)");
        pHost->SetCellComponent(inB);
        const auto hostCellar = registerObjects(pHost, kHostConn, {object(kCellarDoor, inB, false, 50)});
        Check(hostCellar.size() == 1 && hostCellar[0].CurrentLockData.IsLocked,
              "switch off: and the host is answered 'locked' for the door he picked (4.1.0)");
        bEnableUnlockMemory = true;

        // --- 4. Activations: relayed to the players in range, with the state before them --------
        pHost->SetCellComponent(inA);
        pGuest->SetCellComponent(inA);
        Harness::ClearOutbox();
        ActivateRequest openGate{};
        openGate.Id = kGate;
        openGate.CellId = kCellA;
        openGate.ActivatorId = hostChar;
        openGate.PreActivationOpenState = 3; // closed
        Check(DeliverWire(world, openGate, pHost), "the host opens the gate");
        const auto guestSeesGate = DecodeOutboxMessagesFor<NotifyActivate>(kGuestConn);
        Check(guestSeesGate.size() == 1 && guestSeesGate[0].Id == kGate && guestSeesGate[0].ActivatorId == hostChar &&
                  guestSeesGate[0].PreActivationOpenState == 3,
              "the guest is told who opened which gate and that it was closed before");
        Check(DecodeOutboxMessagesFor<NotifyActivate>(kHostConn).empty(), "the host is not sent his own activation");

        pGuest->SetCellComponent(farAway);
        Harness::ClearOutbox();
        ActivateRequest closeGate = openGate;
        closeGate.PreActivationOpenState = 1; // open
        Check(DeliverWire(world, closeGate, pHost) && DecodeOutboxMessagesFor<NotifyActivate>(kGuestConn).empty(),
              "an activation is not sent to a player far away");
        (void)DeliverWire(world, openGate, pHost); // the gate stands open when the guest returns
        pGuest->SetCellComponent(inA);
        const auto guestReturns = registerObjects(pGuest, kGuestConn, {object(kGate, inA, true, 50)});
        Check(guestReturns.size() == 1 && DecodeOutboxMessagesFor<NotifyActivate>(kGuestConn).empty(),
              "known limit: the server keeps no open/closed state, a returning player is not told the gate stands open");

        // --- 5. Script animations (levers, puzzle pillars): relayed in range --------------------
        Harness::ClearOutbox();
        ScriptAnimationRequest lever{};
        lever.FormID = 0x000A0003u; // plugin 0, form A0003: the way the client packs it
        lever.Animation = TiltedPhoques::String("PullLever");
        Check(DeliverWire(world, lever, pHost), "the host's game plays a lever animation");
        const auto guestSeesLever = DecodeOutboxMessagesFor<NotifyScriptAnimation>(kGuestConn);
        Check(guestSeesLever.size() == 1 && guestSeesLever[0].FormID == 0x000A0003u && guestSeesLever[0].Animation == "PullLever" &&
                  guestSeesLever[0].EventName.empty(),
              "the guest's game is told to play it on the same lever");
        Check(DecodeOutboxMessagesFor<NotifyScriptAnimation>(kHostConn).empty(), "the host is not sent his own animation");
        pGuest->SetCellComponent(farAway);
        Harness::ClearOutbox();
        Check(DeliverWire(world, lever, pHost) && DecodeOutboxMessagesFor<NotifyScriptAnimation>(kGuestConn).empty(),
              "a script animation is not sent to a player far away");
        pGuest->SetCellComponent(inA);
        Harness::ClearOutbox();
        ScriptAnimationRequest nothing{};
        nothing.FormID = 0x000A0003u;
        Check(DeliverWire(world, nothing, pHost) && DecodeOutboxMessagesFor<NotifyScriptAnimation>(kGuestConn).empty(),
              "a request naming neither an animation nor an event is not relayed");

        // --- 6. Quest stages: the party is one quest log -----------------------------------------
        const GameId kQuest{0u, 0x000C0001u};
        const GameId kOwnQuest{0u, 0x000C0002u};
        constexpr uint8_t kMainQuest = 1;
        const auto quest = [](const GameId& acId, uint16_t aStage, RequestQuestUpdate::StatusCode aStatus, uint8_t aType) {
            RequestQuestUpdate update{};
            update.Id = acId;
            update.Stage = aStage;
            update.Status = aStatus;
            update.ClientQuestType = aType;
            return update;
        };
        const auto stageOf = [](Player* apPlayer, const GameId& acId) -> int {
            for (const auto& entry : apPlayer->GetQuestLogComponent().QuestContent.Entries)
            {
                if (entry.Id == acId)
                    return static_cast<int>(entry.Stage);
            }
            return -1;
        };
        const auto questNoticesFor = [&](TiltedPhoques::ConnectionId_t aConn, const GameId& acId) {
            std::vector<NotifyQuestUpdate> notices;
            for (const auto& notice : DecodeOutboxMessagesFor<NotifyQuestUpdate>(aConn))
            {
                if (notice.Id == acId)
                    notices.push_back(notice);
            }
            return notices;
        };

        // The guest stands far away: quest progress does not depend on where a player is.
        pGuest->SetCellComponent(farAway);
        Harness::ClearOutbox();
        Check(DeliverWire(world, quest(kQuest, 10, RequestQuestUpdate::Started, kMainQuest), pHost), "the host starts a quest");
        auto guestQuest = questNoticesFor(kGuestConn, kQuest);
        Check(guestQuest.size() == 1 && guestQuest[0].Status == NotifyQuestUpdate::Started && guestQuest[0].Stage == 10 &&
                  guestQuest[0].ClientQuestType == kMainQuest,
              "the guest is told the quest started, wherever he is");
        Check(questNoticesFor(kHostConn, kQuest).empty() && stageOf(pHost, kQuest) == 10 && stageOf(pGuest, kQuest) == 10,
              "the host is not, and the server's log shows the quest at stage 10 for both");

        Harness::ClearOutbox();
        Check(DeliverWire(world, quest(kQuest, 20, RequestQuestUpdate::StageUpdate, kMainQuest), pHost), "the host advances it to stage 20");
        guestQuest = questNoticesFor(kGuestConn, kQuest);
        Check(guestQuest.size() == 1 && guestQuest[0].Status == NotifyQuestUpdate::StageUpdate && guestQuest[0].Stage == 20,
              "the guest is told stage 20, once");

        // The guest's game reached stage 20 by itself as well (both saw the same trigger): its
        // report crosses the server's notice.
        Harness::ClearOutbox();
        Check(DeliverWire(world, quest(kQuest, 20, RequestQuestUpdate::StageUpdate, kMainQuest), pGuest) &&
                  questNoticesFor(kHostConn, kQuest).empty() && questNoticesFor(kGuestConn, kQuest).empty(),
              "the guest reporting the stage it was just told does not bounce back to anybody");

        // A stage advanced by the player who does not lead the party.
        Harness::ClearOutbox();
        Check(DeliverWire(world, quest(kQuest, 30, RequestQuestUpdate::StageUpdate, kMainQuest), pGuest), "the guest pulls the lever: stage 30");
        const auto hostQuest = questNoticesFor(kHostConn, kQuest);
        Check(hostQuest.size() == 1 && hostQuest[0].Stage == 30 && hostQuest[0].Status == NotifyQuestUpdate::StageUpdate,
              "the party leader is told stage 30");
        Check(questNoticesFor(kGuestConn, kQuest).empty() && stageOf(pHost, kQuest) == 30 && stageOf(pGuest, kQuest) == 30,
              "the guest is not, and both stand at stage 30");
        Harness::ClearOutbox();
        Check(DeliverWire(world, quest(kQuest, 30, RequestQuestUpdate::StageUpdate, kMainQuest), pGuest) &&
                  questNoticesFor(kHostConn, kQuest).empty(),
              "the same stage reported twice by the same player is relayed once");

        // A third player joins with an older save: the shared quest at stage 10, and a quest of
        // his own that the party does not have.
        Player* pLate = world.GetPlayerManager().Create(kLateConn);
        world.GetDispatcher().trigger(PlayerJoinEvent{pLate, {}, {}, DateTime{t0}});
        pLate->SetCellComponent(inA);
        QuestLog lateLog{};
        lateLog.Entries.emplace_back(kQuest, uint16_t{10});
        lateLog.Entries.emplace_back(kOwnQuest, uint16_t{5});
        const uint32_t lateChar = enterWorld(pLate, kLateConn, lateLog);
        const auto lateQuest = questNoticesFor(kLateConn, kQuest);
        Check(lateChar != 0xFFFFFFFFu && lateQuest.size() == 1 && lateQuest[0].Stage == 30 && lateQuest[0].Status == NotifyQuestUpdate::StageUpdate &&
                  stageOf(pLate, kQuest) == 30,
              "a player joining with the quest at stage 10 is brought to the party's stage 30, once");
        Check(questNoticesFor(kLateConn, kOwnQuest).empty() && questNoticesFor(kHostConn, kOwnQuest).empty() &&
                  questNoticesFor(kGuestConn, kOwnQuest).empty() && stageOf(pHost, kOwnQuest) == -1,
              "his own quest stays his own: joining does not push it into the party's games");

        Harness::ClearOutbox();
        Check(DeliverWire(world, quest(kQuest, 40, RequestQuestUpdate::StageUpdate, kMainQuest), pHost) &&
                  questNoticesFor(kGuestConn, kQuest).size() == 1 && questNoticesFor(kLateConn, kQuest).size() == 1,
              "the next stage reaches both other players");

        Harness::ClearOutbox();
        Check(DeliverWire(world, quest(kQuest, 40, RequestQuestUpdate::Stopped, kMainQuest), pHost), "the host's game ends the quest");
        const auto guestStop = questNoticesFor(kGuestConn, kQuest);
        const auto lateStop = questNoticesFor(kLateConn, kQuest);
        Check(guestStop.size() == 1 && guestStop[0].Status == NotifyQuestUpdate::Stopped && lateStop.size() == 1 &&
                  lateStop[0].Status == NotifyQuestUpdate::Stopped,
              "both other players are told the quest stopped");
        Check(stageOf(pHost, kQuest) == -1 && stageOf(pGuest, kQuest) == -1 && stageOf(pLate, kQuest) == -1,
              "and it leaves the server's log of every player");

        // --- 7. A rollback: unlocks made after the checkpoint never happened --------------------
        auto& saves = world.GetSaveSnapshotService();
        Check(saves.TriggerCoordinatedSave(true, nullptr), "a checkpoint save starts");
        if (const auto pending = saves.GetCoordinator().GetPendingBarrier(); pending.has_value())
        {
            CoordinatedSaveAck ack{};
            ack.EpochId = pending->EpochId;
            ack.SlotName = String(pending->SlotName.c_str());
            ack.Success = true;
            for (Player* pPlayer : {pHost, pGuest, pLate})
                Deliver(world, ack, pPlayer);
        }
        Check(saves.GetSnapshotCount() > 0, "the checkpoint is recorded");

        // After the checkpoint the host picks one more lock across the border.
        const GameId kWellDoor{0u, 0x000B0006u};
        pHost->SetCellComponent(inA);
        pGuest->SetCellComponent(farAway);
        (void)DeliverWire(world, lockChange(kWellDoor, kCellB, false, 50), pHost);

        Check(saves.TriggerRollbackToLatest(false), "the party rolls back to the checkpoint");
        Check(isUnlocked(kHouseDoor) && isLocked(kGate), "locks registered before the checkpoint come back as the checkpoint held them");
        // Every game reloads the checkpoint's save, in which the well door is locked.
        pGuest->SetCellComponent(inB);
        const auto guestWell = registerObjects(pGuest, kGuestConn, {object(kWellDoor, inB, true, 50)});
        Check(guestWell.size() == 1 && guestWell[0].IsSenderFirst && isLocked(kWellDoor) && unlockNoticesFor(kGuestConn, kWellDoor) == 0,
              "an unlock made after the checkpoint is forgotten by the rollback");
    }

    // =========================================================================
    // Scene 18: movement, animation events and load doors for two players (work plan 5.5).
    // =========================================================================
    //
    // The harness never looked at what a movement report turns into for the other player, and
    // never sent the packet a client sends when its player walks through a load door. The scene
    // pins both: who is sent an NPC's movement and animation events, what a player who arrives
    // later is told about the NPC, and who simulates it when a player or the NPC changes cell.
    {
        std::printf("[scene 18] movement, animation events and load doors for two players\n");
        std::filesystem::remove(SaveSnapshotService::GetDefaultCheckpointStorePath(), removeEc);

        GameServer server{registry};
        World& world = server.GetWorld();

        constexpr TiltedPhoques::ConnectionId_t kHostConn = 0xF301u;
        constexpr TiltedPhoques::ConnectionId_t kGuestConn = 0xF302u;
        const GameId kPlayerRef{0u, 0x14u};
        const GameId kHall{0u, 0x000E0001u};
        const GameId kCellar{0u, 0x000E0002u};
        const GameId kGuardBase{0u, 0x000E1001u};
        constexpr uint32_t kHealthIndex = ActorValuesComponent::kHealthActorValueIndex;

        Player* pHost = world.GetPlayerManager().Create(kHostConn);
        Player* pGuest = world.GetPlayerManager().Create(kGuestConn);
        TimeModel t0{};
        t0.Time = 12.0f;
        t0.Day = 5;
        t0.Month = 5;
        t0.Year = 201;
        world.GetDispatcher().trigger(PlayerJoinEvent{pHost, {}, {}, DateTime{t0}});
        world.GetDispatcher().trigger(PlayerJoinEvent{pGuest, {}, {}, DateTime{t0}});

        // A player walks through a load door: the packet his client sends.
        const auto walkInto = [&](Player* apPlayer, const GameId& acCell) {
            Harness::ClearOutbox();
            EnterInteriorCellRequest request{};
            request.CellId = acCell;
            return DeliverWire(world, request, apPlayer);
        };
        Check(walkInto(pHost, kHall) && walkInto(pGuest, kHall) && pHost->GetCellComponent().Cell == kHall &&
                  pGuest->GetCellComponent().Cell == kHall,
              "both players enter the hall");

        uint32_t nextCookie = 1800u;
        const auto assign = [&](Player* apOwner, TiltedPhoques::ConnectionId_t aConn, const GameId& acRefId) {
            Harness::ClearOutbox();
            AssignCharacterRequest request{};
            request.Cookie = nextCookie++;
            request.ReferenceId = acRefId;
            if (acRefId != kPlayerRef)
                request.FormId = kGuardBase;
            request.CellId = kHall;
            request.Position = glm::vec3{100.0f, 100.0f, 0.0f};
            request.CurrentActorData.BaseId = request.FormId;
            request.CurrentActorData.InitialActorValues.ActorValuesList[kHealthIndex] = 100.0f;
            request.CurrentActorData.InitialActorValues.ActorMaxValuesList[kHealthIndex] = 100.0f;
            (void)DeliverWire(world, request, apOwner);
            const auto responses = DecodeOutboxMessagesFor<AssignCharacterResponse>(aConn);
            return responses.size() == 1 ? responses[0].ServerId : 0xFFFFFFFFu;
        };
        const uint32_t hostChar = assign(pHost, kHostConn, kPlayerRef);
        const uint32_t guestChar = assign(pGuest, kGuestConn, kPlayerRef);
        const uint32_t guard = assign(pHost, kHostConn, GameId{0u, 0x000E2001u});
        Check(hostChar != 0xFFFFFFFFu && guestChar != 0xFFFFFFFFu && guard != 0xFFFFFFFFu,
              "two players and a guard simulated by the host are registered");

        const auto entityOf = [](uint32_t aServerId) { return static_cast<entt::entity>(aServerId); };
        const auto ownerOf = [&](uint32_t aServerId) { return world.get<OwnerComponent>(entityOf(aServerId)).GetOwner(); };
        const auto positionOf = [&](uint32_t aServerId) { return glm::vec3(world.get<MovementComponent>(entityOf(aServerId)).Position); };
        const auto cellOf = [&](uint32_t aServerId) { return world.get<CellIdComponent>(entityOf(aServerId)).Cell; };
        const auto at = [](const glm::vec3& acLhs, const glm::vec3& acRhs) { return glm::distance(acLhs, acRhs) < 1.0f; };

        // One movement report, the way a client builds it for an actor it simulates.
        const auto movementOf = [&](uint32_t aServerId, const GameId& acCell, const glm::vec3& acPosition,
                                    std::initializer_list<const char*> aEvents) {
            ClientReferencesMoveRequest request{};
            request.Tick = 5000u;
            ReferenceUpdate update{};
            update.UpdatedMovement.CellId = acCell;
            update.UpdatedMovement.Position = acPosition;
            update.UpdatedMovement.Direction = 0.25f;
            for (const char* pEvent : aEvents)
            {
                ActionEvent action{};
                action.EventName = TiltedPhoques::String(pEvent);
                action.TargetEventName = TiltedPhoques::String(pEvent);
                update.ActionEvents.push_back(action);
            }
            request.Updates[aServerId] = update;
            return request;
        };
        // The update for one actor in what a player was sent, if any.
        const auto relayedTo = [&](TiltedPhoques::ConnectionId_t aConn, uint32_t aServerId) -> std::optional<ReferenceUpdate> {
            std::optional<ReferenceUpdate> found;
            for (const auto& relay : DecodeOutboxMessagesFor<ServerReferencesMoveRequest>(aConn))
            {
                const auto it = relay.Updates.find(aServerId);
                if (it != relay.Updates.end())
                    found = it->second;
            }
            return found;
        };
        const auto spawnedFor = [&](TiltedPhoques::ConnectionId_t aConn, uint32_t aServerId) -> std::optional<CharacterSpawnRequest> {
            for (const auto& spawn : DecodeOutboxMessagesFor<CharacterSpawnRequest>(aConn))
            {
                if (spawn.ServerId == aServerId)
                    return spawn;
            }
            return std::nullopt;
        };
        const auto removedFor = [&](TiltedPhoques::ConnectionId_t aConn, uint32_t aServerId) {
            for (const auto& removal : DecodeOutboxMessagesFor<NotifyRemoveCharacter>(aConn))
            {
                if (removal.ServerId == aServerId)
                    return true;
            }
            return false;
        };
        const auto grantedTo = [&](TiltedPhoques::ConnectionId_t aConn, uint32_t aServerId) {
            for (const auto& grant : DecodeOutboxMessagesFor<NotifyOwnershipTransfer>(aConn))
            {
                if (grant.ServerId == aServerId)
                    return true;
            }
            return false;
        };
        const auto toldToLetGo = [&](TiltedPhoques::ConnectionId_t aConn, uint32_t aServerId) {
            for (const auto& notice : DecodeOutboxMessagesFor<NotifyRelinquishControl>(aConn))
            {
                if (notice.ServerId == aServerId)
                    return true;
            }
            return false;
        };
        const auto replays = [](const CharacterSpawnRequest& acSpawn, const char* apEvent) {
            for (const auto& action : acSpawn.ActionsToReplay.Actions)
            {
                if (action.EventName == apEvent)
                    return true;
            }
            return false;
        };

        // --- 1. Movement and animation events of an NPC ----------------------------------------
        Harness::ClearOutbox();
        ClientReferencesMoveRequest guardWalks = movementOf(guard, kHall, glm::vec3{300.0f, 150.0f, 10.0f}, {"moveStart", "JumpUp"});
        guardWalks.Updates[guard].UpdatedMovement.Variables.Booleans = {true, false, true};
        guardWalks.Updates[guard].UpdatedMovement.Variables.Integers = {7u};
        guardWalks.Updates[guard].UpdatedMovement.Variables.Floats = {0.5f, 2.0f};
        Check(DeliverWire(world, guardWalks, pHost) && at(positionOf(guard), glm::vec3{300.0f, 150.0f, 10.0f}),
              "the host's game moves the guard: the server holds the new position");
        const auto guestSeesWalk = relayedTo(kGuestConn, guard);
        Check(guestSeesWalk.has_value() && at(glm::vec3(guestSeesWalk->UpdatedMovement.Position), glm::vec3{300.0f, 150.0f, 10.0f}) &&
                  std::fabs(guestSeesWalk->UpdatedMovement.Direction - 0.25f) < 0.01f,
              "the guest is sent the guard's position and direction at once");
        Check(guestSeesWalk.has_value() && guestSeesWalk->ActionEvents.size() == 2 && guestSeesWalk->ActionEvents[0].EventName == "moveStart" &&
                  guestSeesWalk->ActionEvents[1].EventName == "JumpUp",
              "with both animation events, in the order they happened");
        Check(guestSeesWalk.has_value() && guestSeesWalk->UpdatedMovement.Variables.Booleans.size() == 3 &&
                  guestSeesWalk->UpdatedMovement.Variables.Booleans[0] && !guestSeesWalk->UpdatedMovement.Variables.Booleans[1] &&
                  guestSeesWalk->UpdatedMovement.Variables.Booleans[2] && guestSeesWalk->UpdatedMovement.Variables.Integers.size() == 1 &&
                  guestSeesWalk->UpdatedMovement.Variables.Integers[0] == 7u && guestSeesWalk->UpdatedMovement.Variables.Floats.size() == 2 &&
                  std::fabs(guestSeesWalk->UpdatedMovement.Variables.Floats[1] - 2.0f) < 0.01f,
              "and the animation variables as they were sent: 3 flags, 1 number, 2 fractions");
        Check(!relayedTo(kHostConn, guard).has_value(), "the host is not sent his own guard's movement back");

        Harness::ClearOutbox();
        Check(DeliverWire(world, movementOf(guard, kHall, glm::vec3{900.0f, 900.0f, 0.0f}, {"attackStart"}), pGuest) &&
                  at(positionOf(guard), glm::vec3{300.0f, 150.0f, 10.0f}) && !relayedTo(kHostConn, guard).has_value(),
              "a movement report from the player who does not simulate the guard is ignored");

        Harness::ClearOutbox();
        ClientReferencesMoveRequest broken = movementOf(guard, kHall, glm::vec3{300.0f, 150.0f, 10.0f}, {});
        broken.Updates[guard].UpdatedMovement.Position = glm::vec3{std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f};
        (void)Deliver(world, broken, pHost);
        ClientReferencesMoveRequest tooFar = movementOf(guard, kHall, glm::vec3{5.0e6f, 0.0f, 0.0f}, {});
        (void)Deliver(world, tooFar, pHost);
        Check(at(positionOf(guard), glm::vec3{300.0f, 150.0f, 10.0f}) && !relayedTo(kGuestConn, guard).has_value(),
              "a position that is not a number, or lies outside any world, is neither stored nor relayed");

        // --- 2. Player avatars ------------------------------------------------------------------
        Harness::ClearOutbox();
        Check(DeliverWire(world, movementOf(hostChar, kHall, glm::vec3{120.0f, 100.0f, 0.0f}, {"SprintStart"}), pHost) &&
                  relayedTo(kGuestConn, hostChar).has_value() && relayedTo(kGuestConn, hostChar)->ActionEvents.size() == 1,
              "the host's own movement and sprint reach the guest");
        Harness::ClearOutbox();
        Check(DeliverWire(world, movementOf(hostChar, kHall, glm::vec3{800.0f, 800.0f, 0.0f}, {}), pGuest) &&
                  at(positionOf(hostChar), glm::vec3{120.0f, 100.0f, 0.0f}) && !relayedTo(kHostConn, hostChar).has_value(),
              "a player cannot move another player's character");

        // --- 3. The guest walks through a load door and comes back ------------------------------
        Check(walkInto(pGuest, kCellar) && removedFor(kGuestConn, guard), "the guest leaves for the cellar: his game is told to remove the guard");
        Check(!removedFor(kHostConn, guestChar) && !DecodeOutboxMessagesFor<NotifyPlayerCellChanged>(kHostConn).empty(),
              "the host is told that the guest changed cell; a player's character is never taken away from a party member");
        // While he is away the guard walks to a chair and sits down.
        Harness::ClearOutbox();
        Check(DeliverWire(world, movementOf(guard, kHall, glm::vec3{400.0f, 200.0f, 0.0f}, {"moveStop", "IdleChairFrontEnter"}), pHost) &&
                  !relayedTo(kGuestConn, guard).has_value() && ownerOf(guard) == pHost,
              "the guard's movement is no longer sent to him, and the guard stays with the host");

        Check(walkInto(pGuest, kHall), "the guest comes back");
        const auto guardForGuest = spawnedFor(kGuestConn, guard);
        Check(guardForGuest.has_value() && at(glm::vec3(guardForGuest->Position), glm::vec3{400.0f, 200.0f, 0.0f}) && guardForGuest->CellId == kHall,
              "he is sent the guard where it is now, not where he last saw it");
        Check(guardForGuest.has_value() && replays(*guardForGuest, "IdleChairEnterInstant") && !replays(*guardForGuest, "IdleChairFrontEnter"),
              "and that it sits: the sit-down is replayed in its instant form, so the guard is not seen sitting down again");
        Check(ownerOf(guard) == pHost && !grantedTo(kGuestConn, guard), "coming back does not hand the guard to the guest");

        // The guard stands up while the guest is away again.
        Check(walkInto(pGuest, kCellar), "the guest leaves once more");
        (void)DeliverWire(world, movementOf(guard, kHall, glm::vec3{400.0f, 200.0f, 0.0f}, {"IdleStop", "moveStart"}), pHost);
        Check(walkInto(pGuest, kHall), "and comes back after the guard stood up");
        const auto standingGuard = spawnedFor(kGuestConn, guard);
        Check(standingGuard.has_value() && standingGuard->ActionsToReplay.ResetAnimationGraph && !replays(*standingGuard, "IdleChairEnterInstant") &&
                  replays(*standingGuard, "moveStart"),
              "he is told to reset the guard's animation and is not shown it sitting");

        // --- 4. The simulating player walks through the door ------------------------------------
        // His client hands over what it leaves behind: a RequestOwnershipTransfer for each actor.
        // For an actor whose cell is already unloaded the request carries no place.
        Check(walkInto(pHost, kCellar) && ownerOf(guard) == pHost, "the host leaves for the cellar; the server waits for his client to let the guard go");
        RequestOwnershipTransfer leaveGuard{};
        leaveGuard.ServerId = guard;
        Check(DeliverWire(world, leaveGuard, pHost) && ownerOf(guard) == pGuest && grantedTo(kGuestConn, guard),
              "the guard he leaves behind goes to the guest, who stays in the hall");
        Check(at(positionOf(guard), glm::vec3{400.0f, 200.0f, 0.0f}) && cellOf(guard) == kHall,
              "a handover without a place leaves the guard where the server last held it");
        Harness::ClearOutbox();
        Check(DeliverWire(world, movementOf(guard, kHall, glm::vec3{50.0f, 50.0f, 0.0f}, {}), pHost) &&
                  at(positionOf(guard), glm::vec3{400.0f, 200.0f, 0.0f}),
              "a late movement report from the previous simulator does not move the guard");
        Check(DeliverWire(world, movementOf(guard, kHall, glm::vec3{450.0f, 220.0f, 0.0f}, {"moveStart"}), pGuest) &&
                  at(positionOf(guard), glm::vec3{450.0f, 220.0f, 0.0f}) && !relayedTo(kHostConn, guard).has_value(),
              "the guest's game moves it now; the host, in another cell, is not sent that");

        // The host comes back. A calm NPC placed in the world returns to the party leader's game.
        Check(walkInto(pHost, kHall), "the host comes back to the hall");
        const auto guardForHost = spawnedFor(kHostConn, guard);
        Check(guardForHost.has_value() && at(glm::vec3(guardForHost->Position), glm::vec3{450.0f, 220.0f, 0.0f}),
              "he is sent the guard where the guest's game left it");
        Check(ownerOf(guard) == pHost && grantedTo(kHostConn, guard) && toldToLetGo(kGuestConn, guard),
              "the calm guard returns to the party leader's game, and the guest's game is told to let it go");

        // In a fight the guard stays with whoever simulates it.
        Check(walkInto(pHost, kCellar), "the host leaves again");
        Check(DeliverWire(world, leaveGuard, pHost) && ownerOf(guard) == pGuest, "the guard goes to the guest again");
        DrawWeaponRequest guardDraws{};
        guardDraws.Id = guard;
        guardDraws.IsWeaponDrawn = true;
        Check(DeliverWire(world, guardDraws, pGuest) && world.get<CharacterComponent>(entityOf(guard)).IsWeaponDrawn(),
              "the guard draws its weapon in the guest's game");
        Check(walkInto(pHost, kHall) && ownerOf(guard) == pGuest && !grantedTo(kHostConn, guard) && !toldToLetGo(kGuestConn, guard) &&
                  spawnedFor(kHostConn, guard).has_value(),
              "the returning leader is shown the guard, but a guard with a drawn weapon stays with the guest");

        // --- 5. The NPC itself goes through the door -------------------------------------------
        // The guest's game reports the guard in the cellar, then lets it go there (the actor left
        // the loaded cell): this handover carries the place.
        Harness::ClearOutbox();
        Check(DeliverWire(world, movementOf(guard, kCellar, glm::vec3{10.0f, 20.0f, 0.0f}, {}), pGuest) && cellOf(guard) == kCellar &&
                  at(positionOf(guard), glm::vec3{10.0f, 20.0f, 0.0f}),
              "the guard runs through the door: the server holds it in the cellar");
        Check(removedFor(kHostConn, guard), "the host, who stays in the hall, is told to remove it");
        RequestOwnershipTransfer guardGone{};
        guardGone.ServerId = guard;
        guardGone.CellId = kCellar;
        guardGone.Position = glm::vec3{12.0f, 24.0f, 0.0f};
        Check(DeliverWire(world, guardGone, pGuest) && ownerOf(guard) == nullptr && cellOf(guard) == kCellar &&
                  at(positionOf(guard), glm::vec3{12.0f, 24.0f, 0.0f}),
              "the guest's game lets it go: nobody is in the cellar, so nobody simulates it, and the server keeps its place");
        Harness::ClearOutbox();
        Check(DeliverWire(world, movementOf(guard, kCellar, glm::vec3{99.0f, 99.0f, 0.0f}, {}), pGuest) &&
                  at(positionOf(guard), glm::vec3{12.0f, 24.0f, 0.0f}),
              "after that its former simulator cannot move it");
        // The host follows it.
        Check(walkInto(pHost, kCellar), "the host follows the guard into the cellar");
        const auto guardInCellar = spawnedFor(kHostConn, guard);
        Check(guardInCellar.has_value() && guardInCellar->CellId == kCellar && at(glm::vec3(guardInCellar->Position), glm::vec3{12.0f, 24.0f, 0.0f}),
              "and is sent the guard in the cellar, at the place its last simulator reported");
        Check(ownerOf(guard) == pHost && grantedTo(kHostConn, guard), "the guard nobody simulated is given to him");
        Check(world.get<CharacterComponent>(entityOf(guard)).IsWeaponDrawn(), "still with its weapon drawn");
    }

    // =========================================================================
    // Scene 19: a player's summon belongs to the summoner (work plan 6.2).
    // =========================================================================
    //
    // Conjuration is a build of its own in RFAB, and no scene had a summon in it. A summon is not
    // a world NPC: it exists in its summoner's game, follows him, and must never change hands. The
    // scene pins every rule of NPC ownership that has an exception for summons, and what happens to
    // a summon when its summoner lets it go or leaves.
    {
        std::printf("[scene 19] a player's summon belongs to the summoner\n");
        std::filesystem::remove(SaveSnapshotService::GetDefaultCheckpointStorePath(), removeEc);

        GameServer server{registry};
        World& world = server.GetWorld();

        constexpr TiltedPhoques::ConnectionId_t kHostConn = 0xF401u;
        constexpr TiltedPhoques::ConnectionId_t kGuestConn = 0xF402u;
        const GameId kPlayerRef{0u, 0x14u};
        const GameId kHall{0u, 0x000F0001u};
        const GameId kCellar{0u, 0x000F0002u};
        const GameId kAtronachBase{0u, 0x000F1001u};
        constexpr uint32_t kTemporary = std::numeric_limits<uint32_t>::max();
        constexpr uint32_t kHealthIndex = ActorValuesComponent::kHealthActorValueIndex;

        Player* pHost = world.GetPlayerManager().Create(kHostConn);
        Player* pGuest = world.GetPlayerManager().Create(kGuestConn);
        TimeModel t0{};
        t0.Time = 12.0f;
        t0.Day = 6;
        t0.Month = 5;
        t0.Year = 201;
        world.GetDispatcher().trigger(PlayerJoinEvent{pHost, {}, {}, DateTime{t0}});
        world.GetDispatcher().trigger(PlayerJoinEvent{pGuest, {}, {}, DateTime{t0}});

        const auto walkInto = [&](Player* apPlayer, const GameId& acCell) {
            Harness::ClearOutbox();
            EnterInteriorCellRequest request{};
            request.CellId = acCell;
            return DeliverWire(world, request, apPlayer);
        };
        Check(walkInto(pHost, kHall) && walkInto(pGuest, kHall), "both players enter the hall");

        uint32_t nextCookie = 1900u;
        uint32_t nextTemporaryRef = 0xFF000A00u;
        std::optional<AssignCharacterResponse> lastAssign;
        // Registers the player's own character (acRefId == kPlayerRef) or a conjured atronach.
        const auto assign = [&](Player* apOwner, TiltedPhoques::ConnectionId_t aConn, bool aSummon) {
            Harness::ClearOutbox();
            AssignCharacterRequest request{};
            request.Cookie = nextCookie++;
            if (aSummon)
            {
                request.ReferenceId = GameId{kTemporary, nextTemporaryRef++};
                request.FormId = kAtronachBase;
                request.IsPlayerSummon = true;
            }
            else
            {
                request.ReferenceId = kPlayerRef;
            }
            request.CellId = kHall;
            request.Position = glm::vec3{200.0f, 200.0f, 0.0f};
            request.CurrentActorData.BaseId = request.FormId;
            request.CurrentActorData.InitialActorValues.ActorValuesList[kHealthIndex] = 100.0f;
            request.CurrentActorData.InitialActorValues.ActorMaxValuesList[kHealthIndex] = 100.0f;
            (void)DeliverWire(world, request, apOwner);
            const auto responses = DecodeOutboxMessagesFor<AssignCharacterResponse>(aConn);
            lastAssign = responses.size() == 1 ? std::optional<AssignCharacterResponse>{responses[0]} : std::nullopt;
            return responses.size() == 1 ? responses[0].ServerId : 0xFFFFFFFFu;
        };
        const uint32_t hostChar = assign(pHost, kHostConn, false);
        const uint32_t guestChar = assign(pGuest, kGuestConn, false);
        Check(hostChar != 0xFFFFFFFFu && guestChar != 0xFFFFFFFFu, "both players' characters are registered");

        const auto entityOf = [](uint32_t aServerId) { return static_cast<entt::entity>(aServerId); };
        const auto exists = [&](uint32_t aServerId) { return world.valid(entityOf(aServerId)); };
        const auto ownerOf = [&](uint32_t aServerId) { return world.get<OwnerComponent>(entityOf(aServerId)).GetOwner(); };
        const auto healthOf = [&](uint32_t aServerId) {
            return world.get<ActorValuesComponent>(entityOf(aServerId)).CurrentActorValues.ActorValuesList[kHealthIndex];
        };
        const auto near = [](float aLhs, float aRhs) { return std::fabs(aLhs - aRhs) < 0.01f; };
        const auto spawnedFor = [&](TiltedPhoques::ConnectionId_t aConn, uint32_t aServerId) -> std::optional<CharacterSpawnRequest> {
            for (const auto& spawn : DecodeOutboxMessagesFor<CharacterSpawnRequest>(aConn))
            {
                if (spawn.ServerId == aServerId)
                    return spawn;
            }
            return std::nullopt;
        };
        const auto removedFor = [&](TiltedPhoques::ConnectionId_t aConn, uint32_t aServerId) {
            for (const auto& removal : DecodeOutboxMessagesFor<NotifyRemoveCharacter>(aConn))
            {
                if (removal.ServerId == aServerId)
                    return true;
            }
            return false;
        };
        const auto grantedTo = [&](TiltedPhoques::ConnectionId_t aConn, uint32_t aServerId) {
            for (const auto& grant : DecodeOutboxMessagesFor<NotifyOwnershipTransfer>(aConn))
            {
                if (grant.ServerId == aServerId)
                    return true;
            }
            return false;
        };
        const auto toldToLetGo = [&](TiltedPhoques::ConnectionId_t aConn, uint32_t aServerId) {
            for (const auto& notice : DecodeOutboxMessagesFor<NotifyRelinquishControl>(aConn))
            {
                if (notice.ServerId == aServerId)
                    return true;
            }
            return false;
        };
        uint32_t nextHitId = 19000u;
        const auto swing = [&](uint32_t aTarget, float aDelta) {
            RequestHealthChangeBroadcast hit{};
            hit.Id = aTarget;
            hit.DeltaHealth = aDelta;
            hit.HitId = nextHitId++;
            return hit;
        };

        // --- 1. The guest conjures an atronach --------------------------------------------------
        const uint32_t atronach = assign(pGuest, kGuestConn, true);
        Check(atronach != 0xFFFFFFFFu && lastAssign.has_value() && lastAssign->Owner && lastAssign->PlayerId == pGuest->GetId() &&
                  world.get<CharacterComponent>(entityOf(atronach)).IsPlayerSummon() && ownerOf(atronach) == pGuest,
              "the guest's atronach is registered as his summon");
        const auto atronachForHost = spawnedFor(kHostConn, atronach);
        Check(atronachForHost.has_value() && atronachForHost->IsPlayerSummon && atronachForHost->PlayerId == pGuest->GetId() &&
                  atronachForHost->BaseId == kAtronachBase && atronachForHost->FormId == GameId{},
              "the host is sent it as the guest's summon, so his game binds it to the guest's character and not to his own");

        // The host conjures the same creature on the same spot: two summons, not one shared.
        const uint32_t hostAtronach = assign(pHost, kHostConn, true);
        Check(hostAtronach != 0xFFFFFFFFu && hostAtronach != atronach && lastAssign.has_value() && lastAssign->Owner &&
                  ownerOf(hostAtronach) == pHost && ownerOf(atronach) == pGuest,
              "the same creature conjured by the host on the same spot is a second summon: summons are never merged");

        // --- 2. It cannot change hands ----------------------------------------------------------
        Harness::ClearOutbox();
        RequestOwnershipClaim claim{};
        claim.ServerId = atronach;
        claim.NewActorData.InitialActorValues.ActorValuesList[kHealthIndex] = 100.0f;
        claim.NewActorData.InitialActorValues.ActorMaxValuesList[kHealthIndex] = 100.0f;
        Check(DeliverWire(world, claim, pHost) && ownerOf(atronach) == pGuest && toldToLetGo(kHostConn, atronach) && !grantedTo(kHostConn, atronach),
              "the party leader's claim on it is refused");

        // A calm world NPC goes to the party leader when he walks in. A summon does not.
        Check(walkInto(pHost, kCellar) && walkInto(pHost, kHall) && ownerOf(atronach) == pGuest && !grantedTo(kHostConn, atronach) &&
                  spawnedFor(kHostConn, atronach).has_value(),
              "the leader walking back in is shown the atronach, and it stays with the guest");

        // The guest stands in a menu for a minute while the host plays on.
        constexpr uint64_t kLeaseNow = 9'000'000ull;
        world.GetCharacterService().NotePlayerSimulationActivity(pGuest, kLeaseNow);
        world.GetCharacterService().NotePlayerSimulationActivity(pHost, kLeaseNow + 59'000ull);
        Harness::ClearOutbox();
        world.GetCharacterService().SweepExpiredWorldNpcLeases(kLeaseNow + 60'000ull);
        Check(ownerOf(atronach) == pGuest && !grantedTo(kHostConn, atronach) && !toldToLetGo(kGuestConn, atronach),
              "a summoner who is silent for a minute keeps his summon: there is no lease to lose");

        // --- 3. Movement and damage -------------------------------------------------------------
        const auto movementOf = [&](uint32_t aServerId, const glm::vec3& acPosition) {
            ClientReferencesMoveRequest request{};
            request.Tick = 7000u;
            ReferenceUpdate update{};
            update.UpdatedMovement.CellId = kHall;
            update.UpdatedMovement.Position = acPosition;
            request.Updates[aServerId] = update;
            return request;
        };
        const auto relayedTo = [&](TiltedPhoques::ConnectionId_t aConn, uint32_t aServerId) {
            for (const auto& relay : DecodeOutboxMessagesFor<ServerReferencesMoveRequest>(aConn))
            {
                if (relay.Updates.find(aServerId) != relay.Updates.end())
                    return true;
            }
            return false;
        };
        Harness::ClearOutbox();
        Check(DeliverWire(world, movementOf(atronach, glm::vec3{260.0f, 210.0f, 0.0f}), pGuest) && relayedTo(kHostConn, atronach),
              "the atronach's movement comes from the guest's game and is sent to the host");
        Harness::ClearOutbox();
        Check(DeliverWire(world, movementOf(atronach, glm::vec3{900.0f, 900.0f, 0.0f}), pHost) && !relayedTo(kGuestConn, atronach) &&
                  glm::distance(glm::vec3(world.get<MovementComponent>(entityOf(atronach)).Position), glm::vec3{260.0f, 210.0f, 0.0f}) < 1.0f,
              "the host's game cannot move it");

        Harness::ClearOutbox();
        Check(DeliverWire(world, swing(atronach, -40.0f), pHost) && near(healthOf(atronach), 100.0f) &&
                  DecodeOutboxMessagesFor<NotifyHealthChangeBroadcast>(kGuestConn).empty(),
              "a party member's stray blow does not hurt the summon");
        Harness::ClearOutbox();
        Check(DeliverWire(world, swing(atronach, -30.0f), pGuest) && near(healthOf(atronach), 70.0f) &&
                  DecodeOutboxMessagesFor<NotifyHealthChangeBroadcast>(kHostConn).size() == 1,
              "damage an enemy does to it in the summoner's game is applied and sent to the host: 100 -> 70");

        // --- 4. The summoner lets it go ---------------------------------------------------------
        // His client hands over every actor it unloads. A world NPC goes to the player who stays;
        // a summon is removed, because nobody else's game can run it.
        // The summon expired, was dismissed, or was unloaded with the summoner's cell: for the server
        // this request is the only sign of it (defect M26).
        RequestOwnershipTransfer letGo{};
        letGo.ServerId = atronach;
        letGo.OwnershipEpoch = world.get<OwnerComponent>(entityOf(atronach)).GetEpoch();

        Check(static_cast<bool>(bEnableSummonDismissal), "Gameplay:bEnableSummonDismissal is on by default");
        bEnableSummonDismissal = false;
        Harness::ClearOutbox();
        Check(DeliverWire(world, letGo, pGuest) && exists(atronach) && ownerOf(atronach) == pGuest && !removedFor(kHostConn, atronach) &&
                  toldToLetGo(kGuestConn, atronach),
              "with the switch off the request is refused, as in 4.1.0: the atronach stays for the host where it last stood");
        bEnableSummonDismissal = true;

        Harness::ClearOutbox();
        RequestOwnershipTransfer staleLetGo = letGo;
        staleLetGo.OwnershipEpoch = letGo.OwnershipEpoch + 5u;
        Check(DeliverWire(world, staleLetGo, pGuest) && exists(atronach) && !removedFor(kHostConn, atronach),
              "a request that quotes an epoch the lease never had is refused");

        Harness::ClearOutbox();
        Check(DeliverWire(world, letGo, pGuest) && !exists(atronach) && removedFor(kHostConn, atronach) && !grantedTo(kHostConn, atronach) &&
                  !removedFor(kGuestConn, atronach),
              "let go by the summoner, the atronach is removed for the host and given to nobody");
        Check(exists(hostAtronach) && ownerOf(hostAtronach) == pHost, "the host's own atronach is untouched");
        // A player who walks in later is not shown a summon that is gone.
        Check(walkInto(pHost, kCellar) && walkInto(pHost, kHall) && !spawnedFor(kHostConn, atronach).has_value(),
              "walking back into the hall, the host is not shown the atronach that is gone");

        // A request from somebody else to let the host's summon go is refused.
        Harness::ClearOutbox();
        RequestOwnershipTransfer notMine{};
        notMine.ServerId = hostAtronach;
        Check(DeliverWire(world, notMine, pGuest) && exists(hostAtronach) && ownerOf(hostAtronach) == pHost && toldToLetGo(kGuestConn, hostAtronach),
              "the guest cannot make the server drop the host's summon");

        // --- 5. The summoner leaves the game ----------------------------------------------------
        const uint32_t secondAtronach = assign(pGuest, kGuestConn, true);
        Check(secondAtronach != 0xFFFFFFFFu && spawnedFor(kHostConn, secondAtronach).has_value(), "the guest conjures again and the host is shown the new atronach");
        Harness::ClearOutbox();
        Disconnect(server, kGuestConn);
        Check(!exists(secondAtronach) && removedFor(kHostConn, secondAtronach) && !grantedTo(kHostConn, secondAtronach),
              "the guest disconnects: his atronach is removed for the host, not handed to him");
        Check(!exists(guestChar) && removedFor(kHostConn, guestChar), "so is the guest's character");
        Check(exists(hostAtronach) && ownerOf(hostAtronach) == pHost && exists(hostChar), "the host and his atronach stay");
    }

    // =========================================================================
    // Scene 20: a merchant's chest under the rules of a shared chest (work plan 6.4, defect M27).
    // =========================================================================
    //
    // A chest of loot is shared: the first player to register it decides what is in it, and a take
    // the server's copy cannot cover is taken back from the taker. That is right for loot. A
    // merchant's chest is filled by each player's own game (RFAB keeps the stock in each player's
    // save and the game restocks it locally), so the players' copies differ all the time. The scene
    // shows what the shared rules then do to a trade: the two halves of a trade are separate
    // packets, and only the half the server cannot cover is undone.
    //
    // The server cannot tell a merchant's chest from any other. Since 4.1.1 the client does not
    // register one (Gameplay:bEnablePersonalMerchantStock); this scene is why.
    {
        std::printf("[scene 20] a merchant's chest under the rules of a shared chest\n");
        std::filesystem::remove(SaveSnapshotService::GetDefaultCheckpointStorePath(), removeEc);

        GameServer server{registry};
        World& world = server.GetWorld();

        constexpr TiltedPhoques::ConnectionId_t kFirstConn = 0xF501u;
        constexpr TiltedPhoques::ConnectionId_t kSecondConn = 0xF502u;
        const GameId kShop{0u, 0x00101001u};
        const GameId kMerchantChest{0u, 0x00102001u};
        const GameId kGold{0u, 0x0000000Fu};
        const GameId kPotion{0u, 0x0003EADDu};
        const GameId kSword{0u, 0x000139B9u};
        const GameId kAxe{0u, 0x000139B3u};
        const GameId kHelmet{0u, 0x00013913u};

        Player* pFirst = world.GetPlayerManager().Create(kFirstConn);
        Player* pSecond = world.GetPlayerManager().Create(kSecondConn);
        TimeModel t0{};
        t0.Time = 13.0f;
        t0.Day = 7;
        t0.Month = 5;
        t0.Year = 201;
        world.GetDispatcher().trigger(PlayerJoinEvent{pFirst, {}, {}, DateTime{t0}});
        world.GetDispatcher().trigger(PlayerJoinEvent{pSecond, {}, {}, DateTime{t0}});
        pFirst->SetCellComponent(CellIdComponent{kShop});
        pSecond->SetCellComponent(CellIdComponent{kShop});

        const auto registerPlayerCharacter = [&](Player* apPlayer, TiltedPhoques::ConnectionId_t aConn, uint32_t aCookie) {
            Harness::ClearOutbox();
            AssignCharacterRequest request{};
            request.Cookie = aCookie;
            request.ReferenceId = GameId{0u, 0x14u};
            request.CellId = kShop;
            request.CurrentActorData.InitialActorValues.ActorValuesList[24] = 100.0f;
            request.CurrentActorData.InitialActorValues.ActorMaxValuesList[24] = 100.0f;
            (void)DeliverWire(world, request, apPlayer);
            const auto responses = DecodeOutboxMessagesFor<AssignCharacterResponse>(aConn);
            return responses.size() == 1 ? responses[0].ServerId : 0xFFFFFFFFu;
        };
        const auto stack = [](const GameId& acItem, int32_t aCount) {
            Inventory::Entry entry{};
            entry.BaseId = acItem;
            entry.Count = aCount;
            return entry;
        };
        const auto registerChest = [&](Player* apPlayer, TiltedPhoques::ConnectionId_t aConn, std::initializer_list<Inventory::Entry> aLocalContents) {
            Harness::ClearOutbox();
            AssignObjectsRequest request{};
            ObjectData chest{};
            chest.Id = kMerchantChest;
            chest.CellId = kShop;
            for (const auto& entry : aLocalContents)
                chest.CurrentInventory.AddOrRemoveEntry(entry);
            request.Objects.push_back(chest);
            (void)DeliverWire(world, request, apPlayer);
            const auto responses = DecodeOutboxMessagesFor<AssignObjectsResponse>(aConn);
            return (responses.size() == 1 && responses[0].Objects.size() == 1) ? responses[0].Objects[0] : ObjectData{};
        };
        // One half of a trade as the client reports it: an item enters (+) or leaves (-) the chest.
        const auto chestChange = [&](Player* apPlayer, uint32_t aChestServerId, const GameId& acItem, int32_t aDelta) {
            RequestInventoryChanges request{};
            request.ServerId = aChestServerId;
            request.Item = stack(acItem, aDelta);
            request.UpdateClients = true;
            return DeliverWire(world, request, apPlayer);
        };
        // What the server told this player to take out of his own character, for one item.
        const auto takenBackFrom = [](TiltedPhoques::ConnectionId_t aConn, uint32_t aCharacterServerId, const GameId& acItem) {
            int32_t total = 0;
            for (const auto& notify : DecodeOutboxMessagesFor<NotifyInventoryChanges>(aConn))
                if (notify.ServerId == aCharacterServerId && notify.Item.BaseId == acItem)
                    total -= notify.Item.Count;
            return total;
        };

        const uint32_t firstChar = registerPlayerCharacter(pFirst, kFirstConn, 2000u);
        const uint32_t secondChar = registerPlayerCharacter(pSecond, kSecondConn, 2001u);
        Check(firstChar != 0xFFFFFFFFu && secondChar != 0xFFFFFFFFu, "both player characters are registered");

        const ObjectData asFirst = registerChest(pFirst, kFirstConn, {stack(kGold, 500), stack(kPotion, 3)});
        Check(asFirst.IsSenderFirst && asFirst.ServerId != 0, "the first player's copy of the merchant's chest becomes the server's: 500 gold, 3 potions");
        const uint32_t chestId = asFirst.ServerId;
        const auto inChest = [&](const GameId& acItem) {
            GameId lookup = acItem;
            return world.get<InventoryComponent>(static_cast<entt::entity>(chestId)).Content.GetEntryCountById(lookup);
        };

        // --- 1. Trades the server's copy can cover ---------------------------------------------
        Harness::ClearOutbox();
        Check(chestChange(pFirst, chestId, kSword, +1) && chestChange(pFirst, chestId, kGold, -100) &&
                  takenBackFrom(kFirstConn, firstChar, kGold) == 0 && inChest(kGold) == 400 && inChest(kSword) == 1,
              "the first player sells a sword for 100 gold: the chest holds the sword and 400 gold, nothing is taken back");
        Harness::ClearOutbox();
        Check(chestChange(pFirst, chestId, kPotion, -3) && chestChange(pFirst, chestId, kGold, +150) &&
                  takenBackFrom(kFirstConn, firstChar, kPotion) == 0 && inChest(kPotion) == 0 && inChest(kGold) == 550,
              "he buys all 3 potions for 150 gold: the chest holds no potions and 550 gold");

        // --- 2. The other player buys from his own copy of the stock -----------------------------
        // His game rolled its own stock; his barter window shows 2 potions. He buys one for 50.
        Harness::ClearOutbox();
        Check(chestChange(pSecond, chestId, kPotion, -1) && chestChange(pSecond, chestId, kGold, +50),
              "the second player buys a potion his own game shows in the merchant's stock");
        Check(takenBackFrom(kSecondConn, secondChar, kPotion) == 1, "the potion is taken back from him: the server's copy has none, the first player bought them");
        Check(takenBackFrom(kSecondConn, secondChar, kGold) == 0 && inChest(kGold) == 600,
              "the 50 gold he paid stay in the chest: he paid and holds nothing");

        // --- 3. He sells to a merchant whose gold his game counts differently ---------------------
        // His barter window shows 750 gold. He sells an axe for 700.
        Harness::ClearOutbox();
        Check(chestChange(pSecond, chestId, kAxe, +1) && chestChange(pSecond, chestId, kGold, -700) && inChest(kAxe) == 1,
              "the second player sells an axe for 700 gold; the chest takes the axe");
        Check(takenBackFrom(kSecondConn, secondChar, kGold) == 100 && inChest(kGold) == 0,
              "the server's copy held 600 gold: 100 of the 700 are taken back from him");

        // --- 4. The first player's game restocks the merchant -------------------------------------
        // Two game days later his game refills the chest: 750 gold on his screen, none on the server.
        Harness::ClearOutbox();
        Check(chestChange(pFirst, chestId, kHelmet, +1) && chestChange(pFirst, chestId, kGold, -200) && inChest(kHelmet) == 1,
              "after a restock in his own game the first player sells a helmet for 200 gold; the chest takes the helmet");
        Check(takenBackFrom(kFirstConn, firstChar, kGold) == 200,
              "all 200 gold are taken back from him: the gold was emptied by the other player, so he sold the helmet for nothing");

        std::filesystem::remove(SaveSnapshotService::GetDefaultCheckpointStorePath(), removeEc);
    }

    // =========================================================================
    // Scene 21: an encounter both games spawn is one group of enemies (work plan 6.3).
    // =========================================================================
    //
    // RFAB's encounter scripts run in every player's game and place their own creatures: the gates
    // of the Deadlands tower call PlaceActorAtMe on a timer (RFAB_OblTowerGateSummonScript), the
    // Dwarven centurion calls it for its spiders (DwarvenCenturionSupportsSummon). Such a creature
    // has no form id the two games share, so two games would make two of it.
    //
    // Two layers prevent that. The client of a player who does not lead the party deletes its own
    // copy when a party member is within 100 m (DespawnSuppressedNonLeaderWildTempSpawn), so only
    // the leader's game places the creature. What still reaches the server is merged here: a
    // creature another player registered in the last 12 s within 20 m is given to the second game
    // as the same creature. The scene pins this second layer; the first runs only in the game.
    {
        std::printf("[scene 21] an encounter both games spawn is one group of enemies\n");
        std::filesystem::remove(SaveSnapshotService::GetDefaultCheckpointStorePath(), removeEc);

        GameServer server{registry};
        World& world = server.GetWorld();

        constexpr TiltedPhoques::ConnectionId_t kHostConn = 0xF601u;
        constexpr TiltedPhoques::ConnectionId_t kGuestConn = 0xF602u;
        const GameId kPlayerRef{0u, 0x14u};
        const GameId kArena{0u, 0x00110001u};
        const GameId kSpider{0u, 0x00111001u};
        const GameId kWolf{0u, 0x00111002u};
        const GameId kDremora{0u, 0x00111003u};
        const GameId kScamp{0u, 0x00111004u};
        const GameId kHorse{0u, 0x00111005u};
        constexpr uint32_t kTemporary = std::numeric_limits<uint32_t>::max();
        constexpr uint32_t kHealthIndex = ActorValuesComponent::kHealthActorValueIndex;
        // The merge radius is 20 m, about 1400 game units. Each step uses a spot of its own, far
        // from the others, because the records of the earlier steps are still fresh.
        constexpr float kStep = 100.0f;      // 1.4 m
        constexpr float kFar = 4000.0f;      // 57 m

        Player* pHost = world.GetPlayerManager().Create(kHostConn);
        Player* pGuest = world.GetPlayerManager().Create(kGuestConn);
        TimeModel t0{};
        t0.Time = 12.0f;
        t0.Day = 8;
        t0.Month = 5;
        t0.Year = 201;
        world.GetDispatcher().trigger(PlayerJoinEvent{pHost, {}, {}, DateTime{t0}});
        world.GetDispatcher().trigger(PlayerJoinEvent{pGuest, {}, {}, DateTime{t0}});

        const auto walkInto = [&](Player* apPlayer, const GameId& acCell) {
            Harness::ClearOutbox();
            EnterInteriorCellRequest request{};
            request.CellId = acCell;
            return DeliverWire(world, request, apPlayer);
        };
        Check(walkInto(pHost, kArena) && walkInto(pGuest, kArena), "both players enter the arena");

        struct Placed
        {
            uint32_t ServerId{0xFFFFFFFFu};
            bool Owner{false};
            bool Answered{false};
        };
        uint32_t nextCookie = 2100u;
        uint32_t nextTemporaryRef = 0xFF000C00u;
        // What a game does when a script places a creature (or, with kPlayerRef, when the player's
        // own character registers): one AssignCharacterRequest, one answer.
        const auto place = [&](Player* apPlayer, TiltedPhoques::ConnectionId_t aConn, const GameId& acBase, float aX, float aY,
                               bool aMount = false) {
            Harness::ClearOutbox();
            AssignCharacterRequest request{};
            request.Cookie = nextCookie++;
            if (acBase == kPlayerRef)
            {
                request.ReferenceId = kPlayerRef;
            }
            else
            {
                request.ReferenceId = GameId{kTemporary, nextTemporaryRef++};
                request.FormId = acBase;
                request.IsMount = aMount;
            }
            request.CellId = kArena;
            request.Position = glm::vec3{aX, aY, 0.0f};
            request.CurrentActorData.BaseId = request.FormId;
            request.CurrentActorData.InitialActorValues.ActorValuesList[kHealthIndex] = 100.0f;
            request.CurrentActorData.InitialActorValues.ActorMaxValuesList[kHealthIndex] = 100.0f;
            (void)DeliverWire(world, request, apPlayer);
            const auto responses = DecodeOutboxMessagesFor<AssignCharacterResponse>(aConn);
            return responses.size() == 1 ? Placed{responses[0].ServerId, responses[0].Owner, true} : Placed{};
        };
        Check(place(pHost, kHostConn, kPlayerRef, 0.0f, 0.0f).Answered && place(pGuest, kGuestConn, kPlayerRef, 0.0f, 0.0f).Answered,
              "both players' characters are registered");

        const auto entityOf = [](uint32_t aServerId) { return static_cast<entt::entity>(aServerId); };
        const auto ownerOf = [&](uint32_t aServerId) { return world.get<OwnerComponent>(entityOf(aServerId)).GetOwner(); };
        const auto characters = [&]() { return world.view<CharacterComponent>().size(); };
        const auto spawnedFor = [&](TiltedPhoques::ConnectionId_t aConn, uint32_t aServerId) -> std::optional<CharacterSpawnRequest> {
            for (const auto& spawn : DecodeOutboxMessagesFor<CharacterSpawnRequest>(aConn))
            {
                if (spawn.ServerId == aServerId)
                    return spawn;
            }
            return std::nullopt;
        };
        const auto nothingSpawnedFor = [&](TiltedPhoques::ConnectionId_t aConn) {
            return DecodeOutboxMessagesFor<CharacterSpawnRequest>(aConn).empty();
        };

        // --- 1. One creature placed by both games ----------------------------------------------
        const float spot1 = 10000.0f;
        const Placed hostSpider = place(pHost, kHostConn, kSpider, spot1, 0.0f);
        const auto spiderForGuest = spawnedFor(kGuestConn, hostSpider.ServerId);
        Check(hostSpider.Answered && hostSpider.Owner && ownerOf(hostSpider.ServerId) == pHost && spiderForGuest.has_value() &&
                  spiderForGuest->BaseId == kSpider && spiderForGuest->FormId == GameId{},
              "the host's game places a spider: it is the host's, and the guest is sent it as a new creature of that kind");

        const size_t beforeGuestSpider = characters();
        const Placed guestSpider = place(pGuest, kGuestConn, kSpider, spot1 + kStep, 0.0f);
        Check(guestSpider.Answered && !guestSpider.Owner && guestSpider.ServerId == hostSpider.ServerId && characters() == beforeGuestSpider,
              "the guest's game places the same spider 1.4 m away: it is given the host's spider, and the server keeps one");
        Check(spawnedFor(kGuestConn, hostSpider.ServerId).has_value() && nothingSpawnedFor(kHostConn),
              "the guest is sent the host's spider once more with that answer; the host is told nothing");

        const Placed guestSecondSpider = place(pGuest, kGuestConn, kSpider, spot1 + kStep, 0.0f);
        Check(guestSecondSpider.Answered && guestSecondSpider.Owner && guestSecondSpider.ServerId != hostSpider.ServerId &&
                  spawnedFor(kHostConn, guestSecondSpider.ServerId).has_value(),
              "a second spider of the guest at that spot is his own: the host placed only one, and the host is sent it");

        // --- 2. A pack: several creatures of one kind at one spot -------------------------------
        const float spot2 = 20000.0f;
        const Placed hostWolf1 = place(pHost, kHostConn, kWolf, spot2, 0.0f);
        const Placed hostWolf2 = place(pHost, kHostConn, kWolf, spot2, 0.0f);
        Check(hostWolf1.Owner && hostWolf2.Owner && hostWolf1.ServerId != hostWolf2.ServerId,
              "the host's game places two wolves at one spot: they stay two, a player's own creatures are never merged");
        const Placed guestWolf1 = place(pGuest, kGuestConn, kWolf, spot2, 0.0f);
        const Placed guestWolf2 = place(pGuest, kGuestConn, kWolf, spot2, 0.0f);
        const bool oneEach = (guestWolf1.ServerId == hostWolf1.ServerId && guestWolf2.ServerId == hostWolf2.ServerId) ||
                             (guestWolf1.ServerId == hostWolf2.ServerId && guestWolf2.ServerId == hostWolf1.ServerId);
        Check(!guestWolf1.Owner && !guestWolf2.Owner && oneEach,
              "the guest's two wolves are given the host's two, one each");

        // --- 3. The two games rolled different kinds --------------------------------------------
        const float spot3 = 30000.0f;
        const Placed hostDremora = place(pHost, kHostConn, kDremora, spot3, 0.0f);
        const Placed guestScamp = place(pGuest, kGuestConn, kScamp, spot3, 0.0f);
        const auto dremoraForGuest = spawnedFor(kGuestConn, hostDremora.ServerId);
        Check(hostDremora.Owner && !guestScamp.Owner && guestScamp.ServerId == hostDremora.ServerId && dremoraForGuest.has_value() &&
                  dremoraForGuest->BaseId == kDremora,
              "the host's gate rolled a dremora, the guest's a scamp: the scamp is given the dremora, and the guest is told its kind");

        const float spot4 = 40000.0f;
        const Placed nearDremora = place(pHost, kHostConn, kDremora, spot4, 0.0f);
        const Placed awayScamp = place(pHost, kHostConn, kScamp, spot4 + 5.0f * kStep, 0.0f);
        const Placed guestScamp2 = place(pGuest, kGuestConn, kScamp, spot4, 0.0f);
        Check(nearDremora.Owner && awayScamp.Owner && !guestScamp2.Owner && guestScamp2.ServerId == awayScamp.ServerId,
              "with a dremora at the spot and a scamp 7 m away, the guest's scamp is given the scamp: the same kind comes first");

        // --- 4. What is not merged --------------------------------------------------------------
        const float spot5 = 50000.0f;
        const Placed hostFarSpider = place(pHost, kHostConn, kSpider, spot5, 0.0f);
        const Placed guestFarSpider = place(pGuest, kGuestConn, kSpider, spot5 + kFar, 0.0f);
        Check(hostFarSpider.Owner && guestFarSpider.Owner && guestFarSpider.ServerId != hostFarSpider.ServerId,
              "a spider the guest's game places 57 m from the host's is another spider");

        const float spot6 = 60000.0f;
        const Placed hostHorse = place(pHost, kHostConn, kHorse, spot6, 0.0f);
        const Placed guestHorse = place(pGuest, kGuestConn, kHorse, spot6, 0.0f, true);
        Check(hostHorse.Owner && guestHorse.Owner && guestHorse.ServerId != hostHorse.ServerId,
              "a mount is never merged: the guest's horse next to a creature of the same kind stays his horse");

        std::filesystem::remove(SaveSnapshotService::GetDefaultCheckpointStorePath(), removeEc);
    }

    // =========================================================================
    // Scene 22: a take between a registration and its answer (work plan 6.4, defect M28).
    // =========================================================================
    //
    // A client registers a container by sending its list: the cell scan when the player enters a
    // cell, an activation of a container the client holds no id for, and a take from such a
    // container (the take is queued, the list is sent as it is after the take). The answer "you
    // are first" made 4.1.0's client drop every queued take.
    //
    // A take made between a request and its answer meets a client that still holds no id, so it
    // registers the chest a second time. The scene shows what the server makes of it. Parts 1 and
    // 4 are 4.1.0's client: two registrations, the queued take dropped by the first answer. Parts
    // 2, 3 and 5 are the client with Gameplay:bEnableEarlyTakeReplay: one registration, and the
    // change sent when the answer arrives. The server is the same in all of them; which sequence
    // the client sends is pinned by the core suite rfab_v411_early_take_replay.cpp, and the
    // client's code runs only in the game (P21).
    {
        std::printf("[scene 22] a take between a registration and its answer\n");
        std::filesystem::remove(SaveSnapshotService::GetDefaultCheckpointStorePath(), removeEc);

        GameServer server{registry};
        World& world = server.GetWorld();

        constexpr TiltedPhoques::ConnectionId_t kFirstConn = 0xF701u;
        constexpr TiltedPhoques::ConnectionId_t kSecondConn = 0xF702u;
        const GameId kCamp{0u, 0x00121001u};
        const GameId kChestOld{0u, 0x00122001u};
        const GameId kChestNew{0u, 0x00122002u};
        const GameId kChestPut{0u, 0x00122003u};
        const GameId kChestTwoOld{0u, 0x00122004u};
        const GameId kChestTwoNew{0u, 0x00122005u};
        const GameId kGold{0u, 0x0000000Fu};
        const GameId kSword{0u, 0x000139B9u};
        const GameId kAxe{0u, 0x000139B3u};
        const GameId kHelmet{0u, 0x00013913u};

        Player* pFirst = world.GetPlayerManager().Create(kFirstConn);
        Player* pSecond = world.GetPlayerManager().Create(kSecondConn);
        TimeModel t0{};
        t0.Time = 13.0f;
        t0.Day = 7;
        t0.Month = 5;
        t0.Year = 201;
        world.GetDispatcher().trigger(PlayerJoinEvent{pFirst, {}, {}, DateTime{t0}});
        world.GetDispatcher().trigger(PlayerJoinEvent{pSecond, {}, {}, DateTime{t0}});
        pFirst->SetCellComponent(CellIdComponent{kCamp});
        pSecond->SetCellComponent(CellIdComponent{kCamp});

        const auto registerPlayerCharacter = [&](Player* apPlayer, TiltedPhoques::ConnectionId_t aConn, uint32_t aCookie) {
            Harness::ClearOutbox();
            AssignCharacterRequest request{};
            request.Cookie = aCookie;
            request.ReferenceId = GameId{0u, 0x14u};
            request.CellId = kCamp;
            request.CurrentActorData.InitialActorValues.ActorValuesList[24] = 100.0f;
            request.CurrentActorData.InitialActorValues.ActorMaxValuesList[24] = 100.0f;
            (void)DeliverWire(world, request, apPlayer);
            const auto responses = DecodeOutboxMessagesFor<AssignCharacterResponse>(aConn);
            return responses.size() == 1 ? responses[0].ServerId : 0xFFFFFFFFu;
        };
        const auto stack = [](const GameId& acItem, int32_t aCount) {
            Inventory::Entry entry{};
            entry.BaseId = acItem;
            entry.Count = aCount;
            return entry;
        };
        // A registration as either path of the client sends it: the container and its list.
        const auto registerChest = [&](Player* apPlayer, TiltedPhoques::ConnectionId_t aConn, const GameId& acChest,
                                       std::initializer_list<Inventory::Entry> aLocalContents) {
            Harness::ClearOutbox();
            AssignObjectsRequest request{};
            ObjectData chest{};
            chest.Id = acChest;
            chest.CellId = kCamp;
            for (const auto& entry : aLocalContents)
                chest.CurrentInventory.AddOrRemoveEntry(entry);
            request.Objects.push_back(chest);
            (void)DeliverWire(world, request, apPlayer);
            const auto responses = DecodeOutboxMessagesFor<AssignObjectsResponse>(aConn);
            return (responses.size() == 1 && responses[0].Objects.size() == 1) ? responses[0].Objects[0] : ObjectData{};
        };
        const auto chestChange = [&](Player* apPlayer, uint32_t aChestServerId, const GameId& acItem, int32_t aDelta) {
            RequestInventoryChanges request{};
            request.ServerId = aChestServerId;
            request.Item = stack(acItem, aDelta);
            request.UpdateClients = true;
            return DeliverWire(world, request, apPlayer);
        };
        const auto takenBackFrom = [](TiltedPhoques::ConnectionId_t aConn, uint32_t aCharacterServerId, const GameId& acItem) {
            int32_t total = 0;
            for (const auto& notify : DecodeOutboxMessagesFor<NotifyInventoryChanges>(aConn))
                if (notify.ServerId == aCharacterServerId && notify.Item.BaseId == acItem)
                    total -= notify.Item.Count;
            return total;
        };
        const auto toldToChange = [](TiltedPhoques::ConnectionId_t aConn, uint32_t aEntityServerId, const GameId& acItem) {
            int32_t total = 0;
            for (const auto& notify : DecodeOutboxMessagesFor<NotifyInventoryChanges>(aConn))
                if (notify.ServerId == aEntityServerId && notify.Item.BaseId == acItem)
                    total += notify.Item.Count;
            return total;
        };
        const auto inChest = [&](uint32_t aChestServerId, const GameId& acItem) {
            GameId lookup = acItem;
            return world.get<InventoryComponent>(static_cast<entt::entity>(aChestServerId)).Content.GetEntryCountById(lookup);
        };
        const auto countOf = [](const Inventory& acInventory, const GameId& acItem) {
            GameId lookup = acItem;
            return acInventory.GetEntryCountById(lookup);
        };

        const uint32_t firstChar = registerPlayerCharacter(pFirst, kFirstConn, 2200u);
        const uint32_t secondChar = registerPlayerCharacter(pSecond, kSecondConn, 2201u);
        Check(firstChar != 0xFFFFFFFFu && secondChar != 0xFFFFFFFFu, "both player characters are registered");

        // --- 1. 4.1.0's client -------------------------------------------------------------------
        // The first player crosses into the cell; the scan sends the chest: a sword and 50 gold.
        const ObjectData scanOld = registerChest(pFirst, kFirstConn, kChestOld, {stack(kSword, 1), stack(kGold, 50)});
        Check(scanOld.IsSenderFirst && scanOld.ServerId != 0, "the scan's registration makes the first player's list the server's: a sword and 50 gold");
        // Before that answer reaches him he takes the sword. His client holds no id for the chest,
        // so the take registers it again, with the list after the take.
        const ObjectData onDemandOld = registerChest(pFirst, kFirstConn, kChestOld, {stack(kGold, 50)});
        Check(!onDemandOld.IsSenderFirst && onDemandOld.ServerId == scanOld.ServerId,
              "the take's own registration of the same chest is answered 'not first'");
        Check(countOf(onDemandOld.CurrentInventory, kSword) == 1,
              "and carries the list as the scan read it: the sword he holds is in it, and his client writes that list over the chest");
        // The first answer ("you are first") dropped the queued take: 4.1.0 sends nothing more.
        Check(inChest(scanOld.ServerId, kSword) == 1, "nothing told the server about the take: its copy of the chest still holds the sword");

        const ObjectData secondSeesOld = registerChest(pSecond, kSecondConn, kChestOld, {stack(kGold, 35)});
        Check(!secondSeesOld.IsSenderFirst && countOf(secondSeesOld.CurrentInventory, kSword) == 1,
              "the second player enters the cell: the chest he is shown holds the sword");
        Harness::ClearOutbox();
        Check(chestChange(pSecond, scanOld.ServerId, kSword, -1) && takenBackFrom(kSecondConn, secondChar, kSword) == 0 &&
                  inChest(scanOld.ServerId, kSword) == 0,
              "he takes it and keeps it: one sword in the chest has become two, one in each pack");

        // --- 2. The client with bEnableEarlyTakeReplay --------------------------------------------
        const ObjectData scanNew = registerChest(pFirst, kFirstConn, kChestNew, {stack(kAxe, 1), stack(kGold, 50)});
        Check(scanNew.IsSenderFirst && scanNew.ServerId != 0 && scanNew.ServerId != scanOld.ServerId,
              "another chest, the same scan: an axe and 50 gold");
        // He takes the axe before the answer. The client knows the scan's request is unanswered:
        // it sends no second registration, and when the answer says "you are first" it sends the
        // take, because the scan read the list before the take.
        Harness::ClearOutbox();
        Check(chestChange(pFirst, scanNew.ServerId, kAxe, -1) && inChest(scanNew.ServerId, kAxe) == 0,
              "the take is sent at the answer: the server's copy of the chest gives up the axe");
        Check(takenBackFrom(kFirstConn, firstChar, kAxe) == 0, "nothing is taken back from him: the server's list held the axe");
        Check(toldToChange(kSecondConn, scanNew.ServerId, kAxe) == -1, "the second player, in the same cell, is told the axe left the chest");

        const ObjectData secondSeesNew = registerChest(pSecond, kSecondConn, kChestNew, {stack(kAxe, 1), stack(kGold, 35)});
        Check(!secondSeesNew.IsSenderFirst && countOf(secondSeesNew.CurrentInventory, kAxe) == 0 &&
                  countOf(secondSeesNew.CurrentInventory, kGold) == 50,
              "the chest the second player is shown holds 50 gold and no axe, whatever his own game had rolled");
        Harness::ClearOutbox();
        Check(chestChange(pSecond, scanNew.ServerId, kAxe, -1) && takenBackFrom(kSecondConn, secondChar, kAxe) == 1,
              "an axe he takes from his own stale copy is taken back from him: the axe went to the first player, once");

        // --- 3. A put in the same moment ----------------------------------------------------------
        const ObjectData scanPut = registerChest(pFirst, kFirstConn, kChestPut, {stack(kGold, 50)});
        Check(scanPut.IsSenderFirst && inChest(scanPut.ServerId, kHelmet) == 0, "a third chest: 50 gold");
        // He puts a helmet in before the answer. 4.1.0 dropped that change too: the helmet stayed
        // in his own copy only, and vanished when the server's list was next written over it.
        Harness::ClearOutbox();
        Check(chestChange(pFirst, scanPut.ServerId, kHelmet, +1) && inChest(scanPut.ServerId, kHelmet) == 1,
              "the put is sent at the answer: the server's copy of the chest holds the helmet");
        const ObjectData secondSeesPut = registerChest(pSecond, kSecondConn, kChestPut, {stack(kGold, 35)});
        Check(!secondSeesPut.IsSenderFirst && countOf(secondSeesPut.CurrentInventory, kHelmet) == 1,
              "the second player is shown the helmet in the chest");

        // --- 4. Two takes in a row from a chest nobody had registered: 4.1.0's client -------------
        // The chest stands across a cell border: a sword, an axe, 50 gold. The first player takes
        // the sword through QuickLootIE's window; the take registers the chest with the list after it.
        const ObjectData takeOneOld = registerChest(pFirst, kFirstConn, kChestTwoOld, {stack(kAxe, 1), stack(kGold, 50)});
        Check(takeOneOld.IsSenderFirst && takeOneOld.ServerId != 0, "the first take registers the chest: an axe and 50 gold are left");
        // He takes the axe before that answer reaches him: still no id, so a registration again.
        const ObjectData takeTwoOld = registerChest(pFirst, kFirstConn, kChestTwoOld, {stack(kGold, 50)});
        Check(!takeTwoOld.IsSenderFirst && countOf(takeTwoOld.CurrentInventory, kAxe) == 1 && inChest(takeOneOld.ServerId, kAxe) == 1,
              "the second take's registration is answered 'not first' with the axe in the list: the axe he holds is back in the chest for everybody");

        // --- 5. The same two takes from the client with bEnableEarlyTakeReplay ---------------------
        const ObjectData takeOneNew = registerChest(pFirst, kFirstConn, kChestTwoNew, {stack(kAxe, 1), stack(kGold, 50)});
        Check(takeOneNew.IsSenderFirst && takeOneNew.ServerId != 0, "another such chest, the first take registers it");
        // The second take finds the first registration unanswered: no registration, the take is sent at the answer.
        Harness::ClearOutbox();
        Check(chestChange(pFirst, takeOneNew.ServerId, kAxe, -1) && inChest(takeOneNew.ServerId, kAxe) == 0 &&
                  takenBackFrom(kFirstConn, firstChar, kAxe) == 0,
              "the second take is sent at the answer: the chest gives up the axe, nothing is taken back");
        const ObjectData secondSeesTwo = registerChest(pSecond, kSecondConn, kChestTwoNew, {stack(kSword, 1), stack(kAxe, 1), stack(kGold, 35)});
        Check(!secondSeesTwo.IsSenderFirst && countOf(secondSeesTwo.CurrentInventory, kAxe) == 0 &&
                  countOf(secondSeesTwo.CurrentInventory, kSword) == 0 && countOf(secondSeesTwo.CurrentInventory, kGold) == 50,
              "the second player is shown 50 gold: neither the sword nor the axe");

        std::filesystem::remove(SaveSnapshotService::GetDefaultCheckpointStorePath(), removeEc);
    }

    // =========================================================================================
    // Scene 23: checkpoint-hydrated dormant enemies keep their weapons & spells upon wake-up
    // =========================================================================================
    {
        std::printf("[scene 23] checkpoint-hydrated dormant enemies keep their weapons and spells\n");
        std::filesystem::remove(SaveSnapshotService::GetDefaultCheckpointStorePath(), removeEc);

        GameServer server{registry};
        World& world = server.GetWorld();

        constexpr TiltedPhoques::ConnectionId_t kHostConn = 0xF801u;
        constexpr TiltedPhoques::ConnectionId_t kGuestConn = 0xF802u;
        const GameId kFort{0u, 0x00131001u};
        const GameId kBanditRef{0u, 0x00132001u};
        const GameId kBanditBase{0u, 0x00039CF4u};
        const GameId kMageRef{0u, 0x00132002u};
        const GameId kMageBase{0u, 0x00039CF5u};
        const GameId kIronSword{0u, 0x00012EB7u};
        const GameId kHuntingBow{0u, 0x00013985u};
        const GameId kIronArrow{0u, 0x0001397Du};
        const GameId kIceSpike{0u, 0x0002B96Cu};

        // Hydrate two persistent dungeon enemies as SaveSnapshotService does from rfab_checkpoints.bin
        // (position + health only, no stored inventory).
        const entt::entity banditEntity = world.GetCharacterService().EnsureDormantPersistentCharacter(
            kBanditRef, kBanditBase, kFort, GameId{}, GridCellCoords{},
            glm::vec3{120.f, 240.f, 10.f}, glm::vec3{0.f, 0.f, 1.5f},
            /*aIsDead=*/false, /*aCurrentHealth=*/180.f, /*aMaxHealth=*/250.f);
        const entt::entity mageEntity = world.GetCharacterService().EnsureDormantPersistentCharacter(
            kMageRef, kMageBase, kFort, GameId{}, GridCellCoords{},
            glm::vec3{160.f, 260.f, 10.f}, glm::vec3{0.f, 0.f, 1.5f},
            /*aIsDead=*/false, /*aCurrentHealth=*/140.f, /*aMaxHealth=*/140.f);

        Check(banditEntity != entt::null && mageEntity != entt::null &&
                  !world.get<InventoryComponent>(banditEntity).HasSeededInventory &&
                  !world.get<InventoryComponent>(mageEntity).HasSeededInventory,
              "both persistent enemies start dormant with unseeded checkpoint inventories");

        Player* pHost = world.GetPlayerManager().Create(kHostConn);
        Player* pGuest = world.GetPlayerManager().Create(kGuestConn);
        TimeModel t0{};
        t0.Time = 14.0f;
        t0.Day = 8;
        t0.Month = 5;
        t0.Year = 201;
        world.GetDispatcher().trigger(PlayerJoinEvent{pHost, {}, {}, DateTime{t0}});
        world.GetDispatcher().trigger(PlayerJoinEvent{pGuest, {}, {}, DateTime{t0}});

        // Both players enter the fort interior cell.
        EnterInteriorCellRequest enterCell{};
        enterCell.CellId = kFort;
        Harness::ClearOutbox();
        (void)DeliverWire(world, enterCell, pHost);
        const auto hostTransfers = DecodeOutboxMessagesFor<NotifyOwnershipTransfer>(kHostConn);
        const uint32_t banditServerId = World::ToInteger(banditEntity);
        const uint32_t mageServerId = World::ToInteger(mageEntity);
        uint64_t banditEpoch = 0;
        for (const auto& tr : hostTransfers)
            if (tr.ServerId == banditServerId)
                banditEpoch = tr.OwnershipEpoch;
        Check(banditEpoch != 0, "entering the fort wakes the dormant bandit and grants ownership to the host");

        Harness::ClearOutbox();
        (void)DeliverWire(world, enterCell, pGuest);

        // 1. Host's client kept its live bandit weapons (ShouldApplyWholesaleActorInventory rejected the
        //    unseeded empty spawn) and sends RequestOwnershipClaim with sword + bow + arrows.
        Inventory armedBanditInv{};
        {
            Inventory::Entry sword{};
            sword.BaseId = kIronSword;
            sword.Count = 1;
            sword.ExtraWorn = true;
            armedBanditInv.Entries.push_back(sword);

            Inventory::Entry bow{};
            bow.BaseId = kHuntingBow;
            bow.Count = 1;
            armedBanditInv.Entries.push_back(bow);

            Inventory::Entry arrows{};
            arrows.BaseId = kIronArrow;
            arrows.Count = 24;
            armedBanditInv.Entries.push_back(arrows);
        }

        RequestOwnershipClaim claimBandit{};
        claimBandit.ServerId = banditServerId;
        claimBandit.NewActorData.BaseId = kBanditBase;
        claimBandit.NewActorData.InitialInventory = armedBanditInv;
        claimBandit.NewActorData.InitialActorValues.ActorValuesList[24] = 250.0f;
        claimBandit.NewActorData.InitialActorValues.ActorMaxValuesList[24] = 250.0f;
        claimBandit.NewActorData.IsDead = false;
        claimBandit.NewActorData.IsWeaponDrawn = true;

        Harness::ClearOutbox();
        Check(DeliverWire(world, claimBandit, pHost), "the host claims the woken bandit with its full RFAB weapon loadout");

        const auto& serverBanditInv = world.get<InventoryComponent>(banditEntity);
        GameId swordLookup = kIronSword;
        GameId bowLookup = kHuntingBow;
        GameId arrowLookup = kIronArrow;
        Check(serverBanditInv.HasSeededInventory &&
                  serverBanditInv.Content.GetEntryCountById(swordLookup) == 1 &&
                  serverBanditInv.Content.GetEntryCountById(bowLookup) == 1 &&
                  serverBanditInv.Content.GetEntryCountById(arrowLookup) == 24,
              "the server stores the bandit's sword, bow, and 24 arrows as seeded inventory");

        float reconciledBanditHp = 0.0f;
        Check(world.get<ActorValuesComponent>(banditEntity).TryGetHealth(reconciledBanditHp) &&
                  std::fabs(reconciledBanditHp - 180.0f) < 0.05f,
              "checkpoint battle damage (180/250 HP) is preserved when the bandit's weapons are seeded");

        const auto guestBanditSync = DecodeOutboxMessagesFor<NotifySpawnData>(kGuestConn);
        bool guestGotWeapons = false;
        for (const auto& sync : guestBanditSync)
        {
            if (sync.Id == banditServerId)
            {
                Inventory copy = sync.NewActorData.InitialInventory;
                guestGotWeapons = (copy.GetEntryCountById(swordLookup) == 1 &&
                                   copy.GetEntryCountById(bowLookup) == 1 &&
                                   copy.GetEntryCountById(arrowLookup) == 24);
            }
        }
        Check(guestGotWeapons, "the guest in the same cell receives the bandit's full weapon loadout");

        // 2. A transient empty snapshot on a living NPC must never strip the server's seeded weapons.
        RequestOwnershipClaim bogusEmptyClaim{};
        bogusEmptyClaim.ServerId = banditServerId;
        bogusEmptyClaim.NewActorData.BaseId = kBanditBase;
        bogusEmptyClaim.NewActorData.InitialActorValues.ActorValuesList[24] = 180.0f;
        bogusEmptyClaim.NewActorData.InitialActorValues.ActorMaxValuesList[24] = 250.0f;
        bogusEmptyClaim.NewActorData.IsDead = false;
        Harness::ClearOutbox();
        (void)DeliverWire(world, bogusEmptyClaim, pHost);
        Check(world.get<InventoryComponent>(banditEntity).Content.GetEntryCountById(swordLookup) == 1,
              "an empty snapshot for a living enemy does not wipe the bandit's weapons on the server");

        // 3. AssignCharacterRequest on the dormant spellcaster (0 physical items, equipped Destruction spell)
        //    seeds CurrentMagicEquipment and broadcasts it to the guest.
        AssignCharacterRequest assignMage{};
        assignMage.Cookie = 2301u;
        assignMage.ReferenceId = kMageRef;
        assignMage.FormId = kMageBase;
        assignMage.CellId = kFort;
        assignMage.CurrentActorData.InitialInventory.CurrentMagicEquipment.RightHandSpell = kIceSpike;
        assignMage.CurrentActorData.InitialActorValues.ActorValuesList[24] = 140.0f;
        assignMage.CurrentActorData.InitialActorValues.ActorValuesList[25] = 320.0f;
        assignMage.CurrentActorData.InitialActorValues.ActorMaxValuesList[24] = 140.0f;
        assignMage.CurrentActorData.IsDead = false;

        Harness::ClearOutbox();
        Check(DeliverWire(world, assignMage, pHost), "the host registers the woken mage carrying an equipped Destruction spell");

        const auto mageResponses = DecodeOutboxMessagesFor<AssignCharacterResponse>(kHostConn);
        Check(mageResponses.size() == 1 &&
                  mageResponses[0].ServerId == mageServerId &&
                  mageResponses[0].CurrentInventory.CurrentMagicEquipment.RightHandSpell == kIceSpike &&
                  world.get<InventoryComponent>(mageEntity).HasSeededInventory,
              "AssignCharacterResponse echoes the mage's equipped Ice Spike spell instead of an empty inventory");

        const auto guestMageSync = DecodeOutboxMessagesFor<NotifySpawnData>(kGuestConn);
        bool guestGotMageSpell = false;
        for (const auto& sync : guestMageSync)
        {
            if (sync.Id == mageServerId &&
                sync.NewActorData.InitialInventory.CurrentMagicEquipment.RightHandSpell == kIceSpike)
            {
                guestGotMageSpell = true;
            }
        }
        Check(guestGotMageSpell, "the guest receives NotifySpawnData with the mage's equipped spell");

        std::filesystem::remove(SaveSnapshotService::GetDefaultCheckpointStorePath(), removeEc);
    }

    std::printf("%s (%d failure%s)\n", g_failures == 0 ? "PASSED" : "FAILED", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
