/*
 * ARPG Codex pages, fragments and runes as loot: see ArpgCodex.h.
 */

#include "Arpg/ArpgCodex.h"
#include "Arpg/ArpgCombat.h"
#include "Arpg/ArpgDungeons.h"
#include "Arpg/ArpgPacks.h"
#include "Arpg/ArpgSkills.h"

#include "Chat/Chat.h"
#include "Database/DatabaseEnv.h"
#include "Entities/Creature.h"
#include "Entities/Player.h"
#include "Log/Log.h"
#include "Loot/LootMgr.h"
#include "World/World.h"

#include <string>
#include <vector>

namespace
{
    using namespace Arpg;

    // Icons: the paladin's tome, a torn scroll, and runestones and sigils for the runes.
    constexpr uint32 DISPLAY_PAGE = 13005, DISPLAY_FRAGMENT = 2616;
    uint32 const DISPLAY_RUNE[] = { 7217, 7218, 7026, 7189, 7106, 22443, 3669, 7246, 7247 };

    constexpr uint32 PAGE_PCT = 35, PAGE_TIER_PCT = 7;

    // --- The Core Sigil: Molten Core's key ---

    struct KeyPiece
    {
        uint32 item;
        uint16 thing;         // held as (Arpg::Held)
        uint32 boss;
        char const* name;
        char const* bossName;
        uint32 display;
    };
    // Each piece drops from its boss on Brutal or harder, one per ARPG player there.
    constexpr uint8 KEY_MIN_TIER = 2;
    KeyPiece const CORE_PIECES[] =
    {
        { 91001, 101, 9019,  "Ember of Thaurissan", "Emperor Dagran Thaurissan", 12736 },
        { 91002, 102, 9568,  "Spire Brand", "Overlord Wyrmthalak", 7244 },
        { 91003, 103, 10363, "Drakkisath's Seal", "General Drakkisath", 7366 },
    };
    constexpr uint32 CORE_SIGIL = 91010;
    constexpr uint16 CORE_SIGIL_THING = 110;
    constexpr uint32 CORE_SIGIL_DISPLAY = 11766;
    constexpr uint32 MOLTEN_CORE_MAP = 409;
    constexpr float TIER_SHARE = 0.25f;     // each tier adds a quarter to the other chances

    std::string Escaped(char const* text)
    {
        std::string out;
        for (char const* c = text; *c; ++c)
        {
            if (*c == '\'' || *c == '\\')
                out += '\\';
            out += *c;
        }
        return out;
    }

    void WriteItem(uint32 entry, std::string const& name, uint32 display, uint32 quality, uint32 stack,
                   std::string const& description)
    {
        // class 15 (miscellaneous), subclass 0; every other column keeps its default.
        WorldDatabase.DirectPExecute(
            "REPLACE INTO item_template (entry, class, subclass, name, displayid, Quality, stackable, description, ItemLevel, SellPrice) "
            "VALUES (%u, 15, 0, '%s', %u, %u, %u, '%s', 1, 0)",
            entry, Escaped(name.c_str()).c_str(), display, quality, stack, Escaped(description.c_str()).c_str());
    }

    bool Roll(float pct)
    {
        return roll_chance_f(pct);
    }

    void AddToLoot(Loot* loot, uint32 item)
    {
        loot->AddItem(item, 1, 0, 0);
    }

    uint32 RandomRune()
    {
        std::vector<uint8> const runes = AllRunes();
        return runes.empty() ? 0 : CODEX_RUNE_BASE + runes[urand(0, uint32(runes.size() - 1))];
    }
}

namespace Arpg
{
    void EnsureCodexItems()
    {
        if (!sWorld.getConfig(CONFIG_BOOL_ARPG_ENABLE))
            return;
        // Direct, so they are in before the templates load.
        for (uint16 node : AllPages())
            WriteItem(CODEX_PAGE_BASE + node, std::string("Codex: ") + PageName(node), DISPLAY_PAGE, 4, 1,
                      "Read when picked up: unseals its capstone in your Skills window.");
        for (KeyPiece const& piece : CORE_PIECES)
            WriteItem(piece.item, piece.name, piece.display, 4, 1,
                      std::string("A piece of the Core Sigil, Molten Core's key. Dropped by ") + piece.bossName + " on Brutal or harder.");
        WriteItem(CORE_SIGIL, "Core Sigil", CORE_SIGIL_DISPLAY, 4, 1,
                  "Molten Core's key: the Ember of Thaurissan, the Spire Brand and Drakkisath's Seal, forged into one.");
        WriteItem(CODEX_FRAGMENT, "Codex Fragment", DISPLAY_FRAGMENT, 2, 20,
                  "Five unseal any capstone of a skill you have specialised.");
        std::vector<uint8> const runes = AllRunes();
        for (size_t i = 0; i < runes.size(); ++i)
            WriteItem(CODEX_RUNE_BASE + runes[i], RuneName(runes[i]), DISPLAY_RUNE[i % std::size(DISPLAY_RUNE)], 3, 1,
                      std::string(RuneText(runes[i])) + " Socket it in a skill with 10 points.");
        sLog.outString("ARPG Codex: wrote %u Codex items into item_template", uint32(AllPages().size() + 1 + runes.size()));
    }

    bool TakeCodexItem(Player* player, uint32 item, uint32 count)
    {
        // Pages 90101-90999, fragments 90001, runes 90011-90089, raid key pieces 91001 up.
        if (item < CODEX_PAGE_BASE || item > CODEX_PAGE_BASE + 1999 || !Active(player))
            return false;
        for (KeyPiece const& piece : CORE_PIECES)
        {
            if (item != piece.item)
                continue;
            ChatHandler chat(player);
            if (Held(player, CORE_SIGIL_THING) || Held(player, piece.thing))
            {
                // Left on the ground for one who needs it (the store refuses it into the bags).
                chat.PSendSysMessage("|cffa335eeYou already hold the %s; leave this one for another.|r",
                                     Held(player, CORE_SIGIL_THING) ? "Core Sigil" : piece.name);
                return true;
            }
            AddHeld(player, piece.thing, 1);
            uint32 have = 0;
            for (KeyPiece const& p : CORE_PIECES)
                if (Held(player, p.thing))
                    ++have;
            if (have < std::size(CORE_PIECES))
            {
                chat.PSendSysMessage("|cffa335eeYou take the %s: %u of %u pieces of the Core Sigil.|r",
                                     piece.name, have, uint32(std::size(CORE_PIECES)));
                return true;
            }
            for (KeyPiece const& p : CORE_PIECES)
                AddHeld(player, p.thing, -1);
            AddHeld(player, CORE_SIGIL_THING, 1);
            chat.PSendSysMessage("|cffff8000The three pieces fuse into the Core Sigil. Molten Core is open to you.|r");
            return true;
        }
        if (item == CODEX_FRAGMENT)
        {
            AddFragments(player, std::max<uint32>(count, 1));
            return true;
        }
        if (item > CODEX_RUNE_BASE && item < CODEX_RUNE_BASE + 90)
        {
            for (uint32 i = 0; i < std::max<uint32>(count, 1); ++i)
                AddRune(player, uint8(item - CODEX_RUNE_BASE));
            return true;
        }
        uint16 const node = uint16(item - CODEX_PAGE_BASE);
        for (uint16 page : AllPages())
            if (page == node)
            {
                ReadPage(player, node);
                return true;
            }
        return false;
    }

    void OnCodexLoot(Creature* victim, bool warden, bool cache, uint8 tier)
    {
        if (!sWorld.getConfig(CONFIG_BOOL_ARPG_ENABLE) || !victim || !victim->m_loot)
            return;
        // Only for an ARPG looter: to anyone else they'd be dead weight in the bags.
        Player* looter = victim->GetLootRecipient();
        if (!looter || !Active(looter))
            return;
        Loot* loot = victim->m_loot;
        float const more = 1.0f + TIER_SHARE * float(tier);
        bool const dungeon = victim->GetMap()->IsDungeon();

        // The Core Sigil's pieces, from Brutal: one each for the ARPG players there.
        for (KeyPiece const& piece : CORE_PIECES)
        {
            if (!dungeon || victim->GetEntry() != piece.boss)
                continue;
            if (tier < KEY_MIN_TIER)
            {
                for (auto const& ref : victim->GetMap()->GetPlayers())
                    if (Player* player = ref.getSource())
                        if (Active(player) && !Held(player, CORE_SIGIL_THING))
                            ChatHandler(player).PSendSysMessage("|cffa335eeOn Brutal or harder, %s would drop the %s, a piece of the Core Sigil.|r",
                                                                piece.bossName, piece.name);
                continue;
            }
            // One for each ARPG player there who still needs it.
            for (auto const& ref : victim->GetMap()->GetPlayers())
                if (Player* player = ref.getSource())
                    if (Active(player) && !Held(player, CORE_SIGIL_THING) && !Held(player, piece.thing))
                        AddToLoot(loot, piece.item);
        }

        if (dungeon)
            for (uint16 node : PagesFrom(victim->GetEntry()))
                if (Roll(float(PAGE_PCT + PAGE_TIER_PCT * tier)))
                    AddToLoot(loot, CODEX_PAGE_BASE + node);

        float fragment = 0.0f, rune = 0.0f;
        uint32 sureFragments = 0;
        if (warden)
        {
            sureFragments = 2;
            rune = 30.0f;
        }
        else if (cache)
        {
            sureFragments = 1;
            fragment = 50.0f;
            rune = 40.0f;
        }
        else if (dungeon && IsDungeonBoss(victim))
        {
            fragment = 25.0f;
            rune = 12.0f;
        }
        else
        {
            switch (ChampionTier(victim))
            {
                case 2: fragment = 40.0f; rune = 8.0f; break;
                case 1: fragment = 12.0f; rune = 2.0f; break;
                default: return;
            }
        }
        for (uint32 i = 0; i < sureFragments; ++i)
            AddToLoot(loot, CODEX_FRAGMENT);
        if (fragment > 0.0f && Roll(std::min(100.0f, fragment * more)))
            AddToLoot(loot, CODEX_FRAGMENT);
        if (rune > 0.0f && Roll(std::min(100.0f, rune * more)))
            if (uint32 const item = RandomRune())
                AddToLoot(loot, item);
    }

    bool IsAttuned(Player const* player, uint32 mapId)
    {
        if (mapId != MOLTEN_CORE_MAP)
            return true;
        return Held(player, CORE_SIGIL_THING) > 0;
    }

    uint32 AttunementItem(uint32 mapId)
    {
        return mapId == MOLTEN_CORE_MAP ? CORE_SIGIL : 0;
    }

    bool CodexRefuses(Player* player, uint32 item)
    {
        if (!Active(player))
            return false;
        for (KeyPiece const& piece : CORE_PIECES)
            if (item == piece.item && (Held(player, CORE_SIGIL_THING) || Held(player, piece.thing)))
            {
                ChatHandler(player).PSendSysMessage("|cffa335eeYou already hold the %s; it stays for another.|r",
                                                    Held(player, CORE_SIGIL_THING) ? "Core Sigil" : piece.name);
                return true;
            }
        return false;
    }
}
