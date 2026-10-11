/*
 * ARPG item affixes: see ArpgAffixes.h.
 */

#include "Arpg/ArpgAffixes.h"
#include "Arpg/ArpgCombat.h"
#include "Arpg/ArpgDungeons.h"
#include "Arpg/ArpgTags.h"
#include "Arpg/ArpgUniques.h"

#include "Database/DatabaseEnv.h"
#include "Entities/Item.h"
#include "Entities/Player.h"
#include "Globals/ObjectMgr.h"
#include "Loot/LootMgr.h"
#include "Server/Opcodes.h"
#include "Server/WorldPacket.h"
#include "Server/WorldSession.h"
#include "World/World.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace
{
    using namespace Arpg;

    enum AffixKind : uint8 { AK_STAT, AK_TAG, AK_HEALING, AK_ARMOUR, AK_BLOCK, AK_MOVE, AK_COOLDOWN, AK_LIFE_ON_KILL, AK_MANA_ON_HIT };

    struct AffixDef
    {
        AffixKind kind;
        uint8 param;              // the stat or the tag
        int32 low, high;          // the range at item level 60, Normal
        char const* format;       // the tooltip line, %d the value
        uint32 onlyType;          // an InventoryType it is limited to (0: any)
        int32 cap;                // the most one affix may give (0: none), for stats with a cap
    };

    AffixDef const AFFIXES[] =
    {
        { AK_STAT, STAT_STRENGTH,  8, 30, "+%d Strength", 0, 0 },
        { AK_STAT, STAT_AGILITY,   8, 30, "+%d Agility", 0, 0 },
        { AK_STAT, STAT_STAMINA,   8, 30, "+%d Stamina", 0, 0 },
        { AK_STAT, STAT_INTELLECT, 8, 30, "+%d Intellect", 0, 0 },
        { AK_STAT, STAT_SPIRIT,    8, 30, "+%d Spirit", 0, 0 },
        { AK_TAG, TAG_HOLY,        6, 20, "+%d%% Holy damage", 0, 0 },
        { AK_TAG, TAG_PHYSICAL,    6, 20, "+%d%% Physical damage", 0, 0 },
        { AK_TAG, TAG_FIRE,        6, 20, "+%d%% Fire damage", 0, 0 },
        { AK_TAG, TAG_FROST,       6, 20, "+%d%% Frost damage", 0, 0 },
        { AK_TAG, TAG_ARCANE,      6, 20, "+%d%% Arcane damage", 0, 0 },
        { AK_TAG, TAG_NATURE,      6, 20, "+%d%% Nature damage", 0, 0 },
        { AK_TAG, TAG_SHADOW,      6, 20, "+%d%% Shadow damage", 0, 0 },
        { AK_TAG, TAG_AREA,        8, 24, "+%d%% Area damage", 0, 0 },
        { AK_TAG, TAG_PROJECTILE,  6, 18, "+%d%% Projectile damage", 0, 0 },
        { AK_TAG, TAG_MELEE,       5, 16, "+%d%% Melee damage", 0, 0 },
        { AK_TAG, TAG_SPELL,       5, 16, "+%d%% Spell damage", 0, 0 },
        { AK_HEALING, 0,           5, 18, "+%d%% healing", 0, 0 },
        { AK_ARMOUR, 0,            5, 25, "+%d%% armour", 0, 60 },
        { AK_BLOCK, 0,             2, 6,  "+%d%% block chance", INVTYPE_SHIELD, 15 },
        { AK_MOVE, 0,              3, 8,  "+%d%% movement speed", INVTYPE_FEET, 15 },
        { AK_COOLDOWN, 0,          3, 10, "+%d%% cooldown recovery", 0, 20 },
        { AK_LIFE_ON_KILL, 0,      5, 40, "+%d life on kill", 0, 0 },
        { AK_MANA_ON_HIT, 0,       2, 10, "+%d mana on hit", 0, 0 },
    };

    // A value's share of the level 60 range, by item level: it grows by half again every nine
    // levels or so, so a level 20 drop rolls about a seventh of a level 60 one, Molten Core a
    // third more, Blackwing Lair twice, and Naxxramas three to four times.
    constexpr float SCALE_PER_LEVEL = 0.04777f;     // ln 3 / 23: three times at item level 83
    constexpr float SCALE_MIN = 0.06f, SCALE_MAX = 4.0f, SCALE_LEVEL = 60.0f;
    // Each dungeon tier the item dropped at raises it again: Normal to Torment III.
    float const TIER_SCALE[MAX_TIER + 1] = { 1.0f, 1.15f, 1.3f, 1.5f, 1.75f, 2.0f };
    // A raid's purples (from this item level) and anything from Torment I up roll one more affix.
    constexpr uint32 RAID_ITEM_LEVEL = 66;
    constexpr uint8 TORMENT_TIER = 3;
    // The tier's word over the affixes, from Hard up.
    char const* const TIER_COLOUR[MAX_TIER + 1] = { "", "|cff9d9d9d", "|cffe6cc80", "|cffff8000", "|cffff5020", "|cffff2020" };

    float LevelScale(uint32 itemLevel)
    {
        float const scale = std::exp(SCALE_PER_LEVEL * (float(itemLevel) - SCALE_LEVEL));
        return std::max(SCALE_MIN, std::min(SCALE_MAX, scale));
    }

    struct Rolled
    {
        AffixDef const* def;
        int32 value;
    };

    // A small, fixed generator, so a seed always gives the same affixes.
    struct Rng
    {
        uint32 state;
        uint32 Next()
        {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            return state;
        }
    };

    bool Takes(ItemPrototype const* proto)
    {
        if (!proto || proto->Quality < ITEM_QUALITY_UNCOMMON || proto->Quality > ITEM_QUALITY_EPIC)
            return false;
        if (proto->Class != ITEM_CLASS_WEAPON && proto->Class != ITEM_CLASS_ARMOR)
            return false;
        if (proto->InventoryType == INVTYPE_NON_EQUIP || proto->InventoryType == INVTYPE_AMMO)
            return false;
        for (UniqueMechanic const& unique : Uniques())
            if (unique.item == proto->ItemId)
                return false;
        return true;
    }

    // How many affixes: green 1, blue 2, purple 3; a raid purple and a Torment drop one more each.
    uint32 AffixCount(ItemPrototype const* proto, uint8 tier)
    {
        uint32 count = proto->Quality - ITEM_QUALITY_NORMAL;
        if (proto->Quality == ITEM_QUALITY_EPIC && proto->ItemLevel >= RAID_ITEM_LEVEL)
            ++count;
        if (tier >= TORMENT_TIER)
            ++count;
        return count;
    }

    std::vector<Rolled> Roll(ItemPrototype const* proto, uint32 seed, uint8 tier)
    {
        std::vector<Rolled> out;
        if (!Takes(proto) || !seed)
            return out;
        tier = std::min<uint8>(tier, MAX_TIER);
        Rng rng{ seed ^ (proto->ItemId * 2654435761u) };
        if (!rng.state)
            rng.state = 0x9E3779B9u;
        uint32 const count = AffixCount(proto, tier);
        float const scale = LevelScale(proto->ItemLevel) * TIER_SCALE[tier];
        std::vector<AffixDef const*> pool;
        for (AffixDef const& def : AFFIXES)
            if (!def.onlyType || def.onlyType == proto->InventoryType)
                pool.push_back(&def);
        for (uint32 i = 0; i < count && !pool.empty(); ++i)
        {
            size_t const pick = rng.Next() % pool.size();
            AffixDef const* def = pool[pick];
            pool.erase(pool.begin() + pick);
            int32 const span = def->high - def->low;
            int32 const raw = def->low + int32(rng.Next() % uint32(span + 1));
            int32 value = std::max<int32>(1, int32(float(raw) * scale + 0.5f));
            if (def->cap)
                value = std::min(value, def->cap);
            out.push_back({ def, value });
        }
        return out;
    }

    struct Seeded
    {
        uint32 seed;
        uint32 entry;             // the item it was rolled for, so a reused guid never takes it
        uint8 tier;               // the dungeon tier it dropped at
    };

    std::mutex sSeedsLock;
    std::unordered_map<uint32, Seeded> sSeeds;      // item guid low -> its seed

    Seeded SeedOf(Item const* item)
    {
        std::lock_guard<std::mutex> guard(sSeedsLock);
        auto it = sSeeds.find(item->GetGUIDLow());
        if (it == sSeeds.end() || it->second.entry != item->GetEntry())
            return { 0, 0, 0 };
        return it->second;
    }

    void EnsureTable()
    {
        static bool const created = []()
        {
            CharacterDatabase.DirectExecute(
                "CREATE TABLE IF NOT EXISTS character_arpg_item ("
                "item INT UNSIGNED NOT NULL, entry MEDIUMINT UNSIGNED NOT NULL, seed INT UNSIGNED NOT NULL, "
                "tier TINYINT UNSIGNED NOT NULL DEFAULT 0, "
                "PRIMARY KEY (item)) ENGINE=InnoDB DEFAULT CHARSET=utf8 COMMENT='ARPG item affix seeds (Arpg/ArpgAffixes.h)'");
            // A table from before tiers: add the column.
            if (!CharacterDatabase.Query("SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() "
                                         "AND TABLE_NAME = 'character_arpg_item' AND COLUMN_NAME = 'tier'"))
                CharacterDatabase.DirectExecute("ALTER TABLE character_arpg_item ADD COLUMN tier TINYINT UNSIGNED NOT NULL DEFAULT 0");
            return true;
        }();
        (void)created;
    }

    std::string Joined(std::vector<std::string> const& lines)
    {
        std::string out;
        for (std::string const& line : lines)
        {
            if (!out.empty())
                out += '\n';
            out += line;
        }
        return out;
    }

    void SendLines(Player* player, std::vector<std::pair<ObjectGuid, std::string>> const& items)
    {
        if (items.empty())
            return;
        // Chunks keep each packet small.
        for (size_t at = 0; at < items.size(); at += 64)
        {
            size_t const end = std::min(items.size(), at + 64);
            WorldPacket data(SMSG_ARPG_ITEM_AFFIXES, 2 + (end - at) * 80);
            data << uint16(end - at);
            for (size_t i = at; i < end; ++i)
                data << items[i].first << items[i].second;
            player->GetSession()->SendPacket(data);
        }
    }
}

namespace Arpg
{
    std::vector<std::string> AffixLines(uint32 entry, uint32 seed, uint8 tier)
    {
        std::vector<std::string> lines;
        std::vector<Rolled> const rolled = Roll(sObjectMgr.GetItemPrototype(entry), seed, tier);
        tier = std::min<uint8>(tier, MAX_TIER);
        if (!rolled.empty() && tier)
            lines.push_back(std::string(TIER_COLOUR[tier]) + TierName(tier) + "|r");
        for (Rolled const& r : rolled)
        {
            char buf[96];
            std::snprintf(buf, sizeof(buf), r.def->format, int(r.value));
            lines.emplace_back(buf);
        }
        return lines;
    }

    void SeedArpgLoot(Loot* loot, Player* looter, uint8 tier)
    {
        if (!loot || !looter || !Active(looter))
            return;
        tier = std::min<uint8>(tier, MAX_TIER);
        loot->ForEachArpgItem([tier](LootItem* item)
        {
            if (!item->arpgSeed && Takes(item->itemProto))
            {
                item->arpgSeed = urand(1, 0x7FFFFFFF);
                item->arpgTier = tier;
            }
        });
    }

    void OnAffixedItemStored(Player* player, Item* item, uint32 seed, uint8 tier)
    {
        if (!player || !item || !seed || !Takes(item->GetProto()))
            return;
        // The looter's own roll: two players taking one free-for-all drop get different affixes.
        seed ^= player->GetGUIDLow() * 0x9E3779B1u;
        if (!seed)
            seed = 1;
        EnsureTable();
        {
            std::lock_guard<std::mutex> guard(sSeedsLock);
            sSeeds[item->GetGUIDLow()] = { seed, item->GetEntry(), tier };
        }
        CharacterDatabase.PExecute("REPLACE INTO character_arpg_item (item, entry, seed, tier) VALUES (%u, %u, %u, %u)",
                                   item->GetGUIDLow(), item->GetEntry(), seed, uint32(tier));
        SendLines(player, { { item->GetObjectGuid(), Joined(AffixLines(item->GetEntry(), seed, tier)) } });
    }

    void AddItemTotals(Player const* player, WebTotals& totals)
    {
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item || item->IsBroken())
                continue;
            Seeded const seeded = SeedOf(item);
            if (!seeded.seed)
                continue;
            for (Rolled const& r : Roll(item->GetProto(), seeded.seed, seeded.tier))
            {
                switch (r.def->kind)
                {
                    case AK_STAT: totals.stat[r.def->param] += r.value; break;
                    case AK_TAG: totals.tag[r.def->param] += r.value; break;
                    case AK_HEALING: totals.healingPct += r.value; break;
                    case AK_ARMOUR: totals.armourPct += r.value; break;
                    case AK_BLOCK: totals.blockPct += r.value; break;
                    case AK_MOVE: totals.movePct += r.value; break;
                    case AK_COOLDOWN: totals.cooldownPct += r.value; break;
                    case AK_LIFE_ON_KILL: totals.lifeOnKill += r.value; break;
                    case AK_MANA_ON_HIT: totals.manaOnHit += r.value; break;
                }
            }
        }
    }

    void OnEquipChanged(Player* player, Item* item)
    {
        if (!player || !item || !Active(player) || !SeedOf(item).seed)
            return;
        // Out of the equip path: the totals re-apply stats of their own.
        player->m_events.AddEvent(new UnitLambdaEvent(*player, [](Unit& unit)
        {
            if (unit.IsInWorld())
                RefreshTotals(static_cast<Player*>(&unit));
        }), player->m_events.CalculateTime(1));
    }

    void LoadAffixes(Player* player)
    {
        if (!player)
            return;
        EnsureTable();
        std::vector<std::pair<ObjectGuid, std::string>> lines;
        // By the item's holder now (a trade or the mail moves it), and only for the item the seed
        // was rolled for: a guid reused after a crash, or a row left by a sold item, takes none.
        if (auto result = CharacterDatabase.PQuery(
                    "SELECT a.item, a.seed, a.entry, a.tier FROM character_arpg_item a JOIN item_instance i ON i.guid = a.item "
                    "WHERE i.owner_guid = %u AND i.itemEntry = a.entry", player->GetGUIDLow()))
        {
            do
            {
                Field* fields = result->Fetch();
                uint32 const low = fields[0].GetUInt32();
                uint32 const seed = fields[1].GetUInt32();
                uint32 const entry = fields[2].GetUInt32();
                uint8 const tier = uint8(std::min<uint32>(fields[3].GetUInt32(), MAX_TIER));
                {
                    std::lock_guard<std::mutex> guard(sSeedsLock);
                    sSeeds[low] = { seed, entry, tier };
                }
                if (Item* item = player->GetItemByGuid(ObjectGuid(HIGHGUID_ITEM, low)))
                    lines.emplace_back(item->GetObjectGuid(), Joined(AffixLines(item->GetEntry(), seed, tier)));
            }
            while (result->NextRow());
        }
        SendLines(player, lines);
        RefreshTotals(player);
    }
}
