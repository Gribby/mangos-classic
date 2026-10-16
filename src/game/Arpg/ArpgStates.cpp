/*
 * ARPG enemy states (Arpg/ArpgStates.h).
 */

#include "Arpg/ArpgStates.h"
#include "Arpg/ArpgCombat.h"
#include "Arpg/ArpgDamage.h"
#include "Arpg/ArpgDungeons.h"
#include "Arpg/ArpgPacks.h"
#include "Arpg/ArpgSynergy.h"
#include "Arpg/ArpgTags.h"
#include "Arpg/ArpgTree.h"

#include "Entities/Creature.h"
#include "Entities/Player.h"
#include "Grids/CellImpl.h"
#include "Grids/GridNotifiers.h"
#include "Grids/GridNotifiersImpl.h"
#include "Server/WorldPacket.h"
#include "Server/WorldSession.h"
#include "Spells/SpellAuras.h"
#include "Util/Timer.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace
{
    using namespace Arpg;

    char const* const STATE_NAMES[MAX_STATE] = {
        "Chilled", "Frozen", "Ignited", "Bleeding", "Poisoned", "Withered", "Overloaded", "Judged",
        "Dazed", "Stunned", "Consecrated", "Scorched Ground", "Frozen Ground",
    };

    // The ailment (a damage tag) behind each state, or 0xFF.
    uint8 const STATE_TAG[MAX_STATE] = {
        TAG_FROST, TAG_FROST, TAG_FIRE, TAG_PHYSICAL, TAG_NATURE, TAG_SHADOW, TAG_ARCANE, TAG_HOLY,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    };

    constexpr uint32 SPELL_CHILLED = 12484, SPELL_FROSTBITE = 12494, SPELL_DAZED = 1604;

    // Daze: a hit builds this many times its share of the enemy's health (a hit for a tenth of its
    // health builds a fifth of the threshold); it ebbs at DAZE_EBB a second once the hits stop.
    constexpr float DAZE_GAIN = 2.0f;
    constexpr uint32 DAZE_HOLD_MS = 3000;
    constexpr float DAZE_EBB = 30.0f;
    constexpr uint32 DAZED_MS = 4000;
    // Dazed enemies take this much more damage.
    constexpr float DAZED_TAKEN = 1.10f;
    // A Consecration's tick builds this much Daze.
    constexpr float CONSECRATION_DAZE = 8.0f;

    constexpr uint32 SEND_MS = 250;
    constexpr float SEND_RANGE = 50.0f;
    constexpr size_t SEND_MAX = 64;

    struct Record
    {
        float daze = 0.0f;
        uint32 lastBuild = 0;
        uint32 dazedUntil = 0;
    };

    // By map copy and guid: guids repeat between copies of a dungeon.
    struct Key
    {
        uint64 where;
        ObjectGuid guid;
        bool operator==(Key const& o) const { return where == o.where && guid == o.guid; }
    };
    struct KeyHash
    {
        size_t operator()(Key const& k) const
        {
            return std::hash<uint64>()(k.where * 0x9E3779B97F4A7C15ull ^ k.guid.GetRawValue());
        }
    };

    std::mutex sLock;
    std::unordered_map<Key, Record, KeyHash> sRecords;

    struct Sent
    {
        uint32 lastSend = 0;
        std::unordered_map<ObjectGuid, uint32> told;    // guid -> mask | daze << 16
    };
    std::unordered_map<ObjectGuid, Sent> sSent;         // by player

    Key KeyOf(Unit const* unit)
    {
        return { (uint64(unit->GetMapId()) << 32) | unit->GetInstanceId(), unit->GetObjectGuid() };
    }

    // The Daze left after the ebb since the last hit; sLock held.
    float Ebbed(Record const& r, uint32 now)
    {
        uint32 const since = WorldTimer::getMSTimeDiff(r.lastBuild, now);
        if (since <= DAZE_HOLD_MS)
            return r.daze;
        return std::max(0.0f, r.daze - DAZE_EBB * float(since - DAZE_HOLD_MS) / 1000.0f);
    }

    // How hard `victim` is to Daze: a champion, a rare, an elite or a boss holds out longer.
    float DazeResist(Unit const* victim)
    {
        if (victim->GetTypeId() != TYPEID_UNIT)
            return 1.0f;
        Creature const* c = static_cast<Creature const*>(victim);
        if (IsDungeonBoss(c) || c->IsWorldBoss())
            return 0.25f;
        float resist = c->IsElite() ? 0.75f : 1.0f;
        uint8 const tier = ChampionTier(c);
        if (tier >= 2)
            resist *= 0.45f;
        else if (tier == 1)
            resist *= 0.6f;
        return resist;
    }

    // The ground states `unit` stands in: the areas' auras on it, by name.
    uint32 GroundMask(Unit const* unit)
    {
        uint32 mask = 0;
        for (auto const& [id, holder] : unit->GetSpellAuraHolderMap())
        {
            char const* name = holder->GetSpellProto()->SpellName[0];
            if (!name)
                continue;
            if (std::strncmp(name, "Consecration", 12) == 0)
                mask |= StateBit(STATE_CONSECRATED);
            else if (std::strncmp(name, "Flamestrike", 11) == 0 || std::strncmp(name, "Rain of Fire", 12) == 0)
                mask |= StateBit(STATE_SCORCHED_GROUND);
            else if (std::strncmp(name, "Blizzard", 8) == 0)
                mask |= StateBit(STATE_FROZEN_GROUND);
        }
        return mask;
    }

    // Dazed: slowed, its cast broken (not a boss's or a champion's: their casts are their fight);
    // told to the one who dazed it, and to the triggers.
    void Daze(Player* by, Unit* victim)
    {
        if (DazeResist(victim) >= 0.75f)
            victim->InterruptNonMeleeSpells(false);
        by->CastSpell(victim, SPELL_DAZED, TRIGGERED_OLD_TRIGGERED);
        ReportProcOn(by, victim, TAG_PHYSICAL, PROC_EFFECT, 0, "Dazed", "dazes %s.", victim->GetName());
        OnStateApplied(by, victim, STATE_DAZED);
    }
}

namespace Arpg
{
    char const* StateName(uint8 state)
    {
        return state < MAX_STATE ? STATE_NAMES[state] : "?";
    }

    uint32 StateMask(Unit const* victim)
    {
        if (!victim || !victim->IsAlive())
            return 0;
        uint32 mask = 0;
        uint32 const ailments = AfflictionMask(victim);
        for (uint8 s = 0; s < MAX_STATE; ++s)
            if (STATE_TAG[s] != 0xFF && STATE_TAG[s] != TAG_FROST && (ailments & (1u << STATE_TAG[s])))
                mask |= StateBit(State(s));
        if (Chilled(victim))
            mask |= StateBit(STATE_CHILLED);
        if (Frozen(victim))
            mask |= StateBit(STATE_FROZEN);
        if (victim->IsStunned())
            mask |= StateBit(STATE_STUNNED);
        mask |= GroundMask(victim);
        uint32 const now = WorldTimer::getMSTime();
        std::lock_guard<std::mutex> guard(sLock);
        auto it = sRecords.find(KeyOf(victim));
        if (it != sRecords.end() && int32(it->second.dazedUntil - now) > 0)
            mask |= StateBit(STATE_DAZED);
        return mask;
    }

    bool HasState(Unit const* victim, State state)
    {
        return (StateMask(victim) & StateBit(state)) != 0;
    }

    uint8 DazeOf(Unit const* victim)
    {
        if (!victim)
            return 0;
        uint32 const now = WorldTimer::getMSTime();
        std::lock_guard<std::mutex> guard(sLock);
        auto it = sRecords.find(KeyOf(victim));
        if (it == sRecords.end() || int32(it->second.dazedUntil - now) > 0)
            return 0;
        return uint8(std::min(100.0f, Ebbed(it->second, now)));
    }

    void AddDaze(Player* by, Unit* victim, float pct)
    {
        if (!by || !victim || pct <= 0.0f || !victim->IsAlive() || victim->GetTypeId() != TYPEID_UNIT ||
                victim->IsPlayerControlled() || !by->CanAttack(victim))
            return;
        uint32 const now = WorldTimer::getMSTime();
        bool dazed = false;
        {
            std::lock_guard<std::mutex> guard(sLock);
            Record& r = sRecords[KeyOf(victim)];
            if (int32(r.dazedUntil - now) > 0)
                return;                         // already Dazed: no build-up until it ends
            r.daze = Ebbed(r, now) + pct * DazeResist(victim);
            r.lastBuild = now ? now : 1;
            if (r.daze >= 100.0f)
            {
                r.daze = 0.0f;
                r.dazedUntil = now + DAZED_MS;
                dazed = true;
            }
        }
        if (dazed)
            Daze(by, victim);
    }

    void BuildDaze(Player* by, Unit* victim, uint32 dealt, uint32 tags)
    {
        if (!by || !victim || !dealt || !(tags & (TagBit(TAG_PHYSICAL) | TagBit(TAG_HOLY))))
            return;
        uint32 const most = std::max<uint32>(1, victim->GetMaxHealth());
        float pct = float(dealt) * 100.0f / float(most) * DAZE_GAIN;
        if (std::shared_ptr<WebTotals const> t = TotalsOf(by))
            pct *= std::max(0.0f, 1.0f + t->dazeBuildPct / 100.0f);
        AddDaze(by, victim, pct);
    }

    bool ApplyState(Player* by, Unit* victim, State state)
    {
        if (!by || !victim || !victim->IsAlive() || !by->CanAttack(victim) || victim->GetTypeId() != TYPEID_UNIT ||
                victim->IsPlayerControlled())
            return false;
        switch (state)
        {
            case STATE_DAZED:
            {
                // A hard one (a champion, a rare, a boss) takes it as a full build-up, which its
                // resistance shrinks: no stun-lock on a boss.
                if (DazeResist(victim) < 1.0f)
                {
                    AddDaze(by, victim, 100.0f);
                    return true;
                }
                uint32 const now = WorldTimer::getMSTime();
                {
                    std::lock_guard<std::mutex> guard(sLock);
                    Record& r = sRecords[KeyOf(victim)];
                    if (int32(r.dazedUntil - now) > 0)
                        return false;
                    r.daze = 0.0f;
                    r.lastBuild = now ? now : 1;
                    r.dazedUntil = now + DAZED_MS;
                }
                Daze(by, victim);
                return true;
            }
            case STATE_CHILLED:
                Chill(by, victim, false);
                OnStateApplied(by, victim, state);
                return true;
            case STATE_FROZEN:
                Chill(by, victim, true);
                OnStateApplied(by, victim, state);
                return true;
            default:
                return false;                   // ailments: InflictAilment; stuns and ground: their spells
        }
    }

    bool ConsumeState(Player* by, Unit* victim, State state, uint32& worth)
    {
        worth = 0;
        if (!by || !victim || state >= MAX_STATE || !HasState(victim, state))
            return false;
        switch (state)
        {
            case STATE_CHILLED:
                victim->RemoveAurasDueToSpell(SPELL_CHILLED);
                break;
            case STATE_FROZEN:
                victim->RemoveAurasDueToSpell(SPELL_FROSTBITE);
                break;
            case STATE_DAZED:
            {
                {
                    std::lock_guard<std::mutex> guard(sLock);
                    auto it = sRecords.find(KeyOf(victim));
                    if (it != sRecords.end())
                        it->second.dazedUntil = WorldTimer::getMSTime();
                }
                victim->RemoveAurasDueToSpell(SPELL_DAZED);
                break;
            }
            case STATE_STUNNED:
            case STATE_CONSECRATED:
            case STATE_SCORCHED_GROUND:
            case STATE_FROZEN_GROUND:
                return false;                   // a stun or a ground is read, never consumed
            default:
                worth = TakeAffliction(victim, STATE_TAG[state]);
                break;
        }
        OnStateConsumed(by, victim, state, worth);
        return true;
    }

    void OnStateTick(Unit* caster, Unit* target, SpellEntry const* spellInfo)
    {
        if (!caster || !target || !spellInfo || caster->GetTypeId() != TYPEID_PLAYER || !Active(caster) || !target->IsAlive())
            return;
        char const* name = spellInfo->SpellName[0];
        if (name && std::strncmp(name, "Consecration", 12) == 0)
            AddDaze(static_cast<Player*>(caster), target, CONSECRATION_DAZE);
    }

    float StateTakenMod(Unit const* victim)
    {
        if (!victim || victim->GetTypeId() != TYPEID_UNIT)
            return 1.0f;
        uint32 const now = WorldTimer::getMSTime();
        std::lock_guard<std::mutex> guard(sLock);
        auto it = sRecords.find(KeyOf(victim));
        return it != sRecords.end() && int32(it->second.dazedUntil - now) > 0 ? DAZED_TAKEN : 1.0f;
    }

    void UpdateStates(Player* player)
    {
        if (!player || !player->IsInWorld() || !player->GetSession() || !Active(player))
            return;
        uint32 const now = WorldTimer::getMSTime();
        {
            std::lock_guard<std::mutex> guard(sLock);
            Sent& sent = sSent[player->GetObjectGuid()];
            if (sent.lastSend && WorldTimer::getMSTimeDiff(sent.lastSend, now) < SEND_MS)
                return;
            sent.lastSend = now ? now : 1;
        }
        // The enemies near it, their states now.
        UnitList around;
        MaNGOS::AnyUnitInObjectRangeCheck check(player, SEND_RANGE);
        MaNGOS::UnitListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> searcher(around, check);
        Cell::VisitAllObjects(player, searcher, SEND_RANGE);
        std::unordered_map<ObjectGuid, uint32> current;
        for (Unit* unit : around)
        {
            if (unit->GetTypeId() != TYPEID_UNIT || !unit->IsAlive() || !player->CanAttack(unit))
                continue;
            uint32 const mask = StateMask(unit);
            uint32 const daze = DazeOf(unit);
            if (mask || daze)
                current[unit->GetObjectGuid()] = mask | daze << 16;
        }
        // What changed since it was last told: new and changed states, and those that ended.
        std::vector<std::pair<ObjectGuid, uint32>> changes;
        {
            std::lock_guard<std::mutex> guard(sLock);
            Sent& sent = sSent[player->GetObjectGuid()];
            for (auto const& [guid, packed] : current)
            {
                auto it = sent.told.find(guid);
                if (it == sent.told.end() || it->second != packed)
                    changes.emplace_back(guid, packed);
            }
            for (auto const& [guid, packed] : sent.told)
                if (!current.count(guid))
                    changes.emplace_back(guid, 0);
            if (changes.size() > SEND_MAX)
                changes.resize(SEND_MAX);
            for (auto const& [guid, packed] : changes)
            {
                if (packed)
                    sent.told[guid] = packed;
                else
                    sent.told.erase(guid);
            }
        }
        if (changes.empty())
            return;
        WorldPacket data(SMSG_ARPG_STATES, 1 + changes.size() * 11);
        data << uint8(changes.size());
        for (auto const& [guid, packed] : changes)
        {
            data << guid;
            data << uint16(packed & 0xFFFF);
            data << uint8(packed >> 16);
        }
        player->GetSession()->SendPacket(data);
    }

    void ForgetStatesSent(Player* player)
    {
        if (!player)
            return;
        std::lock_guard<std::mutex> guard(sLock);
        sSent.erase(player->GetObjectGuid());
    }

    void ForgetStates(Creature* creature)
    {
        if (!creature)
            return;
        std::lock_guard<std::mutex> guard(sLock);
        sRecords.erase(KeyOf(creature));
    }
}
