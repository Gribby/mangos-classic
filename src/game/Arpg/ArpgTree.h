/*
 * ARPG passive web, for the benilla ARPG client (see ArpgCombat.h and the client's
 * docs/ARPG-CHARACTER.md, "The passive web").
 *
 * An ARPG player spends points in a web of nodes per class in place of vanilla's talents. The
 * web spreads out from a free start node; a node can be taken only if it joins one already
 * taken, and given back only if everything else taken still joins the start. Points: one per
 * level past the first, plus one for the first kill of each dungeon and raid final boss
 * (Bosses()). Respec is free, out of combat.
 *
 * A node is one of:
 *   WEB_START     the start, always taken
 *   WEB_SMALL     a few small effects: attributes, tag damage, ARPG stats
 *   WEB_NOTABLE   one named effect: a vanilla talent at a fixed rank, a hook, or both
 *   WEB_KEYSTONE  a rule that changes how the class plays, with a cost (Keystone)
 * Each node's effects add into the player's WebTotals, which ArpgCharacter.h applies (stats)
 * and reads (damage, speed, area, cooldowns). A notable's talent is found by name within the
 * class's own talent tabs and taught at its rank.
 *
 * The web is data in code (Web()), built from its regions and arms; its node ids are its build
 * order, so the build only ever appends.
 *
 * Talents for an ARPG character: its vanilla talent spells go at the first ARPG hello, except
 * the ones the web teaches and the class's talent-granted active spells (Consecration, Holy
 * Shock…), which an ARPG character gets free at the level the talent's row needs (10 + 5 per
 * row). These spells stay out of vanilla's talent accounting (TreeOwnsSpell).
 *
 * Saved in the characters database, which the server creates if missing:
 *   character_arpg_web    (guid, node): the nodes taken; node 1, the start, marks an ARPG character
 *   character_arpg_bosses (guid, entry): the final bosses killed, each worth a point
 *
 *   CMSG_ARPG_ACTION kind 8  take: uint16 node
 *                    kind 9  respec
 *                    kind 10 web query
 *                    kind 11 give back: uint16 node
 *   SMSG_ARPG_TREE (0x33F): uint8 version (2), uint16 points total, uint16 points spent,
 *     uint8 region count, per region: cstring name, int16 x, int16 y (its label);
 *     uint16 node count, per node: uint16 id, uint8 kind, uint8 region, int16 x, int16 y,
 *     uint8 taken, uint32 icon spell (0: none), cstring name, cstring text;
 *     uint16 link count, per link: uint16 node, uint16 node.
 */

#ifndef MANGOS_ARPG_TREE_H
#define MANGOS_ARPG_TREE_H

#include "Common.h"
#include "Arpg/ArpgStates.h"
#include "Arpg/ArpgTags.h"
#include "Arpg/ArpgUniques.h"

#include <memory>
#include <set>
#include <vector>

class Player;
class Unit;
struct SpellEntry;

namespace Arpg
{
    enum WebNodeKind : uint8
    {
        WEB_START    = 0,
        WEB_SMALL    = 1,
        WEB_NOTABLE  = 2,
        WEB_KEYSTONE = 3,
    };

    // The rules a node can carry, each with its own hook (ArpgCharacter.h, ArpgUniques.h).
    enum Keystone : uint8
    {
        KEY_NONE = 0,
        // Skill tree capstones (the skill trees come next; no web node gives them).
        KEY_AVENGER,           // Judgement keeps the Seal; its cooldown doubles
        KEY_MARTYRS_WARD,      // Retribution Aura burns every enemy within 10 yd each second, doubled
        KEY_DAWNBRINGER,       // heals on yourself send half their amount as holy damage
        KEY_PURIFYING_LIGHT,   // Exorcism and Holy Wrath strike any enemy
        // Paladin web notables
        KEY_TWO_HANDED_MASTERY,// +12% Melee damage with a two-handed weapon
        KEY_SHIELD_AND_HAMMER, // +10% Melee damage with a one-handed weapon and a shield
        KEY_HOLY_WEAPONS,      // +3% Holy damage per 10 Strength
        KEY_RIGHTEOUS_FURY,    // +20% Holy damage with three or more enemies within 8 yd
        KEY_DIVINE_FAVOUR,     // every 20 sec, the next Holy spell crits
        KEY_BLESSED_RECOVERY,  // healing yourself: +10% cooldown recovery for 4 sec
        KEY_SHIELD_WALL,       // blocks heal you for 1% of your health
        // Paladin web keystones
        KEY_ZEALOT,            // +25% attack speed; Seals drain 1% mana a second
        KEY_CRUSADE,           // +4% damage per kill in the last 5 sec (10 max); 20% slower out of combat
        KEY_LIGHTFORGED,       // your heals become holy bolts and no longer heal you; +30% Holy damage
        KEY_MARTYR,            // 25% of damage taken strikes every enemy within 10 yd; healing taken halved
        KEY_UNYIELDING,        // can't be stunned or slowed; 20% slower
        // Skill tree hooks (ArpgSkills.h); those with ranks read KeyRank
        KEY_WIDE_SWING,        // swings strike every enemy in front, at 20/35/50%
        KEY_LONG_ARM,          // +1 yd melee reach per rank
        KEY_WHIRLING_STRIKES,  // every 4th swing strikes all around
        KEY_QUICKENED,         // +4% attack speed per rank
        KEY_MOMENTUM,          // +1% swing damage per rank per hit in a row, 5 hits at most
        KEY_CRUSADERS_PACE,    // kills give +30% movement speed for 3 sec
        KEY_HEAVY_HAND,        // +5% Physical damage per rank with a two-hander
        KEY_STAGGER,           // 10% chance per rank a swing dazes
        KEY_SHOCKWAVE_STRIKE,  // a swing crit sends a 10 yd shockwave at 40%
        KEY_HOLY_EDGE,         // +6% Seal damage per rank
        KEY_TWIN_SEALS,        // two different Seals at once
        KEY_COMMANDING_SEAL,   // +10% Seal of Command damage per rank
        KEY_RELENTLESS,        // +1 Seal of Command proc a minute per rank
        KEY_ZEAL,              // +10% attack speed per rank while Seal of the Crusader is on
        KEY_MANA_STRIKE,       // Seal hits give 1% of max mana per rank
        KEY_LIGHT_OF_THE_CRUSADER, // Seal hits heal 5% of their damage
        KEY_ECHOING_VERDICT,   // Judgement's chain loses 10% less per rank
        KEY_FINAL_VERDICT,     // a Judgement kill readies Judgement
        KEY_RADIANCE,          // +8% Judgement damage per rank
        KEY_SENTENCE,          // Judgement deals double damage to stunned enemies
        KEY_RIGHTEOUS_MIND,    // Judgement restores 5% of max mana per rank
        KEY_WALKING_CONSECRATION, // Consecration follows you
        KEY_BURNING_GROUND,    // +8% Consecration damage per rank
        KEY_SEARING_LIGHT,     // +5% Holy damage per rank to enemies in your Consecration
        KEY_SANCTIFIED,        // Judgement on a consecrated enemy bursts at 50%
        KEY_HALLOWED_GROUND,   // 1% health a second per rank inside your Consecration
        KEY_STEADFAST,         // no stuns inside your Consecration
        KEY_SACRED_SEAL,       // Seals strike twice on consecrated enemies
        KEY_RICOCHET,          // Hammer of Justice bounces to 1 more enemy per rank
        KEY_BLESSED_HAMMER,    // Hammer of Justice also hurls three blessed hammers
        KEY_HOLY_HAMMER,       // Hammer of Justice deals Holy damage, 30% of attack power + 15% per rank
        KEY_SENTENCE_PASSED,   // +15% damage to stunned enemies
        KEY_WRATHFUL,          // Holy Wrath fires on its own when 4 or more enemies are near
        KEY_EXECUTIONER,       // Hammer of Wrath deals double damage below 20% health
        // Holy Light's tree (revision 2)
        KEY_LIGHT_STRIKE,      // heals strike the nearest enemy for 15% per rank
        KEY_JUDGING_LIGHT,     // heals Judge enemies within 8 yd, a hit of 25% per rank
        KEY_AVENGING_LIGHT,    // healing yourself: 5% more damage per rank for 5 sec
        KEY_HOLY_NOVA,         // heals burst for 50% as Holy around you
        // The mage's web (ArpgMage.cpp)
        KEY_PYROMANCER,        // 40% more Fire damage, 30% less Frost and Arcane
        KEY_WILDFIRE,          // an Ignited enemy's death spreads its Ignite to 3 near it
        KEY_SHATTER,           // 40% more against frozen enemies, 15% less against the rest
        KEY_PERMAFROST,        // enemies that hit you are chilled, frozen if chilled; 25% less Fire
        KEY_BRIMMING_MIND,     // more spell damage with mana above half, less below
        KEY_ARCANE_FLUX,       // Overload bursts 50% more and restore mana; hits 10% less
        // The mage's skill trees (ArpgMage.cpp)
        KEY_PYROCLASM,         // critical Fireballs explode around the target for 60%
        KEY_SHATTERSTORM,      // Frostbolt on a frozen enemy shatters onto those near it for 80%
        KEY_DEEP_FREEZE,       // Cone of Cold freezes
        KEY_OVERCHARGE,        // Overload bursts 40% more
        KEY_ARCANE_SIPHON,     // Arcane Missiles hits restore 0.5% mana per rank
        KEY_MANA_BURST,        // Arcane Explosion restores 1% mana per enemy hit, 5 at most
        KEY_GLACIAL_STORM,     // Blizzard chills, and may freeze the chilled
        KEY_CHAIN_BLAST,       // a kill with Fire Blast readies it again
        KEY_SEARING_BRAND,     // Scorch brands the target with Fire Vulnerability
        KEY_DETONATE,          // Fire Blast sets off the target's Ignite at once, onto those near it
        // The druid's web (ArpgDruid.cpp)
        KEY_CELESTIAL_FOCUS,   // 30% more Arcane and Nature spell damage; attacks 40% less
        KEY_NATURES_BOUNTY,    // Poisons 50% more; hits 15% less
        KEY_PREDATOR,          // Bleeds 60% more; spells 30% less
        KEY_KING_OF_THE_JUNGLE,// 4% more Melee damage per kill in the last 6 sec; heals 50% less
        KEY_TREE_OF_LIFE,      // heals 50% more; damage 20% less
        KEY_LIVING_THORNS,     // Thorns strikes every enemy within 8 yd each second; heals 30% less
        // The druid's skill trees (ArpgDruid.cpp)
        KEY_ECLIPSE,           // critical Wraths and Starfires restore mana and quicken the next cast
        KEY_HIVE_MIND,         // Insect Swarm ticks Poison
        KEY_LUNAR_TIDE,        // Moonfire and Insect Swarm ticks restore mana
        KEY_TEMPEST,           // Hurricane strikes may call down a Wrath
        KEY_BITING_HAIL,       // Hurricane strikes may chill, 5% per rank
        KEY_EYE_OF_THE_WILD,   // Hurricane strikes heal 0.3% of max health per rank
        KEY_BRISTLING_FUR,     // blows taken in Bear Form give 2 rage per rank
        KEY_SURVIVAL_INSTINCTS,// Bear hits heal 2% of their damage per rank
        KEY_THORNED_HIDE,      // in Bear Form, attackers take 30% of attack power as Nature
        KEY_SAVAGE_FURY,       // critical Claws, Shreds and Rakes give an extra combo point
        KEY_BLOOM,             // heals burst for 50% as Nature around you
        KEY_NATURES_WRATH,     // heals strike the nearest enemy for 15% per rank as Nature
        KEY_VERDANT_FURY,      // healing yourself: 5% more damage per rank for 5 sec
        KEY_WRATH_OF_THE_WILD, // heals Poison every enemy within 8 yd
        // The warlock's web (Arpg/ArpgWarlock.cpp)
        KEY_SOUL_SIPHON,       // drains heal 2% more per damage over time on the target; direct hits 15% less
        KEY_PLAGUE_LORD,       // damage over time 40% more; direct hits 20% less
        KEY_HELLSTORM,         // Rain of Fire and Hellfire stun on a crit; damage over time 20% less
        KEY_RUINOUS_POWER,     // Shadow Bolt and Searing Pain 30% more; minions 30% less
        KEY_LEGION,            // every kill raises an imp; your own spells 15% less
        KEY_DEMONIC_KNOWLEDGE, // minions 30% more damage and life; you 20% less
        // The warlock's skill trees (Arpg/ArpgWarlock.cpp, Arpg/ArpgMinions.cpp)
        KEY_NIGHTFALL,         // Corruption and Agony ticks may loose a free Shadow Bolt, 2% per rank
        KEY_SHADOWBURN,        // a kill with Shadowburn readies it again
        KEY_TORMENTED_SOUL,    // Curse of Agony ticks may Wither, 10% per rank
        KEY_HAUNTING_AGONY,    // Curse of Agony 100% more on Withered enemies
        KEY_SOUL_FEAST,        // damage over time ticks heal 0.3% of max health
        KEY_BURNING_RAIN,      // Rain of Fire ticks may Ignite, 10% per rank
        KEY_FIRESTORM,         // Rain of Fire strikes Ignited enemies twice
        KEY_HELLFIRE_WARD,     // 25% less self-damage from Hellfire per rank
        KEY_INFERNAL_HEAT,     // Hellfire ticks give minions 5% more damage per rank for 4 sec
        KEY_BACKLASH,          // 20% less damage taken while channelling Hellfire
        KEY_LIFE_LINK,         // Drain Life ticks heal your minions
        KEY_SOUL_REAPER,       // Drain Soul 30% more per rank below 25% health
        KEY_REAPING,           // a kill under your Drain Soul raises an imp and restores 5% mana
        KEY_DARK_PACT,         // Life Tap gives 1% more of max mana per rank
        KEY_DEMONIC_PACT,      // Life Tap also heals you 5% of max health, from your demon
        KEY_FEL_VITALITY,      // your demon has 10% more life per rank
        KEY_DEMONIC_EMBRACE,   // your demon deals 8% more per rank
        KEY_CLEAVING_DEMON,    // your demon's melee strikes those in front, 30/45/60%
        KEY_FEL_DOMINATION,    // a dead demon is summoned again at once (once every 30 sec)
        KEY_LASH_ARC,          // Lash of Pain and Firebolt strike 1 more per rank
        KEY_DEMONIC_MASTERY,   // while your demon lives, 4% more damage per rank
        KEY_GRAND_PACT,        // your demon: 40% more life, 25% more damage
        KEY_TORMENT_COMMAND,   // your curses send every minion at their target, 20% more to it
        KEY_SACRIFICE_COMMAND, // Life Tap detonates your oldest horde minion
        KEY_SOUL_LINK,         // 10% per rank of damage taken goes to your demon
        KEY_DEMONIC_SACRIFICE, // no demon: 30% more damage
        KEY_IMP_GATE,          // kills raise an imp, 25% per rank
        KEY_IMP_SWARM,         // the horde holds 2 more per rank
        KEY_FIERY_SWARM,       // horde imps' Firebolts Ignite, 10% per rank
        KEY_BURNING_LEGION,    // a horde minion's kill raises an imp
        KEY_INFERNAL_FALL,     // Inferno's landing stuns and strikes within 8/10/12 yd
        KEY_IMMOLATION_AURA,   // Infernals deal 20% more per rank
        KEY_LORD_OF_FLAME,     // Inferno calls two Infernals
        KEY_DOOMBRINGER,       // the Doomguard deals 15% more per rank
        KEY_PIT_LORD,          // the Doomguard stays 10 min
        KEY_EMBERSTORM,        // the Immolate skill, its burns and Rain of Fire too: 6% more per rank
        MAX_KEYSTONE
    };

    // A spell modifier a skill node gives: `op` (SpellModOp) of `type` (SpellModType) by `value`
    // over the paladin spells whose family flags fit `mask`.
    struct SkillSpellMod
    {
        uint8 op;
        uint8 type;
        int32 value;
        uint64 mask;                                    // the family bits the client is told of
        std::shared_ptr<std::set<uint32> const> spells; // the spells it reaches (nullptr: by `mask`)

        bool operator==(SkillSpellMod const& o) const
        {
            return op == o.op && type == o.type && value == o.value && mask == o.mask && spells == o.spells;
        }
    };

    // What a player's taken nodes add up to.
    struct WebTotals
    {
        int32 stat[5] = {};          // Strength, Agility, Stamina, Intellect, Spirit
        int32 armourPct = 0;
        int32 tag[MAX_TAG] = {};     // % damage (or healing, for Heal) per tag
        int32 healingPct = 0;
        int32 movePct = 0;
        int32 cooldownPct = 0;
        int32 lifeOnKill = 0;
        int32 manaOnHit = 0;         // mana per swing or skill hit
        int32 meleeAreaPct = 0;
        int32 spellAreaPct = 0;
        int32 blockPct = 0;
        // Item affixes' own stats (Arpg/ArpgAffixes.h); applied in Arpg/ArpgCharacter.cpp.
        int32 attackSpeedPct = 0;    // swing speed
        int32 castSpeedPct = 0;      // cast time cut
        int32 meleeCritPct = 0;      // melee and ranged critical strike chance
        int32 spellCritPct = 0;      // spell critical strike chance
        int32 spellCritDamagePct = 0; // spell critical strike damage
        int32 costPct = 0;           // mana cost cut
        int32 healthPct = 0;         // maximum health
        int32 manaPct = 0;           // maximum mana
        int32 attackPower = 0;
        int32 spellPower = 0;        // spell damage, every school
        int32 resist[5] = {};        // fire, nature, frost, shadow, arcane
        int32 lifeLeech = 0;         // of damage dealt, in thousandths
        int32 manaLeech = 0;         // of damage dealt, in thousandths
        int32 healthRegen = 0;       // health a second
        int32 manaRegen = 0;         // mana a second
        int32 damageTakenPct = 0;    // damage taken cut
        int32 thorns = 0;            // damage back at each attacker in reach that hits
        int32 elitePct = 0;          // damage to elites and bosses
        int32 slaying[16] = {};      // damage to a creature type (CreatureType)
        int32 globeHealPct = 0;      // healing from globes and the flask
        int32 goldFindPct = 0;
        int32 xpPct = 0;
        // Damage (Arpg/ArpgDamage.h), by damage tag (TAG_PHYSICAL .. TAG_SHADOW).
        int32 critDamagePct = 0;     // critical strike multiplier, every hit
        int32 addedAttack[7] = {};   // damage added to attacks, its average at the ARPG's size
        int32 addedSpell[7] = {};    // damage added to spells
        int32 ailmentChance[7] = {}; // % chance to inflict the type's ailment
        int32 ailmentPct = 0;        // increased damage with ailments
        // Skill levels from gear (Arpg/ArpgSkills.h, GearSkillLevels): to all Melee, Spell, Holy,
        // and elemental (Fire, Frost, Nature, Arcane or Shadow) skills.
        int32 skillLevel[4] = {};
        // The skill trees' ailment nodes: by skill id, the % chance its hits inflict each damage
        // tag's ailment, whatever their own type.
        int32 skillAilment[16][7] = {};
        // Synergy (Arpg/ArpgStates.h): increased damage against enemies in each state, Daze build-up,
        // and the skill trees' conversions: by skill id, a damage tag + 1 its hits count as (0: none).
        int32 vsState[MAX_STATE] = {};
        int32 dazeBuildPct = 0;
        uint8 convert[16] = {};
        // Minions (Arpg/ArpgMinions.h): their life, attack and cast speed, the horde's size, and
        // their life leech (thousandths of their damage). Their damage is the Minion tag's.
        int32 minionLifePct = 0;
        int32 minionSpeedPct = 0;
        int32 hordeSize = 0;
        int32 minionLeech = 0;
        uint8 rank[MAX_KEYSTONE] = {}; // per Keystone: 0 not taken, else its rank

        std::vector<SkillSpellMod> mods; // the skill nodes' spell modifiers

        bool Has(Keystone key) const { return key < MAX_KEYSTONE && rank[key] != 0; }
        uint8 Rank(Keystone key) const { return key < MAX_KEYSTONE ? rank[key] : 0; }
    };

    // Where a share of the totals comes from, for the character sheet.
    enum TotalsSource : uint8 { TOTALS_WEB, TOTALS_SKILLS, TOTALS_ITEMS };

    // One source's share of `player`'s totals.
    WebTotals SourceTotals(Player const* player, TotalsSource source);

    // The final bosses whose first kill gives a point, by creature entry.
    std::vector<uint32> const& Bosses();

    // Web points a player has: one per level past the first, one per final boss killed.
    uint32 TreePointsFor(Player const* player);

    // Load the player's web from the database (at login, before their spells load).
    void LoadTree(Player* player);

    // Forget a logged-out player's web.
    void UnloadTree(Player* player);

    // At the ARPG hello: mark the character, settle its talent spells, apply the web, send it.
    void OnTreeHello(Player* player);

    // Every second for an ARPG player: a level gained unlocks spells and a point.
    void UpdateTree(Player* player);

    // Whether `spell` is one the web or the class's free talent spells own (out of talent accounting).
    bool TreeOwnsSpell(Player const* player, uint32 spell);

    // Take `node`, if the rules allow; answers with the web.
    void SpendNode(Player* player, uint16 node);

    // Give `node` back, if the rest still joins the start; answers with the web.
    void RefundNode(Player* player, uint16 node);

    // Give every node back (free, out of combat); answers with the web.
    void Respec(Player* player);

    // Send the player's web (SMSG_ARPG_TREE).
    void SendTree(Player* player);

    // `victim` died to `killer`'s group: a final boss's first kill gives each ARPG player there a point.
    void CreditBossKill(Player* killer, Unit* victim);

    // What the player's web adds up to; nullptr for a player without an ARPG web.
    std::shared_ptr<WebTotals const> TotalsOf(Unit const* unit);

    // The uniques-kit rows the player's skill nodes give (ArpgSkills.h).
    std::vector<UniqueMechanic const*> LearnedModifiers(Player const* player);

    // Whether the player has taken a node carrying `key`.
    bool HasKeystone(Unit const* unit, Keystone key);

    // The rank of `key` the player has taken (0 for none).
    uint8 KeyRank(Unit const* unit, Keystone key);

    // Recompute the player's totals (web and skills) and apply them.
    void RefreshTotals(Player* player);
}

#endif
