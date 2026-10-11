/*
 * ARPG cross-skill procs: see ArpgProcs.h.
 */

#include "Arpg/ArpgProcs.h"
#include "Arpg/ArpgCombat.h"
#include "Arpg/ArpgSkills.h"

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
    };

    enum Effect : uint8
    {
        EFFECT_INSTANT,       // the next cast of the targets is instant
        EFFECT_DAMAGE,        // ... deals `value`% more (heals heal more)
        EFFECT_CRIT,          // ... is sure to critically strike
        EFFECT_FREE,          // ... costs nothing
        EFFECT_RESET,         // the targets' cooldowns end now
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
    };

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

    using Sockets = std::vector<std::pair<SkillId, uint8>>;

    bool Socketed(Sockets const& sockets, SkillId skill, uint8 rune)
    {
        for (auto const& [s, r] : sockets)
            if (r == rune && (skill == Arpg::SKILL_NONE || s == skill))
                return true;
        return false;
    }

    // Whether `player` has proc `def`: the rune socketed in a skill, or the item worn.
    bool Has(Player const* player, ProcDef const& def, Sockets const& sockets)
    {
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

    // Something that may set procs off happened: `spell` (nullptr: the swing) with `trigger`.
    void Trip(Unit* unit, Trigger trigger, SpellEntry const* spell)
    {
        if (!unit || unit->GetTypeId() != TYPEID_PLAYER || !Active(unit))
            return;
        Player* player = static_cast<Player*>(unit);
        uint32 const now = WorldTimer::getMSTime();
        std::vector<ProcDef> const& procs = Procs();
        Sockets const sockets = SocketedRunes(player);
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
            if (!Has(player, def, sockets) || !Fires(player, def, spell, sockets))
                continue;
            pp.readyAt[i] = now + def.icd;
            Fire(player, i, now);
        }
    }

    void Drop(Player* player, PlayerProcs& pp, size_t at)
    {
        if (pp.readied[at].mod)
            player->AddSpellMod(pp.readied[at].mod, false); // deletes it
        pp.readied.erase(pp.readied.begin() + at);
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

    void OnProcSwing(Unit* attacker, Unit* victim, bool crit)
    {
        if (crit && victim)
            Trip(attacker, TRIG_SWING_CRIT, nullptr);
    }

    void OnProcSpellHit(Spell* spell, Unit* caster, Unit* target, bool crit, bool heal)
    {
        // A triggered spell counts here (a Seal's hit, a Judgement's), an added hit does not.
        if (!spell || spell->IsArpgSecondary() || !target)
            return;
        SpellEntry const* info = spell->m_spellInfo;
        if (heal)
        {
            if (crit)
                Trip(caster, TRIG_HEAL_CRIT, info);
            return;
        }
        Trip(caster, TRIG_SPELL_HIT, info);
        if (crit)
            Trip(caster, TRIG_SPELL_CRIT, info);
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
            Trip(killer, TRIG_KILL, nullptr);
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
            // Damage and crit wait for the hit: they go when the spell is done.
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
            if (stale || !Has(player, Procs()[r.proc], sockets))
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
