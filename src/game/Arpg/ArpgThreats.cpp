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
#include "Grids/CellImpl.h"
#include "Grids/GridNotifiers.h"
#include "Grids/GridNotifiersImpl.h"
#include "MotionGenerators/MotionMaster.h"
#include "Maps/Map.h"
#include "Server/Opcodes.h"
#include "Server/WorldPacket.h"
#include "Server/WorldSession.h"
#include "Spells/SpellMgr.h"
#include "Util/Timer.h"
#include "World/World.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <list>
#include <map>
#include <mutex>
#include <tuple>
#include <vector>

namespace
{
    using namespace Arpg;

    // --- Telegraphs ---

    enum Shape : uint8 { SHAPE_NONE = 0, SHAPE_RING = 1, SHAPE_CONE = 2, SHAPE_BLAST = 3, SHAPE_LINE = 4 };
    // GRADE_MINOR: an ordinary creature's move (its family's, below), not a heavy attack.
    enum Grade : uint8 { GRADE_ELITE, GRADE_CHAMPION, GRADE_RARE, GRADE_BOSS, GRADE_RAID_BOSS, GRADE_MINOR, GRADE_NONE };
    // What a move does besides its damage.
    enum MoveEffect : uint8 { EFFECT_NONE, EFFECT_LEAP, EFFECT_SLOW, EFFECT_BACK_OFF };

    // One attack, wound up and landed: its shape and size (a radius, or a line's length), its
    // width (a cone's half angle in radians, a line's half width in yards), its wind-up, its
    // damage as a share of the player's maximum health, the spell that names it in the combat log
    // (and its school), what else it does, and how near the foe must be for it to start (a blast's
    // range; 0, the shape's own reach).
    struct Attack
    {
        uint8 shape = SHAPE_NONE;
        float size = 0.0f, width = 0.0f;
        uint32 windUpMs = 0;
        uint32 healthPct = 0;
        uint32 spellId = 0;
        SpellSchoolMask school = SPELL_SCHOOL_MASK_NORMAL;
        uint8 effect = EFFECT_NONE;
        float reach = 0.0f;
    };
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
    constexpr uint32 SPELL_POUNCE = 9005, SPELL_CHARGE = 100, SPELL_MAUL = 6807, SPELL_THUNDER_CLAP = 6343;
    constexpr uint32 SPELL_FIRE_BLAST = 2136, SPELL_SINISTER_STRIKE = 1752, SPELL_STING = 3043, SPELL_WEB = 745;
    // The slow a web leaves (Chilled: 50% movement, a few seconds).
    constexpr uint32 SPELL_SLOWED = 12484;

    // --- Ordinary creatures' moves ---
    // Every creature an ARPG player fights that has no heavy attack (no elite, champion or boss)
    // has one move by what it is: a beast by its family, a humanoid or an undead by its class, a
    // kobold by name. Short wind-ups, a tenth or so of the player's health, a few seconds apart:
    // a pack of five keeps the player moving. Casters back away instead and fight with their own
    // (now dodgeable) spells.
    constexpr float LUNGE_HALF_WIDTH = 1.2f;
    constexpr float CHARGE_SPEED = 22.0f;
    constexpr uint32 MOVE_FIRST_MIN = 3000, MOVE_FIRST_MAX = 7000;
    constexpr uint32 MOVE_EVERY_MIN = 8000, MOVE_EVERY_MAX = 13000;
    // A caster backs away from an ARPG player this close, this far, at this pace.
    constexpr float BACK_OFF_NEAR = 6.0f, BACK_OFF_FAR = 9.0f;
    constexpr uint32 BACK_OFF_EVERY_MIN = 5000, BACK_OFF_EVERY_MAX = 8000;
    // Murlocs come to a fight: this far round the first one pulled.
    constexpr float SWARM_RANGE = 22.0f;

    Attack const MOVE_LUNGE   = { SHAPE_LINE,  8.0f,  LUNGE_HALF_WIDTH, 700, 10, SPELL_POUNCE, SPELL_SCHOOL_MASK_NORMAL, EFFECT_LEAP, 9.0f };
    Attack const MOVE_CHARGE  = { SHAPE_LINE,  12.0f, 1.5f, 900, 12, SPELL_CHARGE, SPELL_SCHOOL_MASK_NORMAL, EFFECT_LEAP, 13.0f };
    Attack const MOVE_MAUL    = { SHAPE_CONE,  5.0f,  float(M_PI) / 3.0f, 800, 12, SPELL_MAUL, SPELL_SCHOOL_MASK_NORMAL, EFFECT_NONE, 0.0f };
    Attack const MOVE_SLAM    = { SHAPE_RING,  4.5f,  0.0f, 800, 10, SPELL_THUNDER_CLAP, SPELL_SCHOOL_MASK_NORMAL, EFFECT_NONE, 0.0f };
    Attack const MOVE_WEB     = { SHAPE_BLAST, 3.0f,  0.0f, 800, 6, SPELL_WEB, SPELL_SCHOOL_MASK_NATURE, EFFECT_SLOW, 25.0f };
    Attack const MOVE_STING   = { SHAPE_CONE,  4.5f,  float(M_PI) / 6.0f, 700, 10, SPELL_STING, SPELL_SCHOOL_MASK_NATURE, EFFECT_SLOW, 0.0f };
    Attack const MOVE_BOMB    = { SHAPE_BLAST, 3.0f,  0.0f, 1000, 10, SPELL_FIRE_BLAST, SPELL_SCHOOL_MASK_FIRE, EFFECT_NONE, 20.0f };
    Attack const MOVE_CLEAVE  = { SHAPE_CONE,  4.5f,  float(M_PI) / 3.0f, 800, 10, SPELL_CONE, SPELL_SCHOOL_MASK_NORMAL, EFFECT_NONE, 0.0f };
    Attack const MOVE_DASH    = { SHAPE_LINE,  7.0f,  1.0f, 600, 9, SPELL_SINISTER_STRIKE, SPELL_SCHOOL_MASK_NORMAL, EFFECT_LEAP, 8.0f };
    Attack const MOVE_BACKOFF = { SHAPE_NONE,  0.0f,  0.0f, 0, 0, 0, SPELL_SCHOOL_MASK_NORMAL, EFFECT_BACK_OFF, 0.0f };

    bool NameHas(Creature const& creature, char const* word)
    {
        return creature.GetName() && std::strstr(creature.GetName(), word) != nullptr;
    }

    // The move of an ordinary creature, or none (a critter, a totem, a mechanical).
    Attack const* MoveOf(Creature const& creature)
    {
        CreatureInfo const* info = creature.GetCreatureInfo();
        if (!info)
            return nullptr;
        if (NameHas(creature, "Kobold"))
            return &MOVE_BOMB;
        switch (info->CreatureType)
        {
            case CREATURE_TYPE_BEAST:
                switch (info->Family)
                {
                    case CREATURE_FAMILY_WOLF:
                    case CREATURE_FAMILY_CAT:
                    case CREATURE_FAMILY_RAPTOR:
                    case CREATURE_FAMILY_HYENA:
                    case CREATURE_FAMILY_CARRION_BIRD:
                    case CREATURE_FAMILY_BAT:
                    case CREATURE_FAMILY_OWL:
                    case CREATURE_FAMILY_WIND_SERPENT:
                        return &MOVE_LUNGE;
                    case CREATURE_FAMILY_BOAR:
                    case CREATURE_FAMILY_TALLSTRIDER:
                        return &MOVE_CHARGE;
                    case CREATURE_FAMILY_BEAR:
                    case CREATURE_FAMILY_GORILLA:
                    case CREATURE_FAMILY_CROCOLISK:
                        return &MOVE_MAUL;
                    case CREATURE_FAMILY_SPIDER:
                        return &MOVE_WEB;
                    case CREATURE_FAMILY_SCORPID:
                        return &MOVE_STING;
                    case CREATURE_FAMILY_CRAB:
                    case CREATURE_FAMILY_TURTLE:
                        return &MOVE_SLAM;
                    default:
                        return &MOVE_LUNGE;
                }
            case CREATURE_TYPE_HUMANOID:
            case CREATURE_TYPE_UNDEAD:
                if (creature.GetPowerType() == POWER_MANA && creature.GetMaxPower(POWER_MANA) > 0)
                    return &MOVE_BACKOFF;
                if (info->UnitClass == CLASS_ROGUE)
                    return &MOVE_DASH;
                return &MOVE_CLEAVE;
            case CREATURE_TYPE_DEMON:
            case CREATURE_TYPE_ELEMENTAL:
            case CREATURE_TYPE_GIANT:
            case CREATURE_TYPE_DRAGONKIN:
                return &MOVE_SLAM;
            default:
                return nullptr;
        }
    }

    // A heavy attack's: a boss cycles its shapes, a caster blasts, any other keeps one by entry.
    Attack HeavyAttack(Creature const& creature, uint8 grade, uint8& cycle)
    {
        GradeSpec const& g = GRADES[grade];
        uint8 shape;
        if (grade == GRADE_BOSS || grade == GRADE_RAID_BOSS)
            shape = uint8(SHAPE_RING + cycle++ % 3);
        else if (creature.GetPowerType() == POWER_MANA && creature.GetMaxPower(POWER_MANA) > 0)
            shape = SHAPE_BLAST;
        else
            shape = creature.GetEntry() % 2 ? SHAPE_CONE : SHAPE_RING;
        Attack a;
        a.shape = shape;
        a.size = shape == SHAPE_RING ? g.ring : shape == SHAPE_CONE ? g.cone : g.blast;
        a.width = shape == SHAPE_CONE ? CONE_HALF_ANGLE : 0.0f;
        a.windUpMs = g.windUpMs;
        a.healthPct = g.healthPct;
        a.spellId = shape == SHAPE_RING ? SPELL_RING : shape == SHAPE_CONE ? SPELL_CONE : SPELL_BLAST;
        a.school = shape == SHAPE_BLAST ? SPELL_SCHOOL_MASK_FIRE : SPELL_SCHOOL_MASK_NORMAL;
        a.reach = shape == SHAPE_BLAST ? BLAST_RANGE : 0.0f;
        return a;
    }

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
        uint32 everyMin = 0, everyMax = 0;  // the wait between attacks
        bool winding = false;
        // The wind-up in hand: the attack, and where (the centre, a cone's or a line's start, and
        // its facing).
        uint32 serial = 0;
        Attack attack;
        float x = 0.0f, y = 0.0f, z = 0.0f, o = 0.0f;
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

    uint32 NextDelay(Telegraph const& t)
    {
        return urand(t.everyMin, t.everyMax);
    }

    void Send(Creature& creature, uint8 kind, Telegraph const& t)
    {
        WorldPacket data(SMSG_ARPG_TELEGRAPH, 1 + 4 + 8 + 1 + 1 + 4 * 7);
        data << uint8(kind) << uint32(t.serial) << creature.GetObjectGuid() << uint8(t.attack.shape) << uint8(t.grade);
        data << t.x << t.y << t.z << t.o << t.attack.size;
        data << t.attack.width;
        data << uint32(t.attack.windUpMs);
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
        if (t.attack.shape == SHAPE_LINE)
        {
            // Along the line from its start, and across it.
            float const along = dx * std::cos(t.o) + dy * std::sin(t.o);
            float const across = -dx * std::sin(t.o) + dy * std::cos(t.o);
            return along >= -reach && along <= t.attack.size + reach && std::fabs(across) <= t.attack.width + reach;
        }
        float const dist = std::sqrt(dx * dx + dy * dy);
        if (dist > t.attack.size + reach)
            return false;
        if (t.attack.shape != SHAPE_CONE || dist < 0.5f)
            return true;
        float diff = std::atan2(dy, dx) - t.o;
        while (diff > float(M_PI))
            diff -= 2.0f * float(M_PI);
        while (diff < -float(M_PI))
            diff += 2.0f * float(M_PI);
        // The body's reach widens the cone a little at the edge.
        return std::fabs(diff) <= t.attack.width + std::atan2(reach, dist);
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
        if (t.attack.shape == SHAPE_CONE || t.attack.shape == SHAPE_LINE)
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
        if ((t.attack.shape == SHAPE_CONE || t.attack.shape == SHAPE_LINE) && creature.GetVictim())
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
            Schedule(creature, key, generation, NextDelay(t), false);
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
        Attack const& a = t.attack;
        creature.HandleEmoteCommand(a.shape == SHAPE_BLAST ? EMOTE_ONESHOT_SPELLCAST : EMOTE_ONESHOT_SPECIALATTACK1H);
        // A leap carries the creature down its line as it strikes.
        // Only a creature that moved in the fight before its wind-up (not a script, not one whose AI
        // holds it still); the end stops at the first wall or drop on the way.
        if (a.effect == EFFECT_LEAP && t.froze && t.hadMovement && !creature.hasUnitState(UNIT_STAT_NO_FREE_MOVE))
        {
            Position end;
            creature.GetFirstCollisionPosition(end, a.size, t.o);
            creature.GetMotionMaster()->MoveCharge(end.x, end.y, end.z, CHARGE_SPEED);
        }

        uint32 const spellId = a.spellId;
        SpellSchoolMask const school = a.school;
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
            uint32 damage = std::max<uint32>(1, player->GetMaxHealth() * a.healthPct / 100);
            uint32 absorb = 0;
            Unit::DealDamageMods(&creature, player, damage, &absorb, SPELL_DIRECT_DAMAGE, nullptr);
            Unit::SendSpellNonMeleeDamageLog(&creature, player, spellId, damage, school, absorb, 0, false, 0);
            if (damage)
                Unit::DealDamage(&creature, player, damage, nullptr, SPELL_DIRECT_DAMAGE, school, nullptr, false);
            if (a.effect == EFFECT_SLOW && player->IsAlive())
                creature.CastSpell(player, SPELL_SLOWED, TRIGGERED_OLD_TRIGGERED);
        }
        if (creature.IsAlive())
            Schedule(creature, key, generation, NextDelay(t), false);
    }

    // A wind-up in progress: land it on time, or break it off the moment the creature is
    // stunned, feared, confused, commanded, out of combat or dead.
    void Watch(Creature& creature, Key key, uint32 generation)
    {
        uint32 startedAt = 0;
        uint32 windUp = 0;
        {
            std::lock_guard<std::mutex> guard(sThreatsLock);
            auto it = sTelegraphs.find(key);
            if (it == sTelegraphs.end() || it->second.generation != generation || !it->second.winding)
                return;
            startedAt = it->second.startedAt;
            windUp = it->second.attack.windUpMs;
        }
        if (!creature.IsAlive() || !creature.IsInCombat() || creature.IsCrowdControlled() || Commanded(creature))
        {
            BreakOff(creature, key, generation);
            return;
        }
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
        bool backOff = false;
        {
            std::lock_guard<std::mutex> guard(sThreatsLock);
            auto it = sTelegraphs.find(key);
            if (it == sTelegraphs.end() || it->second.generation != generation || it->second.winding)
                return;
            Telegraph& live = it->second;
            Attack attack;
            if (live.grade == GRADE_MINOR)
            {
                if (Attack const* move = MoveOf(creature))
                    attack = *move;
            }
            else
                attack = HeavyAttack(creature, live.grade, live.cycle);
            float const dist = creature.GetDistance(foe);
            if (attack.effect == EFFECT_BACK_OFF)
                backOff = dist <= BACK_OFF_NEAR;
            else if (attack.shape != SHAPE_NONE)
            {
                float const reach = attack.reach > 0.0f ? attack.reach : attack.size + WIND_UP_SLACK;
                if (dist <= reach)
                {
                    live.winding = true;
                    live.startedAt = WorldTimer::getMSTime();
                    live.serial = ++sSerial;
                    live.attack = attack;
                    Unit const* centre = attack.shape == SHAPE_BLAST ? static_cast<Unit const*>(foe) : &creature;
                    live.x = centre->GetPositionX();
                    live.y = centre->GetPositionY();
                    live.z = centre->GetPositionZ();
                    live.o = attack.shape == SHAPE_CONE || attack.shape == SHAPE_LINE ? creature.GetAngle(foe) : 0.0f;
                    t = live;
                }
            }
        }
        if (backOff)
        {
            // A caster an ARPG player has closed on steps away to cast again.
            float const away = foe->GetAngle(&creature);
            UnitAI* ai = creature.AI();
            if (!creature.hasUnitState(UNIT_STAT_NO_FREE_MOVE) && !creature.GetScriptId() && ai && ai->IsCombatMovement())
            {
                // Stops at the first wall or drop behind it; a step too short to matter is skipped.
                Position end;
                creature.GetFirstCollisionPosition(end, BACK_OFF_FAR, away);
                if (creature.GetDistance2d(end.x, end.y, DIST_CALC_NONE) >= BACK_OFF_FAR * 0.4f)
                    creature.GetMotionMaster()->MoveCharge(end.x, end.y, end.z, creature.GetSpeed(MOVE_RUN));
            }
            Schedule(creature, key, generation, urand(BACK_OFF_EVERY_MIN, BACK_OFF_EVERY_MAX), false);
            return;
        }
        if (!t.winding)
        {
            // Out of reach (being kited), or no move: a boss's turn passes to its next shape.
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
        creature.HandleEmoteCommand(t.attack.shape == SHAPE_BLAST ? EMOTE_ONESHOT_SPELLPRECAST : EMOTE_ONESHOT_BATTLEROAR);
        Send(creature, TELEGRAPH_WIND_UP, t);
        Schedule(creature, key, generation, std::min(WATCH_MS, t.attack.windUpMs), true);
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
        uint8 grade = GradeOf(creature);
        if (grade == GRADE_NONE && MoveOf(*creature) && !creature->IsPet() && !creature->IsTotem() &&
                !creature->GetOwnerGuid() && !creature->IsPlayerControlled() && !creature->IsCritter())
            grade = GRADE_MINOR;
        if (grade == GRADE_NONE)
            return;
        uint32 generation, first;
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
            if (grade == GRADE_MINOR)
            {
                t.everyMin = MOVE_EVERY_MIN;
                t.everyMax = MOVE_EVERY_MAX;
                first = urand(MOVE_FIRST_MIN, MOVE_FIRST_MAX);
            }
            else
            {
                t.everyMin = GRADES[grade].everyMin;
                t.everyMax = GRADES[grade].everyMax;
                first = GRADES[grade].firstMs;
            }
        }
        // Murlocs come running: the rest within reach join the fight.
        // One pull at a time: each murloc it brings in would otherwise call its own (the camp chains).
        static thread_local bool pulling = false;
        if (!pulling && NameHas(*creature, "Murloc"))
        {
            pulling = true;
            std::list<Creature*> kin;
            MaNGOS::AnyAssistCreatureInRangeCheck check(creature, enemy, SWARM_RANGE);
            MaNGOS::CreatureListSearcher<MaNGOS::AnyAssistCreatureInRangeCheck> searcher(kin, check);
            Cell::VisitGridObjects(creature, searcher, SWARM_RANGE);
            for (Creature* other : kin)
                if (other != creature && other->IsAlive() && !other->IsInCombat() && NameHas(*other, "Murloc") && other->AI())
                    other->AI()->AttackStart(enemy);
            pulling = false;
        }
        Schedule(*creature, key, generation, first, false);
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
