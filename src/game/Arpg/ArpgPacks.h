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
 * A pack leader is an open-world creature of normal rank from the `creature` table that isn't
 * friendly to players: no NPC flags, not a civilian, critter, pet or summon. In a spawn group it
 * leads only if the group isn't a formation and it is the group's first member, so a camp gets
 * one pack. The dev tools' test pack logs why the nearest mob was refused.
 *
 * Each pack fight is logged when its last member falls ("ARPG packs:"): its size, level, how
 * long it took from the first aggro, and the damage it dealt, for tuning.
 *
 * Champions and rares (from level 8): about 1 pack in 8 brings two or three
 * champions (three from level 20), sharing one affix (two from level 30); about 1 in 40 brings
 * a rare, with a name of its own and two affixes (three from level 30). Both are extra temporary
 * summons of the leader's kind, so the spawned mob itself never changes. A champion has three
 * times a mob's health, 30% more damage, a larger model, three times the XP and a bonus drop; a
 * rare four times the health, 50% more damage, five times the XP, a blue drop and a chance at a
 * purple. Affixes:
 *   Extra Strong   +50% damage
 *   Extra Fast     +50% attack speed, +30% movement speed
 *   Stone Skin     -40% damage taken
 *   Fire Enchanted its hits burn for a quarter more as fire; it bursts a second after dying
 *   Cold Enchanted its hits chill; at 30% health it casts Frost Nova, once
 *   Vampiric       heals for half the melee damage it deals
 *   Thorns         a fifth of the melee damage it takes strikes back as nature
 *   Teleporter     blinks to its victim when it is more than 12 yards off, every 6 sec at most
 *   Healer         heals its pack within 20 yards for 8% of their health every 4 sec
 *
 *   SMSG_ARPG_CHAMPIONS (0x341): uint8 count, per champion: uint64 guid, uint8 tier (1 champion,
 *     2 rare), cstring name ("": the creature's own), uint8 affix count, a cstring each. Sent to
 *     each ARPG player for the champions within 100 yards it hasn't been told of, once a second.
 *
 *   CMSG_ARPG_ACTION kind 16, with Arpg.DevTools on: uint8 size (0: by level), optional uint8
 *   tier (0 by chance, 1 no champions, 2 a champion pack, 3 a rare). The nearest eligible mob
 *   within 40 yards forms a pack of that size.
 */

#ifndef MANGOS_ARPG_PACKS_H
#define MANGOS_ARPG_PACKS_H

#include "Common.h"

#include <string>
#include <vector>

class Creature;
class Player;
class Unit;

namespace Arpg
{
    // A creature was added to its map (spawned, respawned, or its grid loaded): it may lead a pack.
    void OnCreatureAdded(Creature* creature);

    // Whether `unit` is a pack follower.
    bool IsPackFollower(Unit const* unit);

    // The multiplier on damage `attacker` deals: a follower's share, a champion's, else 1.
    float PackDamageMod(Unit const* attacker);

    // The multiplier on damage `victim` takes (Stone Skin).
    float PackDamageTakenMod(Unit const* victim);

    // The multiplier on `unit`'s run speed (Extra Fast).
    float PackSpeedMod(Unit const* unit);

    // A melee swing of `attacker` hit `victim` for `damage` (Fire and Cold Enchanted, Vampiric,
    // Thorns).
    void OnPackMelee(Unit* attacker, Unit* victim, uint32 damage);

    // Tell `player` of the champions near it it hasn't heard of (SMSG_ARPG_CHAMPIONS).
    void SendNearbyChampions(Player* player);

    // Forget what a logged-out player was told.
    void ForgetChampionsSent(Player* player);

    // The multiplier on XP for killing `victim`: a follower's share, a champion's, else 1.
    float PackXpMod(Unit const* victim);

    // `victim`'s corpse loot was just made: a follower's is thinned, a champion's added to.
    void ThinPackLoot(Creature* victim);

    // `member` entered combat with `enemy`: the rest of its pack joins in.
    void OnPackAggro(Creature* member, Unit* enemy);

    // `attacker` dealt `damage` to `victim` (the fight log).
    void OnPackDamage(Unit* attacker, Unit* victim, uint32 damage);

    // `victim` died: the fight log, once the whole pack is down.
    void OnPackMemberDied(Unit* victim);

    // 0 an ordinary mob, 1 a champion, 2 a rare (pack members and dungeon champions alike).
    uint8 ChampionTier(Unit const* unit);

    // A rare's own name ("" for a champion, an ordinary mob, or a rare of the creature's name).
    std::string ChampionName(Unit const* unit);

    // A champion's or a rare's health multiplier, else 1 (for group scaling's re-scale).
    float ChampionHealthMod(Unit const* unit);

    // Make `creature` (a dungeon mob, a Warden) a champion (`tier` 1) or a rare (2), with the
    // named affixes and `randomAffixes` more. A rare with no `name` gets a random one.
    void MakeChampion(Creature* creature, uint8 tier, uint8 randomAffixes, std::string const& name = "",
                      std::vector<std::string> const& affixNames = {});

    // How many random affixes a champion or a rare of `level` has.
    uint8 ChampionAffixCount(uint32 level, bool rare);

    // `creature` is leaving the world: forget its pack and champion records.
    void OnCreatureRemoved(Creature* creature);

    // Dev tools: the nearest eligible mob forms a pack of `size` (0: by level) and `tier`.
    void DevFormPack(Player* player, uint8 size, uint8 tier);
}

#endif
