/*
 * ARPG druid: the hooks of the druid's keystones, its web's and its skill trees' (the client's
 * docs/ARPG-CHARACTER.md, "The druid, in full"). Their "more" on damage and healing live with
 * the rest (Arpg/ArpgCharacter.cpp's DamagePartsFor and HealingDoneMod, Arpg/ArpgDamage.cpp's
 * ailment ticks); these are the ones set off by a hit, a tick, a heal, a blow taken, a kill or
 * the passing of time.
 *
 * Combo points, for an ARPG druid: they are the druid's, not the target's. They carry
 * from one enemy to the next and outlive the enemy they were built on, so a finisher always has
 * them whichever enemy is under the cursor (Unit::AddComboPoints, Spell::CheckCast).
 */

#ifndef MANGOS_ARPG_DRUID_H
#define MANGOS_ARPG_DRUID_H

#include "Common.h"

class Player;
class Spell;
class Unit;
struct SpellEntry;

namespace Arpg
{
    // A druid's main hit with a spell or an ability landed on `victim` for `dealt` (`victim` may
    // have died of it): Eclipse, Savage Fury, Survival Instincts.
    void OnDruidHit(Player* player, Unit* victim, SpellEntry const* spellInfo, uint32 dealt, bool crit);

    // A periodic damage tick of `spellInfo` from `caster` on `target` for `damage` (any caster):
    // Hive Mind, Lunar Tide, Tempest, Biting Hail, Eye of the Wild.
    void OnDruidTick(Unit* caster, Unit* target, SpellEntry const* spellInfo, uint32 damage);

    // A druid's heal with a Rejuvenation skill spell landed on `target` for `amount`: Bloom,
    // Nature's Wrath, Verdant Fury, Wrath of the Wild.
    void OnDruidHeal(Spell* spell, Player* player, Unit* target, uint32 amount);

    // A heal-over-time tick of `spellInfo` from `caster` on `target` for `amount` (any caster): a
    // druid's Rejuvenation skill heals set off OnDruidHeal, once a second at most.
    void OnDruidHotTick(Unit* caster, Unit* target, SpellEntry const* spellInfo, uint32 amount);

    // `victim` took a blow from `attacker`: Bristling Fur, Thorned Hide.
    void OnDruidStruck(Unit* victim, Unit* attacker);

    // `killer` killed something: King of the Jungle.
    void OnDruidKill(Player* killer);

    // Every player update: Living Thorns.
    void UpdateDruid(Player* player);

    // King of the Jungle's kills in its window; Verdant Fury's more damage now, in percent.
    uint32 KingStacks(Player const* player);
    int32 VerdantFuryPct(Player const* player);

    // Whether `unit`'s combo points carry from enemy to enemy: an ARPG druid's do.
    bool CarriesComboPoints(Unit const* unit);

    // Forget a logged-out player's timers.
    void ForgetDruid(Player* player);
}

#endif
