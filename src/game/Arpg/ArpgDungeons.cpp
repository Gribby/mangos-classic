/*
 * ARPG dungeons and group scaling: see ArpgDungeons.h.
 */

#include "Arpg/ArpgDungeons.h"
#include "Arpg/ArpgCombat.h"
#include "Arpg/ArpgPacks.h"
#include "Arpg/ArpgLoot.h"
#include "Arpg/ArpgTree.h"

#include "AI/ScriptDevAI/ScriptDevAIMgr.h"
#include "Chat/Chat.h"
#include "Entities/Item.h"
#include "Loot/LootMgr.h"

#include "Entities/Creature.h"
#include "Entities/Player.h"
#include "Globals/ObjectMgr.h"
#include "Groups/Group.h"
#include "Maps/Map.h"
#include "Server/DBCStores.h"
#include "World/World.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    using namespace Arpg;

    // Scaling waits this long after the spawn, in ms, so the grid and any script have settled.
    constexpr uint32 SCALE_DELAY_MS = 400;
    // Group members this far from an open-world elite count toward its health.
    constexpr float GROUP_REACH = 100.0f;
    // A raid's solo health is SoloHealth * RAID_SOLO_SIZE / its size; each extra player adds
    // RAID_PLAYER_SHARE of that.
    constexpr float RAID_SOLO_SIZE = 9.0f, RAID_PLAYER_SHARE = 0.75f;
    // Dungeon trash champions and rares, per mob.
    constexpr uint32 DUNGEON_CHAMPION_MIN_LEVEL = 8;
    constexpr uint32 DUNGEON_RARE_PER_MILLE = 15, DUNGEON_CHAMPION_PER_MILLE = 60;
    // A dungeon creature with this health multiplier or more is a boss (vanilla trash stays
    // under it: about 3 in low dungeons, 6 in Stratholme and Blackrock Spire).
    constexpr float BOSS_HEALTH_LOW = 5.0f, BOSS_HEALTH_HIGH = 6.0f;
    constexpr uint32 BOSS_HEALTH_LEVEL = 40;

    // The per-hit caps, as percent of the victim's maximum health.
    struct Caps
    {
        uint32 melee;
        uint32 spell;
    };
    constexpr Caps CAP_TRASH { 6, 12 };
    constexpr Caps CAP_CHAMPION { 9, 15 };
    constexpr Caps CAP_DUNGEON_BOSS { 12, 30 };
    constexpr Caps CAP_RAID_BOSS { 15, 35 };

    enum Kind : uint8 { KIND_NONE, KIND_INSTANCE, KIND_ELITE };

    // A creature's scaling is a percent health modifier (UNIT_MOD_HEALTH, TOTAL_PCT), so the core's
    // own recalculations (a stamina aura, a respawn's UpdateAllStats) keep it.
    struct Scaled
    {
        float pct = 0.0f;       // the modifier applied, in percent
        uint32 players = 0;     // the player count it was set for
        uint8 tier = 0;         // and the tier
    };

    // The difficulty tier of `map`'s instance (below, with the tiers).
    uint8 TierOf(Map* map);
    float TierHealth(uint8 tier);

    std::mutex sScaledLock;
    std::unordered_map<ObjectGuid, Scaled> sScaled;

    bool DungeonsOn()
    {
        return sWorld.getConfig(CONFIG_BOOL_ARPG_ENABLE) && sWorld.getConfig(CONFIG_BOOL_ARPG_DUNGEONS);
    }

    bool FriendlyToPlayers(Creature const* creature)
    {
        FactionTemplateEntry const* faction = creature->GetFactionTemplateEntry();
        return !faction || (faction->friendGroupMask & FACTION_GROUP_MASK_PLAYER) ||
               (faction->factionGroupMask & FACTION_GROUP_MASK_PLAYER);
    }

    bool PlayerOwned(Creature const* creature)
    {
        return creature->IsPet() || creature->IsTotem() || creature->IsPlayerControlled() ||
               creature->GetOwnerGuid().IsPlayer();
    }

    bool EliteRank(uint32 rank)
    {
        return rank == CREATURE_ELITE_ELITE || rank == CREATURE_ELITE_RAREELITE || rank == CREATURE_ELITE_WORLDBOSS;
    }

    Kind KindOf(Creature const* creature)
    {
        if (!creature || PlayerOwned(creature) || FriendlyToPlayers(creature))
            return KIND_NONE;
        CreatureInfo const* info = creature->GetCreatureInfo();
        Map const* map = creature->GetMap();
        if (!info || !map || info->CreatureType == CREATURE_TYPE_CRITTER)
            return KIND_NONE;
        if (map->IsDungeon())
            return KIND_INSTANCE;
        if (map->Instanceable())
            return KIND_NONE;                               // battlegrounds stay vanilla
        return EliteRank(info->Rank) ? KIND_ELITE : KIND_NONE;
    }

    float RaidSolo(uint32 size)
    {
        return sWorld.getConfig(CONFIG_FLOAT_ARPG_DUNGEON_SOLO_HEALTH) * RAID_SOLO_SIZE / float(std::max<uint32>(size, 10));
    }

    float HealthFactor(Creature const* creature, Kind kind, uint32 players)
    {
        players = std::max<uint32>(1, std::min<uint32>(players, 40));
        float const extra = float(players - 1);
        float const perPlayer = sWorld.getConfig(CONFIG_FLOAT_ARPG_DUNGEON_PLAYER_HEALTH);
        Map const* map = creature->GetMap();
        float factor;
        if (kind == KIND_INSTANCE && map->IsRaid())
        {
            uint32 size = 40;
            if (InstanceTemplate const* instance = ObjectMgr::GetInstanceTemplate(map->GetId()))
                if (instance->maxPlayers)
                    size = instance->maxPlayers;
            float const solo = RaidSolo(size);
            factor = solo * (1.0f + RAID_PLAYER_SHARE * extra);
        }
        else if (kind == KIND_INSTANCE)
            factor = sWorld.getConfig(CONFIG_FLOAT_ARPG_DUNGEON_SOLO_HEALTH) + perPlayer * extra;
        else if (creature->GetCreatureInfo()->Rank == CREATURE_ELITE_WORLDBOSS)
            factor = RaidSolo(40) * (1.0f + RAID_PLAYER_SHARE * extra);
        else
            factor = sWorld.getConfig(CONFIG_FLOAT_ARPG_ELITE_SOLO_HEALTH) + perPlayer * extra;
        factor = std::max(0.01f, std::min(1.0f, factor));
        if (kind == KIND_INSTANCE)
            factor *= TierHealth(TierOf(creature->GetMap()));
        return factor;
    }

    // The players `creature` is scaled for: everyone in an instance; for an open-world elite,
    // the group of the player it fights, those near it.
    uint32 PlayersFor(Creature* creature, Kind kind, Unit* enemy)
    {
        if (kind == KIND_INSTANCE)
            return std::max<uint32>(1, creature->GetMap()->GetPlayersCountExceptGMs());
        Player* player = enemy ? enemy->GetBeneficiaryPlayer() : nullptr;
        Group* group = player ? player->GetGroup() : nullptr;
        if (!group)
            return 1;
        uint32 count = 0;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* member = ref->getSource())
                if (member->IsInMap(creature) && member->IsWithinDistInMap(creature, GROUP_REACH))
                    ++count;
        return std::max<uint32>(1, count);
    }

    void Apply(Creature* creature, Kind kind, uint32 players)
    {
        float const pct = (HealthFactor(creature, kind, players) - 1.0f) * 100.0f;
        uint8 const tier = TierOf(creature->GetMap());
        bool had;
        float previous;
        {
            std::lock_guard<std::mutex> guard(sScaledLock);
            auto it = sScaled.find(creature->GetObjectGuid());
            had = it != sScaled.end();
            previous = had ? it->second.pct : 0.0f;
            Scaled& s = sScaled[creature->GetObjectGuid()];
            s.pct = pct;
            s.players = players;
            s.tier = tier;
        }
        if (had && previous == pct)
            return;
        uint32 const before = creature->GetMaxHealth();
        float const share = before ? float(creature->GetHealth()) / float(before) : 1.0f;
        if (had)
            creature->HandleStatModifier(UNIT_MOD_HEALTH, TOTAL_PCT, previous, false);
        creature->HandleStatModifier(UNIT_MOD_HEALTH, TOTAL_PCT, pct, true);
        creature->UpdateMaxHealth();
        if (creature->IsAlive())
            creature->SetHealth(std::max<uint32>(1, uint32(float(creature->GetMaxHealth()) * share)));
    }

    // --- Wardens and Caches ---

    struct WardenTheme
    {
        uint32 map;
        char const* name;
        char const* affixes[2];
    };

    // Each dungeon's Warden: a named rare with two themed affixes (a third, random, from level 30).
    WardenTheme const WARDENS[] =
    {
        { 389, "Searfang the Cinderbrute",  { "Fire Enchanted", "Extra Strong" } },   // Ragefire Chasm
        { 36,  "Gorehowl the Fleshrender",  { "Vampiric", "Extra Strong" } },         // The Deadmines
        { 43,  "Venomgut the Dreamwracked", { "Extra Fast", "Healer" } },             // Wailing Caverns
        { 33,  "Nightfang the Hollow",      { "Vampiric", "Teleporter" } },           // Shadowfang Keep
        { 48,  "Tidecaller Murkfin",        { "Cold Enchanted", "Healer" } },         // Blackfathom Deeps
        { 34,  "Shank the Unchained",       { "Extra Strong", "Extra Fast" } },       // The Stockade
        { 90,  "Sparkcog the Overclocked",  { "Extra Fast", "Fire Enchanted" } },     // Gnomeregan
        { 47,  "Thornhide the Bristled",    { "Thorns", "Stone Skin" } },             // Razorfen Kraul
        { 189, "Inquisitor Ashveil",        { "Fire Enchanted", "Healer" } },         // Scarlet Monastery
        { 129, "Rotskull the Gravebound",   { "Cold Enchanted", "Vampiric" } },       // Razorfen Downs
        { 70,  "Graniteheart the Unbroken", { "Stone Skin", "Extra Strong" } },       // Uldaman
        { 209, "Sandscourge Jin'zalo",      { "Teleporter", "Fire Enchanted" } },     // Zul'Farrak
        { 349, "Vinelash the Corrupted",    { "Thorns", "Healer" } },                 // Maraudon
        { 109, "Dreamwyrm Ysvalen",         { "Cold Enchanted", "Teleporter" } },     // The Temple of Atal'Hakkar
        { 230, "Forgemaster Ironmaw",       { "Fire Enchanted", "Stone Skin" } },     // Blackrock Depths
        { 229, "Skarvok the Bloodbound",    { "Vampiric", "Extra Strong" } },         // Blackrock Spire
        { 429, "Ellarion the Unquiet",      { "Teleporter", "Cold Enchanted" } },     // Dire Maul
        { 289, "Bonelord Varakh",           { "Vampiric", "Cold Enchanted" } },       // Scholomance
        { 329, "Plaguefist the Abomination", { "Thorns", "Extra Strong" } },          // Stratholme
    };

    WardenTheme const* ThemeFor(uint32 map)
    {
        for (WardenTheme const& theme : WARDENS)
            if (theme.map == map)
                return &theme;
        return nullptr;
    }

    // Warden loot: two blues and a chance at a purple; a Cache's per player: a blue, a green and
    // a chance at a purple.
    constexpr uint32 WARDEN_EPIC_PCT = 30, CACHE_EPIC_PCT = 25;
    constexpr uint32 WARDEN_ITEM_LEVELS = 3, CACHE_ITEM_LEVELS = 5;

    std::mutex sWardenLock;
    std::map<uint32, std::vector<uint32>> sWardenCandidates;   // by map id: db guids
    std::map<uint32, uint32> sWardenOf;                        // by instance id: the chosen db guid
    std::map<uint32, uint32> sWardenSeen;                      // by instance id: candidates settled
    // A settling candidate becomes the Warden at 1 in WARDEN_ODDS; the WARDEN_BY-th surely.
    constexpr uint32 WARDEN_ODDS = 12, WARDEN_BY = 25;
    std::unordered_set<ObjectGuid> sWardens;                   // the crowned, by object guid

    // Whether a spawn of `info` at `level` reads as a boss without a creature to ask.
    bool TemplateIsBoss(CreatureInfo const* info)
    {
        if (info->Rank == CREATURE_ELITE_WORLDBOSS)
            return true;
        if (std::string(sScriptDevAIMgr.GetScriptName(info->ScriptID)).compare(0, 5, "boss_") == 0)
            return true;
        float const threshold = info->MinLevel < BOSS_HEALTH_LEVEL ? BOSS_HEALTH_LOW : BOSS_HEALTH_HIGH;
        return info->HealthMultiplier >= threshold;
    }

    // The trash spawns of `map` that may be its Warden, sorted.
    std::vector<uint32> BuildWardenCandidates(uint32 map)
    {
        std::vector<uint32> out;
        std::vector<uint32> const& bosses = Bosses();
        auto worker = [&](CreatureDataPair const& pair)
        {
            CreatureData const& data = pair.second;
            if (data.mapid != map)
                return false;
            CreatureInfo const* info = ObjectMgr::GetCreatureTemplate(data.id);
            if (!info || info->MinLevel < DUNGEON_CHAMPION_MIN_LEVEL || info->NpcFlags ||
                    info->CreatureType == CREATURE_TYPE_CRITTER ||
                    (info->Rank != CREATURE_ELITE_NORMAL && info->Rank != CREATURE_ELITE_ELITE) ||
                    TemplateIsBoss(info) || std::find(bosses.begin(), bosses.end(), info->Entry) != bosses.end())
                return false;
            FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(info->Faction);
            if (!faction || (faction->friendGroupMask & FACTION_GROUP_MASK_PLAYER) ||
                    (faction->factionGroupMask & FACTION_GROUP_MASK_PLAYER))
                return false;
            out.push_back(pair.first);
            return false;
        };
        sObjectMgr.DoCreatureData(worker);
        std::sort(out.begin(), out.end());
        return out;
    }

    // Whether `creature` is its instance's Warden. The Warden is chosen among the eligible
    // spawns that actually load: each one that settles takes the crown at 1 in WARDEN_ODDS, and
    // the WARDEN_BY-th surely, so it stands near the entrance's first stretch, in the wing the
    // group is in (Scarlet Monastery, Dire Maul and Blackrock Spire are one map each).
    bool IsChosenWarden(Creature* creature)
    {
        Map* map = creature->GetMap();
        if (!ThemeFor(map->GetId()) || !creature->GetDbGuid())
            return false;
        uint32 const instance = map->GetInstanceId();
        {
            std::lock_guard<std::mutex> guard(sWardenLock);
            if (sWardenOf.count(instance))
                return false;                       // crowned already
            auto it = sWardenCandidates.find(map->GetId());
            if (it != sWardenCandidates.end())
            {
                if (!std::binary_search(it->second.begin(), it->second.end(), creature->GetDbGuid()))
                    return false;
                uint32& seen = sWardenSeen[instance];
                ++seen;
                if (seen < WARDEN_BY && urand(1, WARDEN_ODDS) != 1)
                    return false;
                sWardenOf[instance] = creature->GetDbGuid();
                return true;
            }
        }
        // The first ask for this map: build its candidates outside the lock, then ask again.
        std::vector<uint32> candidates = BuildWardenCandidates(map->GetId());
        {
            std::lock_guard<std::mutex> guard(sWardenLock);
            sWardenCandidates.emplace(map->GetId(), std::move(candidates));
        }
        return IsChosenWarden(creature);
    }

    void TellMap(Map* map, char const* text)
    {
        for (auto const& ref : map->GetPlayers())
            if (Player* player = ref.getSource())
                if (Active(player))
                    ChatHandler(player).PSendSysMessage("|cffffd100%s|r", text);
    }

    void CrownWarden(Creature* creature, uint8 tierAffixes)
    {
        WardenTheme const* theme = ThemeFor(creature->GetMapId());
        if (!theme)
            return;
        uint8 const extra = (creature->GetLevel() >= 30 ? 1 : 0) + tierAffixes;
        MakeChampion(creature, 2, extra, theme->name, { theme->affixes[0], theme->affixes[1] });
        {
            std::lock_guard<std::mutex> guard(sWardenLock);
            sWardens.insert(creature->GetObjectGuid());
        }
        std::string const text = std::string("A Warden stalks these halls: ") + theme->name + ".";
        TellMap(creature->GetMap(), text.c_str());
    }

    void AddPick(Loot* loot, uint32 quality, uint32 level)
    {
        if (uint32 const id = PickRandomItem(quality, level))
            loot->AddItem(id, 1, 0, Item::GenerateItemRandomPropertyId(id));
    }

    // --- Difficulty tiers ---

    char const* const TIER_NAME[MAX_TIER + 1] = { "Normal", "Hard", "Brutal", "Torment I", "Torment II", "Torment III" };
    float const TIER_HEALTH[MAX_TIER + 1] = { 1.0f, 1.4f, 1.9f, 2.6f, 3.4f, 4.4f };
    float const TIER_DAMAGE[MAX_TIER + 1] = { 1.0f, 1.15f, 1.3f, 1.5f, 1.7f, 2.0f };
    float const TIER_CAP[MAX_TIER + 1] = { 1.0f, 1.2f, 1.4f, 1.65f, 1.9f, 2.2f };
    float const TIER_XP[MAX_TIER + 1] = { 1.0f, 1.15f, 1.3f, 1.5f, 1.7f, 2.0f };
    float const TIER_CHAMPIONS[MAX_TIER + 1] = { 1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f };
    uint8 const TIER_AFFIXES[MAX_TIER + 1] = { 0, 0, 1, 1, 2, 2 };
    // Each tier adds to a Warden's and a Cache's drops: item levels, purple chance, extra blues
    // (one per two tiers).
    constexpr uint32 TIER_ITEM_LEVELS = 2, TIER_EPIC_PCT = 8;

    float TierHealth(uint8 tier)
    {
        return TIER_HEALTH[std::min<uint8>(tier, MAX_TIER)];
    }

    struct PlayerTiers
    {
        uint8 wanted = 0;
        bool loaded = false;
        std::map<uint32, uint8> unlocked;   // by map id: the highest tier open
    };

    std::mutex sTierLock;
    std::unordered_map<ObjectGuid, PlayerTiers> sPlayerTiers;
    std::map<uint32, uint8> sTierOf;        // by instance id

    void EnsureTierTable()
    {
        static bool const created = []()
        {
            CharacterDatabase.DirectExecute(
                "CREATE TABLE IF NOT EXISTS character_arpg_tier ("
                "guid INT UNSIGNED NOT NULL, map SMALLINT UNSIGNED NOT NULL, unlocked TINYINT UNSIGNED NOT NULL, "
                "PRIMARY KEY (guid, map)) ENGINE=InnoDB DEFAULT CHARSET=utf8 COMMENT='ARPG dungeon tiers open (Arpg/ArpgDungeons.h)'");
            return true;
        }();
        (void)created;
    }

    void LoadTiers(Player* player)
    {
        {
            std::lock_guard<std::mutex> guard(sTierLock);
            if (sPlayerTiers[player->GetObjectGuid()].loaded)
                return;
        }
        EnsureTierTable();
        std::map<uint32, uint8> unlocked;
        if (auto result = CharacterDatabase.PQuery("SELECT map, unlocked FROM character_arpg_tier WHERE guid = %u", player->GetGUIDLow()))
        {
            do
            {
                Field* fields = result->Fetch();
                unlocked[fields[0].GetUInt32()] = uint8(std::min<uint32>(fields[1].GetUInt32(), MAX_TIER));
            }
            while (result->NextRow());
        }
        std::lock_guard<std::mutex> guard(sTierLock);
        PlayerTiers& t = sPlayerTiers[player->GetObjectGuid()];
        t.unlocked = std::move(unlocked);
        t.loaded = true;
    }

    // The tier of `map`'s instance: the first ARPG player's wish, as far as that player has it
    // open there. Chosen once, when first asked with an ARPG player inside.
    uint8 TierOf(Map* map)
    {
        if (!map || !map->IsDungeon())
            return 0;
        uint32 const instance = map->GetInstanceId();
        Player* chooser = nullptr;
        uint8 tier = 0, wanted = 0;
        {
            std::lock_guard<std::mutex> guard(sTierLock);
            auto it = sTierOf.find(instance);
            if (it != sTierOf.end())
                return it->second;
            for (auto const& ref : map->GetPlayers())
            {
                Player* player = ref.getSource();
                if (!player || !Active(player))
                    continue;
                PlayerTiers const& t = sPlayerTiers[player->GetObjectGuid()];
                auto open = t.unlocked.find(map->GetId());
                uint8 const unlocked = open == t.unlocked.end() ? 0 : open->second;
                wanted = t.wanted;
                tier = std::min(wanted, unlocked);
                chooser = player;
                break;
            }
            if (!chooser)
                return 0;
            sTierOf[instance] = tier;
        }
        std::string text = std::string(map->GetMapName()) + ": " + TIER_NAME[tier] + ".";
        TellMap(map, text.c_str());
        if (wanted > tier)
            ChatHandler(chooser).PSendSysMessage("|cffffd100%s isn't open here yet: clear it on %s first.|r",
                                                 TIER_NAME[tier + 1], TIER_NAME[tier]);
        return tier;
    }

    // Each ARPG player in `map` who cleared it on `tier` opens the next one there.
    void UnlockNext(Map* map, uint8 tier)
    {
        if (tier >= MAX_TIER)
            return;
        uint8 const next = tier + 1;
        for (auto const& ref : map->GetPlayers())
        {
            Player* player = ref.getSource();
            if (!player || !Active(player))
                continue;
            bool opened = false;
            {
                std::lock_guard<std::mutex> guard(sTierLock);
                uint8& unlocked = sPlayerTiers[player->GetObjectGuid()].unlocked[map->GetId()];
                if (unlocked < next)
                {
                    unlocked = next;
                    opened = true;
                }
            }
            if (!opened)
                continue;
            EnsureTierTable();
            CharacterDatabase.PExecute("REPLACE INTO character_arpg_tier (guid, map, unlocked) VALUES (%u, %u, %u)",
                                       player->GetGUIDLow(), map->GetId(), uint32(next));
            ChatHandler(player).PSendSysMessage("|cffffd100%s is now open in %s.|r", TIER_NAME[next], map->GetMapName());
        }
    }

    bool MayRollChampion(Creature* creature)
    {
        Map const* map = creature->GetMap();
        return map->IsDungeon() && !map->IsRaid() && creature->GetSubtype() == CREATURE_SUBTYPE_GENERIC &&
               creature->HasStaticDBSpawnData() && creature->GetLevel() >= DUNGEON_CHAMPION_MIN_LEVEL &&
               !IsDungeonBoss(creature) && !creature->IsInCombat() && ChampionTier(creature) == 0;
    }

    void Settle(Creature* creature)
    {
        if (!DungeonsOn() || !creature->IsInWorld() || !creature->IsAlive())
            return;
        Kind const kind = KindOf(creature);
        if (kind == KIND_NONE)
            return;
        {
            std::lock_guard<std::mutex> guard(sScaledLock);
            if (sScaled.count(creature->GetObjectGuid()))
                return;
        }
        Apply(creature, kind, PlayersFor(creature, kind, nullptr));
        if (kind != KIND_INSTANCE || !MayRollChampion(creature))
            return;
        uint8 const tier = TierOf(creature->GetMap());
        if (IsChosenWarden(creature))
        {
            CrownWarden(creature, TIER_AFFIXES[tier]);
            return;
        }
        float const more = TIER_CHAMPIONS[tier];
        uint32 const rares = uint32(DUNGEON_RARE_PER_MILLE * more);
        uint32 const champions = uint32(DUNGEON_CHAMPION_PER_MILLE * more);
        uint32 const roll = urand(1, 1000);
        bool const rare = roll <= rares;
        if (!rare && roll > rares + champions)
            return;
        MakeChampion(creature, rare ? 2 : 1, ChampionAffixCount(creature->GetLevel(), rare) + TIER_AFFIXES[tier]);
    }
}

namespace Arpg
{
    void OnScalableCreatureAdded(Creature* creature)
    {
        if (!DungeonsOn() || !creature || KindOf(creature) == KIND_NONE)
            return;
        creature->m_events.AddEvent(new UnitLambdaEvent(*creature, [](Unit& unit)
        {
            Settle(static_cast<Creature*>(&unit));
        }), creature->m_events.CalculateTime(SCALE_DELAY_MS));
    }

    void OnScaledAggro(Creature* creature, Unit* enemy)
    {
        if (!DungeonsOn() || !creature)
            return;
        Kind const kind = KindOf(creature);
        if (kind == KIND_NONE)
            return;
        uint32 const players = PlayersFor(creature, kind, enemy);
        uint8 const tier = TierOf(creature->GetMap());
        {
            std::lock_guard<std::mutex> guard(sScaledLock);
            auto it = sScaled.find(creature->GetObjectGuid());
            if (it != sScaled.end() && it->second.players == players && it->second.tier == tier)
                return;
        }
        Apply(creature, kind, players);
    }

    uint32 DamageCap(Unit const* attacker, Unit const* victim, bool spell)
    {
        if (!DungeonsOn() || !attacker || !victim || attacker->GetTypeId() != TYPEID_UNIT || !Active(victim))
            return 0;
        Creature const* creature = static_cast<Creature const*>(attacker);
        if (PlayerOwned(creature))
            return 0;
        Map const* map = creature->GetMap();
        CreatureInfo const* info = creature->GetCreatureInfo();
        if (!map || !info)
            return 0;
        bool const champion = ChampionTier(creature) != 0;
        Caps caps;
        if (map->IsDungeon())
            caps = IsDungeonBoss(creature) ? (map->IsRaid() ? CAP_RAID_BOSS : CAP_DUNGEON_BOSS) :
                   champion ? CAP_CHAMPION : CAP_TRASH;
        else if (info->Rank == CREATURE_ELITE_WORLDBOSS)
            caps = CAP_RAID_BOSS;
        else if (champion || EliteRank(info->Rank))
            caps = CAP_CHAMPION;
        else
            return 0;                                       // open-world mobs hit as vanilla
        float const pct = float(spell ? caps.spell : caps.melee) * TIER_CAP[TierOf(creature->GetMap())];
        return std::max<uint32>(1, uint32(float(victim->GetMaxHealth()) * pct / 100.0f));
    }

    bool IsDungeonBoss(Creature const* creature)
    {
        if (!creature)
            return false;
        CreatureInfo const* info = creature->GetCreatureInfo();
        if (!info)
            return false;
        if (info->Rank == CREATURE_ELITE_WORLDBOSS)
            return true;
        if (creature->GetScriptName().compare(0, 5, "boss_") == 0)
            return true;
        Map const* map = creature->GetMap();
        if (!map || !map->IsDungeon() || map->IsRaid())
            return false;
        float const threshold = creature->GetLevel() < BOSS_HEALTH_LEVEL ? BOSS_HEALTH_LOW : BOSS_HEALTH_HIGH;
        return info->HealthMultiplier >= threshold;
    }

    void SetWantedTier(Player* player, uint8 tier)
    {
        if (!player)
            return;
        LoadTiers(player);
        std::lock_guard<std::mutex> guard(sTierLock);
        sPlayerTiers[player->GetObjectGuid()].wanted = std::min<uint8>(tier, MAX_TIER);
    }

    float TierDamageMod(Unit const* attacker)
    {
        if (!DungeonsOn() || !attacker || attacker->GetTypeId() != TYPEID_UNIT || !attacker->GetMap()->IsDungeon() ||
                PlayerOwned(static_cast<Creature const*>(attacker)))
            return 1.0f;
        return TIER_DAMAGE[TierOf(attacker->GetMap())];
    }

    float TierXpMod(Unit const* victim)
    {
        if (!DungeonsOn() || !victim || victim->GetTypeId() != TYPEID_UNIT || !victim->GetMap()->IsDungeon())
            return 1.0f;
        return TIER_XP[TierOf(victim->GetMap())];
    }

    void ForgetTiers(Player* player)
    {
        if (!player)
            return;
        std::lock_guard<std::mutex> guard(sTierLock);
        sPlayerTiers.erase(player->GetObjectGuid());
    }

    void OnDungeonLoot(Creature* victim)
    {
        if (!DungeonsOn() || !victim || !victim->m_loot || !victim->GetMap()->IsDungeon())
            return;
        Loot* loot = victim->m_loot;
        uint8 const tier = TierOf(victim->GetMap());
        uint32 const level = victim->GetLevel() + tier * TIER_ITEM_LEVELS;
        uint32 const epicBonus = tier * TIER_EPIC_PCT;
        uint32 const extraBlues = tier / 2;
        bool warden;
        {
            std::lock_guard<std::mutex> guard(sWardenLock);
            warden = sWardens.erase(victim->GetObjectGuid()) > 0;
        }
        if (warden)
        {
            for (uint32 i = 0; i < 2 + extraBlues; ++i)
                AddPick(loot, ITEM_QUALITY_RARE, level + WARDEN_ITEM_LEVELS);
            if (roll_chance_i(int32(WARDEN_EPIC_PCT + epicBonus)))
                AddPick(loot, ITEM_QUALITY_EPIC, level + WARDEN_ITEM_LEVELS);
            std::string name = ChampionName(victim);
            if (name.empty())
                name = victim->GetName();
            std::string const text = "The Warden " + name + " has fallen.";
            TellMap(victim->GetMap(), text.c_str());
            return;
        }
        std::vector<uint32> const& bosses = Bosses();
        if (std::find(bosses.begin(), bosses.end(), victim->GetEntry()) == bosses.end())
            return;
        // The Cache: gold, and for each player there a blue, a green and a chance at a purple.
        uint32 const players = std::max<uint32>(1, victim->GetMap()->GetPlayersCountExceptGMs());
        loot->AddArpgDevGold(level * level * 2 * players * (1 + tier));
        for (uint32 i = 0; i < players; ++i)
        {
            for (uint32 j = 0; j < 1 + extraBlues; ++j)
                AddPick(loot, ITEM_QUALITY_RARE, level + CACHE_ITEM_LEVELS);
            AddPick(loot, ITEM_QUALITY_UNCOMMON, level + CACHE_ITEM_LEVELS);
            if (roll_chance_i(int32(CACHE_EPIC_PCT + epicBonus)))
                AddPick(loot, ITEM_QUALITY_EPIC, level + CACHE_ITEM_LEVELS);
        }
        std::string const text = std::string(victim->GetName()) + "'s Cache spills open (" + TIER_NAME[tier] + ").";
        TellMap(victim->GetMap(), text.c_str());
        UnlockNext(victim->GetMap(), tier);
    }

    void ForgetScaled(Creature const* creature)
    {
        if (!creature)
            return;
        {
            std::lock_guard<std::mutex> guard(sScaledLock);
            sScaled.erase(creature->GetObjectGuid());
        }
        std::lock_guard<std::mutex> guard(sWardenLock);
        sWardens.erase(creature->GetObjectGuid());
    }
}
