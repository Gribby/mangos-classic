/*
 * ARPG boons (Arpg/ArpgBoons.h).
 */

#include "Arpg/ArpgBoons.h"
#include "Arpg/ArpgCombat.h"

#include "Entities/Player.h"
#include "Server/Opcodes.h"
#include "Server/WorldPacket.h"
#include "Server/WorldSession.h"
#include "Util/Timer.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>

namespace
{
    using namespace Arpg;

    // The HUD hears of a change at most this often (a Barrier spent blow by blow).
    constexpr uint32 SEND_EVERY_MS = 200;

    struct Boon
    {
        uint8 id = 0;
        bool standing = false;
        uint32 until = 0;       // timed: when it ends
        uint32 total = 0;       // timed: its whole length
        std::string name;
    };

    struct State
    {
        uint32 barrier = 0;
        uint32 barrierUntil = 0;
        std::vector<Boon> boons;
        bool dirty = false;
        uint32 sentAt = 0;
        uint32 sentMaxHealth = 0;
    };

    std::mutex sLock;
    std::unordered_map<ObjectGuid, State> sStates;

    bool Later(uint32 when, uint32 now)
    {
        return int32(when - now) > 0;
    }
}

namespace Arpg
{
    void GainBarrier(Player* player, uint32 amount)
    {
        if (!player || !amount || !Active(player) || !player->IsAlive())
            return;
        uint32 const cap = uint32(uint64(player->GetMaxHealth()) * BARRIER_CAP_PCT / 100);
        uint32 const now = WorldTimer::getMSTime();
        std::lock_guard<std::mutex> guard(sLock);
        State& s = sStates[player->GetObjectGuid()];
        if (s.barrier && !Later(s.barrierUntil, now))
            s.barrier = 0;
        s.barrier = std::min(cap, s.barrier + amount);
        s.barrierUntil = now + BARRIER_MS;
        s.dirty = true;
    }

    uint32 BarrierOf(Player const* player)
    {
        if (!player)
            return 0;
        std::lock_guard<std::mutex> guard(sLock);
        auto it = sStates.find(player->GetObjectGuid());
        if (it == sStates.end() || !Later(it->second.barrierUntil, WorldTimer::getMSTime()))
            return 0;
        return it->second.barrier;
    }

    uint32 BarrierAbsorb(Unit* victim, uint32 damage)
    {
        if (!victim || !damage || victim->GetTypeId() != TYPEID_PLAYER)
            return 0;
        std::lock_guard<std::mutex> guard(sLock);
        auto it = sStates.find(victim->GetObjectGuid());
        if (it == sStates.end() || !it->second.barrier)
            return 0;
        State& s = it->second;
        if (!Later(s.barrierUntil, WorldTimer::getMSTime()))
        {
            s.barrier = 0;
            s.dirty = true;
            return 0;
        }
        uint32 const taken = std::min(s.barrier, damage);
        s.barrier -= taken;
        s.dirty = true;
        return taken;
    }

    void AddBoon(Player* player, uint8 id, std::string const& name, uint32 ms)
    {
        if (!player || !id || !ms)
            return;
        uint32 const now = WorldTimer::getMSTime();
        std::lock_guard<std::mutex> guard(sLock);
        State& s = sStates[player->GetObjectGuid()];
        auto it = std::find_if(s.boons.begin(), s.boons.end(), [id](Boon const& b) { return b.id == id; });
        if (it == s.boons.end())
            it = s.boons.insert(s.boons.end(), Boon());
        it->id = id;
        it->standing = false;
        it->until = now + ms;
        it->total = ms;
        it->name = name;
        s.dirty = true;
    }

    void RemoveBoon(Player* player, uint8 id)
    {
        if (!player)
            return;
        std::lock_guard<std::mutex> guard(sLock);
        auto st = sStates.find(player->GetObjectGuid());
        if (st == sStates.end())
            return;
        auto& boons = st->second.boons;
        auto it = std::remove_if(boons.begin(), boons.end(), [id](Boon const& b) { return b.id == id; });
        if (it != boons.end())
        {
            boons.erase(it, boons.end());
            st->second.dirty = true;
        }
    }

    bool HasBoon(Player const* player, uint8 id)
    {
        if (!player)
            return false;
        uint32 const now = WorldTimer::getMSTime();
        std::lock_guard<std::mutex> guard(sLock);
        auto st = sStates.find(player->GetObjectGuid());
        if (st == sStates.end())
            return false;
        for (Boon const& b : st->second.boons)
            if (b.id == id)
                return b.standing || Later(b.until, now);
        return false;
    }

    void SetStandingBoons(Player* player, std::vector<std::pair<uint8, std::string>> const& standing)
    {
        if (!player)
            return;
        std::lock_guard<std::mutex> guard(sLock);
        State& s = sStates[player->GetObjectGuid()];
        // The same list again changes nothing (it is set on every entry and update of a place).
        std::vector<std::pair<uint8, std::string>> had;
        for (Boon const& b : s.boons)
            if (b.standing)
                had.emplace_back(b.id, b.name);
        if (had == standing)
            return;
        s.boons.erase(std::remove_if(s.boons.begin(), s.boons.end(), [](Boon const& b) { return b.standing; }), s.boons.end());
        for (auto const& [id, name] : standing)
        {
            Boon b;
            b.id = id;
            b.standing = true;
            b.name = name;
            s.boons.push_back(b);
        }
        s.dirty = true;
    }

    void UpdateBoons(Player* player)
    {
        if (!player || !player->GetSession())
            return;
        uint32 const now = WorldTimer::getMSTime();
        WorldPacket data;
        {
            std::lock_guard<std::mutex> guard(sLock);
            // A first update (a login) sends an empty line, so a HUD that outlived a relog clears.
            auto [it, fresh] = sStates.try_emplace(player->GetObjectGuid());
            State& s = it->second;
            if (fresh)
                s.dirty = true;
            if (s.barrier && (!Later(s.barrierUntil, now) || !player->IsAlive()))
            {
                s.barrier = 0;
                s.dirty = true;
            }
            size_t const before = s.boons.size();
            s.boons.erase(std::remove_if(s.boons.begin(), s.boons.end(), [now](Boon const& b)
            {
                return !b.standing && !Later(b.until, now);
            }), s.boons.end());
            if (s.boons.size() != before)
                s.dirty = true;
            // A new maximum health moves the Barrier's share on the orb.
            if (s.barrier && s.sentMaxHealth != player->GetMaxHealth())
                s.dirty = true;
            if (!s.dirty || (s.sentAt && WorldTimer::getMSTimeDiff(s.sentAt, now) < SEND_EVERY_MS))
                return;
            s.dirty = false;
            s.sentAt = now ? now : 1;
            s.sentMaxHealth = player->GetMaxHealth();
            data.Initialize(SMSG_ARPG_BOONS, 9 + s.boons.size() * 32);
            data << uint32(s.barrier) << uint32(player->GetMaxHealth());
            data << uint8(std::min<size_t>(s.boons.size(), 255));
            size_t n = 0;
            for (Boon const& b : s.boons)
            {
                if (++n > 255)
                    break;
                data << uint8(b.id) << uint8(b.standing ? 1 : 0);
                data << uint32(b.standing ? 0 : uint32(std::max<int32>(0, int32(b.until - now))));
                data << uint32(b.standing ? 0 : b.total);
                data << b.name;
            }
        }
        player->GetSession()->SendPacket(data);
    }

    void ForgetBoons(Player* player)
    {
        if (!player)
            return;
        std::lock_guard<std::mutex> guard(sLock);
        sStates.erase(player->GetObjectGuid());
    }
}
