#!/usr/bin/env python3
# Generates leg.gltf: a minimal 3-joint chain (Hip->Knee->Ankle) skinned to a thin strip mesh,
# with a "KneeBend" animation that rotates the knee so the ankle swings down. The only rigged
# e2e fixture with enough joints (3) for a two-bone IK solve (Phase 13 foot IK).
import base64, json, struct, math

# --- mesh: 2 verts per joint level (y=0,1,2), each weighted 100% to its level's joint ---
positions = [(-0.1,0,0),(0.1,0,0), (-0.1,1,0),(0.1,1,0), (-0.1,2,0),(0.1,2,0)]
joints    = [(0,0,0,0)]*2 + [(1,0,0,0)]*2 + [(2,0,0,0)]*2
weights   = [(1,0,0,0)]*6
indices   = [0,1,3, 0,3,2, 2,3,5, 2,5,4]

# inverse bind matrices = inverse(worldBind): Hip(0,0,0) Knee(0,1,0) Ankle(0,2,0)
def ibm(ty):  # column-major translation by (0,-ty,0)
    return [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,-ty,0,1]
ibms = [ibm(0.0), ibm(1.0), ibm(2.0)]

# animation: rotate the knee about Z from 0 -> 90deg over 1s, swinging the ankle down
times = [0.0, 0.5, 1.0]
def quatZ(deg):
    a = math.radians(deg)/2.0
    return (0.0, 0.0, math.sin(a), math.cos(a))  # x,y,z,w
rots = [quatZ(0), quatZ(45), quatZ(90)]

buf = bytearray()
def align4():
    while len(buf) % 4: buf.append(0)
views = []  # (byteOffset, byteLength, target?)
def add(data: bytes, target=None):
    align4(); off = len(buf); buf.extend(data); views.append((off, len(data), target)); return len(views)-1

ARRAY_BUFFER, ELEMENT_ARRAY_BUFFER = 34962, 34963
pos_v  = add(b"".join(struct.pack("<3f",*p) for p in positions), ARRAY_BUFFER)
jnt_v  = add(b"".join(struct.pack("<4B",*j) for j in joints),   ARRAY_BUFFER)
wgt_v  = add(b"".join(struct.pack("<4f",*w) for w in weights),  ARRAY_BUFFER)
idx_v  = add(struct.pack("<%dH"%len(indices), *indices),        ELEMENT_ARRAY_BUFFER)
ibm_v  = add(b"".join(struct.pack("<16f",*m) for m in ibms))
tin_v  = add(struct.pack("<%df"%len(times), *times))
tout_v = add(b"".join(struct.pack("<4f",*r) for r in rots))

pmin = [min(p[i] for p in positions) for i in range(3)]
pmax = [max(p[i] for p in positions) for i in range(3)]
accessors = [
    {"bufferView":pos_v,"componentType":5126,"count":6,"type":"VEC3","min":pmin,"max":pmax},  # 0 POSITION
    {"bufferView":jnt_v,"componentType":5121,"count":6,"type":"VEC4"},                          # 1 JOINTS_0 (u8)
    {"bufferView":wgt_v,"componentType":5126,"count":6,"type":"VEC4"},                          # 2 WEIGHTS_0
    {"bufferView":idx_v,"componentType":5123,"count":len(indices),"type":"SCALAR"},             # 3 indices (u16)
    {"bufferView":ibm_v,"componentType":5126,"count":3,"type":"MAT4"},                          # 4 IBM
    {"bufferView":tin_v,"componentType":5126,"count":3,"type":"SCALAR","min":[0.0],"max":[1.0]},# 5 anim input
    {"bufferView":tout_v,"componentType":5126,"count":3,"type":"VEC4"},                         # 6 anim output
]
bufferViews = [{"buffer":0,"byteOffset":o,"byteLength":l, **({"target":t} if t else {})} for (o,l,t) in views]

gltf = {
    "asset":{"version":"2.0","generator":"gen_leg.py"},
    "scene":0,
    "scenes":[{"nodes":[0,1]}],
    "nodes":[
        {"name":"LegMesh","mesh":0,"skin":0},
        {"name":"Hip","children":[2],"translation":[0,0,0]},
        {"name":"Knee","children":[3],"translation":[0,1,0]},
        {"name":"Ankle","translation":[0,1,0]},
    ],
    "meshes":[{"name":"Leg","primitives":[{"attributes":{"POSITION":0,"JOINTS_0":1,"WEIGHTS_0":2},"indices":3}]}],
    "skins":[{"skeleton":1,"joints":[1,2,3],"inverseBindMatrices":4}],
    "animations":[{"name":"KneeBend","samplers":[{"input":5,"output":6,"interpolation":"LINEAR"}],
                   "channels":[{"sampler":0,"target":{"node":2,"path":"rotation"}}]}],
    "accessors":accessors,
    "bufferViews":bufferViews,
    "buffers":[{"byteLength":len(buf),"uri":"data:application/octet-stream;base64,"+base64.b64encode(bytes(buf)).decode()}],
}
import os
out = os.path.join(os.path.dirname(__file__), "leg.gltf")
json.dump(gltf, open(out,"w"), indent=1)
print("wrote", out, "buffer bytes:", len(buf))
