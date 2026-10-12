/*
 * ARPG item comparison: see ArpgCompare.h.
 */

#include "Arpg/ArpgCompare.h"
#include "Arpg/ArpgAffixes.h"
#include "Arpg/ArpgCharacter.h"
#include "Arpg/ArpgCombat.h"
#include "Arpg/ArpgDamage.h"
#include "Arpg/ArpgSkills.h"
#include "Arpg/ArpgTags.h"
#include "Arpg/ArpgTree.h"

#include "Entities/Bag.h"
#include "Entities/Item.h"
#include "Entities/Player.h"
#include "Server/DBCStores.h"
#include "Server/Opcodes.h"
#include "Server/WorldPacket.h"
#include "Server/WorldSession.h"
#include "Spells/SpellMgr.h"
#include "Util/Timer.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    using namespace Arpg;

    constexpr uint8 DAMAGE_TAGS = 7;
    // The ailments' share of the hit (Arpg/ArpgDamage.cpp), by damage tag: Bleed, Judged, Ignite,
    // Chill, Overload, Poison, Wither.
    constexpr float AILMENT_SHARE[DAMAGE_TAGS] = { 0.6f, 0.25f, 0.8f, 0.0f, 0.5f, 0.3f, 0.0f };
    // The caps the totals have where they act (Arpg/ArpgCharacter.cpp).
    constexpr int32 ATTACK_SPEED_CAP = 50, CRIT_CAP = 30;
    // At most this many items are compared, the bags' first.
    constexpr size_t COMPARE_MAX = 120;

    // An item's vanilla stats: its own, its equip spells' and its enchantments' (an "of the Bear"
    // suffix's among them).
    struct ItemStats
    {
        int32 stat[MAX_STATS] = {};
        int32 attackPower = 0;
        int32 spellPower[MAX_SPELL_SCHOOL] = {};
        float meleeCrit = 0.0f;
        float spellCrit = 0.0f;
        bool weapon = false;      // a main hand weapon: its swing below
        float weaponAverage = 0.0f;
        float weaponSpeed = 0.0f; // seconds
    };

    void AddAura(SpellEntry const* spell, ItemStats& out, int32 sign)
    {
        if (!spell)
            return;
        for (uint32 i = 0; i < MAX_EFFECT_INDEX; ++i)
        {
            if (spell->Effect[i] != SPELL_EFFECT_APPLY_AURA)
                continue;
            int32 const amount = (spell->EffectBasePoints[i] + 1) * sign;
            switch (spell->EffectApplyAuraName[i])
            {
                case SPELL_AURA_MOD_ATTACK_POWER:
                    out.attackPower += amount;
                    break;
                case SPELL_AURA_MOD_DAMAGE_DONE:
                    for (uint32 school = SPELL_SCHOOL_HOLY; school < MAX_SPELL_SCHOOL; ++school)
                        if (spell->EffectMiscValue[i] & (1 << school))
                            out.spellPower[school] += amount;
                    break;
                case SPELL_AURA_MOD_CRIT_PERCENT:
                    out.meleeCrit += float(amount);
                    break;
                case SPELL_AURA_MOD_SPELL_CRIT_CHANCE:
                    out.spellCrit += float(amount);
                    break;
                case SPELL_AURA_MOD_STAT:
                    if (spell->EffectMiscValue[i] >= 0 && spell->EffectMiscValue[i] < MAX_STATS)
                        out.stat[spell->EffectMiscValue[i]] += amount;
                    break;
                default:
                    break;
            }
        }
    }

    void AddStat(uint32 type, int32 value, ItemStats& out)
    {
        switch (type)
        {
            case ITEM_MOD_STRENGTH:  out.stat[STAT_STRENGTH] += value; break;
            case ITEM_MOD_AGILITY:   out.stat[STAT_AGILITY] += value; break;
            case ITEM_MOD_STAMINA:   out.stat[STAT_STAMINA] += value; break;
            case ITEM_MOD_INTELLECT: out.stat[STAT_INTELLECT] += value; break;
            case ITEM_MOD_SPIRIT:    out.stat[STAT_SPIRIT] += value; break;
            default: break;
        }
    }

    // Add (`sign` 1) or take off (-1) `item`'s vanilla stats; `mainHand`: it is (or would be) the
    // main hand weapon.
    void AddItemStats(Item const* item, ItemStats& out, int32 sign, bool mainHand)
    {
        if (!item)
            return;
        ItemPrototype const* proto = item->GetProto();
        for (uint32 i = 0; i < MAX_ITEM_PROTO_STATS; ++i)
            if (proto->ItemStat[i].ItemStatValue)
                AddStat(proto->ItemStat[i].ItemStatType, proto->ItemStat[i].ItemStatValue * sign, out);
        for (uint32 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
            if (proto->Spells[i].SpellId && proto->Spells[i].SpellTrigger == ITEM_SPELLTRIGGER_ON_EQUIP)
                AddAura(sSpellTemplate.LookupEntry<SpellEntry>(proto->Spells[i].SpellId), out, sign);
        for (uint32 slot = 0; slot < MAX_ENCHANTMENT_SLOT; ++slot)
        {
            uint32 const id = item->GetEnchantmentId(EnchantmentSlot(slot));
            SpellItemEnchantmentEntry const* enchant = id ? sSpellItemEnchantmentStore.LookupEntry(id) : nullptr;
            if (!enchant)
                continue;
            for (uint32 s = 0; s < 3; ++s)
            {
                if (enchant->type[s] == ITEM_ENCHANTMENT_TYPE_STAT)
                    AddStat(enchant->spellid[s], int32(enchant->amount[s]) * sign, out);
                else if (enchant->type[s] == ITEM_ENCHANTMENT_TYPE_EQUIP_SPELL)
                    AddAura(sSpellTemplate.LookupEntry<SpellEntry>(enchant->spellid[s]), out, sign);
            }
        }
        if (mainHand && proto->Class == ITEM_CLASS_WEAPON && sign > 0)
        {
            out.weapon = true;
            out.weaponAverage = (proto->Damage[0].DamageMin + proto->Damage[0].DamageMax) / 2.0f;
            out.weaponSpeed = proto->Delay / 1000.0f;
        }
    }

    // What a skill's score is worked out from: its sample spell (the best known rank that deals
    // damage; nullptr for Strike) and its tags.
    struct Sample
    {
        uint8 skill = 0;
        SpellEntry const* spell = nullptr;
        SpellEffectIndex effect = EFFECT_INDEX_0;
        uint32 tags = 0;
        char const* name = "";
    };

    bool SampleOf(Player const* player, uint8 skill, Sample& out)
    {
        out.skill = skill;
        if (skill == SKILL_STRIKE)
        {
            out.tags = TagsOf(nullptr);
            out.name = "Strike";
            return true;
        }
        for (auto const& [id, known] : player->GetSpellMap())
        {
            if (known.state == PLAYERSPELL_REMOVED || known.disabled)
                continue;
            SpellEntry const* info = sSpellTemplate.LookupEntry<SpellEntry>(id);
            if (!info || SkillOfSpell(player->getClass(), info) != SkillId(skill))
                continue;
            for (uint32 i = 0; i < MAX_EFFECT_INDEX; ++i)
            {
                bool const direct = info->Effect[i] == SPELL_EFFECT_SCHOOL_DAMAGE;
                bool const dot = (info->Effect[i] == SPELL_EFFECT_APPLY_AURA || info->Effect[i] == SPELL_EFFECT_PERSISTENT_AREA_AURA) &&
                                 info->EffectApplyAuraName[i] == SPELL_AURA_PERIODIC_DAMAGE;
                if ((direct || dot) && (!out.spell || info->spellLevel > out.spell->spellLevel))
                {
                    out.spell = info;
                    out.effect = SpellEffectIndex(i);
                }
            }
        }
        if (!out.spell)
            return false;
        out.tags = TagsOf(out.spell) & ~TagBit(TAG_HEAL);
        out.name = out.spell->SpellName[0] ? out.spell->SpellName[0] : "?";
        return true;
    }

    // The score of `s` with totals `t` and the vanilla stats changed by `d` (all zero: as now).
    float Score(Player* player, Sample const& s, WebTotals const& t, WebTotals const& now, ItemStats const& d)
    {
        uint32 const tags = s.tags;
        int32 increased;
        float more;
        DamagePartsOf(player, t, s.spell, tags, increased, more);
        // The attributes' increased damage moves with the stats the item changes.
        auto stat = [&](Stats which) { return int32(player->GetStat(which)); };
        auto moved = [&](Stats which, int32 per) { return (stat(which) + d.stat[which]) / per - stat(which) / per; };
        if (tags & (TagBit(TAG_MELEE) | TagBit(TAG_PHYSICAL)))
            increased += moved(STAT_STRENGTH, STR_PER_DAMAGE);
        if (tags & TagBit(TAG_SPELL))
            increased += moved(STAT_INTELLECT, INT_PER_DAMAGE);
        if (tags & TagBit(TAG_DURATION))
            increased += moved(STAT_SPIRIT, SPI_PER_DOT);
        float const mod = std::max(0.0f, 1.0f + increased / 100.0f) * more;

        // A spell's critical strikes by its damage class, as the hit's (Unit::GetCritMultiplier).
        bool const spell = s.spell && (s.spell->DmgClass == SPELL_DAMAGE_CLASS_MAGIC || s.spell->DmgClass == SPELL_DAMAGE_CLASS_NONE);
        float base = 0.0f;
        float rate = 1.0f;
        if (!s.spell)
        {
            // The swing: as now, with the attack power and the weapon the item changes.
            uint8 const cls = player->getClass();
            bool const strong = cls == CLASS_WARRIOR || cls == CLASS_PALADIN || cls == CLASS_SHAMAN || cls == CLASS_DRUID;
            bool const nimble = cls == CLASS_ROGUE || cls == CLASS_HUNTER;
            float const ap = player->GetTotalAttackPowerValue(BASE_ATTACK);
            float const apAfter = ap + d.attackPower + d.stat[STAT_STRENGTH] * (strong ? 2 : 1) + (nimble ? d.stat[STAT_AGILITY] : 0);
            // The swing's time now (hasted) and the weapons' own delays (not): a hit's attack power
            // share goes by the delay, how often it lands by the hasted time.
            float const hasted = std::max(0.1f, player->GetAttackTime(BASE_ATTACK) / 1000.0f);
            Item const* main = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
            float const delay = main && main->GetProto()->Delay ? main->GetProto()->Delay / 1000.0f : hasted;
            float const weapon = main ? (main->GetProto()->Damage[0].DamageMin + main->GetProto()->Damage[0].DamageMax) / 2.0f : 0.0f;
            float const swing = (player->GetFloatValue(UNIT_FIELD_MINDAMAGE) + player->GetFloatValue(UNIT_FIELD_MAXDAMAGE)) / 2.0f;
            float const delayAfter = d.weapon && d.weaponSpeed > 0.0f ? d.weaponSpeed : delay;
            float const weaponAfter = d.weapon ? d.weaponAverage : weapon;
            base = std::max(1.0f, swing - weapon - ap / 14.0f * delay + weaponAfter + apAfter / 14.0f * delayAfter);
            // A second's worth: the new delay, hasted as now, and the attack speed affixes' change.
            float const haste = 1.0f + std::clamp(t.attackSpeedPct, 0, ATTACK_SPEED_CAP) / 100.0f;
            float const hasteNow = 1.0f + std::clamp(now.attackSpeedPct, 0, ATTACK_SPEED_CAP) / 100.0f;
            float const hastedAfter = std::max(0.1f, delayAfter * (hasted / std::max(0.1f, delay)) * hasteNow / haste);
            rate = 1.0f / hastedAfter;
        }
        else
        {
            SpellSchoolMask const mask = GetSpellSchoolMask(s.spell);
            int32 power = player->SpellBaseDamageBonusDone(mask);
            for (uint32 school = SPELL_SCHOOL_HOLY; school < MAX_SPELL_SCHOOL; ++school)
                if (mask & (1 << school))
                {
                    power += d.spellPower[school];
                    break;
                }
            int32 const points = s.spell->EffectBasePoints[s.effect] + 1 + std::max(0, s.spell->EffectDieSides[s.effect] - 1) / 2;
            base = float(std::max(1, points + player->SpellBonusWithCoeffs(s.spell, s.effect, 0, power, 0, true)));
        }
        float const hit = base * mod;

        // Critical strikes: the chance now, moved by the item's crit and the affixes'.
        float chance;
        if (spell)
            chance = player->GetCritChance(GetSpellSchoolMask(s.spell)) + d.spellCrit +
                     float(std::clamp(t.spellCritPct, 0, CRIT_CAP) - std::clamp(now.spellCritPct, 0, CRIT_CAP));
        else
            chance = player->GetCritChance(BASE_ATTACK) + d.meleeCrit +
                     float(std::clamp(t.meleeCritPct, 0, CRIT_CAP) - std::clamp(now.meleeCritPct, 0, CRIT_CAP));
        chance = std::clamp(chance, 0.0f, 100.0f) / 100.0f;
        int32 const critPct = (stat(STAT_AGILITY) + d.stat[STAT_AGILITY]) / AGI_PER_CRIT + t.critDamagePct + (spell ? t.spellCritDamagePct : 0);
        float const multiplier = (spell ? 1.5f : 2.0f) + std::max(0, critPct) / 100.0f;
        float const critFactor = 1.0f + chance * (multiplier - 1.0f);

        // Added damage: the two biggest types, at vanilla's size like the base.
        int32 const* added = (tags & TagBit(TAG_MELEE)) ? t.addedAttack : t.addedSpell;
        std::vector<std::pair<int32, uint8>> types;
        for (uint8 tag = 0; tag < DAMAGE_TAGS; ++tag)
            if (added[tag] > 0)
                types.emplace_back(added[tag], tag);
        std::sort(types.begin(), types.end(), [](auto const& a, auto const& b) { return a.first > b.first; });
        if (types.size() > 2)
            types.resize(2);
        float const scale = std::max(1.0f, PowerScale(player));

        // Ailments: inflicted on a critical strike, or by the chance on any hit.
        float const ailmentMod = std::max(0.0f, 1.0f + (t.ailmentPct + t.tag[TAG_DURATION] +
                                 (stat(STAT_SPIRIT) + d.stat[STAT_SPIRIT]) / SPI_PER_DOT) / 100.0f);
        auto ailment = [&](uint8 tag, float amount)
        {
            float const inflict = chance + (1.0f - chance) * std::clamp(t.ailmentChance[tag], 0, 100) / 100.0f;
            return inflict * AILMENT_SHARE[tag] * amount * ailmentMod;
        };
        float total = hit * critFactor;
        for (uint8 tag = 0; tag < DAMAGE_TAGS; ++tag)
            if (tags & TagBit(Tag(tag)))
                total += ailment(tag, hit);
        for (auto const& [average, tag] : types)
        {
            float const extra = float(average) / scale * mod;
            total += extra * critFactor + ailment(tag, extra);
        }
        return total * rate;
    }

    struct Sent
    {
        size_t hash = 0;
        uint32 at = 0;            // when it was last worked out
    };
    // Worked out at most this often (a buff's stats flicker), and kept under this many bytes.
    constexpr uint32 COMPARE_EVERY = 3000;
    constexpr size_t COMPARE_BYTES = 40000;

    std::mutex sLock;
    std::unordered_map<ObjectGuid, Sent> sSent;

    // The slot `item` would go in, and what is there now.
    uint8 SlotFor(Player const* player, Item const* item)
    {
        uint8 const slot = player->FindEquipSlot(item->GetProto(), NULL_SLOT, true);
        return slot < EQUIPMENT_SLOT_END ? slot : uint8(NULL_SLOT);
    }

    std::string Format(char const* format, ...) ATTR_PRINTF(1, 2);
    std::string Format(char const* format, ...)
    {
        char buf[256];
        va_list args;
        va_start(args, format);
        std::vsnprintf(buf, sizeof(buf), format, args);
        va_end(args);
        return buf;
    }

    // The comparison lines for `item`, carried by `player`, against `samples`.
    std::string LinesFor(Player* player, Item const* item, std::vector<Sample> const& samples, WebTotals const& now,
                         std::vector<float> const& scores)
    {
        uint8 const slot = SlotFor(player, item);
        if (slot == NULL_SLOT || player->CanUseItem(item->GetProto()) != EQUIP_ERR_OK)
            return std::string();
        Item const* worn = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        WebTotals then = now;
        ItemStats d;
        // A broken item's affixes and stats count for nothing now.
        auto takeOff = [&](Item const* off)
        {
            if (off && !off->IsBroken())
            {
                AddAffixTotals(off, then, -1);
                AddItemStats(off, d, -1, false);
            }
        };
        takeOff(worn);
        // A two-hander takes the off hand's place too.
        if (item->GetProto()->InventoryType == INVTYPE_2HWEAPON && slot == EQUIPMENT_SLOT_MAINHAND)
            takeOff(player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND));
        AddAffixTotals(item, then, 1);
        AddItemStats(item, d, 1, slot == EQUIPMENT_SLOT_MAINHAND);
        std::vector<std::string> lines;
        lines.push_back(worn ? Format("|cff9d9d9dWorn in place of %s:|r", worn->GetProto()->Name1)
                             : std::string("|cff9d9d9dWorn:|r"));
        for (size_t i = 0; i < samples.size(); ++i)
        {
            if (scores[i] <= 0.0f)
                continue;
            float const after = Score(player, samples[i], then, now, d);
            int32 const pct = int32(std::lround((after / scores[i] - 1.0f) * 100.0f));
            char const* colour = pct > 0 ? "|cff20ff20" : pct < 0 ? "|cffff4040" : "|cff9d9d9d";
            lines.push_back(Format("%s%s %s%d%% damage|r", colour, samples[i].name, pct > 0 ? "+" : "", pct));
        }
        if (lines.size() == 1)
            return std::string();
        std::string out;
        for (std::string const& line : lines)
            out += (out.empty() ? "" : "\n") + line;
        return out;
    }

    // What the lines depend on, in one number.
    size_t KeyOf(Player* player, std::vector<Item*> const& items, WebTotals const& t)
    {
        size_t key = player->GetLevel();
        auto mix = [&key](size_t v) { key ^= v + 0x9e3779b97f4a7c15ull + (key << 6) + (key >> 2); };
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (Item const* worn = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                mix(worn->GetGUIDLow() * 31 + slot + (worn->IsBroken() ? 7 : 0));
        for (Item const* item : items)
        {
            mix(item->GetGUIDLow());
            mix(AffixStateOf(item));
            for (uint32 e = 0; e < MAX_ENCHANTMENT_SLOT; ++e)
                mix(item->GetEnchantmentId(EnchantmentSlot(e)));
        }
        for (uint8 skill : SlottedSkills(player))
            mix(skill);
        for (int32 v : t.tag)
            mix(size_t(v));
        for (int i = 0; i < 7; ++i)
            mix(size_t(t.addedAttack[i] * 7 + t.addedSpell[i] * 13 + t.ailmentChance[i]));
        mix(size_t(t.critDamagePct * 3 + t.ailmentPct * 5 + t.attackSpeedPct * 7 + t.meleeCritPct * 11 + t.spellCritPct * 13 +
                   t.spellCritDamagePct * 17));
        mix(player->GetSpellMap().size());
        for (uint8 rank : t.rank)
            mix(rank);
        for (Stats s : { STAT_STRENGTH, STAT_AGILITY, STAT_INTELLECT, STAT_SPIRIT })
            mix(size_t(player->GetStat(s)));
        mix(size_t(player->GetTotalAttackPowerValue(BASE_ATTACK)));
        return key;
    }
}

namespace Arpg
{
    void UpdateCompare(Player* player)
    {
        if (!player || !Active(player) || !player->GetSession())
            return;
        std::shared_ptr<WebTotals const> totals = TotalsOf(player);
        if (!totals)
            return;
        // The wearable items carried: the backpack's, then the bags'.
        std::vector<Item*> items;
        auto consider = [&](Item* item)
        {
            if (item && items.size() < COMPARE_MAX && Craftable(item))
                items.push_back(item);
        };
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            consider(player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
        for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
            if (Bag* container = dynamic_cast<Bag*>(player->GetItemByPos(INVENTORY_SLOT_BAG_0, bag)))
                for (uint32 i = 0; i < container->GetBagSize(); ++i)
                    consider(container->GetItemByPos(uint8(i)));

        size_t const key = KeyOf(player, items, *totals);
        {
            std::lock_guard<std::mutex> guard(sLock);
            Sent& sent = sSent[player->GetObjectGuid()];
            uint32 const at = WorldTimer::getMSTime();
            if (sent.hash == key || (sent.at && WorldTimer::getMSTimeDiff(sent.at, at) < COMPARE_EVERY))
                return;
            sent.hash = key;
            sent.at = at ? at : 1;
        }

        std::vector<Sample> samples;
        for (uint8 skill : SlottedSkills(player))
        {
            Sample s;
            if (SampleOf(player, skill, s))
                samples.push_back(s);
        }
        std::vector<float> scores;
        ItemStats const none;
        for (Sample const& s : samples)
            scores.push_back(Score(player, s, *totals, *totals, none));

        std::vector<std::pair<ObjectGuid, std::string>> out;
        size_t bytes = 2;
        if (!samples.empty())
            for (Item const* item : items)
            {
                std::string lines = LinesFor(player, item, samples, *totals, scores);
                if (lines.empty())
                    continue;
                bytes += 9 + lines.size();
                if (bytes > COMPARE_BYTES)
                    break;
                out.emplace_back(item->GetObjectGuid(), std::move(lines));
            }
        WorldPacket data(SMSG_ARPG_COMPARE, 2 + out.size() * 96);
        data << uint16(out.size());
        for (auto const& [guid, lines] : out)
            data << guid << lines;
        player->GetSession()->SendPacket(data);
    }

    void ForgetCompare(Player* player)
    {
        std::lock_guard<std::mutex> guard(sLock);
        sSent.erase(player->GetObjectGuid());
    }
}
