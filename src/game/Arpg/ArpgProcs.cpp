/*
 * ARPG cross-skill procs: see ArpgProcs.h.
 */

#include "Arpg/ArpgProcs.h"
#include "Arpg/ArpgCombat.h"
#include "Arpg/ArpgSkills.h"
#include "Arpg/ArpgUniques.h"

#include "Globals/ObjectMgr.h"
#include "Maps/Map.h"

#include "Entities/Item.h"
#include "Entities/Player.h"
#include "Spells/Spell.h"
#include "Spells/SpellMgr.h"
#include "Server/DBCStores.h"
#include "Util/Timer.h"

#include <algorithm>
#include <cstring>
#include <map>
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
        // Done at once, not readied:
        EFFECT_HEAL,          // heal `value`% of maximum health
        EFFECT_MANA,          // restore `value`% of maximum mana
        EFFECT_EXPLODE,       // the slain foe bursts: `value`% of its maximum health as Fire nearby
        EFFECT_ARC,           // `value`% of the hit leaps to up to 3 enemies near the struck one
        EFFECT_NOVA,          // `value`% of the hit (or blow) strikes every enemy around the player
    };

    // Which uniques a pool power may fall on (POOL_NONE: an authored row, not in the pool).
    enum Pool : uint8 { POOL_NONE, POOL_ANY, POOL_WEAPON, POOL_ARMOUR };

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
        Pool pool = POOL_NONE;                // a power any unique may carry (Arpg::UniquePowerText)
    };

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
        static std::vector<ProcDef> const procs =
        {
            // --- Runes: socketed in the triggering skill ---
            { RUNE_GRACE, 0, "Rune of Grace", TRIG_SWING_CRIT, SKILL_STRIKE, {}, EFFECT_INSTANT,
              { "Holy Light", "Flash of Light" }, 0, 10000, 10000, "Holy Light or Flash of Light",
              "A critical swing makes your next Holy Light or Flash of Light instant (once every 10 sec)." },
            { RUNE_VERDICT, 0, "Rune of Verdict", TRIG_SPELL_HIT, SKILL_JUDGEMENT, {}, EFFECT_DAMAGE,
              { "Exorcism", "Hammer of Wrath" }, 50, 8000, 8000, "Exorcism or Hammer of Wrath",
              "A Judgement that hits makes your next Exorcism or Hammer of Wrath deal 50% more (once every 8 sec)." },
            { RUNE_ANVIL, 0, "Rune of the Anvil", TRIG_LANDED, SKILL_HAMMER, {}, EFFECT_RESET,
              { "Judgement" }, 0, 10000, 0, "Judgement",
              "Stunning an enemy with Hammer of Justice ends Judgement's cooldown (once every 10 sec)." },
            { RUNE_FERVOUR, 0, "Rune of Fervour", TRIG_SPELL_CRIT, Arpg::SKILL_NONE, {}, EFFECT_CRIT,
              { "Judgement", "Judgement of Righteousness", "Judgement of Command" }, 0, 8000, 10000, "Judgement",
              "A critical hit with the skill makes your next Judgement a sure critical strike (once every 8 sec)." },
            { RUNE_ZEAL, 0, "Rune of Zeal", TRIG_SPELL_CRIT, SKILL_SEALS, {}, EFFECT_FREE,
              { "Holy Light", "Flash of Light" }, 0, 10000, 12000, "Holy Light or Flash of Light",
              "A critical Seal hit makes your next Holy Light or Flash of Light cost no mana (once every 10 sec)." },

            // --- Uniques: worn ---
            { 0, 2243, "Hand of Edward the Odd", TRIG_SWING_CRIT, Arpg::SKILL_NONE, {}, EFFECT_INSTANT,
              { "*" }, 0, 20000, 10000, "spell",
              "A critical swing makes your next spell instant (once every 20 sec)." },
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
              "A critical Frostbolt makes your next Fireball or Pyroblast instant (once every 8 sec)." },
            { 0, 6220, "Meteor Shard", TRIG_SWING_CRIT, Arpg::SKILL_NONE, {}, EFFECT_FREE,
              { "Sinister Strike", "Eviscerate", "Heroic Strike", "Mortal Strike" }, 0, 8000, 8000, "finishing strike",
              "A critical swing makes your next Sinister Strike, Eviscerate, Heroic Strike or Mortal Strike cost nothing (once every 8 sec)." },

            // --- The unique power pool: every other named unique carries one (by its entry) ---
            { 0, 0, "Edge of Ruin", TRIG_SWING_CRIT, Arpg::SKILL_NONE, {}, EFFECT_DAMAGE, { "*" }, 40, 6000, 8000, "spell",
              "A critical swing makes your next spell deal 40% more (once every 6 sec).", POOL_WEAPON },
            { 0, 0, "Reaper's Haste", TRIG_KILL, Arpg::SKILL_NONE, {}, EFFECT_INSTANT, { "*" }, 0, 15000, 10000, "spell",
              "A kill makes your next spell instant (once every 15 sec).", POOL_ANY },
            { 0, 0, "Font of Power", TRIG_SPELL_CRIT, Arpg::SKILL_NONE, {}, EFFECT_FREE, { "*" }, 0, 8000, 10000, "spell",
              "A critical spell makes your next spell cost nothing (once every 8 sec).", POOL_ANY },
            { 0, 0, "Bloodthirst", TRIG_KILL, Arpg::SKILL_NONE, {}, EFFECT_HEAL, {}, 6, 1000, 0, "",
              "Each kill heals you for 6% of your maximum health.", POOL_WEAPON },
            { 0, 0, "Soul Siphon", TRIG_KILL, Arpg::SKILL_NONE, {}, EFFECT_MANA, {}, 4, 1000, 0, "",
              "Each kill restores 4% of your maximum mana.", POOL_ANY },
            { 0, 0, "Pyre", TRIG_KILL, Arpg::SKILL_NONE, {}, EFFECT_EXPLODE, {}, 15, 500, 0, "",
              "Enemies you slay explode for 15% of their maximum health as Fire damage to enemies within 8 yards.", POOL_ANY },
            { 0, 0, "Thunder Edge", TRIG_SWING_CRIT, Arpg::SKILL_NONE, {}, EFFECT_ARC, {}, 35, 1000, 0, "",
              "A critical swing arcs lightning to 3 nearby enemies for 35% of the hit.", POOL_WEAPON },
            { 0, 0, "Storm Conduit", TRIG_SPELL_CRIT, Arpg::SKILL_NONE, {}, EFFECT_ARC, {}, 30, 1000, 0, "",
              "A critical spell arcs lightning to 3 nearby enemies for 30% of the hit.", POOL_ANY },
            { 0, 0, "Second Wind", TRIG_STRUCK, Arpg::SKILL_NONE, {}, EFFECT_HEAL, {}, 15, 30000, 0, "",
              "A heavy blow (a fifth of your health) heals you for 15% of your maximum health (once every 30 sec).", POOL_ARMOUR },
            { 0, 0, "Mender's Grace", TRIG_HEAL_CRIT, Arpg::SKILL_NONE, {}, EFFECT_INSTANT, { "*" }, 0, 10000, 10000, "spell",
              "A critical heal makes your next spell instant (once every 10 sec).", POOL_ANY },
            { 0, 0, "Executioner", TRIG_KILL, Arpg::SKILL_NONE, {}, EFFECT_CRIT, { "*" }, 0, 8000, 10000, "spell",
              "A kill makes your next spell a sure critical strike (once every 8 sec).", POOL_WEAPON },
            { 0, 0, "Wellspring", TRIG_SWING_CRIT, Arpg::SKILL_NONE, {}, EFFECT_MANA, {}, 3, 2000, 0, "",
              "A critical swing restores 3% of your maximum mana.", POOL_WEAPON },
            { 0, 0, "Bastion", TRIG_STRUCK, Arpg::SKILL_NONE, {}, EFFECT_NOVA, {}, 50, 8000, 0, "",
              "A heavy blow makes you strike back with holy fire: 50% of it to every enemy within 8 yards (once every 8 sec).", POOL_ARMOUR },
            { 0, 0, "Wrathstorm", TRIG_SPELL_CRIT, Arpg::SKILL_NONE, {}, EFFECT_NOVA, {}, 25, 3000, 0, "",
              "A critical spell bursts with holy fire: 25% of the hit to every enemy within 8 yards (once every 3 sec).", POOL_ANY },
            { 0, 0, "Vampiric Edge", TRIG_SWING_CRIT, Arpg::SKILL_NONE, {}, EFFECT_HEAL, {}, 3, 1500, 0, "",
              "A critical swing heals you for 3% of your maximum health.", POOL_WEAPON },
            { 0, 0, "Ironskin", TRIG_STRUCK, Arpg::SKILL_NONE, {}, EFFECT_MANA, {}, 10, 15000, 0, "",
              "A heavy blow restores 10% of your maximum mana (once every 15 sec).", POOL_ARMOUR },
        };
        return procs;
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
        SpellModifier* mod;
        uint32 expires;
        Spell const* spentBy = nullptr;       // compared only, never followed
        uint32 spentAt = 0;
    };

    struct PlayerProcs
    {
        std::unordered_map<size_t, uint32> readyAt;   // by row: when it may fire again
        std::vector<Readied> readied;
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
            if (def.item == item)
                return true;
        return false;
    }

    // The pool row unique `item` carries, or -1: a named blue or purple weapon or armour piece
    // that is not authored takes one, the same one always (by its entry), fitting its kind.
    int PowerRow(uint32 item)
    {
        static std::mutex lock;
        static std::unordered_map<uint32, int> cache;
        {
            std::lock_guard<std::mutex> guard(lock);
            auto it = cache.find(item);
            if (it != cache.end())
                return it->second;
        }
        int row = -1;
        ItemPrototype const* proto = sObjectMgr.GetItemPrototype(item);
        bool const equips = proto && proto->Quality >= ITEM_QUALITY_RARE && proto->Quality <= ITEM_QUALITY_EPIC &&
                            (proto->Class == ITEM_CLASS_WEAPON || proto->Class == ITEM_CLASS_ARMOR) &&
                            proto->InventoryType != INVTYPE_NON_EQUIP && proto->InventoryType != INVTYPE_BODY &&
                            proto->InventoryType != INVTYPE_TABARD && proto->InventoryType != INVTYPE_BAG &&
                            proto->InventoryType != INVTYPE_QUIVER && proto->InventoryType != INVTYPE_AMMO;
        if (equips && !Authored(item))
        {
            bool const weapon = proto->Class == ITEM_CLASS_WEAPON;
            uint32 const type = proto->InventoryType;
            bool const jewel = type == INVTYPE_NECK || type == INVTYPE_FINGER || type == INVTYPE_TRINKET;
            std::vector<int> fits;
            std::vector<ProcDef> const& procs = Procs();
            for (size_t i = 0; i < procs.size(); ++i)
            {
                Pool const pool = procs[i].pool;
                if (pool == POOL_ANY || (pool == POOL_WEAPON && weapon) || (pool == POOL_ARMOUR && !weapon && !jewel))
                    fits.push_back(int(i));
            }
            if (!fits.empty())
                row = fits[(item * 2654435761u >> 7) % fits.size()];
        }
        std::lock_guard<std::mutex> guard(lock);
        cache[item] = row;
        return row;
    }

    // The pool rows the player's worn uniques carry.
    std::vector<size_t> WornPowers(Player const* player)
    {
        std::vector<size_t> out;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (Item* it = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                if (!it->IsBroken())
                {
                    int const row = PowerRow(it->GetEntry());
                    if (row >= 0)
                        out.push_back(size_t(row));
                }
        return out;
    }

    using Sockets = std::vector<std::pair<SkillId, uint8>>;

    bool Socketed(Sockets const& sockets, SkillId skill, uint8 rune)
    {
        for (auto const& [s, r] : sockets)
            if (r == rune && (skill == Arpg::SKILL_NONE || s == skill))
                return true;
        return false;
    }

    // Whether `player` has proc row `row`: the rune socketed in a skill, the item worn, or a worn
    // unique carrying the pool power.
    bool Has(Player const* player, size_t row, Sockets const& sockets, std::vector<size_t> const& worn)
    {
        ProcDef const& def = Procs()[row];
        if (def.pool != POOL_NONE)
            return std::find(worn.begin(), worn.end(), row) != worn.end();
        if (def.item)
            return Wears(player, def.item);
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
    void Act(Player* player, ProcDef const& def, Unit* victim, uint32 amount)
    {
        auto foes = [&](WorldObject* centre, float range, Unit const* skip)
        {
            std::vector<Unit*> out;
            for (Unit* unit : EnemiesNear(centre, range))
                if (unit != skip && unit->IsAlive() && MayCatchUnit(player, unit, nullptr))
                    out.push_back(unit);
            return out;
        };
        auto strike = [&](std::vector<Unit*> const& units, uint32 look, uint32 school, uint32 dealt, char const* verb)
        {
            SpellEntry const* spell = sSpellTemplate.LookupEntry<SpellEntry>(look);
            if (!spell || units.empty() || !dealt)
                return;
            for (Unit* unit : units)
                StrikeFoe(player, unit, spell, school, dealt);
            ReportProc(player, PROC_DAMAGE, 0, def.name, "%s %u %s for %u.", verb, uint32(units.size()),
                       units.size() == 1 ? "enemy" : "enemies", dealt);
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
                uint32 const cap = player->GetMaxHealth() * EXPLODE_CAP_PCT / 100;
                uint32 const dealt = std::min(cap, victim->GetMaxHealth() * uint32(def.value) / 100);
                strike(foes(victim, EXPLODE_RANGE, victim), EXPLODE_LOOK, SPELL_SCHOOL_MASK_FIRE, dealt, "bursts onto");
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
                strike(closest, ARC_LOOK, SPELL_SCHOOL_MASK_NATURE, amount * uint32(def.value) / 100, "arcs to");
                break;
            }
            case EFFECT_NOVA:
                strike(foes(player, NOVA_RANGE, nullptr), NOVA_LOOK, SPELL_SCHOOL_MASK_HOLY, amount * uint32(def.value) / 100, "strikes");
                break;
            default:
                break;
        }
    }

    bool Readies(Effect effect)
    {
        return effect == EFFECT_INSTANT || effect == EFFECT_DAMAGE || effect == EFFECT_CRIT || effect == EFFECT_FREE;
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
                if (!entry || entry->SpellFamilyName != family || !Named(entry, def.targets) || player->IsSpellReady(*entry))
                    continue;
                player->RemoveSpellCooldown(*entry, true);
                ++ended;
            }
            if (ended)
                ReportProc(player, PROC_EFFECT, 0, def.name, "ends the cooldown of %s.", def.targetWords);
            return;
        }
        uint32 const owner = FamilyOwner(player);
        uint64 const mask = MaskOf(family, def.targets);
        if (!owner || !mask)
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
                words = "instant";
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
            default:
                return;
        }
        player->AddSpellMod(mod, true);
        pp.readied.push_back({ row, mod, now + def.lasts });
        if (def.effect == EFFECT_DAMAGE)
            ReportProc(player, PROC_EFFECT, 0, def.name, "readies your next %s: +%d%% damage.", def.targetWords, int(def.value));
        else
            ReportProc(player, PROC_EFFECT, 0, def.name, "readies your next %s: %s.", def.targetWords, words);
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
        std::vector<size_t> const worn = WornPowers(player);
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
                if (!Has(player, i, sockets, worn) || !Fires(player, def, spell, sockets))
                    continue;
                pp.readyAt[i] = now + def.icd;
                if (Readies(def.effect) || def.effect == EFFECT_RESET)
                    Fire(player, i, now);
                else
                    acts.push_back(i);
            }
        }
        for (size_t i : acts)
            if (player->IsAlive())
                Act(player, procs[i], victim, amount);
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
                // A Judgement's damage comes from the Seal it releases.
                case SPELL_EFFECT_DUMMY: case SPELL_EFFECT_TRIGGER_SPELL:
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
            if (def.item)
                out.emplace_back(def.item, def.text);
        return out;
    }

    char const* UniquePowerText(uint32 item)
    {
        int const row = PowerRow(item);
        return row < 0 ? nullptr : Procs()[size_t(row)].text;
    }

    char const* UniquePowerName(uint32 item)
    {
        int const row = PowerRow(item);
        return row < 0 ? nullptr : Procs()[size_t(row)].name;
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
            if (r.spentBy || !r.mod || !r.mod->isAffectedOnSpell(info))
                continue;
            if (r.mod->op == SPELLMOD_CASTING_TIME)
            {
                // Spent only by a cast it made instant: not a spell instant anyway (a Seal after
                // the swing), nor one whose cast began before the proc. Its work is done at the
                // go, so it goes now, before a second cast in a missile's flight could use it.
                if (!hasCastTime || spell->GetCastTime() > 0)
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
        std::vector<size_t> const worn = WornPowers(player);
        for (size_t i = pp.readied.size(); i-- > 0;)
        {
            Readied const& r = pp.readied[i];
            bool const stale = r.spentBy ? !Later(r.spentAt + SPENT_GRACE, now) : !Later(r.expires, now);
            // Gone with what readied it: the rune unsocketed, the item taken off.
            if (stale || !Has(player, r.proc, sockets, worn))
                Drop(player, pp, i);
        }
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
