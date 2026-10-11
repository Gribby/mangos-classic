/*
 * ARPG zone events and bounties: see ArpgEvents.h.
 */

#include "Arpg/ArpgEvents.h"
#include "Arpg/ArpgAffixes.h"
#include "Arpg/ArpgCombat.h"
#include "Arpg/ArpgCraft.h"
#include "Arpg/ArpgDungeons.h"
#include "Arpg/ArpgPacks.h"
#include "Arpg/ArpgTree.h"

#include "Chat/Chat.h"
#include "Database/DatabaseEnv.h"
#include "Entities/Creature.h"
#include "Entities/Player.h"
#include "Groups/Group.h"
#include "Loot/LootMgr.h"
#include "Maps/Map.h"
#include "MotionGenerators/MotionMaster.h"
#include "Server/DBCStores.h"
#include "Server/Opcodes.h"
#include "Server/WorldPacket.h"
#include "Server/WorldSession.h"
#include "Tools/Formulas.h"
#include "Util/Timer.h"
#include "World/World.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    using namespace Arpg;

    // --- Zone events ---

    enum EventType : uint8 { EVENT_PURGE = 1, EVENT_HUNT = 2, EVENT_SIEGE = 3 };
    enum EventState : uint8 { STATE_LIVE = 1, STATE_DONE = 2, STATE_FAILED = 3, STATE_GONE = 4 };

    // The spot's size, how far from the player it may be, and the creatures' levels around it.
    constexpr float EVENT_RADIUS = 45.0f;
    constexpr float ANCHOR_MIN = 45.0f, ANCHOR_MAX = 130.0f;
    constexpr int32 LEVEL_BELOW = 4, LEVEL_ABOVE = 3;
    // When the first comes after a player enters a zone, and between one and the next (ms).
    constexpr uint32 EVENT_FIRST_MIN = 45000, EVENT_FIRST_MAX = 90000;
    constexpr uint32 EVENT_COOLDOWN_MIN = 180000, EVENT_COOLDOWN_MAX = 360000;
    constexpr uint32 EVENT_RETRY = 30000;          // no spot found: look again
    constexpr uint32 EVENT_TICK = 500;             // the director's own pace, per zone
    constexpr uint32 PLAYER_TICK = 1000;           // each player's call, at most
    constexpr uint32 SEND_EVERY = 3000;            // the zone told again, for the clock
    constexpr uint32 RESULT_SHOWN = 8000;          // a finished event stays on screen this long
    constexpr uint32 PURGE_MS = 180000, HUNT_MS = 300000, SIEGE_MS = 240000;
    constexpr uint32 SIEGE_WAVES = 3, SIEGE_WAVE_GAP = 30000;
    constexpr float SIEGE_RING = 26.0f;
    constexpr uint32 SIEGE_BREATH = 5000;          // a cleared wave's pause before the next
    constexpr uint32 SUMMON_LIFE = 6 * MINUTE * IN_MILLISECONDS;
    // Who shares in the end: fought in it, or stands this near.
    constexpr float SHARE_RANGE = 120.0f;
    // The XP of a finished event, in thousandths of the player's level: purge, hunt, siege.
    constexpr uint32 EVENT_XP_PERMILLE[4] = { 0, 80, 100, 120 };
    // The cache: orbs, and the purple and unique chances (percent).
    constexpr uint32 CACHE_ORBS_MIN = 2, CACHE_ORBS_MAX = 4;
    constexpr uint32 CACHE_PURPLE_PCT = 20, CACHE_UNIQUE_PCT = 8;

    struct ZoneEvent
    {
        uint32 id = 0;
        uint8 type = 0;
        uint8 state = STATE_LIVE;
        uint32 map = 0, zone = 0;
        float x = 0, y = 0, z = 0;
        uint32 entry = 0, level = 1;
        uint32 endsAt = 0, doneAt = 0, lastTick = 0, lastSent = 0;
        uint16 have = 0, need = 0;
        uint32 wave = 0, nextWaveAt = 0;
        ObjectGuid anchor, target;
        std::vector<ObjectGuid> spawned;
        std::set<ObjectGuid> fighters;
        std::string title, objective;
        bool dirty = true;
    };

    // Every map's thread reaches the events (a kill in any zone), so one lock guards them all; it
    // is held through a whole update or kill, and taken again inside (recursive). The bounty lock
    // is only ever taken inside it, never the other way round.
    std::recursive_mutex sEventsLock;
    std::map<std::pair<uint32, uint32>, ZoneEvent> sEvents;          // by (map, zone)
    std::map<std::pair<uint32, uint32>, uint32> sNextAt;             // the next start, by (map, zone)
    std::unordered_map<ObjectGuid, uint32> sPlayerTick;              // a player's last call
    std::unordered_map<ObjectGuid, uint32> sShownTo;                 // the event id a player was last sent
    std::set<ObjectGuid> sCaches;                                    // corpses owed a cache
    std::unordered_map<ObjectGuid, std::pair<uint32, uint32>> sCacheLevel;   // its level, and when
    constexpr uint32 CACHE_KEPT = 60000;           // a cache no loot claimed (a corpse with none) goes
    uint32 sNextId = 1;

    bool EventsOn()
    {
        return sWorld.getConfig(CONFIG_BOOL_ARPG_ENABLE);
    }

    bool Later(uint32 a, uint32 b) { return int32(a - b) > 0; }

    std::string AreaName(uint32 area)
    {
        AreaTableEntry const* entry = GetAreaEntryByAreaID(area);
        return entry && entry->area_name[0] && *entry->area_name[0] ? entry->area_name[0] : "here";
    }

    bool InZone(Player* player, ZoneEvent const& ev)
    {
        return player->IsInWorld() && player->GetMapId() == ev.map && player->GetZoneId() == ev.zone && Active(player);
    }

    void Send(Player* player, ZoneEvent const& ev, uint32 now)
    {
        WorldPacket data(SMSG_ARPG_EVENT, 64 + ev.title.size() + ev.objective.size());
        data << uint8(ev.state);
        data << uint32(ev.id);
        data << uint8(ev.type);
        data << ev.x << ev.y << ev.z;
        data << float(EVENT_RADIUS);
        data << uint16(ev.have) << uint16(ev.need);
        data << uint32(ev.state == STATE_LIVE && Later(ev.endsAt, now) ? ev.endsAt - now : 0);
        data << ev.title << ev.objective;
        player->GetSession()->SendPacket(data);
        sShownTo[player->GetObjectGuid()] = ev.id;
    }

    void SendGone(Player* player)
    {
        WorldPacket data(SMSG_ARPG_EVENT, 40);
        data << uint8(STATE_GONE) << uint32(0) << uint8(0) << 0.0f << 0.0f << 0.0f << 0.0f;
        data << uint16(0) << uint16(0) << uint32(0) << "" << "";
        player->GetSession()->SendPacket(data);
        sShownTo.erase(player->GetObjectGuid());
    }

    void TellZone(Map* map, ZoneEvent const& ev, char const* text)
    {
        for (auto const& ref : map->GetPlayers())
            if (Player* player = ref.getSource())
                if (InZone(player, ev))
                    ChatHandler(player).PSendSysMessage("|cffff8000%s|r", text);
    }

    void SendZone(Map* map, ZoneEvent& ev, uint32 now)
    {
        for (auto const& ref : map->GetPlayers())
            if (Player* player = ref.getSource())
                if (InZone(player, ev))
                    Send(player, ev, now);
        ev.lastSent = now;
        ev.dirty = false;
    }

    // A spot `dist` yards from (x, y, z) in a random direction, on the ground and in sight of it.
    bool SpotNear(Creature* from, float x, float y, float z, float dist, float& ox, float& oy, float& oz)
    {
        for (int tries = 0; tries < 6; ++tries)
        {
            float const angle = float(rand_norm()) * 2.0f * float(M_PI);
            float const d = dist * (0.4f + 0.6f * float(rand_norm()));
            ox = x + std::cos(angle) * d;
            oy = y + std::sin(angle) * d;
            oz = z;
            from->UpdateAllowedPositionZ(ox, oy, oz);
            if (std::abs(oz - z) < 12.0f && from->IsWithinLOS(ox, oy, oz + 1.5f))
                return true;
        }
        return false;
    }

    Creature* Summon(Creature* anchor, ZoneEvent& ev, float x, float y, float z)
    {
        Creature* c = anchor->SummonCreature(ev.entry, x, y, z, anchor->GetAngle(x, y) + float(M_PI),
                                             TEMPSPAWN_TIMED_OOC_OR_DEAD_DESPAWN, SUMMON_LIFE);
        if (c)
            ev.spawned.push_back(c->GetObjectGuid());
        return c;
    }

    // Whether `c` may anchor an event for a player of `level`: a plain hostile creature, nothing
    // a quest, a script or a town hangs on.
    bool Anchorable(Creature* c, Player* player)
    {
        if (!c->IsAlive() || c->IsInCombat() || c->GetScriptId() || c->GetCreatureInfo()->NpcFlags ||
                c->IsCritter() || c->IsPet() || c->IsTotem() || c->GetOwnerGuid() || c->IsTemporarySummon() ||
                c->IsWorldBoss() || ChampionTier(c) || IsPackFollower(c))
            return false;
        if (!c->CanAttack(player) || c->GetCreatureInfo()->Rank > CREATURE_ELITE_ELITE)
            return false;
        int32 const diff = int32(c->GetLevel()) - int32(player->GetLevel());
        if (diff < -LEVEL_BELOW || diff > LEVEL_ABOVE)
            return false;
        float const dist = player->GetDistance(c);
        return dist >= ANCHOR_MIN && dist <= ANCHOR_MAX;
    }

    bool Start(Player* player, uint32 now)
    {
        std::vector<Creature*> options;
        for (Unit* unit : EnemiesNear(player, ANCHOR_MAX))
            if (unit->GetTypeId() == TYPEID_UNIT && Anchorable(static_cast<Creature*>(unit), player))
                options.push_back(static_cast<Creature*>(unit));
        if (options.empty())
            return false;
        Creature* anchor = options[urand(0, uint32(options.size() - 1))];

        ZoneEvent ev;
        ev.id = sNextId++;
        ev.map = player->GetMapId();
        ev.zone = player->GetZoneId();
        ev.x = anchor->GetPositionX();
        ev.y = anchor->GetPositionY();
        ev.z = anchor->GetPositionZ();
        ev.entry = anchor->GetEntry();
        ev.level = anchor->GetLevel();
        ev.anchor = anchor->GetObjectGuid();
        std::string const where = AreaName(anchor->GetAreaId());
        std::string const kind = anchor->GetName();
        uint32 const roll = urand(1, 100);
        ev.type = roll <= 45 ? EVENT_PURGE : roll <= 75 ? EVENT_HUNT : EVENT_SIEGE;
        char buf[160];
        switch (ev.type)
        {
            case EVENT_PURGE:
            {
                ev.need = uint16(urand(10, 14));
                ev.endsAt = now + PURGE_MS;
                // The camp fills out: half as many again of the anchor's kind.
                for (uint32 i = 0; i < ev.need / 2u; ++i)
                {
                    float x, y, z;
                    if (SpotNear(anchor, ev.x, ev.y, ev.z, 30.0f, x, y, z))
                        if (Creature* c = Summon(anchor, ev, x, y, z))
                            c->GetMotionMaster()->MoveRandomAroundPoint(x, y, z, 6.0f);
                }
                std::snprintf(buf, sizeof(buf), "Purge near %s", where.c_str());
                ev.title = buf;
                std::snprintf(buf, sizeof(buf), "Slay %u enemies around %s", uint32(ev.need), kind.c_str());
                ev.objective = buf;
                break;
            }
            case EVENT_HUNT:
            {
                ev.need = 1;
                ev.endsAt = now + HUNT_MS;
                Creature* rare = Summon(anchor, ev, ev.x, ev.y, ev.z);
                if (!rare)
                    return false;
                MakeChampion(rare, 2, ChampionAffixCount(rare->GetLevel(), true));
                rare->GetMotionMaster()->MoveRandomAroundPoint(ev.x, ev.y, ev.z, 22.0f);
                ev.target = rare->GetObjectGuid();
                for (uint32 i = 0; i < 3; ++i)
                {
                    float x, y, z;
                    if (SpotNear(anchor, ev.x, ev.y, ev.z, 6.0f, x, y, z))
                        if (Creature* c = Summon(anchor, ev, x, y, z))
                            c->GetMotionMaster()->MoveFollow(rare, 2.5f, float(i) * 2.1f);
                }
                std::string name = ChampionName(rare);
                if (name.empty())
                    name = rare->GetName();
                std::snprintf(buf, sizeof(buf), "Hunt: %s", name.c_str());
                ev.title = buf;
                std::snprintf(buf, sizeof(buf), "Slay %s and its pack near %s", name.c_str(), where.c_str());
                ev.objective = buf;
                break;
            }
            default:
            {
                ev.need = uint16(SIEGE_WAVES);
                ev.endsAt = now + SIEGE_MS;
                ev.nextWaveAt = now + 8000;     // a moment to get there
                std::snprintf(buf, sizeof(buf), "Siege at %s", where.c_str());
                ev.title = buf;
                std::snprintf(buf, sizeof(buf), "Hold out against %u waves of %s", SIEGE_WAVES, kind.c_str());
                ev.objective = buf;
                break;
            }
        }
        Map* map = player->GetMap();
        std::pair<uint32, uint32> const key{ ev.map, ev.zone };
        ZoneEvent* live;
        {
            std::lock_guard<std::recursive_mutex> guard(sEventsLock);
            live = &(sEvents[key] = std::move(ev));
        }
        std::string const said = live->title + ": " + live->objective + ".";
        TellZone(map, *live, said.c_str());
        SendZone(map, *live, now);
        return true;
    }

    // The siege's next wave, from a ring round the spot, at whoever holds it.
    void SiegeWave(Map* map, ZoneEvent& ev, uint32 now)
    {
        Creature* from = map->GetCreature(ev.anchor);
        if (!from)
        {
            // The anchor gone (killed early): any of the summons stands in for the summoning.
            for (ObjectGuid const& guid : ev.spawned)
                if ((from = map->GetCreature(guid)))
                    break;
        }
        if (!from)
            return;
        ++ev.wave;
        uint32 const count = 3 + ev.wave;
        Unit* foe = nullptr;
        float nearest = 80.0f;
        for (auto const& ref : map->GetPlayers())
            if (Player* p = ref.getSource())
                if (InZone(p, ev) && p->IsAlive())
                {
                    float const d = p->GetDistance(ev.x, ev.y, ev.z);
                    if (d < nearest)
                    {
                        nearest = d;
                        foe = p;
                    }
                }
        for (uint32 i = 0; i < count; ++i)
        {
            float const angle = float(i) * 2.0f * float(M_PI) / float(count) + float(rand_norm());
            float x = ev.x + std::cos(angle) * SIEGE_RING, y = ev.y + std::sin(angle) * SIEGE_RING, z = ev.z;
            from->UpdateAllowedPositionZ(x, y, z);
            Creature* c = Summon(from, ev, x, y, z);
            if (!c)
                continue;
            if (ev.wave == SIEGE_WAVES && i == 0)
                MakeChampion(c, 1, ChampionAffixCount(c->GetLevel(), false));
            if (foe && c->AI())
                c->AI()->AttackStart(foe);
            else
                c->GetMotionMaster()->MovePoint(0, ev.x, ev.y, ev.z);
        }
        ev.nextWaveAt = now + SIEGE_WAVE_GAP;
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s: wave %u of %u!", ev.title.c_str(), ev.wave, SIEGE_WAVES);
        TellZone(map, ev, buf);
        ev.dirty = true;
    }

    uint32 AliveSpawned(Map* map, ZoneEvent const& ev, ObjectGuid except)
    {
        uint32 n = 0;
        for (ObjectGuid const& guid : ev.spawned)
            if (guid != except)
                if (Creature* c = map->GetCreature(guid))
                    if (c->IsAlive())
                        ++n;
        return n;
    }

    // The event's leftovers go (not `except`: the final kill, still dying).
    void Despawn(Map* map, ZoneEvent const& ev, ObjectGuid except = ObjectGuid())
    {
        for (ObjectGuid const& guid : ev.spawned)
            if (Creature* c = guid != except ? map->GetCreature(guid) : nullptr)
                if (c->IsAlive() && !c->IsInCombat())
                    c->ForcedDespawn();
    }

    void CreditBountyEvent(Player* player, uint32 zone);

    // The event is won: XP and credit to who shared in it, the cache to `last`'s corpse.
    void Finish(Map* map, ZoneEvent& ev, Creature* last, uint32 now)
    {
        ev.state = STATE_DONE;
        ev.doneAt = now;
        ev.dirty = true;
        if (last)
        {
            sCaches.insert(last->GetObjectGuid());
            sCacheLevel[last->GetObjectGuid()] = { ev.level, now };
        }
        for (auto const& ref : map->GetPlayers())
        {
            Player* p = ref.getSource();
            if (!p || !InZone(p, ev) || !p->IsAlive())
                continue;
            if (!ev.fighters.count(p->GetObjectGuid()) && p->GetDistance(ev.x, ev.y, ev.z) > SHARE_RANGE)
                continue;
            ChatHandler chat(p);
            if (p->GetLevel() < sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL))
            {
                uint32 const xp = std::max<uint32>(1, p->GetUInt32Value(PLAYER_NEXT_LEVEL_XP) * EVENT_XP_PERMILLE[ev.type] / 1000);
                p->GiveXP(xp, nullptr);
                chat.PSendSysMessage("|cffff8000%s complete! +%u experience.|r", ev.title.c_str(), xp);
            }
            else
            {
                uint32 const gold = ev.level * ev.level * 30;
                p->ModifyMoney(int32(gold));
                chat.PSendSysMessage("|cffff8000%s complete! +%ug %us.|r", ev.title.c_str(), gold / 10000, gold / 100 % 100);
            }
            CreditBountyEvent(p, ev.zone);
        }
        Despawn(map, ev, last ? last->GetObjectGuid() : ObjectGuid());
    }

    void Fail(Map* map, ZoneEvent& ev, uint32 now)
    {
        ev.state = STATE_FAILED;
        ev.doneAt = now;
        ev.dirty = true;
        std::string const text = ev.title + " failed.";
        TellZone(map, ev, text.c_str());
        Despawn(map, ev);
    }

    void Tick(Map* map, ZoneEvent& ev, uint32 now)
    {
        if (ev.state != STATE_LIVE)
            return;
        if (!Later(ev.endsAt, now))
        {
            Fail(map, ev, now);
            return;
        }
        if (ev.type == EVENT_HUNT)
        {
            Creature* rare = map->GetCreature(ev.target);
            if (!rare)
                Fail(map, ev, now);     // gone without a kill (despawned)
            else if (!rare->IsAlive())
            {
                // Killed, but not by an ARPG player (a guard, another player): done, no cache.
                ev.have = 1;
                Finish(map, ev, nullptr, now);
            }
        }
        else if (ev.type == EVENT_SIEGE && ev.wave >= SIEGE_WAVES && !AliveSpawned(map, ev, ObjectGuid()))
        {
            // The last wave fell to something else (OnEventKill finishes it otherwise).
            ev.have = uint16(SIEGE_WAVES);
            Finish(map, ev, nullptr, now);
        }
        else if (ev.type == EVENT_SIEGE && ev.wave < SIEGE_WAVES)
        {
            // A wave cleared counts, and brings the next on sooner (a breath first); otherwise
            // the next comes when its time does, cleared or not.
            if (ev.wave > 0 && !AliveSpawned(map, ev, ObjectGuid()))
            {
                if (ev.have < ev.wave)
                {
                    ev.have = uint16(ev.wave);
                    ev.dirty = true;
                }
                if (Later(ev.nextWaveAt, now + SIEGE_BREATH))
                    ev.nextWaveAt = now + SIEGE_BREATH;
            }
            if (!Later(ev.nextWaveAt, now))
                SiegeWave(map, ev, now);
        }
    }

    // --- Bounties ---

    enum BountyKind : uint8 { BOUNTY_EVENTS = 1, BOUNTY_SLAY = 2, BOUNTY_CHAMPIONS = 3, BOUNTY_DUNGEON = 4 };
    constexpr uint8 BOUNTY_SLOTS = 3;
    // XP of a finished bounty, in thousandths of a level, by kind.
    constexpr uint32 BOUNTY_XP_PERMILLE[5] = { 0, 300, 200, 250, 450 };

    struct ZoneRange { uint32 map, zone; uint8 low, high; };
    ZoneRange const ZONES[] =
    {
        { 0, 12, 1, 10 }, { 0, 1, 1, 10 }, { 0, 85, 1, 10 }, { 0, 40, 10, 20 }, { 0, 38, 10, 20 },
        { 0, 130, 10, 20 }, { 0, 44, 15, 25 }, { 0, 10, 18, 30 }, { 0, 11, 20, 30 }, { 0, 267, 20, 30 },
        { 0, 36, 30, 40 }, { 0, 45, 30, 40 }, { 0, 33, 30, 45 }, { 0, 3, 35, 45 }, { 0, 8, 35, 45 },
        { 0, 47, 40, 50 }, { 0, 51, 43, 50 }, { 0, 4, 45, 55 }, { 0, 46, 50, 58 }, { 0, 28, 51, 58 },
        { 0, 139, 53, 60 },
        { 1, 14, 1, 10 }, { 1, 215, 1, 10 }, { 1, 141, 1, 10 }, { 1, 148, 10, 20 }, { 1, 17, 10, 25 },
        { 1, 406, 15, 27 }, { 1, 331, 18, 30 }, { 1, 400, 25, 35 }, { 1, 405, 30, 40 }, { 1, 15, 35, 45 },
        { 1, 357, 40, 50 }, { 1, 440, 40, 50 }, { 1, 16, 45, 55 }, { 1, 361, 48, 55 }, { 1, 490, 48, 55 },
        { 1, 1377, 55, 60 }, { 1, 618, 55, 60 },
    };

    struct DungeonRange { uint32 map; char const* name; uint8 low, high; };
    DungeonRange const DUNGEONS[] =
    {
        { 389, "Ragefire Chasm", 13, 18 }, { 43, "Wailing Caverns", 17, 24 }, { 36, "The Deadmines", 17, 26 },
        { 33, "Shadowfang Keep", 22, 30 }, { 48, "Blackfathom Deeps", 24, 32 }, { 34, "The Stockade", 24, 32 },
        { 90, "Gnomeregan", 29, 38 }, { 47, "Razorfen Kraul", 29, 38 }, { 189, "Scarlet Monastery", 34, 45 },
        { 129, "Razorfen Downs", 37, 46 }, { 70, "Uldaman", 41, 51 }, { 209, "Zul'Farrak", 44, 54 },
        { 349, "Maraudon", 46, 55 }, { 109, "the Sunken Temple", 50, 56 }, { 230, "Blackrock Depths", 52, 60 },
        { 229, "Blackrock Spire", 55, 60 }, { 429, "Dire Maul", 55, 60 }, { 289, "Scholomance", 58, 60 },
        { 329, "Stratholme", 58, 60 },
    };

    struct Bounty
    {
        uint8 kind = 0;
        uint32 target = 0;      // a zone, or a dungeon's map
        uint16 have = 0, need = 0;
    };

    struct PlayerBounties
    {
        bool loaded = false;
        Bounty slot[BOUNTY_SLOTS];
    };

    std::mutex sBountyLock;
    std::unordered_map<ObjectGuid, PlayerBounties> sBounties;

    void EnsureBountyTable()
    {
        static bool const created = []()
        {
            CharacterDatabase.DirectExecute(
                "CREATE TABLE IF NOT EXISTS character_arpg_bounty ("
                "guid INT UNSIGNED NOT NULL, slot TINYINT UNSIGNED NOT NULL, kind TINYINT UNSIGNED NOT NULL, "
                "target INT UNSIGNED NOT NULL, have SMALLINT UNSIGNED NOT NULL, need SMALLINT UNSIGNED NOT NULL, "
                "PRIMARY KEY (guid, slot)) ENGINE=InnoDB DEFAULT CHARSET=utf8 COMMENT='ARPG bounties (Arpg/ArpgEvents.h)'");
            return true;
        }();
        (void)created;
    }

    void SaveBounty(Player* player, uint8 slot, Bounty const& b)
    {
        CharacterDatabase.PExecute("REPLACE INTO character_arpg_bounty (guid, slot, kind, target, have, need) VALUES (%u, %u, %u, %u, %u, %u)",
                                   player->GetGUIDLow(), uint32(slot), uint32(b.kind), b.target, uint32(b.have), uint32(b.need));
    }

    std::string ZoneName(uint32 zone)
    {
        return AreaName(zone);
    }

    char const* DungeonName(uint32 map)
    {
        for (DungeonRange const& d : DUNGEONS)
            if (d.map == map)
                return d.name;
        return "a dungeon";
    }

    // A new bounty for `player`, unlike the ones it holds.
    Bounty Generate(Player* player, PlayerBounties const& held)
    {
        uint32 const level = player->GetLevel();
        auto fits = [level](uint8 low, uint8 high) { return int32(low) <= int32(level) + 3 && int32(high) >= int32(level) - 2; };
        auto taken = [&](uint8 kind, uint32 target)
        {
            for (Bounty const& b : held.slot)
                if (b.kind == kind && b.target == target)
                    return true;
            return false;
        };
        std::vector<uint32> zones;
        for (ZoneRange const& z : ZONES)
            if (fits(z.low, z.high))
                zones.push_back(z.zone);
        // The player's own zone first, when it suits.
        uint32 const here = player->GetZoneId();
        bool hereFits = std::find(zones.begin(), zones.end(), here) != zones.end();
        std::vector<uint32> dungeons;
        for (DungeonRange const& d : DUNGEONS)
            if (fits(d.low, d.high))
                dungeons.push_back(d.map);
        for (int tries = 0; tries < 20; ++tries)
        {
            Bounty b;
            uint32 const roll = urand(1, 100);
            b.kind = roll <= 30 ? BOUNTY_EVENTS : roll <= 60 ? BOUNTY_SLAY : roll <= 80 ? BOUNTY_CHAMPIONS : BOUNTY_DUNGEON;
            if (b.kind == BOUNTY_DUNGEON)
            {
                if (dungeons.empty())
                    continue;
                b.target = dungeons[urand(0, uint32(dungeons.size() - 1))];
                b.need = 1;
            }
            else
            {
                if (zones.empty())
                    continue;
                b.target = hereFits && tries < 6 ? here : zones[urand(0, uint32(zones.size() - 1))];
                b.need = uint16(b.kind == BOUNTY_EVENTS ? 2 : b.kind == BOUNTY_SLAY ? urand(20, 30) : urand(2, 4));
            }
            if (!taken(b.kind, b.target))
                return b;
        }
        // Nothing new left: slay anywhere near.
        Bounty b;
        b.kind = BOUNTY_SLAY;
        b.target = zones.empty() ? here : zones.front();
        b.need = 25;
        return b;
    }

    std::string Title(Bounty const& b)
    {
        char buf[128];
        switch (b.kind)
        {
            case BOUNTY_EVENTS:    std::snprintf(buf, sizeof(buf), "%s: complete %u events", ZoneName(b.target).c_str(), uint32(b.need)); break;
            case BOUNTY_SLAY:      std::snprintf(buf, sizeof(buf), "%s: slay %u enemies", ZoneName(b.target).c_str(), uint32(b.need)); break;
            case BOUNTY_CHAMPIONS: std::snprintf(buf, sizeof(buf), "%s: slay %u champions or rares", ZoneName(b.target).c_str(), uint32(b.need)); break;
            case BOUNTY_DUNGEON:   std::snprintf(buf, sizeof(buf), "%s: defeat its final boss", DungeonName(b.target)); break;
            default:               std::snprintf(buf, sizeof(buf), "?"); break;
        }
        return buf;
    }

    uint8 RewardRarity(uint8 kind) { return kind == BOUNTY_DUNGEON ? RARITY_PURPLE : kind == BOUNTY_SLAY ? RARITY_GREEN : RARITY_BLUE; }
    uint32 RewardOrbs(uint8 kind) { return kind == BOUNTY_DUNGEON ? 3 : kind == BOUNTY_SLAY ? 1 : 2; }

    std::string RewardText(Player* player, Bounty const& b)
    {
        char buf[128];
        static char const* const ITEM[] = { "", "a white", "a green", "a blue", "a purple" };
        if (player->GetLevel() < sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL))
            std::snprintf(buf, sizeof(buf), "%u%% of a level, gold, %u %s, %s item", BOUNTY_XP_PERMILLE[b.kind] / 10,
                          RewardOrbs(b.kind), RewardOrbs(b.kind) == 1 ? "orb" : "orbs", ITEM[RewardRarity(b.kind)]);
        else
            std::snprintf(buf, sizeof(buf), "gold, %u %s, %s item", RewardOrbs(b.kind),
                          RewardOrbs(b.kind) == 1 ? "orb" : "orbs", ITEM[RewardRarity(b.kind)]);
        return buf;
    }

    void SendBounties(Player* player)
    {
        PlayerBounties copy;
        {
            std::lock_guard<std::mutex> guard(sBountyLock);
            copy = sBounties[player->GetObjectGuid()];
        }
        WorldPacket data(SMSG_ARPG_BOUNTIES, 1 + BOUNTY_SLOTS * 80);
        data << uint8(BOUNTY_SLOTS);
        for (uint8 i = 0; i < BOUNTY_SLOTS; ++i)
        {
            Bounty const& b = copy.slot[i];
            data << uint8(i) << uint8(b.kind) << uint16(b.have) << uint16(b.need);
            data << Title(b) << RewardText(player, b);
        }
        player->GetSession()->SendPacket(data);
    }

    // `b` (in `slot`) is done: pay it, and roll the next.
    void Pay(Player* player, uint8 slot, Bounty const& b)
    {
        ChatHandler chat(player);
        uint32 const level = player->GetLevel();
        std::string const title = Title(b);
        if (level < sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL))
        {
            uint32 const xp = std::max<uint32>(1, player->GetUInt32Value(PLAYER_NEXT_LEVEL_XP) * BOUNTY_XP_PERMILLE[b.kind] / 1000);
            player->GiveXP(xp, nullptr);
            chat.PSendSysMessage("|cffffd200Bounty complete: %s. +%u experience.|r", title.c_str(), xp);
        }
        else
            chat.PSendSysMessage("|cffffd200Bounty complete: %s.|r", title.c_str());
        uint32 const gold = level * level * (b.kind == BOUNTY_DUNGEON ? 40 : 20);
        player->ModifyMoney(int32(gold));
        GiveOrbs(player, RewardOrbs(b.kind));
        GiveRolledItem(player, RewardRarity(b.kind), level);
    }

    // Add `n` to every held bounty of `kind` on `target` for `player`.
    void Progress(Player* player, uint8 kind, uint32 target, uint16 n)
    {
        if (!Active(player))
            return;
        std::vector<std::pair<uint8, Bounty>> done;
        std::vector<std::pair<uint8, Bounty>> changed;
        {
            std::lock_guard<std::mutex> guard(sBountyLock);
            auto it = sBounties.find(player->GetObjectGuid());
            if (it == sBounties.end() || !it->second.loaded)
                return;
            PlayerBounties& pb = it->second;
            for (uint8 i = 0; i < BOUNTY_SLOTS; ++i)
            {
                Bounty& b = pb.slot[i];
                if (b.kind != kind || b.target != target || b.have >= b.need)
                    continue;
                b.have = uint16(std::min<uint32>(b.have + n, b.need));
                if (b.have >= b.need)
                {
                    done.emplace_back(i, b);
                    b = Generate(player, pb);
                }
                changed.emplace_back(i, b);
            }
        }
        if (changed.empty())
            return;
        for (auto const& [slot, b] : done)
            Pay(player, slot, b);
        for (auto const& [slot, b] : changed)
            SaveBounty(player, slot, b);
        SendBounties(player);
    }

    void CreditBountyEvent(Player* player, uint32 zone)
    {
        Progress(player, BOUNTY_EVENTS, zone, 1);
    }

    // Who a kill credits: the killer, and its group members near it.
    std::vector<Player*> Credited(Player* killer, Unit* victim)
    {
        std::vector<Player*> out{ killer };
        if (Group* group = killer->GetGroup())
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                if (Player* member = ref->getSource())
                    if (member != killer && member->IsInMap(victim) && member->IsWithinDistInMap(victim, 100.0f))
                        out.push_back(member);
        return out;
    }
}

namespace Arpg
{
    void UpdateEvents(Player* player)
    {
        if (!EventsOn() || !Active(player) || !player->IsInWorld())
            return;
        uint32 const now = WorldTimer::getMSTime();
        {
            std::lock_guard<std::recursive_mutex> guard(sEventsLock);
            uint32& last = sPlayerTick[player->GetObjectGuid()];
            if (last && !Later(now, last + PLAYER_TICK - 1))
                return;
            last = now;
        }
        std::lock_guard<std::recursive_mutex> held(sEventsLock);
        // Caches no corpse's loot claimed (it had none to make) are let go.
        for (auto it = sCacheLevel.begin(); it != sCacheLevel.end();)
        {
            if (WorldTimer::getMSTimeDiff(it->second.second, now) > CACHE_KEPT)
            {
                sCaches.erase(it->first);
                it = sCacheLevel.erase(it);
            }
            else
                ++it;
        }
        Map* map = player->GetMap();
        // The open world only: dungeons have their own Wardens, towns their peace.
        bool const world = !map->Instanceable() && (map->GetId() == 0 || map->GetId() == 1);
        std::pair<uint32, uint32> const key{ player->GetMapId(), player->GetZoneId() };
        ZoneEvent* ev = nullptr;
        bool startDue = false;
        {
            std::lock_guard<std::recursive_mutex> guard(sEventsLock);
            auto it = world ? sEvents.find(key) : sEvents.end();
            if (it != sEvents.end())
                ev = &it->second;
            else if (world)
            {
                auto next = sNextAt.find(key);
                if (next == sNextAt.end())
                    sNextAt[key] = now + urand(EVENT_FIRST_MIN, EVENT_FIRST_MAX);
                else if (!Later(next->second, now))
                {
                    startDue = true;
                    next->second = now + EVENT_RETRY;
                }
            }
        }
        if (!ev)
        {
            // Left the event's zone (or none here): the tracker clears.
            if (sShownTo.count(player->GetObjectGuid()))
                SendGone(player);
            if (startDue && player->IsAlive() && !player->IsInCombat() && !player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_RESTING) &&
                    !player->IsTaxiFlying() && player->GetLevel() >= 5)
                Start(player, now);
            return;
        }
        // The zone's event moves at its own pace, whoever is in it.
        if (!ev->lastTick || Later(now, ev->lastTick + EVENT_TICK - 1))
        {
            ev->lastTick = now;
            Tick(map, *ev, now);
        }
        if (ev->state != STATE_LIVE && !Later(ev->doneAt + RESULT_SHOWN, now))
        {
            // Over and shown: gone, and the next is a while off.
            for (auto const& ref : map->GetPlayers())
                if (Player* p = ref.getSource())
                    if (InZone(p, *ev))
                        SendGone(p);
            std::lock_guard<std::recursive_mutex> guard(sEventsLock);
            sNextAt[key] = now + urand(EVENT_COOLDOWN_MIN, EVENT_COOLDOWN_MAX);
            sEvents.erase(key);
            return;
        }
        if (ev->dirty || Later(now, ev->lastSent + SEND_EVERY - 1))
            SendZone(map, *ev, now);
        else
        {
            auto shown = sShownTo.find(player->GetObjectGuid());
            if (shown == sShownTo.end() || shown->second != ev->id)
                Send(player, *ev, now);
        }
    }

    void OnEventKill(Player* killer, Unit* victim)
    {
        if (!EventsOn() || !killer || !victim || victim->GetTypeId() != TYPEID_UNIT || !Active(killer))
            return;
        Creature* creature = static_cast<Creature*>(victim);
        Map* map = victim->GetMap();
        uint32 const now = WorldTimer::getMSTime();

        // Bounties: slaying in a zone (nothing grey), champions and rares, a dungeon's final boss.
        bool const worth = MaNGOS::XP::Gain(killer, creature) > 0;
        for (Player* p : Credited(killer, victim))
        {
            if (!Active(p))
                continue;
            if (worth)
                Progress(p, BOUNTY_SLAY, victim->GetZoneId(), 1);
            if (ChampionTier(victim))
                Progress(p, BOUNTY_CHAMPIONS, victim->GetZoneId(), 1);
            if (map->IsDungeon())
            {
                std::vector<uint32> const& bosses = Bosses();
                if (std::find(bosses.begin(), bosses.end(), victim->GetEntry()) != bosses.end())
                    Progress(p, BOUNTY_DUNGEON, victim->GetMapId(), 1);
            }
        }

        // The zone's event.
        std::lock_guard<std::recursive_mutex> held(sEventsLock);
        ZoneEvent* ev = nullptr;
        {
            std::lock_guard<std::recursive_mutex> guard(sEventsLock);
            auto it = sEvents.find({ victim->GetMapId(), victim->GetZoneId() });
            if (it != sEvents.end() && it->second.state == STATE_LIVE)
                ev = &it->second;
        }
        if (!ev)
            return;
        bool const ours = std::find(ev->spawned.begin(), ev->spawned.end(), victim->GetObjectGuid()) != ev->spawned.end();
        switch (ev->type)
        {
            case EVENT_PURGE:
                if (!ours && victim->GetDistance(ev->x, ev->y, ev->z) > EVENT_RADIUS)
                    return;
                ev->fighters.insert(killer->GetObjectGuid());
                ++ev->have;
                ev->dirty = true;
                if (ev->have >= ev->need)
                    Finish(map, *ev, creature, now);
                break;
            case EVENT_HUNT:
                if (!ours)
                    return;
                ev->fighters.insert(killer->GetObjectGuid());
                if (victim->GetObjectGuid() == ev->target)
                {
                    ev->have = 1;
                    Finish(map, *ev, creature, now);
                }
                break;
            case EVENT_SIEGE:
                if (!ours)
                    return;
                ev->fighters.insert(killer->GetObjectGuid());
                // The last of the last wave (the victim still counts as alive here).
                if (ev->wave >= SIEGE_WAVES && !AliveSpawned(map, *ev, victim->GetObjectGuid()))
                {
                    ev->have = uint16(SIEGE_WAVES);
                    Finish(map, *ev, creature, now);
                }
                break;
            default:
                break;
        }
    }

    void OnEventLoot(Creature* victim)
    {
        if (!victim)
            return;
        uint32 level;
        {
            std::lock_guard<std::recursive_mutex> guard(sEventsLock);
            if (!sCaches.erase(victim->GetObjectGuid()))
                return;
            level = sCacheLevel[victim->GetObjectGuid()].first;
            sCacheLevel.erase(victim->GetObjectGuid());
        }
        // No looter, no loot (the kill was nobody's): no cache either.
        Loot* loot = victim->m_loot;
        if (!loot)
            return;
        // The cache: a green, a blue, chances at a purple and a unique, orbs and gold.
        AddRolledItem(loot, RARITY_GREEN, level);
        AddRolledItem(loot, RARITY_BLUE, level);
        if (roll_chance_i(int32(CACHE_PURPLE_PCT)))
            AddRolledItem(loot, RARITY_PURPLE, level);
        if (roll_chance_i(int32(CACHE_UNIQUE_PCT)))
            AddRolledItem(loot, RARITY_UNIQUE, level);
        AddOrbsToLoot(loot, urand(CACHE_ORBS_MIN, CACHE_ORBS_MAX));
        loot->AddArpgDevGold(level * level * 6);
        // Lootable, whatever the kill decided for a corpse that had nothing of its own.
        victim->SetFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_LOOTABLE);
    }

    void LoadBounties(Player* player)
    {
        if (!player || !Active(player))
            return;
        EnsureBountyTable();
        PlayerBounties pb;
        if (auto result = CharacterDatabase.PQuery("SELECT slot, kind, target, have, need FROM character_arpg_bounty WHERE guid = %u",
                                                   player->GetGUIDLow()))
        {
            do
            {
                Field* f = result->Fetch();
                uint32 const slot = f[0].GetUInt32();
                if (slot >= BOUNTY_SLOTS)
                    continue;
                Bounty& b = pb.slot[slot];
                b.kind = uint8(f[1].GetUInt32());
                b.target = f[2].GetUInt32();
                b.have = uint16(f[3].GetUInt32());
                b.need = uint16(f[4].GetUInt32());
            }
            while (result->NextRow());
        }
        pb.loaded = true;
        for (uint8 i = 0; i < BOUNTY_SLOTS; ++i)
            if (!pb.slot[i].kind || pb.slot[i].kind > BOUNTY_DUNGEON || !pb.slot[i].need)
            {
                pb.slot[i] = Generate(player, pb);
                SaveBounty(player, i, pb.slot[i]);
            }
        {
            std::lock_guard<std::mutex> guard(sBountyLock);
            sBounties[player->GetObjectGuid()] = pb;
        }
        SendBounties(player);
        {
            // A fresh hello: the tracker clears, and the zone's event (if any) is sent anew.
            std::lock_guard<std::recursive_mutex> guard(sEventsLock);
            SendGone(player);
        }
    }

    void AbandonBounty(Player* player, uint8 slot)
    {
        if (!player || !Active(player) || slot >= BOUNTY_SLOTS)
            return;
        Bounty b;
        {
            std::lock_guard<std::mutex> guard(sBountyLock);
            auto it = sBounties.find(player->GetObjectGuid());
            if (it == sBounties.end() || !it->second.loaded)
                return;
            PlayerBounties& pb = it->second;
            b = pb.slot[slot] = Generate(player, pb);
        }
        SaveBounty(player, slot, b);
        SendBounties(player);
    }

    void ForgetEvents(Player* player)
    {
        {
            std::lock_guard<std::mutex> guard(sBountyLock);
            sBounties.erase(player->GetObjectGuid());
        }
        std::lock_guard<std::recursive_mutex> guard(sEventsLock);
        sPlayerTick.erase(player->GetObjectGuid());
        sShownTo.erase(player->GetObjectGuid());
    }
}
