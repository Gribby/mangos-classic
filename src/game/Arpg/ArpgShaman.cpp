/*
 * ARPG shaman (Arpg/ArpgShaman.h).
 */

#include "Arpg/ArpgShaman.h"
#include "Arpg/ArpgCharacter.h"
#include "Arpg/ArpgCombat.h"
#include "Arpg/ArpgDamage.h"
#include "Arpg/ArpgMinions.h"
#include "Arpg/ArpgStates.h"
#include "Arpg/ArpgTags.h"
#include "Arpg/ArpgTree.h"
#include "Arpg/ArpgUniques.h"

#include "Entities/Creature.h"
#include "Entities/Item.h"
#include "Entities/Player.h"
#include "Entities/Totem.h"
#include "Maps/Map.h"
#include "Server/DBCStores.h"
#include "Spells/SpellMgr.h"
#include "Util/Timer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace
{
    using namespace Arpg;

    // The spells the hooks strike with or draw: rank 1 of each.
    constexpr uint32 LIGHTNING_BOLT = 403, CHAIN_LIGHTNING = 421, EARTH_SHOCK = 8042, FLAME_SHOCK = 8050,
                     FROST_SHOCK = 8056, STORMSTRIKE = 17364, CLEAVE = 845, FIRE_NOVA = 8349;
    // Earth Shock's payoff: this share of its hit for each Shock stack consumed.
    constexpr uint32 EARTH_SHOCK_PER_STACK_PCT = 25;
    constexpr float EARTHQUAKE_RANGE = 6.0f, LAVA_BURST_RANGE = 8.0f, DISCHARGE_RANGE = 10.0f;
    constexpr size_t DISCHARGE_COUNT = 2;
    constexpr uint32 CROSSING_PCT = 50;
    constexpr uint32 STATIC_PER_RANK = 10, OVERLOAD_PER_RANK = 5, OVERLOAD_PCT = 50, CONDUIT_PER_RANK = 1;
    constexpr uint32 FROSTBITE_PER_RANK = 15;
    constexpr float THUNDERLORD_RANGE = 10.0f;
    constexpr size_t THUNDERLORD_JUMPS = 2;
    constexpr uint32 THUNDERLORD_PCT = 50;
    constexpr float STORM_TOTEM_RANGE = 10.0f;
    // Ball Lightning's and Static Shock's bolt: this much damage a level, at the vanilla size.
    constexpr uint32 BOLT_PER_LEVEL = 7;
    constexpr uint32 STATIC_SHOCK_PER_RANK = 10;
    constexpr float STORMBRINGER_RANGE = 8.0f;
    // Windfury: a node's 10% a rank, Windwalker's 20% more; the strike is the swing again.
    constexpr uint32 WINDFURY_PER_RANK = 10, WINDWALKER_PCT = 20;
    constexpr uint32 TEMPEST_PCT[3] = { 30, 45, 60 };
    constexpr float TEMPEST_ARC = 2.0f * float(M_PI) / 3.0f, TEMPEST_RANGE = 8.0f;
    constexpr uint32 RAGE_MS = 6000;
    constexpr uint8 RAGE_MAX = 5;
    constexpr uint32 ELEMENTAL_BLADES_PCT = 25;
    // The totems' nodes.
    constexpr float SEARING_BOLTS_RANGE = 8.0f;
    constexpr uint32 SEARING_BOLTS_PCT = 50, ECHOING_NOVA_PCT = 60;
    constexpr int32 STONECLAW_LIFE_PER_RANK = 25;
    constexpr float STONECLAW_TAUNT_RANGE = 10.0f, EARTHBIND_RANGE = 10.0f;
    constexpr float EARTHLIVING_RANGE = 8.0f;
    constexpr uint32 PULSE_MS = 1000;
    constexpr uint32 VIGOR_PERMILLE_PER_RANK = 5;
    // Walking Totems: a totem further than this from its shaman steps back beside them.
    constexpr float WALK_LEASH = 4.0f, WALK_BESIDE = 2.5f;
    constexpr uint32 WALK_EVERY_MS = 500;
    constexpr float WINDS_EDGE_PER_RANK = 4.0f;
    constexpr float TOTEMIC_WARD_TAKEN = 0.9f;
    // The most enemies an area payoff strikes.
    constexpr size_t AREA_MAX = 10;

    struct ShamanState
    {
        uint32 rageUntil = 0;
        uint8 rage = 0;
        uint32 pulseAt = 0;
        uint32 walkAt = 0;
    };

    std::mutex sLock;
    std::unordered_map<ObjectGuid, ShamanState> sShamans;

    Player* Shaman(Unit* unit)
    {
        if (!unit || unit->GetTypeId() != TYPEID_PLAYER || unit->getClass() != CLASS_SHAMAN || !Active(unit))
            return nullptr;
        return static_cast<Player*>(unit);
    }

    bool Named(SpellEntry const* spellInfo, char const* name)
    {
        return spellInfo && spellInfo->SpellName[0] && std::strcmp(spellInfo->SpellName[0], name) == 0;
    }

    SpellEntry const* SpellOf(uint32 id)
    {
        return sSpellTemplate.LookupEntry<SpellEntry>(id);
    }

    bool IsLightning(SpellEntry const* spellInfo)
    {
        return Named(spellInfo, "Lightning Bolt") || Named(spellInfo, "Chain Lightning") || Named(spellInfo, "Lightning Shield") ||
               Named(spellInfo, "Stormstrike");
    }

    // The name of the main hand's imbue ("Windfury 3"), or nullptr.
    char const* ImbueOf(Player const* player)
    {
        Item* weapon = player->GetWeaponForAttack(BASE_ATTACK, true, true);
        if (!weapon)
            return nullptr;
        uint32 const id = weapon->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT);
        SpellItemEnchantmentEntry const* entry = id ? sSpellItemEnchantmentStore.LookupEntry(id) : nullptr;
        return entry && entry->description[0] && entry->description[0][0] ? entry->description[0] : nullptr;
    }

    bool Imbued(Player const* player, char const* name)
    {
        char const* imbue = ImbueOf(player);
        return imbue && std::strncmp(imbue, name, std::strlen(name)) == 0;
    }

    bool TotemNamed(Unit const* unit, char const* name)
    {
        return unit && unit->GetName() && std::strncmp(unit->GetName(), name, std::strlen(name)) == 0;
    }

    // Whether `victim` stands within `range` of a totem of `player`'s.
    bool NearOwnTotem(Player const* player, Unit const* victim, float range)
    {
        for (uint8 slot = 0; slot < MAX_TOTEM_SLOT; ++slot)
            if (Totem const* totem = player->GetTotem(TotemSlot(slot)))
                if (totem->IsAlive() && totem->GetDistance(victim) <= range)
                    return true;
        return false;
    }

    // `extra` more Shock stacks on `victim` from one Lightning hit: Static, Storm Totem.
    uint8 ShockFor(Player* player, WebTotals const& t, Unit const* victim)
    {
        uint8 stacks = 1;
        if (uint8 const rank = t.Rank(KEY_STATIC))
            if (roll_chance_i(int32(STATIC_PER_RANK * rank)))
                ++stacks;
        if (t.Has(KEY_STORM_TOTEM) && NearOwnTotem(player, victim, STORM_TOTEM_RANGE))
            ++stacks;
        return stacks;
    }

    // A bolt of lightning at `to` for the level's worth (Ball Lightning, Static Shock).
    void LevelBolt(Player* player, Unit* to, char const* name)
    {
        SpellEntry const* bolt = SpellOf(LIGHTNING_BOLT);
        if (!bolt || !to || !to->IsAlive())
            return;
        uint32 const dealt = std::max<uint32>(1, ToDamage(player, to, BOLT_PER_LEVEL * player->GetLevel()));
        SendBolt(player, to, bolt, SPELL_SCHOOL_MASK_NATURE, dealt);
        AddShock(player, to, 1);
        ReportProcOn(player, to, TAG_NATURE, PROC_DAMAGE, dealt, name, "strikes %s for %u.", to->GetName(), dealt);
    }
}

namespace Arpg
{
    void OnShamanHit(Player* player, Unit* victim, SpellEntry const* spellInfo, uint32 dealt, bool crit)
    {
        if (!Shaman(player) || !victim || !spellInfo || !dealt)
            return;
        std::shared_ptr<WebTotals const> totals = TotalsOf(player);
        if (!totals)
            return;
        WebTotals const& t = *totals;

        // The Shock: every Lightning hit, Stormstrike's node for more.
        if (IsLightning(spellInfo) && victim->IsAlive())
        {
            uint8 stacks = ShockFor(player, t, victim);
            if (Named(spellInfo, "Stormstrike"))
                stacks = uint8(stacks + t.Rank(KEY_STORMSTRIKE_SHOCK));
            if (Named(spellInfo, "Lightning Shield") && !t.Has(KEY_CHARGED_SHIELD))
                stacks = 0;                                 // the shield's discharge Shocks with its node only
            AddShock(player, victim, stacks);
        }

        // Lightning Overload: a free echo of the hit.
        if (uint8 const rank = t.Rank(KEY_LIGHTNING_OVERLOAD))
            if ((Named(spellInfo, "Lightning Bolt") || Named(spellInfo, "Chain Lightning")) && victim->IsAlive() &&
                    roll_chance_i(int32(OVERLOAD_PER_RANK * rank)))
            {
                uint32 const echo = std::max<uint32>(1, dealt * OVERLOAD_PCT / 100);
                StrikeFoe(player, victim, spellInfo, SPELL_SCHOOL_MASK_NATURE, echo);
                ReportProcOn(player, victim, TAG_NATURE, PROC_DAMAGE, echo, "Lightning Overload", "strikes again for %u.", echo);
            }

        // Thunderlord: Lightning Bolt always forks.
        if (t.Has(KEY_THUNDERLORD) && Named(spellInfo, "Lightning Bolt"))
        {
            std::vector<Unit*> foes = NearestFoes(player, victim, THUNDERLORD_RANGE, THUNDERLORD_JUMPS + 1);
            foes.erase(std::remove(foes.begin(), foes.end(), victim), foes.end());
            if (foes.size() > THUNDERLORD_JUMPS)
                foes.resize(THUNDERLORD_JUMPS);
            uint32 const share = std::max<uint32>(1, dealt * THUNDERLORD_PCT / 100);
            for (Unit* foe : foes)
            {
                SendBolt(player, foe, spellInfo, SPELL_SCHOOL_MASK_NATURE, share);
                AddShock(player, foe, 1);
            }
        }

        // Earth Shock cashes the Shock in.
        if (Named(spellInfo, "Earth Shock"))
        {
            uint32 stacks = 0;
            if (victim->IsAlive() && ConsumeState(player, victim, STATE_SHOCKED, stacks) && stacks)
            {
                uint32 const payoff = std::max<uint32>(1, dealt * EARTH_SHOCK_PER_STACK_PCT * stacks / 100);
                StrikeFoe(player, victim, spellInfo, SPELL_SCHOOL_MASK_NATURE, payoff);
                if (t.Has(KEY_EARTHQUAKE))
                    for (Unit* foe : NearestFoes(player, victim, EARTHQUAKE_RANGE * SpellAreaScale(player), AREA_MAX))
                        if (foe != victim)
                            StrikeFoe(player, foe, spellInfo, SPELL_SCHOOL_MASK_NATURE, payoff);
                if (uint8 const rank = t.Rank(KEY_CONDUIT))
                    ProcMana(player, "Conduit", std::max<uint32>(1, player->GetMaxPower(POWER_MANA) * CONDUIT_PER_RANK * rank * stacks / 100));
                ReportProcOn(player, victim, TAG_NATURE, PROC_DAMAGE, payoff, t.Has(KEY_EARTHQUAKE) ? "Earthquake" : "Earth Shock",
                             "cashes in %u Shock%s for %u.", stacks, stacks == 1 ? "" : "s", payoff);
            }
            if (t.Has(KEY_EARTHEN_POWER) && victim->IsAlive())
                ApplyState(player, victim, STATE_DAZED);
        }

        // Elemental Crossing (the class's own): fire on the Chilled, frost on the Ignited.
        bool const flame = Named(spellInfo, "Flame Shock"), frost = Named(spellInfo, "Frost Shock");
        if ((flame && Chilled(victim)) || (frost && HasState(victim, STATE_IGNITED)))
            if (victim->IsAlive())
            {
                uint32 const again = std::max<uint32>(1, dealt * CROSSING_PCT / 100);
                StrikeFoe(player, victim, spellInfo, flame ? SPELL_SCHOOL_MASK_FIRE : SPELL_SCHOOL_MASK_FROST, again);
                ReportProcOn(player, victim, flame ? TAG_FIRE : TAG_FROST, PROC_DAMAGE, again, "Elemental Crossing", "strikes again for %u.", again);
            }

        // Lava Burst: Flame Shock bursts an Ignite.
        if (flame && t.Has(KEY_LAVA_BURST) && victim->IsAlive())
        {
            uint32 left = 0;
            if (ConsumeState(player, victim, STATE_IGNITED, left) && left)
            {
                StrikeFoe(player, victim, spellInfo, SPELL_SCHOOL_MASK_FIRE, left);
                for (Unit* foe : NearestFoes(player, victim, LAVA_BURST_RANGE * SpellAreaScale(player), AREA_MAX))
                    if (foe != victim)
                        StrikeFoe(player, foe, spellInfo, SPELL_SCHOOL_MASK_FIRE, left);
                ReportProcOn(player, victim, TAG_FIRE, PROC_DAMAGE, left, "Lava Burst", "bursts for %u.", left);
            }
        }

        // Frostbite: Frost Shock freezes the Chilled.
        if (frost)
            if (uint8 const rank = t.Rank(KEY_FROSTBITE_SHOCK))
                if (victim->IsAlive() && Chilled(victim) && roll_chance_i(int32(FROSTBITE_PER_RANK * rank)))
                    ApplyState(player, victim, STATE_FROZEN);
        (void)crit;
    }

    void OnShamanSwing(Player* player, Unit* victim, uint32 dealt, bool crit)
    {
        if (!Shaman(player) || !victim || !dealt)
            return;
        std::shared_ptr<WebTotals const> totals = TotalsOf(player);
        if (!totals)
            return;
        WebTotals const& t = *totals;
        uint32 const now = WorldTimer::getMSTime();

        // Windfury: the swing again, the nodes' chance on a Windfury Weapon.
        if (Imbued(player, "Windfury") && victim->IsAlive())
        {
            uint32 const chance = WINDFURY_PER_RANK * t.Rank(KEY_WINDFURY) + (t.Has(KEY_WINDWALKER) ? WINDWALKER_PCT : 0);
            if (chance && roll_chance_i(int32(chance)))
                if (SpellEntry const* cleave = SpellOf(CLEAVE))
                {
                    StrikeFoe(player, victim, cleave, SPELL_SCHOOL_MASK_NORMAL, dealt, SpellOf(STORMSTRIKE));
                    uint32 struck = 0;
                    if (uint8 const rank = t.Rank(KEY_TEMPEST_SWING))
                    {
                        uint32 const share = std::max<uint32>(1, dealt * TEMPEST_PCT[std::min<uint8>(rank, 3) - 1] / 100);
                        for (Unit* unit : EnemiesNear(player, TEMPEST_RANGE))
                            if (unit != victim && unit->IsAlive() && MayCatchUnit(player, unit, nullptr) &&
                                    player->CanReachWithMeleeAttack(unit) && player->HasInArc(unit, TEMPEST_ARC))
                            {
                                StrikeFoe(player, unit, cleave, SPELL_SCHOOL_MASK_NORMAL, share, SpellOf(STORMSTRIKE));
                                ++struck;
                            }
                    }
                    if (t.Has(KEY_UNLEASHED_RAGE))
                    {
                        std::lock_guard<std::mutex> guard(sLock);
                        ShamanState& s = sShamans[player->GetObjectGuid()];
                        s.rage = int32(s.rageUntil - now) > 0 ? std::min<uint8>(RAGE_MAX, uint8(s.rage + 1)) : 1;
                        s.rageUntil = now + RAGE_MS;
                    }
                    ReportProcOn(player, victim, TAG_PHYSICAL, PROC_DAMAGE, dealt, "Windfury", struck ? "strikes again, and %u more." : "strikes again.", struck);
                }
        }

        // Elemental Blades: the swing again as Fire, and as Frost.
        if (t.Has(KEY_ELEMENTAL_WEAPONS) && victim->IsAlive())
        {
            uint32 const share = std::max<uint32>(1, dealt * ELEMENTAL_BLADES_PCT / 100);
            if (SpellEntry const* flame = SpellOf(FLAME_SHOCK))
                StrikeFoe(player, victim, flame, SPELL_SCHOOL_MASK_FIRE, share);
            if (victim->IsAlive())
                if (SpellEntry const* frost = SpellOf(FROST_SHOCK))
                    StrikeFoe(player, victim, frost, SPELL_SCHOOL_MASK_FROST, share);
        }

        // Static Shock: a swing on the Shocked throws a bolt.
        if (uint8 const rank = t.Rank(KEY_STATIC_SHOCK))
            if (victim->IsAlive() && ShockStacks(victim) && roll_chance_i(int32(STATIC_SHOCK_PER_RANK * rank)))
                LevelBolt(player, victim, "Static Shock");

        // Stormbringer: a critical swing Shocks every enemy near.
        if (crit && t.Has(KEY_STORMBRINGER))
        {
            std::vector<Unit*> const foes = NearestFoes(player, player, STORMBRINGER_RANGE, AREA_MAX);
            for (Unit* foe : foes)
                AddShock(player, foe, 1);
            if (!foes.empty())
                ReportProc(player, PROC_EFFECT, 0, "Stormbringer", "Shocks %u %s.", uint32(foes.size()), foes.size() == 1 ? "enemy" : "enemies");
        }
    }

    void OnShamanMinionHit(Unit* minion, Player* owner, Unit* victim, SpellEntry const* spellInfo, uint32 dealt)
    {
        if (!minion || !Shaman(owner) || !victim || !spellInfo || !dealt || minion->GetTypeId() != TYPEID_UNIT ||
                !static_cast<Creature*>(minion)->IsTotem())
            return;
        std::shared_ptr<WebTotals const> totals = TotalsOf(owner);
        if (!totals)
            return;
        WebTotals const& t = *totals;
        uint32 const school = spellInfo->GetSchoolMask() ? spellInfo->GetSchoolMask() : uint32(SPELL_SCHOOL_MASK_FIRE);

        // Searing Totem's bolt: Twin Totems throws a second; Searing Bolts strikes more.
        if (TotemNamed(minion, "Searing Totem"))
        {
            if (t.Has(KEY_TWIN_TOTEMS) && victim->IsAlive())
                StrikeFoe(minion, victim, spellInfo, school, dealt);
            if (uint8 const rank = t.Rank(KEY_SEARING_BOLTS))
            {
                std::vector<Unit*> foes = NearestFoes(owner, victim, SEARING_BOLTS_RANGE, size_t(rank) + 1);
                foes.erase(std::remove(foes.begin(), foes.end(), victim), foes.end());
                if (foes.size() > rank)
                    foes.resize(rank);
                for (Unit* foe : foes)
                    StrikeFoe(minion, foe, spellInfo, school, std::max<uint32>(1, dealt * SEARING_BOLTS_PCT / 100));
            }
        }

        // Magma Totem's pulse Ignites (Molten Core).
        if (TotemNamed(minion, "Magma Totem") && t.Has(KEY_MOLTEN_CORE) && victim->IsAlive())
            InflictAilment(owner, victim, TAG_FIRE, dealt, false);

        // Fire Nova: Echoing Nova's strikes, Cataclysmic Nova's burst.
        if (Named(spellInfo, "Fire Nova"))
        {
            if (uint8 const rank = t.Rank(KEY_ECHOING_NOVA))
                for (uint8 i = 0; i < rank && victim->IsAlive(); ++i)
                    StrikeFoe(minion, victim, spellInfo, school, std::max<uint32>(1, dealt * ECHOING_NOVA_PCT / 100));
            if (t.Has(KEY_CATACLYSMIC_NOVA) && victim->IsAlive())
            {
                uint32 left = 0;
                if (ConsumeState(owner, victim, STATE_IGNITED, left) && left)
                {
                    StrikeFoe(minion, victim, spellInfo, SPELL_SCHOOL_MASK_FIRE, left);
                    ReportProcOn(owner, victim, TAG_FIRE, PROC_DAMAGE, left, "Cataclysmic Nova", "bursts the burn for %u.", left);
                }
            }
        }
    }

    void OnShamanKill(Player* killer, Unit* victim)
    {
        if (!Shaman(killer) || !victim)
            return;
        if (!ShockStacks(victim))
            return;
        // Static Discharge (the class's own): the Shock jumps to the 2 nearest.
        std::vector<Unit*> foes = NearestFoes(killer, victim, DISCHARGE_RANGE, DISCHARGE_COUNT + 1);
        foes.erase(std::remove(foes.begin(), foes.end(), victim), foes.end());
        if (foes.size() > DISCHARGE_COUNT)
            foes.resize(DISCHARGE_COUNT);
        for (Unit* foe : foes)
            AddShock(killer, foe, 1);
        // Ball Lightning: a bolt at the nearest, on the killer's next update: a bolt that kills
        // sets off another, and inline that nests one kill in another as deep as the pack is big.
        if (HasKeystone(killer, KEY_BALL_LIGHTNING) && !foes.empty())
        {
            ObjectGuid const target = foes.front()->GetObjectGuid();
            killer->m_events.AddEvent(new UnitLambdaEvent(*killer, [target](Unit& owner)
            {
                Player* player = static_cast<Player*>(&owner);
                if (!player->IsInWorld() || !player->IsAlive())
                    return;
                if (Unit* to = player->GetMap()->GetUnit(target))
                    if (to->IsAlive())
                        LevelBolt(player, to, "Ball Lightning");
            }), killer->m_events.CalculateTime(1));
        }
    }

    void OnShamanHeal(Player* player, Unit* /*target*/, uint32 amount)
    {
        if (!Shaman(player) || !amount || !HasKeystone(player, KEY_EARTHLIVING))
            return;
        std::vector<Unit*> const foes = NearestFoes(player, player, EARTHLIVING_RANGE * SpellAreaScale(player), AREA_MAX);
        for (Unit* foe : foes)
            AddShock(player, foe, 1);
        if (!foes.empty())
            ReportProc(player, PROC_EFFECT, 0, "Earthliving", "Shocks %u %s.", uint32(foes.size()), foes.size() == 1 ? "enemy" : "enemies");
    }

    void OnShamanTotem(Unit* owner, Creature* totem)
    {
        Player* player = owner ? Shaman(owner) : nullptr;
        if (!player || !totem)
            return;
        std::shared_ptr<WebTotals const> totals = TotalsOf(player);
        if (!totals)
            return;
        // A minion's life (the web, sets, affixes), and Stoneclaw's more.
        int32 pct = totals->minionLifePct;
        bool const stoneclaw = TotemNamed(totem, "Stoneclaw Totem");
        uint8 const rank = stoneclaw ? totals->Rank(KEY_STONECLAW) : 0;
        pct += STONECLAW_LIFE_PER_RANK * rank;
        if (pct > 0)
        {
            uint32 const life = uint32(uint64(totem->GetMaxHealth()) * uint32(100 + pct) / 100);
            totem->SetMaxHealth(life);
            totem->SetHealth(life);
        }
        // Stoneclaw's node: every enemy near that is fighting turns on it.
        if (rank)
            for (Unit* foe : NearestFoes(player, totem, STONECLAW_TAUNT_RANGE, AREA_MAX))
                if (foe->IsInCombat())
                    foe->AddThreat(totem, float(totem->GetMaxHealth()));
    }

    bool HasTotemUp(Player const* player)
    {
        if (!player)
            return false;
        for (uint8 slot = 0; slot < MAX_TOTEM_SLOT; ++slot)
            if (Totem const* totem = player->GetTotem(TotemSlot(slot)))
                if (totem->IsAlive())
                    return true;
        return false;
    }

    bool HasAirTotemUp(Player const* player)
    {
        Totem const* totem = player ? player->GetTotem(TOTEM_SLOT_AIR) : nullptr;
        return totem && totem->IsAlive();
    }

    uint8 UnleashedRageStacks(Player const* player)
    {
        if (!player)
            return 0;
        uint32 const now = WorldTimer::getMSTime();
        std::lock_guard<std::mutex> guard(sLock);
        auto it = sShamans.find(player->GetObjectGuid());
        if (it == sShamans.end() || int32(it->second.rageUntil - now) <= 0)
            return 0;
        return it->second.rage;
    }

    float ShamanHastePct(Player const* player)
    {
        if (!player || player->getClass() != CLASS_SHAMAN)
            return 0.0f;
        uint8 const rank = KeyRank(player, KEY_WINDS_EDGE);
        return rank && HasAirTotemUp(player) ? WINDS_EDGE_PER_RANK * rank : 0.0f;
    }

    float ShamanTakenMod(Unit const* victim)
    {
        if (!victim || victim->GetTypeId() != TYPEID_PLAYER || victim->getClass() != CLASS_SHAMAN || !Active(victim))
            return 1.0f;
        Player const* player = static_cast<Player const*>(victim);
        return HasKeystone(player, KEY_TOTEMIC_WARD) && HasTotemUp(player) ? TOTEMIC_WARD_TAKEN : 1.0f;
    }

    void UpdateShaman(Player* player)
    {
        if (!Shaman(player) || !player->IsAlive() || !player->IsInWorld())
            return;
        std::shared_ptr<WebTotals const> totals = TotalsOf(player);
        if (!totals)
            return;
        WebTotals const& t = *totals;
        uint32 const now = WorldTimer::getMSTime();
        bool walk = false, pulse = false;
        {
            std::lock_guard<std::mutex> guard(sLock);
            ShamanState& s = sShamans[player->GetObjectGuid()];
            if (t.Has(KEY_WALKING_TOTEMS) && (!s.walkAt || WorldTimer::getMSTimeDiff(s.walkAt, now) >= WALK_EVERY_MS))
            {
                s.walkAt = now ? now : 1;
                walk = true;
            }
            if (!s.pulseAt || WorldTimer::getMSTimeDiff(s.pulseAt, now) >= PULSE_MS)
            {
                s.pulseAt = now ? now : 1;
                pulse = true;
            }
        }

        // Walking Totems: each totem keeps its place around the shaman.
        if (walk)
            for (uint8 slot = 0; slot < MAX_TOTEM_SLOT; ++slot)
                if (Totem* totem = player->GetTotem(TotemSlot(slot)))
                    if (totem->IsAlive() && totem->GetDistance(player) > WALK_LEASH)
                    {
                        float const angle = player->GetOrientation() + float(M_PI) * (0.5f + 0.5f * slot);
                        float x = player->GetPositionX() + WALK_BESIDE * std::cos(angle);
                        float y = player->GetPositionY() + WALK_BESIDE * std::sin(angle);
                        float z = player->GetPositionZ();
                        player->UpdateAllowedPositionZ(x, y, z);
                        totem->NearTeleportTo(x, y, z, player->GetOrientation());
                    }

        if (!pulse)
            return;
        // Earthen Vigor: health while a totem stands.
        if (uint8 const rank = t.Rank(KEY_EARTHEN_VIGOR))
            if (HasTotemUp(player) && player->GetHealth() < player->GetMaxHealth())
                ProcHeal(player, "Earthen Vigor", std::max<uint32>(1, player->GetMaxHealth() * VIGOR_PERMILLE_PER_RANK * rank / 1000));
        // Earth's Grasp: the enemies the Earthbind Totem holds are Vulnerable.
        if (t.Has(KEY_EARTHS_GRASP))
            if (Totem* earth = player->GetTotem(TOTEM_SLOT_EARTH))
                if (earth->IsAlive() && TotemNamed(earth, "Earthbind Totem"))
                    for (Unit* foe : NearestFoes(player, earth, EARTHBIND_RANGE * SpellAreaScale(player), AREA_MAX))
                        ApplyState(player, foe, STATE_VULNERABLE);
    }

    void ForgetShaman(Player* player)
    {
        if (!player)
            return;
        std::lock_guard<std::mutex> guard(sLock);
        sShamans.erase(player->GetObjectGuid());
    }
}
