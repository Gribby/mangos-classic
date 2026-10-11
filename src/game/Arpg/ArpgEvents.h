/*
 * ARPG zone events and the bounty board, for the benilla ARPG client (the client's
 * docs/ARPG-PROGRESSION.md, "Zone events and bounties").
 *
 * Zone events. In the open world (Eastern Kingdoms and Kalimdor), a director starts an event in
 * each zone an ARPG player is in, every few minutes, a stone's throw from them, with the zone's
 * own creatures: it anchors on a hostile creature near the player's level (no script, no quest
 * ties, not a vendor) and summons its kin around it.
 *   Purge   kill `need` enemies within EVENT_RADIUS of the spot before the time runs out; the
 *           kin summoned fill the camp out.
 *   Hunt    a named rare of the anchor's kind and its pack roam the spot; kill the rare in time.
 *   Siege   three waves of the anchor's kind run at the spot and whoever holds it; a champion
 *           leads the last.
 * The final kill's corpse bursts into a cache (rolled items, a chance at a unique, crafting orbs,
 * gold), and every ARPG player who fought in it or stands near gains XP (a share of a level) and
 * the event's bounty credit. Out of time, the event fails and its summons go. The next comes
 * EVENT_COOLDOWN later.
 *
 *   SMSG_ARPG_EVENT (0x349): uint8 state (1 live, 2 done, 3 failed, 4 gone: none here), uint32
 *     id, uint8 type (1 purge, 2 hunt, 3 siege), float x, y, z, float radius, uint16 have,
 *     uint16 need, uint32 ms left, cstring title, cstring objective. To the zone's ARPG players
 *     on a change and every few seconds.
 *
 * Bounties belong to the zone the character is in: three for it (complete its events, slay its
 * creatures, slay its champions and rares; the third now and then a dungeon of its level), kept
 * per zone so going back finds them as left. A city or a zone the character has outlevelled by
 * more than 5 has none; a dungeon keeps the zone's it was entered from. Nothing is handed in: a
 * finished bounty pays at once (a share of a level's XP, gold, orbs, a rolled item) and a new one
 * of its kind takes its slot; one can be abandoned for another (CMSG_ARPG_ACTION kind 25, uint8
 * slot). Saved in character_arpg_zone_bounty (guid, zone, slot, kind, target, have, need), made if
 * missing.
 *
 *   SMSG_ARPG_BOUNTIES (0x34A): uint8 count, per bounty: uint8 slot, uint8 kind, uint16 have,
 *     uint16 need, cstring title, cstring reward.
 */

#ifndef MANGOS_ARPG_EVENTS_H
#define MANGOS_ARPG_EVENTS_H

#include "Common.h"

class Creature;
class Player;
class Unit;

namespace Arpg
{
    // Every player update: the director and the zone's event (throttled inside).
    void UpdateEvents(Player* player);

    // `killer` (an ARPG player, or its group) is credited with `victim`'s death: event and bounty
    // progress. Before the corpse's loot is made.
    void OnEventKill(Player* killer, Unit* victim);

    // `victim`'s corpse loot was just made: a finished event's cache goes into it.
    void OnEventLoot(Creature* victim);

    // After the hello: load the character's bounties and send them, with the zone's event.
    void LoadBounties(Player* player);

    // Abandon bounty `slot` for a new one.
    void AbandonBounty(Player* player, uint8 slot);

    // A logged-out player's bounties and event state.
    void ForgetEvents(Player* player);
}

#endif
