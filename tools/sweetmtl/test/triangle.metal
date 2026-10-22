/*
 * The pair metal_render draws: one triangle, a color per vertex (red at the top, green bottom
 * left, blue bottom right) interpolated across it, its positions scaled by a constant the client
 * sets with -setVertexBytes:length:atIndex:.
 *
 * Built like scale_and_offset.metal, by build.sh:
 *
 *   xcrun -sdk iphoneos metal -c triangle.metal -o triangle.air
 *   xcrun -sdk iphoneos metallib triangle.air -o triangle.metallib
 *
 * The vertex function reads its buffers by vertex_id (no vertex descriptor), which is what the
 * bridge's render pipeline takes.
 */
#include <metal_stdlib>

using namespace metal;

struct Varyings {
	float4 position [[position]];
	float4 color;
};

vertex Varyings
triangle_vertex(device const float2 *positions [[buffer(0)]],
                device const float4 *colors [[buffer(1)]],
                constant float &scale [[buffer(2)]],
                uint id [[vertex_id]])
{
	Varyings out;
	out.position = float4(positions[id] * scale, 0.0, 1.0);
	out.color = colors[id];
	return out;
}

fragment float4
triangle_fragment(Varyings in [[stage_in]])
{
	return in.color;
}
