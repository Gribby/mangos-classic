/*
 * ARPG resources: see ArpgResources.h.
 */

#include "Arpg/ArpgResources.h"
#include "Arpg/ArpgDungeons.h"
#include "Arpg/ArpgShrines.h"
#include "Arpg/ArpgCombat.h"

#include "Entities/Player.h"
#include "Server/Opcodes.h"
#include "Server/WorldPacket.h"
#include "Server/WorldSession.h"
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

    // In-combat mana regeneration, as a share of maximum mana a second. (The play statistics
    // showed no pressure at 1.5: a mage never fell below 61% with Fireball alone.)
    constexpr float REGEN_PCT = 1.0f;
    // A filler's mana back on a hit, and a sealed swing's, as shares of maximum mana (a swing
    // comes twice as often since the melee pace, ArpgCharacter.cpp, so its share is half).
    constexpr uint32 FILLER_PCT = 4;
    constexpr uint32 SEALED_SWING_PCT = 1;
    // A spell not in the table costs its vanilla share of base mana, this much of it, of maximum
    // mana; at least 1% and at most MISC_MAX_PCT.
    constexpr float MISC_SHARE = 0.5f;
    constexpr float MISC_MAX_PCT = 30.0f;

    // The class's main spells, by name, and their cost as a share of maximum mana.
    struct CostRow { char const* name; float pct; };
    CostRow const COSTS[] =
    {
        // Mage: Frostbolt is the free filler.
        { "Frostbolt", 0.0f }, { "Fireball", 15.0f }, { "Fire Blast", 0.0f }, { "Scorch", 6.0f },
        { "Pyroblast", 20.0f }, { "Arcane Missiles", 18.0f }, { "Arcane Explosion", 12.0f },
        { "Blizzard", 25.0f }, { "Flamestrike", 20.0f }, { "Cone of Cold", 12.0f },
        { "Frost Nova", 0.0f }, { "Blink", 0.0f }, { "Ice Barrier", 10.0f }, { "Mana Shield", 0.0f },
        // Paladin: the swing with a Seal on is the generator.
        { "Seal of Righteousness", 6.0f }, { "Seal of Command", 6.0f }, { "Seal of the Crusader", 6.0f },
        { "Seal of Justice", 6.0f }, { "Seal of Light", 6.0f }, { "Seal of Wisdom", 6.0f },
        { "Judgement", 0.0f }, { "Consecration", 22.0f }, { "Hammer of Wrath", 10.0f },
        { "Holy Light", 20.0f }, { "Flash of Light", 12.0f }, { "Exorcism", 12.0f }, { "Holy Shock", 12.0f },
        { "Hammer of Justice", 0.0f }, { "Divine Shield", 0.0f }, { "Divine Protection", 0.0f },
        // Druid: Wrath is the free filler; changing form is free.
        { "Wrath", 0.0f }, { "Moonfire", 8.0f }, { "Starfire", 15.0f }, { "Hurricane", 25.0f },
        { "Insect Swarm", 8.0f }, { "Entangling Roots", 8.0f }, { "Healing Touch", 20.0f },
        { "Rejuvenation", 10.0f }, { "Regrowth", 15.0f },
        { "Bear Form", 0.0f }, { "Dire Bear Form", 0.0f }, { "Cat Form", 0.0f }, { "Travel Form", 0.0f },
        { "Aquatic Form", 0.0f }, { "Moonkin Form", 0.0f },
        // Warlock: Searing Pain is the free filler; Life Tap costs health, not mana.
        { "Shadow Bolt", 9.0f }, { "Searing Pain", 0.0f }, { "Shadowburn", 10.0f }, { "Immolate", 8.0f },
        { "Conflagrate", 8.0f }, { "Rain of Fire", 25.0f }, { "Hellfire", 20.0f }, { "Corruption", 6.0f },
        { "Curse of Agony", 6.0f }, { "Siphon Life", 8.0f }, { "Drain Life", 10.0f }, { "Drain Soul", 8.0f },
        { "Death Coil", 12.0f }, { "Life Tap", 0.0f }, { "Summon Imp", 15.0f }, { "Summon Voidwalker", 15.0f },
        { "Summon Succubus", 15.0f }, { "Summon Felhunter", 15.0f }, { "Inferno", 20.0f }, { "Ritual of Doom", 30.0f },
        { "Fear", 6.0f }, { "Howl of Terror", 10.0f }, { "Curse of Weakness", 5.0f }, { "Curse of Tongues", 5.0f },
        { "Curse of Recklessness", 5.0f }, { "Curse of the Elements", 5.0f }, { "Curse of Shadow", 5.0f },
        { "Curse of Doom", 10.0f }, { "Demon Skin", 0.0f }, { "Demon Armor", 0.0f }, { "Shadow Ward", 8.0f },
        // Shaman: Lightning Bolt is the free filler, Chain Lightning the spender; the imbues and
        // Ghost Wolf are free.
        { "Lightning Bolt", 0.0f }, { "Chain Lightning", 16.0f }, { "Earth Shock", 8.0f }, { "Flame Shock", 8.0f },
        { "Frost Shock", 8.0f }, { "Lightning Shield", 4.0f }, { "Stormstrike", 6.0f }, { "Purge", 4.0f },
        { "Searing Totem", 6.0f }, { "Magma Totem", 14.0f }, { "Fire Nova Totem", 12.0f },
        { "Stoneclaw Totem", 6.0f }, { "Earthbind Totem", 4.0f }, { "Strength of Earth Totem", 5.0f },
        { "Windfury Totem", 6.0f }, { "Grace of Air Totem", 6.0f }, { "Tremor Totem", 4.0f },
        { "Healing Stream Totem", 6.0f }, { "Mana Spring Totem", 4.0f },
        { "Healing Wave", 18.0f }, { "Lesser Healing Wave", 11.0f }, { "Chain Heal", 18.0f },
        { "Ghost Wolf", 0.0f }, { "Rockbiter Weapon", 0.0f }, { "Flametongue Weapon", 0.0f },
        { "Frostbrand Weapon", 0.0f }, { "Windfury Weapon", 0.0f },
    };

    CostRow const* RowOf(SpellEntry const* spellInfo)
    {
        char const* name = spellInfo->SpellName[0];
        if (!name)
            return nullptr;
        for (CostRow const& row : COSTS)
            if (std::strcmp(row.name, name) == 0)
                return &row;
        return nullptr;
    }

    bool IsFiller(SpellEntry const* spellInfo)
    {
        CostRow const* row = RowOf(spellInfo);
        return row && row->pct == 0.0f &&
            (std::strcmp(row->name, "Frostbolt") == 0 || std::strcmp(row->name, "Wrath") == 0 ||
             std::strcmp(row->name, "Searing Pain") == 0 || std::strcmp(row->name, "Lightning Bolt") == 0);
    }

    bool Sealed(Unit const* unit)
    {
        for (auto const& [id, holder] : unit->GetSpellAuraHolderMap())
            if (holder && IsSealSpell(holder->GetSpellProto()))
                return true;
        return false;
    }

    // A filler refunds at most once in this long (one cast's hits).
    constexpr uint32 FILLER_REFUND_GAP_MS = 600;

    // What the client was last told, as one signature: maximum mana, the spells known, the cost
    // fields gear and auras move, and the costs' while-effects (Channeling, Thrift).
    std::mutex sLock;
    std::unordered_map<ObjectGuid, uint64> sTold;
    std::unordered_map<ObjectGuid, uint32> sLastRefund;

    uint64 Signature(Player const* player)
    {
        uint64 sig = 1469598103934665603ULL;
        auto mix = [&sig](uint64 v) { sig = (sig ^ v) * 1099511628211ULL; };
        mix(player->GetMaxPower(POWER_MANA));
        uint64 spells = 0;
        for (auto const& [id, spell] : player->GetSpellMap())
            if (spell.state != PLAYERSPELL_REMOVED && !spell.disabled)
                spells += uint64(id) * 2654435761ULL;   // order-free
        mix(spells);
        for (uint8 school = 0; school < MAX_SPELL_SCHOOL; ++school)
        {
            mix(uint32(player->GetInt32Value(UNIT_FIELD_POWER_COST_MODIFIER + school)));
            float const mult = player->GetFloatValue(UNIT_FIELD_POWER_COST_MULTIPLIER + school);
            uint32 bits;
            std::memcpy(&bits, &mult, sizeof(bits));
            mix(bits);
        }
        mix(ShrineFreeCasting(player) ? 1 : 0);
        Map* map = player->GetMap();
        mix(map && map->IsDungeon() && NightmareHas(map, NM_THRIFT) ? 1 : 0);
        return sig;
    }
}

namespace Arpg
{
    int32 ManaCost(Unit const* caster, SpellEntry const* spellInfo, int32 vanilla)
    {
        if (!caster || !spellInfo || spellInfo->powerType != POWER_MANA || !Active(caster))
            return vanilla;
        // A Shrine of Channeling: no cost at all for its while (Arpg/ArpgShrines.h).
        if (caster->GetTypeId() == TYPEID_PLAYER && ShrineFreeCasting(static_cast<Player const*>(caster)))
            return 0;
        uint32 const most = caster->GetMaxPower(POWER_MANA);
        if (!most)
            return vanilla;
        float pct;
        if (CostRow const* row = RowOf(spellInfo))
            pct = row->pct;
        else
        {
            if (vanilla <= 0)
                return vanilla;
            uint32 const base = std::max<uint32>(1, caster->GetCreateMana());
            pct = std::clamp(float(vanilla) * 100.0f / float(base) * MISC_SHARE, 1.0f, MISC_MAX_PCT);
        }
        // A Thrift nightmare's blessing: a quarter off (Arpg/ArpgDungeons.h).
        if (caster->GetMap() && caster->GetMap()->IsDungeon() && NightmareHas(caster->GetMap(), NM_THRIFT))
            pct *= 0.75f;
        return int32(float(most) * pct / 100.0f + 0.5f);
    }

    bool Overdraws(Unit const* caster, uint32 powerType, uint32 cost)
    {
        if ((powerType != POWER_MANA && powerType != POWER_RAGE) || !caster || !Active(caster))
            return false;
        uint32 const have = caster->GetPower(Powers(powerType));
        return have > 0 && uint64(have) * 2 >= cost;
    }

    float ManaRegen(Player const* player, uint32 diffMs)
    {
        float const secs = float(diffMs) / 1000.0f;
        // The gear's and auras' mana per 5 seconds, on top.
        float const mp5 = float(player->GetTotalAuraModifierByMiscValue(SPELL_AURA_MOD_POWER_REGEN, POWER_MANA));
        // Never below nothing: a draining aura must not wrap the unsigned sum round to full.
        return std::max(0.0f, (float(player->GetMaxPower(POWER_MANA)) * REGEN_PCT / 100.0f + mp5 / 5.0f) * secs);
    }

    void OnResourceHit(Unit* dealer, Unit* victim, uint32 damage, SpellEntry const* spellInfo)
    {
        if (!dealer || !victim || !damage || dealer == victim || dealer->GetTypeId() != TYPEID_PLAYER ||
                !Active(dealer) || dealer->GetPowerType() != POWER_MANA)
            return;
        uint32 pct = 0;
        if (spellInfo)
            pct = IsFiller(spellInfo) ? FILLER_PCT : 0;
        else if (dealer->getClass() == CLASS_PALADIN && Sealed(dealer))
            pct = SEALED_SWING_PCT;
        if (!pct)
            return;
        // A filler's refund once a cast: an extra projectile or a pierce striking several pays once.
        if (spellInfo)
        {
            uint32 const now = WorldTimer::getMSTime();
            std::lock_guard<std::mutex> guard(sLock);
            uint32& last = sLastRefund[dealer->GetObjectGuid()];
            if (last && WorldTimer::getMSTimeDiff(last, now) < FILLER_REFUND_GAP_MS)
                return;
            last = now ? now : 1;
        }
        dealer->ModifyPower(POWER_MANA, int32(dealer->GetMaxPower(POWER_MANA) * pct / 100));
    }

    void SendCosts(Player* player, bool ifChanged)
    {
        if (!player || !Active(player) || !player->GetSession())
            return;
        uint64 const sig = Signature(player);
        {
            std::lock_guard<std::mutex> guard(sLock);
            uint64& told = sTold[player->GetObjectGuid()];
            if (ifChanged && told == sig)
                return;
            told = sig;
        }
        std::vector<std::pair<uint32, uint32>> costs;
        for (auto const& [id, spell] : player->GetSpellMap())
        {
            if (spell.state == PLAYERSPELL_REMOVED || spell.disabled)
                continue;
            SpellEntry const* info = sSpellTemplate.LookupEntry<SpellEntry>(id);
            if (!info || info->powerType != POWER_MANA || (!info->manaCost && !info->ManaCostPercentage))
                continue;
            // Every cost but the spell modifiers, which the client applies itself.
            costs.emplace_back(id, Spell::CalculatePowerCost(info, player));
        }
        if (costs.size() > 0xFFFF)
            costs.resize(0xFFFF);
        WorldPacket data(SMSG_ARPG_COSTS, 2 + costs.size() * 8);
        data << uint16(costs.size());
        for (auto const& [id, cost] : costs)
            data << id << cost;
        player->GetSession()->SendPacket(data);
    }

    void ForgetCosts(Player* player)
    {
        if (!player)
            return;
        std::lock_guard<std::mutex> guard(sLock);
        sTold.erase(player->GetObjectGuid());
        sLastRefund.erase(player->GetObjectGuid());
    }
}
