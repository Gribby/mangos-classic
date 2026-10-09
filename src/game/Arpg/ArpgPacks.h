/*
 * ARPG mob packs, for the benilla ARPG client (the client's docs/ARPG-PROGRESSION.md, "Packs").
 *
 * With Arpg.Enable and Arpg.Packs on, an open-world mob may lead a pack when it spawns: followers
 * of its own kind, one or two levels lower, a little smaller, with Arpg.Packs.FollowerPower of its
 * health and damage. A pack's size grows with the leader's level, because the ARPG paladin's area
 * damage does:
 *   level 1-5: 1-2 in all;  6-11: 2-3;  12-19: 3-4;  20 and up: 3-5.
 * Followers are temporary summons: they don't respawn, they share the pack's aggro, their corpses
 * despawn after PACK_CORPSE_MS, and the pack forms afresh when its leader respawns. A follower
 * gives Arpg.Packs.FollowerXp of a kill's XP and Arpg.Packs.FollowerLoot of its gold, and keeps
 * each item that isn't for a quest at that chance.
 *
 * A pack leader is a hostile or neutral open-world creature of normal rank from the `creature`
 * table: no NPC flags, not a civilian, critter, pet, summon or member of a creature group.
 *
 * Each pack fight is logged when its last member falls ("ARPG packs:"): its size, level, how
 * long it took from the first aggro, and the damage it dealt, for tuning.
 *
 *   CMSG_ARPG_ACTION kind 16, with Arpg.DevTools on: uint8 size (0: by level). The nearest
 *   eligible mob within 40 yards forms a pack of that size.
 */

#ifndef MANGOS_ARPG_PACKS_H
#define MANGOS_ARPG_PACKS_H

#include "Common.h"

class Creature;
class Player;
class Unit;

namespace Arpg
{
    // A creature was added to its map (spawned, respawned, or its grid loaded): it may lead a pack.
    void OnCreatureAdded(Creature* creature);

    // Whether `unit` is a pack follower.
    bool IsPackFollower(Unit const* unit);

    // The multiplier on damage `attacker` deals: a follower's share, else 1.
    float PackDamageMod(Unit const* attacker);

    // The multiplier on XP for killing `victim`: a follower's share, else 1.
    float PackXpMod(Unit const* victim);

    // `victim`'s corpse loot was just made: a follower's is thinned.
    void ThinPackLoot(Creature* victim);

    // `member` entered combat with `enemy`: the rest of its pack joins in.
    void OnPackAggro(Creature* member, Unit* enemy);

    // `attacker` dealt `damage` to `victim` (the fight log).
    void OnPackDamage(Unit* attacker, Unit* victim, uint32 damage);

    // `victim` died: the fight log, once the whole pack is down.
    void OnPackMemberDied(Unit* victim);

    // Dev tools: the nearest eligible mob forms a pack of `size` (0: by level).
    void DevFormPack(Player* player, uint8 size);
}

#endif
