/*
 * ARPG perks (Arpg/ArpgPerks.h).
 */

#include "Arpg/ArpgPerks.h"
#include "Arpg/ArpgSkills.h"
#include "Arpg/ArpgTree.h"

#include "Chat/Chat.h"
#include "Database/DatabaseEnv.h"
#include "Entities/Player.h"
#include "Log/Log.h"

#include <mutex>
#include <set>
#include <unordered_map>

namespace
{
    using namespace Arpg;

    enum PerkKind : uint8 { PK_SKILL_POINT, PK_WEB_POINT, PK_GOLD_PCT, PK_ALL_RESIST };

    struct PerkDef
    {
        PerkKind kind;
        int32 value;
        char const* from;           // where it was earned
        char const* text;           // what it gives
    };

    PerkDef const PERKS[MAX_PERK] =
    {
        { PK_SKILL_POINT, 0, "", "" },
        { PK_SKILL_POINT, 1, "Echo Ridge Diggings", "a skill point" },
        { PK_WEB_POINT, 1, "Wolf Run", "a passive point" },
        { PK_SKILL_POINT, 1, "Fargodeep Pit", "a skill point" },
        { PK_GOLD_PCT, 10, "The Defias Hideout", "10% more gold found" },
        { PK_ALL_RESIST, 15, "Jasperlode Hollow", "15 to every resistance" },
        { PK_WEB_POINT, 1, "Riverpaw Warren", "a passive point" },
    };

    std::mutex sLock;
    std::unordered_map<ObjectGuid, std::set<uint8>> sHeld;

    std::set<uint8> HeldBy(Player const* player)
    {
        std::lock_guard<std::mutex> guard(sLock);
        auto it = sHeld.find(player->GetObjectGuid());
        return it == sHeld.end() ? std::set<uint8>() : it->second;
    }

    int32 Sum(Player const* player, PerkKind kind)
    {
        int32 total = 0;
        for (uint8 id : HeldBy(player))
            if (id > PERK_NONE && id < MAX_PERK && PERKS[id].kind == kind)
                total += PERKS[id].value;
        return total;
    }

    void EnsureTable()
    {
        static bool const created = []()
        {
            CharacterDatabase.DirectExecute(
                "CREATE TABLE IF NOT EXISTS character_arpg_perks ("
                "guid INT UNSIGNED NOT NULL, perk TINYINT UNSIGNED NOT NULL, "
                "PRIMARY KEY (guid, perk)) ENGINE=InnoDB DEFAULT CHARSET=utf8 COMMENT='ARPG perks (Arpg/ArpgPerks.h)'");
            return true;
        }();
        (void)created;
    }
}

namespace Arpg
{
    void LoadPerks(Player* player)
    {
        if (!player)
            return;
        EnsureTable();
        std::set<uint8> held;
        if (auto result = CharacterDatabase.PQuery("SELECT perk FROM character_arpg_perks WHERE guid = %u", player->GetGUIDLow()))
        {
            do
                held.insert(uint8(result->Fetch()[0].GetUInt32()));
            while (result->NextRow());
        }
        std::lock_guard<std::mutex> guard(sLock);
        sHeld[player->GetObjectGuid()] = std::move(held);
    }

    void ForgetPerks(Player* player)
    {
        if (!player)
            return;
        std::lock_guard<std::mutex> guard(sLock);
        sHeld.erase(player->GetObjectGuid());
    }

    bool HasPerk(Player const* player, PerkId perk)
    {
        return player && HeldBy(player).count(perk);
    }

    bool GrantPerk(Player* player, PerkId perk)
    {
        if (!player || perk <= PERK_NONE || perk >= MAX_PERK)
            return false;
        {
            std::lock_guard<std::mutex> guard(sLock);
            if (!sHeld[player->GetObjectGuid()].insert(perk).second)
                return false;
        }
        EnsureTable();
        CharacterDatabase.PExecute("REPLACE INTO character_arpg_perks (guid, perk) VALUES (%u, %u)", player->GetGUIDLow(), uint32(perk));
        PerkDef const& def = PERKS[perk];
        ChatHandler(player).PSendSysMessage("|cffffd100%s cleared:|r you gain %s, for good.", def.from, def.text);
        sLog.outString("ARPG perks: %s earns %s (%s)", player->GetName(), def.text, def.from);
        switch (def.kind)
        {
            case PK_SKILL_POINT: SendSkills(player); break;
            case PK_WEB_POINT: SendTree(player); break;
            default: RefreshTotals(player); break;
        }
        return true;
    }

    uint32 PerkSkillPoints(Player const* player)
    {
        return player ? uint32(Sum(player, PK_SKILL_POINT)) : 0;
    }

    uint32 PerkWebPoints(Player const* player)
    {
        return player ? uint32(Sum(player, PK_WEB_POINT)) : 0;
    }

    void AddPerkTotals(Player const* player, WebTotals& totals)
    {
        if (!player)
            return;
        totals.goldFindPct += Sum(player, PK_GOLD_PCT);
        int32 const resist = Sum(player, PK_ALL_RESIST);
        for (int32& r : totals.resist)
            r += resist;
    }

    std::vector<std::string> PerkSheetLines(Player const* player)
    {
        std::vector<std::string> lines;
        if (!player)
            return lines;
        for (uint8 id : HeldBy(player))
            if (id > PERK_NONE && id < MAX_PERK)
                lines.push_back(std::string("Perk: ") + PERKS[id].text + " (" + PERKS[id].from + ")");
        return lines;
    }
}
