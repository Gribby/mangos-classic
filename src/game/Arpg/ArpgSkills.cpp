/*
 * ARPG skill specialisation: see ArpgSkills.h.
 */

#include "Arpg/ArpgSkills.h"
#include "Arpg/ArpgCombat.h"

#include "Chat/Chat.h"

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
#include <set>
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

    // A capstone built from a kit row.
    SkillNode KitCap(uint16 id, uint8 col, uint8 row, uint16 parent, char const* name, char const* text, uint32 icon,
                     UniqueKit kit, uint32 spell, uint8 n, float value, uint32 pct)
    {
        SkillNode node = Kit(id, col, row, parent, 1, name, text, icon, kit, spell, { n }, { value }, { pct });
        node.kind = SKILL_NODE_CAPSTONE;
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
            { SKILL_EXORCISM, CLASS_PALADIN, "Exorcism", "Holy fire at an enemy. Specialised, it strikes any foe, not only undead and demons.", 879, { "Exorcism" },
              { "Burning", "Chain", "Purge" },
              {
                  Kit(601, 0, 1, 0, 3, "Holy Fire", "Exorcism bursts onto enemies within 4/6/8 yards of its target, at 35%.", 2812,
                      KIT_BURST, 879, { 0, 0, 0 }, { 4, 6, 8 }, { 35, 35, 35 }),
                  Mod(602, 0, 2, 601, 4, "Searing Word", "+10% Exorcism damage per rank.", 20218, SPELLMOD_DAMAGE, SPELLMOD_PCT, 10),
                  KitCap(603, 0, 3, 602, "Purging Light", "Exorcism's light passes through its target: every enemy within 10 yards behind it takes the full hit.", 19750,
                         KIT_FRAGMENTS, 879, 10, 10.0f, 100),
                  Kit(604, 1, 1, 0, 3, "Chain Exorcism", "Exorcism leaps to 1/2/3 more enemies within 10 yards, at 60%.", 20186,
                      KIT_CHAIN, 879, { 1, 2, 3 }, { 10, 10, 10 }, { 60, 60, 60 }),
                  Mod(605, 1, 2, 604, 3, "Swift Exorcism", "-2 sec Exorcism cooldown per rank.", 1044, SPELLMOD_COOLDOWN, SPELLMOD_FLAT, -2000),
                  Kit(606, 1, 3, 605, 2, "Echoing Word", "Every 4th/3rd Exorcism repeats for free along the same aim, at 50%.", 20185,
                      KIT_ECHO, 879, { 4, 3 }, { 0, 0 }, { 50, 50 }),
                  Mod(607, 2, 1, 0, 3, "Exorcist", "+5% Exorcism critical strike chance per rank.", 20208, SPELLMOD_CRITICAL_CHANCE, SPELLMOD_FLAT, 5),
                  Mod(608, 2, 2, 607, 3, "Zealous Word", "-15% Exorcism mana cost per rank.", 20166, SPELLMOD_COST, SPELLMOD_PCT, -15),
                  KitCap(609, 2, 3, 608, "Twin Exorcism", "Exorcism fires a second bolt beside the first, at 70%.", 879,
                         KIT_EXTRA_PROJECTILES, 879, 1, 12.0f, 70),
              } },
            { SKILL_HOLY_WRATH, CLASS_PALADIN, "Holy Wrath", "A nova of holy fire around you. Specialised, it strikes any foe.", 2812, { "Holy Wrath" },
              { "Radius", "Fury", "Zeal" },
              {
                  Mod(701, 0, 1, 0, 4, "Wide Wrath", "+12% Holy Wrath radius per rank.", 2812, SPELLMOD_RADIUS, SPELLMOD_PCT, 12),
                  Mod(702, 0, 2, 701, 3, "Searing Wrath", "+10% Holy Wrath damage per rank.", 20218, SPELLMOD_DAMAGE, SPELLMOD_PCT, 10),
                  Key(703, SKILL_NODE_CAPSTONE, 0, 3, 702, 1, "Wrathful", "Holy Wrath fires on its own whenever 4 or more enemies are within 10 yards, every 12 sec at most.", 20925, KEY_WRATHFUL),
                  Mod(704, 1, 1, 0, 4, "Swift Wrath", "-8 sec Holy Wrath cooldown per rank.", 1044, SPELLMOD_COOLDOWN, SPELLMOD_FLAT, -8000),
                  Mod(705, 1, 2, 704, 3, "Quick Wrath", "-0.4 sec Holy Wrath cast time per rank.", 20166, SPELLMOD_CASTING_TIME, SPELLMOD_FLAT, -400),
                  Kit(706, 1, 3, 705, 2, "Shattering Wrath", "Each enemy Holy Wrath strikes bursts onto those within 4/6 yards, at 20/30%.", 24275,
                      KIT_BURST, 2812, { 0, 0 }, { 4, 6 }, { 20, 30 }),
                  Mod(707, 2, 1, 0, 3, "Zealous Wrath", "-15% Holy Wrath mana cost per rank.", 20208, SPELLMOD_COST, SPELLMOD_PCT, -15),
                  Mod(708, 2, 2, 707, 3, "Righteous Wrath", "+5% Holy Wrath critical strike chance per rank.", 20101, SPELLMOD_CRITICAL_CHANCE, SPELLMOD_FLAT, 5),
                  Mod(709, 2, 3, 708, 2, "Radiant Wrath", "+10% Holy Wrath damage per rank.", 19750, SPELLMOD_DAMAGE, SPELLMOD_PCT, 10),
              } },
            { SKILL_HAMMER_OF_WRATH, CLASS_PALADIN, "Hammer of Wrath", "Hurl a hammer at an enemy, at any health.", 24275, { "Hammer of Wrath" },
              { "Volley", "Pierce", "Execute" },
              {
                  Kit(801, 0, 1, 0, 2, "Hammer Volley", "Hammer of Wrath throws 1/2 more hammers, fanned 15 degrees apart, at 60%.", 24275,
                      KIT_EXTRA_PROJECTILES, 24275, { 1, 2 }, { 15, 15 }, { 60, 60 }),
                  Mod(802, 0, 2, 801, 4, "Weighted Hammer", "+8% Hammer of Wrath damage per rank.", 20218, SPELLMOD_DAMAGE, SPELLMOD_PCT, 8),
                  KitCap(803, 0, 3, 802, "Storm of Hammers", "Every 2nd Hammer of Wrath repeats for free along the same aim, at 60%.", 20185,
                         KIT_ECHO, 24275, 2, 0.0f, 60),
                  Kit(804, 1, 1, 0, 3, "Piercing Hammer", "Hammer of Wrath flies on through 2/4/10 enemies in its path, at 60%.", 20186,
                      KIT_PIERCE, 24275, { 2, 4, 10 }, { 0, 0, 0 }, { 60, 60, 60 }),
                  Mod(805, 1, 2, 804, 3, "Long Throw", "+5 yards Hammer of Wrath range per rank.", 5588, SPELLMOD_RANGE, SPELLMOD_FLAT, 5),
                  Mod(806, 1, 3, 805, 3, "Swift Hammer", "-1 sec Hammer of Wrath cooldown per rank.", 1044, SPELLMOD_COOLDOWN, SPELLMOD_FLAT, -1000),
                  Mod(807, 2, 1, 0, 3, "Righteous Hammer", "+5% Hammer of Wrath critical strike chance per rank.", 20101, SPELLMOD_CRITICAL_CHANCE, SPELLMOD_FLAT, 5),
                  Mod(808, 2, 2, 807, 3, "Zealous Hammer", "-15% Hammer of Wrath mana cost per rank.", 20166, SPELLMOD_COST, SPELLMOD_PCT, -15),
                  Key(809, SKILL_NODE_CAPSTONE, 2, 3, 808, 1, "Executioner", "Hammer of Wrath deals double damage to enemies below 20% health.", 879, KEY_EXECUTIONER),
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

    // --- Codex pages: each capstone's home boss ---

    struct PageHome
    {
        uint16 node;
        uint32 boss;          // creature entry
        char const* where;    // for the sealed node's tooltip
    };

    PageHome const PAGES[] =
    {
        { 103, 3975,  "Herod, Scarlet Monastery" },                     // Whirling Strikes
        { 106, 646,   "Mr. Smite, the Deadmines" },                     // Crusader's Pace
        { 109, 7228,  "Ironaya, Uldaman" },                             // Shockwave
        { 203, 6487,  "Arcanist Doan, Scarlet Monastery" },             // Twin Seals
        { 209, 4542,  "High Inquisitor Fairbanks, Scarlet Monastery" }, // Light of the Crusader
        { 303, 3976,  "Scarlet Commander Mograine, Scarlet Monastery" },// Final Verdict
        { 306, 9019,  "Emperor Dagran Thaurissan, Blackrock Depths" },  // Sentence
        { 403, 3977,  "High Inquisitor Whitemane, Scarlet Monastery" }, // Walking Consecration
        { 409, 10813, "Balnazzar, Stratholme" },                        // Sacred Seal
        { 503, 10440, "Baron Rivendare, Stratholme" },                  // Blessed Hammer
        { 508, 9568,  "Overlord Wyrmthalak, Lower Blackrock Spire" },   // Sentence Passed
        { 603, 10508, "Ras Frostwhisper, Scholomance" },                // Purging Light
        { 609, 10435, "Magistrate Barthilas, Stratholme" },             // Twin Exorcism
        { 703, 10811, "Archivist Galford, Stratholme" },                // Wrathful
        { 803, 10429, "Warchief Rend Blackhand, Upper Blackrock Spire" },// Storm of Hammers
        { 809, 9816,  "Pyroguard Emberseer, Upper Blackrock Spire" },   // Executioner
    };

    PageHome const* PageOf(uint16 node)
    {
        for (PageHome const& page : PAGES)
            if (page.node == node)
                return &page;
        return nullptr;
    }

    // --- Runes ---

    enum RuneEffect : uint8 { RUNE_KIT, RUNE_MODS, RUNE_KEY, RUNE_HOOK };

    struct RuneDef
    {
        uint8 id;
        char const* name;
        char const* text;
        uint32 icon;              // spell icon for the window
        uint8 fits;               // bit per SkillId
        RuneEffect effect;
        UniqueKit kit;            // RUNE_KIT: the row, on each fitting skill's kit spells
        uint8 n;
        float value;
        uint32 pct;
        std::vector<std::tuple<uint8, uint8, int32>> mods; // RUNE_MODS: op, type, amount
    };

    // A rune fits skills 1-7 (the wire's mask is a byte); later skills take none yet.
    constexpr uint8 FIT(SkillId id) { return id < 8 ? uint8(1u << id) : uint8(0); }

    std::vector<RuneDef> const& Runes()
    {
        static std::vector<RuneDef> const runes =
        {
            { RUNE_CHAINS, "Rune of Chains", "The skill's hit chains to 1 more enemy within 10 yards, at 50% (one more jump on Chain of Judgement).", 20186,
              uint8(FIT(SKILL_JUDGEMENT) | FIT(SKILL_EXORCISM)), RUNE_KIT, KIT_CHAIN, 1, 10.0f, 50, {} },
            { RUNE_SHATTERING, "Rune of Shattering", "The skill's hits burst onto enemies within 6 yards of the target, at 35% (+15% on Hammer of Light).", 24275,
              uint8(FIT(SKILL_JUDGEMENT) | FIT(SKILL_SEALS) | FIT(SKILL_EXORCISM)), RUNE_KIT, KIT_BURST, 0, 6.0f, 35, {} },
            { RUNE_EXPANSE, "Rune of Expanse", "+30% area.", 2812,
              uint8(FIT(SKILL_CONSECRATION) | FIT(SKILL_HOLY_WRATH)), RUNE_MODS, UniqueKit(0), 0, 0.0f, 0, { std::make_tuple(uint8(SPELLMOD_RADIUS), uint8(SPELLMOD_PCT), int32(30)) } },
            { RUNE_LINGERING, "Rune of Lingering", "+50% duration.", 20925,
              uint8(FIT(SKILL_CONSECRATION) | FIT(SKILL_HAMMER)), RUNE_MODS, UniqueKit(0), 0, 0.0f, 0, { std::make_tuple(uint8(SPELLMOD_DURATION), uint8(SPELLMOD_PCT), int32(50)) } },
            { RUNE_HASTE, "Rune of Haste", "-25% cooldown, -15% damage.", 1044,
              uint8(FIT(SKILL_JUDGEMENT) | FIT(SKILL_CONSECRATION) | FIT(SKILL_HAMMER) | FIT(SKILL_EXORCISM) | FIT(SKILL_HOLY_WRATH)), RUNE_MODS, UniqueKit(0), 0, 0.0f, 0,
              { std::make_tuple(uint8(SPELLMOD_COOLDOWN), uint8(SPELLMOD_PCT), int32(-25)), std::make_tuple(uint8(SPELLMOD_DAMAGE), uint8(SPELLMOD_PCT), int32(-15)),
                std::make_tuple(uint8(SPELLMOD_DOT), uint8(SPELLMOD_PCT), int32(-15)) } },
            { RUNE_LEECH, "Rune of Leech", "3% of the skill's damage heals you.", 20166,
              uint8(FIT(SKILL_STRIKE) | FIT(SKILL_SEALS) | FIT(SKILL_JUDGEMENT) | FIT(SKILL_EXORCISM) | FIT(SKILL_HOLY_WRATH)), RUNE_HOOK, UniqueKit(0), 0, 0.0f, 3, {} },
            { RUNE_COMMAND, "Rune of Command", "The stun spreads to 1 more enemy within 8 yards.", 20549,
              uint8(FIT(SKILL_HAMMER)), RUNE_KIT, KIT_SPREAD, 1, 8.0f, 100, {} },
            { RUNE_SANCTITY, "Rune of Sanctity", "+15% damage.", 20218,
              uint8(FIT(SKILL_SEALS) | FIT(SKILL_JUDGEMENT) | FIT(SKILL_CONSECRATION) | FIT(SKILL_EXORCISM) | FIT(SKILL_HOLY_WRATH)), RUNE_MODS, UniqueKit(0), 0, 0.0f, 0,
              { std::make_tuple(uint8(SPELLMOD_DAMAGE), uint8(SPELLMOD_PCT), int32(15)), std::make_tuple(uint8(SPELLMOD_DOT), uint8(SPELLMOD_PCT), int32(15)) } },
            { RUNE_FURY, "Rune of Fury", "Your swing strikes one rank wider (Wide Swing +1, or 20% to every enemy in front).", 845,
              uint8(FIT(SKILL_STRIKE)), RUNE_KEY, UniqueKit(0), 0, 0.0f, 0, {} },
        };
        return runes;
    }

    RuneDef const* FindRune(uint8 id)
    {
        for (RuneDef const& rune : Runes())
            if (rune.id == id)
                return &rune;
        return nullptr;
    }

    // The spells a rune's kit row lands on, by skill: the kit spells its tree's own rows use.
    std::vector<uint32> KitSpellsOf(SkillId skill)
    {
        switch (skill)
        {
            case SKILL_SEALS: return { 20154, 20375 };      // Seal of Righteousness, of Command
            case SKILL_JUDGEMENT: return { 20271 };
            case SKILL_CONSECRATION: return { 26573 };
            case SKILL_HAMMER: return { 853 };
            case SKILL_EXORCISM: return { 879 };
            case SKILL_HOLY_WRATH: return { 2812 };
            case SKILL_HAMMER_OF_WRATH: return { 24275 };
            default: return {};
        }
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
        std::set<uint16> pages;               // Codex pages read, by capstone node
        uint32 fragments = 0;                 // Codex fragments held
        std::map<uint8, uint8> runes;         // runes held, socketed ones included
        std::map<uint8, uint8> sockets;       // by skill: the rune in its socket
        std::map<uint16, uint32> things;      // other held things (100 and up: raid key pieces)
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
            CharacterDatabase.DirectExecute(
                "CREATE TABLE IF NOT EXISTS character_arpg_codex ("
                "guid INT UNSIGNED NOT NULL, page SMALLINT UNSIGNED NOT NULL, "
                "PRIMARY KEY (guid, page)) ENGINE=InnoDB DEFAULT CHARSET=utf8 COMMENT='ARPG Codex pages read (Arpg/ArpgSkills.h)'");
            CharacterDatabase.DirectExecute(
                "CREATE TABLE IF NOT EXISTS character_arpg_held ("
                "guid INT UNSIGNED NOT NULL, thing SMALLINT UNSIGNED NOT NULL, count SMALLINT UNSIGNED NOT NULL, "
                "PRIMARY KEY (guid, thing)) ENGINE=InnoDB DEFAULT CHARSET=utf8 COMMENT='ARPG Codex fragments (0) and runes held (Arpg/ArpgSkills.h)'");
            CharacterDatabase.DirectExecute(
                "CREATE TABLE IF NOT EXISTS character_arpg_socket ("
                "guid INT UNSIGNED NOT NULL, skill TINYINT UNSIGNED NOT NULL, rune TINYINT UNSIGNED NOT NULL, "
                "PRIMARY KEY (guid, skill)) ENGINE=InnoDB DEFAULT CHARSET=utf8 COMMENT='ARPG rune sockets (Arpg/ArpgSkills.h)'");
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

    void SaveHeld(Player* player, uint16 thing, uint32 count)
    {
        if (count)
            CharacterDatabase.PExecute("REPLACE INTO character_arpg_held (guid, thing, count) VALUES (%u, %u, %u)",
                                       player->GetGUIDLow(), uint32(thing), std::min<uint32>(count, 65535));
        else
            CharacterDatabase.PExecute("DELETE FROM character_arpg_held WHERE guid = %u AND thing = %u",
                                       player->GetGUIDLow(), uint32(thing));
    }

    void SaveSocket(Player* player, uint8 skill, uint8 rune)
    {
        if (rune)
            CharacterDatabase.PExecute("REPLACE INTO character_arpg_socket (guid, skill, rune) VALUES (%u, %u, %u)",
                                       player->GetGUIDLow(), uint32(skill), uint32(rune));
        else
            CharacterDatabase.PExecute("DELETE FROM character_arpg_socket WHERE guid = %u AND skill = %u",
                                       player->GetGUIDLow(), uint32(skill));
    }

    void SavePage(Player* player, uint16 node)
    {
        CharacterDatabase.PExecute("REPLACE INTO character_arpg_codex (guid, page) VALUES (%u, %u)", player->GetGUIDLow(), uint32(node));
    }

    // A rune socket opens with this many points in the skill.
    constexpr uint32 SOCKET_POINTS = 10;

    void ClearSkill(Player* player, uint8 skill)
    {
        bool unsocketed = false;
        Edit(player, [&](PlayerSkills& s)
        {
            for (auto it = s.ranks.begin(); it != s.ranks.end();)
                it = it->first / 100 == skill ? s.ranks.erase(it) : std::next(it);
            unsocketed = s.sockets.erase(skill) > 0;
        });
        if (unsocketed)
            SaveSocket(player, skill, 0);
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

    // A rune's kit row on `spell`; rows live as long as the server.
    UniqueMechanic const* RuneRow(RuneDef const& rune, uint32 spell)
    {
        static std::mutex lock;
        static std::map<std::pair<uint8, uint32>, UniqueMechanic> rows;
        std::lock_guard<std::mutex> guard(lock);
        auto key = std::make_pair(rune.id, spell);
        auto it = rows.find(key);
        if (it == rows.end())
            it = rows.emplace(key, UniqueMechanic{ 0, rune.kit, spell, rune.n, rune.value, rune.pct, rune.text }).first;
        return &it->second;
    }

    // A rune's kit row on top of a tree node's row of the same kit and spell, so the rune adds to
    // the node rather than losing to it (only the best row of a kit applies): one more chain jump,
    // or 15 points more burst, at the wider reach.
    constexpr uint32 RUNE_STACK_PCT = 15;
    UniqueMechanic const* StackedRow(RuneDef const& rune, UniqueMechanic const* node)
    {
        static std::mutex lock;
        static std::map<std::pair<uint8, UniqueMechanic const*>, UniqueMechanic> rows;
        std::lock_guard<std::mutex> guard(lock);
        auto key = std::make_pair(rune.id, node);
        auto it = rows.find(key);
        if (it == rows.end())
        {
            UniqueMechanic row = *node;
            if (rune.n)
                row.n = uint8(std::min<uint32>(node->n + rune.n, 10));
            else
                row.pct = node->pct + RUNE_STACK_PCT;
            row.value = std::max(node->value, rune.value);
            it = rows.emplace(key, row).first;
        }
        return &it->second;
    }

    // The runes in effect: socketed in a specialised skill with SOCKET_POINTS in it.
    std::vector<std::pair<SkillId, RuneDef const*>> ActiveRunes(PlayerSkills const& s)
    {
        std::vector<std::pair<SkillId, RuneDef const*>> out;
        for (auto const& [skill, id] : s.sockets)
            if (RuneDef const* rune = FindRune(id))
                if ((rune->fits & FIT(SkillId(skill))) && SlotOf(s, skill) >= 0 && SpentIn(s, skill) >= SOCKET_POINTS)
                    out.emplace_back(SkillId(skill), rune);
        return out;
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
        if (auto result = CharacterDatabase.PQuery("SELECT page FROM character_arpg_codex WHERE guid = %u", player->GetGUIDLow()))
        {
            do
                s.pages.insert(uint16(result->Fetch()[0].GetUInt32()));
            while (result->NextRow());
        }
        if (auto result = CharacterDatabase.PQuery("SELECT thing, count FROM character_arpg_held WHERE guid = %u", player->GetGUIDLow()))
        {
            do
            {
                Field* fields = result->Fetch();
                uint32 const thing = fields[0].GetUInt32();
                uint32 const count = fields[1].GetUInt32();
                if (thing == 0)
                    s.fragments = count;
                else if (thing >= HELD_OTHER)
                    s.things[uint16(thing)] = count;
                else if (FindRune(uint8(thing)))
                    s.runes[uint8(thing)] = uint8(std::min<uint32>(count, 255));
            }
            while (result->NextRow());
        }
        if (auto result = CharacterDatabase.PQuery("SELECT skill, rune FROM character_arpg_socket WHERE guid = %u", player->GetGUIDLow()))
        {
            do
            {
                Field* fields = result->Fetch();
                uint8 const skill = uint8(fields[0].GetUInt32());
                uint8 const rune = uint8(fields[1].GetUInt32());
                if (FindSkill(classId, skill) && FindRune(rune) && s.runes[rune] > 0)
                    s.sockets[skill] = rune;
            }
            while (result->NextRow());
        }
        // A capstone taken before Codex pages sealed them stays: its page counts as read.
        std::vector<uint16> grandfathered;
        for (auto const& [id, rank] : s.ranks)
        {
            SkillNode const* node = FindNode(classId, id);
            if (node && node->kind == SKILL_NODE_CAPSTONE && rank && !s.pages.count(id))
            {
                s.pages.insert(id);
                grandfathered.push_back(id);
            }
        }
        for (uint16 id : grandfathered)
            SavePage(player, id);
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

        WorldPacket data(SMSG_ARPG_SKILLS, 512 + mine.size() * 1600);
        data << uint8(2);
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
            auto socket = s.sockets.find(skill->id);
            data << uint8(socket == s.sockets.end() ? 0 : socket->second);
            data << uint8(SOCKET_POINTS);
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
                PageHome const* page = node.kind == SKILL_NODE_CAPSTONE ? PageOf(node.id) : nullptr;
                data << uint8(page && !s.pages.count(node.id) ? 1 : 0);
                data << (page ? page->where : "");
            }
        }
        data << uint16(std::min<uint32>(s.fragments, 65535));
        data << uint8(FRAGMENTS_PER_PAGE);
        std::vector<RuneDef> const& runes = Runes();
        data << uint8(runes.size());
        for (RuneDef const& rune : runes)
        {
            auto held = s.runes.find(rune.id);
            data << uint8(rune.id);
            data << uint32(rune.icon);
            data << rune.name;
            data << rune.text;
            data << uint8(held == s.runes.end() ? 0 : held->second);
            data << uint8(rune.fits);
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
        if (node->kind == SKILL_NODE_CAPSTONE && PageOf(id) && !s.pages.count(id))
            return refuse("sealed: its Codex page is unread");
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
        for (auto const& [skillId, rune] : ActiveRunes(s))
        {
            if (rune->effect == RUNE_MODS)
                if (SkillDef const* skill = FindSkill(classId, skillId))
                    if (uint64 mask = SkillMask(*skill))
                        for (auto const& [op, type, amount] : rune->mods)
                            totals.mods.push_back({ op, type, amount, mask });
            // Fury: one rank wider, to Wide Swing's top.
            if (rune->effect == RUNE_KEY)
                totals.rank[KEY_WIDE_SWING] = std::min<uint8>(totals.rank[KEY_WIDE_SWING] + 1, 3);
        }
    }

    std::vector<UniqueMechanic const*> SkillModifiers(Player const* player)
    {
        std::vector<UniqueMechanic const*> rows;
        PlayerSkills const s = SkillsFor(player);
        std::vector<std::pair<SkillId, RuneDef const*>> const runes = ActiveRunes(s);
        // The tree's rows first, then each kit rune: on top of a node row of its kit and spell,
        // else a row of its own.
        auto addRunes = [&]()
        {
            for (auto const& [skillId, rune] : runes)
            {
                if (rune->effect != RUNE_KIT)
                    continue;
                for (uint32 spell : KitSpellsOf(skillId))
                {
                    bool stacked = false;
                    for (UniqueMechanic const*& row : rows)
                        if (row->kit == rune->kit && row->spell == spell)
                        {
                            row = StackedRow(*rune, row);
                            stacked = true;
                        }
                    if (!stacked)
                        rows.push_back(RuneRow(*rune, spell));
                }
            }
        };
        if (s.ranks.empty())
        {
            addRunes();
            return rows;
        }
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
        addRunes();
        return rows;
    }

    bool HasRune(Player const* player, SkillId skill, uint8 rune)
    {
        PlayerSkills const s = SkillsFor(player);
        for (auto const& [id, def] : ActiveRunes(s))
            if (id == skill && def->id == rune)
                return true;
        return false;
    }

    std::vector<uint16> PagesFrom(uint32 bossEntry)
    {
        std::vector<uint16> out;
        for (PageHome const& page : PAGES)
            if (page.boss == bossEntry)
                out.push_back(page.node);
        return out;
    }

    std::vector<uint16> AllPages()
    {
        std::vector<uint16> out;
        for (PageHome const& page : PAGES)
            out.push_back(page.node);
        return out;
    }

    std::vector<uint8> AllRunes()
    {
        std::vector<uint8> out;
        for (RuneDef const& rune : Runes())
            out.push_back(rune.id);
        return out;
    }

    char const* PageName(uint16 node)
    {
        for (SkillDef const& skill : Skills())
            for (SkillNode const& n : skill.nodes)
                if (n.id == node)
                    return n.name;
        return "";
    }

    char const* RuneName(uint8 rune)
    {
        RuneDef const* def = FindRune(rune);
        return def ? def->name : "";
    }

    char const* RuneText(uint8 rune)
    {
        RuneDef const* def = FindRune(rune);
        return def ? def->text : "";
    }

    void ReadPage(Player* player, uint16 node)
    {
        if (!PageOf(node))
            return;
        bool fresh = false;
        Edit(player, [&](PlayerSkills& s) { fresh = s.pages.insert(node).second; });
        if (fresh)
        {
            SavePage(player, node);
            ChatHandler(player).PSendSysMessage("|cffa335eeYou read the Codex page: %s is unsealed.|r", PageName(node));
        }
        else
        {
            // A page already read is torn into fragments.
            AddFragments(player, FRAGMENTS_FOR_DUPLICATE);
            return;
        }
        SendSkills(player);
    }

    void AddFragments(Player* player, uint32 count)
    {
        uint32 total = 0;
        Edit(player, [&](PlayerSkills& s) { s.fragments += count; total = s.fragments; });
        SaveHeld(player, 0, total);
        ChatHandler(player).PSendSysMessage("|cff1eff00Codex fragments: %u (%u unseal a capstone).|r", total, uint32(FRAGMENTS_PER_PAGE));
        SendSkills(player);
    }

    void AddRune(Player* player, uint8 rune)
    {
        if (!FindRune(rune))
            return;
        uint32 held = 0;
        Edit(player, [&](PlayerSkills& s) { held = std::min<uint32>(s.runes[rune] + 1, 255); s.runes[rune] = uint8(held); });
        SaveHeld(player, rune, held);
        ChatHandler(player).PSendSysMessage("|cff0070ddYou gain a %s (%u held). Socket it in the Skills window.|r", RuneName(rune), held);
        SendSkills(player);
    }

    void UnsealWithFragments(Player* player, uint16 node)
    {
        SkillDef const* skill = nullptr;
        SkillNode const* n = FindNode(player->getClass(), node, &skill);
        PlayerSkills const s = SkillsFor(player);
        if (!n || n->kind != SKILL_NODE_CAPSTONE || !PageOf(node) || s.pages.count(node) ||
                s.fragments < FRAGMENTS_PER_PAGE || SlotOf(s, skill->id) < 0)
            return SendSkills(player);
        uint32 left = 0;
        Edit(player, [&](PlayerSkills& e) { e.fragments -= FRAGMENTS_PER_PAGE; left = e.fragments; e.pages.insert(node); });
        SaveHeld(player, 0, left);
        SavePage(player, node);
        ChatHandler(player).PSendSysMessage("|cffa335eeThe fragments bind into a page: %s is unsealed.|r", n->name);
        SendSkills(player);
    }

    void SocketRune(Player* player, uint8 skill, uint8 rune)
    {
        PlayerSkills const s = SkillsFor(player);
        SkillDef const* def = FindSkill(player->getClass(), skill);
        auto refuse = [&](char const* why)
        {
            sLog.outDetail("ARPG skills: %s cannot socket rune %u in skill %u: %s", player->GetName(), uint32(rune), uint32(skill), why);
            SendSkills(player);
        };
        if (!def || SlotOf(s, skill) < 0)
            return refuse("the skill is not specialised");
        if (player->IsInCombat())
            return refuse("in combat");
        if (rune)
        {
            RuneDef const* r = FindRune(rune);
            if (!r || !(r->fits & FIT(SkillId(skill))))
                return refuse("the rune does not fit the skill");
            if (SpentIn(s, skill) < SOCKET_POINTS)
                return refuse("the socket is not open yet");
            auto held = s.runes.find(rune);
            uint32 inUse = 0;
            for (auto const& [other, socketed] : s.sockets)
                if (socketed == rune && other != skill)
                    ++inUse;
            if (held == s.runes.end() || held->second <= inUse)
                return refuse("no such rune free");
        }
        Edit(player, [&](PlayerSkills& e)
        {
            if (rune)
                e.sockets[skill] = rune;
            else
                e.sockets.erase(skill);
        });
        SaveSocket(player, skill, rune);
        RefreshTotals(player);
        SendSkills(player);
    }

    uint32 Held(Player const* player, uint16 thing)
    {
        PlayerSkills const s = SkillsFor(player);
        auto it = s.things.find(thing);
        return it == s.things.end() ? 0 : it->second;
    }

    uint32 AddHeld(Player* player, uint16 thing, int32 delta)
    {
        if (thing < HELD_OTHER)
            return 0;
        uint32 count = 0;
        Edit(player, [&](PlayerSkills& s)
        {
            int64 const now = int64(s.things[thing]) + delta;
            count = uint32(std::max<int64>(0, std::min<int64>(now, 65535)));
            if (count)
                s.things[thing] = count;
            else
                s.things.erase(thing);
        });
        SaveHeld(player, thing, count);
        return count;
    }

    bool IsSpecialised(Player const* player, SkillId skill)
    {
        return SlotOf(SkillsFor(player), skill) >= 0;
    }
}
