/*
 * ARPG skill specialisation: see ArpgSkills.h.
 */

#include "Arpg/ArpgSkills.h"
#include "Arpg/ArpgCombat.h"
#include "Arpg/ArpgCharacter.h"
#include "Arpg/ArpgTags.h"
#include "Arpg/ArpgProcs.h"

#include "Chat/Chat.h"

#include "Database/DatabaseEnv.h"
#include "Entities/Item.h"
#include "Entities/Player.h"
#include "Globals/ObjectMgr.h"
#include "Log/Log.h"
#include "Server/DBCStores.h"
#include "Server/Opcodes.h"
#include "Server/WorldPacket.h"
#include "Server/WorldSession.h"
#include "Spells/SpellMgr.h"
#include "Util/Timer.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
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
        char const* only = nullptr; // a spell modifier over this spell (by name) alone, not the skill's every spell
        int8 ailTag = -1;         // an ailment node: its damage tag, and the chance per rank
        uint8 ailPerRank = 0;
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
                  uint32 icon, SpellModOp op, SpellModType type, int32 perRank, char const* only = nullptr)
    {
        SkillNode n{ id, SKILL_NODE_MODIFIER, col, row, parent, max, name, text, icon };
        n.modOp = int8(op);
        n.modType = uint8(type);
        n.modPerRank = perRank;
        n.only = only;
        return n;
    }

    // An ailment node: the skill's hits have `perRank`% per rank to inflict `tag`'s ailment
    // (Arpg/ArpgDamage.h), whatever their own damage type.
    SkillNode Ail(uint16 id, uint8 col, uint8 row, uint16 parent, uint8 max, char const* name, char const* text,
                  uint32 icon, Tag tag, uint8 perRank)
    {
        SkillNode n{ id, SKILL_NODE_MODIFIER, col, row, parent, max, name, text, icon };
        n.ailTag = int8(tag);
        n.ailPerRank = perRank;
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

    // A capstone built from a spell modifier, or from an ailment chance.
    SkillNode ModCap(uint16 id, uint8 col, uint8 row, uint16 parent, char const* name, char const* text, uint32 icon,
                     SpellModOp op, SpellModType type, int32 amount, char const* only = nullptr)
    {
        SkillNode node = Mod(id, col, row, parent, 1, name, text, icon, op, type, amount, only);
        node.kind = SKILL_NODE_CAPSTONE;
        return node;
    }

    SkillNode AilCap(uint16 id, uint8 col, uint8 row, uint16 parent, char const* name, char const* text, uint32 icon,
                     Tag tag, uint8 chance)
    {
        SkillNode node = Ail(id, col, row, parent, 1, name, text, icon, tag, chance);
        node.kind = SKILL_NODE_CAPSTONE;
        return node;
    }

    // The paladin's specialisable skills (docs/ARPG-CHARACTER.md, "The paladin, in full"): six,
    // for five slots. A tree is three branches (two for Holy Light) of four or five tiers.
    std::vector<SkillDef> const& Skills()
    {
        static std::vector<SkillDef> const skills =
        {
            { SKILL_STRIKE, CLASS_PALADIN, "Crusader", "Your held swing and your Seals, as one: the Seal is the swing's damage.", 20154, { "Seal of" },
              { "Reach", "Righteous", "Zeal" },
              {
                  Key(101, SKILL_NODE_TRANSFORMER, 0, 1, 0, 3, "Wide Swing", "Your swings also strike every other enemy in front of you, for 20/35/50% of the hit.", 845, KEY_WIDE_SWING),
                  Key(102, SKILL_NODE_MODIFIER, 0, 2, 101, 2, "Long Arm", "+1 yard melee reach per rank.", 12292, KEY_LONG_ARM),
                  Key(103, SKILL_NODE_MODIFIER, 0, 3, 102, 4, "Heavy Hand", "5% more Physical damage per rank while you wield a two-handed weapon.", 20111, KEY_HEAVY_HAND),
                  Ail(104, 0, 4, 103, 3, "Rending Strikes", "Your Crusader hits have an 8% chance per rank to cause Bleeding.", 772, TAG_PHYSICAL, 8),
                  Key(105, SKILL_NODE_CAPSTONE, 0, 5, 104, 1, "Whirling Strikes", "Every 4th swing also strikes every enemy around you, for 75% of the hit.", 1680, KEY_WHIRLING_STRIKES),
                  Kit(106, 1, 1, 0, 3, "Sweeping Seal", "Seal of Righteousness strikes every enemy in front of you; the extras take 20/35/50%.", 20154,
                      KIT_ARC, 20154, { 0, 0, 0 }, { 0, 0, 0 }, { 20, 35, 50 }),
                  Key(107, SKILL_NODE_MODIFIER, 1, 2, 106, 4, "Holy Edge", "6% more Seal damage per rank.", 20101, KEY_HOLY_EDGE),
                  Ail(108, 1, 3, 107, 3, "Judging Seals", "Your Crusader hits have an 8% chance per rank to Judge.", 20271, TAG_HOLY, 8),
                  Key(109, SKILL_NODE_SYNERGY, 1, 4, 108, 3, "Mana Strike", "Your Seal hits give you 1% of your maximum mana per rank.", 19742, KEY_MANA_STRIKE),
                  Key(110, SKILL_NODE_CAPSTONE, 1, 5, 109, 1, "Twin Seals", "Two different Seals can be on you at once.", 20164, KEY_TWIN_SEALS),
                  Key(111, SKILL_NODE_MODIFIER, 2, 1, 0, 4, "Quickened", "+4% attack speed per rank.", 19740, KEY_QUICKENED),
                  Key(112, SKILL_NODE_MODIFIER, 2, 2, 111, 3, "Momentum", "Each Crusader hit within 3 sec of the last gives 1% more Crusader damage per rank, up to 5 hits.", 12294, KEY_MOMENTUM),
                  Key(113, SKILL_NODE_MODIFIER, 2, 3, 112, 3, "Zeal", "+10% attack speed per rank while Seal of the Crusader is on you.", 21082, KEY_ZEAL),
                  Key(114, SKILL_NODE_SYNERGY, 2, 4, 113, 1, "Light of the Crusader", "Your Seal hits heal you for 10% of their damage.", 19977, KEY_LIGHT_OF_THE_CRUSADER),
                  Key(115, SKILL_NODE_CAPSTONE, 2, 5, 114, 1, "Shockwave", "Your critical swings send a shockwave 10 yards ahead, for 40% of the hit.", 20549, KEY_SHOCKWAVE_STRIKE),
              } },
            { SKILL_HOLY_LIGHT, CLASS_PALADIN, "Holy Light", "Holy Light and Flash of Light: heals, which its tree turns on your enemies too.", 635, { "Holy Light", "Flash of Light" },
              { "Radiance", "", "Retribution" },
              {
                  Mod(201, 0, 1, 0, 4, "Healing Light", "6% more healing per rank.", 20237, SPELLMOD_DAMAGE, SPELLMOD_PCT, 6),
                  Mod(202, 0, 2, 201, 3, "Light's Grace", "+4% critical strike chance per rank.", 20208, SPELLMOD_CRITICAL_CHANCE, SPELLMOD_FLAT, 4),
                  Mod(203, 0, 3, 202, 3, "Swift Light", "-0.2 sec cast time per rank.", 19750, SPELLMOD_CASTING_TIME, SPELLMOD_FLAT, -200),
                  Mod(204, 0, 4, 203, 3, "Divine Grace", "10% less mana cost per rank.", 20166, SPELLMOD_COST, SPELLMOD_PCT, -10),
                  Key(205, SKILL_NODE_CAPSTONE, 0, 5, 204, 1, "Dawnbringer", "Your heals on yourself also send half their amount at the nearest enemy as a Holy bolt.", 19977, KEY_DAWNBRINGER),
                  Key(206, SKILL_NODE_SYNERGY, 2, 1, 0, 3, "Searing Radiance", "Your heals strike the nearest enemy within 20 yards for 15% of the heal per rank, as Holy damage.", 879, KEY_LIGHT_STRIKE),
                  Key(207, SKILL_NODE_SYNERGY, 2, 2, 206, 2, "Judging Light", "Your heals Judge every enemy within 8 yards, as a hit of 25% of the heal per rank (once every 3 sec).", 20271, KEY_JUDGING_LIGHT),
                  Key(208, SKILL_NODE_MODIFIER, 2, 3, 207, 3, "Avenging Light", "Healing yourself gives you 5% more damage per rank for 5 sec.", 20218, KEY_AVENGING_LIGHT),
                  Key(209, SKILL_NODE_CAPSTONE, 2, 4, 208, 1, "Holy Nova", "Your heals burst outward: every enemy within 8 yards takes 50% of the heal as Holy damage.", 2812, KEY_HOLY_NOVA),
              } },
            { SKILL_JUDGEMENT, CLASS_PALADIN, "Judgement", "Unleash your Seal on an enemy. Its damage nodes need a Seal whose Judgement deals damage: Righteousness or Command.", 20271, { "Judgement" },
              { "Chain", "Burst", "Verdict" },
              {
                  Kit(301, 0, 1, 0, 3, "Chain of Judgement", "Judgement chains to 1/2/3 more enemies within 10 yards, at 50%.", 20186,
                      KIT_CHAIN, 20271, { 1, 2, 3 }, { 10, 10, 10 }, { 50, 50, 50 }),
                  Key(302, SKILL_NODE_MODIFIER, 0, 2, 301, 3, "Echoing Verdict", "Judgement's chain jumps deal 10% more per rank.", 20185, KEY_ECHOING_VERDICT),
                  Ail(303, 0, 3, 302, 3, "Condemn", "Judgement has a 10% chance per rank to Judge, beside its critical strikes.", 20184, TAG_HOLY, 10),
                  Key(304, SKILL_NODE_CAPSTONE, 0, 4, 303, 1, "Final Verdict", "A kill with Judgement readies Judgement at once.", 20184, KEY_FINAL_VERDICT),
                  Kit(305, 1, 1, 0, 3, "Hammer of Light", "Judgement bursts onto enemies within 4/6/8 yards of its target, at 40%.", 24275,
                      KIT_BURST, 20271, { 0, 0, 0 }, { 4, 6, 8 }, { 40, 40, 40 }),
                  Key(306, SKILL_NODE_MODIFIER, 1, 2, 305, 4, "Radiance", "8% more Judgement damage per rank.", 20218, KEY_RADIANCE),
                  Mod(307, 1, 3, 306, 3, "Severity", "Judgement's critical strikes deal 20% more of their bonus per rank.", 20101, SPELLMOD_CRIT_DAMAGE_BONUS, SPELLMOD_PCT, 20),
                  Key(308, SKILL_NODE_CAPSTONE, 1, 4, 307, 1, "Sentence", "Judgement deals 100% more damage to stunned enemies.", 879, KEY_SENTENCE),
                  Key(309, SKILL_NODE_SYNERGY, 2, 1, 0, 1, "Avenger", "Judgement no longer uses up your Seal, but its cooldown is doubled.", 25780, KEY_AVENGER),
                  Mod(310, 2, 2, 309, 3, "Swift Judgement", "-1 sec Judgement cooldown per rank.", 1044, SPELLMOD_COOLDOWN, SPELLMOD_FLAT, -1000),
                  Mod(311, 2, 3, 310, 3, "Unerring", "+5% Judgement critical strike chance per rank.", 20208, SPELLMOD_CRITICAL_CHANCE, SPELLMOD_FLAT, 5),
                  Key(312, SKILL_NODE_MODIFIER, 2, 4, 311, 2, "Righteous Mind", "Judgement restores 5% of your maximum mana per rank.", 20166, KEY_RIGHTEOUS_MIND),
                  Key(313, SKILL_NODE_CAPSTONE, 2, 5, 312, 1, "Sanctified", "Your Judgement on an enemy in your Consecration bursts onto enemies within 8 yards, at 50%.", 26573, KEY_SANCTIFIED),
              } },
            { SKILL_CONSECRATION, CLASS_PALADIN, "Consecration", "Consecrate the ground beneath you.", 26573, { "Consecration" },
              { "Ground", "Smite", "Refuge" },
              {
                  Mod(401, 0, 1, 0, 4, "Wide Blessing", "+10% Consecration radius per rank. Intellect widens it too.", 2812, SPELLMOD_RADIUS, SPELLMOD_PCT, 10),
                  Mod(402, 0, 2, 401, 3, "Lasting Ground", "+2 sec Consecration duration per rank.", 20925, SPELLMOD_DURATION, SPELLMOD_FLAT, 2000),
                  Key(403, SKILL_NODE_MODIFIER, 0, 3, 402, 4, "Burning Ground", "8% more Consecration damage per rank.", 26573, KEY_BURNING_GROUND),
                  Key(404, SKILL_NODE_CAPSTONE, 0, 4, 403, 1, "Walking Consecration", "Your Consecration follows you.", 1044, KEY_WALKING_CONSECRATION),
                  Key(405, SKILL_NODE_SYNERGY, 1, 1, 0, 2, "Searing Light", "Your Holy damage is 5% more per rank on enemies standing in your Consecration.", 20218, KEY_SEARING_LIGHT),
                  Mod(406, 1, 2, 405, 3, "Swift Consecration", "-1 sec Consecration cooldown per rank.", 1044, SPELLMOD_COOLDOWN, SPELLMOD_FLAT, -1000),
                  Key(407, SKILL_NODE_CAPSTONE, 1, 3, 406, 1, "Sacred Seal", "Your Seals strike twice on enemies in your Consecration.", 20154, KEY_SACRED_SEAL),
                  Key(408, SKILL_NODE_MODIFIER, 2, 1, 0, 3, "Hallowed Ground", "While you stand in your Consecration, you regain 1% of your health a second per rank.", 633, KEY_HALLOWED_GROUND),
                  Key(409, SKILL_NODE_SYNERGY, 2, 2, 408, 1, "Steadfast", "While you stand in your Consecration, you can't be stunned.", 498, KEY_STEADFAST),
                  Key(410, SKILL_NODE_CAPSTONE, 2, 3, 409, 1, "Martyr's Ward", "Your Retribution Aura burns every enemy within 10 yards each second, doubled.", 7294, KEY_MARTYRS_WARD),
              } },
            { SKILL_HAMMER, CLASS_PALADIN, "Hammers", "Hammer of Justice, the stun, and Hammer of Wrath, the throw, as one.", 853, { "Hammer of Justice", "Hammer of Wrath" },
              { "Thrown", "Stun", "Execute" },
              {
                  Mod(501, 0, 1, 0, 1, "Hurled Hammer", "Hammer of Justice flies 10 yards farther along your aim.", 24275, SPELLMOD_RANGE, SPELLMOD_FLAT, 10, "Hammer of Justice"),
                  Key(502, SKILL_NODE_TRANSFORMER, 0, 2, 501, 3, "Ricochet", "Hammer of Justice bounces on to 1 more enemy within 10 yards per rank.", 10308, KEY_RICOCHET),
                  Key(503, SKILL_NODE_MODIFIER, 0, 3, 502, 3, "Holy Hammer", "Hammer of Justice also deals Holy damage of 30/45/60% of your attack power.", 20218, KEY_HOLY_HAMMER),
                  Key(504, SKILL_NODE_CAPSTONE, 0, 4, 503, 1, "Blessed Hammer", "Hammer of Justice also hurls a blessed hammer at each of the three nearest enemies within 15 yards, for Holy damage of half your attack power.", 19752, KEY_BLESSED_HAMMER),
                  Kit(505, 1, 1, 0, 2, "Shockwave Hammer", "Hammer of Justice also stuns 1/2 more enemies within 8 yards of its target.", 20549,
                      KIT_SPREAD, 853, { 1, 2 }, { 8, 8 }, { 100, 100 }),
                  Mod(506, 1, 2, 505, 3, "Long Stun", "+0.5 sec stun per rank.", 5588, SPELLMOD_DURATION, SPELLMOD_FLAT, 500, "Hammer of Justice"),
                  Mod(507, 1, 3, 506, 3, "Quick Hammer", "-10 sec Hammer of Justice cooldown per rank.", 5589, SPELLMOD_COOLDOWN, SPELLMOD_FLAT, -10000, "Hammer of Justice"),
                  Key(508, SKILL_NODE_CAPSTONE, 1, 4, 507, 1, "Sentence Passed", "Your damage is 15% more on stunned enemies.", 879, KEY_SENTENCE_PASSED),
                  Kit(509, 2, 1, 0, 2, "Hammer Volley", "Hammer of Wrath throws 1/2 more hammers, fanned 15 degrees apart, at 60%.", 24275,
                      KIT_EXTRA_PROJECTILES, 24275, { 1, 2 }, { 15, 15 }, { 60, 60 }),
                  Mod(510, 2, 2, 509, 4, "Weighted Hammer", "8% more Hammer of Wrath damage per rank.", 20218, SPELLMOD_DAMAGE, SPELLMOD_PCT, 8, "Hammer of Wrath"),
                  Mod(511, 2, 3, 510, 3, "Righteous Hammer", "+5% Hammer of Wrath critical strike chance per rank.", 20101, SPELLMOD_CRITICAL_CHANCE, SPELLMOD_FLAT, 5, "Hammer of Wrath"),
                  Kit(512, 2, 4, 511, 3, "Piercing Hammer", "Hammer of Wrath flies on through 2/4/10 enemies in its path, at 60%.", 20186,
                      KIT_PIERCE, 24275, { 2, 4, 10 }, { 0, 0, 0 }, { 60, 60, 60 }),
                  Key(513, SKILL_NODE_CAPSTONE, 2, 5, 512, 1, "Executioner", "Hammer of Wrath deals 100% more damage to enemies below 20% health.", 879, KEY_EXECUTIONER),
              } },
            { SKILL_WRATH, CLASS_PALADIN, "Wrath", "Exorcism, a bolt of holy fire, and Holy Wrath, a nova, as one. Specialised, they strike any foe, not only undead and demons.", 879, { "Exorcism", "Holy Wrath" },
              { "Exorcism", "Nova", "Zeal" },
              {
                  Kit(601, 0, 1, 0, 3, "Holy Fire", "Exorcism bursts onto enemies within 4/6/8 yards of its target, at 35%.", 2812,
                      KIT_BURST, 879, { 0, 0, 0 }, { 4, 6, 8 }, { 35, 35, 35 }),
                  Mod(602, 0, 2, 601, 4, "Searing Word", "10% more Exorcism damage per rank.", 20218, SPELLMOD_DAMAGE, SPELLMOD_PCT, 10, "Exorcism"),
                  Kit(603, 0, 3, 602, 3, "Chain Exorcism", "Exorcism leaps to 1/2/3 more enemies within 10 yards, at 60%.", 20186,
                      KIT_CHAIN, 879, { 1, 2, 3 }, { 10, 10, 10 }, { 60, 60, 60 }),
                  KitCap(604, 0, 4, 603, "Purging Light", "Exorcism's light passes through its target: every enemy within 10 yards behind it takes the full hit.", 19750,
                         KIT_FRAGMENTS, 879, 10, 10.0f, 100),
                  Mod(605, 1, 1, 0, 4, "Wide Wrath", "+12% Holy Wrath radius per rank.", 2812, SPELLMOD_RADIUS, SPELLMOD_PCT, 12, "Holy Wrath"),
                  Mod(606, 1, 2, 605, 3, "Searing Wrath", "10% more Holy Wrath damage per rank.", 20218, SPELLMOD_DAMAGE, SPELLMOD_PCT, 10, "Holy Wrath"),
                  Kit(607, 1, 3, 606, 2, "Shattering Wrath", "Each enemy Holy Wrath strikes bursts onto those within 4/6 yards, at 20/30%.", 24275,
                      KIT_BURST, 2812, { 0, 0 }, { 4, 6 }, { 20, 30 }),
                  Key(608, SKILL_NODE_CAPSTONE, 1, 4, 607, 1, "Wrathful", "Holy Wrath fires on its own whenever 4 or more enemies are within 10 yards, every 12 sec at most.", 20925, KEY_WRATHFUL),
                  Mod(609, 2, 1, 0, 3, "Swift Exorcism", "-2 sec Exorcism cooldown per rank.", 1044, SPELLMOD_COOLDOWN, SPELLMOD_FLAT, -2000, "Exorcism"),
                  Mod(610, 2, 2, 609, 3, "Swift Wrath", "-8 sec Holy Wrath cooldown per rank.", 1044, SPELLMOD_COOLDOWN, SPELLMOD_FLAT, -8000, "Holy Wrath"),
                  Mod(611, 2, 3, 610, 3, "Exorcist", "+5% critical strike chance per rank with Exorcism and Holy Wrath.", 20208, SPELLMOD_CRITICAL_CHANCE, SPELLMOD_FLAT, 5),
                  Mod(612, 2, 4, 611, 3, "Quick Wrath", "-0.4 sec Holy Wrath cast time per rank.", 20166, SPELLMOD_CASTING_TIME, SPELLMOD_FLAT, -400, "Holy Wrath"),
                  KitCap(613, 2, 5, 612, "Twin Exorcism", "Exorcism fires a second bolt beside the first, at 70%.", 879,
                         KIT_EXTRA_PROJECTILES, 879, 1, 12.0f, 70),
              } },
            // --- The mage's six. A class's nodes take their own numbers within each skill's hundred
            // (the paladin's 1-19, the mage's 20-39), so no two classes share a Codex page. ---
            { SKILL_FIREBALL, CLASS_MAGE, "Fireball", "Fireball and Pyroblast: the mage's bolt and its great charge.", 133, { "Fireball", "Pyroblast" },
              { "Volley", "Inferno", "Pyre" },
              {
                  Kit(121, 0, 1, 0, 3, "Twin Fireball", "Fireball launches 1/1/2 extra fireballs, at 45/60/60%.", 133,
                      KIT_EXTRA_PROJECTILES, 133, { 1, 1, 2 }, { 14, 14, 14 }, { 45, 60, 60 }),
                  Mod(122, 0, 2, 121, 3, "Quick Fireball", "-0.15 sec Fireball cast time per rank.", 12043, SPELLMOD_CASTING_TIME, SPELLMOD_FLAT, -150, "Fireball"),
                  Kit(123, 0, 3, 122, 3, "Piercing Flame", "Fireball flies through 1/2/4 enemies, at 60%.", 2136,
                      KIT_PIERCE, 133, { 1, 2, 4 }, { 0, 0, 0 }, { 60, 60, 60 }),
                  KitCap(124, 0, 4, 123, "Flame Echo", "Every 3rd Fireball is echoed for free, at 70%.", 11129, KIT_ECHO, 133, 3, 0.0f, 70),
                  Mod(125, 1, 1, 0, 4, "Searing Bolt", "6% more Fireball and Pyroblast damage per rank.", 11366, SPELLMOD_DAMAGE, SPELLMOD_PCT, 6),
                  Ail(126, 1, 2, 125, 3, "Kindle", "Your Fireball and Pyroblast hits have an 8% chance per rank to Ignite, beside their critical strikes.", 2120, TAG_FIRE, 8),
                  Mod(127, 1, 3, 126, 3, "Lingering Flame", "Fireball's and Pyroblast's burn deals 15% more per rank.", 2948, SPELLMOD_DOT, SPELLMOD_PCT, 15),
                  Key(128, SKILL_NODE_CAPSTONE, 1, 4, 127, 1, "Pyroclasm", "Critical Fireballs and Pyroblasts explode: every enemy within 8 yards of the target takes 60% of the hit.", 11113, KEY_PYROCLASM),
                  Mod(129, 2, 1, 0, 3, "Quick Pyre", "-0.5 sec Pyroblast cast time per rank.", 12043, SPELLMOD_CASTING_TIME, SPELLMOD_FLAT, -500, "Pyroblast"),
                  Mod(130, 2, 2, 129, 3, "Hot Streak", "+5% Pyroblast critical strike chance per rank.", 11366, SPELLMOD_CRITICAL_CHANCE, SPELLMOD_FLAT, 5, "Pyroblast"),
                  Mod(131, 2, 3, 130, 3, "Pyre", "Pyroblast's critical strikes deal 20% more of their bonus per rank.", 11129, SPELLMOD_CRIT_DAMAGE_BONUS, SPELLMOD_PCT, 20, "Pyroblast"),
                  KitCap(132, 2, 4, 131, "Cataclysm", "Pyroblast bursts onto every enemy within 10 yards of its target, at 60%.", 11113, KIT_BURST, 11366, 0, 10.0f, 60),
              } },
            { SKILL_FROSTBOLT, CLASS_MAGE, "Frostbolt", "Frostbolt and Cone of Cold: cold that slows, chills and shatters.", 116, { "Frostbolt", "Cone of Cold" },
              { "Shards", "Winter", "Cone" },
              {
                  Kit(221, 0, 1, 0, 3, "Splintering Bolt", "Frostbolt shatters into 2/3/4 ice shards that strike the nearest enemies within 10 yards, at 35%.", 116,
                      KIT_FRAGMENTS, 116, { 2, 3, 4 }, { 10, 10, 10 }, { 35, 35, 35 }),
                  Mod(222, 0, 2, 221, 4, "Bitter Cold", "6% more Frostbolt damage per rank.", 116, SPELLMOD_DAMAGE, SPELLMOD_PCT, 6, "Frostbolt"),
                  Kit(223, 0, 3, 222, 2, "Glacial Chain", "Frostbolt chains to 1/2 more enemies within 10 yards, at 50%.", 6143,
                      KIT_CHAIN, 116, { 1, 2 }, { 10, 10 }, { 50, 50 }),
                  KitCap(224, 0, 4, 223, "Echoing Frost", "Every 3rd Frostbolt is echoed for free, at 60%.", 12472, KIT_ECHO, 116, 3, 0.0f, 60),
                  Ail(225, 1, 1, 0, 3, "Deep Chill", "Your Frostbolt and Cone of Cold hits have a 10% chance per rank to chill; a critical hit on the chilled freezes.", 12484, TAG_FROST, 10),
                  Mod(226, 1, 2, 225, 3, "Brittle", "+4% critical strike chance per rank.", 12494, SPELLMOD_CRITICAL_CHANCE, SPELLMOD_FLAT, 4),
                  Mod(227, 1, 3, 226, 3, "Razor Ice", "Critical strikes deal 20% more of their bonus per rank.", 11958, SPELLMOD_CRIT_DAMAGE_BONUS, SPELLMOD_PCT, 20),
                  Key(228, SKILL_NODE_CAPSTONE, 1, 4, 227, 1, "Shatterstorm", "Frostbolt on a frozen enemy shatters: every enemy within 8 yards of it takes 80% of the hit.", 122, KEY_SHATTERSTORM),
                  Mod(229, 2, 1, 0, 3, "Wide Cone", "+15% Cone of Cold area per rank.", 120, SPELLMOD_RADIUS, SPELLMOD_PCT, 15, "Cone of Cold"),
                  Mod(230, 2, 2, 229, 3, "Biting Cone", "10% more Cone of Cold damage per rank.", 120, SPELLMOD_DAMAGE, SPELLMOD_PCT, 10, "Cone of Cold"),
                  Mod(231, 2, 3, 230, 3, "Swift Cone", "-1.5 sec Cone of Cold cooldown per rank.", 12472, SPELLMOD_COOLDOWN, SPELLMOD_FLAT, -1500, "Cone of Cold"),
                  Key(232, SKILL_NODE_CAPSTONE, 2, 4, 231, 1, "Deep Freeze", "Cone of Cold freezes every enemy it hits.", 122, KEY_DEEP_FREEZE),
              } },
            { SKILL_ARCANE_MISSILES, CLASS_MAGE, "Arcane Missiles", "A channelled stream of arcane bolts.", 5143, { "Arcane Missiles", "Arcane Missile" },
              { "Barrage", "Flux", "Focus" },
              {
                  Kit(321, 0, 1, 0, 3, "Seeking Missiles", "Each missile jumps to 1/1/2 more enemies within 8 yards, at 40/55/55%.", 5143,
                      KIT_CHAIN, 5143, { 1, 1, 2 }, { 8, 8, 8 }, { 40, 55, 55 }),
                  Mod(322, 0, 2, 321, 4, "Empowered Missiles", "6% more Arcane Missiles damage per rank.", 5143, SPELLMOD_DAMAGE, SPELLMOD_PCT, 6),
                  Kit(323, 0, 3, 322, 2, "Piercing Missiles", "Each missile flies through 1/2 enemies, at 50%.", 1953,
                      KIT_PIERCE, 5143, { 1, 2 }, { 0, 0 }, { 50, 50 }),
                  KitCap(324, 0, 4, 323, "Arcane Barrage", "Each missile bursts onto enemies within 4 yards of its target, at 30%.", 1449, KIT_BURST, 5143, 0, 4.0f, 30),
                  Ail(325, 1, 1, 0, 3, "Unstable Missiles", "Each missile has a 6% chance per rank to Overload.", 12042, TAG_ARCANE, 6),
                  Mod(326, 1, 2, 325, 3, "Sharp Mind", "+3% critical strike chance per rank.", 12043, SPELLMOD_CRITICAL_CHANCE, SPELLMOD_FLAT, 3),
                  Mod(327, 1, 3, 326, 3, "Arcane Thrift", "10% less mana cost per rank.", 12051, SPELLMOD_COST, SPELLMOD_PCT, -10),
                  Key(328, SKILL_NODE_CAPSTONE, 1, 4, 327, 1, "Overcharge", "Your Overload bursts deal 40% more.", 12042, KEY_OVERCHARGE),
                  Mod(329, 2, 1, 0, 3, "Focused Mind", "Critical strikes deal 15% more of their bonus per rank.", 12043, SPELLMOD_CRIT_DAMAGE_BONUS, SPELLMOD_PCT, 15),
                  Mod(330, 2, 2, 329, 2, "Long Reach", "+10% Arcane Missiles range per rank.", 1953, SPELLMOD_RANGE, SPELLMOD_PCT, 10),
                  Key(331, SKILL_NODE_CAPSTONE, 2, 3, 330, 1, "Arcane Siphon", "Each missile that hits restores 0.5% of your maximum mana.", 12051, KEY_ARCANE_SIPHON),
              } },
            { SKILL_ARCANE_EXPLOSION, CLASS_MAGE, "Arcane Explosion", "A burst of arcane power all around you.", 1449, { "Arcane Explosion" },
              { "Nova", "Fury", "Mana" },
              {
                  Mod(421, 0, 1, 0, 4, "Wide Explosion", "+10% Arcane Explosion radius per rank. Intellect widens it too.", 1449, SPELLMOD_RADIUS, SPELLMOD_PCT, 10),
                  Mod(422, 0, 2, 421, 4, "Arcane Force", "6% more Arcane Explosion damage per rank.", 12042, SPELLMOD_DAMAGE, SPELLMOD_PCT, 6),
                  Ail(423, 0, 3, 422, 3, "Volatile", "Arcane Explosion has a 10% chance per rank to Overload each enemy it hits.", 1449, TAG_ARCANE, 10),
                  AilCap(424, 0, 4, 423, "Detonation", "Arcane Explosion Overloads every enemy it hits.", 12042, TAG_ARCANE, 100),
                  Mod(425, 1, 1, 0, 3, "Unstable Power", "+3% Arcane Explosion critical strike chance per rank.", 12043, SPELLMOD_CRITICAL_CHANCE, SPELLMOD_FLAT, 3),
                  Mod(426, 1, 2, 425, 3, "Arcane Fury", "Critical strikes deal 20% more of their bonus per rank.", 12042, SPELLMOD_CRIT_DAMAGE_BONUS, SPELLMOD_PCT, 20),
                  ModCap(427, 1, 3, 426, "Arcane Nova", "Arcane Explosion's radius is 40% larger.", 1449, SPELLMOD_RADIUS, SPELLMOD_PCT, 40),
                  Mod(428, 2, 1, 0, 3, "Mana Efficiency", "10% less Arcane Explosion mana cost per rank.", 12051, SPELLMOD_COST, SPELLMOD_PCT, -10),
                  Mod(429, 2, 2, 428, 3, "Resonance", "5% more Arcane Explosion damage per rank.", 1008, SPELLMOD_DAMAGE, SPELLMOD_PCT, 5),
                  Key(430, SKILL_NODE_CAPSTONE, 2, 3, 429, 1, "Mana Burst", "Each enemy Arcane Explosion hits restores 1% of your maximum mana, 5% at most a cast.", 12051, KEY_MANA_BURST),
              } },
            { SKILL_STORMS, CLASS_MAGE, "Storms", "Blizzard and Flamestrike: storms of ice and fire on the ground.", 10, { "Blizzard", "Flamestrike" },
              { "Blizzard", "Storm", "Flamestrike" },
              {
                  Mod(521, 0, 1, 0, 3, "Wide Blizzard", "+12% Blizzard radius per rank.", 10, SPELLMOD_RADIUS, SPELLMOD_PCT, 12, "Blizzard"),
                  Mod(522, 0, 2, 521, 4, "Biting Storm", "8% more Blizzard damage per rank.", 10, SPELLMOD_DOT, SPELLMOD_PCT, 8, "Blizzard"),
                  Mod(523, 0, 3, 522, 3, "Thrifty Storm", "10% less Blizzard mana cost per rank.", 12051, SPELLMOD_COST, SPELLMOD_PCT, -10, "Blizzard"),
                  Key(524, SKILL_NODE_CAPSTONE, 0, 4, 523, 1, "Glacial Storm", "Blizzard chills every enemy it strikes, and each strike has a 15% chance to freeze one chilled already.", 12484, KEY_GLACIAL_STORM),
                  Mod(525, 1, 1, 0, 3, "Stormcaller", "10% less mana cost per rank.", 12051, SPELLMOD_COST, SPELLMOD_PCT, -10),
                  Mod(526, 1, 2, 525, 3, "Storm Surge", "+8% radius per rank.", 10, SPELLMOD_RADIUS, SPELLMOD_PCT, 8),
                  Mod(527, 1, 3, 526, 3, "Raging Storm", "Blizzard's strikes and Flamestrike's burn deal 6% more per rank.", 2120, SPELLMOD_DOT, SPELLMOD_PCT, 6),
                  ModCap(528, 1, 4, 527, "Eye of the Storm", "Blizzard and Flamestrike are 30% wider.", 10, SPELLMOD_RADIUS, SPELLMOD_PCT, 30),
                  Mod(529, 2, 1, 0, 3, "Quick Flamestrike", "-0.3 sec Flamestrike cast time per rank.", 2120, SPELLMOD_CASTING_TIME, SPELLMOD_FLAT, -300, "Flamestrike"),
                  Mod(530, 2, 2, 529, 3, "Searing Ground", "Flamestrike's burn deals 15% more per rank.", 2120, SPELLMOD_DOT, SPELLMOD_PCT, 15, "Flamestrike"),
                  Ail(531, 2, 3, 530, 3, "Kindling Strike", "Flamestrike's hits have a 10% chance per rank to Ignite.", 2120, TAG_FIRE, 10),
                  ModCap(532, 2, 4, 531, "Meteor", "Flamestrike is instant.", 11113, SPELLMOD_CASTING_TIME, SPELLMOD_PCT, -100, "Flamestrike"),
              } },
            { SKILL_FIRE_BLAST, CLASS_MAGE, "Fire Blast", "Fire Blast and Scorch: quick fire, close in.", 2136, { "Fire Blast", "Scorch" },
              { "Blast", "Scorch", "Combust" },
              {
                  Mod(621, 0, 1, 0, 3, "Swift Blast", "-1 sec Fire Blast cooldown per rank.", 2136, SPELLMOD_COOLDOWN, SPELLMOD_FLAT, -1000, "Fire Blast"),
                  Kit(622, 0, 2, 621, 3, "Explosive Blast", "Fire Blast bursts onto enemies within 4/6/8 yards of its target, at 40%.", 11113,
                      KIT_BURST, 2136, { 0, 0, 0 }, { 4, 6, 8 }, { 40, 40, 40 }),
                  Mod(623, 0, 3, 622, 3, "Incineration", "+5% Fire Blast critical strike chance per rank.", 2136, SPELLMOD_CRITICAL_CHANCE, SPELLMOD_FLAT, 5, "Fire Blast"),
                  Key(624, SKILL_NODE_CAPSTONE, 0, 4, 623, 1, "Chain Blast", "A kill with Fire Blast readies Fire Blast at once.", 11129, KEY_CHAIN_BLAST),
                  Mod(625, 1, 1, 0, 3, "Quick Scorch", "-0.2 sec Scorch cast time per rank.", 2948, SPELLMOD_CASTING_TIME, SPELLMOD_FLAT, -200, "Scorch"),
                  Kit(626, 1, 2, 625, 2, "Arcing Scorch", "Scorch chains to 1/2 more enemies within 10 yards, at 60%.", 2948,
                      KIT_CHAIN, 2948, { 1, 2 }, { 10, 10 }, { 60, 60 }),
                  Ail(627, 1, 3, 626, 3, "Scorching", "Your Fire Blast and Scorch hits have a 10% chance per rank to Ignite.", 2120, TAG_FIRE, 10),
                  Key(628, SKILL_NODE_CAPSTONE, 1, 4, 627, 1, "Searing Brand", "Scorch brands its target with Fire Vulnerability: 3% more Fire damage taken per stack, up to 5.", 2948, KEY_SEARING_BRAND),
                  Mod(629, 2, 1, 0, 3, "Flashpoint", "Critical strikes deal 15% more of their bonus per rank.", 11129, SPELLMOD_CRIT_DAMAGE_BONUS, SPELLMOD_PCT, 15),
                  Mod(630, 2, 2, 629, 4, "Blazing", "6% more Fire Blast and Scorch damage per rank.", 2136, SPELLMOD_DAMAGE, SPELLMOD_PCT, 6),
                  Key(631, SKILL_NODE_CAPSTONE, 2, 3, 630, 1, "Detonate", "Fire Blast sets off your Ignite on its target at once: the burn left strikes it and every enemy within 6 yards.", 11113, KEY_DETONATE),
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
        { 105, 3975,  "Herod, Scarlet Monastery" },                     // Whirling Strikes
        { 110, 6487,  "Arcanist Doan, Scarlet Monastery" },             // Twin Seals
        { 115, 7228,  "Ironaya, Uldaman" },                             // Shockwave
        { 205, 10429, "Warchief Rend Blackhand, Upper Blackrock Spire" },// Dawnbringer
        { 209, 4542,  "High Inquisitor Fairbanks, Scarlet Monastery" }, // Holy Nova
        { 304, 3976,  "Scarlet Commander Mograine, Scarlet Monastery" },// Final Verdict
        { 308, 9019,  "Emperor Dagran Thaurissan, Blackrock Depths" },  // Sentence
        { 404, 3977,  "High Inquisitor Whitemane, Scarlet Monastery" }, // Walking Consecration
        { 407, 10813, "Balnazzar, Stratholme" },                        // Sacred Seal
        { 410, 646,   "Mr. Smite, the Deadmines" },                     // Martyr's Ward
        { 504, 10440, "Baron Rivendare, Stratholme" },                  // Blessed Hammer
        { 508, 9568,  "Overlord Wyrmthalak, Lower Blackrock Spire" },   // Sentence Passed
        { 513, 9816,  "Pyroguard Emberseer, Upper Blackrock Spire" },   // Executioner
        { 604, 10508, "Ras Frostwhisper, Scholomance" },                // Purging Light
        { 608, 10811, "Archivist Galford, Stratholme" },                // Wrathful
        { 613, 10435, "Magistrate Barthilas, Stratholme" },             // Twin Exorcism
        // The mage's
        { 124, 4275,  "Archmage Arugal, Shadowfang Keep" },             // Flame Echo
        { 128, 9816,  "Pyroguard Emberseer, Upper Blackrock Spire" },   // Pyroclasm
        { 132, 9019,  "Emperor Dagran Thaurissan, Blackrock Depths" },  // Cataclysm
        { 224, 7358,  "Amnennar the Coldbringer, Razorfen Downs" },     // Echoing Frost
        { 228, 10508, "Ras Frostwhisper, Scholomance" },                // Shatterstorm
        { 232, 1853,  "Darkmaster Gandling, Scholomance" },             // Deep Freeze
        { 324, 6487,  "Arcanist Doan, Scarlet Monastery" },             // Arcane Barrage
        { 328, 7800,  "Mekgineer Thermaplugg, Gnomeregan" },            // Overcharge
        { 331, 11496, "Immol'thar, Dire Maul" },                        // Arcane Siphon
        { 424, 2748,  "Archaedas, Uldaman" },                           // Detonation
        { 427, 11486, "Prince Tortheldrin, Dire Maul" },                // Arcane Nova
        { 430, 10813, "Balnazzar, Stratholme" },                        // Mana Burst
        { 524, 10440, "Baron Rivendare, Stratholme" },                  // Glacial Storm
        { 528, 3654,  "Mutanus the Devourer, Wailing Caverns" },        // Eye of the Storm
        { 532, 10429, "Warchief Rend Blackhand, Upper Blackrock Spire" },// Meteor
        { 624, 639,   "Edwin VanCleef, the Deadmines" },                // Chain Blast
        { 628, 10811, "Archivist Galford, Stratholme" },                // Searing Brand
        { 631, 9568,  "Overlord Wyrmthalak, Lower Blackrock Spire" },   // Detonate
    };

    // Revision 2 of the trees renumbered the capstones: a page read for an old one is read for its
    // new one (Light of the Crusader's and Crusader's Pace's became Holy Nova's and Martyr's Ward's,
    // Storm of Hammers' Dawnbringer's).
    std::pair<uint16, uint16> const PAGE_RENUMBER[] =
    {
        { 103, 105 }, { 106, 410 }, { 109, 115 }, { 203, 110 }, { 209, 209 }, { 303, 304 }, { 306, 308 },
        { 403, 404 }, { 409, 407 }, { 503, 504 }, { 508, 508 }, { 603, 604 }, { 609, 613 }, { 703, 608 },
        { 803, 205 }, { 809, 513 },
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
        uint8 mageFits = 0;       // `fits` for a mage's skills (`fits` is a paladin's)
    };

    // The skills of `classId`'s a rune fits: a bit per SkillId (0: none, a rune of another class).
    uint8 FitsFor(RuneDef const& rune, uint8 classId)
    {
        switch (classId)
        {
            case CLASS_PALADIN: return rune.fits;
            case CLASS_MAGE: return rune.mageFits;
            default: return 0;
        }
    }

    // A rune fits skills 1-7 (the wire's mask is a byte); later skills take none yet.
    constexpr uint8 FIT(SkillId id) { return id < 8 ? uint8(1u << id) : uint8(0); }

    std::vector<RuneDef> const& Runes()
    {
        static std::vector<RuneDef> const runes =
        {
            { RUNE_CHAINS, "Rune of Chains", "The skill's hit chains to 1 more enemy within 10 yards, at 50% (one more jump on a chain its tree gives).", 20186,
              uint8(FIT(SKILL_JUDGEMENT) | FIT(SKILL_WRATH)), RUNE_KIT, KIT_CHAIN, 1, 10.0f, 50, {}, uint8(FIT(SKILL_FIREBALL) | FIT(SKILL_FROSTBOLT) | FIT(SKILL_ARCANE_MISSILES) | FIT(SKILL_FIRE_BLAST)) },
            { RUNE_SHATTERING, "Rune of Shattering", "The skill's hits burst onto enemies within 6 yards of the target, at 35% (+15% on a burst its tree gives).", 24275,
              uint8(FIT(SKILL_JUDGEMENT) | FIT(SKILL_STRIKE) | FIT(SKILL_WRATH)), RUNE_KIT, KIT_BURST, 0, 6.0f, 35, {}, uint8(FIT(SKILL_FIREBALL) | FIT(SKILL_FROSTBOLT) | FIT(SKILL_ARCANE_MISSILES) | FIT(SKILL_FIRE_BLAST)) },
            { RUNE_EXPANSE, "Rune of Expanse", "+30% area.", 2812,
              uint8(FIT(SKILL_CONSECRATION) | FIT(SKILL_WRATH)), RUNE_MODS, UniqueKit(0), 0, 0.0f, 0, { std::make_tuple(uint8(SPELLMOD_RADIUS), uint8(SPELLMOD_PCT), int32(30)) },
              uint8(FIT(SKILL_FROSTBOLT) | FIT(SKILL_ARCANE_EXPLOSION) | FIT(SKILL_STORMS)) },
            { RUNE_LINGERING, "Rune of Lingering", "+50% duration.", 20925,
              uint8(FIT(SKILL_CONSECRATION) | FIT(SKILL_HAMMER)), RUNE_MODS, UniqueKit(0), 0, 0.0f, 0, { std::make_tuple(uint8(SPELLMOD_DURATION), uint8(SPELLMOD_PCT), int32(50)) }, uint8(FIT(SKILL_STORMS)) },
            { RUNE_HASTE, "Rune of Haste", "-25% cooldown, -15% damage.", 1044,
              uint8(FIT(SKILL_JUDGEMENT) | FIT(SKILL_CONSECRATION) | FIT(SKILL_HAMMER) | FIT(SKILL_WRATH) | FIT(SKILL_HOLY_LIGHT)), RUNE_MODS, UniqueKit(0), 0, 0.0f, 0,
              { std::make_tuple(uint8(SPELLMOD_COOLDOWN), uint8(SPELLMOD_PCT), int32(-25)), std::make_tuple(uint8(SPELLMOD_DAMAGE), uint8(SPELLMOD_PCT), int32(-15)),
                std::make_tuple(uint8(SPELLMOD_DOT), uint8(SPELLMOD_PCT), int32(-15)) },
              uint8(FIT(SKILL_FROSTBOLT) | FIT(SKILL_FIRE_BLAST)) },
            { RUNE_LEECH, "Rune of Leech", "5% of the skill's damage heals you.", 20166,
              uint8(FIT(SKILL_STRIKE) | FIT(SKILL_JUDGEMENT) | FIT(SKILL_WRATH) | FIT(SKILL_HAMMER)), RUNE_HOOK, UniqueKit(0), 0, 0.0f, 5, {}, uint8(FIT(SKILL_FIREBALL) | FIT(SKILL_FROSTBOLT) | FIT(SKILL_ARCANE_MISSILES) | FIT(SKILL_ARCANE_EXPLOSION) | FIT(SKILL_STORMS) | FIT(SKILL_FIRE_BLAST)) },
            { RUNE_COMMAND, "Rune of Command", "The stun spreads to 1 more enemy within 8 yards.", 20549,
              uint8(FIT(SKILL_HAMMER)), RUNE_KIT, KIT_SPREAD, 1, 8.0f, 100, {} },
            { RUNE_SANCTITY, "Rune of Sanctity", "+15% damage.", 20218,
              uint8(FIT(SKILL_STRIKE) | FIT(SKILL_JUDGEMENT) | FIT(SKILL_CONSECRATION) | FIT(SKILL_WRATH) | FIT(SKILL_HAMMER) | FIT(SKILL_HOLY_LIGHT)), RUNE_MODS, UniqueKit(0), 0, 0.0f, 0,
              { std::make_tuple(uint8(SPELLMOD_DAMAGE), uint8(SPELLMOD_PCT), int32(15)), std::make_tuple(uint8(SPELLMOD_DOT), uint8(SPELLMOD_PCT), int32(15)) },
              uint8(FIT(SKILL_FIREBALL) | FIT(SKILL_FROSTBOLT) | FIT(SKILL_ARCANE_MISSILES) | FIT(SKILL_ARCANE_EXPLOSION) | FIT(SKILL_STORMS) | FIT(SKILL_FIRE_BLAST)) },
            { RUNE_FURY, "Rune of Fury", "Your swing strikes one rank wider (Wide Swing +1, or 20% to every enemy in front).", 845,
              uint8(FIT(SKILL_STRIKE)), RUNE_KEY, UniqueKit(0), 0, 0.0f, 0, {} },
            // Cross-skill procs: the proc engine (Arpg/ArpgProcs.h) reads the socket.
            { RUNE_GRACE, "Rune of Grace", ProcRuneText(RUNE_GRACE), 19750,
              uint8(FIT(SKILL_STRIKE)), RUNE_HOOK, UniqueKit(0), 0, 0.0f, 0, {} },
            { RUNE_VERDICT, "Rune of Verdict", ProcRuneText(RUNE_VERDICT), 20271,
              uint8(FIT(SKILL_JUDGEMENT)), RUNE_HOOK, UniqueKit(0), 0, 0.0f, 0, {} },
            { RUNE_ANVIL, "Rune of the Anvil", ProcRuneText(RUNE_ANVIL), 853,
              uint8(FIT(SKILL_HAMMER)), RUNE_HOOK, UniqueKit(0), 0, 0.0f, 0, {} },
            { RUNE_FERVOUR, "Rune of Fervour", ProcRuneText(RUNE_FERVOUR), 2812,
              uint8(FIT(SKILL_WRATH)), RUNE_HOOK, UniqueKit(0), 0, 0.0f, 0, {} },
            { RUNE_ZEAL, "Rune of Zeal", ProcRuneText(RUNE_ZEAL), 20154,
              uint8(FIT(SKILL_STRIKE)), RUNE_HOOK, UniqueKit(0), 0, 0.0f, 0, {} },
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

    // A mage's skills' kit spells, for its kit runes.
    std::vector<uint32> MageKitSpellsOf(SkillId skill)
    {
        switch (skill)
        {
            case SKILL_FIREBALL: return { 133, 11366 };     // Fireball, Pyroblast
            case SKILL_FROSTBOLT: return { 116 };
            case SKILL_ARCANE_MISSILES: return { 5143 };
            case SKILL_FIRE_BLAST: return { 2136, 2948 };   // Fire Blast, Scorch
            default: return {};
        }
    }

    // The spells a rune's kit row lands on, by skill: the kit spells its tree's own rows use.
    std::vector<uint32> KitSpellsOf(uint8 classId, SkillId skill)
    {
        if (classId == CLASS_MAGE)
            return MageKitSpellsOf(skill);
        if (classId != CLASS_PALADIN)
            return {};
        switch (skill)
        {
            case SKILL_STRIKE: return { 20154, 20375 };     // Seal of Righteousness, of Command
            case SKILL_JUDGEMENT: return { 20271 };
            case SKILL_CONSECRATION: return { 26573 };
            case SKILL_HAMMER: return { 853, 24275 };       // Hammer of Justice, of Wrath
            case SKILL_WRATH: return { 879, 2812 };         // Exorcism, Holy Wrath
            default: return {};
        }
    }

    // The family flags of the class's spells a skill covers, for its spell modifiers.
    // The spell family of a class's spells.
    uint32 FamilyOf(uint8 classId)
    {
        switch (classId)
        {
            case CLASS_PALADIN: return SPELLFAMILY_PALADIN;
            case CLASS_MAGE:    return SPELLFAMILY_MAGE;
            case CLASS_DRUID:   return SPELLFAMILY_DRUID;
            case CLASS_WARLOCK: return SPELLFAMILY_WARLOCK;
            case CLASS_SHAMAN:  return SPELLFAMILY_SHAMAN;
            case CLASS_WARRIOR: return SPELLFAMILY_WARRIOR;
            case CLASS_PRIEST:  return SPELLFAMILY_PRIEST;
            case CLASS_ROGUE:   return SPELLFAMILY_ROGUE;
            case CLASS_HUNTER:  return SPELLFAMILY_HUNTER;
            default:            return SPELLFAMILY_GENERIC;
        }
    }

    // The class's spells named `names` (prefixes at a word boundary), every rank and triggered
    // part: the spells a skill's modifiers reach, by id, and the family bits only they carry,
    // for the client. Classic shares its family bits between unrelated spells (every mage bolt
    // carries one; Exorcism carries none), so the bits alone can't say which spells are meant.
    struct NamedSpells
    {
        uint64 mask = 0;
        std::shared_ptr<std::set<uint32> const> spells;
    };

    NamedSpells NamesMask(uint8 classId, std::vector<char const*> const& names, char const* what)
    {
        static std::mutex lock;
        static std::map<std::string, NamedSpells> cache;
        std::string key = std::to_string(classId);
        for (char const* name : names)
            key += std::string("|") + name;
        std::lock_guard<std::mutex> guard(lock);
        auto it = cache.find(key);
        if (it != cache.end())
            return it->second;
        uint32 const family = FamilyOf(classId);
        uint64 mine = 0, others = 0;
        auto spells = std::make_shared<std::set<uint32>>();
        for (uint32 id = 1; id < sSpellTemplate.GetMaxEntry(); ++id)
        {
            SpellEntry const* spell = sSpellTemplate.LookupEntry<SpellEntry>(id);
            if (!spell || !spell->SpellName[0])
                continue;
            // The class's spells, and generic ones of the name: Exorcism, Holy Wrath and Judgement
            // of Command's hit carry no class family. Only the class's carry the client's bits.
            bool const ownFamily = spell->SpellFamilyName == family;
            if (!ownFamily && spell->SpellFamilyName != SPELLFAMILY_GENERIC)
                continue;
            bool named = false;
            for (char const* want : names)
                named = named || NameMatches(spell->SpellName[0], want);
            if (named)
            {
                spells->insert(id);
                if (ownFamily)
                    mine |= spell->SpellFamilyFlags.Flags;
            }
            else if (ownFamily)
                others |= spell->SpellFamilyFlags.Flags;
        }
        if (spells->empty())
            sLog.outError("ARPG skills: no spell of %s found; its spell modifiers do nothing", what);
        NamedSpells out;
        out.mask = mine & ~others;
        if (!spells->empty())
            out.spells = spells;
        cache[key] = out;
        return out;
    }

    NamedSpells SkillMask(SkillDef const& skill)
    {
        return NamesMask(skill.classId, skill.spells, skill.name);
    }

    // A node's spell modifiers' reach: its one spell, or the skill's every spell.
    NamedSpells NodeMask(SkillDef const& skill, SkillNode const& node)
    {
        return node.only ? NamesMask(skill.classId, { node.only }, node.name) : SkillMask(skill);
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
        std::map<uint8, uint8> gearSent;      // gear's skill levels the client was last sent
        uint8 classId = 0;                    // the character's class (0: not loaded)
        uint32 overSince = 0;                 // when more was first seen spent than allowed (0: not)
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

    // The skill trees' revision, kept as a held thing: a character from an older one has its
    // points back once (LoadSkills).
    constexpr uint16 HELD_TREE_REVISION = 9000;
    constexpr uint32 TREE_REVISION = 2;

    // Points spent beyond what the gear and level allow are given back once that has lasted this
    // long out of combat (SettleSkillPoints).
    constexpr uint32 OVER_GRACE_MS = 10000;

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
            it = rows.emplace(key, UniqueMechanic{ 0, node.kit, node.kitSpell, node.n[i], node.value[i], node.pct[i] + bonusPct, node.text, node.name }).first;
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
            it = rows.emplace(key, UniqueMechanic{ 0, rune.kit, spell, rune.n, rune.value, rune.pct, rune.text, rune.name }).first;
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
                if ((FitsFor(*rune, s.classId) & FIT(SkillId(skill))) && SlotOf(s, skill) >= 0 && SpentIn(s, skill) >= SOCKET_POINTS)
                    out.emplace_back(SkillId(skill), rune);
        return out;
    }
}

namespace Arpg
{
    uint8 GearSkillLevels(Player const* player, uint8 skill)
    {
        std::shared_ptr<WebTotals const> totals = TotalsOf(player);
        if (!totals || !FindSkill(player->getClass(), skill))
            return 0;
        // The skill's tags: its spells' the character knows (Strike: the swing's).
        uint32 tags = skill == SwingSkill(player->getClass()) ? TagsOf(nullptr) : 0;
        for (auto const& [id, known] : player->GetSpellMap())
            if (known.state != PLAYERSPELL_REMOVED)
                if (SpellEntry const* info = sSpellTemplate.LookupEntry<SpellEntry>(id))
                    if (SkillOfSpell(player->getClass(), info) == SkillId(skill))
                        tags |= TagsOf(info);
        uint32 const elemental = TagBit(TAG_FIRE) | TagBit(TAG_FROST) | TagBit(TAG_NATURE) | TagBit(TAG_ARCANE) | TagBit(TAG_SHADOW);
        int32 levels = 0;
        if (tags & TagBit(TAG_MELEE))
            levels += totals->skillLevel[0];
        if (tags & TagBit(TAG_SPELL))
            levels += totals->skillLevel[1];
        if (tags & TagBit(TAG_HOLY))
            levels += totals->skillLevel[2];
        if (tags & elemental)
            levels += totals->skillLevel[3];
        return uint8(std::clamp(levels, 0, 20));
    }

    std::vector<uint8> SlottedSkills(Player const* player)
    {
        PlayerSkills const s = SkillsFor(player);
        std::vector<uint8> out;
        for (uint8 skill : s.slots)
            if (skill)
                out.push_back(skill);
        return out;
    }

    void UpdateSkillGear(Player* player)
    {
        std::map<uint8, uint8> gear;
        for (SkillDef const& skill : Skills())
            if (skill.classId == player->getClass())
                gear[skill.id] = GearSkillLevels(player, skill.id);
        if (gear != SkillsFor(player).gearSent)
            SendSkills(player);
    }

    uint32 SkillPointsFor(uint32 level)
    {
        // One a level from the second: 19 at 20, 59 at 60, against 20 a skill in five slots, so
        // a character fills two or three skills, not all five.
        return level >= 2 ? std::min<uint32>(level, 60) - 1 : 0;
    }

    bool SettleSkillPoints(Player* player)
    {
        // Each skill within its cap (gear's levels raise it), and the points beyond gear's within
        // what the level gives. Gear taken off, or points rebalanced, gives back the deepest
        // ranks first, one at a time, so the rest of the tree stays. Only once the excess has
        // lasted OVER_GRACE_MS out of combat, with the character's totals (and so its gear) known:
        // at login the gear's affixes load after the skills, and a weapon swap passes through
        // a moment with neither weapon worn.
        if (!TotalsOf(player))
        {
            Edit(player, [](PlayerSkills& e) { e.overSince = 0; });
            return false;
        }
        uint8 const classId = player->getClass();
        uint32 const points = SkillPointsFor(player->GetLevel());
        std::map<uint8, uint8> gear;
        for (SkillDef const& skill : Skills())
            if (skill.classId == classId)
                gear[skill.id] = GearSkillLevels(player, skill.id);
        uint32 trimmed = 0;
        for (int guard = 0; guard < 200; ++guard)
        {
            PlayerSkills const s = SkillsFor(player);
            int over = -1;
            uint32 own = 0, most = 0;
            for (auto const& [id, levels] : gear)
            {
                uint32 const spent = SpentIn(s, id);
                if (spent > SKILL_CAP + levels)
                    over = id;
                uint32 const beyond = spent > levels ? spent - levels : 0;
                own += beyond;
                if (over < 0 && beyond > most)
                    most = beyond;
            }
            if (over < 0 && own > points)
                for (auto const& [id, levels] : gear)
                {
                    uint32 const spent = SpentIn(s, id);
                    if ((spent > levels ? spent - levels : 0) == most)
                    {
                        over = id;
                        break;
                    }
                }
            if (over < 0)
            {
                if (!trimmed)
                    Edit(player, [](PlayerSkills& e) { e.overSince = 0; });
                break;
            }
            if (!trimmed)
            {
                uint32 const now = WorldTimer::getMSTime();
                uint32 since = s.overSince;
                if (!since)
                {
                    Edit(player, [now](PlayerSkills& e) { e.overSince = now ? now : 1; });
                    break;
                }
                if (player->IsInCombat() || WorldTimer::getMSTimeDiff(since, now) < OVER_GRACE_MS)
                    break;
            }
            // The deepest ranked node of the skill with nothing ranked below it.
            SkillDef const* skill = FindSkill(classId, uint8(over));
            SkillNode const* leaf = nullptr;
            for (SkillNode const& node : skill->nodes)
            {
                if (!s.ranks.count(node.id))
                    continue;
                bool parent = false;
                for (SkillNode const& child : skill->nodes)
                    parent = parent || (child.parent == node.id && s.ranks.count(child.id));
                if (!parent && (!leaf || node.row > leaf->row))
                    leaf = &node;
            }
            if (!leaf)
                break;
            uint8 const rank = s.ranks.at(leaf->id) - 1;
            Edit(player, [&](PlayerSkills& e)
            {
                if (rank)
                    e.ranks[leaf->id] = rank;
                else
                    e.ranks.erase(leaf->id);
            });
            SaveNode(player, leaf->id, rank);
            ++trimmed;
        }
        if (!trimmed)
            return false;
        Edit(player, [](PlayerSkills& e) { e.overSince = 0; });
        RefreshTotals(player);
        ChatHandler(player).PSendSysMessage("|cffffd200%u skill %s given back: your gear or level no longer holds %s.|r",
                                            trimmed, trimmed == 1 ? "point was" : "points were", trimmed == 1 ? "it" : "them");
        return true;
    }

    SkillId SkillOfSpell(uint8 classId, SpellEntry const* spellInfo)
    {
        if (!spellInfo)
            return SwingSkill(classId);
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
        s.classId = classId;
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
        // Revision 2 of the trees (six paladin skills, renumbered nodes): a character from before
        // has its skill points back, once, its runes back in its bags (unsocketed), the slots
        // whose skill changed (Seals, Exorcism, Holy Wrath, Hammer of Wrath) emptied, and its
        // Codex pages carried to the capstones that took their place.
        if (s.things[HELD_TREE_REVISION] < TREE_REVISION)
        {
            uint32 const guid = player->GetGUIDLow();
            bool const had = !s.ranks.empty() || !s.pages.empty() || !s.sockets.empty();
            s.ranks.clear();
            s.sockets.clear();
            for (uint8& slot : s.slots)
                if (slot == 2 || slot == 6)
                    slot = 0;
            std::set<uint16> pages;
            for (uint16 old : s.pages)
                for (auto const& [from, to] : PAGE_RENUMBER)
                    if (from == old)
                        pages.insert(to);
            s.pages = pages;
            CharacterDatabase.PExecute("DELETE FROM character_arpg_skill_node WHERE guid = %u", guid);
            CharacterDatabase.PExecute("DELETE FROM character_arpg_socket WHERE guid = %u", guid);
            CharacterDatabase.PExecute("DELETE FROM character_arpg_skill WHERE guid = %u AND skill IN (2, 6, 7, 8)", guid);
            CharacterDatabase.PExecute("DELETE FROM character_arpg_codex WHERE guid = %u", guid);
            for (uint16 page : s.pages)
                SavePage(player, page);
            s.things[HELD_TREE_REVISION] = TREE_REVISION;
            SaveHeld(player, HELD_TREE_REVISION, TREE_REVISION);
            if (had)
                sLog.outString("ARPG skills: %s's skill trees moved to revision %u: points back to spend", player->GetName(),
                               TREE_REVISION);
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
        // A class with no skills is sent none: the client empties its Skills tab.

        WorldPacket data(SMSG_ARPG_SKILLS, 512 + mine.size() * 1600);
        data << uint8(2);
        // Gear's skill levels add points, each spendable in its own skill alone.
        std::map<uint8, uint8> gear;
        uint32 gearPoints = 0;
        for (SkillDef const* skill : mine)
        {
            gear[skill->id] = GearSkillLevels(player, skill->id);
            if (SlotOf(s, skill->id) >= 0)
                gearPoints += gear[skill->id];
        }
        Edit(player, [&](PlayerSkills& e) { e.gearSent = gear; });
        data << uint16(SkillPointsFor(player->GetLevel()) + gearPoints);
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
            data << uint8(SKILL_CAP + gear[skill->id]);
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
        // The class's runes only.
        std::vector<RuneDef const*> runes;
        for (RuneDef const& rune : Runes())
            if (FitsFor(rune, player->getClass()))
                runes.push_back(&rune);
        data << uint8(runes.size());
        for (RuneDef const* each : runes)
        {
            RuneDef const& rune = *each;
            auto held = s.runes.find(rune.id);
            data << uint8(rune.id);
            data << uint32(rune.icon);
            data << rune.name;
            data << rune.text;
            data << uint8(held == s.runes.end() ? 0 : held->second);
            data << uint8(FitsFor(rune, player->getClass()));
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
        // Gear's levels raise the cap, and are spent first in their own skill.
        uint8 const levels = GearSkillLevels(player, skill->id);
        uint32 const spent = SpentIn(s, skill->id);
        if (spent >= uint32(SKILL_CAP) + levels)
            return refuse("the skill is at its cap");
        if (spent >= levels)
        {
            uint32 own = 0;
            for (SkillDef const& other : Skills())
                if (other.classId == player->getClass())
                {
                    uint32 const in = SpentIn(s, other.id);
                    uint8 const theirs = GearSkillLevels(player, other.id);
                    own += in > theirs ? in - theirs : 0;
                }
            if (own >= SkillPointsFor(player->GetLevel()))
                return refuse("no skill points left");
        }
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
            {
                NamedSpells const reach = NodeMask(*skill, *node);
                if (reach.spells)
                    totals.mods.push_back({ uint8(node->modOp), node->modType, node->modPerRank * rank, reach.mask, reach.spells });
            }
            if (node->ailTag >= 0 && node->ailTag < 7 && skill->id < 16)
                totals.skillAilment[skill->id][node->ailTag] += int32(node->ailPerRank) * rank;
        }
        // Avenger: Judgement keeps the Seal, for a doubled cooldown.
        if (totals.rank[KEY_AVENGER])
        {
            NamedSpells const judgement = NamesMask(classId, { "Judgement" }, "Avenger");
            if (judgement.spells)
                totals.mods.push_back({ uint8(SPELLMOD_COOLDOWN), uint8(SPELLMOD_PCT), 100, judgement.mask, judgement.spells });
        }
        for (auto const& [skillId, rune] : ActiveRunes(s))
        {
            if (rune->effect == RUNE_MODS)
                if (SkillDef const* skill = FindSkill(classId, skillId))
                {
                    NamedSpells const reach = SkillMask(*skill);
                    if (reach.spells)
                        for (auto const& [op, type, amount] : rune->mods)
                            totals.mods.push_back({ op, type, amount, reach.mask, reach.spells });
                }
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
                for (uint32 spell : KitSpellsOf(s.classId, skillId))
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

    std::vector<std::pair<SkillId, uint8>> SocketedRunes(Player const* player)
    {
        std::vector<std::pair<SkillId, uint8>> out;
        PlayerSkills const s = SkillsFor(player);
        for (auto const& [id, def] : ActiveRunes(s))
            out.emplace_back(id, def->id);
        return out;
    }

    std::vector<uint16> PagesFrom(uint32 bossEntry, uint32 classMask)
    {
        std::vector<uint16> out;
        for (PageHome const& page : PAGES)
            if (page.boss == bossEntry && (classMask & (1u << PageClass(page.node))))
                out.push_back(page.node);
        return out;
    }

    uint8 PageClass(uint16 node)
    {
        for (SkillDef const& skill : Skills())
            for (SkillNode const& n : skill.nodes)
                if (n.id == node)
                    return skill.classId;
        return 0;
    }

    bool RuneFitsClass(uint8 rune, uint8 classId)
    {
        RuneDef const* def = FindRune(rune);
        return def && FitsFor(*def, classId);
    }

    SkillId SwingSkill(uint8 classId)
    {
        return classId == CLASS_PALADIN ? SKILL_STRIKE : SKILL_NONE;
    }

    std::vector<uint16> AllPages()
    {
        std::vector<uint16> out;
        for (PageHome const& page : PAGES)
            out.push_back(page.node);
        return out;
    }

    uint16 CurrentPage(uint16 node)
    {
        if (PageOf(node))
            return node;
        for (auto const& [from, to] : PAGE_RENUMBER)
            if (from == node)
                return to;
        return 0;
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
            if (!r || !(FitsFor(*r, player->getClass()) & FIT(SkillId(skill))))
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

    std::vector<std::pair<uint8, std::vector<std::string>>> SkillBonusLines(Player const* player)
    {
        auto format = [](char const* fmt, auto... args)
        {
            char buf[256];
            std::snprintf(buf, sizeof(buf), fmt, args...);
            return std::string(buf);
        };
        std::vector<std::pair<uint8, std::vector<std::string>>> out;
        uint8 const classId = player->getClass();
        PlayerSkills const s = SkillsFor(player);
        WebTotals const web = SourceTotals(player, TOTALS_WEB);
        WebTotals const items = SourceTotals(player, TOTALS_ITEMS);
        WebTotals const tree = SourceTotals(player, TOTALS_SKILLS);
        std::vector<std::pair<SkillId, RuneDef const*>> const runes = ActiveRunes(s);
        for (SkillDef const& skill : Skills())
        {
            if (skill.classId != classId)
                continue;
            std::vector<std::string> lines;
            // Strike is the swing itself; the rest by their icon spell (the skill's own).
            SpellEntry const* spell = skill.id == SwingSkill(classId) ? nullptr : sSpellTemplate.LookupEntry<SpellEntry>(skill.icon);
            uint32 const tags = TagsOf(spell) & ~TagBit(TAG_HEAL);
            std::string names;
            for (uint8 tag = 0; tag < MAX_TAG; ++tag)
                if (tags & TagBit(Tag(tag)))
                    names += (names.empty() ? "" : ", ") + std::string(TagName(Tag(tag)));
            lines.push_back(format("|cff9d9d9dTags: %s|r", names.empty() ? "none" : names.c_str()));

            // Its tags' bonuses, summed as the damage is, and where each comes from.
            std::vector<std::string> byTag;
            int32 total = 0;
            for (uint8 tag = 0; tag < MAX_TAG; ++tag)
            {
                if (!(tags & TagBit(Tag(tag))))
                    continue;
                int32 const w = web.tag[tag], i = items.tag[tag], k = tree.tag[tag];
                if (!w && !i && !k)
                    continue;
                total += w + i + k;
                std::string from;
                if (w)
                    from += format("web %d", w);
                if (i)
                    from += format("%sgear %d", from.empty() ? "" : ", ", i);
                if (k)
                    from += format("%sskills %d", from.empty() ? "" : ", ", k);
                byTag.push_back(format("|cffffd200  %s +%d%%|r |cff9d9d9d(%s)|r", TagName(Tag(tag)), w + i + k, from.c_str()));
            }
            lines.push_back(format("|cffffd200Damage +%d%% from its tags|r", total));
            lines.insert(lines.end(), byTag.begin(), byTag.end());
            ConditionalDamageLines(player, tags, uint8(skill.id), lines);

            // Its tree: the ranks taken.
            for (SkillNode const& node : skill.nodes)
            {
                auto it = s.ranks.find(node.id);
                if (it != s.ranks.end() && it->second)
                    lines.push_back(format("|cff20ff20%s %u/%u|r: %s", node.name, uint32(it->second), uint32(node.max), node.text));
            }
            for (auto const& [id, rune] : runes)
                if (id == skill.id)
                    lines.push_back(format("|cffa335ee%s|r: %s", rune->name, rune->text));
            // A rune socketed but not working says why.
            auto socket = s.sockets.find(skill.id);
            if (socket != s.sockets.end())
                if (RuneDef const* rune = FindRune(socket->second))
                {
                    bool active = false;
                    for (auto const& [id, live] : runes)
                        active = active || (id == skill.id && live == rune);
                    if (!active)
                    {
                        if (SlotOf(s, skill.id) < 0)
                            lines.push_back(format("|cffff4040%s does nothing: the skill is not in a slot.|r", rune->name));
                        else
                            lines.push_back(format("|cffff4040%s does nothing yet: it needs %u points in the skill (%u now).|r",
                                                   rune->name, SOCKET_POINTS, uint32(SpentIn(s, skill.id))));
                    }
                }

            // The worn uniques that change it.
            for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            {
                Item const* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
                if (!item)
                    continue;
                for (UniqueMechanic const& row : Uniques())
                {
                    if (row.item != item->GetEntry() || row.kit == KIT_RAISE)
                        continue;
                    SpellEntry const* changes = sSpellTemplate.LookupEntry<SpellEntry>(row.spell);
                    if (changes && SkillOfSpell(classId, changes) == skill.id)
                        lines.push_back(format("|cffe6cc80%s|r: %s", item->GetProto()->Name1, row.text));
                }
            }
            // Kits from several sources on one spell stack: the combined numbers.
            for (std::string const& line : StackedKitLines(player, uint8(skill.id)))
                lines.push_back(line);
            if (lines.size() == 2 && !total)
                lines.push_back("|cff9d9d9dNothing adds to it yet: points in its tree, web nodes and gear with its tags all do.|r");
            // Gear and buffs, the character's numbers, the damage it should do and what it did.
            SkillDamageLines(player, uint8(skill.id), tags, lines);
            out.emplace_back(uint8(skill.id), std::move(lines));
        }
        return out;
    }
}
