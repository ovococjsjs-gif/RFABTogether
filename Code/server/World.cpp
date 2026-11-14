#include <World.h>
#include <Components.h>

#include <algorithm>

#include <Services/CharacterService.h>
#include <Services/ObjectService.h>
#include <Services/QuestService.h>
#include <Services/ServerListService.h>
#include <Services/ActorValueService.h>
#include <Services/AdminService.h>
#include <Services/InventoryService.h>
#include <Services/MagicService.h>
#include <Services/OverlayService.h>
#include <Services/CommandService.h>
#include <Services/StringCacheService.h>
#include <Services/SessionResyncService.h>
#include <Services/WorldItemService.h>
#include <Services/CombatService.h>
#include <Services/WeatherService.h>
#include <Services/ScriptService.h>
#include <Services/MapService.h>

#include <es_loader/ESLoader.h>

World::World()
{
    // Claim entity 0 up front before any service or checkpoint hydration creates entities.
    m_reservedZeroEntity = create();

    m_spAdminService = std::make_shared<AdminService>(*this, m_dispatcher);
    spdlog::default_logger()->sinks().push_back(std::static_pointer_cast<spdlog::sinks::sink>(m_spAdminService));

    ctx().emplace<RfabProgressionService>(*this, m_dispatcher);
    ctx().emplace<SaveSnapshotService>(*this, m_dispatcher);
    ctx().emplace<EncounterService>(*this, m_dispatcher);
    ctx().emplace<CharacterService>(*this, m_dispatcher);
    ctx().emplace<PlayerService>(*this, m_dispatcher);
    ctx().emplace<CalendarService>(*this, m_dispatcher);
    ctx().emplace<ObjectService>(*this, m_dispatcher);
    ctx().emplace<ModsComponent>();
    ctx().emplace<ServerListService>(*this, m_dispatcher);
    ctx().emplace<QuestService>(*this, m_dispatcher);
    ctx().emplace<PartyService>(*this, m_dispatcher);
    ctx().emplace<ActorValueService>(*this, m_dispatcher);
    ctx().emplace<InventoryService>(*this, m_dispatcher);
    // After InventoryService (its one inventory writer) and before HydratePersistedWorldState below,
    // which restores the dropped items of the latest checkpoint.
    ctx().emplace<WorldItemService>(*this, m_dispatcher);
    ctx().emplace<MagicService>(*this, m_dispatcher);
    ctx().emplace<OverlayService>(*this, m_dispatcher);
    ctx().emplace<CommandService>(*this, m_dispatcher);
    ctx().emplace<StringCacheService>(*this, m_dispatcher);
    ctx().emplace<SessionResyncService>(*this, m_dispatcher);
    ctx().emplace<CombatService>(*this, m_dispatcher);
    ctx().emplace<WeatherService>(*this, m_dispatcher);
    ctx().emplace<MapService>(*this, m_dispatcher);

    GetSaveSnapshotService().HydratePersistedWorldState();

    ESLoader::ESLoader loader;
    // emplace loaded mods into modscomponent.
    m_recordCollection = loader.BuildRecordCollection();
    for (const auto& it : loader.GetLoadOrder())
    {
        ctx().emplace<ModsComponent>().AddServerMod(it);
    }

    // late initialize the ScriptService to ensure all components are valid
    m_pScriptService = TiltedPhoques::MakeUnique<ScriptService>(*this, m_dispatcher);
}

World::~World()
{
    // The admin service is also a log sink: the constructor hands it to the default logger, which
    // would keep it alive - with its reference to this World and its connection into the dispatcher -
    // until the process exits. Every log line written after this World is gone would be handed to
    // it, and its connection would be released at exit into a dispatcher freed long before (found by
    // the sanitized server harness, defect M20). The logger gives it back here; as the last owner,
    // m_spAdminService then destroys it with the other members, while the dispatcher is alive.
    if (m_spAdminService)
    {
        if (const auto logger = spdlog::default_logger())
        {
            auto& sinks = logger->sinks();
            const auto asSink = std::static_pointer_cast<spdlog::sinks::sink>(m_spAdminService);
            sinks.erase(std::remove(sinks.begin(), sinks.end(), asSink), sinks.end());
        }
    }

    m_pScriptService.reset();
}
