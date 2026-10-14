/*
 * ARPG druid: see ArpgDruid.h.
 */

#include "Arpg/ArpgDruid.h"
#include "Arpg/ArpgCharacter.h"
#include "Arpg/ArpgCombat.h"
#include "Arpg/ArpgDamage.h"
#include "Arpg/ArpgSkills.h"
#include "Arpg/ArpgTree.h"
#include "Arpg/ArpgUniques.h"

#include "Entities/Player.h"
#include "Spells/SpellAuras.h"
#include "Spells/SpellMgr.h"
#include "Util/Timer.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <unordered_map>

namespace
{
    using namespace Arpg;

    // Eclipse: mana on a critical Wrath or Starfire, in percent of the maximum, and Nature's
    // Grace (the next cast 0.5 sec quicker).
    constexpr uint32 ECLIPSE_MANA_PCT = 2;
    constexpr uint32 SPELL_NATURES_GRACE = 16886;
    // Lunar Tide: mana per tick in thousandths of the maximum, at most this a second.
    constexpr uint32 TIDE_PERMILLE = 5, TIDE_MAX_PERMILLE = 20;
    // Tempest: a strike's chance to hurl a Wrath, for this many times the strike, at most this many a second.
    constexpr uint32 TEMPEST_PCT = 10, TEMPEST_TIMES = 3, TEMPEST_MAX = 2;
    constexpr uint32 SPELL_WRATH = 5176;
    // Biting Hail: a strike's chance to chill, per rank.
    constexpr uint32 HAIL_PCT = 5;
    // Eye of the Wild: health per enemy struck, thousandths of the maximum per rank; at most this a second.
    constexpr uint32 EYE_PERMILLE = 3, EYE_MAX_PERMILLE = 30;
    // Bristling Fur: rage per rank (the wire's tenths), once a second.
    constexpr uint32 FUR_RAGE = 20, FUR_EVERY_MS = 1000;
    // Survival Instincts: a Bear hit heals this share per rank.
    constexpr uint32 INSTINCTS_PCT = 2;
    // Thorned Hide: a share of attack power back at each attacker, once a second each.
    constexpr uint32 THORNED_PCT = 30, THORNED_EVERY_MS = 1000;
    constexpr uint32 SPELL_THORNS = 467;
    // Savage Fury: a critical Claw, Shred or Rake gives this many more combo points.
    constexpr int8 SAVAGE_FURY_POINTS = 1;
    // Bloom: the heal's share at every enemy within this range.
    constexpr uint32 BLOOM_PCT = 50;
    constexpr float BLOOM_RANGE = 8.0f;
    constexpr uint32 BLOOM_LOOK = 740;               // Tranquility
    // Nature's Wrath: per rank, at the nearest enemy within this range.
    constexpr uint32 NATURES_WRATH_PCT = 15;
    constexpr float NATURES_WRATH_RANGE = 20.0f;
    // Verdant Fury: more damage per rank for this long after healing yourself.
    constexpr int32 VERDANT_PCT = 5;
    constexpr uint32 VERDANT_MS = 5000;
    // Wrath of the Wild: a Poison of this share of the heal, within this range, once every so often.
    constexpr uint32 WILD_PCT = 30, WILD_EVERY_MS = 3000;
    constexpr float WILD_RANGE = 8.0f;
    // King of the Jungle: kills this recent count, at most this many.
    constexpr uint32 KING_WINDOW_MS = 6000;
    constexpr size_t KING_MAX = 10;
    // Living Thorns: Thorns strikes every enemy this close this often.
    constexpr float LIVING_THORNS_RANGE = 8.0f;
    constexpr uint32 LIVING_THORNS_MS = 1000;
    // Heal-over-time ticks set off the heal hooks at most this often.
    constexpr uint32 HOT_EVERY_MS = 1000;
    // The areas are at most this many enemies.
    constexpr size_t AREA_MAX = 10;

    struct DruidState
    {
        uint32 second = 0;                    // the start of the current second's allowances
        uint32 tide = 0;                      // Lunar Tide's mana this second, in thousandths
        uint32 tempests = 0;                  // Tempest's Wraths this second
        uint32 eye = 0;                       // Eye of the Wild's health this second, in thousandths
        uint32 furAt = 0;                     // Bristling Fur's last rage
        uint32 wildAt = 0;                    // Wrath of the Wild's last Poison
        uint32 verdantUntil = 0;              // Verdant Fury's end
        uint32 thornsAt = 0;                  // Living Thorns' last pulse
        uint32 hotAt = 0;                     // the last heal-over-time tick that set off OnDruidHeal
        std::deque<uint32> kills;             // King of the Jungle
        std::map<ObjectGuid, uint32> thornedAt; // Thorned Hide: each attacker's last
    };

    std::mutex sLock;
    std::unordered_map<ObjectGuid, DruidState> sDruids;

    bool Named(SpellEntry const* spellInfo, char const* name)
    {
        if (!spellInfo || !spellInfo->SpellName[0])
            return false;
        size_t const n = std::strlen(name);
        char const* have = spellInfo->SpellName[0];
        return std::strncmp(have, name, n) == 0 && (have[n] == '\0' || have[n] == ' ');
    }

    // The state, its per-second allowances renewed. Under sLock.
    DruidState& StateOf(Player const* player, uint32 now)
    {
        DruidState& d = sDruids[player->GetObjectGuid()];
        if (!d.second || WorldTimer::getMSTimeDiff(d.second, now) >= 1000)
        {
            d.second = now ? now : 1;
            d.tide = d.tempests = d.eye = 0;
        }
        return d;
    }

    Player* DruidOf(Unit* unit)
    {
        if (!unit || unit->GetTypeId() != TYPEID_PLAYER)
            return nullptr;
        Player* player = static_cast<Player*>(unit);
        return player->getClass() == CLASS_DRUID && Active(player) ? player : nullptr;
    }

    // The damage a Thorns aura on `player` deals, or 0 for none.
    uint32 ThornsDamage(Player const* player)
    {
        for (Aura const* aura : player->GetAurasByType(SPELL_AURA_DAMAGE_SHIELD))
            if (aura->GetSpellProto()->SpellFamilyName == SPELLFAMILY_DRUID && Named(aura->GetSpellProto(), "Thorns"))
                return uint32(std::max(0, aura->GetModifier()->m_amount));
        return 0;
    }
}

namespace Arpg
{
    void OnDruidHit(Player* player, Unit* victim, SpellEntry const* spellInfo, uint32 dealt, bool crit)
    {
        if (!player || !victim || !spellInfo || player->getClass() != CLASS_DRUID)
            return;
        std::shared_ptr<WebTotals const> totals = TotalsOf(player);
        if (!totals)
            return;
        WebTotals const& t = *totals;

        if (crit && t.Has(KEY_ECLIPSE) && (Named(spellInfo, "Wrath") || Named(spellInfo, "Starfire")))
        {
            ProcMana(player, "Eclipse", player->GetMaxPower(POWER_MANA) * ECLIPSE_MANA_PCT / 100);
            player->CastSpell(player, SPELL_NATURES_GRACE, TRIGGERED_OLD_TRIGGERED);
        }

        if (crit && t.Has(KEY_SAVAGE_FURY) && victim->IsAlive() &&
                (Named(spellInfo, "Claw") || Named(spellInfo, "Shred") || Named(spellInfo, "Rake")))
        {
            player->AddComboPoints(victim, SAVAGE_FURY_POINTS);
            ReportProcOn(player, victim, 0xFF, PROC_EFFECT, 0, "Savage Fury", "gives an extra combo point.");
        }

        if (uint8 const rank = t.Rank(KEY_SURVIVAL_INSTINCTS))
            if (SkillOfSpell(CLASS_DRUID, spellInfo) == SKILL_BEAR && player->IsAlive())
                ProcHeal(player, "Survival Instincts", std::max<uint32>(1, FromDamage(player, victim, dealt) * INSTINCTS_PCT * rank / 100));
    }

    void OnDruidTick(Unit* caster, Unit* target, SpellEntry const* spellInfo, uint32 damage)
    {
        Player* player = DruidOf(caster);
        if (!player || !target || !spellInfo || !damage)
            return;
        std::shared_ptr<WebTotals const> totals = TotalsOf(player);
        if (!totals)
            return;
        WebTotals const& t = *totals;
        uint32 const now = WorldTimer::getMSTime();
        bool const swarm = Named(spellInfo, "Insect Swarm");
        bool const moon = Named(spellInfo, "Moonfire");

        if (swarm && t.Has(KEY_HIVE_MIND) && target->IsAlive())
            InflictAilment(player, target, TAG_NATURE, damage, false);

        if ((swarm || moon) && t.Has(KEY_LUNAR_TIDE))
        {
            uint32 give = 0;
            {
                std::lock_guard<std::mutex> guard(sLock);
                DruidState& d = StateOf(player, now);
                if (d.tide + TIDE_PERMILLE <= TIDE_MAX_PERMILLE)
                {
                    d.tide += TIDE_PERMILLE;
                    give = TIDE_PERMILLE;
                }
            }
            if (give)
                ProcMana(player, "Lunar Tide", std::max<uint32>(1, player->GetMaxPower(POWER_MANA) * give / 1000));
        }

        if (!Named(spellInfo, "Hurricane"))
            return;

        if (t.Has(KEY_TEMPEST) && target->IsAlive() && roll_chance_i(int32(TEMPEST_PCT)))
        {
            bool go = false;
            {
                std::lock_guard<std::mutex> guard(sLock);
                DruidState& d = StateOf(player, now);
                if (d.tempests < TEMPEST_MAX)
                {
                    ++d.tempests;
                    go = true;
                }
            }
            if (SpellEntry const* wrath = go ? sSpellTemplate.LookupEntry<SpellEntry>(SPELL_WRATH) : nullptr)
            {
                uint32 const amount = damage * TEMPEST_TIMES;
                SendBolt(player, target, wrath, SPELL_SCHOOL_MASK_NATURE, amount);
                ReportProcOn(player, target, TAG_NATURE, PROC_DAMAGE, amount, "Tempest", "hurls a Wrath at %s for %u.", target->GetName(), amount);
            }
        }

        if (uint8 const rank = t.Rank(KEY_BITING_HAIL))
            if (target->IsAlive() && !Frozen(target) && !Chilled(target) && roll_chance_i(int32(HAIL_PCT * rank)))
                Chill(player, target, false);

        if (uint8 const rank = t.Rank(KEY_EYE_OF_THE_WILD))
        {
            uint32 give = 0;
            {
                std::lock_guard<std::mutex> guard(sLock);
                DruidState& d = StateOf(player, now);
                uint32 const want = EYE_PERMILLE * rank;
                if (d.eye < EYE_MAX_PERMILLE)
                {
                    give = std::min(want, EYE_MAX_PERMILLE - d.eye);
                    d.eye += give;
                }
            }
            if (give && player->IsAlive())
                ProcHeal(player, "Eye of the Wild", std::max<uint32>(1, player->GetMaxHealth() * give / 1000));
        }
    }

    void OnDruidHeal(Spell* /*spell*/, Player* player, Unit* target, uint32 amount)
    {
        if (!player || !amount || player->getClass() != CLASS_DRUID)
            return;
        std::shared_ptr<WebTotals const> totals = TotalsOf(player);
        if (!totals)
            return;
        WebTotals const& t = *totals;
        uint32 const now = WorldTimer::getMSTime();
        float const area = SpellAreaScale(player);
        SpellEntry const* look = sSpellTemplate.LookupEntry<SpellEntry>(BLOOM_LOOK);
        SpellEntry const* wrath = sSpellTemplate.LookupEntry<SpellEntry>(SPELL_WRATH);

        if (uint8 const rank = t.Rank(KEY_NATURES_WRATH))
        {
            std::vector<Unit*> const foes = NearestFoes(player, player, NATURES_WRATH_RANGE, 1);
            if (!foes.empty() && wrath)
            {
                uint32 const dealt = std::max<uint32>(1, ToDamage(player, foes.front(), amount * NATURES_WRATH_PCT * rank / 100));
                SendBolt(player, foes.front(), wrath, SPELL_SCHOOL_MASK_NATURE, dealt);
                ReportProcOn(player, foes.front(), TAG_NATURE, PROC_DAMAGE, dealt, "Nature's Wrath", "strikes %s for %u.", foes.front()->GetName(), dealt);
            }
        }

        if (target == player && t.Has(KEY_VERDANT_FURY))
        {
            std::lock_guard<std::mutex> guard(sLock);
            StateOf(player, now).verdantUntil = now + VERDANT_MS;
        }

        if (t.Has(KEY_WRATH_OF_THE_WILD))
        {
            bool go = false;
            {
                std::lock_guard<std::mutex> guard(sLock);
                DruidState& d = StateOf(player, now);
                if (!d.wildAt || WorldTimer::getMSTimeDiff(d.wildAt, now) >= WILD_EVERY_MS)
                {
                    d.wildAt = now ? now : 1;
                    go = true;
                }
            }
            if (go)
            {
                std::vector<Unit*> const foes = NearestFoes(player, player, WILD_RANGE * area, AREA_MAX);
                for (Unit* unit : foes)
                    InflictAilment(player, unit, TAG_NATURE, ToDamage(player, unit, amount * WILD_PCT / 100), false);
                if (!foes.empty())
                    ReportProc(player, PROC_EFFECT, 0, "Wrath of the Wild", "poisons %u %s.", uint32(foes.size()), foes.size() == 1 ? "enemy" : "enemies");
            }
        }

        if (t.Has(KEY_BLOOM) && look)
        {
            std::vector<Unit*> const foes = NearestFoes(player, player, BLOOM_RANGE * area, AREA_MAX);
            for (Unit* unit : foes)
                StrikeFoe(player, unit, look, SPELL_SCHOOL_MASK_NATURE, std::max<uint32>(1, ToDamage(player, unit, amount * BLOOM_PCT / 100)));
            if (!foes.empty())
                ReportProc(player, PROC_DAMAGE, 0, "Bloom", "bursts onto %u %s.", uint32(foes.size()), foes.size() == 1 ? "enemy" : "enemies");
        }
    }

    void OnDruidHotTick(Unit* caster, Unit* target, SpellEntry const* spellInfo, uint32 amount)
    {
        Player* player = DruidOf(caster);
        if (!player || !target || !amount || SkillOfSpell(CLASS_DRUID, spellInfo) != SKILL_REJUVENATION)
            return;
        uint32 const now = WorldTimer::getMSTime();
        {
            std::lock_guard<std::mutex> guard(sLock);
            DruidState& d = StateOf(player, now);
            if (d.hotAt && WorldTimer::getMSTimeDiff(d.hotAt, now) < HOT_EVERY_MS)
                return;
            d.hotAt = now ? now : 1;
        }
        OnDruidHeal(nullptr, player, target, amount);
    }

    void OnDruidStruck(Unit* victim, Unit* attacker)
    {
        Player* player = DruidOf(victim);
        if (!player || !attacker || attacker == victim || !attacker->IsAlive() || !player->IsAlive() ||
                player->GetShapeshiftForm() != FORM_BEAR && player->GetShapeshiftForm() != FORM_DIREBEAR)
            return;
        std::shared_ptr<WebTotals const> totals = TotalsOf(player);
        if (!totals)
            return;
        WebTotals const& t = *totals;
        uint32 const now = WorldTimer::getMSTime();

        if (uint8 const rank = t.Rank(KEY_BRISTLING_FUR))
        {
            bool go = false;
            {
                std::lock_guard<std::mutex> guard(sLock);
                DruidState& d = StateOf(player, now);
                if (!d.furAt || WorldTimer::getMSTimeDiff(d.furAt, now) >= FUR_EVERY_MS)
                {
                    d.furAt = now ? now : 1;
                    go = true;
                }
            }
            if (go)
                player->ModifyPower(POWER_RAGE, int32(FUR_RAGE * rank));
        }

        if (t.Has(KEY_THORNED_HIDE) && attacker->GetTypeId() == TYPEID_UNIT)
        {
            bool go = false;
            {
                std::lock_guard<std::mutex> guard(sLock);
                std::map<ObjectGuid, uint32>& at = StateOf(player, now).thornedAt;
                auto it = at.find(attacker->GetObjectGuid());
                if (it == at.end() || WorldTimer::getMSTimeDiff(it->second, now) >= THORNED_EVERY_MS)
                {
                    // Forget the attackers long gone.
                    if (at.size() > 32)
                        for (auto old = at.begin(); old != at.end();)
                            old = WorldTimer::getMSTimeDiff(old->second, now) >= THORNED_EVERY_MS ? at.erase(old) : std::next(old);
                    at[attacker->GetObjectGuid()] = now;
                    go = true;
                }
            }
            SpellEntry const* thorns = go ? sSpellTemplate.LookupEntry<SpellEntry>(SPELL_THORNS) : nullptr;
            if (thorns)
            {
                uint32 const ap = uint32(std::max(0.0f, player->GetTotalAttackPowerValue(BASE_ATTACK)));
                uint32 const amount = std::max<uint32>(1, ToDamage(player, attacker, ap * THORNED_PCT / 100));
                StrikeFoe(player, attacker, thorns, SPELL_SCHOOL_MASK_NATURE, amount);
            }
        }
    }

    void OnDruidKill(Player* killer)
    {
        if (!killer || killer->getClass() != CLASS_DRUID || !HasKeystone(killer, KEY_KING_OF_THE_JUNGLE))
            return;
        uint32 const now = WorldTimer::getMSTime();
        std::lock_guard<std::mutex> guard(sLock);
        std::deque<uint32>& kills = StateOf(killer, now).kills;
        kills.push_back(now);
        while (kills.size() > KING_MAX)
            kills.pop_front();
    }

    uint32 KingStacks(Player const* player)
    {
        uint32 const now = WorldTimer::getMSTime();
        std::lock_guard<std::mutex> guard(sLock);
        auto it = sDruids.find(player->GetObjectGuid());
        if (it == sDruids.end())
            return 0;
        uint32 n = 0;
        for (uint32 at : it->second.kills)
            if (WorldTimer::getMSTimeDiff(at, now) <= KING_WINDOW_MS)
                ++n;
        return n;
    }

    int32 VerdantFuryPct(Player const* player)
    {
        uint8 const rank = KeyRank(player, KEY_VERDANT_FURY);
        if (!rank)
            return 0;
        uint32 const now = WorldTimer::getMSTime();
        std::lock_guard<std::mutex> guard(sLock);
        auto it = sDruids.find(player->GetObjectGuid());
        return it != sDruids.end() && it->second.verdantUntil && now < it->second.verdantUntil ? VERDANT_PCT * rank : 0;
    }

    void UpdateDruid(Player* player)
    {
        if (!player || player->getClass() != CLASS_DRUID || !player->IsAlive() || !HasKeystone(player, KEY_LIVING_THORNS))
            return;
        uint32 const now = WorldTimer::getMSTime();
        {
            std::lock_guard<std::mutex> guard(sLock);
            DruidState& d = StateOf(player, now);
            if (d.thornsAt && WorldTimer::getMSTimeDiff(d.thornsAt, now) < LIVING_THORNS_MS)
                return;
            d.thornsAt = now ? now : 1;
        }
        uint32 const thornsDamage = ThornsDamage(player);
        SpellEntry const* thorns = sSpellTemplate.LookupEntry<SpellEntry>(SPELL_THORNS);
        if (!thornsDamage || !thorns)
            return;
        for (Unit* unit : NearestFoes(player, player, LIVING_THORNS_RANGE * SpellAreaScale(player), AREA_MAX))
            if (unit->IsInCombat())
                StrikeFoe(player, unit, thorns, SPELL_SCHOOL_MASK_NATURE, std::max<uint32>(1, ToDamage(player, unit, thornsDamage)));
    }

    bool CarriesComboPoints(Unit const* unit)
    {
        // A druid's only: a warrior's "point" marks a dodge for Overpower on that enemy alone.
        return unit && unit->GetTypeId() == TYPEID_PLAYER && unit->getClass() == CLASS_DRUID && Active(unit);
    }

    void ForgetDruid(Player* player)
    {
        std::lock_guard<std::mutex> guard(sLock);
        sDruids.erase(player->GetObjectGuid());
    }
}
