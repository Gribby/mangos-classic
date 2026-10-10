/*
 * ARPG actions: the dodge roll and the health flask. See ArpgActions.h.
 */

#include "Arpg/ArpgActions.h"
#include "Arpg/ArpgCombat.h"
#include "Arpg/ArpgDungeons.h"
#include "Arpg/ArpgPacks.h"

#include "Chat/Chat.h"
#include "Entities/Creature.h"
#include "Globals/ObjectMgr.h"
#include "Maps/Map.h"
#include "Maps/MapPersistentStateMgr.h"
#include "Server/DBCStores.h"
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

    // The town portal: the cast, how far the player may drift in it, and how long the way back
    // stays open; "in town" is resting or this near the bind point.
    constexpr uint32 PORTAL_CAST_MS = 2000;
    constexpr float PORTAL_DRIFT = 1.5f;
    constexpr uint32 PORTAL_RETURN_MS = 30 * MINUTE * IN_MILLISECONDS;
    constexpr float PORTAL_HOME_RANGE = 60.0f;
    // A death's checkpoint gives back this share of health and mana.
    constexpr float CHECKPOINT_RESTORE = 0.5f;

    struct Spot
    {
        uint32 map = 0;
        float x = 0.0f, y = 0.0f, z = 0.0f, o = 0.0f;
    };

    struct State
    {
        uint8 charges = FLASK_MAX;
        uint32 points = 0;
        uint32 restSince = 0;       // when the player was last seen in combat
        uint32 dodgeReady = 0;      // the time the dodge is ready again
        uint32 evadeUntil = 0;      // the roll's dodge window ends
        bool dirty = true;
        uint32 lastSent = 0;
        // The town portal being opened (the time it opens, 0 none) and from where; the way back.
        uint32 portalAt = 0;
        Spot portalFrom;
        uint32 returnUntil = 0;
        Spot returnTo;
        uint32 returnInstance = 0;
        // Where the player came into the map it is on (its instance too): a dungeon death rises
        // there, at the door the player used.
        uint32 entryMap = 0, entryInstance = 0;
        Spot entry;
        bool entrySet = false;
        // Released and on the way to the checkpoint as a spirit: risen on arrival.
        bool rising = false;
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

    bool InTown(Player* player)
    {
        if (player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_RESTING))
            return true;
        float x, y, z;
        uint32 map;
        player->GetHomebindLocation(x, y, z, map);
        return map == player->GetMapId() && player->GetDistance(x, y, z) <= PORTAL_HOME_RANGE;
    }

    void TownPortal(Player* player)
    {
        if (!Active(player) || !player->IsAlive() || !player->IsInWorld() || player->IsTaxiFlying())
            return;
        ChatHandler chat(player);
        if (player->InBattleGround())
        {
            chat.SendSysMessage("|cffff8080No town portal here.|r");
            return;
        }
        if (player->IsInCombat())
        {
            chat.SendSysMessage("|cffff8080You can't use a portal in combat.|r");
            return;
        }
        uint32 const now = WorldTimer::getMSTime();
        Spot back;
        uint32 backInstance = 0;
        bool goBack = false;
        {
            std::lock_guard<std::mutex> guard(sActionsLock);
            State& s = sStates[player->GetObjectGuid()];
            if (s.portalAt)
                return;
            if (Later(s.returnUntil, now) && InTown(player))
            {
                back = s.returnTo;
                backInstance = s.returnInstance;
                goBack = true;
            }
        }
        if (goBack)
        {
            // A dungeon reset (or a new group) since: the way back leads into another instance,
            // deep inside, past what it holds; the portal has closed.
            MapEntry const* entry = sMapStore.LookupEntry(back.map);
            if (entry && entry->IsDungeon())
            {
                DungeonPersistentState* bound = player->GetBoundInstanceSaveForSelfOrGroup(back.map);
                if (!bound || bound->GetInstanceId() != backInstance)
                {
                    {
                        std::lock_guard<std::mutex> guard(sActionsLock);
                        sStates[player->GetObjectGuid()].returnUntil = 0;
                    }
                    chat.SendSysMessage("|cffff8080Your portal has closed: that dungeon is not the one you left.|r");
                    return;
                }
            }
            if (player->TeleportTo(back.map, back.x, back.y, back.z, back.o))
            {
                {
                    std::lock_guard<std::mutex> guard(sActionsLock);
                    sStates[player->GetObjectGuid()].returnUntil = 0;
                }
                chat.SendSysMessage("|cff80c0ffYou step back through your portal.|r");
            }
            return;
        }
        if (player->GetTransport())
        {
            chat.SendSysMessage("|cffff8080You can't open a portal on a ship.|r");
            return;
        }
        {
            std::lock_guard<std::mutex> guard(sActionsLock);
            State& s = sStates[player->GetObjectGuid()];
            s.portalAt = After(now, PORTAL_CAST_MS);
            s.portalFrom = { player->GetMapId(), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), player->GetOrientation() };
        }
        player->HandleEmoteCommand(EMOTE_ONESHOT_SPELLPRECAST);
        chat.SendSysMessage("|cff80c0ffOpening a town portal...|r");
    }

    bool RespawnAtCheckpoint(Player* player)
    {
        if (!player || player->IsAlive() || !Active(player) || player->InBattleGround() || !player->IsInWorld())
            return false;
        Map* map = player->GetMap();
        Spot at;
        bool have = false;
        // In a dungeon: the door the player came in by (a dungeon of several wings has several),
        // else its entrance.
        if (map->IsDungeon())
        {
            {
                std::lock_guard<std::mutex> guard(sActionsLock);
                State const& s = sStates[player->GetObjectGuid()];
                if (s.entrySet && s.entryMap == player->GetMapId() && s.entryInstance == player->GetInstanceId())
                {
                    at = s.entry;
                    have = true;
                }
            }
            if (!have)
                if (AreaTrigger const* trigger = sObjectMgr.GetMapEntranceTrigger(player->GetMapId()))
                {
                    at = { trigger->target_mapId, trigger->target_X, trigger->target_Y, trigger->target_Z, trigger->target_Orientation };
                    have = true;
                }
        }
        if (!have)
            if (WorldSafeLocsEntry const* grave = map->GetGraveyardManager().GetClosestGraveYard(
                    player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), player->GetMapId(), player->GetTeam()))
            {
                at = { grave->map_id, grave->x, grave->y, grave->z, grave->o };
                have = true;
            }
        {
            std::lock_guard<std::mutex> guard(sActionsLock);
            State& s = sStates[player->GetObjectGuid()];
            s.portalAt = 0;
            s.rising = true;
        }
        // On the way as a spirit, risen on arrival (UpdateActions): alive at the corpse until the
        // client answers the teleport, it could die there again.
        if (have)
            player->TeleportTo(at.map, at.x, at.y, at.z, at.o);
        return true;
    }

    void UpdateActions(Player* player)
    {
        if (!Active(player))
            return;
        uint32 const now = WorldTimer::getMSTime();
        State copy;
        // The town portal opening: it closes on a move, a fight or a death, else it opens.
        bool portalFizzles = false, portalOpens = false, rise = false;
        {
            std::lock_guard<std::mutex> guard(sActionsLock);
            State& s = sStates[player->GetObjectGuid()];
            // A new map (or instance) under the player: where it came in.
            if (!player->IsBeingTeleported() && player->IsAlive() &&
                    (!s.entrySet || s.entryMap != player->GetMapId() || s.entryInstance != player->GetInstanceId()))
            {
                s.entrySet = true;
                s.entryMap = player->GetMapId();
                s.entryInstance = player->GetInstanceId();
                s.entry = { player->GetMapId(), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), player->GetOrientation() };
            }
            if (s.rising && !player->IsBeingTeleported())
            {
                s.rising = false;
                rise = !player->IsAlive();
            }
            if (s.portalAt)
            {
                Spot const& from = s.portalFrom;
                bool const moved = from.map != player->GetMapId() ||
                    player->GetDistance(from.x, from.y, from.z) > PORTAL_DRIFT;
                if (moved || player->IsInCombat() || !player->IsAlive())
                {
                    s.portalAt = 0;
                    portalFizzles = true;
                }
                else if (!Later(s.portalAt, now))
                {
                    s.portalAt = 0;
                    s.returnTo = from;
                    s.returnInstance = player->GetInstanceId();
                    s.returnUntil = After(now, PORTAL_RETURN_MS);
                    portalOpens = true;
                }
            }
        }
        if (rise)
        {
            player->ResurrectPlayer(CHECKPOINT_RESTORE);
            player->SpawnCorpseBones();
            ChatHandler(player).SendSysMessage("|cffffd200You rise again.|r");
        }
        if (portalFizzles)
            ChatHandler(player).SendSysMessage("|cffff8080Your portal fizzles.|r");
        if (portalOpens)
        {
            ChatHandler(player).SendSysMessage("|cff80c0ffYou step through to town. Your portal stays open for 30 minutes: press it again in town to go back.|r");
            player->TeleportToHomebind();
            return;
        }
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
