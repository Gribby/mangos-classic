/*
 * ARPG item affixes: see ArpgAffixes.h.
 */

#include "Arpg/ArpgAffixes.h"
#include "Arpg/ArpgCombat.h"
#include "Arpg/ArpgCraft.h"
#include "Arpg/ArpgCompare.h"
#include "Arpg/ArpgDamage.h"
#include "Arpg/ArpgDungeons.h"
#include "Arpg/ArpgLoot.h"
#include "Arpg/ArpgPacks.h"
#include "Arpg/ArpgProcs.h"
#include "Arpg/ArpgTags.h"
#include "Arpg/ArpgUniques.h"

#include "Database/DatabaseEnv.h"
#include "Entities/Creature.h"
#include "Entities/Bag.h"
#include "Entities/Item.h"
#include "Entities/Player.h"
#include "Globals/ObjectMgr.h"
#include "Loot/LootMgr.h"
#include "Mails/Mail.h"
#include "Chat/Chat.h"
#include "Server/Opcodes.h"
#include "Server/WorldPacket.h"
#include "Server/WorldSession.h"
#include "World/World.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include "Arpg/ArpgSets.h"

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
        // Damage (Arpg/ArpgDamage.h).
        AK_ADDED_ATTACK, AK_ADDED_SPELL, AK_CRIT_DAMAGE, AK_AILMENT_CHANCE, AK_AILMENT_DAMAGE,
        AK_SKILL_LEVEL,
    };

    // Where an affix may roll.
    enum AffixWhere : uint8 { W_ANY, W_WEAPON, W_ARMOUR, W_FEET, W_SHIELD, W_JEWELLERY, W_OFFENCE, W_SKILL };

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
        { AK_TAG, TAG_HOLY,        10, 30, "+%d%% Holy damage", W_ANY, 0 },
        { AK_TAG, TAG_PHYSICAL,    10, 30, "+%d%% Physical damage", W_ANY, 0 },
        { AK_TAG, TAG_FIRE,        10, 30, "+%d%% Fire damage", W_ANY, 0 },
        { AK_TAG, TAG_FROST,       10, 30, "+%d%% Frost damage", W_ANY, 0 },
        { AK_TAG, TAG_ARCANE,      10, 30, "+%d%% Arcane damage", W_ANY, 0 },
        { AK_TAG, TAG_NATURE,      10, 30, "+%d%% Nature damage", W_ANY, 0 },
        { AK_TAG, TAG_SHADOW,      10, 30, "+%d%% Shadow damage", W_ANY, 0 },
        { AK_TAG, TAG_AREA,        12, 36, "+%d%% Area damage", W_ANY, 0 },
        { AK_TAG, TAG_PROJECTILE,  10, 27, "+%d%% Projectile damage", W_ANY, 0 },
        { AK_TAG, TAG_MELEE,       8, 24, "+%d%% Melee damage", W_ANY, 0 },
        { AK_TAG, TAG_SPELL,       8, 24, "+%d%% Spell damage", W_ANY, 0 },
        { AK_TAG, TAG_DURATION,    10, 30, "+%d%% damage over time", W_ANY, 0 },
        { AK_TAG, TAG_SEAL,        10, 30, "+%d%% Seal damage", W_WEAPON, 0 },
        { AK_TAG, TAG_CHANNEL,     10, 30, "+%d%% channelled spell damage", W_ANY, 0 },
        { AK_VS_ELITES, 0,         8, 22, "+%d%% damage to elites and bosses", W_ANY, 0 },
        { AK_SLAYING, CREATURE_TYPE_UNDEAD,    12, 36, "+%d%% damage to Undead", W_WEAPON, 0 },
        { AK_SLAYING, CREATURE_TYPE_DEMON,     12, 36, "+%d%% damage to Demons", W_WEAPON, 0 },
        { AK_SLAYING, CREATURE_TYPE_BEAST,     12, 36, "+%d%% damage to Beasts", W_WEAPON, 0 },
        { AK_SLAYING, CREATURE_TYPE_HUMANOID,  12, 36, "+%d%% damage to Humanoids", W_WEAPON, 0 },
        { AK_SLAYING, CREATURE_TYPE_DRAGONKIN, 12, 36, "+%d%% damage to Dragonkin", W_WEAPON, 0 },
        { AK_SLAYING, CREATURE_TYPE_ELEMENTAL, 12, 36, "+%d%% damage to Elementals", W_WEAPON, 0 },
        // Offence.
        { AK_ATTACK_POWER, 0,     10, 40, "+%d attack power", W_ANY, 0 },
        { AK_SPELL_POWER, 0,       6, 24, "+%d spell damage", W_ANY, 0 },
        { AK_ATTACK_SPEED, 0,      3, 8,  "+%d%% attack speed", W_WEAPON, 20 },
        { AK_CAST_SPEED, 0,        3, 8,  "+%d%% cast speed", W_ANY, 20 },
        { AK_MELEE_CRIT, 0,        1, 3,  "+%d%% critical strike chance", W_ANY, 8 },
        { AK_SPELL_CRIT, 0,        1, 3,  "+%d%% spell critical strike chance", W_ANY, 8 },
        { AK_SPELL_CRIT_DAMAGE, 0, 12, 36, "+%d%% spell critical strike damage", W_JEWELLERY, 75 },
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
        { AK_HEALING, 0,           8, 26, "+%d%% healing", W_ANY, 0 },
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

        // Damage (Arpg/ArpgDamage.h), from the second pool on (POOL_DAMAGE). Added damage: its
        // average at vanilla's size, shown and dealt at the ARPG's size for the item's level, as a
        // range (60% to 140% of it). Weapons, gloves and jewellery (W_OFFENCE).
        { AK_ADDED_ATTACK, TAG_PHYSICAL, 10, 24, "Adds %d to %d Physical damage to attacks", W_OFFENCE, 0 },
        { AK_ADDED_ATTACK, TAG_HOLY,     10, 24, "Adds %d to %d Holy damage to attacks", W_OFFENCE, 0 },
        { AK_ADDED_ATTACK, TAG_FIRE,     10, 24, "Adds %d to %d Fire damage to attacks", W_OFFENCE, 0 },
        { AK_ADDED_ATTACK, TAG_FROST,    10, 24, "Adds %d to %d Frost damage to attacks", W_OFFENCE, 0 },
        { AK_ADDED_ATTACK, TAG_NATURE,   10, 24, "Adds %d to %d Nature damage to attacks", W_OFFENCE, 0 },
        { AK_ADDED_ATTACK, TAG_SHADOW,   10, 24, "Adds %d to %d Shadow damage to attacks", W_OFFENCE, 0 },
        { AK_ADDED_SPELL, TAG_HOLY,      12, 28, "Adds %d to %d Holy damage to spells", W_OFFENCE, 0 },
        { AK_ADDED_SPELL, TAG_FIRE,      12, 28, "Adds %d to %d Fire damage to spells", W_OFFENCE, 0 },
        { AK_ADDED_SPELL, TAG_FROST,     12, 28, "Adds %d to %d Frost damage to spells", W_OFFENCE, 0 },
        { AK_ADDED_SPELL, TAG_ARCANE,    12, 28, "Adds %d to %d Arcane damage to spells", W_OFFENCE, 0 },
        { AK_ADDED_SPELL, TAG_NATURE,    12, 28, "Adds %d to %d Nature damage to spells", W_OFFENCE, 0 },
        { AK_ADDED_SPELL, TAG_SHADOW,    12, 28, "Adds %d to %d Shadow damage to spells", W_OFFENCE, 0 },
        { AK_CRIT_DAMAGE, 0,              10, 30, "+%d%% critical strike multiplier", W_OFFENCE, 60 },
        { AK_AILMENT_CHANCE, TAG_PHYSICAL, 5, 15, "%d%% chance to cause Bleeding", W_ANY, 40 },
        { AK_AILMENT_CHANCE, TAG_HOLY,     5, 15, "%d%% chance to Judge", W_ANY, 40 },
        { AK_AILMENT_CHANCE, TAG_FIRE,     5, 15, "%d%% chance to Ignite", W_ANY, 40 },
        { AK_AILMENT_CHANCE, TAG_FROST,    5, 15, "%d%% chance to Chill", W_ANY, 40 },
        { AK_AILMENT_CHANCE, TAG_ARCANE,   5, 15, "%d%% chance to Overload", W_ANY, 40 },
        { AK_AILMENT_CHANCE, TAG_NATURE,   5, 15, "%d%% chance to Poison", W_ANY, 40 },
        { AK_AILMENT_CHANCE, TAG_SHADOW,   5, 15, "%d%% chance to Wither", W_ANY, 40 },
        { AK_AILMENT_DAMAGE, 0,            10, 30, "+%d%% increased damage with ailments", W_ANY, 0 },
        // Skill levels (Arpg/ArpgSkills.h): rare, on weapons and amulets; 2 from item level 69 or so.
        { AK_SKILL_LEVEL, 0, 1, 1, "+%d to all Melee skills", W_SKILL, 2 },
        { AK_SKILL_LEVEL, 1, 1, 1, "+%d to all Spell skills", W_SKILL, 2 },
        { AK_SKILL_LEVEL, 2, 1, 1, "+%d to all Holy skills", W_SKILL, 2 },
        { AK_SKILL_LEVEL, 3, 1, 1, "+%d to all Fire, Frost, Nature, Arcane and Shadow skills", W_SKILL, 2 },
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

    // A flat affix (attributes, attack power, added damage, resistances...) grows with the item
    // level as vanilla's own stats do (LevelScale). A percentage grows in step with the item level
    // up to 60 (a level 30 item rolls half its level 60 value, a level 40 two thirds; a tenth at
    // least), so every ten levels shows, then as the flat ones past it, for the raids.
    bool Flat(AffixKind kind)
    {
        switch (kind)
        {
            case AK_STAT: case AK_ALL_STATS: case AK_ATTACK_POWER: case AK_SPELL_POWER: case AK_LIFE_ON_KILL:
            case AK_MANA_ON_HIT: case AK_HEALTH_REGEN: case AK_MANA_REGEN: case AK_THORNS: case AK_RESIST:
            case AK_ALL_RESIST: case AK_ADDED_ATTACK: case AK_ADDED_SPELL:
                return true;
            default:
                return false;
        }
    }

    constexpr float PCT_FLOOR = 0.1f;

    float AffixScale(AffixKind kind, uint32 itemLevel, uint8 tier)
    {
        float level = LevelScale(itemLevel);
        if (!Flat(kind) && itemLevel < uint32(SCALE_LEVEL))
            level = std::max(PCT_FLOOR, float(itemLevel) / SCALE_LEVEL);
        return level * TIER_SCALE[std::min<uint8>(tier, MAX_TIER)];
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
        // A set's piece is as its set made it (Arpg/ArpgSets.h).
        if (IsSetPiece(proto->ItemId))
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
            case W_OFFENCE:   return proto->Class == ITEM_CLASS_WEAPON || jewel || type == INVTYPE_HANDS || type == INVTYPE_HOLDABLE;
            case W_SKILL:     return proto->Class == ITEM_CLASS_WEAPON || type == INVTYPE_NECK;
        }
        return false;
    }

    // The affixes a seed rolls from: an item stored before the damage affixes keeps rolling from
    // the first pool (the first POOL_FIRST_COUNT affixes), so its affixes never change.
    enum AffixPool : uint8 { POOL_FIRST = 0, POOL_DAMAGE = 1, POOL_CURRENT = POOL_DAMAGE };
    constexpr size_t POOL_FIRST_COUNT = 61;

    size_t PoolSize(uint8 pool);

    std::vector<Rolled> Roll(ItemPrototype const* proto, uint32 seed, uint8 tier, uint8 storedRarity, uint8 affixPool)
    {
        std::vector<Rolled> out;
        if (!Takes(proto) || !seed)
            return out;
        tier = std::min<uint8>(tier, MAX_TIER);
        Rng rng{ seed ^ (proto->ItemId * 2654435761u) };
        if (!rng.state)
            rng.state = 0x9E3779B9u;
        uint8 const shown = RarityOf(proto, storedRarity);
        uint8 const bracket = BracketOf(proto, shown);
        // A unique's affixes are its own: which ones, and how many (its bracket's top), follow its
        // entry alone, the same on every copy; only their values roll, within their ranges.
        bool const unique = shown == RARITY_UNIQUE;
        Rng fixed{ (proto->ItemId * 2246822519u) ^ 0x5BD1E995u };
        if (!fixed.state)
            fixed.state = 0x9E3779B9u;
        Rng& picks = unique ? fixed : rng;
        uint32 const count = unique ? (bracket >= RARITY_PURPLE ? 6u : 4u) : AffixCount(proto, tier, bracket, rng);
        std::vector<AffixDef const*> pool;
        // A unique's affixes come from the first pool whatever its row's: every copy has the same.
        size_t const size = PoolSize(unique ? uint8(POOL_FIRST) : affixPool);
        for (size_t i = 0; i < size; ++i)
            if (Fits(AFFIXES[i], proto))
                pool.push_back(&AFFIXES[i]);
        for (uint32 i = 0; i < count && !pool.empty(); ++i)
        {
            size_t const pick = picks.Next() % pool.size();
            AffixDef const* def = pool[pick];
            pool.erase(pool.begin() + pick);
            int32 const span = def->high - def->low;
            int32 const raw = def->low + int32(rng.Next() % uint32(span + 1));
            int32 value = std::max<int32>(1, int32(float(raw) * AffixScale(def->kind, proto->ItemLevel, tier) + 0.5f));
            if (def->cap)
                value = std::min(value, def->cap);
            out.push_back({ def, value });
        }
        return out;
    }

    struct Seeded
    {
        uint32 seed = 0;
        uint32 entry = 0;         // the item it was rolled for, so a reused guid never takes it
        uint8 tier = 0;           // the dungeon tier it dropped at
        uint8 rarity = 0;         // its rolled rarity (0: from before rarities)
        std::string affixes;      // a crafted item's own list (Crafted), else empty: the seed's
        uint8 forged = 0;         // the shard crafts it has taken
        uint8 pool = POOL_CURRENT; // the affixes its seed rolls from (AffixPool)
    };

    // A crafted item's affixes are kept as a list, not a seed: "v1|" then "id:value" by ";", an
    // affix's id its place in AFFIXES from 1 (the list only ever grows at its end).
    constexpr char const* CRAFTED = "v1|";
    constexpr uint16 AFFIX_COUNT = uint16(sizeof(AFFIXES) / sizeof(AFFIXES[0]));
    static_assert(POOL_FIRST_COUNT < AFFIX_COUNT, "the first pool is the affixes before the damage ones");

    size_t PoolSize(uint8 pool)
    {
        return pool == POOL_FIRST ? POOL_FIRST_COUNT : AFFIX_COUNT;
    }

    uint16 IdOf(AffixDef const* def) { return uint16(def - AFFIXES + 1); }

    std::vector<Rolled> Parsed(std::string const& text)
    {
        std::vector<Rolled> out;
        size_t at = std::strlen(CRAFTED);
        while (at < text.size())
        {
            size_t const end = std::min(text.find(';', at), text.size());
            unsigned id = 0;
            int value = 0;
            if (std::sscanf(text.c_str() + at, "%u:%d", &id, &value) == 2 && id >= 1 && id <= AFFIX_COUNT)
            {
                AffixDef const* def = &AFFIXES[id - 1];
                value = std::max(1, value);
                if (def->cap)
                    value = std::min<int>(value, def->cap);
                out.push_back({ def, int32(value) });
            }
            at = end + 1;
        }
        return out;
    }

    std::string Encoded(std::vector<Rolled> const& list)
    {
        std::string out = CRAFTED;
        char buf[24];
        for (size_t i = 0; i < list.size(); ++i)
        {
            std::snprintf(buf, sizeof(buf), "%s%u:%d", i ? ";" : "", uint32(IdOf(list[i].def)), int(list[i].value));
            out += buf;
        }
        return out;
    }

    bool IsCrafted(std::string const& affixes) { return affixes.compare(0, std::strlen(CRAFTED), CRAFTED) == 0; }

    // An item's affixes: its crafted list, else what its seed rolls.
    std::vector<Rolled> AffixesOf(ItemPrototype const* proto, Seeded const& s)
    {
        if (!Takes(proto) || !s.seed)
            return {};
        if (IsCrafted(s.affixes))
            return Parsed(s.affixes);
        return Roll(proto, s.seed, s.tier, s.rarity, s.pool);
    }

    std::mutex sSeedsLock;
    std::unordered_map<uint32, Seeded> sSeeds;      // item guid low -> its seed

    Seeded SeedOf(Item const* item)
    {
        std::lock_guard<std::mutex> guard(sSeedsLock);
        auto it = sSeeds.find(item->GetGUIDLow());
        if (it == sSeeds.end() || it->second.entry != item->GetEntry())
            return Seeded();
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
                "affixes VARCHAR(255) NOT NULL DEFAULT '', forged TINYINT UNSIGNED NOT NULL DEFAULT 0, "
                "pool TINYINT UNSIGNED NOT NULL DEFAULT 0, "
                "PRIMARY KEY (item)) ENGINE=InnoDB DEFAULT CHARSET=utf8 COMMENT='ARPG item affix seeds (Arpg/ArpgAffixes.h)'");
            // A table from before tiers or rarities: add the columns.
            for (char const* column : { "tier", "rarity" })
                if (!CharacterDatabase.PQuery("SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() "
                                              "AND TABLE_NAME = 'character_arpg_item' AND COLUMN_NAME = '%s'", column))
                    CharacterDatabase.DirectPExecute("ALTER TABLE character_arpg_item ADD COLUMN %s TINYINT UNSIGNED NOT NULL DEFAULT 0", column);
            // And from before crafting.
            if (!CharacterDatabase.PQuery("SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() "
                                          "AND TABLE_NAME = 'character_arpg_item' AND COLUMN_NAME = 'affixes'"))
                CharacterDatabase.DirectExecute("ALTER TABLE character_arpg_item ADD COLUMN affixes VARCHAR(255) NOT NULL DEFAULT '', "
                                                "ADD COLUMN forged TINYINT UNSIGNED NOT NULL DEFAULT 0");
            // And from before the damage affixes: its rows roll from the first pool.
            if (!CharacterDatabase.PQuery("SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() "
                                          "AND TABLE_NAME = 'character_arpg_item' AND COLUMN_NAME = 'pool'"))
                CharacterDatabase.DirectExecute("ALTER TABLE character_arpg_item ADD COLUMN pool TINYINT UNSIGNED NOT NULL DEFAULT 0");
            return true;
        }();
        (void)created;
    }

    // Keep `s` as `item`'s, in memory and in the table.
    void Store(Item const* item, Seeded const& s)
    {
        EnsureTable();
        {
            std::lock_guard<std::mutex> guard(sSeedsLock);
            sSeeds[item->GetGUIDLow()] = s;
        }
        std::string affixes = s.affixes;
        CharacterDatabase.escape_string(affixes);
        CharacterDatabase.PExecute("REPLACE INTO character_arpg_item (item, entry, seed, tier, rarity, affixes, forged, pool) "
                                   "VALUES (%u, %u, %u, %u, %u, '%s', %u, %u)", item->GetGUIDLow(), s.entry, s.seed,
                                   uint32(s.tier), uint32(s.rarity), affixes.c_str(), uint32(s.forged), uint32(s.pool));
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

    std::vector<std::string> LinesOf(ItemPrototype const* proto, Seeded const& s);

    Shown ShownOf(Item const* item, Seeded const& s)
    {
        ItemPrototype const* proto = item->GetProto();
        uint8 const shown = RarityOf(proto, s.rarity);
        return { item->GetObjectGuid(), shown, uint8(shown == RARITY_UNIQUE ? BracketOf(proto, shown) : 0),
                 Joined(LinesOf(proto, s)) };
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

    // Added damage's range at the ARPG's size for an item of `itemLevel` (Arpg/ArpgDamage.h).
    void AddedRange(int32 average, uint32 itemLevel, int32& low, int32& high)
    {
        float const size = PowerScaleAt(std::min<uint32>(itemLevel, 60));
        low = std::max<int32>(1, int32(average * 0.6f * size + 0.5f));
        high = std::max<int32>(low, int32(average * 1.4f * size + 0.5f));
    }

    std::string FormatAffix(Rolled const& r, uint32 itemLevel)
    {
        char value[16];
        char buf[128];
        if (r.def->kind == AK_ADDED_ATTACK || r.def->kind == AK_ADDED_SPELL)
        {
            int32 low, high;
            AddedRange(r.value, itemLevel, low, high);
            std::snprintf(buf, sizeof(buf), r.def->format, int(low), int(high));
        }
        else if (r.def->tenths)
        {
            std::snprintf(value, sizeof(value), "%d.%d", int(r.value / 10), int(r.value % 10));
            std::snprintf(buf, sizeof(buf), r.def->format, value);
        }
        else
            std::snprintf(buf, sizeof(buf), r.def->format, int(r.value));
        return buf;
    }
}

namespace
{
    std::vector<std::string> LinesOf(ItemPrototype const* proto, Seeded const& s)
    {
        std::vector<std::string> lines;
        if (!proto || !Takes(proto))
            return lines;
        uint8 tier = s.tier;
        std::vector<Rolled> const rolled = AffixesOf(proto, s);
        uint8 const shown = RarityOf(proto, s.rarity);
        // A unique says so, with its affixes' rarity in that rarity's colour (its border).
        if (shown == RARITY_UNIQUE)
        {
            uint8 const bracket = BracketOf(proto, shown);
            char buf[160];
            std::snprintf(buf, sizeof(buf), "%sUnique|r %s(%s: %u affixes)|r", RARITY_COLOUR[RARITY_UNIQUE],
                          RARITY_COLOUR[bracket], RARITY_NAME[bracket], uint32(rolled.size()));
            lines.emplace_back(buf);
            // Its power, when it carries one from the pool (an authored one's comes with the
            // mechanics table, Arpg/ArpgUniques.h).
            if (char const* power = UniquePowerText(proto->ItemId))
                lines.push_back(std::string(RARITY_COLOUR[RARITY_UNIQUE]) + UniquePowerName(proto->ItemId) + ":|r " +
                                RARITY_COLOUR[RARITY_UNIQUE] + power + "|r");
        }
        tier = std::min<uint8>(tier, MAX_TIER);
        if (!rolled.empty() && tier)
            lines.push_back(std::string(TIER_COLOUR[tier]) + TierName(tier) + "|r");
        for (Rolled const& r : rolled)
            lines.push_back(FormatAffix(r, proto->ItemLevel));
        if (s.forged)
        {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "|cff808080Forged %u/%u|r", uint32(s.forged), uint32(FORGE_LIMIT));
            lines.emplace_back(buf);
        }
        return lines;
    }
}

namespace Arpg
{
    std::vector<std::string> AffixLines(uint32 entry, uint32 seed, uint8 tier, uint8 rarity)
    {
        Seeded s;
        s.seed = seed;
        s.entry = entry;
        s.tier = tier;
        s.rarity = rarity;
        return LinesOf(sObjectMgr.GetItemPrototype(entry), s);
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
        // A piece of the looter's class's set, sometimes (Arpg/ArpgSets.h).
        MaybeAddSetPiece(loot, looter, source);
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
        Seeded s;
        s.seed = seed;
        s.entry = item->GetEntry();
        s.tier = tier;
        s.rarity = rarity;
        Store(item, s);
        SendLines(player, { ShownOf(item, s) });
    }

    void AddItemTotals(Player const* player, WebTotals& totals)
    {
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (item && !item->IsBroken())
                AddAffixTotals(item, totals, 1);
        }
        // The worn sets' bonuses (Arpg/ArpgSets.h).
        AddSetTotals(player, totals);
    }

    uint32 AffixStateOf(Item const* item)
    {
        Seeded const s = SeedOf(item);
        uint32 state = s.seed * 2654435761u ^ (uint32(s.tier) << 3) ^ (uint32(s.rarity) << 7) ^ (uint32(s.forged) << 11) ^
                       (uint32(s.pool) << 15);
        state ^= uint32(std::hash<std::string>()(s.affixes));
        return state;
    }

    void AddAffixTotals(Item const* item, WebTotals& totals, int32 sign)
    {
        {
            Seeded const seeded = SeedOf(item);
            if (!seeded.seed)
                return;
            for (Rolled const& r : AffixesOf(item->GetProto(), seeded))
            {
                int32 const v = r.value * sign;
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
                    // Added damage at the ARPG's size for the item's level, its average.
                    case AK_ADDED_ATTACK:
                    case AK_ADDED_SPELL:
                        if (p < 7)
                        {
                            int32 low, high;
                            AddedRange(r.value, item->GetProto()->ItemLevel, low, high);
                            (r.def->kind == AK_ADDED_ATTACK ? totals.addedAttack : totals.addedSpell)[p] += sign * ((low + high) / 2);
                        }
                        break;
                    case AK_CRIT_DAMAGE: totals.critDamagePct += v; break;
                    case AK_AILMENT_CHANCE: if (p < 7) totals.ailmentChance[p] += v; break;
                    case AK_AILMENT_DAMAGE: totals.ailmentPct += v; break;
                    case AK_SKILL_LEVEL: if (p < 4) totals.skillLevel[p] += v; break;
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
                    "SELECT a.item, a.seed, a.entry, a.tier, a.rarity, a.affixes, a.forged, a.pool FROM character_arpg_item a JOIN item_instance i ON i.guid = a.item "
                    "WHERE i.owner_guid = %u AND i.itemEntry = a.entry", player->GetGUIDLow()))
        {
            do
            {
                Field* fields = result->Fetch();
                uint32 const low = fields[0].GetUInt32();
                uint32 const seed = fields[1].GetUInt32();
                uint32 const entry = fields[2].GetUInt32();
                uint8 const tier = uint8(std::min<uint32>(fields[3].GetUInt32(), MAX_TIER));
                Seeded s;
                s.seed = seed;
                s.entry = entry;
                s.tier = tier;
                s.rarity = uint8(std::min<uint32>(fields[4].GetUInt32(), RARITY_UNIQUE));
                s.affixes = fields[5].GetCppString();
                s.forged = uint8(fields[6].GetUInt32());
                s.pool = uint8(std::min<uint32>(fields[7].GetUInt32(), POOL_CURRENT));
                {
                    std::lock_guard<std::mutex> guard(sSeedsLock);
                    sSeeds[low] = s;
                }
                if (Item* item = player->GetItemByGuid(ObjectGuid(HIGHGUID_ITEM, low)))
                    lines.push_back(ShownOf(item, s));
            }
            while (result->NextRow());
        }
        // A unique the character came by another way (a quest reward, from before the ARPG) has
        // no row: it still shows that it is one, and its power.
        std::unordered_set<ObjectGuid> sent;
        for (Shown const& shown : lines)
            sent.insert(shown.guid);
        auto addUnseeded = [&](Item* item)
        {
            if (!item || sent.count(item->GetObjectGuid()) || !Takes(item->GetProto()) || !UniqueBase(item->GetProto()))
                return;
            Seeded s;
            s.entry = item->GetEntry();
            lines.push_back(ShownOf(item, s));
            sent.insert(item->GetObjectGuid());
        };
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            addUnseeded(player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
        for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
            if (Bag* container = dynamic_cast<Bag*>(player->GetItemByPos(INVENTORY_SLOT_BAG_0, bag)))
                for (uint32 i = 0; i < container->GetBagSize(); ++i)
                    addUnseeded(container->GetItemByPos(uint8(i)));
        SendLines(player, lines);
        RefreshTotals(player);
        // A fresh hello (a reconnect) is sent its comparisons again (Arpg/ArpgCompare.h).
        ForgetCompare(player);
    }

    // --- Crafting (Arpg/ArpgCraft.h) ---

    uint16 AffixTotal() { return AFFIX_COUNT; }

    std::string ShardName(uint16 affix)
    {
        if (!affix || affix > AFFIX_COUNT)
            return "Shard";
        AffixDef const& def = AFFIXES[affix - 1];
        switch (def.kind)
        {
            case AK_LIFE_LEECH:   return "Shard of Life Leech";
            case AK_MANA_LEECH:   return "Shard of Mana Leech";
            case AK_THORNS:       return "Shard of Thorns";
            case AK_COST:         return "Shard of Thrift";
            case AK_DAMAGE_TAKEN: return "Shard of Warding";
            case AK_CRIT_DAMAGE:  return "Shard of Ruin";
            case AK_AILMENT_DAMAGE: return "Shard of Affliction";
            case AK_SKILL_LEVEL:
            {
                static char const* const NAMES[4] = { "Shard of Arms", "Shard of Sorcery", "Shard of the Light", "Shard of the Elements" };
                return NAMES[std::min<uint8>(def.param, 3)];
            }
            case AK_ADDED_ATTACK: return std::string("Shard of ") + TagName(Tag(def.param)) + " Strikes";
            case AK_ADDED_SPELL:  return std::string("Shard of ") + TagName(Tag(def.param)) + " Spells";
            case AK_AILMENT_CHANCE:
            {
                static char const* const NAMES[7] = { "Bleeding", "Judgement", "Ignition", "Chilling", "Overload", "Poison", "Withering" };
                return std::string("Shard of ") + NAMES[std::min<uint8>(def.param, 6)];
            }
            default: break;
        }
        // The line without its number: "+%d%% Holy damage" is a Shard of Holy damage.
        std::string name = def.format;
        for (char const* token : { "+%d%% ", "+%d ", "%d%% ", "%s%% " })
        {
            size_t const at = name.find(token);
            if (at != std::string::npos)
                name.erase(at, std::strlen(token));
        }
        if (!name.empty())
            name[0] = char(std::toupper(static_cast<unsigned char>(name[0])));
        return "Shard of " + name;
    }

    bool Craftable(Item const* item)
    {
        return item && Takes(item->GetProto());
    }

    char const* CraftError(CraftResult result)
    {
        switch (result)
        {
            case CRAFT_OK:            return "";
            case CRAFT_NOT_ITEM:      return "Only weapons and armour take crafting.";
            case CRAFT_WRONG_RARITY:  return "That orb does not work on an item of this rarity.";
            case CRAFT_NO_AFFIXES:    return "The item has no affixes for that.";
            case CRAFT_FULL:          return "The item has no room for another affix.";
            case CRAFT_FORGED_OUT:    return "The item has taken all the shards it can.";
            case CRAFT_NO_FIT:        return "That affix cannot go on this kind of item.";
            case CRAFT_AT_TOP:        return "That affix is already at its highest on this item.";
            case CRAFT_UNIQUE_FIXED:  return "A unique's affixes are its own: only their values can change.";
        }
        return "";
    }

    CraftResult CraftItem(Player* player, Item* item, uint8 op, uint16 affix, std::string& done)
    {
        if (!player || !Craftable(item))
            return CRAFT_NOT_ITEM;
        ItemPrototype const* proto = item->GetProto();
        Seeded s = SeedOf(item);
        if (!s.seed)
        {
            // An item from before the ARPG (a quest reward, a vendor's): a seed of its own, and no
            // affixes yet, as it shows.
            s.seed = urand(1, 0x7FFFFFFF);
            s.entry = item->GetEntry();
            s.affixes = CRAFTED;
        }
        std::vector<Rolled> list = AffixesOf(proto, s);
        uint8 const shown = RarityOf(proto, s.rarity);
        bool const unique = shown == RARITY_UNIQUE;
        uint8 const bracket = BracketOf(proto, shown);
        uint32 const most = bracket == RARITY_GREEN ? 2 : bracket == RARITY_BLUE ? 4 : bracket == RARITY_PURPLE ? 6 : 0;
        Rng rng{ urand(1, 0x7FFFFFFF) };

        auto value = [&](AffixDef const* def, int32 raw)
        {
            int32 v = std::max<int32>(1, int32(float(raw) * AffixScale(def->kind, proto->ItemLevel, s.tier) + 0.5f));
            return def->cap ? std::min(v, def->cap) : v;
        };
        auto rolled = [&](AffixDef const* def)
        {
            return value(def, def->low + int32(rng.Next() % uint32(def->high - def->low + 1)));
        };
        auto has = [&](AffixDef const* def)
        {
            for (Rolled const& r : list)
                if (r.def == def)
                    return true;
            return false;
        };
        auto addRandom = [&](uint32 count)
        {
            std::vector<AffixDef const*> pool;
            for (AffixDef const& def : AFFIXES)
                if (Fits(def, proto) && !has(&def))
                    pool.push_back(&def);
            for (uint32 i = 0; i < count && !pool.empty(); ++i)
            {
                size_t const pick = rng.Next() % pool.size();
                list.push_back({ pool[pick], rolled(pool[pick]) });
                pool.erase(pool.begin() + pick);
            }
        };
        auto raise = [&](uint8 to, uint32 low)
        {
            s.rarity = to;
            uint32 const want = low + rng.Next() % 2;
            if (list.size() < want)
                addRandom(uint32(want - list.size()));
        };

        switch (op)
        {
            case ORB_AWAKENING:
                if (unique || shown != RARITY_WHITE)
                    return CRAFT_WRONG_RARITY;
                list.clear();
                raise(RARITY_GREEN, 1);
                done = "The item awakens: it is Magic now.";
                break;
            case ORB_REFINEMENT:
                if (unique || shown != RARITY_GREEN)
                    return CRAFT_WRONG_RARITY;
                raise(RARITY_BLUE, 3);
                done = "The item is refined: it is Rare now.";
                break;
            case ORB_ASCENSION:
                if (unique || shown != RARITY_BLUE)
                    return CRAFT_WRONG_RARITY;
                raise(RARITY_PURPLE, 5);
                done = "The item ascends: it is Epic now.";
                break;
            case ORB_FEL:
            {
                if (unique)
                    return CRAFT_UNIQUE_FIXED;
                if (list.empty())
                    return CRAFT_NO_AFFIXES;
                uint32 const count = uint32(list.size());
                list.clear();
                addRandom(count);
                done = "Fel fire remakes every affix.";
                break;
            }
            case ORB_INVOCATION:
                if (unique)
                    return CRAFT_UNIQUE_FIXED;
                if (list.size() >= most)
                    return CRAFT_FULL;
                addRandom(1);
                done = "A new affix answers the call.";
                break;
            case ORB_VOID:
                if (unique)
                    return CRAFT_UNIQUE_FIXED;
                if (list.empty())
                    return CRAFT_NO_AFFIXES;
                list.erase(list.begin() + rng.Next() % list.size());
                done = "The void takes an affix.";
                break;
            case ORB_LENS:
                if (list.empty())
                    return CRAFT_NO_AFFIXES;
                for (Rolled& r : list)
                    r.value = rolled(r.def);
                done = "The lens refocuses every value.";
                break;
            case CRAFT_SHARD:
            {
                if (!affix || affix > AFFIX_COUNT)
                    return CRAFT_NO_FIT;
                AffixDef const* def = &AFFIXES[affix - 1];
                if (!Fits(*def, proto))
                    return CRAFT_NO_FIT;
                if (s.forged >= FORGE_LIMIT)
                    return CRAFT_FORGED_OUT;
                int32 const top = value(def, def->high);
                bool found = false;
                for (Rolled& r : list)
                    if (r.def == def)
                    {
                        if (r.value >= top)
                            return CRAFT_AT_TOP;
                        // A third of the way to the top again, at least one.
                        r.value = std::min(top, r.value + std::max<int32>(1, (top - r.value + 2) / 3));
                        found = true;
                    }
                if (!found)
                {
                    // A shard raises a unique's own affix, but adds none.
                    if (unique)
                        return CRAFT_UNIQUE_FIXED;
                    if (list.size() >= most)
                        return CRAFT_FULL;
                    list.push_back({ def, rolled(def) });
                }
                ++s.forged;
                done = found ? "The shard strengthens its affix." : "The shard binds a new affix.";
                break;
            }
            default:
                return CRAFT_NOT_ITEM;
        }
        if (!s.rarity)
            s.rarity = shown;
        s.affixes = Encoded(list);
        Store(item, s);
        SendLines(player, { ShownOf(item, s) });
        if (item->IsEquipped())
            RefreshTotals(player);
        return CRAFT_OK;
    }

    std::vector<uint16> SalvageShards(Item const* item)
    {
        std::vector<uint16> out;
        if (!Craftable(item))
            return out;
        ItemPrototype const* proto = item->GetProto();
        Seeded const s = SeedOf(item);
        std::vector<Rolled> list = AffixesOf(proto, s);
        uint8 const bracket = BracketOf(proto, RarityOf(proto, s.rarity));
        uint32 const take = bracket >= RARITY_BLUE ? 2 : 1;
        for (uint32 i = 0; i < take && !list.empty(); ++i)
        {
            size_t const pick = urand(0, uint32(list.size() - 1));
            out.push_back(IdOf(list[pick].def));
            list.erase(list.begin() + pick);
        }
        return out;
    }

    uint8 ShownRarity(Item const* item)
    {
        if (!Craftable(item))
            return 0;
        return RarityOf(item->GetProto(), SeedOf(item).rarity);
    }

    bool GiveRolledItem(Player* player, uint8 rarity, uint32 level)
    {
        if (!player)
            return false;
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
        uint8 const stored = rarity == RARITY_UNIQUE ? uint8(RARITY_UNIQUE)
                             : std::max<uint8>(RARITY_WHITE, std::min<uint8>(rarity, RARITY_PURPLE));
        uint32 const seed = urand(1, 0x7FFFFFFF);
        ItemPosCountVec dest;
        if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, id, 1) == EQUIP_ERR_OK)
        {
            Item* item = player->StoreNewItem(dest, id, true, Item::GenerateItemRandomPropertyId(id));
            if (!item)
                return false;
            player->SendNewItem(item, 1, true, false);
            OnAffixedItemStored(player, item, seed, 0, stored);
            return true;
        }
        // The bags full: it comes by mail.
        Item* item = Item::CreateItem(id, 1, player);
        if (!item)
            return false;
        if (int32 const random = Item::GenerateItemRandomPropertyId(id))
            item->SetItemRandomProperties(random);
        item->SaveToDB();
        OnAffixedItemStored(player, item, seed, 0, stored);
        MailDraft draft("Bounty reward", "Your bags were full, so your reward was sent here.");
        draft.AddItem(item);
        draft.SendMailTo(player, MailSender(player, MAIL_STATIONERY_GM), MAIL_CHECK_MASK_RETURNED);
        ChatHandler(player).PSendSysMessage("|cffffd200Your bags are full: the reward was sent by mail.|r");
        return true;
    }
}
