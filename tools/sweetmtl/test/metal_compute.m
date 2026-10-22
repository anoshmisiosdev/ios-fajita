/*
 * metal_compute - the driver's end-to-end test, from a normal Metal client:
 * MTLCreateSystemDefaultDevice() and the MTLDevice / MTLBuffer / MTLLibrary /
 * MTLComputePipelineState / MTLCommandBuffer / MTLComputeCommandEncoder surface, over
 * SweetMTLDevice, over the bridge, over metal2vulkan, over the Vulkan device. One dispatch, its
 * result read back on the CPU and checked.
 *
 *     metal_compute [<library>]
 *
 * `<library>` is a metallib, built from scale_and_offset.metal next to this file, and is loaded
 * with -newLibraryWithData:error: the way any Metal client loads one. With SWEETMTL_AIR=1 the
 * argument is AIR bitcode instead (`llvm-as` output: the translator's own fixture, which the
 * bridge's C test uses), which Metal has no selector for - that goes through the driver's private
 * door, SweetMTLAirLibrary.
 *
 * MTLCreateSystemDefaultDevice() is the real path: Metal reads the IOAcceleratorES service, loads
 * SweetMTLDriver.bundle and instantiates SweetMTLDevice. On a test machine there is no accelerator
 * service, so SWEETMTL_DIRECT=<driver bundle> instantiates the class from that bundle directly
 * instead (build.sh --host builds the bundle it wants); everything after the device is the same
 * code either way.
 *
 * The kernel is the bridge's C test's: 1000 floats, output[i] = input[i] * scale + i = 3i,
 * dispatched as 1000 threads in 64-wide threadgroups. Exit 0 and "...: OK", or 1 with what failed.
 */

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#import "SweetMTLDriver.h"
#import "test_device.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The bridge's C test's numbers, so both tests dispatch the same thing. */
#define COUNT 1000
#define THREADS_PER_THREADGROUP 64

static int
failed(NSString *what)
{
	fprintf(stderr, "FAIL %s\n", what.UTF8String);
	return 1;
}

/* The failure of a call that has an `error:` out, which says more than the selector does. */
static int
failed_with_error(NSString *what, NSError *error)
{
	fprintf(stderr, "FAIL %s: %s\n", what.UTF8String,
	    error != nil ? error.localizedDescription.UTF8String : "(no error)");
	return 1;
}

static const char *
status_name(MTLCommandBufferStatus status)
{
	switch (status) {
	case MTLCommandBufferStatusNotEnqueued:
		return "NotEnqueued";
	case MTLCommandBufferStatusEnqueued:
		return "Enqueued";
	case MTLCommandBufferStatusCommitted:
		return "Committed";
	case MTLCommandBufferStatusScheduled:
		return "Scheduled";
	case MTLCommandBufferStatusCompleted:
		return "Completed";
	case MTLCommandBufferStatusError:
		return "Error";
	}
	return "Unknown";
}

/* The library file, through the door the caller named: a metallib (Metal's own
 * -newLibraryWithData:error:), or AIR bitcode (the driver's private door). */
static id<MTLLibrary>
make_library(id<MTLDevice> device, NSString *path, BOOL air, NSError **error)
{
	NSData *contents = [NSData dataWithContentsOfFile:path options:0 error:error];
	if (contents == nil) {
		return nil;
	}
	if (air) {
		if (![device respondsToSelector:@selector(newLibraryWithAIR:error:)]) {
			*error = [NSError errorWithDomain:@"metal_compute"
			                             code:1
			                         userInfo:@{NSLocalizedDescriptionKey :
			                                        @"the device is not SweetMTLDevice: no AIR door"}];
			return nil;
		}
		return [(id<SweetMTLAirLibrary>)device newLibraryWithAIR:contents error:error];
	}
	/* A normal client hands Metal a dispatch_data_t; DISPATCH_DATA_DESTRUCTOR_DEFAULT copies the
	 * bytes, so nothing here has to outlive the call. */
	dispatch_data_t data = dispatch_data_create(contents.bytes, contents.length, NULL,
	    DISPATCH_DATA_DESTRUCTOR_DEFAULT);
	return [device newLibraryWithData:data error:error];
}

static int
run(NSString *library_path, BOOL air)
{
	id<MTLDevice> device = make_device();
	if (device == nil) {
		return 1;
	}
	printf("device: %s (%s)\n", device.name.UTF8String, NSStringFromClass([device class]).UTF8String);

	NSError *error = nil;
	id<MTLLibrary> library = make_library(device, library_path, air, &error);
	if (library == nil) {
		return failed_with_error(@"the library", error);
	}
	printf("library: %s: %s\n", air ? "AIR bitcode" : "metallib", library_path.UTF8String);
	/* A selector the bridge does not cover (it has no list of a library's functions): the driver
	 * answers it with the stub - one line on stderr and nil here - rather than by trapping, which
	 * is what makes a client's probes survivable. */
	printf("functionNames (not implemented by the bridge): %lu\n",
	    (unsigned long)library.functionNames.count);

	id<MTLFunction> function = [library newFunctionWithName:@"scale_and_offset"];
	if (function == nil) {
		return failed(@"newFunctionWithName:@\"scale_and_offset\"");
	}
	id<MTLComputePipelineState> pipeline =
	    [device newComputePipelineStateWithFunction:function error:&error];
	if (pipeline == nil) {
		return failed_with_error(@"newComputePipelineStateWithFunction:error:", error);
	}
	printf("pipeline: %lu threads per threadgroup at most, SIMD width %lu\n",
	    (unsigned long)pipeline.maxTotalThreadsPerThreadgroup,
	    (unsigned long)pipeline.threadExecutionWidth);

	float input[COUNT];
	for (int i = 0; i < COUNT; i++) {
		input[i] = (float)i;
	}
	const float scale = 2.0f;
	id<MTLBuffer> in = [device newBufferWithBytes:input
	                                      length:sizeof input
	                                     options:MTLResourceStorageModeShared];
	id<MTLBuffer> out = [device newBufferWithLength:COUNT * sizeof(float)
	                                       options:MTLResourceStorageModeShared];
	id<MTLBuffer> scale_buffer = [device newBufferWithBytes:&scale
	                                                 length:sizeof scale
	                                                options:MTLResourceStorageModeShared];
	if (in == nil || out == nil || scale_buffer == nil) {
		return failed(@"newBuffer...:options:");
	}
	/* What the CPU sees of a shared buffer, before the GPU is asked for anything: this is what the
	 * kernel reads and where its result lands. */
	if (in.length != sizeof input || memcmp(in.contents, input, sizeof input) != 0) {
		return failed(@"the input buffer's contents are not what was written to it");
	}

	id<MTLCommandQueue> queue = [device newCommandQueue];
	id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
	id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
	if (encoder == nil) {
		return failed(@"computeCommandEncoder");
	}
	[encoder setComputePipelineState:pipeline];
	[encoder setBuffer:in offset:0 atIndex:0];
	[encoder setBuffer:out offset:0 atIndex:1];
	[encoder setBuffer:scale_buffer offset:0 atIndex:2];
	[encoder dispatchThreads:MTLSizeMake(COUNT, 1, 1)
	    threadsPerThreadgroup:MTLSizeMake(THREADS_PER_THREADGROUP, 1, 1)];
	[encoder endEncoding];
	if (command_buffer.error != nil) {
		return failed_with_error(@"encoding", command_buffer.error);
	}

	[command_buffer commit];
	printf("committed; waiting\n");
	[command_buffer waitUntilCompleted];
	printf("status: %s\n", status_name(command_buffer.status));
	if (command_buffer.error != nil) {
		return failed_with_error(@"the command buffer", command_buffer.error);
	}
	if (command_buffer.status != MTLCommandBufferStatusCompleted) {
		return failed([NSString stringWithFormat:@"the command buffer is %s, not Completed",
	                              status_name(command_buffer.status)]);
	}

	const float *output = (const float *)out.contents;
	if (output == NULL) {
		return failed(@"the output buffer has no CPU address");
	}
	for (int i = 0; i < COUNT; i++) {
		if (output[i] != 3.0f * (float)i) {
			return failed([NSString stringWithFormat:@"output[%d] is %g, not %g", i,
			                              (double)output[i], (double)(3 * i)]);
		}
	}
	printf("output[0..4] = %g %g %g %g, ..., output[%d] = %g: as expected\n", (double)output[0],
	    (double)output[1], (double)output[2], (double)output[3], COUNT - 1,
	    (double)output[COUNT - 1]);
	printf("sweetmtl: metal_compute through Metal.framework: OK\n");
	return 0;
}

int
main(int argc, char **argv)
{
	if (argc < 2 || argc > 3) {
		fprintf(stderr, "usage: %s <library>      (a .metallib; SWEETMTL_AIR=1: AIR bitcode)\n",
		    argv[0]);
		return 2;
	}
	/* Unbuffered, because a driver that dies inside its own shader compiler (llvmpipe's on a test
	 * machine; see the bridge's README) would otherwise take the progress report with it. */
	setvbuf(stdout, NULL, _IONBF, 0);
	@autoreleasepool {
		return run(@(argv[1]), getenv("SWEETMTL_AIR") != NULL);
	}
}
