// Fixes the 1-texel octahedral gutter around each probe tile so hardware bilinear sampling
// across the tile interior wraps correctly. For each border texel, copy from the mirrored
// interior texel per the octahedral wrap rules. One thread per atlas texel; interior texels
// are left untouched. Set 0: b0 = irradiance atlas (rgba16f storage, read+write).

[[vk::image_format("rgba16f")]]
[[vk::binding(0, 0)]] RWTexture2D<float4> atlas;

struct Push
{
    uint4 probeCount;  // xyz = probes per axis
    uint4 tile;        // x = interior size
};
[[vk::push_constant]] Push pc;

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 tid : SV_DispatchThreadID)
{
    uint interior = pc.tile.x;
    uint full = interior + 2;
    uint tilesX = pc.probeCount.x * pc.probeCount.y;
    uint atlasW = tilesX * full;
    uint atlasH = pc.probeCount.z * full;
    if (tid.x >= atlasW || tid.y >= atlasH)
    {
        return;
    }
    uint2 local = uint2(tid.x % full, tid.y % full);
    bool isBorder = (local.x == 0 || local.y == 0 || local.x == full - 1 || local.y == full - 1);
    if (!isBorder)
    {
        return;
    }
    uint2 tileOrigin = uint2(tid.x - local.x, tid.y - local.y);

    // Octahedral border wrap: corners map to the diagonally-opposite interior corner; edges
    // map to the same edge reversed (the octahedron's fold). interior coords are 1..interior.
    int li = int(local.x);
    int lj = int(local.y);
    int last = int(interior);  // last interior coordinate
    int2 src;
    bool corner = (local.x == 0 || local.x == full - 1) && (local.y == 0 || local.y == full - 1);
    if (corner)
    {
        // Opposite interior corner.
        src.x = (local.x == 0) ? last : 1;
        src.y = (local.y == 0) ? last : 1;
    }
    else if (local.x == 0 || local.x == full - 1)
    {
        // Left/right border: same column edge, row reversed within the interior.
        src.x = (local.x == 0) ? 1 : last;
        src.y = last - (lj - 1);
    }
    else
    {
        // Top/bottom border: same row edge, column reversed.
        src.y = (local.y == 0) ? 1 : last;
        src.x = last - (li - 1);
    }
    uint2 srcPx = tileOrigin + uint2(src);
    atlas[tid.xy] = atlas[srcPx];
}
