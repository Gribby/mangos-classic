/*
 * ARPG uniques: see ArpgUniques.h.
 */

#include "Arpg/ArpgUniques.h"
#include "Arpg/ArpgCombat.h"

#include "Entities/Item.h"
#include "Entities/Player.h"
#include "Server/Opcodes.h"
#include "Server/WorldPacket.h"
#include "Server/WorldSession.h"
#include "Spells/Spell.h"
#include "Spells/SpellMgr.h"

#include <algorithm>
#include <cmath>

namespace
{
    // Fragments fly on into a cone this wide (half-angle, radians) behind the target.
    constexpr float FRAGMENT_HALF_CONE = float(M_PI) / 4.0f;
}

namespace Arpg
{
    std::vector<UniqueMechanic> const& Uniques()
    {
        // item, kit, spell (rank 1), n, value, pct, tooltip line
        static std::vector<UniqueMechanic> const table =
        {
            // Emberstone Staff, Captain Greenskin (Deadmines): Fireball.
            { 5201,  KIT_EXTRA_PROJECTILES, 133,  1, 14.0f, 60, "Fireball launches 1 extra fireball. Each deals 60% damage." },
            // Quillshooter, Razorfen Downs: Arcane Shot.
            { 10567, KIT_EXTRA_PROJECTILES, 3044, 2, 15.0f, 50, "Arcane Shot fires 3 quills in a 30 degree fan. Each extra deals 50% damage." },
            // Rod of the Sleepwalker, Twilight Lord Kelris (Blackfathom Deeps): Wrath.
            { 1155,  KIT_FRAGMENTS,         5176, 3, 8.0f,  30, "Wrath bursts into 3 motes on hit, each dealing 30% to the enemies behind." },
            // Staff of Jordan, world drop: Frostbolt.
            { 873,   KIT_FRAGMENTS,         116,  4, 8.0f,  30, "Frostbolt shatters on hit into 4 ice shards, each dealing 30% to the enemies behind." },
            // Staff of Dominance, Golemagg (Molten Core): Fireball.
            { 18842, KIT_FRAGMENTS,         133,  5, 10.0f, 25, "Fireball bursts into 5 fragments on hit, each dealing 25% to the enemies behind." },
        };
        return table;
    }

    UniqueMechanic const* WornMechanic(Player const* player, UniqueKit kit, SpellEntry const* spellInfo)
    {
        if (!player || !spellInfo)
            return nullptr;
        uint32 const first = sSpellMgr.GetFirstSpellInChain(spellInfo->Id);
        UniqueMechanic const* best = nullptr;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item const* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item)
                continue;
            for (UniqueMechanic const& row : Uniques())
                if (row.item == item->GetEntry() && row.kit == kit && row.spell == first &&
                        (!best || row.n > best->n))
                    best = &row;
        }
        return best;
    }

    void SendUniques(Player* player)
    {
        std::vector<UniqueMechanic> const& table = Uniques();
        size_t const n = std::min<size_t>(table.size(), 255);
        WorldPacket data(SMSG_ARPG_ITEM_MECHANICS, 1 + n * 64);
        data << uint8(n);
        for (size_t i = 0; i < n; ++i)
        {
            data << uint32(table[i].item);
            data << table[i].text;
        }
        player->GetSession()->SendPacket(data);
    }

    void OnLineLaunch(Spell* spell, Unit* struck)
    {
        if (!spell || spell->IsArpgSecondary())
            return;
        Unit* caster = spell->GetCaster();
        if (!Active(caster))
            return;
        Player* player = static_cast<Player*>(caster);
        SpellEntry const* spellInfo = spell->m_spellInfo;
        UniqueMechanic const* row = WornMechanic(player, KIT_EXTRA_PROJECTILES, spellInfo);
        if (!row)
            return;

        // Alternate sides of the aim, stepping out: +1, -1, +2, -2 lines.
        float const step = row->value * float(M_PI) / 180.0f;
        ObjectGuid const exclude = struck ? struck->GetObjectGuid() : ObjectGuid();
        for (uint8 i = 1; i <= row->n; ++i)
        {
            float const side = (i % 2) ? 1.0f : -1.0f;
            float const bearing = spell->GetArpgAim() + side * float((i + 1) / 2) * step;
            Spell* extra = new Spell(player, spellInfo, TRIGGERED_OLD_TRIGGERED);
            extra->SetArpgSecondary(row->pct, exclude);
            extra->SetArpgAim(bearing, spell->GetArpgAimDist());
            SpellCastTargets targets;
            extra->SpellStart(&targets);
        }
    }

    void OnSpellDamage(Spell* spell, Unit* caster, Unit* victim, uint32 dealt)
    {
        if (!spell || spell->IsArpgSecondary() || !victim || !dealt || !Active(caster))
            return;
        Player* player = static_cast<Player*>(caster);
        SpellEntry const* spellInfo = spell->m_spellInfo;
        UniqueMechanic const* row = WornMechanic(player, KIT_FRAGMENTS, spellInfo);
        if (!row)
            return;

        // Onward from the caster through the target: the fragments fly on behind it.
        float const dir = caster->GetAngle(victim);
        float const dirX = std::cos(dir);
        float const dirY = std::sin(dir);
        std::vector<std::pair<float, Unit*>> struckBy;
        for (Unit* unit : EnemiesNear(victim, row->value + 5.0f))
        {
            if (unit == victim || !unit->IsAlive() || !MayCatchUnit(caster, unit, nullptr))
                continue;
            float const offX = unit->GetPositionX() - victim->GetPositionX();
            float const offY = unit->GetPositionY() - victim->GetPositionY();
            float const dist = std::sqrt(offX * offX + offY * offY);
            if (dist - unit->GetCombatReach() > row->value)
                continue;
            float const along = offX * dirX + offY * dirY;
            float const across = std::fabs(offX * dirY - offY * dirX);
            if (dist > unit->GetCombatReach() && std::atan2(across, along) > FRAGMENT_HALF_CONE)
                continue;
            if (!victim->IsWithinLOSInMap(unit))
                continue;
            struckBy.emplace_back(dist, unit);
        }
        std::sort(struckBy.begin(), struckBy.end(),
                  [](auto const& a, auto const& b) { return a.first < b.first; });
        if (struckBy.size() > row->n)
            struckBy.resize(row->n);

        uint32 const share = std::max<uint32>(1, dealt * row->pct / 100);
        for (auto const& [dist, unit] : struckBy)
        {
            SpellNonMeleeDamage damage(caster, unit, spellInfo->Id, GetFirstSchoolInMask(spell->GetSchoolMask()));
            damage.damage = share;
            unit->CalculateAbsorbResistBlock(caster, &damage, spellInfo);
            Unit::DealDamageMods(caster, unit, damage.damage, &damage.absorb, SPELL_DIRECT_DAMAGE, spellInfo);
            Unit::SendSpellNonMeleeDamageLog(&damage);
            Unit::DealSpellDamage(caster, &damage, true, false);
        }
    }
}
