// Infinite analytic ground grid (debug overlay). A fullscreen triangle (no vertex buffer); the
// fragment reconstructs the world-space view ray from the inverse view-projection, intersects the
// y = 0 plane, and draws anti-aliased grid lines whose width is one screen pixel via fwidth. The
// fragment writes SV_Depth (the plane point reprojected) so scene geometry occludes the grid, and
// it fades with distance so the horizon never aliases. Runs at 1x after tonemap, alpha-blended
// over the resolved scene color — color is display-referred (no tonemap applied to it).

struct GridPush
{
    float4x4 viewProj;     // world -> clip (this frame's camera) — for the fragment SV_Depth
    float4x4 invViewProj;  // clip -> world (inverted) — for the view-ray reconstruction
};
[[vk::push_constant]] GridPush push;

struct VertexOutput
{
    float4 position : SV_Position;
    float2 ndc : TEXCOORD0;
};

struct FragmentOutput
{
    float4 color : SV_Target;
    float depth : SV_Depth;
};

[shader("vertex")]
VertexOutput vertexMain(uint vertexId : SV_VertexID)
{
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    VertexOutput output;
    output.ndc = uv * 2.0 - 1.0;
    output.position = float4(output.ndc, 0.0, 1.0);
    return output;
}

// One grid scale: anti-aliased lines at integer world units on the y = 0 plane, the axis
// through z = 0 tinted red (X) and through x = 0 tinted blue (Z), like a standard editor grid.
[shader("fragment")]
FragmentOutput fragmentMain(VertexOutput input)
{
    float4 nearW = mul(push.invViewProj, float4(input.ndc, 0.0, 1.0));
    float4 farW = mul(push.invViewProj, float4(input.ndc, 1.0, 1.0));
    float3 ro = nearW.xyz / nearW.w;
    float3 rd = normalize(farW.xyz / farW.w - ro);

    FragmentOutput output;
    // Parallel to, or looking away from, the ground plane — nothing to draw.
    if (abs(rd.y) < 1e-5)
    {
        output.color = float4(0.0, 0.0, 0.0, 0.0);
        output.depth = 1.0;
        return output;
    }
    float t = -ro.y / rd.y;
    if (t <= 0.0)
    {
        output.color = float4(0.0, 0.0, 0.0, 0.0);
        output.depth = 1.0;
        return output;
    }
    float3 p = ro + rd * t;

    float2 coord = p.xz;
    float2 deriv = fwidth(coord);
    float2 grid = abs(frac(coord - 0.5) - 0.5) / max(deriv, 1e-6);
    float lineDist = min(grid.x, grid.y);
    float alpha = 1.0 - min(lineDist, 1.0);

    // Distance fade so the converging far lines dissolve instead of shimmering.
    float dist = length(p - ro);
    float fade = 1.0 - smoothstep(40.0, 90.0, dist);
    alpha *= fade;

    float3 color = float3(0.55, 0.55, 0.6);
    if (abs(p.z) < deriv.y)
    {
        color = float3(0.85, 0.3, 0.3);  // world X axis (z = 0)
    }
    else if (abs(p.x) < deriv.x)
    {
        color = float3(0.3, 0.45, 0.9);  // world Z axis (x = 0)
    }

    float4 clip = mul(push.viewProj, float4(p, 1.0));
    output.color = float4(color, alpha);
    output.depth = clip.z / clip.w;  // Vulkan [0,1] clip depth; geometry in front occludes the grid
    return output;
}
