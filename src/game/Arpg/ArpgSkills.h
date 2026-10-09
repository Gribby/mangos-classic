/*
 * ARPG skill specialisation, for the benilla ARPG client (the client's docs/ARPG-CHARACTER.md,
 * "Skills and skill trees").
 *
 * A character specialises up to five skills, one per slot; the slots open at levels 1, 10, 20,
 * 30 and 40. Skill points come two a level from level 2 to 51 (100 in all) and go into any
 * specialised skill's tree, at most SKILL_CAP in one skill. Taking a skill out of its slot gives
 * its points back; respec is free, out of combat.
 *
 * Each skill's tree is a small graph rooted at the skill: three branches of three or four nodes,
 * each node needing a rank in the one above it. A node is one of:
 *   SKILL_NODE_MODIFIER     numbers: a spell modifier (the client is sent it) or a damage bonus
 *   SKILL_NODE_TRANSFORMER  changes how the skill works: a uniques kit row, or a hook
 *   SKILL_NODE_SYNERGY      works with another skill or a state (stunned, consecrated)
 *   SKILL_NODE_CAPSTONE     a rule change at the end of a branch (Codex pages will seal them)
 * A node's effect is one of: a spell modifier over the skill's spells (SkillSpellMod), a kit
 * row (ArpgUniques.h), or a Keystone with a rank that a hook reads (ArpgCharacter.h). They add
 * into the player's WebTotals beside the passive web's.
 *
 * Which spells a skill covers is by name (Seals: every "Seal of …", their procs included;
 * Judgement: every "Judgement…"); Strike is the held swing, no spell at all.
 *
 * Saved in the characters database, created if missing:
 *   character_arpg_skill       (guid, slot, skill)
 *   character_arpg_skill_node  (guid, node, rank); a node's id is its skill * 100 + its number
 *
 *   CMSG_ARPG_ACTION kind 12 slot: uint8 slot, uint8 skill (0 clears the slot)
 *                    kind 13 take a rank: uint16 node
 *                    kind 14 give a rank back: uint16 node
 *                    kind 15 respec a skill: uint8 skill
 *   SMSG_ARPG_SKILLS (0x340): uint8 version (1), uint16 points total, uint16 points spent,
 *     uint8 level; uint8 slot count, per slot: uint8 level it opens at, uint8 skill (0: empty);
 *     uint8 skill count, per skill: uint8 id, uint32 icon spell, cstring name, cstring text,
 *     uint8 points in it, uint8 cap, uint8 branch count and a cstring each, uint8 node count, per
 *     node: uint16 id, uint8 kind, uint8 column, uint8 row, uint16 parent (0: the root), uint8 max
 *     rank, uint8 rank, uint32 icon spell, cstring name, cstring text.
 */

#ifndef MANGOS_ARPG_SKILLS_H
#define MANGOS_ARPG_SKILLS_H

#include "Common.h"
#include "Arpg/ArpgTree.h"
#include "Arpg/ArpgUniques.h"

#include <vector>

class Player;
struct SpellEntry;

namespace Arpg
{
    enum SkillId : uint8
    {
        SKILL_NONE         = 0,
        SKILL_STRIKE       = 1,
        SKILL_SEALS        = 2,
        SKILL_JUDGEMENT    = 3,
        SKILL_CONSECRATION = 4,
        SKILL_HAMMER       = 5,
    };

    enum SkillNodeKind : uint8
    {
        SKILL_NODE_ROOT        = 0,
        SKILL_NODE_MODIFIER    = 1,
        SKILL_NODE_TRANSFORMER = 2,
        SKILL_NODE_SYNERGY     = 3,
        SKILL_NODE_CAPSTONE    = 4,
    };

    constexpr uint8 SKILL_SLOTS = 5;
    constexpr uint32 SLOT_LEVEL[SKILL_SLOTS] = { 1, 10, 20, 30, 40 };
    constexpr uint8 SKILL_CAP = 20;

    // Skill points a character of `level` has: two a level from 2 to 51.
    uint32 SkillPointsFor(uint32 level);

    // The skill `spellInfo` belongs to for `classId` (nullptr: the swing, Strike); SKILL_NONE for none.
    SkillId SkillOfSpell(uint8 classId, SpellEntry const* spellInfo);

    // Load the player's skills from the database (at login).
    void LoadSkills(Player* player);

    // Forget a logged-out player's skills.
    void UnloadSkills(Player* player);

    // Send the player's skills (SMSG_ARPG_SKILLS).
    void SendSkills(Player* player);

    // Put `skill` in `slot` (SKILL_NONE: empty it); a skill leaving its slot gives its points back.
    void SlotSkill(Player* player, uint8 slot, uint8 skill);

    // Take a rank of `node`, or give one back.
    void SpendSkillNode(Player* player, uint16 node);
    void RefundSkillNode(Player* player, uint16 node);

    // Give back every point in `skill`.
    void RespecSkill(Player* player, uint8 skill);

    // Add what the player's skill nodes give to `totals`.
    void AddSkillTotals(Player const* player, WebTotals& totals);

    // The uniques-kit rows the player's skill nodes give.
    std::vector<UniqueMechanic const*> SkillModifiers(Player const* player);
}

#endif
