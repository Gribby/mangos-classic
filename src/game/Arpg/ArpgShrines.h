/*
 * ARPG shrines (Diablo II's and IV's): a stone that gives a short, strong boon to the first ARPG
 * player who walks onto it (the client's docs/ARPG-PROGRESSION.md, "Shrines").
 *
 * Where: beside a pack placed ahead of a character in the open world (SHRINE_AHEAD_PCT of them),
 * and in a dungeon beside its settling trash (SHRINE_DUNGEON_ODDS a creature, SHRINE_DUNGEON_MAX
 * an instance). A shrine is a generic game object written into gameobject_template at start
 * (entries SHRINE_ENTRY_FROM up, a vanilla shrine's or altar's look), so it shows its name on
 * hover and can't be clicked: stepping within SHRINE_REACH yards of it is the touch. An open-world
 * shrine left alone goes after SHRINE_IDLE_SECS.
 *
 * The kinds, each a boon (Arpg/ArpgBoons.h) on the HUD with its time:
 *   Wrath        20 sec  50% more damage
 *   Protection   10 sec  a Barrier of half your health, filled again each second
 *   Greed        30 sec  your kills' loot rolls as three tiers higher, a chance at an extra item,
 *                        three times the gold
 *   Celerity     15 sec  50% faster movement (Swiftness), 25% faster attacks
 *   Channeling   15 sec  your skills cost no mana
 *   Storms       20 sec  an arcane nova around you each second
 */

#ifndef MANGOS_ARPG_SHRINES_H
#define MANGOS_ARPG_SHRINES_H

#include "Common.h"

class Creature;
class Map;
class Player;

namespace Arpg
{
    enum ShrineKind : uint8
    {
        SHRINE_WRATH = 0,
        SHRINE_PROTECTION,
        SHRINE_GREED,
        SHRINE_CELERITY,
        SHRINE_CHANNELING,
        SHRINE_STORMS,
        MAX_SHRINE
    };

    constexpr uint32 SHRINE_ENTRY_FROM = 98000;

    // Writes the shrines' game object templates; call before the templates load.
    void EnsureShrineObjects();

    // A pack was placed ahead of a character at its leader: a shrine beside it, sometimes.
    void OnShrinePackPlaced(Creature* leader);
    // A dungeon's trash creature settled: a shrine beside it, sometimes.
    void OnShrineDungeonSettle(Creature* creature);

    // Whether `player` stands in the boon of a kind of shrine now.
    bool ShrineActive(Player const* player, ShrineKind kind);
    // The effects' hooks: Wrath's more (percent), Celerity's attack speed (percent), Greed's luck
    // (extra tiers on loot rarity), Channeling's free skills.
    int32 ShrineDamagePct(Player const* player);
    float ShrineHastePct(Player const* player);
    uint8 ShrineLootTiers(Player const* player);
    bool ShrineFreeCasting(Player const* player);

    // Every player update: a shrine stepped on, and Protection's and Storms' pulses.
    void UpdateShrines(Player* player);
    void ForgetShrines(Player* player);
    // A map is going: its shrines' records with it (its shrines go with the map).
    void ForgetShrineMap(Map const* map);
}

#endif
