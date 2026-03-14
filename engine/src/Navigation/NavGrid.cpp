/*
 * NavGrid — Grid baking from scene colliders + A* pathfinding.
 */
#include "VibeEngine/Navigation/NavGrid.h"
#include "VibeEngine/Scene/Scene.h"
#include "VibeEngine/Scene/Entity.h"
#include "VibeEngine/Scene/Components.h"
#include "VibeEngine/Core/Log.h"

#include <queue>
#include <cmath>
#include <algorithm>
#include <unordered_set>

namespace VE {

// ── Grid Baking ─────────────────────────────────────────────────────

NavGrid NavGridBuilder::BuildFromScene(Scene& scene, float cellSize,
                                        float worldSize, float agentRadius) {
    NavGrid grid;
    grid.CellSize = cellSize;
    grid.OriginX = -worldSize;
    grid.OriginZ = -worldSize;
    grid.Width  = static_cast<int>(worldSize * 2.0f / cellSize);
    grid.Height = static_cast<int>(worldSize * 2.0f / cellSize);
    grid.Cells.assign(grid.Width * grid.Height, 0); // all walkable

    auto& reg = scene.GetRegistry();

    // Mark cells blocked by static box colliders
    auto boxView = reg.view<TransformComponent, BoxColliderComponent>();
    for (auto entity : boxView) {
        // Only block static or no-rigidbody entities
        auto* rb = reg.try_get<RigidbodyComponent>(entity);
        if (rb && rb->Type != BodyType::Static) continue;

        auto& tc = boxView.get<TransformComponent>(entity);
        auto& bc = boxView.get<BoxColliderComponent>(entity);

        // Compute world-space AABB
        float halfX = bc.Size[0] * tc.Scale[0] * 0.5f + agentRadius;
        float halfZ = bc.Size[2] * tc.Scale[2] * 0.5f + agentRadius;
        float cx = tc.Position[0] + bc.Offset[0];
        float cz = tc.Position[2] + bc.Offset[2];

        // Skip ground plane (very thin in Y and large in XZ)
        if (bc.Size[1] * tc.Scale[1] < 0.2f && halfX > 5.0f && halfZ > 5.0f)
            continue;

        float minX = cx - halfX;
        float maxX = cx + halfX;
        float minZ = cz - halfZ;
        float maxZ = cz + halfZ;

        int gxMin, gzMin, gxMax, gzMax;
        grid.WorldToGrid(minX, minZ, gxMin, gzMin);
        grid.WorldToGrid(maxX, maxZ, gxMax, gzMax);

        gxMin = std::max(0, gxMin);
        gzMin = std::max(0, gzMin);
        gxMax = std::min(grid.Width - 1, gxMax);
        gzMax = std::min(grid.Height - 1, gzMax);

        for (int gz = gzMin; gz <= gzMax; gz++)
            for (int gx = gxMin; gx <= gxMax; gx++)
                grid.Cells[gz * grid.Width + gx] = 1;
    }

    // Mark cells blocked by static sphere colliders
    auto sphereView = reg.view<TransformComponent, SphereColliderComponent>();
    for (auto entity : sphereView) {
        auto* rb = reg.try_get<RigidbodyComponent>(entity);
        if (rb && rb->Type != BodyType::Static) continue;

        auto& tc = sphereView.get<TransformComponent>(entity);
        auto& sc = sphereView.get<SphereColliderComponent>(entity);

        float maxScale = std::max({tc.Scale[0], tc.Scale[1], tc.Scale[2]});
        float r = sc.Radius * maxScale + agentRadius;
        float cx = tc.Position[0] + sc.Offset[0];
        float cz = tc.Position[2] + sc.Offset[2];

        int gxMin, gzMin, gxMax, gzMax;
        grid.WorldToGrid(cx - r, cz - r, gxMin, gzMin);
        grid.WorldToGrid(cx + r, cz + r, gxMax, gzMax);

        gxMin = std::max(0, gxMin);
        gzMin = std::max(0, gzMin);
        gxMax = std::min(grid.Width - 1, gxMax);
        gzMax = std::min(grid.Height - 1, gzMax);

        for (int gz = gzMin; gz <= gzMax; gz++) {
            for (int gx = gxMin; gx <= gxMax; gx++) {
                float wx, wz;
                grid.GridToWorld(gx, gz, wx, wz);
                float dx = wx - cx, dz = wz - cz;
                if (dx * dx + dz * dz <= r * r)
                    grid.Cells[gz * grid.Width + gx] = 1;
            }
        }
    }

    int blocked = 0;
    for (auto c : grid.Cells) if (c) blocked++;
    VE_ENGINE_INFO("NavGrid baked: {}x{} cells, {} blocked, cellSize={:.2f}m",
                    grid.Width, grid.Height, blocked, cellSize);
    return grid;
}

// ── A* Pathfinding ──────────────────────────────────────────────────

struct AStarNode {
    int X, Z;
    float G, F;
    int ParentIdx;
};

struct AStarCmp {
    bool operator()(const AStarNode& a, const AStarNode& b) const {
        return a.F > b.F; // min-heap
    }
};

std::vector<std::array<float, 2>> NavGridBuilder::FindPath(
    const NavGrid& grid, float startX, float startZ, float endX, float endZ) {

    int sx, sz, ex, ez;
    grid.WorldToGrid(startX, startZ, sx, sz);
    grid.WorldToGrid(endX, endZ, ex, ez);

    // Clamp to grid bounds
    sx = std::clamp(sx, 0, grid.Width - 1);
    sz = std::clamp(sz, 0, grid.Height - 1);
    ex = std::clamp(ex, 0, grid.Width - 1);
    ez = std::clamp(ez, 0, grid.Height - 1);

    if (!grid.IsWalkable(ex, ez)) {
        // Target is blocked, find nearest walkable cell
        for (int r = 1; r < 10; r++) {
            for (int dx = -r; dx <= r; dx++) {
                for (int dz = -r; dz <= r; dz++) {
                    if (grid.IsWalkable(ex + dx, ez + dz)) {
                        ex += dx; ez += dz;
                        goto found;
                    }
                }
            }
        }
        return {}; // no walkable cell near target
        found:;
    }

    if (sx == ex && sz == ez) {
        float wx, wz;
        grid.GridToWorld(ex, ez, wx, wz);
        return {{ {wx, wz} }};
    }

    int totalCells = grid.Width * grid.Height;
    std::vector<float> gScore(totalCells, 1e9f);
    std::vector<int> parent(totalCells, -1);
    std::vector<bool> closed(totalCells, false);

    auto idx = [&](int x, int z) { return z * grid.Width + x; };

    auto heuristic = [&](int x, int z) -> float {
        int dx = std::abs(x - ex), dz = std::abs(z - ez);
        return static_cast<float>(std::max(dx, dz)) + 0.414f * static_cast<float>(std::min(dx, dz));
    };

    std::priority_queue<AStarNode, std::vector<AStarNode>, AStarCmp> open;
    gScore[idx(sx, sz)] = 0;
    open.push({ sx, sz, 0, heuristic(sx, sz), -1 });

    static const int DX[] = { 1, -1, 0, 0, 1, -1, 1, -1 };
    static const int DZ[] = { 0, 0, 1, -1, 1, 1, -1, -1 };
    static const float COST[] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.414f, 1.414f, 1.414f, 1.414f };

    int iterations = 0;
    const int MAX_ITER = 50000;

    while (!open.empty() && iterations < MAX_ITER) {
        iterations++;
        AStarNode cur = open.top();
        open.pop();

        int ci = idx(cur.X, cur.Z);
        if (closed[ci]) continue;
        closed[ci] = true;

        if (cur.X == ex && cur.Z == ez) {
            // Reconstruct path
            std::vector<std::array<float, 2>> path;
            int i = ci;
            while (i != -1) {
                int gz = i / grid.Width;
                int gx = i % grid.Width;
                float wx, wz;
                grid.GridToWorld(gx, gz, wx, wz);
                path.push_back({ wx, wz });
                i = parent[i];
            }
            std::reverse(path.begin(), path.end());
            return path;
        }

        for (int d = 0; d < 8; d++) {
            int nx = cur.X + DX[d];
            int nz = cur.Z + DZ[d];
            if (!grid.IsWalkable(nx, nz)) continue;

            // For diagonals, check that both adjacent cardinals are walkable
            if (d >= 4) {
                if (!grid.IsWalkable(cur.X + DX[d], cur.Z) ||
                    !grid.IsWalkable(cur.X, cur.Z + DZ[d]))
                    continue;
            }

            int ni = idx(nx, nz);
            if (closed[ni]) continue;

            float ng = gScore[ci] + COST[d];
            if (ng < gScore[ni]) {
                gScore[ni] = ng;
                parent[ni] = ci;
                open.push({ nx, nz, ng, ng + heuristic(nx, nz), ci });
            }
        }
    }

    return {}; // no path found
}

} // namespace VE
