// Builds the coarse voxel scene proxy DDGI traces against: one thread per voxel, tested
// against the scene's per-draw world AABBs (a small SSBO). A voxel inside any box stores
// that box's albedo (rgb) + occupancy (a=1); empty voxels are cleared to a=0. Box-shaped
// scenes voxelize exactly; arbitrary meshes get a coarse AABB fill — fine for a global GI
// proxy. Set 0: b0 = voxel 3D storage image (rgba16f), b1 = box SSBO. Push = grid + volume.

struct Box
{
    float4 boundsMin;  // xyz world min
    float4 boundsMax;  // xyz world max
    float4 albedo;     // rgb base color
};

[[vk::image_format("rgba16f")]]
[[vk::binding(0, 0)]] RWTexture3D<float4> voxels;
[[vk::binding(1, 0)]] StructuredBuffer<Box> boxes;

struct Push
{
    uint4 gridCount;     // xyz = voxel resolution, w = box count
    float4 volumeMin;    // xyz = world-space min corner of the volume
    float4 volumeExtent; // xyz = world-space size of the volume
};
[[vk::push_constant]] Push pc;

[shader("compute")]
[numthreads(4, 4, 4)]
void computeMain(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= pc.gridCount.x || tid.y >= pc.gridCount.y || tid.z >= pc.gridCount.z)
    {
        return;
    }
    // Voxel center in world space.
    float3 t = (float3(tid) + 0.5) / float3(pc.gridCount.xyz);
    float3 world = pc.volumeMin.xyz + t * pc.volumeExtent.xyz;

    float4 result = float4(0.0, 0.0, 0.0, 0.0);
    for (uint i = 0; i < pc.gridCount.w; i = i + 1)
    {
        Box b = boxes[i];
        if (all(world >= b.boundsMin.xyz) && all(world <= b.boundsMax.xyz))
        {
            result = float4(b.albedo.rgb, 1.0);
            break;
        }
    }
    voxels[tid] = result;
}
