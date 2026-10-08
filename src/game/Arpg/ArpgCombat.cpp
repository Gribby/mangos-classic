/*
 * ARPG combat for the benilla ARPG client. See ArpgCombat.h.
 */

#include "Arpg/ArpgCombat.h"

#include "Entities/Unit.h"
#include "Entities/Creature.h"
#include "Entities/Player.h"
#include "Entities/Item.h"
#include "World/World.h"
#include "Log/Log.h"
#include "Spells/Spell.h"
#include "Spells/SpellMgr.h"
#include "Server/DBCStores.h"
#include "Grids/GridNotifiers.h"
#include "Grids/GridNotifiersImpl.h"
#include "Grids/CellImpl.h"

#include <cmath>
#include <limits>

namespace
{
    // A swing strikes inside a 120 degree arc ahead of the attacker, as the stock facing test does.
    constexpr float MELEE_ARC = 2 * M_PI_F / 3;
    // How far out to gather melee candidates, in yards: past any melee reach, combat reach included.
    constexpr float MELEE_SEARCH = 12.0f;
    // A helpful spell lands on the friend standing within this many yards of the aim point.
    constexpr float ALLY_PICK = 4.0f;
    // An aim point this close to the caster gives no bearing: the facing is used instead.
    constexpr float AIM_MIN = 1.0f;

    // Whether `unit` may catch `actor`'s swing or skillshot: the intended target always; anyone
    // else only as a fair enemy, as an area spell would take it (no PvP flagging of bystanders),
    // visible to the player, not a critter, not held by crowd control a hit would break, and
    // hostile or already fighting, so a stray swing never pulls a neutral or a civilian.
    bool MayCatch(WorldObject* actor, Unit* unit, Unit const* intended)
    {
        if (unit == intended)
            return true;
        if (!actor->CanAttackSpell(unit, nullptr, true))
            return false;
        if (unit->HasBreakableByDamageCrowdControlAura())
            return false;
        if (unit->GetTypeId() == TYPEID_UNIT && unit->GetCreatureType() == CREATURE_TYPE_CRITTER)
            return false;
        if (actor->GetTypeId() == TYPEID_PLAYER && !static_cast<Player*>(actor)->HasAtClient(unit))
            return false;
        return actor->IsEnemy(unit) || unit->IsInCombat();
    }

    // Live attackable units around `center` within `radius` yards, in line of sight, no totems.
    UnitList EnemiesAround(WorldObject* center, float radius)
    {
        UnitList units;
        MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck check(center, radius);
        MaNGOS::UnitListSearcher<MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck> searcher(units, check);
        Cell::VisitAllObjects(center, searcher, radius);
        return units;
    }

    // The friend `spellInfo` can help nearest the aim point, within ALLY_PICK yards of it and in
    // the spell's range and sight; the caster when nobody stands there.
    Unit* SelectAllyNear(Player* player, SpellEntry const* spellInfo, float x, float y, float maxRange)
    {
        float const reach = std::min(player->GetDistance2d(x, y, DIST_CALC_NONE), maxRange) + ALLY_PICK;
        UnitList units;
        MaNGOS::AnyFriendlyUnitInObjectRangeCheck check(player, reach);
        MaNGOS::UnitListSearcher<MaNGOS::AnyFriendlyUnitInObjectRangeCheck> searcher(units, check);
        Cell::VisitAllObjects(player, searcher, reach);

        Unit* nearest = nullptr;
        float nearestDist = ALLY_PICK;
        for (Unit* unit : units)
        {
            if (!player->CanAssistSpell(unit, spellInfo) || !player->IsWithinDistInMap(unit, maxRange) ||
                    !player->IsWithinLOSInMap(unit))
                continue;
            float const dist = std::hypot(unit->GetPositionX() - x, unit->GetPositionY() - y);
            if (dist <= nearestDist)
            {
                nearest = unit;
                nearestDist = dist;
            }
        }
        return nearest ? nearest : player;
    }

    // The unit the held swing was pressed on, if it is still on the map.
    Unit* SwingTarget(Player* player)
    {
        ObjectGuid const guid = player->GetArpgSwingTarget();
        return guid ? player->GetMap()->GetUnit(guid) : nullptr;
    }

    // The attack animation a whiff plays, by what the hand holds.
    uint32 WhiffEmote(Player* player)
    {
        if (!player->GetWeaponForAttack(BASE_ATTACK, true, true))
            return EMOTE_ONESHOT_ATTACKUNARMED;
        return player->IsTwoHandUsed() ? EMOTE_ONESHOT_ATTACK2HTIGHT : EMOTE_ONESHOT_ATTACK1H;
    }

    // One hand's swing, if its timer is ready: strike the nearest fair enemy in the arc, or whiff.
    // Main and off hand are kept apart as the stock loop keeps them (Unit::UpdateMeleeAttackingState).
    void SwingHand(Player* player, WeaponAttackType hand)
    {
        if (!player->isAttackReady(hand))
            return;

        if (hand == BASE_ATTACK && player->hasOffhandWeaponForAttack() &&
                player->getAttackTimer(OFF_ATTACK) < ATTACK_DISPLAY_DELAY)
            player->setAttackTimer(OFF_ATTACK, ATTACK_DISPLAY_DELAY);
        if (hand == OFF_ATTACK && player->getAttackTimer(BASE_ATTACK) < ATTACK_DISPLAY_DELAY)
            player->setAttackTimer(BASE_ATTACK, ATTACK_DISPLAY_DELAY);

        if (Unit* struck = Arpg::SelectMeleeVictim(player, SwingTarget(player)))
        {
            // A queued next-swing ability (Heroic Strike, Cleave, Maul) lands with the swing.
            if (hand == BASE_ATTACK)
                if (Spell* nextSwing = player->GetCurrentSpell(CURRENT_MELEE_SPELL))
                    nextSwing->m_targets.setUnitTarget(struck);
            player->AttackerStateUpdate(struck, hand);
        }
        else if (hand == BASE_ATTACK)
            player->HandleEmoteCommand(WhiffEmote(player));

        player->resetAttackTimer(hand);
    }
}

namespace Arpg
{
    bool Active(WorldObject const* object)
    {
        if (!object || object->GetTypeId() != TYPEID_PLAYER)
            return false;
        Player const* player = static_cast<Player const*>(object);
        return sWorld.getConfig(CONFIG_BOOL_ARPG_ENABLE) && player->IsArpgClient() && !player->HasCharmer();
    }

    void OnHello(Player* player)
    {
        if (player->IsArpgClient())
            return;
        player->SetArpgClient(true);
        // The backpedal speed follows the run speed from here on (Unit::UpdateSpeed).
        player->UpdateSpeed(MOVE_RUN_BACK, true);
        sLog.outString("ARPG: %s plays on the ARPG client", player->GetName());
    }

    void UpdateSwing(Player* player)
    {
        if (!Active(player))
            return;
        if (!player->IsAlive())
        {
            player->SetArpgSwinging(false);
            return;
        }

        // Extra attacks (Windfury, Sword Specialization, Hand of Justice) land on whoever the swing
        // that earned them can strike now; with nobody there they are lost, as a stock extra attack
        // at a target out of reach is, so they never hold back the next proc.
        if (player->m_extraAttacks)
        {
            if (Unit* struck = Arpg::SelectMeleeVictim(player, SwingTarget(player)))
            {
                player->m_extraAttackGuid = struck->GetObjectGuid();
                player->DoExtraAttacks(struck);
            }
            player->m_extraAttacks = 0;
            player->m_extraAttackGuid = ObjectGuid();
        }

        if (!player->IsArpgSwinging())
            return;
        if (player->IsMounted() || player->IsNonMeleeSpellCasted(false) ||
                player->hasUnitState(UNIT_STAT_CAN_NOT_REACT_OR_LOST_CONTROL) ||
                player->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PACIFIED))
            return;

        SwingHand(player, BASE_ATTACK);
        if (player->hasOffhandWeaponForAttack())
            SwingHand(player, OFF_ATTACK);
    }

    void CastAt(Player* player, SpellEntry const* spellInfo, CastAim aim, float x, float y, float /*z*/, ObjectGuid intendedGuid)
    {
        SpellRangeEntry const* range = sSpellRangeStore.LookupEntry(spellInfo->rangeIndex);
        bool const melee = range && (range->Flags & SPELL_RANGE_FLAG_MELEE);
        float const minRange = GetSpellMinRange(range);
        // Range talents (Hawk Eye, Flame Throwing) reach as far for the pick as for the cast.
        float maxRange = GetSpellMaxRange(range);
        player->ApplySpellMod(spellInfo->Id, SPELLMOD_RANGE, maxRange, false);

        // The unit under the cursor: it may always catch the spell, so a neutral, a sheep or a
        // sapped mob can be opened on, but only if it is near where the player aimed.
        Unit* intended = intendedGuid ? player->GetMap()->GetUnit(intendedGuid) : nullptr;
        if (intended && intended->GetDistance2d(x, y, DIST_CALC_NONE) > ALLY_PICK + intended->GetCombatReach())
            intended = nullptr;

        // The line the player aimed along: toward the aim point, or ahead with the cursor underfoot.
        bool const pointed = player->GetDistance2d(x, y, DIST_CALC_NONE) >= AIM_MIN;
        float const bearing = pointed ? player->GetAngle(x, y) : player->GetOrientation();

        Unit* target = nullptr;
        if (aim == AIM_ALLY)
        {
            if (intended && player->CanAssistSpell(intended, spellInfo))
                target = intended;
            else
                target = SelectAllyNear(player, spellInfo, x, y, maxRange);
            // A buff takes the rank the friend's level allows, as the stock cast does.
            if (target)
                if (SpellEntry const* ranked = sSpellMgr.SelectAuraRankForLevel(spellInfo, target->GetLevel()))
                    spellInfo = ranked;
        }
        else if (melee)
        {
            target = SelectMeleeVictim(player, intended);
            // A next-swing ability (Heroic Strike, Maul, Cleave) queues on the unit aimed at even
            // out of reach; the swing that carries it strikes whoever it reaches.
            if (!target && IsNextMeleeSwingSpell(spellInfo) && intended && intended->IsAlive() &&
                    player->CanAttack(intended))
                target = intended;
        }
        else
            target = SelectLineTarget(player, bearing, minRange, maxRange, intended);

        // A skillshot with nobody on its line still fires: it flies out to its range and is spent
        // (Spell::cast's ARPG miss). Anything else needs a unit.
        // An auto-repeat shot (Shoot, Auto Shot) needs a unit to repeat at.
        bool const line = aim == AIM_ENEMY && !melee && IsLineSpell(spellInfo) && !IsAutoRepeatRangedSpell(spellInfo);
        if (!target && !line)
        {
            Spell::SendCastResult(player, spellInfo, SPELL_FAILED_BAD_TARGETS);
            return;
        }

        SpellCastTargets targets;
        if (target)
            targets.setUnitTarget(target);

        Spell* spell = new Spell(player, spellInfo, TRIGGERED_NONE);
        spell->m_clientCast = true;
        if (aim == AIM_ENEMY && !melee)
            spell->SetArpgAim(bearing, pointed ? player->GetDistance2d(x, y, DIST_CALC_NONE) : 0.0f);
        spell->SpellStart(&targets);
    }

    bool FacingFront(WorldObject const* actor, WorldObject const* target)
    {
        if (Active(actor))
            return target && target->HasInArc(actor);
        return actor->IsFacingTargetsFront(target);
    }

    bool FacingBack(WorldObject const* actor, WorldObject const* target)
    {
        if (Active(actor))
            return target && !target->HasInArc(actor);
        return actor->IsFacingTargetsBack(target);
    }

    Unit* SelectMeleeVictim(Unit* attacker, Unit* victim)
    {
        auto strikes = [attacker, victim](Unit* unit)
        {
            return unit->IsAlive() && attacker->CanReachWithMeleeAttack(unit) &&
                   attacker->HasInArc(unit, MELEE_ARC) && attacker->CanAttackInCombat(unit, false, false) &&
                   MayCatch(attacker, unit, victim);
        };

        if (victim && strikes(victim))
            return victim;

        Unit* nearest = nullptr;
        float nearestDist = std::numeric_limits<float>::max();
        for (Unit* unit : EnemiesAround(attacker, MELEE_SEARCH))
        {
            if (unit == victim || !strikes(unit))
                continue;
            float const dist = attacker->GetDistance(unit);
            if (dist < nearestDist)
            {
                nearest = unit;
                nearestDist = dist;
            }
        }
        return nearest;
    }

    bool IsLineSpell(SpellEntry const* spellInfo)
    {
        if (!spellInfo || IsAreaOfEffectSpell(spellInfo))
            return false;

        // A channel holds its target (Mind Control, Tame Beast, Drain Life), and a spell cast
        // through walls would fizzle against the line's sight test.
        if (IsChanneledSpell(spellInfo) || IsIgnoreLosSpell(spellInfo))
            return false;

        // Melee-range abilities (Sinister Strike, Mortal Strike) take the arc instead.
        if (SpellRangeEntry const* range = sSpellRangeStore.LookupEntry(spellInfo->rangeIndex))
            if (range->Flags & SPELL_RANGE_FLAG_MELEE)
                return false;

        // Some effect lands on one enemy unit, and none is aimed anywhere else.
        bool atEnemy = false;
        for (uint32 i = 0; i < MAX_EFFECT_INDEX; ++i)
        {
            if (!spellInfo->Effect[i])
                continue;
            uint32 targetA = spellInfo->EffectImplicitTargetA[i];
            if (targetA == TARGET_UNIT_ENEMY)
                atEnemy = true;
            else if (targetA != TARGET_NONE && targetA != TARGET_UNIT_CASTER)
                return false;
        }
        return atEnemy;
    }

    Unit* SelectLineTarget(WorldObject* caster, float aim, float minRange, float maxRange, Unit const* intended)
    {
        float const dirX = std::cos(aim);
        float const dirY = std::sin(aim);
        float const originX = caster->GetPositionX();
        float const originY = caster->GetPositionY();

        Unit* first = nullptr;
        float firstAlong = std::numeric_limits<float>::max();
        // Gather a little past the range: a large target's edge can reach into the line's end.
        for (Unit* unit : EnemiesAround(caster, maxRange + 10.0f))
        {
            if (!MayCatch(caster, unit, intended))
                continue;
            float const reach = unit->GetCombatReach();
            float const offX = unit->GetPositionX() - originX;
            float const offY = unit->GetPositionY() - originY;
            // Distance along the line, and off to its side.
            float const along = offX * dirX + offY * dirY;
            float const across = std::fabs(offX * dirY - offY * dirX);
            // Past the far end, or inside the dead zone of a spell with a minimum range.
            if (along <= 0.0f || along - reach > maxRange || along + reach < minRange)
                continue;
            if (across > LINE_HALF_WIDTH + reach)
                continue;
            if (along < firstAlong)
            {
                first = unit;
                firstAlong = along;
            }
        }
        return first;
    }
}
