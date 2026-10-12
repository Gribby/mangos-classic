/*
 * ARPG mage: see ArpgMage.h.
 */

#include "Arpg/ArpgMage.h"
#include "Arpg/ArpgCharacter.h"
#include "Arpg/ArpgCombat.h"
#include "Arpg/ArpgDamage.h"
#include "Arpg/ArpgTree.h"
#include "Arpg/ArpgUniques.h"

#include "Entities/Player.h"
#include "Spells/SpellMgr.h"
#include "Util/Timer.h"

#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>

namespace
{
    using namespace Arpg;

    // Pyroclasm: a critical Fireball or Pyroblast explodes for this share within this range.
    constexpr uint32 PYROCLASM_PCT = 60;
    constexpr float PYROCLASM_RANGE = 8.0f;
    // Shatterstorm: a Frostbolt on a frozen enemy shatters for this share within this range.
    constexpr uint32 SHATTERSTORM_PCT = 80;
    constexpr float SHATTERSTORM_RANGE = 8.0f;
    // Arcane Siphon: mana per missile, in thousandths of the maximum.
    constexpr uint32 SIPHON_PERMILLE = 5;
    // Mana Burst: mana per enemy hit (percent of the maximum), at most this many enemies a cast,
    // hits this close together being one cast.
    constexpr uint32 MANA_BURST_PCT = 1, MANA_BURST_MAX = 5, MANA_BURST_CAST_MS = 500;
    // Detonate: the Ignite's burn left strikes the target and those within this range.
    constexpr float DETONATE_RANGE = 6.0f;
    // Glacial Storm: a tick's chance to freeze an enemy chilled already.
    constexpr uint32 GLACIAL_FREEZE_PCT = 15;
    // Wildfire: an Ignite spreads to this many within this range.
    constexpr uint32 WILDFIRE_COUNT = 3;
    constexpr float WILDFIRE_RANGE = 8.0f;
    // Permafrost: each attacker is chilled at most once every this long.
    constexpr uint32 PERMAFROST_MS = 3000;
    // The burst areas are at most this many enemies.
    constexpr size_t AREA_MAX = 10;
    // The stock spells whose hits stand for the explosions: Blast Wave, Frost Nova.
    constexpr uint32 FIRE_LOOK = 11113, FROST_LOOK = 122;
    // Fire Vulnerability (Improved Scorch's).
    constexpr uint32 SPELL_FIRE_VULNERABILITY = 22959;

    struct MageState
    {
        uint32 burstAt = 0;                       // Mana Burst: the cast's first hit
        uint32 burstCount = 0;                    // and the enemies it has paid for
        std::map<ObjectGuid, uint32> struckAt;    // Permafrost: when each attacker was last chilled
    };

    std::mutex sLock;
    std::unordered_map<ObjectGuid, MageState> sMages;

    bool Named(SpellEntry const* spellInfo, char const* name)
    {
        if (!spellInfo || !spellInfo->SpellName[0])
            return false;
        size_t const n = std::strlen(name);
        char const* have = spellInfo->SpellName[0];
        return std::strncmp(have, name, n) == 0 && (have[n] == '\0' || have[n] == ' ');
    }

    // `share`% of `dealt` to every enemy within `range` (spell area widens it) of `center`, not
    // `center` itself; how many.
    size_t Explode(Player* player, Unit* center, SpellEntry const* spellInfo, uint32 schoolMask, uint32 dealt, uint32 share,
                   float range, uint32 lookId)
    {
        uint32 const amount = std::max<uint32>(1, dealt * share / 100);
        SpellEntry const* look = sSpellTemplate.LookupEntry<SpellEntry>(lookId);
        std::vector<Unit*> const foes = NearestFoes(player, center, range * SpellAreaScale(player), AREA_MAX);
        for (Unit* unit : foes)
            StrikeFoe(player, unit, spellInfo, schoolMask, amount, look);
        return foes.size();
    }
}

namespace Arpg
{
    void OnMageHit(Player* player, Unit* victim, SpellEntry const* spellInfo, uint32 dealt, bool crit)
    {
        if (!player || !victim || !spellInfo || player->getClass() != CLASS_MAGE)
            return;
        std::shared_ptr<WebTotals const> totals = TotalsOf(player);
        if (!totals)
            return;
        WebTotals const& t = *totals;
        uint32 const school = spellInfo->GetSchoolMask() ? spellInfo->GetSchoolMask() : uint32(SPELL_SCHOOL_MASK_NORMAL);

        if (t.Has(KEY_PYROCLASM) && crit && (Named(spellInfo, "Fireball") || Named(spellInfo, "Pyroblast")))
            if (size_t n = Explode(player, victim, spellInfo, school, dealt, PYROCLASM_PCT, PYROCLASM_RANGE, FIRE_LOOK))
                ReportProc(player, PROC_DAMAGE, 0, "Pyroclasm", "explodes onto %u %s for %u.", uint32(n), n == 1 ? "enemy" : "enemies",
                           dealt * PYROCLASM_PCT / 100);

        if (t.Has(KEY_SHATTERSTORM) && Named(spellInfo, "Frostbolt") && Frozen(victim))
            if (size_t n = Explode(player, victim, spellInfo, school, dealt, SHATTERSTORM_PCT, SHATTERSTORM_RANGE, FROST_LOOK))
                ReportProc(player, PROC_DAMAGE, 0, "Shatterstorm", "shatters onto %u %s for %u.", uint32(n), n == 1 ? "enemy" : "enemies",
                           dealt * SHATTERSTORM_PCT / 100);

        if (t.Has(KEY_DEEP_FREEZE) && Named(spellInfo, "Cone of Cold") && victim->IsAlive() && !Frozen(victim))
            Chill(player, victim, true);

        if (t.Has(KEY_ARCANE_SIPHON) && Named(spellInfo, "Arcane Missiles"))
            ProcMana(player, "Arcane Siphon", std::max<uint32>(1, player->GetMaxPower(POWER_MANA) * SIPHON_PERMILLE / 1000));

        if (t.Has(KEY_MANA_BURST) && Named(spellInfo, "Arcane Explosion"))
        {
            bool pay = false;
            {
                uint32 const now = WorldTimer::getMSTime();
                std::lock_guard<std::mutex> guard(sLock);
                MageState& m = sMages[player->GetObjectGuid()];
                if (!m.burstAt || WorldTimer::getMSTimeDiff(m.burstAt, now) > MANA_BURST_CAST_MS)
                {
                    m.burstAt = now ? now : 1;
                    m.burstCount = 0;
                }
                if (m.burstCount < MANA_BURST_MAX)
                {
                    ++m.burstCount;
                    pay = true;
                }
            }
            if (pay)
                ProcMana(player, "Mana Burst", player->GetMaxPower(POWER_MANA) * MANA_BURST_PCT / 100);
        }

        if (t.Has(KEY_SEARING_BRAND) && Named(spellInfo, "Scorch") && victim->IsAlive())
            player->CastSpell(victim, SPELL_FIRE_VULNERABILITY, TRIGGERED_OLD_TRIGGERED);

        if (Named(spellInfo, "Fire Blast"))
        {
            if (t.Has(KEY_DETONATE))
                if (uint32 const burn = TakeIgnite(player, victim))
                {
                    SpellEntry const* look = sSpellTemplate.LookupEntry<SpellEntry>(FIRE_LOOK);
                    if (victim->IsAlive())
                        StrikeFoe(player, victim, spellInfo, SPELL_SCHOOL_MASK_FIRE, burn, look);
                    size_t const n = Explode(player, victim, spellInfo, SPELL_SCHOOL_MASK_FIRE, burn, 100, DETONATE_RANGE, FIRE_LOOK);
                    ReportProc(player, PROC_DAMAGE, 0, "Detonate", "sets off the Ignite on %s for %u, striking %u more.", victim->GetName(), burn,
                               uint32(n));
                }
            if (t.Has(KEY_CHAIN_BLAST) && !victim->IsAlive())
            {
                player->RemoveSpellCooldown(*spellInfo, true);
                ReportProc(player, PROC_EFFECT, 0, "Chain Blast", "readies Fire Blast again.");
            }
        }
    }

    void OnPeriodicHit(Unit* caster, Unit* target, SpellEntry const* spellInfo)
    {
        if (!caster || !target || !spellInfo || caster->GetTypeId() != TYPEID_PLAYER || !target->IsAlive())
            return;
        Player* player = static_cast<Player*>(caster);
        if (player->getClass() != CLASS_MAGE || !Active(player) || !HasKeystone(player, KEY_GLACIAL_STORM) || !Named(spellInfo, "Blizzard"))
            return;
        if (Frozen(target))
            return;
        if (Chilled(target))
        {
            if (roll_chance_i(int32(GLACIAL_FREEZE_PCT)))
                Chill(player, target, true);
        }
        else
            Chill(player, target, false);
    }

    void OnMageKill(Player* killer, Unit* victim)
    {
        if (!killer || !victim || killer->getClass() != CLASS_MAGE || !HasKeystone(killer, KEY_WILDFIRE))
            return;
        if (size_t n = SpreadIgnite(killer, victim, WILDFIRE_RANGE * SpellAreaScale(killer), WILDFIRE_COUNT))
            ReportProc(killer, PROC_EFFECT, 0, "Wildfire", "spreads the Ignite to %u %s.", uint32(n), n == 1 ? "enemy" : "enemies");
    }

    void OnMageStruck(Unit* victim, Unit* attacker)
    {
        if (!victim || !attacker || victim == attacker || victim->GetTypeId() != TYPEID_PLAYER || !attacker->IsAlive())
            return;
        Player* player = static_cast<Player*>(victim);
        if (player->getClass() != CLASS_MAGE || !HasKeystone(player, KEY_PERMAFROST) || Frozen(attacker))
            return;
        {
            uint32 const now = WorldTimer::getMSTime();
            std::lock_guard<std::mutex> guard(sLock);
            std::map<ObjectGuid, uint32>& struck = sMages[player->GetObjectGuid()].struckAt;
            auto it = struck.find(attacker->GetObjectGuid());
            if (it != struck.end() && WorldTimer::getMSTimeDiff(it->second, now) < PERMAFROST_MS)
                return;
            // Forget the attackers long gone.
            if (struck.size() > 32)
                for (auto old = struck.begin(); old != struck.end();)
                    old = WorldTimer::getMSTimeDiff(old->second, now) >= PERMAFROST_MS ? struck.erase(old) : std::next(old);
            struck[attacker->GetObjectGuid()] = now;
        }
        Chill(player, attacker, Chilled(attacker));
    }

    void ForgetMage(Player* player)
    {
        std::lock_guard<std::mutex> guard(sLock);
        sMages.erase(player->GetObjectGuid());
    }
}
