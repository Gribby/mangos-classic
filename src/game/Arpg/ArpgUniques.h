/*
 * ARPG uniques, for the benilla ARPG client (see ArpgCombat.h and the client's
 * docs/ARPG-UNIQUES.md).
 *
 * Named vanilla items keep their stats and gain one interaction with a specific spell, for an
 * ARPG player wearing them. The table lives here (Uniques()): one row per item, naming its kit
 * (the engine piece), the spell it changes (rank 1; any rank matches), its numbers and the line
 * its tooltip shows. Mechanics read what the player wears when they fire, so equipping or
 * removing an item needs no bookkeeping.
 *
 * Built so far:
 *   KIT_EXTRA_PROJECTILES (A)  A line skillshot fires `n` more lines, fanned `value` degrees
 *                              apart around the aim, each a free cast of the same spell dealing
 *                              `pct` percent (Spell::SetArpgSecondary).
 *   KIT_FRAGMENTS (D)          On hit, `n` enemies within `value` yards behind the target, in a
 *                              90 degree cone away from the caster, take `pct` percent of the hit.
 * If two worn items change the same spell with the same kit, the one with the larger `n` applies.
 *
 *   SMSG_ARPG_ITEM_MECHANICS: uint8 count, then per row: uint32 item id, cstring tooltip line.
 *   Sent in answer to every hello, so the client can show the lines.
 */

#ifndef MANGOS_ARPG_UNIQUES_H
#define MANGOS_ARPG_UNIQUES_H

#include "Common.h"

#include <vector>

class Player;
class Spell;
class Unit;
struct SpellEntry;

namespace Arpg
{
    enum UniqueKit : uint8
    {
        KIT_EXTRA_PROJECTILES = 'A',
        KIT_FRAGMENTS         = 'D',
    };

    struct UniqueMechanic
    {
        uint32 item;
        UniqueKit kit;
        uint32 spell;     // rank 1 of the spell it changes
        uint8 n;          // projectiles or fragments
        float value;      // A: degrees between lines; D: reach behind the target, in yards
        uint32 pct;       // each added hit's share of the main hit, in percent
        char const* text; // the tooltip line
    };

    // Every unique, in table order.
    std::vector<UniqueMechanic> const& Uniques();

    // The mechanic of `kit` that `player`'s worn items give `spellInfo`, if any.
    UniqueMechanic const* WornMechanic(Player const* player, UniqueKit kit, SpellEntry const* spellInfo);

    // Send `player` the uniques' tooltip lines (SMSG_ARPG_ITEM_MECHANICS).
    void SendUniques(Player* player);

    // A skillshot just launched along its line, at `struck` (nullptr: into the empty air): launch
    // the extra projectiles its caster's uniques add. Added projectiles add none.
    void OnLineLaunch(Spell* spell, Unit* struck);

    // `spell` hit `victim` for `dealt` (after mitigation, absorbs included): apply the caster's
    // on-hit uniques (fragments). Added hits set off none.
    void OnSpellDamage(Spell* spell, Unit* caster, Unit* victim, uint32 dealt);
}

#endif
