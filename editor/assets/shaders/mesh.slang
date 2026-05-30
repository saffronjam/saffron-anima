// Instanced mesh shader. Vertex inputs come from an interleaved buffer
// (position/normal/uv0); per-instance model + normal matrix + base color come from
// a storage buffer (set 2) indexed by the absolute Vulkan InstanceIndex
// (firstInstance + instance). SV_VulkanInstanceID is that builtin — plain
// SV_InstanceID is base-relative (excludes firstInstance), which would break batching.
// The push constant is the camera viewProj. Set 0 is albedo; set 1 is the lighting set:
// directional UBO (0), punctual light list (1), per-cluster light lists (2), cluster
// params UBO (3). Punctual lights are clustered-forward: the fragment loops only its
// froxel cluster's lights (params.screenSize.z != 0), or every light otherwise.

static const uint MAX_LIGHTS_PER_CLUSTER = 64;

struct VertexInput
{
    [[vk::location(0)]] float3 position;
    [[vk::location(1)]] float3 normal;
    [[vk::location(2)]] float2 uv0;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float3 worldPos : POSITION0;
    float3 worldNormal : NORMAL;
    float2 uv0 : TEXCOORD0;
    float4 baseColor : COLOR0;
};

struct Camera
{
    float4x4 viewProj;
};
[[vk::push_constant]] Camera camera;

[[vk::binding(0, 0)]] Sampler2D albedo;

struct LightGlobals
{
    float4 directionAmbient;  // xyz = travel direction, w = ambient
    float4 colorIntensity;    // rgb = color, a = intensity
    uint4 counts;             // x = punctual light count
};
[[vk::binding(0, 1)]] ConstantBuffer<LightGlobals> globals;

struct GpuLight
{
    float4 positionRange;   // xyz = world position, w = range
    float4 colorIntensity;  // rgb = color, a = intensity
    float4 directionType;   // xyz = world direction (spot), w = type (0 point, 1 spot)
    float4 spotCos;         // x = cos(inner half-angle), y = cos(outer half-angle)
};
[[vk::binding(1, 1)]] StructuredBuffer<GpuLight> lights;

struct Cluster
{
    uint count;
    uint indices[MAX_LIGHTS_PER_CLUSTER];
};
[[vk::binding(2, 1)]] StructuredBuffer<Cluster> clusters;

struct ClusterParams
{
    float4x4 view;
    float4x4 inverseProjection;
    uint4 gridSize;       // xyz = grid dims, w = light count
    uint4 screenSize;     // xy = pixels, z = clustered flag
    float4 zPlanes;       // x = near, y = far
};
[[vk::binding(3, 1)]] ConstantBuffer<ClusterParams> clusterParams;

struct Instance
{
    float4x4 model;
    float4x4 normalMatrix;
    float4 baseColor;
};
[[vk::binding(0, 2)]] StructuredBuffer<Instance> instances;

[shader("vertex")]
VertexOutput vertexMain(VertexInput input, uint instanceIndex : SV_VulkanInstanceID)
{
    Instance inst = instances[instanceIndex];
    VertexOutput output;
    float4 worldPos = mul(inst.model, float4(input.position, 1.0));
    output.worldPos = worldPos.xyz;
    output.position = mul(camera.viewProj, worldPos);
    output.worldNormal = mul((float3x3)inst.normalMatrix, input.normal);
    output.uv0 = input.uv0;
    output.baseColor = inst.baseColor;
    return output;
}

// UE4-style punctual attenuation: inverse-square with a smooth range window.
float distanceAttenuation(float dist, float range)
{
    float invSquare = 1.0 / max(dist * dist, 0.0001);
    float t = saturate(1.0 - pow(dist / range, 4.0));
    return invSquare * t * t;
}

// One punctual light's diffuse contribution at a world-space surface point.
float3 punctual(GpuLight lt, float3 worldPos, float3 n)
{
    float3 toLight = lt.positionRange.xyz - worldPos;
    float dist = length(toLight);
    if (dist > lt.positionRange.w)
    {
        return float3(0.0);
    }
    float3 l = toLight / max(dist, 0.0001);
    float nl = max(dot(n, l), 0.0);
    float attenuation = distanceAttenuation(dist, lt.positionRange.w);

    float cone = 1.0;
    if (lt.directionType.w > 0.5)
    {
        float3 spotDir = normalize(lt.directionType.xyz);
        float cosAngle = dot(spotDir, -l);
        cone = smoothstep(lt.spotCos.y, lt.spotCos.x, cosAngle);
    }
    return lt.colorIntensity.rgb * lt.colorIntensity.a * nl * attenuation * cone;
}

// Froxel cluster index for a fragment, from its pixel position + view-space depth.
uint clusterIndexFor(float2 pixel, float viewZ)
{
    float near = clusterParams.zPlanes.x;
    float far = clusterParams.zPlanes.y;
    float depth = max(-viewZ, near);
    uint zSlice = uint(log(depth / near) / log(far / near) * float(clusterParams.gridSize.z));
    zSlice = min(zSlice, clusterParams.gridSize.z - 1);

    float2 tileSize = float2(clusterParams.screenSize.xy) / float2(clusterParams.gridSize.xy);
    uint tileX = min(uint(pixel.x / tileSize.x), clusterParams.gridSize.x - 1);
    uint tileY = min(uint(pixel.y / tileSize.y), clusterParams.gridSize.y - 1);
    return tileX + tileY * clusterParams.gridSize.x + zSlice * clusterParams.gridSize.x * clusterParams.gridSize.y;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target
{
    float3 n = normalize(input.worldNormal);
    float4 tex = albedo.Sample(input.uv0);

    // Directional light + ambient.
    float3 lightDir = normalize(globals.directionAmbient.xyz);
    float ndotl = max(dot(n, -lightDir), 0.0);
    float3 lit = globals.colorIntensity.rgb * globals.colorIntensity.a * ndotl + globals.directionAmbient.w;

    // Punctual lights — clustered when enabled, else loop every light.
    if (clusterParams.screenSize.z != 0)
    {
        float viewZ = mul(clusterParams.view, float4(input.worldPos, 1.0)).z;
        uint clusterIndex = clusterIndexFor(input.position.xy, viewZ);
        uint count = clusters[clusterIndex].count;
        for (uint i = 0; i < count; i = i + 1)
        {
            lit += punctual(lights[clusters[clusterIndex].indices[i]], input.worldPos, n);
        }
    }
    else
    {
        for (uint i = 0; i < globals.counts.x; i = i + 1)
        {
            lit += punctual(lights[i], input.worldPos, n);
        }
    }

    float3 color = tex.rgb * input.baseColor.rgb * lit;
    return float4(color, tex.a * input.baseColor.a);
}
