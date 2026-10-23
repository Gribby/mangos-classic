/*
 * ARPG actions: the dodge roll and the health flask. See ArpgActions.h.
 */

#include "Arpg/ArpgActions.h"
#include "Arpg/ArpgResources.h"
#include "Arpg/ArpgCombat.h"
#include "Arpg/ArpgCharacter.h"
#include "Arpg/ArpgDungeons.h"
#include "Arpg/ArpgPacks.h"
#include "Arpg/ArpgStats.h"

#include "Chat/Chat.h"
#include "Entities/Creature.h"
#include "Entities/GameObject.h"
#include "Globals/ObjectMgr.h"
#include "Maps/Map.h"
#include "MotionGenerators/MotionMaster.h"
#include "Maps/MapPersistentStateMgr.h"
#include "Server/DBCStores.h"
#include "Entities/Player.h"
#include "Server/Opcodes.h"
#include "Server/WorldPacket.h"
#include "Server/WorldSession.h"
#include "Spells/Spell.h"
#include "Spells/SpellMgr.h"
#include "Util/Timer.h"
#include "World/World.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <unordered_map>

namespace
{
    using namespace Arpg;

    constexpr uint32 DODGE_COOLDOWN_MS = 2500;
    constexpr uint32 DODGE_EVADE_MS = 450;
    // Knockback speeds: about 0.42 s in the air (2 * 4 / 19.29) at 17 yd/s, some seven yards.
    // The roll: a ground spline of DODGE_DISTANCE yards at DODGE_SPEED yards a second, along the
    // ground (over a rise, up a stair) and stopping at a wall or a drop, the same length every
    // time; the client rides it as it rides a Charge, standing on the ground under it.
    constexpr float DODGE_DISTANCE = 7.5f, DODGE_SPEED = 20.0f;
    // A roll blocked shorter than this (backed into a wall) does not move at all.
    constexpr float DODGE_MIN = 1.0f;

    constexpr uint8 FLASK_MAX = 3;
    constexpr uint32 FLASK_CHARGE_POINTS = 10;
    constexpr uint32 FLASK_INSTANT_PCT = 15, FLASK_OVER_TIME_PCT = 25;
    constexpr uint32 FLASK_TICKS = 6, FLASK_TICK_MS = 500;
    constexpr uint32 FLASK_MANA_INSTANT_PCT = 15, FLASK_MANA_OVER_TIME_PCT = 25;
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
    // The portal's look: the Hearthstone's swirl while it opens, then a mage's portal (its door
    // object, from the faction's Portal spell) standing where it opened, and where it leads back
    // to, for PORTAL_DOOR_SECS. Only the look: it can't be clicked.
    constexpr uint32 PORTAL_SWIRL = 8690;
    constexpr uint32 PORTAL_DOOR_ALLIANCE = 10059, PORTAL_DOOR_HORDE = 11417;
    constexpr uint32 PORTAL_DOOR_SECS = 8;
    // While the way back is open, both ends stand: where it opened, and a few yards in front of
    // the player in town; walking into the town end (within PORTAL_STEP yards, PORTAL_SETTLE_MS
    // after arriving) goes back through it, as the key does. Going back closes both.
    constexpr uint32 PORTAL_STAND_SECS = PORTAL_RETURN_MS / IN_MILLISECONDS;
    constexpr float PORTAL_TOWN_AHEAD = 4.0f;
    constexpr float PORTAL_STEP = 1.8f;
    constexpr uint32 PORTAL_SETTLE_MS = 2500;
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
        // The portal's two standing ends, each on its map and instance; the town end is placed on
        // arrival there; an end is taken down on its own map once the way back is used.
        ObjectGuid fieldDoor, townDoor;
        std::pair<uint32, uint32> fieldDoorOn{ 0, 0 }, townDoorOn{ 0, 0 };
        bool placeTownDoor = false;
        bool closeFieldDoor = false;
        uint32 arrivedAt = 0;
        // Where the player came into each map (and instance) it has been in: a dungeon death rises
        // there, at the door the player used. Stepping back through a town portal is not a door:
        // the dungeon's own door stays the one it rises at.
        std::map<std::pair<uint32, uint32>, Spot> doors;
        std::pair<uint32, uint32> onMap{ 0, 0 };
        bool mapSeen = false;
        bool viaPortal = false;     // the next arrival is a portal's way back
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

    // Health globes: the heal, how long one lies there, how near a taker must stand (server
    // positions lag the client's a little), and the drop chances in percent.
    constexpr uint32 GLOBE_HEAL_PCT = 20;
    constexpr uint32 GLOBE_LIFE_MS = 60 * IN_MILLISECONDS;
    constexpr float GLOBE_REACH = 4.0f;
    constexpr float GLOBE_SEND_RANGE = 100.0f;
    constexpr uint32 GLOBE_PCT_CHAMPION = 50, GLOBE_PCT = 7, GLOBE_PCT_HURT = 15;
    enum GlobeKind : uint8 { GLOBE_DROPPED = 1, GLOBE_GONE = 2 };

    struct Globe
    {
        uint32 map = 0, instance = 0;
        float x = 0.0f, y = 0.0f, z = 0.0f;
        uint32 until = 0;
    };
    std::mutex sGlobesLock;
    std::unordered_map<uint32, Globe> sGlobes;
    uint32 sGlobeSerial = 0;

    void SendGlobe(WorldObject const* source, uint8 kind, uint32 id, Globe const& g)
    {
        WorldPacket data(SMSG_ARPG_GLOBE, 1 + 4 + 12);
        data << uint8(kind) << uint32(id) << g.x << g.y << g.z;
        for (auto const& ref : source->GetMap()->GetPlayers())
        {
            Player* player = ref.getSource();
            if (player && player->IsInWorld() && Active(player) && player->IsWithinDist3d(g.x, g.y, g.z, GLOBE_SEND_RANGE))
                player->GetSession()->SendPacket(data);
        }
    }

    void DropGlobe(Player* killer, Unit* victim)
    {
        if (victim->GetTypeId() != TYPEID_UNIT)
            return;
        Creature* creature = static_cast<Creature*>(victim);
        // Nothing from critters, totems or anyone's pets and guardians.
        if (creature->IsCritter() || creature->IsTotem() || creature->IsPet() || creature->GetOwnerGuid() ||
                creature->IsPlayerControlled())
            return;
        uint32 pct = killer->GetHealth() * 2 < killer->GetMaxHealth() ? GLOBE_PCT_HURT : GLOBE_PCT;
        if (IsDungeonBoss(creature) || ChampionTier(creature) == 2)
            pct = 100;
        else if (ChampionTier(creature) == 1)
            pct = GLOBE_PCT_CHAMPION;
        if (!roll_chance_i(int(pct)))
            return;
        uint32 const now = WorldTimer::getMSTime();
        Globe g{ victim->GetMapId(), victim->GetInstanceId(), victim->GetPositionX(), victim->GetPositionY(),
                 victim->GetPositionZ(), After(now, GLOBE_LIFE_MS) };
        uint32 id;
        {
            std::lock_guard<std::mutex> guard(sGlobesLock);
            // Old ones go as new ones come.
            for (auto it = sGlobes.begin(); it != sGlobes.end();)
                it = Later(it->second.until, now) ? std::next(it) : sGlobes.erase(it);
            id = ++sGlobeSerial;
            if (!id)
                id = ++sGlobeSerial;
            sGlobes[id] = g;
        }
        SendGlobe(victim, GLOBE_DROPPED, id, g);
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
        float const dx = x - player->GetPositionX(), dy = y - player->GetPositionY();
        float const angle = (dx * dx + dy * dy) < 0.25f ? player->GetOrientation() : std::atan2(dy, dx);
        Position end;
        player->GetFirstCollisionPosition(end, DODGE_DISTANCE, angle);
        if (player->GetDistance2d(end.x, end.y, DIST_CALC_NONE) < DODGE_MIN || !std::isfinite(end.z))
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
        player->GetMotionMaster()->MoveCharge(end.x, end.y, end.z, DODGE_SPEED);
        OnStatsRoll(player);
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
        OnStatsFlask(player);
        uint32 const max = player->GetMaxHealth();
        int32 const instant = int32(std::max<uint32>(1, uint32(float(max * FLASK_INSTANT_PCT / 100) * GlobeHealMod(player))));
        player->CastCustomSpell(player, SPELL_HEALING_POTION, &instant, nullptr, nullptr, TRIGGERED_OLD_TRIGGERED);
        uint32 const tick = std::max<uint32>(1, uint32(float(max * FLASK_OVER_TIME_PCT / 100 / FLASK_TICKS) * GlobeHealMod(player)));
        // Mana alike, for any mana pool (a druid in a form included).
        uint32 const mana = player->GetMaxPower(POWER_MANA);
        if (mana)
            player->ModifyPower(POWER_MANA, int32(mana * FLASK_MANA_INSTANT_PCT / 100));
        int32 const manaTick = int32(mana * FLASK_MANA_OVER_TIME_PCT / 100 / FLASK_TICKS);
        for (uint32 i = 1; i <= FLASK_TICKS; ++i)
        {
            player->m_events.AddEvent(new UnitLambdaEvent(*player, [tick, manaTick](Unit& unit)
            {
                if (!unit.IsAlive() || !unit.IsInWorld())
                    return;
                if (SpellEntry const* potion = sSpellTemplate.LookupEntry<SpellEntry>(SPELL_HEALING_POTION))
                    unit.DealHeal(&unit, tick, potion);
                if (manaTick > 0)
                    unit.ModifyPower(POWER_MANA, manaTick);
            }), player->m_events.CalculateTime(i * FLASK_TICK_MS));
        }
    }

    void OnFlaskKill(Player* killer, Unit* victim)
    {
        if (!killer || !victim || !Active(killer))
            return;
        DropGlobe(killer, victim);
        uint32 const points = KillPoints(victim);
        std::lock_guard<std::mutex> guard(sActionsLock);
        AddPoints(sStates[killer->GetObjectGuid()], points);
    }

    void TakeGlobe(Player* player, uint32 id)
    {
        if (!Active(player) || !player->IsAlive() || !player->IsInWorld())
            return;
        Globe g;
        {
            std::lock_guard<std::mutex> guard(sGlobesLock);
            auto it = sGlobes.find(id);
            if (it == sGlobes.end())
                return;
            Globe const& found = it->second;
            if (found.map != player->GetMapId() || found.instance != player->GetInstanceId() ||
                    !player->IsWithinDist3d(found.x, found.y, found.z, GLOBE_REACH))
                return;
            bool const fresh = Later(found.until, WorldTimer::getMSTime());
            g = found;
            sGlobes.erase(it);
            if (!fresh)
                return;
        }
        ProcHeal(player, "Health Globe", std::max<uint32>(1, uint32(float(player->GetMaxHealth() * GLOBE_HEAL_PCT / 100) * GlobeHealMod(player))));
        // And a share of mana (Arpg/ArpgResources.h).
        if (player->GetMaxPower(POWER_MANA))
            player->ModifyPower(POWER_MANA, int32(player->GetMaxPower(POWER_MANA) * GLOBE_MANA_PCT / 100));
        SendGlobe(player, GLOBE_GONE, id, g);
    }

    // The swirl over `player` while its portal opens (SMSG_SPELL_START, nothing cast), or its end:
    // `opened` finishes it as a cast (SMSG_SPELL_GO), else it breaks off.
    void ShowPortalSwirl(Player* player, bool start, bool opened = false)
    {
        if (!sSpellTemplate.LookupEntry<SpellEntry>(PORTAL_SWIRL))
            return;
        if (opened)
        {
            WorldPacket data(SMSG_SPELL_GO, 40);
            data << player->GetPackGUID();
            data << player->GetPackGUID();
            data << uint32(PORTAL_SWIRL);
            data << uint16(CAST_FLAG_UNKNOWN9 | CAST_FLAG_HIDDEN_COMBATLOG);
            data << uint8(1);
            data << player->GetObjectGuid();
            data << uint8(0);
            SpellCastTargets targets;
            targets.setUnitTarget(player);
            data << targets;
            player->SendMessageToSet(data, true);
            return;
        }
        if (start)
        {
            WorldPacket data(SMSG_SPELL_START, 32);
            data << player->GetPackGUID();
            data << player->GetPackGUID();
            data << uint32(PORTAL_SWIRL);
            data << uint16(CAST_FLAG_UNKNOWN2 | CAST_FLAG_HIDDEN_COMBATLOG);
            data << uint32(PORTAL_CAST_MS);
            SpellCastTargets targets;
            targets.setUnitTarget(player);
            data << targets;
            player->SendMessageToSet(data, true);
            return;
        }
        WorldPacket data(SMSG_SPELL_FAILURE, 16);
        data << player->GetPackGUID();
        data << uint32(PORTAL_SWIRL);
        data << uint8(SPELL_FAILED_INTERRUPTED);
        player->SendMessageToSet(data, true);
        WorldPacket other(SMSG_SPELL_FAILED_OTHER, 12);
        other << player->GetObjectGuid();
        other << uint32(PORTAL_SWIRL);
        player->SendMessageToSet(other, true);
    }

    // A portal standing at (x, y, z) on `player`'s map for a few seconds.
    ObjectGuid ShowPortalDoor(Player* player, float x, float y, float z, float o, uint32 secs = PORTAL_DOOR_SECS)
    {
        SpellEntry const* door = sSpellTemplate.LookupEntry<SpellEntry>(
            player->GetTeam() == HORDE ? PORTAL_DOOR_HORDE : PORTAL_DOOR_ALLIANCE);
        uint32 entry = 0;
        if (door)
            for (uint32 i = 0; i < MAX_EFFECT_INDEX; ++i)
                if (door->Effect[i] == SPELL_EFFECT_TRANS_DOOR)
                    entry = uint32(door->EffectMiscValue[i]);
        if (!entry || !ObjectMgr::GetGameObjectInfo(entry) || !player->IsInWorld())
            return ObjectGuid();
        Map* map = player->GetMap();
        GameObject* go = new GameObject;
        uint32 const low = map->GenerateLocalLowGuid(HIGHGUID_GAMEOBJECT);
        if (!go->Create(low, low, entry, map, x, y, z, o))
        {
            delete go;
            return ObjectGuid();
        }
        go->SetRespawnTime(secs);
        go->SetOwnerGuid(player->GetObjectGuid());
        go->SetUInt32Value(GAMEOBJECT_LEVEL, player->GetLevel());
        go->SetFlag(GAMEOBJECT_FLAGS, GO_FLAG_NO_INTERACT);
        go->SetSpellId(door->Id);
        go->SetSpawnerGuid(player->GetObjectGuid());
        map->Add(go);
        go->AIM_Initialize();
        return go->GetObjectGuid();
    }

    // Take down a portal end standing on `player`'s map.
    void TakeDownDoor(Player* player, ObjectGuid guid)
    {
        if (guid.IsEmpty() || !player->IsInWorld())
            return;
        if (GameObject* go = player->GetMap()->GetGameObject(guid))
            go->Delete();
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
            // Going back closes the portal: its town end goes now, its field end on arrival there.
            ObjectGuid townDoor;
            std::pair<uint32, uint32> townDoorOn;
            {
                std::lock_guard<std::mutex> guard(sActionsLock);
                State const& s = sStates[player->GetObjectGuid()];
                townDoor = s.townDoor;
                townDoorOn = s.townDoorOn;
            }
            if (townDoorOn == std::make_pair(player->GetMapId(), player->GetInstanceId()))
                TakeDownDoor(player, townDoor);
            bool const newMap = back.map != player->GetMapId();
            if (player->TeleportTo(back.map, back.x, back.y, back.z, back.o))
            {
                {
                    std::lock_guard<std::mutex> guard(sActionsLock);
                    State& s = sStates[player->GetObjectGuid()];
                    s.returnUntil = 0;
                    s.townDoor = ObjectGuid();
                    s.closeFieldDoor = true;
                    // Only an arrival on another map is the portal's; a near one never changes map.
                    s.viaPortal = newMap;
                }
                chat.SendSysMessage("|cff80c0ffYou step back through your portal, and it closes behind you.|r");
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
        ShowPortalSwirl(player, true);
        chat.SendSysMessage("|cff80c0ffOpening a town portal...|r");
    }

    bool RespawnAtCheckpoint(Player* player)
    {
        if (!player || player->IsAlive() || !Active(player) || player->InBattleGround() || !player->IsInWorld())
            return false;
        Map* map = player->GetMap();
        Spot at;
        bool have = false, casting = false;
        // In a dungeon: the door the player came in by (a dungeon of several wings has several),
        // else its entrance.
        if (map->IsDungeon())
        {
            {
                std::lock_guard<std::mutex> guard(sActionsLock);
                State const& s = sStates[player->GetObjectGuid()];
                auto door = s.doors.find({ player->GetMapId(), player->GetInstanceId() });
                if (door != s.doors.end())
                {
                    at = door->second;
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
            casting = s.portalAt != 0;
            s.portalAt = 0;
            s.rising = true;
        }
        if (casting)
            ShowPortalSwirl(player, false);
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
            std::pair<uint32, uint32> const here{ player->GetMapId(), player->GetInstanceId() };
            if (!player->IsBeingTeleported() && player->IsAlive() && (!s.mapSeen || s.onMap != here))
            {
                s.mapSeen = true;
                s.onMap = here;
                if (!s.viaPortal || !s.doors.count(here))
                {
                    // A long session's oldest-looking doors go, never the one being written.
                    if (s.doors.size() >= 16 && !s.doors.count(here))
                        s.doors.erase(s.doors.begin());
                    s.doors[here] = { player->GetMapId(), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), player->GetOrientation() };
                }
                s.viaPortal = false;
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
        {
            ShowPortalSwirl(player, false);
            ChatHandler(player).SendSysMessage("|cffff8080Your portal fizzles.|r");
        }
        if (portalOpens)
        {
            ShowPortalSwirl(player, false, true);
            // The portal stands where it opened while the way back is open; its other end rises in
            // town as the player arrives.
            ObjectGuid const door = ShowPortalDoor(player, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
                                                   player->GetOrientation(), PORTAL_STAND_SECS);
            {
                std::lock_guard<std::mutex> guard(sActionsLock);
                State& s = sStates[player->GetObjectGuid()];
                s.fieldDoor = door;
                s.fieldDoorOn = { player->GetMapId(), player->GetInstanceId() };
                s.placeTownDoor = true;
                s.closeFieldDoor = false;
            }
            ChatHandler(player).SendSysMessage("|cff80c0ffYou step through to town. Your portal stays open for 30 minutes: walk into it, or press it again, to go back.|r");
            player->TeleportToHomebind();
            return;
        }
        // The portal's ends: the town end rises on arrival there, the field end comes down once
        // the way back was used and the player is there, and walking into the town end goes back.
        if (player->IsInWorld() && !player->IsBeingTeleported() && player->IsAlive())
        {
            std::pair<uint32, uint32> const here{ player->GetMapId(), player->GetInstanceId() };
            bool placeTown = false, walkIn = false;
            ObjectGuid closeField, townDoor;
            {
                std::lock_guard<std::mutex> guard(sActionsLock);
                State& s = sStates[player->GetObjectGuid()];
                if (s.placeTownDoor && here != s.fieldDoorOn)
                {
                    s.placeTownDoor = false;
                    s.arrivedAt = now ? now : 1;
                    placeTown = Later(s.returnUntil, now);
                }
                if (s.closeFieldDoor && here == s.fieldDoorOn)
                {
                    s.closeFieldDoor = false;
                    closeField = s.fieldDoor;
                    s.fieldDoor = ObjectGuid();
                }
                townDoor = s.townDoor;
                walkIn = !townDoor.IsEmpty() && here == s.townDoorOn && Later(s.returnUntil, now) &&
                         (!s.arrivedAt || WorldTimer::getMSTimeDiff(s.arrivedAt, now) >= PORTAL_SETTLE_MS);
            }
            TakeDownDoor(player, closeField);
            if (placeTown)
            {
                float const o = player->GetOrientation();
                float const x = player->GetPositionX() + std::cos(o) * PORTAL_TOWN_AHEAD;
                float const y = player->GetPositionY() + std::sin(o) * PORTAL_TOWN_AHEAD;
                float z = player->GetMap()->GetHeight(x, y, player->GetPositionZ() + 3.0f);
                if (z <= INVALID_HEIGHT)
                    z = player->GetPositionZ();
                ObjectGuid const door = ShowPortalDoor(player, x, y, z, o + float(M_PI), PORTAL_STAND_SECS);
                std::lock_guard<std::mutex> guard(sActionsLock);
                State& s = sStates[player->GetObjectGuid()];
                s.townDoor = door;
                s.townDoorOn = here;
            }
            else if (walkIn && !player->IsInCombat())
            {
                GameObject* go = player->GetMap()->GetGameObject(townDoor);
                if (go && player->GetDistance2d(go->GetPositionX(), go->GetPositionY()) <= PORTAL_STEP)
                {
                    TownPortal(player);
                    return;
                }
            }
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
