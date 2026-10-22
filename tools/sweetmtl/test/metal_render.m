/*
 * metal_render - the driver's render test, from a normal Metal client: one triangle drawn into a
 * private render target, the texels copied out with a blit, checked, and written to disk so a run
 * on the driver can be compared texel for texel with a run on another Metal device.
 *
 *     metal_render <triangle.metallib> <out-prefix>
 *
 * It uses the MTLDevice, MTLTexture, MTLRenderPipelineState, MTLRenderCommandEncoder and
 * MTLBlitCommandEncoder surface: `newTextureWithDescriptor:`,
 * `newRenderPipelineStateWithDescriptor:error:`, `renderCommandEncoderWithDescriptor:`,
 * `setVertexBuffer:`, `setVertexBytes:`, `drawPrimitives:`, `blitCommandEncoder` and
 * `copyFromTexture:...toBuffer:`.
 *
 * The device is test_device.h's: MTLCreateSystemDefaultDevice() - on a Mac that is Apple's own
 * Metal driver, which is what makes the reference image - or with SWEETMTL_DIRECT=<bundle>,
 * SweetMTLDevice out of that bundle.
 *
 * The picture: SIDE x SIDE RGBA8Unorm, cleared to (0.2, 0.2, 0.2, 1); a triangle whose vertices
 * (0, 1), (-1, -1), (1, -1) are scaled by 0.8 in the vertex function (a constant set with
 * -setVertexBytes:), red at the top, green bottom left, blue bottom right. The tool checks what
 * any correct Metal device draws - the clear color in the corners, each vertex's color dominant
 * near it, a grey near (85, 85, 85) at the centroid - and then:
 *
 *   <out-prefix>.rgba   the texels, row 0 first (SIDE * SIDE * 4 bytes, no header)
 *   <out-prefix>.png    the same, for a person
 *
 * With SWEETMTL_REFERENCE=<ref.rgba> it also compares every texel with that file (another run's
 * .rgba) and reports how many differ and by how much; it fails when any channel differs by more
 * than SWEETMTL_TOLERANCE (default 1, for a rounding of the interpolated colors).
 *
 * Exit 0 and "...: OK", or 1 with what failed.
 */

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>

#import "test_device.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIDE 64
#define BYTES_PER_ROW (SIDE * 4)
#define IMAGE_BYTES (BYTES_PER_ROW * SIDE)

static int
failed(NSString *what)
{
	fprintf(stderr, "FAIL %s\n", what.UTF8String);
	return 1;
}

static int
failed_with_error(NSString *what, NSError *error)
{
	fprintf(stderr, "FAIL %s: %s\n", what.UTF8String,
	    error != nil ? error.localizedDescription.UTF8String : "(no error)");
	return 1;
}

static const unsigned char *
texel_at(const unsigned char *texels, int x, int y)
{
	return texels + (size_t)y * BYTES_PER_ROW + (size_t)x * 4;
}

/* The pixel of Metal's normalized device coordinates (y up, row 0 at the top). */
static void
pixel_of(float ndc_x, float ndc_y, int *x, int *y)
{
	*x = (int)((ndc_x + 1.0f) * 0.5f * SIDE);
	*y = (int)((1.0f - ndc_y) * 0.5f * SIDE);
}

/* What any Metal device draws for this picture, without a reference to compare with. */
static int
check_picture(const unsigned char *texels)
{
	static const unsigned char cleared[4] = {51, 51, 51, 255};
	const int corners[4][2] = {{0, 0}, {SIDE - 1, 0}, {0, SIDE - 1}, {SIDE - 1, SIDE - 1}};
	for (int i = 0; i < 4; i++) {
		const unsigned char *texel = texel_at(texels, corners[i][0], corners[i][1]);
		if (memcmp(texel, cleared, 4) != 0) {
			return failed([NSString stringWithFormat:@"corner (%d, %d) is %u %u %u %u, not the "
			                                         @"clear color 51 51 51 255",
			                              corners[i][0], corners[i][1], texel[0], texel[1],
			                              texel[2], texel[3]]);
		}
	}
	/* Near each vertex, inside the triangle, its own channel is the one that dominates. */
	const struct {
		const char *name;
		float x, y;
		int channel;
	} near[3] = {
		{"top (red)", 0.0f, 0.65f, 0},
		{"bottom left (green)", -0.6f, -0.72f, 1},
		{"bottom right (blue)", 0.6f, -0.72f, 2},
	};
	for (int i = 0; i < 3; i++) {
		int x, y;
		pixel_of(near[i].x, near[i].y, &x, &y);
		const unsigned char *texel = texel_at(texels, x, y);
		for (int c = 0; c < 3; c++) {
			if (c != near[i].channel && texel[c] >= texel[near[i].channel]) {
				return failed([NSString stringWithFormat:@"near the %s vertex, (%d, %d) is %u %u "
				                                         @"%u %u: its channel does not dominate",
				                              near[i].name, x, y, texel[0], texel[1], texel[2],
				                              texel[3]]);
			}
		}
		printf("near the %s vertex: (%d, %d) = %u %u %u %u\n", near[i].name, x, y, texel[0],
		    texel[1], texel[2], texel[3]);
	}
	/* The centroid of (0, 0.8), (-0.8, -0.8), (0.8, -0.8) is (0, -0.267): a third of each. */
	int x, y;
	pixel_of(0.0f, -0.8f / 3.0f, &x, &y);
	const unsigned char *texel = texel_at(texels, x, y);
	for (int c = 0; c < 3; c++) {
		if (abs((int)texel[c] - 85) > 8) {
			return failed([NSString stringWithFormat:@"the centroid (%d, %d) is %u %u %u %u, not "
			                                         @"near 85 85 85 255",
			                              x, y, texel[0], texel[1], texel[2], texel[3]]);
		}
	}
	printf("centroid: (%d, %d) = %u %u %u %u\n", x, y, texel[0], texel[1], texel[2], texel[3]);
	return 0;
}

/* A 16x16 sketch of the picture, each cell the dominant channel of its centre texel (R, G, B) or
 * '.' for the clear color: a log line a person can read. */
static void
print_sketch(const unsigned char *texels)
{
	for (int row = 0; row < 16; row++) {
		char line[17];
		for (int column = 0; column < 16; column++) {
			const unsigned char *texel =
			    texel_at(texels, column * (SIDE / 16) + SIDE / 32, row * (SIDE / 16) + SIDE / 32);
			if (texel[0] == 51 && texel[1] == 51 && texel[2] == 51) {
				line[column] = '.';
			} else if (texel[0] >= texel[1] && texel[0] >= texel[2]) {
				line[column] = 'R';
			} else if (texel[1] >= texel[2]) {
				line[column] = 'G';
			} else {
				line[column] = 'B';
			}
		}
		line[16] = 0;
		printf("  %s\n", line);
	}
}

static BOOL
write_png(const unsigned char *texels, NSString *path)
{
	CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
	CGDataProviderRef provider = CGDataProviderCreateWithData(NULL, texels, IMAGE_BYTES, NULL);
	CGImageRef image = CGImageCreate(SIDE, SIDE, 8, 32, BYTES_PER_ROW, space,
	    (CGBitmapInfo)kCGImageAlphaLast | kCGBitmapByteOrderDefault, provider, NULL, false,
	    kCGRenderingIntentDefault);
	CGImageDestinationRef destination = CGImageDestinationCreateWithURL(
	    (__bridge CFURLRef)[NSURL fileURLWithPath:path], CFSTR("public.png"), 1, NULL);
	BOOL written = NO;
	if (image != NULL && destination != NULL) {
		CGImageDestinationAddImage(destination, image, NULL);
		written = CGImageDestinationFinalize(destination);
	}
	if (destination != NULL) {
		CFRelease(destination);
	}
	CGImageRelease(image);
	CGDataProviderRelease(provider);
	CGColorSpaceRelease(space);
	return written;
}

/* Every texel against another run's, and the verdict at the tolerance. */
static int
compare(const unsigned char *texels, NSString *reference_path)
{
	NSData *reference = [NSData dataWithContentsOfFile:reference_path];
	if (reference.length != IMAGE_BYTES) {
		return failed([NSString stringWithFormat:@"the reference %@ is %lu bytes, not %d",
		                              reference_path, (unsigned long)reference.length,
		                              IMAGE_BYTES]);
	}
	const char *tolerance_text = getenv("SWEETMTL_TOLERANCE");
	int tolerance = tolerance_text != NULL ? atoi(tolerance_text) : 1;
	const unsigned char *expected = reference.bytes;
	int differing = 0, max_difference = 0, worst_x = 0, worst_y = 0;
	for (int y = 0; y < SIDE; y++) {
		for (int x = 0; x < SIDE; x++) {
			const unsigned char *got = texel_at(texels, x, y);
			const unsigned char *want = texel_at(expected, x, y);
			int difference = 0;
			for (int c = 0; c < 4; c++) {
				int d = abs((int)got[c] - (int)want[c]);
				difference = d > difference ? d : difference;
			}
			if (difference != 0) {
				differing++;
			}
			if (difference > max_difference) {
				max_difference = difference;
				worst_x = x;
				worst_y = y;
			}
		}
	}
	printf("against %s: %d of %d texels differ, by %d at most", reference_path.UTF8String,
	    differing, SIDE * SIDE, max_difference);
	if (max_difference != 0) {
		const unsigned char *got = texel_at(texels, worst_x, worst_y);
		const unsigned char *want = texel_at(expected, worst_x, worst_y);
		printf(" (at (%d, %d): %u %u %u %u here, %u %u %u %u there)", worst_x, worst_y, got[0],
		    got[1], got[2], got[3], want[0], want[1], want[2], want[3]);
	}
	printf("\n");
	if (max_difference > tolerance) {
		return failed([NSString stringWithFormat:@"a channel differs from the reference by %d, "
		                                         @"more than the tolerance %d",
		                              max_difference, tolerance]);
	}
	return 0;
}

static int
run(NSString *library_path, NSString *prefix)
{
	id<MTLDevice> device = make_device();
	if (device == nil) {
		return 1;
	}
	printf("device: %s (%s)\n", device.name.UTF8String, NSStringFromClass([device class]).UTF8String);

	NSError *error = nil;
	NSData *contents = [NSData dataWithContentsOfFile:library_path options:0 error:&error];
	if (contents == nil) {
		return failed_with_error(@"the library", error);
	}
	dispatch_data_t data = dispatch_data_create(contents.bytes, contents.length, NULL,
	    DISPATCH_DATA_DESTRUCTOR_DEFAULT);
	id<MTLLibrary> library = [device newLibraryWithData:data error:&error];
	if (library == nil) {
		return failed_with_error(@"newLibraryWithData:error:", error);
	}
	id<MTLFunction> vertex = [library newFunctionWithName:@"triangle_vertex"];
	id<MTLFunction> fragment = [library newFunctionWithName:@"triangle_fragment"];
	if (vertex == nil || fragment == nil) {
		return failed(@"newFunctionWithName: (triangle_vertex, triangle_fragment)");
	}
	if (vertex.functionType != MTLFunctionTypeVertex ||
	    fragment.functionType != MTLFunctionTypeFragment) {
		return failed([NSString stringWithFormat:@"function types %lu and %lu, not vertex (1) and "
		                                         @"fragment (2)",
		                              (unsigned long)vertex.functionType,
		                              (unsigned long)fragment.functionType]);
	}
	printf("library: metallib: %s (vertex + fragment)\n", library_path.UTF8String);

	MTLRenderPipelineDescriptor *pipeline_descriptor = [MTLRenderPipelineDescriptor new];
	pipeline_descriptor.label = @"metal_render triangle";
	pipeline_descriptor.vertexFunction = vertex;
	pipeline_descriptor.fragmentFunction = fragment;
	pipeline_descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
	id<MTLRenderPipelineState> pipeline =
	    [device newRenderPipelineStateWithDescriptor:pipeline_descriptor error:&error];
	if (pipeline == nil) {
		return failed_with_error(@"newRenderPipelineStateWithDescriptor:error:", error);
	}
	printf("render pipeline: RGBA8Unorm, no blending\n");

	MTLTextureDescriptor *texture_descriptor =
	    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
	                                                       width:SIDE
	                                                      height:SIDE
	                                                   mipmapped:NO];
	texture_descriptor.usage = MTLTextureUsageRenderTarget;
	texture_descriptor.storageMode = MTLStorageModePrivate;
	id<MTLTexture> target = [device newTextureWithDescriptor:texture_descriptor];
	if (target == nil) {
		return failed(@"newTextureWithDescriptor:");
	}
	printf("texture: %lux%lu, pixel format %lu, private\n", (unsigned long)target.width,
	    (unsigned long)target.height, (unsigned long)target.pixelFormat);

	const float positions[3][2] = {{0.0f, 1.0f}, {-1.0f, -1.0f}, {1.0f, -1.0f}};
	const float colors[3][4] = {
	    {1.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f, 1.0f}};
	const float scale = 0.8f;
	id<MTLBuffer> position_buffer = [device newBufferWithBytes:positions
	                                                   length:sizeof positions
	                                                  options:MTLResourceStorageModeShared];
	id<MTLBuffer> color_buffer = [device newBufferWithBytes:colors
	                                                length:sizeof colors
	                                               options:MTLResourceStorageModeShared];
	id<MTLBuffer> readback = [device newBufferWithLength:IMAGE_BYTES
	                                             options:MTLResourceStorageModeShared];
	if (position_buffer == nil || color_buffer == nil || readback == nil) {
		return failed(@"newBuffer...:options:");
	}
	/* A pattern the GPU must overwrite, so a copy that did nothing cannot pass. */
	memset(readback.contents, 0xa5, IMAGE_BYTES);

	MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
	pass.colorAttachments[0].texture = target;
	pass.colorAttachments[0].loadAction = MTLLoadActionClear;
	pass.colorAttachments[0].storeAction = MTLStoreActionStore;
	pass.colorAttachments[0].clearColor = MTLClearColorMake(0.2, 0.2, 0.2, 1.0);

	id<MTLCommandQueue> queue = [device newCommandQueue];
	id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
	id<MTLRenderCommandEncoder> encoder =
	    [command_buffer renderCommandEncoderWithDescriptor:pass];
	if (encoder == nil) {
		return failed_with_error(@"renderCommandEncoderWithDescriptor:", command_buffer.error);
	}
	[encoder setRenderPipelineState:pipeline];
	[encoder setVertexBuffer:position_buffer offset:0 atIndex:0];
	[encoder setVertexBuffer:color_buffer offset:0 atIndex:1];
	[encoder setVertexBytes:&scale length:sizeof scale atIndex:2];
	[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
	[encoder endEncoding];

	id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
	if (blit == nil) {
		return failed_with_error(@"blitCommandEncoder", command_buffer.error);
	}
	[blit copyFromTexture:target
	                 sourceSlice:0
	                 sourceLevel:0
	                sourceOrigin:MTLOriginMake(0, 0, 0)
	                  sourceSize:MTLSizeMake(SIDE, SIDE, 1)
	                    toBuffer:readback
	           destinationOffset:0
	      destinationBytesPerRow:BYTES_PER_ROW
	    destinationBytesPerImage:IMAGE_BYTES];
	[blit endEncoding];
	if (command_buffer.error != nil) {
		return failed_with_error(@"encoding", command_buffer.error);
	}

	[command_buffer commit];
	printf("committed; waiting\n");
	[command_buffer waitUntilCompleted];
	if (command_buffer.error != nil) {
		return failed_with_error(@"the command buffer", command_buffer.error);
	}
	if (command_buffer.status != MTLCommandBufferStatusCompleted) {
		return failed([NSString stringWithFormat:@"the command buffer's status is %lu, not "
		                                         @"Completed",
		                              (unsigned long)command_buffer.status]);
	}
	printf("status: Completed\n");

	const unsigned char *texels = readback.contents;
	print_sketch(texels);
	NSString *raw_path = [prefix stringByAppendingString:@".rgba"];
	NSString *png_path = [prefix stringByAppendingString:@".png"];
	if (![[NSData dataWithBytes:texels length:IMAGE_BYTES] writeToFile:raw_path atomically:YES] ||
	    !write_png(texels, png_path)) {
		return failed([NSString stringWithFormat:@"cannot write %@ / %@", raw_path, png_path]);
	}
	printf("wrote %s and %s\n", raw_path.UTF8String, png_path.UTF8String);
	if (check_picture(texels) != 0) {
		return 1;
	}
	const char *reference = getenv("SWEETMTL_REFERENCE");
	if (reference != NULL && reference[0] != '\0' && compare(texels, @(reference)) != 0) {
		return 1;
	}
	printf("sweetmtl: metal_render through Metal.framework: OK\n");
	return 0;
}

int
main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: %s <triangle.metallib> <out-prefix>\n", argv[0]);
		return 2;
	}
	setvbuf(stdout, NULL, _IONBF, 0);
	@autoreleasepool {
		return run(@(argv[1]), @(argv[2]));
	}
}
