/*
 * ARPG Codex pages, fragments and runes as loot, for the benilla ARPG client (the client's
 * docs/ARPG-CHARACTER.md, "Drops that unlock skills"; the skills side is Arpg/ArpgSkills.h).
 *
 * They are items so they can lie on the ground, show a label and a tooltip: the server writes
 * their item_template rows itself at start, before the item templates load (EnsureCodexItems),
 * so no SQL needs applying. Entries:
 *   CODEX_PAGE_BASE + capstone node   a Codex page (epic, the paladin's tome)
 *   CODEX_FRAGMENT                    a Codex fragment (green, stacks)
 *   CODEX_RUNE_BASE + rune id         a rune (blue)
 * An ARPG player picking one up keeps it in the character's Codex, not the bags (Loot::SendItem
 * asks TakeCodexItem): a page is read at once, fragments and runes are held.
 *
 * Drops (a tier adds a quarter to every chance, and the page chance 7 points):
 *   a capstone's home boss   its page, 35%
 *   champion                 a fragment 12%, a rune 2%
 *   rare                     a fragment 40%, a rune 8%
 *   other dungeon boss       a fragment 25%, a rune 12%
 *   Warden                   two fragments, a rune 30%
 *   Cache                    a fragment and a 50% chance at another, a rune 40%
 */

#ifndef MANGOS_ARPG_CODEX_H
#define MANGOS_ARPG_CODEX_H

#include "Common.h"

class Creature;
class Player;

namespace Arpg
{
    constexpr uint32 CODEX_FRAGMENT = 90001;
    constexpr uint32 CODEX_RUNE_BASE = 90010;
    constexpr uint32 CODEX_PAGE_BASE = 90000;     // + the capstone's node (101 to 999)

    // Write the Codex items into item_template; before the item templates load.
    void EnsureCodexItems();

    // Whether `item` is a Codex item `player` keeps in the Codex (then it is taken).
    bool TakeCodexItem(Player* player, uint32 item, uint32 count);

    // `victim`'s corpse loot was just made: its Codex drops. `warden` and `cache` from the
    // dungeon module; `tier` the dungeon's difficulty tier.
    void OnCodexLoot(Creature* victim, bool warden, bool cache, uint8 tier);
}

#endif
