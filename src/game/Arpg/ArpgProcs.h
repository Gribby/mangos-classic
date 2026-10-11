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
 * uniques (worn named items, their implicit line). A readied cast is a spell modifier on the
 * player, which the client hears of as stock (so its cast bar shows the instant cast), spent by
 * the next cast it fits and taken away when that spell is done (its missile landed, its effects
 * applied), or when it waits too long.
 */

#ifndef MANGOS_ARPG_PROCS_H
#define MANGOS_ARPG_PROCS_H

#include "Common.h"

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

    // Triggers. A player's swing landed (crit or not).
    void OnProcSwing(Unit* attacker, Unit* victim, bool crit);
    // A spell of the caster's hit `target`: damage, or a heal.
    void OnProcSpellHit(Spell* spell, Unit* caster, Unit* target, bool crit, bool heal);
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
}

#endif
