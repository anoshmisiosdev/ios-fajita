/*
 * SweetMTLDevice - the iOS Metal driver class Metal.framework instantiates for
 * the SweetGPU accelerator (Adreno 6xx, OnePlus 6T / branch fajita-gpu).
 *
 * Metal does not dlopen this bundle itself. It reads two properties off the
 * IOAcceleratorES service CoreAnimation looks for:
 *
 *      MetalPluginName      = ../../../usr/lib/sweetmtl/SweetMTLDriver
 *      MetalPluginClassName = SweetMTLDevice
 *
 * (published by the kernel, iokit/Kernel/arm/SweetGPU.cpp, boot-args
 * `sweetaccel=1 sweetmtl=... sweetmtlclass=...`), loads
 * .../SweetMTLDriver.bundle and instantiates the named class. That is the same
 * path NVAccel.kext + NVMTLDriver.bundle use on macOS to load a Metal driver
 * that Apple did not sign, and it is exactly what the code-signing relaxation
 * already in patch 0085 exists to allow: cs_enforcement_disable=1 plus the AMFI
 * policy syscall (op 0x5a -> 0x7b, policy renamed "AMFI") let dyld/mmap load an
 * ad-hoc-signed bundle into backboardd. See the "Loading the userland driver"
 * section of FAJITA.md.
 *
 * MTLIOAccelDevice is Metal's private wrapper around an IOAccelerator user
 * client; Metal calls -initWithAcceleratorPort: with the accelerator port. The
 * interface is declared here because the header is private (the macOS driver
 * does the same). If iOS's Metal.framework does not export it, this class fails
 * to instantiate - the first thing to check in the boot log.
 *
 * This is the load milestone: prove Metal loads the bundle and calls
 * initWithAcceleratorPort:. The MTLDevice method surface on top of Vulkan
 * (Mesa Turnip/freedreno) with the metal2vulkan translator is what gets filled
 * in next; see README.md.
 */

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

/* Metal's private base class: same contract NullMoth's NVMTLDevice uses. */
@interface MTLIOAccelDevice : NSObject
- (instancetype)initWithAcceleratorPort:(mach_port_t)port;
@end

@interface SweetMTLDevice : MTLIOAccelDevice <MTLDevice>
@end

static void
sweetmtl_log(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "sweetmtl: ");
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

/*
 * The Vulkan backend and the metal2vulkan translator are staged next to the
 * bundle by build.sh. The ICD is Mesa's Turnip freedreno driver, built like
 * tools/sweetgl (mesa-setup.sh) but with -Dvulkan-drivers=freedreno.
 */
static const char *
sweetmtl_pick(const char *staged, const char *dev)
{
	if (staged != NULL && access(staged, R_OK) == 0) {
		return staged;
	}
	return dev;
}

@implementation SweetMTLDevice

- (instancetype)initWithAcceleratorPort:(mach_port_t)port
{
	sweetmtl_log("initWithAcceleratorPort:%u pid %d (%s)", port, getpid(), getprogname());
	self = [super initWithAcceleratorPort:port];
	if (self == nil) {
		sweetmtl_log("super initWithAcceleratorPort: returned nil (MTLIOAccelDevice missing?)");
		return nil;
	}
	sweetmtl_log("Metal device live - SweetGPU driver bundle loaded");

	const char *icd = sweetmtl_pick(
	    "/usr/lib/sweetmtl/libvulkan_freedreno.dylib",
	    "/usr/lib/libvulkan_freedreno.dylib");
	void *vk = icd ? dlopen(icd, RTLD_NOW | RTLD_LOCAL) : NULL;
	sweetmtl_log("Vulkan ICD %s -> %s", icd ? icd : "(none)",
	    vk ? "loaded" : (dlerror() ?: "open failed"));

	return self;
}

@end
