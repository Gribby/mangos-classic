/*
 * ARPG play statistics, for tuning the game from play (the client's docs/ARPG-HANDOFF.md,
 * "Play statistics").
 *
 * With Arpg.Stats on (the default), the server records what an ARPG player's play looks like and
 * appends it as JSON lines to arpg-stats/YYYY-MM-DD.jsonl in its run folder:
 *   - "death": who killed the character (name, entry, level, elite, champion tier, boss), the
 *     last blows taken within 10 sec (source, spell, amount), where, its level, mana left;
 *   - "pack": a pack fight from its first aggro to its last member's death (size, level, seconds,
 *     the damage it dealt), as the server log's "ARPG packs:" line; only a pack a player killed;
 *   - "fight": each fight a character takes part in, from entering combat to leaving it or dying
 *     (one with a kill, a death or at least 3 seconds): seconds, kills by kind and foes by name,
 *     damage dealt and per second, damage taken (and as a share of max health), lowest health and
 *     mana, flasks and rolls, the five spells that dealt the most;
 *   - "session": every 5 minutes and at logout, the session so far: minutes, levels gained and
 *     how long each took, kills by kind, deaths, damage dealt by spell (casts, hits, damage),
 *     damage taken by source, seconds in combat, seconds in combat below 20% mana and below 35%
 *     health, casts refused for want of mana or rage or energy (by spell), casts that overdrew
 *     (cost more than was left, by spell), flasks drunk, rolls,
 *     minutes per zone. A later "session" line of the same id supersedes an earlier one.
 * update-arpg-server.bat sends the folder to the repository's arpg-stats branch (send-arpg-stats.bat).
 */

#ifndef MANGOS_ARPG_STATS_H
#define MANGOS_ARPG_STATS_H

#include "Common.h"

#include <string>

class Player;
class Spell;
class Unit;
struct SpellEntry;

namespace Arpg
{
    // `dealer` dealt `damage` to `victim` with `spell` (nullptr: a swing), any damage at all. Only
    // what the victim had left counts (no overkill).
    void OnStatsDamage(Unit* dealer, Unit* victim, uint32 damage, SpellEntry const* spell);
    // While one stands, the damage dealt on this thread is a secondary hit's (a proc, an added
    // hit, an ailment's tick) and is logged as "<spell> (proc)" or "<spell> (ailment)", apart from
    // the spell's own casts.
    struct StatsSecondary
    {
        explicit StatsSecondary(bool periodic);
        ~StatsSecondary();
        StatsSecondary(StatsSecondary const&) = delete;
        StatsSecondary& operator=(StatsSecondary const&) = delete;
        char const* previous;
    };
    // `victim` died to `killer` (either may be an ARPG player).
    void OnStatsKill(Unit* killer, Unit* victim);
    // `spell` was cast (its go sent).
    void OnStatsCast(Spell const* spell);
    // `caster`'s cast of `spellInfo` was refused with `result` (a SpellCastResult).
    void OnStatsCastFailed(Player const* caster, SpellEntry const* spellInfo, uint32 result);
    // A dev note on the act's road ("#note ...", Arpg/ArpgActs.h), with where the character stands.
    void OnStatsNote(Player const* player, std::string const& text);
    // A cast cost more mana or rage than was left, and took it all (overdraw).
    void OnStatsOverdraw(Player const* caster, SpellEntry const* spellInfo);
    // A flask drunk, a roll made.
    void OnStatsFlask(Player const* player);
    void OnStatsRoll(Player const* player);
    // A pack fight ended.
    void OnStatsPack(char const* name, uint32 size, uint32 level, float seconds, uint32 damage);
    // Every player update: the samples and the periodic session line.
    void UpdateStats(Player* player);
    // The player is leaving: its last session line, and forget it.
    void FlushStats(Player* player);
}

#endif
