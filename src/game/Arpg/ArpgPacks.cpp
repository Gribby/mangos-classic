/*
 * ARPG mob packs: see ArpgPacks.h.
 */

#include "Arpg/ArpgPacks.h"
#include "Arpg/ArpgCombat.h"
#include "Arpg/ArpgLoot.h"
#include "Arpg/ArpgUniques.h"

#include "AI/BaseAI/CreatureAI.h"
#include "Entities/Creature.h"
#include "Entities/Item.h"
#include "Entities/Player.h"
#include "Log/Log.h"
#include "Loot/LootMgr.h"
#include "Maps/Map.h"
#include "MotionGenerators/MotionMaster.h"
#include "Server/DBCStores.h"
#include "Server/Opcodes.h"
#include "Server/WorldPacket.h"
#include "Server/WorldSession.h"
#include "Spells/SpellMgr.h"
#include "Util/Timer.h"
#include "World/World.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <string>
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

    // --- Champions and rares ---

    enum Tier : uint8 { TIER_NONE = 0, TIER_CHAMPION = 1, TIER_RARE = 2 };

    enum Affix : uint8
    {
        AFFIX_EXTRA_STRONG,
        AFFIX_EXTRA_FAST,
        AFFIX_STONE_SKIN,
        AFFIX_FIRE_ENCHANTED,
        AFFIX_COLD_ENCHANTED,
        AFFIX_VAMPIRIC,
        AFFIX_THORNS,
        AFFIX_TELEPORTER,
        AFFIX_HEALER,
        MAX_AFFIX
    };

    char const* const AFFIX_NAME[MAX_AFFIX] =
    {
        "Extra Strong", "Extra Fast", "Stone Skin", "Fire Enchanted", "Cold Enchanted", "Vampiric",
        "Thorns", "Teleporter", "Healer",
    };
    // A rare's epithet, by its first affix.
    char const* const AFFIX_EPITHET[MAX_AFFIX] =
    {
        "the Mighty", "the Swift", "the Unbroken", "the Burning", "the Frozen", "the Bloodthirsty",
        "the Thorned", "the Phantom", "the Mender",
    };
    char const* const NAME_HEAD[] =
    {
        "Gore", "Skull", "Ash", "Blight", "Grim", "Rot", "Storm", "Bone", "Blood", "Dread", "Gloom", "Rust",
        "Grave", "Thorn", "Murk", "Cinder",
    };
    char const* const NAME_TAIL[] =
    {
        "maw", "fang", "hide", "claw", "howl", "spine", "tooth", "gut", "rend", "shank", "shriek", "grin",
        "jaw", "brand", "scar", "mire",
    };

    constexpr uint32 CHAMPION_MIN_LEVEL = 8;
    constexpr uint32 RARE_PER_MILLE = 25, CHAMPION_PER_MILLE = 125;
    constexpr float CHAMPION_HEALTH = 3.0f, RARE_HEALTH = 4.0f;
    constexpr float CHAMPION_DAMAGE = 1.3f, RARE_DAMAGE = 1.5f;
    constexpr float CHAMPION_SCALE = 1.2f, RARE_SCALE = 1.3f;
    constexpr float CHAMPION_XP = 3.0f, RARE_XP = 5.0f;
    constexpr float STRONG_DAMAGE = 1.5f, STONE_TAKEN = 0.6f, FAST_SPEED = 1.3f, FAST_HASTE = 33.0f;
    constexpr uint32 FIRE_SHARE_PCT = 25, FIRE_BURST_PCT = 8, FIRE_BURST_MS = 1000;
    constexpr float FIRE_BURST_RANGE = 6.0f;
    constexpr uint32 VAMPIRIC_PCT = 50, THORNS_PCT = 20;
    constexpr float TELEPORT_REACH = 12.0f;
    constexpr uint32 TELEPORT_EVERY = 6000, HEAL_EVERY = 4000, HEAL_PCT = 8, TICK_MS = 1000;
    constexpr float HEAL_RANGE = 20.0f;
    constexpr float NOVA_AT = 0.3f;
    constexpr float SEND_RANGE = 100.0f;
    constexpr uint32 SPELL_FIRE_BLAST = 2136, SPELL_CHILLED = 12484, SPELL_FROST_NOVA = 122, SPELL_THORNS = 467;

    struct Champion
    {
        Tier tier = TIER_NONE;
        std::string name;
        std::vector<Affix> affixes;
        uint32 lastTeleport = 0;
        uint32 lastHeal = 0;
        bool novaDone = false;

        bool Has(Affix affix) const { return std::find(affixes.begin(), affixes.end(), affix) != affixes.end(); }
    };

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
    std::unordered_map<ObjectGuid, Champion> sChampions;
    std::unordered_map<ObjectGuid, std::unordered_set<ObjectGuid>> sChampionsSent; // by player

    // A copy of `unit`'s champion record; tier TIER_NONE for an ordinary mob.
    Champion ChampionOf(Unit const* unit)
    {
        if (!unit || unit->GetTypeId() != TYPEID_UNIT ||
                static_cast<Creature const*>(unit)->GetSubtype() != CREATURE_SUBTYPE_TEMPORARY_SUMMON)
            return Champion();
        std::lock_guard<std::mutex> guard(sPacksLock);
        auto it = sChampions.find(unit->GetObjectGuid());
        return it == sChampions.end() ? Champion() : it->second;
    }

    std::vector<Affix> RollAffixes(uint32 count)
    {
        std::vector<Affix> all;
        for (uint8 a = 0; a < MAX_AFFIX; ++a)
            all.push_back(Affix(a));
        std::vector<Affix> out;
        while (out.size() < count && !all.empty())
        {
            size_t const i = urand(0, uint32(all.size() - 1));
            out.push_back(all[i]);
            all.erase(all.begin() + i);
        }
        return out;
    }

    std::string RareName(std::vector<Affix> const& affixes)
    {
        std::string name = NAME_HEAD[urand(0, uint32(std::size(NAME_HEAD) - 1))];
        name += NAME_TAIL[urand(0, uint32(std::size(NAME_TAIL) - 1))];
        if (!affixes.empty())
            name += std::string(" ") + AFFIX_EPITHET[affixes.front()];
        return name;
    }

    void ChampionTick(Creature* champion);

    // Make `creature` a champion or a rare.
    void Crown(Creature* creature, Tier tier, std::vector<Affix> affixes, std::string name)
    {
        float const health = tier == TIER_RARE ? RARE_HEALTH : CHAMPION_HEALTH;
        creature->SetMaxHealth(std::max<uint32>(1, uint32(creature->GetMaxHealth() * health)));
        creature->SetHealth(creature->GetMaxHealth());
        creature->SetObjectScale(creature->GetObjectScale() * (tier == TIER_RARE ? RARE_SCALE : CHAMPION_SCALE));
        Champion c;
        c.tier = tier;
        c.name = std::move(name);
        c.affixes = std::move(affixes);
        bool const fast = c.Has(AFFIX_EXTRA_FAST);
        {
            std::lock_guard<std::mutex> guard(sPacksLock);
            sChampions[creature->GetObjectGuid()] = std::move(c);
        }
        if (fast)
        {
            creature->ApplyAttackTimePercentMod(BASE_ATTACK, FAST_HASTE, true);
            creature->UpdateSpeed(MOVE_RUN, true);
        }
        creature->m_events.AddEvent(new UnitLambdaEvent(*creature, [](Unit& unit)
        {
            ChampionTick(static_cast<Creature*>(&unit));
        }), creature->m_events.CalculateTime(TICK_MS));
    }

    // Once a second while it lives: Teleporter, Healer, Cold Enchanted's nova.
    void ChampionTick(Creature* champion)
    {
        if (!champion->IsInWorld() || !champion->IsAlive())
            return;
        Champion const c = ChampionOf(champion);
        if (c.tier == TIER_NONE)
            return;
        uint32 const now = WorldTimer::getMSTime();
        if (champion->IsInCombat())
        {
            Unit* victim = champion->GetVictim();
            if (c.Has(AFFIX_TELEPORTER) && victim && WorldTimer::getMSTimeDiff(c.lastTeleport, now) >= TELEPORT_EVERY &&
                    !champion->IsWithinDistInMap(victim, TELEPORT_REACH))
            {
                float x, y, z;
                victim->GetNearPoint(champion, x, y, z, champion->GetObjectBoundingRadius(), 2.0f, victim->GetAngle(champion));
                champion->NearTeleportTo(x, y, z, champion->GetAngle(victim));
                std::lock_guard<std::mutex> guard(sPacksLock);
                sChampions[champion->GetObjectGuid()].lastTeleport = now;
            }
            if (c.Has(AFFIX_HEALER) && WorldTimer::getMSTimeDiff(c.lastHeal, now) >= HEAL_EVERY)
            {
                ObjectGuid leader;
                std::vector<ObjectGuid> members;
                {
                    std::lock_guard<std::mutex> guard(sPacksLock);
                    sChampions[champion->GetObjectGuid()].lastHeal = now;
                    auto it = sLeaderOf.find(champion->GetObjectGuid());
                    if (it != sLeaderOf.end())
                    {
                        leader = it->second;
                        auto pack = sPacks.find(leader);
                        if (pack != sPacks.end())
                        {
                            members.push_back(leader);
                            members.insert(members.end(), pack->second.followers.begin(), pack->second.followers.end());
                        }
                    }
                }
                for (ObjectGuid const& guid : members)
                    if (Creature* member = champion->GetMap()->GetCreature(guid))
                        if (member->IsAlive() && member->IsWithinDistInMap(champion, HEAL_RANGE) &&
                                member->GetHealth() < member->GetMaxHealth())
                            member->ModifyHealth(int32(member->GetMaxHealth() * HEAL_PCT / 100));
            }
            if (c.Has(AFFIX_COLD_ENCHANTED) && !c.novaDone && champion->GetHealth() < champion->GetMaxHealth() * NOVA_AT)
            {
                champion->CastSpell(champion, SPELL_FROST_NOVA, TRIGGERED_OLD_TRIGGERED);
                std::lock_guard<std::mutex> guard(sPacksLock);
                sChampions[champion->GetObjectGuid()].novaDone = true;
            }
        }
        champion->m_events.AddEvent(new UnitLambdaEvent(*champion, [](Unit& unit)
        {
            ChampionTick(static_cast<Creature*>(&unit));
        }), champion->m_events.CalculateTime(TICK_MS));
    }

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
                sChampions.erase(guid);
            }
        }
        for (ObjectGuid const& guid : followers)
            if (Creature* follower = leader->GetMap()->GetCreature(guid))
                if (!follower->IsInCombat())
                    follower->ForcedDespawn();
    }

    enum Role : uint8 { ROLE_FOLLOWER, ROLE_CHAMPION, ROLE_RARE };

    // `tier`: 0 by chance, 1 no champions, 2 a champion pack, 3 a rare (dev tools).
    void FormPack(Creature* leader, uint32 size, uint8 tier = 0)
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
        uint32 followers = size > 1 ? size - 1 : 0;

        // Champions or a rare, from level 8, by chance or as asked.
        Tier special = TIER_NONE;
        if (tier == 2)
            special = TIER_CHAMPION;
        else if (tier == 3)
            special = TIER_RARE;
        else if (tier == 0 && level >= CHAMPION_MIN_LEVEL)
        {
            uint32 const roll = urand(1, 1000);
            if (roll <= RARE_PER_MILLE)
                special = TIER_RARE;
            else if (roll <= RARE_PER_MILLE + CHAMPION_PER_MILLE)
                special = TIER_CHAMPION;
        }
        std::vector<Role> roles;
        std::vector<Affix> affixes;
        std::string rareName;
        if (special == TIER_CHAMPION)
        {
            uint32 const champions = level >= 20 ? 3 : 2;
            roles.assign(champions, ROLE_CHAMPION);
            followers = followers > champions ? followers - champions : 0;
            affixes = RollAffixes(level >= 30 ? 2 : 1);
        }
        else if (special == TIER_RARE)
        {
            roles.push_back(ROLE_RARE);
            affixes = RollAffixes(level >= 30 ? 3 : 2);
            rareName = RareName(affixes);
        }
        roles.insert(roles.end(), followers, ROLE_FOLLOWER);
        if (roles.empty())
            return;
        uint32 const count = uint32(roles.size());
        float const power = std::max(0.05f, sWorld.getConfig(CONFIG_FLOAT_ARPG_PACK_FOLLOWER_POWER));

        Pack pack;
        pack.level = level;
        std::vector<ObjectGuid> lesser;
        for (uint32 i = 0; i < count; ++i)
        {
            // Behind and around the leader, fanned out.
            float const fan = (float(i) - float(count - 1) / 2.0f) * 0.9f;
            float const angle = leader->GetOrientation() + float(M_PI) + fan;
            float const dist = FOLLOW_MIN + float(rand_norm()) * FOLLOW_SPREAD;
            float x, y, z;
            leader->GetNearPoint(leader, x, y, z, leader->GetObjectBoundingRadius(), dist, angle);
            Creature* member = leader->SummonCreature(leader->GetEntry(), x, y, z, leader->GetOrientation(),
                               TEMPSPAWN_CORPSE_TIMED_DESPAWN, PACK_CORPSE_MS);
            if (!member)
                continue;
            if (roles[i] == ROLE_FOLLOWER)
            {
                member->SelectLevel(std::max<uint32>(1, level - std::min<uint32>(level - 1, urand(1, 2))));
                member->SetMaxHealth(std::max<uint32>(1, uint32(member->GetMaxHealth() * power)));
                member->SetHealth(member->GetMaxHealth());
                member->SetObjectScale(member->GetObjectScale() * FOLLOWER_SCALE);
                lesser.push_back(member->GetObjectGuid());
            }
            else
                Crown(member, roles[i] == ROLE_RARE ? TIER_RARE : TIER_CHAMPION, affixes, rareName);
            switch (leader->GetDefaultMovementType())
            {
                case WAYPOINT_MOTION_TYPE:
                    member->GetMotionMaster()->MoveFollow(leader, dist, float(M_PI) + fan);
                    break;
                case RANDOM_MOTION_TYPE:
                    member->GetMotionMaster()->MoveRandomAroundPoint(x, y, z, FOLLOWER_WANDER);
                    break;
                default:
                    break;
            }
            pack.followers.push_back(member->GetObjectGuid());
        }
        if (pack.followers.empty())
            return;
        std::lock_guard<std::mutex> guard(sPacksLock);
        ObjectGuid const leaderGuid = leader->GetObjectGuid();
        sLeaderOf[leaderGuid] = leaderGuid;
        for (ObjectGuid const& guid : pack.followers)
            sLeaderOf[guid] = leaderGuid;
        for (ObjectGuid const& guid : lesser)
            sFollowers.insert(guid);
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
        if (!MaybeFollower(attacker))
            return 1.0f;
        if (IsPackFollower(attacker))
            return sWorld.getConfig(CONFIG_FLOAT_ARPG_PACK_FOLLOWER_POWER);
        Champion const c = ChampionOf(attacker);
        if (c.tier == TIER_NONE)
            return 1.0f;
        float mod = c.tier == TIER_RARE ? RARE_DAMAGE : CHAMPION_DAMAGE;
        if (c.Has(AFFIX_EXTRA_STRONG))
            mod *= STRONG_DAMAGE;
        return mod;
    }

    float PackDamageTakenMod(Unit const* victim)
    {
        if (!MaybeFollower(victim))
            return 1.0f;
        return ChampionOf(victim).Has(AFFIX_STONE_SKIN) ? STONE_TAKEN : 1.0f;
    }

    float PackSpeedMod(Unit const* unit)
    {
        if (!MaybeFollower(unit))
            return 1.0f;
        return ChampionOf(unit).Has(AFFIX_EXTRA_FAST) ? FAST_SPEED : 1.0f;
    }

    float PackXpMod(Unit const* victim)
    {
        if (!MaybeFollower(victim))
            return 1.0f;
        if (IsPackFollower(victim))
            return sWorld.getConfig(CONFIG_FLOAT_ARPG_PACK_FOLLOWER_XP);
        Tier const tier = ChampionOf(victim).tier;
        return tier == TIER_RARE ? RARE_XP : tier == TIER_CHAMPION ? CHAMPION_XP : 1.0f;
    }

    void ThinPackLoot(Creature* victim)
    {
        if (!victim || !victim->m_loot)
            return;
        if (IsPackFollower(victim))
        {
            victim->m_loot->ThinArpgLoot(sWorld.getConfig(CONFIG_FLOAT_ARPG_PACK_FOLLOWER_LOOT));
            return;
        }
        Tier const tier = ChampionOf(victim).tier;
        if (tier == TIER_NONE)
            return;
        // A champion: more gold and a green or blue; a rare: a blue, a chance at a purple.
        Loot* loot = victim->m_loot;
        uint32 const level = victim->GetLevel();
        loot->AddArpgDevGold(loot->GetGoldAmount() * (tier == TIER_RARE ? 2 : 1) + level * 5);
        auto add = [&](uint32 quality)
        {
            if (uint32 const id = PickRandomItem(quality, level + 3))
                loot->AddItem(id, 1, 0, Item::GenerateItemRandomPropertyId(id));
        };
        if (tier == TIER_CHAMPION)
            add(roll_chance_i(30) ? ITEM_QUALITY_RARE : ITEM_QUALITY_UNCOMMON);
        else
        {
            add(ITEM_QUALITY_RARE);
            if (roll_chance_i(15))
                add(ITEM_QUALITY_EPIC);
        }
    }

    void OnPackMelee(Unit* attacker, Unit* victim, uint32 damage)
    {
        if (!attacker || !victim || !damage)
            return;
        // A champion's own blows.
        Champion const striker = ChampionOf(attacker);
        if (striker.tier != TIER_NONE && victim->IsAlive())
        {
            if (striker.Has(AFFIX_FIRE_ENCHANTED))
                if (SpellEntry const* fire = sSpellTemplate.LookupEntry<SpellEntry>(SPELL_FIRE_BLAST))
                    StrikeFoe(attacker, victim, fire, SPELL_SCHOOL_MASK_FIRE, std::max<uint32>(1, damage * FIRE_SHARE_PCT / 100));
            if (striker.Has(AFFIX_COLD_ENCHANTED))
                attacker->CastSpell(victim, SPELL_CHILLED, TRIGGERED_OLD_TRIGGERED);
            if (striker.Has(AFFIX_VAMPIRIC) && attacker->IsAlive())
                attacker->ModifyHealth(int32(damage * VAMPIRIC_PCT / 100));
        }
        // Blows on a thorned champion.
        Champion const struck = ChampionOf(victim);
        if (struck.Has(AFFIX_THORNS) && attacker->IsAlive())
            if (SpellEntry const* thorns = sSpellTemplate.LookupEntry<SpellEntry>(SPELL_THORNS))
                StrikeFoe(victim, attacker, thorns, SPELL_SCHOOL_MASK_NATURE, std::max<uint32>(1, damage * THORNS_PCT / 100));
    }

    void SendNearbyChampions(Player* player)
    {
        if (!player || !player->IsInWorld())
            return;
        std::vector<std::pair<ObjectGuid, Champion>> fresh;
        {
            std::lock_guard<std::mutex> guard(sPacksLock);
            if (sChampions.empty())
                return;
            std::unordered_set<ObjectGuid>& sent = sChampionsSent[player->GetObjectGuid()];
            for (auto const& [guid, c] : sChampions)
                if (!sent.count(guid))
                    fresh.emplace_back(guid, c);
        }
        std::vector<std::pair<ObjectGuid, Champion>> inRange;
        for (auto const& entry : fresh)
            if (Creature* creature = player->GetMap()->GetCreature(entry.first))
                if (creature->IsWithinDistInMap(player, SEND_RANGE))
                    inRange.push_back(entry);
        if (inRange.empty())
            return;
        if (inRange.size() > 64)
            inRange.resize(64);
        WorldPacket data(SMSG_ARPG_CHAMPIONS, 1 + inRange.size() * 48);
        data << uint8(inRange.size());
        for (auto const& [guid, c] : inRange)
        {
            data << guid;
            data << uint8(c.tier);
            data << c.name;
            data << uint8(c.affixes.size());
            for (Affix affix : c.affixes)
                data << AFFIX_NAME[affix];
        }
        player->GetSession()->SendPacket(data);
        std::lock_guard<std::mutex> guard(sPacksLock);
        std::unordered_set<ObjectGuid>& sent = sChampionsSent[player->GetObjectGuid()];
        for (auto const& entry : inRange)
            sent.insert(entry.first);
    }

    void ForgetChampionsSent(Player* player)
    {
        std::lock_guard<std::mutex> guard(sPacksLock);
        sChampionsSent.erase(player->GetObjectGuid());
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
        // Fire Enchanted bursts a moment after it dies: stand clear of the corpse.
        if (ChampionOf(victim).Has(AFFIX_FIRE_ENCHANTED))
        {
            float const x = victim->GetPositionX(), y = victim->GetPositionY();
            victim->m_events.AddEvent(new UnitLambdaEvent(*victim, [x, y](Unit& corpse)
            {
                SpellEntry const* fire = sSpellTemplate.LookupEntry<SpellEntry>(SPELL_FIRE_BLAST);
                if (!fire || !corpse.IsInWorld())
                    return;
                for (auto const& ref : corpse.GetMap()->GetPlayers())
                    if (Player* player = ref.getSource())
                        if (player->IsAlive() && std::hypot(player->GetPositionX() - x, player->GetPositionY() - y) <= FIRE_BURST_RANGE)
                            StrikeFoe(&corpse, player, fire, SPELL_SCHOOL_MASK_FIRE,
                                      std::max<uint32>(1, player->GetMaxHealth() * FIRE_BURST_PCT / 100));
            }), victim->m_events.CalculateTime(FIRE_BURST_MS));
        }
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

    void DevFormPack(Player* player, uint8 size, uint8 tier)
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
        FormPack(nearest, std::min<uint32>(size, 8), tier);
        sLog.outString("ARPG packs: %s formed a test pack round %s", player->GetName(), nearest->GetName());
    }
}
