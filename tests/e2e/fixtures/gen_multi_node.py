#!/usr/bin/env python3
# Generates multi-node.gltf: a STATIC multi-mesh-node forest — two sibling root nodes, each
# carrying its own box mesh, offset apart on X. NO skin, NO animation, more than one node, so
# the importer takes the node-forest path (not the single-identity-root collapse) and the
# meshes land on the CHILD entities while the spawned container root carries none. This is the
# GothicCommode shape: the case that probing a single resolved entity wrongly rejects. The two
# boxes sit apart so the forest has real extent (a bounds union that frames the whole model
# differs visibly from framing one node).
import base64, json, struct, os


def box(cx):
    # A unit box centered at (cx, 0, 0).
    p = [
        (-0.5, -0.5, -0.5), (0.5, -0.5, -0.5), (0.5, -0.5, 0.5), (-0.5, -0.5, 0.5),
        (-0.5, 0.5, -0.5), (0.5, 0.5, -0.5), (0.5, 0.5, 0.5), (-0.5, 0.5, 0.5),
    ]
    return [(x + cx, y, z) for (x, y, z) in p]


indices = [
    0, 1, 2, 0, 2, 3, 4, 6, 5, 4, 7, 6, 0, 4, 5, 0, 5, 1,
    1, 5, 6, 1, 6, 2, 2, 6, 7, 2, 7, 3, 3, 7, 4, 3, 4, 0,
]

buf = bytearray()
views = []


def align4():
    while len(buf) % 4:
        buf.append(0)


def add(data, target=None):
    align4()
    off = len(buf)
    buf.extend(data)
    views.append((off, len(data), target))
    return len(views) - 1


ARRAY_BUFFER, ELEMENT_ARRAY_BUFFER = 34962, 34963

# Two nodes' geometry authored at the origin; the node translation offsets them so the spawned
# forest keeps live per-node transforms (the importer does not bake them).
pos_a = add(b"".join(struct.pack("<3f", *p) for p in box(0.0)), ARRAY_BUFFER)
pos_b = add(b"".join(struct.pack("<3f", *p) for p in box(0.0)), ARRAY_BUFFER)
idx_v = add(struct.pack("<%dH" % len(indices), *indices), ELEMENT_ARRAY_BUFFER)


def accessor(view, count, kind, comp, mn=None, mx=None):
    a = {"bufferView": view, "componentType": comp, "count": count, "type": kind}
    if mn is not None:
        a["min"], a["max"] = mn, mx
    return a


bounds = [-0.5, -0.5, -0.5], [0.5, 0.5, 0.5]
accessors = [
    accessor(pos_a, 8, "VEC3", 5126, bounds[0], bounds[1]),
    accessor(pos_b, 8, "VEC3", 5126, bounds[0], bounds[1]),
    accessor(idx_v, len(indices), "SCALAR", 5123),
]

meshes = [
    {"primitives": [{"attributes": {"POSITION": 0}, "indices": 2}]},
    {"primitives": [{"attributes": {"POSITION": 1}, "indices": 2}]},
]

nodes = [
    {"name": "BoxLeft", "mesh": 0, "translation": [-1.5, 0.0, 0.0]},
    {"name": "BoxRight", "mesh": 1, "translation": [1.5, 0.0, 0.0]},
]

gltf = {
    "asset": {"version": "2.0", "generator": "gen_multi_node.py"},
    "scene": 0,
    "scenes": [{"nodes": [0, 1]}],
    "nodes": nodes,
    "meshes": meshes,
    "accessors": accessors,
    "bufferViews": [
        {"buffer": 0, "byteOffset": o, "byteLength": n, **({"target": t} if t else {})}
        for (o, n, t) in views
    ],
    "buffers": [
        {"byteLength": len(buf), "uri": "data:application/octet-stream;base64," + base64.b64encode(bytes(buf)).decode()}
    ],
}

out = os.path.join(os.path.dirname(__file__), "multi-node.gltf")
with open(out, "w") as f:
    json.dump(gltf, f, indent=2)
print("wrote", out)
