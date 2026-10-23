/*
 * ARPG skill specialisation: see ArpgSkills.h.
 */

#include "Arpg/ArpgSkills.h"
#include "Arpg/ArpgPerks.h"
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
        int8 convertTag = -1;     // a conversion node: the damage tag the skill's hits count as
        uint8 choice = 0;         // a choice group: of a skill's nodes sharing one, only one may hold ranks
        int16 morePerRank = 0;    // a "more" node: this much more of the skill's damage per rank (only `only`'s)
    };

    // `node`, one side of choice group `group` (Diablo IV's mutually exclusive upgrades): its
    // alternatives share the group, and only one of them may hold ranks.
    SkillNode Choice(SkillNode node, uint8 group)
    {
        node.choice = group;
        return node;
    }

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

    // A "more" node: `perRank`% more damage per rank for the skill's hits, its ticks and its
    // minions' too (only the spell named `only`, when set): Arpg::SkillMore.
    SkillNode More(uint16 id, uint8 col, uint8 row, uint16 parent, uint8 max, char const* name, char const* text,
                   uint32 icon, int16 perRank, char const* only = nullptr)
    {
        SkillNode n{ id, SKILL_NODE_MODIFIER, col, row, parent, max, name, text, icon };
        n.morePerRank = perRank;
        n.only = only;
        return n;
    }

    // An ailment node: the skill's hits have `perRank`% per rank to inflict `tag`'s ailment
    // (Arpg/ArpgDamage.h), whatever their own damage type.
    // A conversion: the skill's hits count as `tag` for their ailments, their Daze and the
    // synergy triggers (Arpg/ArpgStates.h), moving the skill into that damage type's pairings.
    SkillNode Conv(uint16 id, uint8 col, uint8 row, uint16 parent, char const* name, char const* text, uint32 icon, Tag tag)
    {
        SkillNode n{ id, SKILL_NODE_TRANSFORMER, col, row, parent, 1, name, text, icon };
        n.convertTag = int8(tag);
        return n;
    }

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
                  Key(101, SKILL_NODE_TRANSFORMER, 0, 1, 0, 3, "Wide Swing", "Your swings strike every other enemy in front of you for 70/85/100% of the hit, not 55%.", 845, KEY_WIDE_SWING),
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
                  Key(113, SKILL_NODE_MODIFIER, 2, 3, 112, 3, "Zeal", "+5% attack speed per rank under any Seal, +10% under Seal of the Crusader.", 21082, KEY_ZEAL),
                  Key(114, SKILL_NODE_SYNERGY, 2, 4, 113, 1, "Light of the Crusader", "Your Seal hits heal you for 10% of their damage.", 19977, KEY_LIGHT_OF_THE_CRUSADER),
                  Key(115, SKILL_NODE_CAPSTONE, 2, 5, 114, 1, "Shockwave", "Your critical swings send a shockwave 10 yards ahead, for 40% of the hit.", 20549, KEY_SHOCKWAVE_STRIKE),
                  // Synergy (Arpg/ArpgSynergy.h, ArpgStates.h).
                  Key(116, SKILL_NODE_SYNERGY, 0, 6, 105, 3, "Concussive Blows", "Your critical physical or holy Crusader hits add 15% of the way to a Daze per rank.", 12809, KEY_NONE),
                  Conv(117, 1, 6, 110, "Sun Seal", "Your swings and Seals count as Fire: they Ignite instead of Judge (on a crit, or by your Ignite chances), and build no Daze.", 2136, TAG_FIRE),
              } },
            { SKILL_HOLY_LIGHT, CLASS_PALADIN, "Holy Light", "Holy Light and Flash of Light: heals, which its tree turns on your enemies too.", 635, { "Holy Light", "Flash of Light" },
              { "Radiance", "Grace", "Retribution" },
              {
                  Mod(201, 0, 1, 0, 4, "Healing Light", "6% more healing per rank.", 20237, SPELLMOD_DAMAGE, SPELLMOD_PCT, 6),
                  Mod(202, 0, 2, 201, 3, "Light's Grace", "+4% critical strike chance per rank.", 20208, SPELLMOD_CRITICAL_CHANCE, SPELLMOD_FLAT, 4),
                  Mod(203, 0, 3, 202, 3, "Swift Light", "-0.2 sec recovery per rank.", 19750, SPELLMOD_CASTING_TIME, SPELLMOD_FLAT, -200),
                  Key(204, SKILL_NODE_SYNERGY, 0, 4, 203, 1, "Crusader's Grace", "A critical Judgement, Exorcism or Hammer of Wrath lets your next Holy Light or Flash of Light skip its recovery (once every 8 sec).", 635, KEY_NONE),
                  Key(205, SKILL_NODE_CAPSTONE, 0, 5, 204, 1, "Dawnbringer", "Your heals on yourself also send half their amount at the nearest enemy as a Holy bolt.", 19977, KEY_DAWNBRINGER),
                  Mod(210, 1, 1, 0, 3, "Pure Light", "10% less Holy Light and Flash of Light mana cost per rank.", 20237, SPELLMOD_COST, SPELLMOD_PCT, -10),
                  Mod(211, 1, 2, 210, 3, "Radiant Light", "Critical heals heal 20% more of their bonus per rank.", 20208, SPELLMOD_CRIT_DAMAGE_BONUS, SPELLMOD_PCT, 20),
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
                  Key(308, SKILL_NODE_CAPSTONE, 1, 4, 307, 1, "Sentence", "Judgement deals 100% more damage to stunned or Dazed enemies.", 879, KEY_SENTENCE),
                  Conv(314, 1, 5, 308, "Frozen Verdict", "Judgement counts as Frost: a critical one Chills (Freezes the Chilled), and your Chill chances apply.", 116, TAG_FROST),
                  Key(309, SKILL_NODE_SYNERGY, 2, 1, 0, 1, "Avenger", "Judgement no longer uses up your Seal, but its cooldown is doubled.", 25780, KEY_AVENGER),
                  Mod(310, 2, 2, 309, 3, "Swift Judgement", "-1 sec Judgement cooldown per rank.", 1044, SPELLMOD_COOLDOWN, SPELLMOD_FLAT, -1000),
                  Key(311, SKILL_NODE_SYNERGY, 2, 3, 310, 1, "Crusader's Verdict", "A critical swing makes your next Judgement deal 50% more (once every 6 sec).", 20271, KEY_NONE),
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
                  Key(406, SKILL_NODE_SYNERGY, 1, 2, 405, 1, "Hallowed Strike", "A Judgement that hits ends Consecration's cooldown (once every 4 sec).", 26573, KEY_NONE),
                  Key(407, SKILL_NODE_CAPSTONE, 1, 3, 406, 1, "Sacred Seal", "Your Seals strike twice on enemies in your Consecration.", 20154, KEY_SACRED_SEAL),
                  Key(408, SKILL_NODE_MODIFIER, 2, 1, 0, 3, "Hallowed Ground", "While you stand in your Consecration, you regain 1% of your health a second per rank.", 633, KEY_HALLOWED_GROUND),
                  Key(409, SKILL_NODE_SYNERGY, 2, 2, 408, 1, "Steadfast", "While you stand in your Consecration, you can't be stunned.", 498, KEY_STEADFAST),
                  Key(410, SKILL_NODE_CAPSTONE, 2, 3, 409, 1, "Martyr's Ward", "Your Retribution Aura burns every enemy within 10 yards each second, for five times its damage.", 7294, KEY_MARTYRS_WARD),
                  Mod(411, 1, 4, 407, 3, "Devotion", "10% less Consecration mana cost per rank.", 20237, SPELLMOD_COST, SPELLMOD_PCT, -10),
                  More(412, 2, 4, 410, 3, "Zealous Ground", "6% more Consecration damage per rank.", 26573, 6),
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
                  Mod(507, 1, 3, 506, 3, "Quick Hammer", "-2 sec Hammer of Justice cooldown per rank (it is 15 sec while Hammers is slotted).", 5589, SPELLMOD_COOLDOWN, SPELLMOD_FLAT, -2000, "Hammer of Justice"),
                  Key(508, SKILL_NODE_CAPSTONE, 1, 4, 507, 1, "Sentence Passed", "Your damage is 15% more on stunned or Dazed enemies.", 879, KEY_SENTENCE_PASSED),
                  Key(514, SKILL_NODE_SYNERGY, 1, 5, 508, 1, "Aftershock", "When you consume Daze, the 2 nearest enemies within 10 yards are Dazed (once every 2 sec).", 20549, KEY_NONE),
                  Kit(509, 2, 1, 0, 2, "Hammer Volley", "Hammer of Wrath throws 1/2 more hammers, fanned 15 degrees apart, at 60%.", 24275,
                      KIT_EXTRA_PROJECTILES, 24275, { 1, 2 }, { 15, 15 }, { 60, 60 }),
                  Mod(510, 2, 2, 509, 4, "Weighted Hammer", "8% more Hammer of Wrath damage per rank.", 20218, SPELLMOD_DAMAGE, SPELLMOD_PCT, 8, "Hammer of Wrath"),
                  Key(511, SKILL_NODE_SYNERGY, 2, 3, 510, 1, "Wrath of the Light", "A critical Judgement or Exorcism makes your next Hammer of Wrath deal 60% more and fly through every enemy in its path for 60%.", 24275, KEY_NONE),
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
                  Key(614, SKILL_NODE_SYNERGY, 1, 5, 608, 1, "Cleansing Fire", "Exorcism and Holy Wrath consume Ignite to burst onto every enemy within 8 yards for 80% of the hit and the burn left.", 2120, KEY_NONE),
                  Mod(609, 2, 1, 0, 3, "Swift Exorcism", "-2 sec Exorcism cooldown per rank.", 1044, SPELLMOD_COOLDOWN, SPELLMOD_FLAT, -2000, "Exorcism"),
                  Key(610, SKILL_NODE_SYNERGY, 2, 2, 609, 1, "Righteous Thunder", "Stunning an enemy with Hammer of Justice makes your next Holy Wrath instant (once every 10 sec).", 2812, KEY_NONE),
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
                  Mod(122, 0, 2, 121, 3, "Quick Fireball", "-0.15 sec Fireball recovery per rank.", 12043, SPELLMOD_CASTING_TIME, SPELLMOD_FLAT, -150, "Fireball"),
                  Kit(123, 0, 3, 122, 3, "Piercing Flame", "Fireball flies through 1/2/4 enemies, at 60%.", 2136,
                      KIT_PIERCE, 133, { 1, 2, 4 }, { 0, 0, 0 }, { 60, 60, 60 }),
                  KitCap(124, 0, 4, 123, "Flame Echo", "Every 3rd Fireball is echoed for free, at 70%.", 11129, KIT_ECHO, 133, 3, 0.0f, 70),
                  Mod(125, 1, 1, 0, 4, "Searing Bolt", "6% more Fireball and Pyroblast damage per rank.", 11366, SPELLMOD_DAMAGE, SPELLMOD_PCT, 6),
                  Ail(126, 1, 2, 125, 3, "Kindle", "Your Fireball and Pyroblast hits have an 8% chance per rank to Ignite, beside their critical strikes.", 2120, TAG_FIRE, 8),
                  Key(127, SKILL_NODE_MODIFIER, 1, 3, 126, 3, "Lingering Flame", "Your Ignites burn 15% more per rank.", 2948, KEY_LINGERING_FLAME),
                  Key(128, SKILL_NODE_CAPSTONE, 1, 4, 127, 1, "Pyroclasm", "Critical Fireballs and Pyroblasts explode: every enemy within 8 yards of the target takes 60% of the hit.", 11113, KEY_PYROCLASM),
                  Mod(129, 2, 1, 0, 3, "Quick Pyre", "-0.5 sec Pyroblast recovery per rank.", 12043, SPELLMOD_CASTING_TIME, SPELLMOD_FLAT, -500, "Pyroblast"),
                  Key(130, SKILL_NODE_SYNERGY, 2, 2, 129, 1, "Hot Streak", "A critical Fire Blast or Scorch lets your next Pyroblast skip its recovery (once every 8 sec).", 11366, KEY_NONE),
                  Mod(131, 2, 3, 130, 3, "Pyre", "Pyroblast's critical strikes deal 20% more of their bonus per rank.", 11129, SPELLMOD_CRIT_DAMAGE_BONUS, SPELLMOD_PCT, 20, "Pyroblast"),
                  KitCap(132, 2, 4, 131, "Cataclysm", "Pyroblast bursts onto every enemy within 10 yards of its target, at 60%.", 11113, KIT_BURST, 11366, 0, 10.0f, 60),
                  Conv(133, 2, 5, 132, "Frostfire Bolt", "Fireball and Pyroblast count as Frost: a critical one Chills (Freezes the Chilled), and your Chill chances apply.", 116, TAG_FROST),
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
                  Ail(225, 1, 1, 0, 3, "Deep Chill", "Your Frostbolt and Cone of Cold hits have a 10% chance per rank to deepen the cold: they Freeze the Chilled, and Chill the rest.", 12484, TAG_FROST, 10),
                  Key(226, SKILL_NODE_SYNERGY, 1, 2, 225, 1, "Winter's Grasp", "Frost Nova lets your next Frostbolt skip its recovery, and it shatters into 3 shards at the nearest enemies within 10 yards for 50%.", 122, KEY_NONE),
                  Mod(227, 1, 3, 226, 3, "Razor Ice", "Critical strikes deal 20% more of their bonus per rank.", 11958, SPELLMOD_CRIT_DAMAGE_BONUS, SPELLMOD_PCT, 20),
                  Key(228, SKILL_NODE_CAPSTONE, 1, 4, 227, 1, "Shatterstorm", "Frostbolt on a frozen enemy shatters: every enemy within 8 yards of it takes 80% of the hit.", 122, KEY_SHATTERSTORM),
                  Key(233, SKILL_NODE_SYNERGY, 1, 5, 228, 1, "Cold Snap", "When you consume a Freeze, the 2 nearest enemies within 10 yards are Chilled.", 12472, KEY_NONE),
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
                  Key(327, SKILL_NODE_SYNERGY, 1, 3, 326, 1, "Arcane Echoes", "A critical Arcane Explosion makes your next Arcane Missiles cost no mana, and each missile jumps to 1 more enemy within 8 yards for 50%.", 1449, KEY_NONE),
                  Key(328, SKILL_NODE_CAPSTONE, 1, 4, 327, 1, "Overcharge", "Your Overload bursts deal 40% more.", 12042, KEY_OVERCHARGE),
                  Mod(329, 2, 1, 0, 3, "Focused Mind", "Critical strikes deal 15% more of their bonus per rank.", 12043, SPELLMOD_CRIT_DAMAGE_BONUS, SPELLMOD_PCT, 15),
                  Mod(330, 2, 2, 329, 2, "Lingering Barrage", "+1 sec Arcane Missiles channel per rank: one more missile.", 1953, SPELLMOD_DURATION, SPELLMOD_FLAT, 1000, "Arcane Missiles"),
                  Key(331, SKILL_NODE_CAPSTONE, 2, 3, 330, 1, "Arcane Siphon", "Each missile that hits restores 0.5% of your maximum mana.", 12051, KEY_ARCANE_SIPHON),
                  Key(332, SKILL_NODE_SYNERGY, 2, 4, 331, 1, "Arcane Detonation", "Arcane Missiles consume Overload to burst onto every enemy within 8 yards for 50% of the hit and the stored damage (once a second).", 1449, KEY_NONE),
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
                  Conv(431, 1, 4, 427, "Glacial Explosion", "Arcane Explosion counts as Frost: a critical one Chills (Freezes the Chilled), and your Chill chances apply.", 122, TAG_FROST),
                  Mod(428, 2, 1, 0, 3, "Mana Efficiency", "10% less Arcane Explosion mana cost per rank.", 12051, SPELLMOD_COST, SPELLMOD_PCT, -10),
                  Key(429, SKILL_NODE_SYNERGY, 2, 2, 428, 1, "Resonance", "A critical Arcane Missile makes your next Arcane Explosion 50% wider (once every 4 sec).", 5143, KEY_NONE),
                  Key(430, SKILL_NODE_CAPSTONE, 2, 3, 429, 1, "Mana Burst", "Each enemy Arcane Explosion hits restores 1% of your maximum mana, 5% at most a cast.", 12051, KEY_MANA_BURST),
                  More(432, 2, 4, 430, 4, "Arcane Mastery", "6% more Arcane Explosion damage per rank.", 1449, 6),
              } },
            { SKILL_STORMS, CLASS_MAGE, "Storms", "Blizzard and Flamestrike: storms of ice and fire on the ground.", 10, { "Blizzard", "Flamestrike" },
              { "Blizzard", "Storm", "Flamestrike" },
              {
                  Mod(521, 0, 1, 0, 3, "Wide Blizzard", "+12% Blizzard radius per rank.", 10, SPELLMOD_RADIUS, SPELLMOD_PCT, 12, "Blizzard"),
                  Mod(522, 0, 2, 521, 4, "Biting Storm", "8% more Blizzard damage per rank.", 10, SPELLMOD_DOT, SPELLMOD_PCT, 8, "Blizzard"),
                  Mod(523, 0, 3, 522, 3, "Thrifty Storm", "10% less Blizzard mana cost per rank.", 12051, SPELLMOD_COST, SPELLMOD_PCT, -10, "Blizzard"),
                  Key(524, SKILL_NODE_CAPSTONE, 0, 4, 523, 1, "Glacial Storm", "Blizzard chills every enemy it strikes, and each strike has a 15% chance to freeze one chilled already.", 12484, KEY_GLACIAL_STORM),
                  Key(533, SKILL_NODE_SYNERGY, 0, 5, 524, 1, "Rime Field", "Your Frost hits Freeze an enemy standing in a Blizzard (once every 3 sec).", 10, KEY_NONE),
                  Key(525, SKILL_NODE_SYNERGY, 1, 1, 0, 1, "Firestorm", "A critical Fire Blast or Scorch makes your next Blizzard or Flamestrike 50% wider (once every 6 sec).", 2120, KEY_NONE),
                  Mod(526, 1, 2, 525, 3, "Storm Surge", "+8% radius per rank.", 10, SPELLMOD_RADIUS, SPELLMOD_PCT, 8),
                  Mod(527, 1, 3, 526, 3, "Raging Storm", "Blizzard's strikes and Flamestrike's burn deal 6% more per rank.", 2120, SPELLMOD_DOT, SPELLMOD_PCT, 6),
                  ModCap(528, 1, 4, 527, "Eye of the Storm", "Blizzard and Flamestrike are 30% wider.", 10, SPELLMOD_RADIUS, SPELLMOD_PCT, 30),
                  Mod(529, 2, 1, 0, 3, "Quick Flamestrike", "-0.3 sec Flamestrike recovery per rank.", 2120, SPELLMOD_CASTING_TIME, SPELLMOD_FLAT, -300, "Flamestrike"),
                  Mod(530, 2, 2, 529, 3, "Searing Ground", "Flamestrike's burn deals 15% more per rank.", 2120, SPELLMOD_DOT, SPELLMOD_PCT, 15, "Flamestrike"),
                  Ail(531, 2, 3, 530, 3, "Kindling Strike", "Flamestrike's hits have a 10% chance per rank to Ignite.", 2120, TAG_FIRE, 10),
                  ModCap(532, 2, 4, 531, "Meteor", "Flamestrike skips its recovery.", 11113, SPELLMOD_CASTING_TIME, SPELLMOD_PCT, -100, "Flamestrike"),
              } },
            { SKILL_FIRE_BLAST, CLASS_MAGE, "Fire Blast", "Fire Blast and Scorch: quick fire, close in.", 2136, { "Fire Blast", "Scorch" },
              { "Blast", "Scorch", "Combust" },
              {
                  Mod(621, 0, 1, 0, 3, "Swift Blast", "-1 sec Fire Blast cooldown per rank.", 2136, SPELLMOD_COOLDOWN, SPELLMOD_FLAT, -1000, "Fire Blast"),
                  Kit(622, 0, 2, 621, 3, "Explosive Blast", "Fire Blast bursts onto enemies within 4/6/8 yards of its target, at 40%.", 11113,
                      KIT_BURST, 2136, { 0, 0, 0 }, { 4, 6, 8 }, { 40, 40, 40 }),
                  Mod(623, 0, 3, 622, 3, "Incineration", "+5% Fire Blast critical strike chance per rank.", 2136, SPELLMOD_CRITICAL_CHANCE, SPELLMOD_FLAT, 5, "Fire Blast"),
                  Key(624, SKILL_NODE_CAPSTONE, 0, 4, 623, 1, "Chain Blast", "A kill with Fire Blast readies Fire Blast at once.", 11129, KEY_CHAIN_BLAST),
                  Mod(625, 1, 1, 0, 3, "Quick Scorch", "-0.2 sec Scorch recovery per rank.", 2948, SPELLMOD_CASTING_TIME, SPELLMOD_FLAT, -200, "Scorch"),
                  Kit(626, 1, 2, 625, 2, "Arcing Scorch", "Scorch chains to 1/2 more enemies within 10 yards, at 60%.", 2948,
                      KIT_CHAIN, 2948, { 1, 2 }, { 10, 10 }, { 60, 60 }),
                  Ail(627, 1, 3, 626, 3, "Scorching", "Your Fire Blast and Scorch hits have a 10% chance per rank to Ignite.", 2120, TAG_FIRE, 10),
                  Key(628, SKILL_NODE_CAPSTONE, 1, 4, 627, 1, "Searing Brand", "Scorch brands its target with Fire Vulnerability: 3% more Fire damage taken per stack, up to 5.", 2948, KEY_SEARING_BRAND),
                  Key(629, SKILL_NODE_SYNERGY, 2, 1, 0, 1, "Kindling Blast", "Within 4 sec of a critical Fireball, your Fire Blast deals 60% more and bursts onto every enemy within 6 yards for 50%.", 2136, KEY_NONE),
                  Mod(630, 2, 2, 629, 4, "Blazing", "6% more Fire Blast and Scorch damage per rank.", 2136, SPELLMOD_DAMAGE, SPELLMOD_PCT, 6),
                  Key(631, SKILL_NODE_CAPSTONE, 2, 3, 630, 1, "Detonate", "Fire Blast sets off your Ignite on its target at once: the burn left strikes it and every enemy within 6 yards.", 11113, KEY_DETONATE),
                  Key(632, SKILL_NODE_SYNERGY, 2, 4, 631, 1, "Combustion Point", "A critical Fire Blast on an Ignited enemy spreads Ignite to the 2 nearest enemies within 10 yards.", 11129, KEY_NONE),
              } },
            // --- The druid's six, numbered 40-59 within each skill's hundred. ---
            { SKILL_DRUID_WRATH, CLASS_DRUID, "Wrath", "Wrath and Starfire: the druid's bolts of nature and starlight.", 5176, { "Wrath", "Starfire" },
              { "Volley", "Starfall", "Grace" },
              {
                  Kit(141, 0, 1, 0, 3, "Splitting Wrath", "Wrath launches 1/1/2 extra bolts, at 45/60/60%.", 5176,
                      KIT_EXTRA_PROJECTILES, 5176, { 1, 1, 2 }, { 14, 14, 14 }, { 45, 60, 60 }),
                  Mod(142, 0, 2, 141, 3, "Quick Wrath", "-0.15 sec Wrath recovery per rank.", 16814, SPELLMOD_CASTING_TIME, SPELLMOD_FLAT, -150, "Wrath"),
                  Ail(143, 0, 3, 142, 3, "Thorned Wrath", "Your Wrath and Starfire hits have an 8% chance per rank to Poison.", 467, TAG_NATURE, 8),
                  KitCap(144, 0, 4, 143, "Wrath of Cenarius", "Wrath chains to 3 more enemies within 10 yards, at 60%.", 5176, KIT_CHAIN, 5176, 3, 10.0f, 60),
                  Key(153, SKILL_NODE_SYNERGY, 0, 5, 144, 1, "Thorns of Cenarius", "A critical Wrath on a Poisoned enemy spreads Poison to the 2 nearest enemies within 10 yards.", 467, KEY_NONE),
                  Mod(145, 1, 1, 0, 4, "Lunar Fury", "6% more Starfire damage per rank.", 2912, SPELLMOD_DAMAGE, SPELLMOD_PCT, 6, "Starfire"),
                  Mod(146, 1, 2, 145, 3, "Quick Starfire", "-0.2 sec Starfire recovery per rank.", 16850, SPELLMOD_CASTING_TIME, SPELLMOD_FLAT, -200, "Starfire"),
                  Ail(147, 1, 3, 146, 3, "Moonstruck", "Your Wrath and Starfire hits have a 10% chance per rank to Overload.", 8921, TAG_ARCANE, 10),
                  KitCap(148, 1, 4, 147, "Starfall", "Starfire bursts onto every enemy within 10 yards of its target, at 60%.", 20687, KIT_BURST, 2912, 0, 10.0f, 60),
                  Mod(149, 2, 1, 0, 3, "Nature's Grace", "+3% Wrath and Starfire critical strike chance per rank.", 16880, SPELLMOD_CRITICAL_CHANCE, SPELLMOD_FLAT, 3),
                  Mod(150, 2, 2, 149, 3, "Moonfury", "Critical strikes deal 20% more of their bonus per rank.", 16896, SPELLMOD_CRIT_DAMAGE_BONUS, SPELLMOD_PCT, 20),
                  Key(151, SKILL_NODE_SYNERGY, 2, 3, 150, 1, "Lunar Eclipse", "A critical Wrath lets your next Starfire skip its recovery, and it bursts onto every enemy within 6 yards for 40% (once every 8 sec).", 2912, KEY_NONE),
                  Key(152, SKILL_NODE_CAPSTONE, 2, 4, 151, 1, "Eclipse", "Your critical Wraths and Starfires restore 2% of your maximum mana and make your next cast 0.5 sec quicker.", 16886, KEY_ECLIPSE),
              } },
            { SKILL_MOONFIRE, CLASS_DRUID, "Moonfire", "Moonfire and Insect Swarm: burns and swarms that spread.", 8921, { "Moonfire", "Insect Swarm" },
              { "Lunar", "Swarm", "Moonglow" },
              {
                  Kit(241, 0, 1, 0, 3, "Spreading Moon", "Moonfire also lands on 1/2/3 more enemies within 8 yards.", 8921,
                      KIT_SPREAD, 8921, { 1, 2, 3 }, { 8, 8, 8 }, { 100, 100, 100 }),
                  Mod(242, 0, 2, 241, 3, "Lunar Burn", "Moonfire's burn deals 15% more per rank.", 16821, SPELLMOD_DOT, SPELLMOD_PCT, 15, "Moonfire"),
                  Key(243, SKILL_NODE_SYNERGY, 0, 3, 242, 1, "Moonkissed", "A critical Wrath or Starfire makes your next Moonfire also land on the 3 nearest enemies within 8 yards (once every 6 sec).", 8921, KEY_NONE),
                  KitCap(244, 0, 4, 243, "Full Moon", "Moonfire spreads to every enemy within 10 yards when its target dies.", 20690, KIT_SPREAD, 8921, 0, 10.0f, 100),
                  Kit(245, 1, 1, 0, 2, "Plague", "Insect Swarm also lands on 1/2 more enemies within 8 yards.", 5570,
                      KIT_SPREAD, 5570, { 1, 2 }, { 8, 8 }, { 100, 100 }),
                  Ail(246, 1, 2, 245, 3, "Locusts", "Your Moonfire hits have a 10% chance per rank to Poison.", 5570, TAG_NATURE, 10),
                  Mod(247, 1, 3, 246, 3, "Swarm Fury", "10% more Insect Swarm damage per rank.", 5570, SPELLMOD_DOT, SPELLMOD_PCT, 10, "Insect Swarm"),
                  Key(248, SKILL_NODE_CAPSTONE, 1, 4, 247, 1, "Hive Mind", "Every tick of your Insect Swarm Poisons its target.", 5570, KEY_HIVE_MIND),
                  Mod(249, 2, 1, 0, 3, "Moonglow", "10% less mana cost per rank.", 16845, SPELLMOD_COST, SPELLMOD_PCT, -10),
                  Mod(250, 2, 2, 249, 2, "Long Night", "+3 sec Moonfire and Insect Swarm duration per rank.", 16821, SPELLMOD_DURATION, SPELLMOD_FLAT, 3000),
                  Key(251, SKILL_NODE_CAPSTONE, 2, 3, 250, 1, "Lunar Tide", "Every tick of your Moonfire and Insect Swarm restores 0.5% of your maximum mana, 2% a second at most.", 16845, KEY_LUNAR_TIDE),
                  Conv(252, 2, 4, 251, "Solar Flare", "Moonfire's hit counts as Fire: a critical one Ignites instead of Overloading, and your Ignite chances apply.", 8921, TAG_FIRE),
                  More(253, 0, 5, 244, 3, "Moonlit Wrath", "8% more Moonfire damage per rank.", 8921, 8, "Moonfire"),
              } },
            { SKILL_HURRICANE, CLASS_DRUID, "Hurricane", "A storm of wind and hail on the ground. An ARPG druid has it from level 20.", 16914, { "Hurricane" },
              { "Gale", "Squall", "Hail" },
              {
                  Mod(341, 0, 1, 0, 3, "Wide Hurricane", "+12% Hurricane radius per rank.", 16914, SPELLMOD_RADIUS, SPELLMOD_PCT, 12),
                  Mod(342, 0, 2, 341, 4, "Howling Gale", "8% more Hurricane damage per rank.", 16914, SPELLMOD_DOT, SPELLMOD_PCT, 8),
                  Key(343, SKILL_NODE_SYNERGY, 0, 3, 342, 1, "Gathering Storm", "A critical Moonfire or Starfire makes your next Hurricane cost no mana (once every 10 sec).", 16914, KEY_NONE),
                  Key(344, SKILL_NODE_CAPSTONE, 0, 4, 343, 1, "Tempest", "Each Hurricane strike has a 10% chance to hurl a Wrath at the enemy struck, for three times the strike, 2 a second at most.", 5176, KEY_TEMPEST),
                  Mod(345, 2, 1, 0, 2, "Grasping Winds", "+2 sec Hurricane duration per rank.", 16914, SPELLMOD_DURATION, SPELLMOD_FLAT, 2000),
                  Key(346, SKILL_NODE_TRANSFORMER, 2, 2, 345, 3, "Biting Hail", "Each Hurricane strike has a 5% chance per rank to chill.", 12484, KEY_BITING_HAIL),
                  Key(347, SKILL_NODE_SYNERGY, 2, 3, 346, 3, "Eye of the Wild", "Each enemy your Hurricane strikes heals you for 0.3% of your maximum health per rank, 3% a second at most.", 740, KEY_EYE_OF_THE_WILD),
                  ModCap(348, 2, 4, 347, "Thunderhead", "Hurricane costs half the mana.", 16914, SPELLMOD_COST, SPELLMOD_PCT, -50),
                  Ail(349, 1, 1, 0, 3, "Stinging Hail", "Your Hurricane strikes have a 6% chance per rank to Poison.", 16914, TAG_NATURE, 6),
                  More(350, 1, 2, 349, 4, "Gale Force", "6% more Hurricane damage per rank.", 16914, 6),
                  Mod(351, 1, 3, 350, 3, "Thrifty Gale", "10% less Hurricane mana cost per rank.", 12051, SPELLMOD_COST, SPELLMOD_PCT, -10),
              } },
            { SKILL_BEAR, CLASS_DRUID, "Bear", "Maul, Swipe and Demoralizing Roar: the bear. An ARPG druid has Bear Form and Swipe from level 10.", 5487, { "Maul", "Swipe", "Demoralizing Roar" },
              { "Maul", "Swipe", "Guardian" },
              {
                  Mod(441, 0, 1, 0, 4, "Brutal Maul", "8% more Maul damage per rank.", 6807, SPELLMOD_DAMAGE, SPELLMOD_PCT, 8, "Maul"),
                  Kit(442, 0, 2, 441, 3, "Mauling Arc", "Maul strikes every enemy in front of you; the extras take 20/35/50%.", 6807,
                      KIT_ARC, 6807, { 0, 0, 0 }, { 0, 0, 0 }, { 20, 35, 50 }),
                  Ail(443, 0, 3, 442, 3, "Savage Maul", "Your Bear hits have a 10% chance per rank to cause Bleeding.", 16952, TAG_PHYSICAL, 10),
                  KitCap(444, 0, 4, 443, "Earthshaker", "Maul sends a shockwave 10 yards ahead, at 60%.", 16940, KIT_SHOCKWAVE, 6807, 0, 10.0f, 60),
                  Key(451, SKILL_NODE_SYNERGY, 0, 5, 444, 1, "Crushing Maul", "Maul consumes Daze for an extra hit of 80% of the hit.", 6807, KEY_NONE),
                  More(452, 1, 4, 447, 4, "Ursine Might", "6% more Bear damage per rank.", 5487, 6),
                  Key(445, SKILL_NODE_SYNERGY, 1, 1, 0, 1, "Roaring Swipe", "Demoralizing Roar makes your next Swipe deal 60% more (once every 6 sec).", 99, KEY_NONE),
                  Mod(446, 1, 2, 445, 2, "Wide Swipe", "+15% Swipe area per rank.", 779, SPELLMOD_RADIUS, SPELLMOD_PCT, 15, "Swipe"),
                  ModCap(447, 1, 3, 446, "Ravaging Swipe", "Swipe strikes up to 10 enemies.", 779, SPELLMOD_JUMP_TARGETS, SPELLMOD_FLAT, 7, "Swipe"),
                  Key(448, SKILL_NODE_SYNERGY, 2, 1, 0, 3, "Bristling Fur", "In Bear Form, each blow you take gives you 2 rage per rank, once a second.", 5229, KEY_BRISTLING_FUR),
                  Key(449, SKILL_NODE_MODIFIER, 2, 2, 448, 3, "Survival Instincts", "Your Bear hits heal you for 2% of their damage per rank.", 22842, KEY_SURVIVAL_INSTINCTS),
                  Key(450, SKILL_NODE_CAPSTONE, 2, 3, 449, 1, "Thorned Hide", "In Bear Form, enemies that hit you take 30% of your attack power as Nature damage, once a second each.", 467, KEY_THORNED_HIDE),
              } },
            { SKILL_CAT, CLASS_DRUID, "Cat", "Claw, Shred, Rake, Rip and Ferocious Bite: the cat. Your combo points carry from enemy to enemy.", 768, { "Claw", "Shred", "Rake", "Rip", "Ferocious Bite" },
              { "Claws", "Bleed", "Finisher" },
              {
                  Mod(541, 0, 1, 0, 4, "Sharpened Claws", "6% more Cat damage per rank.", 16942, SPELLMOD_DAMAGE, SPELLMOD_PCT, 6),
                  Kit(542, 0, 2, 541, 3, "Raking Claws", "Claw strikes every enemy in front of you; the extras take 20/35/50%.", 1082,
                      KIT_ARC, 1082, { 0, 0, 0 }, { 0, 0, 0 }, { 20, 35, 50 }),
                  Mod(543, 0, 3, 542, 2, "Light Paws", "Your Cat abilities cost 5 less energy per rank.", 17002, SPELLMOD_COST, SPELLMOD_FLAT, -5),
                  KitCap(544, 0, 4, 543, "Stalker's Step", "A kill with Shred steps you behind the nearest enemy within 10 yards.", 5221, KIT_STEP, 5221, 0, 10.0f, 0),
                  Mod(545, 1, 1, 0, 3, "Deep Wounds", "15% more Rake and Rip damage per rank.", 1822, SPELLMOD_DOT, SPELLMOD_PCT, 15),
                  Ail(546, 1, 2, 545, 3, "Rending Claws", "Your Cat hits have an 8% chance per rank to cause Bleeding.", 16952, TAG_PHYSICAL, 8),
                  Kit(547, 1, 3, 546, 2, "Spreading Rake", "Rake also lands on 1/2 more enemies within 8 yards.", 1822,
                      KIT_SPREAD, 1822, { 1, 2 }, { 8, 8 }, { 100, 100 }),
                  KitCap(548, 1, 4, 547, "Bloodletting", "Rip also lands on 4 more enemies within 8 yards.", 1079, KIT_SPREAD, 1079, 4, 8.0f, 100),
                  Key(553, SKILL_NODE_SYNERGY, 1, 5, 548, 1, "Blood Frenzy", "When you consume Bleeding, it spreads to the 2 nearest enemies within 10 yards.", 1079, KEY_NONE),
                  Key(549, SKILL_NODE_SYNERGY, 2, 1, 0, 1, "Bloodied Bite", "A critical Claw or Shred makes your next Ferocious Bite a sure critical strike (once every 6 sec).", 22568, KEY_NONE),
                  Mod(550, 2, 2, 549, 3, "Savagery", "+4% Cat critical strike chance per rank.", 16998, SPELLMOD_CRITICAL_CHANCE, SPELLMOD_FLAT, 4),
                  Key(551, SKILL_NODE_TRANSFORMER, 2, 3, 550, 1, "Savage Fury", "Your critical Claws, Shreds and Rakes give an extra combo point.", 16958, KEY_SAVAGE_FURY),
                  KitCap(552, 2, 4, 551, "Feast", "Ferocious Bite strikes every enemy in front of you, at 60%.", 22568, KIT_ARC, 22568, 0, 0.0f, 60),
              } },
            { SKILL_REJUVENATION, CLASS_DRUID, "Rejuvenation", "Rejuvenation, Regrowth, Healing Touch and Entangling Roots: the druid's gifts, which its tree turns on your enemies too.", 774,
              { "Rejuvenation", "Regrowth", "Healing Touch", "Entangling Roots" },
              { "Bloom", "Wild", "Roots" },
              {
                  Mod(641, 0, 1, 0, 4, "Lush Growth", "6% more Regrowth and Healing Touch healing per rank.", 5185, SPELLMOD_DAMAGE, SPELLMOD_PCT, 6),
                  Mod(642, 0, 2, 641, 3, "Living Bloom", "Rejuvenation and Regrowth heal 10% more over time per rank.", 774, SPELLMOD_DOT, SPELLMOD_PCT, 10),
                  Key(643, SKILL_NODE_SYNERGY, 0, 3, 642, 1, "Overgrowth's Gift", "Entangling Roots lets your next Regrowth or Healing Touch skip its recovery (once every 10 sec).", 339, KEY_NONE),
                  Key(644, SKILL_NODE_CAPSTONE, 0, 4, 643, 1, "Bloom", "Your heals burst outward: every enemy within 8 yards takes 50% of the heal as Nature damage.", 740, KEY_BLOOM),
                  Key(645, SKILL_NODE_SYNERGY, 1, 1, 0, 3, "Nature's Wrath", "Your heals strike the nearest enemy within 20 yards for 15% of the heal per rank, as Nature damage.", 5176, KEY_NATURES_WRATH),
                  Key(646, SKILL_NODE_MODIFIER, 1, 2, 645, 2, "Verdant Fury", "Healing yourself gives you 5% more damage per rank for 5 sec.", 17116, KEY_VERDANT_FURY),
                  Key(647, SKILL_NODE_CAPSTONE, 1, 3, 646, 1, "Wrath of the Wild", "Your heals Poison every enemy within 8 yards, as a hit of 30% of the heal (once every 3 sec).", 5570, KEY_WRATH_OF_THE_WILD),
                  Kit(648, 2, 1, 0, 2, "Living Root", "Entangling Roots also roots 1/2 more enemies within 6 yards.", 339,
                      KIT_SPREAD, 339, { 1, 2 }, { 6, 6 }, { 100, 100 }),
                  Mod(649, 2, 2, 648, 3, "Grasping Vines", "20% more Entangling Roots damage per rank.", 16918, SPELLMOD_DOT, SPELLMOD_PCT, 20, "Entangling Roots"),
                  KitCap(650, 2, 3, 649, "Overgrowth", "Entangling Roots roots every enemy within 10 yards of its target.", 16689, KIT_SPREAD, 339, 10, 10.0f, 100),
                  Mod(651, 1, 4, 647, 3, "Thrift of the Grove", "10% less Rejuvenation skill mana cost per rank.", 17116, SPELLMOD_COST, SPELLMOD_PCT, -10),
                  More(652, 2, 4, 650, 3, "Thorny Vines", "10% more Entangling Roots damage per rank.", 339, 10, "Entangling Roots"),
              } },
            // --- The warlock's six, numbered 60-79 within each skill's hundred. ---
            { SKILL_SHADOW_BOLT, CLASS_WARLOCK, "Shadow Bolt", "Shadow Bolt, Searing Pain and Shadowburn: the warlock's bolts.", 686, { "Shadow Bolt", "Searing Pain", "Shadowburn" },
              { "Volley", "Nightfall", "Ruin" },
              {
                  Kit(161, 0, 1, 0, 3, "Splitting Bolt", "Shadow Bolt launches 1/1/2 extra bolts, at 45/60/60%.", 686,
                      KIT_EXTRA_PROJECTILES, 686, { 1, 1, 2 }, { 14, 14, 14 }, { 45, 60, 60 }),
                  Kit(162, 0, 2, 161, 3, "Piercing Shadow", "Shadow Bolt flies through 1/2/4 enemies, at 60%.", 686,
                      KIT_PIERCE, 686, { 1, 2, 4 }, { 0, 0, 0 }, { 60, 60, 60 }),
                  Ail(163, 0, 3, 162, 3, "Withering Bolt", "Your Shadow Bolt skill's hits have an 8% chance per rank to Wither.", 17877, TAG_SHADOW, 8),
                  KitCap(164, 0, 4, 163, "Shadowstorm", "Shadow Bolt bursts onto every enemy within 6 yards of its target, at 50%.", 17877, KIT_BURST, 686, 0, 6.0f, 50),
                  Key(165, SKILL_NODE_SYNERGY, 1, 1, 0, 3, "Nightfall", "Each tick of your Corruption and Curse of Agony has a 2% chance per rank to loose a free Shadow Bolt at its target.", 18094, KEY_NIGHTFALL),
                  Mod(166, 1, 2, 165, 3, "Quick Bolt", "-0.15 sec Shadow Bolt recovery per rank.", 17803, SPELLMOD_CASTING_TIME, SPELLMOD_FLAT, -150, "Shadow Bolt"),
                  Key(167, SKILL_NODE_SYNERGY, 1, 3, 166, 1, "Shadow Trance", "Shadow Bolt consumes Withered for an extra hit of 100% of the hit and the stacks.", 17941, KEY_NONE),
                  Mod(168, 2, 1, 0, 3, "Devastation", "+3% Shadow Bolt skill critical strike chance per rank.", 18130, SPELLMOD_CRITICAL_CHANCE, SPELLMOD_FLAT, 3),
                  Mod(169, 2, 2, 168, 3, "Ruin", "Critical strikes deal 20% more of their bonus per rank.", 17959, SPELLMOD_CRIT_DAMAGE_BONUS, SPELLMOD_PCT, 20),
                  Mod(170, 2, 3, 169, 3, "Searing Pain", "10% more Searing Pain damage per rank.", 5676, SPELLMOD_DAMAGE, SPELLMOD_PCT, 10, "Searing Pain"),
                  Key(171, SKILL_NODE_CAPSTONE, 2, 4, 170, 1, "Shadowburn", "A kill with Shadowburn readies it again.", 17877, KEY_SHADOWBURN),
              } },
            { SKILL_CORRUPTION, CLASS_WARLOCK, "Corruption", "Corruption, Curse of Agony and Siphon Life: shadow that rots and spreads.", 172, { "Corruption", "Curse of Agony", "Siphon Life" },
              { "Plague", "Agony", "Siphon" },
              {
                  Kit(261, 0, 1, 0, 3, "Spreading Corruption", "Corruption also lands on 1/2/3 more enemies within 8 yards.", 172,
                      KIT_SPREAD, 172, { 1, 2, 3 }, { 8, 8, 8 }, { 100, 100, 100 }),
                  Mod(262, 0, 2, 261, 3, "Rot", "15% more Corruption damage per rank.", 17810, SPELLMOD_DOT, SPELLMOD_PCT, 15, "Corruption"),
                  KitCap(263, 0, 3, 262, "Pandemic", "Corruption spreads to every enemy within 10 yards when its target dies.", 18265, KIT_SPREAD, 172, 0, 10.0f, 100),
                  Kit(264, 1, 1, 0, 2, "Lingering Curse", "Curse of Agony also lands on 1/2 more enemies within 8 yards.", 980,
                      KIT_SPREAD, 980, { 1, 2 }, { 8, 8 }, { 100, 100 }),
                  Key(265, SKILL_NODE_SYNERGY, 1, 2, 264, 3, "Tormented Soul", "Each tick of your Curse of Agony has a 10% chance per rank to Wither its target.", 18827, KEY_TORMENTED_SOUL),
                  Key(266, SKILL_NODE_CAPSTONE, 1, 3, 265, 1, "Haunting Agony", "Curse of Agony deals 100% more to Withered enemies.", 18829, KEY_HAUNTING_AGONY),
                  Mod(267, 2, 1, 0, 3, "Siphoning", "15% more Siphon Life damage (and healing) per rank.", 18265, SPELLMOD_DOT, SPELLMOD_PCT, 15, "Siphon Life"),
                  Mod(268, 2, 2, 267, 2, "Grim Reach", "+3 sec Corruption, Curse of Agony and Siphon Life duration per rank.", 18218, SPELLMOD_DURATION, SPELLMOD_FLAT, 3000),
                  Key(269, SKILL_NODE_CAPSTONE, 2, 3, 268, 1, "Soul Feast", "Every tick of your damage over time heals you for 0.3% of your maximum health, 3% a second at most.", 18265, KEY_SOUL_FEAST),
                  More(270, 0, 4, 263, 4, "Blight", "6% more Corruption skill damage per rank.", 17810, 6),
                  More(271, 1, 4, 266, 3, "Anguish", "10% more Curse of Agony damage per rank.", 980, 10, "Curse of Agony"),
                  Mod(272, 2, 4, 269, 2, "Dark Thrift", "15% less Corruption skill mana cost per rank.", 18218, SPELLMOD_COST, SPELLMOD_PCT, -15),
              } },
            { SKILL_IMMOLATE, CLASS_WARLOCK, "Immolate", "Immolate, Conflagrate, Rain of Fire and Hellfire: the warlock's fire. An ARPG warlock has Rain of Fire from level 10 and Hellfire from 20.", 348,
              { "Immolate", "Conflagrate", "Rain of Fire", "Hellfire" },
              { "Pyre", "Cataclysm", "Hellfire" },
              {
                  Kit(361, 0, 1, 0, 2, "Spreading Flame", "Immolate also lands on 1/2 more enemies within 8 yards.", 348,
                      KIT_SPREAD, 348, { 1, 2 }, { 8, 8 }, { 100, 100 }),
                  Key(362, SKILL_NODE_MODIFIER, 0, 2, 361, 4, "Emberstorm", "6% more Immolate skill damage per rank, its burns and Rain of Fire's waves too.", 17954, KEY_EMBERSTORM),
                  Key(363, SKILL_NODE_SYNERGY, 0, 3, 362, 1, "Conflagration", "Conflagrate consumes Ignite to burst onto every enemy within 8 yards for the hit and the burn left.", 17962, KEY_NONE),
                  Mod(364, 1, 1, 0, 3, "Wide Rain", "+12% Rain of Fire radius per rank.", 5740, SPELLMOD_RADIUS, SPELLMOD_PCT, 12, "Rain of Fire"),
                  Key(365, SKILL_NODE_TRANSFORMER, 1, 2, 364, 3, "Burning Rain", "Each Rain of Fire wave has a 10% chance per rank to Ignite the enemy it strikes.", 5740, KEY_BURNING_RAIN),
                  Key(366, SKILL_NODE_CAPSTONE, 1, 3, 365, 1, "Firestorm", "Rain of Fire strikes Ignited enemies twice.", 11683, KEY_FIRESTORM),
                  Key(367, SKILL_NODE_MODIFIER, 2, 1, 0, 3, "Hellfire Ward", "25% less damage to yourself from Hellfire per rank.", 1949, KEY_HELLFIRE_WARD),
                  Key(368, SKILL_NODE_SYNERGY, 2, 2, 367, 3, "Infernal Heat", "Each tick of your Hellfire gives your minions 5% more damage per rank for 4 sec.", 11683, KEY_INFERNAL_HEAT),
                  Key(369, SKILL_NODE_CAPSTONE, 2, 3, 368, 1, "Backlash", "While you channel Hellfire, you take 20% less damage.", 11684, KEY_BACKLASH),
                  Mod(370, 0, 4, 363, 3, "Fel Kindling", "10% less Immolate skill mana cost per rank.", 348, SPELLMOD_COST, SPELLMOD_PCT, -10),
                  More(371, 1, 4, 366, 3, "Downpour", "10% more Rain of Fire damage per rank.", 5740, 10, "Rain of Fire"),
              } },
            { SKILL_DRAIN, CLASS_WARLOCK, "Drain", "Drain Life, Drain Soul, Death Coil and Life Tap: the warlock lives off its enemies, and off itself.", 689,
              { "Drain Life", "Drain Soul", "Death Coil", "Life Tap" },
              { "Siphon", "Reaping", "Pact" },
              {
                  Mod(461, 0, 1, 0, 4, "Grim Drain", "10% more Drain Life damage per rank.", 17783, SPELLMOD_DOT, SPELLMOD_PCT, 10, "Drain Life"),
                  Kit(462, 0, 2, 461, 2, "Chained Drain", "Drain Life also drains 1/2 more enemies within 10 yards.", 689,
                      KIT_SPREAD, 689, { 1, 2 }, { 10, 10 }, { 100, 100 }),
                  Key(463, SKILL_NODE_CAPSTONE, 0, 3, 462, 1, "Life Link", "Each tick of your Drain Life heals your minions for 50% of its damage.", 755, KEY_LIFE_LINK),
                  Key(464, SKILL_NODE_MODIFIER, 1, 1, 0, 3, "Soul Reaper", "Drain Soul deals 30% more per rank to enemies below 25% health.", 1120, KEY_SOUL_REAPER),
                  Key(465, SKILL_NODE_CAPSTONE, 1, 2, 464, 1, "Reaping", "A kill under your Drain Soul raises an imp to fight for you and restores 5% of your maximum mana.", 18371, KEY_REAPING),
                  Key(466, SKILL_NODE_MODIFIER, 2, 1, 0, 3, "Dark Pact", "Life Tap gives you 1% more of your maximum mana per rank.", 18220, KEY_DARK_PACT),
                  Mod(467, 2, 2, 466, 2, "Death's Grasp", "-10 sec Death Coil cooldown per rank.", 6789, SPELLMOD_COOLDOWN, SPELLMOD_FLAT, -10000, "Death Coil"),
                  Key(468, SKILL_NODE_CAPSTONE, 2, 3, 467, 1, "Demonic Pact", "Life Tap also heals you for 5% of your maximum health, taken from your demon.", 18220, KEY_DEMONIC_PACT),
                  More(469, 0, 4, 463, 4, "Essence Drain", "6% more Drain skill damage per rank.", 17783, 6),
                  More(470, 1, 3, 465, 3, "Soul Hunger", "10% more Drain Soul damage per rank.", 1120, 10, "Drain Soul"),
                  Mod(471, 2, 4, 468, 3, "Dark Bargain", "10% less Drain skill mana cost per rank.", 18220, SPELLMOD_COST, SPELLMOD_PCT, -10),
              } },
            { SKILL_DEMON, CLASS_WARLOCK, "Demon", "Your demon: the Imp burns, the Voidwalker holds, the Succubus withers, the Felhunter eats spells. Summoning needs no Soul Shard and casts at once.", 688,
              { "Summon Imp", "Summon Voidwalker", "Summon Succubus", "Summon Felhunter", "Firebolt", "Blood Pact", "Fire Shield", "Phase Shift",
                "Torment", "Sacrifice", "Suffering", "Consume Shadows", "Lash of Pain", "Seduction", "Soothing Kiss", "Lesser Invisibility",
                "Devour Magic", "Spell Lock", "Tainted Blood", "Paranoia", "Fel Domination", "Soul Link", "Health Funnel" },
              { "Brute", "Seduction", "Command" },
              {
                  Key(561, SKILL_NODE_MODIFIER, 0, 1, 0, 4, "Fel Vitality", "Your demon has 10% more life per rank.", 18731, KEY_FEL_VITALITY),
                  Key(562, SKILL_NODE_MODIFIER, 0, 2, 561, 3, "Demonic Embrace", "Your demon deals 8% more damage per rank.", 18697, KEY_DEMONIC_EMBRACE),
                  Key(563, SKILL_NODE_TRANSFORMER, 0, 3, 562, 3, "Cleaving Demon", "Your demon's melee also strikes every enemy in front of it, the extras at 30/45/60%.", 845, KEY_CLEAVING_DEMON),
                  Key(564, SKILL_NODE_CAPSTONE, 0, 4, 563, 1, "Fel Domination", "When your demon dies, it is summoned again at once (once every 30 sec).", 18708, KEY_FEL_DOMINATION),
                  Key(565, SKILL_NODE_TRANSFORMER, 1, 1, 0, 2, "Lash Arc", "Lash of Pain and Firebolt also strike 1/2 more enemies within 6 yards, at 50%.", 7814, KEY_LASH_ARC),
                  Key(566, SKILL_NODE_MODIFIER, 1, 2, 565, 3, "Demonic Mastery", "While your demon lives, you deal 4% more damage per rank.", 23825, KEY_DEMONIC_MASTERY),
                  Key(567, SKILL_NODE_CAPSTONE, 1, 3, 566, 1, "Grand Pact", "Your demon has 40% more life and deals 25% more damage.", 18788, KEY_GRAND_PACT),
                  Key(568, SKILL_NODE_SYNERGY, 2, 1, 0, 1, "Torment", "Your curses command: every minion turns on the cursed enemy and deals it 20% more for 4 sec.", 3716, KEY_TORMENT_COMMAND),
                  // A choice under Torment: command the horde and lean on your demon, or give the
                  // demon up for power of your own (the Destroyer's way, no demon nodes bought).
                  Choice(Key(569, SKILL_NODE_SYNERGY, 2, 2, 568, 1, "Sacrifice", "Life Tap detonates your oldest horde minion: every enemy within 8 yards takes Fire damage of a quarter of its life.", 7812, KEY_SACRIFICE_COMMAND), 1),
                  Key(570, SKILL_NODE_MODIFIER, 2, 3, 569, 3, "Soul Link", "10% of the damage you take per rank is taken by your demon instead.", 19028, KEY_SOUL_LINK),
                  Choice(Key(571, SKILL_NODE_CAPSTONE, 2, 2, 568, 1, "Demonic Sacrifice", "With no demon at your side, you deal 30% more damage.", 18788, KEY_DEMONIC_SACRIFICE), 1),
              } },
            { SKILL_LEGION, CLASS_WARLOCK, "Legion", "Inferno and Ritual of Doom, and the horde: imps, Infernals and the Doomguard. An ARPG warlock has Inferno at 20 and the Doomguard at 40, with no stone, party or ritual.", 1122,
              { "Inferno", "Ritual of Doom" },
              { "Imps", "Infernal", "Doom" },
              {
                  Key(661, SKILL_NODE_TRANSFORMER, 0, 1, 0, 3, "Imp Gate", "Your kills have a 25% chance per rank to raise an imp that fights for you for 20 sec. Under Legion, each kill has that chance to raise a second.", 688, KEY_IMP_GATE),
                  Key(662, SKILL_NODE_MODIFIER, 0, 2, 661, 2, "Imp Swarm", "The horde holds 2 more minions per rank.", 688, KEY_IMP_SWARM),
                  Key(663, SKILL_NODE_SYNERGY, 0, 3, 662, 3, "Fiery Swarm", "Your horde imps' Firebolts have a 10% chance per rank to Ignite.", 3110, KEY_FIERY_SWARM),
                  Key(664, SKILL_NODE_CAPSTONE, 0, 4, 663, 1, "Burning Legion", "A horde minion's kill raises another imp.", 1122, KEY_BURNING_LEGION),
                  Key(665, SKILL_NODE_TRANSFORMER, 1, 1, 0, 3, "Infernal Fall", "The Infernal's landing stuns and strikes every enemy within 8/10/12 yards.", 22703, KEY_INFERNAL_FALL),
                  Key(666, SKILL_NODE_MODIFIER, 1, 2, 665, 3, "Immolation Aura", "Your Infernals deal 20% more damage per rank.", 19483, KEY_IMMOLATION_AURA),
                  Key(667, SKILL_NODE_CAPSTONE, 1, 3, 666, 1, "Lord of Flame", "Inferno calls down two Infernals.", 1122, KEY_LORD_OF_FLAME),
                  Mod(668, 2, 1, 0, 2, "Ritual Haste", "-60 sec Ritual of Doom cooldown per rank.", 18540, SPELLMOD_COOLDOWN, SPELLMOD_FLAT, -60000, "Ritual of Doom"),
                  Key(669, SKILL_NODE_MODIFIER, 2, 2, 668, 3, "Doombringer", "Your Doomguard deals 15% more damage per rank.", 18540, KEY_DOOMBRINGER),
                  Key(670, SKILL_NODE_CAPSTONE, 2, 3, 669, 1, "Pit Lord", "Your Doomguard stays for 10 min.", 18540, KEY_PIT_LORD),
              } },
            // --- The shaman's six, numbered 81-99 within each skill's hundred (the client's
            // docs/ARPG-CHARACTER.md, "The shaman, in full"; the hooks are Arpg/ArpgShaman.cpp). ---
            { SKILL_LIGHTNING, CLASS_SHAMAN, "Lightning", "Lightning Bolt, Chain Lightning and Lightning Shield: lightning that jumps, and leaves its foes Shocked.", 403,
              { "Lightning Bolt", "Chain Lightning", "Lightning Shield" },
              { "Storm", "Overload", "Conduit" },
              {
                  Kit(181, 0, 1, 0, 3, "Forked Lightning", "Lightning Bolt chains to 1/2/3 more enemies within 10 yards, at 50%.", 403,
                      KIT_CHAIN, 403, { 1, 2, 3 }, { 10, 10, 10 }, { 50, 50, 50 }),
                  Mod(182, 0, 2, 181, 3, "Quick Bolt", "-0.15 sec Lightning Bolt recovery per rank.", 403, SPELLMOD_CASTING_TIME, SPELLMOD_FLAT, -150, "Lightning Bolt"),
                  Key(183, SKILL_NODE_MODIFIER, 0, 3, 182, 3, "Static", "Your Lightning hits have a 10% chance per rank to Shock twice.", 324, KEY_STATIC),
                  KitCap(184, 0, 4, 183, "Thunderstorm", "Chain Lightning bursts onto every enemy within 6 yards of each it strikes, at 40%.", 421, KIT_BURST, 421, 0, 6.0f, 40),
                  Key(185, SKILL_NODE_SYNERGY, 1, 1, 0, 3, "Lightning Overload", "Your Lightning hits have a 5% chance per rank to strike again, at 50%.", 16166, KEY_LIGHTNING_OVERLOAD),
                  More(186, 1, 2, 185, 4, "Concussive Storm", "6% more Lightning damage per rank.", 421, 6),
                  ModCap(187, 1, 3, 186, "Lightning Fury", "Your Lightning's critical strikes deal 100% more of their bonus.", 16166, SPELLMOD_CRIT_DAMAGE_BONUS, SPELLMOD_PCT, 100),
                  Key(188, SKILL_NODE_MODIFIER, 2, 1, 0, 3, "Conduit", "Each Shock consumed restores 1% of your maximum mana per rank.", 324, KEY_CONDUIT),
                  Key(189, SKILL_NODE_TRANSFORMER, 2, 2, 188, 3, "Charged Shield", "Lightning Shield's discharge Shocks its attacker, and deals 25% more per rank.", 324, KEY_CHARGED_SHIELD),
                  Key(190, SKILL_NODE_CAPSTONE, 2, 3, 189, 1, "Ball Lightning", "Each Shocked enemy's death throws a Lightning Bolt at the nearest enemy.", 403, KEY_BALL_LIGHTNING),
                  Mod(191, 2, 4, 190, 3, "Thrift of the Storm", "10% less Chain Lightning mana cost per rank.", 421, SPELLMOD_COST, SPELLMOD_PCT, -10, "Chain Lightning"),
              } },
            { SKILL_SHOCKS, CLASS_SHAMAN, "Shocks", "Earth Shock, Flame Shock and Frost Shock: the payoffs, one for each element.", 8042,
              { "Earth Shock", "Flame Shock", "Frost Shock" },
              { "Earth", "Flame", "Frost" },
              {
                  Key(281, SKILL_NODE_SYNERGY, 0, 1, 0, 1, "Earthquake", "When Earth Shock consumes a Shock, every enemy within 6 yards takes the payoff too.", 8042, KEY_EARTHQUAKE),
                  Mod(282, 0, 2, 281, 3, "Reverberation", "-1 sec Shock cooldown per rank.", 8042, SPELLMOD_COOLDOWN, SPELLMOD_FLAT, -1000),
                  Key(283, SKILL_NODE_CAPSTONE, 0, 3, 282, 1, "Earthen Power", "Earth Shock Dazes its target.", 8042, KEY_EARTHEN_POWER),
                  More(284, 0, 4, 283, 4, "Tectonic Force", "8% more Earth Shock damage per rank.", 8042, 8, "Earth Shock"),
                  Ail(285, 1, 1, 0, 3, "Searing Flames", "Your Shock hits have a 15% chance per rank to Ignite.", 8050, TAG_FIRE, 15),
                  Kit(286, 1, 2, 285, 2, "Flame Spread", "Flame Shock also lands on 1/2 more enemies within 8 yards.", 8050,
                      KIT_SPREAD, 8050, { 1, 2 }, { 8, 8 }, { 100, 100 }),
                  Key(287, SKILL_NODE_CAPSTONE, 1, 3, 286, 1, "Lava Burst", "Flame Shock on an Ignited enemy consumes the Ignite to burst onto every enemy within 8 yards for the burn left.", 8050, KEY_LAVA_BURST),
                  More(288, 1, 4, 287, 3, "Magma Core", "10% more Flame Shock damage per rank.", 8050, 10, "Flame Shock"),
                  More(289, 2, 1, 0, 3, "Frozen Power", "10% more Frost Shock damage per rank.", 8056, 10, "Frost Shock"),
                  Key(290, SKILL_NODE_SYNERGY, 2, 2, 289, 3, "Frostbite", "Frost Shock on a Chilled enemy has a 15% chance per rank to Freeze it.", 8056, KEY_FROSTBITE_SHOCK),
                  KitCap(291, 2, 3, 290, "Glacial Shock", "Frost Shock bursts onto every enemy within 6 yards of its target, at 80%.", 8056, KIT_BURST, 8056, 0, 6.0f, 80),
                  Mod(292, 2, 4, 291, 3, "Shock Thrift", "10% less Shock mana cost per rank.", 8042, SPELLMOD_COST, SPELLMOD_PCT, -10),
              } },
            { SKILL_FIRE_TOTEMS, CLASS_SHAMAN, "Fire Totems", "Searing, Magma and Fire Nova Totems: totems that fight, as minions. An ARPG shaman has Fire Nova Totem at 10 and Magma Totem at 20.", 3599,
              { "Searing Totem", "Magma Totem", "Fire Nova Totem", "Fire Nova", "Attack" },
              { "Searing", "Magma", "Nova" },
              {
                  Key(381, SKILL_NODE_TRANSFORMER, 0, 1, 0, 3, "Searing Bolts", "Searing Totem's bolts strike 1/2/3 more enemies within 8 yards of their target, at 50%.", 3599, KEY_SEARING_BOLTS),
                  Ail(382, 0, 2, 381, 3, "Burning Totem", "Your Fire Totems' hits have a 10% chance per rank to Ignite.", 3599, TAG_FIRE, 10),
                  Key(383, SKILL_NODE_CAPSTONE, 0, 3, 382, 1, "Twin Totems", "Searing Totem throws two bolts at once, as two totems would.", 3599, KEY_TWIN_TOTEMS),
                  More(384, 0, 4, 383, 4, "Searing Heat", "8% more Searing Totem damage per rank.", 3599, 8, "Attack"),
                  Mod(385, 1, 1, 0, 3, "Wide Magma", "+15% Magma Totem radius per rank.", 8190, SPELLMOD_RADIUS, SPELLMOD_PCT, 15, "Magma Totem"),
                  More(386, 1, 2, 385, 4, "Magma Pulse", "8% more Magma Totem damage per rank.", 8190, 8, "Magma Totem"),
                  Key(387, SKILL_NODE_CAPSTONE, 1, 3, 386, 1, "Molten Core", "Magma Totem's pulses Ignite every enemy they strike.", 8190, KEY_MOLTEN_CORE),
                  More(388, 2, 1, 0, 3, "Nova Fury", "10% more Fire Nova damage per rank.", 1535, 10, "Fire Nova"),
                  Key(389, SKILL_NODE_MODIFIER, 2, 2, 388, 2, "Echoing Nova", "Fire Nova strikes each enemy 1/2 more times, at 60%.", 1535, KEY_ECHOING_NOVA),
                  Key(390, SKILL_NODE_CAPSTONE, 2, 3, 389, 1, "Cataclysmic Nova", "Fire Nova consumes the Ignite on every enemy it strikes, for the burn left.", 1535, KEY_CATACLYSMIC_NOVA),
                  Mod(391, 2, 4, 390, 3, "Totem Thrift", "10% less Fire Totem mana cost per rank.", 3599, SPELLMOD_COST, SPELLMOD_PCT, -10),
              } },
            { SKILL_EARTHEN_TOTEMS, CLASS_SHAMAN, "Earthen Totems", "Stoneclaw, Earthbind, Strength of Earth, Windfury, Grace of Air and Tremor Totems: totems that hold and empower.", 5730,
              { "Stoneclaw Totem", "Earthbind Totem", "Strength of Earth Totem", "Windfury Totem", "Grace of Air Totem", "Tremor Totem", "Stoneskin Totem" },
              { "Stone", "Wind", "Ward" },
              {
                  Key(481, SKILL_NODE_TRANSFORMER, 0, 1, 0, 3, "Stoneclaw", "Stoneclaw Totem has 25% more life per rank, and every enemy within 10 yards turns on it when it is placed.", 5730, KEY_STONECLAW),
                  Key(482, SKILL_NODE_SYNERGY, 0, 2, 481, 1, "Earth's Grasp", "The enemies your Earthbind Totem slows are Vulnerable.", 2484, KEY_EARTHS_GRASP),
                  Key(483, SKILL_NODE_CAPSTONE, 0, 3, 482, 1, "Totemic Ward", "While a totem of yours stands, you take 10% less damage.", 8071, KEY_TOTEMIC_WARD),
                  Mod(484, 0, 4, 483, 3, "Earthen Thrift", "10% less Earthen totem mana cost per rank.", 8071, SPELLMOD_COST, SPELLMOD_PCT, -10),
                  Key(485, SKILL_NODE_MODIFIER, 1, 1, 0, 3, "Wind's Edge", "+4% attack speed per rank while an air totem of yours stands.", 8512, KEY_WINDS_EDGE),
                  Key(486, SKILL_NODE_MODIFIER, 1, 2, 485, 3, "Air Mastery", "Your Lightning deals 4% more per rank while an air totem of yours stands.", 8835, KEY_AIR_MASTERY),
                  Key(487, SKILL_NODE_CAPSTONE, 1, 3, 486, 1, "Storm Totem", "Your Lightning hits on enemies within 10 yards of a totem of yours Shock twice.", 8512, KEY_STORM_TOTEM),
                  Mod(488, 2, 1, 0, 3, "Totemic Reach", "+15% Earthen totem radius per rank.", 8143, SPELLMOD_RADIUS, SPELLMOD_PCT, 15),
                  Key(489, SKILL_NODE_MODIFIER, 2, 2, 488, 2, "Earthen Vigor", "While a totem of yours stands, you regain 0.5% of your maximum health a second per rank.", 8143, KEY_EARTHEN_VIGOR),
                  ModCap(490, 2, 3, 489, "Ancestral Totems", "Your Earthen totems last twice as long.", 8143, SPELLMOD_DURATION, SPELLMOD_PCT, 100),
              } },
            { SKILL_SHAMAN_STRIKE, CLASS_SHAMAN, "Strike", "Your held swing, Stormstrike and the weapon imbues: the Enhancer.", 17364,
              { "Stormstrike", "Windfury Weapon", "Flametongue Weapon", "Frostbrand Weapon", "Rockbiter Weapon", "Windfury Attack",
                "Flametongue Attack", "Frostbrand Attack" },
              { "Windfury", "Imbue", "Storm" },
              {
                  Key(581, SKILL_NODE_MODIFIER, 0, 1, 0, 3, "Windfury", "Your Windfury Weapon strikes 10% more often per rank.", 8232, KEY_WINDFURY),
                  Key(582, SKILL_NODE_TRANSFORMER, 0, 2, 581, 3, "Tempest Swing", "Windfury's strikes also hit every enemy in front of you, at 30/45/60%.", 8232, KEY_TEMPEST_SWING),
                  Key(583, SKILL_NODE_CAPSTONE, 0, 3, 582, 1, "Unleashed Rage", "Each Windfury strike gives you 5% more Melee damage for 6 sec, stacking to 5.", 16257, KEY_UNLEASHED_RAGE),
                  More(584, 0, 4, 583, 4, "Weapon Fury", "6% more Strike damage per rank.", 17364, 6),
                  Key(585, SKILL_NODE_MODIFIER, 1, 1, 0, 3, "Rockbiter", "Your hits build 15% more Daze per rank.", 8017, KEY_ROCKBITER),
                  // A choice of imbue: fire's Ignites or frost's Chills.
                  Choice(Conv(586, 1, 2, 585, "Flametongue", "Your Strike hits count as Fire: they Ignite, not Bleed, and build no Daze.", 8024, TAG_FIRE), 1),
                  Choice(Conv(587, 1, 2, 585, "Frostbrand", "Your Strike hits count as Frost: a critical one Chills (Freezes the Chilled), and build no Daze.", 8033, TAG_FROST), 1),
                  Key(588, SKILL_NODE_MODIFIER, 1, 3, 585, 3, "Flurry", "+4% attack speed per rank.", 16257, KEY_QUICKENED),
                  Key(589, SKILL_NODE_SYNERGY, 2, 1, 0, 3, "Charged Strike", "Stormstrike Shocks its target 1/2/3 times.", 17364, KEY_STORMSTRIKE_SHOCK),
                  Key(590, SKILL_NODE_TRANSFORMER, 2, 2, 589, 3, "Static Shock", "Your swings on a Shocked enemy have a 10% chance per rank to throw a Lightning Bolt at it.", 403, KEY_STATIC_SHOCK),
                  Key(591, SKILL_NODE_CAPSTONE, 2, 3, 590, 1, "Stormbringer", "Your critical swings Shock every enemy within 8 yards.", 421, KEY_STORMBRINGER),
                  Mod(592, 2, 4, 591, 3, "Quick Storm", "-1 sec Stormstrike cooldown per rank.", 17364, SPELLMOD_COOLDOWN, SPELLMOD_FLAT, -1000, "Stormstrike"),
              } },
            { SKILL_HEALING_WAVE, CLASS_SHAMAN, "Healing Wave", "Healing Wave, Lesser Healing Wave and Chain Heal: heals, which its tree turns on your enemies too.", 331,
              { "Healing Wave", "Lesser Healing Wave", "Chain Heal" },
              { "Tide", "Ancestral", "Torrent" },
              {
                  Mod(681, 0, 1, 0, 4, "Tidal Force", "6% more healing per rank.", 331, SPELLMOD_DAMAGE, SPELLMOD_PCT, 6),
                  Mod(682, 0, 2, 681, 3, "Healing Grace", "-0.2 sec recovery per rank.", 8004, SPELLMOD_CASTING_TIME, SPELLMOD_FLAT, -200),
                  Key(683, SKILL_NODE_SYNERGY, 0, 3, 682, 3, "Riptide", "Your heals strike the nearest enemy within 20 yards for 15% of the heal per rank, as Nature.", 403, KEY_NATURES_WRATH),
                  Key(684, SKILL_NODE_SYNERGY, 1, 1, 0, 2, "Ancestral Healing", "Healing yourself gives you 5% more damage per rank for 5 sec.", 16176, KEY_VERDANT_FURY),
                  Mod(685, 1, 2, 684, 3, "Tidal Focus", "10% less mana cost per rank.", 16179, SPELLMOD_COST, SPELLMOD_PCT, -10),
                  Key(686, SKILL_NODE_CAPSTONE, 1, 3, 685, 1, "Earthliving", "Your heals Shock every enemy within 8 yards.", 421, KEY_EARTHLIVING),
                  Mod(687, 2, 1, 0, 3, "Healing Way", "+4% critical strike chance per rank.", 331, SPELLMOD_CRITICAL_CHANCE, SPELLMOD_FLAT, 4),
                  Key(688, SKILL_NODE_SYNERGY, 2, 2, 687, 1, "Torrent", "Your heals burst outward: every enemy within 8 yards takes 50% of the heal as Nature.", 1064, KEY_BLOOM),
                  Key(689, SKILL_NODE_CAPSTONE, 2, 3, 688, 1, "Healing Stream", "Your heals Poison every enemy within 8 yards, a hit of 30% of the heal (once every 3 sec).", 5394, KEY_WRATH_OF_THE_WILD),
                  Mod(690, 1, 4, 686, 3, "Nature's Guidance", "-0.2 sec Chain Heal recovery per rank.", 1064, SPELLMOD_CASTING_TIME, SPELLMOD_FLAT, -200, "Chain Heal"),
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
        // The druid's
        { 144, 11492, "Alzzin the Wildshaper, Dire Maul" },             // Wrath of Cenarius
        { 148, 5709,  "Shade of Eranikus, the Temple of Atal'Hakkar" }, // Starfall
        { 152, 12201, "Princess Theradras, Maraudon" },                 // Eclipse
        { 244, 4832,  "Twilight Lord Kelris, Blackfathom Deeps" },      // Full Moon
        { 248, 13282, "Noxxion, Maraudon" },                            // Hive Mind
        { 251, 4829,  "Aku'mai, Blackfathom Deeps" },                   // Lunar Tide
        { 344, 3654,  "Mutanus the Devourer, Wailing Caverns" },        // Tempest
        { 348, 3673,  "Lord Serpentis, Wailing Caverns" },              // Thunderhead
        { 444, 4421,  "Charlga Razorflank, Razorfen Kraul" },           // Earthshaker
        { 447, 11489, "Tendris Warpwood, Dire Maul" },                  // Ravaging Swipe
        { 450, 12225, "Celebras the Cursed, Maraudon" },                // Thorned Hide
        { 544, 3669,  "Lord Cobrahn, Wailing Caverns" },                // Stalker's Step
        { 548, 3975,  "Herod, Scarlet Monastery" },                     // Bloodletting
        { 552, 11501, "King Gordok, Dire Maul" },                       // Feast
        { 644, 5775,  "Verdan the Everliving, Wailing Caverns" },       // Bloom
        { 647, 3671,  "Lady Anacondra, Wailing Caverns" },              // Wrath of the Wild
        { 650, 3670,  "Lord Pythas, Wailing Caverns" },                 // Overgrowth
        // The warlock's
        { 164, 4275,  "Archmage Arugal, Shadowfang Keep" },             // Shadowstorm
        { 171, 9019,  "Emperor Dagran Thaurissan, Blackrock Depths" },  // Shadowburn
        { 263, 7358,  "Amnennar the Coldbringer, Razorfen Downs" },     // Pandemic
        { 266, 10503, "Jandice Barov, Scholomance" },                   // Haunting Agony
        { 269, 1853,  "Darkmaster Gandling, Scholomance" },             // Soul Feast
        { 366, 9017,  "Lord Incendius, Blackrock Depths" },             // Firestorm
        { 369, 9816,  "Pyroguard Emberseer, Upper Blackrock Spire" },   // Backlash
        { 463, 10813, "Balnazzar, Stratholme" },                        // Life Link
        { 465, 10440, "Baron Rivendare, Stratholme" },                  // Reaping
        { 468, 11486, "Prince Tortheldrin, Dire Maul" },                // Demonic Pact
        { 564, 11520, "Taragaman the Hungerer, Ragefire Chasm" },       // Fel Domination
        { 567, 11496, "Immol'thar, Dire Maul" },                        // Grand Pact
        { 571, 5709,  "Shade of Eranikus, the Temple of Atal'Hakkar" }, // Demonic Sacrifice
        { 664, 2748,  "Archaedas, Uldaman" },                           // Burning Legion
        { 667, 9568,  "Overlord Wyrmthalak, Lower Blackrock Spire" },   // Lord of Flame
        { 670, 10429, "Warchief Rend Blackhand, Upper Blackrock Spire" },// Pit Lord
        // The shaman's
        { 184, 3654,  "Mutanus the Devourer, Wailing Caverns" },        // Thunderstorm
        { 187, 7267,  "Chief Ukorz Sandscalp, Zul'Farrak" },            // Lightning Fury
        { 190, 4422,  "Agathelos the Raging, Razorfen Kraul" },         // Ball Lightning
        { 283, 7228,  "Ironaya, Uldaman" },                             // Earthen Power
        { 287, 9938,  "Magmus, Blackrock Depths" },                     // Lava Burst
        { 291, 4829,  "Aku'mai, Blackfathom Deeps" },                   // Glacial Shock
        { 383, 9156,  "Ambassador Flamelash, Blackrock Depths" },       // Twin Totems
        { 387, 9816,  "Pyroguard Emberseer, Upper Blackrock Spire" },   // Molten Core
        { 390, 9017,  "Lord Incendius, Blackrock Depths" },             // Cataclysmic Nova
        { 483, 2748,  "Archaedas, Uldaman" },                           // Totemic Ward
        { 487, 12201, "Princess Theradras, Maraudon" },                 // Storm Totem
        { 490, 4421,  "Charlga Razorflank, Razorfen Kraul" },           // Ancestral Totems
        { 583, 11501, "King Gordok, Dire Maul" },                       // Unleashed Rage
        { 591, 9019,  "Emperor Dagran Thaurissan, Blackrock Depths" },  // Stormbringer
        { 686, 5775,  "Verdan the Everliving, Wailing Caverns" },       // Earthliving
        { 689, 3671,  "Lady Anacondra, Wailing Caverns" },              // Healing Stream
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
        uint8 druidFits = 0;      // and a druid's
        uint8 warlockFits = 0;    // and a warlock's
        uint8 shamanFits = 0;     // and a shaman's
    };

    // The skills of `classId`'s a rune fits: a bit per SkillId (0: none, a rune of another class).
    uint8 FitsFor(RuneDef const& rune, uint8 classId)
    {
        switch (classId)
        {
            case CLASS_PALADIN: return rune.fits;
            case CLASS_MAGE: return rune.mageFits;
            case CLASS_DRUID: return rune.druidFits;
            case CLASS_WARLOCK: return rune.warlockFits;
            case CLASS_SHAMAN: return rune.shamanFits;
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
              uint8(FIT(SKILL_JUDGEMENT) | FIT(SKILL_WRATH)), RUNE_KIT, KIT_CHAIN, 1, 10.0f, 50, {}, uint8(FIT(SKILL_FIREBALL) | FIT(SKILL_FROSTBOLT) | FIT(SKILL_ARCANE_MISSILES) | FIT(SKILL_FIRE_BLAST)), uint8(FIT(SKILL_DRUID_WRATH) | FIT(SKILL_MOONFIRE)),
              uint8(FIT(SKILL_SHADOW_BOLT) | FIT(SKILL_DRAIN)), uint8(FIT(SKILL_LIGHTNING) | FIT(SKILL_SHOCKS)) },
            { RUNE_SHATTERING, "Rune of Shattering", "The skill's hits burst onto enemies within 6 yards of the target, at 35% (+15% on a burst its tree gives).", 24275,
              uint8(FIT(SKILL_JUDGEMENT) | FIT(SKILL_STRIKE) | FIT(SKILL_WRATH)), RUNE_KIT, KIT_BURST, 0, 6.0f, 35, {}, uint8(FIT(SKILL_FIREBALL) | FIT(SKILL_FROSTBOLT) | FIT(SKILL_ARCANE_MISSILES) | FIT(SKILL_FIRE_BLAST)), uint8(FIT(SKILL_DRUID_WRATH) | FIT(SKILL_MOONFIRE) | FIT(SKILL_BEAR) | FIT(SKILL_CAT)),
              uint8(FIT(SKILL_SHADOW_BOLT) | FIT(SKILL_IMMOLATE)), uint8(FIT(SKILL_LIGHTNING) | FIT(SKILL_SHOCKS)) },
            { RUNE_EXPANSE, "Rune of Expanse", "+30% area.", 2812,
              uint8(FIT(SKILL_CONSECRATION) | FIT(SKILL_WRATH)), RUNE_MODS, UniqueKit(0), 0, 0.0f, 0, { std::make_tuple(uint8(SPELLMOD_RADIUS), uint8(SPELLMOD_PCT), int32(30)) },
              uint8(FIT(SKILL_FROSTBOLT) | FIT(SKILL_ARCANE_EXPLOSION) | FIT(SKILL_STORMS)), uint8(FIT(SKILL_HURRICANE) | FIT(SKILL_BEAR)),
              uint8(FIT(SKILL_IMMOLATE)), uint8(FIT(SKILL_FIRE_TOTEMS) | FIT(SKILL_EARTHEN_TOTEMS)) },
            { RUNE_LINGERING, "Rune of Lingering", "+50% duration.", 20925,
              uint8(FIT(SKILL_CONSECRATION) | FIT(SKILL_HAMMER)), RUNE_MODS, UniqueKit(0), 0, 0.0f, 0, { std::make_tuple(uint8(SPELLMOD_DURATION), uint8(SPELLMOD_PCT), int32(50)) }, uint8(FIT(SKILL_STORMS)), uint8(FIT(SKILL_MOONFIRE) | FIT(SKILL_HURRICANE) | FIT(SKILL_REJUVENATION)),
              uint8(FIT(SKILL_CORRUPTION) | FIT(SKILL_IMMOLATE)), uint8(FIT(SKILL_FIRE_TOTEMS) | FIT(SKILL_EARTHEN_TOTEMS)) },
            { RUNE_HASTE, "Rune of Haste", "-25% cooldown, -15% damage.", 1044,
              uint8(FIT(SKILL_JUDGEMENT) | FIT(SKILL_CONSECRATION) | FIT(SKILL_HAMMER) | FIT(SKILL_WRATH) | FIT(SKILL_HOLY_LIGHT)), RUNE_MODS, UniqueKit(0), 0, 0.0f, 0,
              { std::make_tuple(uint8(SPELLMOD_COOLDOWN), uint8(SPELLMOD_PCT), int32(-25)), std::make_tuple(uint8(SPELLMOD_DAMAGE), uint8(SPELLMOD_PCT), int32(-15)),
                std::make_tuple(uint8(SPELLMOD_DOT), uint8(SPELLMOD_PCT), int32(-15)) },
              uint8(FIT(SKILL_FROSTBOLT) | FIT(SKILL_FIRE_BLAST)), 0, 0, uint8(FIT(SKILL_SHOCKS) | FIT(SKILL_SHAMAN_STRIKE)) },
            { RUNE_LEECH, "Rune of Leech", "5% of the skill's damage heals you.", 20166,
              uint8(FIT(SKILL_STRIKE) | FIT(SKILL_JUDGEMENT) | FIT(SKILL_WRATH) | FIT(SKILL_HAMMER)), RUNE_HOOK, UniqueKit(0), 0, 0.0f, 5, {}, uint8(FIT(SKILL_FIREBALL) | FIT(SKILL_FROSTBOLT) | FIT(SKILL_ARCANE_MISSILES) | FIT(SKILL_ARCANE_EXPLOSION) | FIT(SKILL_STORMS) | FIT(SKILL_FIRE_BLAST)), uint8(FIT(SKILL_DRUID_WRATH) | FIT(SKILL_MOONFIRE) | FIT(SKILL_HURRICANE) | FIT(SKILL_BEAR) | FIT(SKILL_CAT)),
              uint8(FIT(SKILL_SHADOW_BOLT) | FIT(SKILL_CORRUPTION) | FIT(SKILL_IMMOLATE) | FIT(SKILL_DRAIN)),
              uint8(FIT(SKILL_LIGHTNING) | FIT(SKILL_SHOCKS) | FIT(SKILL_SHAMAN_STRIKE)) },
            { RUNE_COMMAND, "Rune of Command", "The stun spreads to 1 more enemy within 8 yards.", 20549,
              uint8(FIT(SKILL_HAMMER)), RUNE_KIT, KIT_SPREAD, 1, 8.0f, 100, {} },
            { RUNE_SANCTITY, "Rune of Sanctity", "+15% damage.", 20218,
              uint8(FIT(SKILL_STRIKE) | FIT(SKILL_JUDGEMENT) | FIT(SKILL_CONSECRATION) | FIT(SKILL_WRATH) | FIT(SKILL_HAMMER) | FIT(SKILL_HOLY_LIGHT)), RUNE_MODS, UniqueKit(0), 0, 0.0f, 0,
              { std::make_tuple(uint8(SPELLMOD_DAMAGE), uint8(SPELLMOD_PCT), int32(15)), std::make_tuple(uint8(SPELLMOD_DOT), uint8(SPELLMOD_PCT), int32(15)) },
              uint8(FIT(SKILL_FIREBALL) | FIT(SKILL_FROSTBOLT) | FIT(SKILL_ARCANE_MISSILES) | FIT(SKILL_ARCANE_EXPLOSION) | FIT(SKILL_STORMS) | FIT(SKILL_FIRE_BLAST)), uint8(FIT(SKILL_DRUID_WRATH) | FIT(SKILL_MOONFIRE) | FIT(SKILL_HURRICANE) | FIT(SKILL_BEAR) | FIT(SKILL_CAT) | FIT(SKILL_REJUVENATION)),
              uint8(FIT(SKILL_SHADOW_BOLT) | FIT(SKILL_CORRUPTION) | FIT(SKILL_IMMOLATE) | FIT(SKILL_DRAIN)),
              uint8(FIT(SKILL_LIGHTNING) | FIT(SKILL_SHOCKS) | FIT(SKILL_FIRE_TOTEMS) | FIT(SKILL_SHAMAN_STRIKE) | FIT(SKILL_HEALING_WAVE)) },
            { RUNE_FURY, "Rune of Fury", "Your swing strikes one rank wider (Wide Swing +1, or 55% to every enemy in front).", 845,
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

    // A druid's skills' kit spells, for its kit runes.
    std::vector<uint32> DruidKitSpellsOf(SkillId skill)
    {
        switch (skill)
        {
            case SKILL_DRUID_WRATH: return { 5176, 2912 };  // Wrath, Starfire
            case SKILL_MOONFIRE: return { 8921 };
            case SKILL_BEAR: return { 6807 };               // Maul
            case SKILL_CAT: return { 1082, 5221 };          // Claw, Shred
            default: return {};
        }
    }

    // A shaman's skills' kit spells, for its kit runes.
    std::vector<uint32> ShamanKitSpellsOf(SkillId skill)
    {
        switch (skill)
        {
            case SKILL_LIGHTNING: return { 403, 421 };      // Lightning Bolt, Chain Lightning
            case SKILL_SHOCKS: return { 8042, 8050, 8056 }; // Earth, Flame and Frost Shock
            default: return {};
        }
    }

    // A warlock's skills' kit spells, for its kit runes.
    std::vector<uint32> WarlockKitSpellsOf(SkillId skill)
    {
        switch (skill)
        {
            case SKILL_SHADOW_BOLT: return { 686, 5676 };   // Shadow Bolt, Searing Pain
            case SKILL_CORRUPTION: return { 172, 980 };     // Corruption, Curse of Agony
            case SKILL_IMMOLATE: return { 348 };
            case SKILL_DRAIN: return { 689 };               // Drain Life
            default: return {};
        }
    }

    // The spells a rune's kit row lands on, by skill: the kit spells its tree's own rows use.
    std::vector<uint32> KitSpellsOf(uint8 classId, SkillId skill)
    {
        if (classId == CLASS_MAGE)
            return MageKitSpellsOf(skill);
        if (classId == CLASS_DRUID)
            return DruidKitSpellsOf(skill);
        if (classId == CLASS_WARLOCK)
            return WarlockKitSpellsOf(skill);
        if (classId == CLASS_SHAMAN)
            return ShamanKitSpellsOf(skill);
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
        uint32 const points = (SkillPointsFor(player->GetLevel()) + PerkSkillPoints(player));
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
        // A tree reshaped since the ranks were saved: a node whose parent holds no rank, or the
        // second side of a choice, goes (its points come back), lowest node numbers kept first.
        std::vector<uint16> dropped;
        for (bool changed = true; changed;)
        {
            changed = false;
            for (auto it = s.ranks.begin(); it != s.ranks.end();)
            {
                SkillDef const* owner = nullptr;
                SkillNode const* node = FindNode(classId, it->first, &owner);
                bool orphan = !node || (node->parent && !s.ranks.count(node->parent));
                if (node && !orphan && node->choice)
                    for (SkillNode const& other : owner->nodes)
                        if (other.id < node->id && other.choice == node->choice && s.ranks.count(other.id))
                            orphan = true;
                if (orphan)
                {
                    dropped.push_back(it->first);
                    it = s.ranks.erase(it);
                    changed = true;
                }
                else
                    ++it;
            }
        }
        for (uint16 id : dropped)
            CharacterDatabase.PExecute("DELETE FROM character_arpg_skill_node WHERE guid = %u AND node = %u",
                                       player->GetGUIDLow(), uint32(id));
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
        data << uint8(3);
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
        data << uint16((SkillPointsFor(player->GetLevel()) + PerkSkillPoints(player)) + gearPoints);
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
                data << uint8(node.choice);
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
        if (node->choice)
            for (SkillNode const& other : skill->nodes)
                if (other.id != id && other.choice == node->choice && s.ranks.count(other.id))
                    return refuse("its alternative in the choice is taken");
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
            if (own >= (SkillPointsFor(player->GetLevel()) + PerkSkillPoints(player)))
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
            if (node->convertTag >= 0 && node->convertTag < 7 && skill->id < 16)
                totals.convert[skill->id] = uint8(node->convertTag + 1);
            if (node->morePerRank)
                totals.skillMores.push_back({ uint8(skill->id), int32(node->morePerRank) * rank, node->only, node->name });
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
        return classId == CLASS_PALADIN ? SKILL_STRIKE : classId == CLASS_SHAMAN ? SKILL_SHAMAN_STRIKE : SKILL_NONE;
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

    uint8 SkillNodeRank(Player const* player, uint16 node)
    {
        std::lock_guard<std::mutex> guard(sSkillsLock);
        auto it = sSkills.find(player->GetObjectGuid());
        if (it == sSkills.end() || SlotOf(it->second, uint8(node / 100)) < 0)
            return 0;
        auto rank = it->second.ranks.find(node);
        return rank == it->second.ranks.end() ? 0 : rank->second;
    }

    void AddClassSpellMods(Player const* player, std::vector<SkillSpellMod>& mods)
    {
        // The paladin's Hammers slotted: Hammer of Justice comes back in 15 sec, not a minute, so the
        // Thrown branch's hammers (which fly with it) are a build, not a once-a-minute flourish.
        if (player->getClass() == CLASS_PALADIN)
        {
            PlayerSkills const s = SkillsFor(player);
            if (std::find(std::begin(s.slots), std::end(s.slots), uint8(SKILL_HAMMER)) != std::end(s.slots))
            {
                constexpr int32 STOCK = 60000, SLOTTED = 15000;
                NamedSpells const hoj = NamesMask(CLASS_PALADIN, { "Hammer of Justice" }, "Hammer of Justice");
                if (hoj.spells)
                    mods.push_back({ uint8(SPELLMOD_COOLDOWN), uint8(SPELLMOD_FLAT), SLOTTED - STOCK, hoj.mask, hoj.spells });
            }
            return;
        }
        if (player->getClass() != CLASS_WARLOCK)
            return;
        // Their stock cooldown is an hour: these bring it to INFERNO_COOLDOWN and RITUAL_COOLDOWN.
        constexpr int32 STOCK = 3600000, INFERNO_COOLDOWN = 60000, RITUAL_COOLDOWN = 180000;
        NamedSpells const inferno = NamesMask(CLASS_WARLOCK, { "Inferno" }, "Inferno");
        if (inferno.spells)
            mods.push_back({ uint8(SPELLMOD_COOLDOWN), uint8(SPELLMOD_FLAT), INFERNO_COOLDOWN - STOCK, inferno.mask, inferno.spells });
        NamedSpells const ritual = NamesMask(CLASS_WARLOCK, { "Ritual of Doom" }, "Ritual of Doom");
        if (ritual.spells)
            mods.push_back({ uint8(SPELLMOD_COOLDOWN), uint8(SPELLMOD_FLAT), RITUAL_COOLDOWN - STOCK, ritual.mask, ritual.spells });
    }

    std::vector<std::pair<std::string, std::vector<std::string>>> SkillBuild(Player const* player)
    {
        std::vector<std::pair<std::string, std::vector<std::string>>> out;
        std::lock_guard<std::mutex> guard(sSkillsLock);
        auto it = sSkills.find(player->GetObjectGuid());
        if (it == sSkills.end())
            return out;
        PlayerSkills const& ps = it->second;
        for (uint8 slot = 0; slot < SKILL_SLOTS; ++slot)
        {
            SkillDef const* skill = ps.slots[slot] ? FindSkill(player->getClass(), ps.slots[slot]) : nullptr;
            if (!skill)
                continue;
            std::vector<std::string> nodes;
            for (auto const& [id, rank] : ps.ranks)
                if (id / 100 == ps.slots[slot] && rank)
                    if (SkillNode const* node = FindNode(player->getClass(), id))
                        nodes.push_back(std::string(node->name) + " " + std::to_string(rank));
            out.emplace_back(skill->name, std::move(nodes));
        }
        return out;
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
