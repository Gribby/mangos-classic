/*
 * ARPG acts: the story's road through vanilla's zones (the client's docs/ARPG-ACT-1.md).
 *
 * An act is a chain of story quests the server writes into the world database at start
 * (quest_template, creature_questrelation, creature_involvedrelation; entries ACT_QUEST_FIRST and
 * up), given and taken by vanilla's own NPCs. Each quest sends the character to a place and asks it
 * to break something there: kill counts of the creatures that live there and a named leader. The
 * quest log is the journal; vanilla's own quests stay as side quests, but those an act quest tells
 * again are taken off their givers.
 *
 * Act I, chapter 1: Elwynn Forest for the Alliance (Marshal McBride and Marshal Dughan, both in
 * Goldshire, Act I's hub; its areas end with Hogger), Durotar for the Horde (Gornek, then Gar'Thok
 * in Razor Hill, ending with Zalazane). Every Alliance character starts in Goldshire before Dughan
 * and every Horde one in the Valley of Trials, whatever its race
 * (ObjectMgr::ArpgOneStartPerFaction), so the act has one road.
 *
 * Names, not numbers: the NPCs, creatures and vanilla quests are found by name in the world
 * database, so a name it lacks drops that objective (logged) and the act still loads.
 *
 * Packs ahead: out in the open world, as an ARPG character travels, packs of the creatures that
 * live where it is heading appear ahead of it, just out of sight, never in or near a town; they
 * leave when it is far past them. (It replaced a per-quest road, which did not read as one.)
 *
 * The chapter's named leaders are champions, its last one (Hogger, Zalazane) a rare with a
 * guaranteed blue: on the existing champion, telegraph and loot code.
 *
 * Dev notes: with Arpg.DevTools on, a chat line starting "#note " is not said; it is written to
 * the play statistics with where the character stands (Arpg/ArpgStats.h), for tuning the road.
 */

#ifndef MANGOS_ARPG_ACTS_H
#define MANGOS_ARPG_ACTS_H

#include "Common.h"

#include <string>
#include <vector>

class Creature;
class Player;

namespace Arpg
{
    // The act quests' entries start here.
    constexpr uint32 ACT_QUEST_FIRST = 59000;

    // Before creatures load: Marshal McBride stands in Goldshire beside Dughan (Northshire has
    // fallen; Goldshire is Act I's hub, Arpg/ArpgRift.h). His world database spawn is moved.
    void PlaceActNpcs();
    // A named NPC's spawn on map 0 (the first), from the world database: false if none.
    bool ActNpcSpot(char const* name, float& x, float& y, float& z, float& o);

    // Before quests load: write the act quests and their givers and takers into the database.
    void EnsureActQuests();

    // After the quest relations load: take the vanilla quests an act quest retells off their givers.
    void AfterActQuestsLoaded();

    // Whether `quest` is an act quest.
    bool IsActQuest(uint32 quest);

    // Whether creatures of `entry` are an act's named leader (never copied into packs).
    bool IsActLeader(uint32 entry);

    // A creature joined its map: an act's leader is crowned.
    void OnActCreatureAdded(Creature* creature);

    // An act's last leader fell: its guaranteed drop. True when `victim` was one.
    bool OnActBossLoot(Creature* victim);

    // Every update of an ARPG character: packs are placed ahead of it in the open world.
    void UpdateActs(Player* player);

    // A logged-out character's packs ahead go.
    void ForgetActs(Player* player);

    // The creature named `name` on map 0 (the one with the most spawns there), or 0: an area's
    // creatures (Arpg/ArpgRift.h).
    uint32 ActEntryNamed(char const* name);
    // A summoned act leader (a Goldshire area's): crowned as the world's own spawn is.
    void CrownActLeader(Creature* creature);

    // The hostile creatures that live on `map` (0 or 1) in the box, each with at least
    // `minSpawns` spawns there: a rift's natives (Arpg/ArpgRift.h).
    std::vector<uint32> NativesWithin(uint32 map, float minX, float maxX, float minY, float maxY, uint32 minSpawns);

    // A chat line: true when it was a dev note (written to the statistics, not said).
    bool OnActChat(Player* player, std::string const& text);
}

#endif
