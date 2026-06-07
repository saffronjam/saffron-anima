struct VertexInput
{
    [[vk::location(0)]] float2 position;
    [[vk::location(1)]] float4 color;
    [[vk::location(2)]] float4 edge;  // xy: signed coords per feather direction (±1 at the
                                      // nominal edge), zw: half-extents in pixels; a
                                      // non-positive half-extent disables that direction
};

struct VertexOutput
{
    float4 position : SV_Position;
    float4 color : COLOR0;
    float4 edge : TEXCOORD0;
};

[shader("vertex")]
VertexOutput vertexMain(VertexInput input)
{
    VertexOutput output;
    output.position = float4(input.position, 0.0, 1.0);
    output.color = input.color;
    output.edge = input.edge;
    return output;
}

// Approximate pixel coverage from an interpolated signed edge coordinate: 0.5 at the
// nominal edge, reaching 0/1 over ~1px on either side.
float coverage(float x, float halfPx)
{
    return halfPx > 0.0 ? clamp((1.0 - abs(x)) * halfPx + 0.5, 0.0, 1.0) : 1.0;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target
{
    // The overlay draws after the MSAA resolve, so edges are anti-aliased analytically.
    float4 color = input.color;
    color.a *= coverage(input.edge.x, input.edge.z) * coverage(input.edge.y, input.edge.w);
    return color;
}
