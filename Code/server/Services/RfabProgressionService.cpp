#include <Services/RfabProgressionService.h>

#include <common/RfabModpackCompat.h>
#include <common/RfabWorldImmersion.h>
#include <Events/PlayerJoinEvent.h>
#include <Events/PlayerLeaveEvent.h>
#include <GameServer.h>
#include <Messages/NotifyRfabProgressionEvent.h>
#include <Messages/SyncRfabProgressionEvent.h>
#include <Services/PartyService.h>
#include <Services/SaveSnapshotService.h>
#include <Setting.h>
#include <World.h>

namespace
{
constexpr int32_t kMaxSingleXpGrant = 1'000'000;
constexpr uint64_t kTacticalPingRateWindowMs = 2500;
constexpr uint32_t kMaxTacticalPingsPerWindow = 6;
}

// Off by default: the feature hands out XP, and it has not been through a playtest. The accounting
// below runs either way, so switching it on later never pays for the time it was off.
Console::Setting bEnableLateJoinCatchUpXp{
    "Gameplay:bEnableLateJoinCatchUpXp",
    "Pays a (re)joining player the shared RFAB XP the party earned while they were away, up to 25000",
    false};

// Off by default: only the game can show how many souls each player ends up with.
Console::Setting bEnableSharedDragonSouls{
    "Gameplay:bEnableSharedDragonSouls",
    "Gives +1 dragon soul to party members who were out of range when the party killed a dragon",
    false};

RfabProgressionService::RfabProgressionService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
    , m_progressionConnection(aDispatcher.sink<PacketEvent<SyncRfabProgressionEvent>>().connect<&RfabProgressionService::OnSyncProgressionEvent>(this))
    , m_playerJoinConnection(aDispatcher.sink<PlayerJoinEvent>().connect<&RfabProgressionService::OnPlayerJoin>(this))
    , m_playerLeaveConnection(aDispatcher.sink<PlayerLeaveEvent>().connect<&RfabProgressionService::OnPlayerLeave>(this))
{
}

void RfabProgressionService::OnSyncProgressionEvent(const PacketEvent<SyncRfabProgressionEvent>& acMessage) noexcept
{
    const auto& pkt = acMessage.Packet;
    Player* pSender = acMessage.pPlayer;
    if (!pSender)
        return;

    const uint64_t currentWorldEpoch = m_world.GetSaveSnapshotService().GetCurrentWorldEpoch();
    if (pkt.EpochId != 0 && pkt.EpochId < currentWorldEpoch)
    {
        m_world.GetSaveSnapshotService().GetCoordinator().RecordFlightEvent(
            0, CoopFlight::FlightEventKind::kStaleEpochRejected, pSender->GetId(), pkt.EpochId);
        spdlog::debug(
            "[RFAB Progression] Discarded stale progression packet from player {} (pktEpoch={} < currentWorldEpoch={})",
            pSender->GetId(),
            pkt.EpochId,
            currentWorldEpoch);
        return;
    }

    const uint64_t currentEpoch = currentWorldEpoch;
    const std::string sourceKey(pkt.SourceKey.c_str(), pkt.SourceKey.size());
    const std::string editorId(pkt.EditorId.c_str(), pkt.EditorId.size());

    // Feature switches (common/Session/Features.h): the server owns these relays, so a client that
    // ignores the flag still cannot push a disabled feature onto its peers.
    if ((pkt.Kind == SyncRfabProgressionEvent::EventKind::TacticalPingSync && !IsCoopFeatureEnabled(Session::Feature::kTacticalHud)) ||
        ((pkt.Kind == SyncRfabProgressionEvent::EventKind::AllyProtectionSync ||
          pkt.Kind == SyncRfabProgressionEvent::EventKind::BossEncounterSync) &&
         !IsCoopFeatureEnabled(Session::Feature::kRfabCombatRules)))
    {
        spdlog::debug("[RFAB Features] Dropped progression event kind {} from player {}: feature disabled on this server",
                      static_cast<uint32_t>(pkt.Kind), pSender->GetId());
        return;
    }

    switch (pkt.Kind)
    {
    case SyncRfabProgressionEvent::EventKind::BookRead:
    {
        const bool isNewBook = m_ledger.RecordBookRead(pkt.BookId);
        const int32_t clampedXp = std::clamp(pkt.XpDelta, 0, kMaxSingleXpGrant);
        const auto outcome = isNewBook ? m_ledger.TryGrantXp(sourceKey, clampedXp)
                                       : RfabProgressionLedger::XpGrantOutcome{false, 0, -clampedXp, m_ledger.GetTotalSharedXp()};

        if (isNewBook)
        {
            NotifyRfabProgressionEvent broadcast{};
            broadcast.Kind = NotifyRfabProgressionEvent::EventKind::BookRead;
            broadcast.EpochId = currentEpoch;
            broadcast.SourceKey = pkt.SourceKey;
            broadcast.BookId = pkt.BookId;
            broadcast.XpDelta = outcome.AppliedDelta;
            broadcast.AuthoritativeTotalXp = outcome.AuthoritativeTotalXp;
            GameServer::Get()->SendToPlayers(broadcast, pSender);
            // A book is read once per world: a late joiner gets it marked as read without its XP.
            CreditConnectedPlayers(outcome.AppliedDelta, outcome.AuthoritativeTotalXp);
        }
        else if (outcome.DuplicateRebateDelta < 0)
        {
            NotifyRfabProgressionEvent rebate{};
            rebate.Kind = NotifyRfabProgressionEvent::EventKind::LedgerXpRebate;
            rebate.EpochId = currentEpoch;
            rebate.SourceKey = pkt.SourceKey;
            rebate.BookId = pkt.BookId;
            rebate.XpDelta = outcome.DuplicateRebateDelta;
            rebate.AuthoritativeTotalXp = outcome.AuthoritativeTotalXp;
            pSender->Send(rebate);
        }
        break;
    }

    case SyncRfabProgressionEvent::EventKind::GlobalValueSync:
    {
        // Isolate personal Divine Shrine blessings, RFAB SaveBlessing flags, and CrimeGold/bounty
        // globals so one player's theft or shrine interaction never contaminates teammates!
        if (RfabWorldImmersion::IsPersonalCrimeOrBlessingGlobal(editorId))
            return;

        if (!m_ledger.UpdateGlobal(editorId, pkt.GlobalValue))
            return;

        NotifyRfabProgressionEvent broadcast{};
        broadcast.Kind = NotifyRfabProgressionEvent::EventKind::GlobalValueSync;
        broadcast.EpochId = currentEpoch;
        broadcast.EditorId = pkt.EditorId;
        broadcast.GlobalValue = pkt.GlobalValue;
        broadcast.AuthoritativeTotalXp = m_ledger.GetTotalSharedXp();
        GameServer::Get()->SendToPlayers(broadcast, pSender);
        break;
    }

    case SyncRfabProgressionEvent::EventKind::AllyProtectionSync:
    {
        // Strictly Host-authoritative: only the authorized Host (Party Leader / Server Admin / first host)
        // may enable or disable Co-Op Ally Protection («Единство в битве») for the group!
        if (!m_world.GetSaveSnapshotService().IsPlayerAuthorizedHost(pSender))
        {
            spdlog::warn(
                "[RFAB AllyProtection] Rejected non-host toggle request from '{}' (id={})",
                pSender->GetUsername().c_str(),
                pSender->GetId());
            SendAllyProtectionStateToPlayer(pSender, false);
            break;
        }

        const bool requestedEnabled = (pkt.GlobalValue >= 0.5f);
        const bool changed = (m_allyProtectionEnabled != requestedEnabled);
        m_allyProtectionEnabled = requestedEnabled;

        spdlog::info(
            "[RFAB AllyProtection] Host '{}' (id={}) set Co-Op Ally Protection = {}",
            pSender->GetUsername().c_str(),
            pSender->GetId(),
            m_allyProtectionEnabled ? "ON (zero ally damage)" : "OFF (ally damage enabled)");

        BroadcastAllyProtectionState(changed || pkt.EditorId == "toggle");
        break;
    }

    case SyncRfabProgressionEvent::EventKind::BossEncounterSync:
    {
        if (sourceKey == "boss_ghost" && pkt.XpDelta != 0)
        {
            const uint32_t bossServerId = static_cast<uint32_t>(pkt.XpDelta);
            const entt::entity bossEntity = static_cast<entt::entity>(bossServerId);
            if (!m_world.valid(bossEntity))
                break;

            const auto* pOwnerComp = m_world.try_get<OwnerComponent>(bossEntity);
            if (!pOwnerComp || pOwnerComp->GetOwner() != pSender)
                break;

            const bool isGhost = (pkt.GlobalValue >= 0.5f);
            if (!m_ledger.UpdateBossGhostState(bossServerId, isGhost))
                break;

            NotifyRfabProgressionEvent broadcast{};
            broadcast.Kind = NotifyRfabProgressionEvent::EventKind::BossEncounterSync;
            broadcast.EpochId = currentEpoch;
            broadcast.SourceKey = pkt.SourceKey;
            broadcast.EditorId = pkt.EditorId;
            broadcast.XpDelta = pkt.XpDelta;
            broadcast.GlobalValue = isGhost ? 1.0f : 0.0f;
            broadcast.AuthoritativeTotalXp = m_ledger.GetTotalSharedXp();
            if (GameServer::Get())
                GameServer::Get()->SendToPlayersInRange(broadcast, bossEntity, pSender);
        }
        else if (sourceKey == "boss_lock" && pkt.XpDelta != 0)
        {
            const uint32_t bossServerId = static_cast<uint32_t>(pkt.XpDelta);
            const entt::entity bossEntity = static_cast<entt::entity>(bossServerId);
            if (!m_world.valid(bossEntity))
                break;

            const auto* pOwnerComp = m_world.try_get<OwnerComponent>(bossEntity);
            if (!pOwnerComp || pOwnerComp->GetOwner() != pSender)
                break;

            const bool locked = (pkt.GlobalValue >= 0.5f);
            m_ledger.SetBossEncounterLocked(bossServerId, locked);
        }
        else if (sourceKey == "arena_unlock" && pkt.BookId)
        {
            NotifyRfabProgressionEvent broadcast{};
            broadcast.Kind = NotifyRfabProgressionEvent::EventKind::BossEncounterSync;
            broadcast.EpochId = currentEpoch;
            broadcast.SourceKey = pkt.SourceKey;
            broadcast.EditorId = pkt.EditorId;
            broadcast.BookId = pkt.BookId;
            broadcast.GlobalValue = pkt.GlobalValue;
            broadcast.AuthoritativeTotalXp = m_ledger.GetTotalSharedXp();
            for (Player* pOther : m_world.GetPlayerManager())
            {
                if (!pOther || pOther == pSender)
                    continue;
                if (pOther->GetCellComponent().IsInRange(pSender->GetCellComponent(), false))
                    pOther->Send(broadcast);
            }
        }
        break;
    }

    case SyncRfabProgressionEvent::EventKind::DifficultyHandshakeSync:
    {
        const auto reported = RfabCoopProgression::UnpackDifficultyHandshake(pkt.XpDelta, pkt.GlobalValue);
        if (!reported.IsValid())
            break;

        if (m_world.GetSaveSnapshotService().IsPlayerAuthorizedHost(pSender) || !m_hostDifficultyState.IsValid())
        {
            m_hostDifficultyState = reported;
            NotifyRfabProgressionEvent hostNotice{};
            hostNotice.Kind = NotifyRfabProgressionEvent::EventKind::DifficultyHandshakeSync;
            hostNotice.EpochId = currentEpoch;
            hostNotice.XpDelta = RfabCoopProgression::PackDifficultyHandshake(m_hostDifficultyState.Tier, m_hostDifficultyState.IsCursed);
            hostNotice.GlobalValue = m_hostDifficultyState.IncomingMult;
            hostNotice.EditorId = String("host");
            hostNotice.AuthoritativeTotalXp = m_ledger.GetTotalSharedXp();
            if (GameServer::Get())
                GameServer::Get()->SendToPlayers(hostNotice, pSender);
        }
        else
        {
            const std::string mismatch = RfabCoopProgression::FormatDifficultyMismatchNotice(m_hostDifficultyState, reported);
            if (!mismatch.empty())
            {
                NotifyRfabProgressionEvent warnPkt{};
                warnPkt.Kind = NotifyRfabProgressionEvent::EventKind::DifficultyHandshakeSync;
                warnPkt.EpochId = currentEpoch;
                warnPkt.SourceKey = String(mismatch.c_str());
                warnPkt.EditorId = String("mismatch");
                warnPkt.XpDelta = RfabCoopProgression::PackDifficultyHandshake(m_hostDifficultyState.Tier, m_hostDifficultyState.IsCursed);
                warnPkt.GlobalValue = m_hostDifficultyState.IncomingMult;
                warnPkt.AuthoritativeTotalXp = m_ledger.GetTotalSharedXp();
                pSender->Send(warnPkt);
            }
        }
        break;
    }

    case SyncRfabProgressionEvent::EventKind::TacticalPingSync:
    {
        const uint64_t nowMs = GameServer::Get() ? GameServer::Get()->GetTick() : 0ULL;
        auto& bucket = m_pingRateByPlayerId[pSender->GetId()];
        if (nowMs == 0 || nowMs < bucket.WindowStartMs || (nowMs - bucket.WindowStartMs) > kTacticalPingRateWindowMs)
        {
            bucket.WindowStartMs = nowMs;
            bucket.CountInWindow = 1;
        }
        else
        {
            ++bucket.CountInWindow;
            if (bucket.CountInWindow > kMaxTacticalPingsPerWindow)
                break;
        }

        RfabCompat::TacticalPingPayload payload{};
        if (!RfabCompat::DeserializeTacticalPingPayload(sourceKey, payload))
            break;

        // Authoritative sender identity stamp (prevents any client from spoofing another player's ping).
        payload.SenderPlayerId = pSender->GetId();
        payload.SenderName = std::string(pSender->GetUsername().c_str(), pSender->GetUsername().size());

        const std::string authoritativeEncoded = RfabCompat::SerializeTacticalPingPayload(payload);

        NotifyRfabProgressionEvent broadcast{};
        broadcast.Kind = NotifyRfabProgressionEvent::EventKind::TacticalPingSync;
        broadcast.EpochId = currentEpoch;
        broadcast.SourceKey = String(authoritativeEncoded.c_str());
        broadcast.EditorId = pkt.EditorId;
        broadcast.BookId = pkt.BookId;
        broadcast.XpDelta = static_cast<int32_t>(pSender->GetId());
        broadcast.GlobalValue = pkt.GlobalValue;
        broadcast.AuthoritativeTotalXp = m_ledger.GetTotalSharedXp();

        auto& partyService = m_world.GetPartyService();
        if (const auto* pParty = partyService.GetPlayerParty(pSender))
        {
            for (Player* pMember : pParty->Members)
            {
                if (pMember && pMember != pSender)
                    pMember->Send(broadcast);
            }
        }
        else if (GameServer::Get())
        {
            GameServer::Get()->SendToPlayers(broadcast, pSender);
        }
        break;
    }

    default:
        // Reserved kinds and anything a newer client may send. Nothing here is acted on:
        //  - LedgerXpGrant (the zero default of the message): no client has ever sent a keyed XP
        //    grant; RFAB XP is shared by SyncRfabExperienceRequest (OnSharedRfabXpRelayed).
        //  - DragonSoulSync: a 4.1.0 client reports every dead dragon it simulates, a corpse it
        //    walked up to included; the server sees the death itself (OnDragonSlain).
        //  - WordWallSync: sharing Words of Power was never wired to a client (M4).
        break;
    }
}

int32_t RfabProgressionService::ResolveAuthorizedHostPlayerId() const noexcept
{
    auto& partyService = m_world.GetPartyService();
    Player* pLowestIdPlayer = nullptr;

    for (Player* pCandidate : m_world.GetPlayerManager())
    {
        if (!pCandidate)
            continue;

        if (partyService.IsPlayerInParty(pCandidate) && partyService.IsPlayerLeader(pCandidate))
            return static_cast<int32_t>(pCandidate->GetId());

        if (!pLowestIdPlayer || pCandidate->GetId() < pLowestIdPlayer->GetId())
            pLowestIdPlayer = pCandidate;
    }

    return pLowestIdPlayer ? static_cast<int32_t>(pLowestIdPlayer->GetId()) : -1;
}

void RfabProgressionService::SendAllyProtectionStateToPlayer(Player* apTarget, bool aNotifyHudChange) const noexcept
{
    if (!apTarget)
        return;

    NotifyRfabProgressionEvent allySync{};
    allySync.Kind = NotifyRfabProgressionEvent::EventKind::AllyProtectionSync;
    allySync.EpochId = m_world.GetSaveSnapshotService().GetCurrentWorldEpoch();
    allySync.XpDelta = ResolveAuthorizedHostPlayerId();
    allySync.GlobalValue = m_allyProtectionEnabled ? 1.0f : 0.0f;
    allySync.EditorId = aNotifyHudChange ? String("toggle") : String("");
    allySync.AuthoritativeTotalXp = m_ledger.GetTotalSharedXp();
    apTarget->Send(allySync);
}

void RfabProgressionService::BroadcastAllyProtectionState(bool aNotifyHudChange) const noexcept
{
    if (!GameServer::Get())
        return;

    NotifyRfabProgressionEvent allySync{};
    allySync.Kind = NotifyRfabProgressionEvent::EventKind::AllyProtectionSync;
    allySync.EpochId = m_world.GetSaveSnapshotService().GetCurrentWorldEpoch();
    allySync.XpDelta = ResolveAuthorizedHostPlayerId();
    allySync.GlobalValue = m_allyProtectionEnabled ? 1.0f : 0.0f;
    allySync.EditorId = aNotifyHudChange ? String("toggle") : String("");
    allySync.AuthoritativeTotalXp = m_ledger.GetTotalSharedXp();
    GameServer::Get()->SendToPlayers(allySync, nullptr);
}

void RfabProgressionService::OnPlayerJoin(const PlayerJoinEvent& acEvent) noexcept
{
    Player* pNewPlayer = acEvent.pPlayer;
    if (!pNewPlayer)
        return;

    const uint64_t currentEpoch = m_world.GetSaveSnapshotService().GetCurrentWorldEpoch();

    // Sync current WorldEpoch even if no books/globals have been recorded yet.
    NotifyRfabProgressionEvent epochMarker{};
    epochMarker.Kind = NotifyRfabProgressionEvent::EventKind::LedgerXpGrant;
    epochMarker.EpochId = currentEpoch;
    epochMarker.XpDelta = 0;
    epochMarker.AuthoritativeTotalXp = m_ledger.GetTotalSharedXp();
    pNewPlayer->Send(epochMarker);

    // Sync all already-read XP books so the newly joined player adds them to XPBooksReaded immediately.
    for (const auto& bookId : m_ledger.GetReadBooks())
    {
        NotifyRfabProgressionEvent bookSync{};
        bookSync.Kind = NotifyRfabProgressionEvent::EventKind::BookRead;
        bookSync.EpochId = currentEpoch;
        bookSync.BookId = bookId;
        bookSync.XpDelta = 0; // Do not re-grant historical XP bursts; sync the FormList guard cleanly.
        bookSync.AuthoritativeTotalXp = m_ledger.GetTotalSharedXp();
        pNewPlayer->Send(bookSync);
    }

    // Sync all authoritative RFAB TESGlobals (counters, boss phases, unlock triggers).
    for (const auto& [editorId, val] : m_ledger.GetSyncedGlobals())
    {
        NotifyRfabProgressionEvent globalSync{};
        globalSync.Kind = NotifyRfabProgressionEvent::EventKind::GlobalValueSync;
        globalSync.EpochId = currentEpoch;
        globalSync.EditorId = String(editorId.c_str());
        globalSync.GlobalValue = val;
        globalSync.AuthoritativeTotalXp = m_ledger.GetTotalSharedXp();
        pNewPlayer->Send(globalSync);
    }

    // Sync active boss invulnerable/ethereal phase states (SetGhost) for ongoing encounters.
    for (const auto& [bossServerId, isGhost] : m_ledger.GetBossGhostStates())
    {
        if (!isGhost)
            continue;
        NotifyRfabProgressionEvent bossSync{};
        bossSync.Kind = NotifyRfabProgressionEvent::EventKind::BossEncounterSync;
        bossSync.EpochId = currentEpoch;
        bossSync.SourceKey = String("boss_ghost");
        bossSync.XpDelta = static_cast<int32_t>(bossServerId);
        bossSync.GlobalValue = 1.0f;
        bossSync.AuthoritativeTotalXp = m_ledger.GetTotalSharedXp();
        pNewPlayer->Send(bossSync);
    }

    // Send Host's RFAB_StartMenu Difficulty & Cursed state for handshake validation.
    if (m_hostDifficultyState.IsValid())
    {
        NotifyRfabProgressionEvent diffSync{};
        diffSync.Kind = NotifyRfabProgressionEvent::EventKind::DifficultyHandshakeSync;
        diffSync.EpochId = currentEpoch;
        diffSync.EditorId = String("host");
        diffSync.XpDelta = RfabCoopProgression::PackDifficultyHandshake(m_hostDifficultyState.Tier, m_hostDifficultyState.IsCursed);
        diffSync.GlobalValue = m_hostDifficultyState.IncomingMult;
        diffSync.AuthoritativeTotalXp = m_ledger.GetTotalSharedXp();
        pNewPlayer->Send(diffSync);
    }

    // Catch-up XP: what the party earned while this player was away, counted per persistent
    // identity. A reconnect, a second join event or a server restart finds the identity level with
    // the party and pays nothing (the tracker this replaces was keyed by the connection id).
    {
        const std::string identityKey = SaveSnapshotService::GetPlayerIdentityKey(pNewPlayer);
        const auto catchUp = m_ledger.GetXpCredits().EvaluateJoin(
            identityKey, m_ledger.GetTotalSharedXp(), bEnableLateJoinCatchUpXp);
        if (catchUp.ShouldGrant)
        {
            NotifyRfabProgressionEvent catchUpPkt{};
            catchUpPkt.Kind = NotifyRfabProgressionEvent::EventKind::LateJoinCatchUpXp;
            catchUpPkt.EpochId = currentEpoch;
            catchUpPkt.SourceKey = String("late_join_catchup");
            catchUpPkt.XpDelta = catchUp.GrantXpDelta;
            catchUpPkt.AuthoritativeTotalXp = m_ledger.GetTotalSharedXp();
            pNewPlayer->Send(catchUpPkt);
            spdlog::info(
                "[RFAB Progression] Catch-up XP +{} for '{}' ({} forfeited above the cap)",
                catchUp.GrantXpDelta, identityKey, catchUp.ForfeitedXp);
        }
    }

    // Sync authoritative Host-controlled Co-Op Ally Protection («Единство в битве») state & Host ID.
    SendAllyProtectionStateToPlayer(pNewPlayer, false);
}

void RfabProgressionService::OnSharedRfabXpRelayed(const uint32_t aBaseXp) noexcept
{
    // The relay carries an amount and no source, so nothing can be deduplicated here: the party
    // total grows by what was relayed, and everyone connected holds it (the sender earned it, the
    // others were sent it). Only a player who is away misses it.
    const auto clampedXp = static_cast<int32_t>(std::min<uint32_t>(aBaseXp, static_cast<uint32_t>(kMaxSingleXpGrant)));
    const auto outcome = m_ledger.TryGrantXp({}, clampedXp);
    if (outcome.Accepted)
        CreditConnectedPlayers(outcome.AppliedDelta, outcome.AuthoritativeTotalXp);
}

void RfabProgressionService::OnDragonSlain(const entt::entity aDragon, Player* apKiller) noexcept
{
    if (!apKiller || !m_world.valid(aDragon))
        return;

    GameId dragonFormId{};
    if (const auto* pFormId = m_world.try_get<FormIdComponent>(aDragon))
        dragonFormId = pFormId->Id;
    const std::string dragonKey =
        RfabCoopProgression::DragonSoulLedger::MakeDragonKey(dragonFormId, World::ToInteger(aDragon));
    if (dragonKey.empty())
        return;

    // Everyone who has the dragon loaded absorbs its soul the way the game always did: the dragon's
    // own script waits for that client's player to walk up to the corpse (dragonActorSCRIPT,
    // state deadAndWaiting). The shared soul is for party members who were somewhere else, and
    // without the dragon's cell nobody can be told apart.
    std::vector<Player*> absentMembers;
    if (const auto* pDragonCell = m_world.try_get<CellIdComponent>(aDragon))
    {
        const auto consider = [&](Player* apMember) {
            if (apMember && apMember != apKiller && !pDragonCell->IsInRange(apMember->GetCellComponent(), true))
                absentMembers.push_back(apMember);
        };

        if (const auto* pParty = m_world.GetPartyService().GetPlayerParty(apKiller))
        {
            for (Player* pMember : pParty->Members)
                consider(pMember);
        }
        else
        {
            for (Player* pOther : m_world.GetPlayerManager())
                consider(pOther);
        }
    }

    // The kill is recorded whether or not souls are shared (it is part of the checkpoint).
    (void)m_ledger.GetDragonSoulLedger().RecordDragonKill(dragonKey, apKiller->GetId(), {}, false);

    if (!bEnableSharedDragonSouls || absentMembers.empty())
        return;

    NotifyRfabProgressionEvent soulGrant{};
    soulGrant.Kind = NotifyRfabProgressionEvent::EventKind::DragonSoulSync;
    soulGrant.EpochId = m_world.GetSaveSnapshotService().GetCurrentWorldEpoch();
    soulGrant.SourceKey = String(dragonKey.c_str());
    soulGrant.BookId = dragonFormId;
    soulGrant.XpDelta = 1; // souls to add
    soulGrant.AuthoritativeTotalXp = m_ledger.GetTotalSharedXp();

    for (Player* pAbsent : absentMembers)
        pAbsent->Send(soulGrant);

    spdlog::info("[RFAB Progression] Dragon {} slain by player {}: shared soul sent to {} absent party member(s)",
                 dragonKey, apKiller->GetId(), absentMembers.size());
}

void RfabProgressionService::CreditConnectedPlayers(int32_t aXpDelta, int64_t aTotalAfterGrant) noexcept
{
    if (aXpDelta <= 0)
        return;

    // The sender granted the XP locally and everyone else receives the broadcast, so every
    // connected player holds it now. Only players who are away are owed it later.
    m_world.GetPlayerManager().ForEach([&](const Player* apConnected) {
        m_ledger.GetXpCredits().CreditConnected(
            SaveSnapshotService::GetPlayerIdentityKey(apConnected), aXpDelta, aTotalAfterGrant);
    });
}

void RfabProgressionService::OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept
{
    if (acEvent.pPlayer)
        m_pingRateByPlayerId.erase(acEvent.pPlayer->GetId());
    BroadcastAllyProtectionState(false);
}
