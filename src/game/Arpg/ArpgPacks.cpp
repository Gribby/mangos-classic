/*
 * ARPG mob packs: see ArpgPacks.h.
 */

#include "Arpg/ArpgPacks.h"
#include "Arpg/ArpgCombat.h"

#include "AI/BaseAI/CreatureAI.h"
#include "Entities/Creature.h"
#include "Entities/Player.h"
#include "Log/Log.h"
#include "Loot/LootMgr.h"
#include "Maps/Map.h"
#include "MotionGenerators/MotionMaster.h"
#include "Server/DBCStores.h"
#include "Util/Timer.h"
#include "World/World.h"

#include <cmath>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    using namespace Arpg;

    // A follower's corpse lies this long, in ms, so its ground loot can be picked up.
    constexpr uint32 PACK_CORPSE_MS = 3 * MINUTE * IN_MILLISECONDS;
    // Followers stand this far from their leader, in yards, and are drawn this much smaller.
    constexpr float FOLLOW_MIN = 2.5f, FOLLOW_SPREAD = 1.5f;
    constexpr float FOLLOWER_SCALE = 0.85f;
    // Followers wander this far from where they appeared, if their leader wanders.
    constexpr float FOLLOWER_WANDER = 3.0f;
    // A pack member this far from the one pulled doesn't join in.
    constexpr float AGGRO_REACH = 40.0f;
    // Formation waits this long after the spawn, in ms, so the grid has finished loading.
    constexpr uint32 FORM_DELAY_MIN = 500, FORM_DELAY_MAX = 1500;

    struct Pack
    {
        std::vector<ObjectGuid> followers;
        uint32 level = 0;
        uint32 aggroAt = 0;     // the first aggro of this fight, 0 out of combat
        uint32 damage = 0;      // what the pack dealt this fight
    };

    std::mutex sPacksLock;
    std::unordered_map<ObjectGuid, Pack> sPacks;            // by leader
    std::unordered_map<ObjectGuid, ObjectGuid> sLeaderOf;   // follower and leader alike -> leader
    std::unordered_set<ObjectGuid> sFollowers;
    std::unordered_set<ObjectGuid> sForming;                // leaders with a formation pending

    bool PacksOn()
    {
        return sWorld.getConfig(CONFIG_BOOL_ARPG_ENABLE) && sWorld.getConfig(CONFIG_BOOL_ARPG_PACKS);
    }

    // The pack's size, leader included, for a leader of `level`.
    std::pair<uint32, uint32> SizeFor(uint32 level)
    {
        if (level <= 5)
            return { 1, 2 };
        if (level <= 11)
            return { 2, 3 };
        if (level <= 19)
            return { 3, 4 };
        return { 3, 5 };
    }

    bool Eligible(Creature* creature)
    {
        if (!creature || !creature->IsInWorld() || !creature->IsAlive() || creature->IsInCombat())
            return false;
        if (creature->GetSubtype() != CREATURE_SUBTYPE_GENERIC || !creature->HasStaticDBSpawnData() ||
                creature->IsPet() || creature->IsTotem() || creature->GetCreatureGroup())
            return false;
        if (creature->GetMap()->Instanceable())
            return false;
        CreatureInfo const* info = creature->GetCreatureInfo();
        if (!info || info->Rank != CREATURE_ELITE_NORMAL || info->NpcFlags || creature->IsCivilian() ||
                info->CreatureType == CREATURE_TYPE_CRITTER || creature->IsNoXp())
            return false;
        FactionTemplateEntry const* faction = creature->GetFactionTemplateEntry();
        return faction && (faction->IsHostileToPlayers() || faction->IsNeutralToAll());
    }

    // Send away the followers that aren't fighting, and forget the pack.
    void Disband(Creature* leader)
    {
        std::vector<ObjectGuid> followers;
        {
            std::lock_guard<std::mutex> guard(sPacksLock);
            auto it = sPacks.find(leader->GetObjectGuid());
            if (it == sPacks.end())
                return;
            followers = it->second.followers;
            sPacks.erase(it);
            sLeaderOf.erase(leader->GetObjectGuid());
            for (ObjectGuid const& guid : followers)
            {
                sLeaderOf.erase(guid);
                sFollowers.erase(guid);
            }
        }
        for (ObjectGuid const& guid : followers)
            if (Creature* follower = leader->GetMap()->GetCreature(guid))
                if (!follower->IsInCombat())
                    follower->ForcedDespawn();
    }

    void FormPack(Creature* leader, uint32 size)
    {
        {
            std::lock_guard<std::mutex> guard(sPacksLock);
            sForming.erase(leader->GetObjectGuid());
        }
        if (!PacksOn() || !Eligible(leader))
            return;
        Disband(leader);

        uint32 const level = leader->GetLevel();
        if (!size)
        {
            auto const [lo, hi] = SizeFor(level);
            size = urand(lo, hi);
        }
        if (size < 2)
            return;
        uint32 const count = size - 1;
        float const power = std::max(0.05f, sWorld.getConfig(CONFIG_FLOAT_ARPG_PACK_FOLLOWER_POWER));

        Pack pack;
        pack.level = level;
        for (uint32 i = 0; i < count; ++i)
        {
            // Behind and around the leader, fanned out.
            float const fan = (float(i) - float(count - 1) / 2.0f) * 0.9f;
            float const angle = leader->GetOrientation() + float(M_PI) + fan;
            float const dist = FOLLOW_MIN + float(rand_norm()) * FOLLOW_SPREAD;
            float x, y, z;
            leader->GetNearPoint(leader, x, y, z, leader->GetObjectBoundingRadius(), dist, angle);
            Creature* follower = leader->SummonCreature(leader->GetEntry(), x, y, z, leader->GetOrientation(),
                                 TEMPSPAWN_CORPSE_TIMED_DESPAWN, PACK_CORPSE_MS);
            if (!follower)
                continue;
            follower->SelectLevel(std::max<uint32>(1, level - std::min<uint32>(level - 1, urand(1, 2))));
            follower->SetMaxHealth(std::max<uint32>(1, uint32(follower->GetMaxHealth() * power)));
            follower->SetHealth(follower->GetMaxHealth());
            follower->SetObjectScale(follower->GetObjectScale() * FOLLOWER_SCALE);
            switch (leader->GetDefaultMovementType())
            {
                case WAYPOINT_MOTION_TYPE:
                    follower->GetMotionMaster()->MoveFollow(leader, dist, float(M_PI) + fan);
                    break;
                case RANDOM_MOTION_TYPE:
                    follower->GetMotionMaster()->MoveRandomAroundPoint(x, y, z, FOLLOWER_WANDER);
                    break;
                default:
                    break;
            }
            pack.followers.push_back(follower->GetObjectGuid());
        }
        if (pack.followers.empty())
            return;
        std::lock_guard<std::mutex> guard(sPacksLock);
        ObjectGuid const leaderGuid = leader->GetObjectGuid();
        sLeaderOf[leaderGuid] = leaderGuid;
        for (ObjectGuid const& guid : pack.followers)
        {
            sLeaderOf[guid] = leaderGuid;
            sFollowers.insert(guid);
        }
        sPacks[leaderGuid] = std::move(pack);
    }

    // The leader of `unit`'s pack and its members (leader first), if it is in one.
    bool PackOf(Unit const* unit, ObjectGuid& leader, std::vector<ObjectGuid>& members)
    {
        std::lock_guard<std::mutex> guard(sPacksLock);
        auto it = sLeaderOf.find(unit->GetObjectGuid());
        if (it == sLeaderOf.end())
            return false;
        leader = it->second;
        auto pack = sPacks.find(leader);
        if (pack == sPacks.end())
            return false;
        members.push_back(leader);
        members.insert(members.end(), pack->second.followers.begin(), pack->second.followers.end());
        return true;
    }

    bool MaybeFollower(Unit const* unit)
    {
        return unit && unit->GetTypeId() == TYPEID_UNIT &&
               static_cast<Creature const*>(unit)->GetSubtype() == CREATURE_SUBTYPE_TEMPORARY_SUMMON;
    }
}

namespace Arpg
{
    void OnCreatureAdded(Creature* creature)
    {
        if (!PacksOn() || !creature || creature->GetSubtype() != CREATURE_SUBTYPE_GENERIC ||
                !creature->HasStaticDBSpawnData() || creature->GetMap()->Instanceable())
            return;
        {
            std::lock_guard<std::mutex> guard(sPacksLock);
            if (!sForming.insert(creature->GetObjectGuid()).second)
                return;
        }
        creature->m_events.AddEvent(new UnitLambdaEvent(*creature, [](Unit& unit)
        {
            FormPack(static_cast<Creature*>(&unit), 0);
        }), creature->m_events.CalculateTime(urand(FORM_DELAY_MIN, FORM_DELAY_MAX)));
    }

    bool IsPackFollower(Unit const* unit)
    {
        if (!MaybeFollower(unit))
            return false;
        std::lock_guard<std::mutex> guard(sPacksLock);
        return sFollowers.count(unit->GetObjectGuid()) != 0;
    }

    float PackDamageMod(Unit const* attacker)
    {
        return IsPackFollower(attacker) ? sWorld.getConfig(CONFIG_FLOAT_ARPG_PACK_FOLLOWER_POWER) : 1.0f;
    }

    float PackXpMod(Unit const* victim)
    {
        return IsPackFollower(victim) ? sWorld.getConfig(CONFIG_FLOAT_ARPG_PACK_FOLLOWER_XP) : 1.0f;
    }

    void ThinPackLoot(Creature* victim)
    {
        if (victim && victim->m_loot && IsPackFollower(victim))
            victim->m_loot->ThinArpgLoot(sWorld.getConfig(CONFIG_FLOAT_ARPG_PACK_FOLLOWER_LOOT));
    }

    void OnPackAggro(Creature* member, Unit* enemy)
    {
        // Each one pulled pulls the rest; the first is enough.
        static thread_local bool pulling = false;
        if (pulling || !member || !enemy)
            return;
        ObjectGuid leader;
        std::vector<ObjectGuid> members;
        if (!PackOf(member, leader, members))
            return;
        {
            std::lock_guard<std::mutex> guard(sPacksLock);
            auto it = sPacks.find(leader);
            if (it != sPacks.end() && !it->second.aggroAt)
            {
                it->second.aggroAt = WorldTimer::getMSTime();
                it->second.damage = 0;
            }
        }
        pulling = true;
        for (ObjectGuid const& guid : members)
        {
            Creature* other = member->GetMap()->GetCreature(guid);
            if (!other || other == member || !other->IsAlive() || other->IsInCombat() || !other->AI() ||
                    !other->IsWithinDistInMap(member, AGGRO_REACH))
                continue;
            other->AI()->AttackStart(enemy);
        }
        pulling = false;
    }

    void OnPackDamage(Unit* attacker, Unit* victim, uint32 damage)
    {
        if (!attacker || !damage || attacker->GetTypeId() != TYPEID_UNIT || !victim || !victim->IsPlayer())
            return;
        std::lock_guard<std::mutex> guard(sPacksLock);
        auto it = sLeaderOf.find(attacker->GetObjectGuid());
        if (it == sLeaderOf.end())
            return;
        auto pack = sPacks.find(it->second);
        if (pack != sPacks.end())
            pack->second.damage += damage;
    }

    void OnPackMemberDied(Unit* victim)
    {
        if (!victim || victim->GetTypeId() != TYPEID_UNIT)
            return;
        ObjectGuid leader;
        std::vector<ObjectGuid> members;
        if (!PackOf(victim, leader, members))
            return;
        for (ObjectGuid const& guid : members)
            if (Creature* other = victim->GetMap()->GetCreature(guid))
                if (other != victim && other->IsAlive())
                    return;
        std::lock_guard<std::mutex> guard(sPacksLock);
        auto it = sPacks.find(leader);
        if (it == sPacks.end() || !it->second.aggroAt)
            return;
        float const seconds = WorldTimer::getMSTimeDiff(it->second.aggroAt, WorldTimer::getMSTime()) / 1000.0f;
        sLog.outString("ARPG packs: a pack of %u %s (level %u) fell in %.1f s; it dealt %u damage",
                       uint32(members.size()), victim->GetName(), it->second.level, seconds, it->second.damage);
        it->second.aggroAt = 0;
        it->second.damage = 0;
    }

    void DevFormPack(Player* player, uint8 size)
    {
        if (!sWorld.getConfig(CONFIG_BOOL_ARPG_DEV_TOOLS) || !player)
            return;
        Creature* nearest = nullptr;
        float nearestDist = AGGRO_REACH;
        for (Unit* unit : EnemiesNear(player, AGGRO_REACH))
        {
            if (unit->GetTypeId() != TYPEID_UNIT || !Eligible(static_cast<Creature*>(unit)))
                continue;
            float const dist = player->GetDistance(unit);
            if (dist < nearestDist)
            {
                nearest = static_cast<Creature*>(unit);
                nearestDist = dist;
            }
        }
        if (!nearest)
        {
            sLog.outString("ARPG packs: %s asked for a test pack, but no mob near can lead one", player->GetName());
            return;
        }
        if (!PacksOn())
        {
            sLog.outString("ARPG packs: %s asked for a test pack, but Arpg.Packs is off", player->GetName());
            return;
        }
        FormPack(nearest, std::min<uint32>(size, 8));
        sLog.outString("ARPG packs: %s formed a test pack round %s", player->GetName(), nearest->GetName());
    }
}
