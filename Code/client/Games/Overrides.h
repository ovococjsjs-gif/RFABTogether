#pragma once

// Sets a thread-local flag for the current scope and puts the PREVIOUS value back when the scope
// ends (work plan 4.3). `flag = true; call(); flag = false;` is wrong as soon as the call can come
// back into the same wrapper on the same thread: the inner call clears the flag under the outer one,
// which then goes on unguarded. Tools/rfab/check_hook_discipline.py rejects the literal reset.
template <class T> struct ScopedTlsValue
{
    ScopedTlsValue(T& aSlot, T aValue) noexcept
        : m_slot(aSlot)
        , m_previous(aSlot)
    {
        aSlot = aValue;
    }

    ~ScopedTlsValue() noexcept { m_slot = m_previous; }

    TP_NOCOPYMOVE(ScopedTlsValue);

private:
    T& m_slot;
    T m_previous;
};

template <class T> struct ScopedOverride
{
    ScopedOverride() { s_refCount++; }

    ~ScopedOverride() { s_refCount--; }

    TP_NOCOPYMOVE(ScopedOverride);

    static bool IsOverriden() { return s_refCount > 0; }

private:
    static thread_local uint32_t s_refCount;
};

namespace details
{
struct Equip
{
};
struct Unequip
{
};
struct SaveLoad
{
};
struct Experience
{
};
struct Activate
{
};
struct Inventory
{
};
struct ExtraData
{
};
struct Quest
{
};
struct SpellCast
{
};
struct ActorValue
{
};
} // namespace details

using ScopedReferencesOverride = ScopedOverride<TESObjectREFR>;
using ScopedEquipOverride = ScopedOverride<details::Equip>;
using ScopedUnequipOverride = ScopedOverride<details::Unequip>;
using ScopedSaveLoadOverride = ScopedOverride<details::SaveLoad>;
using ScopedExperienceOverride = ScopedOverride<details::Experience>;
using ScopedActivateOverride = ScopedOverride<details::Activate>;
using ScopedInventoryOverride = ScopedOverride<details::Inventory>;
using ScopedExtraDataOverride = ScopedOverride<details::ExtraData>;
using ScopedQuestOverride = ScopedOverride<details::Quest>;
using ScopedSpellCastOverride = ScopedOverride<details::SpellCast>;
using ScopedActorValueOverride = ScopedOverride<details::ActorValue>;
