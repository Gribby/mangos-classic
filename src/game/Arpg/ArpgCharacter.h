/*
 * ARPG character: the attributes' ARPG effects, the passive web's totals and its keystones, for
 * the benilla ARPG client (the client's docs/ARPG-CHARACTER.md, "Attributes" and "The passive
 * web"). Everything here is for an ARPG player (Arpg::Active) only.
 *
 * Each vanilla attribute keeps its vanilla effects and gains one ARPG effect:
 *   Strength   melee area      +1% per 10, to 40%
 *   Agility    movement speed  +1% per 20, to 20%
 *   Stamina    life on kill    a quarter of it per kill
 *   Intellect  spell area      +1% per 10, to 40%
 *   Spirit     cooldown        +1% recovery per 10, to 30%
 * The web's own ARPG stats (ArpgTree.h, WebTotals) add to these, under the same caps.
 *
 * Where they act:
 *   - stats and armour from the web: unit stat modifiers, applied and taken off with the web
 *   - tag damage and healing: one multiplier in the spell and melee damage and healing bonus paths
 *   - area: the spell's radius (Spell target fill, persistent area auras), for spells tagged Area;
 *     a melee one scales with Strength, any other with Intellect
 *   - movement speed: Unit::UpdateSpeed's run speed
 *   - cooldown recovery: a percent cooldown spell modifier over the class's spells, which the
 *     client is sent like any talent's, so its cooldowns agree
 *   - life on kill: a heal when the player kills a creature
 *   - the skill trees' nodes: spell modifiers sent to the client like a talent's, and the hooks
 *     below, each reading its Keystone's rank
 */

#ifndef MANGOS_ARPG_CHARACTER_H
#define MANGOS_ARPG_CHARACTER_H

#include "Common.h"

#include <string>
#include <vector>

class Player;
class Spell;
class Unit;
struct SpellEntry;

namespace Arpg
{
    // Apply the player's web totals: stats, armour, and the keystones that hold a state.
    void ApplyCharacter(Player* player);

    // Forget a logged-out player's applied state.
    void ForgetCharacter(Player* player);

    // Every player update: the keystones' pulses and the once-a-second refresh.
    void UpdateCharacter(Player* player);

    // The multiplier on damage `attacker` deals with `spellInfo` (nullptr: a weapon swing).
    float DamageDoneMod(Unit const* attacker, Unit const* victim, SpellEntry const* spellInfo);

    // The item affixes' experience bonus for `gainer`, a multiplier.
    float XpGainMod(Unit const* gainer);

    // The item affixes' spell damage (every school) for `caster`.
    int32 SpellPowerBonus(Unit const* caster);

    // The multiplier on a globe's or the flask's healing for `unit`.
    float GlobeHealMod(Unit const* unit);

    // The multiplier on healing `healer` does with `spellInfo`.
    float HealingDoneMod(Unit const* healer, SpellEntry const* spellInfo);

    // The multiplier on healing `target` takes.
    float HealingTakenMod(Unit const* target);

    // The multiplier on `spellInfo`'s radius cast by `caster`.
    float AreaScale(Unit const* caster, SpellEntry const* spellInfo);

    // The multiplier on `unit`'s run speed.
    float MoveSpeedMod(Unit const* unit);

    // Extra block chance, in percent.
    float BlockChanceBonus(Unit const* unit);

    // Whether `caster`'s `spellInfo` crits for sure (Divine Favour); spends the favour.
    bool ForcesCrit(Unit* caster, SpellEntry const* spellInfo);

    // Whether `caster`'s heal on `target` heals nothing (Lightforged).
    bool BlocksHeal(Unit const* caster, Unit const* target);

    // `caster` healed `target` for `amount` with `spell` (or would have, Lightforged).
    void OnCharacterHeal(Spell* spell, Unit* caster, Unit* target, uint32 amount);

    // `victim` lost `damage` health to `attacker` (Martyr).
    void OnDamageTaken(Unit* victim, Unit* attacker, uint32 damage);

    // `victim` blocked a melee hit (Shield Wall).
    void OnBlocked(Unit* victim);

    // `killer` is credited with `victim`'s death: life on kill, Crusade, a final boss's point.
    void OnCharacterKill(Player* killer, Unit* victim);

    // --- The skill trees' hooks (ArpgSkills.h) ---

    // Extra melee reach, in yards (Long Arm).
    float ExtraMeleeReach(Unit const* attacker);

    // `attacker`'s weapon swing hit `victim` for `damage` (Wide Swing, Whirling Strikes, Momentum,
    // Stagger, Shockwave).
    void OnSwingHit(Unit* attacker, Unit* victim, uint32 damage, bool crit);

    // `spell` of `caster` hurt `victim` for `dealt` (Mana Strike, Light of the Crusader, Sacred
    // Seal, Final Verdict, Sanctified). Added hits never come here.
    void OnSkillSpellDamage(Spell* spell, Unit* caster, Unit* victim, uint32 dealt);

    // `spell` of `caster` landed on `target` (Hammer of Justice's Holy Hammer, Ricochet, Blessed Hammer).
    void OnSkillSpellLanded(Spell* spell, Unit* caster, Unit* target);

    // `caster` judged (Righteous Mind).
    void OnJudgement(Unit* caster);

    // Extra procs a minute for `caster`'s proc aura `spellInfo` (Relentless).
    float ExtraPpm(Unit const* caster, SpellEntry const* spellInfo);

    // Whether `unit` keeps its Seal `existing` while gaining the Seal `adding` (Twin Seals).
    bool KeepsSecondSeal(Unit const* unit, SpellEntry const* adding, SpellEntry const* existing);

    // --- The character sheet, for the benilla ARPG client ---
    //
    // What the attributes do and what adds to each skill, with the player's own numbers, so
    // nothing in the build is a guess:
    //   SMSG_ARPG_SHEET (0x347): uint8 version (1); uint8 attribute count (5, Strength to Spirit),
    //     a cstring each; uint8 skill count, per skill: uint8 skill id, cstring; cstring the
    //     summary (movement, cooldowns, areas, damage by tag, worn uniques). Each cstring is lines
    //     with "\n" between, colour escapes included.
    // Built at most every two seconds from the once-a-second refresh, sent when it changed.

    // The sheet's lines for the conditional damage bonuses DamageDoneMod adds to a spell of
    // `tags` in `skill` (keystones that depend on the weapon, the enemies around, a kill streak).
    void ConditionalDamageLines(Player const* player, uint32 tags, uint8 skill, std::vector<std::string>& out);

    // The sheet's lines for `skill` (of `tags`) beyond the tags: the gear and buff auras that add
    // to it, the character's attack power or spell damage, an estimate of its damage, and its
    // hits of the last minute.
    void SkillDamageLines(Player const* player, uint8 skill, uint32 tags, std::vector<std::string>& out);

    // Send `player` its sheet if it changed (`force`: now, whatever it last sent).
    void UpdateSheet(Player* player, bool force);
}

#endif
