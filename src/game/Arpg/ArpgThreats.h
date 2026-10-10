/*
 * ARPG threats: what an ARPG player dodges, for the benilla ARPG client (the client's
 * docs/ARPG-PROGRESSION.md, "Threats and the roll").
 *
 * Bolts. A travelling spell (a Spell.dbc speed) that a hostile creature sends at an ARPG player,
 * one target and no area, no longer follows the player: it flies at the point where the player
 * stood when it left, and on arrival hits only a player still in its path (within BOLT_RADIUS of
 * the line from the caster to that point, plus the body's reach). A player who stepped out of the
 * path is missed; one mid-roll (Arpg/ArpgActions.h) dodges it whatever. Spell::AddUnitTarget
 * notes the line (AimsBolt) and Spell::DoAllEffectOnTarget asks at arrival (BoltOutcome); the
 * client flies the bolt at the same point.
 *
 * Telegraphs. Open-world elites, champions, rares, dungeon and raid bosses fighting an ARPG player
 * wind up a heavy attack every so often: they stop, roar, and the ground shows where it lands, for
 * a wind-up of 1.2 to 1.7 sec. Then it lands on every ARPG player still inside: a share of the
 * player's maximum health (20% from an elite to 45% from a raid boss), past the per-hit damage
 * caps (Arpg/ArpgDungeons.h), since it can always be avoided: walk out, or roll (mid-roll
 * dodges). A stun, fear or confuse on the creature during the wind-up breaks it (Hammer of
 * Justice answers a telegraph). Dungeon trash, elite as it is, does not telegraph, only its
 * champions and rares, so a pull reads at a glance. Shapes:
 *   ring   around the creature (War Stomp)
 *   cone   in front of it, at the player (Cleave)
 *   blast  a circle at the player's feet (Flamestrike); a caster's (a mana user's) shape
 * A boss cycles all three; any other creature keeps one, by its entry.
 *
 *   SMSG_ARPG_TELEGRAPH (0x344): uint8 kind (1 wind-up, 2 broken off), uint32 serial, uint64
 *     caster guid, uint8 shape (1 ring, 2 cone, 3 blast), uint8 grade (0 elite, 1 champion,
 *     2 rare, 3 boss, 4 raid boss), float x, y, z (the centre or the cone's apex), float
 *     orientation (the cone's), float radius, float half angle (the cone's, radians), uint32
 *     wind-up ms. Sent to the ARPG players near the creature.
 */

#ifndef MANGOS_ARPG_THREATS_H
#define MANGOS_ARPG_THREATS_H

#include "Common.h"
#include "Globals/SharedDefines.h"

class Creature;
class Unit;
class WorldObject;
struct SpellEntry;

namespace Arpg
{
    // How far from a bolt's line a body still takes it, in yards, before the body's own reach.
    constexpr float BOLT_RADIUS = 1.6f;

    // Whether `spell`, cast by `caster` at `target`, flies at the point `target` stands on now.
    bool AimsBolt(WorldObject const* caster, Unit const* target, SpellEntry const* spell);

    // A bolt from (fromX, fromY) at (aimX, aimY, aimZ) arrives at `target`: SPELL_MISS_NONE for a
    // hit, SPELL_MISS_DODGE mid-roll, SPELL_MISS_MISS out of its path.
    SpellMissInfo BoltOutcome(Unit const* target, float fromX, float fromY, float aimX, float aimY, float aimZ);

    // `creature` went into combat with `enemy`: an ARPG player's foe of a grade that telegraphs
    // starts its wind-ups.
    void OnTelegraphAggro(Creature* creature, Unit* enemy);

    // `creature` left the world.
    void ForgetTelegraph(Creature const* creature);
}

#endif
