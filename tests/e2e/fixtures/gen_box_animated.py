#!/usr/bin/env python3
# Generates BoxAnimated.gltf: a node-TRS fixture (the canonical glTF sample shape). A static
# "Root" node parents an "AnimatedBox" node carrying a box mesh; a clip animates the child
# node's translation and rotation — NO skin, NO joints. It proves the importer keeps a live
# node forest (the transform is NOT baked into vertices) and that node-TRS playback drives the
# child entity. The forest has >1 node so the single-identity-root collapse does not fire.
import base64, json, struct, math, os

positions = [
    (-0.5, -0.5, -0.5), (0.5, -0.5, -0.5), (0.5, -0.5, 0.5), (-0.5, -0.5, 0.5),
    (-0.5, 0.5, -0.5), (0.5, 0.5, -0.5), (0.5, 0.5, 0.5), (-0.5, 0.5, 0.5),
]
indices = [
    0, 1, 2, 0, 2, 3, 4, 6, 5, 4, 7, 6, 0, 4, 5, 0, 5, 1,
    1, 5, 6, 1, 6, 2, 2, 6, 7, 2, 7, 3, 3, 7, 4, 3, 4, 0,
]

times = [0.0, 0.5, 1.0]
# translate the box up and back down; rotate it a half-turn about Y.
trans_out = [(0, 0, 0), (0, 1.5, 0), (0, 0, 0)]
def quatY(deg):
    a = math.radians(deg) / 2.0
    return (0.0, math.sin(a), 0.0, math.cos(a))
rot_out = [quatY(0), quatY(90), quatY(180)]

buf = bytearray()
def align4():
    while len(buf) % 4:
        buf.append(0)
views = []
def add(data, target=None):
    align4(); off = len(buf); buf.extend(data); views.append((off, len(data), target)); return len(views) - 1

ARRAY_BUFFER, ELEMENT_ARRAY_BUFFER = 34962, 34963
pos_v = add(b"".join(struct.pack("<3f", *p) for p in positions), ARRAY_BUFFER)
idx_v = add(struct.pack("<%dH" % len(indices), *indices), ELEMENT_ARRAY_BUFFER)
tin_v = add(struct.pack("<%df" % len(times), *times))
tr_v = add(b"".join(struct.pack("<3f", *t) for t in trans_out))
ro_v = add(b"".join(struct.pack("<4f", *r) for r in rot_out))

pmin = [min(p[i] for p in positions) for i in range(3)]
pmax = [max(p[i] for p in positions) for i in range(3)]
accessors = [
    {"bufferView": pos_v, "componentType": 5126, "count": 8, "type": "VEC3", "min": pmin, "max": pmax},  # 0 POSITION
    {"bufferView": idx_v, "componentType": 5123, "count": len(indices), "type": "SCALAR"},               # 1 indices
    {"bufferView": tin_v, "componentType": 5126, "count": 3, "type": "SCALAR", "min": [0.0], "max": [1.0]},  # 2 anim input
    {"bufferView": tr_v, "componentType": 5126, "count": 3, "type": "VEC3"},                              # 3 translation out
    {"bufferView": ro_v, "componentType": 5126, "count": 3, "type": "VEC4"},                              # 4 rotation out
]
bufferViews = [{"buffer": 0, "byteOffset": o, "byteLength": l, **({"target": t} if t else {})} for (o, l, t) in views]

gltf = {
    "asset": {"version": "2.0", "generator": "gen_box_animated.py"},
    "scene": 0,
    "scenes": [{"nodes": [0]}],
    "nodes": [
        {"name": "Root", "children": [1], "translation": [0, 0, 0]},
        {"name": "AnimatedBox", "mesh": 0, "translation": [0, 0, 0]},
    ],
    "meshes": [{"name": "Box", "primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}],
    "animations": [{
        "name": "BoxMove",
        "samplers": [
            {"input": 2, "output": 3, "interpolation": "LINEAR"},
            {"input": 2, "output": 4, "interpolation": "LINEAR"},
        ],
        "channels": [
            {"sampler": 0, "target": {"node": 1, "path": "translation"}},
            {"sampler": 1, "target": {"node": 1, "path": "rotation"}},
        ],
    }],
    "accessors": accessors,
    "bufferViews": bufferViews,
    "buffers": [{"byteLength": len(buf), "uri": "data:application/octet-stream;base64," + base64.b64encode(bytes(buf)).decode()}],
}
out = os.path.join(os.path.dirname(__file__), "BoxAnimated.gltf")
json.dump(gltf, open(out, "w"), indent=1)
print("wrote", out, "buffer bytes:", len(buf))
