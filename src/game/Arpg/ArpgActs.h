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
 * Act I, chapter 1: Elwynn Forest for the Alliance (Marshal McBride, then Marshal Dughan in
 * Goldshire, ending with Hogger), Durotar for the Horde (Gornek, then Gar'Thok in Razor Hill,
 * ending with Zalazane). Every Alliance character starts in Northshire and every Horde one in the
 * Valley of Trials, whatever its race (ObjectMgr::ArpgOneStartPerFaction), so the act has one road.
 *
 * Names, not numbers: the NPCs, creatures and vanilla quests are found by name in the world
 * database, so a name it lacks drops that objective (logged) and the act still loads.
 *
 * The road: each act quest runs from its giver to where it sends the character (its leader's
 * spawn, or the middle of its first creature's spawns nearest the giver, or the next giver). The
 * map's own pathfinding draws the walkable way between them, and while the quest is under way the
 * corridor fills ahead of the character, just out of sight, with packs of the creatures that live
 * along it: the way forward is always a fight. They leave when the character is far past them.
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

class Creature;
class Player;

namespace Arpg
{
    // The act quests' entries start here.
    constexpr uint32 ACT_QUEST_FIRST = 59000;

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

    // Every update of an ARPG character: its act quest's corridor fills ahead of it.
    void UpdateActs(Player* player);

    // A logged-out character's corridor packs go.
    void ForgetActs(Player* player);

    // A chat line: true when it was a dev note (written to the statistics, not said).
    bool OnActChat(Player* player, std::string const& text);
}

#endif
