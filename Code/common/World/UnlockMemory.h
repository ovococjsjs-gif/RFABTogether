#pragma once

/**
 * An unlock the server heard about before it knew the object.
 *
 * A client registers the doors and containers of the one cell its player stands in, and reports
 * every lock change its game makes, registered object or not. So a lock picked across an exterior
 * cell border (the player stands in one cell, the door or chest belongs to the next) reaches the
 * server as "unlocked" for an object it has no record of. The server relayed it to the players in
 * range and kept nothing. Whoever walked into that cell first then registered the object as his
 * own game showed it: a party member who had been elsewhere registered it locked, and the player
 * who picked the lock got it locked again on his next visit.
 *
 * Found by reading the code (work plan 5.4, defect M23); how often it happens in a real session
 * is not known.
 *
 * This is the record that was missing. It holds unlocks only:
 *
 *  - an unlock is the safe direction. The worst a stale one does is open a door that one player
 *    of the party has already opened;
 *  - a remembered "locked" could shut a player out of a room the others stand in (arena barriers
 *    are locked and unlocked by scripts that do not report every change), so a lock report only
 *    cancels a remembered unlock and is otherwise left to the registration that follows.
 *
 * No actors, no packets: values in, values out.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace WorldObjects
{

template <size_t Capacity = 2048>
class UnlockMemory
{
public:
    static constexpr size_t kCapacity = Capacity;

    // A lock change reported for an object that is not registered. Returns true when the report
    // changed what is remembered.
    bool Note(uint64_t aObjectKey, bool aIsLocked, uint8_t aLockLevel) noexcept
    {
        if (aObjectKey == 0)
            return false;

        if (Entry* pEntry = Find(aObjectKey))
        {
            if (aIsLocked)
            {
                *pEntry = Entry{};
                --m_count;
                return true;
            }
            const bool changed = pEntry->LockLevel != aLockLevel;
            pEntry->LockLevel = aLockLevel;
            return changed;
        }

        if (aIsLocked)
            return false;

        Entry* pSlot = nullptr;
        if (m_count < Capacity)
        {
            for (Entry& entry : m_entries)
            {
                if (!entry.Used)
                {
                    pSlot = &entry;
                    break;
                }
            }
        }
        if (pSlot != nullptr)
        {
            ++m_count;
        }
        else
        {
            // Full: an old record gives way - an unlock nobody came back for while as many
            // other objects as the memory holds were unlocked.
            pSlot = &m_entries[m_cursor % Capacity];
            ++m_cursor;
        }
        *pSlot = Entry{aObjectKey, aLockLevel, true};
        return true;
    }

    // The object is being registered. Returns the lock level of the remembered unlock, if there
    // is one, and forgets it: from here on the registered object carries the state.
    [[nodiscard]] std::optional<uint8_t> Take(uint64_t aObjectKey) noexcept
    {
        if (m_count == 0 || aObjectKey == 0)
            return std::nullopt;

        if (Entry* pEntry = Find(aObjectKey))
        {
            const uint8_t level = pEntry->LockLevel;
            *pEntry = Entry{};
            --m_count;
            return level;
        }
        return std::nullopt;
    }

    [[nodiscard]] bool Remembers(uint64_t aObjectKey) const noexcept
    {
        if (m_count == 0 || aObjectKey == 0)
            return false;
        for (const Entry& entry : m_entries)
        {
            if (entry.Used && entry.ObjectKey == aObjectKey)
                return true;
        }
        return false;
    }

    [[nodiscard]] size_t Size() const noexcept { return m_count; }

    // A rollback returns the world to an earlier moment: unlocks made after it never happened.
    void Clear() noexcept
    {
        m_entries.fill(Entry{});
        m_cursor = 0;
        m_count = 0;
    }

private:
    struct Entry
    {
        uint64_t ObjectKey{0};
        uint8_t LockLevel{0};
        bool Used{false};
    };

    [[nodiscard]] Entry* Find(uint64_t aObjectKey) noexcept
    {
        if (m_count == 0)
            return nullptr;
        for (Entry& entry : m_entries)
        {
            if (entry.Used && entry.ObjectKey == aObjectKey)
                return &entry;
        }
        return nullptr;
    }

    std::array<Entry, Capacity> m_entries{};
    size_t m_cursor{0};
    size_t m_count{0};
};

// What the first registration of an object does with a remembered unlock.
struct RegistrationLock
{
    bool IsLocked{false};
    uint8_t LockLevel{0};
    // The registrant's own game still shows the object locked: it has to be told.
    bool TellRegistrant{false};
};

[[nodiscard]] constexpr RegistrationLock ResolveRegistrationLock(bool aRegistrantSeesLocked, uint8_t aRegistrantLockLevel,
                                                                 std::optional<uint8_t> aRememberedUnlockLevel) noexcept
{
    RegistrationLock result{};
    result.IsLocked = aRegistrantSeesLocked;
    result.LockLevel = aRegistrantLockLevel;
    if (aRememberedUnlockLevel.has_value() && aRegistrantSeesLocked)
    {
        result.IsLocked = false;
        result.LockLevel = *aRememberedUnlockLevel;
        result.TellRegistrant = true;
    }
    return result;
}

} // namespace WorldObjects
