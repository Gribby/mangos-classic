/*
 * ARPG uniques, for the benilla ARPG client (see ArpgCombat.h and the client's
 * docs/ARPG-UNIQUES.md).
 *
 * Named vanilla items keep their stats and gain one interaction with a specific spell, for an
 * ARPG player wearing them. The table lives here (Uniques()): one row per item, naming its kit
 * (the engine piece), the spell it changes (rank 1; any rank matches), its numbers and the line
 * its tooltip shows. Mechanics read what the player wears when they fire, so equipping or
 * removing an item needs no bookkeeping.
 *
 * Built so far:
 *   KIT_EXTRA_PROJECTILES (A)  A line skillshot fires `n` more lines, fanned `value` degrees
 *                              apart around the aim, each a free cast of the same spell dealing
 *                              `pct` percent (Spell::SetArpgSecondary).
 *   KIT_PIERCE (B)             A hit flies on along its line through `n` more enemies (0: all of
 *                              them, up to 10) out to the spell's range, each taking `pct` percent.
 *   KIT_CHAIN (C)              On hit, the hit leaps to the nearest enemy not yet hit within
 *                              `value` yards, `n` times, each for `pct` percent of the first hit.
 *   KIT_FRAGMENTS (D)          On hit, `n` enemies within `value` yards behind the target, in a
 *                              90 degree cone away from the caster, take `pct` percent of the hit.
 *   KIT_BURST (E)              On hit, every enemy within `value` yards of the target (up to 10)
 *                              takes `pct` percent of the hit at once.
 *   KIT_SPREAD (F)             When the spell lands, a free copy of it lands on the `n` enemies
 *                              nearest its target within `value` yards; with `n` 0, when its
 *                              target dies with the caster's aura of it on, on every enemy within
 *                              `value` yards (up to 10) instead.
 *   KIT_ARC (G)                A melee ability's hit also strikes every other enemy in the
 *                              swing's 120 degree arc and reach, for `pct` percent.
 *   KIT_SHOCKWAVE (H)          A melee ability's hit also strikes every enemy along the caster's
 *                              facing out to `value` yards, for `pct` percent.
 *   KIT_ECHO (I)               Every `n`th cast of the skillshot repeats for free half a second
 *                              later along the same aim, at `pct` percent.
 *   KIT_RAISE (J)              A kill (any) has a `value` percent chance to raise creature
 *                              `spell` (the column holds the entry) at the corpse, fighting for
 *                              the player for `pct` seconds.
 *   KIT_STEP (K)               A kill with the spell moves the player behind the nearest enemy
 *                              within `value` yards.
 * Chain jumps and fragments fly: a cosmetic SMSG_SPELL_GO of the spell from the unit they leave
 * to the one they reach draws the missile, and the hit lands when it arrives.
 * A row's spell covers every rank of it and any spell named after it ("Judgement of
 * Righteousness" under Judgement, each missile under Arcane Missiles).
 * If two worn items change the same spell with the same kit, the one with the larger `n` applies.
 *
 *   SMSG_ARPG_ITEM_MECHANICS: uint8 count, then per row: uint32 item id, cstring tooltip line.
 *   Sent in answer to every hello, so the client can show the lines.
 */

#ifndef MANGOS_ARPG_UNIQUES_H
#define MANGOS_ARPG_UNIQUES_H

#include "Common.h"

#include <vector>

class Player;
class Spell;
class Unit;
class WorldObject;
struct SpellEntry;

namespace Arpg
{
    enum UniqueKit : uint8
    {
        KIT_EXTRA_PROJECTILES = 'A',
        KIT_PIERCE            = 'B',
        KIT_CHAIN             = 'C',
        KIT_FRAGMENTS         = 'D',
        KIT_BURST             = 'E',
        KIT_SPREAD            = 'F',
        KIT_ARC               = 'G',
        KIT_SHOCKWAVE         = 'H',
        KIT_ECHO              = 'I',
        KIT_RAISE             = 'J',
        KIT_STEP              = 'K',
    };

    struct UniqueMechanic
    {
        uint32 item;
        UniqueKit kit;
        uint32 spell;     // rank 1 of the spell it changes (J: the creature entry raised)
        uint8 n;          // projectiles, fragments or chain jumps (0 where unused)
        float value;      // A: degrees between lines; C: jump range; D: reach behind; E: radius
        uint32 pct;       // each added hit's share of the main hit, in percent
        char const* text; // the tooltip line
    };

    // Every unique, in table order.
    std::vector<UniqueMechanic> const& Uniques();

    // The mechanic of `kit` that `player`'s worn items give `spellInfo`, if any.
    UniqueMechanic const* WornMechanic(Player const* player, UniqueKit kit, SpellEntry const* spellInfo);

    // Send `player` the uniques' tooltip lines (SMSG_ARPG_ITEM_MECHANICS).
    void SendUniques(Player* player);

    // A skillshot just launched along its line, at `struck` (nullptr: into the empty air): launch
    // the extra projectiles its caster's uniques add. Added projectiles add none.
    void OnLineLaunch(Spell* spell, Unit* struck);

    // `spell` hit `victim` for `dealt` (after mitigation, absorbs included): apply the caster's
    // on-hit uniques (fragments). Added hits set off none.
    void OnSpellDamage(Spell* spell, Unit* caster, Unit* victim, uint32 dealt);

    // `spell` landed on `target` (hit, not missed or resisted): the caster's on-land uniques
    // (spread). Added casts set off none.
    void OnSpellLanded(Spell* spell, Unit* caster, Unit* target);

    // `victim` is dying, `killer` the player credited, if any: the on-kill uniques (a DoT spreading
    // from the corpse, a fallen foe rising). Called while its auras still stand.
    void OnKill(Player* killer, Unit* victim);

    // The skill tree's combat keystones (ArpgTree.h):
    // Avenger: whether Judgement leaves `caster`'s Seal on.
    bool KeepsSealOnJudgement(Unit const* caster);
    // Purifying Light: whether `caster`'s Exorcism or Holy Wrath takes any creature type.
    bool IgnoresCreatureType(WorldObject const* caster, SpellEntry const* spellInfo);
    // Dawnbringer: `caster` healed `target` for `amount` with `spell`.
    void OnHeal(Spell* spell, Unit* caster, Unit* target, uint32 amount);
    // Martyr's Ward's pulse; called every player update.
    void UpdateKeystones(Player* player);

    // The kit's pieces, for the web's keystones (ArpgCharacter.h):
    // The `count` fair enemies nearest `center` within `radius` yards, in sight, nearest first.
    std::vector<Unit*> NearestFoes(Unit* caster, Unit* center, float radius, size_t count);
    // `amount` of `spellInfo`'s school `schoolMask` to `unit` as `caster`'s, through the spell damage path.
    void StrikeFoe(Unit* caster, Unit* unit, SpellEntry const* spellInfo, uint32 schoolMask, uint32 amount);
    // A holy bolt (Holy Shock's missile) from `player` at `to`, landing for `amount`.
    void SendHolyBolt(Player* player, Unit* to, uint32 amount);
}

#endif
