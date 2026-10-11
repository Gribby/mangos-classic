/*
 * ARPG uniques: see ArpgUniques.h.
 */

#include "Arpg/ArpgUniques.h"
#include "Arpg/ArpgSkills.h"
#include "Arpg/ArpgCombat.h"
#include "Arpg/ArpgTree.h"
#include "Arpg/ArpgCharacter.h"

#include "Entities/Item.h"
#include "Entities/Player.h"
#include "Globals/ObjectMgr.h"
#include "Log/Log.h"
#include "Maps/Map.h"
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
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>

namespace
{
    // Fragments fly on into a cone this wide (half-angle, radians) behind the target.
    // A scattering shard lands this share of the shatter's reach out.
    constexpr float SHARD_SCATTER = 0.7f;
    // A melee ability's arc (kit G): the swing's own 120 degrees.
    constexpr float ARC_WIDTH = 2.0f * float(M_PI) / 3.0f;
    // A burst (kit E), a pierce through all (B), a spread on death (F): at most this many enemies.
    constexpr size_t BURST_MAX = 10;
    // An echo (kit I) repeats this long after the cast it echoes, in ms.
    constexpr uint32 ECHO_DELAY = 500;
    // A bounced cast (the Ricochet node) reaches its next enemy this long after the last, in ms.
    constexpr uint32 BOUNCE_DELAY = 200;
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

    // A cosmetic SMSG_SPELL_GO of `spellInfo` from `from` at the ground point (x, y, z): its
    // missile flies there and lands (a shard scattering with no enemy to strike).
    void SendGroundVisual(Unit* from, float x, float y, float z, SpellEntry const* spellInfo)
    {
        WorldPacket data(SMSG_SPELL_GO, 40);
        data << from->GetPackGUID();
        data << from->GetPackGUID();
        data << uint32(spellInfo->Id);
        data << uint16(CAST_FLAG_UNKNOWN9 | CAST_FLAG_HIDDEN_COMBATLOG);
        data << uint8(0);                                   // hit
        data << uint8(0);                                   // missed
        SpellCastTargets targets;
        targets.setDestination(x, y, z);
        data << targets;
        from->SendMessageToSet(data, true);
    }

    // How long `spellInfo`'s missile flies from `from` to `to`, in ms (0 for an instant spell).
    uint32 FlightMs(WorldObject const* from, WorldObject const* to, SpellEntry const* spellInfo)
    {
        if (spellInfo->speed <= 0.0f)
            return 0;
        return uint32(from->GetDistance(to) / spellInfo->speed * float(IN_MILLISECONDS));
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

    // The impact of `spellInfo` on `unit`, for the ARPG clients near it (SMSG_ARPG_IMPACT): an
    // added hit with no missile of its own (an arc, a burst, a cleave) still lands with the
    // spell's own hit visual. The client finds the kit, or its school's stock one.
    constexpr float IMPACT_SEEN = 80.0f;
    void ShowImpact(Unit* unit, SpellEntry const* spellInfo, SpellSchoolMask mask)
    {
        WorldPacket data(SMSG_ARPG_IMPACT, 16);
        data << unit->GetObjectGuid();
        data << uint32(spellInfo->Id);
        data << uint32(mask);
        // Only to ARPG clients: a stock client knows no such opcode.
        for (auto const& ref : unit->GetMap()->GetPlayers())
            if (Player* player = ref.getSource())
                if (Arpg::Active(player) && player->IsWithinDistInMap(unit, IMPACT_SEEN))
                    player->GetSession()->SendPacket(data);
    }

    // Deal `share` of `spellInfo`'s school to `unit` as `caster`'s, through the ordinary spell
    // damage path: armour for a physical hit, resistances, absorbs, the combat log. `show`: the
    // hit's impact plays (a relayed hit's missile already brings its own).
    void DealShare(Unit* caster, Unit* unit, SpellEntry const* spellInfo, SpellSchoolMask mask, uint32 share, bool show = true)
    {
        if (!unit->IsAlive())
            return;
        if (show)
            ShowImpact(unit, spellInfo, mask);
        SpellNonMeleeDamage damage(caster, unit, spellInfo->Id, GetFirstSchoolInMask(mask));
        damage.damage = share;
        if (mask & SPELL_SCHOOL_MASK_NORMAL)
            damage.damage = Unit::CalcArmorReducedDamage(caster, unit, damage.damage);
        unit->CalculateAbsorbResistBlock(caster, &damage, spellInfo);
        Unit::DealDamageMods(caster, unit, damage.damage, &damage.absorb, SPELL_DIRECT_DAMAGE, spellInfo);
        Unit::SendSpellNonMeleeDamageLog(&damage);
        Unit::DealSpellDamage(caster, &damage, true, false);
    }

    // The name a mechanic goes by in the combat log: the skill node's or rune's, else the item's.
    char const* MechanicName(Arpg::UniqueMechanic const* row)
    {
        if (row->name)
            return row->name;
        ItemPrototype const* proto = ObjectMgr::GetItemPrototype(row->item);
        return proto ? proto->Name1 : "A unique";
    }

    char const* Plural(size_t n, char const* one, char const* many)
    {
        return n == 1 ? one : many;
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
            DealShare(caster, target, spellInfo, mask, share, false);
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
            { 1155,  KIT_FRAGMENTS,         5176, 3, 8.0f,  30, "Wrath bursts into 3 motes on hit that strike the nearest enemies within 8 yards for 30% damage each." },
            // Staff of Jordan, world drop: Frostbolt.
            { 873,   KIT_FRAGMENTS,         116,  4, 10.0f, 35, "Frostbolt shatters on hit into 4 ice shards that strike the nearest enemies within 10 yards for 35% damage each." },
            // Staff of Dominance, Golemagg (Molten Core): Fireball.
            { 18842, KIT_FRAGMENTS,         133,  5, 10.0f, 25, "Fireball bursts into 5 fragments on hit that strike the nearest enemies within 10 yards for 25% damage each." },
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

    // Several sources of one kit on one spell (a unique, a skill node, a rune) stack into one row:
    // counts add, shares add (to at most a whole hit; three quarters for what multiplies hits),
    // reach takes the widest, an echo comes as often as its most frequent source. Combined rows
    // live as long as the server.
    UniqueMechanic const* Stacked(std::vector<UniqueMechanic const*> rows)
    {
        if (rows.empty())
            return nullptr;
        std::sort(rows.begin(), rows.end());
        rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
        if (rows.size() == 1)
            return rows.front();
        UniqueKit const kit = rows.front()->kit;
        // One of a kind: a step or a raise does not grow.
        if (kit == KIT_STEP || kit == KIT_RAISE)
            return rows.front();
        static std::mutex lock;
        static std::map<std::vector<UniqueMechanic const*>, std::pair<UniqueMechanic, std::string>> cache;
        std::lock_guard<std::mutex> guard(lock);
        auto it = cache.find(rows);
        if (it != cache.end())
            return &it->second.first;
        UniqueMechanic out = *rows.front();
        std::string name;
        uint32 n = 0, pct = 0, echoEvery = 0, bestN = 0, bestPct = 0;
        bool all = false;
        for (UniqueMechanic const* row : rows)
        {
            name += (name.empty() ? "" : " + ") + std::string(MechanicName(row));
            out.value = std::max(out.value, row->value);
            pct += row->pct;
            bestPct = std::max<uint32>(bestPct, row->pct);
            bestN = std::max<uint32>(bestN, row->n);
            if (row->n == 0)
                all = true;
            n += row->n;
            if (row->n && (!echoEvery || row->n < echoEvery))
                echoEvery = row->n;
        }
        bool const multiplies = kit == KIT_CHAIN || kit == KIT_FRAGMENTS || kit == KIT_EXTRA_PROJECTILES || kit == KIT_ECHO;
        // Never weaker than the strongest source alone.
        out.pct = std::max(bestPct, std::min<uint32>(pct, multiplies ? 75 : 100));
        auto count = [&](uint32 cap) { return uint8(std::max(bestN, std::min(n, cap))); };
        switch (kit)
        {
            case KIT_CHAIN:             out.n = count(6); break;
            case KIT_FRAGMENTS:         out.n = count(10); break;
            case KIT_EXTRA_PROJECTILES: out.n = count(8); break;
            case KIT_SPREAD:            out.n = count(6); out.pct = 100; break;
            case KIT_PIERCE:            out.n = all ? 0 : count(10); break;
            case KIT_ECHO:              out.n = uint8(std::max<uint32>(echoEvery, 2)); break;
            default:                    out.n = 0; break;    // arcs, bursts, shockwaves take everyone
        }
        auto& slot = cache[rows];
        slot.second = name;
        slot.first = out;
        slot.first.name = slot.second.c_str();
        return &slot.first;
    }

    UniqueMechanic const* WornMechanic(Player const* player, UniqueKit kit, SpellEntry const* spellInfo, bool onDeath)
    {
        if (!player || !spellInfo)
            return nullptr;
        return WornMechanicFrom(player, kit, spellInfo, LearnedModifiers(player), onDeath);
    }

    UniqueMechanic const* WornMechanicFrom(Player const* player, UniqueKit kit, SpellEntry const* spellInfo,
                                           std::vector<UniqueMechanic const*> const& learned, bool onDeath)
    {
        if (!player || !spellInfo)
            return nullptr;
        // A spread on landing (n > 0) and one from a corpse (n 0) are two mechanics: never mixed.
        auto mode = [&](UniqueMechanic const* row) { return kit != KIT_SPREAD || (row->n == 0) == onDeath; };
        std::vector<UniqueMechanic const*> rows;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item const* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item)
                continue;
            for (UniqueMechanic const& row : Uniques())
                if (row.item == item->GetEntry() && row.kit == kit && mode(&row) && SpellMatches(row.spell, spellInfo))
                    rows.push_back(&row);
        }
        // The skill trees' nodes and runes too: they all stack.
        for (UniqueMechanic const* row : learned)
            if (row->kit == kit && mode(row) && SpellMatches(row->spell, spellInfo))
                rows.push_back(row);
        return Stacked(rows);
    }

    std::vector<std::string> StackedKitLines(Player const* player, uint8 skill)
    {
        std::vector<std::string> out;
        uint8 const classId = player->getClass();
        // Every kit row on a spell of this skill, by kit and spell.
        std::map<std::pair<uint8, uint32>, std::vector<UniqueMechanic const*>> groups;
        auto take = [&](UniqueMechanic const* row)
        {
            if (row->kit == KIT_RAISE || row->kit == KIT_STEP)
                return;
            SpellEntry const* spell = sSpellTemplate.LookupEntry<SpellEntry>(row->spell);
            // A corpse's spread groups apart from a landing one.
            uint8 const kind = row->kit == KIT_SPREAD && row->n == 0 ? 0 : uint8(row->kit);
            if (spell && SkillOfSpell(classId, spell) == SkillId(skill))
                groups[{ kind, row->spell }].push_back(row);
        };
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (Item const* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                for (UniqueMechanic const& row : Uniques())
                    if (row.item == item->GetEntry())
                        take(&row);
        for (UniqueMechanic const* row : LearnedModifiers(player))
            take(row);
        char buf[256];
        for (auto const& [key, rows] : groups)
        {
            if (rows.size() < 2)
                continue;
            UniqueMechanic const* m = Stacked(rows);
            SpellEntry const* spell = sSpellTemplate.LookupEntry<SpellEntry>(key.second);
            char const* what = spell ? spell->SpellName[0] : "it";
            switch (m->kit)
            {
                case KIT_ARC:       std::snprintf(buf, sizeof(buf), "%s: every other enemy in front takes %u%%", what, m->pct); break;
                case KIT_BURST:     std::snprintf(buf, sizeof(buf), "%s bursts within %.0f yards for %u%%", what, m->value, m->pct); break;
                case KIT_CHAIN:     std::snprintf(buf, sizeof(buf), "%s chains %u times within %.0f yards for %u%%", what, uint32(m->n), m->value, m->pct); break;
                case KIT_FRAGMENTS: std::snprintf(buf, sizeof(buf), "%s shatters into %u shards within %.0f yards for %u%% each", what, uint32(m->n), m->value, m->pct); break;
                case KIT_PIERCE:    std::snprintf(buf, sizeof(buf), "%s pierces %s for %u%%", what, m->n ? "several enemies" : "every enemy", m->pct); break;
                case KIT_EXTRA_PROJECTILES: std::snprintf(buf, sizeof(buf), "%s fires %u extra at %u%%", what, uint32(m->n), m->pct); break;
                case KIT_SPREAD:    std::snprintf(buf, sizeof(buf), "%s spreads to %u more within %.0f yards", what, uint32(m->n), m->value); break;
                case KIT_ECHO:      std::snprintf(buf, sizeof(buf), "every %u casts %s echoes at %u%%", uint32(m->n), what, m->pct); break;
                case KIT_SHOCKWAVE: std::snprintf(buf, sizeof(buf), "%s sends a shockwave %.0f yards for %u%%", what, m->value, m->pct); break;
                default:            std::snprintf(buf, sizeof(buf), "%s", what); break;
            }
            out.push_back(std::string("|cffe6cc80Stacked (") + m->name + "):|r " + buf);
        }
        return out;
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
        std::vector<UniqueMechanic const*> const learned = LearnedModifiers(player);

        // Extra projectiles (A), alternating sides of the aim, stepping out: +1, -1, +2, -2 lines.
        if (UniqueMechanic const* row = WornMechanicFrom(player, KIT_EXTRA_PROJECTILES, spellInfo, learned))
        {
            float const step = row->value * float(M_PI) / 180.0f;
            // No target is left out: up close the fan lands on one enemy (a shotgun), farther out
            // it spreads over several.
            ObjectGuid const exclude;
            (void)struck;
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
            ReportProc(player, PROC_EFFECT, 0, MechanicName(row), "fires %u extra %s at %u%% damage.",
                       uint32(row->n), row->n == 1 ? spellInfo->SpellName[0] : "projectiles", row->pct);
        }

        // Echo (I): every `n`th cast flies again along the same aim a moment later.
        if (UniqueMechanic const* row = WornMechanicFrom(player, KIT_ECHO, spellInfo, learned))
        {
            uint32& count = sEchoCounts[player->GetObjectGuid()];
            if (++count >= std::max<uint8>(row->n, 1))
            {
                count = 0;
                uint32 const pct = row->pct;
                char const* echoName = MechanicName(row);
                player->m_events.AddEvent(new UnitLambdaEvent(*player, [spellInfo, aim, aimDist, pct, echoName](Unit& owner)
                {
                    Player* caster = static_cast<Player*>(&owner);
                    if (!caster->IsInWorld() || !caster->IsAlive())
                        return;
                    Spell* echo = new Spell(caster, spellInfo, TRIGGERED_OLD_TRIGGERED);
                    echo->SetArpgSecondary(pct, ObjectGuid());
                    echo->SetArpgAim(aim, aimDist);
                    SpellCastTargets targets;
                    echo->SpellStart(&targets);
                    ReportProc(caster, PROC_EFFECT, 0, echoName, "echoes %s at %u%% damage.", spellInfo->SpellName[0], pct);
                }), player->m_events.CalculateTime(ECHO_DELAY));
            }
        }
    }

    void OnSpellDamage(Spell* spell, Unit* caster, Unit* victim, uint32 dealt)
    {
        if (!spell || !victim || !dealt || !Active(caster))
            return;
        // The skill trees' on-hit nodes (Seals, Judgement), before the kits': the main hit only.
        // An extra projectile or an echo (a secondary) still sets off the on-hit kits below, which
        // only relay damage and never cast again, so nothing multiplies without end.
        if (!spell->IsArpgSecondary())
            OnSkillSpellDamage(spell, caster, victim, dealt);
        Player* player = static_cast<Player*>(caster);
        SpellEntry const* spellInfo = spell->m_spellInfo;
        SpellSchoolMask const mask = spell->GetSchoolMask();
        // The caster's areas: melee (Strength, gear) widens arcs and shockwaves, spell (Intellect,
        // gear) bursts and shatters.
        float areaAt = -1.0f, spellAreaAt = -1.0f;
        auto area = [&]() { return areaAt < 0.0f ? (areaAt = MeleeAreaScale(caster)) : areaAt; };
        auto spellArea = [&]() { return spellAreaAt < 0.0f ? (spellAreaAt = SpellAreaScale(caster)) : spellAreaAt; };
        // The skill rows once for every kit below.
        std::vector<UniqueMechanic const*> const learned = LearnedModifiers(player);

        // Fragments (D): the hit shatters, `n` shards flying out from the target to the nearest
        // enemies around it within `value` yards; shards with no enemy to find scatter on the
        // ground, so a shatter always shows.
        if (UniqueMechanic const* row = WornMechanicFrom(player, KIT_FRAGMENTS, spellInfo, learned))
        {
            std::vector<std::pair<float, Unit*>> struckBy;
            float const reach = row->value * spellArea();
            for (Unit* unit : EnemiesNear(victim, reach + 5.0f))
            {
                if (unit == victim || !unit->IsAlive() || !MayCatchUnit(caster, unit, nullptr))
                    continue;
                float const dist = victim->GetDistance(unit);
                if (dist > reach || !victim->IsWithinLOSInMap(unit))
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
            ReportProc(player, PROC_DAMAGE, 0, MechanicName(row), "shatters %s into %u %s: %u %s hit for %u each.",
                       spellInfo->SpellName[0], uint32(row->n), Plural(row->n, "shard", "shards"),
                       uint32(struckBy.size()), Plural(struckBy.size(), "enemy", "enemies"), share);
            // The rest scatter outward, fanned about the line the spell flew.
            uint32 const spare = row->n - uint32(struckBy.size());
            float const away = caster->GetAngle(victim);
            for (uint32 k = 0; k < spare; ++k)
            {
                float const fan = away + (float(k) - float(spare - 1) * 0.5f) * (float(M_PI) / 4.0f);
                float x = victim->GetPositionX() + std::cos(fan) * reach * SHARD_SCATTER;
                float y = victim->GetPositionY() + std::sin(fan) * reach * SHARD_SCATTER;
                float z = victim->GetPositionZ();
                victim->UpdateAllowedPositionZ(x, y, z);
                SendGroundVisual(victim, x, y, z, spellInfo);
            }
        }

        // Chain (C): leaps from the target to the nearest enemy not yet hit, `n` times.
        if (UniqueMechanic const* row = WornMechanicFrom(player, KIT_CHAIN, spellInfo, learned))
        {
            GuidVector hit{ victim->GetObjectGuid() };
            if (Unit* first = NextInChain(caster, victim, row->value, hit))
            {
                hit.push_back(first->GetObjectGuid());
                uint32 const share = std::max<uint32>(1, dealt * row->pct / 100);
                RelayHit(player, victim->GetObjectGuid(), first->GetObjectGuid(), spellInfo, mask, share,
                         uint8(row->n - 1), row->value, hit);
                ReportProc(player, PROC_DAMAGE, 0, MechanicName(row), "chains %s on to %s (up to %u %s) for %u each.",
                           spellInfo->SpellName[0], first->GetName(), uint32(row->n), Plural(row->n, "jump", "jumps"), share);
            }
        }

        // Burst (E): everything within `value` yards of the target, at once.
        if (UniqueMechanic const* row = WornMechanicFrom(player, KIT_BURST, spellInfo, learned))
        {
            uint32 const share = std::max<uint32>(1, dealt * row->pct / 100);
            size_t done = 0;
            float const reach = row->value * spellArea();
            for (Unit* unit : EnemiesNear(victim, reach + 5.0f))
            {
                if (done >= BURST_MAX)
                    break;
                if (unit == victim || !unit->IsAlive() || !MayCatchUnit(caster, unit, nullptr))
                    continue;
                if (victim->GetDistance(unit) > reach || !victim->IsWithinLOSInMap(unit))
                    continue;
                DealShare(caster, unit, spellInfo, mask, share);
                ++done;
            }
            if (done)
                ReportProc(player, PROC_DAMAGE, 0, MechanicName(row), "bursts %s onto %u %s for %u each.",
                           spellInfo->SpellName[0], uint32(done), Plural(done, "enemy", "enemies"), share);
        }

        // Arc (G): a melee ability strikes every other enemy in the swing's arc and reach too.
        if (UniqueMechanic const* row = WornMechanicFrom(player, KIT_ARC, spellInfo, learned))
        {
            uint32 const share = std::max<uint32>(1, dealt * row->pct / 100);
            size_t done = 0;
            for (Unit* unit : EnemiesNear(caster, 10.0f + (area() - 1.0f) * 5.0f))
            {
                if (unit == victim || !unit->IsAlive() || !MayCatchUnit(caster, unit, nullptr))
                    continue;
                float const width = ARC_WIDTH * area();
                if (!caster->CanReachWithMeleeAttack(unit, (area() - 1.0f) * 5.0f) ||
                        (width < 2.0f * float(M_PI) && !caster->HasInArc(unit, width)))
                    continue;
                DealShare(caster, unit, spellInfo, mask, share);
                ++done;
            }
            if (done)
                ReportProc(player, PROC_DAMAGE, 0, MechanicName(row), "sweeps %s through %u more %s for %u each.",
                           spellInfo->SpellName[0], uint32(done), Plural(done, "enemy", "enemies"), share);
        }

        // Pierce (B): on along the line past the target, out to the spell's range.
        if (UniqueMechanic const* row = WornMechanicFrom(player, KIT_PIERCE, spellInfo, learned))
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
            if (!onward.empty())
                ReportProc(player, PROC_DAMAGE, 0, MechanicName(row), "drives %s through %u more %s for %u each.",
                           spellInfo->SpellName[0], uint32(onward.size()), Plural(onward.size(), "enemy", "enemies"), share);
        }

        // Shockwave (H): along the caster's facing, past the target too.
        if (UniqueMechanic const* row = WornMechanicFrom(player, KIT_SHOCKWAVE, spellInfo, learned))
        {
            uint32 const share = std::max<uint32>(1, dealt * row->pct / 100);
            std::vector<Unit*> const line = UnitsAlongLine(caster, caster->GetOrientation(), 0.0f, row->value * area(), victim);
            for (Unit* unit : line)
                DealShare(caster, unit, spellInfo, mask, share);
            if (!line.empty())
                ReportProc(player, PROC_DAMAGE, 0, MechanicName(row), "sends a shockwave ahead: %u %s hit for %u each.",
                       uint32(line.size()), Plural(line.size(), "enemy", "enemies"), share);
        }

        // Step (K): a kill moves the player behind the nearest enemy, just after the hit.
        if (!victim->IsAlive() && !spell->IsArpgSecondary())
        {
            if (UniqueMechanic const* row = WornMechanicFrom(player, KIT_STEP, spellInfo, learned))
            {
                std::vector<Unit*> next = NearestTo(caster, caster, row->value, 1);
                if (!next.empty())
                {
                    ObjectGuid const nextGuid = next.front()->GetObjectGuid();
                    char const* stepName = MechanicName(row);
                    player->m_events.AddEvent(new UnitLambdaEvent(*player, [nextGuid, stepName](Unit& owner)
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
                        ReportProc(&owner, PROC_EFFECT, 0, stepName, "steps you behind %s.", enemy->GetName());
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
        // The skill trees' on-land nodes (Hammer of Justice).
        OnSkillSpellLanded(spell, caster, target);
        // Spread on landing (F, `n` > 0).
        UniqueMechanic const* row = WornMechanic(player, KIT_SPREAD, spellInfo);
        if (!row || !row->n || !caster->CanAttack(target))
            return;
        std::vector<Unit*> const more = NearestTo(caster, target, row->value, row->n);
        for (Unit* unit : more)
            CastCopyAt(player, spellInfo, unit);
        if (!more.empty())
            ReportProc(player, PROC_EFFECT, 0, MechanicName(row), "spreads %s to %u more %s.",
                       spellInfo->SpellName[0], uint32(more.size()), Plural(more.size(), "enemy", "enemies"));
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
            UniqueMechanic const* row = WornMechanic(static_cast<Player*>(auraCaster), KIT_SPREAD, holder->GetSpellProto(), true);
            if (row && !row->n)
                spreads.emplace_back(auraCaster->GetObjectGuid(), holder->GetSpellProto());
        }
        for (auto const& [guid, spellInfo] : spreads)
        {
            Player* caster = victim->GetMap()->GetPlayer(guid);
            UniqueMechanic const* row = caster ? WornMechanic(caster, KIT_SPREAD, spellInfo, true) : nullptr;
            if (!row)
                continue;
            std::vector<Unit*> const more = NearestTo(caster, victim, row->value, BURST_MAX);
            for (Unit* unit : more)
                CastCopyAt(caster, spellInfo, unit);
            if (!more.empty())
                ReportProc(caster, PROC_EFFECT, 0, MechanicName(row), "spreads %s from the corpse to %u %s.",
                           spellInfo->SpellName[0], uint32(more.size()), Plural(more.size(), "enemy", "enemies"));
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
        ReportProc(killer, PROC_EFFECT, 0, MechanicName(raise), "raises %s to fight for you.", risen->GetName());
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
        bool const exorcism = std::strcmp(name, "Exorcism") == 0;
        bool const wrath = std::strcmp(name, "Holy Wrath") == 0;
        if (!exorcism && !wrath)
            return false;
        Player const* player = static_cast<Player const*>(caster);
        // The web's Purifying Light, or the skill specialised (Arpg/ArpgSkills.h).
        return HasKeystone(player, KEY_PURIFYING_LIGHT) ||
               IsSpecialised(player, exorcism ? SKILL_EXORCISM : SKILL_HOLY_WRATH);
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
        ReportProc(player, PROC_DAMAGE, 0, "Dawnbringer", "sends a Holy bolt at %s for %u.", foes.front()->GetName(),
                   std::max<uint32>(1, amount / 2));
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
            std::vector<Unit*> const foes = NearestTo(player, player, MARTYRS_WARD_RANGE, BURST_MAX);
            for (Unit* unit : foes)
                DealShare(player, unit, proto, SPELL_SCHOOL_MASK_HOLY, damage);
            if (!foes.empty())
                ReportProc(player, PROC_DAMAGE, 0, "Martyr's Ward", "pulses Holy at %u %s for %u.",
                           uint32(foes.size()), Plural(foes.size(), "enemy", "enemies"), damage);
            return;
        }
    }

    std::vector<Unit*> NearestFoes(Unit* caster, Unit* center, float radius, size_t count)
    {
        return NearestTo(caster, center, radius, count);
    }

    void StrikeFoe(Unit* caster, Unit* unit, SpellEntry const* spellInfo, uint32 schoolMask, uint32 amount)
    {
        DealShare(caster, unit, spellInfo, SpellSchoolMask(schoolMask), amount);
    }

    void SendHolyBolt(Player* player, Unit* to, uint32 amount)
    {
        SpellEntry const* bolt = sSpellTemplate.LookupEntry<SpellEntry>(HOLY_SHOCK_DAMAGE);
        if (!bolt || !to)
            return;
        RelayHit(player, player->GetObjectGuid(), to->GetObjectGuid(), bolt, SPELL_SCHOOL_MASK_HOLY,
                 std::max<uint32>(1, amount), 0, 0.0f, GuidVector{});
    }

    std::vector<Unit*> FoesAlongLine(Unit* caster, float bearing, float fromDist, float toDist, Unit const* exclude)
    {
        return UnitsAlongLine(caster, bearing, fromDist, toDist, exclude);
    }

    void SendBolt(Player* player, Unit* to, SpellEntry const* visual, uint32 schoolMask, uint32 amount)
    {
        if (!visual || !to)
            return;
        RelayHit(player, player->GetObjectGuid(), to->GetObjectGuid(), visual, SpellSchoolMask(schoolMask),
                 std::max<uint32>(1, amount), 0, 0.0f, GuidVector{});
    }

    void BounceCast(Player* player, Unit* from, SpellEntry const* spellInfo, uint8 hops, float range, GuidVector hit)
    {
        if (!hops || !from)
            return;
        if (hit.empty())
            hit.push_back(from->GetObjectGuid());
        Unit* next = NextInChain(player, from, range, hit);
        if (!next)
            return;
        hit.push_back(next->GetObjectGuid());
        SendRelayVisual(from, next, spellInfo);
        ObjectGuid const nextGuid = next->GetObjectGuid();
        auto land = [nextGuid, spellInfo, hops, range, hit](Unit& owner)
        {
            Player* caster = static_cast<Player*>(&owner);
            if (!caster->IsInWorld())
                return;
            Unit* target = caster->GetMap()->GetUnit(nextGuid);
            if (!target || !target->IsAlive())
                return;
            CastCopyAt(caster, spellInfo, target);
            BounceCast(caster, target, spellInfo, hops - 1, range, hit);
        };
        player->m_events.AddEvent(new UnitLambdaEvent(*player, land), player->m_events.CalculateTime(BOUNCE_DELAY));
    }
}
