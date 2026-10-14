/*
 * ARPG base items past vanilla's, for the benilla ARPG client (the client's
 * docs/ARPG-CHARACTER.md, "Base items for the raids").
 *
 * Vanilla's whites and greens stop near item level 63, so a rolled green, blue or purple from a
 * raid, or from a dungeon on a high tier, had a dungeon item's base and a dungeon item's affixes
 * (they scale from the base's own item level). The bank fills the gap: one base for every slot
 * and armour or weapon kind, at each of BANK_TIERS item levels from 64 to 92, written into
 * item_template at start (entries BANK_BASE_ENTRY and up), as the Codex's and the orbs' are.
 *   - A base's armour (or damage, or a shield's block) is fitted from vanilla's own blues and epics
 *     of its kind, at its item level, a little under them.
 *   - Its implicits are vanilla's green suffixes, one family per base (so a kind comes as up to
 *     three bases: "Molten Breastplate of the Bear", "... of the Tiger", "... of Healing"): two
 *     attributes, or intellect with spell damage or healing as vanilla's own equip spells, on a
 *     budget of its item level and slot. The affixes it rolls come on top.
 *   - It wears the look of a vanilla blue or epic of its kind near its item level, and a name of
 *     the content it belongs to ("Molten Breastplate", "Necrotic Greatsword").
 *   - It is never a unique: vanilla's named blues and purples are those.
 *
 * A rolled drop at item level BANK_FROM or more takes its base from the bank (PickRandomItem). In
 * a raid, a white or green the corpse drops becomes the bank's base of its kind at the raid's
 * item level, and the raid's foes drop rolled bases of their own (OnRaidLoot).
 */

#ifndef MANGOS_ARPG_BASES_H
#define MANGOS_ARPG_BASES_H

#include "Common.h"

#include <map>
#include <string>
#include <vector>

class Creature;
class Map;

namespace Arpg
{
    constexpr uint32 BANK_BASE_ENTRY = 94000;
    // The first bank's entries (no implicits), kept for items already held, never dropped.
    constexpr uint32 OLD_BANK_FROM = 93000, OLD_BANK_TO = 93999;
    // From this item level a rolled drop's base comes from the bank.
    constexpr uint32 BANK_FROM = 64;

    // `text` escaped for a quoted SQL literal.
    std::string SqlEscaped(std::string const& text);

    // Copy item `donor`'s row to `entry` in item_template, every column but `overrides` (column ->
    // SQL literal) as the donor has it; only `columns`, the ones this database's table has, are named.
    void CopyItemRow(std::vector<std::string> const& columns, uint32 entry, uint32 donor,
                     std::map<std::string, std::string> const& overrides);

    // Write the bank into item_template; before the item templates load.
    void EnsureBaseItems();

    // A random bank base near item level `level` (0: none, below the bank).
    uint32 RandomBase(uint32 level);

    // The item level a raid's drops roll at (with its tier), or 0 for a map that is not a raid.
    uint32 RaidItemLevel(Map* map);

    // `victim`'s corpse loot was just made: in a raid, its whites and greens become the bank's
    // bases at the raid's item level, and it may drop rolled bases of its own (before their seeds).
    void OnRaidLoot(Creature* victim);
}

#endif
