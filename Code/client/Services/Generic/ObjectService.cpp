#include <Services/ObjectService.h>

#include <CrashBreadcrumbs.h>
#include <common/CoopTeleportGuard.h>
#include <common/CoopWorldStabilizer.h>
#include <common/RfabModpackCompat.h>
#include <Services/RfabAdapterService.h>
#include <World.h>
#include <Events/UpdateEvent.h>
#include <Events/CellChangeEvent.h>
#include <Events/InventoryChangeEvent.h>
#include <Events/ActivateEvent.h>
#include <Events/LockChangeEvent.h>
#include <Events/ScriptAnimationEvent.h>
#include <Messages/ServerTimeSettings.h>
#include <Messages/AssignObjectsRequest.h>
#include <Messages/AssignObjectsResponse.h>
#include <Messages/ActivateRequest.h>
#include <Messages/NotifyActivate.h>
#include <Messages/LockChangeRequest.h>
#include <Messages/NotifyLockChange.h>
#include <Messages/RequestInventoryChanges.h>
#include <Messages/ScriptAnimationRequest.h>
#include <Messages/NotifyScriptAnimation.h>

#include <PlayerCharacter.h>
#include <Games/TES.h>
#include <Forms/TESFaction.h>
#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>
#include <Forms/BGSEncounterZone.h>

#include <chrono>
#include <inttypes.h>
#include <unordered_set>

ObjectService::ObjectService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport)
    : m_world(aWorld)
    , m_transport(aTransport)
{
    m_cellChangeConnection = aDispatcher.sink<CellChangeEvent>().connect<&ObjectService::OnCellChange>(this);
    m_onActivateConnection = aDispatcher.sink<ActivateEvent>().connect<&ObjectService::OnActivate>(this);
    m_activateConnection = aDispatcher.sink<NotifyActivate>().connect<&ObjectService::OnActivateNotify>(this);
    m_lockChangeConnection = aDispatcher.sink<LockChangeEvent>().connect<&ObjectService::OnLockChange>(this);
    m_lockChangeNotifyConnection = aDispatcher.sink<NotifyLockChange>().connect<&ObjectService::OnLockChangeNotify>(this);
    m_assignObjectConnection = aDispatcher.sink<AssignObjectsResponse>().connect<&ObjectService::OnAssignObjectsResponse>(this);
    m_scriptAnimationConnection = aDispatcher.sink<ScriptAnimationEvent>().connect<&ObjectService::OnScriptAnimationEvent>(this);
    m_scriptAnimationNotifyConnection = aDispatcher.sink<NotifyScriptAnimation>().connect<&ObjectService::OnNotifyScriptAnimation>(this);

    EventDispatcherManager::Get()->activateEvent.RegisterSink(this);
}

namespace
{
uint64_t SteadyNowMs() noexcept
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
} // namespace

bool IsPlayerHome(const TESObjectCELL* pCell) noexcept
{
    if (pCell)
    {
        auto* pEncounterZone = static_cast<ExtraEncounterZone*>(pCell->extraData.GetByType(ExtraDataType::EncounterZone));

        // Only return true if cell has the NoResetZone encounter zone
        if (pEncounterZone && pEncounterZone->zone && pEncounterZone->zone->formID == 0xf90b1)
        {
            switch (pCell->formID)
            {
            case 0xeec55: // one known exception: Sinderion's Field Lab
                return false;
            default: return true;
            }
        }
    }

    return false;
}

bool ObjectService::IsFactionCrimeContainer(const uint32_t aFormId) noexcept
{
    if (aFormId == 0)
        return false;

    auto* pModManager = ModManager::Get();
    if (!pModManager)
        return false;

    for (uint32_t i = 0; i < pModManager->factions.length; ++i)
    {
        const TESFaction* pFaction = pModManager->factions[i];
        if (!pFaction)
            continue;

        if (pFaction->crimeData.factionPlayerInventoryContainer &&
            pFaction->crimeData.factionPlayerInventoryContainer->formID == aFormId)
        {
            return true;
        }

        if (pFaction->crimeData.factionStolenContainer &&
            pFaction->crimeData.factionStolenContainer->formID == aFormId)
        {
            return true;
        }
    }

    return false;
}

bool ObjectService::IsFactionMerchantContainer(const TESObjectREFR* apContainer) noexcept
{
    if (!apContainer)
        return false;

    auto* pModManager = ModManager::Get();
    if (!pModManager)
        return false;

    for (uint32_t i = 0; i < pModManager->factions.length; ++i)
    {
        const TESFaction* pFaction = pModManager->factions[i];
        // The pointer is compared and never followed: a faction without a merchant chest holds
        // null here, and no value of this field can make the check touch other memory.
        if (pFaction && pFaction->vendorData.merchantContainer == apContainer)
            return true;
    }

    return false;
}

namespace
{
// Is the container where the local player can reach it: in his interior cell, or in the worldspace
// he walks in? Compares the cells themselves and does not ask whether they have finished loading,
// so the answer is the same at the moment the player enters a cell as a minute later.
bool IsContainerWherePlayerIs(const TESObjectREFR* apContainer) noexcept
{
    const PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!apContainer || !pPlayer)
        return true;

    const TESObjectCELL* pPlayerCell = pPlayer->GetParentCellEx();
    if (!pPlayerCell)
        return true; // between cells: nothing is registered then anyway

    const TESObjectCELL* pContainerCell = apContainer->GetParentCellEx();
    if (!pContainerCell)
        return false;
    if (pContainerCell == pPlayerCell)
        return true;

    const TESWorldSpace* pPlayerWorld = pPlayer->GetWorldSpace();
    return pPlayerWorld != nullptr && apContainer->GetWorldSpace() == pPlayerWorld;
}

bool ShouldSyncObject(const TESObjectREFR* apObject) noexcept
{
    if (!apObject)
        return false;

    switch (apObject->formID)
    {
    case 0x39CF1: // Don't sync the chest in the "Diplomatic Immunity" quest
        return false;
    case 0x3EF03: // ...as well as in the "No One Escapes Cidhna Mine" quest
        return false;
    default:
        break;
    }

    // Never sync faction prisoner belongings or stolen-goods evidence chests (#893):
    // otherwise one arrested player's confiscated inventory overwrites teammates' jail chests.
    if (apObject->baseForm && apObject->baseForm->formType == FormType::Container &&
        ObjectService::IsFactionCrimeContainer(apObject->formID))
    {
        return false;
    }

    // Gameplay:bEnablePersonalMerchantStock on the server (defect M27). A merchant's chest and a
    // container that is not where the player is (a script opened it from somewhere else: RFAB's
    // secure chest) hold what this player's own game put there. Shared, the first player to touch
    // one would replace the other player's stock or stored items with his own, and the server
    // would take back from a trade whatever its copy of the chest did not hold.
    if (apObject->baseForm && apObject->baseForm->formType == FormType::Container &&
        World::IsFeatureEnabled(Session::Feature::kPersonalMerchantStock))
    {
        RfabCompat::ContainerSyncFacts facts{};
        facts.IsMerchantContainer = ObjectService::IsFactionMerchantContainer(apObject);
        facts.IsWherePlayerIs = IsContainerWherePlayerIs(apObject);
        if (RfabCompat::IsPersonalContainer(facts, true))
            return false;
    }

    return true;
}
} // namespace

void ObjectService::OnSessionEnd(const Session::Transition&) noexcept
{
    DestroyObjectEntities();
}

void ObjectService::OnWorldLoadBegin(const Session::Transition&) noexcept
{
    DestroyObjectEntities();
}

void ObjectService::DestroyObjectEntities() noexcept
{
    m_pendingContainerLoot.Clear();
    m_registrationsInFlight.Clear();
    auto view = m_world.view<ObjectComponent>();
    Vector<entt::entity> toDestroy(view.begin(), view.end());
    for (entt::entity entity : toDestroy)
    {
        if (m_world.valid(entity))
            m_world.destroy(entity);
    }
}

bool ObjectService::RequestContainerSyncForLoot(
    TESObjectREFR* apContainer,
    const Inventory::Entry& acItem,
    bool aDrop,
    bool aUpdateClients,
    uint64_t aChangeSequence) noexcept
{
    if (!m_transport.IsConnected() || !apContainer || !apContainer->baseForm)
        return false;

    if (apContainer->baseForm->formType != FormType::Container)
        return false;

    if (!ShouldSyncObject(apContainer))
        return false;

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    TESObjectCELL* pCell = apContainer->parentCell ? apContainer->parentCell : (pPlayer ? pPlayer->parentCell : nullptr);
    if (!pCell)
        return false;

    if (!World::Get().GetServerSettings().SyncPlayerHomes && IsPlayerHome(pCell))
        return false;

    ObjectData objectData{};
    if (!m_world.GetModSystem().GetServerModId(apContainer->formID, objectData.Id) ||
        objectData.Id.ModId == 0xFFFFFFFF)
    {
        return false;
    }

    const uint64_t nowMs = SteadyNowMs();

    // Gameplay:bEnableEarlyTakeReplay on the server (defect M28, common/CoopLootLedger.h). This
    // container has been sent to the server (by the cell scan, an activation or an earlier take)
    // and the answer is on its way. A second registration would be answered "not first" with the
    // list as the first one read it and written over the chest, after the first answer ("you are
    // first") had dropped this change. So the change is only queued, marked with whether the
    // list the server was sent already has it, and the answer decides.
    const bool earlyTakeReplay = World::IsFeatureEnabled(Session::Feature::kEarlyTakeReplay);
    if (earlyTakeReplay)
    {
        const auto sent = m_registrationsInFlight.Find(apContainer->formID, nowMs);
        if (sent.InFlight)
        {
            const bool reflected = CoopLootLedger::IsChangeReflectedInRegistration(aChangeSequence, sent.Sequence);
            RFAB_EVENT_CRUMB("early-container-change", apContainer->formID, reflected ? 1u : 0u);
            m_pendingContainerLoot.Enqueue(
                apContainer->formID,
                acItem,
                aDrop,
                aUpdateClients,
                nowMs,
                CoopLootLedger::PendingContainerLootQueue<Inventory::Entry>::kDefaultTtlMs,
                reflected);
            return true;
        }
    }

    m_world.GetModSystem().GetServerModId(pCell->formID, objectData.CellId);
    if (TESWorldSpace* pWorldSpace = pPlayer ? pPlayer->GetWorldSpace() : nullptr)
        m_world.GetModSystem().GetServerModId(pWorldSpace->formID, objectData.WorldSpaceId);
    objectData.CurrentCoords = GridCellCoords::CalculateGridCellCoords(apContainer->position.x, apContainer->position.y);

    if (Lock* pLock = apContainer->GetLock())
    {
        objectData.CurrentLockData.IsLocked = pLock->IsLocked();
        objectData.CurrentLockData.LockLevel = pLock->lockLevel;
    }

    // The counter is read before the list (defect M28): this change is in the list, a later one is not.
    if (earlyTakeReplay)
        m_registrationsInFlight.Note(apContainer->formID, nowMs, CurrentInventoryChangeSequence());
    objectData.CurrentInventory = apContainer->GetInventory();

    m_pendingContainerLoot.Enqueue(apContainer->formID, acItem, aDrop, aUpdateClients, nowMs);

    AssignObjectsRequest request{};
    request.Objects.push_back(std::move(objectData));
    m_transport.Send(request);
    return true;
}

void ObjectService::RefreshCurrentCellObjectsAfterRollback() noexcept
{
    m_registrationsInFlight.Clear();
    auto view = m_world.view<ObjectComponent>();
    Vector<entt::entity> toDestroy(view.begin(), view.end());
    for (entt::entity entity : toDestroy)
    {
        if (m_world.valid(entity))
            m_world.destroy(entity);
    }

    CellChangeEvent refreshEvt{};
    OnCellChange(refreshEvt);
}

void ObjectService::OnCellChange(const CellChangeEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || !pPlayer->parentCell)
        return;

    TESObjectCELL* pCell = pPlayer->parentCell;

    // Player homes should not be synced, so that chest contents,
    // which are often used as storage, are never accidentally wiped.
    if (!World::Get().GetServerSettings().SyncPlayerHomes && IsPlayerHome(pCell))
        return;

    GameId cellId = acEvent.CellId;
    if (!cellId)
        m_world.GetModSystem().GetServerModId(pCell->formID, cellId);

    GameId worldSpaceId = acEvent.WorldSpaceId;
    if (!worldSpaceId)
    {
        if (TESWorldSpace* pWorldSpace = pPlayer->GetWorldSpace())
            m_world.GetModSystem().GetServerModId(pWorldSpace->formID, worldSpaceId);
    }

    Vector<FormType> formTypes = {FormType::Container, FormType::Door};
    // Door seemed to be at the wrong form id (29, now 32), verify this.
    Vector<TESObjectREFR*> objects = pCell->GetRefsByFormTypes(formTypes);

    const bool earlyTakeReplay = World::IsFeatureEnabled(Session::Feature::kEarlyTakeReplay);
    const uint64_t nowMs = SteadyNowMs();

    AssignObjectsRequest request{};

    for (TESObjectREFR* pObject : objects)
    {
        if (!ShouldSyncObject(pObject))
        {
            // Not a warning: merchants' chests are excluded in every shop, on every visit.
            spdlog::debug("Excluding sync for {:X}", pObject->formID);
            continue;
        }

        ObjectData objectData{};
        objectData.CellId = cellId;
        objectData.WorldSpaceId = worldSpaceId;
        objectData.CurrentCoords = GridCellCoords::CalculateGridCellCoords(pObject->position.x, pObject->position.y);

        if (!m_world.GetModSystem().GetServerModId(pObject->formID, objectData.Id) || objectData.Id.ModId == 0xFFFFFFFF)
        {
            spdlog::debug("Skipping runtime/unmapped object with form id {:X}", pObject->formID);
            continue;
        }

        if (Lock* pLock = pObject->GetLock())
        {
            objectData.CurrentLockData.IsLocked = pLock->IsLocked();
            objectData.CurrentLockData.LockLevel = pLock->lockLevel;
        }

        if (pObject->baseForm->formType == FormType::Container)
        {
            // The counter is read before the list (defect M28): a change counted later is not in it.
            if (earlyTakeReplay)
                m_registrationsInFlight.Note(pObject->formID, nowMs, CurrentInventoryChangeSequence());
            objectData.CurrentInventory = pObject->GetInventory();
        }

        request.Objects.push_back(objectData);
    }

    if (!request.Objects.empty())
        m_transport.Send(request);

    RegisterNeighbourCellContainers();
}

void ObjectService::RegisterNeighbourCellContainers() noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || !pPlayer->parentCell)
        return;

    // Gameplay:bEnableNeighbourCellContainers on the server. Why, and why outdoors only:
    // RfabCompat::ShouldScanNeighbourCells.
    TESWorldSpace* pWorldSpace = pPlayer->GetWorldSpace();
    if (!RfabCompat::ShouldScanNeighbourCells(
            World::IsFeatureEnabled(Session::Feature::kNeighbourCellContainers), pWorldSpace != nullptr))
    {
        return;
    }

    const TES* pTES = TES::Get();
    ModManager* pModManager = ModManager::Get();
    if (!pTES || !pModManager)
        return;

    GameId worldSpaceId{};
    if (!m_world.GetModSystem().GetServerModId(pWorldSpace->formID, worldSpaceId))
        return;

    // The containers this client already holds the server's id for.
    std::unordered_set<uint32_t> held;
    {
        const auto view = m_world.view<FormIdComponent, ObjectComponent>();
        for (entt::entity entity : view)
            held.insert(view.get<FormIdComponent>(entity).Id);
    }

    // The player's cell as the game counts it: the same pair the cell change was detected from.
    const int32_t playerCellX = pTES->currentGridX;
    const int32_t playerCellY = pTES->currentGridY;

    const bool syncPlayerHomes = World::Get().GetServerSettings().SyncPlayerHomes;
    const bool earlyTakeReplay = World::IsFeatureEnabled(Session::Feature::kEarlyTakeReplay);
    const uint64_t nowMs = SteadyNowMs();
    const Vector<FormType> formTypes = {FormType::Container};

    for (const RfabCompat::CellOffset& offset : RfabCompat::kNeighbourCellOffsets)
    {
        // false: the cell is looked up, never loaded for this. A cell the game is still attaching
        // is left for the next cell change; until then the first take registers its containers,
        // as before.
        TESObjectCELL* pCell = pModManager->GetCellFromCoordinates(playerCellX + offset.X, playerCellY + offset.Y, pWorldSpace, false);
        if (!pCell || pCell == pPlayer->parentCell || !pCell->IsAttachedAndLoaded())
            continue;

        if (!syncPlayerHomes && IsPlayerHome(pCell))
            continue;

        GameId cellId{};
        if (!m_world.GetModSystem().GetServerModId(pCell->formID, cellId))
            continue;

        RFAB_EVENT_CRUMB("neighbour-cell-containers", pCell->formID);

        // One request per cell keeps each packet no larger than the scan of the player's own cell.
        AssignObjectsRequest request{};
        for (TESObjectREFR* pObject : pCell->GetRefsByFormTypes(formTypes))
        {
            if (!pObject || !pObject->baseForm || pObject->IsDeleted())
                continue;

            const bool withinReach =
                RfabCompat::IsWithinReachOfCell(pObject->position.x, pObject->position.y, playerCellX, playerCellY);
            // The reach is tested first: it is arithmetic, and it spares the walk over the factions
            // that ShouldSyncObject does for every container further away.
            // "Held" includes a container already sent whose answer is on its way.
            const bool alreadyHeld =
                held.find(pObject->formID) != held.end() || m_registrationsInFlight.Find(pObject->formID, nowMs).InFlight;
            if (!withinReach || !RfabCompat::ShouldRegisterNeighbourContainer(alreadyHeld, !ShouldSyncObject(pObject), true))
                continue;

            ObjectData objectData{};
            if (!m_world.GetModSystem().GetServerModId(pObject->formID, objectData.Id) || objectData.Id.ModId == 0xFFFFFFFF)
                continue;

            objectData.CellId = cellId;
            objectData.WorldSpaceId = worldSpaceId;
            objectData.CurrentCoords = GridCellCoords::CalculateGridCellCoords(pObject->position.x, pObject->position.y);

            if (Lock* pLock = pObject->GetLock())
            {
                objectData.CurrentLockData.IsLocked = pLock->IsLocked();
                objectData.CurrentLockData.LockLevel = pLock->lockLevel;
            }

            // The counter is read before the list (defect M28): a change counted later is not in it.
            if (earlyTakeReplay)
                m_registrationsInFlight.Note(pObject->formID, nowMs, CurrentInventoryChangeSequence());
            objectData.CurrentInventory = pObject->GetInventory();

            request.Objects.push_back(std::move(objectData));
        }

        if (!request.Objects.empty())
            m_transport.Send(request);
    }
}

void ObjectService::OnAssignObjectsResponse(const AssignObjectsResponse& acMessage) noexcept
{
    for (const ObjectData& objectData : acMessage.Objects)
    {
        const uint32_t cObjectId = World::Get().GetModSystem().GetGameId(objectData.Id);
        TESObjectREFR* pObject = Cast<TESObjectREFR>(TESForm::GetById(cObjectId));
        if (!pObject)
        {
            spdlog::error("Object not found for form id {:X}", objectData.Id);
            continue;
        }

        CreateObjectEntity(pObject->formID, objectData.ServerId);
        m_registrationsInFlight.Erase(pObject->formID);

        const uint64_t nowMs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        auto pendingDeltas = m_pendingContainerLoot.TakeForContainer(pObject->formID, nowMs);

        if (objectData.IsSenderFirst)
        {
            // Server initialized its authoritative inventory from our AssignObjectsRequest
            // (which already reflects the QuickLootIE / AutoLoot mutation), so drop pending deltas
            // instead of double-applying them!
            //
            // Except a change made after the registration read the list it sent (defect M28):
            // the server's copy lacks it. Such a change is queued with ReflectedInRegistration
            // false (RequestContainerSyncForLoot) and is sent here.
            for (const auto& pending : pendingDeltas)
            {
                if (!CoopLootLedger::ShouldReplayPendingDelta(true, pending.ReflectedInRegistration))
                    continue;

                RequestInventoryChanges req{};
                req.ServerId = objectData.ServerId;
                req.Item = pending.Item;
                req.Drop = pending.Drop;
                req.UpdateClients = pending.UpdateClients;
                m_transport.Send(req);
            }
            continue;
        }

        if (objectData.CurrentLockData.IsLocked)
        {
            Lock* pLock = pObject->GetLock();

            if (!pLock)
            {
                pLock = pObject->CreateLock();
            }

            if (pLock)
            {
                pLock->lockLevel = objectData.CurrentLockData.LockLevel;
                pLock->SetLock(true);
                pObject->LockChange();
            }
        }
        else if (Lock* pLock = pObject->GetLock(); pLock && pLock->IsLocked())
        {
            // Unlock locally locked doors/chests when a co-op peer already unlocked them,
            // even when CurrentLockData == LockData{} ({IsLocked = false, LockLevel = 0})!
            pLock->lockLevel = objectData.CurrentLockData.LockLevel;
            pLock->SetLock(false);
            pObject->LockChange();
        }

        if (pObject->baseForm->formType == FormType::Container)
        {
            Inventory currentInventory = pObject->GetInventory();

            if (currentInventory.ContainsQuestItems())
                pObject->SetInventoryRetainingQuestItems(currentInventory, objectData.CurrentInventory);
            else
                pObject->SetInventory(objectData.CurrentInventory);

            // Replay any QuickLootIE / RFAB AutoLoot deltas captured while registration was in flight
            // so the server arbitrates them against its authoritative LootAntiDupeLedger.
            for (const auto& pending : pendingDeltas)
            {
                RequestInventoryChanges req{};
                req.ServerId = objectData.ServerId;
                req.Item = pending.Item;
                req.Drop = pending.Drop;
                req.UpdateClients = pending.UpdateClients;
                m_transport.Send(req);
            }
        }
    }
}

entt::entity ObjectService::CreateObjectEntity(const uint32_t acFormId, const uint32_t acServerId) noexcept
{
    const auto view = m_world.view<FormIdComponent, ObjectComponent>();

    // Deduplicate by BOTH FormId and ServerId so cell transitions / reconnects never leave
    // stale ServerId entities at the front of FormIdComponent view!
    entt::entity matchedEntity = entt::null;
    Vector<entt::entity> staleDuplicates;

    for (entt::entity entity : view)
    {
        const uint32_t existingFormId = view.get<FormIdComponent>(entity).Id;
        const uint32_t existingServerId = view.get<ObjectComponent>(entity).Id;

        if (existingFormId == acFormId || existingServerId == acServerId)
        {
            if (matchedEntity == entt::null)
                matchedEntity = entity;
            else
                staleDuplicates.push_back(entity);
        }
    }

    for (entt::entity duplicate : staleDuplicates)
    {
        if (m_world.valid(duplicate))
            m_world.destroy(duplicate);
    }

    if (matchedEntity != entt::null)
    {
        m_world.emplace_or_replace<FormIdComponent>(matchedEntity, acFormId);
        m_world.emplace_or_replace<ObjectComponent>(matchedEntity, acServerId);
        return matchedEntity;
    }

    entt::entity entity = m_world.create();
    spdlog::info("Created object entity, server id: {:X}, form id {:X}", acServerId, acFormId);

    m_world.emplace<FormIdComponent>(entity, acFormId);
    m_world.emplace<ObjectComponent>(entity, acServerId);

    return entity;
}

namespace
{
bool IsPersonalBlessingActivator(const TESObjectREFR* apObject) noexcept
{
    if (!apObject || !apObject->baseForm || apObject->baseForm->formType != FormType::Activator)
        return false;

    if (const char* rawEdid = apObject->baseForm->GetFormEditorID(); rawEdid && rawEdid[0] != '\0')
    {
        const std::string edid = RfabCompat::Detail::ToLowerAscii(rawEdid);
        if (RfabCompat::Detail::StartsWithIgnoreCase(edid, "rfab_shrine_") ||
            RfabCompat::Detail::StartsWithIgnoreCase(edid, "doomstone") ||
            RfabCompat::Detail::StartsWithIgnoreCase(edid, "dlc2standingstone") ||
            edid == "rfab_ba_universalaltar" ||
            edid == "t01dibellasaltaractivator")
        {
            return true;
        }
    }

    const uint32_t fullFormId = apObject->baseForm->formID;
    const uint32_t modIdx = (fullFormId >> 24) & 0xFFu;
    const uint32_t baseLocalId = fullFormId & 0x00FFFFFFu;

    // Vanilla Skyrim.esm (0x00) & Dragonborn.esm (0x04) personal shrines & standing stones
    if (modIdx == 0x00u &&
        ((baseLocalId >= 0x0D232Du && baseLocalId <= 0x0D2339u) || // Vanilla DoomstoneWarrior..DoomstoneSerpent
         (baseLocalId >= 0x0D987Bu && baseLocalId <= 0x0D9887u) || // Vanilla Shrine Divines
         baseLocalId == 0x06C9E1u))                                // T01DibellasAltarActivator
    {
        return true;
    }

    if (modIdx == 0x04u && (baseLocalId >= 0x01EDA5u && baseLocalId <= 0x01EDB1u)) // Dragonborn All-Maker Stones
        return true;

    // RFAB personal shrines (dynamically resolved via RfabAdapterService across any RFAB plugin name or load-order slot)
    const auto& rfabAdapter = World::Get().GetRfabAdapterService();
    const auto& bindings = rfabAdapter.GetBindings();
    if (TESForm* pAltar = rfabAdapter.ResolveFormByEditorIdOrBaseId(
            bindings.UniversalAltarEditorId, bindings.FallbackAltarBaseFormId);
        pAltar && pAltar->formID == fullFormId)
    {
        return true;
    }

    if (Mod* pRfabMod = rfabAdapter.ResolveRfabPluginMod();
        pRfabMod && pRfabMod->GetFormId(baseLocalId) == fullFormId)
    {
        switch (baseLocalId)
        {
        case 0x071854u: // RFAB_Shrine_Arkay
        case 0x100780u: // RFAB_Shrine_Talos
        case 0x10E8B0u: // RFAB_Shrine_Nocturnal
        case 0x00C86Bu: // RFAB_Shrine_Auriel
        case 0x039E34u: // RFAB_Shrine_Boethiah
        case 0x03A481u: // RFAB_Shrine_Mephala
        case 0x03A484u: // RFAB_Shrine_Azura
        case 0x0031EDu: // RFAB_Shrine_Malacath
        case 0x0031EEu: // RFAB_Shrine_Peryite
        case 0x0031EFu: // RFAB_Shrine_Sanguine
        case 0x0CE38Fu: // RFAB_Shrine_Dagon
        case 0x239FC5u: // RFAB_BA_UniversalAltar
            return true;
        default:
            break;
        }
    }

    return false;
}
} // namespace

void ObjectService::OnActivate(const ActivateEvent& acEvent) noexcept
{
    if (acEvent.ObjectId == 0)
        return;

    TESObjectREFR* pObject = Cast<TESObjectREFR>(TESForm::GetById(acEvent.ObjectId));
    if (!pObject || pObject->IsDeleted() || !pObject->baseForm)
        return;

    if (acEvent.ActivateFlag)
    {
        Actor* pActivator = acEvent.ActivatorId ? Cast<Actor>(TESForm::GetById(acEvent.ActivatorId)) : nullptr;
        TESBoundObject* pObjectToGet = acEvent.ObjectToGetId ? Cast<TESBoundObject>(TESForm::GetById(acEvent.ObjectToGetId)) : nullptr;
        if (pActivator && !pActivator->IsDeleted())
            pObject->Activate(pActivator, acEvent.Unk1, pObjectToGet, acEvent.Count, acEvent.DefaultProcessing);

        pObject = Cast<TESObjectREFR>(TESForm::GetById(acEvent.ObjectId));
        if (!pObject || pObject->IsDeleted() || !pObject->baseForm)
            return;
    }

    if (!m_transport.IsConnected())
        return;

    // Personal RFAB Aedra/Daedra shrines, Universal Altars, and Standing Stones are strictly per-character:
    // forwarding Activate to peers triggers RFAB_BA_UniversalShrine.psc / TempleBlessingScript on Game.GetPlayer()
    // or locks the shrine in its cooldown state on peer clients before they can pray!
    if (IsPersonalBlessingActivator(pObject))
        return;

    // On-demand registration for containers/doors activated across exterior cell boundaries or before OnCellChange.
    if ((pObject->baseForm->formType == FormType::Container ||
         pObject->baseForm->formType == FormType::Door) &&
        ShouldSyncObject(pObject))
    {
        const auto objView = m_world.view<FormIdComponent, ObjectComponent>();
        const bool alreadyAssigned = std::any_of(
            objView.begin(),
            objView.end(),
            [objView, formId = pObject->formID](entt::entity e)
            { return objView.get<FormIdComponent>(e).Id == formId; });

        if (!alreadyAssigned)
        {
            ObjectData objectData{};
            if (m_world.GetModSystem().GetServerModId(pObject->formID, objectData.Id) &&
                objectData.Id.ModId != 0xFFFFFFFF)
            {
                if (TESObjectCELL* pObjCell = pObject->GetParentCellEx())
                    m_world.GetModSystem().GetServerModId(pObjCell->formID, objectData.CellId);
                if (PlayerCharacter* pPlayer = PlayerCharacter::Get())
                {
                    if (TESWorldSpace* pWs = pPlayer->GetWorldSpace())
                        m_world.GetModSystem().GetServerModId(pWs->formID, objectData.WorldSpaceId);
                }
                objectData.CurrentCoords = GridCellCoords::CalculateGridCellCoords(
                    pObject->position.x, pObject->position.y);
                if (Lock* pObjLock = pObject->GetLock())
                {
                    objectData.CurrentLockData.IsLocked = pObjLock->IsLocked();
                    objectData.CurrentLockData.LockLevel = pObjLock->lockLevel;
                }
                if (pObject->baseForm->formType == FormType::Container)
                {
                    // The counter is read before the list (defect M28): a take made in the opened
                    // container before the answer is not in this list.
                    if (World::IsFeatureEnabled(Session::Feature::kEarlyTakeReplay))
                        m_registrationsInFlight.Note(pObject->formID, SteadyNowMs(), CurrentInventoryChangeSequence());
                    objectData.CurrentInventory = pObject->GetInventory();
                }

                AssignObjectsRequest assignReq{};
                assignReq.Objects.push_back(std::move(objectData));
                m_transport.Send(assignReq);
            }
        }
    }

    if (Lock* pLock = pObject->GetLock())
    {
        // IMPORTANT: Check pLock->IsLocked() (Flag::kLocked = 1 << 0), NOT (pLock->flags & 0xFF)!
        // Leveled locks keep Flag::kLeveled (1 << 2 = 0x04) set in pLock->flags even after being unlocked,
        // so (pLock->flags & 0xFF) permanently blocked door/chest activation sync after lockpicking or key unlock!
        if (pLock->IsLocked())
            return;
    }

    const bool isTargetActor = (pObject->formType == Actor::Type);
    const bool isTempTarget = ((pObject->formID & 0xFF000000u) == 0xFF000000u);
    const bool isTeleportLoadDoor =
        (pObject->baseForm &&
         pObject->baseForm->formType == FormType::Door &&
         pObject->extraData.Contains(ExtraDataType::Teleport));
    if (!CoopWorldStabilizer::ShouldBroadcastObjectActivation(isTargetActor, isTempTarget, isTeleportLoadDoor))
        return;

    ActivateRequest request;

    if (!m_world.GetModSystem().GetServerModId(pObject->formID, request.Id) ||
        request.Id.ModId == 0xFFFFFFFFu)
    {
        spdlog::debug("Skipping non-static object activation form id {:X}", pObject->formID);
        return;
    }

    if (TESObjectCELL* pCell = pObject->GetParentCellEx())
    {
        m_world.GetModSystem().GetServerModId(pCell->formID, request.CellId);
    }

    if (acEvent.ActivatorId == 0)
        return;

    const entt::entity activatorEntity = m_world.GetCharacterService().FindAnyEntityByFormId(acEvent.ActivatorId);
    if (activatorEntity == entt::null)
        return;

    std::optional<uint32_t> serverIdRes = Utils::GetServerId(activatorEntity);
    if (!serverIdRes.has_value())
        return;

    request.ActivatorId = serverIdRes.value();
    request.PreActivationOpenState = acEvent.PreActivationOpenState;

    m_transport.Send(request);
}

void ObjectService::OnActivateNotify(const NotifyActivate& acMessage) noexcept
{
    const uint32_t cObjectId = World::Get().GetModSystem().GetGameId(acMessage.Id);
    TESObjectREFR* pObject = Cast<TESObjectREFR>(TESForm::GetById(cObjectId));
    if (!pObject || !pObject->baseForm)
    {
        spdlog::error("Failed to retrieve object to activate.");
        return;
    }

    if (IsPersonalBlessingActivator(pObject))
        return;

    const bool isTargetActor = (pObject->formType == Actor::Type);
    const bool isTeleportLoadDoor =
        (pObject->baseForm->formType == FormType::Door &&
         pObject->extraData.Contains(ExtraDataType::Teleport));

    // Never execute `Activate` on a Teleport Load Door from a remote packet!
    // Previously, when a peer (or an unmaterialized remote NPC) activated a load door, `pActor` fell
    // back to `PlayerCharacter::Get()` (`0x14`) and called `pObject->Activate(0x14, ...)`, spontaneously
    // teleporting the local player through that nearby load door into another cell/location!
    if (isTeleportLoadDoor)
    {
        if (Lock* pLock = pObject->GetLock(); pLock && pLock->IsLocked())
        {
            pLock->SetLock(false);
            pObject->LockChange();
        }
        return;
    }

    if (!CoopWorldStabilizer::ShouldExecuteRemoteObjectActivate(isTargetActor, isTeleportLoadDoor))
        return;

    Actor* pActor = Utils::GetByServerId<Actor>(acMessage.ActivatorId);
    if (!pActor)
    {
        if (pObject->baseForm->formType == FormType::Door)
            pActor = PlayerCharacter::Get();

        if (!pActor)
        {
            spdlog::debug("{}: could not find actor server id {:X}", __FUNCTION__, acMessage.ActivatorId);
            return;
        }
    }

    if (pObject->baseForm->formType == FormType::Door)
    {
        // Ensure locally locked doors are unlocked when a peer opens them (e.g. via dungeon key).
        if (Lock* pLock = pObject->GetLock(); pLock && pLock->IsLocked())
        {
            pLock->SetLock(false);
            pObject->LockChange();
        }

        const auto remotePreActivationState = static_cast<uint8_t>(acMessage.PreActivationOpenState);
        const auto localState = static_cast<uint8_t>(pObject->GetOpenState());

        if (!RfabCompat::AreDoorStatesCompatibleForSync(remotePreActivationState, localState))
        {
            // The doors are unsynced at this point. If we'll Activate the one on our side
            // it'll just continue to be unsynced (open remotely, closed locally and vice versa)
            return;
        }
    }

    // Suppress involuntary `Game.GetPlayer().MoveTo(...)` side effects from remote lever/portal/shrine
    // `OnActivate` Papyrus scripts while preserving local direct player activations (`0x14` pressed E).
    CoopTeleportGuard::ScopedRemoteQuestMoveToSuppression remoteActivateTeleportGuard(
        CoopTeleportGuard::NowSteadyNs(),
        CoopTeleportGuard::RemoteQuestMoveToGuard::kExtendedLateSessionPapyrusHoldNs);
    pObject->Activate(pActor, 0, nullptr, 1, 0);
}

void ObjectService::OnLockChange(const LockChangeEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    LockChangeRequest request;

    if (!m_world.GetModSystem().GetServerModId(acEvent.FormId, request.Id))
    {
        spdlog::error("Server form id for lock object not found, form id: {:X}", acEvent.FormId);
        return;
    }

    if (const auto* const pObject = Cast<TESObjectREFR>(TESForm::GetById(acEvent.FormId)))
    {
        if (TESObjectCELL* pCell = pObject->GetParentCellEx())
            m_world.GetModSystem().GetServerModId(pCell->formID, request.CellId);
    }

    request.IsLocked = acEvent.IsLocked;
    request.LockLevel = acEvent.LockLevel;

    m_transport.Send(request);
}

void ObjectService::OnLockChangeNotify(const NotifyLockChange& acMessage) noexcept
{
    const auto cObjectId = World::Get().GetModSystem().GetGameId(acMessage.Id);
    if (cObjectId == 0)
    {
        spdlog::error("Failed to retrieve object id to (un)lock.");
        return;
    }

    auto* pObject = Cast<TESObjectREFR>(TESForm::GetById(cObjectId));
    if (!pObject)
    {
        spdlog::error("Failed to retrieve object to (un)lock.");
        return;
    }

    auto* pLock = pObject->GetLock();

    if(!acMessage.IsLocked)
    {
        if (!pLock || !pLock->IsLocked())
            return;
    }

    if (!pLock && acMessage.IsLocked)
    {
        pLock = pObject->CreateLock();
        if (!pLock)
        {
            spdlog::error("Failed to create lock for object form id {:X}", pObject->formID);
            return;
        }
    }

    pLock->lockLevel = acMessage.LockLevel;
    pLock->SetLock(acMessage.IsLocked);
    pObject->LockChange();
}

void ObjectService::OnScriptAnimationEvent(const ScriptAnimationEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected() || (acEvent.Animation.empty() && acEvent.EventName.empty()))
        return;

    ScriptAnimationRequest request{};
    GameId netFormId{};
    if (m_world.GetModSystem().GetServerModId(acEvent.FormID, netFormId))
        request.FormID = ((netFormId.ModId & 0xFFu) << 24) | (netFormId.BaseId & 0x00FFFFFFu);
    else
        request.FormID = acEvent.FormID;

    request.Animation = acEvent.Animation;
    request.EventName = acEvent.EventName;

    m_transport.Send(request);
}

void ObjectService::OnNotifyScriptAnimation(const NotifyScriptAnimation& acMessage) noexcept
{
    if (acMessage.FormID == 0 || (acMessage.Animation.empty() && acMessage.EventName.empty()))
        return;

    uint32_t localFormId = acMessage.FormID;
    const GameId netFormId(
        (acMessage.FormID >> 24) & 0xFFu,
        acMessage.FormID & 0x00FFFFFFu);
    if (const uint32_t resolvedId = m_world.GetModSystem().GetGameId(netFormId); resolvedId != 0)
        localFormId = resolvedId;

    auto* pObject = Cast<TESObjectREFR>(TESForm::GetById(localFormId));
    if (!pObject)
    {
        pObject = Cast<TESObjectREFR>(TESForm::GetById(acMessage.FormID));
        if (!pObject)
        {
            spdlog::debug("Failed to fetch notify script animation object, form id: {:X}", acMessage.FormID);
            return;
        }
    }

    const char* animName = !acMessage.Animation.empty() ? acMessage.Animation.c_str() : acMessage.EventName.c_str();
    if (!animName || animName[0] == '\0')
        return;

    BSFixedString animation(animName);
    pObject->PlayAnimation(&animation);
}

BSTEventResult ObjectService::OnEvent(const TESActivateEvent* acEvent, const EventDispatcher<TESActivateEvent>* aDispatcher)
{
    // Activation is replicated by the TESObjectREFR::Activate hook. This sink used to hold an
    // ENVIRONMENT_DEBUG block that read the registry on the sending thread; the macro is defined
    // nowhere and the block called a function that does not exist, so it was removed in v4.1.0.
    (void)acEvent;
    (void)aDispatcher;
    return BSTEventResult::kOk;
}
