/*
 * ARPG cross-skill procs, for the benilla ARPG client (the client's docs/ARPG-CHARACTER.md,
 * "Cross-skill procs").
 *
 * One skill sets another up: a swing that crits readies an instant Holy Light, a Judgement that
 * lands empowers the next Exorcism, a stun takes Judgement off its cooldown. Each proc has a
 * trigger (a swing's crit, a spell's hit, crit or landing, a heal's crit, a kill), what it does
 * (the next cast of the named spells is instant, deals more, is sure to crit or costs no mana; or
 * the named spells' cooldowns end now), an internal cooldown so it paces a rotation rather than
 * replacing one, and how long the readied cast waits.
 *
 * They come from runes (socketed in the triggering skill's tree, Arpg/ArpgSkills.h) and from
 * uniques (worn named items, their implicit line). Every named unique without an authored mechanic
 * carries a power from a pool, fixed by its entry: a cross-skill proc, a heal or mana on a kill
 * or crit, a slain foe's burst, lightning that arcs from a crit, holy fire back at a heavy blow.
 *
 * A readied cast is a spell modifier on the player, which the client hears of as stock (so its
 * cast bar shows the instant cast), spent by
 * the next cast it fits and taken away when that spell is done (its missile landed, its effects
 * applied), or when it waits too long.
 */

#ifndef MANGOS_ARPG_PROCS_H
#define MANGOS_ARPG_PROCS_H

#include "Common.h"
#include "Arpg/ArpgUniques.h"

#include <string>
#include <utility>
#include <vector>

class Player;
class Spell;
class Unit;

namespace Arpg
{
    // The runes that are procs (Arpg/ArpgSkills.h): their text, for the rune's tooltip.
    char const* ProcRuneText(uint8 rune);

    // The worn uniques' proc lines, by item entry, for SMSG_ARPG_ITEM_MECHANICS.
    std::vector<std::pair<uint32, std::string>> ProcItemLines();

    // The power a named unique without an authored mechanic carries (its tooltip line and name),
    // or nullptr. Every such unique has one, from a pool, the same one always for its entry.
    char const* UniquePowerText(uint32 item);
    char const* UniquePowerName(uint32 item);

    // The more damage (percent) and movement speed a unique's power gives `unit` now.
    int32 UniqueFuryPct(Unit const* unit);
    int32 UniqueSwiftPct(Unit const* unit);

    // Triggers. A player's swing landed (crit or not) for `damage`.
    void OnProcSwing(Unit* attacker, Unit* victim, bool crit, uint32 damage);
    // A spell of the caster's hit `target` for `amount`: damage, or a heal.
    void OnProcSpellHit(Spell* spell, Unit* caster, Unit* target, bool crit, bool heal, uint32 amount);
    // A blow of `damage` from `attacker` landed on the player `victim`.
    void OnProcStruck(Unit* victim, Unit* attacker, uint32 damage);
    // A spell of the caster's landed on an enemy (its auras on, a stun).
    void OnProcLanded(Spell* spell, Unit* caster, Unit* target);
    // `killer` killed something worth XP or honour (or anything, with no XP to gain).
    void OnProcKill(Player* killer, Unit* victim);

    // Spending. `spell` was just cast (its go sent): a readied cast it fits is spent by it.
    void OnProcCast(Spell* spell);
    // `spell` is done: what it spent is taken away.
    void OnProcFinish(Spell* spell);

    // Every player update: a readied cast that waited too long goes.
    void UpdateProcs(Player* player);
    // The player is leaving: its readied casts go.
    void ForgetProcs(Player* player);

    // The uniques-kit rows `player`'s readied casts carry now (a synergy's burst, chain or spread on
    // its next cast of a spell), for the kit lookups (Arpg/ArpgUniques.h).
    std::vector<UniqueMechanic const*> ReadiedKits(Player const* player);
}

#endif
