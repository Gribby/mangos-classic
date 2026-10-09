/*
 * ARPG uniques: see ArpgUniques.h.
 */

#include "Arpg/ArpgUniques.h"
#include "Arpg/ArpgCombat.h"
#include "Arpg/ArpgTree.h"

#include "Entities/Item.h"
#include "Entities/Player.h"
#include "Globals/ObjectMgr.h"
#include "Log/Log.h"
#include "Server/Opcodes.h"
#include "Server/WorldPacket.h"
#include "Server/WorldSession.h"
#include "Spells/Spell.h"
#include "Spells/SpellMgr.h"
#include "Spells/SpellAuras.h"
#include "Util/Timer.h"
#include "Server/DBCStores.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace
{
    // Fragments fly on into a cone this wide (half-angle, radians) behind the target.
    constexpr float FRAGMENT_HALF_CONE = float(M_PI) / 4.0f;
    // A melee ability's arc (kit G): the swing's own 120 degrees.
    constexpr float ARC_WIDTH = 2.0f * float(M_PI) / 3.0f;
    // A burst (kit E), a pierce through all (B), a spread on death (F): at most this many enemies.
    constexpr size_t BURST_MAX = 10;
    // An echo (kit I) repeats this long after the cast it echoes, in ms.
    constexpr uint32 ECHO_DELAY = 500;
    // A step (kit K) moves the player this long after the kill, in ms, out of the spell's hit.
    constexpr uint32 STEP_DELAY = 100;

    // Dawnbringer's bolt reaches this far, in yards, and flies as Holy Shock's damage spell.
    constexpr float DAWNBRINGER_RANGE = 20.0f;
    constexpr uint32 HOLY_SHOCK_DAMAGE = 25912;
    // Martyr's Ward pulses this often, in ms, this far round the player, in yards.
    constexpr uint32 MARTYRS_WARD_TICK = 1000;
    constexpr float MARTYRS_WARD_RANGE = 10.0f;
    // Each player's last Martyr's Ward pulse, by guid.
    std::unordered_map<ObjectGuid, uint32> sWardTicks;

    // Each player's count of echoing casts (kit I), by guid.
    std::unordered_map<ObjectGuid, uint32> sEchoCounts;

    // The fair enemies along the line from `caster` at `bearing` between `fromDist` and `toDist`
    // yards (the skillshot's width plus each one's reach), nearest first, not `exclude`.
    std::vector<Unit*> UnitsAlongLine(Unit* caster, float bearing, float fromDist, float toDist, Unit const* exclude)
    {
        float const dirX = std::cos(bearing);
        float const dirY = std::sin(bearing);
        std::vector<std::pair<float, Unit*>> found;
        for (Unit* unit : Arpg::EnemiesNear(caster, toDist + 10.0f))
        {
            if (unit == exclude || !unit->IsAlive() || !Arpg::MayCatchUnit(caster, unit, nullptr))
                continue;
            float const reach = unit->GetCombatReach();
            float const offX = unit->GetPositionX() - caster->GetPositionX();
            float const offY = unit->GetPositionY() - caster->GetPositionY();
            float const along = offX * dirX + offY * dirY;
            float const across = std::fabs(offX * dirY - offY * dirX);
            if (along + reach <= fromDist || along - reach > toDist || across > Arpg::LINE_HALF_WIDTH + reach)
                continue;
            if (!caster->IsWithinLOSInMap(unit))
                continue;
            found.emplace_back(along, unit);
        }
        std::sort(found.begin(), found.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
        std::vector<Unit*> units;
        for (auto const& [along, unit] : found)
            units.push_back(unit);
        return units;
    }

    // The `count` fair enemies nearest `center` within `radius` yards, in its sight, not `center`.
    std::vector<Unit*> NearestTo(Unit* caster, Unit* center, float radius, size_t count)
    {
        std::vector<std::pair<float, Unit*>> found;
        for (Unit* unit : Arpg::EnemiesNear(center, radius + 5.0f))
        {
            if (unit == center || !unit->IsAlive() || !Arpg::MayCatchUnit(caster, unit, nullptr))
                continue;
            float const dist = center->GetDistance(unit);
            if (dist <= radius && center->IsWithinLOSInMap(unit))
                found.emplace_back(dist, unit);
        }
        std::sort(found.begin(), found.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
        if (found.size() > count)
            found.resize(count);
        std::vector<Unit*> units;
        for (auto const& [dist, unit] : found)
            units.push_back(unit);
        return units;
    }

    // A free copy of `spellInfo` from `player` at `target`, setting off no mechanic of its own.
    void CastCopyAt(Player* player, SpellEntry const* spellInfo, Unit* target)
    {
        Spell* copy = new Spell(player, spellInfo, TRIGGERED_OLD_TRIGGERED);
        copy->SetArpgSecondary(100, ObjectGuid());
        SpellCastTargets targets;
        targets.setUnitTarget(target);
        copy->SpellStart(&targets);
    }

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
            // Witching Stave, Shadowfang Keep: Shadow Bolt.
            { 1484,  KIT_PIERCE,            686,  1, 0.0f,  70, "Shadow Bolt pierces 1 enemy, dealing 70% damage to it." },
            // Bow of Searing Arrows, world drop: Auto Shot.
            { 2825,  KIT_PIERCE,            75,   0, 0.0f,  60, "Your Auto Shot arrows pierce every enemy in their path for 60% damage." },
            // Hammer of the Grand Crusader, Balnazzar (Stratholme): Hammer of Wrath.
            { 18717, KIT_PIERCE,            24275, 0, 0.0f, 70, "Hammer of Wrath pierces every enemy in its path for 70% damage." },
            // Venomstrike, Lord Serpentis (Wailing Caverns): Serpent Sting.
            { 6469,  KIT_SPREAD,            1978, 2, 8.0f,  100, "Serpent Sting spreads to 2 enemies within 8 yards when it lands." },
            // Living Root, Verdan the Everliving (Wailing Caverns): Entangling Roots.
            { 6631,  KIT_SPREAD,            339,  2, 6.0f,  100, "Entangling Roots also roots 2 more enemies within 6 yards of the target." },
            // Hypnotic Blade, Arcanist Doan (Scarlet Monastery): Polymorph.
            { 7714,  KIT_SPREAD,            118,  1, 8.0f,  100, "Polymorph also turns 1 more enemy within 8 yards into a sheep." },
            // Lok'amir il Romathis, Nefarian (Blackwing Lair): Shadow Word: Pain.
            { 19360, KIT_SPREAD,            589,  0, 8.0f,  100, "Shadow Word: Pain spreads to every enemy within 8 yards when its target dies." },
            // Meteor Shard, Archmage Arugal (Shadowfang Keep): Sinister Strike.
            { 6220,  KIT_SHOCKWAVE,         1752, 0, 12.0f, 50, "Sinister Strike sends a burning shard 12 yards ahead for 50% damage." },
            // Azuresong Mageblade, Golemagg (Molten Core): Frostbolt.
            { 17103, KIT_ECHO,              116,  3, 0.0f,  60, "Every 3rd Frostbolt is echoed for free at 60% damage." },
            // Book of the Dead, Balnazzar (Stratholme): any kill raises a Skeleton (6412) for 20 sec.
            { 13353, KIT_RAISE,             6412, 0, 20.0f, 20, "Enemies you kill have a 20% chance to rise as a skeleton that fights for you for 20 sec." },
            // Paladin. Kresh's Back, Kresh (Wailing Caverns): Hammer of Justice.
            { 13245, KIT_SPREAD,            853,  2, 8.0f,  100, "Hammer of Justice also stuns 2 more enemies within 8 yards." },
            // Smite's Mighty Hammer, Mr. Smite (Deadmines): Seal of Righteousness's holy strikes.
            { 7230,  KIT_ARC,               20154, 0, 0.0f, 50, "Seal of Righteousness strikes every enemy in front of you. The extra enemies take 50% damage." },
            // Taskmaster Axe, Sneed (Deadmines): Judgement.
            { 5194,  KIT_CHAIN,             20271, 2, 10.0f, 60, "Judgement chains to 2 more enemies within 10 yards for 60% damage." },
            // Hand of Righteousness, High Inquisitor Whitemane (Scarlet Monastery): Seal of Righteousness.
            { 7721,  KIT_BURST,             20154, 0, 5.0f, 35, "Seal of Righteousness strikes burst onto enemies within 5 yards for 35% damage." },
            // Hand of Edward the Odd, world drop: Holy Shock.
            { 2243,  KIT_CHAIN,             20473, 2, 10.0f, 60, "Holy Shock chains to 2 more enemies within 10 yards for 60% damage." },
            // Spinal Reaper, Ragnaros (Molten Core): Seal of Command.
            { 17104, KIT_ARC,               20375, 0, 0.0f, 60, "Seal of Command strikes every enemy in front of you for 60% damage." },
            // Perdition's Blade, Ragnaros (Molten Core): Sinister Strike.
            { 18816, KIT_STEP,              1752, 0, 10.0f, 0,  "A kill with Sinister Strike steps you behind the nearest enemy within 10 yards." },
        };
        // Checked once against the loaded data, so a wrong id shows in the log, not in play.
        static bool const checked = []()
        {
            for (UniqueMechanic const& row : table)
            {
                if (!ObjectMgr::GetItemPrototype(row.item))
                    sLog.outError("ARPG uniques: item %u is not in item_template", row.item);
                if (row.kit == KIT_RAISE)
                    continue;
                SpellEntry const* spell = sSpellTemplate.LookupEntry<SpellEntry>(row.spell);
                if (!spell)
                    sLog.outError("ARPG uniques: item %u names spell %u, which does not exist", row.item, row.spell);
                else
                    sLog.outDetail("ARPG uniques: item %u, kit %c, spell %u (%s)", row.item, char(row.kit), row.spell, spell->SpellName[0]);
            }
            return true;
        }();
        (void)checked;
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
        // The skill tree's modifier nodes, by the same rule: the larger count applies.
        for (UniqueMechanic const* row : LearnedModifiers(player))
            if (row->kit == kit && SpellMatches(row->spell, spellInfo) && (!best || row->n > best->n))
                best = row;
        return best;
    }

    // The `kit` row worn by `player` whatever the spell (kit J), if any.
    UniqueMechanic const* WornAny(Player const* player, UniqueKit kit)
    {
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (Item const* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                for (UniqueMechanic const& row : Uniques())
                    if (row.item == item->GetEntry() && row.kit == kit)
                        return &row;
        return nullptr;
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
        float const aim = spell->GetArpgAim();
        float const aimDist = spell->GetArpgAimDist();

        // Extra projectiles (A), alternating sides of the aim, stepping out: +1, -1, +2, -2 lines.
        if (UniqueMechanic const* row = WornMechanic(player, KIT_EXTRA_PROJECTILES, spellInfo))
        {
            float const step = row->value * float(M_PI) / 180.0f;
            ObjectGuid const exclude = struck ? struck->GetObjectGuid() : ObjectGuid();
            for (uint8 i = 1; i <= row->n; ++i)
            {
                float const side = (i % 2) ? 1.0f : -1.0f;
                float const bearing = aim + side * float((i + 1) / 2) * step;
                Spell* extra = new Spell(player, spellInfo, TRIGGERED_OLD_TRIGGERED);
                extra->SetArpgSecondary(row->pct, exclude);
                extra->SetArpgAim(bearing, aimDist);
                SpellCastTargets targets;
                extra->SpellStart(&targets);
            }
        }

        // Echo (I): every `n`th cast flies again along the same aim a moment later.
        if (UniqueMechanic const* row = WornMechanic(player, KIT_ECHO, spellInfo))
        {
            uint32& count = sEchoCounts[player->GetObjectGuid()];
            if (++count >= std::max<uint8>(row->n, 1))
            {
                count = 0;
                uint32 const pct = row->pct;
                player->m_events.AddEvent(new UnitLambdaEvent(*player, [spellInfo, aim, aimDist, pct](Unit& owner)
                {
                    Player* caster = static_cast<Player*>(&owner);
                    if (!caster->IsInWorld() || !caster->IsAlive())
                        return;
                    Spell* echo = new Spell(caster, spellInfo, TRIGGERED_OLD_TRIGGERED);
                    echo->SetArpgSecondary(pct, ObjectGuid());
                    echo->SetArpgAim(aim, aimDist);
                    SpellCastTargets targets;
                    echo->SpellStart(&targets);
                }), player->m_events.CalculateTime(ECHO_DELAY));
            }
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

        // Pierce (B): on along the line past the target, out to the spell's range.
        if (UniqueMechanic const* row = WornMechanic(player, KIT_PIERCE, spellInfo))
        {
            float const bearing = spell->IsArpgLine() ? spell->GetArpgAim() : caster->GetAngle(victim);
            float maxRange = GetSpellMaxRange(sSpellRangeStore.LookupEntry(spellInfo->rangeIndex));
            player->ApplySpellMod(spellInfo->Id, SPELLMOD_RANGE, maxRange, false);
            float const offX = victim->GetPositionX() - caster->GetPositionX();
            float const offY = victim->GetPositionY() - caster->GetPositionY();
            float const past = offX * std::cos(bearing) + offY * std::sin(bearing);
            std::vector<Unit*> onward = UnitsAlongLine(caster, bearing, past, maxRange, victim);
            size_t const cap = row->n ? row->n : BURST_MAX;
            if (onward.size() > cap)
                onward.resize(cap);
            uint32 const share = std::max<uint32>(1, dealt * row->pct / 100);
            ObjectGuid from = victim->GetObjectGuid();
            for (Unit* unit : onward)
            {
                RelayHit(player, from, unit->GetObjectGuid(), spellInfo, mask, share, 0, 0.0f, GuidVector{});
                from = unit->GetObjectGuid();
            }
        }

        // Shockwave (H): along the caster's facing, past the target too.
        if (UniqueMechanic const* row = WornMechanic(player, KIT_SHOCKWAVE, spellInfo))
        {
            uint32 const share = std::max<uint32>(1, dealt * row->pct / 100);
            for (Unit* unit : UnitsAlongLine(caster, caster->GetOrientation(), 0.0f, row->value, victim))
                DealShare(caster, unit, spellInfo, mask, share);
        }

        // Step (K): a kill moves the player behind the nearest enemy, just after the hit.
        if (!victim->IsAlive())
        {
            if (UniqueMechanic const* row = WornMechanic(player, KIT_STEP, spellInfo))
            {
                std::vector<Unit*> next = NearestTo(caster, caster, row->value, 1);
                if (!next.empty())
                {
                    ObjectGuid const nextGuid = next.front()->GetObjectGuid();
                    player->m_events.AddEvent(new UnitLambdaEvent(*player, [nextGuid](Unit& owner)
                    {
                        Unit* enemy = owner.IsInWorld() ? owner.GetMap()->GetUnit(nextGuid) : nullptr;
                        if (!enemy || !enemy->IsAlive() || !owner.IsAlive() || owner.IsRooted())
                            return;
                        float const o = enemy->GetOrientation();
                        float const back = enemy->GetCombatReach() + 1.0f;
                        float x = enemy->GetPositionX() - std::cos(o) * back;
                        float y = enemy->GetPositionY() - std::sin(o) * back;
                        float z = enemy->GetPositionZ();
                        owner.UpdateAllowedPositionZ(x, y, z);
                        if (!owner.IsWithinLOS(x, y, z + 1.0f))
                            return;
                        owner.NearTeleportTo(x, y, z, o);
                    }), player->m_events.CalculateTime(STEP_DELAY));
                }
            }
        }
    }

    void OnSpellLanded(Spell* spell, Unit* caster, Unit* target)
    {
        if (!spell || spell->IsArpgSecondary() || !target || target == caster || !Active(caster))
            return;
        Player* player = static_cast<Player*>(caster);
        SpellEntry const* spellInfo = spell->m_spellInfo;
        // Spread on landing (F, `n` > 0).
        UniqueMechanic const* row = WornMechanic(player, KIT_SPREAD, spellInfo);
        if (!row || !row->n || !caster->CanAttack(target))
            return;
        for (Unit* unit : NearestTo(caster, target, row->value, row->n))
            CastCopyAt(player, spellInfo, unit);
    }

    void OnKill(Player* killer, Unit* victim)
    {
        if (!victim)
            return;

        // Spread on death (F, `n` 0): each ARPG player's aura on the corpse that a worn unique
        // spreads, copied onto every enemy around it.
        std::vector<std::pair<ObjectGuid, SpellEntry const*>> spreads;
        for (auto const& [id, holder] : victim->GetSpellAuraHolderMap())
        {
            Unit* auraCaster = holder->GetCaster();
            if (!Active(auraCaster) || !auraCaster->IsInMap(victim))
                continue;
            UniqueMechanic const* row = WornMechanic(static_cast<Player*>(auraCaster), KIT_SPREAD, holder->GetSpellProto());
            if (row && !row->n)
                spreads.emplace_back(auraCaster->GetObjectGuid(), holder->GetSpellProto());
        }
        for (auto const& [guid, spellInfo] : spreads)
        {
            Player* caster = victim->GetMap()->GetPlayer(guid);
            UniqueMechanic const* row = caster ? WornMechanic(caster, KIT_SPREAD, spellInfo) : nullptr;
            if (!row)
                continue;
            for (Unit* unit : NearestTo(caster, victim, row->value, BURST_MAX))
                CastCopyAt(caster, spellInfo, unit);
        }

        // Raise (J): the fallen foe may rise to fight for the killer a while.
        if (!Active(killer) || victim->GetTypeId() != TYPEID_UNIT)
            return;
        UniqueMechanic const* raise = WornAny(killer, KIT_RAISE);
        if (!raise || !roll_chance_f(raise->value))
            return;
        Creature* risen = killer->SummonCreature(raise->spell, victim->GetPositionX(), victim->GetPositionY(),
                          victim->GetPositionZ(), victim->GetOrientation(), TEMPSPAWN_TIMED_OR_DEAD_DESPAWN,
                          raise->pct * IN_MILLISECONDS);
        if (!risen)
            return;
        risen->SelectLevel(killer->GetLevel());
        risen->setFaction(killer->GetFaction());
        risen->SetOwnerGuid(killer->GetObjectGuid());
        std::vector<Unit*> foes = NearestTo(killer, risen, 20.0f, 1);
        if (!foes.empty() && risen->AI())
            risen->AI()->AttackStart(foes.front());
    }

    // --- The skill tree's keystones (ArpgTree.h) that hook into combat ---

    bool KeepsSealOnJudgement(Unit const* caster)
    {
        return HasKeystone(caster, KEY_AVENGER);
    }

    bool IgnoresCreatureType(WorldObject const* caster, SpellEntry const* spellInfo)
    {
        if (!caster || !spellInfo || !spellInfo->SpellName[0] || !caster->IsPlayer())
            return false;
        char const* name = spellInfo->SpellName[0];
        bool const purifiable = std::strcmp(name, "Exorcism") == 0 || std::strcmp(name, "Holy Wrath") == 0;
        return purifiable && HasKeystone(static_cast<Unit const*>(caster), KEY_PURIFYING_LIGHT);
    }

    void OnHeal(Spell* spell, Unit* caster, Unit* target, uint32 amount)
    {
        if (!spell || spell->IsArpgSecondary() || caster != target || !amount || !HasKeystone(caster, KEY_DAWNBRINGER))
            return;
        Player* player = static_cast<Player*>(caster);
        std::vector<Unit*> foes = NearestTo(caster, caster, DAWNBRINGER_RANGE, 1);
        if (foes.empty())
            return;
        // The bolt flies as Holy Shock's does; with no Holy Shock data, as the heal itself.
        SpellEntry const* bolt = sSpellTemplate.LookupEntry<SpellEntry>(HOLY_SHOCK_DAMAGE);
        if (!bolt)
            bolt = spell->m_spellInfo;
        RelayHit(player, caster->GetObjectGuid(), foes.front()->GetObjectGuid(), bolt, SPELL_SCHOOL_MASK_HOLY,
                 std::max<uint32>(1, amount / 2), 0, 0.0f, GuidVector{});
    }

    void UpdateKeystones(Player* player)
    {
        if (!player->IsInCombat() || !player->IsAlive() || !HasKeystone(player, KEY_MARTYRS_WARD))
            return;
        uint32 const now = WorldTimer::getMSTime();
        uint32& last = sWardTicks[player->GetObjectGuid()];
        if (WorldTimer::getMSTimeDiff(last, now) < MARTYRS_WARD_TICK)
            return;
        last = now;
        // The player's own Retribution Aura: its damage, doubled, to every enemy around.
        for (Aura* aura : player->GetAurasByType(SPELL_AURA_DAMAGE_SHIELD))
        {
            SpellEntry const* proto = aura->GetSpellProto();
            if (aura->GetCasterGuid() != player->GetObjectGuid() || !proto->SpellName[0] ||
                    std::strncmp(proto->SpellName[0], "Retribution Aura", 16) != 0)
                continue;
            uint32 const damage = uint32(std::max(0, aura->GetModifier()->m_amount)) * 2;
            if (!damage)
                return;
            for (Unit* unit : NearestTo(player, player, MARTYRS_WARD_RANGE, BURST_MAX))
                DealShare(player, unit, proto, SPELL_SCHOOL_MASK_HOLY, damage);
            return;
        }
    }
}
