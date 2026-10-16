/*
 * ARPG synergy triggers: "when X happens, do Y" over the enemy states (Arpg/ArpgStates.h; the
 * client's docs/ARPG-SYNERGY.md).
 *
 * Events: a hit, a critical hit, a kill, a state applied, a state consumed, a block, a dodge (the
 * enemy is the one whose blow it was). A trigger row names an
 * event and its conditions (a spell by name, a skill, the hit's tags, a state the enemy must be in
 * or the state the event is about) and an effect:
 *
 *   apply    put the enemy in a state (Dazed, Chilled, Frozen) or inflict an ailment
 *   strike   consume a state for an extra hit on the enemy: a share of the hit, and what the
 *            state held (a damage over time's damage left, Judged's stored damage)
 *   burst    consume a state for a hit on every enemy around it
 *   spread   copy a state from the enemy to those around it
 *   daze     build Daze
 *
 * A row belongs to a class (every character of it: the class's own rules), a skill-tree node (its
 * rank scales the value), or a worn unique (by name). Each has its own cooldown. Effects deal their
 * damage through StrikeFoe, which no hit hook sees, so a trigger never sets off another hit's
 * triggers; state events nest at most a few deep.
 */

#ifndef MANGOS_ARPG_SYNERGY_H
#define MANGOS_ARPG_SYNERGY_H

#include "Arpg/ArpgStates.h"
#include "Common.h"

#include <string>
#include <utility>
#include <vector>

class Player;
class Unit;
struct SpellEntry;

namespace Arpg
{
    // A hit landed (the main hit of a swing or a spell, not an added, ailment or trigger hit).
    void OnSynergyHit(Player* player, Unit* victim, SpellEntry const* spellInfo, uint32 tags, uint32 dealt, bool crit);
    // `player` killed `victim`.
    void OnSynergyKill(Player* player, Unit* victim);
    // `defender` blocked (or dodged) a blow from `attacker`.
    void OnSynergyDefend(Unit* defender, Unit* attacker, bool block);
    // A state was put on, or consumed from, `victim` by `player`.
    void OnStateApplied(Player* player, Unit* victim, State state);
    void OnStateConsumed(Player* player, Unit* victim, State state, uint32 worth);

    // The worn uniques' synergy lines, by item entry, for their tooltips.
    std::vector<std::pair<uint32, std::string>> SynergyItemLines();

    // A logged-out character's cooldowns go.
    void ForgetSynergy(Player* player);

    // The trigger rows `player` has, as lines for the character sheet.
    std::vector<std::string> SynergyLines(Player const* player);
}

#endif
