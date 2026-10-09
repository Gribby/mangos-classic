/*
 * ARPG skill trees: see ArpgTree.h.
 */

#include "Arpg/ArpgTree.h"
#include "Arpg/ArpgCombat.h"

#include "Database/DatabaseEnv.h"
#include "Entities/Player.h"
#include "Log/Log.h"
#include "Server/DBCStores.h"
#include "Server/Opcodes.h"
#include "Server/WorldPacket.h"
#include "Server/WorldSession.h"
#include "Spells/SpellMgr.h"

#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>

namespace
{
    using namespace Arpg;

    struct TreeNode
    {
        uint16 id;
        uint8 classId;
        uint8 branch;
        uint8 tier;        // 1 to 3, 4 the keystone
        uint8 column;      // 0 to 2
        TreeNodeKind kind;
        uint8 maxRank;
        char const* name;
        char const* talent;  // passive and skill: the vanilla talent, by name
        // modifier: the kit row, its count per rank
        UniqueKit kit;
        char const* spell;   // modifier: the spell it changes; keystone: its icon's spell
        uint8 n[3];
        float value;
        uint32 pct;
        Keystone key;
        char const* text;
    };

    char const* const PALADIN_BRANCHES[3] = { "Crusader", "Bulwark", "Lightbringer" };

    // The paladin tree (docs/ARPG-SKILL-TREES.md). Columns 0-2, tiers 1-3, keystone in tier 4.
    std::vector<TreeNode> const& Nodes()
    {
        static std::vector<TreeNode> const nodes =
        {
            // Crusader: two-handed melee.
            { 1,  CLASS_PALADIN, 0, 1, 0, NODE_PASSIVE,  3, "Two-Handed Specialization", "Two-Handed Weapon Specialization", KIT_ARC, nullptr, {}, 0, 0, KEY_NONE,
              "Increases the damage you deal with two-handed melee weapons by 2% per rank." },
            { 2,  CLASS_PALADIN, 0, 1, 1, NODE_PASSIVE,  5, "Benediction", "Benediction", KIT_ARC, nullptr, {}, 0, 0, KEY_NONE,
              "Reduces the mana cost of your Judgement and Seal spells by 3% per rank." },
            { 3,  CLASS_PALADIN, 0, 1, 2, NODE_MODIFIER, 1, "Sweeping Seal", nullptr, KIT_ARC, "Seal of Righteousness", { 0 }, 0.0f, 40, KEY_NONE,
              "Seal of Righteousness strikes every enemy in front of you. The extra enemies take 40% damage." },
            { 4,  CLASS_PALADIN, 0, 2, 0, NODE_PASSIVE,  5, "Conviction", "Conviction", KIT_ARC, nullptr, {}, 0, 0, KEY_NONE,
              "Increases your chance to get a critical strike with melee weapons by 1% per rank." },
            { 5,  CLASS_PALADIN, 0, 2, 1, NODE_MODIFIER, 2, "Chain of Judgement", nullptr, KIT_CHAIN, "Judgement", { 1, 2 }, 10.0f, 50, KEY_NONE,
              "Judgement chains to 1 more enemy within 10 yards per rank, for 50% damage." },
            { 6,  CLASS_PALADIN, 0, 2, 2, NODE_SKILL,    1, "Seal of Command", "Seal of Command", KIT_ARC, nullptr, {}, 0, 0, KEY_NONE,
              "Teaches Seal of Command." },
            { 7,  CLASS_PALADIN, 0, 3, 0, NODE_PASSIVE,  5, "Vengeance", "Vengeance", KIT_ARC, nullptr, {}, 0, 0, KEY_NONE,
              "After a critical strike, your physical and Holy damage rises by 3% per rank for 8 sec." },
            { 8,  CLASS_PALADIN, 0, 3, 2, NODE_MODIFIER, 1, "Commanding Sweep", nullptr, KIT_ARC, "Seal of Command", { 0 }, 0.0f, 60, KEY_NONE,
              "Seal of Command strikes every enemy in front of you for 60% damage." },
            { 9,  CLASS_PALADIN, 0, 4, 1, NODE_KEYSTONE, 1, "Avenger", nullptr, KIT_ARC, "Judgement", {}, 0, 0, KEY_AVENGER,
              "Judgement no longer consumes your Seal, but its cooldown is doubled." },

            // Bulwark: shield and control.
            { 21, CLASS_PALADIN, 1, 1, 0, NODE_PASSIVE,  5, "Toughness", "Toughness", KIT_ARC, nullptr, {}, 0, 0, KEY_NONE,
              "Increases the armour from your items by 2% per rank." },
            { 22, CLASS_PALADIN, 1, 1, 1, NODE_PASSIVE,  5, "Redoubt", "Redoubt", KIT_ARC, nullptr, {}, 0, 0, KEY_NONE,
              "After a critical strike against you, your chance to block rises for your next 5 blocks." },
            { 23, CLASS_PALADIN, 1, 1, 2, NODE_MODIFIER, 2, "Shockwave Hammer", nullptr, KIT_SPREAD, "Hammer of Justice", { 1, 2 }, 8.0f, 100, KEY_NONE,
              "Hammer of Justice also stuns 1 more enemy within 8 yards per rank." },
            { 24, CLASS_PALADIN, 1, 2, 0, NODE_PASSIVE,  2, "Improved Retribution Aura", "Improved Retribution Aura", KIT_ARC, nullptr, {}, 0, 0, KEY_NONE,
              "Increases the damage of your Retribution Aura by 25% per rank." },
            { 25, CLASS_PALADIN, 1, 2, 1, NODE_PASSIVE,  5, "Reckoning", "Reckoning", KIT_ARC, nullptr, {}, 0, 0, KEY_NONE,
              "A chance per rank after being critically hit to gain an extra attack." },
            { 26, CLASS_PALADIN, 1, 2, 2, NODE_SKILL,    1, "Holy Shield", "Holy Shield", KIT_ARC, nullptr, {}, 0, 0, KEY_NONE,
              "Teaches Holy Shield." },
            { 27, CLASS_PALADIN, 1, 3, 0, NODE_SKILL,    1, "Blessing of Sanctuary", "Blessing of Sanctuary", KIT_ARC, nullptr, {}, 0, 0, KEY_NONE,
              "Teaches Blessing of Sanctuary." },
            { 28, CLASS_PALADIN, 1, 3, 2, NODE_MODIFIER, 1, "Radiant Shield", nullptr, KIT_BURST, "Holy Shield", { 0 }, 5.0f, 50, KEY_NONE,
              "Holy Shield's damage bursts onto every enemy within 5 yards of the one it hits, for 50%." },
            { 29, CLASS_PALADIN, 1, 4, 1, NODE_KEYSTONE, 1, "Martyr's Ward", nullptr, KIT_ARC, "Retribution Aura", {}, 0, 0, KEY_MARTYRS_WARD,
              "While you are in combat, your Retribution Aura also strikes every enemy within 10 yards once a second, for double its damage." },

            // Lightbringer: holy caster.
            { 41, CLASS_PALADIN, 2, 1, 0, NODE_PASSIVE,  5, "Divine Intellect", "Divine Intellect", KIT_ARC, nullptr, {}, 0, 0, KEY_NONE,
              "Increases your Intellect by 2% per rank." },
            { 42, CLASS_PALADIN, 2, 1, 1, NODE_PASSIVE,  3, "Healing Light", "Healing Light", KIT_ARC, nullptr, {}, 0, 0, KEY_NONE,
              "Increases the healing of Holy Light and Flash of Light by 4% per rank." },
            { 43, CLASS_PALADIN, 2, 1, 2, NODE_SKILL,    1, "Consecration", "Consecration", KIT_ARC, nullptr, {}, 0, 0, KEY_NONE,
              "Teaches Consecration." },
            { 44, CLASS_PALADIN, 2, 2, 0, NODE_PASSIVE,  5, "Illumination", "Illumination", KIT_ARC, nullptr, {}, 0, 0, KEY_NONE,
              "Your healing critical strikes have a 20% chance per rank to refund their mana." },
            { 45, CLASS_PALADIN, 2, 2, 1, NODE_SKILL,    1, "Holy Shock", "Holy Shock", KIT_ARC, nullptr, {}, 0, 0, KEY_NONE,
              "Teaches Holy Shock." },
            { 46, CLASS_PALADIN, 2, 2, 2, NODE_MODIFIER, 2, "Arcing Shock", nullptr, KIT_CHAIN, "Holy Shock", { 1, 2 }, 10.0f, 60, KEY_NONE,
              "Holy Shock chains to 1 more enemy within 10 yards per rank, for 60% damage." },
            { 47, CLASS_PALADIN, 2, 3, 0, NODE_PASSIVE,  5, "Holy Power", "Holy Power", KIT_ARC, nullptr, {}, 0, 0, KEY_NONE,
              "Increases the critical strike chance of your Holy spells by 1% per rank." },
            { 48, CLASS_PALADIN, 2, 3, 2, NODE_MODIFIER, 1, "Purifying Light", nullptr, KIT_ARC, "Exorcism", {}, 0, 0, KEY_PURIFYING_LIGHT,
              "Exorcism and Holy Wrath strike every enemy, not only undead and demons." },
            { 49, CLASS_PALADIN, 2, 4, 1, NODE_KEYSTONE, 1, "Dawnbringer", nullptr, KIT_ARC, "Holy Light", {}, 0, 0, KEY_DAWNBRINGER,
              "Your heals on yourself also send a bolt of holy damage at the nearest enemy, for 50% of the amount healed." },
        };
        return nodes;
    }

    TreeNode const* FindNode(uint16 id)
    {
        for (TreeNode const& node : Nodes())
            if (node.id == id)
                return &node;
        return nullptr;
    }

    // The first spell named `name`, or 0.
    uint32 SpellByName(char const* name)
    {
        if (!name)
            return 0;
        for (uint32 id = 1; id < sSpellTemplate.GetMaxEntry(); ++id)
            if (SpellEntry const* spell = sSpellTemplate.LookupEntry<SpellEntry>(id))
                if (spell->SpellName[0] && std::strcmp(spell->SpellName[0], name) == 0)
                    return id;
        return 0;
    }

    // What the data gives each node, resolved once: a talent's rank spells, a modifier's rows
    // (one per rank), and the icon's spell.
    struct Resolved
    {
        std::vector<uint32> ranks;
        std::vector<UniqueMechanic> rows;
        uint32 icon = 0;
    };

    std::unordered_map<uint16, Resolved> const& Resolve()
    {
        static std::unordered_map<uint16, Resolved> const resolved = []()
        {
            std::unordered_map<uint16, Resolved> out;
            for (TreeNode const& node : Nodes())
            {
                Resolved& r = out[node.id];
                if (node.talent)
                {
                    uint32 const classMask = 1 << (node.classId - 1);
                    for (uint32 i = 0; i < sTalentStore.GetNumRows() && r.ranks.empty(); ++i)
                    {
                        TalentEntry const* talent = sTalentStore.LookupEntry(i);
                        TalentTabEntry const* tab = talent ? sTalentTabStore.LookupEntry(talent->TalentTab) : nullptr;
                        if (!tab || !(tab->ClassMask & classMask) || !talent->RankID[0])
                            continue;
                        SpellEntry const* first = sSpellTemplate.LookupEntry<SpellEntry>(talent->RankID[0]);
                        if (!first || !first->SpellName[0] || std::strcmp(first->SpellName[0], node.talent) != 0)
                            continue;
                        for (uint32 spell : talent->RankID)
                            if (spell)
                                r.ranks.push_back(spell);
                    }
                    if (r.ranks.empty())
                        sLog.outError("ARPG tree: node %u names talent \"%s\", which this class does not have", node.id, node.talent);
                    else
                        r.icon = r.ranks.front();
                }
                if (node.spell)
                {
                    uint32 const spell = SpellByName(node.spell);
                    if (!spell)
                        sLog.outError("ARPG tree: node %u names spell \"%s\", which does not exist", node.id, node.spell);
                    if (!r.icon)
                        r.icon = spell;
                    if (node.kind == NODE_MODIFIER && node.key == KEY_NONE && spell)
                        for (uint8 rank = 0; rank < node.maxRank; ++rank)
                            r.rows.push_back({ 0, node.kit, spell, node.n[rank], node.value, node.pct, node.text });
                }
            }
            return out;
        }();
        return resolved;
    }

    // Each player's learned nodes and ranks, by guid. World and session threads both read it.
    std::mutex sTreesLock;
    std::unordered_map<ObjectGuid, std::map<uint16, uint8>> sTrees;

    std::map<uint16, uint8> TreeOf(Player const* player)
    {
        std::lock_guard<std::mutex> guard(sTreesLock);
        auto it = sTrees.find(player->GetObjectGuid());
        return it == sTrees.end() ? std::map<uint16, uint8>() : it->second;
    }

    void EnsureTable()
    {
        static bool const created = []()
        {
            CharacterDatabase.DirectExecute(
                "CREATE TABLE IF NOT EXISTS character_arpg_tree ("
                "guid INT UNSIGNED NOT NULL, node SMALLINT UNSIGNED NOT NULL, `rank` TINYINT UNSIGNED NOT NULL, "
                "PRIMARY KEY (guid, node)) ENGINE=InnoDB DEFAULT CHARSET=utf8 COMMENT='ARPG skill tree (Arpg/ArpgTree.h)'");
            return true;
        }();
        (void)created;
    }

    uint32 PointsSpent(std::map<uint16, uint8> const& tree, int branch = -1)
    {
        uint32 spent = 0;
        for (auto const& [id, rank] : tree)
            if (TreeNode const* node = FindNode(id))
                if (branch < 0 || node->branch == branch)
                    spent += rank;
        return spent;
    }

    // Teach the spells `rank` of `node` gives, dropping lower ranks of a talent.
    void TeachRank(Player* player, TreeNode const& node, uint8 rank)
    {
        Resolved const& r = Resolve().at(node.id);
        if (r.ranks.empty() || !rank)
            return;
        uint32 const index = std::min<uint32>(rank, r.ranks.size()) - 1;
        if (node.kind == NODE_SKILL)
        {
            if (!player->HasSpell(r.ranks.front()))
                player->learnSpell(r.ranks.front(), false);
            return;
        }
        for (uint32 i = 0; i < index; ++i)
            if (player->HasSpell(r.ranks[i]))
                player->removeSpell(r.ranks[i], false, false);
        if (!player->HasSpell(r.ranks[index]))
            player->learnSpell(r.ranks[index], false);
    }

    void Save(Player* player, uint16 node, uint8 rank)
    {
        CharacterDatabase.PExecute("REPLACE INTO character_arpg_tree (guid, node, `rank`) VALUES (%u, %u, %u)",
                                   player->GetGUIDLow(), uint32(node), uint32(rank));
    }
}

namespace Arpg
{
    uint32 TreePointsFor(uint32 level)
    {
        return level > 1 ? level - 1 : 0;
    }

    void LoadTree(Player* player)
    {
        EnsureTable();
        std::map<uint16, uint8> tree;
        if (auto result = CharacterDatabase.PQuery("SELECT node, `rank` FROM character_arpg_tree WHERE guid = %u", player->GetGUIDLow()))
        {
            do
            {
                Field* fields = result->Fetch();
                uint16 const node = uint16(fields[0].GetUInt32());
                uint8 const rank = uint8(fields[1].GetUInt32());
                if (TreeNode const* known = FindNode(node))
                    if (known->classId == player->getClass() && rank)
                        tree[node] = std::min(rank, known->maxRank);
            }
            while (result->NextRow());
        }
        std::lock_guard<std::mutex> guard(sTreesLock);
        sTrees[player->GetObjectGuid()] = std::move(tree);
    }

    void UnloadTree(Player* player)
    {
        std::lock_guard<std::mutex> guard(sTreesLock);
        sTrees.erase(player->GetObjectGuid());
    }

    bool TreeOwnsSpell(Player const* player, uint32 spell)
    {
        std::map<uint16, uint8> const tree = TreeOf(player);
        if (tree.empty())
            return false;
        for (auto const& [id, rank] : tree)
        {
            auto it = Resolve().find(id);
            if (it == Resolve().end())
                continue;
            for (uint32 known : it->second.ranks)
                if (known == spell)
                    return true;
        }
        return false;
    }

    void OnTreeHello(Player* player)
    {
        // Vanilla talents give way to the tree, once: a fresh ARPG character spent none.
        if (player->CalculateTalentsPoints() > player->GetFreeTalentPoints())
        {
            player->resetTalents(true);
            sLog.outString("ARPG tree: %s's vanilla talents were reset for the ARPG tree", player->GetName());
        }
        for (auto const& [id, rank] : TreeOf(player))
            if (TreeNode const* node = FindNode(id))
                TeachRank(player, *node, rank);
        SendTree(player);
    }

    void SpendNode(Player* player, uint16 id)
    {
        TreeNode const* node = FindNode(id);
        std::map<uint16, uint8> tree = TreeOf(player);
        auto refuse = [&](char const* why)
        {
            sLog.outDetail("ARPG tree: %s cannot spend in node %u: %s", player->GetName(), uint32(id), why);
            SendTree(player);
        };
        if (!node || node->classId != player->getClass())
            return refuse("not this class's node");
        uint8 const rank = tree.count(id) ? tree[id] : 0;
        if (rank >= node->maxRank)
            return refuse("already at its top rank");
        if (PointsSpent(tree) >= TreePointsFor(player->GetLevel()))
            return refuse("no points left");
        if (PointsSpent(tree, node->branch) < TIER_GATE[node->tier])
            return refuse("tier not open yet");
        // A talent's next rank (or a taught spell) waits for the level vanilla gives it at.
        Resolved const& r = Resolve().at(id);
        if (!r.ranks.empty())
        {
            uint32 const next = r.ranks[std::min<uint32>(rank, r.ranks.size() - 1)];
            if (SpellEntry const* spell = sSpellTemplate.LookupEntry<SpellEntry>(next))
                if (spell->spellLevel > player->GetLevel())
                    return refuse("the spell needs a higher level");
        }
        {
            std::lock_guard<std::mutex> guard(sTreesLock);
            sTrees[player->GetObjectGuid()][id] = rank + 1;
        }
        TeachRank(player, *node, rank + 1);
        Save(player, id, rank + 1);
        SendTree(player);
    }

    void Respec(Player* player)
    {
        if (player->IsInCombat())
        {
            SendTree(player);
            return;
        }
        std::map<uint16, uint8> const tree = TreeOf(player);
        // Unlearn while the tree still owns the spells, so no talent accounting runs.
        for (auto const& [id, rank] : tree)
        {
            auto it = Resolve().find(id);
            if (it == Resolve().end())
                continue;
            for (uint32 spell : it->second.ranks)
                if (player->HasSpell(spell))
                    player->removeSpell(spell, false, false);
        }
        {
            std::lock_guard<std::mutex> guard(sTreesLock);
            sTrees[player->GetObjectGuid()].clear();
        }
        CharacterDatabase.PExecute("DELETE FROM character_arpg_tree WHERE guid = %u", player->GetGUIDLow());
        SendTree(player);
    }

    void SendTree(Player* player)
    {
        std::map<uint16, uint8> const tree = TreeOf(player);
        std::vector<TreeNode const*> mine;
        for (TreeNode const& node : Nodes())
            if (node.classId == player->getClass())
                mine.push_back(&node);
        uint8 const branches = mine.empty() ? 0 : 3;

        WorldPacket data(SMSG_ARPG_TREE, 8 + mine.size() * 96);
        data << uint8(1);
        data << uint16(TreePointsFor(player->GetLevel()));
        data << uint16(PointsSpent(tree));
        data << uint8(branches);
        for (uint8 b = 0; b < branches; ++b)
            data << PALADIN_BRANCHES[b];
        data << uint8(mine.size());
        for (TreeNode const* node : mine)
        {
            auto it = tree.find(node->id);
            data << uint16(node->id);
            data << uint8(node->branch);
            data << uint8(node->tier);
            data << uint8(node->column);
            data << uint8(node->kind);
            data << uint8(node->maxRank);
            data << uint8(it == tree.end() ? 0 : it->second);
            data << uint32(Resolve().at(node->id).icon);
            data << node->name;
            data << node->text;
        }
        player->GetSession()->SendPacket(data);
    }

    std::vector<UniqueMechanic const*> LearnedModifiers(Player const* player)
    {
        std::vector<UniqueMechanic const*> rows;
        std::map<uint16, uint8> const tree = TreeOf(player);
        for (auto const& [id, rank] : tree)
        {
            auto it = Resolve().find(id);
            if (it == Resolve().end() || it->second.rows.empty() || !rank)
                continue;
            rows.push_back(&it->second.rows[std::min<size_t>(rank, it->second.rows.size()) - 1]);
        }
        return rows;
    }

    bool HasKeystone(Unit const* unit, Keystone key)
    {
        if (!Active(unit))
            return false;
        for (auto const& [id, rank] : TreeOf(static_cast<Player const*>(unit)))
            if (TreeNode const* node = FindNode(id))
                if (node->key == key && rank)
                    return true;
        return false;
    }
}
