/*
 * ARPG threats: bolts that fly at a point, and telegraphed attacks. See ArpgThreats.h.
 */

#include "Arpg/ArpgThreats.h"
#include "Arpg/ArpgActions.h"
#include "Arpg/ArpgCombat.h"
#include "Arpg/ArpgDungeons.h"
#include "Arpg/ArpgPacks.h"

#include "AI/BaseAI/UnitAI.h"
#include "Entities/Creature.h"
#include "Entities/Player.h"
#include "Maps/Map.h"
#include "Server/Opcodes.h"
#include "Server/WorldPacket.h"
#include "Server/WorldSession.h"
#include "Spells/SpellMgr.h"
#include "Util/Timer.h"
#include "World/World.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <tuple>
#include <vector>

namespace
{
    using namespace Arpg;

    // --- Telegraphs ---

    enum Shape : uint8 { SHAPE_RING = 1, SHAPE_CONE = 2, SHAPE_BLAST = 3 };
    enum Grade : uint8 { GRADE_ELITE, GRADE_CHAMPION, GRADE_RARE, GRADE_BOSS, GRADE_RAID_BOSS, GRADE_NONE };
    enum PacketKind : uint8 { TELEGRAPH_WIND_UP = 1, TELEGRAPH_BROKEN = 2 };

    struct GradeSpec
    {
        uint32 windUpMs;
        float ring, cone, blast;      // the shapes' radii, in yards
        uint32 healthPct;             // of the player's maximum health
        uint32 firstMs;               // the first wind-up, after the pull
        uint32 everyMin, everyMax;    // then one every so often
    };
    GradeSpec const GRADES[] =
    {
        { 1500, 6.0f,  10.0f, 4.0f, 20, 5000, 11000, 15000 },  // open-world elite
        { 1300, 7.0f,  11.0f, 4.0f, 25, 4000, 10000, 14000 },  // champion
        { 1200, 7.5f,  12.0f, 4.5f, 30, 4000,  9000, 12000 },  // rare
        { 1500, 9.0f,  14.0f, 5.0f, 35, 3000,  8000, 11000 },  // dungeon boss
        { 1700, 10.0f, 16.0f, 6.0f, 45, 3000,  8000, 11000 },  // raid boss
    };
    constexpr float CONE_HALF_ANGLE = float(M_PI) / 4.0f;      // a 90 degree cone
    // A ring or a cone is only wound up with the player this close (beyond the shape's own reach):
    // a creature that is being kited waits.
    constexpr float WIND_UP_SLACK = 4.0f;
    constexpr float BLAST_RANGE = 35.0f;
    constexpr float FOE_RANGE = 40.0f;                         // an ARPG player it may aim at
    constexpr float SEND_RANGE = 100.0f;                       // who sees the ground marks
    constexpr float HEIGHT_REACH = 6.0f;                       // a body this far above or below is out
    constexpr uint32 RETRY_MS = 1000, IDLE_RETRY_MS = 2000;
    // How often a wind-up looks at its creature: a stun, fear or confuse breaks it off at once.
    constexpr uint32 WATCH_MS = 100;
    // A chain that has not stepped for this long stopped without a word (its creature died out of
    // sight of the chain, say): longer than any wait between steps.
    constexpr uint32 STALE_MS = 22000;
    // The names in the combat log, and the schools.
    constexpr uint32 SPELL_RING = 20549;     // War Stomp
    constexpr uint32 SPELL_CONE = 845;       // Cleave
    constexpr uint32 SPELL_BLAST = 2120;     // Flamestrike

    struct Key
    {
        uint32 map, instance;
        ObjectGuid guid;
        bool operator<(Key const& o) const { return std::tie(map, instance, guid) < std::tie(o.map, o.instance, o.guid); }
    };

    struct Telegraph
    {
        uint32 generation = 0;
        uint32 lastTick = 0;            // the chain's last step, to spot one that died with its creature
        uint8 grade = GRADE_NONE;
        uint8 cycle = 0;                // a boss's next shape
        bool winding = false;
        // The wind-up in hand.
        uint32 serial = 0;
        uint8 shape = 0;
        float x = 0.0f, y = 0.0f, z = 0.0f, o = 0.0f, radius = 0.0f;
        uint32 startedAt = 0;           // the wind-up's start, for the landing time
        // What the wind-up took from the creature's AI, to give back (to that AI only).
        bool hadMovement = true, hadMelee = true;
        bool froze = false;
        UnitAI const* frozenAi = nullptr;
    };

    std::mutex sThreatsLock;
    std::map<Key, Telegraph> sTelegraphs;
    uint32 sGeneration = 0;
    uint32 sSerial = 0;

    Key KeyOf(Creature const* creature)
    {
        return { creature->GetMapId(), creature->GetInstanceId(), creature->GetObjectGuid() };
    }

    uint8 GradeOf(Creature* creature)
    {
        if (creature->IsPet() || creature->IsTotem() || creature->GetOwnerGuid() || creature->IsPlayerControlled())
            return GRADE_NONE;
        Map* map = creature->GetMap();
        bool const instanced = map->IsDungeon();
        if (instanced && IsDungeonBoss(creature))
            return map->IsRaid() ? GRADE_RAID_BOSS : GRADE_BOSS;
        if (creature->IsWorldBoss())
            return GRADE_RAID_BOSS;
        switch (ChampionTier(creature))
        {
            case 2: return GRADE_RARE;
            case 1: return GRADE_CHAMPION;
            default: break;
        }
        // Dungeon trash is elite through and through: only its champions and rares telegraph.
        if (!instanced && creature->IsElite())
            return GRADE_ELITE;
        return GRADE_NONE;
    }

    bool IsArpgPlayer(Unit const* unit)
    {
        return unit && unit->GetTypeId() == TYPEID_PLAYER && unit->IsAlive() && Active(unit);
    }

    // The ARPG player the creature aims at: its victim if that is one, else the nearest one it is
    // fighting.
    Player* FoeOf(Creature& creature)
    {
        Unit* victim = creature.GetVictim();
        if (IsArpgPlayer(victim) && creature.IsWithinDist(victim, FOE_RANGE))
            return static_cast<Player*>(victim);
        Player* best = nullptr;
        float bestDist = FOE_RANGE;
        for (auto const& ref : creature.GetMap()->GetPlayers())
        {
            Player* player = ref.getSource();
            if (!IsArpgPlayer(player) || !player->IsInWorld())
                continue;
            if (creature.getThreatManager().getThreat(player) <= 0.0f)
                continue;
            float const d = creature.GetDistance(player);
            if (d < bestDist)
            {
                bestDist = d;
                best = player;
            }
        }
        return best;
    }

    uint8 ShapeFor(Creature const& creature, Telegraph& t)
    {
        if (t.grade == GRADE_BOSS || t.grade == GRADE_RAID_BOSS)
        {
            uint8 const shape = uint8(SHAPE_RING + t.cycle % 3);
            ++t.cycle;
            return shape;
        }
        if (creature.GetPowerType() == POWER_MANA && creature.GetMaxPower(POWER_MANA) > 0)
            return SHAPE_BLAST;
        return creature.GetEntry() % 2 ? SHAPE_CONE : SHAPE_RING;
    }

    uint32 NextDelay(uint8 grade)
    {
        GradeSpec const& g = GRADES[grade];
        return urand(g.everyMin, g.everyMax);
    }

    void Send(Creature& creature, uint8 kind, Telegraph const& t)
    {
        GradeSpec const& g = GRADES[t.grade];
        WorldPacket data(SMSG_ARPG_TELEGRAPH, 1 + 4 + 8 + 1 + 1 + 4 * 7);
        data << uint8(kind) << uint32(t.serial) << creature.GetObjectGuid() << uint8(t.shape) << uint8(t.grade);
        data << t.x << t.y << t.z << t.o << t.radius;
        data << float(t.shape == SHAPE_CONE ? CONE_HALF_ANGLE : 0.0f);
        data << uint32(g.windUpMs);
        for (auto const& ref : creature.GetMap()->GetPlayers())
        {
            Player* player = ref.getSource();
            if (player && player->IsInWorld() && Active(player) && creature.IsWithinDist(player, SEND_RANGE))
                player->GetSession()->SendPacket(data);
        }
    }

    // Whether `unit` stands inside the wind-up's shape.
    bool Inside(Telegraph const& t, Unit const* unit)
    {
        float const reach = unit->GetObjectBoundingRadius();
        if (std::fabs(unit->GetPositionZ() - t.z) > HEIGHT_REACH)
            return false;
        float const dx = unit->GetPositionX() - t.x, dy = unit->GetPositionY() - t.y;
        float const dist = std::sqrt(dx * dx + dy * dy);
        if (dist > t.radius + reach)
            return false;
        if (t.shape != SHAPE_CONE || dist < 0.5f)
            return true;
        float diff = std::atan2(dy, dx) - t.o;
        while (diff > float(M_PI))
            diff -= 2.0f * float(M_PI);
        while (diff < -float(M_PI))
            diff += 2.0f * float(M_PI);
        // The body's reach widens the cone a little at the edge.
        return std::fabs(diff) <= CONE_HALF_ANGLE + std::atan2(reach, dist);
    }

    // Whether the creature is someone else's to command now (charmed, mind-controlled).
    bool Commanded(Creature const& creature)
    {
        return creature.HasCharmer() || creature.IsPlayerControlled();
    }

    void Freeze(Creature& creature, Telegraph& t)
    {
        // A scripted creature (a boss) runs its own movement and phases: it is left to them, and
        // its wind-up is the mark and the roar alone.
        if (creature.GetScriptId())
            return;
        if (UnitAI* ai = creature.AI())
        {
            t.froze = true;
            t.frozenAi = ai;
            t.hadMovement = ai->IsCombatMovement();
            t.hadMelee = ai->IsMeleeEnabled();
            ai->SetCombatMovement(false, true);
            ai->SetMeleeEnabled(false);
        }
        if (t.shape == SHAPE_CONE)
        {
            // Hold the facing: the client turns a model toward its target.
            creature.SetTarget(nullptr);
            creature.SetFacingTo(t.o);
        }
    }

    void Thaw(Creature& creature, Telegraph const& t)
    {
        if (!t.froze)
            return;
        // Only the AI that was frozen: one swapped in since (a charm's end) starts fresh.
        UnitAI* ai = creature.AI();
        if (ai && ai == t.frozenAi)
        {
            ai->SetCombatMovement(t.hadMovement, true);
            ai->SetMeleeEnabled(t.hadMelee);
        }
        if (t.shape == SHAPE_CONE && creature.GetVictim())
            creature.SetTarget(creature.GetVictim());
    }

    void Schedule(Creature& creature, Key key, uint32 generation, uint32 delay, bool watch);

    // Break the wind-up off: tell the players, give the AI back, and wait for the next one (or
    // end the chain, with the creature dead, out of combat or commanded).
    void BreakOff(Creature& creature, Key key, uint32 generation)
    {
        Telegraph t;
        bool const goOn = creature.IsAlive() && creature.IsInCombat() && !Commanded(creature);
        {
            std::lock_guard<std::mutex> guard(sThreatsLock);
            auto it = sTelegraphs.find(key);
            if (it == sTelegraphs.end() || it->second.generation != generation || !it->second.winding)
                return;
            it->second.winding = false;
            t = it->second;
            if (!goOn)
                sTelegraphs.erase(it);
        }
        Send(creature, TELEGRAPH_BROKEN, t);
        // Dead too: a respawn keeps the AI's movement and melee switches.
        Thaw(creature, t);
        if (goOn)
            Schedule(creature, key, generation, NextDelay(t.grade), false);
    }

    // The wind-up lands on whoever is still inside.
    void Detonate(Creature& creature, Key key, uint32 generation)
    {
        Telegraph t;
        {
            std::lock_guard<std::mutex> guard(sThreatsLock);
            auto it = sTelegraphs.find(key);
            if (it == sTelegraphs.end() || it->second.generation != generation || !it->second.winding)
                return;
            it->second.winding = false;
            t = it->second;
        }
        Thaw(creature, t);
        creature.HandleEmoteCommand(t.shape == SHAPE_BLAST ? EMOTE_ONESHOT_SPELLCAST : EMOTE_ONESHOT_SPECIALATTACK1H);

        GradeSpec const& g = GRADES[t.grade];
        uint32 const spellId = t.shape == SHAPE_RING ? SPELL_RING : t.shape == SHAPE_CONE ? SPELL_CONE : SPELL_BLAST;
        SpellSchoolMask const school = t.shape == SHAPE_BLAST ? SPELL_SCHOOL_MASK_FIRE : SPELL_SCHOOL_MASK_NORMAL;
        std::vector<Player*> struck;
        for (auto const& ref : creature.GetMap()->GetPlayers())
        {
            Player* player = ref.getSource();
            if (IsArpgPlayer(player) && player->IsInWorld() && !player->IsGameMaster() &&
                    creature.CanAttack(player) && Inside(t, player))
                struck.push_back(player);
        }
        for (Player* player : struck)
        {
            if (Dodging(player))
            {
                Unit::SendSpellMiss(&creature, player, spellId, SPELL_MISS_DODGE);
                continue;
            }
            if (player->IsImmuneToDamage(school))
            {
                Unit::SendSpellMiss(&creature, player, spellId, SPELL_MISS_IMMUNE);
                continue;
            }
            uint32 damage = std::max<uint32>(1, player->GetMaxHealth() * g.healthPct / 100);
            uint32 absorb = 0;
            Unit::DealDamageMods(&creature, player, damage, &absorb, SPELL_DIRECT_DAMAGE, nullptr);
            Unit::SendSpellNonMeleeDamageLog(&creature, player, spellId, damage, school, absorb, 0, false, 0);
            if (damage)
                Unit::DealDamage(&creature, player, damage, nullptr, SPELL_DIRECT_DAMAGE, school, nullptr, false);
        }
        if (creature.IsAlive())
            Schedule(creature, key, generation, NextDelay(t.grade), false);
    }

    // A wind-up in progress: land it on time, or break it off the moment the creature is
    // stunned, feared, confused, commanded, out of combat or dead.
    void Watch(Creature& creature, Key key, uint32 generation)
    {
        uint32 startedAt = 0;
        uint8 grade = GRADE_NONE;
        {
            std::lock_guard<std::mutex> guard(sThreatsLock);
            auto it = sTelegraphs.find(key);
            if (it == sTelegraphs.end() || it->second.generation != generation || !it->second.winding)
                return;
            startedAt = it->second.startedAt;
            grade = it->second.grade;
        }
        if (!creature.IsAlive() || !creature.IsInCombat() || creature.IsCrowdControlled() || Commanded(creature))
        {
            BreakOff(creature, key, generation);
            return;
        }
        uint32 const windUp = GRADES[grade].windUpMs;
        uint32 const gone = WorldTimer::getMSTimeDiff(startedAt, WorldTimer::getMSTime());
        if (gone >= windUp)
            Detonate(creature, key, generation);
        else
            Schedule(creature, key, generation, std::min(WATCH_MS, windUp - gone), true);
    }

    // Time for a wind-up: begin one, or wait a little and look again.
    void WindUp(Creature& creature, Key key, uint32 generation)
    {
        if (!creature.IsAlive() || !creature.IsInCombat() || Commanded(creature))
        {
            std::lock_guard<std::mutex> guard(sThreatsLock);
            auto it = sTelegraphs.find(key);
            if (it != sTelegraphs.end() && it->second.generation == generation)
                sTelegraphs.erase(it);
            return;
        }
        Player* foe = FoeOf(creature);
        if (!foe)
        {
            Schedule(creature, key, generation, IDLE_RETRY_MS, false);
            return;
        }
        if (creature.IsCrowdControlled() || creature.IsNonMeleeSpellCasted(false))
        {
            Schedule(creature, key, generation, RETRY_MS, false);
            return;
        }
        Telegraph t;
        {
            std::lock_guard<std::mutex> guard(sThreatsLock);
            auto it = sTelegraphs.find(key);
            if (it == sTelegraphs.end() || it->second.generation != generation || it->second.winding)
                return;
            Telegraph& live = it->second;
            GradeSpec const& g = GRADES[live.grade];
            uint8 const shape = ShapeFor(creature, live);
            float const dist = creature.GetDistance(foe);
            float const radius = shape == SHAPE_RING ? g.ring : shape == SHAPE_CONE ? g.cone : g.blast;
            bool const inReach = shape == SHAPE_BLAST ? dist <= BLAST_RANGE : dist <= radius + WIND_UP_SLACK;
            if (!inReach)
            {
                // Being kited: a boss's turn passes to its next shape.
                t.grade = GRADE_NONE;
            }
            else
            {
                live.winding = true;
                live.startedAt = WorldTimer::getMSTime();
                live.serial = ++sSerial;
                live.shape = shape;
                live.radius = radius;
                Unit const* centre = shape == SHAPE_BLAST ? static_cast<Unit const*>(foe) : &creature;
                live.x = centre->GetPositionX();
                live.y = centre->GetPositionY();
                live.z = centre->GetPositionZ();
                live.o = shape == SHAPE_CONE ? creature.GetAngle(foe) : 0.0f;
                t = live;
            }
        }
        if (t.grade == GRADE_NONE)
        {
            Schedule(creature, key, generation, RETRY_MS, false);
            return;
        }
        Freeze(creature, t);
        {
            // What the freeze took, for the thaw.
            std::lock_guard<std::mutex> guard(sThreatsLock);
            auto it = sTelegraphs.find(key);
            if (it != sTelegraphs.end() && it->second.generation == generation)
            {
                it->second.hadMovement = t.hadMovement;
                it->second.hadMelee = t.hadMelee;
                it->second.froze = t.froze;
                it->second.frozenAi = t.frozenAi;
            }
        }
        creature.HandleEmoteCommand(t.shape == SHAPE_BLAST ? EMOTE_ONESHOT_SPELLPRECAST : EMOTE_ONESHOT_BATTLEROAR);
        Send(creature, TELEGRAPH_WIND_UP, t);
        Schedule(creature, key, generation, std::min(WATCH_MS, GRADES[t.grade].windUpMs), true);
    }

    void Schedule(Creature& creature, Key key, uint32 generation, uint32 delay, bool watch)
    {
        {
            std::lock_guard<std::mutex> guard(sThreatsLock);
            auto it = sTelegraphs.find(key);
            if (it == sTelegraphs.end() || it->second.generation != generation)
                return;
            it->second.lastTick = WorldTimer::getMSTime();
        }
        creature.m_events.AddEvent(new UnitLambdaEvent(creature, [key, generation, watch](Unit& unit)
        {
            Creature& c = static_cast<Creature&>(unit);
            if (!c.IsInWorld())
                return;
            if (watch)
                Watch(c, key, generation);
            else
                WindUp(c, key, generation);
        }), creature.m_events.CalculateTime(delay));
    }

    // --- Bolts ---

    // The distance from (px, py) to the segment from (ax, ay) to (bx, by).
    float SegmentDistance(float px, float py, float ax, float ay, float bx, float by)
    {
        float const vx = bx - ax, vy = by - ay;
        float const len2 = vx * vx + vy * vy;
        float s = len2 > 0.0f ? ((px - ax) * vx + (py - ay) * vy) / len2 : 0.0f;
        s = std::max(0.0f, std::min(1.0f, s));
        float const dx = px - (ax + vx * s), dy = py - (ay + vy * s);
        return std::sqrt(dx * dx + dy * dy);
    }
}

namespace Arpg
{
    bool AimsBolt(WorldObject const* caster, Unit const* target, SpellEntry const* spell)
    {
        if (!caster || !target || !spell || !sWorld.getConfig(CONFIG_BOOL_ARPG_ENABLE))
            return false;
        if (target->GetTypeId() != TYPEID_PLAYER || !Active(target))
            return false;
        if (caster->GetTypeId() != TYPEID_UNIT)
            return false;
        Unit const* unit = static_cast<Unit const*>(caster);
        if (unit->IsPlayerControlled() || unit->GetOwnerGuid())
            return false;
        return !IsAreaOfEffectSpell(spell) && !IsPositiveSpell(spell, caster, target);
    }

    SpellMissInfo BoltOutcome(Unit const* target, float fromX, float fromY, float aimX, float aimY, float aimZ)
    {
        if (Dodging(target))
            return SPELL_MISS_DODGE;
        if (std::fabs(target->GetPositionZ() - aimZ) > HEIGHT_REACH)
            return SPELL_MISS_MISS;
        float const off = SegmentDistance(target->GetPositionX(), target->GetPositionY(), fromX, fromY, aimX, aimY);
        return off <= BOLT_RADIUS + target->GetObjectBoundingRadius() ? SPELL_MISS_NONE : SPELL_MISS_MISS;
    }

    void OnTelegraphAggro(Creature* creature, Unit* enemy)
    {
        if (!creature || !enemy || !sWorld.getConfig(CONFIG_BOOL_ARPG_ENABLE) || !creature->IsInWorld() ||
                !creature->IsAlive())
            return;
        Player* player = enemy->GetBeneficiaryPlayer();
        if (!player || !Active(player))
            return;
        Key const key = KeyOf(creature);
        // A chain already running for this fight: nothing to do (the cheap test, every hit).
        auto running = [&key]()
        {
            auto it = sTelegraphs.find(key);
            return it != sTelegraphs.end() && WorldTimer::getMSTimeDiff(it->second.lastTick, WorldTimer::getMSTime()) < STALE_MS;
        };
        {
            std::lock_guard<std::mutex> guard(sThreatsLock);
            if (running())
                return;
        }
        uint8 const grade = GradeOf(creature);
        if (grade == GRADE_NONE)
            return;
        uint32 generation;
        {
            std::lock_guard<std::mutex> guard(sThreatsLock);
            if (running())
                return;
            // A stale record (its chain stopped without a word) is replaced.
            Telegraph& t = sTelegraphs[key];
            t = Telegraph();
            t.generation = generation = ++sGeneration;
            t.grade = grade;
            t.cycle = uint8(urand(0, 2));
        }
        Schedule(*creature, key, generation, GRADES[grade].firstMs, false);
    }

    // The packet to the ARPG players near `source`.
    void SendMark(WorldObject const* source, uint8 kind, uint32 serial, uint8 shape, uint8 grade, float x, float y,
                  float z, float orientation, float radius, uint32 windUpMs)
    {
        WorldPacket data(SMSG_ARPG_TELEGRAPH, 1 + 4 + 8 + 1 + 1 + 4 * 7);
        data << uint8(kind) << uint32(serial) << source->GetObjectGuid() << uint8(shape) << uint8(grade);
        data << x << y << z << orientation << radius;
        data << float(shape == SHAPE_CONE ? CONE_HALF_ANGLE : 0.0f);
        data << uint32(windUpMs);
        for (auto const& ref : source->GetMap()->GetPlayers())
        {
            Player* player = ref.getSource();
            if (player && player->IsInWorld() && Active(player) && source->IsWithinDist(player, SEND_RANGE))
                player->GetSession()->SendPacket(data);
        }
    }

    uint32 ShowTelegraph(WorldObject const* source, uint8 shape, uint8 grade, float x, float y, float z,
                         float orientation, float radius, uint32 windUpMs)
    {
        if (!source || !source->IsInWorld())
            return 0;
        uint32 serial;
        {
            std::lock_guard<std::mutex> guard(sThreatsLock);
            serial = ++sSerial;
        }
        SendMark(source, TELEGRAPH_WIND_UP, serial, shape, grade, x, y, z, orientation, radius, windUpMs);
        return serial;
    }

    void HideTelegraph(WorldObject const* source, uint32 serial)
    {
        if (source && source->IsInWorld() && serial)
            SendMark(source, TELEGRAPH_BROKEN, serial, 0, 0, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0);
    }

    void ForgetTelegraph(Creature const* creature)
    {
        if (!creature)
            return;
        std::lock_guard<std::mutex> guard(sThreatsLock);
        sTelegraphs.erase(KeyOf(creature));
    }
}
