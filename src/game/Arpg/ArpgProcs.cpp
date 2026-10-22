/*
 * ARPG cross-skill procs: see ArpgProcs.h.
 */

#include "Arpg/ArpgProcs.h"
#include "Arpg/ArpgStates.h"
#include "Arpg/ArpgDamage.h"
#include "Arpg/ArpgCharacter.h"
#include "Arpg/ArpgCombat.h"
#include "Arpg/ArpgSkills.h"
#include "Arpg/ArpgUniques.h"

#include "Globals/ObjectMgr.h"
#include "Log/Log.h"
#include "Maps/Map.h"

#include "Entities/Item.h"
#include "Entities/Player.h"
#include "Spells/Spell.h"
#include "Spells/SpellMgr.h"
#include "Server/DBCStores.h"
#include "Util/Timer.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <numeric>
#include <string>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace
{
    using namespace Arpg;

    enum Trigger : uint8
    {
        TRIG_SWING_CRIT,      // a swing crits
        TRIG_SPELL_HIT,       // a damage spell hits
        TRIG_SPELL_CRIT,      // a damage spell crits
        TRIG_HEAL_CRIT,       // a heal crits
        TRIG_LANDED,          // a spell lands on an enemy (its auras applied)
        TRIG_KILL,            // a kill
        TRIG_STRUCK,          // a blow of a fifth of the player's health or more lands on them
    };

    enum Effect : uint8
    {
        EFFECT_INSTANT,       // the next cast of the targets is instant
        EFFECT_DAMAGE,        // ... deals `value`% more (heals heal more)
        EFFECT_CRIT,          // ... is sure to critically strike
        EFFECT_FREE,          // ... costs nothing
        EFFECT_RESET,         // the targets' cooldowns end now
        EFFECT_WIDE,          // ... has `value`% more area
        EFFECT_KIT,           // ... carries the row's `kit` alone (a burst, a chain, a spread)
        // Done at once, not readied:
        EFFECT_HEAL,          // heal `value`% of maximum health
        EFFECT_MANA,          // restore `value`% of maximum mana
        EFFECT_EXPLODE,       // the slain foe bursts: `value`% of its maximum health as Fire nearby
        EFFECT_ARC,           // `value`% of the hit leaps to up to 3 enemies near the struck one
        EFFECT_NOVA,          // `value`% of the hit (or blow) strikes every enemy around the player
    };


    struct ProcDef
    {
        uint8 rune;                           // a rune's id; 0 for an item's
        uint32 item;                          // an item's entry; 0 for a rune's
        char const* name;
        Trigger trigger;
        SkillId skill;                        // the triggering spell's skill (a rune's: its own)
        std::vector<char const*> from;        // or the triggering spells, by name (empty: any)
        Effect effect;
        std::vector<char const*> targets;     // the spells it readies or resets, by name ("*": all)
        int32 value;                          // EFFECT_DAMAGE's percent
        uint32 icd;                           // ms between procs
        uint32 lasts;                         // ms a readied cast waits
        char const* targetWords;              // "Holy Light or Flash of Light", for the lines
        char const* text;                     // the rune's or the item's line
        // A skill tree's synergy node it comes from, by id (0: none): taken, it is the player's.
        uint16 node = 0;
        // An item's name, for an item by name (its entry found when the table is first read).
        char const* itemName = nullptr;
        // A uniques-kit row the readied cast also carries (kit 0: none), beside its effect.
        UniqueMechanic kit{};
    };

    // A row from a skill tree's synergy node: `trigger` from `from` readies `effect` (and `kit`)
    // on the next cast of `targets` within `lasts` ms, once every `icd` ms.
    ProcDef Synergy(uint16 node, char const* name, Trigger trigger, std::vector<char const*> from, Effect effect,
                    std::vector<char const*> targets, int32 value, uint32 icd, uint32 lasts, char const* targetWords,
                    char const* text, UniqueMechanic kit = {})
    {
        ProcDef def{ 0, 0, name, trigger, Arpg::SKILL_NONE, std::move(from), effect, std::move(targets), value, icd, lasts,
                     targetWords, text };
        def.node = node;
        def.kit = kit;
        return def;
    }

    // A row from an item by name.
    ProcDef Worn(char const* item, Trigger trigger, std::vector<char const*> from, Effect effect,
                 std::vector<char const*> targets, int32 value, uint32 icd, uint32 lasts, char const* targetWords,
                 char const* text, UniqueMechanic kit = {})
    {
        ProcDef def{ 0, 0, item, trigger, Arpg::SKILL_NONE, std::move(from), effect, std::move(targets), value, icd, lasts,
                     targetWords, text };
        def.itemName = item;
        def.kit = kit;
        return def;
    }

    // The stock spells whose hits stand for the powers' damage: Fire, Nature, Holy.
    constexpr uint32 EXPLODE_LOOK = 133, ARC_LOOK = 403, NOVA_LOOK = 2812;
    constexpr float EXPLODE_RANGE = 8.0f, ARC_RANGE = 10.0f, NOVA_RANGE = 8.0f;
    constexpr uint32 ARC_COUNT = 3;
    // A burst never deals more than this share of the player's own health (a boss's corpse).
    constexpr uint32 EXPLODE_CAP_PCT = 60;

    // A readied cast spent by a spell lasts until that spell is done, or this long at most.
    constexpr uint32 SPENT_GRACE = 10000;

    std::vector<ProcDef> const& Procs()
    {
        static std::vector<ProcDef> const procs = []()
        {
        std::vector<ProcDef> rows =
        {
            // --- Runes: socketed in the triggering skill ---
            { RUNE_GRACE, 0, "Rune of Grace", TRIG_SWING_CRIT, SKILL_STRIKE, {}, EFFECT_INSTANT,
              { "Holy Light", "Flash of Light" }, 0, 10000, 10000, "Holy Light or Flash of Light",
              "A critical swing lets your next Holy Light or Flash of Light skip its recovery (once every 10 sec)." },
            { RUNE_VERDICT, 0, "Rune of Verdict", TRIG_SPELL_HIT, SKILL_JUDGEMENT, {}, EFFECT_DAMAGE,
              { "Exorcism", "Hammer of Wrath" }, 50, 8000, 8000, "Exorcism or Hammer of Wrath",
              "A Judgement that hits makes your next Exorcism or Hammer of Wrath deal 50% more (once every 8 sec)." },
            { RUNE_ANVIL, 0, "Rune of the Anvil", TRIG_LANDED, SKILL_HAMMER, {}, EFFECT_RESET,
              { "Judgement" }, 0, 10000, 0, "Judgement",
              "Stunning an enemy with Hammer of Justice ends Judgement's cooldown (once every 10 sec)." },
            { RUNE_FERVOUR, 0, "Rune of Fervour", TRIG_SPELL_CRIT, Arpg::SKILL_NONE, {}, EFFECT_CRIT,
              { "Judgement", "Judgement of Righteousness", "Judgement of Command" }, 0, 8000, 10000, "Judgement",
              "A critical hit with the skill makes your next Judgement a sure critical strike (once every 8 sec)." },
            { RUNE_ZEAL, 0, "Rune of Zeal", TRIG_SPELL_CRIT, SKILL_STRIKE, {}, EFFECT_FREE,
              { "Holy Light", "Flash of Light" }, 0, 10000, 12000, "Holy Light or Flash of Light",
              "A critical Seal hit makes your next Holy Light or Flash of Light cost no mana (once every 10 sec)." },

            // --- Uniques: worn ---
            { 0, 2243, "Hand of Edward the Odd", TRIG_SWING_CRIT, Arpg::SKILL_NONE, {}, EFFECT_INSTANT,
              { "*" }, 0, 20000, 10000, "spell",
              "A critical swing lets your next spell skip its recovery (once every 20 sec)." },
            { 0, 11684, "Ironfoe", TRIG_SWING_CRIT, Arpg::SKILL_NONE, {}, EFFECT_DAMAGE,
              { "Judgement", "Judgement of Righteousness", "Judgement of Command" }, 60, 6000, 8000, "Judgement",
              "A critical swing makes your next Judgement deal 60% more (once every 6 sec)." },
            { 0, 6953, "Verigan's Fist", TRIG_KILL, Arpg::SKILL_NONE, {}, EFFECT_RESET,
              { "Hammer of Wrath", "Exorcism" }, 0, 8000, 0, "Hammer of Wrath and Exorcism",
              "A kill ends the cooldowns of Hammer of Wrath and Exorcism (once every 8 sec)." },
            { 0, 18608, "Benediction", TRIG_HEAL_CRIT, Arpg::SKILL_NONE, {}, EFFECT_DAMAGE,
              { "Exorcism", "Holy Wrath", "Holy Shock" }, 50, 8000, 10000, "Exorcism, Holy Wrath or Holy Shock",
              "A critical heal makes your next Exorcism, Holy Wrath or Holy Shock deal 50% more (once every 8 sec)." },
            { 0, 873, "Staff of Jordan", TRIG_SPELL_CRIT, Arpg::SKILL_NONE, { "Frostbolt" }, EFFECT_INSTANT,
              { "Fireball", "Pyroblast" }, 0, 8000, 10000, "Fireball or Pyroblast",
              "A critical Frostbolt lets your next Fireball or Pyroblast skip its recovery (once every 8 sec)." },
            { 0, 6220, "Meteor Shard", TRIG_SWING_CRIT, Arpg::SKILL_NONE, {}, EFFECT_FREE,
              { "Sinister Strike", "Eviscerate", "Heroic Strike", "Mortal Strike" }, 0, 8000, 8000, "finishing strike",
              "A critical swing makes your next Sinister Strike, Eviscerate, Heroic Strike or Mortal Strike cost nothing (once every 8 sec)." },

            // --- Synergies: the skill trees' synergy nodes (docs/ARPG-CHARACTER.md, "Synergies") ---
            // The mage's.
            Synergy(629, "Kindling Blast", TRIG_SPELL_CRIT, { "Fireball" }, EFFECT_DAMAGE, { "Fire Blast" }, 60, 3000, 4000,
                    "Fire Blast", "Within 4 sec of a critical Fireball, your Fire Blast deals 60% more and bursts onto every enemy within 6 yards for 50%.",
                    { 0, KIT_BURST, 2136, 0, 6.0f, 50, "", "Kindling Blast" }),
            Synergy(130, "Hot Streak", TRIG_SPELL_CRIT, { "Fire Blast", "Scorch" }, EFFECT_INSTANT, { "Pyroblast" }, 0, 8000, 8000,
                    "Pyroblast", "A critical Fire Blast or Scorch lets your next Pyroblast skip its recovery (once every 8 sec)."),
            Synergy(226, "Winter's Grasp", TRIG_LANDED, { "Frost Nova" }, EFFECT_INSTANT, { "Frostbolt" }, 0, 6000, 6000,
                    "Frostbolt", "Frost Nova lets your next Frostbolt skip its recovery, and it shatters into 3 shards at the nearest enemies within 10 yards for 50%.",
                    { 0, KIT_FRAGMENTS, 116, 3, 10.0f, 50, "", "Winter's Grasp" }),
            Synergy(327, "Arcane Echoes", TRIG_SPELL_CRIT, { "Arcane Explosion" }, EFFECT_FREE, { "Arcane Missiles" }, 0, 6000, 6000,
                    "Arcane Missiles", "A critical Arcane Explosion makes your next Arcane Missiles cost no mana, and each missile jumps to 1 more enemy within 8 yards for 50%.",
                    { 0, KIT_CHAIN, 5143, 1, 8.0f, 50, "", "Arcane Echoes" }),
            Synergy(429, "Resonance", TRIG_SPELL_CRIT, { "Arcane Missiles", "Arcane Missile" }, EFFECT_WIDE, { "Arcane Explosion" }, 50, 4000, 5000,
                    "Arcane Explosion", "A critical Arcane Missile makes your next Arcane Explosion 50% wider (once every 4 sec)."),
            Synergy(525, "Firestorm", TRIG_SPELL_CRIT, { "Fire Blast", "Scorch" }, EFFECT_WIDE, { "Blizzard", "Flamestrike" }, 50, 6000, 8000,
                    "Blizzard or Flamestrike", "A critical Fire Blast or Scorch makes your next Blizzard or Flamestrike 50% wider (once every 6 sec)."),
            // The paladin's.
            Synergy(311, "Crusader's Verdict", TRIG_SWING_CRIT, {}, EFFECT_DAMAGE, { "Judgement", "Judgement of Righteousness", "Judgement of Command" }, 50,
                    6000, 6000, "Judgement", "A critical swing makes your next Judgement deal 50% more (once every 6 sec)."),
            Synergy(406, "Hallowed Strike", TRIG_SPELL_HIT, { "Judgement", "Judgement of Righteousness", "Judgement of Command" }, EFFECT_RESET, { "Consecration" }, 0, 4000, 0,
                    "Consecration", "A Judgement that hits ends Consecration's cooldown (once every 4 sec)."),
            Synergy(511, "Wrath of the Light", TRIG_SPELL_CRIT, { "Judgement", "Judgement of Righteousness", "Judgement of Command", "Exorcism" }, EFFECT_DAMAGE, { "Hammer of Wrath" }, 60, 6000, 8000,
                    "Hammer of Wrath", "A critical Judgement or Exorcism makes your next Hammer of Wrath deal 60% more and fly through every enemy in its path for 60%.",
                    { 0, KIT_PIERCE, 24275, 0, 0.0f, 60, "", "Wrath of the Light" }),
            Synergy(610, "Righteous Thunder", TRIG_LANDED, { "Hammer of Justice" }, EFFECT_INSTANT, { "Holy Wrath" }, 0, 10000, 6000,
                    "Holy Wrath", "Stunning an enemy with Hammer of Justice makes your next Holy Wrath instant (once every 10 sec)."),
            Synergy(204, "Crusader's Grace", TRIG_SPELL_CRIT, { "Judgement", "Judgement of Righteousness", "Judgement of Command", "Exorcism", "Hammer of Wrath" }, EFFECT_INSTANT,
                    { "Holy Light", "Flash of Light" }, 0, 8000, 8000, "Holy Light or Flash of Light",
                    "A critical Judgement, Exorcism or Hammer of Wrath lets your next Holy Light or Flash of Light skip its recovery (once every 8 sec)."),
            // The druid's.
            Synergy(151, "Lunar Eclipse", TRIG_SPELL_CRIT, { "Wrath" }, EFFECT_INSTANT, { "Starfire" }, 0, 8000, 6000,
                    "Starfire", "A critical Wrath lets your next Starfire skip its recovery, and it bursts onto every enemy within 6 yards for 40% (once every 8 sec).",
                    { 0, KIT_BURST, 2912, 0, 6.0f, 40, "", "Lunar Eclipse" }),
            Synergy(243, "Moonkissed", TRIG_SPELL_CRIT, { "Wrath", "Starfire" }, EFFECT_KIT, { "Moonfire" }, 0, 6000, 6000,
                    "Moonfire", "A critical Wrath or Starfire makes your next Moonfire also land on the 3 nearest enemies within 8 yards (once every 6 sec).",
                    { 0, KIT_SPREAD, 8921, 3, 8.0f, 100, "", "Moonkissed" }),
            Synergy(343, "Gathering Storm", TRIG_SPELL_CRIT, { "Moonfire", "Starfire" }, EFFECT_FREE, { "Hurricane" }, 0, 10000, 8000,
                    "Hurricane", "A critical Moonfire or Starfire makes your next Hurricane cost no mana (once every 10 sec)."),
            Synergy(445, "Roaring Swipe", TRIG_LANDED, { "Demoralizing Roar" }, EFFECT_DAMAGE, { "Swipe" }, 60, 6000, 6000,
                    "Swipe", "Demoralizing Roar makes your next Swipe deal 60% more (once every 6 sec)."),
            Synergy(549, "Bloodied Bite", TRIG_SPELL_CRIT, { "Claw", "Shred" }, EFFECT_CRIT, { "Ferocious Bite" }, 0, 6000, 8000,
                    "Ferocious Bite", "A critical Claw or Shred makes your next Ferocious Bite a sure critical strike (once every 6 sec)."),
            Synergy(643, "Overgrowth's Gift", TRIG_LANDED, { "Entangling Roots" }, EFFECT_INSTANT, { "Regrowth", "Healing Touch" }, 0, 10000, 8000,
                    "Regrowth or Healing Touch", "Entangling Roots lets your next Regrowth or Healing Touch skip its recovery (once every 10 sec)."),

            // --- Synergy uniques, by name ---
            Worn("Mana Igniting Cord", TRIG_SPELL_CRIT, { "Fireball" }, EFFECT_RESET, { "Fire Blast" }, 0, 6000, 0,
                 "Fire Blast", "A critical Fireball ends Fire Blast's cooldown (once every 6 sec)."),
            Worn("Robe of the Archmage", TRIG_LANDED, { "Frost Nova" }, EFFECT_CRIT, { "Fire Blast" }, 0, 10000, 6000,
                 "Fire Blast", "Frost Nova makes your next Fire Blast a sure critical strike (once every 10 sec)."),
            Worn("Wolfshead Helm", TRIG_SPELL_CRIT, { "Claw", "Shred" }, EFFECT_FREE, { "Rake", "Rip" }, 0, 6000, 6000,
                 "Rake or Rip", "A critical Claw or Shred makes your next Rake or Rip cost no energy (once every 6 sec)."),
            Worn("Staff of the Shadow Flame", TRIG_SPELL_CRIT, { "Wrath" }, EFFECT_DAMAGE, { "Starfire" }, 50, 6000, 6000,
                 "Starfire", "A critical Wrath makes your next Starfire deal 50% more (once every 6 sec)."),
        };
        // Items by name keep it: their entries are found when first needed (ItemOf), as the
        // table is first read before the item templates load (a rune's text).
        return rows;
        }();
        return procs;
    }

    // A row's item entry: its own, or its name's once the item templates are loaded (0: none, or not
    // in the database, logged once).
    uint32 ItemOf(ProcDef const& def)
    {
        if (def.item || !def.itemName)
            return def.item;
        static std::mutex lock;
        static std::map<std::string, uint32> found;
        std::lock_guard<std::mutex> guard(lock);
        auto it = found.find(def.itemName);
        if (it != found.end())
            return it->second;
        if (!sItemStorage.GetMaxEntry())
            return 0;                       // not loaded yet: ask again later
        uint32 entry = 0;
        for (uint32 id = 0; id < sItemStorage.GetMaxEntry() && !entry; ++id)
            if (ItemPrototype const* proto = sItemStorage.LookupEntry<ItemPrototype>(id))
                if (proto->Name1 && std::strcmp(proto->Name1, def.itemName) == 0)
                    entry = id;
        if (!entry)
            sLog.outError("ARPG procs: no item named \"%s\"", def.itemName);
        found[def.itemName] = entry;
        return entry;
    }

    bool Named(SpellEntry const* spell, std::vector<char const*> const& names)
    {
        if (names.empty())
            return true;
        char const* name = spell ? spell->SpellName[0] : nullptr;
        if (!name)
            return false;
        for (char const* want : names)
            if (!std::strcmp(want, "*") || !std::strcmp(name, want))
                return true;
        return false;
    }

    // The family flags of a class's spells named `names` ("*": every spell of the family).
    uint64 MaskOf(uint32 family, std::vector<char const*> const& names)
    {
        static std::mutex lock;
        static std::map<std::pair<uint32, std::string>, uint64> cache;
        uint64 mask = 0;
        for (char const* want : names)
        {
            if (!std::strcmp(want, "*"))
                return ~uint64(0);
            std::lock_guard<std::mutex> guard(lock);
            auto const key = std::make_pair(family, std::string(want));
            auto it = cache.find(key);
            if (it == cache.end())
            {
                uint64 m = 0;
                for (uint32 id = 1; id < sSpellTemplate.GetMaxEntry(); ++id)
                    if (SpellEntry const* s = sSpellTemplate.LookupEntry<SpellEntry>(id))
                        if (s->SpellFamilyName == family && s->SpellName[0] && !std::strcmp(s->SpellName[0], want))
                            m |= s->SpellFamilyFlags.Flags;
                it = cache.emplace(key, m).first;
            }
            mask |= it->second;
        }
        return mask;
    }

    // The spells of `family` (and generic ones: Exorcism, Holy Wrath, Judgement of Command's hit
    // carry no class family) named exactly `names`, by id, for a readied modifier's reach; and the
    // family bits only they carry, for the client. Classic shares its bits between unrelated
    // spells (every mage bolt has one), so the bits alone would ready the wrong spells too.
    std::pair<uint64, std::shared_ptr<std::set<uint32> const>> SpellsNamed(uint32 family, std::vector<char const*> const& names)
    {
        static std::mutex lock;
        static std::map<std::string, std::pair<uint64, std::shared_ptr<std::set<uint32> const>>> cache;
        std::string key = std::to_string(family);
        for (char const* want : names)
            key += std::string("|") + want;
        std::lock_guard<std::mutex> guard(lock);
        auto it = cache.find(key);
        if (it != cache.end())
            return it->second;
        auto ids = std::make_shared<std::set<uint32>>();
        uint64 mine = 0, others = 0;
        for (uint32 id = 1; id < sSpellTemplate.GetMaxEntry(); ++id)
        {
            SpellEntry const* s = sSpellTemplate.LookupEntry<SpellEntry>(id);
            if (!s || !s->SpellName[0])
                continue;
            bool const own = s->SpellFamilyName == family;
            if (!own && s->SpellFamilyName != SPELLFAMILY_GENERIC)
                continue;
            bool named = false;
            for (char const* want : names)
                named = named || !std::strcmp(s->SpellName[0], want);
            if (named)
            {
                ids->insert(id);
                if (own)
                    mine |= s->SpellFamilyFlags.Flags;
            }
            else if (own)
                others |= s->SpellFamilyFlags.Flags;
        }
        auto out = std::make_pair(mine & ~others, ids->empty() ? nullptr : std::shared_ptr<std::set<uint32> const>(ids));
        cache.emplace(key, out);
        return out;
    }

    // A spell of the player's class family, to own its modifiers (Player::ApplySpellMod and the
    // client match a modifier to spells of its owner's family).
    uint32 FamilyOwner(Player const* player)
    {
        for (auto const& [id, spell] : player->GetSpellMap())
        {
            if (spell.state == PLAYERSPELL_REMOVED)
                continue;
            if (SpellEntry const* entry = sSpellTemplate.LookupEntry<SpellEntry>(id))
                if (entry->SpellFamilyName && entry->SpellFamilyName == uint32(player->GetSpellClass()))
                    return id;
        }
        return 0;
    }

    struct Readied
    {
        size_t proc;                          // its row in Procs()
        SpellModifier* mod;                   // nullptr: a kit row alone, or the mod spent already
        uint32 expires;
        Spell const* spentBy = nullptr;       // compared only, never followed
        uint32 spentAt = 0;
    };

    struct PlayerProcs
    {
        std::unordered_map<size_t, uint32> readyAt;   // by row: when it may fire again
        std::vector<Readied> readied;
        std::unordered_map<uint32, uint32> powerReadyAt; // a unique's generated power, by item entry
        uint32 furyUntil = 0;                         // a power's more damage, and until when
        int32 furyPct = 0;
        uint32 swiftUntil = 0;                        // a power's movement speed
        int32 swiftPct = 0;
    };

    std::mutex sLock;
    std::unordered_map<ObjectGuid, PlayerProcs> sProcs;

    bool Later(uint32 a, uint32 b) { return int32(a - b) > 0; }

    bool Wears(Player const* player, uint32 item)
    {
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (Item* it = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                if (it->GetEntry() == item && !it->IsBroken())
                    return true;
        return false;
    }

    // An authored unique (a mechanic of Arpg/ArpgUniques.h, or an item row above) keeps its own.
    bool Authored(uint32 item)
    {
        for (UniqueMechanic const& u : Uniques())
            if (u.item == item)
                return true;
        for (ProcDef const& def : Procs())
            if (ItemOf(def) == item && item)
                return true;
        return false;
    }

    // --- Unique powers: every named unique not authored has one of its own ---
    //
    // A power is a trigger, an effect (of a damage type, for the damaging ones), and a smaller
    // second effect, the rider, with numbers that grow with the item's level. The combinations a
    // kind of item may carry (weapons, armour, jewellery) are dealt out over its uniques in entry
    // order, a coprime stride apart, so no two uniques of a kind share one while there are
    // combinations to go round (about 1,500 a kind).

    enum PowerEffect : uint8
    {
        PE_NOVA,              // `value`% of the basis to every enemy around the player
        PE_ARC,               // ... to the three enemies nearest the struck one
        PE_EXPLODE,           // the slain enemy bursts: `value`% of its maximum health around it
        PE_AILMENTS,          // the element's ailment on every enemy around, as a hit of `value`%
        PE_HEAL,              // `value`% of maximum health
        PE_MANA,              // `value`% of maximum mana
        PE_FURY,              // `value`% more damage for FURY_MS
        PE_SWIFT,             // `value`% movement speed for SWIFT_MS
        PE_COUNT,
    };
    // Riders: none, heal, mana, fury, swiftness, or an ailment (RIDER_AILMENT + damage tag) on the
    // target.
    enum PowerRider : uint8 { RIDER_NONE, RIDER_HEAL, RIDER_MANA, RIDER_FURY, RIDER_SWIFT, RIDER_AILMENT };
    constexpr uint8 POWER_ELEMENTS = 7;
    constexpr uint8 RIDER_COUNT = RIDER_AILMENT + POWER_ELEMENTS;

    constexpr uint32 FURY_MS = 6000, SWIFT_MS = 4000, RIDER_FURY_MS = 4000, RIDER_SWIFT_MS = 3000;
    constexpr float POWER_RANGE = 8.0f, POWER_ARC_RANGE = 10.0f;
    constexpr size_t POWER_ARC_COUNT = 3, POWER_MAX_FOES = 10;

    struct UniquePower
    {
        Trigger trigger = TRIG_KILL;
        PowerEffect effect = PE_HEAL;
        uint8 element = 0;        // damage tag (TAG_PHYSICAL .. TAG_SHADOW)
        int32 value = 0;
        uint8 rider = RIDER_NONE;
        int32 riderValue = 0;
        uint32 icd = 0;
        std::string name;
        std::string text;
    };

    bool Damaging(PowerEffect effect) { return effect <= PE_AILMENTS; }

    // The item level's weight on a power's numbers: half at level 1 to all at 60, then a quarter
    // more every ten levels (Naxxramas about 1.7).
    float PowerLevel(uint32 itemLevel)
    {
        float const to60 = 0.5f + 0.5f * float(std::min<uint32>(itemLevel, 60)) / 60.0f;
        return itemLevel > 60 ? to60 * (1.0f + float(itemLevel - 60) / 40.0f) : to60;
    }

    // An effect's number at level 60, by what sets it off: a hit's or heal's share, a blow's, or a
    // share of the slain enemy's maximum health.
    int32 BaseValue(PowerEffect effect, Trigger trigger)
    {
        bool const kill = trigger == TRIG_KILL, struck = trigger == TRIG_STRUCK, heal = trigger == TRIG_HEAL_CRIT;
        switch (effect)
        {
            case PE_NOVA:     return kill ? 8 : struck ? 50 : heal ? 60 : 30;
            case PE_ARC:      return kill ? 10 : struck ? 60 : heal ? 70 : 35;
            case PE_EXPLODE:  return 12;
            case PE_AILMENTS: return kill ? 15 : struck ? 80 : heal ? 100 : 60;
            case PE_HEAL:     return struck ? 12 : 4;
            case PE_MANA:     return struck ? 8 : 4;
            case PE_FURY:     return struck ? 25 : 15;
            case PE_SWIFT:    return 20;
            default:          return 0;
        }
    }

    int32 RiderBase(uint8 rider)
    {
        switch (rider)
        {
            case RIDER_HEAL: case RIDER_MANA: return 2;
            case RIDER_FURY:  return 8;
            case RIDER_SWIFT: return 12;
            default:          return rider >= RIDER_AILMENT ? 40 : 0;
        }
    }

    uint32 PowerIcd(Trigger trigger, PowerEffect effect)
    {
        uint32 icd;
        switch (trigger)
        {
            case TRIG_SWING_CRIT: icd = 2000; break;
            case TRIG_SPELL_CRIT: icd = 3000; break;
            case TRIG_KILL:       icd = 1000; break;
            case TRIG_STRUCK:     icd = 8000; break;
            case TRIG_HEAL_CRIT:  icd = 5000; break;
            default:              icd = 3000; break;
        }
        // The buffs last a while: no sooner than they run out.
        if (effect == PE_FURY)
            icd = std::max(icd, FURY_MS + 2000);
        if (effect == PE_SWIFT)
            icd = std::max(icd, SWIFT_MS + 2000);
        return icd;
    }

    char const* const ELEMENT_WORDS[POWER_ELEMENTS] = { "force", "holy light", "fire", "frost", "arcane energy", "lightning", "shadow" };
    char const* const AILMENT_VERBS[POWER_ELEMENTS] = { "bleeds", "judges", "ignites", "chills", "overloads", "poisons", "withers" };
    char const* const ELEMENT_NAMES[POWER_ELEMENTS][3] =
    {
        { "Iron", "Blade", "Bone" }, { "Dawn", "Radiant", "Sacred" }, { "Ember", "Cinder", "Blaze" },
        { "Rime", "Winter", "Glacial" }, { "Starfall", "Mana", "Arcane" }, { "Storm", "Thunder", "Venom" },
        { "Dusk", "Grave", "Shadow" },
    };
    char const* const EFFECT_NOUNS[PE_COUNT][3] =
    {
        { "Nova", "Burst", "Flare" }, { "Arc", "Fork", "Lash" }, { "Pyre", "Rupture", "Detonation" },
        { "Brand", "Plague", "Curse" }, { "Lifebloom", "Second Wind", "Bloodrush" }, { "Wellspring", "Font", "Insight" },
        { "Fury", "Wrath", "Frenzy" }, { "Swiftness", "Haste", "Wind" },
    };
    char const* const UTILITY_PREFIX[3] = { "", "Greater ", "True " };

    std::string Printf(char const* format, ...) ATTR_PRINTF(1, 2);
    std::string Printf(char const* format, ...)
    {
        char buf[512];
        va_list args;
        va_start(args, format);
        std::vsnprintf(buf, sizeof(buf), format, args);
        va_end(args);
        return buf;
    }

    // The power's line, as its tooltip shows it.
    // The state a power's element feeds on (Arpg/ArpgStates.h): its own ailment's, so every damaging
    // power pairs with whatever sets that ailment up.
    State FedState(uint8 element)
    {
        switch (element)
        {
            case TAG_PHYSICAL: return STATE_BLEEDING;
            case TAG_HOLY:     return STATE_JUDGED;
            case TAG_FIRE:     return STATE_IGNITED;
            case TAG_FROST:    return STATE_CHILLED;
            case TAG_ARCANE:   return STATE_OVERLOADED;
            case TAG_NATURE:   return STATE_POISONED;
            default:           return STATE_WITHERED;
        }
    }
    constexpr uint32 FED_PCT = 50;

    std::string PowerText(UniquePower const& p)
    {
        char const* when = "";
        char const* basis = "the hit";
        switch (p.trigger)
        {
            case TRIG_SWING_CRIT: when = "A critical swing"; break;
            case TRIG_SPELL_CRIT: when = "A critical spell"; break;
            case TRIG_KILL:       when = "A kill"; break;
            case TRIG_STRUCK:     when = "A heavy blow (a fifth of your health)"; basis = "the blow"; break;
            case TRIG_HEAL_CRIT:  when = "A critical heal"; basis = "the heal"; break;
            default: break;
        }
        bool const kill = p.trigger == TRIG_KILL;
        char const* element = ELEMENT_WORDS[p.element];
        std::string what;
        switch (p.effect)
        {
            case PE_NOVA:
                what = kill ? Printf("sends out a nova of %s: %d%% of the slain enemy's maximum health to every enemy within 8 yards", element, p.value)
                            : Printf("sends out a nova of %s: %d%% of %s to every enemy within 8 yards", element, p.value, basis);
                break;
            case PE_ARC:
                what = kill ? Printf("arcs %s from the slain enemy to the 3 nearest for %d%% of its maximum health", element, p.value)
                            : Printf("arcs %s to the 3 enemies nearest the target for %d%% of %s", element, p.value, basis);
                break;
            case PE_EXPLODE:
                what = Printf("makes the slain enemy explode in %s for %d%% of its maximum health to enemies within 8 yards", element, p.value);
                break;
            case PE_AILMENTS:
                what = kill ? Printf("%s every enemy within 8 yards of the slain one, as a hit of %d%% of its maximum health", AILMENT_VERBS[p.element], p.value)
                            : Printf("%s every enemy within 8 yards, as a hit of %d%% of %s", AILMENT_VERBS[p.element], p.value, basis);
                break;
            case PE_HEAL:  what = Printf("heals you for %d%% of your maximum health", p.value); break;
            case PE_MANA:  what = Printf("restores %d%% of your maximum mana", p.value); break;
            case PE_FURY:  what = Printf("gives you %d%% more damage for %u sec", p.value, FURY_MS / 1000); break;
            case PE_SWIFT: what = Printf("gives you %d%% movement speed for %u sec", p.value, SWIFT_MS / 1000); break;
            default: break;
        }
        std::string rider;
        switch (p.rider)
        {
            case RIDER_NONE: break;
            case RIDER_HEAL:  rider = Printf(", and heals you for %d%% of your maximum health", p.riderValue); break;
            case RIDER_MANA:  rider = Printf(", and restores %d%% of your maximum mana", p.riderValue); break;
            case RIDER_FURY:  rider = Printf(", and gives you %d%% more damage for %u sec", p.riderValue, RIDER_FURY_MS / 1000); break;
            case RIDER_SWIFT: rider = Printf(", and %d%% movement speed for %u sec", p.riderValue, RIDER_SWIFT_MS / 1000); break;
            default:
                rider = Printf(", and %s the %s as a hit of %d%% of %s", AILMENT_VERBS[p.rider - RIDER_AILMENT],
                               p.trigger == TRIG_STRUCK ? "attacker" : kill ? "nearest enemy" : "target", p.riderValue,
                               kill ? "its maximum health" : basis);
                break;
        }
        return Printf("%s %s%s (once every %u sec).", when, what.c_str(), rider.c_str(), std::max<uint32>(1, p.icd / 1000));
    }

    std::string PowerName(UniquePower const& p)
    {
        uint8 const pick = uint8(p.rider % 3), noun = uint8(uint8(p.trigger) % 3);
        if (Damaging(p.effect))
            return std::string(ELEMENT_NAMES[p.element][pick]) + " " + EFFECT_NOUNS[p.effect][noun];
        return std::string(UTILITY_PREFIX[pick]) + EFFECT_NOUNS[p.effect][noun];
    }

    enum PowerKind : uint8 { KIND_WEAPON, KIND_ARMOUR, KIND_JEWEL, KIND_COUNT };

    // A named blue or purple weapon or armour piece (not a shirt, tabard, bag or ammunition) that
    // is not authored: it carries a generated power.
    bool TakesPower(ItemPrototype const* proto, PowerKind& kind)
    {
        if (!proto || proto->Quality < ITEM_QUALITY_RARE || proto->Quality > ITEM_QUALITY_EPIC ||
                (proto->Class != ITEM_CLASS_WEAPON && proto->Class != ITEM_CLASS_ARMOR))
            return false;
        switch (proto->InventoryType)
        {
            case INVTYPE_NON_EQUIP: case INVTYPE_BODY: case INVTYPE_TABARD: case INVTYPE_BAG: case INVTYPE_QUIVER:
            case INVTYPE_AMMO:
                return false;
            default:
                break;
        }
        uint32 const type = proto->InventoryType;
        kind = proto->Class == ITEM_CLASS_WEAPON ? KIND_WEAPON
               : (type == INVTYPE_NECK || type == INVTYPE_FINGER || type == INVTYPE_TRINKET || type == INVTYPE_CLOAK ||
                  type == INVTYPE_HOLDABLE) ? KIND_JEWEL : KIND_ARMOUR;
        return !Authored(proto->ItemId);
    }

    // A combination a kind's uniques are dealt: its numbers come with the item.
    struct Combo
    {
        Trigger trigger;
        PowerEffect effect;
        uint8 element;
        uint8 rider;
    };

    std::vector<Combo> CombosFor(PowerKind kind)
    {
        // Every kind takes every trigger (about 1,500 combinations a kind, more than vanilla has
        // uniques of any kind); the order only moves which entries draw which.
        std::vector<Trigger> triggers;
        switch (kind)
        {
            case KIND_WEAPON: triggers = { TRIG_SWING_CRIT, TRIG_SPELL_CRIT, TRIG_KILL, TRIG_STRUCK, TRIG_HEAL_CRIT }; break;
            case KIND_ARMOUR: triggers = { TRIG_STRUCK, TRIG_KILL, TRIG_HEAL_CRIT, TRIG_SWING_CRIT, TRIG_SPELL_CRIT }; break;
            default:          triggers = { TRIG_SPELL_CRIT, TRIG_KILL, TRIG_HEAL_CRIT, TRIG_STRUCK, TRIG_SWING_CRIT }; break;
        }
        std::vector<Combo> out;
        for (Trigger trigger : triggers)
            for (uint8 e = 0; e < PE_COUNT; ++e)
            {
                PowerEffect const effect = PowerEffect(e);
                if (effect == PE_EXPLODE && trigger != TRIG_KILL)
                    continue;
                uint8 const elements = Damaging(effect) ? POWER_ELEMENTS : 1;
                for (uint8 element = 0; element < elements; ++element)
                    for (uint8 rider = 0; rider < RIDER_COUNT; ++rider)
                    {
                        // A rider is something else than the power itself.
                        if ((rider == RIDER_HEAL && effect == PE_HEAL) || (rider == RIDER_MANA && effect == PE_MANA) ||
                                (rider == RIDER_FURY && effect == PE_FURY) || (rider == RIDER_SWIFT && effect == PE_SWIFT) ||
                                (effect == PE_AILMENTS && rider == RIDER_AILMENT + element))
                            continue;
                        out.push_back({ trigger, effect, element, rider });
                    }
            }
        return out;
    }

    std::unordered_map<uint32, UniquePower> const& GeneratedPowers()
    {
        static std::unordered_map<uint32, UniquePower> const powers = []()
        {
            std::unordered_map<uint32, UniquePower> out;
            std::vector<uint32> uniques[KIND_COUNT];
            for (uint32 id = 1; id < sItemStorage.GetMaxEntry(); ++id)
            {
                PowerKind kind;
                if (TakesPower(sItemStorage.LookupEntry<ItemPrototype>(id), kind))
                    uniques[kind].push_back(id);
            }
            for (uint8 k = 0; k < KIND_COUNT; ++k)
            {
                std::vector<Combo> const combos = CombosFor(PowerKind(k));
                size_t const n = combos.size();
                if (!n)
                    continue;
                // A stride coprime with the count walks every combination once before any comes
                // back, and keeps neighbouring entries (a set's pieces) apart.
                size_t stride = 7919 % n;
                while (stride < 2 || std::gcd(stride, n) != 1)
                    stride = (stride + 1) % n;
                for (size_t i = 0; i < uniques[k].size(); ++i)
                {
                    uint32 const entry = uniques[k][i];
                    Combo const& c = combos[(i * stride + 17) % n];
                    ItemPrototype const* proto = sItemStorage.LookupEntry<ItemPrototype>(entry);
                    float const level = PowerLevel(proto->ItemLevel);
                    UniquePower p;
                    p.trigger = c.trigger;
                    p.effect = c.effect;
                    p.element = c.element;
                    p.value = std::max<int32>(1, int32(float(BaseValue(c.effect, c.trigger)) * level + 0.5f));
                    p.rider = c.rider;
                    p.riderValue = c.rider ? std::max<int32>(1, int32(float(RiderBase(c.rider)) * level + 0.5f)) : 0;
                    p.icd = PowerIcd(c.trigger, c.effect);
                    p.name = PowerName(p);
                    p.text = PowerText(p);
                    // Not a frost ailment power: a Chill has no size to grow.
                    if (Damaging(p.effect) && !(p.effect == PE_AILMENTS && p.element == TAG_FROST))
                        p.text += std::string(" It deals ") + std::to_string(FED_PCT) + "% more to " + StateName(FedState(p.element)) +
                                  " enemies.";
                    out.emplace(entry, std::move(p));
                }
                sLog.outString("ARPG uniques: %u %s uniques dealt their own powers from %u combinations", uint32(uniques[k].size()),
                               k == KIND_WEAPON ? "weapon" : k == KIND_ARMOUR ? "armour" : "jewellery", uint32(n));
            }
            return out;
        }();
        return powers;
    }

    UniquePower const* PowerOf(uint32 item)
    {
        std::unordered_map<uint32, UniquePower> const& powers = GeneratedPowers();
        auto it = powers.find(item);
        return it == powers.end() ? nullptr : &it->second;
    }

    using Sockets = std::vector<std::pair<SkillId, uint8>>;

    bool Socketed(Sockets const& sockets, SkillId skill, uint8 rune)
    {
        for (auto const& [s, r] : sockets)
            if (r == rune && (skill == Arpg::SKILL_NONE || s == skill))
                return true;
        return false;
    }

    // Whether `player` has proc row `row`: the rune socketed in a skill, or the item worn.
    bool Has(Player const* player, size_t row, Sockets const& sockets)
    {
        ProcDef const& def = Procs()[row];
        if (def.node)
            return SkillNodeRank(player, def.node) > 0;
        if (uint32 const item = ItemOf(def))
            return Wears(player, item);
        if (def.itemName)
            return false;
        return Socketed(sockets, Arpg::SKILL_NONE, def.rune);
    }

    // Whether `spell` (nullptr: the swing) sets off `def` for `player`.
    bool Fires(Player const* player, ProcDef const& def, SpellEntry const* spell, Sockets const& sockets)
    {
        if (!def.from.empty() && !Named(spell, def.from))
            return false;
        if (def.rune)
        {
            // A rune fires from the skill it is socketed in (Strike: the swing).
            SkillId const skill = SkillOfSpell(player->getClass(), spell);
            if (skill == Arpg::SKILL_NONE || !Socketed(sockets, skill, def.rune))
                return false;
            return def.skill == Arpg::SKILL_NONE || def.skill == skill;
        }
        if (def.skill != Arpg::SKILL_NONE)
            return SkillOfSpell(player->getClass(), spell) == def.skill;
        return true;
    }

    // The effects done at once (heal, mana, damage): outside the lock, as a kill they cause trips
    // procs again (its row's cooldown, already set, stops a chain).
    // `flat`: `amount` is a blow taken or a heal, vanilla-sized: it hits each creature at the ARPG's
    // size (Arpg/ArpgDamage.h); else it is a hit's damage, sized already.
    void Act(Player* player, ProcDef const& def, Unit* victim, uint32 amount, bool flat)
    {
        auto foes = [&](WorldObject* centre, float range, Unit const* skip)
        {
            std::vector<Unit*> out;
            for (Unit* unit : EnemiesNear(centre, range))
                if (unit != skip && unit->IsAlive() && MayCatchUnit(player, unit, nullptr))
                    out.push_back(unit);
            return out;
        };
        auto strike = [&](std::vector<Unit*> const& units, uint32 look, uint32 school, uint32 dealt, char const* verb, bool size)
        {
            SpellEntry const* spell = sSpellTemplate.LookupEntry<SpellEntry>(look);
            if (!spell || units.empty() || !dealt)
                return;
            for (Unit* unit : units)
                StrikeFoe(player, unit, spell, school, size ? ToDamage(player, unit, dealt) : dealt);
            ReportProc(player, PROC_DAMAGE, 0, def.name, "%s %u %s for %u.", verb, uint32(units.size()),
                       units.size() == 1 ? "enemy" : "enemies", size ? ToDamage(player, nullptr, dealt) : dealt);
        };
        switch (def.effect)
        {
            case EFFECT_HEAL:
                ProcHeal(player, def.name, std::max<uint32>(1, player->GetMaxHealth() * uint32(def.value) / 100));
                break;
            case EFFECT_MANA:
                ProcMana(player, def.name, std::max<uint32>(1, player->GetMaxPower(POWER_MANA) * uint32(def.value) / 100));
                break;
            case EFFECT_EXPLODE:
            {
                if (!victim)
                    break;
                uint32 const cap = ToDamage(player, victim, player->GetMaxHealth() * EXPLODE_CAP_PCT / 100);
                uint32 const dealt = std::min(cap, victim->GetMaxHealth() * uint32(def.value) / 100);
                strike(foes(victim, EXPLODE_RANGE, victim), EXPLODE_LOOK, SPELL_SCHOOL_MASK_FIRE, dealt, "bursts onto", false);
                break;
            }
            case EFFECT_ARC:
            {
                if (!victim)
                    break;
                std::vector<Unit*> closest = foes(victim, ARC_RANGE, victim);
                std::sort(closest.begin(), closest.end(), [victim](Unit* a, Unit* b) { return victim->GetDistance(a) < victim->GetDistance(b); });
                if (closest.size() > ARC_COUNT)
                    closest.resize(ARC_COUNT);
                strike(closest, ARC_LOOK, SPELL_SCHOOL_MASK_NATURE, amount * uint32(def.value) / 100, "arcs to", flat);
                break;
            }
            case EFFECT_NOVA:
                strike(foes(player, NOVA_RANGE, nullptr), NOVA_LOOK, SPELL_SCHOOL_MASK_HOLY, amount * uint32(def.value) / 100, "strikes", flat);
                break;
            default:
                break;
        }
    }

    bool Readies(Effect effect)
    {
        return effect == EFFECT_INSTANT || effect == EFFECT_DAMAGE || effect == EFFECT_CRIT || effect == EFFECT_FREE ||
               effect == EFFECT_WIDE || effect == EFFECT_KIT;
    }

    // A readying effect, or a cooldown reset; the caller holds sLock.
    void Fire(Player* player, size_t row, uint32 now)
    {
        ProcDef const& def = Procs()[row];
        uint32 const family = uint32(player->GetSpellClass());
        if (def.effect == EFFECT_RESET)
        {
            uint32 ended = 0;
            for (auto const& [id, spell] : player->GetSpellMap())
            {
                if (spell.state == PLAYERSPELL_REMOVED || !spell.active)
                    continue;
                SpellEntry const* entry = sSpellTemplate.LookupEntry<SpellEntry>(id);
                if (!entry || (entry->SpellFamilyName != family && entry->SpellFamilyName != SPELLFAMILY_GENERIC) ||
                        !Named(entry, def.targets) || player->IsSpellReady(*entry))
                    continue;
                player->RemoveSpellCooldown(*entry, true);
                ++ended;
            }
            if (ended)
                ReportProc(player, PROC_EFFECT, 0, def.name, "ends the cooldown of %s.", def.targetWords);
            return;
        }
        if (def.effect == EFFECT_KIT)
        {
            PlayerProcs& pp = sProcs[player->GetObjectGuid()];
            for (Readied& r : pp.readied)
                if (r.proc == row && !r.spentBy)
                {
                    r.expires = now + def.lasts;
                    return;
                }
            pp.readied.push_back({ row, nullptr, now + def.lasts });
            ReportProc(player, PROC_EFFECT, 0, def.name, "readies your next %s.", def.targetWords);
            return;
        }
        uint32 const owner = FamilyOwner(player);
        // "*" reaches every spell of the family by its bits; named targets by their ids.
        bool const every = std::find_if(def.targets.begin(), def.targets.end(),
                                        [](char const* t) { return !std::strcmp(t, "*"); }) != def.targets.end();
        auto const named = every ? std::make_pair(uint64(0), std::shared_ptr<std::set<uint32> const>())
                                 : SpellsNamed(family, def.targets);
        uint64 const mask = every ? MaskOf(family, def.targets) : named.first;
        if (!owner || (every ? !mask : !named.second))
            return;
        PlayerProcs& pp = sProcs[player->GetObjectGuid()];
        // Already readied and unspent: it waits longer, but does not stack.
        for (Readied& r : pp.readied)
            if (r.proc == row && !r.spentBy)
            {
                r.expires = now + def.lasts;
                return;
            }
        SpellModifier* mod = nullptr;
        char const* words = "";
        switch (def.effect)
        {
            case EFFECT_INSTANT:
                mod = new SpellModifier(SPELLMOD_CASTING_TIME, SPELLMOD_PCT, -100, owner, mask);
                words = "swift";
                break;
            case EFFECT_DAMAGE:
                mod = new SpellModifier(SPELLMOD_DAMAGE, SPELLMOD_PCT, def.value, owner, mask);
                words = "empowered";
                break;
            case EFFECT_CRIT:
                mod = new SpellModifier(SPELLMOD_CRITICAL_CHANCE, SPELLMOD_FLAT, 100, owner, mask);
                words = "a sure critical strike";
                break;
            case EFFECT_FREE:
                mod = new SpellModifier(SPELLMOD_COST, SPELLMOD_PCT, -100, owner, mask);
                words = "free";
                break;
            case EFFECT_WIDE:
                mod = new SpellModifier(SPELLMOD_RADIUS, SPELLMOD_PCT, def.value, owner, mask);
                words = "wider";
                break;
            default:
                return;
        }
        mod->arpgSpells = named.second;
        player->AddSpellMod(mod, true);
        pp.readied.push_back({ row, mod, now + def.lasts });
        if (def.effect == EFFECT_DAMAGE)
            ReportProc(player, PROC_EFFECT, 0, def.name, "readies your next %s: +%d%% damage.", def.targetWords, int(def.value));
        else
            ReportProc(player, PROC_EFFECT, 0, def.name, "readies your next %s: %s.", def.targetWords, words);
    }

    // The trigger a generated power answers to for a wearer of `classId`: a critical heal on a
    // class with no heals is its spell crit (a mage, a warlock) or its swing crit; a spell crit on
    // a class with no damage spells to speak of is its swing crit. So no power is dead weight.
    Trigger TriggerFor(Trigger trigger, uint8 classId)
    {
        bool const heals = classId == CLASS_PALADIN || classId == CLASS_PRIEST || classId == CLASS_DRUID || classId == CLASS_SHAMAN;
        bool const casts = classId != CLASS_WARRIOR && classId != CLASS_ROGUE;
        if (trigger == TRIG_HEAL_CRIT && !heals)
            return casts && classId != CLASS_HUNTER ? TRIG_SPELL_CRIT : TRIG_SWING_CRIT;
        if (trigger == TRIG_SPELL_CRIT && !casts)
            return TRIG_SWING_CRIT;
        return trigger;
    }

    // The generated powers of the uniques `player` wears (not broken), with their items' entries.
    std::vector<std::pair<uint32, UniquePower const*>> WornGenerated(Player const* player)
    {
        std::vector<std::pair<uint32, UniquePower const*>> out;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (Item* it = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                if (!it->IsBroken())
                    if (UniquePower const* power = PowerOf(it->GetEntry()))
                        out.emplace_back(it->GetEntry(), power);
        return out;
    }

    // A generated power goes off: `victim` is the struck, slain or striking unit, `amount` the hit,
    // heal or blow (0 for a kill, whose numbers come from the slain's maximum health).
    void PowerAct(Player* player, UniquePower const& p, Unit* victim, uint32 amount, Trigger fired)
    {
        bool const kill = p.trigger == TRIG_KILL;
        // A blow or a heal is vanilla-sized: sized for each creature it hits. A hit is sized
        // already; a share of a creature's health is too, held under a share of the player's own.
        // The trigger that fired (a power answers to its wearer's, TriggerFor) says what size the
        // amount is.
        bool const flat = fired == TRIG_STRUCK || fired == TRIG_HEAL_CRIT;
        auto basisOf = [&](int32 pct) -> uint32
        {
            if (kill)
            {
                if (!victim)
                    return 0;
                uint32 const cap = ToDamage(player, victim, player->GetMaxHealth() * EXPLODE_CAP_PCT / 100);
                return std::min(cap, uint32(uint64(victim->GetMaxHealth()) * uint32(pct) / 100));
            }
            return uint32(uint64(amount) * uint32(pct) / 100);
        };
        auto sized = [&](Unit* unit, uint32 dealt) { return flat ? ToDamage(player, unit, dealt) : dealt; };
        auto foesNear = [&](WorldObject* centre, float range, Unit const* skip)
        {
            std::vector<Unit*> out;
            for (Unit* unit : EnemiesNear(centre, range))
                if (unit != skip && unit->IsAlive() && MayCatchUnit(player, unit, nullptr) && out.size() < POWER_MAX_FOES)
                    out.push_back(unit);
            return out;
        };
        float const area = SpellAreaScale(player);
        SpellEntry const* look = sSpellTemplate.LookupEntry<SpellEntry>(LookOfTag(p.element));
        uint32 const school = SchoolOfTag(p.element);
        uint32 const now = WorldTimer::getMSTime();
        auto grant = [&](bool fury, int32 pct, uint32 ms)
        {
            {
                std::lock_guard<std::mutex> guard(sLock);
                PlayerProcs& pp = sProcs[player->GetObjectGuid()];
                if (fury)
                {
                    pp.furyPct = Later(pp.furyUntil, now) ? std::max(pp.furyPct, pct) : pct;
                    pp.furyUntil = now + ms;
                }
                else
                {
                    pp.swiftPct = Later(pp.swiftUntil, now) ? std::max(pp.swiftPct, pct) : pct;
                    pp.swiftUntil = now + ms;
                }
            }
            if (!fury)
                player->UpdateSpeed(MOVE_RUN, true);
        };

        uint32 const basis = basisOf(p.value);
        switch (p.effect)
        {
            case PE_NOVA:
            case PE_ARC:
            case PE_EXPLODE:
            case PE_AILMENTS:
            {
                if (!basis || !look)
                    break;
                std::vector<Unit*> foes;
                if (p.effect == PE_NOVA)
                    foes = foesNear(victim ? static_cast<WorldObject*>(victim) : player, POWER_RANGE * area, nullptr);
                else if (p.effect == PE_ARC && victim)
                {
                    foes = foesNear(victim, POWER_ARC_RANGE, victim);
                    std::sort(foes.begin(), foes.end(), [victim](Unit* a, Unit* b) { return victim->GetDistance(a) < victim->GetDistance(b); });
                    if (foes.size() > POWER_ARC_COUNT)
                        foes.resize(POWER_ARC_COUNT);
                }
                else if (p.effect == PE_EXPLODE && victim)
                    foes = foesNear(victim, POWER_RANGE * area, victim);
                else if (p.effect == PE_AILMENTS)
                    foes = foesNear(victim ? static_cast<WorldObject*>(victim) : player, POWER_RANGE * area, nullptr);
                State const fed = FedState(p.element);
                for (Unit* unit : foes)
                {
                    // More on an enemy its element's state is already on (Arpg/ArpgStates.h).
                    uint32 dealt = sized(unit, basis);
                    if (StateMask(unit) & StateBit(fed))
                        dealt = dealt * (100 + FED_PCT) / 100;
                    if (p.effect == PE_AILMENTS)
                        InflictAilment(player, unit, p.element, dealt, false);
                    else
                        StrikeFoe(player, unit, look, school, std::max<uint32>(1, dealt));
                }
                if (!foes.empty())
                    ReportProc(player, PROC_DAMAGE, 0, p.name.c_str(), "strikes %u %s.", uint32(foes.size()),
                               foes.size() == 1 ? "enemy" : "enemies");
                break;
            }
            case PE_HEAL:
                ProcHeal(player, p.name.c_str(), std::max<uint32>(1, player->GetMaxHealth() * uint32(p.value) / 100));
                break;
            case PE_MANA:
                ProcMana(player, p.name.c_str(), std::max<uint32>(1, player->GetMaxPower(POWER_MANA) * uint32(p.value) / 100));
                break;
            case PE_FURY:
                grant(true, p.value, FURY_MS);
                ReportProc(player, PROC_EFFECT, 0, p.name.c_str(), "%d%% more damage for %u sec.", p.value, FURY_MS / 1000);
                break;
            case PE_SWIFT:
                grant(false, p.value, SWIFT_MS);
                ReportProc(player, PROC_EFFECT, 0, p.name.c_str(), "%d%% movement speed for %u sec.", p.value, SWIFT_MS / 1000);
                break;
            default:
                break;
        }

        // The rider.
        switch (p.rider)
        {
            case RIDER_NONE:
                break;
            case RIDER_HEAL:
                ProcHeal(player, p.name.c_str(), std::max<uint32>(1, player->GetMaxHealth() * uint32(p.riderValue) / 100));
                break;
            case RIDER_MANA:
                ProcMana(player, p.name.c_str(), std::max<uint32>(1, player->GetMaxPower(POWER_MANA) * uint32(p.riderValue) / 100));
                break;
            case RIDER_FURY:
                grant(true, p.riderValue, RIDER_FURY_MS);
                break;
            case RIDER_SWIFT:
                grant(false, p.riderValue, RIDER_SWIFT_MS);
                break;
            default:
            {
                // The target: the struck one, the attacker, or for a kill the nearest enemy left.
                Unit* target = victim && victim->IsAlive() ? victim : nullptr;
                if (!target && victim)
                {
                    std::vector<Unit*> around = foesNear(victim, POWER_RANGE * area, victim);
                    if (!around.empty())
                        target = around.front();
                }
                if (target)
                    if (uint32 const dealt = basisOf(p.riderValue))
                        InflictAilment(player, target, uint8(p.rider - RIDER_AILMENT), sized(target, dealt), false);
                break;
            }
        }
    }

    // Something that may set procs off happened: `spell` (nullptr: the swing) with `trigger`, on
    // `victim` (the struck or slain unit), for `amount` (the hit, the heal, the blow).
    void Trip(Unit* unit, Trigger trigger, SpellEntry const* spell, Unit* victim = nullptr, uint32 amount = 0)
    {
        if (!unit || unit->GetTypeId() != TYPEID_PLAYER || !Active(unit))
            return;
        Player* player = static_cast<Player*>(unit);
        uint32 const now = WorldTimer::getMSTime();
        std::vector<ProcDef> const& procs = Procs();
        Sockets const sockets = SocketedRunes(player);
        std::vector<size_t> acts;
        {
            std::lock_guard<std::mutex> guard(sLock);
            PlayerProcs& pp = sProcs[player->GetObjectGuid()];
            for (size_t i = 0; i < procs.size(); ++i)
            {
                ProcDef const& def = procs[i];
                if (def.trigger != trigger)
                    continue;
                auto ready = pp.readyAt.find(i);
                if (ready != pp.readyAt.end() && Later(ready->second, now))
                    continue;
                if (!Has(player, i, sockets) || !Fires(player, def, spell, sockets))
                    continue;
                pp.readyAt[i] = now + def.icd;
                if (Readies(def.effect) || def.effect == EFFECT_RESET)
                    Fire(player, i, now);
                else
                    acts.push_back(i);
            }
        }
        // A blow taken or a heal is vanilla-sized: Act sizes it for each creature it hits.
        bool const flat = trigger == TRIG_STRUCK || trigger == TRIG_HEAL_CRIT;
        for (size_t i : acts)
            if (player->IsAlive())
                Act(player, procs[i], victim, amount, flat);

        // The worn uniques' own powers.
        std::vector<UniquePower const*> powers;
        {
            std::vector<std::pair<uint32, UniquePower const*>> const worn = WornGenerated(player);
            std::lock_guard<std::mutex> guard(sLock);
            PlayerProcs& pp = sProcs[player->GetObjectGuid()];
            for (auto const& [entry, power] : worn)
            {
                if (TriggerFor(power->trigger, player->getClass()) != trigger)
                    continue;
                auto ready = pp.powerReadyAt.find(entry);
                if (ready != pp.powerReadyAt.end() && Later(ready->second, now))
                    continue;
                pp.powerReadyAt[entry] = now + power->icd;
                powers.push_back(power);
            }
        }
        for (UniquePower const* power : powers)
            if (player->IsAlive())
                PowerAct(player, *power, victim, amount, trigger);
    }

    void Drop(Player* player, PlayerProcs& pp, size_t at)
    {
        if (pp.readied[at].mod)
            player->AddSpellMod(pp.readied[at].mod, false); // deletes it
        pp.readied.erase(pp.readied.begin() + at);
    }

    // Whether `info` deals damage or heals (directly, by weapon, or over time).
    bool HitsOrHeals(SpellEntry const* info)
    {
        for (int i = 0; i < MAX_EFFECT_INDEX; ++i)
        {
            switch (info->Effect[i])
            {
                case SPELL_EFFECT_SCHOOL_DAMAGE: case SPELL_EFFECT_HEAL: case SPELL_EFFECT_WEAPON_DAMAGE:
                case SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL: case SPELL_EFFECT_NORMALIZED_WEAPON_DMG:
                case SPELL_EFFECT_WEAPON_PERCENT_DAMAGE: case SPELL_EFFECT_HEALTH_LEECH:
                    return true;
                case SPELL_EFFECT_APPLY_AURA: case SPELL_EFFECT_PERSISTENT_AREA_AURA:
                    switch (info->EffectApplyAuraName[i])
                    {
                        case SPELL_AURA_PERIODIC_DAMAGE: case SPELL_AURA_PERIODIC_HEAL: case SPELL_AURA_PERIODIC_LEECH:
                        case SPELL_AURA_PERIODIC_TRIGGER_SPELL:
                            return true;
                        default:
                            break;
                    }
                    break;
                // A Judgement's damage comes from the Seal it releases (Judgement itself is a
                // script effect).
                case SPELL_EFFECT_DUMMY: case SPELL_EFFECT_TRIGGER_SPELL: case SPELL_EFFECT_SCRIPT_EFFECT:
                    return true;
                default:
                    break;
            }
        }
        return false;
    }

    // A spell a proc's own readied mod should not count: added hits, triggered spells.
    bool Counts(Spell const* spell)
    {
        return spell && !spell->m_IsTriggeredSpell && !spell->IsArpgSecondary();
    }
}

namespace Arpg
{
    char const* ProcRuneText(uint8 rune)
    {
        for (ProcDef const& def : Procs())
            if (def.rune == rune)
                return def.text;
        return "";
    }

    std::vector<std::pair<uint32, std::string>> ProcItemLines()
    {
        std::vector<std::pair<uint32, std::string>> out;
        for (ProcDef const& def : Procs())
            if (uint32 const item = ItemOf(def))
                out.emplace_back(item, def.text);
        return out;
    }

    char const* UniquePowerText(uint32 item)
    {
        UniquePower const* power = PowerOf(item);
        return power ? power->text.c_str() : nullptr;
    }

    char const* UniquePowerName(uint32 item)
    {
        UniquePower const* power = PowerOf(item);
        return power ? power->name.c_str() : nullptr;
    }

    int32 UniqueFuryPct(Unit const* unit)
    {
        if (!unit || unit->GetTypeId() != TYPEID_PLAYER)
            return 0;
        uint32 const now = WorldTimer::getMSTime();
        std::lock_guard<std::mutex> guard(sLock);
        auto it = sProcs.find(unit->GetObjectGuid());
        return it != sProcs.end() && Later(it->second.furyUntil, now) ? it->second.furyPct : 0;
    }

    int32 UniqueSwiftPct(Unit const* unit)
    {
        if (!unit || unit->GetTypeId() != TYPEID_PLAYER)
            return 0;
        uint32 const now = WorldTimer::getMSTime();
        std::lock_guard<std::mutex> guard(sLock);
        auto it = sProcs.find(unit->GetObjectGuid());
        return it != sProcs.end() && Later(it->second.swiftUntil, now) ? it->second.swiftPct : 0;
    }

    void OnProcSwing(Unit* attacker, Unit* victim, bool crit, uint32 damage)
    {
        if (crit && victim)
            Trip(attacker, TRIG_SWING_CRIT, nullptr, victim, damage);
    }

    void OnProcStruck(Unit* victim, Unit* attacker, uint32 damage)
    {
        if (!victim || !attacker || victim == attacker || !victim->IsAlive() || damage * 5 < victim->GetMaxHealth() ||
                victim->GetTypeId() != TYPEID_PLAYER || !Active(victim))
            return;
        // The blow is not dealt yet (Unit::DealDamage asks first): a moment later, so a heal
        // lands after it, and only if the player lived through it.
        ObjectGuid const from = attacker->GetObjectGuid();
        victim->m_events.AddEvent(new UnitLambdaEvent(*victim, [from, damage](Unit& unit)
        {
            if (unit.IsInWorld() && unit.IsAlive())
                Trip(&unit, TRIG_STRUCK, nullptr, unit.GetMap()->GetUnit(from), damage);
        }), victim->m_events.CalculateTime(1));
    }

    void OnProcSpellHit(Spell* spell, Unit* caster, Unit* target, bool crit, bool heal, uint32 amount)
    {
        // A triggered spell counts here (a Seal's hit, a Judgement's), an added hit does not.
        if (!spell || spell->IsArpgSecondary() || !target)
            return;
        SpellEntry const* info = spell->m_spellInfo;
        if (heal)
        {
            if (crit)
                Trip(caster, TRIG_HEAL_CRIT, info, target, amount);
            return;
        }
        Trip(caster, TRIG_SPELL_HIT, info, target, amount);
        if (crit)
            Trip(caster, TRIG_SPELL_CRIT, info, target, amount);
    }

    void OnProcLanded(Spell* spell, Unit* caster, Unit* target)
    {
        if (!spell || spell->IsArpgSecondary() || !target || !caster || !caster->CanAttack(target))
            return;
        Trip(caster, TRIG_LANDED, spell->m_spellInfo);
    }

    void OnProcKill(Player* killer, Unit* victim)
    {
        if (killer && victim && victim != killer)
            Trip(killer, TRIG_KILL, nullptr, victim, 0);
    }

    void OnProcCast(Spell* spell)
    {
        if (!Counts(spell))
            return;
        Unit* caster = spell->GetCaster();
        if (!caster || caster->GetTypeId() != TYPEID_PLAYER || !Active(caster))
            return;
        Player* player = static_cast<Player*>(caster);
        uint32 const now = WorldTimer::getMSTime();
        std::lock_guard<std::mutex> guard(sLock);
        auto it = sProcs.find(player->GetObjectGuid());
        if (it == sProcs.end())
            return;
        SpellEntry const* info = spell->m_spellInfo;
        // Player::ApplySpellMod only lets a modifier touch the player's own class's spells.
        if (info->SpellFamilyName != uint32(player->GetSpellClass()))
            return;
        SpellCastTimesEntry const* castTime = sSpellCastTimesStore.LookupEntry(info->CastingTimeIndex);
        bool const hasCastTime = castTime && castTime->CastTime > 0;
        PlayerProcs& pp = it->second;
        for (size_t i = pp.readied.size(); i-- > 0;)
        {
            Readied& r = pp.readied[i];
            ProcDef const& def = Procs()[r.proc];
            // A cast-time proc on a spell that casts at once goes by the client's rule, the family
            // mask: that is how its recovery was reckoned (Arpg::ClientRecovery), so a spell it made
            // free of recovery spends it, named or not.
            bool const byMask = r.mod && r.mod->op == SPELLMOD_CASTING_TIME && CastsAtOnce(player, info);
            if (r.spentBy || !(r.mod ? (byMask ? r.mod->mask.IsFitToFamilyMask(info->SpellFamilyFlags) : r.mod->isAffectedOnSpell(info))
                                     : Named(info, def.targets)))
                continue;
            bool const carriesKit = def.kit.kit != UniqueKit(0);
            if (!r.mod)
            {
                // A kit row alone waits for the hit, as damage does.
                if (HitsOrHeals(info))
                {
                    r.spentBy = spell;
                    r.spentAt = now;
                }
                continue;
            }
            if (carriesKit && (r.mod->op == SPELLMOD_CASTING_TIME || r.mod->op == SPELLMOD_COST))
            {
                // Its mod's work is done at the go, as below, but its kit row waits for the hit:
                // the mod goes now, the row when the spell is done.
                if (r.mod->op == SPELLMOD_CASTING_TIME ? (!hasCastTime || spell->GetArpgCastTime() > 0)
                                                       : (spell->GetPowerCost() > 0 || (!info->manaCost && !info->ManaCostPercentage)))
                    continue;
                player->AddSpellMod(r.mod, false); // deletes it
                r.mod = nullptr;
                r.spentBy = spell;
                r.spentAt = now;
                continue;
            }
            if (r.mod->op == SPELLMOD_CASTING_TIME)
            {
                // Spent only by a cast it made instant: not a spell instant anyway (a Seal after
                // the swing), nor one whose cast began before the proc. Its work is done at the
                // go, so it goes now, before a second cast in a missile's flight could use it.
                if (!hasCastTime || spell->GetArpgCastTime() > 0)
                    continue;
                Drop(player, pp, i);
                continue;
            }
            if (r.mod->op == SPELLMOD_COST)
            {
                // Likewise: spent by a cast it made free, and gone at once (the cost is taken).
                if (spell->GetPowerCost() > 0 || (!info->manaCost && !info->ManaCostPercentage))
                    continue;
                Drop(player, pp, i);
                continue;
            }
            // Damage and crit wait for the hit: they go when the spell is done. A spell that neither
            // hits nor heals (a Seal, a blessing) does not spend them.
            if (!HitsOrHeals(info))
                continue;
            r.spentBy = spell;
            r.spentAt = now;
        }
    }

    void OnProcFinish(Spell* spell)
    {
        if (!spell)
            return;
        Unit* caster = spell->GetCaster();
        if (!caster || caster->GetTypeId() != TYPEID_PLAYER)
            return;
        Player* player = static_cast<Player*>(caster);
        std::lock_guard<std::mutex> guard(sLock);
        auto it = sProcs.find(player->GetObjectGuid());
        if (it == sProcs.end())
            return;
        PlayerProcs& pp = it->second;
        for (size_t i = pp.readied.size(); i-- > 0;)
            if (pp.readied[i].spentBy == spell)
                Drop(player, pp, i);
    }

    void UpdateProcs(Player* player)
    {
        uint32 const now = WorldTimer::getMSTime();
        std::lock_guard<std::mutex> guard(sLock);
        auto it = sProcs.find(player->GetObjectGuid());
        if (it == sProcs.end())
            return;
        PlayerProcs& pp = it->second;
        if (pp.readied.empty())
            return;
        Sockets const sockets = SocketedRunes(player);
        for (size_t i = pp.readied.size(); i-- > 0;)
        {
            Readied const& r = pp.readied[i];
            bool const stale = r.spentBy ? !Later(r.spentAt + SPENT_GRACE, now) : !Later(r.expires, now);
            // Gone with what readied it: the rune unsocketed, the item taken off.
            if (stale || !Has(player, r.proc, sockets))
                Drop(player, pp, i);
        }
    }

    std::vector<UniqueMechanic const*> ReadiedKits(Player const* player)
    {
        std::vector<UniqueMechanic const*> out;
        std::lock_guard<std::mutex> guard(sLock);
        auto it = sProcs.find(player->GetObjectGuid());
        if (it == sProcs.end())
            return out;
        // Only a row a cast has spent, while that cast's hits land: an unspent one would reach
        // a cast already under way (a Frostbolt in flight, a Starfire begun before the proc).
        for (Readied const& r : it->second.readied)
        {
            UniqueMechanic const& kit = Procs()[r.proc].kit;
            if (kit.kit != UniqueKit(0) && r.spentBy)
                out.push_back(&kit);
        }
        return out;
    }

    void ForgetProcs(Player* player)
    {
        std::lock_guard<std::mutex> guard(sLock);
        auto it = sProcs.find(player->GetObjectGuid());
        if (it == sProcs.end())
            return;
        for (size_t i = it->second.readied.size(); i-- > 0;)
            Drop(player, it->second, i);
        sProcs.erase(it);
    }
}
