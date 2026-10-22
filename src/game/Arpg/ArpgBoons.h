/*
 * ARPG boons: Barrier and the timed effects on a player (the client's docs/ARPG-CHARACTER.md,
 * "Barrier and boons").
 *
 * Barrier (Diablo IV's): a pool that takes damage before health. Each gain adds to the pool, up
 * to BARRIER_CAP_PCT of maximum health, and holds it BARRIER_MS from the latest gain; then the
 * pool is gone. It is spent by every blow, of any school, before resistances' rolls are shown,
 * as an absorb in the combat log (Unit::CalculateDamageAbsorbAndResist).
 *
 * Boons: named, timed effects the HUD lists beside the health orb with their time left (a
 * shrine's, Arpg/ArpgShrines.h) and the standing ones of the place the player is in (a nightmare
 * dungeon's affixes, shown without a timer).
 *
 *   SMSG_ARPG_BOONS (0x355), when anything changes: uint32 barrier, uint32 maximum health,
 *     uint8 n, then per boon: uint8 id, uint8 kind (0 timed, 1 standing), uint32 ms left,
 *     uint32 ms in all, cstring name.
 */

#ifndef MANGOS_ARPG_BOONS_H
#define MANGOS_ARPG_BOONS_H

#include "Common.h"

#include <string>
#include <vector>

class Player;
class Unit;

namespace Arpg
{
    constexpr uint32 BARRIER_MS = 6000;
    constexpr uint32 BARRIER_CAP_PCT = 50;

    // `player` gains `amount` Barrier (capped, its time renewed).
    void GainBarrier(Player* player, uint32 amount);
    uint32 BarrierOf(Player const* player);
    // The share of `damage` to `victim` its Barrier takes (and spends).
    uint32 BarrierAbsorb(Unit* victim, uint32 damage);

    // The timed effects (BOON_*), by id: one of each at a time, a new one renewing its time.
    enum BoonId : uint8
    {
        BOON_NONE = 0,
        BOON_SHRINE_FIRST = 1,      // the shrines' (Arpg/ArpgShrines.h), 1-15
        BOON_STANDING_FIRST = 100,  // standing boons (a nightmare dungeon's affixes)
    };
    void AddBoon(Player* player, uint8 id, std::string const& name, uint32 ms);
    void RemoveBoon(Player* player, uint8 id);
    bool HasBoon(Player const* player, uint8 id);
    // The standing boons (no timer), replacing those there were; empty clears them.
    void SetStandingBoons(Player* player, std::vector<std::pair<uint8, std::string>> const& boons);

    // Every player update: expiry, and the HUD's message when anything changed.
    void UpdateBoons(Player* player);
    void ForgetBoons(Player* player);
}

#endif
