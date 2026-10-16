/*
 * ARPG synergy triggers (Arpg/ArpgSynergy.h).
 */

#include "Arpg/ArpgSynergy.h"
#include "Arpg/ArpgCombat.h"
#include "Arpg/ArpgDamage.h"
#include "Arpg/ArpgSkills.h"
#include "Arpg/ArpgTags.h"
#include "Arpg/ArpgUniques.h"

#include "Entities/Item.h"
#include "Entities/Player.h"
#include "Server/SQLStorages.h"
#include "Util/Timer.h"

#include <cstring>
#include <mutex>
#include <unordered_map>

namespace
{
    using namespace Arpg;

    enum Event : uint8 { EV_HIT, EV_CRIT, EV_KILL, EV_STATE_APPLIED, EV_STATE_CONSUMED };
    enum Effect : uint8 { FX_APPLY, FX_STRIKE, FX_BURST, FX_SPREAD, FX_DAZE };

    constexpr int8 ANY = -1;
    constexpr uint8 DAMAGE_TAGS = 7;        // the damage tags, Physical to Shadow (Arpg/ArpgTags.h)
    constexpr float BURST_REACH = 8.0f, SPREAD_REACH = 10.0f;
    constexpr size_t BURST_MAX = 8;
    constexpr int DEPTH_MAX = 3;

    struct Row
    {
        char const* name;
        Event event;
        uint8 classId;          // 0: any class
        char const* spell;      // the event's spell, by name prefix; nullptr: any; "" the swing
        SkillId skill;          // the event's skill; Arpg::SKILL_NONE: any
        uint32 tags;            // the hit must have one of these tags; 0: any
        int8 needState;         // the enemy must be in this state; ANY: none needed
        int8 eventState;        // a state event's state; ANY: any
        Effect effect;
        int8 state;             // the effect's state
        int32 value;            // a share in percent, a count, or Daze in percent: per rank
        uint32 icd;
        uint16 node;            // a skill-tree node that owns it; 0 with classId: every character of the class
        char const* item;       // a worn unique that owns it, by name
        char const* text;
    };

    // The rows. A class's own rules first, then the tree nodes', then the uniques'.
    std::vector<Row> const& Rows()
    {
        static std::vector<Row> const rows = {
            // --- The paladin's own: Daze is its currency (built by its swings, Seals and
            // Consecration), Hammer of Wrath and Judgement its payoffs. ---
            { "Shattering Blow", EV_HIT, CLASS_PALADIN, "Hammer of Wrath", Arpg::SKILL_NONE, 0, STATE_DAZED, ANY,
              FX_STRIKE, STATE_DAZED, 100, 0, 0, nullptr,
              "Hammer of Wrath consumes Daze for an extra hit of 100% of the hit." },
            { "Verdict", EV_HIT, CLASS_PALADIN, "Judgement", Arpg::SKILL_NONE, 0, STATE_DAZED, ANY,
              FX_BURST, STATE_DAZED, 60, 0, 0, nullptr,
              "Judgement consumes Daze to burst onto every enemy within 8 yards for 60% of the hit." },

            // --- Paladin tree nodes (Arpg/ArpgSkills.cpp). ---
            { "Concussive Blows", EV_CRIT, 0, nullptr, SKILL_STRIKE, TagBit(TAG_PHYSICAL) | TagBit(TAG_HOLY), ANY, ANY,
              FX_DAZE, STATE_DAZED, 15, 0, 116, nullptr,
              "Your critical physical or holy Crusader hits add 15% of the way to a Daze per rank." },
            { "Aftershock", EV_STATE_CONSUMED, 0, nullptr, Arpg::SKILL_NONE, 0, ANY, STATE_DAZED,
              FX_SPREAD, STATE_DAZED, 2, 2000, 514, nullptr,
              "When you consume Daze, the 2 nearest enemies within 10 yards are Dazed (once every 2 sec)." },
            { "Cleansing Fire", EV_HIT, 0, nullptr, SKILL_WRATH, 0, STATE_IGNITED, ANY,
              FX_BURST, STATE_IGNITED, 80, 0, 614, nullptr,
              "Exorcism and Holy Wrath consume Ignite to burst onto every enemy within 8 yards for 80% of the hit and the burn left." },
        };
        return rows;
    }

    struct PlayerState
    {
        std::unordered_map<size_t, uint32> readyAt;
    };
    std::mutex sLock;
    std::unordered_map<ObjectGuid, PlayerState> sPlayers;
    thread_local int tDepth = 0;

    bool Wears(Player const* player, char const* name)
    {
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (Item const* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                if (item->GetProto() && std::strcmp(item->GetProto()->Name1, name) == 0)
                    return true;
        return false;
    }

    // How strongly `player` has `row`: 0 not at all, else its rank.
    uint8 RankOf(Player const* player, Row const& row)
    {
        if (row.node)
            return SkillNodeRank(player, row.node);
        if (row.item)
            return Wears(player, row.item) ? 1 : 0;
        return row.classId && player->getClass() == row.classId ? 1 : 0;
    }

    bool SpellFits(Row const& row, SpellEntry const* spellInfo)
    {
        if (!row.spell)
            return true;
        if (!*row.spell)
            return !spellInfo;
        char const* name = spellInfo ? spellInfo->SpellName[0] : nullptr;
        return name && std::strncmp(name, row.spell, std::strlen(row.spell)) == 0;
    }

    uint8 FirstDamageTag(uint32 tags)
    {
        for (uint8 tag = 0; tag < DAMAGE_TAGS; ++tag)
            if (tags & TagBit(Tag(tag)))
                return tag;
        return TAG_HOLY;
    }

    SpellEntry const* LookOf(uint8 tag)
    {
        return sSpellTemplate.LookupEntry<SpellEntry>(LookOfTag(tag));
    }

    // The state's ailment tag, for an inflicted spread (0xFF: not an ailment).
    uint8 AilmentOf(int8 state)
    {
        switch (state)
        {
            case STATE_IGNITED: return TAG_FIRE;
            case STATE_BLEEDING: return TAG_PHYSICAL;
            case STATE_POISONED: return TAG_NATURE;
            case STATE_WITHERED: return TAG_SHADOW;
            case STATE_OVERLOADED: return TAG_ARCANE;
            case STATE_JUDGED: return TAG_HOLY;
            default: return 0xFF;
        }
    }

    // Put `foe` in `state` for `player`, a hit of `dealt` behind it for an ailment.
    void Put(Player* player, Unit* foe, int8 state, uint32 dealt)
    {
        uint8 const tag = AilmentOf(state);
        if (tag != 0xFF)
            InflictAilment(player, foe, tag, std::max<uint32>(1, dealt), false);
        else if (state >= 0)
            ApplyState(player, foe, State(state));
    }

    struct Context
    {
        Event event;
        Unit* victim;
        SpellEntry const* spell;
        uint32 tags;
        uint32 dealt;
        int8 state;             // a state event's
    };

    void Act(Player* player, Row const& row, uint8 rank, Context const& c)
    {
        Unit* victim = c.victim;
        int32 const value = row.value * rank;
        switch (row.effect)
        {
            case FX_APPLY:
                Put(player, victim, row.state, c.dealt);
                ReportProcOn(player, victim, 0xFF, PROC_EFFECT, 0, row.name, "puts %s in %s.", victim->GetName(), StateName(row.state));
                break;
            case FX_DAZE:
                AddDaze(player, victim, float(value));
                break;
            case FX_STRIKE:
            {
                uint32 worth = 0;
                if (!ConsumeState(player, victim, State(row.state), worth) || !victim->IsAlive())
                    break;
                uint8 const tag = FirstDamageTag(c.tags);
                uint32 const amount = std::max<uint32>(1, uint32(uint64(c.dealt) * value / 100) + worth);
                StrikeFoe(player, victim, LookOf(tag), SchoolOfTag(tag), amount);
                ReportProcOn(player, victim, tag, PROC_DAMAGE, amount, row.name, "consumes %s on %s for %u.",
                             StateName(row.state), victim->GetName(), amount);
                break;
            }
            case FX_BURST:
            {
                uint32 worth = 0;
                if (!ConsumeState(player, victim, State(row.state), worth))
                    break;
                uint8 const tag = FirstDamageTag(c.tags);
                uint32 const amount = std::max<uint32>(1, uint32((uint64(c.dealt) + worth) * value / 100));
                std::vector<Unit*> foes = NearestFoes(player, victim, BURST_REACH, BURST_MAX);
                if (std::find(foes.begin(), foes.end(), victim) == foes.end() && victim->IsAlive())
                    foes.push_back(victim);
                for (Unit* foe : foes)
                    if (foe->IsAlive())
                        StrikeFoe(player, foe, LookOf(tag), SchoolOfTag(tag), amount);
                ReportProcOn(player, victim, tag, PROC_DAMAGE, amount, row.name, "consumes %s on %s, bursting onto %u %s for %u.",
                             StateName(row.state), victim->GetName(), uint32(foes.size()), foes.size() == 1 ? "enemy" : "enemies", amount);
                break;
            }
            case FX_SPREAD:
            {
                uint32 const count = uint32(std::max<int32>(1, value));
                std::vector<Unit*> foes = NearestFoes(player, victim, SPREAD_REACH, count + 1);
                uint32 spread = 0;
                for (Unit* foe : foes)
                {
                    if (foe == victim || !foe->IsAlive() || spread >= count)
                        continue;
                    Put(player, foe, row.state, c.dealt);
                    ++spread;
                }
                if (spread)
                    ReportProcOn(player, victim, 0xFF, PROC_EFFECT, 0, row.name, "spreads %s to %u %s.", StateName(row.state),
                                 spread, spread == 1 ? "enemy" : "enemies");
                break;
            }
        }
    }

    void Trip(Player* player, Context const& c)
    {
        if (!player || !c.victim || tDepth >= DEPTH_MAX)
            return;
        std::vector<Row> const& rows = Rows();
        uint32 const now = WorldTimer::getMSTime();
        uint32 mask = 0;
        bool maskRead = false;
        SkillId const skill = c.spell || c.event == EV_HIT || c.event == EV_CRIT ? SkillOfSpell(player->getClass(), c.spell) : Arpg::SKILL_NONE;
        std::vector<std::pair<size_t, uint8>> due;
        for (size_t i = 0; i < rows.size(); ++i)
        {
            Row const& row = rows[i];
            if (row.event != c.event)
                continue;
            if ((row.event == EV_STATE_APPLIED || row.event == EV_STATE_CONSUMED) && row.eventState != ANY && row.eventState != c.state)
                continue;
            if ((row.event == EV_HIT || row.event == EV_CRIT) &&
                    (!SpellFits(row, c.spell) || (row.skill != Arpg::SKILL_NONE && row.skill != skill) || (row.tags && !(row.tags & c.tags))))
                continue;
            uint8 const rank = RankOf(player, row);
            if (!rank)
                continue;
            if (row.needState != ANY)
            {
                if (!maskRead)
                {
                    mask = StateMask(c.victim);
                    maskRead = true;
                }
                if (!(mask & StateBit(State(row.needState))))
                    continue;
            }
            due.emplace_back(i, rank);
        }
        if (due.empty())
            return;
        std::vector<std::pair<size_t, uint8>> ready;
        {
            std::lock_guard<std::mutex> guard(sLock);
            PlayerState& ps = sPlayers[player->GetObjectGuid()];
            for (auto const& [i, rank] : due)
            {
                uint32& at = ps.readyAt[i];
                if (rows[i].icd && at && int32(at - now) > 0)
                    continue;
                if (rows[i].icd)
                    at = now + rows[i].icd;
                ready.emplace_back(i, rank);
            }
        }
        ++tDepth;
        for (auto const& [i, rank] : ready)
            if (c.victim->IsInWorld())
                Act(player, rows[i], rank, c);
        --tDepth;
    }
}

namespace Arpg
{
    void OnSynergyHit(Player* player, Unit* victim, SpellEntry const* spellInfo, uint32 tags, uint32 dealt, bool crit)
    {
        Context c{ EV_HIT, victim, spellInfo, tags, dealt, ANY };
        Trip(player, c);
        if (crit)
        {
            c.event = EV_CRIT;
            Trip(player, c);
        }
    }

    void OnSynergyKill(Player* player, Unit* victim)
    {
        Trip(player, Context{ EV_KILL, victim, nullptr, 0, 0, ANY });
    }

    void OnStateApplied(Player* player, Unit* victim, State state)
    {
        Trip(player, Context{ EV_STATE_APPLIED, victim, nullptr, 0, 0, int8(state) });
    }

    void OnStateConsumed(Player* player, Unit* victim, State state, uint32 worth)
    {
        Trip(player, Context{ EV_STATE_CONSUMED, victim, nullptr, 0, worth, int8(state) });
    }

    void ForgetSynergy(Player* player)
    {
        if (!player)
            return;
        std::lock_guard<std::mutex> guard(sLock);
        sPlayers.erase(player->GetObjectGuid());
    }

    std::vector<std::string> SynergyLines(Player const* player)
    {
        std::vector<std::string> lines;
        if (!player)
            return lines;
        for (Row const& row : Rows())
            if (uint8 const rank = RankOf(player, row))
            {
                std::string line = std::string(row.name) + ": " + row.text;
                if (rank > 1)
                    line += " (rank " + std::to_string(rank) + ")";
                lines.push_back(line);
            }
        return lines;
    }
}
