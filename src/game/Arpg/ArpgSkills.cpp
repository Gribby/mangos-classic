/*
 * ARPG skill specialisation: see ArpgSkills.h.
 */

#include "Arpg/ArpgSkills.h"
#include "Arpg/ArpgCombat.h"

#include "Database/DatabaseEnv.h"
#include "Entities/Player.h"
#include "Log/Log.h"
#include "Server/DBCStores.h"
#include "Server/Opcodes.h"
#include "Server/WorldPacket.h"
#include "Server/WorldSession.h"
#include "Spells/SpellMgr.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <tuple>
#include <unordered_map>

namespace
{
    using namespace Arpg;

    // A node's effect: a Keystone a hook reads at the node's rank, a spell modifier per rank over
    // the skill's spells, or a kit row per rank.
    struct SkillNode
    {
        uint16 id;
        SkillNodeKind kind;
        uint8 column;
        uint8 row;
        uint16 parent;            // 0: the root
        uint8 max;
        char const* name;
        char const* text;
        uint32 icon;
        Keystone key = KEY_NONE;
        int8 modOp = -1;          // SpellModOp, -1 for none
        uint8 modType = SPELLMOD_FLAT;
        int32 modPerRank = 0;
        UniqueKit kit = UniqueKit(0);
        uint32 kitSpell = 0;      // rank 1 of the spell the kit row changes
        uint8 n[4] = {};
        float value[4] = {};
        uint32 pct[4] = {};
    };

    struct SkillDef
    {
        SkillId id;
        uint8 classId;
        char const* name;
        char const* text;
        uint32 icon;
        std::vector<char const*> spells;  // name prefixes, matched at a word boundary
        std::vector<char const*> branches;
        std::vector<SkillNode> nodes;
    };

    SkillNode Key(uint16 id, SkillNodeKind kind, uint8 col, uint8 row, uint16 parent, uint8 max, char const* name,
                  char const* text, uint32 icon, Keystone key)
    {
        SkillNode n{ id, kind, col, row, parent, max, name, text, icon };
        n.key = key;
        return n;
    }

    SkillNode Mod(uint16 id, uint8 col, uint8 row, uint16 parent, uint8 max, char const* name, char const* text,
                  uint32 icon, SpellModOp op, SpellModType type, int32 perRank)
    {
        SkillNode n{ id, SKILL_NODE_MODIFIER, col, row, parent, max, name, text, icon };
        n.modOp = int8(op);
        n.modType = uint8(type);
        n.modPerRank = perRank;
        return n;
    }

    SkillNode Kit(uint16 id, uint8 col, uint8 row, uint16 parent, uint8 max, char const* name, char const* text,
                  uint32 icon, UniqueKit kit, uint32 spell, std::initializer_list<uint8> n,
                  std::initializer_list<float> value, std::initializer_list<uint32> pct)
    {
        SkillNode node{ id, SKILL_NODE_TRANSFORMER, col, row, parent, max, name, text, icon };
        node.kit = kit;
        node.kitSpell = spell;
        uint8 i = 0;
        for (uint8 v : n) node.n[i++] = v;
        i = 0;
        for (float v : value) node.value[i++] = v;
        i = 0;
        for (uint32 v : pct) node.pct[i++] = v;
        return node;
    }

    // The paladin's specialisable skills (docs/ARPG-CHARACTER.md, "The paladin, in full").
    std::vector<SkillDef> const& Skills()
    {
        static std::vector<SkillDef> const skills =
        {
            { SKILL_STRIKE, CLASS_PALADIN, "Strike", "Your held swing: the left click.", 6603, {},
              { "Reach", "Rhythm", "Weight" },
              {
                  Key(101, SKILL_NODE_TRANSFORMER, 0, 1, 0, 3, "Wide Swing", "Your swings also strike every other enemy in front of you, for 20/35/50% of the hit.", 845, KEY_WIDE_SWING),
                  Key(102, SKILL_NODE_MODIFIER, 0, 2, 101, 2, "Long Arm", "+1 yard melee reach per rank.", 12292, KEY_LONG_ARM),
                  Key(103, SKILL_NODE_CAPSTONE, 0, 3, 102, 1, "Whirling Strikes", "Every 4th swing also strikes every enemy around you, for 75% of the hit.", 1680, KEY_WHIRLING_STRIKES),
                  Key(104, SKILL_NODE_MODIFIER, 1, 1, 0, 4, "Quickened", "+4% attack speed per rank.", 19740, KEY_QUICKENED),
                  Key(105, SKILL_NODE_MODIFIER, 1, 2, 104, 3, "Momentum", "Each swing that hits within 3 sec of the last gives +1% swing damage per rank, up to 5 hits.", 12294, KEY_MOMENTUM),
                  Key(106, SKILL_NODE_CAPSTONE, 1, 3, 105, 1, "Crusader's Pace", "Your kills give +30% movement speed for 3 sec.", 2983, KEY_CRUSADERS_PACE),
                  Key(107, SKILL_NODE_MODIFIER, 2, 1, 0, 4, "Heavy Hand", "+5% Physical damage per rank while you wield a two-handed weapon.", 20111, KEY_HEAVY_HAND),
                  Key(108, SKILL_NODE_TRANSFORMER, 2, 2, 107, 2, "Stagger", "Your swings have a 10% chance per rank to daze the enemy.", 1715, KEY_STAGGER),
                  Key(109, SKILL_NODE_CAPSTONE, 2, 3, 108, 1, "Shockwave", "Your swing critical strikes send a shockwave 10 yards ahead, for 40% of the hit.", 20549, KEY_SHOCKWAVE_STRIKE),
              } },
            { SKILL_SEALS, CLASS_PALADIN, "Seals", "Your Seals, as one skill: they are the swing's damage.", 20154, { "Seal of" },
              { "Righteous", "Command", "Crusader" },
              {
                  Kit(201, 0, 1, 0, 3, "Sweeping Seal", "Seal of Righteousness strikes every enemy in front of you; the extras take 20/35/50%.", 20154,
                      KIT_ARC, 20154, { 0, 0, 0 }, { 0, 0, 0 }, { 20, 35, 50 }),
                  Key(202, SKILL_NODE_MODIFIER, 0, 2, 201, 4, "Holy Edge", "+6% Seal damage per rank.", 20101, KEY_HOLY_EDGE),
                  Key(203, SKILL_NODE_CAPSTONE, 0, 3, 202, 1, "Twin Seals", "Two different Seals can be on you at once.", 20164, KEY_TWIN_SEALS),
                  Key(204, SKILL_NODE_MODIFIER, 1, 1, 0, 3, "Commanding Seal", "+10% Seal of Command damage per rank.", 20375, KEY_COMMANDING_SEAL),
                  Kit(205, 1, 2, 204, 2, "Command Arc", "Seal of Command strikes every enemy in front of you, at 40/60%.", 20467,
                      KIT_ARC, 20375, { 0, 0 }, { 0, 0 }, { 40, 60 }),
                  Key(206, SKILL_NODE_MODIFIER, 1, 3, 205, 3, "Relentless", "Seal of Command strikes 1 more time a minute per rank.", 20424, KEY_RELENTLESS),
                  Key(207, SKILL_NODE_MODIFIER, 2, 1, 0, 3, "Zeal", "+10% attack speed per rank while Seal of the Crusader is on you.", 21082, KEY_ZEAL),
                  Key(208, SKILL_NODE_SYNERGY, 2, 2, 207, 3, "Mana Strike", "Your Seal hits give you 1% of your maximum mana per rank.", 19742, KEY_MANA_STRIKE),
                  Key(209, SKILL_NODE_CAPSTONE, 2, 3, 208, 1, "Light of the Crusader", "Your Seal hits heal you for 5% of their damage.", 19977, KEY_LIGHT_OF_THE_CRUSADER),
              } },
            { SKILL_JUDGEMENT, CLASS_PALADIN, "Judgement", "Unleash your Seal on an enemy.", 20271, { "Judgement" },
              { "Chain", "Burst", "Seal" },
              {
                  Kit(301, 0, 1, 0, 3, "Chain of Judgement", "Judgement chains to 1/2/3 more enemies within 10 yards, at 50%.", 20186,
                      KIT_CHAIN, 20271, { 1, 2, 3 }, { 10, 10, 10 }, { 50, 50, 50 }),
                  Key(302, SKILL_NODE_MODIFIER, 0, 2, 301, 3, "Echoing Verdict", "Judgement's chain jumps deal 10% more per rank.", 20185, KEY_ECHOING_VERDICT),
                  Key(303, SKILL_NODE_CAPSTONE, 0, 3, 302, 1, "Final Verdict", "A kill with Judgement readies Judgement at once.", 20184, KEY_FINAL_VERDICT),
                  Kit(304, 1, 1, 0, 3, "Hammer of Light", "Judgement bursts onto enemies within 4/6/8 yards of its target, at 40%.", 24275,
                      KIT_BURST, 20271, { 0, 0, 0 }, { 4, 6, 8 }, { 40, 40, 40 }),
                  Key(305, SKILL_NODE_MODIFIER, 1, 2, 304, 4, "Radiance", "+8% Judgement damage per rank.", 20218, KEY_RADIANCE),
                  Key(306, SKILL_NODE_CAPSTONE, 1, 3, 305, 1, "Sentence", "Judgement deals double damage to stunned enemies.", 879, KEY_SENTENCE),
                  Key(307, SKILL_NODE_SYNERGY, 2, 1, 0, 1, "Avenger", "Judgement no longer uses up your Seal, but its cooldown is doubled.", 25780, KEY_AVENGER),
                  Mod(308, 2, 2, 307, 3, "Swift Judgement", "-1 sec Judgement cooldown per rank.", 1044, SPELLMOD_COOLDOWN, SPELLMOD_FLAT, -1000),
                  Key(309, SKILL_NODE_MODIFIER, 2, 3, 308, 2, "Righteous Mind", "Judgement restores 5% of your maximum mana per rank.", 20166, KEY_RIGHTEOUS_MIND),
              } },
            { SKILL_CONSECRATION, CLASS_PALADIN, "Consecration", "Consecrate the ground beneath you.", 26573, { "Consecration" },
              { "Ground", "Smite", "Refuge" },
              {
                  Mod(401, 0, 1, 0, 4, "Wide Blessing", "+10% Consecration radius per rank. Intellect widens it too.", 2812, SPELLMOD_RADIUS, SPELLMOD_PCT, 10),
                  Mod(402, 0, 2, 401, 3, "Lasting Ground", "+2 sec Consecration duration per rank.", 20925, SPELLMOD_DURATION, SPELLMOD_FLAT, 2000),
                  Key(403, SKILL_NODE_CAPSTONE, 0, 3, 402, 1, "Walking Consecration", "Your Consecration follows you.", 1044, KEY_WALKING_CONSECRATION),
                  Key(404, SKILL_NODE_MODIFIER, 1, 1, 0, 4, "Burning Ground", "+8% Consecration damage per rank.", 26573, KEY_BURNING_GROUND),
                  Key(405, SKILL_NODE_SYNERGY, 1, 2, 404, 2, "Searing Light", "Enemies standing in your Consecration take +5% Holy damage from you per rank.", 20218, KEY_SEARING_LIGHT),
                  Key(406, SKILL_NODE_SYNERGY, 1, 3, 405, 1, "Sanctified", "Your Judgement on an enemy in your Consecration bursts onto enemies within 8 yards, at 50%.", 20271, KEY_SANCTIFIED),
                  Key(407, SKILL_NODE_MODIFIER, 2, 1, 0, 3, "Hallowed Ground", "While you stand in your Consecration, you regain 1% of your health a second per rank.", 633, KEY_HALLOWED_GROUND),
                  Key(408, SKILL_NODE_SYNERGY, 2, 2, 407, 1, "Steadfast", "While you stand in your Consecration, you can't be stunned.", 498, KEY_STEADFAST),
                  Key(409, SKILL_NODE_CAPSTONE, 2, 3, 408, 1, "Sacred Seal", "Your Seals strike twice on enemies in your Consecration.", 20154, KEY_SACRED_SEAL),
              } },
            { SKILL_HAMMER, CLASS_PALADIN, "Hammer of Justice", "Stun an enemy.", 853, { "Hammer of Justice" },
              { "Thrown", "Control", "Holy" },
              {
                  Mod(501, 0, 1, 0, 1, "Hurled Hammer", "Hammer of Justice flies 10 yards farther along your aim.", 24275, SPELLMOD_RANGE, SPELLMOD_FLAT, 10),
                  Key(502, SKILL_NODE_TRANSFORMER, 0, 2, 501, 3, "Ricochet", "Hammer of Justice bounces on to 1 more enemy within 10 yards per rank.", 10308, KEY_RICOCHET),
                  Key(503, SKILL_NODE_CAPSTONE, 0, 3, 502, 1, "Blessed Hammer", "Hammer of Justice also hurls a blessed hammer at each of the three nearest enemies within 15 yards, for Holy damage of half your attack power.", 19752, KEY_BLESSED_HAMMER),
                  Kit(504, 1, 1, 0, 2, "Shockwave Hammer", "Hammer of Justice also stuns 1/2 more enemies within 8 yards of its target.", 20549,
                      KIT_SPREAD, 853, { 1, 2 }, { 8, 8 }, { 100, 100 }),
                  Mod(505, 1, 2, 504, 3, "Long Stun", "+0.5 sec stun per rank.", 5588, SPELLMOD_DURATION, SPELLMOD_FLAT, 500),
                  Mod(506, 1, 3, 505, 3, "Quick Hammer", "-10 sec Hammer of Justice cooldown per rank.", 5589, SPELLMOD_COOLDOWN, SPELLMOD_FLAT, -10000),
                  Key(507, SKILL_NODE_MODIFIER, 2, 1, 0, 3, "Holy Hammer", "Hammer of Justice also deals Holy damage of 30/45/60% of your attack power.", 20218, KEY_HOLY_HAMMER),
                  Key(508, SKILL_NODE_CAPSTONE, 2, 2, 507, 1, "Sentence Passed", "Stunned enemies take +15% damage from you.", 879, KEY_SENTENCE_PASSED),
              } },
        };
        return skills;
    }

    SkillDef const* FindSkill(uint8 classId, uint8 id)
    {
        for (SkillDef const& skill : Skills())
            if (skill.id == id && skill.classId == classId)
                return &skill;
        return nullptr;
    }

    SkillNode const* FindNode(uint8 classId, uint16 id, SkillDef const** owner = nullptr)
    {
        SkillDef const* skill = FindSkill(classId, uint8(id / 100));
        if (!skill)
            return nullptr;
        for (SkillNode const& node : skill->nodes)
            if (node.id == id)
            {
                if (owner)
                    *owner = skill;
                return &node;
            }
        return nullptr;
    }

    bool NameMatches(char const* have, char const* want)
    {
        size_t const n = std::strlen(want);
        return std::strncmp(have, want, n) == 0 && (have[n] == '\0' || have[n] == ' ');
    }

    // The family flags of the class's spells a skill covers, for its spell modifiers.
    uint64 SkillMask(SkillDef const& skill)
    {
        static std::mutex lock;
        static std::map<uint8, uint64> cache;
        std::lock_guard<std::mutex> guard(lock);
        auto it = cache.find(skill.id);
        if (it != cache.end())
            return it->second;
        uint64 mask = 0;
        for (uint32 id = 1; id < sSpellTemplate.GetMaxEntry(); ++id)
        {
            SpellEntry const* spell = sSpellTemplate.LookupEntry<SpellEntry>(id);
            if (!spell || spell->SpellFamilyName != SPELLFAMILY_PALADIN || !spell->SpellName[0])
                continue;
            for (char const* want : skill.spells)
                if (NameMatches(spell->SpellName[0], want))
                    mask |= spell->SpellFamilyFlags.Flags;
        }
        if (!mask)
            sLog.outError("ARPG skills: no spell of %s has family flags; its spell modifiers do nothing", skill.name);
        cache[skill.id] = mask;
        return mask;
    }

    // --- Each player's skills ---

    struct PlayerSkills
    {
        uint8 slots[SKILL_SLOTS] = {};
        std::map<uint16, uint8> ranks;
    };

    std::mutex sSkillsLock;
    std::unordered_map<ObjectGuid, PlayerSkills> sSkills;

    PlayerSkills SkillsFor(Player const* player)
    {
        std::lock_guard<std::mutex> guard(sSkillsLock);
        auto it = sSkills.find(player->GetObjectGuid());
        return it == sSkills.end() ? PlayerSkills() : it->second;
    }

    template <class F> void Edit(Player const* player, F f)
    {
        std::lock_guard<std::mutex> guard(sSkillsLock);
        f(sSkills[player->GetObjectGuid()]);
    }

    void EnsureTables()
    {
        static bool const created = []()
        {
            CharacterDatabase.DirectExecute(
                "CREATE TABLE IF NOT EXISTS character_arpg_skill ("
                "guid INT UNSIGNED NOT NULL, slot TINYINT UNSIGNED NOT NULL, skill TINYINT UNSIGNED NOT NULL, "
                "PRIMARY KEY (guid, slot)) ENGINE=InnoDB DEFAULT CHARSET=utf8 COMMENT='ARPG specialised skills (Arpg/ArpgSkills.h)'");
            CharacterDatabase.DirectExecute(
                "CREATE TABLE IF NOT EXISTS character_arpg_skill_node ("
                "guid INT UNSIGNED NOT NULL, node SMALLINT UNSIGNED NOT NULL, `rank` TINYINT UNSIGNED NOT NULL, "
                "PRIMARY KEY (guid, node)) ENGINE=InnoDB DEFAULT CHARSET=utf8 COMMENT='ARPG skill tree ranks (Arpg/ArpgSkills.h)'");
            return true;
        }();
        (void)created;
    }

    uint32 SpentIn(PlayerSkills const& s, int skill = -1)
    {
        uint32 spent = 0;
        for (auto const& [id, rank] : s.ranks)
            if (skill < 0 || id / 100 == skill)
                spent += rank;
        return spent;
    }

    int SlotOf(PlayerSkills const& s, uint8 skill)
    {
        for (int i = 0; i < SKILL_SLOTS; ++i)
            if (s.slots[i] == skill)
                return i;
        return -1;
    }

    void SaveNode(Player* player, uint16 node, uint8 rank)
    {
        if (rank)
            CharacterDatabase.PExecute("REPLACE INTO character_arpg_skill_node (guid, node, `rank`) VALUES (%u, %u, %u)",
                                       player->GetGUIDLow(), uint32(node), uint32(rank));
        else
            CharacterDatabase.PExecute("DELETE FROM character_arpg_skill_node WHERE guid = %u AND node = %u",
                                       player->GetGUIDLow(), uint32(node));
    }

    void ClearSkill(Player* player, uint8 skill)
    {
        Edit(player, [&](PlayerSkills& s)
        {
            for (auto it = s.ranks.begin(); it != s.ranks.end();)
                it = it->first / 100 == skill ? s.ranks.erase(it) : std::next(it);
        });
        CharacterDatabase.PExecute("DELETE FROM character_arpg_skill_node WHERE guid = %u AND node >= %u AND node < %u",
                                   player->GetGUIDLow(), uint32(skill) * 100, uint32(skill) * 100 + 100);
    }

    // The kit row a node gives at `rank`, `bonusPct` added to its share; rows live as long as the
    // server, so a hit in flight can hold one.
    UniqueMechanic const* KitRow(SkillNode const& node, uint8 rank, uint32 bonusPct)
    {
        static std::mutex lock;
        static std::map<std::tuple<uint16, uint8, uint32>, UniqueMechanic> rows;
        std::lock_guard<std::mutex> guard(lock);
        auto key = std::make_tuple(node.id, rank, bonusPct);
        auto it = rows.find(key);
        if (it == rows.end())
        {
            uint8 const i = std::min<uint8>(rank, node.max) - 1;
            it = rows.emplace(key, UniqueMechanic{ 0, node.kit, node.kitSpell, node.n[i], node.value[i], node.pct[i] + bonusPct, node.text }).first;
        }
        return &it->second;
    }
}

namespace Arpg
{
    uint32 SkillPointsFor(uint32 level)
    {
        return level >= 2 ? 2 * (std::min<uint32>(level, 51) - 1) : 0;
    }

    SkillId SkillOfSpell(uint8 classId, SpellEntry const* spellInfo)
    {
        if (!spellInfo)
            return classId == CLASS_PALADIN ? SKILL_STRIKE : SKILL_NONE;
        char const* name = spellInfo->SpellName[0];
        if (!name)
            return SKILL_NONE;
        for (SkillDef const& skill : Skills())
            if (skill.classId == classId)
                for (char const* want : skill.spells)
                    if (NameMatches(name, want))
                        return skill.id;
        return SKILL_NONE;
    }

    void LoadSkills(Player* player)
    {
        EnsureTables();
        PlayerSkills s;
        uint8 const classId = player->getClass();
        if (auto result = CharacterDatabase.PQuery("SELECT slot, skill FROM character_arpg_skill WHERE guid = %u", player->GetGUIDLow()))
        {
            do
            {
                Field* fields = result->Fetch();
                uint32 const slot = fields[0].GetUInt32();
                uint8 const skill = uint8(fields[1].GetUInt32());
                if (slot < SKILL_SLOTS && FindSkill(classId, skill) && SlotOf(s, skill) < 0)
                    s.slots[slot] = skill;
            }
            while (result->NextRow());
        }
        if (auto result = CharacterDatabase.PQuery("SELECT node, `rank` FROM character_arpg_skill_node WHERE guid = %u", player->GetGUIDLow()))
        {
            do
            {
                Field* fields = result->Fetch();
                uint16 const id = uint16(fields[0].GetUInt32());
                uint8 const rank = uint8(fields[1].GetUInt32());
                if (SkillNode const* node = FindNode(classId, id))
                    if (rank && SlotOf(s, uint8(id / 100)) >= 0)
                        s.ranks[id] = std::min(rank, node->max);
            }
            while (result->NextRow());
        }
        std::lock_guard<std::mutex> guard(sSkillsLock);
        sSkills[player->GetObjectGuid()] = std::move(s);
    }

    void UnloadSkills(Player* player)
    {
        std::lock_guard<std::mutex> guard(sSkillsLock);
        sSkills.erase(player->GetObjectGuid());
    }

    void SendSkills(Player* player)
    {
        PlayerSkills const s = SkillsFor(player);
        uint8 const classId = player->getClass();
        std::vector<SkillDef const*> mine;
        for (SkillDef const& skill : Skills())
            if (skill.classId == classId)
                mine.push_back(&skill);
        if (mine.empty())
            return;

        WorldPacket data(SMSG_ARPG_SKILLS, 64 + mine.size() * 1200);
        data << uint8(1);
        data << uint16(SkillPointsFor(player->GetLevel()));
        data << uint16(SpentIn(s));
        data << uint8(std::min<uint32>(player->GetLevel(), 255));
        data << uint8(SKILL_SLOTS);
        for (int i = 0; i < SKILL_SLOTS; ++i)
            data << uint8(SLOT_LEVEL[i]) << uint8(s.slots[i]);
        data << uint8(mine.size());
        for (SkillDef const* skill : mine)
        {
            data << uint8(skill->id);
            data << uint32(skill->icon);
            data << skill->name;
            data << skill->text;
            data << uint8(SpentIn(s, skill->id));
            data << uint8(SKILL_CAP);
            data << uint8(skill->branches.size());
            for (char const* branch : skill->branches)
                data << branch;
            data << uint8(skill->nodes.size());
            for (SkillNode const& node : skill->nodes)
            {
                auto it = s.ranks.find(node.id);
                data << uint16(node.id);
                data << uint8(node.kind);
                data << uint8(node.column);
                data << uint8(node.row);
                data << uint16(node.parent);
                data << uint8(node.max);
                data << uint8(it == s.ranks.end() ? 0 : it->second);
                data << uint32(node.icon);
                data << node.name;
                data << node.text;
            }
        }
        player->GetSession()->SendPacket(data);
    }

    void SlotSkill(Player* player, uint8 slot, uint8 skill)
    {
        auto refuse = [&](char const* why)
        {
            sLog.outDetail("ARPG skills: %s cannot put skill %u in slot %u: %s", player->GetName(), uint32(skill), uint32(slot), why);
            SendSkills(player);
        };
        if (slot >= SKILL_SLOTS)
            return refuse("no such slot");
        if (player->GetLevel() < SLOT_LEVEL[slot])
            return refuse("the slot is not open yet");
        if (skill != SKILL_NONE && !FindSkill(player->getClass(), skill))
            return refuse("not this class's skill");
        if (player->IsInCombat())
            return refuse("in combat");
        PlayerSkills const s = SkillsFor(player);
        uint8 const leaving = s.slots[slot];
        if (leaving == skill)
            return SendSkills(player);
        int const from = skill != SKILL_NONE ? SlotOf(s, skill) : -1;
        // A skill moving between slots keeps its points; one put out of every slot gives them back.
        if (leaving != SKILL_NONE && from < 0)
            ClearSkill(player, leaving);
        Edit(player, [&](PlayerSkills& e)
        {
            if (from >= 0)
                e.slots[from] = leaving;
            e.slots[slot] = skill;
        });
        if (from >= 0)
        {
            if (leaving != SKILL_NONE)
                CharacterDatabase.PExecute("REPLACE INTO character_arpg_skill (guid, slot, skill) VALUES (%u, %u, %u)",
                                           player->GetGUIDLow(), uint32(from), uint32(leaving));
            else
                CharacterDatabase.PExecute("DELETE FROM character_arpg_skill WHERE guid = %u AND slot = %u",
                                           player->GetGUIDLow(), uint32(from));
        }
        if (skill != SKILL_NONE)
            CharacterDatabase.PExecute("REPLACE INTO character_arpg_skill (guid, slot, skill) VALUES (%u, %u, %u)",
                                       player->GetGUIDLow(), uint32(slot), uint32(skill));
        else
            CharacterDatabase.PExecute("DELETE FROM character_arpg_skill WHERE guid = %u AND slot = %u",
                                       player->GetGUIDLow(), uint32(slot));
        RefreshTotals(player);
        SendSkills(player);
    }

    void SpendSkillNode(Player* player, uint16 id)
    {
        auto refuse = [&](char const* why)
        {
            sLog.outDetail("ARPG skills: %s cannot take node %u: %s", player->GetName(), uint32(id), why);
            SendSkills(player);
        };
        SkillDef const* skill = nullptr;
        SkillNode const* node = FindNode(player->getClass(), id, &skill);
        if (!node)
            return refuse("not this class's node");
        PlayerSkills const s = SkillsFor(player);
        if (SlotOf(s, skill->id) < 0)
            return refuse("the skill is not specialised");
        auto it = s.ranks.find(id);
        uint8 const rank = it == s.ranks.end() ? 0 : it->second;
        if (rank >= node->max)
            return refuse("at its top rank");
        if (node->parent && !s.ranks.count(node->parent))
            return refuse("the node above it has no rank");
        if (SpentIn(s, skill->id) >= SKILL_CAP)
            return refuse("the skill is at its cap");
        if (SpentIn(s) >= SkillPointsFor(player->GetLevel()))
            return refuse("no skill points left");
        Edit(player, [&](PlayerSkills& e) { e.ranks[id] = rank + 1; });
        SaveNode(player, id, rank + 1);
        RefreshTotals(player);
        SendSkills(player);
    }

    void RefundSkillNode(Player* player, uint16 id)
    {
        auto refuse = [&](char const* why)
        {
            sLog.outDetail("ARPG skills: %s cannot give back node %u: %s", player->GetName(), uint32(id), why);
            SendSkills(player);
        };
        SkillDef const* skill = nullptr;
        SkillNode const* node = FindNode(player->getClass(), id, &skill);
        PlayerSkills const s = SkillsFor(player);
        auto it = node ? s.ranks.find(id) : s.ranks.end();
        if (it == s.ranks.end())
            return refuse("no rank in it");
        if (player->IsInCombat())
            return refuse("in combat");
        if (it->second == 1)
            for (SkillNode const& child : skill->nodes)
                if (child.parent == id && s.ranks.count(child.id))
                    return refuse("a node below it has ranks");
        uint8 const rank = it->second - 1;
        Edit(player, [&](PlayerSkills& e)
        {
            if (rank)
                e.ranks[id] = rank;
            else
                e.ranks.erase(id);
        });
        SaveNode(player, id, rank);
        RefreshTotals(player);
        SendSkills(player);
    }

    void RespecSkill(Player* player, uint8 skill)
    {
        if (!player->IsInCombat() && FindSkill(player->getClass(), skill))
        {
            ClearSkill(player, skill);
            RefreshTotals(player);
        }
        SendSkills(player);
    }

    void AddSkillTotals(Player const* player, WebTotals& totals)
    {
        PlayerSkills const s = SkillsFor(player);
        uint8 const classId = player->getClass();
        for (auto const& [id, rank] : s.ranks)
        {
            SkillDef const* skill = nullptr;
            SkillNode const* node = FindNode(classId, id, &skill);
            if (!node || !rank)
                continue;
            if (node->key != KEY_NONE)
                totals.rank[node->key] = std::max(totals.rank[node->key], rank);
            if (node->modOp >= 0)
                if (uint64 mask = SkillMask(*skill))
                    totals.mods.push_back({ uint8(node->modOp), node->modType, node->modPerRank * rank, mask });
        }
    }

    std::vector<UniqueMechanic const*> SkillModifiers(Player const* player)
    {
        std::vector<UniqueMechanic const*> rows;
        PlayerSkills const s = SkillsFor(player);
        if (s.ranks.empty())
            return rows;
        uint8 const classId = player->getClass();
        // Echoing Verdict adds to Chain of Judgement's share.
        auto echo = s.ranks.find(302);
        uint32 const echoPct = echo == s.ranks.end() ? 0 : 10 * echo->second;
        for (auto const& [id, rank] : s.ranks)
        {
            SkillNode const* node = FindNode(classId, id);
            if (node && node->kit && rank)
                rows.push_back(KitRow(*node, rank, id == 301 ? echoPct : 0));
        }
        return rows;
    }
}
