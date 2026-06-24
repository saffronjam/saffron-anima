#!/usr/bin/env python3
# Generates MorphStressTest.gltf: an NxN grid plane with MANY morph targets (one per grid row),
# each lifting its row, plus a clip that ramps every weight together. The perf fixture: a high
# active-target count exercises the morph scatter dispatch + the per-frame active compaction so
# `pass-timings` can confirm the morph pass is present and timed.
import base64, json, struct, os

N = 16                       # NxN grid -> N*N vertices
TARGETS = N                  # one morph target per row
verts = []
for j in range(N):
    for i in range(N):
        verts.append((i / (N - 1) - 0.5, 0.0, j / (N - 1) - 0.5))
normals = [(0, 1, 0)] * len(verts)

indices = []
for j in range(N - 1):
    for i in range(N - 1):
        a = j * N + i
        b = a + 1
        c = a + N
        d = c + 1
        indices += [a, c, b, b, c, d]

# One morph target per row r: every vertex in row r lifts by +0.5 in Y, the rest stay.
targets = []
for r in range(TARGETS):
    dpos = [(0, 0.5, 0) if (k // N) == r else (0, 0, 0) for k in range(len(verts))]
    targets.append(dpos)

times = [0.0, 1.0]
# Each keyframe is a flat run of TARGETS weights; ramp every target 0 -> 1 together.
weights_key0 = [0.0] * TARGETS
weights_key1 = [1.0] * TARGETS

buf = bytearray()
def align4():
    while len(buf) % 4:
        buf.append(0)
views = []
def add(data, target=None):
    align4(); off = len(buf); buf.extend(data); views.append((off, len(data), target)); return len(views) - 1

ARRAY_BUFFER, ELEMENT_ARRAY_BUFFER = 34962, 34963
pos_v = add(b"".join(struct.pack("<3f", *p) for p in verts), ARRAY_BUFFER)
nrm_v = add(b"".join(struct.pack("<3f", *n) for n in normals), ARRAY_BUFFER)
idx_v = add(struct.pack("<%dH" % len(indices), *indices), ELEMENT_ARRAY_BUFFER)

def bounds(vs):
    return [min(v[i] for v in vs) for i in range(3)], [max(v[i] for v in vs) for i in range(3)]
pmin, pmax = bounds(verts)
accessors = [
    {"bufferView": pos_v, "componentType": 5126, "count": len(verts), "type": "VEC3", "min": pmin, "max": pmax},  # 0
    {"bufferView": nrm_v, "componentType": 5126, "count": len(verts), "type": "VEC3"},                            # 1
    {"bufferView": idx_v, "componentType": 5123, "count": len(indices), "type": "SCALAR"},                        # 2
]
# One POSITION-delta accessor per morph target.
target_attrs = []
for dpos in targets:
    v = add(b"".join(struct.pack("<3f", *d) for d in dpos), ARRAY_BUFFER)
    dmin, dmax = bounds(dpos)
    accessors.append({"bufferView": v, "componentType": 5126, "count": len(verts), "type": "VEC3", "min": dmin, "max": dmax})
    target_attrs.append({"POSITION": len(accessors) - 1})

tin_v = add(struct.pack("<%df" % len(times), *times))
tin_acc = len(accessors)
accessors.append({"bufferView": tin_v, "componentType": 5126, "count": len(times), "type": "SCALAR", "min": [0.0], "max": [1.0]})
wout_v = add(struct.pack("<%df" % (TARGETS * 2), *(weights_key0 + weights_key1)))
wout_acc = len(accessors)
accessors.append({"bufferView": wout_v, "componentType": 5126, "count": TARGETS * 2, "type": "SCALAR"})

bufferViews = [{"buffer": 0, "byteOffset": o, "byteLength": l, **({"target": t} if t else {})} for (o, l, t) in views]

gltf = {
    "asset": {"version": "2.0", "generator": "gen_morph_stress.py"},
    "scene": 0,
    "scenes": [{"nodes": [0]}],
    "nodes": [{"name": "MorphStress", "mesh": 0}],
    "meshes": [{
        "name": "MorphStress",
        "weights": [0.0] * TARGETS,
        "extras": {"targetNames": [f"row_{r}" for r in range(TARGETS)]},
        "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}, "targets": target_attrs, "indices": 2}],
    }],
    "animations": [{
        "name": "RampAll",
        "samplers": [{"input": tin_acc, "output": wout_acc, "interpolation": "LINEAR"}],
        "channels": [{"sampler": 0, "target": {"node": 0, "path": "weights"}}],
    }],
    "accessors": accessors,
    "bufferViews": bufferViews,
    "buffers": [{"byteLength": len(buf), "uri": "data:application/octet-stream;base64," + base64.b64encode(bytes(buf)).decode()}],
}
out = os.path.join(os.path.dirname(__file__), "MorphStressTest.gltf")
json.dump(gltf, open(out, "w"), indent=1)
print("wrote", out, "buffer bytes:", len(buf), "targets:", TARGETS)
