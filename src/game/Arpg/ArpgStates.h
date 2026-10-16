/*
 * ARPG enemy states: the shared language of synergy (the client's docs/ARPG-SYNERGY.md).
 *
 * A state is something an enemy is in, which any skill, passive, item or trigger can set up, read
 * or consume, without naming the skill that put it there: one skill sets up, another pays off, and
 * any pair that shares a state works together. The states are the ailments (Arpg/ArpgDamage.h:
 * Chilled and Frozen, Ignited, Bleeding, Poisoned, Withered, Overloaded, Judged), and:
 *
 *   Dazed        built up by physical and holy hits (a share of the enemy's health per hit, less
 *                on champions, rares and bosses; it ebbs when the hits stop); at 100% the enemy is
 *                Dazed for a few seconds: slowed, its cast broken, and taking more damage.
 *   Stunned      any stun.
 *   Consecrated  standing in a Consecration.
 *
 * States are kept per enemy, whoever put them there. Consuming a state ends it and gives back what
 * it held (a damage over time's damage left, Judged's stored damage), for the consumer to pay out.
 * The client is told each nearby enemy's states (SMSG_ARPG_STATES) for its hover bar.
 */

#ifndef MANGOS_ARPG_STATES_H
#define MANGOS_ARPG_STATES_H

#include "Common.h"

class Player;
class Unit;
class Creature;
struct SpellEntry;

namespace Arpg
{
    enum State : uint8
    {
        STATE_CHILLED,
        STATE_FROZEN,
        STATE_IGNITED,
        STATE_BLEEDING,
        STATE_POISONED,
        STATE_WITHERED,
        STATE_OVERLOADED,
        STATE_JUDGED,
        STATE_DAZED,
        STATE_STUNNED,
        STATE_CONSECRATED,
        MAX_STATE
    };

    constexpr uint32 StateBit(State s) { return 1u << s; }

    char const* StateName(uint8 state);

    // The states `victim` is in now, as StateBit()s.
    uint32 StateMask(Unit const* victim);
    bool HasState(Unit const* victim, State state);

    // A hit's Daze: `dealt` of `tags` by `by` on `victim` (physical and holy hits build it).
    void BuildDaze(Player* by, Unit* victim, uint32 dealt, uint32 tags);
    // Daze build-up by a share of the threshold (a Consecration's tick, a trigger), in percent.
    void AddDaze(Player* by, Unit* victim, float pct);
    // How far `victim`'s Daze has built, 0-100.
    uint8 DazeOf(Unit const* victim);

    // Put `victim` in `state` (Dazed, Chilled, Frozen; an ailment takes InflictAilment).
    void ApplyState(Player* by, Unit* victim, State state);

    // End `state` on `victim` for `by`'s payoff: true if it was in it; `worth` is what the state
    // held (damage left, stored damage), 0 for those that hold none.
    bool ConsumeState(Player* by, Unit* victim, State state, uint32& worth);

    // Dazed's own effect: the more damage a Dazed enemy takes, as a multiplier.
    float StateTakenMod(Unit const* victim);

    // A periodic aura's tick by `caster` on `target`: a Consecration's builds Daze.
    void OnStateTick(Unit* caster, Unit* target, SpellEntry const* spellInfo);

    // Every update of an ARPG player: nearby enemies' states to its client, when they change.
    void UpdateStates(Player* player);
    void ForgetStatesSent(Player* player);
    // A creature left its map.
    void ForgetStates(Creature* creature);
}

#endif
