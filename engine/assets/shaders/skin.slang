// Compute skinning pre-pass. Deforms each skinned vertex once into a base-layout
// Vertex buffer, so every graphics pass (scene, depth, shadow, AO, motion, RT BLAS)
// reads it as an ordinary static mesh — no skinned vertex-shader permutation.
//
// One thread per vertex. The deformed position is the skin-matrix applied to the
// bind pose, WITHOUT the instance model matrix: the graphics passes still apply
// `inst.model` / `inst.normalMatrix`, exactly as they do for a static mesh, so the
// result is identical to the old `vertexMainSkinned`.

// Mirrors engine se::Vertex (32 B): position(12) + normal(12) + uv0(8).
struct Vertex
{
    float3 position;
    float3 normal;
    float2 uv0;
};

// Mirrors engine se::VertexSkin (24 B): u16x4 joints + float4 weights. Read without a
// 16-bit-storage dependency by taking the joints as two packed u32 words.
struct VertexSkin
{
    uint2 jointsPacked;
    float4 weights;
};

[[vk::binding(0, 0)]] StructuredBuffer<Vertex> inVertices;       // static bind-pose stream
[[vk::binding(1, 0)]] StructuredBuffer<VertexSkin> inSkins;      // parallel skin influences
[[vk::binding(2, 0)]] StructuredBuffer<float4x4> jointMatrices;  // the frame's joint palette
[[vk::binding(3, 0)]] RWStructuredBuffer<Vertex> outVertices;    // deformed output (this frame)

struct Push
{
    uint vertexCount;     // vertices in this skinned mesh-instance
    uint jointOffset;     // base of this instance's joints in the palette
    uint deformedOffset;  // base of this instance's vertices in the deformed buffer
    uint pad;
};
[[vk::push_constant]] Push push;

[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 tid: SV_DispatchThreadID)
{
    const uint i = tid.x;
    if (i >= push.vertexCount)
    {
        return;
    }
    const Vertex v = inVertices[i];
    const VertexSkin s = inSkins[i];
    const uint4 joints = uint4(s.jointsPacked.x & 0xFFFFu, s.jointsPacked.x >> 16,
                               s.jointsPacked.y & 0xFFFFu, s.jointsPacked.y >> 16);
    const uint base = push.jointOffset;
    const float4x4 skin = s.weights.x * jointMatrices[base + joints.x]
                        + s.weights.y * jointMatrices[base + joints.y]
                        + s.weights.z * jointMatrices[base + joints.z]
                        + s.weights.w * jointMatrices[base + joints.w];

    Vertex o;
    o.position = mul(skin, float4(v.position, 1.0)).xyz;
    o.normal = normalize(mul((float3x3)skin, v.normal));
    o.uv0 = v.uv0;
    outVertices[push.deformedOffset + i] = o;
}
