/*
 * ARPG item affixes, for the benilla ARPG client (the client's docs/ARPG-CHARACTER.md, "Items").
 *
 * Every green, blue and purple weapon or armour piece that drops for an ARPG player, other than a
 * named unique (Arpg/ArpgUniques.h), rolls affixes on top of its own stats: green 1, blue 2,
 * purple 3, from the passive web's vocabulary: attributes, tag damage (+% Holy, Physical, Area,
 * Melee, Spell), healing, armour, block, movement speed, cooldown recovery and life on kill. A
 * value scales with the item level, so a level 60 drop rolls up to the table's top and a level 10
 * one about a sixth of it.
 *
 * An item's affixes come from one seed: rolled into the loot item when the corpse's loot is made
 * (SeedArpgLoot), mixed with the looter's guid and kept with the item when it is stored
 * (character_arpg_item: item guid, entry, seed, created at runtime; loaded by the item's current
 * holder, so a trade keeps them), and derived again whenever they are needed, so they never drift.
 * Worn, they add into the player's totals beside the web's and the skill trees'.
 *
 *   SMSG_ARPG_ITEM_AFFIXES (0x343): uint16 count, per item: uint64 item guid, cstring lines
 *     ("\n" between them). Sent after the hello for every affixed item the character has, and
 *     when one is stored.
 */

#ifndef MANGOS_ARPG_AFFIXES_H
#define MANGOS_ARPG_AFFIXES_H

#include "Common.h"
#include "Arpg/ArpgTree.h"

#include <string>
#include <vector>

class Item;
class Loot;
class Player;

namespace Arpg
{
    // `loot` was just made for an ARPG looter: seed its affix-taking items.
    void SeedArpgLoot(Loot* loot, Player* looter);

    // `item` was stored from a loot item with `seed`: keep the seed with it, and tell the client.
    void OnAffixedItemStored(Player* player, Item* item, uint32 seed);

    // Add the player's worn items' affixes to `totals`.
    void AddItemTotals(Player const* player, WebTotals& totals);

    // An item went on or came off: refresh the totals, a moment later.
    void OnEquipChanged(Player* player, Item* item);

    // After the hello: load the character's affixed items and send their lines.
    void LoadAffixes(Player* player);

    // The tooltip lines of an item of `entry` and `level` with `seed`.
    std::vector<std::string> AffixLines(uint32 entry, uint32 seed);
}

#endif
