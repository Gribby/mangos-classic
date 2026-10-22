/*
 * ARPG shaman: the hooks of the shaman's keystones, its web's and its skill trees' (the client's
 * docs/ARPG-CHARACTER.md, "The shaman, in full"). Shocked is a state (Arpg/ArpgStates.h); the
 * "more" on damage lives with the rest (Arpg/ArpgCharacter.cpp's DamagePartsFor).
 *
 * The class's own rules:
 *   - Shock: Lightning Bolt, Chain Lightning, Lightning Shield's discharge and Stormstrike hits
 *     Shock their target, a stack each (Static, Storm Totem and Stormstrike's node for more).
 *   - Earth Shock consumes Shocked for an extra hit of 25% of its own per stack (Earthquake: on
 *     every enemy within 6 yards too).
 *   - Static Discharge: a Shocked enemy's death Shocks the 2 nearest enemies.
 *   - Elemental Crossing: Frost Shock on an Ignited enemy, or Flame Shock on a Chilled one,
 *     strikes again for 50% (the mage's Thermal Shock, the shaman's way).
 *   - Totems are minions (Arpg/ArpgMinions.h): their hits carry the Minion and Totem tags.
 *
 * What an ARPG shaman gets beyond the stock class: no totem items (Spell::CheckItems), Fire Nova
 * Totem at 10, Chain Lightning and Magma Totem at 20 (Arpg/ArpgTree.cpp's early spells).
 */

#ifndef MANGOS_ARPG_SHAMAN_H
#define MANGOS_ARPG_SHAMAN_H

#include "Common.h"

class Creature;
class Player;
class Unit;
struct SpellEntry;

namespace Arpg
{
    // A shaman's own spell hit landed: the Shock, its payoffs, Overload, Thunderlord.
    void OnShamanHit(Player* player, Unit* victim, SpellEntry const* spellInfo, uint32 dealt, bool crit);
    // A shaman's swing landed: Windfury and its nodes, Static Shock, Stormbringer, Elemental Blades.
    void OnShamanSwing(Player* player, Unit* victim, uint32 dealt, bool crit);
    // A totem's (a minion's) hit landed: Searing Bolts, Twin Totems, Molten Core, the Novas.
    void OnShamanMinionHit(Unit* minion, Player* owner, Unit* victim, SpellEntry const* spellInfo, uint32 dealt);
    // `killer` killed `victim`: Static Discharge, Ball Lightning.
    void OnShamanKill(Player* killer, Unit* victim);
    // A shaman's heal landed on `target` for `amount`: Earthliving.
    void OnShamanHeal(Player* player, Unit* target, uint32 amount);
    // A totem of an ARPG shaman's went up: Stoneclaw's life and taunt.
    void OnShamanTotem(Unit* owner, Creature* totem);

    // Whether a totem of `player`'s stands (an air one: Windfury or Grace of Air's slot).
    bool HasTotemUp(Player const* player);
    bool HasAirTotemUp(Player const* player);
    // Unleashed Rage's stacks now (5% more Melee damage each).
    uint8 UnleashedRageStacks(Player const* player);
    // Wind's Edge's attack speed, in percent.
    float ShamanHastePct(Player const* player);
    // The share of damage an ARPG shaman takes after Totemic Ward, as a multiplier.
    float ShamanTakenMod(Unit const* victim);

    // Every player update: Walking Totems, Earthen Vigor, Earth's Grasp.
    void UpdateShaman(Player* player);
    void ForgetShaman(Player* player);
}

#endif
