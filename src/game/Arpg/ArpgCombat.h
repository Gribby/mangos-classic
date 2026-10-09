/*
 * ARPG combat for the benilla ARPG client. With Arpg.Enable on in mangosd.conf, a client that
 * sends the ARPG hello (CMSG_ARPG_ACTION) plays targetless, top-down ARPG combat; every other
 * player, playerbots included, keeps stock combat.
 *
 * For an ARPG player:
 *   - Swings are held, not targeted: while the swing is held the character swings on its weapon
 *     timer toward its facing (the cursor) and strikes the nearest fair enemy in its frontal arc,
 *     or whiffs with the attack animation. The stock auto-attack loop is off.
 *   - Casts carry an aim point instead of a target: a ranged hostile spell is a skillshot along
 *     the aimed line (the first enemy in its path, re-checked when the cast completes), a melee
 *     ability takes the nearest enemy in the arc, and a helpful spell the friend nearest the aim
 *     point, else the caster.
 *   - Facing never fails a cast or a swing; the target's own facing still decides parry, block,
 *     dodge, Gouge and Backstab.
 *   - Backpedal is as fast as running, as the character always faces the cursor.
 *
 * Only the intended target or a fair enemy can catch a swing or a skillshot: hostile or already
 * in combat, visible to the player, not a critter, not held by breakable crowd control.
 */

#ifndef MANGOS_ARPG_COMBAT_H
#define MANGOS_ARPG_COMBAT_H

#include "Common.h"
#include "Entities/ObjectGuid.h"

class Player;
class Unit;
class WorldObject;
struct SpellEntry;

namespace Arpg
{
    // CMSG_ARPG_ACTION kinds. `intended` is the unit under the client's cursor (0 for none): it
    // may always catch the swing or the spell, so a neutral, a sheep or a sapped mob can be opened on.
    enum ActionKind : uint8
    {
        ACTION_HELLO       = 0,                             // uint8 protocol version
        ACTION_SWING_START = 1,                             // uint64 intended
        ACTION_SWING_STOP  = 2,
        ACTION_CAST        = 3,                             // uint32 spell, uint8 CastAim, float x, y, z, uint64 intended
        ACTION_AIM         = 4,                             // float x, y, z, uint64 intended
        ACTION_LOOT        = 5,                             // uint64 corpse, uint8 loot slot (0xFF the gold)
        ACTION_LOOT_QUERY  = 6,                             // uint64 corpse
    };

    // What the client's cast aims at, by the spell's own target word.
    enum CastAim : uint8
    {
        AIM_ENEMY = 0,
        AIM_ALLY  = 1,
    };

    // The ARPG protocol version this server speaks.
    constexpr uint8 ARPG_PROTOCOL_VERSION = 2;

    // Whether `object` is a player on the ARPG client, with Arpg.Enable on, and driving itself: a
    // mind-controlled ARPG player fights as its controller's AI makes it, the stock way.
    bool Active(WorldObject const* object);

    // The ARPG client said hello: mark the player and refresh its backpedal speed.
    void OnHello(Player* player);

    // Whether items bind to `player` as stock (on pickup, equip or use): not for an ARPG player,
    // whose gear can always be traded and sold on.
    bool ItemsBind(Player const* player);

    // Run the held swing and any pending extra attacks for this tick: called from Player::Update.
    void UpdateSwing(Player* player);

    // Start a cast of `spellInfo` aimed at a world point, `intendedGuid` the unit under the cursor.
    void CastAt(Player* player, SpellEntry const* spellInfo, CastAim aim, float x, float y, float z, ObjectGuid intendedGuid);

    // Re-aim the player's running skillshot at a world point, `intendedGuid` the unit under the
    // cursor now: the client streams the cursor while a cast runs, so the shot goes where the
    // player points when it is released. A cursor underfoot keeps the cast's aim.
    void UpdateAim(Player* player, float x, float y, float z, ObjectGuid intendedGuid);

    // WorldObject::IsFacingTargetsFront / IsFacingTargetsBack, where for an ARPG actor only the
    // target's facing counts (the target faces it, or turns its back).
    bool FacingFront(WorldObject const* actor, WorldObject const* target);
    bool FacingBack(WorldObject const* actor, WorldObject const* target);

    // Who a swing strikes: `victim` (if any) while that stands in reach and in the frontal arc,
    // else the nearest fair enemy that does, else nobody (a whiff).
    Unit* SelectMeleeVictim(Unit* attacker, Unit* victim);

    // Whether a spell is a skillshot: hostile, aimed at one enemy unit, not melee range, no area
    // effect, not channeled (Mind Control, Tame Beast) and not line-of-sight exempt.
    bool IsLineSpell(SpellEntry const* spellInfo);

    // The first fair enemy (or `intended`) along the line from `caster` at `aim` (radians, world
    // orientation) between `minRange` and `maxRange` yards, the line LINE_HALF_WIDTH wide plus each
    // target's combat reach, in line of sight; nullptr when the line is clear.
    Unit* SelectLineTarget(WorldObject* caster, float aim, float minRange, float maxRange, Unit const* intended);

    // Half the width of a skillshot's path, in yards.
    constexpr float LINE_HALF_WIDTH = 1.75f;

    // A skillshot that misses flies this far past the aim point, in yards (capped at the spell's
    // range), and never stops nearer than MISS_MIN.
    constexpr float MISS_OVERSHOOT = 6.0f;
    constexpr float MISS_MIN = 5.0f;
}

#endif
