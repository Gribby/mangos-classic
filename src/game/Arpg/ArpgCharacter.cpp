/*
 * ARPG character: see ArpgCharacter.h.
 */

#include "Arpg/ArpgCharacter.h"
#include "Arpg/ArpgCombat.h"
#include "Arpg/ArpgTags.h"
#include "Arpg/ArpgTree.h"
#include "Arpg/ArpgUniques.h"

#include "Entities/Creature.h"
#include "Entities/Item.h"
#include "Entities/Player.h"
#include "Spells/Spell.h"
#include "Spells/SpellAuras.h"
#include "Spells/SpellMgr.h"
#include "Util/Timer.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace
{
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

    // What has been applied to a player, and the keystones' running state.
    struct Runtime
    {
        int32 stat[5] = {};
        int32 armourPct = 0;
        bool zealot = false;
        bool unyielding = false;
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

    // The once-a-second refresh: speed, cooldown recovery, Zealot's drain, a level gained.
    void Refresh(Player* player, Runtime& r, uint32 now)
    {
        UpdateTree(player);
        std::shared_ptr<WebTotals const> totals = TotalsOf(player);
        WebTotals const& t = totals ? *totals : NoTotals();

        float const speed = MoveSpeedMod(player);
        if (std::abs(speed - r.speed) > 0.001f)
        {
            r.speed = speed;
            player->UpdateSpeed(MOVE_RUN, true);
        }
        SetCooldownMod(player, r, CooldownPct(player, t, r, now));

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

        bool const zealot = t.Has(KEY_ZEALOT);
        if (zealot != r.zealot)
        {
            player->ApplyAttackTimePercentMod(BASE_ATTACK, ZEALOT_HASTE, zealot);
            player->ApplyAttackTimePercentMod(OFF_ATTACK, ZEALOT_HASTE, zealot);
            r.zealot = zealot;
        }
        bool const unyielding = t.Has(KEY_UNYIELDING);
        if (unyielding != r.unyielding)
        {
            player->ApplySpellImmune(nullptr, IMMUNITY_MECHANIC, MECHANIC_STUN, unyielding);
            player->ApplySpellImmune(nullptr, IMMUNITY_MECHANIC, MECHANIC_SNARE, unyielding);
            player->ApplySpellImmune(nullptr, IMMUNITY_STATE, SPELL_AURA_MOD_DECREASE_SPEED, unyielding);
            if (unyielding)
            {
                player->RemoveSpellsCausingAura(SPELL_AURA_MOD_STUN);
                player->RemoveSpellsCausingAura(SPELL_AURA_MOD_DECREASE_SPEED);
            }
            r.unyielding = unyielding;
        }
        // Speed and cooldowns at once, not at the next refresh.
        r.lastRefresh = 0;
    }

    void ForgetCharacter(Player* player)
    {
        // The player is going; its cooldown modifier stays in its spell modifier list, which
        // Player never frees, as an aura's stays until the aura goes.
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

        if (r.lastRefresh && WorldTimer::getMSTimeDiff(r.lastRefresh, now) < REFRESH_MS)
            return;
        r.lastRefresh = now ? now : 1;
        Refresh(player, r, now);
    }

    float DamageDoneMod(Unit const* attacker, Unit const* /*victim*/, SpellEntry const* spellInfo)
    {
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
                size_t near = 0;
                for (Unit* unit : EnemiesNear(const_cast<Unit*>(attacker), FURY_RANGE))
                    if (unit->IsAlive() && MayCatchUnit(const_cast<Unit*>(attacker), unit, nullptr) && ++near >= FURY_COUNT)
                        break;
                if (near >= FURY_COUNT)
                    pct += 20;
            }
        }
        if (t.Has(KEY_CRUSADE))
            pct += CRUSADE_PER_KILL * int32(CrusadeStacks(R(attacker), WorldTimer::getMSTime()));
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
    }
}
