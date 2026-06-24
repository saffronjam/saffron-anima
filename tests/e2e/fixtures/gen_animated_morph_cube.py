#!/usr/bin/env python3
# Generates AnimatedMorphCube.gltf: a unit cube with ONE morph target ("bulge") that lifts the
# top four vertices by +1 in Y, plus a "Bulge" animation whose weights channel drives that
# target 0 -> 1 -> 0 over 1s. The minimal morph fixture: it exercises sparse-free morph import,
# the durable MorphComponent seed, the set/get-morph-weights round-trip, and the GPU deform.
import base64, json, struct, os

# 8 cube corners (unit cube centered at origin), the top four (y=+0.5) get the morph delta.
positions = [
    (-0.5, -0.5, -0.5), (0.5, -0.5, -0.5), (0.5, -0.5, 0.5), (-0.5, -0.5, 0.5),  # bottom 0..3
    (-0.5, 0.5, -0.5), (0.5, 0.5, -0.5), (0.5, 0.5, 0.5), (-0.5, 0.5, 0.5),      # top    4..7
]
normals = [(0, -1, 0)] * 4 + [(0, 1, 0)] * 4
# The morph target: only the top four vertices move (+1 in Y); the bottom four stay put.
deltas = [(0, 0, 0)] * 4 + [(0, 1, 0)] * 4
delta_nrm = [(0, 0, 0)] * 8
# A closed box (12 triangles). Winding is irrelevant for this import/deform test.
indices = [
    0, 1, 2, 0, 2, 3,  # bottom
    4, 6, 5, 4, 7, 6,  # top
    0, 4, 5, 0, 5, 1,  # sides
    1, 5, 6, 1, 6, 2,
    2, 6, 7, 2, 7, 3,
    3, 7, 4, 3, 4, 0,
]

times = [0.0, 0.5, 1.0]
weights_out = [0.0, 1.0, 0.0]  # one weight per keyframe (one morph target)

buf = bytearray()
def align4():
    while len(buf) % 4:
        buf.append(0)
views = []
def add(data, target=None):
    align4(); off = len(buf); buf.extend(data); views.append((off, len(data), target)); return len(views) - 1

ARRAY_BUFFER, ELEMENT_ARRAY_BUFFER = 34962, 34963
pos_v = add(b"".join(struct.pack("<3f", *p) for p in positions), ARRAY_BUFFER)
nrm_v = add(b"".join(struct.pack("<3f", *n) for n in normals), ARRAY_BUFFER)
dps_v = add(b"".join(struct.pack("<3f", *d) for d in deltas), ARRAY_BUFFER)
dnr_v = add(b"".join(struct.pack("<3f", *d) for d in delta_nrm), ARRAY_BUFFER)
idx_v = add(struct.pack("<%dH" % len(indices), *indices), ELEMENT_ARRAY_BUFFER)
tin_v = add(struct.pack("<%df" % len(times), *times))
wout_v = add(struct.pack("<%df" % len(weights_out), *weights_out))

def bounds(vs):
    return [min(v[i] for v in vs) for i in range(3)], [max(v[i] for v in vs) for i in range(3)]
pmin, pmax = bounds(positions)
dmin, dmax = bounds(deltas)
accessors = [
    {"bufferView": pos_v, "componentType": 5126, "count": 8, "type": "VEC3", "min": pmin, "max": pmax},  # 0 POSITION
    {"bufferView": nrm_v, "componentType": 5126, "count": 8, "type": "VEC3"},                            # 1 NORMAL
    {"bufferView": dps_v, "componentType": 5126, "count": 8, "type": "VEC3", "min": dmin, "max": dmax},  # 2 target POSITION
    {"bufferView": dnr_v, "componentType": 5126, "count": 8, "type": "VEC3"},                            # 3 target NORMAL
    {"bufferView": idx_v, "componentType": 5123, "count": len(indices), "type": "SCALAR"},               # 4 indices
    {"bufferView": tin_v, "componentType": 5126, "count": 3, "type": "SCALAR", "min": [0.0], "max": [1.0]},  # 5 anim input
    {"bufferView": wout_v, "componentType": 5126, "count": 3, "type": "SCALAR"},                         # 6 anim output (weights)
]
bufferViews = [{"buffer": 0, "byteOffset": o, "byteLength": l, **({"target": t} if t else {})} for (o, l, t) in views]

gltf = {
    "asset": {"version": "2.0", "generator": "gen_animated_morph_cube.py"},
    "scene": 0,
    "scenes": [{"nodes": [0]}],
    "nodes": [{"name": "MorphCube", "mesh": 0}],
    "meshes": [{
        "name": "MorphCube",
        "weights": [0.0],
        "extras": {"targetNames": ["bulge"]},
        "primitives": [{
            "attributes": {"POSITION": 0, "NORMAL": 1},
            "targets": [{"POSITION": 2, "NORMAL": 3}],
            "indices": 4,
        }],
    }],
    "animations": [{
        "name": "Bulge",
        "samplers": [{"input": 5, "output": 6, "interpolation": "LINEAR"}],
        "channels": [{"sampler": 0, "target": {"node": 0, "path": "weights"}}],
    }],
    "accessors": accessors,
    "bufferViews": bufferViews,
    "buffers": [{"byteLength": len(buf), "uri": "data:application/octet-stream;base64," + base64.b64encode(bytes(buf)).decode()}],
}
out = os.path.join(os.path.dirname(__file__), "AnimatedMorphCube.gltf")
json.dump(gltf, open(out, "w"), indent=1)
print("wrote", out, "buffer bytes:", len(buf))
