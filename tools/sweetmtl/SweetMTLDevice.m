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
 * interface is declared in SweetMTLDriver.h because it is not in the SDK's
 * headers (iPhoneOS27.0's Metal.framework does export the class: it is in
 * Metal.framework/Metal.tbd's objc-classes).
 *
 * Where a method's work happens: every one of them drives tools/sweetmtl/bridge,
 * the Rust C ABI over metal2vulkan's runtime, which translates Apple AIR to
 * SPIR-V and runs the Metal object model on the Vulkan device the bridge opened
 * through Mesa's ICD - Turnip (libvulkan_freedreno.dylib) on the phone, Lavapipe
 * (libvulkan_lvp.dylib) in the QEMU VM or on a test machine. The classes for the
 * objects this device hands out live in SweetMTLObjects.m; this file is the
 * device itself, the logging, and the rule for the rest of Metal's protocols
 * (sweetmtl_stub_selector). What is implemented and what is not: README.md.
 */

#import "SweetMTLDriver.h"

#import <objc/runtime.h>

#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

void
sweetmtl_log(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	fputs("sweetmtl: ", stderr);
	vfprintf(stderr, format, args);
	fputc('\n', stderr);
	va_end(args);
}

/* The one error domain this driver reports under: the bridge's own message. None of Metal's error
 * codes is claimed (the bridge does not know them), so a caller can only read the text. */
static NSString *const SweetMTLErrorDomain = @"SweetMTLErrorDomain";

void
sweetmtl_set_message(NSError **error, NSString *what, NSString *message)
{
	if (error == NULL) {
		return;
	}
	*error = [NSError errorWithDomain:SweetMTLErrorDomain
	                             code:1
	                         userInfo:@{NSLocalizedDescriptionKey : message,
	                                    @"sweetmtl.failedCall" : what}];
}

void
sweetmtl_log_failure(NSString *what)
{
	sweetmtl_log("%s: %s", what.UTF8String, sweetmtl_last_error());
}

void
sweetmtl_set_error(NSError **error, NSString *what)
{
	/* The message is this thread's; it is valid until the next failing bridge call, so it is
	 * copied out here, before anything else can call the bridge. */
	NSString *message = @(sweetmtl_last_error());
	sweetmtl_log("%s: %s", what.UTF8String, message.UTF8String);
	sweetmtl_set_message(error, what, message);
}

/* The stub every unimplemented selector is sent to: it clears the return registers (nil, 0, NO,
 * a zeroed two-word struct) and returns. The call site was compiled with the protocol's own
 * signature, so this is what the caller reads back; a wider struct return (through the pointer in
 * x8) and a float return are left as they were - see sweetmtl_stub_selector in the header. */
#if defined(__arm64__)
__attribute__((naked)) static void
SweetMTLStubIMP(void)
{
	__asm__ volatile("mov x0, #0\n\t"
	                 "mov x1, #0\n\t"
	                 "mov x2, #0\n\t"
	                 "mov x3, #0\n\t"
	                 "mov x4, #0\n\t"
	                 "mov x5, #0\n\t"
	                 "mov x6, #0\n\t"
	                 "mov x7, #0\n\t"
	                 "ret\n\t");
}
#elif defined(__x86_64__)
__attribute__((naked)) static void
SweetMTLStubIMP(void)
{
	__asm__ volatile("xorl %eax, %eax\n\t"
	                 "xorl %edx, %edx\n\t"
	                 "xorps %xmm0, %xmm0\n\t"
	                 "xorps %xmm1, %xmm1\n\t"
	                 "retq\n\t");
}
#else
#error "SweetMTLStubIMP: no known return registers to clear for this architecture"
#endif

/* The selectors noted so far, as "-[SweetMTLBuffer setPurgeableState:]". */
static NSMutableSet<NSString *> *
SweetMTLNotedSelectors(void)
{
	static NSMutableSet *noted;
	static dispatch_once_t once;
	dispatch_once(&once, ^{
		noted = [NSMutableSet set];
	});
	return noted;
}

void
sweetmtl_note_unimplemented(Class cls, SEL sel, BOOL class_method)
{
	NSString *name = [NSString stringWithFormat:@"%c[%s %s]", class_method ? '+' : '-',
	                                            class_getName(cls), sel_getName(sel)];
	BOOL first = NO;
	@synchronized(SweetMTLNotedSelectors()) {
		NSMutableSet *noted = SweetMTLNotedSelectors();
		if (![noted containsObject:name]) {
			[noted addObject:name];
			first = YES;
		}
	}
	if (first) {
		sweetmtl_log("unimplemented %s", name.UTF8String);
	}
}

BOOL
sweetmtl_stub_selector(Class cls, SEL sel, BOOL class_method)
{
	sweetmtl_note_unimplemented(cls, sel, class_method);
	/* A class method's implementation lives on the metaclass, not on the class. */
	Class target = class_method ? object_getClass(cls) : cls;
	class_addMethod(target, sel, (IMP)SweetMTLStubIMP, "v@:");
	/* YES even where `class_addMethod` refused (only possible if the class grew the method between
	 * the runtime's lookup and here): the message is then sent to that implementation instead. */
	return YES;
}

@implementation SweetMTLObject

/* A selector this class does not implement: log it once and answer through the stub, rather than
 * let the runtime trap in -doesNotRecognizeSelector:. */
+ (BOOL)resolveInstanceMethod:(SEL)sel
{
	if ([super resolveInstanceMethod:sel]) {
		return YES;
	}
	return sweetmtl_stub_selector(self, sel, NO);
}

+ (BOOL)resolveClassMethod:(SEL)sel
{
	if ([super resolveClassMethod:sel]) {
		return YES;
	}
	return sweetmtl_stub_selector(self, sel, YES);
}

@end

/* The ICD to hand the bridge. On the phone the staged Turnip; Lavapipe (CPU Vulkan) for the QEMU
 * VM, where there is no GPU to drive. SWEETMTL_ICD overrides both. */
static const char *
SweetMTLPickICD(const char **origin)
{
	static const char *const staged[] = {
	    "/usr/lib/sweetmtl/libvulkan_freedreno.dylib",
	    "/usr/lib/sweetmtl/libvulkan_lvp.dylib",
	};
	const char *chosen = getenv("SWEETMTL_ICD");
	if (chosen != NULL && chosen[0] != '\0') {
		*origin = "SWEETMTL_ICD";
		return chosen;
	}
	for (size_t i = 0; i < sizeof staged / sizeof staged[0]; i++) {
		if (access(staged[i], R_OK) == 0) {
			*origin = "staged next to the driver";
			return staged[i];
		}
	}
	*origin = NULL;
	return NULL;
}

/* The bridge makes a shared-storage buffer and nothing else (it has no way to copy into device
 * memory yet): a caller asking for another storage mode gets one anyway, after one line. */
static void
SweetMTLNoteStorageMode(MTLResourceOptions options)
{
	static BOOL noted;
	if ((options & MTLResourceStorageModeMask) == MTLResourceStorageModeShared || noted) {
		return;
	}
	noted = YES;
	sweetmtl_log("newBuffer...:options: storage mode 0x%lx asked for: the bridge makes shared-"
	             "storage buffers only",
	    (unsigned long)(options & MTLResourceStorageModeMask));
}

@implementation SweetMTLDevice {
	/* Prefixed because this class extends Metal's private MTLIOAccelDevice: the names of its own
	 * ivars are Apple's to take. */
	SweetMTLDeviceHandle *_sweetmtlBridge;
	NSString *_sweetmtlName;
}

+ (BOOL)resolveInstanceMethod:(SEL)sel
{
	/* MTLIOAccelDevice's own implementation first (NSObject's says no), then the stub - the
	 * MTLDevice protocol is wide and the bridge answers a corner of it. */
	if ([super resolveInstanceMethod:sel]) {
		return YES;
	}
	return sweetmtl_stub_selector(self, sel, NO);
}

+ (BOOL)resolveClassMethod:(SEL)sel
{
	if ([super resolveClassMethod:sel]) {
		return YES;
	}
	return sweetmtl_stub_selector(self, sel, YES);
}

- (instancetype)initWithAcceleratorPort:(mach_port_t)port
{
	sweetmtl_log("initWithAcceleratorPort:%u pid %d (%s)", port, getpid(), getprogname());
#if SWEETMTL_HOST
	/* build.sh --host, for the local self-check (README.md, "Where it was tested"):
	 * Metal's MTLIOAccelDevice answers -initWithAcceleratorPort: for a real accelerator port
	 * only, and a test machine has none, so the host build takes the plain -init and connects
	 * the bridge all the same. */
	self = [super init];
#else
	self = [super initWithAcceleratorPort:port];
#endif
	if (self == nil) {
		sweetmtl_log("no Metal device: Metal's MTLIOAccelDevice refused the port");
		return nil;
	}

	const char *origin = NULL;
	const char *icd = SweetMTLPickICD(&origin);
	if (icd != NULL) {
		sweetmtl_log("ICD %s (%s)", icd, origin);
	} else {
		sweetmtl_log("no ICD: SWEETMTL_ICD is unset and neither staged driver is readable");
	}

	/* The bridge opens the ICD and makes the VkInstance and VkDevice through it (iOS has no
	 * Vulkan loader); SWEETMTL_CACHE names the translated-shader cache, NULL the platform
	 * default. A failed bridge means no accelerated device at all, which is what returning nil
	 * tells Metal - the same answer as an accelerator with no plugin. */
	_sweetmtlBridge = sweetmtl_device_new(icd, getenv("SWEETMTL_CACHE"));
	if (_sweetmtlBridge == NULL) {
		sweetmtl_log("no Metal device: %s", sweetmtl_last_error());
		return nil;
	}
	const char *name = sweetmtl_device_name(_sweetmtlBridge);
	_sweetmtlName = name != NULL ? @(name) : @"SweetMTL";
	sweetmtl_log("device: %s", _sweetmtlName.UTF8String);
	return self;
}

- (void)dealloc
{
	/* Every object made from the bridge device holds this object, so none outlives it. */
	if (_sweetmtlBridge != NULL) {
		sweetmtl_device_release(_sweetmtlBridge);
	}
}

#pragma mark - MTLDevice

- (NSString *)name
{
	return _sweetmtlName;
}

- (nullable id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options
{
	SweetMTLNoteStorageMode(options);
	SweetMTLBufferHandle *handle = sweetmtl_device_new_buffer(_sweetmtlBridge, (uint64_t)length);
	if (handle == NULL) {
		sweetmtl_log_failure(@"newBufferWithLength:options:");
		return nil;
	}
	return [[SweetMTLBuffer alloc] initWithDevice:self handle:handle];
}

- (nullable id<MTLBuffer>)newBufferWithBytes:(const void *)pointer
                                      length:(NSUInteger)length
                                     options:(MTLResourceOptions)options
{
	SweetMTLNoteStorageMode(options);
	SweetMTLBufferHandle *handle = sweetmtl_device_new_buffer_with_bytes(
	    _sweetmtlBridge, pointer, (uint64_t)length);
	if (handle == NULL) {
		sweetmtl_log_failure(@"newBufferWithBytes:length:options:");
		return nil;
	}
	return [[SweetMTLBuffer alloc] initWithDevice:self handle:handle];
}

- (nullable id<MTLLibrary>)newLibraryWithData:(dispatch_data_t)data error:(NSError **)error
{
	if (data == NULL) {
		sweetmtl_set_message(error, @"newLibraryWithData:error:", @"the data is nil");
		return nil;
	}
	/* `dispatch_data_t` may be a scatter of regions; the bridge takes one run of bytes, so map it
	 * for the duration of the call (libdispatch copies only where it must). The mapping lives as
	 * long as the mapped object, hence objc_precise_lifetime: without it ARC could release it -
	 * and the mapping with it - before the bridge has copied the bytes. */
	const void *bytes = NULL;
	size_t length = 0;
	__attribute__((objc_precise_lifetime)) dispatch_data_t mapped =
	    dispatch_data_create_map(data, &bytes, &length);
	SweetMTLLibraryHandle *handle =
	    sweetmtl_device_new_library_with_data(_sweetmtlBridge, bytes, (uint64_t)length);
	if (handle == NULL) {
		sweetmtl_set_error(error, @"newLibraryWithData:error:");
		return nil;
	}
	return [[SweetMTLLibrary alloc] initWithDevice:self handle:handle];
}

- (nullable id<MTLLibrary>)newLibraryWithSource:(NSString *)source
                                        options:(nullable MTLCompileOptions *)options
                                          error:(NSError **)error
{
	/* The driver has no Metal compiler: a library comes from a metallib (newLibraryWithData:) or
	 * from AIR bitcode (the private door below). */
	sweetmtl_note_unimplemented(self.class, _cmd, NO);
	sweetmtl_set_message(error, @"newLibraryWithSource:options:error:",
	    @"the driver does not compile Metal source: build a metallib (xcrun -sdk iphoneos metal "
	    @"+ metallib) and load it with -newLibraryWithData:error:");
	return nil;
}

- (nullable id<MTLLibrary>)newLibraryWithAIR:(NSData *)air error:(NSError **)error
{
	/* The bridge's door for `llvm-as` output (a raw module or a bitcode wrapper), translated by
	 * metal2vulkan rather than read as a metallib; Metal has no selector for it. Declared by
	 * SweetMTLAirLibrary for the driver's test tool. */
	SweetMTLLibraryHandle *handle =
	    sweetmtl_device_new_library_with_air(_sweetmtlBridge, air.bytes, (uint64_t)air.length);
	if (handle == NULL) {
		sweetmtl_set_error(error, @"newLibraryWithAIR:error:");
		return nil;
	}
	return [[SweetMTLLibrary alloc] initWithDevice:self handle:handle];
}

- (nullable id<MTLComputePipelineState>)newComputePipelineStateWithFunction:(id<MTLFunction>)computeFunction
                                                                       error:(NSError **)error
{
	if (![computeFunction isKindOfClass:[SweetMTLFunction class]]) {
		/* The bridge compiles the function against the device that made it; a function of another
		 * device (or of a driver that is not this one) cannot be used here. Metal clients are
		 * told so in the usual way, not by a trap. */
		sweetmtl_set_message(error, @"newComputePipelineStateWithFunction:error:",
		    @"the function is not this device's");
		return nil;
	}
	SweetMTLComputePipelineHandle *handle = sweetmtl_device_new_compute_pipeline(
	    _sweetmtlBridge, [(SweetMTLFunction *)computeFunction handle]);
	if (handle == NULL) {
		sweetmtl_set_error(error, @"newComputePipelineStateWithFunction:error:");
		return nil;
	}
	return [[SweetMTLComputePipelineState alloc] initWithDevice:self handle:handle];
}

- (nullable id<MTLCommandQueue>)newCommandQueue
{
	SweetMTLCommandQueueHandle *handle = sweetmtl_device_new_command_queue(_sweetmtlBridge);
	if (handle == NULL) {
		sweetmtl_log_failure(@"newCommandQueue");
		return nil;
	}
	return [[SweetMTLCommandQueue alloc] initWithDevice:self handle:handle];
}

- (MTLSize)maxThreadsPerThreadgroup
{
	/* The bridge reports the *pipeline's* limit (maxTotalThreadsPerThreadgroup), not the device's,
	 * and a client that sizes a threadgroup from this must not read a stack variable. Zeroes, and
	 * the line the stub would have written. */
	sweetmtl_note_unimplemented(self.class, _cmd, NO);
	return MTLSizeMake(0, 0, 0);
}

@end
