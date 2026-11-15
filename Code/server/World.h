#pragma once

#include "Services/AdminService.h"

#include <Services/PlayerService.h>
#include <Services/PartyService.h>
#include <Services/CharacterService.h>
#include <Services/CalendarService.h>
#include <Services/QuestService.h>
#include <Services/ScriptService.h>
#include <Services/RfabProgressionService.h>
#include <Services/SaveSnapshotService.h>
#include <Services/EncounterService.h>
#include <Services/ActorValueService.h>
#include <Services/InventoryService.h>
#include <Services/ObjectService.h>

#include "Game/PlayerManager.h"

namespace ESLoader
{
struct RecordCollection;
}

namespace WorldParts
{
// Owns the event dispatcher of World - as a base class, and the FIRST one, on purpose.
//
// Every service lives in the registry's context (the entt::registry base of World) and holds scoped
// connections into this dispatcher; a connection disconnects itself, in the dispatcher, when its
// service is destroyed. Base classes are destroyed in reverse order of declaration, so the registry
// and every service in it are gone before the dispatcher is.
//
// Until 4.1.1 the dispatcher was a member of World. Members die before base classes: at shutdown
// the dispatcher was freed first and then every service disconnected from freed memory, writing
// into it. The sanitized server harness reported it the first time it ran (work plan 1.7).
struct DispatcherOwner
{
protected:
    entt::dispatcher m_dispatcher;
};
} // namespace WorldParts

struct World : WorldParts::DispatcherOwner, entt::registry
{
    World();
    ~World() noexcept;

    TP_NOCOPYMOVE(World);

    entt::dispatcher& GetDispatcher() noexcept { return m_dispatcher; }
    const entt::dispatcher& GetDispatcher() const noexcept { return m_dispatcher; }
    CharacterService& GetCharacterService() noexcept { return ctx().at<CharacterService>(); }
    const CharacterService& GetCharacterService() const noexcept { return ctx().at<const CharacterService>(); }
    PlayerService& GetPlayerService() noexcept { return ctx().at<PlayerService>(); }
    const PlayerService& GetPlayerService() const noexcept { return ctx().at<const PlayerService>(); }
    PartyService& GetPartyService() noexcept { return ctx().at<PartyService>(); }
    const PartyService& GetPartyService() const noexcept { return ctx().at<const PartyService>(); }
    CalendarService& GetCalendarService() noexcept { return ctx().at<CalendarService>(); }
    const CalendarService& GetCalendarService() const noexcept { return ctx().at<const CalendarService>(); }
    QuestService& GetQuestService() noexcept { return ctx().at<QuestService>(); }
    const QuestService& GetQuestService() const noexcept { return ctx().at<const QuestService>(); }
    RfabProgressionService& GetRfabProgressionService() noexcept { return ctx().at<RfabProgressionService>(); }
    const RfabProgressionService& GetRfabProgressionService() const noexcept { return ctx().at<const RfabProgressionService>(); }
    SaveSnapshotService& GetSaveSnapshotService() noexcept { return ctx().at<SaveSnapshotService>(); }
    const SaveSnapshotService& GetSaveSnapshotService() const noexcept { return ctx().at<const SaveSnapshotService>(); }
    EncounterService& GetEncounterService() noexcept { return ctx().at<EncounterService>(); }
    const EncounterService& GetEncounterService() const noexcept { return ctx().at<const EncounterService>(); }
    ActorValueService& GetActorValueService() noexcept { return ctx().at<ActorValueService>(); }
    const ActorValueService& GetActorValueService() const noexcept { return ctx().at<const ActorValueService>(); }
    InventoryService& GetInventoryService() noexcept { return ctx().at<InventoryService>(); }
    const InventoryService& GetInventoryService() const noexcept { return ctx().at<const InventoryService>(); }
    ObjectService& GetObjectService() noexcept { return ctx().at<ObjectService>(); }
    const ObjectService& GetObjectService() const noexcept { return ctx().at<const ObjectService>(); }
    PlayerManager& GetPlayerManager() noexcept { return m_playerManager; }
    const PlayerManager& GetPlayerManager() const noexcept { return m_playerManager; }
    ScriptService& GetScriptService() const noexcept { return *m_pScriptService; }

    // Null checked at start when MoPo is on!
    ESLoader::RecordCollection* GetRecordCollection() noexcept { return m_recordCollection.get(); }

    const ESLoader::RecordCollection* GetRecordCollection() const noexcept { return m_recordCollection.get(); }

    [[nodiscard]] static uint32_t ToInteger(entt::entity aEntity) { return to_integral(aEntity); }

    // Reserve EnTT entity 0 so no real character/object/player ever receives ServerId == 0
    // (which many client/server/protocol paths treat as "no entity / unassigned sentinel").
    [[nodiscard]] bool valid(entt::entity aEntity) const noexcept
    {
        return aEntity != m_reservedZeroEntity && entt::registry::valid(aEntity);
    }

    auto destroy(entt::entity aEntity)
    {
        if (aEntity == m_reservedZeroEntity)
            return version_type{};
        return entt::registry::destroy(aEntity);
    }

    [[nodiscard]] entt::entity GetReservedZeroEntity() const noexcept { return m_reservedZeroEntity; }

private:
    entt::entity m_reservedZeroEntity{entt::null};

    TiltedPhoques::SharedPtr<AdminService> m_spAdminService;
    TiltedPhoques::UniquePtr<ScriptService> m_pScriptService;
    PlayerManager m_playerManager;
    UniquePtr<ESLoader::RecordCollection> m_recordCollection;
};
