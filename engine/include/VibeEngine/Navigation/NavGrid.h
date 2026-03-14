/*
 * NavGrid — Grid-based 2D navigation on the XZ plane.
 *
 * Cells are marked walkable or blocked from scene static colliders.
 * A* pathfinding finds shortest path between two world positions.
 */
#pragma once

#include <vector>
#include <array>
#include <cstdint>

namespace VE {

class Scene;

struct NavGrid {
    float OriginX = 0, OriginZ = 0; // world-space corner (min X, min Z)
    float CellSize = 0.5f;
    int   Width = 0, Height = 0;
    std::vector<uint8_t> Cells; // 0 = walkable, 1 = blocked

    bool IsWalkable(int gx, int gz) const {
        if (gx < 0 || gz < 0 || gx >= Width || gz >= Height) return false;
        return Cells[gz * Width + gx] == 0;
    }

    void WorldToGrid(float wx, float wz, int& gx, int& gz) const {
        gx = static_cast<int>((wx - OriginX) / CellSize);
        gz = static_cast<int>((wz - OriginZ) / CellSize);
    }

    void GridToWorld(int gx, int gz, float& wx, float& wz) const {
        wx = OriginX + (gx + 0.5f) * CellSize;
        wz = OriginZ + (gz + 0.5f) * CellSize;
    }
};

class NavGridBuilder {
public:
    // Build grid from scene static colliders
    // worldSize: half-extent of the grid centered at origin
    static NavGrid BuildFromScene(Scene& scene, float cellSize = 0.5f,
                                   float worldSize = 50.0f, float agentRadius = 0.4f);

    // A* pathfinding on the grid
    // Returns list of XZ world-space waypoints (empty if no path)
    static std::vector<std::array<float, 2>> FindPath(
        const NavGrid& grid, float startX, float startZ, float endX, float endZ);
};

} // namespace VE
