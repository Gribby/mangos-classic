/*
 * ARPG resources: mana that matters without ever leaving the player stuck (the client's
 * docs/ARPG-ROADMAP-RESOURCES.md, phase 1).
 *
 *   - Overdraw: an ARPG player may cast a mana or rage spell with at least half its cost left; the
 *     cast empties what is left (a miss's refund is of what it took). The free filler and the
 *     regeneration keep a player from ever being stuck.
 *   - Costs as a share of the pool: a class's main spells cost a set share of maximum mana
 *     (Fireball 9%, Arcane Missiles 18%, Consecration 22%...); any other mana spell its vanilla
 *     share of base mana, halved, of maximum mana. Talents and gear's cost cuts still apply.
 *   - A free filler per class that gives mana back on a hit: the mage's Frostbolt and the druid's
 *     Wrath (3% each), and a paladin's melee swing while a Seal is on (2%). Shapeshifting is free.
 *   - Regeneration: 1.5% of maximum mana a second, and the gear's mana per 5 seconds, whether or
 *     not the player cast lately (no five-second rule); out of combat the rest refills it
 *     (Arpg/ArpgCharacter.h). A health globe also gives back GLOBE_MANA_PCT of maximum mana.
 *
 * The client is told every known mana spell's cost (SMSG_ARPG_COSTS, 0x350): uint16 count, then
 * per spell uint32 spell id and uint32 cost; sent with the hello and again whenever maximum mana,
 * the spells known or the cost fields change. It shows those costs and allows the same overdraw.
 */

#ifndef MANGOS_ARPG_RESOURCES_H
#define MANGOS_ARPG_RESOURCES_H

#include "Common.h"

class Player;
class Unit;
struct SpellEntry;

namespace Arpg
{
    constexpr uint32 GLOBE_MANA_PCT = 8;

    // The mana cost of `spellInfo` for `caster`, from its vanilla base cost `vanilla` (before any
    // cost modifier); `vanilla` itself for a non-ARPG caster or a spell not costing mana.
    int32 ManaCost(Unit const* caster, SpellEntry const* spellInfo, int32 vanilla);

    // Whether `caster` may cast a spell of `powerType` costing `cost` with what it has now, short
    // of the cost (overdraw: at least half of it).
    bool Overdraws(Unit const* caster, uint32 powerType, uint32 cost);

    // The mana an ARPG player regains over `diffMs` milliseconds.
    float ManaRegen(Player const* player, uint32 diffMs);

    // `dealer` hit `victim` directly for `damage` with `spellInfo` (nullptr: a melee swing): the
    // filler's mana back.
    void OnResourceHit(Unit* dealer, Unit* victim, uint32 damage, SpellEntry const* spellInfo);

    // Send `player` its spells' costs (SMSG_ARPG_COSTS); with `ifChanged`, only when maximum mana
    // or the spells known changed since the last.
    void SendCosts(Player* player, bool ifChanged = false);
    void ForgetCosts(Player* player);
}

#endif
