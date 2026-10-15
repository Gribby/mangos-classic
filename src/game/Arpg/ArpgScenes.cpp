/*
 * ARPG scenes (Arpg/ArpgScenes.h).
 */

#include "Arpg/ArpgScenes.h"
#include "Arpg/ArpgCombat.h"

#include "Database/DatabaseEnv.h"
#include "Entities/Creature.h"
#include "Entities/Player.h"
#include "Grids/CellImpl.h"
#include "Grids/GridNotifiers.h"
#include "Grids/GridNotifiersImpl.h"
#include "Log/Log.h"
#include "Server/WorldPacket.h"
#include "Server/WorldSession.h"
#include "Util/Timer.h"
#include "World/World.h"

#include <cmath>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    using namespace Arpg;

    enum Trigger : uint8 { ON_ACCEPT, ON_NEAR };

    // A camera key in the anchor's frame: yards forward (toward the character), left and up from
    // the anchor's feet, for the eye and the point it looks at.
    struct Key
    {
        uint32 ms;
        float eye[3];
        float at[3];
    };

    // A subtitle; a null speaker is the anchor.
    struct Line
    {
        uint32 from, to;
        char const* speaker;
        char const* text;
    };

    // The anchor's emote at `ms`.
    struct Cue
    {
        uint32 ms;
        uint32 emote;
    };

    struct Scene
    {
        uint32 id;
        uint32 quest;               // the act quest it belongs to
        Trigger trigger;
        char const* anchor;         // the NPC or creature it is shot around, by name
        float reach;                // ON_NEAR: how close the character comes to its anchor
        uint32 duration;
        std::vector<Key> keys;
        std::vector<Line> lines;
        std::vector<Cue> cues;
    };

    // The scenes. Ids are saved per character: never reuse one.
    std::vector<Scene> const& Scenes()
    {
        static std::vector<Scene> const scenes = {
            // Act I opens in Northshire: Marshal McBride, as the first act quest is taken.
            { 1, 59000, ON_ACCEPT, "Marshal McBride", 0.0f, 16000,
              {
                  { 0,     { 14.0f,  7.0f, 10.0f }, { 0.0f, 0.0f, 1.6f } },
                  { 5500,  {  6.0f,  3.5f,  3.0f }, { 0.0f, 0.0f, 1.7f } },
                  { 11000, {  2.6f, -1.2f,  1.9f }, { 0.0f, 0.0f, 1.75f } },
                  { 16000, {  3.4f, -2.6f,  2.1f }, { 0.0f, 0.0f, 1.7f } },
              },
              {
                  { 600,   5400,  nullptr, "Northshire stood quiet for a hundred years. Not any more." },
                  { 5600,  10600, nullptr, "Kobolds in the vineyards, Defias cutthroats in the woods, and the Watch stretched thin." },
                  { 10800, 15800, nullptr, "You'll do, $N. Clear them out, and bring me Garrick Padfoot." },
              },
              { { 700, EMOTE_ONESHOT_TALK }, { 5700, EMOTE_ONESHOT_POINT }, { 10900, EMOTE_ONESHOT_TALK } } },

            // Act I opens in the Valley of Trials: Gornek.
            { 2, 59020, ON_ACCEPT, "Gornek", 0.0f, 16000,
              {
                  { 0,     { 14.0f, -7.0f, 10.0f }, { 0.0f, 0.0f, 1.6f } },
                  { 5500,  {  6.0f, -3.5f,  3.0f }, { 0.0f, 0.0f, 1.7f } },
                  { 11000, {  2.8f,  1.2f,  1.9f }, { 0.0f, 0.0f, 1.8f } },
                  { 16000, {  3.6f,  2.6f,  2.2f }, { 0.0f, 0.0f, 1.75f } },
              },
              {
                  { 600,   5400,  nullptr, "Thrall broke our chains. Freedom is not a gift, $N. It is earned." },
                  { 5600,  10600, nullptr, "Boars trample the valley, the warlocks' imps run wild, and Sarkoth nests in the north." },
                  { 10800, 15800, nullptr, "Cull them. Bring me Sarkoth's end, and you will have earned your name." },
              },
              { { 700, EMOTE_ONESHOT_TALK }, { 5700, EMOTE_ONESHOT_POINT }, { 10900, EMOTE_ONESHOT_TALK } } },

            // Hogger, first sighted: the end of Elwynn's chapter.
            { 3, 59005, ON_NEAR, "Hogger", 40.0f, 9500,
              {
                  { 0,    { 44.0f, -2.5f, 3.5f }, { 0.0f, 0.0f, 1.6f } },
                  { 4000, { 11.0f, -3.5f, 2.4f }, { 0.0f, 0.0f, 2.0f } },
                  { 7000, {  4.5f,  1.5f, 0.8f }, { 0.0f, 0.0f, 2.4f } },
                  { 9500, {  4.0f,  2.2f, 0.7f }, { 0.0f, 0.0f, 2.4f } },
              },
              {
                  { 4300, 9300, nullptr, "Grrrr... more bones to gnaw on!" },
              },
              { { 4300, EMOTE_ONESHOT_ROAR } } },

            // Zalazane, first sighted: the end of Durotar's chapter.
            { 4, 59024, ON_NEAR, "Zalazane", 40.0f, 9500,
              {
                  { 0,    { 44.0f, 2.5f, 3.5f }, { 0.0f, 0.0f, 1.6f } },
                  { 4000, { 10.0f, 3.5f, 2.4f }, { 0.0f, 0.0f, 1.9f } },
                  { 7000, {  4.2f, -1.4f, 0.9f }, { 0.0f, 0.0f, 2.1f } },
                  { 9500, {  3.8f, -2.0f, 0.8f }, { 0.0f, 0.0f, 2.1f } },
              },
              {
                  { 4300, 9300, nullptr, "Another one come to die? The loa be hungry today, mon!" },
              },
              { { 4300, EMOTE_ONESHOT_LAUGH } } },
        };
        return scenes;
    }

    Scene const* SceneById(uint32 id)
    {
        for (Scene const& s : Scenes())
            if (s.id == id)
                return &s;
        return nullptr;
    }

    constexpr uint32 GRACE_MS = 5000;           // past its length, a scene the client never closed ends
    constexpr float ANCHOR_REACH = 60.0f;       // an ON_ACCEPT anchor is looked for this close

    struct State
    {
        bool loaded = false;
        std::set<uint32> seen;
        uint32 queued = 0;                      // a scene to start on the next update
        // The scene on screen.
        uint32 playing = 0;
        uint32 startedAt = 0;
        size_t nextCue = 0;
        ObjectGuid anchor;
        uint32 mapId = 0;
        bool done = false;                      // the client closed it
        bool madeImmune = false;
        // Scenes the server ended before the client closed them: the client's close for each is
        // still to come (CMSG_COMPLETE_CINEMATIC names no scene), and must not end the next one.
        uint32 owedAcks = 0;
    };
    // Each state is read and written by its character's map thread (its update and its thread-safe
    // handlers) or by the world thread while no map updates (its unsafe handlers, logout), so the
    // lock guards only the map itself; nodes stay put while others are added.
    std::mutex sLock;
    std::unordered_map<ObjectGuid, State> sStates;

    State& StateOf(Player const* player)
    {
        std::lock_guard<std::mutex> guard(sLock);
        return sStates[player->GetObjectGuid()];
    }

    bool ScenesOn()
    {
        return sWorld.getConfig(CONFIG_BOOL_ARPG_ENABLE) && sWorld.getConfig(CONFIG_BOOL_ARPG_ACTS);
    }

    void EnsureTable()
    {
        static bool const created = []()
        {
            CharacterDatabase.DirectExecute(
                "CREATE TABLE IF NOT EXISTS character_arpg_scene ("
                "guid INT UNSIGNED NOT NULL, scene INT UNSIGNED NOT NULL, PRIMARY KEY (guid, scene)) "
                "ENGINE=InnoDB DEFAULT CHARSET=utf8 COMMENT='ARPG scenes seen (Arpg/ArpgScenes.h)'");
            return true;
        }();
        (void)created;
    }

    void Load(Player* player, State& st)
    {
        if (st.loaded)
            return;
        st.loaded = true;
        EnsureTable();
        if (auto result = CharacterDatabase.PQuery("SELECT scene FROM character_arpg_scene WHERE guid = %u", player->GetGUIDLow()))
        {
            do
                st.seen.insert(result->Fetch()[0].GetUInt32());
            while (result->NextRow());
        }
    }

    void MarkSeen(Player* player, State& st, uint32 scene)
    {
        st.seen.insert(scene);
        CharacterDatabase.PExecute("REPLACE INTO character_arpg_scene (guid, scene) VALUES (%u, %u)", player->GetGUIDLow(), scene);
    }

    // The nearest living creature named `name` within `range` of `player`.
    Creature* Nearest(Player* player, char const* name, float range)
    {
        UnitList around;
        MaNGOS::AnyUnitInObjectRangeCheck check(player, range);
        MaNGOS::UnitListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> searcher(around, check);
        Cell::VisitAllObjects(player, searcher, range);
        Creature* best = nullptr;
        float bestDist = range;
        for (Unit* unit : around)
        {
            if (unit->GetTypeId() != TYPEID_UNIT || !unit->IsAlive() || std::strcmp(unit->GetName(), name) != 0)
                continue;
            float const d = player->GetDistance(unit);
            if (d <= bestDist)
            {
                bestDist = d;
                best = static_cast<Creature*>(unit);
            }
        }
        return best;
    }

    std::string Spoken(char const* text, Player* player)
    {
        std::string s = text;
        for (size_t at = s.find("$N"); at != std::string::npos; at = s.find("$N", at))
        {
            s.replace(at, 2, player->GetName());
            at += std::strlen(player->GetName());
        }
        return s;
    }

    void Send(Player* player, Scene const& scene, Creature* anchor)
    {
        // The anchor's frame: forward from it toward the character.
        float const ax = anchor->GetPositionX(), ay = anchor->GetPositionY(), az = anchor->GetPositionZ();
        float const o = std::atan2(player->GetPositionY() - ay, player->GetPositionX() - ax);
        float const c = std::cos(o), s = std::sin(o);
        auto world = [&](float const local[3], WorldPacket& data)
        {
            data << float(ax + local[0] * c - local[1] * s);
            data << float(ay + local[0] * s + local[1] * c);
            data << float(az + local[2]);
        };
        WorldPacket data(SMSG_ARPG_SCENE, 64 + scene.keys.size() * 28 + scene.lines.size() * 96);
        data << uint32(scene.id);
        data << uint32(scene.duration);
        data << uint8(scene.keys.size());
        for (Key const& k : scene.keys)
        {
            data << uint32(k.ms);
            world(k.eye, data);
            world(k.at, data);
        }
        data << uint8(scene.lines.size());
        for (Line const& l : scene.lines)
        {
            data << uint32(l.from) << uint32(l.to);
            data << std::string(l.speaker ? l.speaker : anchor->GetName());
            data << Spoken(l.text, player);
        }
        player->GetSession()->SendPacket(data);
    }

    void Start(Player* player, State& st, Scene const& scene, Creature* anchor)
    {
        MarkSeen(player, st, scene.id);
        st.playing = scene.id;
        st.startedAt = WorldTimer::getMSTime();
        st.nextCue = 0;
        st.anchor = anchor->GetObjectGuid();
        st.mapId = player->GetMapId();
        st.done = false;
        // Held still and left alone while the camera is away.
        player->SetImmobilizedState(true);
        st.madeImmune = !player->IsImmuneToNPC();
        if (st.madeImmune)
            player->SetImmuneToNPC(true);
        anchor->StopMoving();
        anchor->SetFacingToObject(player);
        Send(player, scene, anchor);
        sLog.outDetail("ARPG scenes: %s watches scene %u around %s", player->GetName(), scene.id, anchor->GetName());
    }

    // `acked`: the client closed it; otherwise its close is owed.
    void End(Player* player, State& st, bool acked)
    {
        if (!st.playing)
            return;
        if (!acked)
            ++st.owedAcks;
        st.playing = 0;
        st.done = false;
        if (player->IsInWorld())
        {
            player->SetImmobilizedState(false);
            if (st.madeImmune)
                player->SetImmuneToNPC(false);
        }
        st.madeImmune = false;
    }

    // Whether `player`'s act quest `quest` is taken and not yet done.
    bool UnderWay(Player* player, uint32 quest)
    {
        return player->GetQuestStatus(quest) == QUEST_STATUS_INCOMPLETE;
    }
}

namespace Arpg
{
    void OnSceneQuestAccepted(Player* player, uint32 quest)
    {
        if (!player || !ScenesOn() || !Active(player))
            return;
        for (Scene const& scene : Scenes())
            if (scene.trigger == ON_ACCEPT && scene.quest == quest)
            {
                StateOf(player).queued = scene.id;
                return;
            }
    }

    void UpdateScenes(Player* player)
    {
        if (!player || !player->GetSession())
            return;
        bool const on = ScenesOn() && Active(player);
        if (!on && !InScene(player))
            return;
        State& st = StateOf(player);
        uint32 const now = WorldTimer::getMSTime();

        if (st.playing)
        {
            Scene const* scene = SceneById(st.playing);
            uint32 const elapsed = WorldTimer::getMSTimeDiff(st.startedAt, now);
            if (st.done)
            {
                End(player, st, true);
                return;
            }
            if (!on || !scene || !player->IsInWorld() || !player->IsAlive() || player->GetMapId() != st.mapId ||
                    elapsed > scene->duration + GRACE_MS)
            {
                End(player, st, false);
                return;
            }
            Creature* anchor = player->GetMap()->GetCreature(st.anchor);
            while (st.nextCue < scene->cues.size() && scene->cues[st.nextCue].ms <= elapsed)
            {
                if (anchor && anchor->IsAlive())
                    anchor->HandleEmote(scene->cues[st.nextCue].emote);
                ++st.nextCue;
            }
            return;
        }

        if (!player->IsInWorld() || !player->IsAlive() || player->IsInCombat() || player->IsTaxiFlying())
            return;
        Load(player, st);
        // A taken quest's scene.
        if (st.queued)
        {
            uint32 const id = st.queued;
            st.queued = 0;
            Scene const* scene = SceneById(id);
            if (scene && !st.seen.count(id) && UnderWay(player, scene->quest))
            {
                if (Creature* anchor = Nearest(player, scene->anchor, ANCHOR_REACH))
                    Start(player, st, *scene, anchor);
                else
                    sLog.outError("ARPG scenes: scene %u finds no \"%s\" near %s", id, scene->anchor, player->GetName());
            }
            return;
        }
        // A leader's reveal, on first coming near it.
        for (Scene const& scene : Scenes())
        {
            if (scene.trigger != ON_NEAR || st.seen.count(scene.id) || !UnderWay(player, scene.quest))
                continue;
            Creature* anchor = Nearest(player, scene.anchor, scene.reach);
            if (!anchor || anchor->IsInCombat())
                continue;
            Start(player, st, scene, anchor);
            return;
        }
    }

    bool InScene(Player const* player)
    {
        if (!player)
            return false;
        std::lock_guard<std::mutex> guard(sLock);
        auto it = sStates.find(player->GetObjectGuid());
        return it != sStates.end() && it->second.playing != 0;
    }

    void OnSceneDone(Player* player)
    {
        if (!player)
            return;
        std::lock_guard<std::mutex> guard(sLock);
        auto it = sStates.find(player->GetObjectGuid());
        if (it == sStates.end())
            return;
        // A close for a scene the server already ended is not this one's.
        if (it->second.owedAcks)
            --it->second.owedAcks;
        else if (it->second.playing)
            it->second.done = true;
    }

    void ForgetScenes(Player* player)
    {
        if (!player)
            return;
        std::lock_guard<std::mutex> guard(sLock);
        auto it = sStates.find(player->GetObjectGuid());
        if (it == sStates.end())
            return;
        End(player, it->second, true);
        sStates.erase(it);
    }
}
