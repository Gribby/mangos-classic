/*
 * ARPG passive web: see ArpgTree.h.
 */

#include "Arpg/ArpgTree.h"
#include "Arpg/ArpgActions.h"
#include "Arpg/ArpgAffixes.h"
#include "Arpg/ArpgDungeons.h"
#include "Arpg/ArpgCharacter.h"
#include "Arpg/ArpgCombat.h"
#include "Arpg/ArpgSkills.h"

#include "Chat/Chat.h"
#include "Database/DatabaseEnv.h"
#include "Entities/Player.h"
#include "Globals/ObjectMgr.h"
#include "Groups/Group.h"
#include "Log/Log.h"
#include "Server/DBCStores.h"
#include "Server/Opcodes.h"
#include "Server/WorldPacket.h"
#include "Server/WorldSession.h"
#include "Spells/SpellMgr.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>

namespace
{
    using namespace Arpg;

    // --- The web's data ---

    enum FxKind : uint8
    {
        FX_STAT,          // arg: the attribute (Stats)
        FX_ALL_STATS,
        FX_ARMOUR,        // % armour
        FX_TAG,           // arg: the Tag; % damage (healing, for Heal)
        FX_HEALING,       // % healing done
        FX_MOVE,          // % movement speed
        FX_COOLDOWN,      // % cooldown recovery
        FX_LIFE_ON_KILL,  // health per kill
        FX_MELEE_AREA,    // % melee area
        FX_SPELL_AREA,    // % spell area
        FX_BLOCK,         // % block chance
        FX_MANA_ON_HIT,   // mana per swing or skill hit
    };

    struct Fx
    {
        FxKind kind;
        uint8 arg;
        int32 value;
    };

    struct WebNode
    {
        uint16 id;
        WebNodeKind kind;
        uint8 region;
        int16 x, y;
        std::string name;
        std::string text;
        uint32 icon;              // the spell whose icon it shows; 0 for none (a talent's own)
        std::vector<Fx> fx;
        char const* talent;       // a vanilla talent it teaches, by name, at `rank`
        uint8 rank;
        Keystone key;
    };

    struct Region
    {
        std::string name;
        int16 x, y;
    };

    struct ClassWeb
    {
        std::vector<Region> regions;
        std::vector<WebNode> nodes;          // index = id - 1
        std::vector<std::pair<uint16, uint16>> links;
        std::vector<std::vector<uint16>> next; // index = id - 1: the nodes it joins
    };

    // The spec a node is built from.
    struct Spec
    {
        char const* name;
        char const* text;
        std::vector<Fx> fx;
        uint32 icon = 0;
        char const* talent = nullptr;
        uint8 rank = 0;
        Keystone key = KEY_NONE;
    };

    struct ArmSpec
    {
        Spec step;      // each of its small nodes
        Spec notable;
        Spec end;       // a keystone, or a second notable
        bool endIsKeystone;
    };

    struct RegionSpec
    {
        char const* name;
        float angle;    // degrees, screen y down: -90 is up
        Spec entry;
        ArmSpec arms[3];
        Spec sides[2];  // between arms 0 and 1, and 1 and 2
    };

    struct BridgeSpec
    {
        uint8 fromRegion, fromArm, toRegion, toArm;
        float angle;
        Spec notable;
    };

    // Layout radii, in web units (the client scales them).
    constexpr float R_ENTRY1 = 80, R_ENTRY2 = 135, R_RING = 150, R_ARM0 = 190, R_ARM_STEP = 42, R_SIDE = 316,
                    R_NOTABLE = 405, R_END = 525, R_LABEL = 640, R_BRIDGE_SMALL = 275, R_BRIDGE = 300;
    constexpr int ARM_SMALLS = 5;

    Fx Stat(Stats stat, int32 v) { return { FX_STAT, uint8(stat), v }; }
    Fx TagFx(Tag tag, int32 v) { return { FX_TAG, uint8(tag), v }; }
    Fx Of(FxKind kind, int32 v) { return { kind, 0, v }; }

    ClassWeb BuildPaladin()
    {
        RegionSpec const regions[3] =
        {
            { "Crusader", -150.0f,
              { "Crusader", "+2 to all attributes", { Of(FX_ALL_STATS, 2) } },
              {
                  { { "Might", "+5 Strength", { Stat(STAT_STRENGTH, 5) } },
                    { "Two-Handed Mastery", "+12% Melee damage while you wield a two-handed weapon.", {}, 20111, nullptr, 0, KEY_TWO_HANDED_MASTERY },
                    { "Zealot", "+25% attack speed. While a Seal is on you, it drains 1% of your mana each second.", {}, 20375, nullptr, 0, KEY_ZEALOT },
                    true },
                  { { "Edge", "+3% Melee damage", { TagFx(TAG_MELEE, 3) } },
                    { "Conviction", "+5% chance to critically strike with melee weapons.", {}, 0, "Conviction", 5 },
                    { "Vengeance", "After a critical strike, +15% Physical and Holy damage for 8 sec.", {}, 0, "Vengeance", 5 },
                    false },
                  { { "Fervour", "+4% Seal damage", { TagFx(TAG_SEAL, 4) } },
                    { "Righteous Fervour", "+15% Seal damage. Your Seals and Judgement cost 15% less mana.", { TagFx(TAG_SEAL, 15) }, 0, "Benediction", 5 },
                    { "Crusade", "Each kill in the last 5 sec gives +4% damage, up to 10 kills. Out of combat you move 20% slower.", {}, 20162, nullptr, 0, KEY_CRUSADE },
                    true },
              },
              { { "Reach", "+5% melee area", { Of(FX_MELEE_AREA, 5) } },
                { "Bloodthirst", "+5 life on kill and +6 mana on hit", { Of(FX_LIFE_ON_KILL, 5), Of(FX_MANA_ON_HIT, 6) } } } },
            { "Lightbringer", -30.0f,
              { "Lightbringer", "+2 to all attributes", { Of(FX_ALL_STATS, 2) } },
              {
                  { { "Wisdom", "+5 Intellect", { Stat(STAT_INTELLECT, 5) } },
                    { "Illumination", "Your healing critical strikes refund their mana cost.", {}, 0, "Illumination", 5 },
                    { "Lightforged", "Your heals become Holy bolts at the nearest enemy, for their full amount, and no longer heal you. +30% Holy damage.", {}, 635, nullptr, 0, KEY_LIGHTFORGED },
                    true },
                  { { "Radiant", "+4% Holy damage", { TagFx(TAG_HOLY, 4) } },
                    { "Holy Power", "+5% chance to critically strike with Holy spells.", {}, 0, "Holy Power", 5 },
                    { "Divine Favour", "Every 20 sec, your next Holy spell is a critical strike.", {}, 20216, nullptr, 0, KEY_DIVINE_FAVOUR },
                    false },
                  { { "Devotion", "+4 Spirit and +3 mana on hit", { Stat(STAT_SPIRIT, 4), Of(FX_MANA_ON_HIT, 3) } },
                    { "Healing Light", "+12% Holy Light and Flash of Light healing, and +10% all healing.", { Of(FX_HEALING, 10) }, 0, "Healing Light", 3 },
                    { "Blessed Recovery", "Healing yourself gives +10% cooldown recovery for 4 sec.", {}, 633, nullptr, 0, KEY_BLESSED_RECOVERY },
                    false },
              },
              { { "Expanse", "+5% spell area", { Of(FX_SPELL_AREA, 5) } },
                { "Tempo", "+4% cooldown recovery", { Of(FX_COOLDOWN, 4) } } } },
            { "Templar", 90.0f,
              { "Templar", "+2 to all attributes", { Of(FX_ALL_STATS, 2) } },
              {
                  { { "Fortitude", "+6 Stamina", { Stat(STAT_STAMINA, 6) } },
                    { "Shield Wall", "+10% chance to block. Your blocks heal you for 1% of your health.", { Of(FX_BLOCK, 10) }, 20925, nullptr, 0, KEY_SHIELD_WALL },
                    { "Martyr", "25% of the damage you take strikes every enemy within 10 yards as Holy damage. Healing you take is halved.", {}, 7294, nullptr, 0, KEY_MARTYR },
                    true },
                  { { "Plate", "+4% armour", { Of(FX_ARMOUR, 4) } },
                    { "Redoubt", "After a critical strike against you, +30% chance to block for 10 sec or 5 blocks.", {}, 0, "Redoubt", 5 },
                    { "Reckoning", "When a damaging attack hits you, a 20% chance that your next swing strikes an extra time.", {}, 0, "Reckoning", 2 },
                    false },
                  { { "Zeal", "+4% Area damage", { TagFx(TAG_AREA, 4) } },
                    { "Righteous Fury", "+20% Holy damage while three or more enemies are within 8 yards.", {}, 25780, nullptr, 0, KEY_RIGHTEOUS_FURY },
                    { "Unyielding", "You can't be stunned or slowed. You move 20% slower.", {}, 498, nullptr, 0, KEY_UNYIELDING },
                    true },
              },
              { { "Endurance", "+8 life on kill", { Of(FX_LIFE_ON_KILL, 8) } },
                { "Stride", "+3% movement speed", { Of(FX_MOVE, 3) } } } },
        };
        BridgeSpec const bridges[3] =
        {
            { 0, 2, 1, 0, -90.0f, { "Holy Weapons", "+3% Holy damage per 10 Strength.", {}, 20154, nullptr, 0, KEY_HOLY_WEAPONS } },
            { 1, 2, 2, 0, 30.0f, { "Aegis", "+8 Stamina, +8 Intellect and +8 Spirit.", { Stat(STAT_STAMINA, 8), Stat(STAT_INTELLECT, 8), Stat(STAT_SPIRIT, 8) }, 465 } },
            { 2, 2, 0, 0, 150.0f, { "Shield and Hammer", "+10% Melee damage while you wield a one-handed weapon and a shield.", {}, 20196, nullptr, 0, KEY_SHIELD_AND_HAMMER } },
        };
        Spec const ring = { "Swiftness", "+3% movement speed", { Of(FX_MOVE, 3) } };
        Spec const bridgeSmall = { "Bridge", "+3 to all attributes", { Of(FX_ALL_STATS, 3) } };

        ClassWeb web;
        auto add = [&web](WebNodeKind kind, uint8 region, float r, float deg, Spec const& spec) -> uint16
        {
            float const a = deg * float(M_PI) / 180.0f;
            WebNode node;
            node.id = uint16(web.nodes.size() + 1);
            node.kind = kind;
            node.region = region;
            node.x = int16(std::lround(std::cos(a) * r));
            node.y = int16(std::lround(std::sin(a) * r));
            node.name = spec.name;
            node.text = spec.text;
            node.icon = spec.icon;
            node.fx = spec.fx;
            node.talent = spec.talent;
            node.rank = spec.rank;
            node.key = spec.key;
            web.nodes.push_back(node);
            return node.id;
        };
        auto link = [&web](uint16 a, uint16 b) { web.links.emplace_back(a, b); };

        uint16 const start = add(WEB_START, 3, 0.0f, 0.0f, { "Paladin", "Your starting point. Every path leads out from here.", {}, 20154 });
        uint16 entry[3] = {};
        uint16 tips[3][3] = {};
        for (uint8 ri = 0; ri < 3; ++ri)
        {
            RegionSpec const& reg = regions[ri];
            float const ra = reg.angle * float(M_PI) / 180.0f;
            web.regions.push_back({ reg.name, int16(std::lround(std::cos(ra) * R_LABEL)), int16(std::lround(std::sin(ra) * R_LABEL)) });
            uint16 const e1 = add(WEB_SMALL, ri, R_ENTRY1, reg.angle, reg.entry);
            uint16 const e2 = add(WEB_SMALL, ri, R_ENTRY2, reg.angle, reg.entry);
            link(start, e1);
            link(e1, e2);
            entry[ri] = e2;
            uint16 mids[3] = {};
            for (int i = 0; i < 3; ++i)
            {
                ArmSpec const& arm = reg.arms[i];
                float const off = float(i - 1) * 30.0f;
                uint16 prev = e2;
                for (int k = 0; k < ARM_SMALLS; ++k)
                {
                    float const wobble = (k % 2 ? 4.0f : -4.0f) * (i == 1 ? 1.0f : float(i - 1));
                    uint16 const s = add(WEB_SMALL, ri, R_ARM0 + k * R_ARM_STEP, reg.angle + off + wobble, arm.step);
                    link(prev, s);
                    prev = s;
                    if (k == 1)
                        tips[ri][i] = s;
                    if (k == 3)
                        mids[i] = s;
                }
                uint16 const notable = add(WEB_NOTABLE, ri, R_NOTABLE, reg.angle + off, arm.notable);
                link(prev, notable);
                uint16 const end = add(arm.endIsKeystone ? WEB_KEYSTONE : WEB_NOTABLE, ri, R_END, reg.angle + off, arm.end);
                link(notable, end);
            }
            link(tips[ri][0], tips[ri][1]);
            link(tips[ri][1], tips[ri][2]);
            for (int g = 0; g < 2; ++g)
            {
                uint16 const side = add(WEB_SMALL, ri, R_SIDE, reg.angle + (g ? 15.0f : -15.0f), reg.sides[g]);
                link(mids[g], side);
                link(side, mids[g + 1]);
            }
        }
        // Between the regions, near the start and out at the bridges.
        for (BridgeSpec const& b : bridges)
        {
            uint16 const r = add(WEB_SMALL, 3, R_RING, b.angle, ring);
            link(entry[b.fromRegion], r);
            link(r, entry[b.toRegion]);
        }
        for (BridgeSpec const& b : bridges)
        {
            uint16 const a = add(WEB_SMALL, 3, R_BRIDGE_SMALL, b.angle - 14.0f, bridgeSmall);
            uint16 const mid = add(WEB_NOTABLE, 3, R_BRIDGE, b.angle, b.notable);
            uint16 const c = add(WEB_SMALL, 3, R_BRIDGE_SMALL, b.angle + 14.0f, bridgeSmall);
            link(tips[b.fromRegion][b.fromArm], a);
            link(a, mid);
            link(mid, c);
            link(c, tips[b.toRegion][b.toArm]);
        }
        web.regions.push_back({ "", 0, 0 }); // region 3: the start and the bridges, unlabelled

        web.next.resize(web.nodes.size());
        for (auto const& [a, b] : web.links)
        {
            web.next[a - 1].push_back(b);
            web.next[b - 1].push_back(a);
        }
        return web;
    }

    ClassWeb const* WebOf(uint8 classId)
    {
        static ClassWeb const paladin = BuildPaladin();
        return classId == CLASS_PALADIN ? &paladin : nullptr;
    }

    WebNode const* FindNode(ClassWeb const& web, uint16 id)
    {
        return id >= 1 && id <= web.nodes.size() ? &web.nodes[id - 1] : nullptr;
    }

    // --- Talents ---

    // A class's talent spells by talent name (rank 1's spell name): their rank spells, in order.
    struct ClassTalents
    {
        std::map<std::string, std::vector<uint32>> byName;
        std::set<uint32> all;                         // every rank spell of every talent
        std::vector<std::pair<uint32, uint32>> free;  // talent-granted active spells and their level
    };

    ClassTalents const& TalentsOf(uint8 classId)
    {
        static std::mutex lock;
        static std::map<uint8, ClassTalents> cache;
        std::lock_guard<std::mutex> guard(lock);
        auto it = cache.find(classId);
        if (it != cache.end())
            return it->second;
        ClassTalents& out = cache[classId];
        uint32 const classMask = 1 << (classId - 1);
        for (uint32 i = 0; i < sTalentStore.GetNumRows(); ++i)
        {
            TalentEntry const* talent = sTalentStore.LookupEntry(i);
            TalentTabEntry const* tab = talent ? sTalentTabStore.LookupEntry(talent->TalentTab) : nullptr;
            if (!tab || !(tab->ClassMask & classMask) || !talent->RankID[0])
                continue;
            SpellEntry const* first = sSpellTemplate.LookupEntry<SpellEntry>(talent->RankID[0]);
            if (!first || !first->SpellName[0])
                continue;
            std::vector<uint32>& ranks = out.byName[first->SpellName[0]];
            for (uint32 spell : talent->RankID)
                if (spell)
                {
                    ranks.push_back(spell);
                    out.all.insert(spell);
                }
            if (ranks.size() == 1 && !first->HasAttribute(SPELL_ATTR_PASSIVE))
            {
                // A paladin without area damage can't play an ARPG: Consecration comes at 6.
                bool const early = classId == CLASS_PALADIN && std::strcmp(first->SpellName[0], "Consecration") == 0;
                out.free.emplace_back(first->Id, early ? 6 : 10 + talent->Row * 5);
            }
        }
        return out;
    }

    // The rank spell a notable teaches, or 0.
    uint32 TalentSpell(uint8 classId, WebNode const& node)
    {
        if (!node.talent || !node.rank)
            return 0;
        ClassTalents const& talents = TalentsOf(classId);
        auto it = talents.byName.find(node.talent);
        if (it == talents.byName.end() || it->second.empty())
            return 0;
        return it->second[std::min<size_t>(node.rank, it->second.size()) - 1];
    }

    // The icon a node shows: its own, or its talent's.
    uint32 IconOf(uint8 classId, WebNode const& node)
    {
        if (node.icon)
            return node.icon;
        if (!node.talent)
            return 0;
        ClassTalents const& talents = TalentsOf(classId);
        auto it = talents.byName.find(node.talent);
        return it == talents.byName.end() || it->second.empty() ? 0 : it->second.front();
    }

    // --- Each player's web ---

    struct PlayerWeb
    {
        uint8 classId = 0;
        bool arpg = false;                     // the start is saved: an ARPG character
        std::set<uint16> nodes;                // taken, the start among them once arpg
        std::set<uint32> bosses;
        uint32 level = 0;                      // the level its free spells were last settled at
        std::shared_ptr<WebTotals const> totals;
    };

    std::mutex sWebsLock;
    std::unordered_map<ObjectGuid, PlayerWeb> sWebs;

    PlayerWeb WebFor(Player const* player)
    {
        std::lock_guard<std::mutex> guard(sWebsLock);
        auto it = sWebs.find(player->GetObjectGuid());
        return it == sWebs.end() ? PlayerWeb() : it->second;
    }

    template <class F> void Edit(Player const* player, F f)
    {
        std::lock_guard<std::mutex> guard(sWebsLock);
        f(sWebs[player->GetObjectGuid()]);
    }

    void EnsureTables()
    {
        static bool const created = []()
        {
            CharacterDatabase.DirectExecute(
                "CREATE TABLE IF NOT EXISTS character_arpg_web ("
                "guid INT UNSIGNED NOT NULL, node SMALLINT UNSIGNED NOT NULL, "
                "PRIMARY KEY (guid, node)) ENGINE=InnoDB DEFAULT CHARSET=utf8 COMMENT='ARPG passive web (Arpg/ArpgTree.h)'");
            CharacterDatabase.DirectExecute(
                "CREATE TABLE IF NOT EXISTS character_arpg_bosses ("
                "guid INT UNSIGNED NOT NULL, entry INT UNSIGNED NOT NULL, "
                "PRIMARY KEY (guid, entry)) ENGINE=InnoDB DEFAULT CHARSET=utf8 COMMENT='ARPG final bosses killed (Arpg/ArpgTree.h)'");
            // The first tree's table, which the web replaced.
            CharacterDatabase.DirectExecute("DROP TABLE IF EXISTS character_arpg_tree");
            return true;
        }();
        (void)created;
    }

    WebTotals Sum(ClassWeb const& web, std::set<uint16> const& nodes)
    {
        WebTotals t;
        for (uint16 id : nodes)
        {
            WebNode const* node = FindNode(web, id);
            if (!node)
                continue;
            if (node->key != KEY_NONE)
                t.rank[node->key] = 1;
            for (Fx const& fx : node->fx)
            {
                switch (fx.kind)
                {
                    case FX_STAT:         if (fx.arg < 5) t.stat[fx.arg] += fx.value; break;
                    case FX_ALL_STATS:    for (int32& s : t.stat) s += fx.value; break;
                    case FX_ARMOUR:       t.armourPct += fx.value; break;
                    case FX_TAG:          if (fx.arg < MAX_TAG) t.tag[fx.arg] += fx.value; break;
                    case FX_HEALING:      t.healingPct += fx.value; break;
                    case FX_MOVE:         t.movePct += fx.value; break;
                    case FX_COOLDOWN:     t.cooldownPct += fx.value; break;
                    case FX_LIFE_ON_KILL: t.lifeOnKill += fx.value; break;
                    case FX_MELEE_AREA:   t.meleeAreaPct += fx.value; break;
                    case FX_SPELL_AREA:   t.spellAreaPct += fx.value; break;
                    case FX_BLOCK:        t.blockPct += fx.value; break;
                    case FX_MANA_ON_HIT:  t.manaOnHit += fx.value; break;
                }
            }
        }
        return t;
    }

    // Recompute the player's totals from its nodes, and apply them.
    void Refresh(Player* player)
    {
        ClassWeb const* web = WebOf(player->getClass());
        if (!web)
            return;
        std::set<uint16> const taken = WebFor(player).nodes;
        WebTotals totals = Sum(*web, taken);
        AddSkillTotals(player, totals);
        AddItemTotals(player, totals);
        Edit(player, [&](PlayerWeb& w)
        {
            w.totals = std::make_shared<WebTotals const>(std::move(totals));
        });
        ApplyCharacter(player);
    }

    // Teach or unteach the talent spell a notable gives.
    void TeachNode(Player* player, WebNode const& node, bool learn)
    {
        uint32 const spell = TalentSpell(player->getClass(), node);
        if (!spell)
        {
            if (node.talent)
                sLog.outError("ARPG web: node %u names talent \"%s\", which this class does not have", node.id, node.talent);
            return;
        }
        if (learn && !player->HasSpell(spell))
            player->learnSpell(spell, false);
        else if (!learn && player->HasSpell(spell))
            player->removeSpell(spell, false, false);
    }

    // The talent spells an ARPG character keeps: the web's and the free ones it is old enough for.
    std::set<uint32> KeptTalentSpells(Player const* player, PlayerWeb const& w)
    {
        std::set<uint32> keep;
        ClassWeb const* web = WebOf(player->getClass());
        if (web)
            for (uint16 id : w.nodes)
                if (WebNode const* node = FindNode(*web, id))
                    if (uint32 spell = TalentSpell(player->getClass(), *node))
                        keep.insert(spell);
        for (auto const& [spell, level] : TalentsOf(player->getClass()).free)
            if (player->GetLevel() >= level)
                keep.insert(spell);
        return keep;
    }

    // Every spell a class trainer teaches, by the trainers' own lists, dropping repeats.
    std::vector<TrainerSpell const*> const& ClassTrainerSpells(uint8 classId)
    {
        static std::mutex lock;
        static std::map<uint8, std::vector<TrainerSpell const*>> cache;
        std::lock_guard<std::mutex> guard(lock);
        auto it = cache.find(classId);
        if (it != cache.end())
            return it->second;
        std::vector<TrainerSpell const*>& out = cache[classId];
        std::set<uint32> seen;
        auto take = [&](TrainerSpellData const* data)
        {
            if (!data)
                return;
            for (auto const& [id, spell] : data->spellList)
                if (spell.learnedSpell && !spell.conditionId && seen.insert(spell.learnedSpell).second)
                    out.push_back(&spell);
        };
        for (uint32 i = 1; i < sCreatureStorage.GetMaxEntry(); ++i)
        {
            CreatureInfo const* info = sCreatureStorage.LookupEntry<CreatureInfo>(i);
            if (!info || info->TrainerType != TRAINER_TYPE_CLASS || info->TrainerClass != classId ||
                    !(info->NpcFlags & UNIT_NPC_FLAG_TRAINER))
                continue;
            take(sObjectMgr.GetNpcTrainerSpells(info->Entry));
            if (info->TrainerTemplateId)
                take(sObjectMgr.GetNpcTrainerTemplateSpells(info->TrainerTemplateId));
        }
        std::sort(out.begin(), out.end(), [](TrainerSpell const* a, TrainerSpell const* b) { return a->reqLevel < b->reqLevel; });
        return out;
    }

    // The other spells a trainer spell teaches: TrainerSpell::learnedSpell keeps only one of its
    // learn effects, and the paladin's level 4 lesson teaches two, Seal of Righteousness rank 2
    // and Judgement.
    void TeachAlso(Player* player, TrainerSpell const* spell)
    {
        // A lesson that is itself the ability teaches nothing more.
        SpellEntry const* teach = spell->learnedSpell != spell->spell ? sSpellTemplate.LookupEntry<SpellEntry>(spell->spell) : nullptr;
        if (!teach)
            return;
        for (uint32 i = 0; i < MAX_EFFECT_INDEX; ++i)
        {
            uint32 const other = teach->EffectTriggerSpell[i];
            if (teach->Effect[i] != SPELL_EFFECT_LEARN_SPELL || !other || other == spell->learnedSpell)
                continue;
            // The lesson's own learn effects, as ObjectMgr::LoadTrainers reads them.
            if (teach->EffectImplicitTargetA[i] != TARGET_NONE && teach->EffectImplicitTargetA[i] != TARGET_UNIT_CASTER)
                continue;
            if (!player->HasSpell(other) && player->IsSpellFitByClassAndRace(other))
                player->learnSpell(other, false);
        }
    }

    // Spells without ranks, first pass: the class's trainer spells come free at their level, so
    // each spell is the rank for the character's level (the server's spellbook shows a chain's
    // highest rank only, and the client keeps the bar on it).
    void TeachClassSpells(Player* player)
    {
        std::vector<TrainerSpell const*> const& spells = ClassTrainerSpells(player->getClass());
        // A lesson already learned still gives what it left out (a character from before this).
        for (TrainerSpell const* spell : spells)
            if (player->HasSpell(spell->learnedSpell))
                TeachAlso(player, spell);
        for (int pass = 0; pass < 8; ++pass)
        {
            bool learned = false;
            for (TrainerSpell const* spell : spells)
            {
                uint32 reqLevel = 0;
                if (!player->IsSpellFitByClassAndRace(spell->learnedSpell, &reqLevel))
                    continue;
                if (spell->isProvidedReqLevel)
                    reqLevel = spell->reqLevel;
                if (player->GetTrainerSpellState(spell, reqLevel) != TRAINER_SPELL_GREEN)
                    continue;
                player->learnSpell(spell->learnedSpell, false);
                TeachAlso(player, spell);
                learned = true;
            }
            if (!learned)
                break;
        }
    }

    // Drop the talent spells the character shouldn't have, and give it the free ones.
    void SettleTalentSpells(Player* player)
    {
        PlayerWeb const w = WebFor(player);
        std::set<uint32> const keep = KeptTalentSpells(player, w);
        for (uint32 spell : TalentsOf(player->getClass()).all)
            if (!keep.count(spell) && player->HasSpell(spell))
                player->removeSpell(spell, false, false);
        for (auto const& [spell, level] : TalentsOf(player->getClass()).free)
            if (player->GetLevel() >= level && !player->HasSpell(spell))
                player->learnSpell(spell, false);
        TeachClassSpells(player);
        Edit(player, [&](PlayerWeb& e) { e.level = player->GetLevel(); });
    }

    // Whether every taken node but `without` still joins the start.
    bool StillJoined(ClassWeb const& web, std::set<uint16> const& nodes, uint16 without)
    {
        std::set<uint16> seen = { 1 };
        std::vector<uint16> stack = { 1 };
        while (!stack.empty())
        {
            uint16 const at = stack.back();
            stack.pop_back();
            for (uint16 n : web.next[at - 1])
                if (n != without && nodes.count(n) && seen.insert(n).second)
                    stack.push_back(n);
        }
        for (uint16 n : nodes)
            if (n != without && !seen.count(n))
                return false;
        return true;
    }

    uint32 Spent(std::set<uint16> const& nodes)
    {
        // The start is free.
        return nodes.empty() ? 0 : uint32(nodes.size() - (nodes.count(1) ? 1 : 0));
    }
}

namespace Arpg
{
    std::vector<uint32> const& Bosses()
    {
        static std::vector<uint32> const bosses =
        {
            // Dungeons
            11520, // Taragaman the Hungerer (Ragefire Chasm)
            3654,  // Mutanus the Devourer (Wailing Caverns)
            639,   // Edwin VanCleef (The Deadmines)
            4275,  // Archmage Arugal (Shadowfang Keep)
            4829,  // Aku'mai (Blackfathom Deeps)
            1716,  // Bazil Thredd (The Stockade)
            7800,  // Mekgineer Thermaplugg (Gnomeregan)
            4421,  // Charlga Razorflank (Razorfen Kraul)
            4543,  // Bloodmage Thalnos (Scarlet Monastery Graveyard)
            6487,  // Arcanist Doan (Scarlet Monastery Library)
            3975,  // Herod (Scarlet Monastery Armory)
            3977,  // High Inquisitor Whitemane (Scarlet Monastery Cathedral)
            7358,  // Amnennar the Coldbringer (Razorfen Downs)
            2748,  // Archaedas (Uldaman)
            7267,  // Chief Ukorz Sandscalp (Zul'Farrak)
            12201, // Princess Theradras (Maraudon)
            5709,  // Shade of Eranikus (The Temple of Atal'Hakkar)
            9019,  // Emperor Dagran Thaurissan (Blackrock Depths)
            9568,  // Overlord Wyrmthalak (Lower Blackrock Spire)
            10363, // General Drakkisath (Upper Blackrock Spire)
            11492, // Alzzin the Wildshaper (Dire Maul East)
            11486, // Prince Tortheldrin (Dire Maul West)
            11501, // King Gordok (Dire Maul North)
            1853,  // Darkmaster Gandling (Scholomance)
            10440, // Baron Rivendare (Stratholme)
            10813, // Balnazzar (Stratholme)
            // Raids
            10184, // Onyxia
            11502, // Ragnaros
            11583, // Nefarian
            14834, // Hakkar
            15339, // Ossirian the Unscarred
            15727, // C'Thun
            15990, // Kel'Thuzad
        };
        return bosses;
    }

    uint32 TreePointsFor(Player const* player)
    {
        // One every second level (30 at 60) and one per final boss first killed.
        uint32 const level = player->GetLevel();
        return level / 2 + uint32(WebFor(player).bosses.size());
    }

    void LoadTree(Player* player)
    {
        EnsureTables();
        PlayerWeb w;
        w.classId = player->getClass();
        ClassWeb const* web = WebOf(w.classId);
        if (auto result = CharacterDatabase.PQuery("SELECT node FROM character_arpg_web WHERE guid = %u", player->GetGUIDLow()))
        {
            do
            {
                uint16 const node = uint16(result->Fetch()[0].GetUInt32());
                if (node == 1)
                    w.arpg = true;
                if (web && FindNode(*web, node))
                    w.nodes.insert(node);
            }
            while (result->NextRow());
        }
        if (auto result = CharacterDatabase.PQuery("SELECT entry FROM character_arpg_bosses WHERE guid = %u", player->GetGUIDLow()))
        {
            do
                w.bosses.insert(result->Fetch()[0].GetUInt32());
            while (result->NextRow());
        }
        if (web)
            w.totals = std::make_shared<WebTotals const>(Sum(*web, w.nodes));
        {
            std::lock_guard<std::mutex> guard(sWebsLock);
            sWebs[player->GetObjectGuid()] = std::move(w);
        }
        LoadSkills(player);
    }

    void UnloadTree(Player* player)
    {
        ForgetCharacter(player);
        ForgetActions(player);
        ForgetTiers(player);
        UnloadSkills(player);
        std::lock_guard<std::mutex> guard(sWebsLock);
        sWebs.erase(player->GetObjectGuid());
    }

    bool TreeOwnsSpell(Player const* player, uint32 spell)
    {
        PlayerWeb const w = WebFor(player);
        if (!w.arpg)
            return false;
        return TalentsOf(player->getClass()).all.count(spell) != 0;
    }

    // More nodes than the level gives (the points were rebalanced): the web comes back whole. Not
    // mid-fight; the once-a-second update tries again after it.
    bool SettleWebPoints(Player* player)
    {
        PlayerWeb const w = WebFor(player);
        if (!w.arpg || player->IsInCombat() || Spent(w.nodes) <= TreePointsFor(player))
            return false;
        Respec(player);
        ChatHandler(player).SendSysMessage("|cffffd200Passive points were rebalanced: your web's points are back to spend.|r");
        return true;
    }

    void OnTreeHello(Player* player)
    {
        if (!WebOf(player->getClass()))
        {
            // A class with no web (yet) still answers: an empty web and no skills, so the client
            // drops what another character of the session left in its windows.
            SettleSkillPoints(player);
            SendTree(player);
            SendSkills(player);
            return;
        }
        bool fresh = false;
        Edit(player, [&](PlayerWeb& w)
        {
            fresh = !w.arpg;
            w.arpg = true;
            w.nodes.insert(1);
        });
        if (fresh)
        {
            CharacterDatabase.PExecute("REPLACE INTO character_arpg_web (guid, node) VALUES (%u, 1)", player->GetGUIDLow());
            sLog.outString("ARPG web: %s's vanilla talents give way to the passive web", player->GetName());
        }
        SettleTalentSpells(player);
        ClassWeb const* web = WebOf(player->getClass());
        std::set<uint16> const taken = WebFor(player).nodes;
        for (uint16 id : taken)
            if (WebNode const* node = FindNode(*web, id))
                if (node->talent)
                    TeachNode(player, *node, true);
        SettleWebPoints(player);
        SettleSkillPoints(player);
        Refresh(player);
        SendTree(player);
        SendSkills(player);
    }

    void UpdateTree(Player* player)
    {
        // A refund held back by a fight at login goes through once it ends.
        bool const settled = SettleWebPoints(player) | SettleSkillPoints(player);
        if (settled)
        {
            Refresh(player);
            SendTree(player);
            SendSkills(player);
        }
        PlayerWeb const w = WebFor(player);
        if (!w.arpg || w.level == player->GetLevel())
            return;
        SettleTalentSpells(player);
        Refresh(player);
        SendTree(player);
        SendSkills(player);
    }

    void RefreshTotals(Player* player)
    {
        Refresh(player);
    }

    uint8 KeyRank(Unit const* unit, Keystone key)
    {
        std::shared_ptr<WebTotals const> totals = TotalsOf(unit);
        return totals ? totals->Rank(key) : 0;
    }

    void SpendNode(Player* player, uint16 id)
    {
        ClassWeb const* web = WebOf(player->getClass());
        PlayerWeb const w = WebFor(player);
        auto refuse = [&](char const* why)
        {
            sLog.outDetail("ARPG web: %s cannot take node %u: %s", player->GetName(), uint32(id), why);
            SendTree(player);
        };
        WebNode const* node = web ? FindNode(*web, id) : nullptr;
        if (!node || !w.arpg)
            return refuse("not this class's node");
        if (w.nodes.count(id))
            return refuse("already taken");
        if (Spent(w.nodes) >= TreePointsFor(player))
            return refuse("no points left");
        bool joined = false;
        for (uint16 n : web->next[id - 1])
            joined = joined || w.nodes.count(n);
        if (!joined)
            return refuse("joins nothing taken");
        Edit(player, [&](PlayerWeb& e) { e.nodes.insert(id); });
        CharacterDatabase.PExecute("REPLACE INTO character_arpg_web (guid, node) VALUES (%u, %u)", player->GetGUIDLow(), uint32(id));
        if (node->talent)
            TeachNode(player, *node, true);
        Refresh(player);
        SendTree(player);
    }

    void RefundNode(Player* player, uint16 id)
    {
        ClassWeb const* web = WebOf(player->getClass());
        PlayerWeb const w = WebFor(player);
        auto refuse = [&](char const* why)
        {
            sLog.outDetail("ARPG web: %s cannot give back node %u: %s", player->GetName(), uint32(id), why);
            SendTree(player);
        };
        WebNode const* node = web ? FindNode(*web, id) : nullptr;
        if (!node || id == 1 || !w.nodes.count(id))
            return refuse("not taken");
        if (player->IsInCombat())
            return refuse("in combat");
        if (!StillJoined(*web, w.nodes, id))
            return refuse("other nodes hang from it");
        Edit(player, [&](PlayerWeb& e) { e.nodes.erase(id); });
        CharacterDatabase.PExecute("DELETE FROM character_arpg_web WHERE guid = %u AND node = %u", player->GetGUIDLow(), uint32(id));
        if (node->talent)
            TeachNode(player, *node, false);
        Refresh(player);
        SendTree(player);
    }

    void Respec(Player* player)
    {
        ClassWeb const* web = WebOf(player->getClass());
        if (!web || player->IsInCombat())
        {
            SendTree(player);
            return;
        }
        std::set<uint16> const taken = WebFor(player).nodes;
        for (uint16 id : taken)
            if (WebNode const* node = FindNode(*web, id))
                if (node->talent)
                    TeachNode(player, *node, false);
        Edit(player, [&](PlayerWeb& e) { e.nodes = { 1 }; });
        CharacterDatabase.PExecute("DELETE FROM character_arpg_web WHERE guid = %u AND node <> 1", player->GetGUIDLow());
        Refresh(player);
        SendTree(player);
    }

    void SendTree(Player* player)
    {
        ClassWeb const* web = WebOf(player->getClass());
        PlayerWeb const w = WebFor(player);
        uint8 const classId = player->getClass();

        WorldPacket data(SMSG_ARPG_TREE, 16 + (web ? web->nodes.size() * 80 + web->links.size() * 4 : 0));
        data << uint8(2);
        data << uint16(TreePointsFor(player));
        data << uint16(Spent(w.nodes));
        data << uint8(web ? web->regions.size() : 0);
        if (web)
            for (Region const& region : web->regions)
                data << region.name << int16(region.x) << int16(region.y);
        data << uint16(web ? web->nodes.size() : 0);
        if (web)
            for (WebNode const& node : web->nodes)
            {
                data << uint16(node.id);
                data << uint8(node.kind);
                data << uint8(node.region);
                data << int16(node.x);
                data << int16(node.y);
                data << uint8((node.id == 1 || w.nodes.count(node.id)) ? 1 : 0);
                data << uint32(IconOf(classId, node));
                data << node.name;
                data << node.text;
            }
        data << uint16(web ? web->links.size() : 0);
        if (web)
            for (auto const& [a, b] : web->links)
                data << uint16(a) << uint16(b);
        player->GetSession()->SendPacket(data);
    }

    void CreditBossKill(Player* killer, Unit* victim)
    {
        if (!killer || !victim || victim->GetTypeId() != TYPEID_UNIT)
            return;
        uint32 const entry = victim->GetEntry();
        std::vector<uint32> const& bosses = Bosses();
        if (std::find(bosses.begin(), bosses.end(), entry) == bosses.end())
            return;
        std::vector<Player*> credited;
        if (Group* group = killer->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                if (Player* member = ref->getSource())
                    if (member->IsInMap(victim) && member->IsAtGroupRewardDistance(victim))
                        credited.push_back(member);
        }
        else
            credited.push_back(killer);
        for (Player* player : credited)
        {
            if (!Active(player))
                continue;
            bool added = false;
            Edit(player, [&](PlayerWeb& w) { added = w.arpg && w.bosses.insert(entry).second; });
            if (!added)
                continue;
            CharacterDatabase.PExecute("REPLACE INTO character_arpg_bosses (guid, entry) VALUES (%u, %u)", player->GetGUIDLow(), entry);
            sLog.outString("ARPG web: %s's first kill of %s gives a passive point", player->GetName(), victim->GetName());
            SendTree(player);
        }
    }

    std::shared_ptr<WebTotals const> TotalsOf(Unit const* unit)
    {
        if (!Active(unit))
            return nullptr;
        std::lock_guard<std::mutex> guard(sWebsLock);
        auto it = sWebs.find(unit->GetObjectGuid());
        return it == sWebs.end() || !it->second.arpg ? nullptr : it->second.totals;
    }

    std::vector<UniqueMechanic const*> LearnedModifiers(Player const* player)
    {
        return SkillModifiers(player);
    }

    bool HasKeystone(Unit const* unit, Keystone key)
    {
        std::shared_ptr<WebTotals const> totals = TotalsOf(unit);
        return totals && totals->Has(key);
    }
}
