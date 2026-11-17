// Phase 0 in-process server harness: compiles and executes the real shipping Code/server
// (upstream TiltedEvolution + Phase 0 stability fixes) on Linux under ASan + UBSan.
//
// Nothing mocks the server's services or World: only the socket transport (TiltedPhoques::Server)
// is replaced by an outbox recorder (ConnectStub.cpp), and every scenario drives the real
// wire serializer/deserializer + entt::dispatcher.

#include <spdlog/sinks/stdout_color_sinks.h>

#include <ConsoleRegistry.h>
#include <Components.h>
#include <GameServer.h>
#include <World.h>

#include <Events/PacketEvent.h>
#include <Events/PlayerJoinEvent.h>
#include <Events/PlayerLeaveEvent.h>
#include <Events/UpdateEvent.h>

#include <Messages/ActivateRequest.h>
#include <Messages/AssignCharacterRequest.h>
#include <Messages/AssignCharacterResponse.h>
#include <Messages/CharacterSpawnRequest.h>
#include <Messages/ClientMessageFactory.h>
#include <Messages/ClientReferencesMoveRequest.h>
#include <Messages/DrawWeaponRequest.h>
#include <Messages/EnterInteriorCellRequest.h>
#include <Messages/LockChangeRequest.h>
#include <Messages/NotifyActivate.h>
#include <Messages/NotifyActorMaxValueChanges.h>
#include <Messages/NotifyActorValueChanges.h>
#include <Messages/NotifyChatMessageBroadcast.h>
#include <Messages/NotifyDeathStateChange.h>
#include <Messages/NotifyDrawWeapon.h>
#include <Messages/NotifyHealthChangeBroadcast.h>
#include <Messages/NotifyLockChange.h>
#include <Messages/NotifyOwnershipTransfer.h>
#include <Messages/NotifyPartyInfo.h>
#include <Messages/NotifyPartyJoined.h>
#include <Messages/NotifyQuestUpdate.h>
#include <Messages/PartyAcceptInviteRequest.h>
#include <Messages/PartyCreateRequest.h>
#include <Messages/PartyInviteRequest.h>
#include <Messages/RequestActionEvents.h>
#include <Messages/RequestActorMaxValueChanges.h>
#include <Messages/RequestActorValueChanges.h>
#include <Messages/RequestDeathStateChange.h>
#include <Messages/RequestHealthChangeBroadcast.h>
#include <Messages/RequestOwnershipClaim.h>
#include <Messages/RequestOwnershipTransfer.h>
#include <Messages/RequestQuestUpdate.h>
#include <Messages/SendChatMessageRequest.h>
#include <Messages/ServerMessageFactory.h>
#include <Messages/ServerReferencesMoveRequest.h>

#include <Services/CharacterService.h>
#include <Services/PartyService.h>

#include "TransportSpy.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace
{
int g_failures = 0;

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

class TemporaryWorkingDirectory
{
public:
    TemporaryWorkingDirectory()
    {
        std::error_code ec;
        m_original = std::filesystem::current_path(ec);
        const auto seed = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned i = 0; i < 100; ++i)
        {
            m_path = std::filesystem::temp_directory_path(ec) /
                     ("rfab-phase0-server-" + std::to_string(seed) + "-" + std::to_string(i));
            if (std::filesystem::create_directory(m_path, ec) && !ec)
            {
                std::filesystem::current_path(m_path, ec);
                return;
            }
        }
    }

    ~TemporaryWorkingDirectory()
    {
        std::error_code ec;
        if (!m_original.empty())
            std::filesystem::current_path(m_original, ec);
        if (!m_path.empty())
            std::filesystem::remove_all(m_path, ec);
    }

private:
    std::filesystem::path m_original;
    std::filesystem::path m_path;
};

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

struct TransportAccess : TiltedPhoques::Server
{
    using TiltedPhoques::Server::OnDisconnection;
};

void Disconnect(GameServer& aServer, TiltedPhoques::ConnectionId_t aConnectionId)
{
    TiltedPhoques::Server& transport = aServer;
    (transport.*(&TransportAccess::OnDisconnection))(aConnectionId, TiltedPhoques::Server::EDisconnectReason::Quit);
}

Player* JoinPlayer(World& aWorld, TiltedPhoques::ConnectionId_t aConn, const char* apName, GameId aCellId,
                   uint32_t aCookie)
{
    Player* pPlayer = aWorld.GetPlayerManager().Create(aConn);
    if (!pPlayer)
        return nullptr;

    pPlayer->SetUsername(apName);
    pPlayer->SetLevel(10);

    CellIdComponent cell{};
    cell.Cell = aCellId;
    pPlayer->SetCellComponent(cell);

    aWorld.GetDispatcher().trigger(PlayerJoinEvent(pPlayer));

    // Assign player character via real wire message so CharacterService creates entity + OwnerComponent.
    // Upstream requires FormId == GameId{} for player characters (ReferenceId == 0:0x14).
    AssignCharacterRequest req{};
    req.Cookie = aCookie;
    req.ReferenceId = GameId(0, 0x14);
    req.FormId = GameId{};
    req.CellId = aCellId;
    req.Position.x = 100.f;
    req.Position.y = 200.f;
    req.Position.z = 0.f;
    req.CurrentActorData.InitialActorValues.ActorValuesList[24] = 100.f;
    req.CurrentActorData.InitialActorValues.ActorMaxValuesList[24] = 100.f;
    DeliverWire(aWorld, req, pPlayer);

    return pPlayer;
}

void RunEntityZeroAndLifecycleScenario(Console::ConsoleRegistry& aConsole)
{
    std::printf("[scenario 1] entity 0 reservation + World destruction order\n");
    {
        GameServer server(aConsole);
        auto& world = server.GetWorld();

        const entt::entity e1 = world.create();
        Check(e1 != entt::entity{0}, "world.create() never returns reserved entity 0");
        world.destroy(e1);

        // Create and populate an entity right before destroying GameServer/World to exercise
        // WorldParts::DispatcherOwner destruction order under ASan/UBSan.
        auto* pAlice = JoinPlayer(world, 1001, "Alice", GameId(0, 0x165A8), 11);
        Check(pAlice != nullptr && pAlice->GetCharacter().has_value(), "player joins and receives a character entity");
        if (pAlice && pAlice->GetCharacter())
            Check(*pAlice->GetCharacter() != entt::entity{0}, "player character entity is never 0");
    }
    Check(true, "GameServer and World tear down cleanly with live entities and signal connections");
}

void RunTwoPlayerSyncAndOwnershipScenario(Console::ConsoleRegistry& aConsole)
{
    std::printf("[scenario 2] two-player assignment, ownership epochs, health/death, and disconnect handoff\n");
    GameServer server(aConsole);
    auto& world = server.GetWorld();

    const GameId kBanneredMare(0, 0x165A8);
    constexpr TiltedPhoques::ConnectionId_t kAliceConn = 2001;
    constexpr TiltedPhoques::ConnectionId_t kBobConn = 2002;

    Player* pAlice = JoinPlayer(world, kAliceConn, "Alice", kBanneredMare, 101);
    Player* pBob = JoinPlayer(world, kBobConn, "Bob", kBanneredMare, 102);
    Check(pAlice != nullptr && pBob != nullptr, "both players join into the same interior cell");

    // Party flow
    Harness::ClearOutbox();
    PartyCreateRequest createParty{};
    Check(DeliverWire(world, createParty, pAlice), "Alice creates a party");
    PartyInviteRequest invite{};
    invite.PlayerId = pBob->GetId();
    Check(DeliverWire(world, invite, pAlice), "Alice invites Bob");
    PartyAcceptInviteRequest accept{};
    accept.InviterId = pAlice->GetId();
    Check(DeliverWire(world, accept, pBob), "Bob accepts the invite");
    Check(pAlice->GetParty().JoinedPartyId != -1 &&
              pAlice->GetParty().JoinedPartyId == pBob->GetParty().JoinedPartyId,
          "Alice and Bob share the same party ID");

    // Assign a shared NPC (Bandit) owned by Alice
    Harness::ClearOutbox();
    AssignCharacterRequest npcReq{};
    npcReq.Cookie = 5001;
    npcReq.ReferenceId = GameId(0, 0x20001);
    npcReq.FormId = GameId(0, 0x30001);
    npcReq.CellId = kBanneredMare;
    npcReq.Position.x = 120.f;
    npcReq.Position.y = 220.f;
    npcReq.Position.z = 0.f;
    npcReq.CurrentActorData.InitialActorValues.ActorValuesList[24] = 150.f;
    npcReq.CurrentActorData.InitialActorValues.ActorMaxValuesList[24] = 150.f;
    Check(DeliverWire(world, npcReq, pAlice), "Alice assigns shared NPC over wire");

    const auto responses = DecodeOutboxMessagesFor<AssignCharacterResponse>(kAliceConn);
    Check(responses.size() == 1 && responses[0].Owner && responses[0].ServerId != 0 && responses[0].OwnershipEpoch != 0,
          "Alice receives AssignCharacterResponse as owner with non-zero ServerId and OwnershipEpoch");

    const uint32_t npcServerId = responses.empty() ? 0u : responses[0].ServerId;
    const uint32_t initialEpoch = responses.empty() ? 0u : responses[0].OwnershipEpoch;
    const auto npcEntity = static_cast<entt::entity>(npcServerId);

    // Valid owner update with current epoch succeeds and relays to Bob
    Harness::ClearOutbox();
    RequestActorValueChanges validAv{};
    validAv.Id = npcServerId;
    validAv.OwnershipEpoch = initialEpoch;
    validAv.Values[24] = 120.f;
    Check(DeliverWire(world, validAv, pAlice), "owner sends valid actor value update");
    const auto bobAvNotifies = DecodeOutboxMessagesFor<NotifyActorValueChanges>(kBobConn);
    Check(bobAvNotifies.size() == 1 && std::fabs(bobAvNotifies[0].Values.at(24) - 120.f) < 0.01f,
          "Bob receives NotifyActorValueChanges with updated health 120");

    // Stale ownership epoch (0 or wrong epoch) is rejected
    Harness::ClearOutbox();
    RequestActorValueChanges staleAv{};
    staleAv.Id = npcServerId;
    staleAv.OwnershipEpoch = initialEpoch + 77;
    staleAv.Values[24] = 10.f;
    Check(DeliverWire(world, staleAv, pAlice), "stale-epoch actor value update deserializes cleanly");
    Check(DecodeOutboxMessagesFor<NotifyActorValueChanges>(kBobConn).empty(),
          "stale-epoch actor value update is rejected by server");

    // Non-owner update is rejected
    Harness::ClearOutbox();
    RequestActorValueChanges nonOwnerAv{};
    nonOwnerAv.Id = npcServerId;
    nonOwnerAv.OwnershipEpoch = initialEpoch;
    nonOwnerAv.Values[24] = 5.f;
    Check(DeliverWire(world, nonOwnerAv, pBob), "non-owner actor value update deserializes cleanly");
    Check(DecodeOutboxMessagesFor<NotifyActorValueChanges>(kAliceConn).empty(),
          "non-owner actor value update is rejected by server");

    // Phase 0 stability guard: NaN / Inf / kInfamy (61) actor values & non-finite DeltaHealth
    Harness::ClearOutbox();
    RequestActorValueChanges poisonAv{};
    poisonAv.Id = npcServerId;
    poisonAv.OwnershipEpoch = initialEpoch;
    poisonAv.Values[24] = std::numeric_limits<float>::quiet_NaN();
    poisonAv.Values[25] = std::numeric_limits<float>::infinity();
    poisonAv.Values[61] = 42.f; // kInfamy
    Check(DeliverWire(world, poisonAv, pAlice), "poison actor value packet delivers safely");
    Check(DecodeOutboxMessagesFor<NotifyActorValueChanges>(kBobConn).empty(),
          "NaN/Inf/kInfamy-only actor value packet is filtered out");
    const auto& storedAv = world.get<ActorValuesComponent>(npcEntity).CurrentActorValues.ActorValuesList;
    Check(std::fabs(storedAv.at(24) - 120.f) < 0.01f && storedAv.count(61) == 0,
          "server actor values remain uncorrupted (health still 120, kInfamy absent)");

    Harness::ClearOutbox();
    RequestHealthChangeBroadcast nanDmg{};
    nanDmg.Id = npcServerId;
    nanDmg.DeltaHealth = std::numeric_limits<float>::quiet_NaN();
    Check(DeliverWire(world, nanDmg, pBob), "NaN health delta delivers safely");
    Check(DecodeOutboxMessagesFor<NotifyHealthChangeBroadcast>(kAliceConn).empty(),
          "NaN health delta is dropped before mutating health or broadcasting");

    // Movement relay
    Harness::ClearOutbox();
    ClientReferencesMoveRequest moveReq{};
    moveReq.Tick = 42;
    auto& upd = moveReq.Updates[npcServerId];
    upd.CellId = kBanneredMare;
    upd.Position.x = 135.f;
    upd.Position.y = 240.f;
    upd.Position.z = 5.f;
    Check(DeliverWire(world, moveReq, pAlice), "Alice sends movement update for owned NPC");
    const auto moveNotifies = DecodeOutboxMessagesFor<ServerReferencesMoveRequest>(kBobConn);
    Check(moveNotifies.size() == 1 && moveNotifies[0].Updates.count(npcServerId) == 1,
          "Bob receives relayed ServerReferencesMoveRequest for NPC");

    // Ownership transfer on disconnect hands NPC to Bob with incremented epoch
    Harness::ClearOutbox();
    Disconnect(server, kAliceConn);
    const auto& ownerAfter = world.get<OwnerComponent>(npcEntity);
    Check(ownerAfter.GetOwner() == pBob && ownerAfter.OwnershipEpoch > initialEpoch,
          "disconnecting Alice transfers NPC ownership to Bob with a higher OwnershipEpoch");

    // Death state from new owner with new epoch succeeds
    Harness::ClearOutbox();
    RequestDeathStateChange deathReq{};
    deathReq.Id = npcServerId;
    deathReq.OwnershipEpoch = ownerAfter.OwnershipEpoch;
    deathReq.IsDead = true;
    Check(DeliverWire(world, deathReq, pBob), "Bob marks newly-owned NPC dead with new epoch");
    Check(world.get<CharacterComponent>(npcEntity).IsDead(), "NPC death state is recorded on server");

    Disconnect(server, kBobConn);
}

void RunOverlayChatSafetyScenario(Console::ConsoleRegistry& aConsole)
{
    std::printf("[scenario 3] OverlayService chat linear HTML stripping and pre-character safety\n");
    GameServer server(aConsole);
    auto& world = server.GetWorld();

    // Player created BEFORE character assignment (GetCharacter() == std::nullopt) must not crash on chat
    Player* pPreSpawn = world.GetPlayerManager().Create(3001);
    pPreSpawn->SetUsername("<b>Hero</b>");
    Check(!pPreSpawn->GetCharacter().has_value(), "pre-spawn player has no character yet");

    Harness::ClearOutbox();
    SendChatMessageRequest chatReq{};
    chatReq.MessageType = kGlobalChat;
    chatReq.ChatMessage = "<script>alert(1)</script>   <div>Hello</div> " + std::string(2048, '<');
    Check(DeliverWire(world, chatReq, pPreSpawn), "chat from pre-spawn player with hostile '<' payload succeeds");

    const auto broadcasts = DecodeOutboxMessagesFor<NotifyChatMessageBroadcast>(3001);
    Check(broadcasts.size() == 1, "global chat broadcast emitted once");
    if (!broadcasts.empty())
    {
        Check(std::string(broadcasts[0].PlayerName.c_str()) == "Hero", "HTML stripped from player name");
        const std::string body(broadcasts[0].ChatMessage.c_str());
        Check(body.find("<script>") == std::string::npos && body.find("Hello") != std::string::npos,
              "HTML tags stripped linearly without regex DoS while preserving message text");
    }

    Disconnect(server, 3001);
}
} // namespace

int main()
{
    auto conOut = spdlog::stdout_color_mt("ConOut");
    conOut->set_level(spdlog::level::err);
    spdlog::set_default_logger(conOut);
    spdlog::set_level(spdlog::level::err);

    TemporaryWorkingDirectory workdir;
    Console::ConsoleRegistry console("ConOut");

    RunEntityZeroAndLifecycleScenario(console);
    RunTwoPlayerSyncAndOwnershipScenario(console);
    RunOverlayChatSafetyScenario(console);

    if (g_failures != 0)
    {
        std::printf("\n[RFAB_RESULT] server harness FAILED (%d failure(s))\n", g_failures);
        return 1;
    }

    std::printf("\n[RFAB_RESULT] server harness PASSED (all Phase 0 scenarios ok)\n");
    return 0;
}
