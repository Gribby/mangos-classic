/*
 * ARPG item sets: see ArpgSets.h.
 */

#include "Arpg/ArpgSets.h"
#include "Arpg/ArpgBases.h"
#include "Arpg/ArpgCombat.h"
#include "Arpg/ArpgDungeons.h"
#include "Arpg/ArpgPacks.h"
#include "Arpg/ArpgTags.h"

#include "Database/DatabaseEnv.h"
#include "Entities/Creature.h"
#include "Entities/Item.h"
#include "Entities/Player.h"
#include "Log/Log.h"
#include "Loot/LootMgr.h"
#include "Server/Opcodes.h"
#include "Server/WorldPacket.h"
#include "Server/WorldSession.h"
#include "World/World.h"

#include <algorithm>
#include <map>
#include <string>

namespace
{
    using namespace Arpg;

    enum BonusKind : uint8
    {
        B_STAT,            // param: the attribute (Stats)
        B_TAG,             // param: the Tag; % damage
        B_ARMOUR,          // % armour
        B_ATTACK_SPEED,    // % attack speed
        B_CAST_SPEED,      // % cast speed
        B_CRIT_DAMAGE,     // % critical strike damage
        B_MANA,            // % maximum mana
        B_SKILLS,          // param: the skill group (0 Melee, 1 Spell, 2 Holy, 3 elemental); levels
        B_MINION_LIFE,     // % minion life (Arpg/ArpgMinions.h)
        B_HORDE,           // the horde's size
    };

    struct Bonus
    {
        uint8 pieces;
        BonusKind kind;
        uint8 param;
        int32 value;
        char const* text;
    };

    // A piece: its slot's inventory types (the first is the one it is written as, unless its look's
    // item is a robe) and its share of the attribute budget.
    struct SlotDef { uint32 inv; uint32 alt; char const* noun; float weight; };
    SlotDef const HEAD = { INVTYPE_HEAD, INVTYPE_HEAD, "Helm", 1.0f };
    SlotDef const SHOULDERS = { INVTYPE_SHOULDERS, INVTYPE_SHOULDERS, "Spaulders", 0.75f };
    SlotDef const CHEST = { INVTYPE_CHEST, INVTYPE_ROBE, "Chest", 1.0f };
    SlotDef const LEGS = { INVTYPE_LEGS, INVTYPE_LEGS, "Legs", 1.0f };
    SlotDef const HANDS = { INVTYPE_HANDS, INVTYPE_HANDS, "Gloves", 0.75f };
    SlotDef const FEET = { INVTYPE_FEET, INVTYPE_FEET, "Boots", 0.75f };

    struct PieceDef { SlotDef slot; char const* name; };

    struct SetDef
    {
        uint8 classId;
        uint32 level;
        uint8 armour;                 // ITEM_SUBCLASS_ARMOR_*
        char const* name;
        std::vector<PieceDef> pieces;
        std::vector<Bonus> bonuses;
        UniqueMechanic power;         // the whole set's
    };

    // Attribute shares of the pieces' budget, by class: Strength, Agility, Stamina, Intellect, Spirit.
    struct ClassStats { uint8 classId; float share[5]; };
    ClassStats const CLASS_STATS[] =
    {
        { CLASS_PALADIN, { 0.45f, 0.0f, 0.35f, 0.20f, 0.0f } },
        { CLASS_MAGE,    { 0.0f, 0.0f, 0.35f, 0.50f, 0.15f } },
        { CLASS_DRUID,   { 0.0f, 0.35f, 0.30f, 0.35f, 0.0f } },
        { CLASS_WARLOCK, { 0.0f, 0.0f, 0.40f, 0.45f, 0.15f } },
    };
    // The attribute budget of a piece: its level times this, times its slot's weight.
    constexpr float BUDGET_PER_LEVEL = 0.55f;
    // ItemModType of each attribute, in Stats order.
    uint32 const STAT_MOD[5] = { ITEM_MOD_STRENGTH, ITEM_MOD_AGILITY, ITEM_MOD_STAMINA, ITEM_MOD_INTELLECT, ITEM_MOD_SPIRIT };

    std::vector<SetDef> const& Sets()
    {
        static std::vector<SetDef> const sets =
        {
            // --- The paladin ---
            { CLASS_PALADIN, 20, ITEM_SUBCLASS_ARMOR_MAIL, "Squire's Resolve",
              { { CHEST, "Squire's Hauberk" }, { LEGS, "Squire's Legguards" }, { HANDS, "Squire's Gauntlets" }, { FEET, "Squire's Sabatons" } },
              { { 2, B_STAT, STAT_STRENGTH, 8, "+8 Strength." },
                { 3, B_TAG, TAG_HOLY, 10, "+10% Holy damage." } },
              { 0, KIT_CHAIN, 20271, 1, 10.0f, 50, "Judgement chains to 1 more enemy within 10 yards for 50% damage.", "Squire's Resolve" } },
            { CLASS_PALADIN, 30, ITEM_SUBCLASS_ARMOR_MAIL, "Crusader's Vow",
              { { CHEST, "Vowkeeper's Chain" }, { LEGS, "Vowkeeper's Leggings" }, { HANDS, "Vowkeeper's Grips" }, { FEET, "Vowkeeper's Greaves" } },
              { { 2, B_STAT, STAT_STAMINA, 12, "+12 Stamina." },
                { 3, B_ATTACK_SPEED, 0, 6, "+6% attack speed." } },
              { 0, KIT_ARC, 20154, 0, 0.0f, 30, "Seal of Righteousness strikes every enemy in front of you. The extra enemies take 30% damage.", "Crusader's Vow" } },
            { CLASS_PALADIN, 40, ITEM_SUBCLASS_ARMOR_PLATE, "Lightbringer's Bulwark",
              { { HEAD, "Lightbringer's Crown" }, { SHOULDERS, "Lightbringer's Pauldrons" }, { CHEST, "Lightbringer's Breastplate" },
                { LEGS, "Lightbringer's Legplates" }, { HANDS, "Lightbringer's Gauntlets" } },
              { { 2, B_ARMOUR, 0, 10, "+10% armour." },
                { 3, B_TAG, TAG_HOLY, 12, "+12% Holy damage." },
                { 4, B_SKILLS, 2, 1, "+1 to all Holy skills." } },
              { 0, KIT_BURST, 20271, 0, 6.0f, 40, "Judgement bursts onto every enemy within 6 yards of its target for 40% damage.", "Lightbringer's Bulwark" } },
            { CLASS_PALADIN, 50, ITEM_SUBCLASS_ARMOR_PLATE, "Ashen Oath",
              { { HEAD, "Oathsworn Helm" }, { SHOULDERS, "Oathsworn Shoulderplates" }, { CHEST, "Oathsworn Chestguard" },
                { LEGS, "Oathsworn Legplates" }, { HANDS, "Oathsworn Handguards" } },
              { { 2, B_STAT, STAT_STRENGTH, 15, "+15 Strength." },
                { 3, B_CRIT_DAMAGE, 0, 15, "+15% critical strike damage." },
                { 4, B_SKILLS, 0, 1, "+1 to all Melee skills." } },
              { 0, KIT_CHAIN, 879, 2, 10.0f, 60, "Exorcism chains to 2 more enemies within 10 yards for 60% damage.", "Ashen Oath" } },
            // --- The mage ---
            { CLASS_MAGE, 20, ITEM_SUBCLASS_ARMOR_CLOTH, "Apprentice's Embers",
              { { CHEST, "Ember-Touched Robe" }, { LEGS, "Ember-Touched Leggings" }, { HANDS, "Ember-Touched Gloves" }, { FEET, "Ember-Touched Slippers" } },
              { { 2, B_STAT, STAT_INTELLECT, 8, "+8 Intellect." },
                { 3, B_TAG, TAG_FIRE, 10, "+10% Fire damage." } },
              { 0, KIT_EXTRA_PROJECTILES, 133, 1, 14.0f, 40, "Fireball launches 1 extra fireball. It deals 40% damage.", "Apprentice's Embers" } },
            { CLASS_MAGE, 30, ITEM_SUBCLASS_ARMOR_CLOTH, "Frostweave Mantle",
              { { CHEST, "Frostweave Vestment" }, { LEGS, "Frostweave Pants" }, { HANDS, "Frostweave Handwraps" }, { FEET, "Frostweave Sandals" } },
              { { 2, B_TAG, TAG_FROST, 10, "+10% Frost damage." },
                { 3, B_CAST_SPEED, 0, 5, "+5% cast speed." } },
              { 0, KIT_FRAGMENTS, 116, 2, 8.0f, 35, "Frostbolt shatters on hit into 2 ice shards that strike the nearest enemies within 8 yards for 35% damage each.", "Frostweave Mantle" } },
            { CLASS_MAGE, 40, ITEM_SUBCLASS_ARMOR_CLOTH, "Arcanist's Lattice",
              { { HEAD, "Latticed Circlet" }, { SHOULDERS, "Latticed Mantle" }, { CHEST, "Latticed Robe" },
                { LEGS, "Latticed Trousers" }, { HANDS, "Latticed Gloves" } },
              { { 2, B_STAT, STAT_INTELLECT, 15, "+15 Intellect." },
                { 3, B_TAG, TAG_ARCANE, 12, "+12% Arcane damage." },
                { 4, B_MANA, 0, 10, "+10% maximum mana." } },
              { 0, KIT_CHAIN, 5143, 1, 8.0f, 40, "Each Arcane Missiles missile jumps to 1 more enemy within 8 yards for 40% damage.", "Arcanist's Lattice" } },
            { CLASS_MAGE, 50, ITEM_SUBCLASS_ARMOR_CLOTH, "Archmage's Conflux",
              { { HEAD, "Conflux Cowl" }, { SHOULDERS, "Conflux Amice" }, { CHEST, "Conflux Robes" },
                { LEGS, "Conflux Leggings" }, { HANDS, "Conflux Gloves" } },
              { { 2, B_STAT, STAT_INTELLECT, 20, "+20 Intellect." },
                { 3, B_CRIT_DAMAGE, 0, 15, "+15% critical strike damage." },
                { 4, B_SKILLS, 1, 1, "+1 to all Spell skills." } },
              { 0, KIT_ECHO, 133, 3, 0.0f, 60, "Every 3rd Fireball is echoed for free at 60% damage.", "Archmage's Conflux" } },
            // --- The druid ---
            { CLASS_DRUID, 20, ITEM_SUBCLASS_ARMOR_LEATHER, "Grovewalker's Hide",
              { { CHEST, "Grovewalker's Tunic" }, { LEGS, "Grovewalker's Breeches" }, { HANDS, "Grovewalker's Gloves" }, { FEET, "Grovewalker's Boots" } },
              { { 2, B_STAT, STAT_AGILITY, 8, "+8 Agility." },
                { 3, B_TAG, TAG_NATURE, 10, "+10% Nature damage." } },
              { 0, KIT_CHAIN, 5176, 1, 10.0f, 50, "Wrath chains to 1 more enemy within 10 yards for 50% damage.", "Grovewalker's Hide" } },
            { CLASS_DRUID, 30, ITEM_SUBCLASS_ARMOR_LEATHER, "Moonshadow Garb",
              { { CHEST, "Moonshadow Vest" }, { LEGS, "Moonshadow Leggings" }, { HANDS, "Moonshadow Grips" }, { FEET, "Moonshadow Treads" } },
              { { 2, B_STAT, STAT_INTELLECT, 10, "+10 Intellect." },
                { 3, B_TAG, TAG_ARCANE, 10, "+10% Arcane damage." } },
              { 0, KIT_SPREAD, 8921, 1, 8.0f, 100, "Moonfire also lands on 1 more enemy within 8 yards.", "Moonshadow Garb" } },
            { CLASS_DRUID, 40, ITEM_SUBCLASS_ARMOR_LEATHER, "Feralheart Raiment",
              { { HEAD, "Feralheart Headguard" }, { SHOULDERS, "Feralheart Spaulders" }, { CHEST, "Feralheart Vest" },
                { LEGS, "Feralheart Kilt" }, { HANDS, "Feralheart Bracers" } },
              { { 2, B_STAT, STAT_AGILITY, 15, "+15 Agility." },
                { 3, B_TAG, TAG_MELEE, 12, "+12% Melee damage." },
                { 4, B_ATTACK_SPEED, 0, 6, "+6% attack speed." } },
              { 0, KIT_ARC, 1082, 0, 0.0f, 40, "Claw strikes every enemy in front of you. The extra enemies take 40% damage.", "Feralheart Raiment" } },
            { CLASS_DRUID, 50, ITEM_SUBCLASS_ARMOR_LEATHER, "Wildheart Vestments",
              { { HEAD, "Wildheart Cowl" }, { SHOULDERS, "Wildheart Spaulders" }, { CHEST, "Wildheart Vest" },
                { LEGS, "Wildheart Kilt" }, { HANDS, "Wildheart Gloves" } },
              { { 2, B_STAT, STAT_INTELLECT, 15, "+15 Intellect." },
                { 3, B_TAG, TAG_NATURE, 12, "+12% Nature damage." },
                { 4, B_SKILLS, 1, 1, "+1 to all Spell skills." } },
              { 0, KIT_BURST, 2912, 0, 8.0f, 40, "Starfire bursts onto every enemy within 8 yards of its target for 40% damage.", "Wildheart Vestments" } },
            // --- The warlock: shadow and fire, and the horde's stats ---
            { CLASS_WARLOCK, 20, ITEM_SUBCLASS_ARMOR_CLOTH, "Shadowsworn Rags",
              { { CHEST, "Shadowsworn Robe" }, { LEGS, "Shadowsworn Leggings" }, { HANDS, "Shadowsworn Gloves" }, { FEET, "Shadowsworn Slippers" } },
              { { 2, B_STAT, STAT_INTELLECT, 8, "+8 Intellect." },
                { 3, B_TAG, TAG_SHADOW, 10, "+10% Shadow damage." } },
              { 0, KIT_PIERCE, 686, 1, 0.0f, 60, "Shadow Bolt flies through 1 enemy, which takes 60% damage.", "Shadowsworn Rags" } },
            { CLASS_WARLOCK, 30, ITEM_SUBCLASS_ARMOR_CLOTH, "Summoner's Bindings",
              { { CHEST, "Binder's Robe" }, { LEGS, "Binder's Leggings" }, { HANDS, "Binder's Handwraps" }, { FEET, "Binder's Sandals" } },
              { { 2, B_TAG, TAG_MINION, 10, "+10% Minion damage." },
                { 3, B_MINION_LIFE, 0, 15, "+15% Minion life." } },
              { 0, KIT_SPREAD, 172, 1, 8.0f, 100, "Corruption also lands on 1 more enemy within 8 yards.", "Summoner's Bindings" } },
            { CLASS_WARLOCK, 40, ITEM_SUBCLASS_ARMOR_CLOTH, "Felfire Regalia",
              { { HEAD, "Felfire Circlet" }, { SHOULDERS, "Felfire Mantle" }, { CHEST, "Felfire Robe" },
                { LEGS, "Felfire Leggings" }, { HANDS, "Felfire Gloves" } },
              { { 2, B_STAT, STAT_INTELLECT, 15, "+15 Intellect." },
                { 3, B_TAG, TAG_FIRE, 12, "+12% Fire damage." },
                { 4, B_HORDE, 0, 1, "+1 to the horde's size." } },
              { 0, KIT_BURST, 348, 0, 6.0f, 40, "Immolate bursts onto every enemy within 6 yards of its target for 40% damage.", "Felfire Regalia" } },
            { CLASS_WARLOCK, 50, ITEM_SUBCLASS_ARMOR_CLOTH, "Dreadlord's Mantle",
              { { HEAD, "Dreadweave Cowl" }, { SHOULDERS, "Dreadweave Amice" }, { CHEST, "Dreadweave Robe" },
                { LEGS, "Dreadweave Leggings" }, { HANDS, "Dreadweave Gloves" } },
              { { 2, B_TAG, TAG_MINION, 20, "+20% Minion damage." },
                { 3, B_TAG, TAG_SHADOW, 12, "+12% Shadow damage." },
                { 4, B_SKILLS, 1, 1, "+1 to all Spell skills." } },
              { 0, KIT_CHAIN, 686, 2, 10.0f, 50, "Shadow Bolt chains to 2 more enemies within 10 yards for 50% damage.", "Dreadlord's Mantle" } },
        };
        return sets;
    }

    // What was written: per set, its pieces' entries (0 for one that could not be). Written once
    // at start, before the maps update, and only read after: no lock.
    std::vector<std::vector<uint32>> sWritten;

    uint32 EntryOf(size_t set, size_t piece) { return SET_ENTRY_FROM + uint32(set) * 10 + uint32(piece); }

    // The inventory types of `slot`, for SQL.
    std::string InvList(SlotDef const& slot)
    {
        return slot.inv == slot.alt ? std::to_string(slot.inv) : std::to_string(slot.inv) + ", " + std::to_string(slot.alt);
    }

    // The look family for `set`: the first word of the vanilla items of its armour at its level that
    // cover the most of its slots (ties: nearest its level), or "" for none covering half of them.
    std::string LookFamily(SetDef const& set)
    {
        std::string slots;
        for (PieceDef const& piece : set.pieces)
            slots += (slots.empty() ? "" : ", ") + InvList(piece.slot);
        uint32 const lo = set.level >= 3 ? set.level - 3 : 0, hi = set.level + 6;
        auto result = WorldDatabase.PQuery(
                "SELECT SUBSTRING_INDEX(name, ' ', 1) AS word, COUNT(DISTINCT IF(InventoryType = %u, %u, InventoryType)) AS n "
                "FROM item_template WHERE class = %u AND subclass = %u AND InventoryType IN (%s) AND Quality IN (2, 3) "
                "AND RequiredLevel BETWEEN %u AND %u AND entry < %u AND name NOT LIKE '%%Deprecated%%' AND name NOT LIKE '%%Test%%' "
                "AND name LIKE '%% %%' GROUP BY word HAVING n * 2 >= %u ORDER BY n DESC, MIN(ABS(CAST(RequiredLevel AS SIGNED) - %u)) LIMIT 1",
                uint32(INVTYPE_ROBE), uint32(INVTYPE_CHEST), uint32(ITEM_CLASS_ARMOR), uint32(set.armour), slots.c_str(), lo, hi,
                OLD_BANK_FROM, uint32(set.pieces.size()), set.level);
        return result ? result->Fetch()[0].GetCppString() : std::string();
    }

    // The vanilla item a piece wears the look of: one of `family` in its slot, else the nearest of
    // its armour and slot to its level (a blue before a green).
    uint32 LookDonor(SetDef const& set, PieceDef const& piece, std::string const& family)
    {
        uint32 const lo = set.level >= 3 ? set.level - 3 : 0, hi = set.level + 6;
        if (!family.empty())
            if (auto result = WorldDatabase.PQuery(
                        "SELECT entry FROM item_template WHERE class = %u AND subclass = %u AND InventoryType IN (%s) AND Quality IN (2, 3) "
                        "AND RequiredLevel BETWEEN %u AND %u AND entry < %u AND name LIKE '%s %%' "
                        "ORDER BY Quality DESC, ABS(CAST(RequiredLevel AS SIGNED) - %u), entry LIMIT 1",
                        uint32(ITEM_CLASS_ARMOR), uint32(set.armour), InvList(piece.slot).c_str(), lo, hi, OLD_BANK_FROM,
                        SqlEscaped(family).c_str(), set.level))
                return result->Fetch()[0].GetUInt32();
        if (auto result = WorldDatabase.PQuery(
                    "SELECT entry FROM item_template WHERE class = %u AND subclass = %u AND InventoryType IN (%s) AND Quality IN (2, 3) "
                    "AND RequiredLevel BETWEEN %u AND %u AND entry < %u AND name NOT LIKE '%%Deprecated%%' "
                    "ORDER BY Quality DESC, ABS(CAST(RequiredLevel AS SIGNED) - %u), entry LIMIT 1",
                    uint32(ITEM_CLASS_ARMOR), uint32(set.armour), InvList(piece.slot).c_str(), lo, hi + 6, OLD_BANK_FROM, set.level))
            return result->Fetch()[0].GetUInt32();
        return 0;
    }

    ClassStats const* StatsOf(uint8 classId)
    {
        for (ClassStats const& c : CLASS_STATS)
            if (c.classId == classId)
                return &c;
        return nullptr;
    }

    // How many of each set `player` wears, by set index. Run on every kit check (a spell's hit):
    // a set's index is its entry's tens.
    std::map<size_t, uint32> WornCounts(Player const* player)
    {
        std::map<size_t, uint32> worn;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (Item const* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            {
                uint32 const entry = item->GetEntry();
                if (!IsSetPiece(entry) || item->IsBroken())
                    continue;
                size_t const s = (entry - SET_ENTRY_FROM) / 10, p = (entry - SET_ENTRY_FROM) % 10;
                if (s < sWritten.size() && p < sWritten[s].size() && sWritten[s][p] == entry)
                    ++worn[s];
            }
        return worn;
    }

    // A piece's chance to drop from `source`, in hundredths of a percent.
    uint32 DropChance(Creature const* source)
    {
        if (IsDungeonBoss(source) || source->IsWorldBoss())
            return 2500;
        uint8 const tier = ChampionTier(source);
        if (tier >= 2)
            return 1200;
        if (tier == 1 || source->IsElite())
            return 400;
        return 60;
    }
}

namespace Arpg
{
    void EnsureSetItems()
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
            sLog.outError("ARPG sets: item_template's columns could not be read; no sets written");
            return;
        }
        auto has = [&columns](char const* column) { return std::find(columns.begin(), columns.end(), column) != columns.end(); };

        std::vector<std::vector<uint32>> written;
        uint32 count = 0;
        for (size_t s = 0; s < Sets().size(); ++s)
        {
            SetDef const& set = Sets()[s];
            ClassStats const* stats = StatsOf(set.classId);
            std::string const family = LookFamily(set);
            std::vector<uint32> entries;
            for (size_t p = 0; p < set.pieces.size(); ++p)
            {
                PieceDef const& piece = set.pieces[p];
                uint32 const entry = EntryOf(s, p);
                uint32 const donor = LookDonor(set, piece, family);
                if (!donor || !stats)
                {
                    sLog.outError("ARPG sets: no look for %s (%s); the piece is left out", piece.name, set.name);
                    entries.push_back(0);
                    continue;
                }
                std::map<std::string, std::string> row;
                auto put = [&](char const* column, std::string const& value) { if (has(column)) row[column] = value; };
                auto num = [&](char const* column, double value) { put(column, std::to_string(int64(std::llround(value)))); };
                put("entry", std::to_string(entry));
                put("name", "'" + SqlEscaped(piece.name) + "'");
                num("Quality", ITEM_QUALITY_UNCOMMON);
                for (char const* zero : { "Flags", "BuyPrice", "RequiredSkill", "RequiredSkillRank", "requiredspell",
                                          "requiredhonorrank", "RequiredCityRank", "RequiredReputationFaction",
                                          "RequiredReputationRank", "maxcount", "holy_res", "fire_res", "nature_res",
                                          "frost_res", "shadow_res", "arcane_res", "bonding", "PageText", "startquest",
                                          "lockid", "RandomProperty", "area", "Map", "DisenchantID", "Duration", "ExtraFlags" })
                    num(zero, 0);
                num("itemset", SET_ID_FROM + s);
                num("AllowableClass", 1 << (set.classId - 1));
                num("AllowableRace", -1);
                num("ItemLevel", set.level + 5);
                num("RequiredLevel", set.level);
                num("SellPrice", double(set.level) * set.level * 8);
                put("description", "''");
                put("ScriptName", "''");
                for (int i = 1; i <= 10; ++i)
                {
                    num(("stat_type" + std::to_string(i)).c_str(), 0);
                    num(("stat_value" + std::to_string(i)).c_str(), 0);
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
                // Its attributes: the class's shares of its budget. The look's armour stays.
                double const budget = double(set.level) * BUDGET_PER_LEVEL * piece.slot.weight;
                int slot = 1;
                for (int a = 0; a < 5; ++a)
                {
                    double const points = budget * stats->share[a];
                    if (points < 1.0)
                        continue;
                    std::string const n = std::to_string(slot++);
                    num(("stat_type" + n).c_str(), STAT_MOD[a]);
                    num(("stat_value" + n).c_str(), std::max(1.0, points));
                }
                CopyItemRow(columns, entry, donor, row);
                entries.push_back(entry);
                ++count;
            }
            sLog.outString("ARPG sets: %s (level %u) wears the %s look", set.name, set.level, family.empty() ? "nearest items'" : family.c_str());
            written.push_back(entries);
        }
        sWritten = written;
        sLog.outString("ARPG sets: wrote %u set pieces into item_template", count);
    }

    bool IsSetPiece(uint32 entry)
    {
        return entry >= SET_ENTRY_FROM && entry <= SET_ENTRY_TO;
    }

    void AddSetTotals(Player const* player, WebTotals& totals)
    {
        for (auto const& [s, worn] : WornCounts(player))
            for (Bonus const& b : Sets()[s].bonuses)
            {
                if (worn < b.pieces)
                    continue;
                switch (b.kind)
                {
                    case B_STAT: totals.stat[b.param] += b.value; break;
                    case B_TAG: totals.tag[b.param] += b.value; break;
                    case B_ARMOUR: totals.armourPct += b.value; break;
                    case B_ATTACK_SPEED: totals.attackSpeedPct += b.value; break;
                    case B_CAST_SPEED: totals.castSpeedPct += b.value; break;
                    case B_CRIT_DAMAGE: totals.critDamagePct += b.value; break;
                    case B_MANA: totals.manaPct += b.value; break;
                    case B_SKILLS: totals.skillLevel[b.param] += b.value; break;
                    case B_MINION_LIFE: totals.minionLifePct += b.value; break;
                    case B_HORDE: totals.hordeSize += b.value; break;
                }
            }
    }

    std::vector<UniqueMechanic const*> SetModifiers(Player const* player)
    {
        std::vector<UniqueMechanic const*> out;
        std::map<size_t, uint32> const worn = WornCounts(player);
        for (auto const& [s, count] : worn)
        {
            size_t pieces = 0;
            for (uint32 entry : sWritten[s])
                if (entry)
                    ++pieces;
            if (pieces && count >= pieces)
                out.push_back(&Sets()[s].power);
        }
        return out;
    }

    void MaybeAddSetPiece(Loot* loot, Player const* looter, Creature const* source)
    {
        if (!loot || !looter || !source)
            return;
        uint32 const level = source->GetLevel();
        std::vector<uint32> choices;
        for (size_t s = 0; s < Sets().size() && s < sWritten.size(); ++s)
        {
            SetDef const& set = Sets()[s];
            uint32 const top = set.level >= 50 ? 63 : set.level + 9;
            if (set.classId != looter->getClass() || level < set.level || level > top)
                continue;
            for (uint32 entry : sWritten[s])
                if (entry)
                    choices.push_back(entry);
        }
        if (choices.empty() || urand(1, 10000) > DropChance(source))
            return;
        uint32 const piece = choices[urand(0, uint32(choices.size() - 1))];
        LootItem* before = nullptr;
        loot->ForEachArpgItem([&](LootItem* item) { before = item; });
        loot->AddItem(piece, 1, 0, 0);
        // The looter's alone: a piece of its class's set is no one else's to roll for.
        LootItem* added = nullptr;
        loot->ForEachArpgItem([&](LootItem* item) { added = item; });
        if (added && added != before && added->itemId == piece)
            added->allowedGuid = { looter->GetObjectGuid() };
    }

    void SendSets(Player* player)
    {
        if (!player || !player->GetSession())
            return;
        WorldPacket data(SMSG_ARPG_SETS, 512);
        data << uint8(sWritten.size());
        for (size_t s = 0; s < sWritten.size(); ++s)
        {
            SetDef const& set = Sets()[s];
            data << uint32(SET_ID_FROM + s);
            data << set.name;
            data << uint8(set.classId) << uint8(set.level);
            uint8 pieces = 0;
            for (uint32 entry : sWritten[s])
                if (entry)
                    ++pieces;
            data << uint8(pieces);
            for (uint32 entry : sWritten[s])
                if (entry)
                    data << uint32(entry);
            data << uint8(set.bonuses.size() + 1);
            for (Bonus const& b : set.bonuses)
                data << uint8(b.pieces) << b.text;
            data << uint8(pieces) << set.power.text;
        }
        player->GetSession()->SendPacket(data);
    }
}
