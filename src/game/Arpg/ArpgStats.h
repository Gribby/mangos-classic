/*
 * ARPG play statistics, for tuning the game from play (the client's docs/ARPG-HANDOFF.md,
 * "Play statistics").
 *
 * With Arpg.Stats on (the default), the server records what an ARPG player's play looks like and
 * appends it as JSON lines to arpg-stats/YYYY-MM-DD.jsonl in its run folder:
 *   - "death": who killed the character (name, entry, level, elite, champion tier, boss), the
 *     last blows taken within 10 sec (source, spell, amount), where, its level, mana left;
 *   - "pack": a pack fight from its first aggro to its last member's death (size, level, seconds,
 *     the damage it dealt), as the server log's "ARPG packs:" line;
 *   - "session": every 5 minutes and at logout, the session so far: minutes, levels gained and
 *     how long each took, kills by kind, deaths, damage dealt by spell (casts, hits, damage),
 *     damage taken by source, seconds in combat, seconds in combat below 15% mana and below 30%
 *     health, casts refused for want of mana or rage or energy (by spell), flasks drunk, rolls,
 *     minutes per zone. A later "session" line of the same id supersedes an earlier one.
 * update-arpg-server.bat sends the folder to the repository's arpg-stats branch (send-arpg-stats.bat).
 */

#ifndef MANGOS_ARPG_STATS_H
#define MANGOS_ARPG_STATS_H

#include "Common.h"

class Player;
class Spell;
class Unit;
struct SpellEntry;

namespace Arpg
{
    // `dealer` dealt `damage` to `victim` with `spell` (nullptr: a swing), any damage at all.
    void OnStatsDamage(Unit* dealer, Unit* victim, uint32 damage, SpellEntry const* spell);
    // `victim` died to `killer` (either may be an ARPG player).
    void OnStatsKill(Unit* killer, Unit* victim);
    // `spell` was cast (its go sent).
    void OnStatsCast(Spell const* spell);
    // `caster`'s cast of `spellInfo` was refused with `result` (a SpellCastResult).
    void OnStatsCastFailed(Player const* caster, SpellEntry const* spellInfo, uint32 result);
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
