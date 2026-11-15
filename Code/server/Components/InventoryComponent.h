#pragma once

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#error Include Components.h instead
#endif

#include <Structs/Inventory.h>

struct InventoryComponent
{
    Inventory Content{};
    // False on dormant persistent NPCs hydrated from rfab_checkpoints.bin (which stores
    // position/health only) until a live client supplies the NPC's concrete inventory.
    bool HasSeededInventory{false};
};
