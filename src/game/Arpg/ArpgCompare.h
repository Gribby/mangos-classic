/*
 * ARPG item comparison, for the benilla ARPG client (the client's docs/ARPG-CHARACTER.md,
 * "Damage", "Showing it"): what a carried item would do to the damage of each skill in the
 * player's slots if it were worn in place of what is, so an upgrade is a number, not a guess.
 *
 * Each slotted skill gets a score: its expected damage a use (Strike: a second), from the same
 * pieces a hit is built of (Arpg/ArpgDamage.h): the base (attack power and weapon, or the spell's
 * points and spell damage), the increased sum and the more product, the critical strike chance and
 * multiplier, added damage and the ailments it should inflict. The score is worked out twice, with
 * the totals and vanilla stats the character has and with the item swapped in, and the line says
 * how far it moves. An estimate: it leaves out Agility's and Intellect's vanilla critical strike
 * chance, set bonuses and the target's armour and resistances.
 *
 *   SMSG_ARPG_COMPARE (0x34C): uint16 count, per item: uint64 item guid, cstring lines ("\n"
 *     between). The whole set every time: items not in it have no lines. Sent from the
 *     once-a-second refresh when the gear, the bags or the character's numbers changed.
 */

#ifndef MANGOS_ARPG_COMPARE_H
#define MANGOS_ARPG_COMPARE_H

#include "Common.h"

class Player;

namespace Arpg
{
    // Send `player` its items' comparison lines if what they depend on changed.
    void UpdateCompare(Player* player);

    // Forget a logged-out player's comparisons.
    void ForgetCompare(Player* player);
}

#endif
