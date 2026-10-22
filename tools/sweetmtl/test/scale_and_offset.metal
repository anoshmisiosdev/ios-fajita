/*
 * The kernel metal_compute dispatches, as a Metal client would write it: the same input the
 * bridge's C test uses (bridge/tests/compute.c), so both ends of the driver are tested with one
 * dispatch.
 *
 * The fixture the bridge's C test assembles with `llvm-as` (the translator's
 * runtime_examples.ll) declares the same function in AIR; this file is what compiles to a metallib
 * with the SDK's own `metal`/`metallib`, which is what a Metal client loads through
 * -newLibraryWithData:error:.
 *
 *   xcrun -sdk iphoneos metal -c scale_and_offset.metal -o scale_and_offset.air
 *   xcrun -sdk iphoneos metallib scale_and_offset.air -o scale_and_offset.metallib
 *
 * output[i] = input[i] * scale + i, dispatched as COUNT threads one per element, 64 per
 * threadgroup: 3i with scale 2.
 */
#include <metal_stdlib>

using namespace metal;

kernel void
scale_and_offset(device const float *in [[buffer(0)]],
                 device float *out [[buffer(1)]],
                 constant float &scale [[buffer(2)]],
                 uint i [[thread_position_in_grid]])
{
	out[i] = in[i] * scale + float(i);
}
