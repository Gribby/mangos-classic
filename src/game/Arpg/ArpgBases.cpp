/*
 * ARPG base items past vanilla's: see ArpgBases.h.
 */

#include "Arpg/ArpgBases.h"
#include "Arpg/ArpgAffixes.h"
#include "Arpg/ArpgCombat.h"
#include "Arpg/ArpgDungeons.h"
#include "Arpg/ArpgUniques.h"

#include "Database/DatabaseEnv.h"
#include "Entities/Creature.h"
#include "Entities/Player.h"
#include "Globals/ObjectMgr.h"
#include "Log/Log.h"
#include "Loot/LootMgr.h"
#include "Maps/Map.h"
#include "World/World.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

namespace
{
    using namespace Arpg;

    // The bank's item levels, and the content each one's names belong to.
    struct BankTier { uint32 level; char const* word; };
    BankTier const BANK_TIERS[] =
    {
        { 64, "Emberforged" }, { 68, "Molten" }, { 72, "Draconic" }, { 76, "Chromatic" },
        { 80, "Qiraji" }, { 84, "Obsidian" }, { 88, "Necrotic" }, { 92, "Frostbound" },
    };
    constexpr uint32 BANK_SLOTS_PER_TIER = 100;

    // A base's kind: item class, subclass and inventory type (a robe counts as a chest, a main- or
    // off-hand-only one-hander as a one-hander), and its name's noun.
    struct Kind { uint8 cls; uint8 sub; uint8 inv; char const* noun; };

    enum : uint8 { CLOTH = 1, LEATHER = 2, MAIL = 3, PLATE = 4 };
    Kind const KINDS[] =
    {
        // Armour: head, shoulders, chest, waist, legs, feet, wrists, hands.
        { 4, CLOTH, 1, "Hood" }, { 4, CLOTH, 3, "Mantle" }, { 4, CLOTH, 5, "Robe" }, { 4, CLOTH, 6, "Sash" },
        { 4, CLOTH, 7, "Leggings" }, { 4, CLOTH, 8, "Slippers" }, { 4, CLOTH, 9, "Cuffs" }, { 4, CLOTH, 10, "Gloves" },
        { 4, LEATHER, 1, "Cap" }, { 4, LEATHER, 3, "Spaulders" }, { 4, LEATHER, 5, "Tunic" }, { 4, LEATHER, 6, "Belt" },
        { 4, LEATHER, 7, "Pants" }, { 4, LEATHER, 8, "Boots" }, { 4, LEATHER, 9, "Bracers" }, { 4, LEATHER, 10, "Grips" },
        { 4, MAIL, 1, "Coif" }, { 4, MAIL, 3, "Pauldrons" }, { 4, MAIL, 5, "Hauberk" }, { 4, MAIL, 6, "Girdle" },
        { 4, MAIL, 7, "Legguards" }, { 4, MAIL, 8, "Sabatons" }, { 4, MAIL, 9, "Wristguards" }, { 4, MAIL, 10, "Gauntlets" },
        { 4, PLATE, 1, "Helm" }, { 4, PLATE, 3, "Shoulderplates" }, { 4, PLATE, 5, "Breastplate" }, { 4, PLATE, 6, "Waistguard" },
        { 4, PLATE, 7, "Legplates" }, { 4, PLATE, 8, "Greaves" }, { 4, PLATE, 9, "Vambraces" }, { 4, PLATE, 10, "Handguards" },
        // Back, neck, finger, shield, held in the off hand.
        { 4, CLOTH, 16, "Cloak" }, { 4, 0, 2, "Choker" }, { 4, 0, 11, "Band" }, { 4, 6, 14, "Bulwark" }, { 4, 0, 23, "Tome" },
        // Weapons.
        { 2, 0, 13, "Hatchet" }, { 2, 1, 17, "Greataxe" }, { 2, 2, 15, "Longbow" }, { 2, 3, 26, "Rifle" },
        { 2, 4, 13, "Mace" }, { 2, 5, 17, "Maul" }, { 2, 6, 17, "Halberd" }, { 2, 7, 13, "Blade" },
        { 2, 8, 17, "Greatsword" }, { 2, 10, 17, "Staff" }, { 2, 13, 13, "Fist" }, { 2, 15, 13, "Dagger" },
        { 2, 18, 26, "Crossbow" }, { 2, 19, 26, "Wand" },
    };
    static_assert(sizeof(KINDS) / sizeof(KINDS[0]) < BANK_SLOTS_PER_TIER, "a tier's slots overflow its entries");

    // A base sits this share of the way to vanilla's blues and epics of its kind.
    constexpr float BASE_SHARE = 0.9f;

    // The inventory type a kind matches on (a robe is a chest, a main-hand one-hander a one-hander).
    uint8 InvGroup(uint32 inv)
    {
        switch (inv)
        {
            case 20: return 5;              // robe
            case 21: case 22: return 13;    // main hand, off hand
            default: return uint8(inv);
        }
    }

    std::string InvList(uint8 inv)
    {
        switch (inv)
        {
            case 5: return "5, 20";
            case 13: return "13, 21, 22";
            default: return std::to_string(inv);
        }
    }

    struct Banked { uint32 entry; uint8 cls, sub, inv; uint32 level; };
    std::mutex sBankLock;
    std::vector<Banked> sBank;

    // A least-squares line through (item level, value), or a level scale from the mean when the
    // points do not make one; 0 when there are none.
    struct Fit
    {
        double sumL = 0, sumV = 0, sumLL = 0, sumLV = 0;
        uint32 n = 0;
        void Add(double l, double v) { if (v <= 0) return; sumL += l; sumV += v; sumLL += l * l; sumLV += l * v; ++n; }
        double At(double l) const
        {
            if (!n)
                return 0;
            double const meanL = sumL / n, meanV = sumV / n;
            double const den = sumLL - sumL * meanL;
            if (n >= 3 && den > 1e-6)
            {
                double const slope = (sumLV - sumL * meanV) / den;
                if (slope > 0)
                    return meanV + slope * (l - meanL);
            }
            // Too few, or a flat cloud: vanilla's items grow about 2.5% an item level.
            return meanV * (1.0 + 0.025 * (l - meanL));
        }
    };

    using KindKey = std::tuple<uint8, uint8, uint8>;

    std::string Escaped(std::string const& text)
    {
        std::string out;
        for (char c : text)
        {
            if (c == '\'' || c == '\\')
                out += '\\';
            out += c;
        }
        return out;
    }

    // Copy item `donor`'s row to `entry`, every column but `overrides` (column -> SQL literal) as
    // the donor has it; only the columns this database's item_template has are named.
    void CopyRow(std::vector<std::string> const& columns, uint32 entry, uint32 donor, std::map<std::string, std::string> const& overrides)
    {
        std::string names, values;
        for (std::string const& column : columns)
        {
            if (!names.empty())
            {
                names += ", ";
                values += ", ";
            }
            names += "`" + column + "`";
            auto it = overrides.find(column);
            values += it != overrides.end() ? it->second : "`" + column + "`";
        }
        // INSERT ... SELECT from the same table is MySQL's documented way (not REPLACE).
        WorldDatabase.DirectPExecute("DELETE FROM item_template WHERE entry = %u", entry);
        WorldDatabase.DirectPExecute("INSERT INTO item_template (%s) SELECT %s FROM item_template WHERE entry = %u",
                                     names.c_str(), values.c_str(), donor);
    }

    // Whether `proto` is a white or green a raid's base swaps for: something to wear or wield,
    // with no mechanic of its own.
    bool Swappable(ItemPrototype const* proto)
    {
        if (!proto || proto->Quality < ITEM_QUALITY_NORMAL || proto->Quality > ITEM_QUALITY_UNCOMMON)
            return false;
        if (proto->Class != ITEM_CLASS_WEAPON && proto->Class != ITEM_CLASS_ARMOR)
            return false;
        if (proto->ItemId >= BANK_BASE_ENTRY && proto->ItemId < BANK_BASE_ENTRY + BANK_SLOTS_PER_TIER * std::size(BANK_TIERS))
            return false;
        for (UniqueMechanic const& u : Uniques())
            if (u.item == proto->ItemId)
                return false;
        return true;
    }

    // The bank's base of `proto`'s kind at the tier for `level`, or 0.
    uint32 BaseLike(ItemPrototype const* proto, uint32 level)
    {
        uint8 const inv = InvGroup(proto->InventoryType);
        uint32 best = 0, bestLevel = 0;
        std::lock_guard<std::mutex> guard(sBankLock);
        for (Banked const& b : sBank)
            if (b.cls == proto->Class && b.sub == proto->SubClass && b.inv == inv && b.level <= std::max(level, BANK_FROM) &&
                    b.level >= bestLevel)
            {
                best = b.entry;
                bestLevel = b.level;
            }
        return best;
    }

    // Raids' drops: a trash foe's chance at a rolled base, a boss's count of them.
    constexpr uint32 RAID_TRASH_PCT = 12, RAID_BOSS_BASES = 2;
}

namespace Arpg
{
    void EnsureBaseItems()
    {
        if (!sWorld.getConfig(CONFIG_BOOL_ARPG_ENABLE))
            return;
        std::vector<std::string> columns;
        if (auto result = WorldDatabase.Query("SELECT COLUMN_NAME FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() "
                                              "AND TABLE_NAME = 'item_template' ORDER BY ORDINAL_POSITION"))
        {
            do
                columns.push_back(result->Fetch()[0].GetCppString());
            while (result->NextRow());
        }
        if (columns.empty())
        {
            sLog.outError("ARPG bases: item_template's columns could not be read; no bank written");
            return;
        }
        auto has = [&columns](char const* column) { return std::find(columns.begin(), columns.end(), column) != columns.end(); };

        // Vanilla's blues and epics of each kind: armour, damage a second and block against item level.
        std::map<KindKey, Fit> armour, dps, block;
        if (auto result = WorldDatabase.PQuery(
                    "SELECT class, subclass, InventoryType, ItemLevel, armor, dmg_min1, dmg_max1, delay, block FROM item_template "
                    "WHERE Quality IN (3, 4) AND class IN (2, 4) AND ItemLevel BETWEEN 50 AND 95 AND entry < %u", BANK_BASE_ENTRY))
        {
            do
            {
                Field* f = result->Fetch();
                KindKey const key{ uint8(f[0].GetUInt32()), uint8(f[1].GetUInt32()), InvGroup(f[2].GetUInt32()) };
                double const level = f[3].GetUInt32();
                armour[key].Add(level, f[4].GetUInt32());
                uint32 const delay = f[7].GetUInt32();
                if (delay)
                    dps[key].Add(level, (f[5].GetFloat() + f[6].GetFloat()) * 0.5 / (delay / 1000.0));
                block[key].Add(level, f[8].GetUInt32());
            }
            while (result->NextRow());
        }

        std::vector<Banked> bank;
        for (size_t t = 0; t < std::size(BANK_TIERS); ++t)
        {
            BankTier const& tier = BANK_TIERS[t];
            for (size_t k = 0; k < std::size(KINDS); ++k)
            {
                Kind const& kind = KINDS[k];
                KindKey const key{ kind.cls, kind.sub, kind.inv };
                // The look: a vanilla blue or epic (else a green) of its kind, nearest its level.
                uint32 donor = 0, delay = 0;
                if (auto result = WorldDatabase.PQuery(
                            "SELECT entry, delay FROM item_template WHERE class = %u AND subclass = %u AND InventoryType IN (%s) "
                            "AND Quality IN (2, 3, 4) AND ItemLevel BETWEEN 45 AND 95 AND entry < %u "
                            "ORDER BY Quality < 3, ABS(CAST(ItemLevel AS SIGNED) - %u), entry LIMIT 1",
                            uint32(kind.cls), uint32(kind.sub), InvList(kind.inv).c_str(), BANK_BASE_ENTRY, tier.level))
                {
                    donor = result->Fetch()[0].GetUInt32();
                    delay = result->Fetch()[1].GetUInt32();
                }
                if (!donor)
                    continue;
                uint32 const entry = BANK_BASE_ENTRY + uint32(t) * BANK_SLOTS_PER_TIER + uint32(k);
                std::string const name = std::string(tier.word) + " " + kind.noun;
                std::map<std::string, std::string> set;
                auto put = [&](char const* column, std::string const& value) { if (has(column)) set[column] = value; };
                auto num = [&](char const* column, double value) { put(column, std::to_string(int64(std::llround(value)))); };
                put("entry", std::to_string(entry));
                put("name", "'" + Escaped(name) + "'");
                num("Quality", ITEM_QUALITY_NORMAL);
                for (char const* zero : { "Flags", "BuyPrice", "RequiredSkill", "RequiredSkillRank", "requiredspell",
                                          "requiredhonorrank", "RequiredCityRank", "RequiredReputationFaction",
                                          "RequiredReputationRank", "maxcount", "holy_res", "fire_res", "nature_res",
                                          "frost_res", "shadow_res", "arcane_res", "bonding", "PageText", "startquest",
                                          "lockid", "RandomProperty", "itemset", "area", "Map", "DisenchantID", "Duration",
                                          "ExtraFlags" })
                    num(zero, 0);
                num("AllowableClass", -1);
                num("AllowableRace", -1);
                num("ItemLevel", tier.level);
                num("RequiredLevel", 60);
                num("SellPrice", double(tier.level) * tier.level * 12);
                put("description", "''");
                put("ScriptName", "''");
                for (int i = 1; i <= 10; ++i)
                {
                    num(("stat_type" + std::to_string(i)).c_str(), 0);
                    num(("stat_value" + std::to_string(i)).c_str(), 0);
                }
                for (int i = 2; i <= 5; ++i)
                {
                    num(("dmg_min" + std::to_string(i)).c_str(), 0);
                    num(("dmg_max" + std::to_string(i)).c_str(), 0);
                }
                for (int i = 1; i <= 5; ++i)
                {
                    std::string const n = std::to_string(i);
                    num(("spellid_" + n).c_str(), 0);
                    num(("spelltrigger_" + n).c_str(), 0);
                    num(("spellcharges_" + n).c_str(), 0);
                    num(("spellppmRate_" + n).c_str(), 0);
                    num(("spellcooldown_" + n).c_str(), -1);
                    num(("spellcategory_" + n).c_str(), 0);
                    num(("spellcategorycooldown_" + n).c_str(), -1);
                }
                // Its own numbers, fitted to vanilla's best of its kind at its level.
                double const level = tier.level;
                if (kind.cls == ITEM_CLASS_WEAPON)
                {
                    double const perSecond = dps[key].At(level) * BASE_SHARE;
                    if (perSecond <= 0 || !delay)
                        continue;
                    double const average = perSecond * delay / 1000.0;
                    num("dmg_min1", average * 0.77);
                    num("dmg_max1", average * 1.23);
                    num("armor", 0);
                }
                else
                {
                    num("armor", armour[key].At(level) * BASE_SHARE);
                    if (kind.sub == 6)
                        num("block", block[key].At(level) * BASE_SHARE);
                }
                CopyRow(columns, entry, donor, set);
                bank.push_back({ entry, kind.cls, kind.sub, kind.inv, tier.level });
            }
        }
        {
            std::lock_guard<std::mutex> guard(sBankLock);
            sBank = bank;
        }
        sLog.outString("ARPG bases: wrote %u raid-level base items into item_template", uint32(bank.size()));
    }

    uint32 RandomBase(uint32 level)
    {
        if (level < BANK_FROM)
            return 0;
        // The nearest tier at or under the level, any kind.
        uint32 tierLevel = 0;
        for (BankTier const& t : BANK_TIERS)
            if (t.level <= level)
                tierLevel = t.level;
        std::vector<uint32> found;
        {
            std::lock_guard<std::mutex> guard(sBankLock);
            for (Banked const& b : sBank)
                if (b.level == tierLevel)
                    found.push_back(b.entry);
        }
        // Only the ones the item templates loaded (a bank written after they were, first run).
        found.erase(std::remove_if(found.begin(), found.end(), [](uint32 id) { return !sObjectMgr.GetItemPrototype(id); }),
                    found.end());
        return found.empty() ? 0 : found[urand(0, uint32(found.size() - 1))];
    }

    uint32 RaidItemLevel(Map* map)
    {
        if (!map || !map->IsRaid())
            return 0;
        uint32 level;
        switch (map->GetId())
        {
            case 309: level = 64; break;     // Zul'Gurub
            case 409: level = 66; break;     // Molten Core
            case 249: level = 68; break;     // Onyxia's Lair
            case 509: level = 66; break;     // Ruins of Ahn'Qiraj
            case 469: level = 74; break;     // Blackwing Lair
            case 531: level = 80; break;     // Temple of Ahn'Qiraj
            case 533: level = 88; break;     // Naxxramas
            default:  level = 66; break;
        }
        return level + uint32(LootTier(map)) * 2;
    }

    void OnRaidLoot(Creature* victim)
    {
        if (!victim || !victim->m_loot || !sWorld.getConfig(CONFIG_BOOL_ARPG_ENABLE))
            return;
        Player* looter = victim->GetLootRecipient();
        if (!looter || !Active(looter))
            return;
        uint32 const level = RaidItemLevel(victim->GetMap());
        if (!level)
            return;
        Loot* loot = victim->m_loot;
        // Its own whites and greens: the raid's bases of their kind.
        loot->ForEachArpgItem([level](LootItem* item)
        {
            if (item->arpgSeed || !Swappable(item->itemProto))
                return;
            uint32 const base = BaseLike(item->itemProto, level);
            ItemPrototype const* proto = base ? sObjectMgr.GetItemPrototype(base) : nullptr;
            if (!proto)
                return;
            item->itemId = base;
            item->itemProto = proto;
            item->displayID = proto->DisplayInfoID;
            item->randomPropertyId = 0;
            item->randomSuffix = 0;
        });
        // And rolled bases of its own: a boss a few blues and purples, a foe now and then one.
        bool const boss = IsDungeonBoss(victim) || victim->IsWorldBoss();
        uint32 const count = boss ? RAID_BOSS_BASES + LootTier(victim->GetMap()) / 2 : (roll_chance_i(int32(RAID_TRASH_PCT)) ? 1 : 0);
        for (uint32 i = 0; i < count; ++i)
        {
            uint32 const roll = urand(1, 100);
            uint8 const rarity = boss ? (roll <= 55 ? RARITY_BLUE : RARITY_PURPLE)
                                      : (roll <= 50 ? RARITY_GREEN : roll <= 85 ? RARITY_BLUE : RARITY_PURPLE);
            AddRolledItem(loot, rarity, level);
        }
    }
}
