/*
 * ARPG item affixes, for the benilla ARPG client (the client's docs/ARPG-CHARACTER.md, "Items").
 *
 * Loot is Diablo's, on vanilla's items. Every weapon and armour piece that drops for an ARPG
 * player is a base, and rolls its rarity at the drop:
 *   - An ordinary item (vanilla's whites and greens, their random "of the Bear" suffix kept as its
 *     implicit) rolls white (no affixes), green (1-2), blue (3-4) or purple (5-6), by who dropped
 *     it (trash, an elite or champion, a rare, Warden or boss) and the dungeon tier; its name
 *     shows the rolled colour, whatever vanilla's colour was.
 *   - A named blue or purple (and every mechanic's item, Arpg/ArpgUniques.h) is a unique: gold,
 *     its own stats its implicits, and affixes as a rare (a vanilla blue, 3-4) or an epic (a
 *     vanilla purple, 5-6), which its border shows.
 * A raid drop (item level 66+) or a Torment one rolls the top of its range. The affixes come from a
 * long list (attributes, damage by tag and by creature type, attack and cast speed, crit, spell
 * power, leech, regeneration, resistances, damage taken, thorns, movement, gold and experience),
 * some limited to weapons, armour, boots, shields or jewellery. A value scales with the item level
 * (exponentially: a level 20 drop about a seventh of a level 60 one, Naxxramas three to four
 * times) and the tier it dropped at.
 *
 * An item's affixes come from one seed: rolled into the loot item with its rarity when the corpse's
 * loot is made (SeedArpgLoot), mixed with the looter's guid and kept with the item when it is stored
 * (character_arpg_item: item guid, entry, seed, tier, rarity; columns added to an older table),
 * loaded by the item's current holder, so a trade keeps them, and derived again whenever they are
 * needed. Worn, they add into the player's totals beside the web's and the skill trees'.
 *
 *   SMSG_ARPG_ITEM_AFFIXES (0x343): uint16 count, per item: uint64 item guid, uint8 the colour its
 *     name shows (Rarity), uint8 a unique's border (its affixes' Rarity; 0 for the rest), cstring
 *     lines ("\n" between). Sent after the hello for every affixed item, and when one is stored.
 *   SMSG_ARPG_LOOT's per-item quality is the rolled rarity, and a unique's border follows it
 *     (Arpg/ArpgLoot.h).
 */

#ifndef MANGOS_ARPG_AFFIXES_H
#define MANGOS_ARPG_AFFIXES_H

#include "Common.h"
#include "Arpg/ArpgTree.h"

#include <string>
#include <vector>

class Creature;
class Item;
class Loot;
class Player;
struct LootItem;

namespace Arpg
{
    // An item's rolled rarity: its name's colour (the item qualities' numbers) and its affix count.
    enum Rarity : uint8
    {
        RARITY_NONE   = 0,    // not rolled
        RARITY_WHITE  = 1,
        RARITY_GREEN  = 2,
        RARITY_BLUE   = 3,
        RARITY_PURPLE = 4,
        RARITY_UNIQUE = 6,    // the Artifact colour, gold
    };

    // `loot` was just made for an ARPG looter, at dungeon tier `tier`, dropped by `source` (for its
    // rarity odds; nullptr: trash): seed its items and roll their rarities; gold find adds gold.
    void SeedArpgLoot(Loot* loot, Player* looter, uint8 tier, Creature const* source, bool goldFind = true);

    // Add a random base item near item level `level` at `rarity` to `loot` (RARITY_UNIQUE: a
    // random unique). False when none was found or the loot is full.
    bool AddRolledItem(Loot* loot, uint8 rarity, uint32 level);

    // `item` was stored from a loot item with `seed`, `tier` and `rarity`: keep them with it, and
    // tell the client.
    void OnAffixedItemStored(Player* player, Item* item, uint32 seed, uint8 tier, uint8 rarity);

    // The quality a ground loot item shows (its rolled rarity), and a unique's border.
    uint8 LootItemQuality(LootItem const* item, uint8* border);

    // Add the player's worn items' affixes to `totals`.
    void AddItemTotals(Player const* player, WebTotals& totals);

    // An item went on or came off: refresh the totals, a moment later.
    void OnEquipChanged(Player* player, Item* item);

    // After the hello: load the character's affixed items and send their lines.
    void LoadAffixes(Player* player);

    // The tooltip lines of an item of `entry` with `seed`, `tier` and `rarity`.
    std::vector<std::string> AffixLines(uint32 entry, uint32 seed, uint8 tier, uint8 rarity);

    // Make a random base item (as AddRolledItem) and give it to `player`: into the bags, or by
    // mail when they are full.
    bool GiveRolledItem(Player* player, uint8 rarity, uint32 level);

    // --- Crafting (Arpg/ArpgCraft.h) ---
    // A crafted item keeps its affixes as a list (character_arpg_item.affixes) in place of its
    // seed's, and counts its shard crafts (forged). An affix's id is its place in the affix list,
    // from 1; the list only grows at its end, so an id always names the same affix.

    enum CraftResult : uint8
    {
        CRAFT_OK, CRAFT_NOT_ITEM, CRAFT_WRONG_RARITY, CRAFT_NO_AFFIXES, CRAFT_FULL, CRAFT_FORGED_OUT,
        CRAFT_NO_FIT, CRAFT_AT_TOP,
    };

    // How many affixes there are (ids 1 to this).
    uint16 AffixTotal();
    // A shard's name, by its affix id.
    std::string ShardName(uint16 affix);
    // Whether `item` takes crafting: a weapon or armour piece.
    bool Craftable(Item const* item);
    // The rarity `item` shows (0: not craftable).
    uint8 ShownRarity(Item const* item);
    // Why a craft was refused, for the player.
    char const* CraftError(CraftResult result);
    // Apply orb `op` (Orb) or CRAFT_SHARD with `affix` to `item`: store it, send its lines, and
    // say in `done` what happened. The caller takes the orb or shard on CRAFT_OK.
    CraftResult CraftItem(Player* player, Item* item, uint8 op, uint16 affix, std::string& done);
    // The shards salvaging `item` gives (its affixes': one, two from a blue or better).
    std::vector<uint16> SalvageShards(Item const* item);
}

#endif
