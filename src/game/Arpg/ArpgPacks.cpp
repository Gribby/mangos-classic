/*
 * ARPG mob packs: see ArpgPacks.h.
 */

#include "Arpg/ArpgPacks.h"
#include "Arpg/ArpgStates.h"
#include "Arpg/ArpgActs.h"
#include "Arpg/ArpgDamage.h"
#include "Arpg/ArpgActions.h"
#include "Arpg/ArpgAffixes.h"
#include "Arpg/ArpgCombat.h"
#include "Arpg/ArpgDungeons.h"
#include "Arpg/ArpgLoot.h"
#include "Arpg/ArpgThreats.h"
#include "Arpg/ArpgUniques.h"
#include "Arpg/ArpgStats.h"

#include "AI/BaseAI/CreatureAI.h"
#include "Entities/Creature.h"
#include "Entities/Item.h"
#include "Entities/Player.h"
#include "Grids/GridNotifiers.h"
#include "Grids/GridNotifiersImpl.h"
#include "Grids/CellImpl.h"
#include "Log/Log.h"
#include "Loot/LootMgr.h"
#include "Maps/Map.h"
#include "Maps/SpawnGroup.h"
#include "MotionGenerators/MotionMaster.h"
#include "Server/DBCStores.h"
#include "Server/Opcodes.h"
#include "Server/WorldPacket.h"
#include "Server/WorldSession.h"
#include "Spells/Spell.h"
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
    // How far a test pack's leader is looked for (DevFormPack).
    constexpr float AGGRO_REACH = 40.0f;
    // A pack member joins a fight only within this many yards of the one pulled, in its sight: a
    // camp is a pack, and a whole camp coming at once from across it was too much.
    constexpr float PULL_REACH = 20.0f;
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
    // Fire Enchanted's death burst, and Cold Enchanted's nova: both telegraphed (Arpg/ArpgThreats.h).
    constexpr uint32 FIRE_SHARE_PCT = 25, FIRE_BURST_PCT = 15, FIRE_BURST_MS = 1200;
    constexpr uint32 NOVA_WIND_UP_MS = 1000;
    constexpr float NOVA_RANGE = 10.0f, NOVA_DRIFT = 3.0f;
    constexpr float FIRE_BURST_RANGE = 6.0f;
    constexpr uint32 VAMPIRIC_PCT = 50, THORNS_PCT = 20;
    constexpr float TELEPORT_REACH = 12.0f;
    constexpr uint32 TELEPORT_EVERY = 6000, HEAL_EVERY = 5000, HEAL_PCT = 4, TICK_MS = 1000;
    constexpr float HEAL_RANGE = 20.0f;
    constexpr uint32 HEAL_VISUAL = 2050;    // Lesser Heal, rank 1: only its look

    // A Healer's heal on `to`, shown as `from` casting Lesser Heal at it (nothing is cast).
    void ShowHeal(Unit* from, Unit* to)
    {
        if (!sSpellTemplate.LookupEntry<SpellEntry>(HEAL_VISUAL))
            return;
        WorldPacket data(SMSG_SPELL_GO, 40);
        data << from->GetPackGUID();
        data << from->GetPackGUID();
        data << uint32(HEAL_VISUAL);
        data << uint16(CAST_FLAG_UNKNOWN9 | CAST_FLAG_HIDDEN_COMBATLOG);
        data << uint8(1);
        data << to->GetObjectGuid();
        data << uint8(0);
        SpellCastTargets targets;
        targets.setUnitTarget(to);
        data << targets;
        from->SendMessageToSet(data, true);
    }
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
    // The records of one map copy (a continent, or one instance of a dungeon): guids repeat between
    // copies (a dungeon's spawns keep their database guids in every copy, and summon counters start
    // alike), so each copy keeps its own.
    struct MapRecords
    {
        std::unordered_map<ObjectGuid, Pack> packs;            // by leader
        std::unordered_map<ObjectGuid, ObjectGuid> leaderOf;   // follower and leader alike -> leader
        std::unordered_set<ObjectGuid> followers;
        std::unordered_set<ObjectGuid> forming;                // leaders with a formation pending
        std::unordered_map<ObjectGuid, std::vector<ObjectGuid>> outriders; // by the spawn that placed them
        std::unordered_map<ObjectGuid, Champion> champions;
    };
    std::unordered_map<uint64, MapRecords> sRecords;            // by map id << 32 | instance id
    std::unordered_map<ObjectGuid, std::unordered_set<ObjectGuid>> sChampionsSent; // by player

    // `object`'s map copy's records; sPacksLock held.
    MapRecords& RecordsOf(WorldObject const* object)
    {
        return sRecords[(uint64(object->GetMapId()) << 32) | object->GetInstanceId()];
    }

    // A copy of `unit`'s champion record; tier TIER_NONE for an ordinary mob.
    Champion ChampionOf(Unit const* unit)
    {
        if (!unit || unit->GetTypeId() != TYPEID_UNIT)
            return Champion();
        std::lock_guard<std::mutex> guard(sPacksLock);
        MapRecords& rec = RecordsOf(unit);
        auto it = rec.champions.find(unit->GetObjectGuid());
        return it == rec.champions.end() ? Champion() : it->second;
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
        // A percent health modifier, which the core's own recalculations keep.
        float const health = tier == TIER_RARE ? RARE_HEALTH : CHAMPION_HEALTH;
        creature->HandleStatModifier(UNIT_MOD_HEALTH, TOTAL_PCT, (health - 1.0f) * 100.0f, true);
        creature->UpdateMaxHealth();
        creature->SetHealth(creature->GetMaxHealth());
        creature->SetObjectScale(creature->GetObjectScale() * (tier == TIER_RARE ? RARE_SCALE : CHAMPION_SCALE));
        Champion c;
        c.tier = tier;
        c.name = std::move(name);
        c.affixes = std::move(affixes);
        bool const fast = c.Has(AFFIX_EXTRA_FAST);
        {
            std::lock_guard<std::mutex> guard(sPacksLock);
            RecordsOf(creature).champions[creature->GetObjectGuid()] = std::move(c);
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
                RecordsOf(champion).champions[champion->GetObjectGuid()].lastTeleport = now;
            }
            if (c.Has(AFFIX_HEALER) && WorldTimer::getMSTimeDiff(c.lastHeal, now) >= HEAL_EVERY)
            {
                ObjectGuid leader;
                std::vector<ObjectGuid> members;
                {
                    std::lock_guard<std::mutex> guard(sPacksLock);
                    MapRecords& rec = RecordsOf(champion);
                    rec.champions[champion->GetObjectGuid()].lastHeal = now;
                    auto it = rec.leaderOf.find(champion->GetObjectGuid());
                    if (it != rec.leaderOf.end())
                    {
                        leader = it->second;
                        auto pack = rec.packs.find(leader);
                        if (pack != rec.packs.end())
                        {
                            members.push_back(leader);
                            members.insert(members.end(), pack->second.followers.begin(), pack->second.followers.end());
                        }
                    }
                }
                std::vector<Unit*> healed;
                for (ObjectGuid const& guid : members)
                    if (Creature* member = champion->GetMap()->GetCreature(guid))
                        healed.push_back(member);
                if (members.empty())
                {
                    // A dungeon champion has no pack: its allies fighting near it.
                    UnitList allies;
                    MaNGOS::AnyFriendlyUnitInObjectRangeCheck check(champion, HEAL_RANGE);
                    MaNGOS::UnitListSearcher<MaNGOS::AnyFriendlyUnitInObjectRangeCheck> searcher(allies, check);
                    Cell::VisitAllObjects(champion, searcher, HEAL_RANGE);
                    for (Unit* ally : allies)
                        if (ally->GetTypeId() == TYPEID_UNIT && ally->IsInCombat())
                            healed.push_back(ally);
                    healed.push_back(champion);
                }
                // Never a boss: a Healer beside one would undo a solo player's damage. Each heal
                // shows as a Lesser Heal from the champion, and its fighters are told, so the
                // Healer reads as the one to kill first.
                uint32 count = 0;
                for (Unit* member : healed)
                {
                    if (!member->IsAlive() || !member->IsWithinDistInMap(champion, HEAL_RANGE) ||
                            member->GetHealth() >= member->GetMaxHealth())
                        continue;
                    if (member->GetTypeId() == TYPEID_UNIT && IsDungeonBoss(static_cast<Creature*>(member)))
                        continue;
                    member->ModifyHealth(int32(member->GetMaxHealth() * HEAL_PCT / 100));
                    ShowHeal(champion, member);
                    ++count;
                }
                if (count)
                    for (auto const& ref : champion->GetMap()->GetPlayers())
                        if (Player* player = ref.getSource())
                            if (Active(player) && player->IsWithinDistInMap(champion, 40.0f))
                                ReportProc(player, PROC_EFFECT, 0, champion->GetName(), "(Healer) mends %u %s for %u%% of their health.",
                                           count, count == 1 ? "ally" : "allies", HEAL_PCT);
            }
            if (c.Has(AFFIX_COLD_ENCHANTED) && !c.novaDone && champion->GetHealth() < champion->GetMaxHealth() * NOVA_AT)
            {
                {
                    std::lock_guard<std::mutex> guard(sPacksLock);
                    RecordsOf(champion).champions[champion->GetObjectGuid()].novaDone = true;
                }
                // The nova is shown on the ground first, then cast where it was shown: the
                // champion holds still (a scripted one is left to its script) and a move, a stun
                // or the end of the fight breaks it off.
                float const x = champion->GetPositionX(), y = champion->GetPositionY();
                uint32 const serial = ShowTelegraph(champion, TELEGRAPH_SHAPE_RING,
                                                    c.tier == TIER_RARE ? TELEGRAPH_GRADE_RARE : TELEGRAPH_GRADE_CHAMPION,
                                                    x, y, champion->GetPositionZ(), 0.0f, NOVA_RANGE, NOVA_WIND_UP_MS);
                UnitAI* ai = champion->GetScriptId() ? nullptr : champion->AI();
                bool const hadMovement = ai && ai->IsCombatMovement();
                if (ai)
                    ai->SetCombatMovement(false, true);
                champion->m_events.AddEvent(new UnitLambdaEvent(*champion, [ai, hadMovement, serial, x, y](Unit& unit)
                {
                    // Only the AI that was frozen is thawed (a charm's end swaps it).
                    if (ai && unit.AI() == ai)
                        ai->SetCombatMovement(hadMovement, true);
                    bool const lands = unit.IsAlive() && unit.IsInWorld() && unit.IsInCombat() && !unit.IsCrowdControlled() &&
                                       std::hypot(unit.GetPositionX() - x, unit.GetPositionY() - y) <= NOVA_DRIFT;
                    if (lands)
                        unit.CastSpell(&unit, SPELL_FROST_NOVA, TRIGGERED_OLD_TRIGGERED);
                    else
                        HideTelegraph(&unit, serial);
                }), champion->m_events.CalculateTime(NOVA_WIND_UP_MS));
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
        if (level <= 29)
            return { 3, 4 };
        return { 3, 5 };
    }

    // A camp: vanilla's own spawns of normal rank, alive, within CAMP_RADIUS of `leader`. A camp
    // is a pack already (a gnoll camp is eight spawns side by side): each spawn leading its own
    // made one of thirty.
    constexpr float CAMP_RADIUS = 18.0f;
    constexpr uint32 CAMP_CROWDED = 4, CAMP_CLOSE = 2;

    uint32 CampNeighbours(Creature* leader)
    {
        UnitList around;
        MaNGOS::AnyFriendlyUnitInObjectRangeCheck check(leader, CAMP_RADIUS);
        MaNGOS::UnitListSearcher<MaNGOS::AnyFriendlyUnitInObjectRangeCheck> searcher(around, check);
        Cell::VisitAllObjects(leader, searcher, CAMP_RADIUS);
        uint32 n = 0;
        for (Unit* unit : around)
        {
            if (unit == leader || unit->GetTypeId() != TYPEID_UNIT || !unit->IsAlive())
                continue;
            Creature* c = static_cast<Creature*>(unit);
            if (c->HasStaticDBSpawnData() && c->GetCreatureInfo() && c->GetCreatureInfo()->Rank == CREATURE_ELITE_NORMAL &&
                    !c->GetCreatureInfo()->NpcFlags && c->GetCreatureInfo()->CreatureType != CREATURE_TYPE_CRITTER)
                ++n;
        }
        return n;
    }

    // Why `creature` can't lead a pack, or nullptr when it can. A creature in a spawn group (most
    // open-world mobs in the current database: random entries and camps) may lead one unless the
    // group is a formation; in a group of several, only its first member does, so a camp gets one
    // pack rather than one per mob.
    char const* WhyNotLeader(Creature* creature)
    {
        if (!creature || !creature->IsInWorld())
            return "not in the world";
        if (!creature->IsAlive())
            return "dead";
        if (creature->IsInCombat())
            return "in combat";
        if (creature->GetSubtype() != CREATURE_SUBTYPE_GENERIC || !creature->HasStaticDBSpawnData())
            return "not a database spawn";
        if (IsActLeader(creature->GetEntry()))
            return "an act's leader";
        if (creature->IsPet() || creature->IsTotem())
            return "a pet or totem";
        if (creature->GetMap()->Instanceable())
            return "in an instance";
        if (CreatureGroup* group = creature->GetCreatureGroup())
        {
            SpawnGroupEntry const& entry = group->GetGroupEntry();
            if (entry.formationEntry)
                return "in a formation";
            if (!entry.DbGuids.empty() && entry.DbGuids.front().DbGuid != creature->GetDbGuid())
                return "not the first of its spawn group";
        }
        CreatureInfo const* info = creature->GetCreatureInfo();
        if (!info || info->Rank != CREATURE_ELITE_NORMAL)
            return "not of normal rank";
        if (info->NpcFlags || creature->IsCivilian())
            return "an NPC or civilian";
        if (info->CreatureType == CREATURE_TYPE_CRITTER)
            return "a critter";
        if (creature->IsNoXp())
            return "gives no XP";
        FactionTemplateEntry const* faction = creature->GetFactionTemplateEntry();
        if (!faction || (faction->friendGroupMask & FACTION_GROUP_MASK_PLAYER) ||
                (faction->factionGroupMask & FACTION_GROUP_MASK_PLAYER))
            return "friendly to players";
        return nullptr;
    }

    bool Eligible(Creature* creature)
    {
        return WhyNotLeader(creature) == nullptr;
    }

    // Send away the followers that aren't fighting, and forget the pack.
    void Disband(Creature* leader)
    {
        std::vector<ObjectGuid> followers;
        {
            std::lock_guard<std::mutex> guard(sPacksLock);
            MapRecords& rec = RecordsOf(leader);
            auto it = rec.packs.find(leader->GetObjectGuid());
            if (it == rec.packs.end())
                return;
            followers = it->second.followers;
            rec.packs.erase(it);
            rec.leaderOf.erase(leader->GetObjectGuid());
            for (ObjectGuid const& guid : followers)
            {
                rec.leaderOf.erase(guid);
                rec.followers.erase(guid);
                rec.champions.erase(guid);
            }
        }
        // A dungeon's followers stay (WatchFollower).
        if (leader->GetMap()->IsDungeon())
            return;
        for (ObjectGuid const& guid : followers)
            if (Creature* follower = leader->GetMap()->GetCreature(guid))
                if (!follower->IsInCombat())
                    follower->ForcedDespawn();
    }

    constexpr uint32 FOLLOWER_WATCH_MS = 20 * IN_MILLISECONDS;

    // Every FOLLOWER_WATCH_MS: a follower whose pack has gone (its leader dead, despawned or
    // unloaded, or the pack formed afresh) leaves once out of combat. Followers never expire alive,
    // so a pack left half-killed would otherwise stand for good.
    void WatchFollower(Creature* follower, ObjectGuid leader)
    {
        follower->m_events.AddEvent(new UnitLambdaEvent(*follower, [leader](Unit& unit)
        {
            Creature* self = static_cast<Creature*>(&unit);
            if (!self->IsInWorld() || !self->IsAlive())
                return;
            bool kept;
            {
                std::lock_guard<std::mutex> guard(sPacksLock);
                MapRecords& rec = RecordsOf(self);
                auto it = rec.leaderOf.find(self->GetObjectGuid());
                kept = it != rec.leaderOf.end() && it->second == leader;
            }
            if (kept)
            {
                Creature const* head = self->GetMap()->GetCreature(leader);
                kept = head && head->IsAlive();
            }
            // A dungeon's escort stays when its leader falls: an instance script may count it, and
            // one despawned alive would never be counted dead.
            if (!kept && self->GetMap()->IsDungeon())
                return;
            if (!kept && !self->IsInCombat())
            {
                self->ForcedDespawn();
                return;
            }
            WatchFollower(self, leader);
        }), follower->m_events.CalculateTime(FOLLOWER_WATCH_MS));
    }

    enum Role : uint8 { ROLE_FOLLOWER, ROLE_CHAMPION, ROLE_RARE };

    // `leader`'s pack, leader eligible already. `tier`: 0 by chance, 1 no champions, 2 a champion
    // pack, 3 a rare (dev tools).
    void FormPackAround(Creature* leader, uint32 size, uint8 tier)
    {
        Disband(leader);

        uint32 const level = leader->GetLevel();
        // A pack the world forms (not the dev tools'): smaller where vanilla's spawns crowd already;
        // in a crowded camp, none but a champion pack's champions.
        bool crowded = false;
        if (!size)
        {
            auto const [lo, hi] = SizeFor(level);
            size = urand(lo, hi);
            uint32 const neighbours = CampNeighbours(leader);
            if (neighbours >= CAMP_CROWDED)
            {
                size = 1;
                crowded = true;
            }
            else if (neighbours >= CAMP_CLOSE)
                size = std::min<uint32>(size, 2);
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
            uint32 const champions = level >= 20 && !crowded ? 3 : 2;
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
                member->HandleStatModifier(UNIT_MOD_HEALTH, TOTAL_PCT, (power - 1.0f) * 100.0f, true);
                member->UpdateMaxHealth();
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
            WatchFollower(member, leader->GetObjectGuid());
        }
        if (pack.followers.empty())
            return;
        std::lock_guard<std::mutex> guard(sPacksLock);
        MapRecords& rec = RecordsOf(leader);
        ObjectGuid const leaderGuid = leader->GetObjectGuid();
        rec.leaderOf[leaderGuid] = leaderGuid;
        for (ObjectGuid const& guid : pack.followers)
            rec.leaderOf[guid] = leaderGuid;
        for (ObjectGuid const& guid : lesser)
            rec.followers.insert(guid);
        rec.packs[leaderGuid] = std::move(pack);
    }

    // Outriders: vanilla spreads its spawns for slow fights, and an ARPG clears them several times
    // faster, so each pack a vanilla spawn forms also places Arpg.Packs.Extra more packs out on
    // open ground near it: a copy of the spawn leading a pack of its own. They are temporary, like
    // followers, and belong to the spawn that placed them: its next formation (a respawn) or its
    // grid unloading sends them away once they're out of combat.
    constexpr float OUTRIDER_MIN = 22.0f, OUTRIDER_MAX = 45.0f;
    constexpr float OUTRIDER_CLEAR = 15.0f;     // no other creature this close
    constexpr float OUTRIDER_UNSEEN = 50.0f;    // no town or NPC this close (players: their sight)
    constexpr uint32 OUTRIDER_TRIES = 6;
    constexpr uint32 OUTRIDER_WATCH_MS = 20 * IN_MILLISECONDS;

    // Whether an outrider may stand at (x, y, z): open ground, clear of other creatures, and out
    // of sight of players and of anyone friendly to them (a town, a camp of NPCs).
    bool OutriderSpotClear(Creature* source, float x, float y, float z)
    {
        Map* map = source->GetMap();
        for (auto const& ref : map->GetPlayers())
            if (Player const* player = ref.getSource())
                if (player->GetDistance(x, y, z) < std::max(OUTRIDER_UNSEEN, map->GetVisibilityDistance()))
                    return false;
        UnitList around;
        MaNGOS::AnyUnitInObjectRangeCheck check(source, OUTRIDER_MAX + OUTRIDER_UNSEEN);
        MaNGOS::UnitListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> searcher(around, check);
        Cell::VisitAllObjects(source, searcher, OUTRIDER_MAX + OUTRIDER_UNSEEN);
        for (Unit* unit : around)
        {
            if (unit == source || unit->GetTypeId() != TYPEID_UNIT || !unit->IsAlive())
                continue;
            float const dist = unit->GetDistance(x, y, z);
            if (dist < OUTRIDER_CLEAR)
                return false;
            if (dist >= OUTRIDER_UNSEEN)
                continue;
            Creature const* c = static_cast<Creature const*>(unit);
            FactionTemplateEntry const* faction = c->GetFactionTemplateEntry();
            if ((c->GetCreatureInfo() && c->GetCreatureInfo()->NpcFlags) || c->IsCivilian() || !faction ||
                    (faction->friendGroupMask & FACTION_GROUP_MASK_PLAYER) || (faction->factionGroupMask & FACTION_GROUP_MASK_PLAYER))
                return false;
        }
        return true;
    }

    bool IsOutriderOf(Creature const* outrider, ObjectGuid source)
    {
        std::lock_guard<std::mutex> guard(sPacksLock);
        MapRecords& rec = RecordsOf(outrider);
        auto it = rec.outriders.find(source);
        return it != rec.outriders.end() &&
               std::find(it->second.begin(), it->second.end(), outrider->GetObjectGuid()) != it->second.end();
    }

    // Every OUTRIDER_WATCH_MS: an outrider whose spawn has gone (its grid unloaded) or has placed
    // others since leaves, with its pack, once out of combat. A dead one is left to its corpse.
    void WatchOutrider(Creature* outrider, ObjectGuid source)
    {
        outrider->m_events.AddEvent(new UnitLambdaEvent(*outrider, [source](Unit& unit)
        {
            Creature* self = static_cast<Creature*>(&unit);
            if (!self->IsInWorld() || !self->IsAlive())
                return;
            bool const kept = self->GetMap()->GetCreature(source) && IsOutriderOf(self, source);
            if (!kept && !self->IsInCombat())
            {
                Disband(self);
                self->ForcedDespawn();
                return;
            }
            WatchOutrider(self, source);
        }), outrider->m_events.CalculateTime(OUTRIDER_WATCH_MS));
    }

    // Send away `source`'s outriders (with their packs) that aren't fighting; a fighting one is
    // forgotten and leaves when its fight ends (WatchOutrider).
    void RecallOutriders(Creature* source)
    {
        std::vector<ObjectGuid> outriders;
        {
            std::lock_guard<std::mutex> guard(sPacksLock);
            MapRecords& rec = RecordsOf(source);
            auto it = rec.outriders.find(source->GetObjectGuid());
            if (it == rec.outriders.end())
                return;
            outriders = std::move(it->second);
            rec.outriders.erase(it);
        }
        for (ObjectGuid const& guid : outriders)
            if (Creature* outrider = source->GetMap()->GetCreature(guid))
                if (outrider->IsAlive() && !outrider->IsInCombat())
                {
                    Disband(outrider);
                    outrider->ForcedDespawn();
                }
    }

    void PlaceOutriders(Creature* source)
    {
        float const extra = std::max(0.0f, sWorld.getConfig(CONFIG_FLOAT_ARPG_PACK_EXTRA));
        uint32 count = uint32(extra);
        if (rand_norm() < extra - float(count))
            ++count;
        std::vector<ObjectGuid> placed;
        for (uint32 n = 0; n < count; ++n)
        {
            for (uint32 attempt = 0; attempt < OUTRIDER_TRIES; ++attempt)
            {
                float x = source->GetPositionX(), y = source->GetPositionY(), z = source->GetPositionZ();
                if (!source->GetMap()->GetReachableRandomPosition(source, x, y, z, OUTRIDER_MAX))
                    continue;
                float const dist = source->GetDistance(x, y, z);
                // Only into a grid already loaded: loading one would form its spawns' packs and
                // their outriders, and so on across the continent.
                if (dist < OUTRIDER_MIN || !source->GetMap()->IsLoaded(x, y) || !OutriderSpotClear(source, x, y, z))
                    continue;
                Creature* outrider = source->SummonCreature(source->GetEntry(), x, y, z, frand(0.0f, 2.0f * float(M_PI)),
                                     TEMPSPAWN_CORPSE_TIMED_DESPAWN, PACK_CORPSE_MS);
                if (!outrider)
                    break;
                outrider->GetMotionMaster()->MoveRandomAroundPoint(x, y, z, FOLLOWER_WANDER * 2.0f);
                placed.push_back(outrider->GetObjectGuid());
                WatchOutrider(outrider, source->GetObjectGuid());
                FormPackAround(outrider, 0, 0);
                break;
            }
        }
        if (placed.empty())
            return;
        std::lock_guard<std::mutex> guard(sPacksLock);
        RecordsOf(source).outriders[source->GetObjectGuid()] = std::move(placed);
    }

    // `leader`'s pack, if it may lead one, and, for a pack the world forms (not the dev tools'),
    // its outriders. `tier` as FormPackAround's.
    void FormPack(Creature* leader, uint32 size, uint8 tier = 0)
    {
        {
            std::lock_guard<std::mutex> guard(sPacksLock);
            RecordsOf(leader).forming.erase(leader->GetObjectGuid());
        }
        if (!PacksOn() || !Eligible(leader))
            return;
        RecallOutriders(leader);
        FormPackAround(leader, size, tier);
        if (!size && !tier)
            PlaceOutriders(leader);
    }

    // The leader of `unit`'s pack and its members (leader first), if it is in one.
    bool PackOf(Unit const* unit, ObjectGuid& leader, std::vector<ObjectGuid>& members)
    {
        std::lock_guard<std::mutex> guard(sPacksLock);
        MapRecords& rec = RecordsOf(unit);
        auto it = rec.leaderOf.find(unit->GetObjectGuid());
        if (it == rec.leaderOf.end())
            return false;
        leader = it->second;
        auto pack = rec.packs.find(leader);
        if (pack == rec.packs.end())
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
    // A dungeon's trash may lead a small escort of lesser copies (vanilla's dungeons are spaced
    // for five slow players): never a boss, a scripted creature or a formation's member.
    bool DungeonEscortable(Creature* creature)
    {
        if (!creature || !creature->IsInWorld() || !creature->IsAlive() || creature->IsInCombat() ||
                creature->GetSubtype() != CREATURE_SUBTYPE_GENERIC || !creature->HasStaticDBSpawnData())
            return false;
        Map* map = creature->GetMap();
        // Dungeons whose scripts count their trash's deaths to open the way (Stratholme's
        // ziggurats, Scholomance, Blackrock Depths): an escort would count too.
        uint32 const mapId = map->GetId();
        if (mapId == 329 || mapId == 289 || mapId == 230)
            return false;
        if (!map->IsDungeon() || map->IsRaid() || IsDungeonBoss(creature) || creature->IsWorldBoss() ||
                creature->IsPet() || creature->IsTotem() || creature->GetScriptId() || ChampionTier(creature))
            return false;
        if (CreatureGroup* group = creature->GetCreatureGroup())
            if (group->GetGroupEntry().formationEntry)
                return false;
        CreatureInfo const* info = creature->GetCreatureInfo();
        if (!info || info->Rank > CREATURE_ELITE_ELITE || info->NpcFlags || creature->IsCivilian() ||
                info->CreatureType == CREATURE_TYPE_CRITTER || creature->IsNoXp())
            return false;
        FactionTemplateEntry const* faction = creature->GetFactionTemplateEntry();
        return faction && !(faction->friendGroupMask & FACTION_GROUP_MASK_PLAYER) &&
               !(faction->factionGroupMask & FACTION_GROUP_MASK_PLAYER);
    }

    void OnCreatureAdded(Creature* creature)
    {
        // An act's named leaders are crowned (Arpg/ArpgActs.h).
        OnActCreatureAdded(creature);
        // Instances and open-world elites: group scaling and dungeon champions (Arpg/ArpgDungeons.h).
        OnScalableCreatureAdded(creature);
        if (!PacksOn() || !creature || creature->GetSubtype() != CREATURE_SUBTYPE_GENERIC ||
                !creature->HasStaticDBSpawnData())
            return;
        // A dungeon's trash: an escort of one or two lesser copies, by Arpg.Packs.Dungeon's chance.
        if (creature->GetMap()->IsDungeon() && !creature->GetMap()->IsRaid())
        {
            float const chance = sWorld.getConfig(CONFIG_FLOAT_ARPG_PACK_DUNGEON);
            if (chance <= 0.0f || rand_norm() >= chance)
                return;
            {
                std::lock_guard<std::mutex> guard(sPacksLock);
                if (!RecordsOf(creature).forming.insert(creature->GetObjectGuid()).second)
                    return;
            }
            creature->m_events.AddEvent(new UnitLambdaEvent(*creature, [](Unit& unit)
            {
                Creature* leader = static_cast<Creature*>(&unit);
                {
                    std::lock_guard<std::mutex> guard(sPacksLock);
                    RecordsOf(leader).forming.erase(leader->GetObjectGuid());
                }
                if (DungeonEscortable(leader))
                    FormPackAround(leader, roll_chance_i(35) ? 3 : 2, 1);   // tier 1: followers only
            }), creature->m_events.CalculateTime(urand(FORM_DELAY_MIN, FORM_DELAY_MAX)));
            return;
        }
        if (creature->GetMap()->Instanceable())
            return;
        {
            std::lock_guard<std::mutex> guard(sPacksLock);
            if (!RecordsOf(creature).forming.insert(creature->GetObjectGuid()).second)
                return;
        }
        creature->m_events.AddEvent(new UnitLambdaEvent(*creature, [](Unit& unit)
        {
            FormPack(static_cast<Creature*>(&unit), 0);
        }), creature->m_events.CalculateTime(urand(FORM_DELAY_MIN, FORM_DELAY_MAX)));
    }

    void FormRoadPack(Creature* leader)
    {
        // A summon, so not Eligible (a database spawn): it only has to be standing free.
        if (!leader || !PacksOn() || !leader->IsInWorld() || !leader->IsAlive() || leader->IsInCombat())
            return;
        leader->GetMotionMaster()->MoveRandomAroundPoint(leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ(),
                                                         FOLLOWER_WANDER * 2.0f);
        FormPackAround(leader, 0, 0);
    }

    bool IsPackFollower(Unit const* unit)
    {
        if (!MaybeFollower(unit))
            return false;
        std::lock_guard<std::mutex> guard(sPacksLock);
        return RecordsOf(unit).followers.count(unit->GetObjectGuid()) != 0;
    }

    // Champions are pack summons and dungeon spawns alike, so these ask any creature.
    float PackDamageMod(Unit const* attacker)
    {
        if (!attacker || attacker->GetTypeId() != TYPEID_UNIT)
            return 1.0f;
        float const tier = TierDamageMod(attacker) * DifficultyDamageMod(attacker);
        if (IsPackFollower(attacker))
            return sWorld.getConfig(CONFIG_FLOAT_ARPG_PACK_FOLLOWER_POWER) * tier;
        Champion const c = ChampionOf(attacker);
        if (c.tier == TIER_NONE)
            return tier;
        float mod = c.tier == TIER_RARE ? RARE_DAMAGE : CHAMPION_DAMAGE;
        if (c.Has(AFFIX_EXTRA_STRONG))
            mod *= STRONG_DAMAGE;
        return mod * tier;
    }

    float PackDamageTakenMod(Unit const* victim)
    {
        if (!victim || victim->GetTypeId() != TYPEID_UNIT)
            return 1.0f;
        return ChampionOf(victim).Has(AFFIX_STONE_SKIN) ? STONE_TAKEN : 1.0f;
    }

    float PackSpeedMod(Unit const* unit)
    {
        if (!unit || unit->GetTypeId() != TYPEID_UNIT)
            return 1.0f;
        return ChampionOf(unit).Has(AFFIX_EXTRA_FAST) ? FAST_SPEED : 1.0f;
    }

    float PackXpMod(Unit const* victim)
    {
        if (!victim || victim->GetTypeId() != TYPEID_UNIT)
            return 1.0f;
        float const tier = TierXpMod(victim);
        if (IsPackFollower(victim))
            return sWorld.getConfig(CONFIG_FLOAT_ARPG_PACK_FOLLOWER_XP) * tier;
        Tier const t = ChampionOf(victim).tier;
        return (t == TIER_RARE ? RARE_XP : t == TIER_CHAMPION ? CHAMPION_XP : 1.0f) * tier;
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
        // A champion: more gold and sometimes a rolled green or blue; a rare: a rolled green or
        // blue, a small chance at a purple. Rolled bases, never uniques (vanilla's named items drop from their own tables),
        // at the foe's level and three over: past vanilla's, the raid bank's (Arpg/ArpgBases.h).
        Loot* loot = victim->m_loot;
        uint32 const level = victim->GetLevel();
        loot->AddArpgDevGold(loot->GetGoldAmount() * (tier == TIER_RARE ? 2 : 1) + level * 5);
        uint32 const itemLevel = level + 3 + uint32(LootTier(victim->GetMap())) * 2;
        // Champions are common, so their extra is a chance, not a promise (Jeff: too much loot).
        if (tier == TIER_CHAMPION)
        {
            if (roll_chance_i(40))
                AddRolledItem(loot, roll_chance_i(20) ? RARITY_BLUE : RARITY_GREEN, itemLevel);
        }
        else
        {
            AddRolledItem(loot, roll_chance_i(60) ? RARITY_BLUE : RARITY_GREEN, itemLevel);
            if (roll_chance_i(3))
                AddRolledItem(loot, RARITY_PURPLE, itemLevel);
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
                attacker->ModifyHealth(int32(damage * VAMPIRIC_PCT / 100 * CreatureHealthScale(attacker->GetLevel())));
        }
        // Blows on a thorned champion.
        Champion const struck = ChampionOf(victim);
        if (struck.Has(AFFIX_THORNS) && attacker->IsAlive())
            if (SpellEntry const* thorns = sSpellTemplate.LookupEntry<SpellEntry>(SPELL_THORNS))
                StrikeFoe(victim, attacker, thorns, SPELL_SCHOOL_MASK_NATURE, std::max<uint32>(1, FromDamage(attacker, victim, damage) * THORNS_PCT / 100));
    }

    void SendNearbyChampions(Player* player)
    {
        if (!player || !player->IsInWorld())
            return;
        std::vector<std::pair<ObjectGuid, Champion>> fresh;
        {
            std::lock_guard<std::mutex> guard(sPacksLock);
            MapRecords& rec = RecordsOf(player);
            if (rec.champions.empty())
                return;
            std::unordered_set<ObjectGuid>& sent = sChampionsSent[player->GetObjectGuid()];
            for (auto const& [guid, c] : rec.champions)
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
            MapRecords& rec = RecordsOf(member);
            auto it = rec.packs.find(leader);
            if (it != rec.packs.end() && !it->second.aggroAt)
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
                    !other->IsWithinDistInMap(member, PULL_REACH) || !other->IsWithinLOSInMap(member))
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
        MapRecords& rec = RecordsOf(attacker);
        auto it = rec.leaderOf.find(attacker->GetObjectGuid());
        if (it == rec.leaderOf.end())
            return;
        auto pack = rec.packs.find(it->second);
        if (pack != rec.packs.end())
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
            ShowTelegraph(victim, TELEGRAPH_SHAPE_BLAST, ChampionTier(victim) == 2 ? TELEGRAPH_GRADE_RARE : TELEGRAPH_GRADE_CHAMPION,
                          x, y, victim->GetPositionZ(), 0.0f, FIRE_BURST_RANGE, FIRE_BURST_MS);
            victim->m_events.AddEvent(new UnitLambdaEvent(*victim, [x, y](Unit& corpse)
            {
                SpellEntry const* fire = sSpellTemplate.LookupEntry<SpellEntry>(SPELL_FIRE_BLAST);
                if (!fire || !corpse.IsInWorld())
                    return;
                for (auto const& ref : corpse.GetMap()->GetPlayers())
                    if (Player* player = ref.getSource())
                        if (player->IsAlive() && !Dodging(player) &&
                                std::hypot(player->GetPositionX() - x, player->GetPositionY() - y) <= FIRE_BURST_RANGE)
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
        MapRecords& rec = RecordsOf(victim);
        auto it = rec.packs.find(leader);
        if (it == rec.packs.end() || !it->second.aggroAt)
            return;
        float const seconds = WorldTimer::getMSTimeDiff(it->second.aggroAt, WorldTimer::getMSTime()) / 1000.0f;
        sLog.outString("ARPG packs: a pack of %u %s (level %u) fell in %.1f s; it dealt %u damage",
                       uint32(members.size()), victim->GetName(), it->second.level, seconds, it->second.damage);
        // Only a pack a player brought down: one that fell to guards or other creatures is not play.
        if (static_cast<Creature*>(victim)->GetLootRecipient())
            OnStatsPack(victim->GetName(), uint32(members.size()), it->second.level, seconds, it->second.damage);
        it->second.aggroAt = 0;
        it->second.damage = 0;
    }

    uint8 ChampionTier(Unit const* unit)
    {
        return uint8(ChampionOf(unit).tier);
    }

    std::string ChampionName(Unit const* unit)
    {
        return ChampionOf(unit).name;
    }

    float ChampionHealthMod(Unit const* unit)
    {
        switch (ChampionOf(unit).tier)
        {
            case TIER_CHAMPION: return CHAMPION_HEALTH;
            case TIER_RARE: return RARE_HEALTH;
            default: return 1.0f;
        }
    }

    void MakeChampion(Creature* creature, uint8 tier, uint8 randomAffixes, std::string const& name,
                      std::vector<std::string> const& affixNames)
    {
        if (!creature || tier == TIER_NONE || ChampionTier(creature) != TIER_NONE)
            return;
        std::vector<Affix> affixes;
        for (std::string const& wanted : affixNames)
            for (uint8 a = 0; a < MAX_AFFIX; ++a)
                if (wanted == AFFIX_NAME[a])
                    affixes.push_back(Affix(a));
        for (Affix extra : RollAffixes(MAX_AFFIX))
        {
            if (randomAffixes == 0)
                break;
            if (std::find(affixes.begin(), affixes.end(), extra) != affixes.end())
                continue;
            affixes.push_back(extra);
            --randomAffixes;
        }
        Tier const t = tier >= TIER_RARE ? TIER_RARE : TIER_CHAMPION;
        std::string shown = name;
        if (t == TIER_RARE && shown.empty())
            shown = RareName(affixes);
        Crown(creature, t, affixes, shown);
    }

    void MakeLesser(Creature* creature, uint32 level)
    {
        if (!creature || !MaybeFollower(creature))
            return;
        float const power = std::max(0.05f, sWorld.getConfig(CONFIG_FLOAT_ARPG_PACK_FOLLOWER_POWER));
        creature->SelectLevel(std::max<uint32>(1, level - std::min<uint32>(level - 1, urand(1, 2))));
        creature->HandleStatModifier(UNIT_MOD_HEALTH, TOTAL_PCT, (power - 1.0f) * 100.0f, true);
        creature->UpdateMaxHealth();
        creature->SetHealth(creature->GetMaxHealth());
        creature->SetObjectScale(creature->GetObjectScale() * FOLLOWER_SCALE);
        std::lock_guard<std::mutex> guard(sPacksLock);
        RecordsOf(creature).followers.insert(creature->GetObjectGuid());
    }

    uint8 ChampionAffixCount(uint32 level, bool rare)
    {
        if (rare)
            return level >= 30 ? 3 : 2;
        return level >= 30 ? 2 : 1;
    }

    void OnCreatureRemoved(Creature* creature)
    {
        if (!creature)
            return;
        ObjectGuid const guid = creature->GetObjectGuid();
        ForgetScaled(creature);
        ForgetStates(creature);
        ForgetTelegraph(creature);
        std::lock_guard<std::mutex> guard(sPacksLock);
        // Guids repeat between map copies, so a stale record could crown another creature.
        uint64 const where = (uint64(creature->GetMapId()) << 32) | creature->GetInstanceId();
        auto records = sRecords.find(where);
        if (records == sRecords.end())
            return;
        MapRecords& rec = records->second;
        rec.champions.erase(guid);
        for (auto& [player, sent] : sChampionsSent)
            sent.erase(guid);
        rec.followers.erase(guid);
        rec.forming.erase(guid);
        // Its outriders leave once out of combat (WatchOutrider).
        rec.outriders.erase(guid);
        auto pack = rec.packs.find(guid);
        if (pack != rec.packs.end())
        {
            for (ObjectGuid const& follower : pack->second.followers)
                rec.leaderOf.erase(follower);
            rec.packs.erase(pack);
        }
        rec.leaderOf.erase(guid);
        // A copy whose creatures are all gone (an instance unloaded) leaves no record behind.
        if (rec.packs.empty() && rec.leaderOf.empty() && rec.followers.empty() && rec.forming.empty() &&
                rec.outriders.empty() && rec.champions.empty())
            sRecords.erase(records);
    }

    void DevFormPack(Player* player, uint8 size, uint8 tier)
    {
        if (!sWorld.getConfig(CONFIG_BOOL_ARPG_DEV_TOOLS) || !player)
            return;
        Creature* nearest = nullptr;
        float nearestDist = AGGRO_REACH;
        Creature* refused = nullptr;
        float refusedDist = AGGRO_REACH;
        for (Unit* unit : EnemiesNear(player, AGGRO_REACH))
        {
            if (unit->GetTypeId() != TYPEID_UNIT)
                continue;
            float const dist = player->GetDistance(unit);
            if (!Eligible(static_cast<Creature*>(unit)))
            {
                if (dist < refusedDist)
                {
                    refused = static_cast<Creature*>(unit);
                    refusedDist = dist;
                }
                continue;
            }
            if (dist < nearestDist)
            {
                nearest = static_cast<Creature*>(unit);
                nearestDist = dist;
            }
        }
        if (!nearest)
        {
            if (refused)
                sLog.outString("ARPG packs: %s asked for a test pack, but the nearest mob, %s (entry %u), can't lead one: %s",
                               player->GetName(), refused->GetName(), refused->GetEntry(), WhyNotLeader(refused));
            else
                sLog.outString("ARPG packs: %s asked for a test pack, but no mob is within %.0f yards",
                               player->GetName(), AGGRO_REACH);
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
