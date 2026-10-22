/*
 * ARPG crafting, for the benilla ARPG client (the client's docs/ARPG-CHARACTER.md, "Crafting").
 *
 * Orbs, PoE's currency on vanilla's gems: seven kinds, dropped by zone events, champions, rares,
 * Wardens and Caches, and paid by bounties. They are items so they can lie on the ground (the
 * server writes their item_template rows at start, as the Codex does), and an ARPG player picking
 * one up keeps it in the character's purse, not the bags (Loot::SendItem asks TakeCraftItem).
 *   Orb of Awakening      a white item turns green (1-2 affixes)
 *   Prism of Refinement   a green turns blue (3-4)
 *   Orb of Ascension      a blue turns purple (5-6)
 *   Fel Orb               every affix rerolled, as many as before
 *   Star of Invocation    one more affix, while the rarity has room
 *   Void Pearl            one affix, at random, gone
 *   Arcane Lens           every affix's value rerolled
 * A unique's affixes are its own: it takes the Arcane Lens (new values) and shards that raise an
 * affix it has, nothing that changes which affixes it has.
 *
 * Gambling, Diablo IV's: gold for a random item of a slot the player names (head, chest, finger,
 * a two-hand weapon...), near the player's level, with better rarity odds than a drop's
 * (GAMBLE_*_PCT). The Forge lists the slots and the price.
 *
 * Shards, Last Epoch's: salvaging an item from the bags (it is destroyed) gives shards of its
 * affixes (one, two on a blue or better) and a chance at an orb. A shard on an item adds its
 * affix (when the rarity has room) or raises the value it has, toward the item's top; each item
 * takes FORGE_LIMIT shard crafts in all. Purse counts are kept with the Codex's (character_arpg_held:
 * orbs 200 + orb, shards 1000 + affix id).
 *
 *   CMSG_ARPG_ACTION kind 24: uint8 op (1-7 an orb, 8 salvage, 9 a shard, 10 a gamble), uint8 bag,
 *     uint8 slot, uint16 affix (a shard's; a gamble's slot kind, 1-16, its bag and slot unused). Bag 255 is the equipped slots, else 0 the backpack, 1-4 the bags,
 *     the slot counting from 1 as the client's API does.
 *   SMSG_ARPG_PURSE (0x34B): uint8 orb count, per orb: uint8 id, uint16 held, cstring name,
 *     cstring text; uint16 shard count, per shard: uint16 affix, uint16 held, cstring name.
 */

#ifndef MANGOS_ARPG_CRAFT_H
#define MANGOS_ARPG_CRAFT_H

#include "Common.h"

class Creature;
class Loot;
class Player;

namespace Arpg
{
    enum Orb : uint8
    {
        ORB_AWAKENING = 1,
        ORB_REFINEMENT,
        ORB_ASCENSION,
        ORB_FEL,
        ORB_INVOCATION,
        ORB_VOID,
        ORB_LENS,
        MAX_ORB
    };

    // An orb's item entry: ORB_ITEM_BASE + its Orb (past the Codex's 90000-91999).
    constexpr uint32 ORB_ITEM_BASE = 92000;

    // The shard crafts one item takes in all.
    constexpr uint8 FORGE_LIMIT = 4;

    // Purse things (Arpg::Held): an orb, a shard.
    constexpr uint16 PURSE_ORB = 200, PURSE_SHARD = 1000;

    // Craft ops beyond the orbs.
    constexpr uint8 CRAFT_SALVAGE = 8, CRAFT_SHARD = 9, CRAFT_GAMBLE = 10;

    // Gambling: the slots one may gamble for (the affix field's 1-16), the gold it costs at a
    // level (GAMBLE_COST_PER_LEVEL_SQ copper times the level squared: 30 silver at 10, about 11 gold
    // at 60; the client's Forge shows the same sum), and the rarity odds in percent (the rest green).
    constexpr uint8 GAMBLE_KIND_COUNT = 16;
    constexpr uint32 GAMBLE_COST_PER_LEVEL_SQ = 30, GAMBLE_COST_MIN = 100;
    constexpr uint32 GAMBLE_UNIQUE_PCT = 4, GAMBLE_PURPLE_PCT = 14, GAMBLE_BLUE_PCT = 36;

    // A gamble's price at `level`, in copper.
    uint32 GambleCost(uint32 level);

    // Write the orbs into item_template; before the item templates load.
    void EnsureCraftItems();

    // Whether `item` is an orb `player` keeps in the purse (then it is taken).
    bool TakeCraftItem(Player* player, uint32 item, uint32 count);

    // A random orb's item entry, the common ones likelier.
    uint32 RandomOrbItem();

    // Add `count` random orbs to `loot`.
    void AddOrbsToLoot(Loot* loot, uint32 count);

    // Give `player` `count` random orbs, straight to the purse, and say so.
    void GiveOrbs(Player* player, uint32 count);

    // `victim`'s corpse loot was just made: a champion, rare or boss may add orbs (a little more
    // each dungeon tier), and now and then any foe near the looter's level.
    void OnCraftLoot(Creature* victim);

    // Do a craft on the item at `bag`/`slot` (see above), and answer with the purse and the item.
    void Craft(Player* player, uint8 op, uint8 bag, uint8 slot, uint16 affix);

    // Send `player` its purse.
    void SendPurse(Player* player);
}

#endif
