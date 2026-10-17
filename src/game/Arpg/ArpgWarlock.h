/*
 * ARPG warlock: the hooks of the warlock's keystones, its web's and its skill trees' (the client's
 * docs/ARPG-CHARACTER.md, "The warlock, in full"). The minions' own rules are Arpg/ArpgMinions.h;
 * the "more" on damage lives with the rest (Arpg/ArpgCharacter.cpp's DamagePartsFor).
 *
 * What an ARPG warlock gets beyond the stock class: its class spells need no reagents (no Soul
 * Shard, Infernal Stone or Demonic Figurine); a demon's summoning, Inferno and Ritual of Doom cast
 * at once with a 1.5 sec recovery (Arpg::CastsAtOnce); Inferno's Infernal and Ritual of Doom's
 * Doomguard join the horde for a while instead of turning on the warlock, and the Ritual needs no
 * party and no portal.
 */

#ifndef MANGOS_ARPG_WARLOCK_H
#define MANGOS_ARPG_WARLOCK_H

#include "Common.h"

class Creature;
class Player;
class Spell;
class Unit;
struct SpellEntry;

namespace Arpg
{
    // A warlock's own hit with a spell landed (not a minion's): Shadowburn, Hellstorm, Burning
    // Rain, Firestorm, Infernal Heat.
    void OnWarlockHit(Player* player, Unit* victim, SpellEntry const* spellInfo, uint32 dealt, bool crit);
    // A periodic damage tick of `spellInfo` from `caster` on `target` (any caster): Nightfall,
    // Tormented Soul, Soul Feast, Soul Siphon, Life Link.
    void OnWarlockTick(Unit* caster, Unit* target, SpellEntry const* spellInfo, uint32 damage);
    // `killer` killed `victim` (by its own hand or a minion's): Legion, Imp Gate, Reaping, Shadowburn.
    void OnWarlockKill(Player* killer, Unit* victim);
    // `killer` (any unit) killed `victim`: a minion's kill (Burning Legion, Fel Bond).
    void OnMinionKill(Unit* killer, Unit* victim);
    // A warlock's spell went off: the curses' command, Life Tap's nodes, a demon's summoning
    // remembered, Ritual of Doom's Doomguard.
    void OnWarlockCast(Spell* spell);
    // Whether `spell`'s effect `effIndex` is skipped (Ritual of Doom's ritual, for an ARPG warlock).
    bool VetoSpellEffect(Spell const* spell, uint8 effIndex);
    // A spell's summon (Inferno's Infernal) for an ARPG warlock: taken into the horde. True if so.
    bool AdoptSpellSummon(Unit* caster, Creature* summon);
    // The damage an ARPG warlock takes after Soul Link, Hellfire Ward and Backlash (never an
    // instant kill's).
    uint32 WarlockDamageTaken(Unit* victim, Unit* dealer, uint32 damage, SpellEntry const* spellInfo, bool instakill);
    // Whether `spellInfo` is one of the warlock's summons that cast at once (a demon, Inferno,
    // Ritual of Doom), whose recovery is a flat SUMMON_RECOVERY_MS.
    constexpr uint32 SUMMON_RECOVERY_MS = 1500;
    bool IsArpgSummon(SpellEntry const* spellInfo);
    // Every player update: Fel Domination.
    void UpdateWarlock(Player* player);
    void ForgetWarlock(Player* player);
}

#endif
