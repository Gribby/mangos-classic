/*
 * ARPG dungeons and group scaling, for the benilla ARPG client (the client's
 * docs/ARPG-PROGRESSION.md, "Scaling for one to five players").
 *
 * With Arpg.Enable and Arpg.Dungeons on, WoW's encounters, sized for a tank, a healer and three
 * damage dealers (or forty players), are fought by one to five ARPG players:
 *
 *   Health scales with the players there. A dungeon creature has Arpg.Dungeons.SoloHealth of its
 *   vanilla health with one player inside, plus Arpg.Dungeons.PlayerHealth per player past the
 *   first; a raid's solo value is SoloHealth * 9 / its size, and each player past the first adds
 *   three quarters of that. An open-world elite has Arpg.Dungeons.EliteSoloHealth against one
 *   player, plus PlayerHealth per group member within 100 yards. A creature is scaled a moment
 *   after it is added to its map, and again whenever it enters combat with a different count,
 *   keeping its share of health.
 *
 *   Each hit on an ARPG player is capped at a share of its maximum health, by its source (melee,
 *   then spell): trash 6% / 12%, champions, rares and open-world elites 9% / 15%, dungeon bosses
 *   12% / 30%, raid and world bosses 15% / 35%. The cap follows the player's own health, so it
 *   never needs retuning by level; blows wear a player down, and what kills is standing in the
 *   big spells. The level gap moves the cap and the hit alike (Arpg::LevelGapMod): a player
 *   above the creature's level takes less, below it more. However the tier, the gap and the
 *   difficulty raise them, a trash hit never takes more than 12% / 20% and a champion's, rare's or
 *   open-world elite's 16% / 25%: no one-shots from a pack. Bosses keep their raised caps.
 *
 *   Dungeon trash (not raids) rolls champions and rares as the open world's packs do: from level
 *   8, 6% of mobs a champion and 1.5% a rare, so a run meets a handful.
 *
 * A Warden in every vanilla dungeon: one trash spawn per instance, among those that load (each
 * eligible one at 1 in 12 as it settles, the 25th surely, so it stands in the wing the group is
 * in), is crowned a named rare with two themed affixes (a third from
 * level 30), and its players are told. It drops two blue items, a 30% chance at a purple and a
 * 35% chance at a unique. Each final boss (Arpg::Bosses) drops a Cache: gold, and for each player
 * there a blue, a green, a 25% chance at a purple and a 50% chance at a unique (rolled rarities on
 * base items, Arpg/ArpgAffixes.h).
 *
 * Difficulty tiers: Normal, Hard, Brutal, Torment I, II and III. A dungeon instance takes the
 * tier its first ARPG player asks for (the client's ARPG View page), as far as that player has
 * it open there; a final boss's kill opens the next tier for everyone in at the kill (saved in
 * character_arpg_tier, which the server makes). Each tier raises its creatures' health (x1.4 to
 * x4.4) and damage (x1.15 to x2), the caps (x1.2 to x2.2), the champion and rare chances, their
 * affixes (one more from Brutal, two from Torment II), XP (x1.15 to x2), and the Warden's and
 * the Cache's drops: item levels, purple chances, and an extra blue per two tiers.
 *
 * Difficulty: every creature hits Arpg.Difficulty.Damage times harder, 1 + DamagePerLevel more
 * per level of the creature, and DungeonDamage more in a dungeon; the caps rise by the square
 * root of that. The knobs are tuned from the play statistics.
 *
 * Enrage timers keep a damage check: a dungeon boss still fighting after 4 minutes (a raid boss
 * after 6) enrages, +50% damage and cap, and a stack more every 30 seconds, up to ten. The
 * players there are told; leaving combat ends it.
 *
 * A boss, for the caps and the rolls: rank 3, or a ScriptDev "boss_" script, or (in a dungeon)
 * a health multiplier of 5 or more (6 from level 40), which vanilla's trash never reaches.
 */

#ifndef MANGOS_ARPG_DUNGEONS_H
#define MANGOS_ARPG_DUNGEONS_H

#include "Common.h"

class Creature;
class Loot;
class Map;
class Player;
class Unit;

namespace Arpg
{
    // Difficulty tiers: 0 Normal, 1 Hard, 2 Brutal, 3-5 Torment I-III.
    constexpr uint8 MAX_TIER = 5;

    // The tier `player` asks for (CMSG_ARPG_ACTION kind 19; its client sends it at the hello and
    // on a change). Loads the tiers it has open.
    void SetWantedTier(Player* player, uint8 tier);

    // The tier `map`'s instance runs at, for its drops (0 outside dungeons, or with Arpg.Dungeons off).
    uint8 LootTier(Map* map);

    // The tier `player` asks for (the dev tools' test loot rolls at it).
    uint8 WantedTier(Player* player);

    // "Normal" .. "Torment III".
    char const* TierName(uint8 tier);

    // The multipliers a dungeon's tier puts on its creatures' damage, and on their kills' XP.
    float TierDamageMod(Unit const* attacker);
    // The difficulty knobs on a creature's damage (Arpg.Difficulty.*): the base, its level's ramp,
    // and a dungeon's extra; 1 for anything a player owns.
    float DifficultyDamageMod(Unit const* attacker);
    float TierXpMod(Unit const* victim);

    // A logged-out player's tiers.
    void ForgetTiers(Player* player);

    // A creature was added to its map: scale it (instances, open-world elites) a moment later.
    void OnScalableCreatureAdded(Creature* creature);

    // `creature` entered combat with `enemy`: re-scale it if the players present changed.
    void OnScaledAggro(Creature* creature, Unit* enemy);

    // The most one hit of `attacker`'s may deal `victim` (0: no cap). `spell` for spell damage.
    uint32 DamageCap(Unit const* attacker, Unit const* victim, bool spell);

    // How long `caster`'s crowd control (`spellInfo`: a stun, root, fear, confuse, charm or
    // silence) may hold `victim`: a creature's on an ARPG player is cut to a moment, as an ARPG
    // keeps the player moving. Anything else keeps `duration`.
    int32 CapControl(Unit const* caster, Unit const* victim, SpellEntry const* spellInfo, int32 duration);

    // Whether `creature` is fighting an ARPG player, which turns off its in-combat "can't reach
    // you" regeneration.
    bool NoEvadeRegen(Creature const* creature);

    // Whether `creature` is a dungeon or raid boss (see above).
    bool IsDungeonBoss(Creature const* creature);

    // `victim`'s corpse loot was just made: a Warden's and a final boss's Cache are added to it.
    void OnDungeonLoot(Creature* victim);

    // Add a rolled item of `rarity` at item level `level` to `loot` (a Warden's, a Cache's, an act's).
    void AddLootPick(Loot* loot, uint8 rarity, uint32 level);

    // `creature` is leaving the world: forget its scaling.
    void ForgetScaled(Creature const* creature);
}

#endif
