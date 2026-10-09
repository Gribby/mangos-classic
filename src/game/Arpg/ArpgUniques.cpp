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
#include "Server/DBCStores.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
    // Fragments fly on into a cone this wide (half-angle, radians) behind the target.
    constexpr float FRAGMENT_HALF_CONE = float(M_PI) / 4.0f;
    // A melee ability's arc (kit G): the swing's own 120 degrees.
    constexpr float ARC_WIDTH = 2.0f * float(M_PI) / 3.0f;
    // A burst (kit E) takes at most this many enemies.
    constexpr size_t BURST_MAX = 10;

    // Whether a unique row's spell (rank 1 id) covers `spellInfo`: any rank of it, or a spell whose
    // name is the row spell's name followed by nothing or a space, which takes in the spells a
    // spell triggers under its own name (each Arcane Missiles missile, each Judgement).
    bool SpellMatches(uint32 rowSpell, SpellEntry const* spellInfo)
    {
        if (sSpellMgr.GetFirstSpellInChain(spellInfo->Id) == rowSpell)
            return true;
        SpellEntry const* row = sSpellTemplate.LookupEntry<SpellEntry>(rowSpell);
        char const* want = row ? row->SpellName[0] : nullptr;
        char const* have = spellInfo->SpellName[0];
        if (!want || !have || !*want)
            return false;
        size_t const n = std::strlen(want);
        return std::strncmp(have, want, n) == 0 && (have[n] == '\0' || have[n] == ' ');
    }

    // How long `spellInfo`'s missile flies from `from` to `to`, in ms (0 for an instant spell).
    uint32 FlightMs(WorldObject const* from, WorldObject const* to, SpellEntry const* spellInfo)
    {
        if (spellInfo->speed <= 0.0f)
            return 0;
        return uint32(from->GetDistance(to) / spellInfo->speed * IN_MILLISECONDS);
    }

    // A cosmetic SMSG_SPELL_GO of `spellInfo` from `from` at `to`: the client draws the spell's
    // missile between them. Nothing is cast; the hit is dealt separately when it lands.
    void SendRelayVisual(Unit* from, Unit* to, SpellEntry const* spellInfo)
    {
        WorldPacket data(SMSG_SPELL_GO, 40);
        data << from->GetPackGUID();
        data << from->GetPackGUID();
        data << uint32(spellInfo->Id);
        data << uint16(CAST_FLAG_UNKNOWN9 | CAST_FLAG_HIDDEN_COMBATLOG);
        data << uint8(1);                                   // hit
        data << to->GetObjectGuid();
        data << uint8(0);                                   // missed
        SpellCastTargets targets;
        targets.setUnitTarget(to);
        data << targets;
        from->SendMessageToSet(data, true);
    }

    // Deal `share` of `spellInfo`'s school to `unit` as `caster`'s, through the ordinary spell
    // damage path: armour for a physical hit, resistances, absorbs, the combat log.
    void DealShare(Unit* caster, Unit* unit, SpellEntry const* spellInfo, SpellSchoolMask mask, uint32 share)
    {
        if (!unit->IsAlive())
            return;
        SpellNonMeleeDamage damage(caster, unit, spellInfo->Id, GetFirstSchoolInMask(mask));
        damage.damage = share;
        if (mask & SPELL_SCHOOL_MASK_NORMAL)
            damage.damage = Unit::CalcArmorReducedDamage(caster, unit, damage.damage);
        unit->CalculateAbsorbResistBlock(caster, &damage, spellInfo);
        Unit::DealDamageMods(caster, unit, damage.damage, &damage.absorb, SPELL_DIRECT_DAMAGE, spellInfo);
        Unit::SendSpellNonMeleeDamageLog(&damage);
        Unit::DealSpellDamage(caster, &damage, true, false);
    }

    // The fair enemy nearest `from` within `range` yards that is not in `hit`, in sight of it.
    Unit* NextInChain(Unit* caster, Unit* from, float range, GuidVector const& hit)
    {
        Unit* best = nullptr;
        float bestDist = range;
        for (Unit* unit : Arpg::EnemiesNear(from, range + 5.0f))
        {
            if (!unit->IsAlive() || std::find(hit.begin(), hit.end(), unit->GetObjectGuid()) != hit.end())
                continue;
            if (!Arpg::MayCatchUnit(caster, unit, nullptr))
                continue;
            float const dist = from->GetDistance(unit);
            if (dist <= bestDist && from->IsWithinLOSInMap(unit))
            {
                best = unit;
                bestDist = dist;
            }
        }
        return best;
    }

    // A relayed hit lands on `to` after its missile's flight: `share` of `spellInfo`'s school. With
    // `jumps` left (kit C) it then leaps on to the nearest enemy not yet hit, within `range`.
    void RelayHit(Player* player, ObjectGuid fromGuid, ObjectGuid toGuid, SpellEntry const* spellInfo,
                  SpellSchoolMask mask, uint32 share, uint8 jumps, float range, GuidVector hit)
    {
        Unit* from = player->GetMap()->GetUnit(fromGuid);
        Unit* to = player->GetMap()->GetUnit(toGuid);
        if (!from || !to)
            return;
        SendRelayVisual(from, to, spellInfo);
        uint32 const delay = FlightMs(from, to, spellInfo);
        auto land = [toGuid, spellInfo, mask, share, jumps, range, hit](Unit& owner)
        {
            Player* caster = static_cast<Player*>(&owner);
            if (!caster->IsInWorld())
                return;
            Unit* target = caster->GetMap()->GetUnit(toGuid);
            if (!target || !target->IsAlive())
                return;
            DealShare(caster, target, spellInfo, mask, share);
            if (!jumps)
                return;
            GuidVector next = hit;
            Unit* onward = NextInChain(caster, target, range, next);
            if (!onward)
                return;
            next.push_back(onward->GetObjectGuid());
            RelayHit(caster, toGuid, onward->GetObjectGuid(), spellInfo, mask, share, jumps - 1, range, next);
        };
        if (!delay)
            land(*player);
        else
            player->m_events.AddEvent(new UnitLambdaEvent(*player, land), player->m_events.CalculateTime(delay));
    }
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
            // Freezing Shard, Razorfen Downs: the wand's Shoot.
            { 10572, KIT_CHAIN,             5019, 2, 8.0f,  50, "Your wand bolts chain to 2 more enemies within 8 yards for 50% damage." },
            // Illusionary Rod, Arcanist Doan (Scarlet Monastery): each Arcane Missiles missile.
            { 7713,  KIT_CHAIN,             5143, 1, 8.0f,  50, "Each Arcane Missiles missile jumps to a second enemy for 50% damage." },
            // Whitemane's Chapeau, High Inquisitor Whitemane (Scarlet Monastery): Smite.
            { 7720,  KIT_CHAIN,             585,  2, 10.0f, 60, "Smite chains to 2 more enemies within 10 yards for 60% damage." },
            // Ramstein's Lightning Bolts, Ramstein the Gorger (Stratholme): Lightning Bolt.
            { 13515, KIT_CHAIN,             403,  2, 10.0f, 50, "Lightning Bolt chains to 2 more enemies within 10 yards for 50% damage." },
            // Cookie's Stirring Rod, Cookie (Deadmines): the wand's Shoot.
            { 5198,  KIT_BURST,             5019, 0, 4.0f,  40, "Your wand bolts splash enemies within 4 yards of the target for 40% damage." },
            // Mograine's Might, Scarlet Commander Mograine (Scarlet Monastery): Judgement.
            { 7723,  KIT_BURST,             20271, 0, 8.0f, 50, "Judgement also strikes every enemy within 8 yards of the target for 50% damage." },
            // Night Reaver, Shadowfang Keep: Heroic Strike.
            { 1318,  KIT_ARC,               78,   0, 0.0f,  50, "Heroic Strike hits every enemy in front of you. The extra enemies take 50% damage." },
            // Felstriker, Warchief Rend Blackhand (Upper Blackrock Spire): Eviscerate.
            { 12590, KIT_ARC,               2098, 0, 0.0f,  60, "Eviscerate strikes every enemy in front of you for 60% damage." },
        };
        return table;
    }

    UniqueMechanic const* WornMechanic(Player const* player, UniqueKit kit, SpellEntry const* spellInfo)
    {
        if (!player || !spellInfo)
            return nullptr;
        UniqueMechanic const* best = nullptr;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item const* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item)
                continue;
            for (UniqueMechanic const& row : Uniques())
                if (row.item == item->GetEntry() && row.kit == kit && SpellMatches(row.spell, spellInfo) &&
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
        SpellSchoolMask const mask = spell->GetSchoolMask();

        // Fragments (D): onward from the caster through the target, into a cone behind it.
        if (UniqueMechanic const* row = WornMechanic(player, KIT_FRAGMENTS, spellInfo))
        {
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
                RelayHit(player, victim->GetObjectGuid(), unit->GetObjectGuid(), spellInfo, mask, share, 0, 0.0f,
                         GuidVector{ victim->GetObjectGuid(), unit->GetObjectGuid() });
        }

        // Chain (C): leaps from the target to the nearest enemy not yet hit, `n` times.
        if (UniqueMechanic const* row = WornMechanic(player, KIT_CHAIN, spellInfo))
        {
            GuidVector hit{ victim->GetObjectGuid() };
            if (Unit* first = NextInChain(caster, victim, row->value, hit))
            {
                hit.push_back(first->GetObjectGuid());
                uint32 const share = std::max<uint32>(1, dealt * row->pct / 100);
                RelayHit(player, victim->GetObjectGuid(), first->GetObjectGuid(), spellInfo, mask, share,
                         uint8(row->n - 1), row->value, hit);
            }
        }

        // Burst (E): everything within `value` yards of the target, at once.
        if (UniqueMechanic const* row = WornMechanic(player, KIT_BURST, spellInfo))
        {
            uint32 const share = std::max<uint32>(1, dealt * row->pct / 100);
            size_t done = 0;
            for (Unit* unit : EnemiesNear(victim, row->value + 5.0f))
            {
                if (done >= BURST_MAX)
                    break;
                if (unit == victim || !unit->IsAlive() || !MayCatchUnit(caster, unit, nullptr))
                    continue;
                if (victim->GetDistance(unit) > row->value || !victim->IsWithinLOSInMap(unit))
                    continue;
                DealShare(caster, unit, spellInfo, mask, share);
                ++done;
            }
        }

        // Arc (G): a melee ability strikes every other enemy in the swing's arc and reach too.
        if (UniqueMechanic const* row = WornMechanic(player, KIT_ARC, spellInfo))
        {
            uint32 const share = std::max<uint32>(1, dealt * row->pct / 100);
            for (Unit* unit : EnemiesNear(caster, 10.0f))
            {
                if (unit == victim || !unit->IsAlive() || !MayCatchUnit(caster, unit, nullptr))
                    continue;
                if (!caster->CanReachWithMeleeAttack(unit) || !caster->HasInArc(unit, ARC_WIDTH))
                    continue;
                DealShare(caster, unit, spellInfo, mask, share);
            }
        }
    }
}
