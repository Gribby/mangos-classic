/*
 * ARPG skill trees, for the benilla ARPG client (see ArpgCombat.h and the client's
 * docs/ARPG-SKILL-TREES.md).
 *
 * An ARPG player spends points in a tree per class in place of vanilla's talents: one point per
 * level past the first, respec free (out of combat). Each node is one of:
 *   NODE_PASSIVE   ranks of a vanilla talent: rank N teaches that talent's rank N spell
 *   NODE_SKILL     a spell vanilla gates behind a talent: teaches its rank 1
 *   NODE_MODIFIER  a uniques kit row on a spell (ArpgUniques.h), its count rising with the rank
 *   NODE_KEYSTONE  a capstone with its own hook (Keystone)
 * Any node may also carry a Keystone flag of its own (Purifying Light is a modifier in name).
 * A branch's tiers open at TIER_GATE points spent in that branch.
 *
 * Talents are found by name (the talent's rank 1 spell name, within the class's tabs), so the
 * table names "Conviction" rather than spell ids. Spells the tree teaches are kept out of
 * vanilla's talent accounting (Player::addSpell / removeSpell), so a level-up never resets them.
 * A character with tree points spent has its vanilla talents reset once, at its first ARPG hello,
 * and an ARPG player's talent window learns nothing.
 *
 * Saved in the characters database, `character_arpg_tree` (guid, node, rank), which the server
 * creates if missing.
 *
 *   CMSG_ARPG_ACTION kind 8  spend: uint16 node
 *                    kind 9  respec
 *                    kind 10 tree query
 *   SMSG_ARPG_TREE (0x33F): uint8 version (1), uint16 points total, uint16 points spent,
 *     uint8 branch count, a cstring name per branch, uint8 node count, then per node: uint16 id,
 *     uint8 branch, uint8 tier, uint8 column, uint8 kind, uint8 max rank, uint8 rank,
 *     uint32 icon spell, cstring name, cstring text.
 */

#ifndef MANGOS_ARPG_TREE_H
#define MANGOS_ARPG_TREE_H

#include "Common.h"
#include "Arpg/ArpgUniques.h"

#include <vector>

class Player;
class Unit;
struct SpellEntry;

namespace Arpg
{
    enum TreeNodeKind : uint8
    {
        NODE_PASSIVE  = 0,
        NODE_SKILL    = 1,
        NODE_MODIFIER = 2,
        NODE_KEYSTONE = 3,
    };

    enum Keystone : uint8
    {
        KEY_NONE            = 0,
        KEY_AVENGER         = 1, // Judgement keeps the Seal; its cooldown doubles
        KEY_MARTYRS_WARD    = 2, // Retribution Aura burns every enemy within 10 yd each second, doubled
        KEY_DAWNBRINGER     = 3, // heals on yourself send half their amount as holy damage
        KEY_PURIFYING_LIGHT = 4, // Exorcism and Holy Wrath strike any enemy
    };

    // Points a tier needs spent in its branch: tier 1 none, then 5, 10, and 20 for the keystone.
    constexpr uint32 TIER_GATE[5] = { 0, 0, 5, 10, 20 };

    // Tree points a player of `level` has: one per level past the first.
    uint32 TreePointsFor(uint32 level);

    // Load the player's tree from the database (at login, before their spells load).
    void LoadTree(Player* player);

    // Forget a logged-out player's tree.
    void UnloadTree(Player* player);

    // At the ARPG hello: reset vanilla talents once, teach what the tree holds, send the tree.
    void OnTreeHello(Player* player);

    // Whether `spell` is one the player's tree taught (kept out of talent accounting).
    bool TreeOwnsSpell(Player const* player, uint32 spell);

    // Spend a point in `node`, if the rules allow; answers with the tree.
    void SpendNode(Player* player, uint16 node);

    // Refund every point (free, out of combat); answers with the tree.
    void Respec(Player* player);

    // Send the player's tree (SMSG_ARPG_TREE).
    void SendTree(Player* player);

    // The uniques-kit rows the player's learned modifier nodes give.
    std::vector<UniqueMechanic const*> LearnedModifiers(Player const* player);

    // Whether the player has learned a node carrying `key`.
    bool HasKeystone(Unit const* unit, Keystone key);
}

#endif
