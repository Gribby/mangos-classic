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
 * Codex pages and runes: each capstone is sealed until its Codex page is read: a book its home
 * boss drops (PAGES), read at once when picked up. Codex fragments (champions, rares, Wardens,
 * Caches) unseal any capstone of a specialised skill, FRAGMENTS_PER_PAGE at a time; a page
 * already read tears into FRAGMENTS_FOR_DUPLICATE. A capstone taken before pages sealed them
 * counts as read. Each skill's tree has one rune socket, open with 10 points in the skill; a
 * rune (Runes()) fits some skills and adds a kit row, spell modifiers, a keystone rank or a hook
 * (Leech) to the skill it is in. Socketing is free, the rune stays held, and unslotting the
 * skill empties its socket. Saved in character_arpg_codex (guid, page), character_arpg_held
 * (guid, thing: 0 fragments, else a rune; count) and character_arpg_socket (guid, skill, rune).
 *
 *   SMSG_ARPG_SKILLS (0x340): uint8 version (2), uint16 points total, uint16 points spent,
 *     uint8 level; uint8 slot count, per slot: uint8 level it opens at, uint8 skill (0: empty);
 *     uint8 skill count, per skill: uint8 id, uint32 icon spell, cstring name, cstring text,
 *     uint8 points in it, uint8 cap, uint8 socketed rune (0 none), uint8 points the socket opens
 *     at, uint8 branch count and a cstring each, uint8 node count, per node: uint16 id, uint8
 *     kind, uint8 column, uint8 row, uint16 parent (0: the root), uint8 max rank, uint8 rank,
 *     uint32 icon spell, cstring name, cstring text, uint8 sealed, cstring the page's home ("" for
 *     none); then uint16 fragments held, uint8 fragments a page takes, uint8 rune count, per rune:
 *     uint8 id, uint32 icon spell, cstring name, cstring text, uint8 held, uint8 the skills it
 *     fits (bit per skill id).
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
        SKILL_EXORCISM     = 6,
        SKILL_HOLY_WRATH   = 7,
        SKILL_HAMMER_OF_WRATH = 8,
    };

    enum SkillNodeKind : uint8
    {
        SKILL_NODE_ROOT        = 0,
        SKILL_NODE_MODIFIER    = 1,
        SKILL_NODE_TRANSFORMER = 2,
        SKILL_NODE_SYNERGY     = 3,
        SKILL_NODE_CAPSTONE    = 4,
    };

    // Runes, one per skill tree's socket (docs/ARPG-CHARACTER.md, "Runes").
    enum RuneId : uint8
    {
        RUNE_NONE       = 0,
        RUNE_CHAINS     = 1,
        RUNE_SHATTERING = 2,
        RUNE_EXPANSE    = 3,
        RUNE_LINGERING  = 4,
        RUNE_HASTE      = 5,
        RUNE_LEECH      = 6,
        RUNE_COMMAND    = 7,
        RUNE_SANCTITY   = 8,
        RUNE_FURY       = 9,
    };

    // Codex fragments that make one page, and what a page already read tears into.
    constexpr uint8 FRAGMENTS_PER_PAGE = 5;
    constexpr uint8 FRAGMENTS_FOR_DUPLICATE = 2;

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

    // The uniques-kit rows the player's skill nodes and socketed runes give.
    std::vector<UniqueMechanic const*> SkillModifiers(Player const* player);

    // Whether `player` has `skill` in a slot.
    bool IsSpecialised(Player const* player, SkillId skill);

    // Whether `rune` is socketed, and in effect, in `player`'s `skill`.
    bool HasRune(Player const* player, SkillId skill, uint8 rune);

    // The capstones whose Codex page `bossEntry` drops; every page; every rune.
    std::vector<uint16> PagesFrom(uint32 bossEntry);
    std::vector<uint16> AllPages();
    std::vector<uint8> AllRunes();
    char const* PageName(uint16 node);
    char const* RuneName(uint8 rune);
    char const* RuneText(uint8 rune);

    // Picked up: a Codex page (read at once; one already read tears into fragments), fragments,
    // a rune (held, for any socket).
    void ReadPage(Player* player, uint16 node);
    void AddFragments(Player* player, uint32 count);
    void AddRune(Player* player, uint8 rune);

    // Other things a character holds outside its bags, from HELD_OTHER up (the raid keys' pieces,
    // Arpg/ArpgCodex.h): how many, and add `delta` (returns the new count). Saved with the runes.
    constexpr uint16 HELD_OTHER = 100;
    uint32 Held(Player const* player, uint16 thing);
    uint32 AddHeld(Player* player, uint16 thing, int32 delta);

    // Unseal capstone `node` of a specialised skill with FRAGMENTS_PER_PAGE fragments
    // (CMSG_ARPG_ACTION kind 20, uint16 node).
    void UnsealWithFragments(Player* player, uint16 node);

    // Socket `rune` (0: empty the socket) in `skill`, once it has 10 points (kind 21, uint8
    // skill, uint8 rune). Socketing is free and the rune stays held.
    void SocketRune(Player* player, uint8 skill, uint8 rune);
}

#endif
