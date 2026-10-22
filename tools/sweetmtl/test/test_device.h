/*
 * test_device.h - the device the driver's test tools run on, shared by metal_compute.m and
 * metal_render.m: MTLCreateSystemDefaultDevice() (Metal's own path: the IOAcceleratorES service and
 * SweetMTLDriver.bundle on the phone, Apple's GPU on a Mac), or with SWEETMTL_DIRECT=<bundle> the
 * driver's class instantiated from that bundle directly, with Metal not in the picture.
 */
#ifndef SWEETMTL_TEST_DEVICE_H
#define SWEETMTL_TEST_DEVICE_H

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <dlfcn.h>
#include <mach/mach.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>

@interface NSObject (SweetMTLTestDevice)
- (instancetype)initWithAcceleratorPort:(mach_port_t)port;
@end

/* The device Metal would hand a client on this machine: the system's, or - with
 * SWEETMTL_DIRECT=<bundle> - the driver's own class out of that bundle (see the top of this
 * file). */
static id<MTLDevice>
make_device(void)
{
	const char *bundle = getenv("SWEETMTL_DIRECT");
	if (bundle == NULL || bundle[0] == '\0') {
		id<MTLDevice> device = MTLCreateSystemDefaultDevice();
		if (device == nil) {
			fprintf(stderr, "FAIL MTLCreateSystemDefaultDevice() answered nil: no Metal device "
			                "(accelerator up? sweetaccel=1? driver installed?)\n");
			return nil;
		}
		return device;
	}
	/* A driver bundle's directory is what a caller has to hand: the image to dlopen is its
	 * executable (dlopen takes a file, not a bundle directory). */
	NSString *image = @(bundle);
	if ([image.pathExtension isEqualToString:@"bundle"]) {
		NSBundle *driver = [NSBundle bundleWithPath:image];
		image = driver.executablePath;
		if (image == nil) {
			fprintf(stderr, "FAIL %s is no driver bundle (no executable in it)\n", bundle);
			return nil;
		}
	}
	if (dlopen(image.fileSystemRepresentation, RTLD_NOW) == NULL) {
		fprintf(stderr, "FAIL dlopen(%s): %s\n", image.UTF8String, dlerror());
		return nil;
	}
	Class cls = objc_getClass("SweetMTLDevice");
	if (cls == Nil) {
		fprintf(stderr, "FAIL %s has no SweetMTLDevice class\n", bundle);
		return nil;
	}
	/* The entry point Metal calls, with no accelerator port: Metal is not in the picture here. */
	id device = [[cls alloc] initWithAcceleratorPort:MACH_PORT_NULL];
	if (device == nil) {
		fprintf(stderr, "FAIL the driver's -initWithAcceleratorPort: answered nil\n");
		return nil;
	}
	return (id<MTLDevice>)device;
}

#endif /* SWEETMTL_TEST_DEVICE_H */
