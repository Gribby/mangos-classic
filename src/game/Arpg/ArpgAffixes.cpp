/*
 * ARPG item affixes: see ArpgAffixes.h.
 */

#include "Arpg/ArpgAffixes.h"
#include "Arpg/ArpgCombat.h"
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
        int32 low, high;          // the range at item level 60
        char const* format;       // the tooltip line, %d the value
        uint32 onlyType;          // an InventoryType it is limited to (0: any)
    };

    AffixDef const AFFIXES[] =
    {
        { AK_STAT, STAT_STRENGTH,  4, 20, "+%d Strength", 0 },
        { AK_STAT, STAT_AGILITY,   4, 20, "+%d Agility", 0 },
        { AK_STAT, STAT_STAMINA,   4, 20, "+%d Stamina", 0 },
        { AK_STAT, STAT_INTELLECT, 4, 20, "+%d Intellect", 0 },
        { AK_STAT, STAT_SPIRIT,    4, 20, "+%d Spirit", 0 },
        { AK_TAG, TAG_HOLY,        2, 12, "+%d%% Holy damage", 0 },
        { AK_TAG, TAG_PHYSICAL,    2, 12, "+%d%% Physical damage", 0 },
        { AK_TAG, TAG_AREA,        3, 15, "+%d%% Area damage", 0 },
        { AK_TAG, TAG_MELEE,       2, 10, "+%d%% Melee damage", 0 },
        { AK_TAG, TAG_SPELL,       2, 10, "+%d%% Spell damage", 0 },
        { AK_HEALING, 0,           3, 12, "+%d%% healing", 0 },
        { AK_ARMOUR, 0,            3, 15, "+%d%% armour", 0 },
        { AK_BLOCK, 0,             1, 5,  "+%d%% block chance", INVTYPE_SHIELD },
        { AK_MOVE, 0,              2, 8,  "+%d%% movement speed", INVTYPE_FEET },
        { AK_COOLDOWN, 0,          2, 10, "+%d%% cooldown recovery", 0 },
        { AK_LIFE_ON_KILL, 0,      3, 30, "+%d life on kill", 0 },
        { AK_MANA_ON_HIT, 0,       6, 40, "+%d mana on hit", 0 },
    };

    // A value's share of the level 60 range, by item level: about a sixth at level 10, raid gear
    // above 60 a little more.
    constexpr float SCALE_LOW = 0.15f, SCALE_HIGH = 1.3f, SCALE_LEVEL = 60.0f;

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

    std::vector<Rolled> Roll(ItemPrototype const* proto, uint32 seed)
    {
        std::vector<Rolled> out;
        if (!Takes(proto) || !seed)
            return out;
        Rng rng{ seed ^ (proto->ItemId * 2654435761u) };
        if (!rng.state)
            rng.state = 0x9E3779B9u;
        uint32 const count = proto->Quality - ITEM_QUALITY_NORMAL;     // green 1, blue 2, purple 3
        float const scale = std::max(SCALE_LOW, std::min(SCALE_HIGH, float(proto->ItemLevel) / SCALE_LEVEL));
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
            out.push_back({ def, std::max<int32>(1, int32(float(raw) * scale + 0.5f)) });
        }
        return out;
    }

    struct Seeded
    {
        uint32 seed;
        uint32 entry;             // the item it was rolled for, so a reused guid never takes it
    };

    std::mutex sSeedsLock;
    std::unordered_map<uint32, Seeded> sSeeds;      // item guid low -> its seed

    uint32 SeedOf(Item const* item)
    {
        std::lock_guard<std::mutex> guard(sSeedsLock);
        auto it = sSeeds.find(item->GetGUIDLow());
        return it == sSeeds.end() || it->second.entry != item->GetEntry() ? 0 : it->second.seed;
    }

    void EnsureTable()
    {
        static bool const created = []()
        {
            CharacterDatabase.DirectExecute(
                "CREATE TABLE IF NOT EXISTS character_arpg_item ("
                "item INT UNSIGNED NOT NULL, entry MEDIUMINT UNSIGNED NOT NULL, seed INT UNSIGNED NOT NULL, "
                "PRIMARY KEY (item)) ENGINE=InnoDB DEFAULT CHARSET=utf8 COMMENT='ARPG item affix seeds (Arpg/ArpgAffixes.h)'");
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
    std::vector<std::string> AffixLines(uint32 entry, uint32 seed)
    {
        std::vector<std::string> lines;
        for (Rolled const& r : Roll(sObjectMgr.GetItemPrototype(entry), seed))
        {
            char buf[96];
            std::snprintf(buf, sizeof(buf), r.def->format, int(r.value));
            lines.emplace_back(buf);
        }
        return lines;
    }

    void SeedArpgLoot(Loot* loot, Player* looter)
    {
        if (!loot || !looter || !Active(looter))
            return;
        loot->ForEachArpgItem([](LootItem* item)
        {
            if (!item->arpgSeed && Takes(item->itemProto))
                item->arpgSeed = urand(1, 0x7FFFFFFF);
        });
    }

    void OnAffixedItemStored(Player* player, Item* item, uint32 seed)
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
            sSeeds[item->GetGUIDLow()] = { seed, item->GetEntry() };
        }
        CharacterDatabase.PExecute("REPLACE INTO character_arpg_item (item, entry, seed) VALUES (%u, %u, %u)",
                                   item->GetGUIDLow(), item->GetEntry(), seed);
        SendLines(player, { { item->GetObjectGuid(), Joined(AffixLines(item->GetEntry(), seed)) } });
    }

    void AddItemTotals(Player const* player, WebTotals& totals)
    {
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item || item->IsBroken())
                continue;
            uint32 const seed = SeedOf(item);
            if (!seed)
                continue;
            for (Rolled const& r : Roll(item->GetProto(), seed))
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
        if (!player || !item || !Active(player) || !SeedOf(item))
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
                    "SELECT a.item, a.seed, a.entry FROM character_arpg_item a JOIN item_instance i ON i.guid = a.item "
                    "WHERE i.owner_guid = %u AND i.itemEntry = a.entry", player->GetGUIDLow()))
        {
            do
            {
                Field* fields = result->Fetch();
                uint32 const low = fields[0].GetUInt32();
                uint32 const seed = fields[1].GetUInt32();
                uint32 const entry = fields[2].GetUInt32();
                {
                    std::lock_guard<std::mutex> guard(sSeedsLock);
                    sSeeds[low] = { seed, entry };
                }
                if (Item* item = player->GetItemByGuid(ObjectGuid(HIGHGUID_ITEM, low)))
                    lines.emplace_back(item->GetObjectGuid(), Joined(AffixLines(item->GetEntry(), seed)));
            }
            while (result->NextRow());
        }
        SendLines(player, lines);
        RefreshTotals(player);
    }
}
