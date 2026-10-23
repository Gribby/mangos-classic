/*
 * ARPG perks: lasting rewards, each given once per character, as Diablo II's quest rewards (the
 * Den of Evil's skill point, Radament's book). A side area's boss gives one the first time it
 * falls (Arpg/ArpgRift.h); a perk is a skill point, a passive-web point, or a bonus in the
 * character's totals, listed on the sheet with where it came from.
 */

#ifndef MANGOS_ARPG_PERKS_H
#define MANGOS_ARPG_PERKS_H

#include "Common.h"

#include <string>
#include <vector>

class Player;

namespace Arpg
{
    struct WebTotals;

    enum PerkId : uint8
    {
        PERK_NONE = 0,
        PERK_ECHO_RIDGE,        // a skill point
        PERK_WOLF_RUN,          // a passive-web point
        PERK_FARGODEEP,         // a skill point
        PERK_HIDEOUT,           // 10% more gold found
        PERK_JASPERLODE,        // 15 to every resistance
        PERK_WARREN,            // a passive-web point
        MAX_PERK
    };

    // At login, with the web (Arpg/ArpgTree.cpp's LoadTree).
    void LoadPerks(Player* player);
    void ForgetPerks(Player* player);

    bool HasPerk(Player const* player, PerkId perk);
    // Gives `perk` once: saved, told, and the points and totals it touches sent again. False
    // when the character had it.
    bool GrantPerk(Player* player, PerkId perk);

    // What the perks add: skill points, web points, and the totals' bonuses.
    uint32 PerkSkillPoints(Player const* player);
    uint32 PerkWebPoints(Player const* player);
    void AddPerkTotals(Player const* player, WebTotals& totals);

    // The sheet's lines, one a perk held.
    std::vector<std::string> PerkSheetLines(Player const* player);
}

#endif
