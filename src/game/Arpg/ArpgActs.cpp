/*
 * ARPG acts: see ArpgActs.h.
 */

#include "Arpg/ArpgActs.h"
#include "Arpg/ArpgAffixes.h"
#include "Arpg/ArpgBases.h"
#include "Arpg/ArpgCombat.h"
#include "Arpg/ArpgDungeons.h"
#include "Arpg/ArpgPacks.h"
#include "Arpg/ArpgStats.h"

#include "Database/DatabaseEnv.h"
#include "Entities/Creature.h"
#include "Entities/Player.h"
#include "Entities/TemporarySpawn.h"
#include "Grids/CellImpl.h"
#include "Grids/GridNotifiers.h"
#include "Grids/GridNotifiersImpl.h"
#include "Globals/ObjectMgr.h"
#include "Log/Log.h"
#include "Loot/LootMgr.h"
#include "Maps/Map.h"
#include "MotionGenerators/PathFinder.h"
#include "Quests/QuestDef.h"
#include "Server/DBCStores.h"
#include "Server/SQLStorages.h"
#include "Util/Timer.h"
#include "World/World.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    using namespace Arpg;

    // Which side an act quest is for, as its RequiredRaces.
    constexpr uint32 RACES_ALLIANCE = (1 << (RACE_HUMAN - 1)) | (1 << (RACE_DWARF - 1)) | (1 << (RACE_NIGHTELF - 1)) | (1 << (RACE_GNOME - 1));
    constexpr uint32 RACES_HORDE = (1 << (RACE_ORC - 1)) | (1 << (RACE_UNDEAD - 1)) | (1 << (RACE_TAUREN - 1)) | (1 << (RACE_TROLL - 1));

    enum Side : uint8 { SIDE_ALLIANCE, SIDE_HORDE };

    // A kill objective: the first of its names the world has, and how many.
    struct Kill
    {
        std::array<char const*, 3> names;
        uint16 count;
    };

    // How a quest's named leader is crowned: a champion, or the chapter's last one, a rare.
    enum Lead : uint8 { LEAD_NONE, LEAD_CHAMPION, LEAD_FINALE };

    struct ActQuest
    {
        uint32 id;
        Side side;
        uint16 zone;                // the quest log's heading (an AreaTable zone)
        uint8 level;
        char const* title;
        char const* giver;          // the NPC who gives it
        char const* taker;          // the NPC who takes it
        char const* details;
        char const* objectives;
        char const* request;        // said while it is not done
        char const* reward;         // said when it is handed in
        std::array<Kill, 3> kills;
        char const* leader;         // its named leader (an objective of 1), or nullptr
        Lead lead;
        char const* leaderAffix;    // the leader's affix, by its name (Arpg/ArpgPacks.h)
        uint32 xp;
        uint32 money;               // copper
        std::array<char const*, 3> retells; // vanilla quests it tells again, by title
    };

    // Act I, chapter 1.
    std::vector<ActQuest> const& Quests()
    {
        static std::vector<ActQuest> const quests =
        {
            // --- Elwynn Forest ---
            { ACT_QUEST_FIRST + 0, SIDE_ALLIANCE, 12, 3, "Act I: The Northshire Watch",
              "Marshal McBride", "Marshal McBride",
              "You've arrived at a poor time, $N. Kobolds have crawled out of Echo Ridge and into our vineyards, and the "
              "Defias Brotherhood has a camp on the valley's edge. Their leader, a cutthroat called Garrick Padfoot, "
              "thinks Northshire is his for the taking.$B$BClear the vermin out, break the thugs, and bring me "
              "Padfoot's end. Then we'll talk about what lies beyond the valley.",
              "Slay the kobolds and Defias thugs of Northshire, and Garrick Padfoot.",
              "The valley isn't safe yet, $N.",
              "Well fought. Padfoot was only one of them, though; the Brotherhood is everywhere now. Marshal Dughan "
              "in Goldshire will want a fighter like you.",
              {{ {{ "Kobold Vermin", nullptr, nullptr }, 8 }, {{ "Defias Thug", nullptr, nullptr }, 6 }, {{ nullptr, nullptr, nullptr }, 0 } }},
              "Garrick Padfoot", LEAD_CHAMPION, "Extra Strong", 450, 100,
              {{ "Garrick Padfoot", "Kobold Camp Cleanup", nullptr }} },

            { ACT_QUEST_FIRST + 1, SIDE_ALLIANCE, 12, 5, "Act I: The Road to Goldshire",
              "Marshal McBride", "Marshal Dughan",
              "Follow the road south out of the valley to Goldshire, $N. Marshal Dughan holds the town with too few "
              "men. Tell him Northshire is cleared, and that I've sent him help.",
              "Report to Marshal Dughan in Goldshire.",
              "", "McBride sent you? Good. I can use every blade I can get.",
              {{ {{ nullptr, nullptr, nullptr }, 0 }, {{ nullptr, nullptr, nullptr }, 0 }, {{ nullptr, nullptr, nullptr }, 0 } }},
              nullptr, LEAD_NONE, nullptr, 200, 50, {{ nullptr, nullptr, nullptr }} },

            { ACT_QUEST_FIRST + 2, SIDE_ALLIANCE, 12, 7, "Act I: The Mines of Elwynn",
              "Marshal Dughan", "Marshal Dughan",
              "The kobolds have taken Fargodeep Mine, just south of here, and they're led by a greedy brute the "
              "miners call Goldtooth. Every day they dig deeper and every night they raid the farms.$B$BGo into "
              "Fargodeep, $N. Break their tunnellers and miners, and put an end to Goldtooth.",
              "Slay the kobolds of Fargodeep Mine, and Goldtooth.",
              "Fargodeep's still crawling with them.",
              "Goldtooth, dead. The miners will sing about this one. But the kobolds were never our real problem.",
              {{ {{ "Kobold Tunneler", nullptr, nullptr }, 8 }, {{ "Kobold Miner", nullptr, nullptr }, 8 }, {{ nullptr, nullptr, nullptr }, 0 } }},
              "Goldtooth", LEAD_CHAMPION, "Stone Skin", 900, 500,
              {{ "Goldtooth", "Kobold Candles", nullptr }} },

            { ACT_QUEST_FIRST + 3, SIDE_ALLIANCE, 12, 8, "Act I: Fire on the Farms",
              "Marshal Dughan", "Marshal Dughan",
              "There it is. The Defias Brotherhood is burning the farms east of Goldshire: the Brackwell pumpkin "
              "patch, the Maclure vineyards. Good folk driven off their land so the Brotherhood can take it.$B$BDrive "
              "them out, $N, and find whoever is giving the orders.",
              "Drive the Defias from the Brackwell Pumpkin Patch and the Maclure Vineyards, and slay their leader.",
              "The farms are still burning.",
              "That's one less Defias captain. They're not bandits, $N; they're an army, and they answer to someone.",
              {{ {{ "Defias Bandit", "Defias Cutpurse", nullptr }, 10 }, {{ "Defias Rogue Wizard", nullptr, nullptr }, 4 }, {{ nullptr, nullptr, nullptr }, 0 } }},
              "Morgan the Collector", LEAD_CHAMPION, "Vampiric", 1000, 600,
              {{ nullptr, nullptr, nullptr }} },

            { ACT_QUEST_FIRST + 4, SIDE_ALLIANCE, 12, 9, "Act I: Jasperlode",
              "Marshal Dughan", "Marshal Dughan",
              "More kobolds in Jasperlode Mine, to the north-east past the river, and these ones have their "
              "geomancers with them. The Eastvale loggers won't go near the hills.$B$BClear Jasperlode, $N.",
              "Slay the kobolds of Jasperlode Mine.",
              "The loggers are still too scared to work.",
              "The road east is open again. You've earned a rest, but I've one more job, and it's the worst of them.",
              {{ {{ "Kobold Geomancer", nullptr, nullptr }, 8 }, {{ "Kobold Laborer", "Kobold Miner", nullptr }, 8 }, {{ nullptr, nullptr, nullptr }, 0 } }},
              nullptr, LEAD_NONE, nullptr, 1100, 700,
              {{ "Protect the Frontier", nullptr, nullptr }} },

            { ACT_QUEST_FIRST + 5, SIDE_ALLIANCE, 12, 10, "Act I: Hogger",
              "Marshal Dughan", "Marshal Dughan",
              "The Riverpaw gnolls have crossed into Elwynn from the west, and they follow a beast called Hogger. "
              "He's killed guards, farmers, and three of the brave fools I sent after him. His camp is at Forest's "
              "Edge, on the border with Westfall.$B$BBreak his pack, $N, and end him. Mind his charge.",
              "Slay the Riverpaw gnolls at Forest's Edge, and Hogger.",
              "Hogger still lives?",
              "Hogger, dead at last. Elwynn owes you, $N. But the gnolls only came east because something drove them out "
              "of Westfall.",
              {{ {{ "Riverpaw Gnoll", "Riverpaw Runt", nullptr }, 10 }, {{ "Riverpaw Outrunner", nullptr, nullptr }, 6 }, {{ nullptr, nullptr, nullptr }, 0 } }},
              "Hogger", LEAD_FINALE, "Extra Strong", 1500, 1500,
              {{ "Wanted:  \"Hogger\"", "Wanted: Hogger", nullptr }} },

            { ACT_QUEST_FIRST + 6, SIDE_ALLIANCE, 40, 10, "Act I: The Fields of Westfall",
              "Marshal Dughan", "Gryan Stoutmantle",
              "Westfall's farmers have fled their land, $N, and Stormwind won't lift a finger. Gryan Stoutmantle is "
              "gathering a militia at Sentinel Hill, down the road west past Forest's Edge. He'll need you more than I "
              "do now. Go with the Light.",
              "Report to Gryan Stoutmantle at Sentinel Hill in Westfall.",
              "", "Dughan sent you? Then you're welcome here. Westfall needs every hand it can get.",
              {{ {{ nullptr, nullptr, nullptr }, 0 }, {{ nullptr, nullptr, nullptr }, 0 }, {{ nullptr, nullptr, nullptr }, 0 } }},
              nullptr, LEAD_NONE, nullptr, 300, 100, {{ nullptr, nullptr, nullptr }} },

            // --- Durotar ---
            { ACT_QUEST_FIRST + 20, SIDE_HORDE, 14, 3, "Act I: The Trial of the Valley",
              "Gornek", "Gornek",
              "Every warrior of the Horde earns their place, $N. The Valley of Trials is where you earn yours. Boars "
              "trample the training grounds, the warlocks' familiars run wild, and a great scorpid called Sarkoth "
              "nests in the north.$B$BCull them. Bring me Sarkoth's end.",
              "Slay the boars and familiars of the Valley of Trials, and Sarkoth.",
              "The trial isn't over, $N.",
              "You've passed. Go to Razor Hill, east past the gate; Gar'Thok will have use for you.",
              {{ {{ "Mottled Boar", nullptr, nullptr }, 8 }, {{ "Vile Familiar", nullptr, nullptr }, 6 }, {{ nullptr, nullptr, nullptr }, 0 } }},
              "Sarkoth", LEAD_CHAMPION, "Extra Fast", 450, 100,
              {{ "Sarkoth", "Cutting Teeth", nullptr }} },

            { ACT_QUEST_FIRST + 21, SIDE_HORDE, 14, 5, "Act I: The Road to Razor Hill",
              "Gornek", "Gar'Thok",
              "Take the road east out of the valley to Razor Hill, $N. Gar'Thok guards it, and he's short of warriors. "
              "Tell him Gornek sent you.",
              "Report to Gar'Thok in Razor Hill.",
              "", "Gornek's pick? We'll see. Durotar is no place for the weak.",
              {{ {{ nullptr, nullptr, nullptr }, 0 }, {{ nullptr, nullptr, nullptr }, 0 }, {{ nullptr, nullptr, nullptr }, 0 } }},
              nullptr, LEAD_NONE, nullptr, 200, 50, {{ nullptr, nullptr, nullptr }} },

            { ACT_QUEST_FIRST + 22, SIDE_HORDE, 14, 7, "Act I: Tiragarde Keep",
              "Gar'Thok", "Gar'Thok",
              "Humans from Kul Tiras have built a keep on our shore, south of here. Tiragarde, they call it. Their "
              "sailors and marines raid our caravans, and their lieutenant, Benedict, thinks he can hold Durotar.$B$BTake "
              "the keep apart, $N. Bring me Benedict's head.",
              "Slay the Kul Tiras sailors and marines at Tiragarde Keep, and Lieutenant Benedict.",
              "The humans still hold the keep.",
              "Benedict, dead. The shore is ours again. But there's worse than humans in Durotar.",
              {{ {{ "Kul Tiras Sailor", nullptr, nullptr }, 10 }, {{ "Kul Tiras Marine", nullptr, nullptr }, 8 }, {{ nullptr, nullptr, nullptr }, 0 } }},
              "Lieutenant Benedict", LEAD_CHAMPION, "Extra Strong", 900, 500,
              {{ nullptr, nullptr, nullptr }} },

            { ACT_QUEST_FIRST + 23, SIDE_HORDE, 14, 8, "Act I: The Burning Blade",
              "Gar'Thok", "Gar'Thok",
              "A demon cult, the Burning Blade, hides in the caves of Skull Rock, north of Orgrimmar's road. They "
              "worship the very fel that enslaved our people. Their warlock Gazz'uz leads them.$B$BBurn them out, $N.",
              "Slay the Burning Blade at Skull Rock, and Gazz'uz.",
              "The cult still festers in Skull Rock.",
              "Gazz'uz is dead and his cult broken. You fight like a true orc, $N.",
              {{ {{ "Burning Blade Cultist", "Burning Blade Apprentice", nullptr }, 10 }, {{ "Burning Blade Neophyte", "Burning Blade Fanatic", nullptr }, 6 }, {{ nullptr, nullptr, nullptr }, 0 } }},
              "Gazz'uz", LEAD_CHAMPION, "Fire Enchanted", 1000, 600,
              {{ "Skull Rock", nullptr, nullptr }} },

            { ACT_QUEST_FIRST + 24, SIDE_HORDE, 14, 10, "Act I: Zalazane",
              "Gar'Thok", "Gar'Thok",
              "Our troll allies have lost the Echo Isles to one of their own, the witch doctor Zalazane. He hexes "
              "his own people into mindless slaves and sends them against Sen'jin Village.$B$BCross to the Echo Isles, "
              "east of Sen'jin, $N. Free the hexed by the blade, and end Zalazane.",
              "Slay the hexed trolls of the Echo Isles, and Zalazane.",
              "Zalazane still rules the isles.",
              "Zalazane is dead, and the Darkspear have their home back. Durotar owes you, $N. The Barrens are calling.",
              {{ {{ "Hexed Troll", nullptr, nullptr }, 10 }, {{ "Voodoo Troll", nullptr, nullptr }, 6 }, {{ nullptr, nullptr, nullptr }, 0 } }},
              "Zalazane", LEAD_FINALE, "Healer", 1500, 1500,
              {{ "Zalazane", nullptr, nullptr }} },

            { ACT_QUEST_FIRST + 25, SIDE_HORDE, 17, 10, "Act I: The Crossroads",
              "Gar'Thok", "Thork",
              "The Barrens lie west of Orgrimmar, across the river. The Crossroads hold the road through them, and Thork "
              "commands there. Raiders and quilboar press on every side. Go, $N. You're needed.",
              "Report to Thork at the Crossroads in the Barrens.",
              "", "Gar'Thok sends his best? Good. The Barrens eat the weak.",
              {{ {{ nullptr, nullptr, nullptr }, 0 }, {{ nullptr, nullptr, nullptr }, 0 }, {{ nullptr, nullptr, nullptr }, 0 } }},
              nullptr, LEAD_NONE, nullptr, 300, 100, {{ nullptr, nullptr, nullptr }} },
        };
        return quests;
    }

    bool ActsOn()
    {
        return sWorld.getConfig(CONFIG_BOOL_ARPG_ENABLE) && sWorld.getConfig(CONFIG_BOOL_ARPG_ACTS);
    }

    uint32 MapOf(ActQuest const& q)
    {
        return q.side == SIDE_ALLIANCE ? 0 : 1;
    }

    struct Spot { float x, y, z; };

    // Every spawn by entry, on the two continents, built once.
    std::mutex sSpawnsLock;
    std::unordered_map<uint32, std::vector<Spot>> sSpawns[2];
    bool sSpawnsBuilt = false;

    void BuildSpawns()
    {
        std::lock_guard<std::mutex> guard(sSpawnsLock);
        if (sSpawnsBuilt)
            return;
        struct Worker
        {
            bool operator()(CreatureDataPair const& pair)
            {
                CreatureData const& d = pair.second;
                if (d.mapid <= 1 && d.id)
                    sSpawns[d.mapid][d.id].push_back({ d.posX, d.posY, d.posZ });
                return false;
            }
        } worker;
        sObjectMgr.DoCreatureData(worker);
        sSpawnsBuilt = true;
    }

    std::vector<Spot> const* SpawnsOf(uint32 map, uint32 entry)
    {
        if (map > 1)
            return nullptr;
        auto it = sSpawns[map].find(entry);
        return it == sSpawns[map].end() ? nullptr : &it->second;
    }

    // The creature named `name` that spawns on `map` (the one with the most spawns there), or 0.
    uint32 EntryNamed(char const* name, uint32 map)
    {
        if (!name)
            return 0;
        uint32 best = 0;
        size_t most = 0;
        for (uint32 id = 1; id < sCreatureStorage.GetMaxEntry(); ++id)
        {
            CreatureInfo const* info = sCreatureStorage.LookupEntry<CreatureInfo>(id);
            if (!info || !info->Name || std::strcmp(info->Name, name) != 0)
                continue;
            std::vector<Spot> const* spawns = SpawnsOf(map, id);
            size_t const n = spawns ? spawns->size() : 0;
            if (!best || n > most)
            {
                best = id;
                most = n;
            }
        }
        return most ? best : 0;
    }

    uint32 FirstNamed(Kill const& kill, uint32 map)
    {
        for (char const* name : kill.names)
            if (uint32 const entry = EntryNamed(name, map))
                return entry;
        return 0;
    }

    float Dist2d(Spot const& a, Spot const& b)
    {
        return std::hypot(a.x - b.x, a.y - b.y);
    }

    // The resolved act: each quest's entries and its road's ends.
    struct Resolved
    {
        uint32 giver = 0, taker = 0, leader = 0;
        std::array<uint32, 3> kills{};
        Spot from{}, to{};
        bool hasRoad = false;
    };
    std::vector<Resolved> sResolved;
    std::set<uint32> sLeaders, sFinales;
    std::map<uint32, std::string> sLeaderAffix;

    // Where a quest sends its character: its leader's spawn, or the middle of its first creature's
    // spawns nearest the giver, or (a report) the taker.
    bool TargetOf(ActQuest const& q, Resolved const& r, Spot& out)
    {
        uint32 const map = MapOf(q);
        auto first = [map](uint32 entry, Spot& at)
        {
            std::vector<Spot> const* spawns = SpawnsOf(map, entry);
            if (!spawns || spawns->empty())
                return false;
            at = spawns->front();
            return true;
        };
        if (r.leader && first(r.leader, out))
            return true;
        if (r.kills[0])
            if (std::vector<Spot> const* spawns = SpawnsOf(map, r.kills[0]))
            {
                std::vector<Spot> closest(*spawns);
                std::sort(closest.begin(), closest.end(), [&r](Spot const& a, Spot const& b) { return Dist2d(a, r.from) < Dist2d(b, r.from); });
                size_t const n = std::min<size_t>(closest.size(), 8);
                if (n)
                {
                    Spot sum{ 0, 0, 0 };
                    for (size_t i = 0; i < n; ++i)
                    {
                        sum.x += closest[i].x;
                        sum.y += closest[i].y;
                        sum.z += closest[i].z;
                    }
                    out = { sum.x / n, sum.y / n, sum.z / n };
                    return true;
                }
            }
        return r.taker && r.taker != r.giver && first(r.taker, out);
    }

    // --- The road ---

    constexpr float ROAD_STEP = 45.0f;          // a corridor point every this far along the road
    constexpr float ROAD_SKIP = 35.0f;          // none this close to its ends (a town, the target itself)
    constexpr float ROAD_POOL = 60.0f;          // a point's packs: creatures that live this close to it
    constexpr float FILL_NEAR = 110.0f, FILL_FAR = 170.0f; // filled ahead, just out of sight
    constexpr float FILL_CLEAR = 15.0f;         // no creature this close to a point filled
    constexpr float LEAVE_FAR = 260.0f;         // a corridor pack this far behind leaves
    constexpr uint32 UPDATE_MS = 2000;
    // A road pack left alone (its character gone, or past it) goes after this long out of combat;
    // its corpse lies as long.
    constexpr uint32 PACK_IDLE_MS = 10 * MINUTE * IN_MILLISECONDS;

    constexpr float LEG = 150.0f;               // the road is drawn a leg this long at a time
    constexpr float LEG_REACH = FILL_FAR + LEG; // a leg is drawn when the character comes this close
    constexpr uint32 LEG_RETRY_MS = 20000;      // a leg the map can't path yet is tried again after this
    constexpr uint32 LEG_TRIES = 6;             // then it is a straight line

    // A stretch of road: drawn when the character nears it, on its map's thread, once the map's
    // navigation tiles there are loaded.
    struct Leg
    {
        Spot a, b;
        bool built = false;
        uint32 tries = 0;
        uint32 triedAt = 0;
        std::vector<Spot> points;
        std::vector<uint32> entries;            // the creature each point fills with (0: none)
    };
    struct Road
    {
        bool laid = false;
        std::vector<Leg> legs;
    };
    std::mutex sRoadsLock;
    std::map<uint32, Road> sRoads;              // by quest

    bool Hostile(CreatureInfo const* info)
    {
        if (!info || info->NpcFlags || (info->ExtraFlags & CREATURE_EXTRA_FLAG_CIVILIAN) ||
                info->CreatureType == CREATURE_TYPE_CRITTER || info->Rank != CREATURE_ELITE_NORMAL || sLeaders.count(info->Entry))
            return false;
        FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(info->Faction);
        return faction && faction->IsHostileToPlayers();
    }

    // `leg`'s way and points, if the map can path it now (true when the leg is done).
    bool BuildLeg(ActQuest const& q, Leg& leg, bool first, bool last, Player* player)
    {
        uint32 const now = WorldTimer::getMSTime();
        if (leg.tries && WorldTimer::getMSTimeDiff(leg.triedAt, now) < LEG_RETRY_MS)
            return false;
        if (player->GetTransport())
            return false;                       // a transport's own navigation mesh would be read
        leg.triedAt = now;
        ++leg.tries;
        std::vector<Spot> way;
        PathFinder path(player);
        path.calculate(Vector3(leg.a.x, leg.a.y, leg.a.z), Vector3(leg.b.x, leg.b.y, leg.b.z), false);
        uint32 const bad = PATHFIND_NOPATH | PATHFIND_NOT_USING_PATH | PATHFIND_SHORTCUT;
        if (!(path.getPathType() & bad) && path.getPath().size() >= 2)
        {
            for (auto const& p : path.getPath())
                way.push_back({ p.x, p.y, p.z });
            // Too winding for the path's length limit (retrying can't help): its walkable start,
            // then straight on to the leg's end.
            if (path.getPathType() & PATHFIND_SHORT)
                way.push_back(leg.b);
        }
        else if (leg.tries < LEG_TRIES)
            return false;                       // the tiles may not be loaded yet: try again later
        else
            way = { leg.a, leg.b };
        float total = 0.0f;
        for (size_t i = 1; i < way.size(); ++i)
            total += Dist2d(way[i - 1], way[i]);
        // A point every ROAD_STEP along it, none within ROAD_SKIP of the road's own ends.
        float const from = first ? ROAD_SKIP : ROAD_STEP * 0.5f;
        float const to = last ? total - ROAD_SKIP : total;
        float walked = 0.0f, next = from;
        for (size_t i = 1; i < way.size() && next < to; ++i)
        {
            float const span = Dist2d(way[i - 1], way[i]);
            while (span > 0.0f && next <= walked + span && next < to)
            {
                float const t = (next - walked) / span;
                leg.points.push_back({ way[i - 1].x + (way[i].x - way[i - 1].x) * t, way[i - 1].y + (way[i].y - way[i - 1].y) * t,
                                       way[i - 1].z + (way[i].z - way[i - 1].z) * t });
                next += ROAD_STEP;
            }
            walked += span;
        }
        // Each point fills with the nearest hostile creature that lives along the road, near the
        // quest's level (never an act's leader).
        uint32 const map = MapOf(q);
        for (Spot const& point : leg.points)
        {
            uint32 best = 0;
            float bestDist = ROAD_POOL;
            for (auto const& [entry, spawns] : sSpawns[map])
            {
                CreatureInfo const* info = sCreatureStorage.LookupEntry<CreatureInfo>(entry);
                if (!Hostile(info) || info->MaxLevel + 3 < q.level || info->MinLevel > q.level + 3u)
                    continue;
                for (Spot const& s : spawns)
                {
                    float const d = Dist2d(point, s);
                    if (d < bestDist)
                    {
                        bestDist = d;
                        best = entry;
                    }
                }
            }
            leg.entries.push_back(best);
        }
        leg.built = true;
        return true;
    }

    // `quest`'s road: its legs laid the first time it is asked for, each drawn as the character
    // nears it (called on `player`'s map thread).
    Road& RoadOf(size_t index, Player* player)
    {
        ActQuest const& q = Quests()[index];
        Road& road = sRoads[q.id];
        Resolved const& r = sResolved[index];
        if (!road.laid)
        {
            road.laid = true;
            if (r.hasRoad)
            {
                // Leg ends on the straight line, set on the ground; the pathfinding finds the way
                // between them.
                Map* map = player->GetMap();
                uint32 const n = std::max(1u, uint32(std::ceil(Dist2d(r.from, r.to) / LEG)));
                Spot prev = r.from;
                for (uint32 i = 1; i <= n; ++i)
                {
                    float const t = float(i) / n;
                    Spot at = r.to;
                    if (i < n)
                    {
                        at = { r.from.x + (r.to.x - r.from.x) * t, r.from.y + (r.to.y - r.from.y) * t,
                               r.from.z + (r.to.z - r.from.z) * t };
                        float const ground = map->GetHeight(at.x, at.y, at.z + 40.0f);
                        if (ground > INVALID_HEIGHT)
                            at.z = ground;
                    }
                    Leg leg;
                    leg.a = prev;
                    leg.b = at;
                    road.legs.push_back(leg);
                    prev = at;
                }
            }
        }
        for (size_t l = 0; l < road.legs.size(); ++l)
        {
            Leg& leg = road.legs[l];
            if (leg.built || std::min(player->GetDistance2d(leg.a.x, leg.a.y), player->GetDistance2d(leg.b.x, leg.b.y)) > LEG_REACH)
                continue;
            if (BuildLeg(q, leg, l == 0, l + 1 == road.legs.size(), player))
                sLog.outDetail("ARPG acts: \"%s\" leg %u of %u drawn: %u points, %u with packs", q.title, uint32(l + 1),
                               uint32(road.legs.size()), uint32(leg.points.size()),
                               uint32(std::count_if(leg.entries.begin(), leg.entries.end(), [](uint32 e) { return e != 0; })));
        }
        return road;
    }

    // A character's road under way: the quest, the points filled, the packs placed.
    struct Run
    {
        uint32 quest = 0;
        std::set<uint32> filled;           // leg << 16 | point
        std::vector<ObjectGuid> packs;
        uint32 lastUpdate = 0;
    };
    std::mutex sRunsLock;
    std::unordered_map<ObjectGuid, Run> sRuns;

    // Whether a pack may stand at `at`: loaded ground, no creature close, out of the character's
    // sight.
    bool SpotClear(Player* player, Spot& at)
    {
        Map* map = player->GetMap();
        if (!map->IsLoaded(at.x, at.y))
            return false;
        float z = at.z;
        player->UpdateAllowedPositionZ(at.x, at.y, z);
        at.z = z;
        UnitList around;
        MaNGOS::AnyUnitInObjectRangeCheck check(player, FILL_FAR + FILL_CLEAR);
        MaNGOS::UnitListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> searcher(around, check);
        Cell::VisitAllObjects(player, searcher, FILL_FAR + FILL_CLEAR);
        for (Unit* unit : around)
            if (unit->IsAlive() && unit->GetDistance(at.x, at.y, at.z) < FILL_CLEAR)
                return false;
        return true;
    }

    void LeaveAll(Player* player, Run& run, bool onlyFar)
    {
        std::vector<ObjectGuid> kept;
        for (ObjectGuid const& guid : run.packs)
        {
            Creature* c = player->IsInWorld() ? player->GetMap()->GetCreature(guid) : nullptr;
            if (!c || !c->IsAlive())
                continue;
            if (c->IsInCombat() || (onlyFar && player->GetDistance(c) < LEAVE_FAR))
            {
                kept.push_back(guid);
                continue;
            }
            c->ForcedDespawn();
        }
        run.packs = std::move(kept);
    }
}

namespace Arpg
{
    void EnsureActQuests()
    {
        // Last start's act quests go first, so turning the acts off takes them out of the world.
        uint32 const last = ACT_QUEST_FIRST + 999;
        WorldDatabase.DirectPExecute("DELETE FROM quest_template WHERE entry BETWEEN %u AND %u", ACT_QUEST_FIRST, last);
        WorldDatabase.DirectPExecute("DELETE FROM creature_questrelation WHERE quest BETWEEN %u AND %u", ACT_QUEST_FIRST, last);
        WorldDatabase.DirectPExecute("DELETE FROM creature_involvedrelation WHERE quest BETWEEN %u AND %u", ACT_QUEST_FIRST, last);
        if (!ActsOn())
            return;
        BuildSpawns();
        std::vector<std::string> columns;
        if (auto result = WorldDatabase.Query("SELECT COLUMN_NAME FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() "
                                              "AND TABLE_NAME = 'quest_template' ORDER BY ORDINAL_POSITION"))
        {
            do
                columns.push_back(result->Fetch()[0].GetCppString());
            while (result->NextRow());
        }
        if (columns.empty())
        {
            sLog.outError("ARPG acts: quest_template's columns could not be read; no act written");
            return;
        }
        auto has = [&columns](char const* column) { return std::find(columns.begin(), columns.end(), column) != columns.end(); };
        std::vector<ActQuest> const& quests = Quests();
        sResolved.assign(quests.size(), Resolved());
        uint32 written = 0;
        std::map<int, uint32> lastOfSide;       // each side's last quest written, for the chain
        for (size_t i = 0; i < quests.size(); ++i)
        {
            ActQuest const& q = quests[i];
            uint32 const map = MapOf(q);
            Resolved& r = sResolved[i];
            r.giver = EntryNamed(q.giver, map);
            r.taker = EntryNamed(q.taker, map);
            if (!r.giver || !r.taker)
            {
                sLog.outError("ARPG acts: \"%s\" has no %s (\"%s\"); left out", q.title, r.giver ? "taker" : "giver",
                              r.giver ? q.taker : q.giver);
                continue;
            }
            std::map<std::string, std::string> row;
            auto put = [&](char const* column, std::string const& value) { if (has(column)) row[column] = value; };
            auto num = [&](char const* column, int64 value) { put(column, std::to_string(value)); };
            auto text = [&](char const* column, char const* value) { put(column, "'" + SqlEscaped(value ? value : "") + "'"); };
            num("entry", q.id);
            num("Method", 2);
            num("ZoneOrSort", q.zone);
            num("MinLevel", 1);
            num("QuestLevel", q.level);
            num("RequiredRaces", q.side == SIDE_ALLIANCE ? RACES_ALLIANCE : RACES_HORDE);
            // The chain: each needs the one written before it on its side (a quest left out is
            // stepped over).
            if (lastOfSide.count(q.side))
                num("PrevQuestId", lastOfSide[q.side]);
            text("Title", q.title);
            text("Details", q.details);
            text("Objectives", q.objectives);
            text("RequestItemsText", q.request);
            text("OfferRewardText", q.reward);
            text("EndText", "");
            for (int o = 1; o <= 4; ++o)
                text(("ObjectiveText" + std::to_string(o)).c_str(), "");
            int slot = 1;
            for (Kill const& kill : q.kills)
            {
                if (!kill.count)
                    continue;
                uint32 const entry = FirstNamed(kill, map);
                r.kills[slot - 1] = entry;
                if (!entry)
                {
                    sLog.outError("ARPG acts: \"%s\" finds no creature named \"%s\"; that objective is left out", q.title,
                                  kill.names[0] ? kill.names[0] : "?");
                    continue;
                }
                num(("ReqCreatureOrGOId" + std::to_string(slot)).c_str(), entry);
                num(("ReqCreatureOrGOCount" + std::to_string(slot)).c_str(), kill.count);
                ++slot;
            }
            if (q.leader)
            {
                r.leader = EntryNamed(q.leader, map);
                if (r.leader && slot <= 4)
                {
                    num(("ReqCreatureOrGOId" + std::to_string(slot)).c_str(), r.leader);
                    num(("ReqCreatureOrGOCount" + std::to_string(slot)).c_str(), 1);
                }
                else
                    sLog.outError("ARPG acts: \"%s\" finds no leader named \"%s\"", q.title, q.leader);
            }
            num("RewOrReqMoney", q.money);
            // Vanilla's quest XP is RewMoneyMaxLevel / 0.6 at levels to 60 (Quest::XPValue).
            num("RewMoneyMaxLevel", int64(std::llround(q.xp * 0.6)));

            std::string names, values;
            for (auto const& [column, value] : row)
            {
                names += (names.empty() ? "`" : ", `") + column + "`";
                values += (values.empty() ? "" : ", ") + value;
            }
            std::string const sql = "INSERT INTO quest_template (" + names + ") VALUES (" + values + ")";
            if (!WorldDatabase.DirectExecute(sql.c_str()))
            {
                sLog.outError("ARPG acts: \"%s\" could not be written", q.title);
                continue;
            }
            if (r.leader)
            {
                sLeaders.insert(r.leader);
                if (q.lead == LEAD_FINALE)
                    sFinales.insert(r.leader);
                if (q.leaderAffix)
                    sLeaderAffix[r.leader] = q.leaderAffix;
            }
            // The quest before it now leads on to it.
            if (lastOfSide.count(q.side))
                WorldDatabase.DirectPExecute("UPDATE quest_template SET NextQuestInChain = %u WHERE entry = %u", q.id, lastOfSide[q.side]);
            lastOfSide[q.side] = q.id;
            WorldDatabase.DirectPExecute("INSERT INTO creature_questrelation (id, quest) VALUES (%u, %u)", r.giver, q.id);
            WorldDatabase.DirectPExecute("INSERT INTO creature_involvedrelation (id, quest) VALUES (%u, %u)", r.taker, q.id);
            ++written;

            // The road's ends: from the giver to where the quest sends the character.
            if (std::vector<Spot> const* at = SpawnsOf(map, r.giver))
            {
                r.from = at->front();
                r.hasRoad = TargetOf(q, r, r.to) && Dist2d(r.from, r.to) > ROAD_SKIP * 2;
            }
        }
        sLog.outString(">> ARPG acts: %u act quests written", written);
    }

    void AfterActQuestsLoaded()
    {
        if (!ActsOn())
            return;
        // The quests other quests follow on from: taking one off its giver would strand its chain,
        // so those stay.
        std::set<uint32> leadsOn;
        for (auto const& [id, quest] : sObjectMgr.GetQuestTemplates())
        {
            if (!quest)
                continue;
            if (quest->GetPrevQuestId())
                leadsOn.insert(uint32(std::abs(quest->GetPrevQuestId())));
            for (int32 prev : quest->prevQuests)
                leadsOn.insert(uint32(std::abs(prev)));
        }
        std::set<uint32> retold;
        for (ActQuest const& q : Quests())
        {
            uint32 const races = q.side == SIDE_ALLIANCE ? RACES_ALLIANCE : RACES_HORDE;
            for (char const* title : q.retells)
            {
                if (!title)
                    continue;
                for (auto const& [id, quest] : sObjectMgr.GetQuestTemplates())
                {
                    // The act's own side's quest of that title (a race mask of 0 is either side's).
                    if (!quest || id >= ACT_QUEST_FIRST || quest->GetTitle() != title ||
                            (quest->GetRequiredRaces() && !(quest->GetRequiredRaces() & races)))
                        continue;
                    if (leadsOn.count(id) || quest->GetNextQuestId() > 0 || quest->GetNextQuestInChain())
                        sLog.outString("ARPG acts: \"%s\" (%u) leads on to other quests, so it stays beside \"%s\"", title, id, q.title);
                    else
                        retold.insert(id);
                }
            }
        }
        QuestRelationsMap& givers = sObjectMgr.GetCreatureQuestRelationsMap();
        size_t taken = 0;
        for (auto it = givers.begin(); it != givers.end();)
        {
            if (retold.count(it->second))
            {
                it = givers.erase(it);
                ++taken;
            }
            else
                ++it;
        }
        QuestRelationsMap& boards = sObjectMgr.GetGOQuestRelationsMap();
        for (auto it = boards.begin(); it != boards.end();)
        {
            if (retold.count(it->second))
            {
                it = boards.erase(it);
                ++taken;
            }
            else
                ++it;
        }
        sLog.outString(">> ARPG acts: %u vanilla quests an act retells taken off %u givers", uint32(retold.size()), uint32(taken));
    }

    bool IsActLeader(uint32 entry)
    {
        return ActsOn() && sLeaders.count(entry) != 0;
    }

    bool IsActQuest(uint32 quest)
    {
        return quest >= ACT_QUEST_FIRST && quest < ACT_QUEST_FIRST + 1000;
    }

    void OnActCreatureAdded(Creature* creature)
    {
        // Only the world's own spawn: not a pack's copy or a summon.
        if (!creature || !ActsOn() || !sLeaders.count(creature->GetEntry()) || ChampionTier(creature) != 0 ||
                creature->GetSubtype() != CREATURE_SUBTYPE_GENERIC || !creature->HasStaticDBSpawnData())
            return;
        auto affix = sLeaderAffix.find(creature->GetEntry());
        std::vector<std::string> affixes;
        if (affix != sLeaderAffix.end())
            affixes.push_back(affix->second);
        MakeChampion(creature, sFinales.count(creature->GetEntry()) ? 2 : 1, 0, creature->GetName(), affixes);
    }

    bool OnActBossLoot(Creature* victim)
    {
        if (!victim || !victim->m_loot || !ActsOn() || !sFinales.count(victim->GetEntry()) ||
                victim->GetSubtype() != CREATURE_SUBTYPE_GENERIC || !victim->HasStaticDBSpawnData())
            return false;
        AddLootPick(victim->m_loot, RARITY_BLUE, victim->GetLevel() + 2);
        return true;
    }

    void UpdateActs(Player* player)
    {
        if (!player || !ActsOn() || !Active(player) || !player->IsInWorld() || !player->IsAlive())
            return;
        uint32 const now = WorldTimer::getMSTime();
        // The act quest under way, if any.
        std::vector<ActQuest> const& quests = Quests();
        size_t index = quests.size();
        for (size_t i = 0; i < quests.size(); ++i)
            // Under way: taken and not yet handed in (a report quest is complete as soon as it is taken).
            if (i < sResolved.size() && sResolved[i].hasRoad && MapOf(quests[i]) == player->GetMapId() &&
                    (player->GetQuestStatus(quests[i].id) == QUEST_STATUS_INCOMPLETE ||
                     (player->GetQuestStatus(quests[i].id) == QUEST_STATUS_COMPLETE && !player->GetQuestRewardStatus(quests[i].id))))
            {
                index = i;
                break;
            }
        std::lock_guard<std::mutex> guard(sRunsLock);
        Run& run = sRuns[player->GetObjectGuid()];
        if (run.lastUpdate && WorldTimer::getMSTimeDiff(run.lastUpdate, now) < UPDATE_MS)
            return;
        run.lastUpdate = now ? now : 1;
        uint32 const quest = index < quests.size() ? quests[index].id : 0;
        if (quest != run.quest)
        {
            run.quest = quest;
            run.filled.clear();
        }
        LeaveAll(player, run, true);
        if (!quest)
            return;
        std::lock_guard<std::mutex> roadGuard(sRoadsLock);
        Road& road = RoadOf(index, player);
        for (size_t l = 0; l < road.legs.size(); ++l)
            for (size_t p = 0; p < road.legs[l].points.size(); ++p)
            {
                uint32 const key = uint32(l << 16 | p);
                if (!road.legs[l].entries[p] || run.filled.count(key))
                    continue;
                Spot at = road.legs[l].points[p];
                float const d = player->GetDistance2d(at.x, at.y);
                if (d < FILL_NEAR || d > FILL_FAR || !SpotClear(player, at))
                    continue;
                TempSpawnSettings settings;
                settings.entry = road.legs[l].entries[p];
                settings.x = at.x;
                settings.y = at.y;
                settings.z = at.z;
                settings.ori = frand(0.0f, 2.0f * float(M_PI));
                settings.spawnType = TEMPSPAWN_TIMED_OOC_DESPAWN;
                settings.despawnTime = PACK_IDLE_MS;
                Creature* leader = WorldObject::SummonCreature(settings, player->GetMap());
                run.filled.insert(key);
                if (!leader)
                    continue;
                run.packs.push_back(leader->GetObjectGuid());
                FormRoadPack(leader);
            }
    }

    void ForgetActs(Player* player)
    {
        if (!player)
            return;
        std::lock_guard<std::mutex> guard(sRunsLock);
        auto it = sRuns.find(player->GetObjectGuid());
        if (it == sRuns.end())
            return;
        LeaveAll(player, it->second, false);
        sRuns.erase(it);
    }

    bool OnActChat(Player* player, std::string const& text)
    {
        if (!player || !Active(player) || !sWorld.getConfig(CONFIG_BOOL_ARPG_DEV_TOOLS) || !sWorld.getConfig(CONFIG_BOOL_ARPG_STATS) ||
                text.compare(0, 6, "#note ") != 0)
            return false;
        OnStatsNote(player, text.substr(6));
        return true;
    }
}
