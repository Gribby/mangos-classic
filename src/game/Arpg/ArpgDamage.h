/*
 * ARPG damage: the size of the numbers, critical strikes, added damage and ailments, for the
 * benilla ARPG client (the client's docs/ARPG-CHARACTER.md, "Damage"). Everything here is for an
 * ARPG player (Arpg::Active) and its pets, totems and guardians only.
 *
 * A hit's damage, from the bottom up:
 *   base       vanilla's: the weapon's swing or the spell's points, with attack power or spell
 *              damage by vanilla's coefficients
 *   x scale    the power scale: ARPG-sized numbers. 1 + Arpg.Power.PerLevel per level past the
 *              first (6.9 at 60 by default), and every creature's health grows to match, with
 *              Arpg.Power.HealthGrowth on top for the build a character grows into. Only against
 *              creatures: a duel stays vanilla's.
 *   x increased  one sum of every "increased" bonus that fits the hit's tags: the web's small
 *              nodes, item affixes, Strength (Melee and Physical) and Intellect (Spell)
 *   x more     each "more" bonus multiplies on its own: notables, keystones, skill tree nodes
 *   x crit     vanilla's multiplier (2 for attacks, 1.5 for spells) plus Agility's and the
 *              affixes' critical strike damage
 * On top of the hit:
 *   - Added damage: "Adds 10 to 24 Fire damage to attacks". Each damage type the gear adds lands
 *     as its own hit beside the main one, in its own school, with its own type's bonuses, critical
 *     when the main hit was, and able to inflict its own ailment.
 *   - Ailments: each damage type has one. A critical hit always inflicts its type's; affixes and
 *     nodes add a chance on any hit. They grow with "increased damage with ailments", damage over
 *     time, and Spirit.
 *       Physical  Bleed     60% of the hit over 5 sec, doubled while the target moves; the
 *                           strongest one counts
 *       Fire      Ignite    80% of the hit over 4 sec; the strongest one counts
 *       Nature    Poison    30% of the hit over 3 sec; stacks without limit (20 at once)
 *       Frost     Chill     slowed; a critical hit on a chilled target freezes it
 *       Shadow    Wither    the target takes 4% more damage from you per stack, up to 10
 *       Arcane    Overload  half the hit is stored, and bursts on the target and those near it
 *                           3 sec after the first
 *       Holy      Judged    a quarter of the hit is stored per stack; at 5 stacks a Holy blast
 *                           strikes the target and those near it for everything stored
 */

#ifndef MANGOS_ARPG_DAMAGE_H
#define MANGOS_ARPG_DAMAGE_H

#include "Common.h"

#include <string>
#include <vector>

class Player;
class Unit;
struct SpellEntry;

namespace Arpg
{
    // The attributes' damage, in points per 1%: Strength's increased Melee and Physical damage,
    // Intellect's increased Spell damage, Agility's critical strike multiplier, Spirit's increased
    // damage over time and with ailments.
    constexpr int32 STR_PER_DAMAGE = 5, INT_PER_DAMAGE = 5, AGI_PER_CRIT = 5, SPI_PER_DOT = 5;

    // --- The power scale ---

    // The damage scale at character level `level` (1 with Arpg.Enable off).
    float PowerScaleAt(uint32 level);

    // `unit`'s damage scale: an ARPG player's, or its owner's for a pet, totem or guardian; else 1.
    float PowerScale(Unit const* unit);

    // The scale on damage `attacker` deals to `victim`: its power scale against a creature, 1
    // against a player or anything a player controls (nullptr: a creature, for estimates).
    float DamageScale(Unit const* attacker, Unit const* victim);

    // A creature's maximum health multiplier at `level` (1 with Arpg.Enable off).
    float CreatureHealthScale(uint32 level);

    // A flat amount (thorns, a heal turned into a bolt, a share of attack power) as damage
    // `attacker` deals to `victim`.
    uint32 ToDamage(Unit const* attacker, Unit const* victim, uint32 amount);

    // Damage `attacker` dealt to `victim` brought back to vanilla's size, for a heal or a mana
    // gain taken from it (leech, Rune of Leech).
    uint32 FromDamage(Unit const* attacker, Unit const* victim, uint32 amount);

    // The level gap on a creature's hit on an ARPG player (and on its damage cap): 10% less per
    // level the player is above it, to 40%; 5% more per level below, to +25%. 1 for anything else.
    float LevelGapMod(Unit const* attacker, Unit const* victim);

    // --- Critical strikes ---

    // What `attacker` adds to its critical strike multiplier with `spell` (nullptr: a swing).
    float CritBonus(Unit const* attacker, SpellEntry const* spell);

    // The full critical strike multiplier `attacker` has for a hit with `spell` (nullptr: a swing):
    // attacks 2, spells (by their damage class, as the hit's) 1.5, plus CritBonus. For its added
    // hits and the sheet.
    float CritMultiplier(Unit const* attacker, SpellEntry const* spell);

    // --- Hits, added damage and ailments ---

    // `attacker`'s main hit (a swing: `spellInfo` nullptr, or a skill's direct hit) of `tags` landed
    // on `victim` for `dealt`, critical or not: its added damage and its ailments. Never for an
    // added hit.
    void OnHitLanded(Unit* attacker, Unit* victim, SpellEntry const* spellInfo, uint32 tags, uint32 dealt, bool crit);

    // Inflict damage tag `tag`'s ailment on `victim` as a hit of `dealt` would (a unique power's
    // burst of ailments; Frost chills, or freezes when `crit` and it is chilled already).
    void InflictAilment(Player* player, Unit* victim, uint8 tag, uint32 dealt, bool crit);

    // A damage tag's school mask and the stock spell whose impact stands for it (Rend, Smite,
    // Fireball, Frostbolt, Arcane Explosion, Lightning Bolt, Shadow Bolt).
    uint32 SchoolOfTag(uint8 tag);
    uint32 LookOfTag(uint8 tag);

    // The "more damage taken" `victim` has from `attacker`'s Wither.
    float AilmentTakenMod(Unit const* attacker, Unit const* victim);

    // Every player update: the ailments `player` inflicted tick, burst and wear off.
    void UpdateAilments(Player* player);

    // Thermal Shock: a mage's fire deals this much more to a chilled enemy.
    constexpr int32 THERMAL_SHOCK_PCT = 30;

    // Whether `unit` is frozen (a freeze, or any root, as Frost Nova's) or chilled.
    bool Frozen(Unit const* unit);
    bool Chilled(Unit const* unit);

    // Chill `victim` as `player`'s, or freeze it.
    void Chill(Player* player, Unit* victim, bool freeze);

    // `player`'s Ignite on `victim`: ended, its burn left returned (0: none).
    uint32 TakeIgnite(Player* player, Unit* victim);

    // Copy `player`'s Ignite on `from` (a dying enemy) onto the `count` enemies nearest it within
    // `range`, fresh; returns how many.
    size_t SpreadIgnite(Player* player, Unit* from, float range, size_t count);

    // Forget a logged-out player's ailments.
    void ForgetAilments(Player* player);

    // The sheet's lines on critical strikes, added damage and ailments for a skill of `tags`.
    void HitExtraLines(Player const* player, SpellEntry const* spell, uint32 tags, std::vector<std::string>& out);
}

#endif
