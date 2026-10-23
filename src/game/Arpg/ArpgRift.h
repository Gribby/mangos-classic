/*
 * ARPG rifts: Diablo II's random outdoor areas, made of pieces of Elwynn Forest (the client's
 * docs/ARPG-RANDOM-AREAS.md).
 *
 * A rift is an instance of a map of its own (RIFT_MAP, defined here: in no DBC), entered by
 * stepping onto a Rift Stone (one in Goldshire, one in Razor Hill). Each entry makes a new layout:
 *   - a piece library, built once from the server's own map 0 grids: every 2x2-chunk square
 *     (about 67 yards) of Elwynn's ground with no water, no holes and outside its towns, flat
 *     ones (10 yards of rise at most) marked;
 *   - a cave-like open region on a 32x32-chunk field (cellular automaton, the largest region
 *     kept), a start on its west side and an exit the farthest walk from it;
 *   - a piece for each 2x2 chunks, flat where the region is open; its relief, levelled to a
 *     common base and blended at the seams; hills rising steeply round the open region as walls.
 * The layout fills a block of 2x2 grids of RIFT_MAP in memory (Maps/GridMap.h's loadSynthetic);
 * instances' blocks differ, so they share the map's terrain without meeting. The client gets
 * the pieces and the final heights (SMSG_ARPG_RIFT) and builds the area's tiles from the real
 * Elwynn chunks (textures and doodads) under those heights.
 *
 * In a rift: packs of Elwynn's natives at the player's level, a rare at the exit, a portal
 * there back to the stone. No vmaps or mmaps: creatures walk straight.
 *
 * Goldshire is Act I's hub, as Diablo II's Rogue Encampment: Alliance characters start there, and
 * a palisade holds them in until Hogger falls (the act's Hogger quest done). Its gates lead into
 * three areas, each a layout of pieces from its part of Elwynn with the act quests' creatures:
 * the Northshire Fields (north gate, levels 1-5, Garrick Padfoot at the end), the Farmlands
 * (east gate, 5-8, Goldtooth and Morgan the Collector), Forest's Edge (south gate, 8-11, Hogger).
 * An area's exit leads into the next, in the same instance (its own block of the map); the last
 * one's leads back to Goldshire, and each area's start has a way back. A gate opens at its
 * level or once the quest before it is done; the west gate, the road to Westfall, once Hogger is.
 *
 * SMSG_ARPG_RIFT (0x356):
 *   kind 1, the layout: uint8 1, uint32 layout id, uint8 gx0, uint8 gy0 (the block's first
 *     server grid; node (i, j) stands at X = (32 - gx0) * TILE - i * TILE / 128, Y likewise from
 *     gy0 and j), uint16 nodes a side (257), uint16 area id, float base height, then 16x16 pieces
 *     (i-major, i along X): uint8 source gx, gy, chunk a, chunk b (the source square's first
 *     chunk in that map 0 grid, a along X).
 *     Then (newer servers) uint8 theme (0 outdoors, 1 a roofless cave), uint8 marks, and per
 *     mark uint8 kind (1 exit, 2 way back, 3 side area's entrance, 4 side area's boss), float x, y;
 *     then uint8 sites, and per site uint8 kind (1 farmstead, 2 crop plot, 3 gnoll camp,
 *     4 watchtower, 5 logging site, 6 Defias camp, 7 wolf den), float x, y, z (its levelled
 *     ground), orientation, radius; then uint8 flavour (0 none, 1 webs, 2 candles, 3 dig works,
 *     4 burnt farms, 5 wolf woods, 6 warren); then uint8 sites again, and per site its building:
 *     a string (the vmaps model name, which is the WMO's file name; empty where it raised none, and
 *     the client dresses the site from its own pick, without collision) and float z
 *     (its origin's height). The server's collision is that model's, standing at the site's x, y
 *     and that z, turned the site's orientation.
 *   kind 2, heights: uint8 2, uint32 layout id, uint16 first row, uint16 rows, then rows x 257
 *     int16: (height - base) * 8, row i, column j. An inner (cell centre) height is its four
 *     corners' mean.
 */

#ifndef MANGOS_ARPG_RIFT_H
#define MANGOS_ARPG_RIFT_H

#include "Common.h"

#include <vector>

class GridMap;
class Creature;
class Map;
class Unit;
class Player;
namespace G3D { class Vector3; }

namespace Arpg
{
    constexpr uint32 RIFT_MAP = 520;
    constexpr uint32 RIFT_STONE_ENTRY = 98010;
    constexpr uint32 RIFT_EXIT_ENTRY = 98011;

    // Where Alliance characters start: Goldshire, by Marshal Dughan (x, y, z, orientation).
    constexpr uint32 GOLDSHIRE_AREA = 87;
    constexpr float GOLDSHIRE_START[4] = { -9460.0f, 58.0f, 57.5f, 0.0f };

    // Whether `mapId` is the rifts' map, registered.
    bool IsRiftMap(uint32 mapId);

    // After the DBCs load, before the instance templates: the rift map's entry and template.
    void RegisterRiftMap();
    // Before the game object templates load: the Rift Stone's and the exit's.
    void EnsureRiftObjects();

    // The rift map's grid (gx, gy) is loading: its ground from the layout in its block, if any.
    void FillRiftGrid(uint32 gx, uint32 gy, GridMap& grid);

    // Every player update: a stone shown and stepped on; in a rift, its layout sent, its
    // creatures placed, its exit.
    void UpdateRift(Player* player);
    void ForgetRift(Player* player);
    // A kill in a rift: a side area's boss gives its perk (Arpg/ArpgPerks.h) to the party there.
    void OnRiftKill(Player* killer, Unit* victim);
    // A rift creature's corpse loot was made: a side area's boss's hoard. True when it was one.
    bool OnRiftBossLoot(Creature* victim);
    // A map is going: a rift instance's layout record, its buildings' collision.
    void ForgetRiftMap(Map* map);
    // A creature's straight walk on the rift map (no navmesh): round the area's buildings, by
    // their corners, when one stands in the way. `points` is the walk's start and end.
    void RiftDetour(Map const* map, std::vector<G3D::Vector3>& points);
}

#endif
