/*
 * ARPG actions: the dodge roll and the health flask, for the benilla ARPG client.
 *
 * Dodge (CMSG_ARPG_ACTION kind 17, float x, y: a WoW-space point to roll toward): a low leap of
 * about seven yards, sent to the client as a knockback (SMSG_MOVE_KNOCK_BACK, which the client
 * flies and the anticheat expects), every DODGE_COOLDOWN_MS at most. While airborne
 * (DODGE_EVADE_MS) melee blows and hostile spells aimed at the player are dodged. Not while
 * rooted, stunned, feared, confused, mounted, on a taxi, swimming or falling.
 *
 * Flask (kind 18): a charge heals 15% of maximum health at once and 25% more over three seconds.
 * It holds FLASK_MAX charges and starts full. Kills fill it: a pack follower 1 point, a mob 2,
 * an elite 4, a champion 5, a rare 10, a boss 20; FLASK_CHARGE_POINTS points make a charge. Out
 * of combat it refills a charge every FLASK_REST_MS.
 *
 *   SMSG_ARPG_STATUS (0x342): uint8 flask charges, uint8 flask max, uint8 the next charge's
 *     progress (0-100), uint32 ms until the dodge is ready (0 ready), uint32 the dodge cooldown
 *     ms. Sent on a change, at most every STATUS_EVERY_MS, and once after the hello.
 */

#ifndef MANGOS_ARPG_ACTIONS_H
#define MANGOS_ARPG_ACTIONS_H

#include "Common.h"

class Player;
class Unit;

namespace Arpg
{
    // Roll toward the WoW-space point (x, y).
    void Dodge(Player* player, float x, float y);

    // Whether `unit` is mid-roll: blows and hostile spells at it are dodged.
    bool Dodging(Unit const* unit);

    // Drink a charge of the flask.
    void DrinkFlask(Player* player);

    // `killer` killed `victim`: its flask fills.
    void OnFlaskKill(Player* killer, Unit* victim);

    // Each player update: the out-of-combat refill and the status packet.
    void UpdateActions(Player* player);

    // A logged-out player's flask and roll.
    void ForgetActions(Player* player);
}

#endif
