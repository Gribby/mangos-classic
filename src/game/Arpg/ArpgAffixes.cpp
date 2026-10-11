/*
 * ARPG item affixes: see ArpgAffixes.h.
 */

#include "Arpg/ArpgAffixes.h"
#include "Arpg/ArpgCombat.h"
#include "Arpg/ArpgDungeons.h"
#include "Arpg/ArpgLoot.h"
#include "Arpg/ArpgPacks.h"
#include "Arpg/ArpgTags.h"
#include "Arpg/ArpgUniques.h"

#include "Database/DatabaseEnv.h"
#include "Entities/Creature.h"
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

    enum AffixKind : uint8
    {
        AK_STAT, AK_ALL_STATS, AK_TAG, AK_HEALING, AK_ARMOUR, AK_BLOCK, AK_MOVE, AK_COOLDOWN,
        AK_LIFE_ON_KILL, AK_MANA_ON_HIT, AK_ATTACK_SPEED, AK_CAST_SPEED, AK_MELEE_CRIT, AK_SPELL_CRIT,
        AK_SPELL_CRIT_DAMAGE, AK_COST, AK_HEALTH_PCT, AK_MANA_PCT, AK_ATTACK_POWER, AK_SPELL_POWER,
        AK_RESIST, AK_ALL_RESIST, AK_LIFE_LEECH, AK_MANA_LEECH, AK_HEALTH_REGEN, AK_MANA_REGEN,
        AK_DAMAGE_TAKEN, AK_THORNS, AK_VS_ELITES, AK_SLAYING, AK_GLOBE_HEAL, AK_GOLD_FIND, AK_XP,
        AK_MELEE_AREA, AK_SPELL_AREA,
    };

    // Where an affix may roll.
    enum AffixWhere : uint8 { W_ANY, W_WEAPON, W_ARMOUR, W_FEET, W_SHIELD, W_JEWELLERY };

    struct AffixDef
    {
        AffixKind kind;
        uint8 param;              // the stat, tag, resistance (0 fire .. 4 arcane) or creature type
        int32 low, high;          // the range at item level 60, Normal
        char const* format;       // the tooltip line, %d the value (%s for a tenths value)
        AffixWhere where;
        int32 cap;                // the most one affix may give (0: none)
        bool tenths = false;      // the value is in tenths of a percent (leech)
    };

    AffixDef const AFFIXES[] =
    {
        // Attributes.
        { AK_STAT, STAT_STRENGTH,  8, 30, "+%d Strength", W_ANY, 0 },
        { AK_STAT, STAT_AGILITY,   8, 30, "+%d Agility", W_ANY, 0 },
        { AK_STAT, STAT_STAMINA,   8, 30, "+%d Stamina", W_ANY, 0 },
        { AK_STAT, STAT_INTELLECT, 8, 30, "+%d Intellect", W_ANY, 0 },
        { AK_STAT, STAT_SPIRIT,    8, 30, "+%d Spirit", W_ANY, 0 },
        { AK_ALL_STATS, 0,         4, 14, "+%d to all attributes", W_JEWELLERY, 0 },
        // Damage by tag.
        { AK_TAG, TAG_HOLY,        6, 20, "+%d%% Holy damage", W_ANY, 0 },
        { AK_TAG, TAG_PHYSICAL,    6, 20, "+%d%% Physical damage", W_ANY, 0 },
        { AK_TAG, TAG_FIRE,        6, 20, "+%d%% Fire damage", W_ANY, 0 },
        { AK_TAG, TAG_FROST,       6, 20, "+%d%% Frost damage", W_ANY, 0 },
        { AK_TAG, TAG_ARCANE,      6, 20, "+%d%% Arcane damage", W_ANY, 0 },
        { AK_TAG, TAG_NATURE,      6, 20, "+%d%% Nature damage", W_ANY, 0 },
        { AK_TAG, TAG_SHADOW,      6, 20, "+%d%% Shadow damage", W_ANY, 0 },
        { AK_TAG, TAG_AREA,        8, 24, "+%d%% Area damage", W_ANY, 0 },
        { AK_TAG, TAG_PROJECTILE,  6, 18, "+%d%% Projectile damage", W_ANY, 0 },
        { AK_TAG, TAG_MELEE,       5, 16, "+%d%% Melee damage", W_ANY, 0 },
        { AK_TAG, TAG_SPELL,       5, 16, "+%d%% Spell damage", W_ANY, 0 },
        { AK_TAG, TAG_DURATION,    6, 20, "+%d%% damage over time", W_ANY, 0 },
        { AK_TAG, TAG_SEAL,        6, 20, "+%d%% Seal damage", W_WEAPON, 0 },
        { AK_TAG, TAG_CHANNEL,     6, 20, "+%d%% channelled spell damage", W_ANY, 0 },
        { AK_VS_ELITES, 0,         5, 15, "+%d%% damage to elites and bosses", W_ANY, 0 },
        { AK_SLAYING, CREATURE_TYPE_UNDEAD,    8, 25, "+%d%% damage to Undead", W_WEAPON, 0 },
        { AK_SLAYING, CREATURE_TYPE_DEMON,     8, 25, "+%d%% damage to Demons", W_WEAPON, 0 },
        { AK_SLAYING, CREATURE_TYPE_BEAST,     8, 25, "+%d%% damage to Beasts", W_WEAPON, 0 },
        { AK_SLAYING, CREATURE_TYPE_HUMANOID,  8, 25, "+%d%% damage to Humanoids", W_WEAPON, 0 },
        { AK_SLAYING, CREATURE_TYPE_DRAGONKIN, 8, 25, "+%d%% damage to Dragonkin", W_WEAPON, 0 },
        { AK_SLAYING, CREATURE_TYPE_ELEMENTAL, 8, 25, "+%d%% damage to Elementals", W_WEAPON, 0 },
        // Offence.
        { AK_ATTACK_POWER, 0,     10, 40, "+%d attack power", W_ANY, 0 },
        { AK_SPELL_POWER, 0,       6, 24, "+%d spell damage", W_ANY, 0 },
        { AK_ATTACK_SPEED, 0,      3, 8,  "+%d%% attack speed", W_WEAPON, 20 },
        { AK_CAST_SPEED, 0,        3, 8,  "+%d%% cast speed", W_ANY, 20 },
        { AK_MELEE_CRIT, 0,        1, 3,  "+%d%% critical strike chance", W_ANY, 8 },
        { AK_SPELL_CRIT, 0,        1, 3,  "+%d%% spell critical strike chance", W_ANY, 8 },
        { AK_SPELL_CRIT_DAMAGE, 0, 8, 25, "+%d%% spell critical strike damage", W_JEWELLERY, 75 },
        { AK_MELEE_AREA, 0,        3, 10, "+%d%% melee area", W_WEAPON, 20 },
        { AK_SPELL_AREA, 0,        3, 10, "+%d%% spell area", W_ANY, 20 },
        { AK_COOLDOWN, 0,          3, 10, "+%d%% cooldown recovery", W_ANY, 20 },
        { AK_COST, 0,              3, 8,  "%d%% less mana cost", W_ANY, 20 },
        // Sustain.
        { AK_LIFE_LEECH, 0,        5, 20, "%s%% of damage dealt leeched as life", W_WEAPON, 50, true },
        { AK_MANA_LEECH, 0,        3, 12, "%s%% of damage dealt leeched as mana", W_JEWELLERY, 30, true },
        { AK_LIFE_ON_KILL, 0,      5, 40, "+%d life on kill", W_ANY, 0 },
        { AK_MANA_ON_HIT, 0,       2, 10, "+%d mana on hit", W_ANY, 0 },
        { AK_HEALTH_REGEN, 0,      2, 10, "+%d health a second", W_ARMOUR, 0 },
        { AK_MANA_REGEN, 0,        1, 5,  "+%d mana a second", W_JEWELLERY, 0 },
        { AK_HEALING, 0,           5, 18, "+%d%% healing", W_ANY, 0 },
        { AK_GLOBE_HEAL, 0,        5, 20, "+%d%% healing from globes and flasks", W_ARMOUR, 60 },
        // Defence.
        { AK_HEALTH_PCT, 0,        3, 8,  "+%d%% maximum health", W_ARMOUR, 25 },
        { AK_MANA_PCT, 0,          3, 8,  "+%d%% maximum mana", W_ANY, 25 },
        { AK_ARMOUR, 0,            5, 25, "+%d%% armour", W_ARMOUR, 60 },
        { AK_BLOCK, 0,             2, 6,  "+%d%% block chance", W_SHIELD, 15 },
        { AK_DAMAGE_TAKEN, 0,      2, 6,  "%d%% less damage taken", W_ARMOUR, 12 },
        { AK_THORNS, 0,            6, 30, "Attackers within reach take %d damage when they hit you", W_ARMOUR, 0 },
        { AK_RESIST, 0,            5, 20, "+%d Fire resistance", W_ANY, 0 },
        { AK_RESIST, 1,            5, 20, "+%d Nature resistance", W_ANY, 0 },
        { AK_RESIST, 2,            5, 20, "+%d Frost resistance", W_ANY, 0 },
        { AK_RESIST, 3,            5, 20, "+%d Shadow resistance", W_ANY, 0 },
        { AK_RESIST, 4,            5, 20, "+%d Arcane resistance", W_ANY, 0 },
        { AK_ALL_RESIST, 0,        3, 10, "+%d to all resistances", W_JEWELLERY, 0 },
        // Utility.
        { AK_MOVE, 0,              3, 8,  "+%d%% movement speed", W_FEET, 15 },
        { AK_GOLD_FIND, 0,         5, 25, "+%d%% gold found", W_ANY, 100 },
        { AK_XP, 0,                2, 6,  "+%d%% experience gained", W_JEWELLERY, 15 },
    };

    // A value's share of the level 60 range, by item level: it grows by half again every nine
    // levels or so, so a level 20 drop rolls about a seventh of a level 60 one, Molten Core a
    // third more, Blackwing Lair twice, and Naxxramas three to four times.
    constexpr float SCALE_PER_LEVEL = 0.04777f;     // ln 3 / 23: three times at item level 83
    constexpr float SCALE_MIN = 0.06f, SCALE_MAX = 4.0f, SCALE_LEVEL = 60.0f;
    // Each dungeon tier the item dropped at raises it again: Normal to Torment III.
    float const TIER_SCALE[MAX_TIER + 1] = { 1.0f, 1.15f, 1.3f, 1.5f, 1.75f, 2.0f };
    // A raid's drops (from this item level) and anything from Torment I up roll the top of their
    // rarity's affix range.
    constexpr uint32 RAID_ITEM_LEVEL = 66;
    constexpr uint8 TORMENT_TIER = 3;
    // The tier's word over the affixes, from Hard up.
    char const* const TIER_COLOUR[MAX_TIER + 1] = { "", "|cff9d9d9d", "|cffe6cc80", "|cffff8000", "|cffff5020", "|cffff2020" };
    // The rarities' words and colours, by Rarity (1 white .. 4 purple, 6 unique).
    char const* const RARITY_NAME[7] = { "", "Common", "Magic", "Rare", "Epic", "", "Unique" };
    char const* const RARITY_COLOUR[7] = { "", "|cffffffff", "|cff1eff00", "|cff0070dd", "|cffa335ee", "", "|cffe6cc80" };

    // The rarity odds of a dropped base item (white, green, blue, purple), by who dropped it.
    struct Odds { float white, green, blue, purple; };
    Odds const ODDS_TRASH = { 50.0f, 33.0f, 13.0f, 4.0f };
    Odds const ODDS_CHAMPION = { 15.0f, 40.0f, 33.0f, 12.0f };
    Odds const ODDS_RARE = { 0.0f, 25.0f, 45.0f, 30.0f };     // rares, Wardens and bosses

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

    // Weapons and armour that equip, from white up (grey is junk; shirts and tabards are dress).
    bool Takes(ItemPrototype const* proto)
    {
        if (!proto || proto->Quality < ITEM_QUALITY_NORMAL || proto->Quality > ITEM_QUALITY_EPIC)
            return false;
        if (proto->Class != ITEM_CLASS_WEAPON && proto->Class != ITEM_CLASS_ARMOR)
            return false;
        switch (proto->InventoryType)
        {
            case INVTYPE_NON_EQUIP: case INVTYPE_AMMO: case INVTYPE_BODY: case INVTYPE_TABARD:
            case INVTYPE_BAG: case INVTYPE_QUIVER:
                return false;
            default:
                return true;
        }
    }

    // A named blue or purple, or a mechanic's item: a unique, its own stats its implicits.
    bool UniqueBase(ItemPrototype const* proto)
    {
        if (proto->Quality >= ITEM_QUALITY_RARE)
            return true;
        for (UniqueMechanic const& unique : Uniques())
            if (unique.item == proto->ItemId)
                return true;
        return false;
    }

    // The rarity an item shows: the one stored, else (a row from before rarities) a unique for a
    // unique base and the item's own colour for the rest.
    uint8 RarityOf(ItemPrototype const* proto, uint8 stored)
    {
        if (UniqueBase(proto))
            return RARITY_UNIQUE;
        if (stored >= RARITY_WHITE && stored <= RARITY_PURPLE)
            return stored;
        return uint8(std::max<uint32>(RARITY_WHITE, std::min<uint32>(proto->Quality, RARITY_PURPLE)));
    }

    // A unique's affix count follows its own colour: a blue rolls a rare's 3-4, a purple an
    // epic's 5-6; its border shows which.
    uint8 BracketOf(ItemPrototype const* proto, uint8 rarity)
    {
        if (rarity != RARITY_UNIQUE)
            return rarity;
        return proto->Quality >= ITEM_QUALITY_EPIC ? RARITY_PURPLE : RARITY_BLUE;
    }

    // How many affixes: white none, green 1-2, blue 3-4, purple 5-6; a raid drop or a Torment one
    // the top of its range.
    uint32 AffixCount(ItemPrototype const* proto, uint8 tier, uint8 bracket, Rng& rng)
    {
        uint32 low;
        switch (bracket)
        {
            case RARITY_GREEN:  low = 1; break;
            case RARITY_BLUE:   low = 3; break;
            case RARITY_PURPLE: low = 5; break;
            default: return 0;
        }
        if (proto->ItemLevel >= RAID_ITEM_LEVEL || tier >= TORMENT_TIER)
            return low + 1;
        return low + rng.Next() % 2;
    }

    bool Fits(AffixDef const& def, ItemPrototype const* proto)
    {
        uint32 const type = proto->InventoryType;
        bool const jewel = type == INVTYPE_NECK || type == INVTYPE_FINGER || type == INVTYPE_TRINKET;
        switch (def.where)
        {
            case W_ANY:       return true;
            case W_WEAPON:    return proto->Class == ITEM_CLASS_WEAPON;
            case W_ARMOUR:    return proto->Class == ITEM_CLASS_ARMOR && !jewel;
            case W_FEET:      return type == INVTYPE_FEET;
            case W_SHIELD:    return type == INVTYPE_SHIELD;
            case W_JEWELLERY: return jewel || type == INVTYPE_CLOAK || type == INVTYPE_HOLDABLE;
        }
        return false;
    }

    std::vector<Rolled> Roll(ItemPrototype const* proto, uint32 seed, uint8 tier, uint8 storedRarity)
    {
        std::vector<Rolled> out;
        if (!Takes(proto) || !seed)
            return out;
        tier = std::min<uint8>(tier, MAX_TIER);
        Rng rng{ seed ^ (proto->ItemId * 2654435761u) };
        if (!rng.state)
            rng.state = 0x9E3779B9u;
        uint8 const bracket = BracketOf(proto, RarityOf(proto, storedRarity));
        uint32 const count = AffixCount(proto, tier, bracket, rng);
        float const scale = LevelScale(proto->ItemLevel) * TIER_SCALE[tier];
        std::vector<AffixDef const*> pool;
        for (AffixDef const& def : AFFIXES)
            if (Fits(def, proto))
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
        uint8 rarity;             // its rolled rarity (0: from before rarities)
    };

    std::mutex sSeedsLock;
    std::unordered_map<uint32, Seeded> sSeeds;      // item guid low -> its seed

    Seeded SeedOf(Item const* item)
    {
        std::lock_guard<std::mutex> guard(sSeedsLock);
        auto it = sSeeds.find(item->GetGUIDLow());
        if (it == sSeeds.end() || it->second.entry != item->GetEntry())
            return { 0, 0, 0, 0 };
        return it->second;
    }

    void EnsureTable()
    {
        static bool const created = []()
        {
            CharacterDatabase.DirectExecute(
                "CREATE TABLE IF NOT EXISTS character_arpg_item ("
                "item INT UNSIGNED NOT NULL, entry MEDIUMINT UNSIGNED NOT NULL, seed INT UNSIGNED NOT NULL, "
                "tier TINYINT UNSIGNED NOT NULL DEFAULT 0, rarity TINYINT UNSIGNED NOT NULL DEFAULT 0, "
                "PRIMARY KEY (item)) ENGINE=InnoDB DEFAULT CHARSET=utf8 COMMENT='ARPG item affix seeds (Arpg/ArpgAffixes.h)'");
            // A table from before tiers or rarities: add the columns.
            for (char const* column : { "tier", "rarity" })
                if (!CharacterDatabase.PQuery("SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() "
                                              "AND TABLE_NAME = 'character_arpg_item' AND COLUMN_NAME = '%s'", column))
                    CharacterDatabase.DirectPExecute("ALTER TABLE character_arpg_item ADD COLUMN %s TINYINT UNSIGNED NOT NULL DEFAULT 0", column);
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

    struct Shown
    {
        ObjectGuid guid;
        uint8 quality;            // the colour its name shows (Rarity)
        uint8 border;             // a unique's affix bracket (Rarity), 0 for the rest
        std::string lines;
    };

    Shown ShownOf(Item const* item, uint32 seed, uint8 tier, uint8 rarity)
    {
        ItemPrototype const* proto = item->GetProto();
        uint8 const shown = RarityOf(proto, rarity);
        return { item->GetObjectGuid(), shown, uint8(shown == RARITY_UNIQUE ? BracketOf(proto, shown) : 0),
                 Joined(AffixLines(item->GetEntry(), seed, tier, rarity)) };
    }

    void SendLines(Player* player, std::vector<Shown> const& items)
    {
        if (items.empty())
            return;
        // Chunks keep each packet small.
        for (size_t at = 0; at < items.size(); at += 64)
        {
            size_t const end = std::min(items.size(), at + 64);
            WorldPacket data(SMSG_ARPG_ITEM_AFFIXES, 2 + (end - at) * 120);
            data << uint16(end - at);
            for (size_t i = at; i < end; ++i)
                data << items[i].guid << uint8(items[i].quality) << uint8(items[i].border) << items[i].lines;
            player->GetSession()->SendPacket(data);
        }
    }

    // The rarity a base item rolls at its drop: by who dropped it, raised by the dungeon tier.
    uint8 RollRarity(Creature const* source, uint8 tier)
    {
        Odds o = ODDS_TRASH;
        if (source)
        {
            uint8 const champion = ChampionTier(source);
            if (champion >= 2 || IsDungeonBoss(source))
                o = ODDS_RARE;
            // An elite counts as a champion out in the world; in a dungeon nearly all are elite.
            else if (champion == 1 || (source->IsElite() && !source->GetMap()->IsDungeon()))
                o = ODDS_CHAMPION;
        }
        o.white *= std::max(0.0f, 1.0f - 0.15f * tier);
        o.blue *= 1.0f + 0.2f * tier;
        o.purple *= 1.0f + 0.4f * tier;
        float roll = float(rand_norm()) * (o.white + o.green + o.blue + o.purple);
        if ((roll -= o.white) < 0.0f)
            return RARITY_WHITE;
        if ((roll -= o.green) < 0.0f)
            return RARITY_GREEN;
        if ((roll -= o.blue) < 0.0f)
            return RARITY_BLUE;
        return RARITY_PURPLE;
    }

    std::string FormatAffix(Rolled const& r)
    {
        char value[16];
        char buf[128];
        if (r.def->tenths)
        {
            std::snprintf(value, sizeof(value), "%d.%d", int(r.value / 10), int(r.value % 10));
            std::snprintf(buf, sizeof(buf), r.def->format, value);
        }
        else
            std::snprintf(buf, sizeof(buf), r.def->format, int(r.value));
        return buf;
    }
}

namespace Arpg
{
    std::vector<std::string> AffixLines(uint32 entry, uint32 seed, uint8 tier, uint8 rarity)
    {
        std::vector<std::string> lines;
        ItemPrototype const* proto = sObjectMgr.GetItemPrototype(entry);
        if (!proto || !Takes(proto))
            return lines;
        std::vector<Rolled> const rolled = Roll(proto, seed, tier, rarity);
        uint8 const shown = RarityOf(proto, rarity);
        // A unique says so, with its affixes' rarity in that rarity's colour (its border).
        if (shown == RARITY_UNIQUE)
        {
            uint8 const bracket = BracketOf(proto, shown);
            char buf[160];
            std::snprintf(buf, sizeof(buf), "%sUnique|r %s(%s: %u affixes)|r", RARITY_COLOUR[RARITY_UNIQUE],
                          RARITY_COLOUR[bracket], RARITY_NAME[bracket], uint32(rolled.size()));
            lines.emplace_back(buf);
        }
        tier = std::min<uint8>(tier, MAX_TIER);
        if (!rolled.empty() && tier)
            lines.push_back(std::string(TIER_COLOUR[tier]) + TierName(tier) + "|r");
        for (Rolled const& r : rolled)
            lines.push_back(FormatAffix(r));
        return lines;
    }

    uint8 LootItemQuality(LootItem const* item, uint8* border)
    {
        if (border)
            *border = 0;
        if (!item || !item->itemProto)
            return 0;
        ItemPrototype const* proto = item->itemProto;
        if (!item->arpgSeed || !Takes(proto))
            return uint8(proto->Quality);
        uint8 const shown = RarityOf(proto, item->arpgRarity);
        if (border && shown == RARITY_UNIQUE)
            *border = BracketOf(proto, shown);
        return shown;
    }

    void SeedArpgLoot(Loot* loot, Player* looter, uint8 tier, Creature const* source, bool goldFind)
    {
        if (!loot || !looter || !Active(looter))
            return;
        tier = std::min<uint8>(tier, MAX_TIER);
        loot->ForEachArpgItem([tier, source](LootItem* item)
        {
            if (item->arpgSeed || !Takes(item->itemProto))
                return;
            item->arpgSeed = urand(1, 0x7FFFFFFF);
            item->arpgTier = tier;
            if (UniqueBase(item->itemProto))
                item->arpgRarity = RARITY_UNIQUE;
            else if (!item->arpgRarity)
                item->arpgRarity = RollRarity(source, tier);
        });
        // Gold find: the looter's affixes add to the corpse's gold.
        if (std::shared_ptr<WebTotals const> totals = goldFind ? TotalsOf(looter) : nullptr)
            if (totals->goldFindPct > 0 && loot->GetGoldAmount())
                loot->AddArpgDevGold(loot->GetGoldAmount() * uint32(totals->goldFindPct) / 100);
    }

    bool AddRolledItem(Loot* loot, uint8 rarity, uint32 level)
    {
        if (!loot)
            return false;
        // A wearable base only: never a white loaf or a bolt of cloth.
        uint32 id = 0;
        for (int tries = 0; tries < 8 && !id; ++tries)
        {
            uint32 const quality = rarity == RARITY_UNIQUE ? (roll_chance_i(30) ? ITEM_QUALITY_EPIC : ITEM_QUALITY_RARE)
                                   : (roll_chance_i(50) ? ITEM_QUALITY_NORMAL : ITEM_QUALITY_UNCOMMON);
            uint32 const pick = PickRandomItem(quality, level);
            if (pick && Takes(sObjectMgr.GetItemPrototype(pick)))
                id = pick;
        }
        if (!id)
            return false;
        // The loot's newest item, if the add took (a full loot takes none): it keeps the rarity.
        LootItem* before = nullptr;
        loot->ForEachArpgItem([&](LootItem* item) { before = item; });
        loot->AddItem(id, 1, 0, Item::GenerateItemRandomPropertyId(id));
        LootItem* added = nullptr;
        loot->ForEachArpgItem([&](LootItem* item) { added = item; });
        if (!added || added == before || added->itemId != id)
            return false;
        if (rarity != RARITY_UNIQUE)
            added->arpgRarity = std::max<uint8>(RARITY_WHITE, std::min<uint8>(rarity, RARITY_PURPLE));
        return true;
    }

    void OnAffixedItemStored(Player* player, Item* item, uint32 seed, uint8 tier, uint8 rarity)
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
            sSeeds[item->GetGUIDLow()] = { seed, item->GetEntry(), tier, rarity };
        }
        CharacterDatabase.PExecute("REPLACE INTO character_arpg_item (item, entry, seed, tier, rarity) VALUES (%u, %u, %u, %u, %u)",
                                   item->GetGUIDLow(), item->GetEntry(), seed, uint32(tier), uint32(rarity));
        SendLines(player, { ShownOf(item, seed, tier, rarity) });
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
            for (Rolled const& r : Roll(item->GetProto(), seeded.seed, seeded.tier, seeded.rarity))
            {
                int32 const v = r.value;
                uint8 const p = r.def->param;
                switch (r.def->kind)
                {
                    case AK_STAT: totals.stat[p] += v; break;
                    case AK_ALL_STATS: for (int32& s : totals.stat) s += v; break;
                    case AK_TAG: totals.tag[p] += v; break;
                    case AK_HEALING: totals.healingPct += v; break;
                    case AK_ARMOUR: totals.armourPct += v; break;
                    case AK_BLOCK: totals.blockPct += v; break;
                    case AK_MOVE: totals.movePct += v; break;
                    case AK_COOLDOWN: totals.cooldownPct += v; break;
                    case AK_LIFE_ON_KILL: totals.lifeOnKill += v; break;
                    case AK_MANA_ON_HIT: totals.manaOnHit += v; break;
                    case AK_ATTACK_SPEED: totals.attackSpeedPct += v; break;
                    case AK_CAST_SPEED: totals.castSpeedPct += v; break;
                    case AK_MELEE_CRIT: totals.meleeCritPct += v; break;
                    case AK_SPELL_CRIT: totals.spellCritPct += v; break;
                    case AK_SPELL_CRIT_DAMAGE: totals.spellCritDamagePct += v; break;
                    case AK_COST: totals.costPct += v; break;
                    case AK_HEALTH_PCT: totals.healthPct += v; break;
                    case AK_MANA_PCT: totals.manaPct += v; break;
                    case AK_ATTACK_POWER: totals.attackPower += v; break;
                    case AK_SPELL_POWER: totals.spellPower += v; break;
                    case AK_RESIST: if (p < 5) totals.resist[p] += v; break;
                    case AK_ALL_RESIST: for (int32& res : totals.resist) res += v; break;
                    case AK_LIFE_LEECH: totals.lifeLeech += v; break;
                    case AK_MANA_LEECH: totals.manaLeech += v; break;
                    case AK_HEALTH_REGEN: totals.healthRegen += v; break;
                    case AK_MANA_REGEN: totals.manaRegen += v; break;
                    case AK_DAMAGE_TAKEN: totals.damageTakenPct += v; break;
                    case AK_THORNS: totals.thorns += v; break;
                    case AK_VS_ELITES: totals.elitePct += v; break;
                    case AK_SLAYING: if (p < 16) totals.slaying[p] += v; break;
                    case AK_GLOBE_HEAL: totals.globeHealPct += v; break;
                    case AK_GOLD_FIND: totals.goldFindPct += v; break;
                    case AK_XP: totals.xpPct += v; break;
                    case AK_MELEE_AREA: totals.meleeAreaPct += v; break;
                    case AK_SPELL_AREA: totals.spellAreaPct += v; break;
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
        std::vector<Shown> lines;
        // By the item's holder now (a trade or the mail moves it), and only for the item the seed
        // was rolled for: a guid reused after a crash, or a row left by a sold item, takes none.
        if (auto result = CharacterDatabase.PQuery(
                    "SELECT a.item, a.seed, a.entry, a.tier, a.rarity FROM character_arpg_item a JOIN item_instance i ON i.guid = a.item "
                    "WHERE i.owner_guid = %u AND i.itemEntry = a.entry", player->GetGUIDLow()))
        {
            do
            {
                Field* fields = result->Fetch();
                uint32 const low = fields[0].GetUInt32();
                uint32 const seed = fields[1].GetUInt32();
                uint32 const entry = fields[2].GetUInt32();
                uint8 const tier = uint8(std::min<uint32>(fields[3].GetUInt32(), MAX_TIER));
                uint8 const rarity = uint8(std::min<uint32>(fields[4].GetUInt32(), RARITY_UNIQUE));
                {
                    std::lock_guard<std::mutex> guard(sSeedsLock);
                    sSeeds[low] = { seed, entry, tier, rarity };
                }
                if (Item* item = player->GetItemByGuid(ObjectGuid(HIGHGUID_ITEM, low)))
                    lines.push_back(ShownOf(item, seed, tier, rarity));
            }
            while (result->NextRow());
        }
        SendLines(player, lines);
        RefreshTotals(player);
    }
}
