/*
 * ARPG shrines (Arpg/ArpgShrines.h).
 */

#include "Arpg/ArpgShrines.h"
#include "Arpg/ArpgBoons.h"
#include "Arpg/ArpgCombat.h"
#include "Arpg/ArpgDamage.h"
#include "Arpg/ArpgDungeons.h"
#include "Arpg/ArpgTags.h"
#include "Arpg/ArpgTree.h"
#include "Arpg/ArpgUniques.h"

#include "Chat/Chat.h"
#include "Database/DatabaseEnv.h"
#include "Entities/Creature.h"
#include "Entities/GameObject.h"
#include "Entities/Player.h"
#include "Log/Log.h"
#include "Maps/Map.h"
#include "Server/SQLStorages.h"
#include "Spells/SpellMgr.h"
#include "Util/Timer.h"
#include "World/World.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    using namespace Arpg;

    constexpr uint32 SHRINE_AHEAD_PCT = 12;         // of the packs placed ahead
    constexpr uint32 SHRINE_DUNGEON_ODDS = 40;      // 1 in this many settling trash creatures
    constexpr uint32 SHRINE_DUNGEON_MAX = 2;        // an instance's at most
    constexpr uint32 SHRINE_IDLE_SECS = 10 * MINUTE;
    constexpr uint32 SHRINE_DUNGEON_SECS = 2 * HOUR;
    constexpr float SHRINE_REACH = 3.0f;
    constexpr float SHRINE_BESIDE = 7.0f;           // how far from its pack
    constexpr uint32 CHECK_MS = 250;
    constexpr uint32 PULSE_MS = 1000;
    // Storms: an arcane nova around the player, of this much damage a level at the vanilla size
    // (ToDamage makes it the ARPG's), every pulse.
    constexpr float STORM_RANGE = 10.0f;
    constexpr uint32 STORM_PER_LEVEL = 12;
    constexpr uint32 STORM_LOOK = 1449;             // Arcane Explosion
    constexpr uint32 SWIFTNESS = 2379;              // Swiftness Potion: 50% run speed, 15 sec
    constexpr uint32 SHRINE_LOOK = 15237;           // Holy Nova's flash, at the touch

    struct KindDef
    {
        char const* name;
        uint32 ms;
        char const* text;
    };

    KindDef const KINDS[MAX_SHRINE] =
    {
        { "Shrine of Wrath", 20000, "Your damage is 50% more for 20 sec." },
        { "Shrine of Protection", 10000, "A Barrier of half your health guards you, filled again each second, for 10 sec." },
        { "Shrine of Greed", 30000, "Your kills' loot rolls richer, with three times the gold, for 30 sec." },
        { "Shrine of Celerity", 15000, "You move 50% and attack 25% faster for 15 sec." },
        { "Shrine of Channeling", 15000, "Your skills cost no mana for 15 sec." },
        { "Shrine of Storms", 20000, "Arcane storms burst around you each second for 20 sec." },
    };

    uint8 BoonOf(ShrineKind kind)
    {
        return uint8(BOON_SHRINE_FIRST + kind);
    }

    struct Spot
    {
        ObjectGuid guid;
        ShrineKind kind;
        float x, y, z;
    };

    struct PlayerShrines
    {
        uint32 checkedAt = 0;
        uint32 pulseAt = 0;
        bool hasted = false;    // Celerity's attack speed is in its totals
    };

    std::mutex sLock;
    // The shrines standing, by map and instance.
    std::unordered_map<uint64, std::vector<Spot>> sSpots;
    std::unordered_map<uint32, uint32> sDungeonCount;   // by instance
    std::unordered_map<ObjectGuid, PlayerShrines> sPlayers;
    bool sReady = false;                                 // the templates were written
    bool sKindReady[MAX_SHRINE] = {};                    // each kind's template is the shrine's

    uint64 KeyOf(Map const* map)
    {
        return (uint64(map->GetId()) << 32) | map->GetInstanceId();
    }

    bool ShrinesOn()
    {
        return sReady && sWorld.getConfig(CONFIG_BOOL_ARPG_ENABLE);
    }

    void Place(Map* map, float x, float y, float z, ShrineKind kind, uint32 secs)
    {
        uint32 const entry = SHRINE_ENTRY_FROM + kind;
        if (!sKindReady[kind] || !sGOStorage.LookupEntry<GameObjectInfo>(entry))
            return;
        GameObject* go = new GameObject;
        uint32 const low = map->GenerateLocalLowGuid(HIGHGUID_GAMEOBJECT);
        if (!go->Create(low, low, entry, map, x, y, z, frand(0.0f, 2.0f * float(M_PI))))
        {
            delete go;
            return;
        }
        // Not spawned by default: shown at once, and gone (deleted) when its time is up.
        go->SetSpellId(0);
        go->SetRespawnTime(secs);
        go->SetFlag(GAMEOBJECT_FLAGS, GO_FLAG_NO_INTERACT);
        map->Add(go);
        go->AIM_Initialize();
        std::lock_guard<std::mutex> guard(sLock);
        sSpots[KeyOf(map)].push_back({ go->GetObjectGuid(), kind, x, y, z });
    }

    // A shrine beside `creature`, on the ground it stands on.
    void PlaceBeside(Creature* creature, uint32 secs)
    {
        float x, y, z;
        creature->GetNearPoint(creature, x, y, z, creature->GetObjectBoundingRadius(), SHRINE_BESIDE, frand(0.0f, 2.0f * float(M_PI)));
        ShrineKind kinds[MAX_SHRINE];
        uint32 n = 0;
        for (uint8 kind = 0; kind < MAX_SHRINE; ++kind)
            if (sKindReady[kind])
                kinds[n++] = ShrineKind(kind);
        if (n)
            Place(creature->GetMap(), x, y, z, kinds[urand(0, n - 1)], secs);
    }

    void Activate(Player* player, ShrineKind kind)
    {
        KindDef const& def = KINDS[kind];
        AddBoon(player, BoonOf(kind), def.name, def.ms);
        if (SpellEntry const* look = sSpellTemplate.LookupEntry<SpellEntry>(SHRINE_LOOK))
            ShowImpactOn(player, look, SPELL_SCHOOL_MASK_HOLY);
        ChatHandler(player).PSendSysMessage("|cffffd100%s:|r %s", def.name, def.text);
        switch (kind)
        {
            case SHRINE_PROTECTION:
                GainBarrier(player, player->GetMaxHealth() * BARRIER_CAP_PCT / 100);
                break;
            case SHRINE_CELERITY:
                player->CastSpell(player, SWIFTNESS, TRIGGERED_OLD_TRIGGERED);
                RefreshTotals(player);
                break;
            default:
                break;
        }
    }

    // Storms' nova: every enemy within STORM_RANGE takes an arcane burst.
    void StormPulse(Player* player)
    {
        SpellEntry const* look = sSpellTemplate.LookupEntry<SpellEntry>(STORM_LOOK);
        if (!look)
            return;
        std::vector<Unit*> const foes = NearestFoes(player, player, STORM_RANGE, 12);
        for (Unit* foe : foes)
            StrikeFoe(player, foe, look, SPELL_SCHOOL_MASK_ARCANE,
                      std::max<uint32>(1, ToDamage(player, foe, STORM_PER_LEVEL * player->GetLevel())));
    }
}

namespace Arpg
{
    void EnsureShrineObjects()
    {
        if (!sWorld.getConfig(CONFIG_BOOL_ARPG_ENABLE))
            return;
        // The looks: vanilla's shrines first, then its altars, each kind its own where there are
        // enough, from objects with a model.
        std::vector<std::pair<uint32, std::string>> looks;
        if (auto result = WorldDatabase.PQuery(
                    "SELECT displayId, MIN(name) FROM gameobject_template WHERE displayId > 0 AND entry < %u AND "
                    "(name LIKE '%%Shrine%%' OR name LIKE '%%Altar%%') AND type IN (2, 3, 5, 8, 10) "
                    "GROUP BY displayId ORDER BY MAX(name LIKE '%%Shrine%%') DESC, MIN(entry) LIMIT 12", SHRINE_ENTRY_FROM))
        {
            do
            {
                Field* fields = result->Fetch();
                looks.emplace_back(fields[0].GetUInt32(), fields[1].GetCppString());
            }
            while (result->NextRow());
        }
        if (looks.empty())
        {
            sLog.outError("ARPG shrines: no shrine or altar look in gameobject_template; no shrines");
            return;
        }
        for (uint8 kind = 0; kind < MAX_SHRINE; ++kind)
        {
            auto const& [display, from] = looks[kind % looks.size()];
            uint32 const entry = SHRINE_ENTRY_FROM + kind;
            // Only a shrine's own row is rewritten: an entry the world database uses for something
            // else is left be, and that kind never appears.
            if (auto result = WorldDatabase.PQuery("SELECT name FROM gameobject_template WHERE entry = %u", entry))
            {
                std::string const name = result->Fetch()[0].GetCppString();
                if (name.rfind("Shrine of ", 0) != 0)
                {
                    sLog.outError("ARPG shrines: gameobject_template %u is '%s', not a shrine; no %s", entry, name.c_str(), KINDS[kind].name);
                    continue;
                }
            }
            WorldDatabase.DirectPExecute("DELETE FROM gameobject_template WHERE entry = %u", entry);
            WorldDatabase.DirectPExecute("INSERT INTO gameobject_template (entry, type, displayId, name, flags, size) "
                                         "VALUES (%u, 5, %u, '%s', %u, 1)",
                                         entry, display, KINDS[kind].name, uint32(GO_FLAG_NO_INTERACT));
            sLog.outString("ARPG shrines: %s wears the look of %s", KINDS[kind].name, from.c_str());
            sKindReady[kind] = true;
        }
        sReady = true;
    }

    void OnShrinePackPlaced(Creature* leader)
    {
        if (!ShrinesOn() || !leader || !leader->IsInWorld() || !roll_chance_i(int32(SHRINE_AHEAD_PCT)))
            return;
        PlaceBeside(leader, SHRINE_IDLE_SECS);
    }

    void OnShrineDungeonSettle(Creature* creature)
    {
        if (!ShrinesOn() || !creature || !creature->IsInWorld())
            return;
        Map* map = creature->GetMap();
        if (!map->IsDungeon() || map->IsRaid())
            return;
        // A nightmare blessed with Shrines has twice as many, found sooner (Arpg/ArpgDungeons.h).
        bool const blessed = NightmareHas(map, NM_SHRINES);
        if (urand(1, blessed ? SHRINE_DUNGEON_ODDS / 3 : SHRINE_DUNGEON_ODDS) != 1)
            return;
        {
            std::lock_guard<std::mutex> guard(sLock);
            uint32& count = sDungeonCount[map->GetInstanceId()];
            if (count >= (blessed ? SHRINE_DUNGEON_MAX * 2 : SHRINE_DUNGEON_MAX))
                return;
            ++count;
        }
        PlaceBeside(creature, SHRINE_DUNGEON_SECS);
    }

    bool ShrineActive(Player const* player, ShrineKind kind)
    {
        return player && kind < MAX_SHRINE && HasBoon(player, BoonOf(kind));
    }

    int32 ShrineDamagePct(Player const* player)
    {
        return ShrineActive(player, SHRINE_WRATH) ? 50 : 0;
    }

    float ShrineHastePct(Player const* player)
    {
        return ShrineActive(player, SHRINE_CELERITY) ? 25.0f : 0.0f;
    }

    uint8 ShrineLootTiers(Player const* player)
    {
        return ShrineActive(player, SHRINE_GREED) ? 3 : 0;
    }

    bool ShrineFreeCasting(Player const* player)
    {
        return ShrineActive(player, SHRINE_CHANNELING);
    }

    void ForgetShrines(Player* player)
    {
        if (!player)
            return;
        std::lock_guard<std::mutex> guard(sLock);
        sPlayers.erase(player->GetObjectGuid());
    }

    void ForgetShrineMap(Map const* map)
    {
        if (!map)
            return;
        std::lock_guard<std::mutex> guard(sLock);
        sSpots.erase(KeyOf(map));
        if (map->IsDungeon())
            sDungeonCount.erase(map->GetInstanceId());
    }

    void UpdateShrines(Player* player)
    {
        if (!ShrinesOn() || !player || !player->IsInWorld())
            return;
        uint32 const now = WorldTimer::getMSTime();
        Map* map = player->GetMap();
        bool pulse = false, hastedEnded = false;
        std::vector<Spot> touched;
        std::vector<ObjectGuid> stale;
        {
            std::lock_guard<std::mutex> guard(sLock);
            PlayerShrines& ps = sPlayers[player->GetObjectGuid()];
            if (ps.checkedAt && WorldTimer::getMSTimeDiff(ps.checkedAt, now) < CHECK_MS)
                return;
            ps.checkedAt = now ? now : 1;
            if (!ps.pulseAt || WorldTimer::getMSTimeDiff(ps.pulseAt, now) >= PULSE_MS)
            {
                ps.pulseAt = now ? now : 1;
                pulse = true;
            }
            auto it = sSpots.find(KeyOf(map));
            if (it != sSpots.end() && player->IsAlive())
            {
                auto& spots = it->second;
                for (auto spot = spots.begin(); spot != spots.end();)
                {
                    float const dx = player->GetPositionX() - spot->x, dy = player->GetPositionY() - spot->y;
                    if (dx * dx + dy * dy <= SHRINE_REACH * SHRINE_REACH && std::fabs(player->GetPositionZ() - spot->z) < 5.0f)
                    {
                        touched.push_back(*spot);
                        spot = spots.erase(spot);
                    }
                    else
                        ++spot;
                }
            }
        }
        for (Spot const& spot : touched)
        {
            GameObject* go = map->GetGameObject(spot.guid);
            // Gone already (its time ran out): no boon.
            if (!go || !go->IsInWorld())
                continue;
            go->Delete();
            Activate(player, spot.kind);
        }
        if (!pulse)
            return;
        // Pulses, a second apart.
        if (ShrineActive(player, SHRINE_PROTECTION))
            GainBarrier(player, player->GetMaxHealth() * BARRIER_CAP_PCT / 100);
        if (ShrineActive(player, SHRINE_STORMS) && player->IsInCombat())
            StormPulse(player);
        // Celerity's attack speed leaves the totals when it ends.
        bool const hasted = ShrineActive(player, SHRINE_CELERITY);
        {
            std::lock_guard<std::mutex> guard(sLock);
            PlayerShrines& ps = sPlayers[player->GetObjectGuid()];
            hastedEnded = ps.hasted && !hasted;
            ps.hasted = hasted;
            // A map's shrines that have gone (their time ran out) leave the list now and then.
            auto it = sSpots.find(KeyOf(map));
            if (it != sSpots.end())
                for (Spot const& spot : it->second)
                    stale.push_back(spot.guid);
        }
        if (hastedEnded)
            RefreshTotals(player);
        std::vector<ObjectGuid> gone;
        for (ObjectGuid const& guid : stale)
            if (!map->GetGameObject(guid))
                gone.push_back(guid);
        if (!gone.empty())
        {
            std::lock_guard<std::mutex> guard(sLock);
            auto it = sSpots.find(KeyOf(map));
            if (it != sSpots.end())
                it->second.erase(std::remove_if(it->second.begin(), it->second.end(), [&gone](Spot const& s)
                {
                    return std::find(gone.begin(), gone.end(), s.guid) != gone.end();
                }), it->second.end());
        }
    }
}
