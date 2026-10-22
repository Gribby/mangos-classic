/*
 * ARPG warlock (Arpg/ArpgWarlock.h).
 */

#include "Arpg/ArpgWarlock.h"
#include "Arpg/ArpgBoons.h"
#include "Arpg/ArpgCombat.h"
#include "Arpg/ArpgDamage.h"
#include "Arpg/ArpgMinions.h"
#include "Arpg/ArpgStates.h"
#include "Arpg/ArpgTags.h"
#include "Arpg/ArpgTree.h"
#include "Arpg/ArpgUniques.h"

#include "Entities/Creature.h"
#include "Entities/Pet.h"
#include "Entities/Player.h"
#include "Spells/Spell.h"
#include "Spells/SpellAuras.h"
#include "Spells/SpellMgr.h"
#include "Util/Timer.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace
{
    using namespace Arpg;

    constexpr uint32 ENTRY_IMP = 416, ENTRY_INFERNAL = 89, ENTRY_DOOMGUARD = 11859;
    constexpr uint32 IMP_LIFE_MS = 20000, INFERNAL_LIFE_MS = 60000, DOOMGUARD_LIFE_MS = 60000, PIT_LORD_LIFE_MS = 600000;
    constexpr uint32 STUN = 56, WAR_STOMP = 20549;
    constexpr uint32 HELLSTORM_EVERY = 4000;
    constexpr uint32 SHADOWBURN_WINDOW = 1500;
    constexpr uint32 NIGHTFALL_PER_RANK = 2, TORMENTED_PER_RANK = 10, IMP_GATE_PER_RANK = 25, BURNING_RAIN_PER_RANK = 10;
    // Soul Feast: per tick, and the most a second, in thousandths of maximum health.
    constexpr uint32 SOUL_FEAST_PERMILLE = 3, SOUL_FEAST_MAX_PERMILLE = 30;
    constexpr uint32 SOUL_SIPHON_PER_DOT = 2, SOUL_SIPHON_MAX_DOTS = 10;
    constexpr uint32 LIFE_LINK_PCT = 50, REAPING_MANA_PCT = 5, DEMONIC_PACT_PCT = 5;
    constexpr uint32 SOUL_REAPER_BELOW_PCT = 25;
    constexpr uint32 FEL_DOMINATION_EVERY = 30000;
    constexpr uint32 TORMENT_MS = 4000;
    constexpr int32 TORMENT_PCT = 20;
    constexpr uint32 SACRIFICE_PCT = 25;
    constexpr uint32 INFERNAL_HEAT_MS = 4000;
    constexpr int32 HELLFIRE_WARD_PER_RANK = 25, BACKLASH_PCT = 20, SOUL_LINK_PER_RANK = 10;
    constexpr uint32 FEL_BOND_PCT = 2;
    // Infernal Fall: its reach by rank, and its strike, a share of the owner's level.
    constexpr float INFERNAL_FALL_REACH[3] = { 8.0f, 10.0f, 12.0f };
    constexpr uint32 INFERNAL_FALL_PER_LEVEL = 8;

    struct WarlockState
    {
        ObjectGuid lastVictim;
        uint32 lastSpell = 0;          // the last spell its own hit landed with
        uint32 lastHitAt = 0;
        uint32 summonSpell = 0;        // the last demon summoned, for Fel Domination
        bool hadPet = false;
        bool resummonDue = false;      // its demon died: summon it again once it is gone
        bool fellWithDemon = false;    // it died with a demon out: the demon comes back as it rises
        uint32 felDominationAt = 0;
        uint32 feastAt = 0, feastGiven = 0;
        std::unordered_map<ObjectGuid, uint32> stunnedAt; // Hellstorm, per enemy
    };

    std::mutex sLock;
    std::unordered_map<ObjectGuid, WarlockState> sWarlocks;

    Player* Warlock(Unit* unit)
    {
        if (!unit || unit->GetTypeId() != TYPEID_PLAYER || unit->getClass() != CLASS_WARLOCK || !Active(unit))
            return nullptr;
        return static_cast<Player*>(unit);
    }

    bool Named(SpellEntry const* spellInfo, char const* name)
    {
        return spellInfo && spellInfo->SpellName[0] && std::strcmp(spellInfo->SpellName[0], name) == 0;
    }

    bool Prefixed(SpellEntry const* spellInfo, char const* prefix)
    {
        return spellInfo && spellInfo->SpellName[0] && std::strncmp(spellInfo->SpellName[0], prefix, std::strlen(prefix)) == 0;
    }

    // The highest rank of the spell `name` the player knows (0: none).
    uint32 HighestRank(Player const* player, char const* name)
    {
        uint32 best = 0, bestLevel = 0;
        for (auto const& [id, spell] : player->GetSpellMap())
        {
            if (spell.state == PLAYERSPELL_REMOVED || !spell.active)
                continue;
            SpellEntry const* info = sSpellTemplate.LookupEntry<SpellEntry>(id);
            if (Named(info, name) && (!best || info->spellLevel > bestLevel))
            {
                best = id;
                bestLevel = info->spellLevel;
            }
        }
        return best;
    }

    // The player's damage over time on `target`.
    uint32 DotsOn(Player const* player, Unit const* target)
    {
        uint32 n = 0;
        for (auto const& [id, holder] : target->GetSpellAuraHolderMap())
            if (holder->GetCasterGuid() == player->GetObjectGuid())
                for (uint8 i = 0; i < MAX_EFFECT_INDEX; ++i)
                    if (holder->GetSpellProto()->EffectApplyAuraName[i] == SPELL_AURA_PERIODIC_DAMAGE ||
                            holder->GetSpellProto()->EffectApplyAuraName[i] == SPELL_AURA_PERIODIC_LEECH)
                    {
                        ++n;
                        break;
                    }
        return n;
    }

    bool HasOwnAura(Unit const* target, Player const* player, char const* name)
    {
        for (auto const& [id, holder] : target->GetSpellAuraHolderMap())
            if (holder->GetCasterGuid() == player->GetObjectGuid() && Named(holder->GetSpellProto(), name))
                return true;
        return false;
    }

    // Fel Ward (the class's own): every kill, the warlock's or a minion's, gives Barrier of a share
    // of maximum health, more under Demon Skin or Demon Armor (Arpg/ArpgBoons.h). A caster in the
    // middle of packs needs a layer that grows with the killing (2026-10-19: 11.8 deaths an hour at
    // levels 10-14, all attrition from melee packs).
    constexpr uint32 FEL_WARD_PCT = 5, FEL_WARD_ARMOR_PCT = 8;

    void FelWard(Player* player)
    {
        if (!player || !player->IsAlive())
            return;
        bool const armored = HasOwnAura(player, player, "Demon Skin") || HasOwnAura(player, player, "Demon Armor");
        GainBarrier(player, std::max<uint32>(1, player->GetMaxHealth() * (armored ? FEL_WARD_ARMOR_PCT : FEL_WARD_PCT) / 100));
    }

    void RaiseImp(Player* player, Unit* at)
    {
        SummonHorde(player, ENTRY_IMP, MINION_IMP, at->GetPositionX(), at->GetPositionY(), at->GetPositionZ(), IMP_LIFE_MS);
    }

    // Hellstorm's stun on `victim`, once every HELLSTORM_EVERY an enemy.
    void Hellstorm(Player* player, Unit* victim, uint32 now)
    {
        bool stun = false;
        {
            std::lock_guard<std::mutex> guard(sLock);
            auto& stunned = sWarlocks[player->GetObjectGuid()].stunnedAt;
            // Old enemies go, so the map stays small.
            if (stunned.size() > 64)
                for (auto it = stunned.begin(); it != stunned.end();)
                    it = WorldTimer::getMSTimeDiff(it->second, now) >= HELLSTORM_EVERY ? stunned.erase(it) : std::next(it);
            uint32& at = stunned[victim->GetObjectGuid()];
            if (!at || WorldTimer::getMSTimeDiff(at, now) >= HELLSTORM_EVERY)
            {
                at = now ? now : 1;
                stun = true;
            }
        }
        if (stun)
            player->CastSpell(victim, STUN, TRIGGERED_OLD_TRIGGERED);
    }
}

namespace Arpg
{
    bool IsArpgSummon(SpellEntry const* spellInfo)
    {
        if (!spellInfo || spellInfo->SpellFamilyName != SPELLFAMILY_WARLOCK)
            return false;
        for (uint8 i = 0; i < MAX_EFFECT_INDEX; ++i)
            if (spellInfo->Effect[i] == SPELL_EFFECT_SUMMON_PET)
                return true;
        return Named(spellInfo, "Inferno") || Named(spellInfo, "Ritual of Doom");
    }

    void OnWarlockHit(Player* player, Unit* victim, SpellEntry const* spellInfo, uint32 dealt, bool crit)
    {
        if (!player || player->getClass() != CLASS_WARLOCK || !spellInfo)
            return;
        std::shared_ptr<WebTotals const> totals = TotalsOf(player);
        if (!totals)
            return;
        WebTotals const& t = *totals;
        uint32 const now = WorldTimer::getMSTime();
        {
            std::lock_guard<std::mutex> guard(sLock);
            WarlockState& w = sWarlocks[player->GetObjectGuid()];
            w.lastVictim = victim->GetObjectGuid();
            w.lastSpell = spellInfo->Id;
            w.lastHitAt = now;
        }
        bool const hellfire = Prefixed(spellInfo, "Hellfire");
        if (hellfire && t.Has(KEY_INFERNAL_HEAT))
            HeatMinions(player, INFERNAL_HEAT_MS);
        if (!victim->IsAlive())
            return;
        // Hellstorm: a critical Hellfire stuns, once every 4 sec an enemy (Rain of Fire's ticks,
        // which can't crit, stun an Ignited enemy instead: OnWarlockTick).
        if (crit && hellfire && t.Has(KEY_HELLSTORM))
            Hellstorm(player, victim, now);
    }

    void OnWarlockTick(Unit* caster, Unit* target, SpellEntry const* spellInfo, uint32 damage)
    {
        Player* player = Warlock(caster);
        if (!player || !target || !spellInfo || !damage)
            return;
        std::shared_ptr<WebTotals const> totals = TotalsOf(player);
        if (!totals)
            return;
        WebTotals const& t = *totals;
        bool const corruption = Named(spellInfo, "Corruption");
        bool const agony = Named(spellInfo, "Curse of Agony");
        if ((corruption || agony) && target->IsAlive() && t.Has(KEY_NIGHTFALL) &&
                roll_chance_i(int32(NIGHTFALL_PER_RANK * t.Rank(KEY_NIGHTFALL))))
            if (uint32 const bolt = HighestRank(player, "Shadow Bolt"))
            {
                player->CastSpell(target, bolt, TRIGGERED_OLD_TRIGGERED);
                ReportProc(player, PROC_EFFECT, 0, "Nightfall", "looses a free Shadow Bolt.");
            }
        if (agony && target->IsAlive() && t.Has(KEY_TORMENTED_SOUL) &&
                roll_chance_i(int32(TORMENTED_PER_RANK * t.Rank(KEY_TORMENTED_SOUL))))
            InflictAilment(player, target, TAG_SHADOW, damage, false);
        // Rain of Fire's nodes, on its ticks (it is an area aura: it never lands as a direct hit).
        if (Prefixed(spellInfo, "Rain of Fire") && target->IsAlive())
        {
            if (t.Has(KEY_BURNING_RAIN) && roll_chance_i(int32(BURNING_RAIN_PER_RANK * t.Rank(KEY_BURNING_RAIN))))
                InflictAilment(player, target, TAG_FIRE, damage, false);
            bool const ignited = HasState(target, STATE_IGNITED);
            // Firestorm: a tick on an Ignited enemy falls twice.
            if (ignited && t.Has(KEY_FIRESTORM))
                StrikeTick(player, target, spellInfo, spellInfo->GetSchoolMask(), damage);
            if (ignited && t.Has(KEY_HELLSTORM) && target->IsAlive())
                Hellstorm(player, target, WorldTimer::getMSTime());
        }
        bool const drainLife = Named(spellInfo, "Drain Life");
        uint32 const size = FromDamage(player, target, damage);
        if (drainLife && t.Has(KEY_LIFE_LINK))
            HealMinions(player, std::max<uint32>(1, size * LIFE_LINK_PCT / 100));
        // Soul Siphon: the drains heal more for each of your damage over time on the target.
        if ((drainLife || Named(spellInfo, "Drain Soul")) && t.Has(KEY_SOUL_SIPHON) && player->IsAlive())
        {
            uint32 const dots = std::min(DotsOn(player, target), SOUL_SIPHON_MAX_DOTS);
            if (dots)
                player->ModifyHealth(int32(std::max<uint32>(1, size * SOUL_SIPHON_PER_DOT * dots / 100)));
        }
        // Soul Feast: every damage over time tick heals, up to a share a second.
        if (t.Has(KEY_SOUL_FEAST) && player->IsAlive() && player->GetHealth() < player->GetMaxHealth())
        {
            uint32 const now = WorldTimer::getMSTime();
            uint32 const per = std::max<uint32>(1, player->GetMaxHealth() * SOUL_FEAST_PERMILLE / 1000);
            uint32 const cap = player->GetMaxHealth() * SOUL_FEAST_MAX_PERMILLE / 1000;
            bool heal = false;
            {
                std::lock_guard<std::mutex> guard(sLock);
                WarlockState& w = sWarlocks[player->GetObjectGuid()];
                if (!w.feastAt || WorldTimer::getMSTimeDiff(w.feastAt, now) >= 1000)
                {
                    w.feastAt = now ? now : 1;
                    w.feastGiven = 0;
                }
                if (w.feastGiven + per <= std::max(cap, per))
                {
                    w.feastGiven += per;
                    heal = true;
                }
            }
            if (heal)
                player->ModifyHealth(int32(per));
        }
    }

    void OnWarlockKill(Player* killer, Unit* victim)
    {
        if (!killer || killer->getClass() != CLASS_WARLOCK || !Active(killer) || !victim ||
                victim->GetTypeId() != TYPEID_UNIT)
            return;
        FelWard(killer);
        std::shared_ptr<WebTotals const> totals = TotalsOf(killer);
        if (!totals)
            return;
        WebTotals const& t = *totals;
        // The horde from the kill: Legion always, Imp Gate by chance, Reaping under Drain Soul.
        bool const drained = HasOwnAura(victim, killer, "Drain Soul");
        if (t.Has(KEY_LEGION) || (t.Has(KEY_IMP_GATE) && roll_chance_i(int32(IMP_GATE_PER_RANK * t.Rank(KEY_IMP_GATE)))) ||
                (drained && t.Has(KEY_REAPING)))
            RaiseImp(killer, victim);
        // Imp Gate under Legion (which raises one always): its chance raises a second.
        if (t.Has(KEY_LEGION) && t.Has(KEY_IMP_GATE) && roll_chance_i(int32(IMP_GATE_PER_RANK * t.Rank(KEY_IMP_GATE))))
            RaiseImp(killer, victim);
        if (drained && t.Has(KEY_REAPING) && killer->GetPowerType() == POWER_MANA)
            killer->ModifyPower(POWER_MANA, int32(killer->GetMaxPower(POWER_MANA) * REAPING_MANA_PCT / 100));
        // Shadowburn: a kill with it readies it again.
        if (t.Has(KEY_SHADOWBURN))
        {
            uint32 spell = 0;
            {
                std::lock_guard<std::mutex> guard(sLock);
                WarlockState const& w = sWarlocks[killer->GetObjectGuid()];
                if (w.lastVictim == victim->GetObjectGuid() && WorldTimer::getMSTimeDiff(w.lastHitAt, WorldTimer::getMSTime()) <= SHADOWBURN_WINDOW)
                    spell = w.lastSpell;
            }
            SpellEntry const* info = spell ? sSpellTemplate.LookupEntry<SpellEntry>(spell) : nullptr;
            if (Named(info, "Shadowburn"))
            {
                killer->RemoveSpellCooldown(*info, true);
                ReportProc(killer, PROC_EFFECT, 0, "Shadowburn", "is ready again.");
            }
        }
    }

    void OnMinionKill(Unit* killer, Unit* victim)
    {
        Player* owner = MinionOwner(killer);
        if (!owner || owner->getClass() != CLASS_WARLOCK || !victim || victim->GetTypeId() != TYPEID_UNIT)
            return;
        std::shared_ptr<WebTotals const> totals = TotalsOf(owner);
        if (!totals)
            return;
        // Fel Bond (the class's own): a minion's kill of a Withered enemy heals its owner.
        if ((StateMask(victim, true) & StateBit(STATE_WITHERED)) && owner->IsAlive())
            ProcHeal(owner, "Fel Bond", std::max<uint32>(1, owner->GetMaxHealth() * FEL_BOND_PCT / 100));
        // Burning Legion: a horde minion's kill raises another imp.
        if (totals->Has(KEY_BURNING_LEGION) && HordeKind(killer) != MINION_NONE)
            RaiseImp(owner, victim);
    }

    void OnWarlockCast(Spell* spell)
    {
        if (!spell)
            return;
        Player* player = Warlock(spell->GetCaster());
        if (!player || spell->m_IsTriggeredSpell)
            return;
        SpellEntry const* info = spell->m_spellInfo;
        std::shared_ptr<WebTotals const> totals = TotalsOf(player);
        if (!totals)
            return;
        WebTotals const& t = *totals;
        Unit* target = spell->m_targets.getUnitTarget();
        // A spell at an enemy points the minions at it; a curse (Torment) commands them.
        if (target && target != player && player->CanAttack(target))
        {
            if (t.Has(KEY_TORMENT_COMMAND) && (TagsOf(info) & TagBit(TAG_CURSE)))
            {
                CommandMinions(player, target, TORMENT_MS, TORMENT_PCT);
                ReportProc(player, PROC_EFFECT, 0, "Torment", "sends your minions at %s.", target->GetName());
            }
            else
                OnOwnerStruck(player, target);
        }
        // A demon's summoning, remembered for Fel Domination.
        for (uint8 i = 0; i < MAX_EFFECT_INDEX; ++i)
            if (info->Effect[i] == SPELL_EFFECT_SUMMON_PET)
            {
                std::lock_guard<std::mutex> guard(sLock);
                sWarlocks[player->GetObjectGuid()].summonSpell = info->Id;
            }
        if (Named(info, "Life Tap"))
        {
            if (t.Has(KEY_DARK_PACT) && player->GetPowerType() == POWER_MANA)
                player->ModifyPower(POWER_MANA, int32(player->GetMaxPower(POWER_MANA) * t.Rank(KEY_DARK_PACT) / 100));
            if (t.Has(KEY_DEMONIC_PACT))
                if (Pet* pet = player->GetPet())
                {
                    uint32 const share = player->GetMaxHealth() * DEMONIC_PACT_PCT / 100;
                    if (pet->IsAlive() && pet->GetHealth() > share)
                    {
                        pet->ModifyHealth(-int32(share));
                        player->ModifyHealth(int32(share));
                    }
                }
            if (t.Has(KEY_SACRIFICE_COMMAND))
                SacrificeOldest(player, SACRIFICE_PCT);
        }
        // Ritual of Doom: the Doomguard at the warlock's side, no portal, no party.
        if (Named(info, "Ritual of Doom"))
        {
            float x, y, z;
            player->GetNearPoint(player, x, y, z, player->GetObjectBoundingRadius(), 3.0f, player->GetOrientation());
            SummonHorde(player, ENTRY_DOOMGUARD, MINION_DOOMGUARD, x, y, z, t.Has(KEY_PIT_LORD) ? PIT_LORD_LIFE_MS : DOOMGUARD_LIFE_MS);
        }
    }

    bool VetoSpellEffect(Spell const* spell, uint8 /*effIndex*/)
    {
        return spell && Warlock(spell->GetCaster()) && Named(spell->m_spellInfo, "Ritual of Doom");
    }

    bool AdoptSpellSummon(Unit* caster, Creature* summon)
    {
        Player* player = Warlock(caster);
        if (!player || !summon || summon->GetEntry() != ENTRY_INFERNAL)
            return false;
        std::shared_ptr<WebTotals const> totals = TotalsOf(player);
        AdoptHorde(player, summon, MINION_INFERNAL, INFERNAL_LIFE_MS);
        if (!totals)
            return true;
        WebTotals const& t = *totals;
        // Infernal Fall: the landing stuns and strikes the enemies around.
        if (uint8 const rank = t.Rank(KEY_INFERNAL_FALL))
        {
            summon->CastSpell(summon, WAR_STOMP, TRIGGERED_OLD_TRIGGERED);
            float const reach = INFERNAL_FALL_REACH[std::min<uint8>(rank, 3) - 1];
            SpellEntry const* look = sSpellTemplate.LookupEntry<SpellEntry>(LookOfTag(TAG_FIRE));
            for (Unit* foe : NearestFoes(player, summon, reach, 10))
                StrikeFoe(player, foe, look, SchoolOfTag(TAG_FIRE), ToDamage(player, foe, INFERNAL_FALL_PER_LEVEL * player->GetLevel()));
        }
        // Lord of Flame: a second Infernal beside it.
        if (t.Has(KEY_LORD_OF_FLAME))
            SummonHorde(player, ENTRY_INFERNAL, MINION_INFERNAL, summon->GetPositionX() + 2.0f, summon->GetPositionY(),
                        summon->GetPositionZ(), INFERNAL_LIFE_MS);
        return true;
    }

    uint32 WarlockDamageTaken(Unit* victim, Unit* dealer, uint32 damage, SpellEntry const* spellInfo, bool instakill)
    {
        Player* player = Warlock(victim);
        if (!player || !damage || instakill)
            return damage;
        std::shared_ptr<WebTotals const> totals = TotalsOf(player);
        if (!totals)
            return damage;
        WebTotals const& t = *totals;
        if (dealer == victim && Prefixed(spellInfo, "Hellfire") && t.Has(KEY_HELLFIRE_WARD))
            damage = damage * uint32(100 - std::min(100, HELLFIRE_WARD_PER_RANK * t.Rank(KEY_HELLFIRE_WARD))) / 100;
        if (dealer != victim && t.Has(KEY_BACKLASH))
            if (Spell const* channel = player->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
                if (Prefixed(channel->m_spellInfo, "Hellfire"))
                    damage = damage * uint32(100 - BACKLASH_PCT) / 100;
        // Soul Link: a share is the demon's.
        if (dealer != victim && t.Has(KEY_SOUL_LINK))
            if (Pet* pet = player->GetPet())
                if (pet->IsAlive())
                {
                    uint32 const share = std::min<uint32>(damage * SOUL_LINK_PER_RANK * t.Rank(KEY_SOUL_LINK) / 100, pet->GetHealth() - 1);
                    if (share)
                    {
                        pet->ModifyHealth(-int32(share));
                        damage -= share;
                    }
                }
        return damage;
    }

    void UpdateWarlock(Player* player)
    {
        if (!player || player->getClass() != CLASS_WARLOCK || !Active(player))
            return;
        Pet* pet = player->GetPet();
        bool const hasPet = pet && pet->IsAlive();
        uint32 spell = 0;
        bool risen = false;
        uint32 const now = WorldTimer::getMSTime();
        {
            std::lock_guard<std::mutex> guard(sLock);
            WarlockState& w = sWarlocks[player->GetObjectGuid()];
            // A death with a demon out: the demon comes back as the warlock rises at its checkpoint
            // (a demon dies with its master; an ARPG warlock is never left without its tank).
            if (!player->IsAlive())
            {
                if (w.hadPet && w.summonSpell)
                    w.fellWithDemon = true;
                w.hadPet = false;
                w.resummonDue = false;
                return;
            }
            if (w.fellWithDemon && !player->IsBeingTeleported())
            {
                w.fellWithDemon = false;
                if (!hasPet)
                {
                    spell = w.summonSpell;
                    risen = true;
                }
            }
            if (!risen)
            {
                if (w.hadPet && !hasPet && w.summonSpell && HasKeystone(player, KEY_FEL_DOMINATION) &&
                        (!w.felDominationAt || WorldTimer::getMSTimeDiff(w.felDominationAt, now) >= FEL_DOMINATION_EVERY))
                    w.resummonDue = true;
                w.hadPet = hasPet;
                if (hasPet)
                    w.resummonDue = false;
                else if (w.resummonDue)
                    spell = w.summonSpell;
            }
        }
        if (!spell)
            return;
        if (risen)
        {
            if (pet && !pet->IsAlive())
                pet->Unsummon(PET_SAVE_NOT_IN_SLOT, player);
            if (!player->GetPet())
                player->CastSpell(player, spell, TRIGGERED_OLD_TRIGGERED);
            return;
        }
        // Fel Domination: the lost demon comes back at once. A dead one lies as a corpse until it
        // expires: it goes first.
        if (pet && !pet->IsAlive())
            pet->Unsummon(PET_SAVE_NOT_IN_SLOT, player);
        if (player->GetPet())
            return;
        player->CastSpell(player, spell, TRIGGERED_OLD_TRIGGERED);
        {
            std::lock_guard<std::mutex> guard(sLock);
            WarlockState& w = sWarlocks[player->GetObjectGuid()];
            w.resummonDue = false;
            w.felDominationAt = now ? now : 1;
        }
        ReportProc(player, PROC_EFFECT, 0, "Fel Domination", "summons your demon again.");
    }

    void ForgetWarlock(Player* player)
    {
        std::lock_guard<std::mutex> guard(sLock);
        sWarlocks.erase(player->GetObjectGuid());
    }
}
