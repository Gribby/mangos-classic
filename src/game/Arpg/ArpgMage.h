/*
 * ARPG mage: the hooks of the mage's keystones, its web's and its skill trees' (the client's
 * docs/ARPG-CHARACTER.md, "The mage"). Their numbers and their "more" on damage live with the
 * rest (Arpg/ArpgCharacter.cpp's DamagePartsFor, Arpg/ArpgDamage.cpp's ailments); these are the
 * ones set off by a hit, a tick, a kill or a blow taken.
 */

#ifndef MANGOS_ARPG_MAGE_H
#define MANGOS_ARPG_MAGE_H

#include "Common.h"

class Player;
class Unit;
struct SpellEntry;

namespace Arpg
{
    // A mage's main hit with a spell landed on `victim` for `dealt` (after its ailments; `victim`
    // may have died of it): Pyroclasm, Shatterstorm, Deep Freeze, Arcane Siphon, Mana Burst,
    // Chain Blast, Searing Brand, Detonate.
    void OnMageHit(Player* player, Unit* victim, SpellEntry const* spellInfo, uint32 dealt, bool crit);

    // A periodic damage tick of `spellInfo` from `caster` on `target` (any caster): Glacial Storm.
    void OnPeriodicHit(Unit* caster, Unit* target, SpellEntry const* spellInfo);

    // An ARPG player's kill: Wildfire.
    void OnMageKill(Player* killer, Unit* victim);

    // `victim` took a blow from `attacker`: Permafrost.
    void OnMageStruck(Unit* victim, Unit* attacker);

    // Forget a logged-out player's timers.
    void ForgetMage(Player* player);
}

#endif
