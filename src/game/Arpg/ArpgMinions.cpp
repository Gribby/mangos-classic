/*
 * ARPG minions (Arpg/ArpgMinions.h).
 */

#include "Arpg/ArpgMinions.h"
#include "Arpg/ArpgCombat.h"
#include "Arpg/ArpgDamage.h"
#include "Arpg/ArpgTags.h"
#include "Arpg/ArpgTree.h"
#include "Arpg/ArpgUniques.h"

#include "Entities/Creature.h"
#include "Entities/Pet.h"
#include "Entities/Player.h"
#include "Maps/Map.h"
#include "MotionGenerators/MotionMaster.h"
#include "Server/Opcodes.h"
#include "Server/WorldPacket.h"
#include "Server/WorldSession.h"
#include "Spells/SpellMgr.h"
#include "Util/Timer.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace
{
    using namespace Arpg;

    // The horde's size: HORDE_BASE, more by gear and Imp Swarm, at most HORDE_MAX.
    constexpr uint32 HORDE_BASE = 5, HORDE_MAX = 15, IMP_SWARM_PER_RANK = 2;
    // The leash: minions fight within LEASH yards of their owner, come back past TELEPORT.
    constexpr float LEASH = 25.0f, TELEPORT = 40.0f, PICK_RANGE = 15.0f, BOLT_RANGE = 30.0f;
    // The owner's target holds this long after its last blow.
    constexpr uint32 FOCUS_MS = 5000;
    constexpr uint32 UPDATE_MS = 500;
    // A horde imp's Firebolt: this often, for a base of IMP_BOLT_BASE + IMP_BOLT_PER_LEVEL a level
    // (the owner's bonuses and the ARPG's scale on top, as any minion hit).
    constexpr uint32 FIREBOLT = 3110, IMP_BOLT_MS = 2000;
    constexpr float IMP_BOLT_BASE = 3.0f, IMP_BOLT_PER_LEVEL = 1.0f;
    // A demon's own state on its hits, in percent (Imp, Succubus, Felhunter; the Voidwalker's
    // physical blows build Daze anyway).
    constexpr uint32 DEMON_AILMENT_PCT = 15;
    constexpr uint32 MINION_THREAT_MULT = 4;     // a minion's hit is worth 4 times its damage in threat
    constexpr uint32 ENTRY_IMP = 416, ENTRY_FELHUNTER = 417, ENTRY_VOIDWALKER = 1860, ENTRY_SUCCUBUS = 1863;
    constexpr uint32 CLEAVE = 845;
    constexpr float CLEAVE_REACH = 5.0f, LASH_REACH = 6.0f, SACRIFICE_REACH = 8.0f;
    constexpr uint32 LASH_PCT = 50;
    constexpr uint32 CLEAVING_DEMON_PCT[3] = { 30, 45, 60 };
    // The minion keystones' and nodes' numbers.
    constexpr int32 DEMONIC_KNOWLEDGE_PCT = 30, RUINOUS_POWER_LESS_PCT = 30;
    constexpr int32 GRAND_PACT_LIFE_PCT = 40, GRAND_PACT_DAMAGE_PCT = 25;
    constexpr int32 FEL_VITALITY_PER_RANK = 10, DEMONIC_EMBRACE_PER_RANK = 8;
    constexpr int32 IMMOLATION_PER_RANK = 20, DOOMBRINGER_PER_RANK = 15, INFERNAL_HEAT_PER_RANK = 5;
    constexpr uint32 FIERY_SWARM_PER_RANK = 10;

    struct HordeEntry
    {
        ObjectGuid guid;
        MinionKind kind;
        uint32 nextAct = 0;
    };

    struct OwnerState
    {
        std::deque<HordeEntry> horde;       // oldest first
        ObjectGuid focus;
        uint32 focusAt = 0;
        ObjectGuid command;
        uint32 commandUntil = 0;
        int32 commandPct = 0;
        uint32 heatUntil = 0;
        uint32 lastUpdate = 0;
        // The pet's applied life and speed, to take back when they change.
        ObjectGuid petApplied;
        uint32 petLevel = 0;         // a level-up resets its speed (Pet::InitStatsForLevel)
        int32 petLife = 0, petSpeed = 0;
        uint32 sentHash = 0xFFFFFFFF;
    };

    std::mutex sLock;
    std::unordered_map<ObjectGuid, OwnerState> sOwners;

    OwnerState& StateOf(Player const* owner)
    {
        return sOwners[owner->GetObjectGuid()];
    }

    bool Within(Unit const* a, Unit const* b, float yards)
    {
        return a && b && a->GetMap() == b->GetMap() && a->GetDistance(b) <= yards;
    }

    bool Fair(Player const* owner, Unit const* foe)
    {
        return foe && foe->IsAlive() && foe->IsInWorld() && owner->CanAttack(foe) && Within(owner, foe, LEASH);
    }

    int32 DemonLifePct(Player const* owner, WebTotals const& t)
    {
        int32 pct = t.minionLifePct + FEL_VITALITY_PER_RANK * t.Rank(KEY_FEL_VITALITY);
        if (t.Has(KEY_GRAND_PACT))
            pct += GRAND_PACT_LIFE_PCT;
        if (t.Has(KEY_DEMONIC_KNOWLEDGE))
            pct += DEMONIC_KNOWLEDGE_PCT;
        (void)owner;
        return pct;
    }

    int32 HordeLifePct(WebTotals const& t)
    {
        return t.minionLifePct + (t.Has(KEY_DEMONIC_KNOWLEDGE) ? DEMONIC_KNOWLEDGE_PCT : 0);
    }

    std::shared_ptr<WebTotals const> OwnerTotals(Player const* owner)
    {
        return TotalsOf(owner);
    }

    // The pet's life and speed from the owner's minion stats, kept in step.
    void ApplyPet(Player* owner, OwnerState& st, WebTotals const& t)
    {
        Pet* pet = owner->GetPet();
        if (!pet)
        {
            st.petApplied = ObjectGuid();
            st.petLife = st.petSpeed = 0;
            return;
        }
        if (pet->GetObjectGuid() != st.petApplied)
        {
            st.petApplied = pet->GetObjectGuid();
            st.petLife = st.petSpeed = 0;
            st.petLevel = pet->GetLevel();
        }
        // A new level set its attack and cast times afresh: the speed goes on again.
        if (pet->GetLevel() != st.petLevel)
        {
            st.petLevel = pet->GetLevel();
            st.petSpeed = 0;
        }
        int32 const life = DemonLifePct(owner, t);
        if (life != st.petLife)
        {
            if (st.petLife)
                pet->HandleStatModifier(UNIT_MOD_HEALTH, TOTAL_PCT, float(st.petLife), false);
            if (life)
                pet->HandleStatModifier(UNIT_MOD_HEALTH, TOTAL_PCT, float(life), true);
            st.petLife = life;
        }
        int32 const speed = std::clamp(t.minionSpeedPct, 0, 100);
        if (speed != st.petSpeed)
        {
            for (bool apply : { false, true })
            {
                int32 const v = apply ? speed : st.petSpeed;
                if (!v)
                    continue;
                pet->ApplyAttackTimePercentMod(BASE_ATTACK, float(v), apply);
                pet->ApplyCastTimePercentMod(float(v), apply);
            }
            st.petSpeed = speed;
        }
    }

    void SendHud(Player* owner, OwnerState& st)
    {
        Pet* pet = owner->GetPet();
        uint8 const demon = pet && pet->IsAlive() ? 1 : 0;
        uint8 const health = demon && pet->GetMaxHealth() ? uint8(uint64(pet->GetHealth()) * 100 / pet->GetMaxHealth()) : 0;
        uint8 const count = uint8(std::min<size_t>(st.horde.size(), 255));
        uint8 const cap = uint8(HordeCap(owner));
        uint32 const hash = (uint32(demon) << 24) | (uint32(health) << 16) | (uint32(count) << 8) | cap;
        if (hash == st.sentHash || !owner->GetSession())
            return;
        st.sentHash = hash;
        WorldPacket data(SMSG_ARPG_MINIONS, 5 + 32);
        data << demon << health << count << cap;
        data << (demon && pet->GetName() ? pet->GetName() : "");
        owner->GetSession()->SendPacket(data);
    }

    // The enemy a minion of `owner` should fight now: a command's, the owner's last blow's, the
    // nearest enemy to the owner. nullptr: none.
    Unit* PickTarget(Player* owner, OwnerState const& st, uint32 now)
    {
        Map* map = owner->GetMap();
        if (st.command && int32(st.commandUntil - now) > 0)
            if (Unit* u = map->GetUnit(st.command))
                if (Fair(owner, u))
                    return u;
        if (st.focus && WorldTimer::getMSTimeDiff(st.focusAt, now) < FOCUS_MS)
            if (Unit* u = map->GetUnit(st.focus))
                if (Fair(owner, u))
                    return u;
        std::vector<Unit*> const closest = NearestFoes(owner, owner, PICK_RANGE, 1);
        return closest.empty() ? nullptr : closest.front();
    }

    // One minion's turn: the leash, its target, a horde imp's bolt.
    void Steer(Player* owner, Creature* minion, Unit* target, bool commanded, HordeEntry* entry, uint32 now)
    {
        if (!minion->IsAlive())
            return;
        if (!Within(minion, owner, TELEPORT))
        {
            minion->AttackStop();
            minion->NearTeleportTo(owner->GetPositionX(), owner->GetPositionY(), owner->GetPositionZ(), owner->GetOrientation());
            return;
        }
        Unit* victim = minion->GetVictim();
        // Its fight is the owner's: a foe that strayed past the leash is let go.
        if (victim && (!victim->IsAlive() || !Within(owner, victim, LEASH)))
        {
            minion->AttackStop();
            victim = nullptr;
        }
        bool const keep = victim && !commanded;
        if (target && !keep && victim != target && minion->AI())
            minion->AI()->AttackStart(target);
        else if (!victim && !target && entry && minion->GetMotionMaster()->GetCurrentMovementGeneratorType() != FOLLOW_MOTION_TYPE)
            minion->GetMotionMaster()->MoveFollow(owner, PET_FOLLOW_DIST + 1.5f, frand(0.0f, 2.0f * float(M_PI)));
        // A horde imp hurls Firebolts.
        victim = minion->GetVictim();
        if (entry && entry->kind == MINION_IMP && victim && Within(minion, victim, BOLT_RANGE) &&
                int32(entry->nextAct - now) <= 0)
        {
            entry->nextAct = now + IMP_BOLT_MS;
            int32 const bp = int32(IMP_BOLT_BASE + IMP_BOLT_PER_LEVEL * float(owner->GetLevel()));
            minion->CastCustomSpell(victim, FIREBOLT, &bp, nullptr, nullptr, TRIGGERED_OLD_TRIGGERED);
        }
    }
}

namespace Arpg
{
    Player* MinionOwner(Unit const* unit)
    {
        if (!unit || unit->GetTypeId() != TYPEID_UNIT || !unit->GetMap())
            return nullptr;
        ObjectGuid const owner = unit->GetOwnerGuid();
        if (!owner.IsPlayer())
            return nullptr;
        Player* player = unit->GetMap()->GetPlayer(owner);
        return player && Active(player) ? player : nullptr;
    }

    MinionKind HordeKind(Unit const* unit)
    {
        Player const* owner = MinionOwner(unit);
        if (!owner)
            return MINION_NONE;
        std::lock_guard<std::mutex> guard(sLock);
        auto it = sOwners.find(owner->GetObjectGuid());
        if (it == sOwners.end())
            return MINION_NONE;
        for (HordeEntry const& e : it->second.horde)
            if (e.guid == unit->GetObjectGuid())
                return e.kind;
        return MINION_NONE;
    }

    uint32 HordeCap(Player const* owner)
    {
        std::shared_ptr<WebTotals const> t = OwnerTotals(owner);
        uint32 cap = HORDE_BASE;
        if (t)
            cap += uint32(std::max(0, t->hordeSize)) + IMP_SWARM_PER_RANK * t->Rank(KEY_IMP_SWARM);
        return std::min(cap, HORDE_MAX);
    }

    uint32 HordeCount(Player const* owner)
    {
        std::lock_guard<std::mutex> guard(sLock);
        auto it = sOwners.find(owner->GetObjectGuid());
        return it == sOwners.end() ? 0 : uint32(it->second.horde.size());
    }

    void AdoptHorde(Player* owner, Creature* creature, MinionKind kind, uint32 lifeMs)
    {
        if (!owner || !creature)
            return;
        creature->SetOwnerGuid(owner->GetObjectGuid());
        creature->setFaction(owner->GetFaction());
        // A player's, as a summon spell's guardian is (Spell::EffectSummon): without it a minion
        // engaging an enemy takes its owner for an AI-run master (Unit::SetInCombatState asserts).
        creature->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PLAYER_CONTROLLED);
        if (std::shared_ptr<WebTotals const> t = OwnerTotals(owner))
        {
            if (int32 const life = HordeLifePct(*t))
            {
                uint32 const max = uint32(uint64(creature->GetMaxHealth()) * uint32(100 + std::max(0, life)) / 100);
                creature->SetMaxHealth(std::max<uint32>(1, max));
                creature->SetHealth(creature->GetMaxHealth());
            }
            if (int32 const speed = std::clamp(t->minionSpeedPct, 0, 100))
            {
                creature->ApplyAttackTimePercentMod(BASE_ATTACK, float(speed), true);
                creature->ApplyCastTimePercentMod(float(speed), true);
            }
        }
        creature->ForcedDespawn(lifeMs);
        uint32 const cap = HordeCap(owner);
        std::vector<ObjectGuid> dismissed;
        {
            std::lock_guard<std::mutex> guard(sLock);
            OwnerState& st = StateOf(owner);
            st.horde.push_back({ creature->GetObjectGuid(), kind, 0 });
            while (st.horde.size() > cap)
            {
                dismissed.push_back(st.horde.front().guid);
                st.horde.pop_front();
            }
        }
        for (ObjectGuid const& guid : dismissed)
            if (Creature* old = owner->GetMap()->GetCreature(guid))
                old->ForcedDespawn();
        if (Unit* target = [&]() -> Unit* { std::lock_guard<std::mutex> guard(sLock); return PickTarget(owner, StateOf(owner), WorldTimer::getMSTime()); }())
            if (creature->AI())
                creature->AI()->AttackStart(target);
    }

    Creature* SummonHorde(Player* owner, uint32 entry, MinionKind kind, float x, float y, float z, uint32 lifeMs)
    {
        if (!owner || !owner->IsInWorld())
            return nullptr;
        Creature* risen = owner->SummonCreature(entry, x, y, z, owner->GetOrientation(), TEMPSPAWN_TIMED_OR_DEAD_DESPAWN, lifeMs);
        if (!risen)
            return nullptr;
        risen->SelectLevel(owner->GetLevel());
        AdoptHorde(owner, risen, kind, lifeMs);
        return risen;
    }

    void OnOwnerStruck(Player* owner, Unit* victim)
    {
        if (!owner || !victim || victim == owner)
            return;
        std::lock_guard<std::mutex> guard(sLock);
        auto it = sOwners.find(owner->GetObjectGuid());
        if (it == sOwners.end() && !owner->GetPet())
            return;
        OwnerState& st = StateOf(owner);
        st.focus = victim->GetObjectGuid();
        uint32 const now = WorldTimer::getMSTime();
        st.focusAt = now ? now : 1;
    }

    void CommandMinions(Player* owner, Unit* target, uint32 ms, int32 morePct)
    {
        if (!owner || !target)
            return;
        {
            std::lock_guard<std::mutex> guard(sLock);
            OwnerState& st = StateOf(owner);
            st.command = target->GetObjectGuid();
            st.commandUntil = WorldTimer::getMSTime() + ms;
            st.commandPct = morePct;
            st.lastUpdate = 0;      // at once
        }
        UpdateMinions(owner);
    }

    void HeatMinions(Player* owner, uint32 ms)
    {
        std::lock_guard<std::mutex> guard(sLock);
        StateOf(owner).heatUntil = WorldTimer::getMSTime() + ms;
    }

    float MinionDamageMod(Unit const* minion, Player const* owner, Unit const* victim)
    {
        std::shared_ptr<WebTotals const> totals = OwnerTotals(owner);
        if (!totals)
            return 1.0f;
        WebTotals const& t = *totals;
        float mod = 1.0f;
        auto more = [&](int32 pct) { if (pct) mod *= std::max(0.0f, 1.0f + pct / 100.0f); };
        if (t.Has(KEY_DEMONIC_KNOWLEDGE))
            more(DEMONIC_KNOWLEDGE_PCT);
        if (t.Has(KEY_RUINOUS_POWER))
            more(-RUINOUS_POWER_LESS_PCT);
        bool const isPet = minion->GetTypeId() == TYPEID_UNIT && static_cast<Creature const*>(minion)->IsPet();
        if (isPet)
        {
            more(DEMONIC_EMBRACE_PER_RANK * t.Rank(KEY_DEMONIC_EMBRACE));
            if (t.Has(KEY_GRAND_PACT))
                more(GRAND_PACT_DAMAGE_PCT);
        }
        uint32 const now = WorldTimer::getMSTime();
        int32 commandPct = 0;
        bool heat = false;
        MinionKind kind = MINION_NONE;
        {
            std::lock_guard<std::mutex> guard(sLock);
            auto it = sOwners.find(owner->GetObjectGuid());
            if (it != sOwners.end())
            {
                OwnerState const& st = it->second;
                if (victim && st.command == victim->GetObjectGuid() && int32(st.commandUntil - now) > 0)
                    commandPct = st.commandPct;
                heat = int32(st.heatUntil - now) > 0;
                for (HordeEntry const& e : st.horde)
                    if (e.guid == minion->GetObjectGuid())
                        kind = e.kind;
            }
        }
        more(commandPct);
        if (heat)
            more(INFERNAL_HEAT_PER_RANK * t.Rank(KEY_INFERNAL_HEAT));
        if (kind == MINION_INFERNAL)
            more(IMMOLATION_PER_RANK * t.Rank(KEY_IMMOLATION_AURA));
        if (kind == MINION_DOOMGUARD)
            more(DOOMBRINGER_PER_RANK * t.Rank(KEY_DOOMBRINGER));
        return mod;
    }

    void OnMinionHit(Unit* minion, Player* owner, Unit* victim, SpellEntry const* spellInfo, uint32 dealt)
    {
        std::shared_ptr<WebTotals const> totals = OwnerTotals(owner);
        if (!totals || !minion || !victim || !dealt)
            return;
        WebTotals const& t = *totals;
        // Its leech, at its own size.
        if (t.minionLeech > 0 && minion->IsAlive())
            minion->ModifyHealth(int32(std::max<uint32>(1, uint32(uint64(FromDamage(minion, victim, dealt)) * uint32(t.minionLeech) / 1000))));
        if (!victim->IsAlive())
            return;
        // Minions hold the enemy's eye: their hits are worth MINION_THREAT_MULT times their damage
        // in threat, so a pack turns on the demons, not the warlock behind them.
        if (victim->CanHaveThreatList())
            victim->AddThreat(minion, float(dealt) * float(MINION_THREAT_MULT - 1));
        bool const isPet = static_cast<Creature*>(minion)->IsPet();
        uint32 const entry = minion->GetEntry();
        // A demon's own state.
        if (isPet)
        {
            uint8 tag = 0xFF;
            if (entry == ENTRY_IMP)
                tag = TAG_FIRE;
            else if (entry == ENTRY_SUCCUBUS)
                tag = TAG_SHADOW;
            else if (entry == ENTRY_FELHUNTER)
                tag = TAG_ARCANE;
            if (tag != 0xFF && roll_chance_i(int32(DEMON_AILMENT_PCT)))
                InflictAilment(owner, victim, tag, dealt, false);
        }
        else if (HordeKind(minion) == MINION_IMP && t.Has(KEY_FIERY_SWARM) && roll_chance_i(int32(FIERY_SWARM_PER_RANK * t.Rank(KEY_FIERY_SWARM))))
            InflictAilment(owner, victim, TAG_FIRE, dealt, false);
        if (!isPet)
            return;
        // Cleaving Demon: its melee strikes those beside its target.
        if (!spellInfo && t.Has(KEY_CLEAVING_DEMON))
            if (SpellEntry const* shown = sSpellTemplate.LookupEntry<SpellEntry>(CLEAVE))
            {
                uint32 const share = std::max<uint32>(1, dealt * CLEAVING_DEMON_PCT[std::min<uint8>(t.Rank(KEY_CLEAVING_DEMON), 3) - 1] / 100);
                for (Unit* foe : NearestFoes(owner, victim, CLEAVE_REACH, 5))
                    if (foe != victim && foe->IsAlive())
                        StrikeFoe(minion, foe, shown, SPELL_SCHOOL_MASK_NORMAL, share);
            }
        // Lash Arc: Lash of Pain and Firebolt strike more.
        if (spellInfo && t.Has(KEY_LASH_ARC) && spellInfo->SpellName[0] &&
                (std::strcmp(spellInfo->SpellName[0], "Lash of Pain") == 0 || std::strcmp(spellInfo->SpellName[0], "Firebolt") == 0))
        {
            uint32 const share = std::max<uint32>(1, dealt * LASH_PCT / 100);
            uint32 struck = 0;
            for (Unit* foe : NearestFoes(owner, victim, LASH_REACH, 4))
                if (foe != victim && foe->IsAlive() && struck < t.Rank(KEY_LASH_ARC))
                {
                    StrikeFoe(minion, foe, spellInfo, spellInfo->GetSchoolMask(), share);
                    ++struck;
                }
        }
    }

    bool SacrificeOldest(Player* owner, uint32 pct)
    {
        Creature* chosen = nullptr;
        {
            std::lock_guard<std::mutex> guard(sLock);
            auto it = sOwners.find(owner->GetObjectGuid());
            if (it == sOwners.end())
                return false;
            for (HordeEntry const& e : it->second.horde)
                if (Creature* c = owner->GetMap()->GetCreature(e.guid))
                    if (c->IsAlive())
                    {
                        chosen = c;
                        break;
                    }
        }
        if (!chosen)
            return false;
        uint32 const amount = std::max<uint32>(1, ToDamage(owner, nullptr, chosen->GetMaxHealth() * pct / 100));
        SpellEntry const* look = sSpellTemplate.LookupEntry<SpellEntry>(LookOfTag(TAG_FIRE));
        std::vector<Unit*> const foes = NearestFoes(owner, chosen, SACRIFICE_REACH, 10);
        for (Unit* foe : foes)
            StrikeFoe(owner, foe, look, SchoolOfTag(TAG_FIRE), amount);
        ReportProc(owner, PROC_DAMAGE, amount, "Sacrifice", "%s bursts, striking %u %s for %u.", chosen->GetName(),
                   uint32(foes.size()), foes.size() == 1 ? "enemy" : "enemies", amount);
        chosen->ForcedDespawn();
        return true;
    }

    void HealMinions(Player* owner, uint32 amount)
    {
        if (!owner || !amount)
            return;
        if (Pet* pet = owner->GetPet())
            if (pet->IsAlive())
                pet->ModifyHealth(int32(amount));
        std::vector<ObjectGuid> guids;
        {
            std::lock_guard<std::mutex> guard(sLock);
            auto it = sOwners.find(owner->GetObjectGuid());
            if (it != sOwners.end())
                for (HordeEntry const& e : it->second.horde)
                    guids.push_back(e.guid);
        }
        for (ObjectGuid const& guid : guids)
            if (Creature* c = owner->GetMap()->GetCreature(guid))
                if (c->IsAlive())
                    c->ModifyHealth(int32(amount));
    }

    void UpdateMinions(Player* owner)
    {
        if (!owner || !owner->IsInWorld())
            return;
        uint32 const now = WorldTimer::getMSTime();
        std::shared_ptr<WebTotals const> totals = OwnerTotals(owner);
        // The horde's living, and the owner's choice of target, under the lock; the engine's calls
        // after it.
        std::vector<std::pair<Creature*, HordeEntry>> horde;
        Unit* target = nullptr;
        bool commanded = false;
        {
            std::lock_guard<std::mutex> guard(sLock);
            auto it = sOwners.find(owner->GetObjectGuid());
            if (it == sOwners.end() && !owner->GetPet())
                return;
            OwnerState& st = StateOf(owner);
            if (st.lastUpdate && WorldTimer::getMSTimeDiff(st.lastUpdate, now) < UPDATE_MS)
                return;
            st.lastUpdate = now ? now : 1;
            Map* map = owner->GetMap();
            st.horde.erase(std::remove_if(st.horde.begin(), st.horde.end(), [&](HordeEntry const& e)
            {
                Creature* c = map->GetCreature(e.guid);
                return !c || !c->IsAlive() || !c->IsInWorld();
            }), st.horde.end());
            if (totals)
                ApplyPet(owner, st, *totals);
            target = PickTarget(owner, st, now);
            commanded = st.command && int32(st.commandUntil - now) > 0 && target && target->GetObjectGuid() == st.command;
            for (HordeEntry const& e : st.horde)
                horde.emplace_back(map->GetCreature(e.guid), e);
            SendHud(owner, st);
        }
        // Copies: a minion's blow may kill, and a kill may summon (Burning Legion), which changes
        // the horde under us. Their bolt timers go back after.
        for (auto& [creature, entry] : horde)
            if (creature)
                Steer(owner, creature, target, commanded, &entry, now);
        if (Pet* pet = owner->GetPet())
            Steer(owner, pet, target, commanded, nullptr, now);
        std::lock_guard<std::mutex> guard(sLock);
        auto it = sOwners.find(owner->GetObjectGuid());
        if (it != sOwners.end())
            for (HordeEntry& e : it->second.horde)
                for (auto const& [creature, copy] : horde)
                    if (copy.guid == e.guid)
                        e.nextAct = copy.nextAct;
    }

    void ForgetMinions(Player* owner)
    {
        std::lock_guard<std::mutex> guard(sLock);
        sOwners.erase(owner->GetObjectGuid());
    }
}
