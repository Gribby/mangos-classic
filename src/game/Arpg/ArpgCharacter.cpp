/*
 * ARPG character: see ArpgCharacter.h.
 */

#include "Arpg/ArpgCharacter.h"
#include "Arpg/ArpgDamage.h"
#include "Arpg/ArpgCompare.h"
#include "Arpg/ArpgDungeons.h"
#include "Arpg/ArpgEvents.h"
#include "Arpg/ArpgMage.h"
#include "Arpg/ArpgProcs.h"
#include "Arpg/ArpgThreats.h"
#include "Arpg/ArpgCombat.h"
#include "Arpg/ArpgPacks.h"
#include "Arpg/ArpgSkills.h"
#include "Arpg/ArpgTags.h"
#include "Arpg/ArpgTree.h"
#include "Arpg/ArpgUniques.h"

#include "Entities/Creature.h"
#include "Entities/DynamicObject.h"
#include "Entities/Item.h"
#include "Entities/Player.h"
#include "Spells/Spell.h"
#include "Spells/SpellAuras.h"
#include "Spells/SpellMgr.h"
#include "Server/Opcodes.h"
#include "Server/WorldPacket.h"
#include "Server/WorldSession.h"
#include "Util/Timer.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <set>
#include <deque>
#include <map>
#include <mutex>
#include <unordered_map>

namespace
{
    // The mage's web keystones' more and less damage (Arpg/ArpgMage.h).
    constexpr int32 PYROMANCER_PCT = 40, PYROMANCER_LESS_PCT = 30, PERMAFROST_LESS_PCT = 25, SHATTER_PCT = 40,
                    SHATTER_LESS_PCT = 15, FLUX_LESS_PCT = 10, BRIMMING_MAX_PCT = 25, BRIMMING_LESS_PCT = 15;
    // Rune of Leech: this share of the damage heals.
    constexpr uint32 RUNE_LEECH_PCT = 5;
    // Light of the Crusader: this share of a Seal hit's damage heals.
    constexpr uint32 CRUSADER_LIGHT_PCT = 10;
    // ARPG pacing: mana comes back from fighting, not from drinking, but slowly: a fight is paid
    // for, and a drain (Zealot) or a costly spell shows on the orb. A swing that hits returns
    // MANA_ON_SWING_PERMILLE of the maximum, a skill hit MANA_ON_SPELL_PERMILLE (at most every
    // MANA_SPELL_EVERY ms), a kill MANA_ON_KILL_PERMILLE, a floor every character has; each swing
    // and each such skill hit adds the "mana on hit" of the web and the items. Out of a fight for
    // REST_DELAY_MS (no hit dealt or taken, out of combat), health comes back REST_HEALTH_PCT a
    // second and mana REST_MANA_PCT, slow enough that a run between packs is not a refill. The run
    // is BASE_SPEED_PCT faster than vanilla's.
    constexpr uint32 MANA_ON_SWING_PERMILLE = 5, MANA_ON_SPELL_PERMILLE = 5, MANA_ON_KILL_PERMILLE = 20;
    constexpr uint32 MANA_SPELL_EVERY = 1000;
    constexpr uint32 REST_DELAY_MS = 5000;
    constexpr float REST_HEALTH_PCT = 5.0f, REST_MANA_PCT = 2.0f;
    constexpr int32 BASE_SPEED_PCT = 10;
    // The global cooldown's cut (1.5 sec to 0.5, a rogue's 1 sec to a third) and the cast time's,
    // for ARPG players. A share, not a flat cut, so no GCD reaches zero.
    constexpr int32 ARPG_GCD_CUT_PCT = -67, ARPG_CAST_TIME_PCT = -50;
    // The longest gap one rest tick covers: after a loading screen, no lump of healing.
    constexpr uint32 REST_GAP_MAX_MS = 1000;

    // A Seal's damage: by name, as the hits a Seal deals (Seal of Righteousness's and Seal of
    // Command's procs) carry none of the Seal auras' family flags.
    bool IsSealHit(SpellEntry const* spellInfo)
    {
        return spellInfo && spellInfo->SpellName[0] && std::strncmp(spellInfo->SpellName[0], "Seal of ", 8) == 0;
    }

    // Give `unit` `permille` thousandths of its maximum mana and `flat` more.
    void GiveManaPermille(Unit* unit, uint32 permille, int32 flat = 0)
    {
        if (unit->GetPowerType() != POWER_MANA || !unit->IsAlive())
            return;
        uint32 const max = unit->GetMaxPower(POWER_MANA);
        int32 const gain = int32(std::max<uint32>(1, max * permille / 1000)) + std::max<int32>(0, flat);
        if (max && unit->GetPower(POWER_MANA) < max)
            unit->ModifyPower(POWER_MANA, gain);
    }

    // Executioner: below this share of health; Wrathful: this many enemies this close, this often.
    constexpr float EXECUTE_BELOW = 0.2f;
    constexpr float WRATHFUL_RANGE = 10.0f;
    constexpr size_t WRATHFUL_COUNT = 4;
    constexpr uint32 WRATHFUL_EVERY = 12000;
    using namespace Arpg;

    // The attributes' ARPG effects: points of the attribute per 1%, and the cap in percent.
    constexpr int32 STR_PER_AREA = 10, AREA_CAP = 40;
    constexpr int32 AGI_PER_SPEED = 20, SPEED_CAP = 30;    // the cap includes BASE_SPEED_PCT
    constexpr int32 STA_PER_LIFE = 4;
    constexpr int32 INT_PER_AREA = 10;
    constexpr int32 SPI_PER_COOLDOWN = 10, COOLDOWN_CAP = 30;

    // The item affixes' totals are capped where stacking them would break the game.
    constexpr int32 ATTACK_SPEED_CAP = 50, CAST_SPEED_CAP = 40, COST_CAP = 50, CRIT_CAP = 30;
    constexpr int32 LEECH_CAP = 100;           // thousandths of the damage dealt
    constexpr int32 DAMAGE_TAKEN_CAP = 40;
    constexpr float THORNS_REACH = 6.0f;
    constexpr uint32 THORNS_VISUAL = 467;      // Thorns, for its look and its log line
    // The sheet's record of a skill's hits: this long, at most this many.
    constexpr uint32 HITS_WINDOW = 60000;
    constexpr size_t HITS_MAX = 300;


    // The keystones' numbers.
    constexpr uint32 REFRESH_MS = 1000;
    // The character sheet is rebuilt at most this often (ms), and sent when it changed.
    constexpr uint32 SHEET_EVERY = 2000;
    constexpr float ZEALOT_HASTE = 25.0f;
    constexpr uint32 CRUSADE_WINDOW = 5000, CRUSADE_MAX = 10;
    constexpr int32 CRUSADE_PER_KILL = 4;
    constexpr float CRUSADE_SLOW = 0.8f, UNYIELDING_SLOW = 0.8f;
    constexpr float MARTYR_RANGE = 10.0f;
    constexpr uint32 MARTYR_SHARE_PCT = 25;
    constexpr uint32 RETRIBUTION_AURA = 7294;
    constexpr float LIGHTFORGED_RANGE = 20.0f;
    constexpr uint32 FAVOUR_COOLDOWN = 20000;
    constexpr uint32 RECOVERY_MS = 4000;
    constexpr int32 RECOVERY_PCT = 10;
    constexpr float FURY_RANGE = 8.0f;
    // Holy Light's tree: Searing Radiance's reach and share per rank, Judging Light's reach, share
    // per rank and cooldown, Avenging Light's more damage per rank and time, Holy Nova's share and reach.
    constexpr float LIGHT_STRIKE_RANGE = 20.0f;
    constexpr uint32 LIGHT_STRIKE_PCT = 15;
    constexpr float JUDGING_LIGHT_RANGE = 8.0f;
    constexpr uint32 JUDGING_LIGHT_PCT = 25, JUDGING_LIGHT_EVERY = 3000;
    constexpr int32 AVENGING_LIGHT_PCT = 5;
    constexpr uint32 AVENGING_LIGHT_MS = 5000;
    constexpr uint32 HOLY_NOVA_PCT = 50;
    constexpr float HOLY_NOVA_RANGE = 8.0f;
    constexpr uint32 HOLY_NOVA_LOOK = 2812;     // Holy Wrath's hit
    constexpr size_t FURY_COUNT = 3;

    // The skill trees' numbers.
    constexpr float SWING_ARC = 2.0f * float(M_PI) / 3.0f;
    // Melee area's reach: each +100% of melee area would add this many yards to the swing's added
    // hits (the cap, +40%, gives 2).
    constexpr float MELEE_AREA_YARDS = 5.0f;
    constexpr uint32 WIDE_SWING_PCT[3] = { 20, 35, 50 };
    constexpr uint32 WHIRL_EVERY = 4, WHIRL_PCT = 75;
    constexpr uint32 MOMENTUM_WINDOW = 3000, MOMENTUM_MAX = 5;
    constexpr uint32 PACE_MS = 3000;
    constexpr float PACE_SPEED = 1.3f;
    constexpr float SHOCKWAVE_REACH = 10.0f;
    constexpr uint32 SHOCKWAVE_PCT = 40;
    constexpr uint32 DAZED = 1604, CLEAVE = 845, WHIRLWIND = 1680, WAR_STOMP = 20549;
    // A paladin's extra swings look holy, not like a warrior's Cleave: Smite's hit.
    constexpr uint32 HOLY_LOOK = 585;

    SpellEntry const* SwingLook(Unit const* attacker)
    {
        return attacker->getClass() == CLASS_PALADIN ? sSpellTemplate.LookupEntry<SpellEntry>(HOLY_LOOK) : nullptr;
    }
    constexpr uint32 CONSECRATION = 26573, JUDGEMENT = 20271;
    constexpr float SANCTIFIED_RANGE = 8.0f;
    constexpr uint32 SANCTIFIED_PCT = 50;
    constexpr float RICOCHET_RANGE = 10.0f;
    constexpr float BLESSED_RANGE = 15.0f;
    constexpr size_t BLESSED_COUNT = 3;
    constexpr float WALK_FOLLOW = 2.0f;
    constexpr uint32 WALK_MS = 250;

    // What has been applied to a player, and the keystones' running state.
    struct Runtime
    {
        int32 stat[5] = {};
        int32 armourPct = 0;
        float haste = 0.0f;          // the attack speed percent applied (Zealot, Quickened, Zeal)
        bool stunImmune = false;     // Unyielding, or Steadfast inside Consecration
        bool snareImmune = false;    // Unyielding
        std::vector<std::pair<SkillSpellMod, SpellModifier*>> skillMods;
        uint32 swings = 0;           // Whirling Strikes: swings that hit
        uint32 momentum = 0;         // Momentum: hits in a row
        uint32 lastHit = 0;
        uint32 paceUntil = 0;        // Crusader's Pace: the time its speed ends
        uint32 lastWalk = 0;         // Walking Consecration: the last time it followed
        SpellModifier* cooldownMod = nullptr;
        int32 cooldownPct = 0;
        float speed = 1.0f;
        uint32 lastRefresh = 0;
        std::deque<uint32> kills;    // Crusade: kill times
        uint32 martyrPending = 0;
        uint32 favourReady = 0;      // Divine Favour: the time it is ready again
        uint32 recoveryUntil = 0;    // Blessed Recovery: the time its bonus ends
        uint32 lastWrath = 0;        // Wrathful: the last time Holy Wrath fired on its own
        uint32 avengeUntil = 0;      // Avenging Light: the time its more damage ends
        uint32 lastJudgingLight = 0; // Judging Light: the last time a heal judged
        uint32 lastManaHit = 0;      // the last skill hit that returned mana
        uint32 combatSeen = 0;       // the last update the player was in combat
        uint32 lastRest = 0;         // the last rest tick
        float restCarry = 0.0f;      // the rest's fraction of a percent not yet given
        // The item affixes' stat modifiers applied (ApplyCharacter).
        int32 healthPct = 0, manaPct = 0, attackPower = 0, meleeCrit = 0;
        int32 resist[5] = {};
        std::vector<ObjectGuid> thornsDue;   // attackers the thorns strike at the next update
        // Each skill's hits of the last HITS_WINDOW ms (time, damage), for the sheet's readout.
        std::map<uint8, std::deque<std::pair<uint32, uint32>>> hits;
        uint32 sheetAt = 0;          // the last time the character sheet was built
        size_t sheetHash = 0;        // and what it said, so only a change is sent
    };

    std::mutex sRuntimeLock;
    std::unordered_map<ObjectGuid, Runtime> sRuntime;

    // The player's runtime state; elements of an unordered_map keep their address.
    Runtime& R(Unit const* unit)
    {
        std::lock_guard<std::mutex> guard(sRuntimeLock);
        return sRuntime[unit->GetObjectGuid()];
    }

    void RecordHit(Runtime& r, uint8 skill, uint32 damage, uint32 now)
    {
        std::deque<std::pair<uint32, uint32>>& hits = r.hits[skill];
        hits.emplace_back(now, damage);
        while (hits.size() > HITS_MAX || (!hits.empty() && WorldTimer::getMSTimeDiff(hits.front().first, now) > HITS_WINDOW))
            hits.pop_front();
    }

    WebTotals const& NoTotals()
    {
        static WebTotals const none;
        return none;
    }

    int32 TagSum(WebTotals const& t, uint32 tags)
    {
        int32 sum = 0;
        for (uint8 tag = 0; tag < MAX_TAG; ++tag)
            if (tags & TagBit(Tag(tag)))
                sum += t.tag[tag];
        return sum;
    }

    Item const* MainHand(Player const* player)
    {
        return player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    }

    bool TwoHanded(Player const* player)
    {
        Item const* item = MainHand(player);
        return item && item->GetProto()->InventoryType == INVTYPE_2HWEAPON;
    }

    bool OneHandAndShield(Player const* player)
    {
        Item const* main = MainHand(player);
        Item const* off = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
        return main && off && main->GetProto()->InventoryType != INVTYPE_2HWEAPON &&
               off->GetProto()->InventoryType == INVTYPE_SHIELD;
    }

    // printf into a std::string, for the character sheet's lines.
    std::string Format(char const* format, ...) ATTR_PRINTF(1, 2);
    std::string Format(char const* format, ...)
    {
        char buf[256];
        va_list args;
        va_start(args, format);
        std::vsnprintf(buf, sizeof(buf), format, args);
        va_end(args);
        return buf;
    }

    // The sheet's colours: what vanilla gives, what the ARPG adds, a cap reached, a quiet note.
    char const* const SHEET_VANILLA = "|cffffffff";
    char const* const SHEET_ARPG = "|cffffd200";
    char const* const SHEET_CAPPED = "|cffff8040";
    char const* const SHEET_NOTE = "|cff9d9d9d";

    // "+12%" in the ARPG colour, or in the capped colour with "(cap)" once at `cap`.
    std::string CappedPct(int32 pct, int32 cap)
    {
        if (pct >= cap)
            return Format("%s+%d%% (the cap)|r", SHEET_CAPPED, cap);
        return Format("+%d%%", pct);
    }

    std::string Joined(std::vector<std::string> const& lines)
    {
        std::string out;
        for (std::string const& line : lines)
        {
            if (!out.empty())
                out += '\n';
            out += line;
        }
        return out;
    }

    // What each attribute does, vanilla and ARPG, at the player's current values.
    std::string AttributeText(Player const* player, WebTotals const& t, uint8 stat)
    {
        int32 const value = int32(player->GetStat(Stats(stat)));
        uint8 const cls = player->getClass();
        bool const strong = cls == CLASS_WARRIOR || cls == CLASS_PALADIN || cls == CLASS_SHAMAN || cls == CLASS_DRUID;
        bool const shield = cls == CLASS_WARRIOR || cls == CLASS_PALADIN || cls == CLASS_SHAMAN;
        bool const nimble = cls == CLASS_ROGUE || cls == CLASS_HUNTER;
        bool const mana = player->GetPowerType() == POWER_MANA;
        std::vector<std::string> lines;
        switch (stat)
        {
            case STAT_STRENGTH:
            {
                lines.push_back(Format("%s+%d attack power.|r", SHEET_VANILLA, strong ? value * 2 : value));
                if (shield)
                    lines.push_back(Format("%sMore damage blocked by your shield.|r", SHEET_VANILLA));
                int32 const own = value / STR_PER_AREA;
                lines.push_back(Format("%sMelee area %s: bigger swings, cleaves, arcs and shockwaves.|r",
                                       SHEET_ARPG, CappedPct(std::min(own + t.meleeAreaPct, AREA_CAP), AREA_CAP).c_str()));
                lines.push_back(Format("%s1%% per %d Strength (%d%% here), %d%% from gear and the web; at most %d%%.|r",
                                       SHEET_NOTE, STR_PER_AREA, own, t.meleeAreaPct, AREA_CAP));
                lines.push_back(Format("%s+%d%% increased Melee and Physical damage (1%% per %d Strength).|r",
                                       SHEET_ARPG, value / STR_PER_DAMAGE, STR_PER_DAMAGE));
                if (t.Has(KEY_HOLY_WEAPONS))
                    lines.push_back(Format("%s+%d%% Holy damage from Holy Weapons (3%% per 10 Strength).|r", SHEET_ARPG, 3 * (value / 10)));
                break;
            }
            case STAT_AGILITY:
            {
                lines.push_back(Format("%s+%d armour, and more critical strike and dodge chance.|r", SHEET_VANILLA, value * 2));
                if (nimble)
                    lines.push_back(Format("%s+%d attack power.|r", SHEET_VANILLA, value));
                int32 const own = value / AGI_PER_SPEED;
                lines.push_back(Format("%sMovement speed %s.|r", SHEET_ARPG,
                                       CappedPct(std::min(BASE_SPEED_PCT + own + t.movePct, SPEED_CAP), SPEED_CAP).c_str()));
                lines.push_back(Format("%sEveryone runs %d%% faster; 1%% more per %d Agility (%d%% here), %d%% from gear and the web; at most %d%%.|r",
                                       SHEET_NOTE, BASE_SPEED_PCT, AGI_PER_SPEED, own, t.movePct, SPEED_CAP));
                lines.push_back(Format("%s+%d%% critical strike multiplier (1%% per %d Agility).|r",
                                       SHEET_ARPG, value / AGI_PER_CRIT, AGI_PER_CRIT));
                break;
            }
            case STAT_STAMINA:
            {
                int32 const health = std::min(value, 20) + std::max(value - 20, 0) * 10;
                lines.push_back(Format("%s+%d maximum health.|r", SHEET_VANILLA, health));
                int32 const own = value / STA_PER_LIFE;
                lines.push_back(Format("%s+%d life on kill.|r", SHEET_ARPG, own + t.lifeOnKill));
                lines.push_back(Format("%s1 per %d Stamina (%d here), %d from gear and the web.|r",
                                       SHEET_NOTE, STA_PER_LIFE, own, t.lifeOnKill));
                break;
            }
            case STAT_INTELLECT:
            {
                if (mana)
                    lines.push_back(Format("%s+%d maximum mana, and more spell critical strike chance.|r", SHEET_VANILLA,
                                           std::min(value, 20) + std::max(value - 20, 0) * 15));
                else
                    lines.push_back(Format("%sFaster weapon skill gains.|r", SHEET_VANILLA));
                int32 const own = value / INT_PER_AREA;
                lines.push_back(Format("%sSpell area %s: bigger Consecration, Holy Wrath, Blizzard and the like.|r",
                                       SHEET_ARPG, CappedPct(std::min(own + t.spellAreaPct, AREA_CAP), AREA_CAP).c_str()));
                lines.push_back(Format("%s1%% per %d Intellect (%d%% here), %d%% from gear and the web; at most %d%%.|r",
                                       SHEET_NOTE, INT_PER_AREA, own, t.spellAreaPct, AREA_CAP));
                lines.push_back(Format("%s+%d%% increased Spell damage (1%% per %d Intellect).|r",
                                       SHEET_ARPG, value / INT_PER_DAMAGE, INT_PER_DAMAGE));
                break;
            }
            case STAT_SPIRIT:
            {
                lines.push_back(Format("%sHealth and mana regeneration while fighting.|r", SHEET_VANILLA));
                int32 const own = value / SPI_PER_COOLDOWN;
                lines.push_back(Format("%sCooldown recovery %s: cooldowns tick down faster.|r", SHEET_ARPG,
                                       CappedPct(std::min(own + t.cooldownPct, COOLDOWN_CAP), COOLDOWN_CAP).c_str()));
                lines.push_back(Format("%s1%% per %d Spirit (%d%% here), %d%% from gear and the web; at most %d%%.|r",
                                       SHEET_NOTE, SPI_PER_COOLDOWN, own, t.cooldownPct, COOLDOWN_CAP));
                lines.push_back(Format("%s+%d%% increased damage over time and damage with ailments (1%% per %d Spirit).|r",
                                       SHEET_ARPG, value / SPI_PER_DOT, SPI_PER_DOT));
                lines.push_back(Format("%sOut of a fight for %u sec, health comes back %d%% a second and mana %d%%, whatever your Spirit.|r",
                                       SHEET_NOTE, REST_DELAY_MS / 1000, int32(REST_HEALTH_PCT), int32(REST_MANA_PCT)));
                break;
            }
            default:
                break;
        }
        return Joined(lines);
    }

    // The character's ARPG numbers, for the sheet's summary.
    std::string SummaryText(Player const* player, WebTotals const& t)
    {
        std::vector<std::string> lines;
        lines.push_back(Format("%sMovement speed %s|r", SHEET_ARPG,
                               CappedPct(std::min(BASE_SPEED_PCT + int32(player->GetStat(STAT_AGILITY)) / AGI_PER_SPEED + t.movePct, SPEED_CAP), SPEED_CAP).c_str()));
        lines.push_back(Format("%sCooldown recovery %s|r", SHEET_ARPG,
                               CappedPct(std::min(int32(player->GetStat(STAT_SPIRIT)) / SPI_PER_COOLDOWN + t.cooldownPct, COOLDOWN_CAP), COOLDOWN_CAP).c_str()));
        lines.push_back(Format("%sMelee area %s, spell area %s|r", SHEET_ARPG,
                               CappedPct(std::min(int32(player->GetStat(STAT_STRENGTH)) / STR_PER_AREA + t.meleeAreaPct, AREA_CAP), AREA_CAP).c_str(),
                               CappedPct(std::min(int32(player->GetStat(STAT_INTELLECT)) / INT_PER_AREA + t.spellAreaPct, AREA_CAP), AREA_CAP).c_str()));
        lines.push_back(Format("%sLife on kill +%d, mana on hit +%d|r", SHEET_ARPG,
                               int32(player->GetStat(STAT_STAMINA)) / STA_PER_LIFE + t.lifeOnKill, t.manaOnHit));
        if (t.healingPct || t.tag[TAG_HEAL])
            lines.push_back(Format("%sHealing +%d%%|r", SHEET_ARPG, t.healingPct + t.tag[TAG_HEAL]));
        if (t.armourPct)
            lines.push_back(Format("%sArmour +%d%%|r", SHEET_ARPG, t.armourPct));
        if (t.blockPct)
            lines.push_back(Format("%sBlock chance +%d%%|r", SHEET_ARPG, t.blockPct));
        // The item affixes' other stats, those the character has.
        auto stat = [&](int32 value, char const* format)
        {
            if (value)
                lines.push_back(Format(format, SHEET_ARPG, value));
        };
        stat(std::min(t.attackSpeedPct, ATTACK_SPEED_CAP), "%sAttack speed +%d%%|r");
        stat(std::min(t.castSpeedPct, CAST_SPEED_CAP), "%sCast speed +%d%%|r");
        stat(std::min(t.meleeCritPct, CRIT_CAP), "%sCritical strike chance +%d%%|r");
        stat(std::min(t.spellCritPct, CRIT_CAP), "%sSpell critical strike chance +%d%%|r");
        stat(t.spellCritDamagePct, "%sSpell critical strike damage +%d%%|r");
        stat(std::min(t.costPct, COST_CAP), "%sMana costs -%d%%|r");
        stat(t.attackPower, "%sAttack power +%d|r");
        stat(t.spellPower, "%sSpell damage +%d|r");
        stat(t.healthPct, "%sMaximum health +%d%%|r");
        stat(t.manaPct, "%sMaximum mana +%d%%|r");
        stat(t.healthRegen, "%sHealth +%d a second|r");
        stat(t.manaRegen, "%sMana +%d a second|r");
        if (t.lifeLeech)
            lines.push_back(Format("%sLife leech %d.%d%% of damage|r", SHEET_ARPG, std::min(t.lifeLeech, LEECH_CAP) / 10, std::min(t.lifeLeech, LEECH_CAP) % 10));
        if (t.manaLeech)
            lines.push_back(Format("%sMana leech %d.%d%% of damage|r", SHEET_ARPG, std::min(t.manaLeech, LEECH_CAP) / 10, std::min(t.manaLeech, LEECH_CAP) % 10));
        stat(std::min(t.damageTakenPct, DAMAGE_TAKEN_CAP), "%sDamage taken -%d%%|r");
        stat(t.thorns, "%sThorns %d|r");
        stat(t.elitePct, "%sDamage to elites and bosses +%d%%|r");
        stat(t.globeHealPct, "%sGlobe and flask healing +%d%%|r");
        stat(t.goldFindPct, "%sGold found +%d%%|r");
        stat(t.xpPct, "%sExperience +%d%%|r");
        {
            std::string res;
            static char const* const RES[5] = { "Fire", "Nature", "Frost", "Shadow", "Arcane" };
            for (int i = 0; i < 5; ++i)
                if (t.resist[i])
                    res += Format("%s%s +%d", res.empty() ? "" : ", ", RES[i], t.resist[i]);
            if (!res.empty())
                lines.push_back(Format("%sResistances: %s|r", SHEET_ARPG, res.c_str()));
        }
        std::string damage;
        for (uint8 tag = 0; tag < MAX_TAG; ++tag)
            if (tag != TAG_HEAL && t.tag[tag])
                damage += Format("%s%s +%d%%", damage.empty() ? "" : ", ", TagName(Tag(tag)), t.tag[tag]);
        lines.push_back(Format("%sDamage: %s|r", SHEET_ARPG, damage.empty() ? "no bonuses yet" : damage.c_str()));
        lines.push_back(Format("%sIncreased bonuses add up: a hit takes every tag it carries (Seal of Righteousness: Holy, Melee and Seal), Strength, Intellect and Spirit. More bonuses (notables, keystones, skill nodes) each multiply.|r", SHEET_NOTE));
        lines.push_back(Format("%sYour hits on creatures are x%.1f at level %u (creatures' health grows to match).|r", SHEET_NOTE,
                               PowerScale(player), uint32(player->GetLevel())));
        if (int32 const crit = t.critDamagePct)
            lines.push_back(Format("%sCritical strike multiplier +%d%%|r", SHEET_ARPG, crit));
        if (int32 const ail = t.ailmentPct)
            lines.push_back(Format("%sDamage with ailments +%d%%|r", SHEET_ARPG, ail));
        lines.push_back(Format("%sEvery class spell: a 0.5 sec global cooldown and half the cast time.|r", SHEET_NOTE));
        // The worn uniques.
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (Item const* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                for (UniqueMechanic const& row : Uniques())
                    if (row.item == item->GetEntry())
                        lines.push_back(Format("|cffe6cc80%s|r: %s", item->GetProto()->Name1, row.text));
        return Joined(lines);
    }

    // The item affixes' leech: a share of each hit's damage back as health and mana, quietly
    // (a stat, not a proc).
    void Leech(Player* player, WebTotals const& t, uint32 damage)
    {
        if (!damage || !player->IsAlive())
            return;
        if (int32 const life = std::clamp(t.lifeLeech, 0, LEECH_CAP))
            if (uint32 const heal = damage * uint32(life) / 1000)
                player->ModifyHealth(int32(heal));
        if (int32 const mana = std::clamp(t.manaLeech, 0, LEECH_CAP))
            if (uint32 const gain = damage * uint32(mana) / 1000)
                if (player->GetPowerType() == POWER_MANA)
                    player->ModifyPower(POWER_MANA, int32(gain));
    }

    uint32 CrusadeStacks(Runtime const& r, uint32 now)
    {
        uint32 n = 0;
        for (uint32 t : r.kills)
            if (WorldTimer::getMSTimeDiff(t, now) <= CRUSADE_WINDOW)
                ++n;
        return std::min(n, CRUSADE_MAX);
    }

    int32 CooldownPct(Player const* player, WebTotals const& t, Runtime const& r, uint32 now)
    {
        int32 pct = int32(player->GetStat(STAT_SPIRIT)) / SPI_PER_COOLDOWN + t.cooldownPct;
        if (now < r.recoveryUntil)
            pct += RECOVERY_PCT;
        return std::clamp(pct, 0, COOLDOWN_CAP);
    }

    // A spell of the player's own class family, to own the cooldown modifier (the client and
    // Player::ApplySpellMod both match a modifier to spells of its spell's family).
    uint32 FamilySpell(Player const* player)
    {
        for (auto const& [id, spell] : player->GetSpellMap())
        {
            if (spell.state == PLAYERSPELL_REMOVED)
                continue;
            if (SpellEntry const* entry = sSpellTemplate.LookupEntry<SpellEntry>(id))
                if (entry->SpellFamilyName == uint32(player->GetSpellClass()) && entry->SpellFamilyName)
                    return id;
        }
        return 0;
    }

    void SetCooldownMod(Player* player, Runtime& r, int32 pct)
    {
        if (pct == r.cooldownPct && (pct == 0) == (r.cooldownMod == nullptr))
            return;
        if (r.cooldownMod)
        {
            player->AddSpellMod(r.cooldownMod, false); // deletes it
            r.cooldownMod = nullptr;
        }
        r.cooldownPct = pct;
        if (!pct)
            return;
        uint32 const owner = FamilySpell(player);
        if (!owner)
            return;
        r.cooldownMod = new SpellModifier(SPELLMOD_COOLDOWN, SPELLMOD_PCT, -pct, owner, ~uint64(0));
        player->AddSpellMod(r.cooldownMod, true);
    }

    bool SealOn(Player const* player)
    {
        for (auto const& [id, holder] : player->GetSpellAuraHolderMap())
            if (holder->GetCasterGuid() == player->GetObjectGuid() && (TagsOf(holder->GetSpellProto()) & TagBit(TAG_SEAL)))
                return true;
        return false;
    }

    bool AuraNamed(Unit const* unit, char const* prefix, ObjectGuid caster)
    {
        size_t const n = std::strlen(prefix);
        for (auto const& [id, holder] : unit->GetSpellAuraHolderMap())
        {
            char const* name = holder->GetSpellProto()->SpellName[0];
            if (name && std::strncmp(name, prefix, n) == 0 && (!caster || holder->GetCasterGuid() == caster))
                return true;
        }
        return false;
    }

    // Whether `victim` stands in `by`'s Consecration (it carries the ground's aura).
    bool Consecrated(Unit const* victim, Unit const* by)
    {
        return victim && by && AuraNamed(victim, "Consecration", by->GetObjectGuid());
    }

    // The player's own Consecration on the ground, any rank.
    DynamicObject* OwnConsecration(Player* player)
    {
        for (uint32 id = CONSECRATION; id; id = sSpellMgr.GetNextSpellInChain(id))
            if (DynamicObject* dyn = player->GetDynObject(id))
                return dyn;
        return nullptr;
    }

    bool Inside(Player* player, DynamicObject* dyn)
    {
        return dyn && player->GetDistance(dyn, false, DIST_CALC_NONE) <= dyn->GetRadius();
    }

    // The attack speed percent the player should have.
    float HasteFor(Player const* player, WebTotals const& t)
    {
        float haste = 0.0f;
        if (t.Has(KEY_ZEALOT))
            haste += ZEALOT_HASTE;
        haste += 4.0f * t.Rank(KEY_QUICKENED);
        if (t.Has(KEY_ZEAL) && AuraNamed(player, "Seal of the Crusader", player->GetObjectGuid()))
            haste += 10.0f * t.Rank(KEY_ZEAL);
        haste += float(std::clamp(t.attackSpeedPct, 0, ATTACK_SPEED_CAP));
        return haste;
    }

    void SetHaste(Player* player, Runtime& r, float haste)
    {
        if (std::abs(haste - r.haste) < 0.01f)
            return;
        for (WeaponAttackType att : { BASE_ATTACK, OFF_ATTACK })
        {
            if (r.haste > 0.0f)
                player->ApplyAttackTimePercentMod(att, r.haste, false);
            if (haste > 0.0f)
                player->ApplyAttackTimePercentMod(att, haste, true);
        }
        r.haste = haste;
    }

    void SetImmunity(Player* player, Runtime& r, bool stun, bool snare)
    {
        if (stun != r.stunImmune)
        {
            player->ApplySpellImmune(nullptr, IMMUNITY_MECHANIC, MECHANIC_STUN, stun);
            if (stun)
                player->RemoveSpellsCausingAura(SPELL_AURA_MOD_STUN);
            r.stunImmune = stun;
        }
        if (snare != r.snareImmune)
        {
            player->ApplySpellImmune(nullptr, IMMUNITY_MECHANIC, MECHANIC_SNARE, snare);
            player->ApplySpellImmune(nullptr, IMMUNITY_STATE, SPELL_AURA_MOD_DECREASE_SPEED, snare);
            if (snare)
                player->RemoveSpellsCausingAura(SPELL_AURA_MOD_DECREASE_SPEED);
            r.snareImmune = snare;
        }
    }

    // The skill nodes' spell modifiers, applied as the totals list them.
    void SetSkillMods(Player* player, Runtime& r, std::vector<SkillSpellMod> const& want)
    {
        bool same = want.size() == r.skillMods.size();
        for (size_t i = 0; same && i < want.size(); ++i)
            same = want[i] == r.skillMods[i].first;
        if (same)
            return;
        for (auto& [spec, mod] : r.skillMods)
            if (mod)
                player->AddSpellMod(mod, false); // deletes it
        r.skillMods.clear();
        uint32 const owner = FamilySpell(player);
        for (SkillSpellMod const& spec : want)
        {
            SpellModifier* mod = nullptr;
            if (owner)
            {
                mod = new SpellModifier(SpellModOp(spec.op), SpellModType(spec.type), spec.value, owner, spec.mask);
                mod->arpgSpells = spec.spells;
                player->AddSpellMod(mod, true);
            }
            r.skillMods.emplace_back(spec, mod);
        }
    }

    // Walking Consecration: the ground itself moves under the player, so the auras it holds on
    // the enemies inside keep ticking (a fresh cast would restart them before their first tick).
    void WalkConsecration(Player* player, DynamicObject* dyn)
    {
        if (!dyn || player->GetDistance(dyn, false, DIST_CALC_NONE) <= WALK_FOLLOW)
            return;
        player->GetMap()->DynamicObjectRelocation(dyn, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ());
    }

    // The once-a-second refresh: speed, cooldown recovery, Zealot's drain, a level gained.
    void Refresh(Player* player, Runtime& r, uint32 now)
    {
        UpdateTree(player);
        UpdateSheet(player, false);
        // What carried items would do if worn (Arpg/ArpgCompare.h).
        UpdateCompare(player);
        {
            // The item affixes' regeneration, a second's worth.
            std::shared_ptr<WebTotals const> regen = TotalsOf(player);
            if (regen && player->IsAlive())
            {
                if (regen->healthRegen > 0 && player->GetHealth() < player->GetMaxHealth())
                    player->ModifyHealth(regen->healthRegen);
                if (regen->manaRegen > 0 && player->GetPowerType() == POWER_MANA)
                    player->ModifyPower(POWER_MANA, regen->manaRegen);
            }
        }
        SendNearbyChampions(player);
        std::shared_ptr<WebTotals const> totals = TotalsOf(player);
        WebTotals const& t = totals ? *totals : NoTotals();

        float const speed = MoveSpeedMod(player);
        if (std::abs(speed - r.speed) > 0.001f)
        {
            r.speed = speed;
            player->UpdateSpeed(MOVE_RUN, true);
        }
        SetCooldownMod(player, r, CooldownPct(player, t, r, now));
        SetHaste(player, r, HasteFor(player, t));

        // Consecration's skill nodes (Walking Consecration moves it in UpdateCharacter).
        DynamicObject* ground = (t.Has(KEY_HALLOWED_GROUND) || t.Has(KEY_STEADFAST)) ? OwnConsecration(player) : nullptr;
        bool const inside = Inside(player, ground);
        if (inside && t.Has(KEY_HALLOWED_GROUND) && player->IsAlive())
            ProcHeal(player, "Hallowed Ground", player->GetMaxHealth() * t.Rank(KEY_HALLOWED_GROUND) / 100);
        SetImmunity(player, r, t.Has(KEY_UNYIELDING) || (inside && t.Has(KEY_STEADFAST)), t.Has(KEY_UNYIELDING));

        if (t.Has(KEY_ZEALOT) && player->IsAlive() && SealOn(player))
        {
            int32 const drain = int32(player->GetMaxPower(POWER_MANA) / 100);
            if (drain > 0)
                player->ModifyPower(POWER_MANA, -drain);
        }
        while (!r.kills.empty() && WorldTimer::getMSTimeDiff(r.kills.front(), now) > CRUSADE_WINDOW)
            r.kills.pop_front();
    }

    // A hit's bonuses: "increased" ones add up into one sum, "more" ones each multiply.
    struct DamageParts
    {
        int32 increased = 0;
        float more = 1.0f;
    };

    // The increased and more bonuses on a hit of `tags` by `player` with `spellInfo` (nullptr: a
    // swing) on `victim` (nullptr: an estimate, the bonuses against a kind of enemy left out).
    // `why`, when given, takes a line per bonus that applies now, for the sheet; the conditions
    // themselves are ConditionalDamageLines's.
    DamageParts DamagePartsFor(Player const* player, WebTotals const& t, Unit const* victim, SpellEntry const* spellInfo,
                               uint32 tags, std::vector<std::string>* why)
    {
        DamageParts p;
        auto increased = [&](int32 pct, char const* from)
        {
            if (!pct)
                return;
            p.increased += pct;
            if (why)
                why->push_back(Format("%s%+d%% increased|r %s(%s)|r", SHEET_ARPG, pct, SHEET_NOTE, from));
        };
        auto more = [&](int32 pct, char const* from)
        {
            if (!pct)
                return;
            p.more *= std::max(0.0f, 1.0f + pct / 100.0f);
            if (why)
                why->push_back(Format("|cff20ff20%d%% more|r %s(%s)|r", pct, SHEET_NOTE, from));
        };
        Unit* self = const_cast<Player*>(player);

        // Increased: the tags (the sheet lists them by source already), the attributes, gear
        // against a kind of enemy.
        for (uint8 tag = 0; tag < MAX_TAG; ++tag)
            if (tag != TAG_HEAL && (tags & TagBit(Tag(tag))))
                p.increased += t.tag[tag];
        if (tags & (TagBit(TAG_MELEE) | TagBit(TAG_PHYSICAL)))
            increased(int32(player->GetStat(STAT_STRENGTH)) / STR_PER_DAMAGE, "Strength");
        if (tags & TagBit(TAG_SPELL))
            increased(int32(player->GetStat(STAT_INTELLECT)) / INT_PER_DAMAGE, "Intellect");
        if (tags & TagBit(TAG_DURATION))
            increased(int32(player->GetStat(STAT_SPIRIT)) / SPI_PER_DOT, "Spirit");
        if ((tags & TagBit(TAG_HOLY)) && t.Has(KEY_HOLY_WEAPONS))
            increased(3 * (int32(player->GetStat(STAT_STRENGTH)) / 10), "Holy Weapons");
        if (victim && victim->GetTypeId() == TYPEID_UNIT)
        {
            Creature const* foe = static_cast<Creature const*>(victim);
            if (IsDungeonBoss(foe) || foe->IsWorldBoss() || (foe->IsElite() && !foe->GetMap()->IsDungeon()))
                increased(t.elitePct, "gear, elites and bosses");
            uint32 const type = foe->GetCreatureType();
            if (type < 16)
                increased(t.slaying[type], "gear, its kind");
        }

        // More: the notables, keystones and skill tree nodes, each on its own.
        if ((tags & TagBit(TAG_MELEE)) && t.Has(KEY_TWO_HANDED_MASTERY) && TwoHanded(player))
            more(12, "Two-Handed Mastery");
        if ((tags & TagBit(TAG_MELEE)) && t.Has(KEY_SHIELD_AND_HAMMER) && OneHandAndShield(player))
            more(10, "Shield and Hammer");
        if (tags & TagBit(TAG_HOLY))
        {
            if (t.Has(KEY_LIGHTFORGED))
                more(30, "Lightforged");
            if (t.Has(KEY_RIGHTEOUS_FURY))
            {
                // (Not `near`: the Windows headers make that a macro.)
                size_t foes = 0;
                for (Unit* unit : EnemiesNear(self, FURY_RANGE))
                    if (unit->IsAlive() && MayCatchUnit(self, unit, nullptr) && ++foes >= FURY_COUNT)
                        break;
                if (foes >= FURY_COUNT)
                    more(20, "Righteous Fury");
            }
        }
        uint32 const now = WorldTimer::getMSTime();
        if (t.Has(KEY_CRUSADE))
            more(CRUSADE_PER_KILL * int32(CrusadeStacks(R(player), now)), "Crusade");
        // The mage's web.
        uint32 const fire = TagBit(TAG_FIRE), coldOrArcane = TagBit(TAG_FROST) | TagBit(TAG_ARCANE);
        if (t.Has(KEY_PYROMANCER))
        {
            if (tags & fire)
                more(PYROMANCER_PCT, "Pyromancer");
            else if (tags & coldOrArcane)
                more(-PYROMANCER_LESS_PCT, "Pyromancer");
        }
        if (t.Has(KEY_PERMAFROST) && (tags & fire))
            more(-PERMAFROST_LESS_PCT, "Permafrost");
        if (t.Has(KEY_SHATTER) && victim)
            more(Frozen(victim) ? SHATTER_PCT : -SHATTER_LESS_PCT, "Shatter");
        if (t.Has(KEY_ARCANE_FLUX))
            more(-FLUX_LESS_PCT, "Arcane Flux");
        if (t.Has(KEY_BRIMMING_MIND) && spellInfo && player->GetMaxPower(POWER_MANA))
        {
            int32 const manaPct = int32(uint64(player->GetPower(POWER_MANA)) * 100 / player->GetMaxPower(POWER_MANA));
            more(manaPct >= 50 ? std::min(BRIMMING_MAX_PCT, (manaPct - 50) / 2) : -BRIMMING_LESS_PCT, "Brimming Mind");
        }

        SkillId const skill = SkillOfSpell(player->getClass(), spellInfo);
        if (skill == SKILL_STRIKE && t.Has(KEY_MOMENTUM))
        {
            Runtime const& r = R(player);
            if (WorldTimer::getMSTimeDiff(r.lastHit, now) <= MOMENTUM_WINDOW)
                more(int32(r.momentum * t.Rank(KEY_MOMENTUM)), "Momentum");
        }
        if ((tags & TagBit(TAG_PHYSICAL)) && t.Has(KEY_HEAVY_HAND) && TwoHanded(player))
            more(5 * t.Rank(KEY_HEAVY_HAND), "Heavy Hand");
        if (IsSealHit(spellInfo))
        {
            more(6 * t.Rank(KEY_HOLY_EDGE), "Holy Edge");
            if (std::strncmp(spellInfo->SpellName[0], "Seal of Command", 15) == 0)
                more(10 * t.Rank(KEY_COMMANDING_SEAL), "Commanding Seal");
        }
        if (skill == SKILL_JUDGEMENT)
            more(8 * t.Rank(KEY_RADIANCE), "Radiance");
        if (skill == SKILL_CONSECRATION)
            more(8 * t.Rank(KEY_BURNING_GROUND), "Burning Ground");
        if (victim && victim->IsStunned())
        {
            if (skill == SKILL_JUDGEMENT && t.Has(KEY_SENTENCE))
                more(100, "Sentence");
            if (t.Has(KEY_SENTENCE_PASSED))
                more(15, "Sentence Passed");
        }
        if ((tags & TagBit(TAG_HOLY)) && t.Has(KEY_SEARING_LIGHT) && Consecrated(victim, player))
            more(5 * t.Rank(KEY_SEARING_LIGHT), "Searing Light");
        // A unique's power's fury (Arpg/ArpgProcs.h).
        more(UniqueFuryPct(player), "a unique's power");
        // Avenging Light: healing yourself, more damage a while.
        if (t.Has(KEY_AVENGING_LIGHT) && int32(R(player).avengeUntil - now) > 0)
            more(AVENGING_LIGHT_PCT * t.Rank(KEY_AVENGING_LIGHT), "Avenging Light");
        // Executioner: Hammer of Wrath doubles on an enemy below 20% health.
        if (spellInfo && spellInfo->SpellName[0] && std::strncmp(spellInfo->SpellName[0], "Hammer of Wrath", 15) == 0 &&
                t.Has(KEY_EXECUTIONER) && victim &&
                victim->GetHealth() < victim->GetMaxHealth() * EXECUTE_BELOW)
            more(100, "Executioner");
        return p;
    }
}

namespace Arpg
{
    void ApplyCharacter(Player* player)
    {
        Runtime& r = R(player);
        std::shared_ptr<WebTotals const> totals = TotalsOf(player);
        WebTotals const& t = totals ? *totals : NoTotals();

        for (int i = 0; i < 5; ++i)
        {
            if (t.stat[i] == r.stat[i])
                continue;
            UnitMods const mod = UnitMods(UNIT_MOD_STAT_START + i);
            if (r.stat[i])
                player->HandleStatModifier(mod, TOTAL_VALUE, float(r.stat[i]), false);
            if (t.stat[i])
                player->HandleStatModifier(mod, TOTAL_VALUE, float(t.stat[i]), true);
            r.stat[i] = t.stat[i];
        }
        if (t.armourPct != r.armourPct)
        {
            if (r.armourPct)
                player->HandleStatModifier(UNIT_MOD_ARMOR, TOTAL_PCT, float(r.armourPct), false);
            if (t.armourPct)
                player->HandleStatModifier(UNIT_MOD_ARMOR, TOTAL_PCT, float(t.armourPct), true);
            r.armourPct = t.armourPct;
        }
        player->UpdateBlockPercentage();
        // The item affixes' stats, each as a modifier swapped for the new total.
        auto swap = [player](UnitMods mod, UnitModifierType type, int32& applied, int32 want)
        {
            if (want == applied)
                return;
            if (applied)
                player->HandleStatModifier(mod, type, float(applied), false);
            if (want)
                player->HandleStatModifier(mod, type, float(want), true);
            applied = want;
        };
        swap(UNIT_MOD_HEALTH, TOTAL_PCT, r.healthPct, t.healthPct);
        swap(UNIT_MOD_MANA, TOTAL_PCT, r.manaPct, t.manaPct);
        {
            int32 ranged = r.attackPower;
            swap(UNIT_MOD_ATTACK_POWER_RANGED, TOTAL_VALUE, ranged, t.attackPower);
            swap(UNIT_MOD_ATTACK_POWER, TOTAL_VALUE, r.attackPower, t.attackPower);
        }
        for (int i = 0; i < 5; ++i)
            swap(UnitMods(UNIT_MOD_RESISTANCE_FIRE + i), TOTAL_VALUE, r.resist[i], t.resist[i]);
        int32 const crit = std::clamp(t.meleeCritPct, 0, CRIT_CAP);
        if (crit != r.meleeCrit)
        {
            for (BaseModGroup group : { CRIT_PERCENTAGE, RANGED_CRIT_PERCENTAGE })
            {
                if (r.meleeCrit)
                    player->HandleBaseModValue(group, FLAT_MOD, float(r.meleeCrit), false);
                if (crit)
                    player->HandleBaseModValue(group, FLAT_MOD, float(crit), true);
            }
            r.meleeCrit = crit;
        }

        SetHaste(player, r, HasteFor(player, t));
        // ARPG pacing on every class spell, beside the trees' modifiers: a 0.5 sec global
        // cooldown and half the cast time. Spell modifiers, so the client's cooldown sweep and
        // the cast bar follow on their own.
        std::vector<SkillSpellMod> mods = t.mods;
        if (Active(player))
        {
            mods.push_back({ uint8(SPELLMOD_GLOBAL_COOLDOWN), uint8(SPELLMOD_PCT), ARPG_GCD_CUT_PCT, ~uint64(0) });
            mods.push_back({ uint8(SPELLMOD_CASTING_TIME), uint8(SPELLMOD_PCT), ARPG_CAST_TIME_PCT, ~uint64(0) });
            // The item affixes' spell stats, over every class spell.
            if (int32 const v = std::clamp(t.castSpeedPct, 0, CAST_SPEED_CAP))
                mods.push_back({ uint8(SPELLMOD_CASTING_TIME), uint8(SPELLMOD_PCT), -v, ~uint64(0) });
            if (int32 const v = std::clamp(t.costPct, 0, COST_CAP))
                mods.push_back({ uint8(SPELLMOD_COST), uint8(SPELLMOD_PCT), -v, ~uint64(0) });
            if (int32 const v = std::clamp(t.spellCritPct, 0, CRIT_CAP))
                mods.push_back({ uint8(SPELLMOD_CRITICAL_CHANCE), uint8(SPELLMOD_FLAT), v, ~uint64(0) });
            // (Spell critical strike damage adds to the multiplier: Arpg::CritBonus.)
        }
        SetSkillMods(player, r, mods);
        SetImmunity(player, r, t.Has(KEY_UNYIELDING) || (r.stunImmune && t.Has(KEY_STEADFAST)), t.Has(KEY_UNYIELDING));
        // Speed and cooldowns at once, not at the next refresh.
        r.lastRefresh = 0;
    }

    void ForgetCharacter(Player* player)
    {
        // The player is going; its cooldown modifier stays in its spell modifier list, which
        // Player never frees, as an aura's stays until the aura goes.
        ForgetChampionsSent(player);
        ForgetEvents(player);
        ForgetProcs(player);
        ForgetAilments(player);
        ForgetMage(player);
        ForgetCompare(player);
        std::lock_guard<std::mutex> guard(sRuntimeLock);
        sRuntime.erase(player->GetObjectGuid());
    }

    void UpdateCharacter(Player* player)
    {
        Runtime& r = R(player);
        uint32 const now = WorldTimer::getMSTime();
        // A readied proc that waited too long goes (Arpg/ArpgProcs.h).
        UpdateProcs(player);
        // The ailments this player inflicted tick and burst (Arpg/ArpgDamage.h).
        UpdateAilments(player);

        // Rest: out of combat a moment, health and mana flow back (no eating or drinking).
        if (player->IsInCombat() || !player->IsAlive() || !r.combatSeen)
            r.combatSeen = now ? now : 1;
        if (r.lastRest && WorldTimer::getMSTimeDiff(r.combatSeen, now) >= REST_DELAY_MS)
        {
            // Carried in thousandths, so the slow mana rate still adds up between updates.
            float const secs = float(std::min(WorldTimer::getMSTimeDiff(r.lastRest, now), REST_GAP_MAX_MS)) / 1000.0f;
            r.restCarry += secs * 10.0f;    // permille per percent
            if (r.restCarry >= 10.0f)
            {
                float const units = std::floor(r.restCarry);
                r.restCarry -= units;
                uint32 const health = uint32(units * REST_HEALTH_PCT);    // permille
                uint32 const mana = uint32(units * REST_MANA_PCT);
                if (player->GetHealth() < player->GetMaxHealth())
                    player->ModifyHealth(int32(std::max<uint32>(1, player->GetMaxHealth() * health / 1000)));
                GiveManaPermille(player, mana);
            }
        }
        else
            r.restCarry = 0.0f;
        r.lastRest = now ? now : 1;

        // Thorns: each attacker that hit since the last update.
        if (!r.thornsDue.empty())
        {
            std::vector<ObjectGuid> const due = std::move(r.thornsDue);
            r.thornsDue.clear();
            std::shared_ptr<WebTotals const> totals = TotalsOf(player);
            SpellEntry const* shown = sSpellTemplate.LookupEntry<SpellEntry>(THORNS_VISUAL);
            if (totals && totals->thorns > 0 && shown && player->IsAlive())
                for (ObjectGuid const& guid : due)
                    if (Unit* foe = player->GetMap()->GetUnit(guid))
                        if (foe->IsAlive() && player->IsWithinDistInMap(foe, THORNS_REACH))
                            StrikeFoe(player, foe, shown, SPELL_SCHOOL_MASK_NATURE, ToDamage(player, foe, uint32(totals->thorns)));
        }

        // Martyr: the damage taken since the last update strikes back.
        if (r.martyrPending && player->IsAlive())
        {
            uint32 const share = r.martyrPending;
            r.martyrPending = 0;
            if (SpellEntry const* aura = sSpellTemplate.LookupEntry<SpellEntry>(RETRIBUTION_AURA))
            {
                std::vector<Unit*> const foes = NearestFoes(player, player, MARTYR_RANGE, 10);
                // At the ARPG's size against each creature (Arpg/ArpgDamage.h).
                for (Unit* unit : foes)
                    StrikeFoe(player, unit, aura, SPELL_SCHOOL_MASK_HOLY, ToDamage(player, unit, share));
                if (!foes.empty())
                    ReportProc(player, PROC_DAMAGE, 0, "Martyr", "strikes back at %u %s for %u Holy.",
                               uint32(foes.size()), foes.size() == 1 ? "enemy" : "enemies", ToDamage(player, nullptr, share));
            }
        }

        // Wrathful: Holy Wrath fires on its own when the player is surrounded.
        if (player->IsAlive() && player->IsInCombat() && WorldTimer::getMSTimeDiff(r.lastWrath, now) >= WRATHFUL_EVERY &&
                HasKeystone(player, KEY_WRATHFUL))
        {
            size_t foes = 0;
            for (Unit* unit : EnemiesNear(player, WRATHFUL_RANGE))
                if (unit->IsAlive() && MayCatchUnit(player, unit, nullptr))
                    ++foes;
            if (foes >= WRATHFUL_COUNT)
            {
                for (uint32 spell : { 10318u, 2812u })      // Holy Wrath, its highest rank known
                    if (player->HasSpell(spell))
                    {
                        player->CastSpell(player, spell, TRIGGERED_OLD_TRIGGERED | TRIGGERED_INSTANT_CAST);
                        r.lastWrath = now ? now : 1;
                        ReportProc(player, PROC_EFFECT, 0, "Wrathful", "unleashes Holy Wrath on %u enemies.", uint32(foes));
                        break;
                    }
            }
        }

        // Walking Consecration follows a few times a second.
        if (WorldTimer::getMSTimeDiff(r.lastWalk, now) >= WALK_MS)
        {
            r.lastWalk = now;
            if (player->IsAlive() && HasKeystone(player, KEY_WALKING_CONSECRATION))
                WalkConsecration(player, OwnConsecration(player));
        }

        if (r.lastRefresh && WorldTimer::getMSTimeDiff(r.lastRefresh, now) < REFRESH_MS)
            return;
        r.lastRefresh = now ? now : 1;
        Refresh(player, r, now);
    }

    float DamageDoneMod(Unit const* attacker, Unit const* victim, SpellEntry const* spellInfo)
    {
        return DamageModForTags(attacker, victim, spellInfo, TagsOf(spellInfo));
    }

    void DamagePartsOf(Player const* player, WebTotals const& t, SpellEntry const* spellInfo, uint32 tags, int32& increased, float& more)
    {
        DamageParts const parts = DamagePartsFor(player, t, nullptr, spellInfo, tags, nullptr);
        increased = parts.increased;
        more = parts.more;
    }

    float DamageModForTags(Unit const* attacker, Unit const* victim, SpellEntry const* spellInfo, uint32 tags)
    {
        // A creature: a pack follower hits for its share (ArpgPacks.h); an ARPG player's pet,
        // totem or guardian at its owner's size.
        if (attacker && attacker->GetTypeId() == TYPEID_UNIT)
        {
            // A player above the creature's level takes less from it, below it more.
            float mod = PackDamageMod(attacker) * DamageScale(attacker, victim) * LevelGapMod(attacker, victim);
            // Damage taken, the victim's item affixes.
            if (victim && victim->GetTypeId() == TYPEID_PLAYER)
                if (std::shared_ptr<WebTotals const> taken = TotalsOf(victim))
                    mod *= 1.0f - std::clamp(taken->damageTakenPct, 0, DAMAGE_TAKEN_CAP) / 100.0f;
            return mod;
        }
        std::shared_ptr<WebTotals const> totals = TotalsOf(attacker);
        if (!totals)
            return 1.0f;
        DamageParts const parts = DamagePartsFor(static_cast<Player const*>(attacker), *totals, victim, spellInfo, tags, nullptr);
        return std::max(0.0f, 1.0f + parts.increased / 100.0f) * parts.more * DamageScale(attacker, victim) *
               AilmentTakenMod(attacker, victim);
    }

    void ConditionalDamageLines(Player const* player, uint32 tags, uint8 skill, std::vector<std::string>& out)
    {
        std::shared_ptr<WebTotals const> totals = TotalsOf(player);
        if (!totals)
            return;
        WebTotals const& t = *totals;
        // Kept beside DamageDoneMod, which they explain: a bonus added there is listed here.
        auto line = [&](bool active, char const* text)
        {
            out.push_back(Format("%s%s%s|r", active ? "|cff20ff20" : SHEET_NOTE, text, active ? " (on now)" : ""));
        };
        if ((tags & TagBit(TAG_MELEE)) && t.Has(KEY_TWO_HANDED_MASTERY))
            line(TwoHanded(player), "Two-Handed Mastery: 12% more with a two-handed weapon");
        if ((tags & TagBit(TAG_MELEE)) && t.Has(KEY_SHIELD_AND_HAMMER))
            line(OneHandAndShield(player), "Shield and Hammer: 10% more with a one-hander and a shield");
        if ((tags & TagBit(TAG_PHYSICAL)) && t.Has(KEY_HEAVY_HAND))
            line(TwoHanded(player), Format("Heavy Hand: %d%% more with a two-handed weapon", 5 * t.Rank(KEY_HEAVY_HAND)).c_str());
        if (tags & TagBit(TAG_HOLY))
        {
            if (t.Has(KEY_HOLY_WEAPONS))
                line(true, Format("Holy Weapons: +%d%% increased from your Strength", 3 * (int32(player->GetStat(STAT_STRENGTH)) / 10)).c_str());
            if (t.Has(KEY_LIGHTFORGED))
                line(true, "Lightforged: 30% more");
            if (t.Has(KEY_RIGHTEOUS_FURY))
                line(false, "Righteous Fury: 20% more with three or more enemies within 8 yards");
            if (t.Has(KEY_SEARING_LIGHT))
                line(false, Format("Searing Light: %d%% more on consecrated enemies", 5 * t.Rank(KEY_SEARING_LIGHT)).c_str());
        }
        if (t.Has(KEY_CRUSADE))
            line(false, "Crusade: 4% more per kill in the last 5 sec, up to 10 kills");
        // The item affixes' bonuses against a kind of enemy.
        if (t.elitePct)
            line(false, Format("Gear: +%d%% increased to elites and bosses", t.elitePct).c_str());
        static char const* const TYPE_NAME[16] = { "", "Beasts", "Dragonkin", "Demons", "Elementals", "Giants", "Undead",
                                                   "Humanoids", "Critters", "Mechanicals", "", "", "", "", "", "" };
        for (uint32 type = 1; type < 16; ++type)
            if (t.slaying[type] && TYPE_NAME[type][0])
                line(false, Format("Gear: +%d%% increased to %s", t.slaying[type], TYPE_NAME[type]).c_str());
        if (skill == SKILL_STRIKE && t.Has(KEY_MOMENTUM))
            line(false, Format("Momentum: %d%% more per hit in a row, up to 5", int32(t.Rank(KEY_MOMENTUM))).c_str());
        if (t.Has(KEY_SENTENCE_PASSED))
            line(false, "Sentence Passed: 15% more on stunned enemies");
        if (tags & TagBit(TAG_AREA))
        {
            bool const melee = (tags & TagBit(TAG_MELEE)) != 0;
            int32 const pct = melee ? int32(player->GetStat(STAT_STRENGTH)) / STR_PER_AREA + t.meleeAreaPct
                                    : int32(player->GetStat(STAT_INTELLECT)) / INT_PER_AREA + t.spellAreaPct;
            out.push_back(Format("%s%s area: %s size (%s)|r", SHEET_ARPG, melee ? "Melee" : "Spell",
                                 CappedPct(std::clamp(pct, 0, AREA_CAP), AREA_CAP).c_str(), melee ? "Strength" : "Intellect"));
        }
    }

    void SkillDamageLines(Player const* constPlayer, uint8 skill, uint32 tags, std::vector<std::string>& out)
    {
        Player* player = const_cast<Player*>(constPlayer);
        // Gear and buffs: the auras that add to this skill's damage, by their spell's name.
        // The damage tags' schools (the tags' order is not the schools').
        static std::pair<Tag, uint32> const SCHOOL_OF[] =
        {
            { TAG_PHYSICAL, SPELL_SCHOOL_MASK_NORMAL }, { TAG_HOLY, SPELL_SCHOOL_MASK_HOLY },
            { TAG_FIRE, SPELL_SCHOOL_MASK_FIRE }, { TAG_FROST, SPELL_SCHOOL_MASK_FROST },
            { TAG_ARCANE, SPELL_SCHOOL_MASK_ARCANE }, { TAG_NATURE, SPELL_SCHOOL_MASK_NATURE },
            { TAG_SHADOW, SPELL_SCHOOL_MASK_SHADOW },
        };
        uint32 schools = 0;
        for (auto const& [tag, mask] : SCHOOL_OF)
            if (tags & TagBit(tag))
                schools |= mask;
        if (!schools)
            schools = SPELL_SCHOOL_MASK_NORMAL;
        bool const melee = (tags & TagBit(TAG_MELEE)) != 0;
        bool const spell = (tags & TagBit(TAG_SPELL)) != 0 || !melee;
        auto aura = [&](AuraType type, bool use, char const* what, bool pct, bool bySchool)
        {
            if (!use)
                return;
            for (Aura* a : player->GetAurasByType(type))
            {
                Modifier const* m = a->GetModifier();
                if (!m || !m->m_amount || (bySchool && !(m->m_miscvalue & schools)))
                    continue;
                char const* name = a->GetSpellProto()->SpellName[0];
                bool const gear = !a->GetCastItemGuid().IsEmpty();
                out.push_back(Format("%s  %+d%s %s|r |cff9d9d9d(%s, %s)|r", gear ? "|cffffffff" : "|cff80c0ff",
                                     m->m_amount, pct ? "%" : "", what, name ? name : "?", gear ? "gear" : "buff"));
            }
        };
        size_t const before = out.size();
        out.push_back("|cffffd200Gear and buffs|r");
        aura(SPELL_AURA_MOD_DAMAGE_DONE, spell, "spell damage", false, true);
        aura(SPELL_AURA_MOD_DAMAGE_PERCENT_DONE, true, "damage", true, true);
        aura(SPELL_AURA_MOD_ATTACK_POWER, melee, "attack power", false, false);
        aura(SPELL_AURA_MOD_CRIT_PERCENT, melee, "critical strike chance", true, false);
        aura(SPELL_AURA_MOD_SPELL_CRIT_CHANCE, spell, "spell critical strike chance", true, false);
        aura(SPELL_AURA_MOD_SPELL_CRIT_CHANCE_SCHOOL, spell, "spell critical strike chance", true, true);
        aura(SPELL_AURA_MOD_MELEE_HASTE, melee, "attack speed", true, false);
        if (out.size() == before + 1)
            out.back() = "|cff9d9d9dGear and buffs: nothing adds to it right now.|r";

        // The character's own numbers behind it.
        if (melee)
            out.push_back(Format("|cffffffffAttack power %d, critical strike %.1f%%|r", int32(player->GetTotalAttackPowerValue(BASE_ATTACK)),
                                 player->GetFloatValue(PLAYER_CRIT_PERCENTAGE)));
        if (spell)
            out.push_back(Format("|cffffffffSpell damage %d|r", player->SpellBaseDamageBonusDone(SpellSchoolMask(schools))));

        // The bonuses that apply now: one sum of increased, each more on its own; then crits,
        // added damage and ailments (Arpg/ArpgDamage.h).
        if (std::shared_ptr<WebTotals const> totals = TotalsOf(player))
        {
            SpellEntry const* sample = nullptr;
            if (skill != SwingSkill(player->getClass()))
                for (auto const& [id, known] : player->GetSpellMap())
                    if (known.state != PLAYERSPELL_REMOVED && !known.disabled)
                        if (SpellEntry const* info = sSpellTemplate.LookupEntry<SpellEntry>(id))
                            if (SkillOfSpell(player->getClass(), info) == SkillId(skill))
                            {
                                sample = info;
                                break;
                            }
            std::vector<std::string> why;
            DamageParts const parts = DamagePartsFor(player, *totals, nullptr, sample, tags, &why);
            out.push_back(Format("|cffffd200Damage|r |cffffffffx%.1f size, +%d%% increased, x%.2f more|r",
                                 PowerScale(player), parts.increased, parts.more));
            out.push_back(Format("  %s+%d%% increased|r %s(its tags, above)|r", SHEET_ARPG, TagSum(*totals, tags & ~TagBit(TAG_HEAL)), SHEET_NOTE));
            for (std::string const& line : why)
                out.push_back("  " + line);
            HitExtraLines(player, sample, tags, out);
        }

        // An estimate: the swing's, or the skill's best known rank's direct or periodic damage, with
        // every bonus above, before the target's armour, resistances and its own modifiers.
        if (skill == SwingSkill(player->getClass()))
        {
            float const lo = player->GetFloatValue(UNIT_FIELD_MINDAMAGE), hi = player->GetFloatValue(UNIT_FIELD_MAXDAMAGE);
            float const mod = DamageDoneMod(player, nullptr, nullptr);
            out.push_back(Format("|cff20ff20Each swing: %d to %d, every %.2f sec|r", int32(lo * mod), int32(hi * mod),
                                 player->GetAttackTime(BASE_ATTACK) / 1000.0f));
        }
        else
        {
            SpellEntry const* best = nullptr;
            SpellEffectIndex effect = EFFECT_INDEX_0;
            bool periodic = false;
            for (auto const& [id, known] : player->GetSpellMap())
            {
                if (known.state == PLAYERSPELL_REMOVED || known.disabled)
                    continue;
                SpellEntry const* info = sSpellTemplate.LookupEntry<SpellEntry>(id);
                if (!info || SkillOfSpell(player->getClass(), info) != SkillId(skill))
                    continue;
                for (uint32 i = 0; i < MAX_EFFECT_INDEX; ++i)
                {
                    bool const direct = info->Effect[i] == SPELL_EFFECT_SCHOOL_DAMAGE;
                    bool const dot = info->Effect[i] == SPELL_EFFECT_APPLY_AURA && info->EffectApplyAuraName[i] == SPELL_AURA_PERIODIC_DAMAGE;
                    bool const ground = info->Effect[i] == SPELL_EFFECT_PERSISTENT_AREA_AURA && info->EffectApplyAuraName[i] == SPELL_AURA_PERIODIC_DAMAGE;
                    if ((direct || dot || ground) && (!best || info->spellLevel > best->spellLevel))
                    {
                        best = info;
                        effect = SpellEffectIndex(i);
                        periodic = !direct;
                    }
                }
            }
            if (best)
            {
                // As SpellDamageBonusDone counts it, without its final spell modifier use: an
                // estimate must never spend a charged modifier.
                int32 const base = best->EffectBasePoints[effect] + 1;
                int32 const spread = std::max(0, best->EffectDieSides[effect] - 1);
                SpellSchoolMask const mask = GetSpellSchoolMask(best);
                float pctMod = DamageDoneMod(player, nullptr, best);
                for (Aura* a : player->GetAurasByType(SPELL_AURA_MOD_DAMAGE_PERCENT_DONE))
                    if ((a->GetModifier()->m_miscvalue & mask) && a->GetSpellProto()->EquippedItemClass == -1 &&
                            a->GetSpellProto()->EquippedItemInventoryTypeMask == 0)
                        pctMod *= (a->GetModifier()->m_amount + 100.0f) / 100.0f;
                int32 const bonus = player->SpellBonusWithCoeffs(best, effect, 0, player->SpellBaseDamageBonusDone(mask), 0, true);
                auto finish = [&](int32 amount)
                {
                    float value = float(amount + bonus) * pctMod;
                    player->ApplySpellMod(best->Id, periodic ? SPELLMOD_DOT : SPELLMOD_DAMAGE, value, false);
                    return uint32(std::max(0.0f, value));
                };
                uint32 const lo = finish(std::max(1, base)), hi = finish(std::max(1, base + spread));
                if (periodic)
                    out.push_back(Format("|cff20ff20%s: about %u a tick|r", best->SpellName[0], lo));
                else
                    out.push_back(Format("|cff20ff20%s: about %u to %u a hit|r", best->SpellName[0], lo, hi));
            }
        }
        // What it really did: the last minute's hits.
        Runtime& r = R(player);
        uint32 const now = WorldTimer::getMSTime();
        auto it = r.hits.find(skill);
        uint32 count = 0, total = 0, top = 0, last = 0;
        if (it != r.hits.end())
            for (auto const& [at, damage] : it->second)
                if (WorldTimer::getMSTimeDiff(at, now) <= HITS_WINDOW)
                {
                    ++count;
                    total += damage;
                    top = std::max(top, damage);
                    last = damage;
                }
        if (count)
            out.push_back(Format("|cff20ff20Last minute: %u hits, %u on average, %u at most, the last %u|r",
                                 count, total / count, top, last));
        else
            out.push_back("|cff9d9d9dLast minute: no hits yet. Use it, then look again.|r");
    }

    void UpdateSheet(Player* player, bool force)
    {
        if (!player || !Active(player) || !player->GetSession())
            return;
        Runtime& r = R(player);
        uint32 const now = WorldTimer::getMSTime();
        if (!force && r.sheetAt && WorldTimer::getMSTimeDiff(r.sheetAt, now) < SHEET_EVERY)
            return;
        r.sheetAt = now ? now : 1;
        std::shared_ptr<WebTotals const> totals = TotalsOf(player);
        WebTotals const& t = totals ? *totals : NoTotals();

        std::vector<std::string> attributes;
        for (uint8 stat = STAT_STRENGTH; stat <= STAT_SPIRIT; ++stat)
            attributes.push_back(AttributeText(player, t, stat));
        std::vector<std::pair<uint8, std::string>> skills;
        for (auto const& [id, lines] : SkillBonusLines(player))
            skills.emplace_back(id, Joined(lines));
        std::string const summary = SummaryText(player, t);

        // Only a change is sent.
        std::string all = summary;
        for (std::string const& a : attributes)
            all += a;
        for (auto const& [id, text] : skills)
            all += char(id) + text;
        size_t const hash = std::hash<std::string>()(all);
        if (!force && hash == r.sheetHash)
            return;
        r.sheetHash = hash;

        WorldPacket data(SMSG_ARPG_SHEET, all.size() + 64);
        data << uint8(1);
        data << uint8(attributes.size());
        for (std::string const& a : attributes)
            data << a;
        data << uint8(skills.size());
        for (auto const& [id, text] : skills)
            data << uint8(id) << text;
        data << summary;
        player->GetSession()->SendPacket(data);
    }

    float XpGainMod(Unit const* gainer)
    {
        if (!gainer || gainer->GetTypeId() != TYPEID_PLAYER)
            return 1.0f;
        std::shared_ptr<WebTotals const> totals = TotalsOf(gainer);
        return totals ? 1.0f + std::clamp(totals->xpPct, 0, 50) / 100.0f : 1.0f;
    }

    int32 SpellPowerBonus(Unit const* caster)
    {
        if (!caster || caster->GetTypeId() != TYPEID_PLAYER)
            return 0;
        std::shared_ptr<WebTotals const> totals = TotalsOf(caster);
        return totals ? std::max(0, totals->spellPower) : 0;
    }

    float GlobeHealMod(Unit const* unit)
    {
        std::shared_ptr<WebTotals const> totals = TotalsOf(unit);
        return totals ? 1.0f + std::clamp(totals->globeHealPct, 0, 150) / 100.0f : 1.0f;
    }

    float HealingDoneMod(Unit const* healer, SpellEntry const* /*spellInfo*/)
    {
        std::shared_ptr<WebTotals const> totals = TotalsOf(healer);
        if (!totals)
            return 1.0f;
        return std::max(0.0f, 1.0f + (totals->healingPct + totals->tag[TAG_HEAL]) / 100.0f);
    }

    float HealingTakenMod(Unit const* target)
    {
        return HasKeystone(target, KEY_MARTYR) ? 0.5f : 1.0f;
    }

    float MeleeAreaScale(Unit const* unit)
    {
        if (!Active(unit))
            return 1.0f;
        std::shared_ptr<WebTotals const> totals = TotalsOf(unit);
        WebTotals const& t = totals ? *totals : NoTotals();
        int32 const pct = int32(unit->GetStat(STAT_STRENGTH)) / STR_PER_AREA + t.meleeAreaPct;
        return 1.0f + std::clamp(pct, 0, AREA_CAP) / 100.0f;
    }

    float SpellAreaScale(Unit const* unit)
    {
        if (!Active(unit))
            return 1.0f;
        std::shared_ptr<WebTotals const> totals = TotalsOf(unit);
        WebTotals const& t = totals ? *totals : NoTotals();
        int32 const pct = int32(unit->GetStat(STAT_INTELLECT)) / INT_PER_AREA + t.spellAreaPct;
        return 1.0f + std::clamp(pct, 0, AREA_CAP) / 100.0f;
    }

    float AreaScale(Unit const* caster, SpellEntry const* spellInfo)
    {
        if (!spellInfo || !Active(caster))
            return 1.0f;
        uint32 const tags = TagsOf(spellInfo);
        if (!(tags & TagBit(TAG_AREA)))
            return 1.0f;
        std::shared_ptr<WebTotals const> totals = TotalsOf(caster);
        WebTotals const& t = totals ? *totals : NoTotals();
        int32 pct = (tags & TagBit(TAG_MELEE))
                    ? int32(caster->GetStat(STAT_STRENGTH)) / STR_PER_AREA + t.meleeAreaPct
                    : int32(caster->GetStat(STAT_INTELLECT)) / INT_PER_AREA + t.spellAreaPct;
        return 1.0f + std::clamp(pct, 0, AREA_CAP) / 100.0f;
    }

    float MoveSpeedMod(Unit const* unit)
    {
        if (unit && unit->GetTypeId() == TYPEID_UNIT)
            return PackSpeedMod(unit);
        if (!Active(unit))
            return 1.0f;
        std::shared_ptr<WebTotals const> totals = TotalsOf(unit);
        WebTotals const& t = totals ? *totals : NoTotals();
        int32 const pct = std::clamp(BASE_SPEED_PCT + int32(unit->GetStat(STAT_AGILITY)) / AGI_PER_SPEED + t.movePct, 0, SPEED_CAP);
        float speed = 1.0f + pct / 100.0f;
        if (t.Has(KEY_CRUSADE) && !unit->IsInCombat())
            speed *= CRUSADE_SLOW;
        if (t.Has(KEY_UNYIELDING))
            speed *= UNYIELDING_SLOW;
        if (t.Has(KEY_CRUSADERS_PACE) && WorldTimer::getMSTime() < R(unit).paceUntil)
            speed *= PACE_SPEED;
        // A unique's power's swiftness (Arpg/ArpgProcs.h).
        if (int32 const swift = UniqueSwiftPct(unit))
            speed *= 1.0f + swift / 100.0f;
        return speed;
    }

    float BlockChanceBonus(Unit const* unit)
    {
        std::shared_ptr<WebTotals const> totals = TotalsOf(unit);
        return totals ? float(totals->blockPct) : 0.0f;
    }

    bool ForcesCrit(Unit* caster, SpellEntry const* spellInfo)
    {
        if (!spellInfo || !HasKeystone(caster, KEY_DIVINE_FAVOUR) || !(TagsOf(spellInfo) & TagBit(TAG_HOLY)))
            return false;
        Runtime& r = R(caster);
        uint32 const now = WorldTimer::getMSTime();
        if (r.favourReady && now < r.favourReady)
            return false;
        r.favourReady = now + FAVOUR_COOLDOWN;
        ReportProc(caster, PROC_EFFECT, 0, "Divine Favour", "makes %s a critical strike.", spellInfo->SpellName[0]);
        return true;
    }

    bool BlocksHeal(Unit const* caster, Unit const* target)
    {
        return caster == target && HasKeystone(caster, KEY_LIGHTFORGED);
    }

    void OnCharacterHeal(Spell* spell, Unit* caster, Unit* target, uint32 amount)
    {
        if (!spell || spell->IsArpgSecondary() || !amount || !Active(caster))
            return;
        Player* player = static_cast<Player*>(caster);
        if (caster == target && HasKeystone(caster, KEY_BLESSED_RECOVERY))
        {
            Runtime& r = R(caster);
            r.recoveryUntil = WorldTimer::getMSTime() + RECOVERY_MS;
            r.lastRefresh = 0;
        }
        // Holy Light's tree: heals that hurt.
        if (player->getClass() == CLASS_PALADIN && SkillOfSpell(player->getClass(), spell->m_spellInfo) == SKILL_HOLY_LIGHT)
            if (std::shared_ptr<WebTotals const> totals = TotalsOf(caster))
            {
                WebTotals const& t = *totals;
                uint32 const now = WorldTimer::getMSTime();
                float const area = SpellAreaScale(player);
                if (uint8 const rank = t.Rank(KEY_LIGHT_STRIKE))
                {
                    std::vector<Unit*> const foes = NearestFoes(player, player, LIGHT_STRIKE_RANGE, 1);
                    if (!foes.empty())
                    {
                        uint32 const dealt = ToDamage(player, foes.front(), amount * LIGHT_STRIKE_PCT * rank / 100);
                        SendHolyBolt(player, foes.front(), std::max<uint32>(1, dealt));
                        ReportProcOn(player, foes.front(), TAG_HOLY, PROC_DAMAGE, dealt, "Searing Radiance", "strikes %s for %u.", foes.front()->GetName(), dealt);
                    }
                }
                if (uint8 const rank = t.Rank(KEY_JUDGING_LIGHT))
                {
                    Runtime& r = R(player);
                    if (WorldTimer::getMSTimeDiff(r.lastJudgingLight, now) >= JUDGING_LIGHT_EVERY)
                    {
                        std::vector<Unit*> const foes = NearestFoes(player, player, JUDGING_LIGHT_RANGE * area, 10);
                        if (!foes.empty())
                        {
                            r.lastJudgingLight = now ? now : 1;
                            for (Unit* unit : foes)
                                InflictAilment(player, unit, TAG_HOLY, ToDamage(player, unit, amount * JUDGING_LIGHT_PCT * rank / 100), false);
                            ReportProc(player, PROC_EFFECT, 0, "Judging Light", "judges %u %s.", uint32(foes.size()),
                                       foes.size() == 1 ? "enemy" : "enemies");
                        }
                    }
                }
                if (caster == target && t.Has(KEY_AVENGING_LIGHT))
                    R(player).avengeUntil = now + AVENGING_LIGHT_MS;
                if (t.Has(KEY_HOLY_NOVA))
                    if (SpellEntry const* look = sSpellTemplate.LookupEntry<SpellEntry>(HOLY_NOVA_LOOK))
                    {
                        std::vector<Unit*> const foes = NearestFoes(player, player, HOLY_NOVA_RANGE * area, 10);
                        for (Unit* unit : foes)
                            StrikeFoe(player, unit, look, SPELL_SCHOOL_MASK_HOLY, std::max<uint32>(1, ToDamage(player, unit, amount * HOLY_NOVA_PCT / 100)));
                        if (!foes.empty())
                            ReportProc(player, PROC_DAMAGE, 0, "Holy Nova", "bursts onto %u %s.", uint32(foes.size()),
                                       foes.size() == 1 ? "enemy" : "enemies");
                    }
            }
        if (HasKeystone(caster, KEY_LIGHTFORGED))
        {
            std::vector<Unit*> foes = NearestFoes(player, player, LIGHTFORGED_RANGE, 1);
            if (!foes.empty())
            {
                // The heal's size turned into damage's (Arpg/ArpgDamage.h).
                uint32 const dealt = ToDamage(player, foes.front(), amount);
                SendHolyBolt(player, foes.front(), dealt);
                ReportProcOn(player, foes.front(), TAG_HOLY, PROC_DAMAGE, dealt, "Lightforged", "turns the heal into a Holy bolt at %s for %u.",
                           foes.front()->GetName(), dealt);
            }
        }
    }

    void OnDamageTaken(Unit* victim, Unit* attacker, uint32 damage)
    {
        OnPackDamage(attacker, victim, damage);
        // A heavy blow may set off a unique's power (Arpg/ArpgProcs.h).
        if (attacker && attacker != victim)
            OnProcStruck(victim, attacker, damage);
        // Leech, the item affixes': every hit an ARPG player deals, whatever dealt it (a swing, a
        // spell, a tick, an added hit, thorns).
        // Only from an enemy, and no more than the health it had left.
        if (damage && attacker && victim && attacker != victim && attacker->GetTypeId() == TYPEID_PLAYER && Active(attacker))
            if (std::shared_ptr<WebTotals const> dealt = TotalsOf(attacker))
                if ((dealt->lifeLeech > 0 || dealt->manaLeech > 0) && attacker->CanAttack(victim) && victim->IsAlive())
                    Leech(static_cast<Player*>(attacker), *dealt, FromDamage(attacker, victim, std::min<uint32>(damage, victim->GetHealth())));
        // A hit taken is fighting: the rest waits.
        if (damage && victim && attacker != victim && Active(victim))
        {
            uint32 const now = WorldTimer::getMSTime();
            R(victim).combatSeen = now ? now : 1;
            // Thorns, the item affixes': back at an attacker in reach, at the next update (not
            // inside the attacker's own blow), once per attacker per update.
            if (attacker && attacker->GetTypeId() == TYPEID_UNIT && victim->IsWithinDistInMap(attacker, THORNS_REACH))
                if (std::shared_ptr<WebTotals const> totals = TotalsOf(victim))
                    if (totals->thorns > 0)
                    {
                        Runtime& r = R(victim);
                        if (std::find(r.thornsDue.begin(), r.thornsDue.end(), attacker->GetObjectGuid()) == r.thornsDue.end() &&
                                r.thornsDue.size() < 16)
                            r.thornsDue.push_back(attacker->GetObjectGuid());
                    }
        }
        if (damage && victim && attacker && Active(victim))
            OnMageStruck(victim, attacker);
        if (!damage || !victim || attacker == victim || !HasKeystone(victim, KEY_MARTYR))
            return;
        R(victim).martyrPending += damage * MARTYR_SHARE_PCT / 100;
    }

    void OnBlocked(Unit* victim)
    {
        if (victim && victim->IsAlive() && HasKeystone(victim, KEY_SHIELD_WALL))
            ProcHeal(victim, "Shield Wall", std::max<uint32>(1, victim->GetMaxHealth() / 100));
    }

    void OnCharacterKill(Player* killer, Unit* victim)
    {
        if (!victim || victim->GetTypeId() != TYPEID_UNIT)
            return;
        OnPackMemberDied(victim);
        OnMageDeath(victim);
        CreditBossKill(killer, victim);
        if (!Active(killer) || !killer->IsAlive())
            return;
        if (static_cast<Creature*>(victim)->GetCreatureInfo()->CreatureType == CREATURE_TYPE_CRITTER)
            return;
        // A kill may ready another skill (Arpg/ArpgProcs.h).
        OnProcKill(killer, victim);
        std::shared_ptr<WebTotals const> totals = TotalsOf(killer);
        WebTotals const& t = totals ? *totals : NoTotals();
        int32 const life = int32(killer->GetStat(STAT_STAMINA)) / STA_PER_LIFE + t.lifeOnKill;
        if (life > 0)
            ProcHeal(killer, "Life on kill", uint32(life));
        GiveManaPermille(killer, MANA_ON_KILL_PERMILLE);
        if (t.Has(KEY_CRUSADE))
        {
            Runtime& r = R(killer);
            r.kills.push_back(WorldTimer::getMSTime());
            while (r.kills.size() > CRUSADE_MAX)
                r.kills.pop_front();
        }
        if (t.Has(KEY_CRUSADERS_PACE))
        {
            R(killer).paceUntil = WorldTimer::getMSTime() + PACE_MS;
            killer->UpdateSpeed(MOVE_RUN, true);
            R(killer).speed = MoveSpeedMod(killer);
        }
    }

    float ExtraMeleeReach(Unit const* attacker)
    {
        return float(KeyRank(attacker, KEY_LONG_ARM));
    }

    void OnSwingHit(Unit* attacker, Unit* victim, uint32 damage, bool crit)
    {
        std::shared_ptr<WebTotals const> totals = TotalsOf(attacker);
        if (!totals || !victim)
            return;
        WebTotals const& t = *totals;
        Player* player = static_cast<Player*>(attacker);
        Runtime& r = R(attacker);
        uint32 const now = WorldTimer::getMSTime();
        // Fighting: the rest waits.
        r.combatSeen = now ? now : 1;

        // A swing that lands gives mana back.
        if (damage)
            GiveManaPermille(attacker, MANA_ON_SWING_PERMILLE, t.manaOnHit);
        // A foe this player joined late still winds up its telegraphs at them.
        if (victim->GetTypeId() == TYPEID_UNIT)
            OnTelegraphAggro(static_cast<Creature*>(victim), attacker);

        SkillId const swing = SwingSkill(player->getClass());
        if (damage && swing != SKILL_NONE)
            RecordHit(r, swing, damage, now);
        // A critical swing may ready another skill (Arpg/ArpgProcs.h).
        if (damage)
            OnProcSwing(attacker, victim, crit, damage);
        // Rune of Leech in Strike.
        if (damage && swing != SKILL_NONE && attacker->IsAlive() && HasRune(player, swing, RUNE_LEECH))
            ProcHeal(attacker, "Rune of Leech", std::max<uint32>(1, FromDamage(attacker, victim, damage) * RUNE_LEECH_PCT / 100));

        if (t.Has(KEY_MOMENTUM) && damage)
        {
            r.momentum = WorldTimer::getMSTimeDiff(r.lastHit, now) <= MOMENTUM_WINDOW ? std::min(r.momentum + 1, MOMENTUM_MAX) : 1;
            r.lastHit = now;
        }
        if (!damage)
            return;
        float const reach = ExtraMeleeReach(attacker);
        // Melee area (Strength, gear) widens the swing's added hits.
        float const area = MeleeAreaScale(attacker);
        bool const whirl = t.Has(KEY_WHIRLING_STRIKES) && ++r.swings % WHIRL_EVERY == 0;
        uint8 const wide = t.Rank(KEY_WIDE_SWING);
        if (whirl || wide)
        {
            uint32 const pct = whirl ? WHIRL_PCT : WIDE_SWING_PCT[std::min<uint8>(wide, 3) - 1];
            SpellEntry const* shown = sSpellTemplate.LookupEntry<SpellEntry>(whirl ? WHIRLWIND : CLEAVE);
            if (shown)
            {
                uint32 struck = 0;
                uint32 const share = std::max<uint32>(1, damage * pct / 100);
                float const extra = reach + (area - 1.0f) * MELEE_AREA_YARDS;
                for (Unit* unit : EnemiesNear(attacker, 8.0f + extra))
                    if (unit != victim && unit->IsAlive() && MayCatchUnit(attacker, unit, nullptr) &&
                            attacker->CanReachWithMeleeAttack(unit, extra) &&
                            (whirl || SWING_ARC * area >= 2.0f * float(M_PI) || attacker->HasInArc(unit, SWING_ARC * area)))
                    {
                        StrikeFoe(attacker, unit, shown, SPELL_SCHOOL_MASK_NORMAL, share, SwingLook(attacker));
                        ++struck;
                    }
                if (struck)
                    ReportProc(attacker, PROC_DAMAGE, 0, whirl ? "Whirling Strikes" : "Wide Swing", "also hits %u %s for %u%% (%u).",
                               struck, struck == 1 ? "enemy" : "enemies", pct, share);
            }
        }
        if (t.Has(KEY_STAGGER) && victim->IsAlive() && roll_chance_i(10 * t.Rank(KEY_STAGGER)))
        {
            attacker->CastSpell(victim, DAZED, TRIGGERED_OLD_TRIGGERED);
            ReportProc(attacker, PROC_EFFECT, 0, "Stagger", "dazes %s.", victim->GetName());
        }
        if (crit && t.Has(KEY_SHOCKWAVE_STRIKE))
            if (SpellEntry const* shown = sSpellTemplate.LookupEntry<SpellEntry>(WAR_STOMP))
            {
                uint32 const share = std::max<uint32>(1, damage * SHOCKWAVE_PCT / 100);
                std::vector<Unit*> const foes = FoesAlongLine(player, attacker->GetOrientation(), 0.0f, SHOCKWAVE_REACH * area, victim);
                for (Unit* unit : foes)
                    StrikeFoe(attacker, unit, shown, SPELL_SCHOOL_MASK_NORMAL, share, SwingLook(attacker));
                if (!foes.empty())
                    ReportProc(attacker, PROC_DAMAGE, 0, "Shockwave", "rolls out from the critical strike, hitting %u %s for %u.",
                           uint32(foes.size()), foes.size() == 1 ? "enemy" : "enemies", share);
            }
    }

    void OnSkillSpellDamage(Spell* spell, Unit* caster, Unit* victim, uint32 dealt)
    {
        std::shared_ptr<WebTotals const> totals = TotalsOf(caster);
        if (!totals || !spell || !victim)
            return;
        WebTotals const& t = *totals;
        Player* player = static_cast<Player*>(caster);
        SpellEntry const* spellInfo = spell->m_spellInfo;
        SkillId const skill = SkillOfSpell(player->getClass(), spellInfo);

        if (dealt && victim->GetTypeId() == TYPEID_UNIT)
            OnTelegraphAggro(static_cast<Creature*>(victim), caster);
        // A skill's hit gives mana back, a little, at most a few times a second.
        if (dealt)
        {
            Runtime& r = R(caster);
            uint32 const now = WorldTimer::getMSTime();
            r.combatSeen = now ? now : 1;
            if (skill != SKILL_NONE)
                RecordHit(r, uint8(skill), dealt, now);
            if (WorldTimer::getMSTimeDiff(r.lastManaHit, now) >= MANA_SPELL_EVERY)
            {
                r.lastManaHit = now;
                GiveManaPermille(caster, MANA_ON_SPELL_PERMILLE, t.manaOnHit);
            }
        }

        // Rune of Leech in the skill.
        if (dealt && skill != SKILL_NONE && caster->IsAlive() && HasRune(player, skill, RUNE_LEECH))
            ProcHeal(caster, "Rune of Leech", std::max<uint32>(1, FromDamage(caster, victim, dealt) * RUNE_LEECH_PCT / 100));

        if (IsSealHit(spellInfo))
        {
            if (t.Has(KEY_MANA_STRIKE))
                ProcMana(player, "Mana Strike", player->GetMaxPower(POWER_MANA) * t.Rank(KEY_MANA_STRIKE) / 100);
            if (t.Has(KEY_LIGHT_OF_THE_CRUSADER) && player->IsAlive())
                ProcHeal(player, "Light of the Crusader", std::max<uint32>(1, FromDamage(caster, victim, dealt) * CRUSADER_LIGHT_PCT / 100));
            if (t.Has(KEY_SACRED_SEAL) && victim->IsAlive() && Consecrated(victim, caster))
            {
                StrikeFoe(caster, victim, spellInfo, spell->GetSchoolMask(), dealt);
                ReportProcOn(caster, victim, TAG_HOLY, PROC_DAMAGE, dealt, "Sacred Seal", "strikes %s again on consecrated ground for %u.", victim->GetName(), dealt);
            }
        }
        if (skill == SKILL_JUDGEMENT)
        {
            if (t.Has(KEY_FINAL_VERDICT) && !victim->IsAlive())
                if (SpellEntry const* judgement = sSpellTemplate.LookupEntry<SpellEntry>(JUDGEMENT))
                {
                    player->RemoveSpellCooldown(*judgement, true);
                    ReportProc(player, PROC_EFFECT, 0, "Final Verdict", "readies Judgement again.");
                }
            if (t.Has(KEY_SANCTIFIED) && Consecrated(victim, caster))
            {
                uint32 const share = std::max<uint32>(1, dealt * SANCTIFIED_PCT / 100);
                std::vector<Unit*> const foes = NearestFoes(caster, victim, SANCTIFIED_RANGE, 10);
                for (Unit* unit : foes)
                    StrikeFoe(caster, unit, spellInfo, spell->GetSchoolMask(), share);
                if (!foes.empty())
                    ReportProcOn(caster, victim, TAG_HOLY, PROC_DAMAGE, share, "Sanctified", "spreads Judgement to %u %s for %u.",
                               uint32(foes.size()), foes.size() == 1 ? "enemy" : "enemies", share);
            }
        }
    }

    void OnSkillSpellLanded(Spell* spell, Unit* caster, Unit* target)
    {
        std::shared_ptr<WebTotals const> totals = TotalsOf(caster);
        if (!totals || !spell || !target || !caster->CanAttack(target))
            return;
        WebTotals const& t = *totals;
        Player* player = static_cast<Player*>(caster);
        SpellEntry const* spellInfo = spell->m_spellInfo;
        // Hammer of Justice's nodes (Hammer of Wrath shares the skill, not these).
        if (SkillOfSpell(player->getClass(), spellInfo) != SKILL_HAMMER || !spellInfo->SpellName[0] ||
                std::strncmp(spellInfo->SpellName[0], "Hammer of Justice", 17) != 0)
            return;
        float const ap = player->GetTotalAttackPowerValue(BASE_ATTACK);
        if (t.Has(KEY_HOLY_HAMMER) && target->IsAlive())
        {
            uint32 const amount = std::max<uint32>(1, ToDamage(caster, target, uint32(ap * (0.15f + 0.15f * t.Rank(KEY_HOLY_HAMMER)))));
            StrikeFoe(caster, target, spellInfo, SPELL_SCHOOL_MASK_HOLY, amount);
            ReportProc(caster, PROC_DAMAGE, 0, "Holy Hammer", "adds %u Holy to the hammer.", amount);
        }
        if (t.Has(KEY_RICOCHET))
        {
            BounceCast(player, target, spellInfo, t.Rank(KEY_RICOCHET), RICOCHET_RANGE);
            if (!NearestFoes(caster, target, RICOCHET_RANGE, 2).empty())
                ReportProc(caster, PROC_EFFECT, 0, "Ricochet", "bounces the hammer on, up to %u more %s.",
                           uint32(t.Rank(KEY_RICOCHET)), t.Rank(KEY_RICOCHET) == 1 ? "time" : "times");
        }
        if (t.Has(KEY_BLESSED_HAMMER))
        {
            uint32 const flat = std::max<uint32>(1, uint32(ap * 0.5f));
            uint32 const amount = ToDamage(caster, nullptr, flat);
            std::vector<Unit*> const foes = NearestFoes(caster, caster, BLESSED_RANGE, BLESSED_COUNT);
            for (Unit* unit : foes)
                SendBolt(player, unit, spellInfo, SPELL_SCHOOL_MASK_HOLY, ToDamage(caster, unit, flat));
            if (!foes.empty())
                ReportProc(caster, PROC_DAMAGE, 0, "Blessed Hammer", "throws %u spinning %s for %u Holy.",
                           uint32(foes.size()), foes.size() == 1 ? "hammer" : "hammers", amount);
        }
    }

    void OnJudgement(Unit* caster)
    {
        if (uint8 rank = KeyRank(caster, KEY_RIGHTEOUS_MIND))
            ProcMana(caster, "Righteous Mind", caster->GetMaxPower(POWER_MANA) * 5 * rank / 100);
    }

    float ExtraPpm(Unit const* caster, SpellEntry const* spellInfo)
    {
        if (!spellInfo || !spellInfo->SpellName[0] || std::strncmp(spellInfo->SpellName[0], "Seal of Command", 15) != 0)
            return 0.0f;
        return float(KeyRank(caster, KEY_RELENTLESS));
    }

    bool KeepsSecondSeal(Unit const* unit, SpellEntry const* adding, SpellEntry const* existing)
    {
        if (!adding || !existing || !IsSealSpell(adding) || !IsSealSpell(existing) || !HasKeystone(unit, KEY_TWIN_SEALS))
            return false;
        uint32 const chain = sSpellMgr.GetFirstSpellInChain(adding->Id);
        if (sSpellMgr.GetFirstSpellInChain(existing->Id) == chain)
            return false;
        // Keep it if it is the only other Seal on.
        std::set<uint32> others;
        for (auto const& [id, holder] : unit->GetSpellAuraHolderMap())
        {
            SpellEntry const* proto = holder->GetSpellProto();
            if (holder->GetCasterGuid() == unit->GetObjectGuid() && IsSealSpell(proto) &&
                    sSpellMgr.GetFirstSpellInChain(proto->Id) != chain)
                others.insert(sSpellMgr.GetFirstSpellInChain(proto->Id));
        }
        return others.size() < 2;
    }
}
