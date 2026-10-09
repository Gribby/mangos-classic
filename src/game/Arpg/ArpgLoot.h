/*
 * ARPG ground loot, for the benilla ARPG client (see ArpgCombat.h).
 *
 * Loot stays where cmangos keeps it, on the corpse; the ARPG client only shows it differently,
 * as items lying on the ground around the corpse that the player clicks to pick up. The server
 * tells an ARPG player what a corpse holds for them (SMSG_ARPG_LOOT) when it dies and whenever
 * that changes, and takes one item or the gold off it on request (CMSG_ARPG_ACTION, ACTION_LOOT)
 * from a few yards away, through the stock loot code, so loot rights, full bags and the corpse's
 * looted state all work as they do for the loot window. A client that comes upon a lootable corpse
 * it has no list for (it walked back, relogged, or a group member made the kill out of its sight)
 * asks for one (ACTION_LOOT_QUERY).
 *
 * An ARPG group has no loot rules: its corpse loot is free for all (Loot::SetGroupLootRight), so
 * nothing is held for a roll and every item shows on the ground for every member who may take it.
 * Nor does anything bind to an ARPG player, on pickup, equip or use (Arpg::ItemsBind).
 *
 *   SMSG_ARPG_LOOT: uint64 corpse, uint32 gold, uint8 item count, then per item:
 *                   uint8 loot slot, uint32 item id, uint32 display id, uint8 quality, uint8 count
 *   An empty list (no gold, no items) means nothing is left there for this player.
 */

#ifndef MANGOS_ARPG_LOOT_H
#define MANGOS_ARPG_LOOT_H

#include "Common.h"
#include "Entities/ObjectGuid.h"

class Creature;
class Loot;
class Player;

namespace Arpg
{
    // The loot slot that names the corpse's gold in ACTION_LOOT.
    constexpr uint8 LOOT_SLOT_GOLD = 0xFF;

    // How near the corpse the player must be to pick its loot up, in yards, past both bodies'
    // combat reach: the client lays the items out within a few yards of it.
    constexpr float GROUND_LOOT_RANGE = 6.0f;

    // Send `player` what `creature`'s corpse holds for them, or an empty list.
    void SendGroundLoot(Player* player, Creature* creature);

    // A creature died and its corpse loot was generated: tell its ARPG looters.
    void OnCorpseLoot(Creature* creature);

    // `loot` changed for `player` (an item or the gold taken, by any route): resend their list.
    void OnLootChanged(Loot* loot, Player* player);

    // Take the item in `slot` (or the gold, LOOT_SLOT_GOLD) off `corpseGuid`'s loot for `player`.
    // Runs on the world thread, as the stock loot opcodes do.
    void PickLoot(Player* player, ObjectGuid corpseGuid, uint8 slot);

    // Send `player` `corpseGuid`'s ground loot list, if it is a corpse near them.
    void QueryLoot(Player* player, ObjectGuid corpseGuid);
}

#endif
