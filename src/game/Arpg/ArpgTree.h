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
#include "Arpg/ArpgTags.h"
#include "Arpg/ArpgUniques.h"

#include <memory>
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
        MAX_KEYSTONE
    };

    // A spell modifier a skill node gives: `op` (SpellModOp) of `type` (SpellModType) by `value`
    // over the paladin spells whose family flags fit `mask`.
    struct SkillSpellMod
    {
        uint8 op;
        uint8 type;
        int32 value;
        uint64 mask;

        bool operator==(SkillSpellMod const& o) const { return op == o.op && type == o.type && value == o.value && mask == o.mask; }
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
        uint8 rank[MAX_KEYSTONE] = {}; // per Keystone: 0 not taken, else its rank

        std::vector<SkillSpellMod> mods; // the skill nodes' spell modifiers

        bool Has(Keystone key) const { return key < MAX_KEYSTONE && rank[key] != 0; }
        uint8 Rank(Keystone key) const { return key < MAX_KEYSTONE ? rank[key] : 0; }
    };

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
