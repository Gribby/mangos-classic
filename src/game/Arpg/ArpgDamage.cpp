/*
 * ARPG damage: see ArpgDamage.h.
 */

#include "Arpg/ArpgDamage.h"
#include "Arpg/ArpgCharacter.h"
#include "Arpg/ArpgCombat.h"
#include "Arpg/ArpgMage.h"
#include "Arpg/ArpgSkills.h"
#include "Arpg/ArpgTags.h"
#include "Arpg/ArpgTree.h"
#include "Arpg/ArpgUniques.h"

#include "Entities/Creature.h"
#include "Entities/Player.h"
#include "Spells/SpellMgr.h"
#include "Util/Timer.h"
#include "World/World.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <unordered_map>

namespace
{
    using namespace Arpg;

    // The damage tags, in the tags' order, and what goes with each.
    constexpr uint8 DAMAGE_TAGS = 7;
    uint32 const SCHOOL_OF_TAG[DAMAGE_TAGS] =
    {
        SPELL_SCHOOL_MASK_NORMAL, SPELL_SCHOOL_MASK_HOLY, SPELL_SCHOOL_MASK_FIRE, SPELL_SCHOOL_MASK_FROST,
        SPELL_SCHOOL_MASK_ARCANE, SPELL_SCHOOL_MASK_NATURE, SPELL_SCHOOL_MASK_SHADOW,
    };
    char const* const AILMENT_NAME[DAMAGE_TAGS] = { "Bleed", "Judged", "Ignite", "Chill", "Overload", "Poison", "Wither" };
    // An added hit's look, by its type: Rend, Smite, Fireball, Frostbolt, Arcane Explosion,
    // Lightning Bolt, Shadow Bolt.
    uint32 const ADDED_LOOK[DAMAGE_TAGS] = { 772, 585, 133, 116, 1449, 403, 686 };
    // The ailments' own looks: Rend, Holy Wrath, Immolate, -, Arcane Explosion, Deadly Poison, -.
    constexpr uint32 BLEED_LOOK = 772, JUDGED_LOOK = 2812, IGNITE_LOOK = 348, OVERLOAD_LOOK = 1449, POISON_LOOK = 2818;
    constexpr uint32 SPELL_CHILLED = 12484, SPELL_FROSTBITE = 12494;

    // The ailments' numbers: share of the hit (percent), ticks a second apart.
    constexpr uint32 TICK_MS = 1000;
    constexpr uint32 BLEED_PCT = 60, BLEED_TICKS = 5;
    constexpr uint32 IGNITE_PCT = 80, IGNITE_TICKS = 4;
    constexpr uint32 POISON_PCT = 30, POISON_TICKS = 3, POISON_MAX = 20;
    constexpr uint32 WITHER_PCT = 4, WITHER_MAX = 10, WITHER_MS = 4000;
    constexpr uint32 OVERLOAD_PCT = 50, OVERLOAD_MS = 3000;
    constexpr uint32 JUDGED_PCT = 25, JUDGED_STACKS = 5, JUDGED_MS = 6000;
    constexpr float BURST_RANGE = 6.0f;
    // The mage's keystones on ailments: Wildfire's weaker Ignites; Arcane Flux's and Overcharge's
    // bigger Overload bursts, and Arcane Flux's mana from each.
    constexpr uint32 WILDFIRE_LESS_PCT = 20, FLUX_MORE_PCT = 50, FLUX_MANA_PCT = 2, OVERCHARGE_MORE_PCT = 40;
    constexpr size_t BURST_MAX = 10;
    // An added hit rolls 60% to 140% of its affixes' average (ArpgAffixes.cpp shows the same range).
    constexpr uint32 ADDED_LOW_PCT = 60, ADDED_HIGH_PCT = 140;
    // At most this many added hits beside one main hit: the largest types.
    constexpr size_t ADDED_MAX = 2;
    constexpr uint32 UPDATE_MS = 200;
    // The level gap on creature hits (LevelGapMod): per level above, its floor; per level below,
    // its ceiling.
    constexpr float GAP_ABOVE = 0.1f, GAP_FLOOR = 0.4f, GAP_BELOW = 0.05f, GAP_CEILING = 1.25f;

    struct Affliction
    {
        ObjectGuid victim;
        uint8 tag = 0;           // the damage tag: which ailment
        uint32 perTick = 0;      // a damage over time's tick
        uint8 ticks = 0;         // its ticks left
        uint32 nextTick = 0;
        uint8 stacks = 0;        // Wither, Judged
        uint32 stored = 0;       // Overload, Judged
        uint32 until = 0;        // when it ends (Overload: when it bursts)
    };

    struct PlayerAilments
    {
        std::vector<Affliction> list;
        uint32 lastUpdate = 0;
    };

    std::mutex sLock;
    std::unordered_map<ObjectGuid, PlayerAilments> sAilments;

    // What a tick or burst does, done outside the lock.
    struct Strike
    {
        ObjectGuid victim;
        uint8 tag;
        uint32 amount;
        bool burst;              // Overload, Judged: an area around the victim
        uint8 stacks;
    };

    float ConfigFloat(eConfigFloatValues index)
    {
        return sWorld.getConfig(index);
    }

    bool Enabled()
    {
        return sWorld.getConfig(CONFIG_BOOL_ARPG_ENABLE);
    }

    // The ARPG player behind `unit`: itself, or the owner of its pet, totem or guardian.
    Player const* ArpgOwner(Unit const* unit)
    {
        if (!unit)
            return nullptr;
        if (unit->GetTypeId() == TYPEID_PLAYER)
            return Active(unit) ? static_cast<Player const*>(unit) : nullptr;
        ObjectGuid const owner = unit->GetOwnerGuid();
        if (!owner.IsPlayer() || !unit->GetMap())
            return nullptr;
        Player const* player = unit->GetMap()->GetPlayer(owner);
        return player && Active(player) ? player : nullptr;
    }

    // Whether `victim` is a fair target for the ARPG's numbers: a creature no player controls.
    bool Scaled(Unit const* victim)
    {
        return !victim || (victim->GetTypeId() != TYPEID_PLAYER && !victim->IsPlayerControlled());
    }

    int32 AilmentIncreased(Player const* player, WebTotals const& t)
    {
        return t.ailmentPct + t.tag[TAG_DURATION] + int32(player->GetStat(STAT_SPIRIT)) / SPI_PER_DOT;
    }

    uint32 AilmentShare(Player const* player, WebTotals const& t, uint32 dealt, uint32 pct)
    {
        float const more = std::max(0.0f, 1.0f + AilmentIncreased(player, t) / 100.0f);
        return uint32(float(dealt) * pct / 100.0f * more);
    }

    Affliction* Find(std::vector<Affliction>& list, ObjectGuid victim, uint8 tag)
    {
        for (Affliction& a : list)
            if (a.victim == victim && a.tag == tag)
                return &a;
        return nullptr;
    }

    // Inflict `tag`'s ailment from a hit of `dealt` (critical or not) by `player` on `victim`.
    void Inflict(Player* player, WebTotals const& t, Unit* victim, uint8 tag, uint32 dealt, bool crit)
    {
        uint32 const now = WorldTimer::getMSTime();
        ObjectGuid const guid = victim->GetObjectGuid();
        // Frost acts at once: a chill, or a freeze on a chilled target hit critically.
        if (tag == TAG_FROST)
        {
            if (crit && victim->HasAura(SPELL_CHILLED))
            {
                player->CastSpell(victim, SPELL_FROSTBITE, TRIGGERED_OLD_TRIGGERED);
                ReportProcOn(player, victim, TAG_FROST, PROC_EFFECT, 0, "Freeze", "freezes %s in place.", victim->GetName());
            }
            else
                player->CastSpell(victim, SPELL_CHILLED, TRIGGERED_OLD_TRIGGERED);
            return;
        }
        std::lock_guard<std::mutex> guard(sLock);
        std::vector<Affliction>& list = sAilments[player->GetObjectGuid()].list;
        switch (tag)
        {
            case TAG_PHYSICAL:
            case TAG_FIRE:
            {
                bool const bleed = tag == TAG_PHYSICAL;
                uint8 const ticks = uint8(bleed ? BLEED_TICKS : IGNITE_TICKS);
                uint32 share = AilmentShare(player, t, dealt, bleed ? BLEED_PCT : IGNITE_PCT);
                if (!bleed && t.Has(KEY_WILDFIRE))
                    share = share * (100 - WILDFIRE_LESS_PCT) / 100;
                uint32 const perTick = std::max<uint32>(1, share / ticks);
                Affliction* a = Find(list, guid, tag);
                // The strongest counts: a weaker one leaves it be.
                if (a && a->ticks && a->perTick > perTick)
                    return;
                if (!a)
                {
                    list.push_back(Affliction());
                    a = &list.back();
                    a->victim = guid;
                    a->tag = tag;
                }
                a->perTick = perTick;
                a->ticks = ticks;
                a->nextTick = now + TICK_MS;
                break;
            }
            case TAG_NATURE:
            {
                uint32 count = 0;
                for (Affliction const& a : list)
                    if (a.victim == guid && a.tag == TAG_NATURE && a.ticks)
                        ++count;
                if (count >= POISON_MAX)
                    return;
                Affliction a;
                a.victim = guid;
                a.tag = TAG_NATURE;
                a.perTick = std::max<uint32>(1, AilmentShare(player, t, dealt, POISON_PCT) / POISON_TICKS);
                a.ticks = uint8(POISON_TICKS);
                a.nextTick = now + TICK_MS;
                list.push_back(a);
                break;
            }
            case TAG_SHADOW:
            {
                Affliction* a = Find(list, guid, tag);
                if (!a)
                {
                    list.push_back(Affliction());
                    a = &list.back();
                    a->victim = guid;
                    a->tag = tag;
                }
                a->stacks = uint8(std::min<uint32>(a->stacks + 1, WITHER_MAX));
                a->until = now + WITHER_MS;
                break;
            }
            case TAG_ARCANE:
            {
                Affliction* a = Find(list, guid, tag);
                if (!a)
                {
                    list.push_back(Affliction());
                    a = &list.back();
                    a->victim = guid;
                    a->tag = tag;
                    a->until = now + OVERLOAD_MS;
                }
                a->stored += AilmentShare(player, t, dealt, OVERLOAD_PCT);
                break;
            }
            case TAG_HOLY:
            {
                Affliction* a = Find(list, guid, tag);
                if (!a)
                {
                    list.push_back(Affliction());
                    a = &list.back();
                    a->victim = guid;
                    a->tag = tag;
                }
                a->stacks = uint8(std::min<uint32>(a->stacks + 1, JUDGED_STACKS));
                a->stored += AilmentShare(player, t, dealt, JUDGED_PCT);
                a->until = now + JUDGED_MS;
                break;
            }
            default:
                break;
        }
    }

    // Roll and inflict the ailments of a hit of `tags`: its own types' (sure on a critical strike,
    // else the gear's chance), and the skill tree's ailment nodes for `skill`'s hits, whatever
    // their type (SKILL_NONE: an added hit, which has none).
    void RollAilments(Player* player, WebTotals const& t, Unit* victim, uint32 tags, uint32 dealt, bool crit, uint8 skill)
    {
        for (uint8 tag = 0; tag < DAMAGE_TAGS; ++tag)
        {
            bool const own = (tags & TagBit(Tag(tag))) != 0;
            int32 chance = own ? t.ailmentChance[tag] : 0;
            if (skill && skill < 16)
                chance += t.skillAilment[skill][tag];
            if ((own && crit) || (chance > 0 && roll_chance_i(std::clamp(chance, 0, 100))))
                Inflict(player, t, victim, tag, dealt, own && crit);
        }
    }

    std::string Format(char const* format, ...) ATTR_PRINTF(1, 2);
    std::string Format(char const* format, ...)
    {
        char buf[256];
        va_list args;
        va_start(args, format);
        std::vsnprintf(buf, sizeof(buf), format, args);
        va_end(args);
        return buf;
    }
}

namespace Arpg
{
    float PowerScaleAt(uint32 level)
    {
        if (!Enabled())
            return 1.0f;
        float const perLevel = std::max(0.0f, ConfigFloat(CONFIG_FLOAT_ARPG_POWER_PER_LEVEL));
        return 1.0f + perLevel * float(std::clamp<uint32>(level, 1, 63) - 1);
    }

    float PowerScale(Unit const* unit)
    {
        Player const* player = ArpgOwner(unit);
        return player ? PowerScaleAt(player->GetLevel()) : 1.0f;
    }

    float DamageScale(Unit const* attacker, Unit const* victim)
    {
        return Scaled(victim) ? PowerScale(attacker) : 1.0f;
    }

    float CreatureHealthScale(uint32 level)
    {
        if (!Enabled())
            return 1.0f;
        float const growth = std::max(0.0f, ConfigFloat(CONFIG_FLOAT_ARPG_POWER_HEALTH_GROWTH));
        return PowerScaleAt(level) * (1.0f + growth * float(std::min<uint32>(level, 63)) / 60.0f);
    }

    uint32 ToDamage(Unit const* attacker, Unit const* victim, uint32 amount)
    {
        return uint32(float(amount) * DamageScale(attacker, victim) + 0.5f);
    }

    uint32 FromDamage(Unit const* attacker, Unit const* victim, uint32 amount)
    {
        return uint32(float(amount) / std::max(1.0f, DamageScale(attacker, victim)));
    }

    float LevelGapMod(Unit const* attacker, Unit const* victim)
    {
        if (!attacker || !victim || attacker->GetTypeId() != TYPEID_UNIT || victim->GetTypeId() != TYPEID_PLAYER ||
                !Active(victim) || attacker->GetOwnerGuid().IsPlayer())
            return 1.0f;
        int32 const gap = int32(victim->GetLevel()) - int32(attacker->GetLevel());
        if (gap > 0)
            return std::max(GAP_FLOOR, 1.0f - GAP_ABOVE * float(gap));
        return std::min(GAP_CEILING, 1.0f + GAP_BELOW * float(-gap));
    }

    float CritBonus(Unit const* attacker, SpellEntry const* spell)
    {
        if (!attacker || attacker->GetTypeId() != TYPEID_PLAYER || !Active(attacker))
            return 0.0f;
        std::shared_ptr<WebTotals const> totals = TotalsOf(attacker);
        int32 pct = int32(attacker->GetStat(STAT_AGILITY)) / AGI_PER_CRIT;
        if (totals)
        {
            pct += totals->critDamagePct;
            // Spells: the jewellery's spell critical strike damage too.
            if (spell && (spell->DmgClass == SPELL_DAMAGE_CLASS_MAGIC || spell->DmgClass == SPELL_DAMAGE_CLASS_NONE))
                pct += totals->spellCritDamagePct;
        }
        return std::max(0, pct) / 100.0f;
    }

    float CritMultiplier(Unit const* attacker, SpellEntry const* spell)
    {
        bool const magic = spell && (spell->DmgClass == SPELL_DAMAGE_CLASS_MAGIC || spell->DmgClass == SPELL_DAMAGE_CLASS_NONE);
        return (magic ? 1.5f : 2.0f) + CritBonus(attacker, spell);
    }

    void OnHitLanded(Unit* attacker, Unit* victim, SpellEntry const* spellInfo, uint32 tags, uint32 dealt, bool crit)
    {
        if (!attacker || !victim || !dealt || attacker == victim || attacker->GetTypeId() != TYPEID_PLAYER || !Active(attacker))
            return;
        if (!Scaled(victim) || !attacker->CanAttack(victim))
            return;
        std::shared_ptr<WebTotals const> totals = TotalsOf(attacker);
        if (!totals)
            return;
        WebTotals const& t = *totals;
        Player* player = static_cast<Player*>(attacker);

        // The main hit's own ailments, and its skill tree's (a dead target takes none).
        if (victim->IsAlive())
            RollAilments(player, t, victim, tags, dealt, crit, uint8(SkillOfSpell(player->getClass(), spellInfo)));
        // The mage's keystones on a spell's hit (Arpg/ArpgMage.h), the dead included.
        if (spellInfo)
            OnMageHit(player, victim, spellInfo, dealt, crit);

        // Added damage: attacks take the attack affixes, spells the spell ones. A Seal's hit rides
        // on a swing, which has its added damage already.
        if (tags & TagBit(TAG_SEAL))
            return;
        bool const attack = (tags & TagBit(TAG_MELEE)) != 0;
        int32 const* added = attack ? t.addedAttack : t.addedSpell;
        std::vector<std::pair<int32, uint8>> types;
        for (uint8 tag = 0; tag < DAMAGE_TAGS; ++tag)
            if (added[tag] > 0)
                types.emplace_back(added[tag], tag);
        if (types.empty() || !victim->IsAlive())
            return;
        std::sort(types.begin(), types.end(), [](auto const& a, auto const& b) { return a.first > b.first; });
        if (types.size() > ADDED_MAX)
            types.resize(ADDED_MAX);
        uint32 constexpr DAMAGE_MASK = (1u << DAMAGE_TAGS) - 1;
        for (auto const& [average, tag] : types)
        {
            if (!victim->IsAlive())
                break;
            SpellEntry const* look = sSpellTemplate.LookupEntry<SpellEntry>(ADDED_LOOK[tag]);
            if (!look)
                continue;
            // Its type's bonuses with the main hit's delivery (Melee, Spell, Area...).
            uint32 const own = (tags & ~DAMAGE_MASK) | TagBit(Tag(tag));
            float amount = float(urand(uint32(average) * ADDED_LOW_PCT / 100, uint32(average) * ADDED_HIGH_PCT / 100));
            // The affixes' numbers are at the ARPG's size already (their item's level): the size
            // comes off the bonuses.
            amount *= DamageModForTags(attacker, victim, spellInfo, own) / std::max(1.0f, DamageScale(attacker, victim));
            if (crit)
                amount *= CritMultiplier(attacker, spellInfo);
            uint32 const hit = std::max<uint32>(1, uint32(amount));
            StrikeFoe(attacker, victim, look, SCHOOL_OF_TAG[tag], hit);
            if (victim->IsAlive())
                RollAilments(player, t, victim, TagBit(Tag(tag)), hit, crit, 0);
        }
    }

    void InflictAilment(Player* player, Unit* victim, uint8 tag, uint32 dealt, bool crit)
    {
        if (!player || !victim || tag >= DAMAGE_TAGS || !dealt || !victim->IsAlive() || !Scaled(victim) || !player->CanAttack(victim))
            return;
        if (std::shared_ptr<WebTotals const> totals = TotalsOf(player))
            Inflict(player, *totals, victim, tag, dealt, crit);
    }

    uint32 SchoolOfTag(uint8 tag)
    {
        return tag < DAMAGE_TAGS ? SCHOOL_OF_TAG[tag] : uint32(SPELL_SCHOOL_MASK_NORMAL);
    }

    uint32 LookOfTag(uint8 tag)
    {
        return tag < DAMAGE_TAGS ? ADDED_LOOK[tag] : ADDED_LOOK[0];
    }

    float AilmentTakenMod(Unit const* attacker, Unit const* victim)
    {
        if (!attacker || !victim || attacker->GetTypeId() != TYPEID_PLAYER)
            return 1.0f;
        uint32 const now = WorldTimer::getMSTime();
        std::lock_guard<std::mutex> guard(sLock);
        auto it = sAilments.find(attacker->GetObjectGuid());
        if (it == sAilments.end())
            return 1.0f;
        for (Affliction const& a : it->second.list)
            if (a.victim == victim->GetObjectGuid() && a.tag == TAG_SHADOW && a.stacks && now < a.until)
                return 1.0f + float(a.stacks * WITHER_PCT) / 100.0f;
        return 1.0f;
    }

    void UpdateAilments(Player* player)
    {
        uint32 const now = WorldTimer::getMSTime();
        std::vector<Strike> due;
        {
            std::lock_guard<std::mutex> guard(sLock);
            auto it = sAilments.find(player->GetObjectGuid());
            if (it == sAilments.end())
                return;
            PlayerAilments& mine = it->second;
            if (mine.lastUpdate && WorldTimer::getMSTimeDiff(mine.lastUpdate, now) < UPDATE_MS)
                return;
            mine.lastUpdate = now ? now : 1;
            std::vector<Affliction>& list = mine.list;
            for (Affliction& a : list)
            {
                Unit* victim = player->IsInWorld() ? player->GetMap()->GetUnit(a.victim) : nullptr;
                if (!victim || !victim->IsAlive())
                {
                    a.ticks = 0;
                    a.stacks = 0;
                    a.stored = 0;
                    a.until = 0;
                    continue;
                }
                switch (a.tag)
                {
                    case TAG_PHYSICAL:
                    case TAG_FIRE:
                    case TAG_NATURE:
                        while (a.ticks && int32(now - a.nextTick) >= 0)
                        {
                            uint32 amount = a.perTick;
                            // A bleeding target that moves bleeds twice as fast.
                            if (a.tag == TAG_PHYSICAL && victim->IsMoving())
                                amount *= 2;
                            due.push_back({ a.victim, a.tag, amount, false, 0 });
                            --a.ticks;
                            a.nextTick += TICK_MS;
                        }
                        break;
                    case TAG_ARCANE:
                        if (a.stored && int32(now - a.until) >= 0)
                        {
                            due.push_back({ a.victim, a.tag, a.stored, true, 0 });
                            a.stored = 0;
                            a.until = 0;
                        }
                        break;
                    case TAG_HOLY:
                        if (a.stacks >= JUDGED_STACKS && a.stored)
                        {
                            due.push_back({ a.victim, a.tag, a.stored, true, a.stacks });
                            a.stacks = 0;
                            a.stored = 0;
                            a.until = 0;
                        }
                        else if (a.stacks && int32(now - a.until) >= 0)
                        {
                            a.stacks = 0;
                            a.stored = 0;
                            a.until = 0;
                        }
                        break;
                    case TAG_SHADOW:
                        if (a.stacks && int32(now - a.until) >= 0)
                            a.stacks = 0;
                        break;
                    default:
                        break;
                }
            }
            list.erase(std::remove_if(list.begin(), list.end(), [](Affliction const& a)
            {
                return !a.ticks && !a.stacks && !a.stored;
            }), list.end());
        }
        // Poison ticks on one target land as one hit.
        std::vector<Strike> merged;
        for (Strike const& s : due)
        {
            auto same = std::find_if(merged.begin(), merged.end(), [&](Strike const& m)
            {
                return !m.burst && !s.burst && m.tag == s.tag && m.victim == s.victim;
            });
            if (same != merged.end())
                same->amount += s.amount;
            else
                merged.push_back(s);
        }
        if (merged.empty() || !player->IsInWorld() || !player->IsAlive())
            return;
        std::shared_ptr<WebTotals const> totals = TotalsOf(player);
        for (Strike s : merged)
        {
            Unit* victim = player->GetMap()->GetUnit(s.victim);
            if (!victim || !victim->IsAlive())
                continue;
            if (!s.burst)
            {
                uint32 const lookId = s.tag == TAG_PHYSICAL ? BLEED_LOOK : s.tag == TAG_FIRE ? IGNITE_LOOK : POISON_LOOK;
                if (SpellEntry const* look = sSpellTemplate.LookupEntry<SpellEntry>(lookId))
                    StrikeTick(player, victim, look, SCHOOL_OF_TAG[s.tag], std::max<uint32>(1, s.amount));
                continue;
            }
            SpellEntry const* look = sSpellTemplate.LookupEntry<SpellEntry>(s.tag == TAG_HOLY ? JUDGED_LOOK : OVERLOAD_LOOK);
            if (!look)
                continue;
            if (s.tag == TAG_ARCANE && totals)
            {
                if (totals->Has(KEY_ARCANE_FLUX))
                {
                    s.amount = s.amount * (100 + FLUX_MORE_PCT) / 100;
                    ProcMana(player, "Arcane Flux", player->GetMaxPower(POWER_MANA) * FLUX_MANA_PCT / 100);
                }
                if (totals->Has(KEY_OVERCHARGE))
                    s.amount = s.amount * (100 + OVERCHARGE_MORE_PCT) / 100;
            }
            float const range = BURST_RANGE * SpellAreaScale(player);
            std::vector<Unit*> foes = NearestFoes(player, victim, range, BURST_MAX);
            if (std::find(foes.begin(), foes.end(), victim) == foes.end())
                foes.insert(foes.begin(), victim);
            for (Unit* unit : foes)
                if (unit->IsAlive())
                    StrikeFoe(player, unit, look, SCHOOL_OF_TAG[s.tag], std::max<uint32>(1, s.amount));
            ReportProcOn(player, victim, s.tag, PROC_DAMAGE, s.amount, AILMENT_NAME[s.tag], "%s on %s for %u, striking %u %s.",
                       s.tag == TAG_HOLY ? "bursts in a Holy blast" : "overloads", victim->GetName(), s.amount,
                       uint32(foes.size()), foes.size() == 1 ? "enemy" : "enemies");
        }
    }

    bool Frozen(Unit const* unit)
    {
        return unit && (unit->HasAura(SPELL_FROSTBITE) || unit->HasAuraType(SPELL_AURA_MOD_ROOT));
    }

    bool Chilled(Unit const* unit)
    {
        return unit && unit->HasAura(SPELL_CHILLED);
    }

    void Chill(Player* player, Unit* victim, bool freeze)
    {
        if (!player || !victim || !victim->IsAlive() || !Scaled(victim) || !player->CanAttack(victim))
            return;
        if (freeze)
        {
            player->CastSpell(victim, SPELL_FROSTBITE, TRIGGERED_OLD_TRIGGERED);
            ReportProcOn(player, victim, TAG_FROST, PROC_EFFECT, 0, "Freeze", "freezes %s in place.", victim->GetName());
        }
        else
            player->CastSpell(victim, SPELL_CHILLED, TRIGGERED_OLD_TRIGGERED);
    }

    uint32 TakeIgnite(Player* player, Unit* victim)
    {
        if (!player || !victim)
            return 0;
        std::lock_guard<std::mutex> guard(sLock);
        auto it = sAilments.find(player->GetObjectGuid());
        if (it == sAilments.end())
            return 0;
        Affliction* a = Find(it->second.list, victim->GetObjectGuid(), TAG_FIRE);
        if (!a || !a->ticks)
            return 0;
        uint32 const left = a->perTick * a->ticks;
        a->ticks = 0;
        return left;
    }

    size_t SpreadIgnite(Player* player, Unit* from, float range, size_t count)
    {
        if (!player || !from)
            return 0;
        uint32 perTick = 0;
        {
            std::lock_guard<std::mutex> guard(sLock);
            auto it = sAilments.find(player->GetObjectGuid());
            if (it == sAilments.end())
                return 0;
            Affliction const* a = Find(it->second.list, from->GetObjectGuid(), TAG_FIRE);
            if (!a || !a->ticks)
                return 0;
            perTick = a->perTick;
        }
        std::vector<Unit*> const foes = NearestFoes(player, from, range, count);
        uint32 const now = WorldTimer::getMSTime();
        size_t spread = 0;
        std::lock_guard<std::mutex> guard(sLock);
        std::vector<Affliction>& list = sAilments[player->GetObjectGuid()].list;
        for (Unit* unit : foes)
        {
            if (!Scaled(unit) || !player->CanAttack(unit))
                continue;
            Affliction* a = Find(list, unit->GetObjectGuid(), TAG_FIRE);
            // The strongest counts: a stronger one there stays.
            if (a && a->ticks && a->perTick > perTick)
                continue;
            if (!a)
            {
                list.push_back(Affliction());
                a = &list.back();
                a->victim = unit->GetObjectGuid();
                a->tag = TAG_FIRE;
            }
            a->perTick = perTick;
            a->ticks = uint8(IGNITE_TICKS);
            a->nextTick = now + TICK_MS;
            ++spread;
        }
        return spread;
    }

    void ForgetAilments(Player* player)
    {
        std::lock_guard<std::mutex> guard(sLock);
        sAilments.erase(player->GetObjectGuid());
    }

    void HitExtraLines(Player const* player, SpellEntry const* spell, uint32 tags, std::vector<std::string>& out)
    {
        std::shared_ptr<WebTotals const> totals = TotalsOf(player);
        if (!totals)
            return;
        WebTotals const& t = *totals;
        bool const magic = spell && (spell->DmgClass == SPELL_DAMAGE_CLASS_MAGIC || spell->DmgClass == SPELL_DAMAGE_CLASS_NONE);
        out.push_back(Format("|cffffd200Critical strikes|r |cffffffffx%.2f|r |cff9d9d9d(%s %.1f, +1%% per %d Agility, gear)|r",
                             CritMultiplier(player, spell), magic ? "spells" : "attacks", magic ? 1.5f : 2.0f, AGI_PER_CRIT));
        bool const attack = (tags & TagBit(TAG_MELEE)) != 0;
        int32 const* added = attack ? t.addedAttack : t.addedSpell;
        for (uint8 tag = 0; tag < DAMAGE_TAGS; ++tag)
            if (added[tag] > 0)
                out.push_back(Format("|cffffd200Adds %u to %u %s|r |cff9d9d9d(a hit of its own, before its bonuses)|r",
                                     uint32(added[tag]) * ADDED_LOW_PCT / 100, uint32(added[tag]) * ADDED_HIGH_PCT / 100,
                                     TagName(Tag(tag))));
        int32 const inc = AilmentIncreased(player, t);
        for (uint8 tag = 0; tag < DAMAGE_TAGS; ++tag)
            if ((tags & TagBit(Tag(tag))) || added[tag] > 0)
                out.push_back(Format("|cffffd200%s|r |cff9d9d9don every critical strike, %d%% chance on any hit%s|r", AILMENT_NAME[tag],
                                     std::clamp(t.ailmentChance[tag], 0, 100),
                                     (tag == TAG_FROST || tag == TAG_SHADOW) ? "" : Format(", +%d%% ailment damage", inc).c_str()));
    }
}
