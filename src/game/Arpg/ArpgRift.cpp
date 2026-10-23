/*
 * ARPG rifts (Arpg/ArpgRift.h).
 */

#include "Arpg/ArpgRift.h"
#include "Arpg/ArpgActs.h"
#include "Arpg/ArpgCombat.h"
#include "Arpg/ArpgPacks.h"
#include "Arpg/ArpgPerks.h"
#include "Arpg/ArpgDungeons.h"
#include "Arpg/ArpgAffixes.h"
#include "Arpg/ArpgShrines.h"

#include "Chat/Chat.h"
#include "Database/DatabaseEnv.h"
#include "Entities/Creature.h"
#include "Entities/GameObject.h"
#include "Entities/Player.h"
#include "Groups/Group.h"
#include "Log/Log.h"
#include "Maps/GridMap.h"
#include "Maps/Map.h"
#include "Maps/MapPersistentStateMgr.h"
#include "Globals/ObjectMgr.h"
#include "Server/DBCStores.h"
#include "Server/Opcodes.h"
#include "Server/SQLStorages.h"
#include "Server/WorldPacket.h"
#include "Server/WorldSession.h"
#include "Util/Timer.h"
#include "World/World.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <unordered_map>
#include <tuple>
#include <vector>

namespace
{
    using namespace Arpg;

    constexpr float TILE = SIZE_OF_GRIDS;
    constexpr float UNIT = TILE / 128.0f;           // a cell's side
    constexpr int NODES = 257;                      // a layout's outer heights a side (2 grids)
    constexpr int CHUNKS = 32;                      // its chunks a side
    constexpr int PIECES = 16;                      // its pieces a side, 2x2 chunks each
    constexpr int PIECE_NODES = 17;
    constexpr int BLOCK_SIDE = 16;                  // layouts' blocks a side on the rift map
    constexpr int BLOCKS = BLOCK_SIDE * BLOCK_SIDE;
    constexpr int BLOCK_FIRST = 8;                  // the first block's first grid
    constexpr int BLOCK_STEP = 3;                   // 2 grids a block, an empty one between

    // The source: Elwynn Forest's grids on map 0, outside Northshire, Goldshire and Stormwind.
    constexpr uint32 SRC_MAP = 0;
    constexpr uint32 SRC_ZONE = 12;
    constexpr int SRC_GX_FROM = 46, SRC_GX_TO = 51, SRC_GY_FROM = 28, SRC_GY_TO = 37;
    uint32 const SRC_EXCLUDED[] = { 9, 24, 87, 1519 };
    constexpr float FLAT_RANGE = 10.0f;             // a flat piece's rise at most

    // The layout.
    constexpr float BASE = 60.0f;                   // the open ground's height
    constexpr float HILLS = 42.0f;                  // the walls' rise round the open region
    constexpr float OPEN_FILL = 0.52f;              // a chunk's first chance to be open
    constexpr int CA_PASSES = 5;
    constexpr int MIN_OPEN = 200;                   // the open region's chunks, at least
    constexpr int SIDE_BORDER = 8;                  // a side area: the middle 16x16 chunks only
    constexpr int SIDE_MIN_OPEN = 70;
    // A cave: carved tunnels (CaveRegion), sheer walls over a near-flat floor.
    constexpr int CAVE_OPEN = 70;                   // chunks carved
    constexpr float CAVE_TURN = 0.3f;               // a step's chance to turn
    constexpr float CAVE_CHAMBER = 0.06f;           // a step's chance to open a 3x3 chamber
    constexpr float CAVE_WALL = 30.0f;
    constexpr float CAVE_RELIEF = 0.2f;
    constexpr uint32 SIDE_PACKS = 12;
    constexpr int BRANCH_APART = 8;                 // chunks between a main area's two branches
    constexpr int TRIES = 24;
    constexpr int SEAM_BAND = 3;                    // nodes either side of a seam blended
    constexpr int SEAM_PASSES = 3;
    constexpr int WALL_BLUR = 3;                    // nodes: how steep the walls are
    constexpr float HEIGHT_STEP = 8.0f;             // heights sent in eighths of a yard
    constexpr int ROWS_PER_PACKET = 32;

    // Its creatures.
    constexpr uint32 PACKS = 20;
    constexpr int PACK_FROM_START = 6;              // chunks' walk from the start, at least
    constexpr int PACK_APART = 3;                   // chunks between packs, at least
    constexpr float NATIVE_MIN_X = -10200.0f, NATIVE_MAX_X = -8600.0f;
    constexpr float NATIVE_MIN_Y = -1400.0f, NATIVE_MAX_Y = 700.0f;
    constexpr uint32 NATIVE_MIN_SPAWNS = 4;
    constexpr uint32 PACK_CORPSE_MS = MINUTE * IN_MILLISECONDS;

    // The stones and the exit.
    struct StoneAt
    {
        uint32 map;
        float x, y, z;                              // z: a guess, the ground's once placed
    };
    StoneAt const STONES[] =
    {
        { 0, -9447.0f, 62.0f, 56.0f },              // Goldshire
        { 1, 300.0f, -4700.0f, 10.0f },             // Razor Hill
    };
    constexpr int STONE_COUNT = int(sizeof(STONES) / sizeof(STONES[0]));
    constexpr float STONE_SHOW = 150.0f;            // placed when an ARPG player is this close
    constexpr float STONE_REACH = 3.0f;
    constexpr float EXIT_REACH = 3.0f;
    constexpr float STONE_RETURN = 7.0f;            // back from a rift this far from the stone
    constexpr uint32 STONE_SECS = HOUR;
    constexpr uint32 EXIT_SECS = 6 * HOUR;
    constexpr uint32 CHECK_MS = 300;
    constexpr uint32 STONE_COOLDOWN_MS = 5000;

    // --- The areas behind Goldshire's gates, and the free rift ---

    enum RiftKind : uint8
    {
        KIND_FREE = 0, KIND_FIELDS, KIND_FARMLANDS, KIND_FOREST,
        // Side areas, each off a main one's dead end, D2's Den of Evil.
        KIND_ECHO_RIDGE, KIND_WOLF_RUN, KIND_FARGODEEP, KIND_HIDEOUT, KIND_JASPERLODE, KIND_WARREN,
        MAX_KIND
    };

    struct AreaDef
    {
        char const* name;
        uint32 minLevel, maxLevel;                  // its creatures' levels (0: the player's)
        float minX, maxX, minY, maxY;               // where its pieces come from (0s: all Elwynn)
        std::array<char const*, 7> natives;         // its creatures, by name (none: Elwynn's hostiles)
        char const* midLeader;                      // a named one halfway (nullptr: none)
        char const* exitLeader;                     // the exit's guard (nullptr: a rare)
        RiftKind next;                              // where its exit leads (MAX_KIND: back)
        char const* text;                           // said on arrival
        // A side area: smaller, its boss at the far end, a perk for its first fall, no exit
        // but its way back; `door` is its entrance's object in the area it branches off.
        bool side;
        bool cave;                                  // a roofless cave (the client paints it)
        PerkId perk;
        uint32 door;
        std::array<RiftKind, 2> branches;           // a main area's side areas (MAX_KIND: none)
    };

    // Act I's Elwynn, as Diablo II's first act: three areas in a chain from Goldshire, each the
    // next one's way in, their creatures the act quests' (Arpg/ArpgActs.cpp), Hogger at the end.
    AreaDef const AREAS[MAX_KIND] =
    {
        { "Elwynn Rift", 0, 0, 0.0f, 0.0f, 0.0f, 0.0f, {{}}, nullptr, nullptr, MAX_KIND,
          "the forest is torn open. A rare guards the way out.", false, false, PERK_NONE, 0, {{ MAX_KIND, MAX_KIND }} },
        { "The Northshire Fields", 1, 5, -9300.0f, -8800.0f, -800.0f, 300.0f,
          {{ "Kobold Vermin", "Kobold Worker", "Defias Thug", "Young Wolf", "Timber Wolf", nullptr, nullptr }},
          nullptr, "Garrick Padfoot", KIND_FARMLANDS,
          "kobolds and Defias thugs overrun the fields. Garrick Padfoot holds the far end.",
          false, false, PERK_NONE, 0, {{ KIND_ECHO_RIDGE, KIND_WOLF_RUN }} },
        { "The Farmlands", 5, 8, -10000.0f, -9500.0f, -1500.0f, 0.0f,
          {{ "Kobold Tunneler", "Kobold Miner", "Kobold Laborer", "Defias Bandit", "Defias Cutpurse", "Defias Rogue Wizard", nullptr }},
          nullptr, nullptr, KIND_FOREST,
          "the farms burn and the mines crawl with kobolds. Goldtooth digs in Fargodeep Pit; the Defias hide off the road.",
          false, false, PERK_NONE, 0, {{ KIND_FARGODEEP, KIND_HIDEOUT }} },
        { "Forest's Edge", 8, 11, -10300.0f, -9600.0f, 200.0f, 1100.0f,
          {{ "Riverpaw Gnoll", "Riverpaw Runt", "Riverpaw Outrunner", "Prowler", "Young Forest Bear", nullptr, nullptr }},
          nullptr, "Hogger", MAX_KIND,
          "the Riverpaw gnolls hold the forest's edge, and Hogger's camp is at the end of it.",
          false, false, PERK_NONE, 0, {{ KIND_JASPERLODE, KIND_WARREN }} },
        { "Echo Ridge Diggings", 2, 5, -9100.0f, -8700.0f, -600.0f, 100.0f,
          {{ "Kobold Vermin", "Kobold Worker", "Kobold Laborer", nullptr, nullptr, nullptr, nullptr }},
          nullptr, nullptr, MAX_KIND,
          "the kobolds of Echo Ridge dig here, and something bigger digs with them.",
          true, true, PERK_ECHO_RIDGE, 98021, {{ MAX_KIND, MAX_KIND }} },
        { "Wolf Run", 3, 5, -9300.0f, -8800.0f, -800.0f, 300.0f,
          {{ "Young Wolf", "Timber Wolf", "Gray Forest Wolf", nullptr, nullptr, nullptr, nullptr }},
          nullptr, "Gruff Swiftbite", MAX_KIND,
          "the wolves run thick here. Gruff Swiftbite leads them.",
          true, false, PERK_WOLF_RUN, 98022, {{ MAX_KIND, MAX_KIND }} },
        { "Fargodeep Pit", 5, 8, -9950.0f, -9600.0f, -200.0f, 400.0f,
          {{ "Kobold Tunneler", "Kobold Miner", "Kobold Laborer", nullptr, nullptr, nullptr, nullptr }},
          nullptr, "Goldtooth", MAX_KIND,
          "the kobolds have dug Fargodeep into a pit. Goldtooth guards his candle-gold at the bottom.",
          true, true, PERK_FARGODEEP, 98023, {{ MAX_KIND, MAX_KIND }} },
        { "The Defias Hideout", 6, 8, -10000.0f, -9600.0f, -900.0f, -200.0f,
          {{ "Defias Bandit", "Defias Cutpurse", "Defias Rogue Wizard", nullptr, nullptr, nullptr, nullptr }},
          nullptr, "Morgan the Collector", MAX_KIND,
          "the Brotherhood's hideout among the burnt farms. Morgan the Collector counts the take inside.",
          true, false, PERK_HIDEOUT, 98024, {{ MAX_KIND, MAX_KIND }} },
        { "Jasperlode Hollow", 8, 10, -9300.0f, -8900.0f, -1200.0f, -500.0f,
          {{ "Kobold Geomancer", "Kobold Laborer", "Mine Spider", nullptr, nullptr, nullptr, nullptr }},
          nullptr, "Mother Fang", MAX_KIND,
          "geomancers and spiders share the hollow. Mother Fang nests at its end.",
          true, true, PERK_JASPERLODE, 98025, {{ MAX_KIND, MAX_KIND }} },
        { "Riverpaw Warren", 9, 11, -10300.0f, -9600.0f, 200.0f, 1100.0f,
          {{ "Riverpaw Gnoll", "Riverpaw Runt", "Riverpaw Outrunner", nullptr, nullptr, nullptr, nullptr }},
          nullptr, nullptr, MAX_KIND,
          "the Riverpaw's warren. Hogger's lieutenant keeps it.",
          true, false, PERK_WARREN, 98026, {{ MAX_KIND, MAX_KIND }} },
    };
    constexpr uint32 WAY_OUT_ENTRY = 98027;         // a side area's way back to the one it branches off

    // Goldshire, the hub: a palisade round the town, its gates the ways out.
    constexpr float HUB_X = -9464.0f, HUB_Y = 62.0f, HUB_Z = 57.0f;
    constexpr float HUB_RADIUS = 105.0f;
    constexpr float HUB_SHOW = 260.0f;              // placed when an ARPG player is this close
    constexpr float GATE_IN = 6.0f;                 // a gate stands this far inside the palisade
    constexpr float GATE_ARRIVE = 8.0f;             // one coming back arrives this far inside its gate
    constexpr float WALL_STEP = 7.0f;               // between the palisade's posts
    constexpr uint32 HUB_SECS = 2 * HOUR;
    constexpr uint32 HUB_CHAPTER_DONE = ACT_QUEST_FIRST + 5;   // Hogger: Elwynn opens
    constexpr uint32 HUB_LEVEL_MAX = 15;            // past it, the palisade holds no one
    // The Westfall road's end: Sentinel Hill.
    constexpr float WESTFALL_X = -10628.0f, WESTFALL_Y = 1037.0f, WESTFALL_Z = 35.0f, WESTFALL_O = 3.1f;

    struct GateDef
    {
        float bearing;                              // from the town's centre (0 north, X)
        uint32 entry;
        char const* name;
        RiftKind kind;                              // the area it opens (MAX_KIND: Westfall)
        uint32 openLevel;                           // open from this level (0: by quest only)
        uint32 openQuest;                           // or once this quest is done
    };
    enum { MAX_GATE = 4 };
    GateDef const GATES[MAX_GATE] =
    {
        { 0.0f, 98012, "Gate: The Northshire Fields", KIND_FIELDS, 0, 0 },
        { -float(M_PI) / 2.0f, 98013, "Gate: The Farmlands", KIND_FARMLANDS, 5, ACT_QUEST_FIRST + 0 },
        { float(M_PI), 98014, "Gate: Forest's Edge", KIND_FOREST, 8, ACT_QUEST_FIRST + 3 },
        { float(M_PI) / 2.0f, 98015, "The Road to Westfall", MAX_KIND, 0, HUB_CHAPTER_DONE },
    };
    constexpr uint32 WALL_ENTRY = 98016;
    constexpr uint32 BACK_ENTRY = 98017;            // an area's way back, at its start
    uint32 const EXIT_ENTRIES[MAX_KIND] = { RIFT_EXIT_ENTRY, 98018, 98019, 98020 };

    struct Piece
    {
        uint8 gx, gy, ca, cb;                       // source grid, first chunk (a along X)
        bool flat;
        float h[PIECE_NODES * PIECE_NODES];         // heights less the piece's mean
    };

    struct Layout
    {
        uint32 id = 0;
        int block = 0;
        uint8 gx0 = 0, gy0 = 0;
        RiftKind kind = KIND_FREE;
        std::vector<int16> q;                       // NODES * NODES: (height - BASE) * 8
        std::array<uint32, PIECES * PIECES> pieces{};
        float startX = 0, startY = 0, startZ = 0;
        float exitX = 0, exitY = 0, exitZ = 0;
        float backX = 0, backY = 0, backZ = 0;      // the way back, beside the start
        // A main area's side areas' entrances, at its dead ends.
        struct Branch
        {
            RiftKind kind = MAX_KIND;
            float x = 0, y = 0, z = 0;
        };
        std::array<Branch, 2> branches;
        // A side area: the layout it branches off, and where in it one leaving arrives.
        uint32 parentId = 0;
        float returnX = 0, returnY = 0, returnZ = 0;
        std::vector<std::array<float, 3>> spots;    // packs' places

        float X(float i) const { return (32 - gx0) * TILE - i * UNIT; }
        float Y(float j) const { return (32 - gy0) * TILE - j * UNIT; }
        float H(int i, int j) const
        {
            i = std::min(std::max(i, 0), NODES - 1);
            j = std::min(std::max(j, 0), NODES - 1);
            return BASE + q[i * NODES + j] / HEIGHT_STEP;
        }
        bool Inside(float x, float y) const
        {
            float const i = ((32 - gx0) * TILE - x) / UNIT, j = ((32 - gy0) * TILE - y) / UNIT;
            return i >= 0 && j >= 0 && i <= NODES - 1 && j <= NODES - 1;
        }
    };
    using LayoutPtr = std::shared_ptr<Layout const>;

    struct PlayerRift
    {
        LayoutPtr pending;                          // made at the stone, not yet arrived
        uint32 sentId = 0;                          // the layout the client has
        uint32 checkedAt = 0;
        uint32 stoneAt = 0;                         // when it last left by a stone or exit
        int returnStone = -1;                       // the stone it came by; -1 its side's
    };

    std::mutex sLock;
    std::vector<Piece> sLibrary;
    std::vector<uint32> sFlat;                      // the flat pieces' indexes
    std::vector<uint32> sPool[MAX_KIND], sPoolFlat[MAX_KIND];   // each kind's pieces
    bool sLibraryBuilt = false;
    bool sRegistered = false;
    std::set<uint32> sObjectReady;                  // the object templates written
    LayoutPtr sBlocks[BLOCKS];
    int sNextBlock = 0;
    uint32 sNextId = 0;                             // seeded from the clock at start
    std::unordered_map<uint32, std::vector<LayoutPtr>> sInstances;  // an instance's areas, by its id
    std::set<uint32> sPopulated;                                 // layouts whose creatures stand
    // A side area's boss, alive: its instance and perk; and those whose corpse gets a hoard.
    // Creature guids restart in each instance, so the instance is part of the record.
    std::unordered_map<ObjectGuid, std::pair<uint32, PerkId>> sSideBosses;
    std::unordered_map<ObjectGuid, uint32> sHoards;
    std::vector<ObjectGuid> sHubObjects;                         // the palisade's posts
    ObjectGuid sHubGates[MAX_GATE];
    bool sHubPlaced = false;
    float sGateZ[MAX_GATE] = { HUB_Z, HUB_Z, HUB_Z, HUB_Z };
    std::unordered_map<ObjectGuid, PlayerRift> sPlayers;
    ObjectGuid sStones[STONE_COUNT];
    float sStoneZ[STONE_COUNT] = { STONES[0].z, STONES[1].z };
    MapEntry sRiftEntry;
    char sRiftName[] = "Elwynn Rift";

    bool RiftsOn()
    {
        return sRegistered && sWorld.getConfig(CONFIG_BOOL_ARPG_ENABLE);
    }

    uint16 ElwynnFlag()
    {
        AreaTableEntry const* area = sAreaStore.LookupEntry(SRC_ZONE);
        return area ? uint16(area->exploreFlag) : 0;
    }

    // --- The piece library ---

    void BuildLibrary()
    {
        // Under sLock.
        if (sLibraryBuilt)
            return;
        sLibraryBuilt = true;
        std::map<uint16, bool> flagOk;
        auto elwynn = [&flagOk](uint16 flag)
        {
            auto it = flagOk.find(flag);
            if (it != flagOk.end())
                return it->second;
            AreaTableEntry const* area = GetAreaEntryByAreaFlagAndMap(flag, SRC_MAP);
            bool ok = area && (area->ID == SRC_ZONE || area->zone == SRC_ZONE);
            if (ok)
                for (uint32 excluded : SRC_EXCLUDED)
                    if (area->ID == excluded || area->zone == excluded)
                        ok = false;
            flagOk[flag] = ok;
            return ok;
        };
        std::vector<float> v9(129 * 129);
        for (int gx = SRC_GX_FROM; gx <= SRC_GX_TO; ++gx)
        {
            for (int gy = SRC_GY_FROM; gy <= SRC_GY_TO; ++gy)
            {
                char path[1024];
                snprintf(path, sizeof(path), "%smaps/%03u%02u%02u.map", sWorld.GetDataPath().c_str(), SRC_MAP, gx, gy);
                GridMap grid;
                if (!grid.loadData(path) || !grid.copyOuterHeights(v9.data()))
                    continue;
                for (int ca = 0; ca + 1 < 16; ++ca)
                {
                    for (int cb = 0; cb + 1 < 16; ++cb)
                    {
                        bool ok = true;
                        for (int da = 0; da < 2 && ok; ++da)
                            for (int db = 0; db < 2 && ok; ++db)
                                ok = elwynn(grid.chunkAreaFlag(ca + da, cb + db)) && !grid.chunkHasLiquid(ca + da, cb + db) &&
                                     !grid.chunkHasHoles(ca + da, cb + db);
                        if (!ok)
                            continue;
                        Piece piece;
                        piece.gx = uint8(gx);
                        piece.gy = uint8(gy);
                        piece.ca = uint8(ca);
                        piece.cb = uint8(cb);
                        float sum = 0.0f, lo = 1e9f, hi = -1e9f;
                        for (int i = 0; i < PIECE_NODES && ok; ++i)
                        {
                            for (int j = 0; j < PIECE_NODES; ++j)
                            {
                                float const h = v9[(ca * 8 + i) * 129 + cb * 8 + j];
                                if (h <= INVALID_HEIGHT || !std::isfinite(h))
                                {
                                    ok = false;
                                    break;
                                }
                                piece.h[i * PIECE_NODES + j] = h;
                                sum += h;
                                lo = std::min(lo, h);
                                hi = std::max(hi, h);
                            }
                        }
                        if (!ok)
                            continue;
                        float const mean = sum / float(PIECE_NODES * PIECE_NODES);
                        for (float& h : piece.h)
                            h -= mean;
                        piece.flat = hi - lo <= FLAT_RANGE;
                        if (piece.flat)
                            sFlat.push_back(uint32(sLibrary.size()));
                        sLibrary.push_back(piece);
                    }
                }
            }
        }
        sLog.outString("ARPG rifts: %u pieces of Elwynn Forest, %u of them flat", uint32(sLibrary.size()), uint32(sFlat.size()));
        // Each area's pieces: from its part of the forest, else (too few there) all of it.
        for (int kind = 0; kind < MAX_KIND; ++kind)
        {
            AreaDef const& area = AREAS[kind];
            for (uint32 index = 0; index < sLibrary.size(); ++index)
            {
                Piece const& piece = sLibrary[index];
                float const x = (32 - piece.gx) * TILE - (piece.ca * 8 + 8) * UNIT;
                float const y = (32 - piece.gy) * TILE - (piece.cb * 8 + 8) * UNIT;
                if (area.minX == area.maxX || (x >= area.minX && x <= area.maxX && y >= area.minY && y <= area.maxY))
                {
                    sPool[kind].push_back(index);
                    if (piece.flat)
                        sPoolFlat[kind].push_back(index);
                }
            }
            if (sPool[kind].size() < 60 || sPoolFlat[kind].size() < 15)
            {
                sPool[kind].resize(sLibrary.size());
                for (uint32 index = 0; index < sLibrary.size(); ++index)
                    sPool[kind][index] = index;
                sPoolFlat[kind] = sFlat;
            }
            sLog.outString("ARPG rifts: %s takes %u pieces, %u flat", area.name, uint32(sPool[kind].size()),
                           uint32(sPoolFlat[kind].size()));
        }
    }

    // --- The layout ---

    // The open region: chunks, 1 open; the largest region of a cellular automaton's caves.
    std::vector<uint8> OpenRegion(std::mt19937& rng, int border, int minOpen, float fill, int passes)
    {
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        std::vector<uint8> open(CHUNKS * CHUNKS);
        for (int attempt = 0; attempt < TRIES; ++attempt)
        {
            for (int i = 0; i < CHUNKS; ++i)
                for (int j = 0; j < CHUNKS; ++j)
                    open[i * CHUNKS + j] = i >= border && j >= border && i < CHUNKS - border && j < CHUNKS - border && unit(rng) < fill;
            for (int pass = 0; pass < passes; ++pass)
            {
                std::vector<uint8> next(open.size());
                for (int i = border; i < CHUNKS - border; ++i)
                {
                    for (int j = border; j < CHUNKS - border; ++j)
                    {
                        int count = 0;
                        for (int di = -1; di <= 1; ++di)
                            for (int dj = -1; dj <= 1; ++dj)
                                count += open[(i + di) * CHUNKS + j + dj];
                        next[i * CHUNKS + j] = count >= 5;
                    }
                }
                open.swap(next);
            }
            // The largest region (4-connected) is kept.
            std::vector<int> region(open.size(), -1);
            int best = -1, bestSize = 0, regions = 0;
            for (int start = 0; start < CHUNKS * CHUNKS; ++start)
            {
                if (!open[start] || region[start] >= 0)
                    continue;
                int size = 0;
                std::deque<int> queue{ start };
                region[start] = regions;
                while (!queue.empty())
                {
                    int const c = queue.front();
                    queue.pop_front();
                    ++size;
                    int const ci = c / CHUNKS, cj = c % CHUNKS;
                    int const nb[4][2] = { { ci + 1, cj }, { ci - 1, cj }, { ci, cj + 1 }, { ci, cj - 1 } };
                    for (auto const& n : nb)
                    {
                        if (n[0] < 0 || n[1] < 0 || n[0] >= CHUNKS || n[1] >= CHUNKS)
                            continue;
                        int const k = n[0] * CHUNKS + n[1];
                        if (open[k] && region[k] < 0)
                        {
                            region[k] = regions;
                            queue.push_back(k);
                        }
                    }
                }
                if (size > bestSize)
                {
                    bestSize = size;
                    best = regions;
                }
                ++regions;
            }
            for (int c = 0; c < CHUNKS * CHUNKS; ++c)
                open[c] = region[c] == best && best >= 0;
            if (bestSize >= minOpen)
                return open;
        }
        return open;
    }

    // A cave's tunnels: a walker carves one-chunk passages through the middle 16x16 chunks from
    // the west side, turning now and then and opening a chamber now and then, till enough is
    // open. One region by construction, with loops where it crosses itself, and dead ends.
    std::vector<uint8> CaveRegion(std::mt19937& rng)
    {
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        std::vector<uint8> open(CHUNKS * CHUNKS, 0);
        int const lo = SIDE_BORDER, hi = CHUNKS - SIDE_BORDER - 1;
        int ci = (lo + hi) / 2, cj = lo;
        int di = 0, dj = 1, carved = 0;
        auto carve = [&](int a, int b)
        {
            if (a >= lo && a <= hi && b >= lo && b <= hi && !open[a * CHUNKS + b])
            {
                open[a * CHUNKS + b] = 1;
                ++carved;
            }
        };
        int const dirs[4][2] = { { 0, 1 }, { 0, -1 }, { 1, 0 }, { -1, 0 } };
        carve(ci, cj);
        for (int step = 0; carved < CAVE_OPEN && step < 5000; ++step)
        {
            if (unit(rng) < CAVE_TURN)
            {
                int const d = std::uniform_int_distribution<int>(0, 3)(rng);
                di = dirs[d][0];
                dj = dirs[d][1];
            }
            int const ni = ci + di, nj = cj + dj;
            if (ni < lo || ni > hi || nj < lo || nj > hi)
            {
                di = -di;
                dj = -dj;
                continue;
            }
            ci = ni;
            cj = nj;
            carve(ci, cj);
            if (unit(rng) < CAVE_CHAMBER)
                for (int a = -1; a <= 1; ++a)
                    for (int b = -1; b <= 1; ++b)
                        carve(ci + a, cj + b);
        }
        return open;
    }

    // Walking distances (chunks) from `from` through the open region; -1 where it can't reach.
    std::vector<int> Walk(std::vector<uint8> const& open, int from)
    {
        std::vector<int> dist(open.size(), -1);
        std::deque<int> queue{ from };
        dist[from] = 0;
        while (!queue.empty())
        {
            int const c = queue.front();
            queue.pop_front();
            int const ci = c / CHUNKS, cj = c % CHUNKS;
            int const nb[4][2] = { { ci + 1, cj }, { ci - 1, cj }, { ci, cj + 1 }, { ci, cj - 1 } };
            for (auto const& n : nb)
            {
                if (n[0] < 0 || n[1] < 0 || n[0] >= CHUNKS || n[1] >= CHUNKS)
                    continue;
                int const k = n[0] * CHUNKS + n[1];
                if (open[k] && dist[k] < 0)
                {
                    dist[k] = dist[c] + 1;
                    queue.push_back(k);
                }
            }
        }
        return dist;
    }

    // A box blur of `radius` over a NODES x NODES field, `passes` times.
    void Blur(std::vector<float>& field, int radius, int passes)
    {
        std::vector<float> tmp(field.size());
        for (int pass = 0; pass < passes; ++pass)
        {
            // Rows, then columns.
            for (int i = 0; i < NODES; ++i)
            {
                for (int j = 0; j < NODES; ++j)
                {
                    float sum = 0.0f;
                    int n = 0;
                    for (int d = -radius; d <= radius; ++d)
                    {
                        int const jj = j + d;
                        if (jj >= 0 && jj < NODES)
                        {
                            sum += field[i * NODES + jj];
                            ++n;
                        }
                    }
                    tmp[i * NODES + j] = sum / float(n);
                }
            }
            for (int i = 0; i < NODES; ++i)
            {
                for (int j = 0; j < NODES; ++j)
                {
                    float sum = 0.0f;
                    int n = 0;
                    for (int d = -radius; d <= radius; ++d)
                    {
                        int const ii = i + d;
                        if (ii >= 0 && ii < NODES)
                        {
                            sum += tmp[ii * NODES + j];
                            ++n;
                        }
                    }
                    field[i * NODES + j] = sum / float(n);
                }
            }
        }
    }

    // A new layout in `block`; null when there are no pieces. Under sLock (the library).
    std::shared_ptr<Layout> MakeLayout(int block, uint32 seed, RiftKind kind)
    {
        if (sLibrary.empty() || sPool[kind].empty())
            return nullptr;
        std::vector<uint32> const& pool = sPool[kind];
        std::vector<uint32> const& flat = sPoolFlat[kind];
        std::mt19937 rng(seed);
        auto layout = std::make_shared<Layout>();
        layout->id = sNextId++;
        layout->block = block;
        layout->kind = kind;
        layout->gx0 = uint8(BLOCK_FIRST + BLOCK_STEP * (block % BLOCK_SIDE));
        layout->gy0 = uint8(BLOCK_FIRST + BLOCK_STEP * (block / BLOCK_SIDE));

        bool const side = AREAS[kind].side;
        bool const cave = AREAS[kind].cave;
        std::vector<uint8> const open = cave ? CaveRegion(rng)
                                        : side ? OpenRegion(rng, SIDE_BORDER, SIDE_MIN_OPEN, OPEN_FILL, CA_PASSES)
                                        : OpenRegion(rng, 2, MIN_OPEN, OPEN_FILL, CA_PASSES);
        // The start: the open chunk farthest west (Y grows westward: the lowest j), nearest the
        // middle; the exit: the farthest walk from it.
        int start = -1;
        for (int c = 0; c < CHUNKS * CHUNKS; ++c)
        {
            if (!open[c])
                continue;
            if (start < 0 || c % CHUNKS < start % CHUNKS ||
                    (c % CHUNKS == start % CHUNKS && std::abs(c / CHUNKS - CHUNKS / 2) < std::abs(start / CHUNKS - CHUNKS / 2)))
                start = c;
        }
        if (start < 0)
            return nullptr;
        std::vector<int> const dist = Walk(open, start);
        int exit = start;
        for (int c = 0; c < CHUNKS * CHUNKS; ++c)
            if (dist[c] > dist[exit])
                exit = c;

        // The pieces: flat ones where any of their chunks is open.
        std::uniform_int_distribution<size_t> anyPiece(0, pool.size() - 1);
        for (int pi = 0; pi < PIECES; ++pi)
        {
            for (int pj = 0; pj < PIECES; ++pj)
            {
                bool anyOpen = false;
                for (int d = 0; d < 4; ++d)
                    anyOpen = anyOpen || open[(pi * 2 + d / 2) * CHUNKS + pj * 2 + d % 2];
                uint32 index = pool[anyPiece(rng)];
                if (anyOpen && !flat.empty())
                    index = flat[std::uniform_int_distribution<size_t>(0, flat.size() - 1)(rng)];
                layout->pieces[pi * PIECES + pj] = index;
            }
        }

        // The relief: each piece's own, its shared edges averaged, blended near the seams.
        std::vector<float> relief(NODES * NODES, 0.0f), count(NODES * NODES, 0.0f);
        for (int pi = 0; pi < PIECES; ++pi)
        {
            for (int pj = 0; pj < PIECES; ++pj)
            {
                Piece const& piece = sLibrary[layout->pieces[pi * PIECES + pj]];
                for (int i = 0; i < PIECE_NODES; ++i)
                {
                    for (int j = 0; j < PIECE_NODES; ++j)
                    {
                        int const n = (pi * 16 + i) * NODES + pj * 16 + j;
                        relief[n] += piece.h[i * PIECE_NODES + j];
                        count[n] += 1.0f;
                    }
                }
            }
        }
        for (int n = 0; n < NODES * NODES; ++n)
            relief[n] /= count[n];
        auto seamDistance = [](int k)
        {
            int const m = k % 16;
            return std::min(m, 16 - m);
        };
        for (int pass = 0; pass < SEAM_PASSES; ++pass)
        {
            std::vector<float> next = relief;
            for (int i = 1; i < NODES - 1; ++i)
            {
                for (int j = 1; j < NODES - 1; ++j)
                {
                    if (std::min(seamDistance(i), seamDistance(j)) > SEAM_BAND)
                        continue;
                    float sum = 0.0f;
                    for (int di = -1; di <= 1; ++di)
                        for (int dj = -1; dj <= 1; ++dj)
                            sum += relief[(i + di) * NODES + j + dj];
                    next[i * NODES + j] = sum / 9.0f;
                }
            }
            relief.swap(next);
        }

        // The walls: hills rising round the open region, steeply, uneven along their length.
        std::vector<float> openness(NODES * NODES, 0.0f);
        for (int i = 0; i < NODES; ++i)
        {
            for (int j = 0; j < NODES; ++j)
            {
                bool o = false;
                for (int di = -1; di <= 0 && !o; ++di)
                {
                    for (int dj = -1; dj <= 0 && !o; ++dj)
                    {
                        // A node on a chunk's edge touches the chunks either side.
                        int const a = std::min(std::max(i + di, 0) / 8, CHUNKS - 1);
                        int const b = std::min(std::max(j + dj, 0) / 8, CHUNKS - 1);
                        o = open[a * CHUNKS + b] != 0;
                    }
                }
                openness[i * NODES + j] = o ? 1.0f : 0.0f;
            }
        }
        Blur(openness, cave ? 1 : WALL_BLUR, cave ? 1 : 2);
        constexpr int NOISE = 9;
        std::uniform_real_distribution<float> unevenness(cave ? 0.9f : 0.7f, cave ? 1.1f : 1.3f);
        float noise[NOISE][NOISE];
        for (auto& row : noise)
            for (float& v : row)
                v = unevenness(rng);
        layout->q.resize(NODES * NODES);
        for (int i = 0; i < NODES; ++i)
        {
            for (int j = 0; j < NODES; ++j)
            {
                float const fi = float(i) / 32.0f, fj = float(j) / 32.0f;
                int const ni = std::min(int(fi), NOISE - 2), nj = std::min(int(fj), NOISE - 2);
                float const ti = fi - ni, tj = fj - nj;
                float const uneven = (noise[ni][nj] * (1 - tj) + noise[ni][nj + 1] * tj) * (1 - ti) +
                                     (noise[ni + 1][nj] * (1 - tj) + noise[ni + 1][nj + 1] * tj) * ti;
                float const closed = 1.0f - openness[i * NODES + j];
                float const h = cave ? relief[i * NODES + j] * CAVE_RELIEF + CAVE_WALL * std::min(1.0f, closed * 1.6f) * uneven
                                     : relief[i * NODES + j] + HILLS * std::pow(closed, 1.5f) * uneven;
                float const steps = std::round(h * HEIGHT_STEP);
                layout->q[i * NODES + j] = int16(std::min(32767.0f, std::max(-32767.0f, steps)));
            }
        }

        auto centre = [&layout](int c, float& x, float& y, float& z)
        {
            int const i = (c / CHUNKS) * 8 + 4, j = (c % CHUNKS) * 8 + 4;
            x = layout->X(float(i));
            y = layout->Y(float(j));
            z = layout->H(i, j);
        };
        // The way back stands at the start's centre; one arriving stands two nodes south of it.
        centre(start, layout->backX, layout->backY, layout->backZ);
        {
            int const i = (start / CHUNKS) * 8 + 6, j = (start % CHUNKS) * 8 + 4;
            layout->startX = layout->X(float(i));
            layout->startY = layout->Y(float(j));
            layout->startZ = layout->H(i, j);
        }
        centre(exit, layout->exitX, layout->exitY, layout->exitZ);

        // A main area's side areas: at its dead ends, as far off the way from start to exit as
        // it has (the way: back from the exit along falling distances).
        std::vector<int> branchChunks;
        if (AREAS[kind].branches[0] != MAX_KIND)
        {
            std::vector<int> fromPath(CHUNKS * CHUNKS, -1);
            std::deque<int> queue;
            for (int c = exit; ; )
            {
                fromPath[c] = 0;
                queue.push_back(c);
                if (c == start)
                    break;
                int next = -1;
                int const ci = c / CHUNKS, cj = c % CHUNKS;
                int const nb[4][2] = { { ci + 1, cj }, { ci - 1, cj }, { ci, cj + 1 }, { ci, cj - 1 } };
                for (auto const& n : nb)
                {
                    if (n[0] < 0 || n[1] < 0 || n[0] >= CHUNKS || n[1] >= CHUNKS)
                        continue;
                    int const k = n[0] * CHUNKS + n[1];
                    if (open[k] && dist[k] >= 0 && dist[k] == dist[c] - 1)
                        next = k;
                }
                if (next < 0)
                    break;
                c = next;
            }
            while (!queue.empty())
            {
                int const c = queue.front();
                queue.pop_front();
                int const ci = c / CHUNKS, cj = c % CHUNKS;
                int const nb[4][2] = { { ci + 1, cj }, { ci - 1, cj }, { ci, cj + 1 }, { ci, cj - 1 } };
                for (auto const& n : nb)
                {
                    if (n[0] < 0 || n[1] < 0 || n[0] >= CHUNKS || n[1] >= CHUNKS)
                        continue;
                    int const k = n[0] * CHUNKS + n[1];
                    if (open[k] && fromPath[k] < 0)
                    {
                        fromPath[k] = fromPath[c] + 1;
                        queue.push_back(k);
                    }
                }
            }
            for (int b = 0; b < 2; ++b)
            {
                int best = -1;
                for (int c = 0; c < CHUNKS * CHUNKS; ++c)
                {
                    if (!open[c] || c == start || c == exit || fromPath[c] < 0)
                        continue;
                    bool apart = true;
                    for (int other : branchChunks)
                        if (std::abs(c / CHUNKS - other / CHUNKS) + std::abs(c % CHUNKS - other % CHUNKS) < BRANCH_APART)
                            apart = false;
                    if (apart && (best < 0 || fromPath[c] > fromPath[best]))
                        best = c;
                }
                if (best < 0)
                    break;
                branchChunks.push_back(best);
                Layout::Branch& branch = layout->branches[b];
                branch.kind = AREAS[kind].branches[b];
                centre(best, branch.x, branch.y, branch.z);
            }
        }

        // Packs: on open chunks a walk from the start, apart, not at the exit or a branch.
        std::vector<int> candidates;
        for (int c = 0; c < CHUNKS * CHUNKS; ++c)
            if (open[c] && dist[c] >= (side ? PACK_FROM_START / 2 : PACK_FROM_START) && c != exit)
                candidates.push_back(c);
        std::shuffle(candidates.begin(), candidates.end(), rng);
        std::vector<int> chosen;
        for (int c : candidates)
        {
            if (chosen.size() >= (side ? SIDE_PACKS : PACKS))
                break;
            bool apart = true;
            for (int other : chosen)
                if (std::abs(c / CHUNKS - other / CHUNKS) + std::abs(c % CHUNKS - other % CHUNKS) < PACK_APART)
                    apart = false;
            if (std::abs(c / CHUNKS - exit / CHUNKS) + std::abs(c % CHUNKS - exit % CHUNKS) < PACK_APART)
                apart = false;
            for (int b : branchChunks)
                if (std::abs(c / CHUNKS - b / CHUNKS) + std::abs(c % CHUNKS - b % CHUNKS) < 2)
                    apart = false;
            if (!apart)
                continue;
            chosen.push_back(c);
            std::array<float, 3> spot;
            centre(c, spot[0], spot[1], spot[2]);
            layout->spots.push_back(spot);
        }
        return layout;
    }

    // A free block: not a live instance's, nor a layout's on its way.
    int FreeBlock()
    {
        // Under sLock.
        std::set<int> used;
        for (auto const& [instance, areas] : sInstances)
            for (LayoutPtr const& layout : areas)
                used.insert(layout->block);
        for (auto const& [guid, pr] : sPlayers)
            if (pr.pending)
                used.insert(pr.pending->block);
        for (int step = 0; step < BLOCKS; ++step)
        {
            int const block = (sNextBlock + step) % BLOCKS;
            if (!used.count(block))
            {
                sNextBlock = (block + 1) % BLOCKS;
                return block;
            }
        }
        return -1;
    }

    // An area's creatures: its named ones the world has, else Elwynn's hostiles.
    std::vector<uint32> NativesOf(RiftKind kind)
    {
        static std::mutex lock;
        static std::vector<uint32> natives[MAX_KIND];
        static bool built[MAX_KIND] = {};
        std::lock_guard<std::mutex> guard(lock);
        if (!built[kind])
        {
            built[kind] = true;
            for (char const* name : AREAS[kind].natives)
                if (name)
                    if (uint32 const entry = ActEntryNamed(name))
                        natives[kind].push_back(entry);
            if (natives[kind].empty())
                natives[kind] = NativesWithin(SRC_MAP, NATIVE_MIN_X, NATIVE_MAX_X, NATIVE_MIN_Y, NATIVE_MAX_Y, NATIVE_MIN_SPAWNS);
        }
        return natives[kind];
    }

    void SendLayout(Player* player, Layout const& layout)
    {
        WorldPacket data(SMSG_ARPG_RIFT, 1 + 4 + 2 + 2 + 2 + 4 + PIECES * PIECES * 4);
        data << uint8(1) << uint32(layout.id) << uint8(layout.gx0) << uint8(layout.gy0) << uint16(NODES) << uint16(SRC_ZONE)
             << float(BASE);
        {
            std::lock_guard<std::mutex> guard(sLock);
            for (uint32 index : layout.pieces)
            {
                Piece const& piece = sLibrary[index];
                data << uint8(piece.gx) << uint8(piece.gy) << uint8(piece.ca) << uint8(piece.cb);
            }
        }
        data << uint8(AREAS[layout.kind].cave ? 1 : 0);    // the theme
        // The marks the client's map shows once seen: 1 the exit, 2 the way back, 3 a side area's
        // entrance, 4 a side area's boss.
        AreaDef const& area = AREAS[layout.kind];
        std::vector<std::tuple<uint8, float, float>> marks;
        if (area.side)
            marks.emplace_back(4, layout.exitX, layout.exitY);
        else
            marks.emplace_back(1, layout.exitX, layout.exitY);
        if (layout.kind != KIND_FREE)
            marks.emplace_back(2, layout.backX, layout.backY);
        for (Layout::Branch const& branch : layout.branches)
            if (branch.kind != MAX_KIND)
                marks.emplace_back(3, branch.x, branch.y);
        data << uint8(marks.size());
        for (auto const& [kind, x, y] : marks)
            data << uint8(kind) << float(x) << float(y);
        player->GetSession()->SendPacket(data);
        for (int row = 0; row < NODES; row += ROWS_PER_PACKET)
        {
            int const rows = std::min(ROWS_PER_PACKET, NODES - row);
            WorldPacket heights(SMSG_ARPG_RIFT, 9 + rows * NODES * 2);
            heights << uint8(2) << uint32(layout.id) << uint16(row) << uint16(rows);
            for (int n = row * NODES; n < (row + rows) * NODES; ++n)
                heights << int16(layout.q[n]);
            player->GetSession()->SendPacket(heights);
        }
    }

    GameObject* PlaceObject(Map* map, uint32 entry, float x, float y, float z, uint32 secs, float ori = -1.0f)
    {
        if (!sGOStorage.LookupEntry<GameObjectInfo>(entry))
            return nullptr;
        GameObject* go = new GameObject;
        uint32 const low = map->GenerateLocalLowGuid(HIGHGUID_GAMEOBJECT);
        if (!go->Create(low, low, entry, map, x, y, z, ori >= 0.0f ? ori : frand(0.0f, 2.0f * float(M_PI))))
        {
            delete go;
            return nullptr;
        }
        go->SetSpellId(0);
        go->SetRespawnTime(secs);
        go->SetFlag(GAMEOBJECT_FLAGS, GO_FLAG_NO_INTERACT);
        map->Add(go);
        go->AIM_Initialize();
        return go;
    }

    // The area's creatures, its exit and its way back: once a layout.
    void Populate(Player* player, Layout const& layout)
    {
        Map* map = player->GetMap();
        AreaDef const& area = AREAS[layout.kind];
        // Its grids stay loaded while the instance lasts, so the packs far off stay too.
        for (int g = 0; g < 4; ++g)
            map->ForceLoadGrid(layout.X(float((g / 2) * 128 + 64)), layout.Y(float((g % 2) * 128 + 64)));
        // A side area ends at its boss: no exit, only the way it came.
        uint32 const exitEntry = EXIT_ENTRIES[layout.kind];
        if (!area.side && sObjectReady.count(exitEntry))
            PlaceObject(map, exitEntry, layout.exitX, layout.exitY, layout.exitZ, EXIT_SECS);
        uint32 const backEntry = area.side ? WAY_OUT_ENTRY : BACK_ENTRY;
        if (layout.kind != KIND_FREE && sObjectReady.count(backEntry))
            PlaceObject(map, backEntry, layout.backX, layout.backY, layout.backZ, EXIT_SECS);
        for (Layout::Branch const& branch : layout.branches)
            if (branch.kind != MAX_KIND && sObjectReady.count(AREAS[branch.kind].door))
                PlaceObject(map, AREAS[branch.kind].door, branch.x, branch.y, branch.z, EXIT_SECS);

        std::vector<uint32> const natives = NativesOf(layout.kind);
        if (natives.empty())
        {
            sLog.outError("ARPG rifts: no creatures for %s; an empty area", area.name);
            return;
        }
        uint32 level = player->GetLevel();
        if (area.maxLevel)
            level = std::min(std::max(level, area.minLevel), area.maxLevel);
        auto summon = [&](uint32 entry, float x, float y, float z, uint32 atLevel) -> Creature*
        {
            float const ground = map->GetHeight(x, y, z + 30.0f);
            TempSpawnSettings settings;
            settings.entry = entry;
            settings.x = x;
            settings.y = y;
            settings.z = (ground > INVALID_HEIGHT ? ground : z) + 0.5f;
            settings.ori = frand(0.0f, 2.0f * float(M_PI));
            settings.spawnType = TEMPSPAWN_CORPSE_TIMED_DESPAWN;
            settings.corpseDespawnTime = PACK_CORPSE_MS;
            settings.despawnTime = PACK_CORPSE_MS;
            settings.level = atLevel;
            return WorldObject::SummonCreature(settings, map);
        };
        auto anyNative = [&natives]()
        {
            return natives[urand(0, uint32(natives.size() - 1))];
        };
        for (auto const& spot : layout.spots)
        {
            if (Creature* leader = summon(anyNative(), spot[0] + frand(-6.0f, 6.0f), spot[1] + frand(-6.0f, 6.0f), spot[2], level))
            {
                FormRoadPack(leader);
                OnShrinePackPlaced(leader);
            }
        }
        // A named one halfway, crowned as the act crowns it (Arpg/ArpgActs.h).
        if (area.midLeader && !layout.spots.empty())
        {
            auto const& spot = layout.spots[layout.spots.size() / 2];
            if (uint32 const entry = ActEntryNamed(area.midLeader))
                if (Creature* named = summon(entry, spot[0] + 8.0f, spot[1], spot[2], level + 1))
                    CrownActLeader(named);
        }
        // The exit's guard, with a pack: the act's leader of the area, else a rare.
        float const angle = frand(0.0f, 2.0f * float(M_PI));
        float const bx = layout.exitX + std::cos(angle) * 8.0f, by = layout.exitY + std::sin(angle) * 8.0f;
        uint32 const named = area.exitLeader ? ActEntryNamed(area.exitLeader) : 0;
        Creature* boss = nullptr;
        if (named)
        {
            if (Creature* guardPack = summon(anyNative(), layout.exitX - std::cos(angle) * 10.0f, layout.exitY - std::sin(angle) * 10.0f,
                                             layout.exitZ, level))
                FormRoadPack(guardPack);
            if ((boss = summon(named, bx, by, layout.exitZ, std::min<uint32>(level + 1, 63))))
            {
                CrownActLeader(boss);
                // A named one the act doesn't crown (Gruff Swiftbite, Mother Fang): a rare.
                if (!ChampionTier(boss))
                    MakeChampion(boss, 2, ChampionAffixCount(boss->GetLevel(), true), boss->GetName());
            }
        }
        else if ((boss = summon(anyNative(), bx, by, layout.exitZ, std::min<uint32>(level + 1, 63))))
            MakeChampion(boss, 2, ChampionAffixCount(boss->GetLevel(), true));
        // A side area's boss: its perk the first time it falls, and a hoard on its corpse.
        if (boss && area.side)
        {
            std::lock_guard<std::mutex> guard(sLock);
            sSideBosses[boss->GetObjectGuid()] = { map->GetInstanceId(), area.perk };
            sHoards[boss->GetObjectGuid()] = map->GetInstanceId();
        }
    }

    void LeaveTo(Player* player, int stone)
    {
        StoneAt const& at = STONES[stone];
        float const angle = frand(0.0f, 2.0f * float(M_PI));
        float z;
        {
            std::lock_guard<std::mutex> guard(sLock);
            z = sStoneZ[stone];
        }
        player->TeleportTo(at.map, at.x + std::cos(angle) * STONE_RETURN, at.y + std::sin(angle) * STONE_RETURN, z + 1.0f,
                           angle);
    }

    // Where a gate stands, and where one coming back through it arrives.
    void GateAt(int gate, float inward, float& x, float& y)
    {
        float const r = HUB_RADIUS - GATE_IN - inward;
        x = HUB_X + std::cos(GATES[gate].bearing) * r;
        y = HUB_Y + std::sin(GATES[gate].bearing) * r;
    }

    // Back to Goldshire, inside the gate of `kind`'s area.
    void LeaveToHub(Player* player, RiftKind kind)
    {
        int gate = 0;
        for (int g = 0; g < MAX_GATE; ++g)
            if (GATES[g].kind == kind)
                gate = g;
        float x, y, z;
        GateAt(gate, GATE_ARRIVE, x, y);
        {
            std::lock_guard<std::mutex> guard(sLock);
            z = sGateZ[gate];
        }
        player->TeleportTo(0, x, y, z + 2.0f, GATES[gate].bearing + float(M_PI));
    }

    // Out of an area or rift, the way it was entered.
    void LeaveFor(Player* player, RiftKind kind, int stone)
    {
        if (kind == KIND_FREE)
            LeaveTo(player, stone);
        else
            LeaveToHub(player, kind);
    }

    int HomeStone(Player const* player)
    {
        return player->GetTeam() == HORDE ? 1 : 0;
    }

    // A new layout of `kind`, its block taken. Under sLock.
    LayoutPtr NewLayout(Player* player, RiftKind kind, Layout const* parent = nullptr, Layout::Branch const* door = nullptr)
    {
        BuildLibrary();
        int const block = FreeBlock();
        if (block < 0)
        {
            ChatHandler(player).PSendSysMessage("|cffffd100%s:|r every rift is open already; try again soon.", AREAS[kind].name);
            return nullptr;
        }
        std::shared_ptr<Layout> made = MakeLayout(block, urand(1, 0x7fffffff), kind);
        if (!made)
        {
            sLog.outError("ARPG rifts: no pieces to make a rift of (are map 0's .map files there?)");
            return nullptr;
        }
        if (parent && door)
        {
            made->parentId = parent->id;
            made->returnX = door->x + 6.0f;
            made->returnY = door->y;
            made->returnZ = parent->H(int(((32 - parent->gx0) * TILE - made->returnX) / UNIT + 0.5f),
                                      int(((32 - parent->gy0) * TILE - made->returnY) / UNIT + 0.5f));
        }
        sBlocks[block] = made;
        return made;
    }

    // From the world (a stone or a gate): into a new instance, or a party member's.
    void EnterArea(Player* player, RiftKind kind, int stone)
    {
        // All on the world thread, before the maps update: a party member in a rift is read
        // there (its map is not updating), the binds change there (the instances' records are
        // its), then the way in.
        ObjectGuid const guid = player->GetObjectGuid();
        sWorld.GetMessager().AddMessage([guid, kind, stone](World* /*world*/)
        {
            Player* player = sObjectMgr.GetPlayer(guid);
            if (!player || !player->IsInWorld() || player->IsBeingTeleported())
                return;
            LayoutPtr layout;
            ObjectGuid joinGuid;
            {
                std::lock_guard<std::mutex> guard(sLock);
                // A party member in a rift: its instance, and its area when it is this one.
                if (Group* group = player->GetGroup())
                {
                    for (GroupReference* ref = group->GetFirstMember(); ref && joinGuid.IsEmpty(); ref = ref->next())
                    {
                        Player* member = ref->getSource();
                        if (!member || member == player || !member->IsInWorld() || member->GetMapId() != RIFT_MAP)
                            continue;
                        auto it = sInstances.find(member->GetInstanceId());
                        if (it == sInstances.end())
                            continue;
                        joinGuid = member->GetObjectGuid();
                        for (LayoutPtr const& l : it->second)
                            if (l->kind == kind && l->Inside(member->GetPositionX(), member->GetPositionY()))
                                layout = l;
                    }
                }
                if (!layout)
                    layout = NewLayout(player, kind);
                if (!layout)
                    return;
                PlayerRift& pr = sPlayers[guid];
                pr.pending = layout;
                pr.returnStone = stone;
                pr.sentId = layout->id;
            }
            player->UnbindInstance(RIFT_MAP);
            Player* member = joinGuid.IsEmpty() ? nullptr : sObjectMgr.GetPlayer(joinGuid);
            if (member && member->IsInWorld() && member->GetMapId() == RIFT_MAP)
            {
                if (DungeonPersistentState* state = dynamic_cast<DungeonPersistentState*>(member->GetMap()->GetPersistentState()))
                    player->BindToInstance(state, false);
            }
            else if (Group* group = player->GetGroup())
                group->UnbindInstance(RIFT_MAP);
            SendLayout(player, *layout);
            ChatHandler(player).PSendSysMessage("|cffffd100%s:|r %s", AREAS[layout->kind].name, AREAS[layout->kind].text);
            if (!player->TeleportTo(RIFT_MAP, layout->startX, layout->startY, layout->startZ + 1.0f, 0.0f))
            {
                std::lock_guard<std::mutex> guard(sLock);
                PlayerRift& pr = sPlayers[player->GetObjectGuid()];
                pr.pending = nullptr;
                pr.sentId = 0;
            }
        });
    }

    // From an area's exit: the next area, in the same instance (its own block of the map).
    void ChainTo(Player* player, RiftKind kind)
    {
        LayoutPtr layout;
        {
            std::lock_guard<std::mutex> guard(sLock);
            // A party member there already: its area.
            auto it = sInstances.find(player->GetInstanceId());
            if (Group* group = player->GetGroup())
                for (GroupReference* ref = group->GetFirstMember(); ref && !layout && it != sInstances.end(); ref = ref->next())
                {
                    Player* member = ref->getSource();
                    if (!member || member == player || !member->IsInWorld() || member->GetMap() != player->GetMap())
                        continue;
                    for (LayoutPtr const& l : it->second)
                        if (l->kind == kind && l->Inside(member->GetPositionX(), member->GetPositionY()))
                            layout = l;
                }
            if (!layout)
                layout = NewLayout(player, kind);
            if (!layout)
                return;
            std::vector<LayoutPtr>& areas = sInstances[player->GetInstanceId()];
            if (std::find(areas.begin(), areas.end(), layout) == areas.end())
                areas.push_back(layout);
            sPlayers[player->GetObjectGuid()].sentId = layout->id;
        }
        SendLayout(player, *layout);
        ChatHandler(player).PSendSysMessage("|cffffd100%s:|r %s", AREAS[kind].name, AREAS[kind].text);
        player->NearTeleportTo(layout->startX, layout->startY, layout->startZ + 1.0f, 0.0f);
    }

    // Into a side area from its entrance: the one of this trip if it was made, else a new one, in
    // the same instance.
    void EnterBranch(Player* player, Layout const& parent, Layout::Branch const& door)
    {
        LayoutPtr layout;
        {
            std::lock_guard<std::mutex> guard(sLock);
            std::vector<LayoutPtr>& areas = sInstances[player->GetInstanceId()];
            for (LayoutPtr const& l : areas)
                if (l->parentId == parent.id && l->kind == door.kind)
                    layout = l;
            if (!layout)
                layout = NewLayout(player, door.kind, &parent, &door);
            if (!layout)
                return;
            if (std::find(areas.begin(), areas.end(), layout) == areas.end())
                areas.push_back(layout);
            sPlayers[player->GetObjectGuid()].sentId = layout->id;
        }
        SendLayout(player, *layout);
        ChatHandler(player).PSendSysMessage("|cffffd100%s:|r %s", AREAS[door.kind].name, AREAS[door.kind].text);
        player->NearTeleportTo(layout->startX, layout->startY, layout->startZ + 1.0f, 0.0f);
    }

    // Out of a side area: back beside its entrance in the area it branches off.
    void ReturnToParent(Player* player, Layout const& side)
    {
        LayoutPtr parent;
        {
            std::lock_guard<std::mutex> guard(sLock);
            auto it = sInstances.find(player->GetInstanceId());
            if (it != sInstances.end())
                for (LayoutPtr const& l : it->second)
                    if (l->id == side.parentId)
                        parent = l;
            if (parent)
                sPlayers[player->GetObjectGuid()].sentId = parent->id;
        }
        if (!parent)
        {
            LeaveToHub(player, KIND_FIELDS);
            return;
        }
        SendLayout(player, *parent);
        player->NearTeleportTo(side.returnX, side.returnY, side.returnZ + 1.0f, 0.0f);
    }

    // Goldshire's palisade, its gates and the stones are placed near a player: ground under each.
    void PlaceHub(Map* map)
    {
        // Under no lock: placing takes the map's. Placed once, and again when any of it went.
        std::vector<ObjectGuid> placedBefore;
        bool placedOnce;
        {
            std::lock_guard<std::mutex> guard(sLock);
            placedOnce = sHubPlaced;
            placedBefore = sHubObjects;
            for (ObjectGuid const& g : sHubGates)
                if (!g.IsEmpty())
                    placedBefore.push_back(g);
        }
        if (placedOnce)
        {
            bool gone = false;
            for (ObjectGuid const& g : placedBefore)
                if (!map->GetGameObject(g))
                {
                    gone = true;
                    break;
                }
            if (!gone)
                return;
        }
        // The town's grids stay loaded, so its palisade and gates stay with them.
        for (float ox = -1.0f; ox <= 1.0f; ox += 2.0f)
            for (float oy = -1.0f; oy <= 1.0f; oy += 2.0f)
                map->ForceLoadGrid(HUB_X + ox * HUB_RADIUS, HUB_Y + oy * HUB_RADIUS);
        // All again: what is left of the last goes first.
        std::vector<ObjectGuid> old;
        {
            std::lock_guard<std::mutex> guard(sLock);
            old.swap(sHubObjects);
            for (ObjectGuid& g : sHubGates)
            {
                old.push_back(g);
                g = ObjectGuid();
            }
        }
        for (ObjectGuid const& g : old)
            if (GameObject* go = map->GetGameObject(g))
                go->Delete();
        std::vector<ObjectGuid> placed;
        ObjectGuid gates[MAX_GATE];
        float gateZ[MAX_GATE];
        for (int g = 0; g < MAX_GATE; ++g)
        {
            float x, y;
            GateAt(g, 0.0f, x, y);
            float const ground = map->GetHeight(x, y, HUB_Z + 60.0f);
            gateZ[g] = ground > INVALID_HEIGHT ? ground : HUB_Z;
            if (sObjectReady.count(GATES[g].entry))
                if (GameObject* go = PlaceObject(map, GATES[g].entry, x, y, gateZ[g], HUB_SECS))
                    gates[g] = go->GetObjectGuid();
        }
        if (sObjectReady.count(WALL_ENTRY))
        {
            int const posts = int(2.0f * float(M_PI) * HUB_RADIUS / WALL_STEP);
            for (int k = 0; k < posts; ++k)
            {
                float const a = 2.0f * float(M_PI) * float(k) / float(posts);
                float const x = HUB_X + std::cos(a) * HUB_RADIUS, y = HUB_Y + std::sin(a) * HUB_RADIUS;
                float const ground = map->GetHeight(x, y, HUB_Z + 60.0f);
                if (ground <= INVALID_HEIGHT)
                    continue;
                float const along = std::fmod(a + float(M_PI) / 2.0f, 2.0f * float(M_PI));
                if (GameObject* go = PlaceObject(map, WALL_ENTRY, x, y, ground, HUB_SECS, along))
                    placed.push_back(go->GetObjectGuid());
            }
        }
        std::lock_guard<std::mutex> guard(sLock);
        sHubPlaced = true;
        sHubObjects.swap(placed);
        for (int g = 0; g < MAX_GATE; ++g)
        {
            sHubGates[g] = gates[g];
            sGateZ[g] = gateZ[g];
        }
    }

    // Whether the palisade holds `player`: an Alliance ARPG character before Hogger falls.
    bool Walled(Player const* player)
    {
        return sWorld.getConfig(CONFIG_BOOL_ARPG_ACTS) && player->GetTeam() == ALLIANCE && !player->IsGameMaster() &&
               player->GetLevel() <= HUB_LEVEL_MAX && sObjectMgr.GetQuestTemplate(HUB_CHAPTER_DONE) &&
               !player->GetQuestRewardStatus(HUB_CHAPTER_DONE) && !player->IsTaxiFlying();
    }

    bool GateOpen(Player const* player, int gate)
    {
        GateDef const& def = GATES[gate];
        if (!def.openLevel && !def.openQuest)
            return true;
        return (def.openLevel && player->GetLevel() >= def.openLevel) || (def.openQuest && player->GetQuestRewardStatus(def.openQuest));
    }

    // On map 0: Goldshire, its palisade and gates.
    void UpdateHub(Player* player, uint32 now)
    {
        Map* map = player->GetMap();
        if (player->GetMapId() != 0 || map->Instanceable())
            return;
        float const dx = player->GetPositionX() - HUB_X, dy = player->GetPositionY() - HUB_Y;
        float const d = std::sqrt(dx * dx + dy * dy);
        bool const walled = Walled(player);
        if (d <= HUB_SHOW)
            PlaceHub(map);
        if (walled && player->IsAlive() && d > HUB_RADIUS - 2.0f)
        {
            // Far off (a character from before the palisade, a summon): into the town.
            if (d > HUB_RADIUS + 40.0f)
                player->NearTeleportTo(HUB_X, HUB_Y, HUB_Z + 2.0f, 0.0f);
            else
            {
                float const x = HUB_X + dx / d * (HUB_RADIUS - 5.0f), y = HUB_Y + dy / d * (HUB_RADIUS - 5.0f);
                float const ground = map->GetHeight(x, y, player->GetPositionZ() + 20.0f);
                player->NearTeleportTo(x, y, (ground > INVALID_HEIGHT ? ground : player->GetPositionZ()) + 0.5f,
                                       player->GetOrientation() + float(M_PI));
            }
            ChatHandler(player).PSendSysMessage("|cffffd100Goldshire:|r the way out is through the town's gates.");
            return;
        }
        if (d > HUB_RADIUS || !player->IsAlive() || player->IsInCombat())
            return;
        for (int g = 0; g < MAX_GATE; ++g)
        {
            float x, y, z;
            GateAt(g, 0.0f, x, y);
            {
                std::lock_guard<std::mutex> guard(sLock);
                z = sGateZ[g];
            }
            float const gx = player->GetPositionX() - x, gy = player->GetPositionY() - y;
            if (gx * gx + gy * gy > STONE_REACH * STONE_REACH || std::fabs(player->GetPositionZ() - z) > 6.0f)
                continue;
            {
                std::lock_guard<std::mutex> guard(sLock);
                PlayerRift& pr = sPlayers[player->GetObjectGuid()];
                if (pr.stoneAt && WorldTimer::getMSTimeDiff(pr.stoneAt, now) < STONE_COOLDOWN_MS)
                    return;
                pr.stoneAt = now ? now : 1;
            }
            if (!GateOpen(player, g))
            {
                ChatHandler(player).PSendSysMessage("|cffffd100%s:|r not open to you yet.", GATES[g].name);
                return;
            }
            if (GATES[g].kind == MAX_KIND)
                player->TeleportTo(0, WESTFALL_X, WESTFALL_Y, WESTFALL_Z, WESTFALL_O);
            else
                EnterArea(player, GATES[g].kind, -1);
            return;
        }
    }

    // On a continent: the stones near, shown and stepped on.
    void UpdateStones(Player* player, uint32 now)
    {
        Map* map = player->GetMap();
        if (map->Instanceable() || player->GetMapId() > 1)
            return;
        for (int stone = 0; stone < STONE_COUNT; ++stone)
        {
            StoneAt const& at = STONES[stone];
            if (at.map != player->GetMapId())
                continue;
            float const dx = player->GetPositionX() - at.x, dy = player->GetPositionY() - at.y;
            float const d2 = dx * dx + dy * dy;
            if (d2 > STONE_SHOW * STONE_SHOW)
                continue;
            ObjectGuid guid;
            float z;
            {
                std::lock_guard<std::mutex> guard(sLock);
                guid = sStones[stone];
                z = sStoneZ[stone];
            }
            if (sObjectReady.count(RIFT_STONE_ENTRY) && (guid.IsEmpty() || !map->GetGameObject(guid)))
            {
                float const ground = map->GetHeight(at.x, at.y, at.z + 40.0f);
                if (ground > INVALID_HEIGHT)
                    z = ground;
                if (GameObject* go = PlaceObject(map, RIFT_STONE_ENTRY, at.x, at.y, z, STONE_SECS))
                {
                    std::lock_guard<std::mutex> guard(sLock);
                    sStones[stone] = go->GetObjectGuid();
                    sStoneZ[stone] = z;
                }
            }
            if (d2 <= STONE_REACH * STONE_REACH && std::fabs(player->GetPositionZ() - z) < 5.0f && player->IsAlive() &&
                    !player->IsInCombat() && !player->IsTaxiFlying())
            {
                {
                    std::lock_guard<std::mutex> guard(sLock);
                    PlayerRift& pr = sPlayers[player->GetObjectGuid()];
                    if (pr.stoneAt && WorldTimer::getMSTimeDiff(pr.stoneAt, now) < STONE_COOLDOWN_MS)
                        return;
                    pr.stoneAt = now ? now : 1;
                }
                EnterArea(player, KIND_FREE, stone);
                return;
            }
        }
    }

    bool Near(float x, float y, float z, Player const* player, float reach)
    {
        float const dx = player->GetPositionX() - x, dy = player->GetPositionY() - y;
        return dx * dx + dy * dy <= reach * reach && std::fabs(player->GetPositionZ() - z) < 5.0f;
    }

    // In a rift: the area the player stands in bound, sent, peopled; its exit and way back.
    void UpdateInRift(Player* player, uint32 now)
    {
        uint32 const instance = player->GetInstanceId();
        float const px = player->GetPositionX(), py = player->GetPositionY();
        LayoutPtr layout, target;
        bool send = false, populate = false;
        int stone = HomeStone(player);
        {
            std::lock_guard<std::mutex> guard(sLock);
            PlayerRift& pr = sPlayers[player->GetObjectGuid()];
            std::vector<LayoutPtr>& areas = sInstances[instance];
            if (pr.pending)
            {
                if (std::find(areas.begin(), areas.end(), pr.pending) == areas.end())
                    areas.push_back(pr.pending);
                target = pr.pending;
                pr.pending = nullptr;
            }
            for (LayoutPtr const& l : areas)
                if (l->Inside(px, py))
                    layout = l;
            // Standing in none (just arrived elsewhere, a relog): the newest one's start.
            if (!layout && !target)
                for (LayoutPtr const& l : areas)
                    if (!l->parentId)
                        target = l;
            if (!layout && !target && !areas.empty())
                target = areas.back();
            if (areas.empty())
                sInstances.erase(instance);
            LayoutPtr const shown = layout ? layout : target;
            if (shown)
            {
                send = pr.sentId != shown->id;
                pr.sentId = shown->id;
            }
            if (pr.returnStone >= 0)
                stone = pr.returnStone;
            if (layout)
                populate = sPopulated.insert(layout->id).second;
        }
        if (!layout && !target)
        {
            // No layout (the server restarted, or a stale instance): out the way it came.
            if (player->GetTeam() == ALLIANCE && Walled(player))
                LeaveToHub(player, KIND_FIELDS);
            else
                LeaveTo(player, stone);
            return;
        }
        if (send)
            SendLayout(player, layout ? *layout : *target);
        if (!layout)
        {
            player->NearTeleportTo(target->startX, target->startY, target->startZ + 1.0f, 0.0f);
            return;
        }
        if (populate)
            Populate(player, *layout);
        if (!player->IsAlive())
            return;
        bool const side = AREAS[layout->kind].side;
        bool const atExit = !side && Near(layout->exitX, layout->exitY, layout->exitZ, player, EXIT_REACH);
        bool const atBack = layout->kind != KIND_FREE && Near(layout->backX, layout->backY, layout->backZ, player, EXIT_REACH);
        Layout::Branch const* door = nullptr;
        for (Layout::Branch const& branch : layout->branches)
            if (branch.kind != MAX_KIND && Near(branch.x, branch.y, branch.z, player, EXIT_REACH))
                door = &branch;
        if (!atExit && !atBack && !door)
            return;
        {
            std::lock_guard<std::mutex> guard(sLock);
            PlayerRift& pr = sPlayers[player->GetObjectGuid()];
            if (pr.stoneAt && WorldTimer::getMSTimeDiff(pr.stoneAt, now) < STONE_COOLDOWN_MS)
                return;
            pr.stoneAt = now ? now : 1;
        }
        if (door)
        {
            EnterBranch(player, *layout, *door);
            return;
        }
        if (side)
        {
            ReturnToParent(player, *layout);
            return;
        }
        RiftKind const next = AREAS[layout->kind].next;
        if (atExit && next != MAX_KIND)
            ChainTo(player, next);
        else
            LeaveFor(player, layout->kind, stone);
    }
}

namespace Arpg
{
    bool IsRiftMap(uint32 mapId)
    {
        return mapId == RIFT_MAP && sRegistered;
    }

    void RegisterRiftMap()
    {
        if (!sWorld.getConfig(CONFIG_BOOL_ARPG_ENABLE))
            return;
        if (RIFT_MAP >= sMapStore.GetNumRows())
        {
            sLog.outError("ARPG rifts: Map.dbc has no room for map %u; no rifts", RIFT_MAP);
            return;
        }
        if (sMapStore.LookupEntry(RIFT_MAP))
        {
            sLog.outError("ARPG rifts: map %u is in Map.dbc already; no rifts", RIFT_MAP);
            return;
        }
        sRiftEntry = MapEntry();
        sRiftEntry.MapID = RIFT_MAP;
        sRiftEntry.map_type = MAP_INSTANCE;
        for (char*& name : sRiftEntry.name)
            name = sRiftName;
        sRiftEntry.linked_zone = 0;
        sRiftEntry.multimap_id = 0;
        sMapStore.InsertEntry(&sRiftEntry, RIFT_MAP);
        // Layout ids name the client's tile directory: never the same twice, restarts included.
        sNextId = uint32(time(nullptr));
        // Its instance template: levels 1 to 60, five players, a ghost back to Goldshire.
        WorldDatabase.DirectPExecute("REPLACE INTO instance_template (map, parent, levelMin, levelMax, maxPlayers, reset_delay, "
                                     "ghostEntranceMap, ghostEntranceX, ghostEntranceY, ScriptName, mountAllowed) "
                                     "VALUES (%u, 0, 1, 60, 5, 0, %u, %f, %f, '', 1)",
                                     RIFT_MAP, STONES[0].map, STONES[0].x, STONES[0].y);
        sRegistered = true;
    }

    void EnsureRiftObjects()
    {
        if (!sRegistered)
            return;
        // The look: a vanilla portal's.
        std::vector<uint32> looks;
        if (auto result = WorldDatabase.PQuery("SELECT displayId FROM gameobject_template WHERE displayId > 0 AND entry < %u AND "
                                               "name LIKE '%%Portal%%' GROUP BY displayId ORDER BY MIN(entry) LIMIT 2",
                                               SHRINE_ENTRY_FROM))
        {
            do
                looks.push_back(result->Fetch()[0].GetUInt32());
            while (result->NextRow());
        }
        if (looks.empty())
        {
            sLog.outError("ARPG rifts: no portal look in gameobject_template; no Rift Stones");
            return;
        }
        // The palisade's look: a vanilla fence's or palisade's, if the world has one.
        uint32 wallLook = 0;
        if (auto result = WorldDatabase.PQuery("SELECT displayId FROM gameobject_template WHERE displayId > 0 AND entry < %u AND "
                                               "(name LIKE '%%Palisade%%' OR name LIKE '%%Fence%%' OR name LIKE '%%Barricade%%') "
                                               "ORDER BY name LIKE '%%Palisade%%' DESC, entry LIMIT 1", SHRINE_ENTRY_FROM))
            wallLook = result->Fetch()[0].GetUInt32();
        struct Def
        {
            uint32 entry;
            char const* name;
            uint32 look;
        };
        uint32 const portal = looks[0], other = looks[looks.size() > 1 ? 1 : 0];
        std::vector<Def> defs = { { RIFT_STONE_ENTRY, "Elwynn Rift", portal }, { RIFT_EXIT_ENTRY, "Rift Exit", other },
                                  { BACK_ENTRY, "To Goldshire", other }, { 98018, "Onward: The Farmlands", portal },
                                  { 98019, "Onward: Forest's Edge", portal }, { 98020, "Back to Goldshire", other } };
        for (GateDef const& gate : GATES)
            defs.push_back({ gate.entry, gate.name, portal });
        for (AreaDef const& area : AREAS)
            if (area.side)
                defs.push_back({ area.door, area.name, other });
        defs.push_back({ WAY_OUT_ENTRY, "Way Out", other });
        if (wallLook)
            defs.push_back({ WALL_ENTRY, "Goldshire Palisade", wallLook });
        else
            sLog.outError("ARPG rifts: no fence or palisade look in gameobject_template; Goldshire's palisade is unseen");
        for (Def const& def : defs)
        {
            std::string name = def.name;
            for (size_t at = name.find('\''); at != std::string::npos; at = name.find('\'', at + 2))
                name.insert(at, 1, '\'');
            if (auto result = WorldDatabase.PQuery("SELECT name FROM gameobject_template WHERE entry = %u", def.entry))
            {
                std::string const was = result->Fetch()[0].GetCppString();
                if (was != def.name)
                {
                    sLog.outError("ARPG rifts: gameobject_template %u is '%s', not the rift's; left be", def.entry, was.c_str());
                    continue;
                }
            }
            WorldDatabase.DirectPExecute("DELETE FROM gameobject_template WHERE entry = %u", def.entry);
            WorldDatabase.DirectPExecute("INSERT INTO gameobject_template (entry, type, displayId, name, flags, size) "
                                         "VALUES (%u, 5, %u, '%s', %u, 1)",
                                         def.entry, def.look, name.c_str(), uint32(GO_FLAG_NO_INTERACT));
            sObjectReady.insert(def.entry);
        }
    }

    void FillRiftGrid(uint32 gx, uint32 gy, GridMap& grid)
    {
        if (int(gx) < BLOCK_FIRST || int(gy) < BLOCK_FIRST)
            return;
        int const bx = (int(gx) - BLOCK_FIRST) / BLOCK_STEP, by = (int(gy) - BLOCK_FIRST) / BLOCK_STEP;
        int const ox = (int(gx) - BLOCK_FIRST) % BLOCK_STEP, oy = (int(gy) - BLOCK_FIRST) % BLOCK_STEP;
        if (ox > 1 || oy > 1 || bx >= BLOCK_SIDE || by >= BLOCK_SIDE)
            return;
        LayoutPtr layout;
        {
            std::lock_guard<std::mutex> guard(sLock);
            layout = sBlocks[by * BLOCK_SIDE + bx];
        }
        if (!layout)
            return;
        std::vector<float> v9(129 * 129), v8(128 * 128);
        int const offA = ox * 128, offB = oy * 128;
        for (int a = 0; a < 129; ++a)
            for (int b = 0; b < 129; ++b)
                v9[a * 129 + b] = layout->H(offA + a, offB + b);
        for (int a = 0; a < 128; ++a)
            for (int b = 0; b < 128; ++b)
                v8[a * 128 + b] = (v9[a * 129 + b] + v9[(a + 1) * 129 + b] + v9[a * 129 + b + 1] + v9[(a + 1) * 129 + b + 1]) / 4.0f;
        grid.loadSynthetic(v9.data(), v8.data(), ElwynnFlag());
    }

    void UpdateRift(Player* player)
    {
        if (!RiftsOn() || !player || !player->IsInWorld() || !Active(player) || player->IsBeingTeleported())
            return;
        uint32 const now = WorldTimer::getMSTime();
        {
            std::lock_guard<std::mutex> guard(sLock);
            PlayerRift& pr = sPlayers[player->GetObjectGuid()];
            if (pr.checkedAt && WorldTimer::getMSTimeDiff(pr.checkedAt, now) < CHECK_MS)
                return;
            pr.checkedAt = now ? now : 1;
        }
        if (player->GetMapId() == RIFT_MAP)
            UpdateInRift(player, now);
        else
        {
            UpdateHub(player, now);
            UpdateStones(player, now);
        }
    }

    void ForgetRift(Player* player)
    {
        if (!player)
            return;
        std::lock_guard<std::mutex> guard(sLock);
        sPlayers.erase(player->GetObjectGuid());
    }

    void OnRiftKill(Player* killer, Unit* victim)
    {
        if (!killer || !victim || !IsRiftMap(victim->GetMapId()))
            return;
        PerkId perk = PERK_NONE;
        {
            std::lock_guard<std::mutex> guard(sLock);
            auto it = sSideBosses.find(victim->GetObjectGuid());
            if (it == sSideBosses.end() || it->second.first != victim->GetInstanceId())
                return;
            perk = it->second.second;
            sSideBosses.erase(it);
        }
        // The party there shares it, as Diablo II's quest rewards are.
        std::vector<Player*> credited;
        if (Group* group = killer->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                if (Player* member = ref->getSource())
                    if (member->IsInMap(victim) && member->IsAtGroupRewardDistance(victim))
                        credited.push_back(member);
        }
        else
            credited.push_back(killer);
        for (Player* player : credited)
            if (Active(player))
                GrantPerk(player, perk);
    }

    bool OnRiftBossLoot(Creature* victim)
    {
        if (!victim || !victim->m_loot || !IsRiftMap(victim->GetMapId()))
            return false;
        {
            std::lock_guard<std::mutex> guard(sLock);
            auto it = sHoards.find(victim->GetObjectGuid());
            if (it == sHoards.end() || it->second != victim->GetInstanceId())
                return false;
            sHoards.erase(it);
        }
        // The hoard: two blues, and a third half the time.
        uint32 const level = victim->GetLevel() + 2;
        AddLootPick(victim->m_loot, RARITY_BLUE, level);
        AddLootPick(victim->m_loot, RARITY_BLUE, level);
        if (roll_chance_i(50))
            AddLootPick(victim->m_loot, RARITY_BLUE, level);
        return true;
    }

    void ForgetRiftMap(Map const* map)
    {
        if (!map || map->GetId() != RIFT_MAP)
            return;
        std::lock_guard<std::mutex> guard(sLock);
        uint32 const instance = map->GetInstanceId();
        for (auto b = sSideBosses.begin(); b != sSideBosses.end();)
            b = b->second.first == instance ? sSideBosses.erase(b) : std::next(b);
        for (auto h = sHoards.begin(); h != sHoards.end();)
            h = h->second == instance ? sHoards.erase(h) : std::next(h);
        auto it = sInstances.find(instance);
        if (it == sInstances.end())
            return;
        for (LayoutPtr const& layout : it->second)
            sPopulated.erase(layout->id);
        sInstances.erase(it);
    }
}
