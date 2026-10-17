/*
 * ARPG minions: the rules every class's minions follow (the client's docs/ARPG-CHARACTER.md,
 * "Minions: the rules").
 *
 * Two kinds: an ARPG player's own pet (a warlock's demon, one at a time, as stock), and the horde,
 * timed minions from skills, kills and items (imps, Infernals, the Doomguard, Book of the Dead's
 * skeletons), at most HordeCap() at once, the oldest first to go.
 *
 * A minion's hit carries the Minion tag beside its own, and its owner's increased and more
 * bonuses where the tags match (Arpg::DamageModForTags), with the minion's own "more" here
 * (MinionDamageMod). Its ailments, Daze and synergy triggers are its owner's (Arpg::OnHitLanded).
 *
 * They fight where the player does: the enemy the player last struck or cast at, else the nearest
 * enemy to the player; never more than LEASH yards from the player, back at its side past
 * TELEPORT. A command (Torment) points every minion at one enemy a while.
 *
 *   SMSG_ARPG_MINIONS (0x354), when it changes: uint8 demon (0 none), uint8 its health in percent,
 *     uint8 horde count, uint8 horde cap, cstring the demon's name.
 */

#ifndef MANGOS_ARPG_MINIONS_H
#define MANGOS_ARPG_MINIONS_H

#include "Common.h"

class Creature;
class Player;
class Unit;
struct SpellEntry;

namespace Arpg
{
    enum MinionKind : uint8
    {
        MINION_NONE = 0,
        MINION_IMP,         // a horde imp: Firebolts
        MINION_INFERNAL,
        MINION_DOOMGUARD,
        MINION_RAISED,      // a fallen enemy risen (Book of the Dead)
    };

    // The ARPG player whose minion `unit` is (its pet, a guardian or the horde's), or nullptr.
    Player* MinionOwner(Unit const* unit);
    // Whether `unit` is one of the horde, and its kind.
    MinionKind HordeKind(Unit const* unit);

    // A horde minion for `owner`: `entry` at the owner's level and faction, for `lifeMs`, at the
    // place given. The oldest goes when the horde is full. Its life and speed take the owner's
    // minion stats. nullptr if it could not be summoned.
    Creature* SummonHorde(Player* owner, uint32 entry, MinionKind kind, float x, float y, float z, uint32 lifeMs);
    // A creature a spell summoned (Inferno's Infernal) taken into the horde.
    void AdoptHorde(Player* owner, Creature* creature, MinionKind kind, uint32 lifeMs);
    uint32 HordeCount(Player const* owner);
    uint32 HordeCap(Player const* owner);

    // The player struck `victim` (or cast at it): its minions turn on it.
    void OnOwnerStruck(Player* owner, Unit* victim);
    // Every minion strikes `target` for `ms`, dealing it `morePct` more (Torment).
    void CommandMinions(Player* owner, Unit* target, uint32 ms, int32 morePct);
    // Infernal Heat: the minions deal more for `ms`.
    void HeatMinions(Player* owner, uint32 ms);
    // The multiplier a minion's hit on `victim` takes on top of its owner's bonuses.
    float MinionDamageMod(Unit const* minion, Player const* owner, Unit const* victim);
    // A minion's hit landed: its leech, its kind's ailments (Arpg::OnHitLanded calls it).
    void OnMinionHit(Unit* minion, Player* owner, Unit* victim, SpellEntry const* spellInfo, uint32 dealt);
    // Detonate the oldest horde minion: every enemy within 8 yards takes `pct`% of its life as
    // Fire. True if one went.
    bool SacrificeOldest(Player* owner, uint32 pct);
    // Heal every minion of `owner` for `amount`.
    void HealMinions(Player* owner, uint32 amount);

    // Every player update: the horde's expiry, the leash, targets, the imps' bolts, the HUD.
    void UpdateMinions(Player* owner);
    void ForgetMinions(Player* owner);
}

#endif
