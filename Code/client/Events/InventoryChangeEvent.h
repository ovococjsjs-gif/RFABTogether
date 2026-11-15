#pragma once

#include <Structs/Inventory.h>
#include <ExtraData/ExtraDataList.h>

#include <atomic>

// Numbers the local inventory changes in the order the hooks saw them (defect M28,
// common/CoopLootLedger.h). An event takes its number when the hook builds it, just before the
// game applies the change; a registration of a container reads the counter before it reads the
// container's list. A change with a higher number than the registration noted is not in the list
// the server was sent.
inline std::atomic<uint64_t> g_inventoryChangeSequence{0};

[[nodiscard]] inline uint64_t NextInventoryChangeSequence() noexcept
{
    return g_inventoryChangeSequence.fetch_add(1, std::memory_order_relaxed) + 1;
}

[[nodiscard]] inline uint64_t CurrentInventoryChangeSequence() noexcept
{
    return g_inventoryChangeSequence.load(std::memory_order_relaxed);
}

/**
 * @brief Dispatched when the contents of an object or actor inventory changes locally.
 *
 * The event has a Drop member variable, since dropped items need to be handled differently.
 */
struct InventoryChangeEvent
{
    InventoryChangeEvent(const uint32_t aFormId, Inventory::Entry arItem)
        : FormId(aFormId)
        , Item(std::move(arItem))
    {
    }

    InventoryChangeEvent(const uint32_t aFormId, Inventory::Entry arItem, bool aDrop)
        : FormId(aFormId)
        , Item(std::move(arItem))
        , Drop(aDrop)
    {
    }

    InventoryChangeEvent(const uint32_t aFormId, Inventory::Entry arItem, bool aDrop, bool aUpdateClients)
        : FormId(aFormId)
        , Item(std::move(arItem))
        , Drop(aDrop)
        , UpdateClients(aUpdateClients)
    {
    }

    uint32_t FormId{};
    Inventory::Entry Item{};
    bool Drop = false;
    bool UpdateClients = true;
    // Drop/pickup of a world reference (architecture step 4, WorldItemService). Drop: the reference the
    // engine created (0 if none). Pickup: the temporary reference that was picked up.
    uint32_t WorldReferenceFormId = 0;
    // Pickup only: the engine took the whole stack, so the reference is gone.
    bool WholeStack = true;
    // The change's place among all local inventory changes (see NextInventoryChangeSequence).
    uint64_t Sequence = NextInventoryChangeSequence();
};
