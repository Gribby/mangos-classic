/*
 * ARPG actions: the dodge roll and the health flask. See ArpgActions.h.
 */

#include "Arpg/ArpgActions.h"
#include "Arpg/ArpgCombat.h"
#include "Arpg/ArpgDungeons.h"
#include "Arpg/ArpgPacks.h"

#include "Entities/Creature.h"
#include "Entities/Player.h"
#include "Server/Opcodes.h"
#include "Server/WorldPacket.h"
#include "Server/WorldSession.h"
#include "Spells/SpellMgr.h"
#include "Util/Timer.h"
#include "World/World.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>

namespace
{
    using namespace Arpg;

    constexpr uint32 DODGE_COOLDOWN_MS = 2500;
    constexpr uint32 DODGE_EVADE_MS = 450;
    // Knockback speeds: about 0.42 s in the air (2 * 4 / 19.29) at 17 yd/s, some seven yards.
    constexpr float DODGE_HORIZONTAL = 17.0f, DODGE_VERTICAL = 4.0f;

    constexpr uint8 FLASK_MAX = 3;
    constexpr uint32 FLASK_CHARGE_POINTS = 10;
    constexpr uint32 FLASK_INSTANT_PCT = 15, FLASK_OVER_TIME_PCT = 25;
    constexpr uint32 FLASK_TICKS = 6, FLASK_TICK_MS = 500;
    constexpr uint32 FLASK_REST_MS = 12000;
    // Healing Potion: its cast plays the drink's sound and visual, and names the heal in the log.
    constexpr uint32 SPELL_HEALING_POTION = 441;

    constexpr uint32 STATUS_EVERY_MS = 200;

    struct State
    {
        uint8 charges = FLASK_MAX;
        uint32 points = 0;
        uint32 restSince = 0;       // when the player was last seen in combat
        uint32 dodgeReady = 0;      // the time the dodge is ready again
        uint32 evadeUntil = 0;      // the roll's dodge window ends
        bool dirty = true;
        uint32 lastSent = 0;
    };

    std::mutex sActionsLock;
    std::unordered_map<ObjectGuid, State> sStates;

    bool Later(uint32 a, uint32 b)
    {
        // a is after b on the wrapping millisecond clock; 0 is "never set", never later.
        return a != 0 && int32(a - b) > 0;
    }

    // A time `ms` from `now`, never 0 (which means unset).
    uint32 After(uint32 now, uint32 ms)
    {
        uint32 const at = now + ms;
        return at ? at : 1;
    }

    void SendStatus(Player* player, State const& s, uint32 now)
    {
        WorldPacket data(SMSG_ARPG_STATUS, 3 + 4 + 4);
        data << uint8(s.charges);
        data << uint8(FLASK_MAX);
        data << uint8(s.charges >= FLASK_MAX ? 0 : std::min<uint32>(100, s.points * 100 / FLASK_CHARGE_POINTS));
        data << uint32(Later(s.dodgeReady, now) ? s.dodgeReady - now : 0);
        data << uint32(DODGE_COOLDOWN_MS);
        player->GetSession()->SendPacket(data);
    }

    uint32 KillPoints(Unit* victim)
    {
        if (victim->GetTypeId() != TYPEID_UNIT)
            return 0;
        Creature* creature = static_cast<Creature*>(victim);
        if (IsDungeonBoss(creature))
            return 20;
        switch (ChampionTier(creature))
        {
            case 2: return 10;
            case 1: return 5;
            default: break;
        }
        if (IsPackFollower(creature))
            return 1;
        return creature->IsElite() ? 4 : 2;
    }

    void AddPoints(State& s, uint32 points)
    {
        if (s.charges >= FLASK_MAX || !points)
            return;
        s.points += points;
        while (s.points >= FLASK_CHARGE_POINTS && s.charges < FLASK_MAX)
        {
            s.points -= FLASK_CHARGE_POINTS;
            ++s.charges;
        }
        if (s.charges >= FLASK_MAX)
            s.points = 0;
        s.dirty = true;
    }
}

namespace Arpg
{
    void Dodge(Player* player, float x, float y)
    {
        if (!Active(player) || !player->IsAlive() || !player->IsInWorld() || !std::isfinite(x) || !std::isfinite(y))
            return;
        if (player->hasUnitState(UNIT_STAT_NO_FREE_MOVE) || player->IsMounted() || player->IsTaxiFlying() ||
                player->IsInWater() || player->IsFalling())
            return;
        uint32 const now = WorldTimer::getMSTime();
        {
            std::lock_guard<std::mutex> guard(sActionsLock);
            State& s = sStates[player->GetObjectGuid()];
            if (Later(s.dodgeReady, now))
                return;
            s.dodgeReady = After(now, DODGE_COOLDOWN_MS);
            s.evadeUntil = After(now, DODGE_EVADE_MS);
            s.dirty = true;
        }
        float const dx = x - player->GetPositionX(), dy = y - player->GetPositionY();
        float const angle = (dx * dx + dy * dy) < 0.25f ? player->GetOrientation() : std::atan2(dy, dx);
        player->KnockBackWithAngle(angle, DODGE_HORIZONTAL, DODGE_VERTICAL);
    }

    bool Dodging(Unit const* unit)
    {
        if (!unit || unit->GetTypeId() != TYPEID_PLAYER)
            return false;
        std::lock_guard<std::mutex> guard(sActionsLock);
        auto it = sStates.find(unit->GetObjectGuid());
        return it != sStates.end() && Later(it->second.evadeUntil, WorldTimer::getMSTime());
    }

    void DrinkFlask(Player* player)
    {
        if (!Active(player) || !player->IsAlive() || !player->IsInWorld())
            return;
        {
            std::lock_guard<std::mutex> guard(sActionsLock);
            State& s = sStates[player->GetObjectGuid()];
            if (s.charges == 0)
                return;
            --s.charges;
            s.dirty = true;
        }
        uint32 const max = player->GetMaxHealth();
        int32 const instant = int32(std::max<uint32>(1, max * FLASK_INSTANT_PCT / 100));
        player->CastCustomSpell(player, SPELL_HEALING_POTION, &instant, nullptr, nullptr, TRIGGERED_OLD_TRIGGERED);
        uint32 const tick = std::max<uint32>(1, max * FLASK_OVER_TIME_PCT / 100 / FLASK_TICKS);
        for (uint32 i = 1; i <= FLASK_TICKS; ++i)
        {
            player->m_events.AddEvent(new UnitLambdaEvent(*player, [tick](Unit& unit)
            {
                if (!unit.IsAlive() || !unit.IsInWorld())
                    return;
                if (SpellEntry const* potion = sSpellTemplate.LookupEntry<SpellEntry>(SPELL_HEALING_POTION))
                    unit.DealHeal(&unit, tick, potion);
            }), player->m_events.CalculateTime(i * FLASK_TICK_MS));
        }
    }

    void OnFlaskKill(Player* killer, Unit* victim)
    {
        if (!killer || !victim || !Active(killer))
            return;
        uint32 const points = KillPoints(victim);
        std::lock_guard<std::mutex> guard(sActionsLock);
        AddPoints(sStates[killer->GetObjectGuid()], points);
    }

    void UpdateActions(Player* player)
    {
        if (!Active(player))
            return;
        uint32 const now = WorldTimer::getMSTime();
        State copy;
        {
            std::lock_guard<std::mutex> guard(sActionsLock);
            State& s = sStates[player->GetObjectGuid()];
            if (player->IsInCombat() || !s.restSince)
                s.restSince = now;
            else if (s.charges < FLASK_MAX && WorldTimer::getMSTimeDiff(s.restSince, now) >= FLASK_REST_MS)
            {
                ++s.charges;
                s.restSince = now;
                if (s.charges >= FLASK_MAX)
                    s.points = 0;
                s.dirty = true;
            }
            // The dodge coming ready is a change too, so the client's cooldown ends on time.
            if (s.dodgeReady && !Later(s.dodgeReady, now))
            {
                s.dodgeReady = 0;
                s.dirty = true;
            }
            if (!s.dirty || WorldTimer::getMSTimeDiff(s.lastSent, now) < STATUS_EVERY_MS)
                return;
            s.dirty = false;
            s.lastSent = now;
            copy = s;
        }
        SendStatus(player, copy, now);
    }

    void ForgetActions(Player* player)
    {
        if (!player)
            return;
        std::lock_guard<std::mutex> guard(sActionsLock);
        sStates.erase(player->GetObjectGuid());
    }
}
