#!/usr/bin/env python3
"""Generate include/daydream_wireframe_data.hpp from Three.js BufferGeometry JSON.

Simplifies the wireframe for embedded rendering:
  - Every boundary edge (one adjacent triangle) is kept — preserves silhouette.
  - Interior edges are filled by longest-first until TARGET_TOTAL edges — drops
    short triangulation spokes while keeping structure readable.
"""
from __future__ import annotations

import json
import math
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_JSON = ROOT / "daydream.json"

# Cap total line segments per frame (boundary is always included first).
TARGET_TOTAL_EDGES = 240


def main() -> None:
    src = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_JSON
    if not src.is_file():
        print(f"Usage: {sys.argv[0]} [path/to/daydream.json]", file=sys.stderr)
        sys.exit(1)

    data = json.loads(src.read_text())
    pos = data["data"]["attributes"]["position"]["array"]
    idx = data["data"]["index"]["array"]

    n = len(pos) // 3
    xs, ys, zs = pos[0::3], pos[1::3], pos[2::3]
    cx = (min(xs) + max(xs)) / 2
    cy = (min(ys) + max(ys)) / 2
    cz = (min(zs) + max(zs)) / 2
    ex = max(xs) - min(xs)
    ey = max(ys) - min(ys)
    ez = max(zs) - min(zs)
    extent = max(ex, ey, ez)
    target_extent = 0.65
    scl = target_extent / extent if extent > 0 else 1.0

    vx = [(pos[i * 3 + 0] - cx) * scl for i in range(n)]
    vy = [(pos[i * 3 + 1] - cy) * scl for i in range(n)]
    vz = [(pos[i * 3 + 2] - cz) * scl for i in range(n)]

    def vert(i: int) -> tuple[float, float, float]:
        return (vx[i], vy[i], vz[i])

    def dist(a: int, b: int) -> float:
        p, q = vert(a), vert(b)
        dx, dy, dz = p[0] - q[0], p[1] - q[1], p[2] - q[2]
        return math.sqrt(dx * dx + dy * dy + dz * dz)

    nt = len(idx) // 3
    edge_faces: dict[tuple[int, int], list[int]] = defaultdict(list)
    for t in range(nt):
        a, b, c = idx[t * 3], idx[t * 3 + 1], idx[t * 3 + 2]
        for u, v in ((a, b), (b, c), (c, a)):
            if u > v:
                u, v = v, u
            edge_faces[(u, v)].append(t)

    boundary: list[tuple[int, int]] = []
    interior: list[tuple[float, tuple[int, int]]] = []
    for e, faces in edge_faces.items():
        length = dist(e[0], e[1])
        if len(faces) == 1:
            boundary.append(e)
        else:
            interior.append((length, e))

    interior.sort(key=lambda x: -x[0])
    budget = max(0, TARGET_TOTAL_EDGES - len(boundary))
    picked_interior = [e for _, e in interior[:budget]]

    edges = sorted(set(boundary) | set(picked_interior))

    out: list[str] = []
    out.append("// AUTO-GENERATED from daydream.json — do not edit by hand")
    out.append("// Wireframe: all boundary edges + longest interior edges (see gen_daydream_wireframe.py).")
    out.append("#pragma once")
    out.append("#include <cstdint>")
    out.append("")
    out.append(f"static constexpr int kDaydreamVtxCount = {n};")
    out.append(f"static constexpr int kDaydreamEdgeCount = {len(edges)};")
    out.append("")

    def fmt_float_arr(name: str, arr: list[float]) -> None:
        out.append(f"static const float {name}[kDaydreamVtxCount] = {{")
        row: list[str] = []
        for v in arr:
            row.append(f"{v:.8f}f")
            if len(row) >= 6:
                out.append("  " + ", ".join(row) + ",")
                row = []
        if row:
            out.append("  " + ", ".join(row) + ",")
        out.append("};")

    fmt_float_arr("kDaydreamVx", vx)
    out.append("")
    fmt_float_arr("kDaydreamVy", vy)
    out.append("")
    fmt_float_arr("kDaydreamVz", vz)
    out.append("")
    out.append("static const uint16_t kDaydreamEdgeIdx[kDaydreamEdgeCount * 2] = {")
    row: list[str] = []
    for a, b in edges:
        row.extend([str(a), str(b)])
        if len(row) >= 16:
            out.append("  " + ", ".join(row) + ",")
            row = []
    if row:
        out.append("  " + ", ".join(row) + ",")
    out.append("};")

    dst = ROOT / "include" / "daydream_wireframe_data.hpp"
    dst.write_text("\n".join(out) + "\n")
    print(
        f"Wrote {dst}\n"
        f"  vertices={n}, edges={len(edges)} (boundary={len(boundary)}, "
        f"interior_picked={len(picked_interior)})\n"
        f"  scale={scl:.6f}"
    )


if __name__ == "__main__":
    main()
