/*
 * ARPG character: see ArpgCharacter.h.
 */

#include "Arpg/ArpgCharacter.h"
#include "Arpg/ArpgCombat.h"
#include "Arpg/ArpgPacks.h"
#include "Arpg/ArpgSkills.h"
#include "Arpg/ArpgTags.h"
#include "Arpg/ArpgTree.h"
#include "Arpg/ArpgUniques.h"

#include "Entities/Creature.h"
#include "Entities/DynamicObject.h"
#include "Entities/Item.h"
#include "Entities/Player.h"
#include "Spells/Spell.h"
#include "Spells/SpellAuras.h"
#include "Spells/SpellMgr.h"
#include "Util/Timer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace
{
    // Rune of Leech: this share of the damage heals.
    constexpr uint32 RUNE_LEECH_PCT = 3;
    using namespace Arpg;

    // The attributes' ARPG effects: points of the attribute per 1%, and the cap in percent.
    constexpr int32 STR_PER_AREA = 10, AREA_CAP = 40;
    constexpr int32 AGI_PER_SPEED = 20, SPEED_CAP = 20;
    constexpr int32 STA_PER_LIFE = 4;
    constexpr int32 INT_PER_AREA = 10;
    constexpr int32 SPI_PER_COOLDOWN = 10, COOLDOWN_CAP = 30;

    // The keystones' numbers.
    constexpr uint32 REFRESH_MS = 1000;
    constexpr float ZEALOT_HASTE = 25.0f;
    constexpr uint32 CRUSADE_WINDOW = 5000, CRUSADE_MAX = 10;
    constexpr int32 CRUSADE_PER_KILL = 4;
    constexpr float CRUSADE_SLOW = 0.8f, UNYIELDING_SLOW = 0.8f;
    constexpr float MARTYR_RANGE = 10.0f;
    constexpr uint32 MARTYR_SHARE_PCT = 25;
    constexpr uint32 RETRIBUTION_AURA = 7294;
    constexpr float LIGHTFORGED_RANGE = 20.0f;
    constexpr uint32 FAVOUR_COOLDOWN = 20000;
    constexpr uint32 RECOVERY_MS = 4000;
    constexpr int32 RECOVERY_PCT = 10;
    constexpr float FURY_RANGE = 8.0f;
    constexpr size_t FURY_COUNT = 3;

    // The skill trees' numbers.
    constexpr float SWING_ARC = 2.0f * float(M_PI) / 3.0f;
    constexpr uint32 WIDE_SWING_PCT[3] = { 20, 35, 50 };
    constexpr uint32 WHIRL_EVERY = 4, WHIRL_PCT = 75;
    constexpr uint32 MOMENTUM_WINDOW = 3000, MOMENTUM_MAX = 5;
    constexpr uint32 PACE_MS = 3000;
    constexpr float PACE_SPEED = 1.3f;
    constexpr float SHOCKWAVE_REACH = 10.0f;
    constexpr uint32 SHOCKWAVE_PCT = 40;
    constexpr uint32 DAZED = 1604, CLEAVE = 845, WHIRLWIND = 1680, WAR_STOMP = 20549;
    constexpr uint32 CONSECRATION = 26573, JUDGEMENT = 20271;
    constexpr float SANCTIFIED_RANGE = 8.0f;
    constexpr uint32 SANCTIFIED_PCT = 50;
    constexpr float RICOCHET_RANGE = 10.0f;
    constexpr float BLESSED_RANGE = 15.0f;
    constexpr size_t BLESSED_COUNT = 3;
    constexpr float WALK_FOLLOW = 2.0f;
    constexpr uint32 WALK_MS = 250;

    // What has been applied to a player, and the keystones' running state.
    struct Runtime
    {
        int32 stat[5] = {};
        int32 armourPct = 0;
        float haste = 0.0f;          // the attack speed percent applied (Zealot, Quickened, Zeal)
        bool stunImmune = false;     // Unyielding, or Steadfast inside Consecration
        bool snareImmune = false;    // Unyielding
        std::vector<std::pair<SkillSpellMod, SpellModifier*>> skillMods;
        uint32 swings = 0;           // Whirling Strikes: swings that hit
        uint32 momentum = 0;         // Momentum: hits in a row
        uint32 lastHit = 0;
        uint32 paceUntil = 0;        // Crusader's Pace: the time its speed ends
        uint32 lastWalk = 0;         // Walking Consecration: the last time it followed
        SpellModifier* cooldownMod = nullptr;
        int32 cooldownPct = 0;
        float speed = 1.0f;
        uint32 lastRefresh = 0;
        std::deque<uint32> kills;    // Crusade: kill times
        uint32 martyrPending = 0;
        uint32 favourReady = 0;      // Divine Favour: the time it is ready again
        uint32 recoveryUntil = 0;    // Blessed Recovery: the time its bonus ends
    };

    std::mutex sRuntimeLock;
    std::unordered_map<ObjectGuid, Runtime> sRuntime;

    // The player's runtime state; elements of an unordered_map keep their address.
    Runtime& R(Unit const* unit)
    {
        std::lock_guard<std::mutex> guard(sRuntimeLock);
        return sRuntime[unit->GetObjectGuid()];
    }

    WebTotals const& NoTotals()
    {
        static WebTotals const none;
        return none;
    }

    int32 TagSum(WebTotals const& t, uint32 tags)
    {
        int32 sum = 0;
        for (uint8 tag = 0; tag < MAX_TAG; ++tag)
            if (tags & TagBit(Tag(tag)))
                sum += t.tag[tag];
        return sum;
    }

    Item const* MainHand(Player const* player)
    {
        return player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    }

    bool TwoHanded(Player const* player)
    {
        Item const* item = MainHand(player);
        return item && item->GetProto()->InventoryType == INVTYPE_2HWEAPON;
    }

    bool OneHandAndShield(Player const* player)
    {
        Item const* main = MainHand(player);
        Item const* off = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
        return main && off && main->GetProto()->InventoryType != INVTYPE_2HWEAPON &&
               off->GetProto()->InventoryType == INVTYPE_SHIELD;
    }

    uint32 CrusadeStacks(Runtime const& r, uint32 now)
    {
        uint32 n = 0;
        for (uint32 t : r.kills)
            if (WorldTimer::getMSTimeDiff(t, now) <= CRUSADE_WINDOW)
                ++n;
        return std::min(n, CRUSADE_MAX);
    }

    int32 CooldownPct(Player const* player, WebTotals const& t, Runtime const& r, uint32 now)
    {
        int32 pct = int32(player->GetStat(STAT_SPIRIT)) / SPI_PER_COOLDOWN + t.cooldownPct;
        if (now < r.recoveryUntil)
            pct += RECOVERY_PCT;
        return std::clamp(pct, 0, COOLDOWN_CAP);
    }

    // A spell of the player's own class family, to own the cooldown modifier (the client and
    // Player::ApplySpellMod both match a modifier to spells of its spell's family).
    uint32 FamilySpell(Player const* player)
    {
        for (auto const& [id, spell] : player->GetSpellMap())
        {
            if (spell.state == PLAYERSPELL_REMOVED)
                continue;
            if (SpellEntry const* entry = sSpellTemplate.LookupEntry<SpellEntry>(id))
                if (entry->SpellFamilyName == uint32(player->GetSpellClass()) && entry->SpellFamilyName)
                    return id;
        }
        return 0;
    }

    void SetCooldownMod(Player* player, Runtime& r, int32 pct)
    {
        if (pct == r.cooldownPct && (pct == 0) == (r.cooldownMod == nullptr))
            return;
        if (r.cooldownMod)
        {
            player->AddSpellMod(r.cooldownMod, false); // deletes it
            r.cooldownMod = nullptr;
        }
        r.cooldownPct = pct;
        if (!pct)
            return;
        uint32 const owner = FamilySpell(player);
        if (!owner)
            return;
        r.cooldownMod = new SpellModifier(SPELLMOD_COOLDOWN, SPELLMOD_PCT, -pct, owner, ~uint64(0));
        player->AddSpellMod(r.cooldownMod, true);
    }

    bool SealOn(Player const* player)
    {
        for (auto const& [id, holder] : player->GetSpellAuraHolderMap())
            if (holder->GetCasterGuid() == player->GetObjectGuid() && (TagsOf(holder->GetSpellProto()) & TagBit(TAG_SEAL)))
                return true;
        return false;
    }

    bool AuraNamed(Unit const* unit, char const* prefix, ObjectGuid caster)
    {
        size_t const n = std::strlen(prefix);
        for (auto const& [id, holder] : unit->GetSpellAuraHolderMap())
        {
            char const* name = holder->GetSpellProto()->SpellName[0];
            if (name && std::strncmp(name, prefix, n) == 0 && (!caster || holder->GetCasterGuid() == caster))
                return true;
        }
        return false;
    }

    // Whether `victim` stands in `by`'s Consecration (it carries the ground's aura).
    bool Consecrated(Unit const* victim, Unit const* by)
    {
        return victim && by && AuraNamed(victim, "Consecration", by->GetObjectGuid());
    }

    // The player's own Consecration on the ground, any rank.
    DynamicObject* OwnConsecration(Player* player)
    {
        for (uint32 id = CONSECRATION; id; id = sSpellMgr.GetNextSpellInChain(id))
            if (DynamicObject* dyn = player->GetDynObject(id))
                return dyn;
        return nullptr;
    }

    bool Inside(Player* player, DynamicObject* dyn)
    {
        return dyn && player->GetDistance(dyn, false, DIST_CALC_NONE) <= dyn->GetRadius();
    }

    // The attack speed percent the player should have.
    float HasteFor(Player const* player, WebTotals const& t)
    {
        float haste = 0.0f;
        if (t.Has(KEY_ZEALOT))
            haste += ZEALOT_HASTE;
        haste += 4.0f * t.Rank(KEY_QUICKENED);
        if (t.Has(KEY_ZEAL) && AuraNamed(player, "Seal of the Crusader", player->GetObjectGuid()))
            haste += 10.0f * t.Rank(KEY_ZEAL);
        return haste;
    }

    void SetHaste(Player* player, Runtime& r, float haste)
    {
        if (std::abs(haste - r.haste) < 0.01f)
            return;
        for (WeaponAttackType att : { BASE_ATTACK, OFF_ATTACK })
        {
            if (r.haste > 0.0f)
                player->ApplyAttackTimePercentMod(att, r.haste, false);
            if (haste > 0.0f)
                player->ApplyAttackTimePercentMod(att, haste, true);
        }
        r.haste = haste;
    }

    void SetImmunity(Player* player, Runtime& r, bool stun, bool snare)
    {
        if (stun != r.stunImmune)
        {
            player->ApplySpellImmune(nullptr, IMMUNITY_MECHANIC, MECHANIC_STUN, stun);
            if (stun)
                player->RemoveSpellsCausingAura(SPELL_AURA_MOD_STUN);
            r.stunImmune = stun;
        }
        if (snare != r.snareImmune)
        {
            player->ApplySpellImmune(nullptr, IMMUNITY_MECHANIC, MECHANIC_SNARE, snare);
            player->ApplySpellImmune(nullptr, IMMUNITY_STATE, SPELL_AURA_MOD_DECREASE_SPEED, snare);
            if (snare)
                player->RemoveSpellsCausingAura(SPELL_AURA_MOD_DECREASE_SPEED);
            r.snareImmune = snare;
        }
    }

    // The skill nodes' spell modifiers, applied as the totals list them.
    void SetSkillMods(Player* player, Runtime& r, std::vector<SkillSpellMod> const& want)
    {
        bool same = want.size() == r.skillMods.size();
        for (size_t i = 0; same && i < want.size(); ++i)
            same = want[i] == r.skillMods[i].first;
        if (same)
            return;
        for (auto& [spec, mod] : r.skillMods)
            if (mod)
                player->AddSpellMod(mod, false); // deletes it
        r.skillMods.clear();
        uint32 const owner = FamilySpell(player);
        for (SkillSpellMod const& spec : want)
        {
            SpellModifier* mod = nullptr;
            if (owner)
            {
                mod = new SpellModifier(SpellModOp(spec.op), SpellModType(spec.type), spec.value, owner, spec.mask);
                player->AddSpellMod(mod, true);
            }
            r.skillMods.emplace_back(spec, mod);
        }
    }

    // Walking Consecration: the ground itself moves under the player, so the auras it holds on
    // the enemies inside keep ticking (a fresh cast would restart them before their first tick).
    void WalkConsecration(Player* player, DynamicObject* dyn)
    {
        if (!dyn || player->GetDistance(dyn, false, DIST_CALC_NONE) <= WALK_FOLLOW)
            return;
        player->GetMap()->DynamicObjectRelocation(dyn, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ());
    }

    // The once-a-second refresh: speed, cooldown recovery, Zealot's drain, a level gained.
    void Refresh(Player* player, Runtime& r, uint32 now)
    {
        UpdateTree(player);
        SendNearbyChampions(player);
        std::shared_ptr<WebTotals const> totals = TotalsOf(player);
        WebTotals const& t = totals ? *totals : NoTotals();

        float const speed = MoveSpeedMod(player);
        if (std::abs(speed - r.speed) > 0.001f)
        {
            r.speed = speed;
            player->UpdateSpeed(MOVE_RUN, true);
        }
        SetCooldownMod(player, r, CooldownPct(player, t, r, now));
        SetHaste(player, r, HasteFor(player, t));

        // Consecration's skill nodes (Walking Consecration moves it in UpdateCharacter).
        DynamicObject* ground = (t.Has(KEY_HALLOWED_GROUND) || t.Has(KEY_STEADFAST)) ? OwnConsecration(player) : nullptr;
        bool const inside = Inside(player, ground);
        if (inside && t.Has(KEY_HALLOWED_GROUND) && player->IsAlive())
            player->ModifyHealth(int32(player->GetMaxHealth() * t.Rank(KEY_HALLOWED_GROUND) / 100));
        SetImmunity(player, r, t.Has(KEY_UNYIELDING) || (inside && t.Has(KEY_STEADFAST)), t.Has(KEY_UNYIELDING));

        if (t.Has(KEY_ZEALOT) && player->IsAlive() && SealOn(player))
        {
            int32 const drain = int32(player->GetMaxPower(POWER_MANA) / 100);
            if (drain > 0)
                player->ModifyPower(POWER_MANA, -drain);
        }
        while (!r.kills.empty() && WorldTimer::getMSTimeDiff(r.kills.front(), now) > CRUSADE_WINDOW)
            r.kills.pop_front();
    }
}

namespace Arpg
{
    void ApplyCharacter(Player* player)
    {
        Runtime& r = R(player);
        std::shared_ptr<WebTotals const> totals = TotalsOf(player);
        WebTotals const& t = totals ? *totals : NoTotals();

        for (int i = 0; i < 5; ++i)
        {
            if (t.stat[i] == r.stat[i])
                continue;
            UnitMods const mod = UnitMods(UNIT_MOD_STAT_START + i);
            if (r.stat[i])
                player->HandleStatModifier(mod, TOTAL_VALUE, float(r.stat[i]), false);
            if (t.stat[i])
                player->HandleStatModifier(mod, TOTAL_VALUE, float(t.stat[i]), true);
            r.stat[i] = t.stat[i];
        }
        if (t.armourPct != r.armourPct)
        {
            if (r.armourPct)
                player->HandleStatModifier(UNIT_MOD_ARMOR, TOTAL_PCT, float(r.armourPct), false);
            if (t.armourPct)
                player->HandleStatModifier(UNIT_MOD_ARMOR, TOTAL_PCT, float(t.armourPct), true);
            r.armourPct = t.armourPct;
        }
        player->UpdateBlockPercentage();

        SetHaste(player, r, HasteFor(player, t));
        SetSkillMods(player, r, t.mods);
        SetImmunity(player, r, t.Has(KEY_UNYIELDING) || (r.stunImmune && t.Has(KEY_STEADFAST)), t.Has(KEY_UNYIELDING));
        // Speed and cooldowns at once, not at the next refresh.
        r.lastRefresh = 0;
    }

    void ForgetCharacter(Player* player)
    {
        // The player is going; its cooldown modifier stays in its spell modifier list, which
        // Player never frees, as an aura's stays until the aura goes.
        ForgetChampionsSent(player);
        std::lock_guard<std::mutex> guard(sRuntimeLock);
        sRuntime.erase(player->GetObjectGuid());
    }

    void UpdateCharacter(Player* player)
    {
        Runtime& r = R(player);
        uint32 const now = WorldTimer::getMSTime();

        // Martyr: the damage taken since the last update strikes back.
        if (r.martyrPending && player->IsAlive())
        {
            uint32 const share = r.martyrPending;
            r.martyrPending = 0;
            if (SpellEntry const* aura = sSpellTemplate.LookupEntry<SpellEntry>(RETRIBUTION_AURA))
                for (Unit* unit : NearestFoes(player, player, MARTYR_RANGE, 10))
                    StrikeFoe(player, unit, aura, SPELL_SCHOOL_MASK_HOLY, share);
        }

        // Walking Consecration follows a few times a second.
        if (WorldTimer::getMSTimeDiff(r.lastWalk, now) >= WALK_MS)
        {
            r.lastWalk = now;
            if (player->IsAlive() && HasKeystone(player, KEY_WALKING_CONSECRATION))
                WalkConsecration(player, OwnConsecration(player));
        }

        if (r.lastRefresh && WorldTimer::getMSTimeDiff(r.lastRefresh, now) < REFRESH_MS)
            return;
        r.lastRefresh = now ? now : 1;
        Refresh(player, r, now);
    }

    float DamageDoneMod(Unit const* attacker, Unit const* victim, SpellEntry const* spellInfo)
    {
        // A pack follower hits for its share (ArpgPacks.h).
        if (attacker && attacker->GetTypeId() == TYPEID_UNIT)
            return PackDamageMod(attacker);
        std::shared_ptr<WebTotals const> totals = TotalsOf(attacker);
        if (!totals)
            return 1.0f;
        WebTotals const& t = *totals;
        Player const* player = static_cast<Player const*>(attacker);
        uint32 const tags = TagsOf(spellInfo);
        int32 pct = TagSum(t, tags & ~TagBit(TAG_HEAL));

        if ((tags & TagBit(TAG_MELEE)) && t.Has(KEY_TWO_HANDED_MASTERY) && TwoHanded(player))
            pct += 12;
        if ((tags & TagBit(TAG_MELEE)) && t.Has(KEY_SHIELD_AND_HAMMER) && OneHandAndShield(player))
            pct += 10;
        if (tags & TagBit(TAG_HOLY))
        {
            if (t.Has(KEY_HOLY_WEAPONS))
                pct += 3 * (int32(player->GetStat(STAT_STRENGTH)) / 10);
            if (t.Has(KEY_LIGHTFORGED))
                pct += 30;
            if (t.Has(KEY_RIGHTEOUS_FURY))
            {
                // (Not `near`: the Windows headers make that a macro.)
                size_t foes = 0;
                for (Unit* unit : EnemiesNear(const_cast<Unit*>(attacker), FURY_RANGE))
                    if (unit->IsAlive() && MayCatchUnit(const_cast<Unit*>(attacker), unit, nullptr) && ++foes >= FURY_COUNT)
                        break;
                if (foes >= FURY_COUNT)
                    pct += 20;
            }
        }
        uint32 const now = WorldTimer::getMSTime();
        if (t.Has(KEY_CRUSADE))
            pct += CRUSADE_PER_KILL * int32(CrusadeStacks(R(attacker), now));

        // The skill trees.
        SkillId const skill = SkillOfSpell(player->getClass(), spellInfo);
        if (skill == SKILL_STRIKE && t.Has(KEY_MOMENTUM))
        {
            Runtime const& r = R(attacker);
            if (WorldTimer::getMSTimeDiff(r.lastHit, now) <= MOMENTUM_WINDOW)
                pct += int32(r.momentum * t.Rank(KEY_MOMENTUM));
        }
        if ((tags & TagBit(TAG_PHYSICAL)) && t.Has(KEY_HEAVY_HAND) && TwoHanded(player))
            pct += 5 * t.Rank(KEY_HEAVY_HAND);
        if (skill == SKILL_SEALS)
        {
            pct += 6 * t.Rank(KEY_HOLY_EDGE);
            if (spellInfo->SpellName[0] && std::strncmp(spellInfo->SpellName[0], "Seal of Command", 15) == 0)
                pct += 10 * t.Rank(KEY_COMMANDING_SEAL);
        }
        if (skill == SKILL_JUDGEMENT)
            pct += 8 * t.Rank(KEY_RADIANCE);
        if (skill == SKILL_CONSECRATION)
            pct += 8 * t.Rank(KEY_BURNING_GROUND);
        if (victim && victim->IsStunned())
        {
            if (skill == SKILL_JUDGEMENT && t.Has(KEY_SENTENCE))
                pct += 100;
            if (t.Has(KEY_SENTENCE_PASSED))
                pct += 15;
        }
        if ((tags & TagBit(TAG_HOLY)) && t.Has(KEY_SEARING_LIGHT) && Consecrated(victim, attacker))
            pct += 5 * t.Rank(KEY_SEARING_LIGHT);
        return std::max(0.0f, 1.0f + pct / 100.0f);
    }

    float HealingDoneMod(Unit const* healer, SpellEntry const* /*spellInfo*/)
    {
        std::shared_ptr<WebTotals const> totals = TotalsOf(healer);
        if (!totals)
            return 1.0f;
        return std::max(0.0f, 1.0f + (totals->healingPct + totals->tag[TAG_HEAL]) / 100.0f);
    }

    float HealingTakenMod(Unit const* target)
    {
        return HasKeystone(target, KEY_MARTYR) ? 0.5f : 1.0f;
    }

    float AreaScale(Unit const* caster, SpellEntry const* spellInfo)
    {
        if (!spellInfo || !Active(caster))
            return 1.0f;
        uint32 const tags = TagsOf(spellInfo);
        if (!(tags & TagBit(TAG_AREA)))
            return 1.0f;
        std::shared_ptr<WebTotals const> totals = TotalsOf(caster);
        WebTotals const& t = totals ? *totals : NoTotals();
        int32 pct = (tags & TagBit(TAG_MELEE))
                    ? int32(caster->GetStat(STAT_STRENGTH)) / STR_PER_AREA + t.meleeAreaPct
                    : int32(caster->GetStat(STAT_INTELLECT)) / INT_PER_AREA + t.spellAreaPct;
        return 1.0f + std::clamp(pct, 0, AREA_CAP) / 100.0f;
    }

    float MoveSpeedMod(Unit const* unit)
    {
        if (unit && unit->GetTypeId() == TYPEID_UNIT)
            return PackSpeedMod(unit);
        if (!Active(unit))
            return 1.0f;
        std::shared_ptr<WebTotals const> totals = TotalsOf(unit);
        WebTotals const& t = totals ? *totals : NoTotals();
        int32 const pct = std::clamp(int32(unit->GetStat(STAT_AGILITY)) / AGI_PER_SPEED + t.movePct, 0, SPEED_CAP);
        float speed = 1.0f + pct / 100.0f;
        if (t.Has(KEY_CRUSADE) && !unit->IsInCombat())
            speed *= CRUSADE_SLOW;
        if (t.Has(KEY_UNYIELDING))
            speed *= UNYIELDING_SLOW;
        if (t.Has(KEY_CRUSADERS_PACE) && WorldTimer::getMSTime() < R(unit).paceUntil)
            speed *= PACE_SPEED;
        return speed;
    }

    float BlockChanceBonus(Unit const* unit)
    {
        std::shared_ptr<WebTotals const> totals = TotalsOf(unit);
        return totals ? float(totals->blockPct) : 0.0f;
    }

    bool ForcesCrit(Unit* caster, SpellEntry const* spellInfo)
    {
        if (!spellInfo || !HasKeystone(caster, KEY_DIVINE_FAVOUR) || !(TagsOf(spellInfo) & TagBit(TAG_HOLY)))
            return false;
        Runtime& r = R(caster);
        uint32 const now = WorldTimer::getMSTime();
        if (r.favourReady && now < r.favourReady)
            return false;
        r.favourReady = now + FAVOUR_COOLDOWN;
        return true;
    }

    bool BlocksHeal(Unit const* caster, Unit const* target)
    {
        return caster == target && HasKeystone(caster, KEY_LIGHTFORGED);
    }

    void OnCharacterHeal(Spell* spell, Unit* caster, Unit* target, uint32 amount)
    {
        if (!spell || spell->IsArpgSecondary() || !amount || !Active(caster))
            return;
        Player* player = static_cast<Player*>(caster);
        if (caster == target && HasKeystone(caster, KEY_BLESSED_RECOVERY))
        {
            Runtime& r = R(caster);
            r.recoveryUntil = WorldTimer::getMSTime() + RECOVERY_MS;
            r.lastRefresh = 0;
        }
        if (HasKeystone(caster, KEY_LIGHTFORGED))
        {
            std::vector<Unit*> foes = NearestFoes(player, player, LIGHTFORGED_RANGE, 1);
            if (!foes.empty())
                SendHolyBolt(player, foes.front(), amount);
        }
    }

    void OnDamageTaken(Unit* victim, Unit* attacker, uint32 damage)
    {
        OnPackDamage(attacker, victim, damage);
        if (!damage || !victim || attacker == victim || !HasKeystone(victim, KEY_MARTYR))
            return;
        R(victim).martyrPending += damage * MARTYR_SHARE_PCT / 100;
    }

    void OnBlocked(Unit* victim)
    {
        if (victim && victim->IsAlive() && HasKeystone(victim, KEY_SHIELD_WALL))
            victim->ModifyHealth(int32(std::max<uint32>(1, victim->GetMaxHealth() / 100)));
    }

    void OnCharacterKill(Player* killer, Unit* victim)
    {
        if (!victim || victim->GetTypeId() != TYPEID_UNIT)
            return;
        OnPackMemberDied(victim);
        CreditBossKill(killer, victim);
        if (!Active(killer) || !killer->IsAlive())
            return;
        if (static_cast<Creature*>(victim)->GetCreatureInfo()->CreatureType == CREATURE_TYPE_CRITTER)
            return;
        std::shared_ptr<WebTotals const> totals = TotalsOf(killer);
        WebTotals const& t = totals ? *totals : NoTotals();
        int32 const life = int32(killer->GetStat(STAT_STAMINA)) / STA_PER_LIFE + t.lifeOnKill;
        if (life > 0)
            killer->ModifyHealth(life);
        if (t.Has(KEY_CRUSADE))
        {
            Runtime& r = R(killer);
            r.kills.push_back(WorldTimer::getMSTime());
            while (r.kills.size() > CRUSADE_MAX)
                r.kills.pop_front();
        }
        if (t.Has(KEY_CRUSADERS_PACE))
        {
            R(killer).paceUntil = WorldTimer::getMSTime() + PACE_MS;
            killer->UpdateSpeed(MOVE_RUN, true);
            R(killer).speed = MoveSpeedMod(killer);
        }
    }

    float ExtraMeleeReach(Unit const* attacker)
    {
        return float(KeyRank(attacker, KEY_LONG_ARM));
    }

    void OnSwingHit(Unit* attacker, Unit* victim, uint32 damage, bool crit)
    {
        std::shared_ptr<WebTotals const> totals = TotalsOf(attacker);
        if (!totals || !victim)
            return;
        WebTotals const& t = *totals;
        Player* player = static_cast<Player*>(attacker);
        Runtime& r = R(attacker);
        uint32 const now = WorldTimer::getMSTime();

        // Rune of Leech in Strike.
        if (damage && attacker->IsAlive() && HasRune(player, SKILL_STRIKE, RUNE_LEECH))
            attacker->ModifyHealth(int32(std::max<uint32>(1, damage * RUNE_LEECH_PCT / 100)));

        if (t.Has(KEY_MOMENTUM) && damage)
        {
            r.momentum = WorldTimer::getMSTimeDiff(r.lastHit, now) <= MOMENTUM_WINDOW ? std::min(r.momentum + 1, MOMENTUM_MAX) : 1;
            r.lastHit = now;
        }
        if (!damage)
            return;
        float const reach = ExtraMeleeReach(attacker);
        bool const whirl = t.Has(KEY_WHIRLING_STRIKES) && ++r.swings % WHIRL_EVERY == 0;
        uint8 const wide = t.Rank(KEY_WIDE_SWING);
        if (whirl || wide)
        {
            uint32 const pct = whirl ? WHIRL_PCT : WIDE_SWING_PCT[std::min<uint8>(wide, 3) - 1];
            SpellEntry const* shown = sSpellTemplate.LookupEntry<SpellEntry>(whirl ? WHIRLWIND : CLEAVE);
            if (shown)
                for (Unit* unit : EnemiesNear(attacker, 8.0f + reach))
                    if (unit != victim && unit->IsAlive() && MayCatchUnit(attacker, unit, nullptr) &&
                            attacker->CanReachWithMeleeAttack(unit, reach) && (whirl || attacker->HasInArc(unit, SWING_ARC)))
                        StrikeFoe(attacker, unit, shown, SPELL_SCHOOL_MASK_NORMAL, std::max<uint32>(1, damage * pct / 100));
        }
        if (t.Has(KEY_STAGGER) && victim->IsAlive() && roll_chance_i(10 * t.Rank(KEY_STAGGER)))
            attacker->CastSpell(victim, DAZED, TRIGGERED_OLD_TRIGGERED);
        if (crit && t.Has(KEY_SHOCKWAVE_STRIKE))
            if (SpellEntry const* shown = sSpellTemplate.LookupEntry<SpellEntry>(WAR_STOMP))
                for (Unit* unit : FoesAlongLine(player, attacker->GetOrientation(), 0.0f, SHOCKWAVE_REACH, victim))
                    StrikeFoe(attacker, unit, shown, SPELL_SCHOOL_MASK_NORMAL, std::max<uint32>(1, damage * SHOCKWAVE_PCT / 100));
    }

    void OnSkillSpellDamage(Spell* spell, Unit* caster, Unit* victim, uint32 dealt)
    {
        std::shared_ptr<WebTotals const> totals = TotalsOf(caster);
        if (!totals || !spell || !victim)
            return;
        WebTotals const& t = *totals;
        Player* player = static_cast<Player*>(caster);
        SpellEntry const* spellInfo = spell->m_spellInfo;
        SkillId const skill = SkillOfSpell(player->getClass(), spellInfo);

        // Rune of Leech in the skill.
        if (dealt && skill != SKILL_NONE && caster->IsAlive() && HasRune(player, skill, RUNE_LEECH))
            caster->ModifyHealth(int32(std::max<uint32>(1, dealt * RUNE_LEECH_PCT / 100)));

        if (skill == SKILL_SEALS)
        {
            if (t.Has(KEY_MANA_STRIKE))
                player->ModifyPower(POWER_MANA, int32(player->GetMaxPower(POWER_MANA) * t.Rank(KEY_MANA_STRIKE) / 100));
            if (t.Has(KEY_LIGHT_OF_THE_CRUSADER) && player->IsAlive())
                player->ModifyHealth(int32(std::max<uint32>(1, dealt * 5 / 100)));
            if (t.Has(KEY_SACRED_SEAL) && victim->IsAlive() && Consecrated(victim, caster))
                StrikeFoe(caster, victim, spellInfo, spell->GetSchoolMask(), dealt);
        }
        if (skill == SKILL_JUDGEMENT)
        {
            if (t.Has(KEY_FINAL_VERDICT) && !victim->IsAlive())
                if (SpellEntry const* judgement = sSpellTemplate.LookupEntry<SpellEntry>(JUDGEMENT))
                    player->RemoveSpellCooldown(*judgement, true);
            if (t.Has(KEY_SANCTIFIED) && Consecrated(victim, caster))
                for (Unit* unit : NearestFoes(caster, victim, SANCTIFIED_RANGE, 10))
                    StrikeFoe(caster, unit, spellInfo, spell->GetSchoolMask(), std::max<uint32>(1, dealt * SANCTIFIED_PCT / 100));
        }
    }

    void OnSkillSpellLanded(Spell* spell, Unit* caster, Unit* target)
    {
        std::shared_ptr<WebTotals const> totals = TotalsOf(caster);
        if (!totals || !spell || !target || !caster->CanAttack(target))
            return;
        WebTotals const& t = *totals;
        Player* player = static_cast<Player*>(caster);
        SpellEntry const* spellInfo = spell->m_spellInfo;
        if (SkillOfSpell(player->getClass(), spellInfo) != SKILL_HAMMER)
            return;
        float const ap = player->GetTotalAttackPowerValue(BASE_ATTACK);
        if (t.Has(KEY_HOLY_HAMMER) && target->IsAlive())
            StrikeFoe(caster, target, spellInfo, SPELL_SCHOOL_MASK_HOLY,
                      std::max<uint32>(1, uint32(ap * (0.15f + 0.15f * t.Rank(KEY_HOLY_HAMMER)))));
        if (t.Has(KEY_RICOCHET))
            BounceCast(player, target, spellInfo, t.Rank(KEY_RICOCHET), RICOCHET_RANGE);
        if (t.Has(KEY_BLESSED_HAMMER))
            for (Unit* unit : NearestFoes(caster, caster, BLESSED_RANGE, BLESSED_COUNT))
                SendBolt(player, unit, spellInfo, SPELL_SCHOOL_MASK_HOLY, std::max<uint32>(1, uint32(ap * 0.5f)));
    }

    void OnJudgement(Unit* caster)
    {
        if (uint8 rank = KeyRank(caster, KEY_RIGHTEOUS_MIND))
            caster->ModifyPower(POWER_MANA, int32(caster->GetMaxPower(POWER_MANA) * 5 * rank / 100));
    }

    float ExtraPpm(Unit const* caster, SpellEntry const* spellInfo)
    {
        if (!spellInfo || !spellInfo->SpellName[0] || std::strncmp(spellInfo->SpellName[0], "Seal of Command", 15) != 0)
            return 0.0f;
        return float(KeyRank(caster, KEY_RELENTLESS));
    }

    bool KeepsSecondSeal(Unit const* unit, SpellEntry const* adding, SpellEntry const* existing)
    {
        if (!adding || !existing || !IsSealSpell(adding) || !IsSealSpell(existing) || !HasKeystone(unit, KEY_TWIN_SEALS))
            return false;
        uint32 const chain = sSpellMgr.GetFirstSpellInChain(adding->Id);
        if (sSpellMgr.GetFirstSpellInChain(existing->Id) == chain)
            return false;
        // Keep it if it is the only other Seal on.
        std::set<uint32> others;
        for (auto const& [id, holder] : unit->GetSpellAuraHolderMap())
        {
            SpellEntry const* proto = holder->GetSpellProto();
            if (holder->GetCasterGuid() == unit->GetObjectGuid() && IsSealSpell(proto) &&
                    sSpellMgr.GetFirstSpellInChain(proto->Id) != chain)
                others.insert(sSpellMgr.GetFirstSpellInChain(proto->Id));
        }
        return others.size() < 2;
    }
}
