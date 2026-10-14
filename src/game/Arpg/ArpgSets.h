/*
 * ARPG item sets, for the benilla ARPG client (the client's docs/ARPG-CHARACTER.md, "Sets").
 *
 * Twelve sets, one per ARPG class (paladin, mage, druid) at levels 20, 30, 40 and 50: four
 * pieces at 20 and 30 (chest, legs, hands, feet), five at 40 and 50 (head, shoulders, chest,
 * legs, hands), in the class's armour (the paladin mail, then plate). Their pieces are written
 * into item_template at start (entries SET_ENTRY_FROM up), as the base bank's are, each wearing a
 * vanilla item's look and armour: the pieces of one look family (the vanilla items of that armour
 * at that level whose names share their first word, "Glimmering", "Scorpashi"...) where one
 * covers the set, else the nearest item of the slot. A piece is green, wearable by its class
 * only, with the class's attributes on a budget of its level and slot; it never rolls affixes.
 *
 * Worn pieces count per set: each bonus (2, 3, 4 pieces) adds into the player's item totals
 * (attributes, damage by tag, armour, speeds, critical damage, mana, skill levels), and the whole
 * set gives a power, a uniques-kit row on a class spell (Arpg/ArpgUniques.h), named for the set.
 *
 * Drops: a creature of the set's level band (its level to 9 above, the 50s' to 63) drops a random
 * piece of the looter's class's set: a boss 25%, a rare 12%, a champion or elite 4%, the rest 0.6%.
 *
 * A piece's item_template.itemset is its set's id (SET_ID_FROM up): not ItemSet.dbc's, so the
 * stock set code leaves it alone (Item.cpp), and the client draws the set block from:
 *   SMSG_ARPG_SETS (0x34F): uint8 count, per set: uint32 id, cstring name, uint8 class, uint8
 *     level, uint8 piece count, a uint32 entry each, uint8 bonus count, per bonus: uint8 pieces,
 *     cstring text. Sent with every hello.
 */

#ifndef MANGOS_ARPG_SETS_H
#define MANGOS_ARPG_SETS_H

#include "Common.h"
#include "Arpg/ArpgTree.h"
#include "Arpg/ArpgUniques.h"

#include <vector>

class Creature;
class Loot;
class Player;

namespace Arpg
{
    constexpr uint32 SET_ENTRY_FROM = 96000, SET_ENTRY_TO = 96199;
    constexpr uint32 SET_ID_FROM = 1001;

    // Write the sets' pieces into item_template; before the item templates load.
    void EnsureSetItems();

    // Whether item `entry` is a set's piece.
    bool IsSetPiece(uint32 entry);

    // Add the bonuses of `player`'s worn set pieces to `totals`.
    void AddSetTotals(Player const* player, WebTotals& totals);

    // The kit rows of `player`'s whole sets.
    std::vector<UniqueMechanic const*> SetModifiers(Player const* player);

    // `loot` was just made for `looter` from `source`: it may gain a piece of the looter's class's set.
    void MaybeAddSetPiece(Loot* loot, Player const* looter, Creature const* source);

    // Send the set catalog (SMSG_ARPG_SETS).
    void SendSets(Player* player);
}

#endif
