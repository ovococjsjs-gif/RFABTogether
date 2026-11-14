#include <TiltedOnlinePCH.h>

#include <Services/RfabAdapterService.h>

#include <Actor.h>
#include <BSGraphics/BSGraphicsRenderer.h>
#include <Events/PartyJoinedEvent.h>
#include <Events/PartyLeftEvent.h>
#include <Events/UpdateEvent.h>
#include <Forms/ActorValueInfo.h>
#include <Games/Overrides.h>
#include <Games/References.h>
#include <Messages/NotifyPlayerCellChanged.h>
#include <Messages/NotifyPlayerHealthUpdate.h>
#include <Messages/NotifyPlayerJoined.h>
#include <Messages/NotifyPlayerLeft.h>
#include <Messages/NotifyPlayerLevel.h>
#include <Messages/NotifyPlayerList.h>
#include <Messages/NotifyRfabProgressionEvent.h>
#include <Messages/SyncRfabProgressionEvent.h>
#include <OverlayApp.hpp>
#include <PlayerCharacter.h>
#include <Services/CharacterService.h>
#include <Services/ImguiService.h>
#include <Services/OverlayService.h>
#include <Services/PartyService.h>
#include <Services/TransportService.h>
#include <Utils.h>
#include <World.h>
#include <common/RfabCoopProgression.h>
#include <imgui.h>

RfabAdapterService::RfabAdapterService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld)
    , m_dispatcher(aDispatcher)
    , m_transport(aTransport)
    , m_partyJoinedConnection(aDispatcher.sink<PartyJoinedEvent>().connect<&RfabAdapterService::OnPartyJoined>(this))
    , m_partyLeftConnection(aDispatcher.sink<PartyLeftEvent>().connect<&RfabAdapterService::OnPartyLeft>(this))
    , m_updateConnection(aDispatcher.sink<UpdateEvent>().connect<&RfabAdapterService::OnUpdate>(this))
    , m_progressionConnection(aDispatcher.sink<NotifyRfabProgressionEvent>().connect<&RfabAdapterService::OnNotifyProgressionEvent>(this))
    , m_playerJoinedConnection(aDispatcher.sink<NotifyPlayerJoined>().connect<&RfabAdapterService::OnPlayerJoined>(this))
    , m_playerLeftConnection(aDispatcher.sink<NotifyPlayerLeft>().connect<&RfabAdapterService::OnPlayerLeft>(this))
    , m_playerLevelConnection(aDispatcher.sink<NotifyPlayerLevel>().connect<&RfabAdapterService::OnPlayerLevel>(this))
    , m_playerCellChangedConnection(aDispatcher.sink<NotifyPlayerCellChanged>().connect<&RfabAdapterService::OnPlayerCellChanged>(this))
    , m_playerHealthUpdateConnection(aDispatcher.sink<NotifyPlayerHealthUpdate>().connect<&RfabAdapterService::OnPlayerHealthUpdate>(this))
    , m_playerListConnection(aDispatcher.sink<NotifyPlayerList>().connect<&RfabAdapterService::OnPlayerList>(this))
    , m_drawImGuiConnection(aWorld.ctx().at<ImguiService>().OnDraw.connect<&RfabAdapterService::OnDrawTacticalHud>(this))
{
    RefreshRuntimeBindings();
}

RfabAdapterService::~RfabAdapterService() noexcept = default;

bool RfabAdapterService::IsTacticalHudFeatureEnabled() noexcept
{
    return World::IsFeatureEnabled(Session::Feature::kTacticalHud);
}

bool RfabAdapterService::IsCombatRulesFeatureEnabled() noexcept
{
    return World::IsFeatureEnabled(Session::Feature::kRfabCombatRules);
}

void RfabAdapterService::ApplyAllyHitPolicy(bool aProtectAllies) noexcept
{
    m_combatRules.ApplyAllyHitGameSetting(aProtectAllies && IsCombatRulesFeatureEnabled());
}

void RfabAdapterService::ApplyFeatureSwitchChanges() noexcept
{
    const bool combatRules = IsCombatRulesFeatureEnabled();
    if (combatRules != m_combatRulesFeatureApplied)
    {
        m_combatRulesFeatureApplied = combatRules;
        ApplyAllyHitPolicy(m_allyProtectionEnabled.load(std::memory_order_relaxed));
        if (!combatRules)
            m_combatRules.ResetBossTracking();
        RefreshCoopCombatSnapshot();
        PushAllyProtectionStateToOverlay();
    }

    const bool hud = IsTacticalHudFeatureEnabled();
    if (hud != m_tacticalHudFeatureApplied)
    {
        m_tacticalHudFeatureApplied = hud;
        if (!hud)
        {
            m_hudAndPings.ResetOnDisconnect();
            ClearTacticalHudSnapshot();
        }
    }
}

uint32_t RfabAdapterService::ResolveEffectiveLocalPlayerId() const noexcept
{
    return m_hasLocalPlayerId ? m_localPlayerId : m_transport.GetLocalPlayerId();
}

void RfabAdapterService::RefreshRuntimeBindings() noexcept
{
    m_formResolver.RefreshRuntimeBindings();
    m_pNoAllyDamageAllowedSpellsList.store(nullptr, std::memory_order_release);
    m_xpAndBookSync.ResetRuntimeWatches();
    m_combatRules.ResetBossTracking();
    m_hudAndPings.ApplyDefaultModeFromBindings(m_formResolver.GetBindings());
}

Mod* RfabAdapterService::ResolveRfabPluginMod() const noexcept
{
    return m_formResolver.ResolveRfabPluginMod();
}

TESForm* RfabAdapterService::ResolveFormByEditorIdOrBaseId(
    const std::string& acEditorId,
    uint32_t aFallbackBaseId) const noexcept
{
    return m_formResolver.ResolveFormByEditorIdOrBaseId(acEditorId, aFallbackBaseId);
}

TESGlobal* RfabAdapterService::ResolveGlobalByEditorId(
    const std::string& acEditorId,
    uint32_t aFallbackBaseId) noexcept
{
    return m_formResolver.ResolveGlobalByEditorId(acEditorId, aFallbackBaseId);
}

BGSListForm* RfabAdapterService::ResolveFormListByEditorId(const std::string& acEditorId) noexcept
{
    return m_formResolver.ResolveFormListByEditorId(acEditorId);
}

TESGlobal* RfabAdapterService::GetRfabXpGlobal() noexcept
{
    return m_formResolver.GetRfabXpGlobal();
}

float RfabAdapterService::GetLocalXpMultiplier() noexcept
{
    return m_formResolver.GetLocalXpMultiplier();
}

int32_t RfabAdapterService::ReadRfabXp(TESGlobal* apGlobal) noexcept
{
    return RfabXpAndBookSync::ReadRfabXp(apGlobal);
}

bool RfabAdapterService::ApplyDualPoolExperienceDelta(int32_t aDelta, int32_t& aOutNewTotalXp) noexcept
{
    return RfabXpAndBookSync::ApplyDualPoolExperienceDelta(GetRfabXpGlobal(), aDelta, aOutNewTotalXp);
}

BGSListForm* RfabAdapterService::GetNoAllyDamageAllowedSpellsList() noexcept
{
    if (BGSListForm* pCached = m_pNoAllyDamageAllowedSpellsList.load(std::memory_order_acquire))
        return pCached;

    BGSListForm* pResolved = m_formResolver.ResolveNoAllyDamageAllowedSpellsList();
    if (pResolved)
        m_pNoAllyDamageAllowedSpellsList.store(pResolved, std::memory_order_release);
    return pResolved;
}

bool RfabAdapterService::IsLocalPlayerAllyProtectionHost() const noexcept
{
    if (!m_transport.IsConnected())
        return false;

    const auto& partySvc = m_world.GetPartyService();
    if (partySvc.IsInParty())
        return partySvc.IsLeader();

    return m_isServerHost;
}

void RfabAdapterService::SendHostAllyProtectionRequest(bool aEnabled, bool aNotifyPeers) const noexcept
{
    if (!m_transport.IsConnected())
        return;

    SyncRfabProgressionEvent ev{};
    ev.Kind = SyncRfabProgressionEvent::EventKind::AllyProtectionSync;
    ev.EpochId = m_xpAndBookSync.GetCurrentWorldEpoch();
    ev.EditorId = aNotifyPeers ? String("toggle") : String("");
    ev.GlobalValue = aEnabled ? 1.0f : 0.0f;
    m_transport.Send(ev);
}

void RfabAdapterService::PushAllyProtectionStateToOverlay() const noexcept
{
    if (auto* pOverlayApp = m_world.GetOverlayService().GetOverlayApp())
    {
        auto pArgs = CefListValue::Create();
        pArgs->SetBool(0, m_allyProtectionEnabled.load(std::memory_order_relaxed));
        pArgs->SetBool(1, IsLocalPlayerAllyProtectionHost());
        pOverlayApp->ExecuteAsync("allyProtectionState", pArgs);
    }
}

void RfabAdapterService::NotifyPartyLeadershipChanged() noexcept
{
    RefreshCoopCombatSnapshot();
    PushAllyProtectionStateToOverlay();
}

void RfabAdapterService::RefreshCoopCombatSnapshot() noexcept
{
    auto spNext = std::make_shared<CoopCombatSnapshot>();
    RfabCombatRules::BuildCombatSnapshot(
        m_world,
        m_transport,
        m_allyProtectionEnabled.load(std::memory_order_relaxed),
        *spNext);

    std::unique_lock<std::shared_mutex> lock(m_coopSnapshotMutex);
    m_coopCombatSnapshot = std::move(spNext);
}

GamePtr<Actor> RfabAdapterService::FindBestStandingPlayerTargetFor(Actor* apAttacker) const noexcept
{
    if (!apAttacker || !IsCombatRulesFeatureEnabled())
        return nullptr;

    std::shared_ptr<const CoopCombatSnapshot> spSnapshot;
    {
        std::shared_lock<std::shared_mutex> lock(m_coopSnapshotMutex);
        spSnapshot = m_coopCombatSnapshot;
    }
    if (!spSnapshot || !spSnapshot->Connected)
        return nullptr;

    return RfabCombatRules::SelectBestStandingTarget(*spSnapshot, apAttacker);
}

void RfabAdapterService::CollectPartyPeerActorFormIds(TiltedPhoques::Vector<uint32_t>& aOutFormIds) const noexcept
{
    std::shared_ptr<const CoopCombatSnapshot> spSnapshot;
    {
        std::shared_lock<std::shared_mutex> lock(m_coopSnapshotMutex);
        spSnapshot = m_coopCombatSnapshot;
    }
    if (!spSnapshot)
    {
        aOutFormIds.clear();
        return;
    }
    RfabCombatRules::CollectPartyPeerActorFormIds(*spSnapshot, aOutFormIds);
}

void RfabAdapterService::SetLocalAllyProtectionEnabled(bool aEnabled, bool aShowHudNotification) noexcept
{
    if (!m_transport.IsConnected())
    {
        PushAllyProtectionStateToOverlay();
        return;
    }

    if (!IsCombatRulesFeatureEnabled())
    {
        if (aShowHudNotification)
            Utils::ShowHudMessage(String("RFAB Online: боевые правила RFAB отключены на сервере"));
        PushAllyProtectionStateToOverlay();
        return;
    }

    if (!IsLocalPlayerAllyProtectionHost())
    {
        if (aShowHudNotification)
            Utils::ShowHudMessage(String("RFAB Online: Только Хост может включать и выключать урон по союзникам"));
        PushAllyProtectionStateToOverlay();
        return;
    }

    m_allyProtectionEnabled.store(aEnabled, std::memory_order_relaxed);
    ApplyAllyHitPolicy(aEnabled);
    RefreshCoopCombatSnapshot();
    SendHostAllyProtectionRequest(aEnabled, aShowHudNotification);
    PushAllyProtectionStateToOverlay();
}

void RfabAdapterService::ToggleLocalAllyProtection() noexcept
{
    SetLocalAllyProtectionEnabled(!m_allyProtectionEnabled.load(std::memory_order_relaxed), true);
}

RfabCompat::CoopAllyActionPolicy RfabAdapterService::EvaluateCoopAction(
    Actor* apAttacker,
    Actor* apTarget,
    bool aIsBeneficialHealOrBuff) const noexcept
{
    if (!apAttacker || !apTarget || apAttacker == apTarget || !IsCombatRulesFeatureEnabled())
        return {};

    // Read a COW shared_ptr snapshot under shared_lock instead of deep-copying maps/vectors
    // on every physical/magic hit across Skyrim's combat worker threads.
    std::shared_ptr<const CoopCombatSnapshot> spSnapshot;
    {
        std::shared_lock<std::shared_mutex> lock(m_coopSnapshotMutex);
        spSnapshot = m_coopCombatSnapshot;
    }
    if (!spSnapshot || !spSnapshot->Connected || !spSnapshot->InParty)
        return {};

    return RfabCombatRules::EvaluateSnapshotAction(*spSnapshot, apAttacker, apTarget, aIsBeneficialHealOrBuff);
}

void RfabAdapterService::OnSessionBegin(const Session::Transition& acTransition) noexcept
{
    RefreshRuntimeBindings();
    m_localPlayerId = acTransition.LocalPlayerId;
    m_hasLocalPlayerId = true;
    m_isServerHost = false;
    m_allyProtectionEnabled.store(true, std::memory_order_relaxed);
    // Settings (and with them the feature mask) are applied before the session begins.
    m_combatRulesFeatureApplied = IsCombatRulesFeatureEnabled();
    m_tacticalHudFeatureApplied = IsTacticalHudFeatureEnabled();
    ApplyAllyHitPolicy(true);
    (void)GetNoAllyDamageAllowedSpellsList();
    RefreshCoopCombatSnapshot();
    PushAllyProtectionStateToOverlay();

    m_xpAndBookSync.ApplySessionNoDeathHandling(m_formResolver);
    m_xpAndBookSync.SeedReadBooksOnConnect(m_formResolver);
    SendLocalDifficultyHandshake();
}

void RfabAdapterService::OnSessionEnd(const Session::Transition&) noexcept
{
    m_xpAndBookSync.ResetOnDisconnect();
    m_combatRules.ApplyAllyHitGameSetting(false);
    m_combatRules.ResetBossTracking();
    m_hudAndPings.ResetOnDisconnect();
    ClearTacticalHudSnapshot();

    m_hasLocalPlayerId = false;
    m_isServerHost = false;
    m_allyProtectionEnabled.store(true, std::memory_order_relaxed);
    RefreshCoopCombatSnapshot();
    PushAllyProtectionStateToOverlay();
}

void RfabAdapterService::OnPartyJoined(const PartyJoinedEvent&) noexcept
{
    RefreshCoopCombatSnapshot();
    PushAllyProtectionStateToOverlay();
}

void RfabAdapterService::OnPartyLeft(const PartyLeftEvent&) noexcept
{
    RefreshCoopCombatSnapshot();
    PushAllyProtectionStateToOverlay();
}

void RfabAdapterService::OnWorldLoadBegin(const Session::Transition&) noexcept
{
    m_xpAndBookSync.ResetRuntimeWatches();
    // Allow 2.0s post-load settle window so BGSSaveLoadManager finishes restoring ESS state
    // and authoritative server resync packets land before we poll local FormLists/TESGlobals again.
    m_pollAccumulatorSec = -2.0;
    m_snapshotAccumulatorSec = 0.0;
}

void RfabAdapterService::OnWorldLoadEnd(const Session::Transition& acTransition) noexcept
{
    if (!acTransition.Online || !acTransition.Succeeded)
        return;

    // Immediately re-enforce REQ_Combat_NoDeathHandling == 1.0 as soon as the save finishes loading,
    // closing the 2.0s post-load window where Requiem's death script could trigger a solo reload.
    m_xpAndBookSync.EnforceNoDeathHandlingTick(m_formResolver);
    RefreshCoopCombatSnapshot();
    SendLocalDifficultyHandshake();
}

void RfabAdapterService::SendLocalDifficultyHandshake() noexcept
{
    if (!m_transport.IsConnected() || !Settings::IsRfabModpackActive())
        return;

    const auto localDiff = RfabCoopProgression::InferDifficultyFromGmst(
        Settings::GetRfabOutgoingDamageMult(),
        Settings::GetRfabIncomingDamageMult());
    if (!localDiff.IsValid())
        return;

    SyncRfabProgressionEvent ev{};
    ev.Kind = SyncRfabProgressionEvent::EventKind::DifficultyHandshakeSync;
    ev.EpochId = m_xpAndBookSync.GetCurrentWorldEpoch();
    ev.XpDelta = RfabCoopProgression::PackDifficultyHandshake(localDiff.Tier, localDiff.IsCursed);
    ev.GlobalValue = localDiff.IncomingMult;
    m_transport.Send(ev);
}

void RfabAdapterService::SelfHealFreeRoamMenuControls() noexcept
{
    RfabHudAndPings::SelfHealFreeRoamMenuControls(m_world);
}

void RfabAdapterService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    m_controlMapHealAccumulatorSec += acEvent.Delta;
    if (m_controlMapHealAccumulatorSec >= 1.0)
    {
        m_controlMapHealAccumulatorSec = 0.0;
        SelfHealFreeRoamMenuControls();
    }

    if (!m_transport.IsConnected())
    {
        ClearTacticalHudSnapshot();
        return;
    }

    ApplyFeatureSwitchChanges();

    if (m_tacticalHudFeatureApplied && m_hudAndPings.UpdatePingsAndInput(
            m_world,
            m_transport,
            m_formResolver,
            m_combatRules,
            ResolveEffectiveLocalPlayerId(),
            m_xpAndBookSync.GetCurrentWorldEpoch(),
            acEvent.Delta))
    {
        ClearTacticalHudSnapshot();
    }

    // Throttle periodic combat/HUD snapshot rebuilds to 10 Hz (immediate refreshes still fire on
    // session begin/end, world load end, party join/leave, ally-protection toggles, and player spawn).
    m_snapshotAccumulatorSec += acEvent.Delta;
    if (m_snapshotAccumulatorSec >= 0.1)
    {
        m_snapshotAccumulatorSec = 0.0;
        RefreshCoopCombatSnapshot();
        if (m_tacticalHudFeatureApplied)
            RefreshTacticalHudSnapshot();
    }

    m_pollAccumulatorSec += acEvent.Delta;
    if (m_pollAccumulatorSec < 0.5)
        return;
    m_pollAccumulatorSec = 0.0;

    m_xpAndBookSync.EnforceNoDeathHandlingTick(m_formResolver);
    PollReadXpBooks();
    PollSyncedGlobals();
    if (m_combatRulesFeatureApplied)
        PollBossEncounterState();
}

void RfabAdapterService::PollReadXpBooks() noexcept
{
    m_xpAndBookSync.PollReadXpBooks(m_world, m_transport, m_formResolver);
}

void RfabAdapterService::PollSyncedGlobals() noexcept
{
    m_xpAndBookSync.PollSyncedGlobals(m_transport, m_formResolver);
}

bool RfabAdapterService::IsBossActor(Actor* apActor) const noexcept
{
    return m_combatRules.IsBossActor(apActor, m_formResolver);
}

void RfabAdapterService::BroadcastArenaBarrierUnlock(const GameId& acObjectId) const noexcept
{
    if (!IsCombatRulesFeatureEnabled())
        return;

    if (!World::IsMainThread())
    {
        const GameId capturedObjectId = acObjectId;
        m_world.GetRunner().QueueForThisWorld([this, capturedObjectId]() noexcept {
            BroadcastArenaBarrierUnlock(capturedObjectId);
        });
        return;
    }

    RfabCombatRules::BroadcastArenaBarrierUnlock(m_transport, acObjectId, m_xpAndBookSync.GetCurrentWorldEpoch());
}

void RfabAdapterService::PollBossEncounterState() noexcept
{
    m_combatRules.PollBossEncounterState(
        m_world, m_transport, m_formResolver, m_xpAndBookSync.GetCurrentWorldEpoch());
}

void RfabAdapterService::OnNotifyProgressionEvent(const NotifyRfabProgressionEvent& acMessage) noexcept
{
    m_xpAndBookSync.AdvanceWorldEpochIfNewer(acMessage.EpochId);

    switch (acMessage.Kind)
    {
    case NotifyRfabProgressionEvent::EventKind::BookRead:
        m_xpAndBookSync.ApplyBookReadEvent(m_world, m_formResolver, acMessage);
        break;

    case NotifyRfabProgressionEvent::EventKind::LedgerXpGrant:
    case NotifyRfabProgressionEvent::EventKind::LedgerXpRebate:
        if (acMessage.XpDelta != 0)
            m_world.GetCharacterService().ApplyAuthoritativeRfabXpDelta(acMessage.XpDelta);
        break;

    case NotifyRfabProgressionEvent::EventKind::GlobalValueSync:
        m_xpAndBookSync.ApplyGlobalValueSyncEvent(m_formResolver, acMessage);
        break;

    case NotifyRfabProgressionEvent::EventKind::AllyProtectionSync:
    {
        if (!IsCombatRulesFeatureEnabled())
            break;

        const bool newState = (acMessage.GlobalValue >= 0.5f);
        m_allyProtectionEnabled.store(newState, std::memory_order_relaxed);

        if (m_hasLocalPlayerId && acMessage.XpDelta >= 0)
            m_isServerHost = (static_cast<uint32_t>(acMessage.XpDelta) == m_localPlayerId);
        else
            m_isServerHost = false;

        ApplyAllyHitPolicy(newState);
        RefreshCoopCombatSnapshot();
        PushAllyProtectionStateToOverlay();

        if (acMessage.EditorId == "toggle")
        {
            if (newState)
                Utils::ShowHudMessage(String("Единство в битве (Кооп): урон по союзникам отключен"));
            else
                Utils::ShowHudMessage(String("Единство в битве (Кооп): урон по союзникам включен"));
        }
        break;
    }

    case NotifyRfabProgressionEvent::EventKind::BossEncounterSync:
        if (IsCombatRulesFeatureEnabled())
            m_combatRules.ApplyBossEncounterSyncEvent(m_world, acMessage);
        break;

    case NotifyRfabProgressionEvent::EventKind::TacticalPingSync:
        if (IsTacticalHudFeatureEnabled())
            m_hudAndPings.ApplyIncomingPingSyncEvent(acMessage, m_formResolver.GetBindings());
        break;

    case NotifyRfabProgressionEvent::EventKind::DragonSoulSync:
    {
        // XpDelta is the number of souls to ADD. The server sends 1; anything else is not a grant.
        constexpr int32_t kMaxSoulsPerGrant = 3;
        if (acMessage.XpDelta <= 0 || acMessage.XpDelta > kMaxSoulsPerGrant)
            break;
        if (PlayerCharacter* pPlayer = PlayerCharacter::Get())
        {
            // ForceActorValue takes the TARGET value, so the grant is "what I have now, plus the
            // delta". Passing the delta alone used to set the soul count to 1.
            const float currentSouls = pPlayer->GetActorPermanentValue(ActorValueInfo::kDragonSouls);
            const float targetSouls = (currentSouls > 0.0f ? currentSouls : 0.0f) + static_cast<float>(acMessage.XpDelta);
            ScopedActorValueOverride avGuard;
            pPlayer->ForceActorValue(ActorValueOwner::ForceMode::PERMANENT, ActorValueInfo::kDragonSouls, targetSouls);
            spdlog::info("[RFAB Progression] Shared dragon soul: {} -> {}", currentSouls, targetSouls);
            Utils::ShowHudMessage(String("RFAB Co-Op: +1 душа дракона (общая награда группы)"));
        }
        break;
    }

    case NotifyRfabProgressionEvent::EventKind::DifficultyHandshakeSync:
    {
        if (acMessage.EditorId == "mismatch" && !acMessage.SourceKey.empty())
        {
            Utils::ShowHudMessage(acMessage.SourceKey);
            break;
        }
        if (acMessage.EditorId == "host" && Settings::IsRfabModpackActive())
        {
            const auto hostDiff = RfabCoopProgression::UnpackDifficultyHandshake(acMessage.XpDelta, acMessage.GlobalValue);
            const auto localDiff = RfabCoopProgression::InferDifficultyFromGmst(
                Settings::GetRfabOutgoingDamageMult(),
                Settings::GetRfabIncomingDamageMult());
            const std::string notice = RfabCoopProgression::FormatDifficultyMismatchNotice(hostDiff, localDiff);
            if (!notice.empty())
                Utils::ShowHudMessage(String(notice.c_str()));
        }
        break;
    }

    case NotifyRfabProgressionEvent::EventKind::LateJoinCatchUpXp:
    {
        if (acMessage.XpDelta > 0)
        {
            m_world.GetCharacterService().ApplyAuthoritativeRfabXpDelta(acMessage.XpDelta);
            Utils::ShowHudMessage(String(fmt::format("RFAB Co-Op: начислено +{} XP (догон группы)", acMessage.XpDelta).c_str()));
        }
        break;
    }

    default:
        // Reserved kinds (FullLedgerResync, WordWallSync) and kinds of a newer server: nothing to do.
        break;
    }
}

void RfabAdapterService::OnPlayerJoined(const NotifyPlayerJoined& acMessage) noexcept
{
    m_hudAndPings.OnPlayerJoined(m_world, acMessage);
}

void RfabAdapterService::OnPlayerLeft(const NotifyPlayerLeft& acMessage) noexcept
{
    m_hudAndPings.OnPlayerLeft(acMessage);
}

void RfabAdapterService::OnPlayerLevel(const NotifyPlayerLevel& acMessage) noexcept
{
    m_hudAndPings.OnPlayerLevel(acMessage);
}

void RfabAdapterService::OnPlayerCellChanged(const NotifyPlayerCellChanged& acMessage) noexcept
{
    m_hudAndPings.OnPlayerCellChanged(m_world, acMessage);
}

void RfabAdapterService::OnPlayerHealthUpdate(const NotifyPlayerHealthUpdate& acMessage) noexcept
{
    m_hudAndPings.OnPlayerHealthUpdate(acMessage);
}

void RfabAdapterService::OnPlayerList(const NotifyPlayerList& acMessage) noexcept
{
    m_hudAndPings.OnPlayerList(acMessage);
}

void RfabAdapterService::SetTacticalHudMode(RfabCompat::TacticalHudMode aMode, bool aShowNotice) noexcept
{
    m_hudAndPings.SetMode(aMode, aShowNotice);
    if (aMode == RfabCompat::TacticalHudMode::Hidden)
        ClearTacticalHudSnapshot();
}

void RfabAdapterService::ClearLocalTacticalPings() noexcept
{
    if (!IsTacticalHudFeatureEnabled())
        return;

    m_hudAndPings.ClearLocalPings(
        m_transport, ResolveEffectiveLocalPlayerId(), m_xpAndBookSync.GetCurrentWorldEpoch());
}

void RfabAdapterService::TriggerContextualTacticalPing(bool aForceDanger, const std::string& acCustomLabel) noexcept
{
    if (!IsTacticalHudFeatureEnabled())
        return;

    m_hudAndPings.TriggerContextualPing(
        m_world,
        m_transport,
        m_formResolver,
        m_combatRules,
        ResolveEffectiveLocalPlayerId(),
        m_xpAndBookSync.GetCurrentWorldEpoch(),
        aForceDanger,
        acCustomLabel);
}

void RfabAdapterService::TriggerExplicitTacticalPing(
    RfabCompat::TacticalPingKind aKind,
    const std::string& acCustomLabel) noexcept
{
    if (!IsTacticalHudFeatureEnabled())
        return;

    m_hudAndPings.TriggerExplicitPing(
        m_world,
        m_transport,
        m_formResolver,
        m_combatRules,
        ResolveEffectiveLocalPlayerId(),
        m_xpAndBookSync.GetCurrentWorldEpoch(),
        aKind,
        acCustomLabel);
}

void RfabAdapterService::ExecuteTacticalChatCommand(const RfabCompat::TacticalChatCommand& acCmd) noexcept
{
    if (!IsTacticalHudFeatureEnabled())
        return;

    bool shouldClearSnapshot = false;
    if (m_hudAndPings.ExecuteChatCommand(
            m_world,
            m_transport,
            m_formResolver,
            m_combatRules,
            ResolveEffectiveLocalPlayerId(),
            m_xpAndBookSync.GetCurrentWorldEpoch(),
            acCmd,
            shouldClearSnapshot) &&
        shouldClearSnapshot)
    {
        ClearTacticalHudSnapshot();
    }
}

void RfabAdapterService::ClearTacticalHudSnapshot() noexcept
{
    auto spEmpty = std::make_shared<RfabTacticalHudRenderSnapshot>();
    std::unique_lock<std::shared_mutex> lock(m_tacticalHudSnapshotMutex);
    m_tacticalHudSnapshot = std::move(spEmpty);
}

void RfabAdapterService::RefreshTacticalHudSnapshot() noexcept
{
    auto spNext = std::make_shared<RfabTacticalHudRenderSnapshot>();
    m_hudAndPings.BuildHudSnapshot(
        m_world,
        m_transport,
        m_formResolver,
        m_combatRules,
        ResolveEffectiveLocalPlayerId(),
        IsAllyProtectionEnabled(),
        IsLocalPlayerAllyProtectionHost(),
        *spNext);

    std::unique_lock<std::shared_mutex> lock(m_tacticalHudSnapshotMutex);
    m_tacticalHudSnapshot = std::move(spNext);
}

void RfabAdapterService::OnDrawTacticalHud() noexcept
{
    // Runs on D3D11 Render Thread (BSGraphics::Hook_Renderer_End): grabs COW shared_ptr with zero deep copy!
    std::shared_ptr<const RfabTacticalHudRenderSnapshot> spSnapshot;
    {
        std::shared_lock<std::shared_mutex> lock(m_tacticalHudSnapshotMutex);
        spSnapshot = m_tacticalHudSnapshot;
    }
    if (!spSnapshot || !spSnapshot->Visible)
        return;
    const auto& snapshot = *spSnapshot;

    auto* pViewport = BSGraphics::GetMainWindow();
    if (!pViewport || pViewport->uiWindowWidth == 0 || pViewport->uiWindowHeight == 0)
        return;

    ImDrawList* pDrawList = ImGui::GetBackgroundDrawList();
    if (!pDrawList)
        return;

    const float viewW = static_cast<float>(pViewport->uiWindowWidth);
    const float viewH = static_cast<float>(pViewport->uiWindowHeight);

    for (const auto& beacon : snapshot.DownedBeacons)
    {
        const auto proj = RfabCompat::ComputeTacticalMarkerScreenPlacement(
            beacon.NormScreenX, beacon.NormScreenY, beacon.NormScreenZ, viewW, viewH, 56.0f);
        RfabHudAndPings::DrawDownedBeacon(pDrawList, beacon, proj);
    }

    for (const auto& ping : snapshot.ActivePings)
    {
        const auto proj = RfabCompat::ComputeTacticalMarkerScreenPlacement(
            ping.NormScreenX, ping.NormScreenY, ping.NormScreenZ, viewW, viewH, 52.0f);
        RfabHudAndPings::DrawTacticalPing(pDrawList, ping, proj);
    }

    RfabHudAndPings::DrawOverlayPanels(pDrawList, snapshot, viewW, viewH);
}
