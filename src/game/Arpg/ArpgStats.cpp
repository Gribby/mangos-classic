/*
 * ARPG play statistics: see ArpgStats.h.
 */

#include "Arpg/ArpgStats.h"
#include "Arpg/ArpgCombat.h"
#include "Arpg/ArpgDungeons.h"
#include "Arpg/ArpgPacks.h"

#include "Entities/Creature.h"
#include "Entities/Player.h"
#include "Server/DBCStores.h"
#include "Spells/Spell.h"
#include "Spells/SpellMgr.h"
#include "Util/Timer.h"
#include "World/World.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>

namespace
{
    using namespace Arpg;

    constexpr uint32 SAMPLE_MS = 1000;            // the combat, mana and health samples
    constexpr uint32 SESSION_EVERY_MS = 5 * 60 * 1000;
    constexpr uint32 LAST_BLOWS_MS = 10000;       // a death's blows: the last this long
    constexpr size_t LAST_BLOWS_MAX = 12;
    constexpr uint32 LOW_MANA_PCT = 15, LOW_HEALTH_PCT = 30;

    struct SpellTally { uint32 casts = 0, hits = 0; uint64 damage = 0; };
    struct Blow { uint32 at; std::string source; std::string spell; uint32 amount; };

    struct Session
    {
        std::string id;                           // guid and login time
        std::time_t started = 0;
        uint32 startMs = 0, lastSample = 0, lastWrite = 0;
        uint32 startLevel = 0, level = 0, levelAt = 0;
        std::vector<std::pair<uint32, float>> levels; // reached, minutes the level before took
        std::map<std::string, uint32> kills;      // by kind
        uint32 deaths = 0;
        std::map<std::string, SpellTally> dealt;  // by spell name ("Melee": swings)
        std::map<std::string, uint64> taken;      // by source name
        std::map<std::string, uint32> refused;    // casts refused for want of power, by spell
        std::deque<Blow> blows;
        uint32 combatSec = 0, lowManaSec = 0, lowHealthSec = 0;
        uint32 flasks = 0, rolls = 0;
        std::map<std::string, uint32> zoneSec;    // by zone name
    };

    std::mutex sLock;
    std::unordered_map<ObjectGuid, Session> sSessions;
    std::mutex sFileLock;

    bool On()
    {
        return sWorld.getConfig(CONFIG_BOOL_ARPG_ENABLE) && sWorld.getConfig(CONFIG_BOOL_ARPG_STATS);
    }

    Player* Tracked(Unit* unit)
    {
        if (!unit || unit->GetTypeId() != TYPEID_PLAYER || !Active(unit))
            return nullptr;
        return static_cast<Player*>(unit);
    }

    std::string Json(std::string const& text)
    {
        std::string out = "\"";
        for (char c : text)
        {
            switch (c)
            {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                default:
                    if (uint8(c) < 0x20)
                        continue;
                    out += c;
            }
        }
        return out + "\"";
    }

    std::string Stamp(std::time_t when)
    {
        char buf[32];
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &when);
#else
        localtime_r(&when, &tm);
#endif
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
        return buf;
    }

    // Append one line to today's file.
    void Write(std::string const& line)
    {
        std::time_t const now = std::time(nullptr);
        std::string const day = Stamp(now).substr(0, 10);
        std::lock_guard<std::mutex> guard(sFileLock);
        std::error_code ec;
        std::filesystem::create_directories("arpg-stats", ec);
        std::ofstream out("arpg-stats/" + day + ".jsonl", std::ios::app);
        if (out)
            out << line << '\n';
    }

    char const* ClassName(uint8 classId)
    {
        switch (classId)
        {
            case CLASS_PALADIN: return "paladin";
            case CLASS_MAGE: return "mage";
            case CLASS_DRUID: return "druid";
            case CLASS_WARLOCK: return "warlock";
            case CLASS_SHAMAN: return "shaman";
            default: return "other";
        }
    }

    std::string ZoneName(Player const* player)
    {
        if (AreaTableEntry const* area = GetAreaEntryByAreaID(player->GetZoneId()))
            return area->area_name[0];
        return std::to_string(player->GetZoneId());
    }

    std::string Who(Player const* player)
    {
        return "\"char\":" + Json(player->GetName()) + ",\"class\":\"" + ClassName(player->getClass()) +
               "\",\"level\":" + std::to_string(player->GetLevel());
    }

    // The kind of a slain creature, for the kill counts.
    char const* KindOf(Unit const* victim)
    {
        if (victim->GetTypeId() != TYPEID_UNIT)
            return "player";
        Creature const* c = static_cast<Creature const*>(victim);
        if (IsDungeonBoss(c) || c->IsWorldBoss())
            return "boss";
        switch (ChampionTier(c))
        {
            case 2: return "rare";
            case 1: return "champion";
            default: break;
        }
        if (IsPackFollower(c))
            return "follower";
        return c->IsElite() ? "elite" : "normal";
    }

    Session& SessionOf(Player const* player, uint32 now)
    {
        Session& s = sSessions[player->GetObjectGuid()];
        if (s.id.empty())
        {
            s.started = std::time(nullptr);
            s.id = std::to_string(player->GetGUIDLow()) + "-" + std::to_string(uint64(s.started));
            s.startMs = s.lastSample = s.lastWrite = s.levelAt = now ? now : 1;
            s.startLevel = s.level = player->GetLevel();
        }
        return s;
    }

    template <class Map> std::string Counts(Map const& m)
    {
        std::string out = "{";
        for (auto const& [k, v] : m)
            out += (out.size() > 1 ? "," : "") + Json(k) + ":" + std::to_string(v);
        return out + "}";
    }

    std::string SessionLine(Player const* player, Session const& s, uint32 now, bool logout)
    {
        float const minutes = WorldTimer::getMSTimeDiff(s.startMs, now) / 60000.0f;
        std::ostringstream o;
        o << "{\"type\":\"session\",\"id\":" << Json(s.id) << ",\"at\":" << Json(Stamp(std::time(nullptr)))
          << ",\"started\":" << Json(Stamp(s.started)) << "," << Who(player) << ",\"logout\":" << (logout ? "true" : "false")
          << ",\"minutes\":" << minutes << ",\"startLevel\":" << s.startLevel << ",\"levels\":[";
        for (size_t i = 0; i < s.levels.size(); ++i)
            o << (i ? "," : "") << "{\"reached\":" << s.levels[i].first << ",\"minutes\":" << s.levels[i].second << "}";
        o << "],\"kills\":" << Counts(s.kills) << ",\"deaths\":" << s.deaths
          << ",\"combatSec\":" << s.combatSec << ",\"lowManaSec\":" << s.lowManaSec << ",\"lowHealthSec\":" << s.lowHealthSec
          << ",\"flasks\":" << s.flasks << ",\"rolls\":" << s.rolls << ",\"refusedForPower\":" << Counts(s.refused)
          << ",\"zoneSec\":" << Counts(s.zoneSec) << ",\"dealt\":{";
        bool first = true;
        for (auto const& [name, t] : s.dealt)
        {
            o << (first ? "" : ",") << Json(name) << ":{\"casts\":" << t.casts << ",\"hits\":" << t.hits << ",\"damage\":" << t.damage << "}";
            first = false;
        }
        // The ten sources that dealt the most.
        std::vector<std::pair<std::string, uint64>> taken(s.taken.begin(), s.taken.end());
        std::sort(taken.begin(), taken.end(), [](auto const& a, auto const& b) { return a.second > b.second; });
        if (taken.size() > 10)
            taken.resize(10);
        o << "},\"takenTop\":{";
        for (size_t i = 0; i < taken.size(); ++i)
            o << (i ? "," : "") << Json(taken[i].first) << ":" << taken[i].second;
        o << "}}";
        return o.str();
    }
}

namespace Arpg
{
    void OnStatsDamage(Unit* dealer, Unit* victim, uint32 damage, SpellEntry const* spell)
    {
        if (!damage || !dealer || !victim || dealer == victim || !On())
            return;
        Player* hitter = Tracked(dealer->GetBeneficiaryPlayer());
        Player* hurt = Tracked(victim);
        if (!hitter && !hurt)
            return;
        uint32 const now = WorldTimer::getMSTime();
        std::string const spellName = spell && spell->SpellName[0] ? spell->SpellName[0] : "Melee";
        std::lock_guard<std::mutex> guard(sLock);
        if (hitter && victim->GetTypeId() == TYPEID_UNIT)
        {
            SpellTally& t = SessionOf(hitter, now).dealt[dealer == hitter ? spellName : std::string("Pet: ") + spellName];
            ++t.hits;
            t.damage += damage;
        }
        if (hurt)
        {
            Session& s = SessionOf(hurt, now);
            std::string const source = dealer->GetName() ? dealer->GetName() : "?";
            s.taken[source] += damage;
            s.blows.push_back({ now, source, spellName, damage });
            while (s.blows.size() > LAST_BLOWS_MAX || (!s.blows.empty() && WorldTimer::getMSTimeDiff(s.blows.front().at, now) > LAST_BLOWS_MS))
                s.blows.pop_front();
        }
    }

    void OnStatsKill(Unit* killer, Unit* victim)
    {
        if (!victim || !On())
            return;
        uint32 const now = WorldTimer::getMSTime();
        if (Player* dead = Tracked(victim))
        {
            std::ostringstream o;
            o << "{\"type\":\"death\",\"at\":" << Json(Stamp(std::time(nullptr))) << "," << Who(dead)
              << ",\"zone\":" << Json(ZoneName(dead)) << ",\"map\":" << dead->GetMapId()
              << ",\"maxHealth\":" << dead->GetMaxHealth() << ",\"manaPct\":"
              << (dead->GetMaxPower(POWER_MANA) ? dead->GetPower(POWER_MANA) * 100 / dead->GetMaxPower(POWER_MANA) : 0);
            if (killer)
            {
                o << ",\"killer\":{\"name\":" << Json(killer->GetName() ? killer->GetName() : "?") << ",\"level\":" << killer->GetLevel();
                if (killer->GetTypeId() == TYPEID_UNIT)
                {
                    Creature const* c = static_cast<Creature const*>(killer);
                    o << ",\"entry\":" << c->GetEntry() << ",\"elite\":" << (c->IsElite() ? "true" : "false")
                      << ",\"champion\":" << uint32(ChampionTier(c)) << ",\"boss\":" << (IsDungeonBoss(c) || c->IsWorldBoss() ? "true" : "false");
                }
                o << "}";
            }
            {
                std::lock_guard<std::mutex> guard(sLock);
                Session& s = SessionOf(dead, now);
                ++s.deaths;
                o << ",\"lastBlows\":[";
                for (size_t i = 0; i < s.blows.size(); ++i)
                {
                    Blow const& b = s.blows[i];
                    o << (i ? "," : "") << "{\"msBefore\":" << WorldTimer::getMSTimeDiff(b.at, now) << ",\"source\":" << Json(b.source)
                      << ",\"spell\":" << Json(b.spell) << ",\"amount\":" << b.amount << "}";
                }
                o << "]}";
                s.blows.clear();
            }
            Write(o.str());
        }
        if (Player* slayer = Tracked(killer ? killer->GetBeneficiaryPlayer() : nullptr))
            if (victim->GetTypeId() == TYPEID_UNIT)
            {
                std::lock_guard<std::mutex> guard(sLock);
                ++SessionOf(slayer, now).kills[KindOf(victim)];
            }
    }

    void OnStatsCast(Spell const* spell)
    {
        if (!spell || spell->m_IsTriggeredSpell || spell->IsArpgSecondary() || !On())
            return;
        Player* caster = Tracked(spell->GetCaster());
        if (!caster || !spell->m_spellInfo->SpellName[0])
            return;
        std::lock_guard<std::mutex> guard(sLock);
        ++SessionOf(caster, WorldTimer::getMSTime()).dealt[spell->m_spellInfo->SpellName[0]].casts;
    }

    void OnStatsCastFailed(Player const* caster, SpellEntry const* spellInfo, uint32 result)
    {
        if (result != SPELL_FAILED_NO_POWER || !caster || !spellInfo || !Active(caster) || !On())
            return;
        std::lock_guard<std::mutex> guard(sLock);
        ++SessionOf(caster, WorldTimer::getMSTime()).refused[spellInfo->SpellName[0] ? spellInfo->SpellName[0] : "?"];
    }

    void OnStatsFlask(Player const* player)
    {
        if (!player || !On())
            return;
        std::lock_guard<std::mutex> guard(sLock);
        ++SessionOf(player, WorldTimer::getMSTime()).flasks;
    }

    void OnStatsRoll(Player const* player)
    {
        if (!player || !On())
            return;
        std::lock_guard<std::mutex> guard(sLock);
        ++SessionOf(player, WorldTimer::getMSTime()).rolls;
    }

    void OnStatsPack(char const* name, uint32 size, uint32 level, float seconds, uint32 damage)
    {
        if (!On())
            return;
        std::ostringstream o;
        o << "{\"type\":\"pack\",\"at\":" << Json(Stamp(std::time(nullptr))) << ",\"name\":" << Json(name ? name : "?")
          << ",\"size\":" << size << ",\"level\":" << level << ",\"seconds\":" << seconds << ",\"damageDealt\":" << damage << "}";
        Write(o.str());
    }

    void UpdateStats(Player* player)
    {
        if (!On() || !Active(player) || !player->IsInWorld())
            return;
        uint32 const now = WorldTimer::getMSTime();
        std::string line;
        {
            std::lock_guard<std::mutex> guard(sLock);
            Session& s = SessionOf(player, now);
            if (WorldTimer::getMSTimeDiff(s.lastSample, now) < SAMPLE_MS)
                return;
            s.lastSample = now;
            ++s.zoneSec[ZoneName(player)];
            if (player->IsInCombat() && player->IsAlive())
            {
                ++s.combatSec;
                if (player->GetMaxPower(POWER_MANA) && player->GetPower(POWER_MANA) * 100 < player->GetMaxPower(POWER_MANA) * LOW_MANA_PCT)
                    ++s.lowManaSec;
                if (player->GetHealth() * 100 < uint64(player->GetMaxHealth()) * LOW_HEALTH_PCT)
                    ++s.lowHealthSec;
            }
            if (player->GetLevel() > s.level)
            {
                s.levels.emplace_back(player->GetLevel(), WorldTimer::getMSTimeDiff(s.levelAt, now) / 60000.0f);
                s.level = player->GetLevel();
                s.levelAt = now;
            }
            if (WorldTimer::getMSTimeDiff(s.lastWrite, now) >= SESSION_EVERY_MS)
            {
                s.lastWrite = now;
                line = SessionLine(player, s, now, false);
            }
        }
        if (!line.empty())
            Write(line);
    }

    void FlushStats(Player* player)
    {
        if (!player)
            return;
        std::string line;
        {
            std::lock_guard<std::mutex> guard(sLock);
            auto it = sSessions.find(player->GetObjectGuid());
            if (it == sSessions.end())
                return;
            if (On())
                line = SessionLine(player, it->second, WorldTimer::getMSTime(), true);
            sSessions.erase(it);
        }
        if (!line.empty())
            Write(line);
    }
}
