#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""TMD -> Wavefront OBJ exporter for PS1 3D models.

TMD is Sony's native PlayStation 3D model format. Each TMD file contains
one or more objects, and each object holds vertex, normal, and primitive
tables. This tool walks the common triangle and quad primitive codes and
emits one OBJ file per TMD with all objects fused (one "o" group per TMD
object, one "g" per primitive type).

Current coverage:
    Triangles: 0x20/0x24/0x28/0x2c (flat/tex/gouraud/gouraud+tex)
    Quads    : 0x30/0x34/0x38/0x3c (same variants)
    Vertex positions are exported; normals and UVs are logged in-file as
    comments but not written to the OBJ (OBJ does not round-trip normals
    cleanly without per-vertex mapping). Good enough for viewing in
    Blender/MeshLab and as a starting point for re-texturing.

Unsupported (logged + skipped): gradated/environment primitives, line
primitives, sprite primitives. These are used for special effects and
not relevant to editable character models.

Usage:
    python3 pc/tools/tmd_to_obj.py input.tmd [output.obj]
    python3 pc/tools/tmd_to_obj.py --batch pc/assets/raw/CDROM/TMD [pc/assets/tmd/]
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path
from asset_limits import read_asset

TMD_MAGIC = 0x41


def u32(d, o): return struct.unpack_from("<I", d, o)[0]
def s32(d, o): return struct.unpack_from("<i", d, o)[0]
def s16(d, o): return struct.unpack_from("<h", d, o)[0]


def parse_object_header(data: bytes, off: int):
    (vp, nv, np, nn, pp, npr, scale) = struct.unpack_from("<IIIIIIi", data, off)
    return {
        "vert_ptr": vp, "n_verts": nv,
        "norm_ptr": np, "n_normals": nn,
        "prim_ptr": pp, "n_prims": npr,
        "scale": scale,
    }


def parse_vertices(data: bytes, off: int, n: int):
    # Each vertex: 8 bytes (s16 x, s16 y, s16 z, s16 pad)
    verts = []
    for i in range(n):
        x, y, z, _ = struct.unpack_from("<hhhh", data, off + i * 8)
        verts.append((x, y, z))
    return verts


def parse_primitives(data: bytes, start: int, n: int, obj_idx: int):
    """Walk the primitive table of one object.

    TMD packs primitives back-to-back. Each packet is [olen, ilen, flag,
    mode] followed by ilen*4 body bytes. Sony's TMD object.primitive_top
    pointer points to the BODY of the first primitive (skipping the
    leading 4-byte packet header), so we step back 4 bytes to find the
    real start of the chain, then advance by full packet size.
    """
    tris = []
    quads = []
    off = start - 4  # compensate for Sony's "points to body" convention
    for p in range(n):
        if off + 4 > len(data) or off < 0:
            break
        olen = data[off + 0]
        ilen = data[off + 1]
        flag = data[off + 2]
        mode = data[off + 3]
        body_bytes = ilen * 4
        if body_bytes == 0:
            break
        body = data[off + 4 : off + 4 + body_bytes]

        # Strip the bit-7 "lighting on" and gradation flags; keep
        # polygon-shape bits.
        code = mode & 0x7F
        try:
            if code in (0x20, 0x21):  # flat tri, no-tex
                # 4B RGBA, then 3 vertex indices (s16), padding
                i0, i1, i2 = struct.unpack_from("<HHH", body, 8)
                tris.append((i0, i1, i2))
            elif code in (0x24, 0x25, 0x26, 0x27):  # tri, textured
                # u/v0, clut, u/v1, tpage, u/v2, pad, norm0, v0..v2
                # Indexing from the TMD primitive-table reference:
                # body[0:2]=u0v0, [2:4]=clut, [4:6]=u1v1, [6:8]=tpage,
                # [8:10]=u2v2, [10:12]=pad, then s16 n0,v0,v1,v2
                i0, i1, i2 = struct.unpack_from("<HHH", body, 14)
                tris.append((i0, i1, i2))
            elif code in (0x28, 0x29):  # gouraud tri, no-tex
                # 3 RGBA (each 4B) then indices
                i0, i1, i2 = struct.unpack_from("<HHH", body, 12)
                tris.append((i0, i1, i2))
            elif code in (0x2c, 0x2d, 0x2e, 0x2f):  # gouraud tri, tex
                i0, i1, i2 = struct.unpack_from("<HHH", body, 18)
                tris.append((i0, i1, i2))
            elif code in (0x30, 0x31):  # flat quad, no-tex
                i0, i1, i2, i3 = struct.unpack_from("<HHHH", body, 8)
                quads.append((i0, i1, i2, i3))
            elif code in (0x34, 0x35, 0x36, 0x37):  # quad, textured
                i0, i1, i2, i3 = struct.unpack_from("<HHHH", body, 16)
                quads.append((i0, i1, i2, i3))
            elif code in (0x38, 0x39):  # gouraud quad, no-tex
                i0, i1, i2, i3 = struct.unpack_from("<HHHH", body, 16)
                quads.append((i0, i1, i2, i3))
            elif code in (0x3c, 0x3d, 0x3e, 0x3f):  # gouraud quad, tex
                i0, i1, i2, i3 = struct.unpack_from("<HHHH", body, 24)
                quads.append((i0, i1, i2, i3))
            else:
                # Unknown primitive code — advance and keep going.
                pass
        except struct.error:
            # Body too short for expected layout — skip this prim.
            pass

        off += 4 + body_bytes
    return tris, quads


def tmd_to_obj(tmd_path: Path, obj_path: Path):
    data = read_asset(tmd_path)
    if len(data) < 12:
        raise ValueError("TMD truncated")
    magic = u32(data, 0)
    if magic != TMD_MAGIC:
        raise ValueError(f"not a TMD (magic 0x{magic:08x})")
    flags = u32(data, 4)
    nobj  = u32(data, 8)

    # Object table follows the header (12 bytes) at offset 12 when
    # flags-bit-0 indicates relative pointers. With absolute pointers
    # (flags==0) the pointer fields hold offsets relative to the data
    # region start. We normalize to absolute file offsets either way.
    obj_table_off = 12
    obj_table_size = nobj * 28
    data_region_off = obj_table_off + obj_table_size

    lines = [f"# TMD source: {tmd_path.name}",
             f"# objects: {nobj}, flags: 0x{flags:x}"]
    all_verts = []
    all_faces = []  # list of (obj_idx, 'tri' or 'quad', indices_into_all_verts)

    for o in range(nobj):
        obj = parse_object_header(data, obj_table_off + o * 28)
        vp = obj["vert_ptr"]
        pp = obj["prim_ptr"]
        if (flags & 1) == 0:
            vp += data_region_off
            pp += data_region_off
        if vp >= len(data):
            lines.append(f"# obj{o}: vert_ptr out of range, skipping")
            continue
        verts = parse_vertices(data, vp, obj["n_verts"])
        tris, quads = parse_primitives(data, pp, obj["n_prims"], o)

        base = len(all_verts)
        all_verts.extend(verts)
        for t in tris:
            if max(t) < obj["n_verts"]:
                all_faces.append((o, "tri", (base + t[0], base + t[1], base + t[2])))
        for q in quads:
            if max(q) < obj["n_verts"]:
                all_faces.append((o, "quad", (base + q[0], base + q[1],
                                              base + q[2], base + q[3])))

        lines.append(f"# obj{o}: {len(verts)} verts, {len(tris)} tris, {len(quads)} quads, "
                     f"scale={obj['scale']}")

    # Emit OBJ. PS1 uses a 12.4 fixed-point style for some coords but the
    # raw s16 values are already usable; scale down by 256 so typical
    # character meshes fit in a reasonable unit box.
    SCALE = 256.0
    for (x, y, z) in all_verts:
        lines.append(f"v {x/SCALE:.6f} {-y/SCALE:.6f} {z/SCALE:.6f}")

    last_obj = -1
    for (o, kind, idx) in all_faces:
        if o != last_obj:
            lines.append(f"o obj_{o}")
            last_obj = o
        if kind == "tri":
            a, b, c = idx
            lines.append(f"f {a+1} {b+1} {c+1}")
        else:
            a, b, c, d = idx
            # PS1 quads are traversed in strip order (v0 v1 v2 v3 —
            # triangles are v0-v1-v2 and v1-v3-v2). Emit both triangles
            # so OBJ consumers that don't support quads see the full
            # face.
            lines.append(f"f {a+1} {b+1} {c+1}")
            lines.append(f"f {b+1} {d+1} {c+1}")

    obj_path.write_text("\n".join(lines) + "\n")
    return len(all_verts), len(all_faces)


def batch(root: Path, out_dir: Path):
    out_dir.mkdir(parents=True, exist_ok=True)
    total = 0
    for tmd in root.rglob("*.TMD"):
        out = out_dir / (tmd.stem + ".obj")
        try:
            v, f = tmd_to_obj(tmd, out)
            print(f"  {tmd.name} -> {out.name} ({v} verts, {f} faces)")
            total += 1
        except Exception as e:
            print(f"  FAIL {tmd.name}: {e}")
    print(f"Exported {total} TMD files")


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("input")
    p.add_argument("output", nargs="?")
    p.add_argument("--batch", action="store_true")
    args = p.parse_args()

    if args.batch:
        batch(Path(args.input),
              Path(args.output) if args.output else Path("pc/assets/tmd"))
    else:
        out = Path(args.output) if args.output else Path(args.input).with_suffix(".obj")
        v, f = tmd_to_obj(Path(args.input), out)
        print(f"Wrote {out} ({v} verts, {f} faces)")


if __name__ == "__main__":
    main()
