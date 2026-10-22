/*
 * SweetMTLDriver.h - the driver's own classes: SweetMTLDevice, the class Metal.framework
 * instantiates for the SweetGPU accelerator, and the objects it hands out, each over a handle of
 * the bridge in bridge/include/sweetmtl_bridge.h.
 *
 * This is not a public header. Metal knows the class by name (the kernel's MetalPluginClassName)
 * and declares the MTLDevice protocols itself; the only reader outside the driver is its test
 * tools, test/metal_compute.m and test/metal_render.m; metal_compute needs the driver's private
 * door for AIR bitcode (SweetMTLAirLibrary).
 */
#ifndef SWEETMTL_DRIVER_H
#define SWEETMTL_DRIVER_H

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <mach/mach.h>
#include <stdlib.h>

/*
 * The bridge names its C handles after the Metal classes they stand for (its C test reads like
 * Metal that way), which are also the names of the classes below - and an Objective-C class
 * shares the C namespace: `@interface SweetMTLBuffer` cannot stand next to a C `SweetMTLBuffer`
 * ("redefinition ... as different kind of symbol"). The handles are renamed out of the way once,
 * here, for the translation units that include this header. The ABI itself is untouched: the
 * `sweetmtl_*` functions keep their names, and bridge/tests/compute.c still sees the originals.
 */
#define SweetMTLDevice          SweetMTLDeviceHandle
#define SweetMTLBuffer          SweetMTLBufferHandle
#define SweetMTLLibrary         SweetMTLLibraryHandle
#define SweetMTLFunction        SweetMTLFunctionHandle
#define SweetMTLComputePipeline SweetMTLComputePipelineHandle
#define SweetMTLCommandQueue    SweetMTLCommandQueueHandle
#define SweetMTLCommandBuffer   SweetMTLCommandBufferHandle
#define SweetMTLComputeEncoder  SweetMTLComputeEncoderHandle
#define SweetMTLTexture         SweetMTLTextureHandle
#define SweetMTLRenderPipeline  SweetMTLRenderPipelineHandle
#define SweetMTLRenderEncoder   SweetMTLRenderEncoderHandle
#define SweetMTLBlitEncoder     SweetMTLBlitEncoderHandle
#include "sweetmtl_bridge.h"
#undef SweetMTLDevice
#undef SweetMTLBuffer
#undef SweetMTLLibrary
#undef SweetMTLFunction
#undef SweetMTLComputePipeline
#undef SweetMTLCommandQueue
#undef SweetMTLCommandBuffer
#undef SweetMTLComputeEncoder
#undef SweetMTLTexture
#undef SweetMTLRenderPipeline
#undef SweetMTLRenderEncoder
#undef SweetMTLBlitEncoder

NS_ASSUME_NONNULL_BEGIN

/* Metal's private base class for an accelerator-backed device: Metal calls
 * -initWithAcceleratorPort: on the class the kernel named, with the IOAcceleratorES port. The
 * interface is declared here because it is not in the SDK's Metal headers (iOS 27's
 * Metal.framework does export the class; the macOS driver does the same as this). */
@interface MTLIOAccelDevice : NSObject
- (instancetype)initWithAcceleratorPort:(mach_port_t)port;
@end

/* One line on stderr, "sweetmtl: " first, like the kernel's own SWEET GPU lines. */
void sweetmtl_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

/* Logs `what` and the bridge's last error after it: a bridge call that failed. */
void sweetmtl_log_failure(NSString *what);

/* Logs `what` and stores the bridge's last error in a caller's `error:` out (NULL is allowed). */
void sweetmtl_set_error(NSError *_Nullable *_Nullable error, NSString *what);

/* Stores `message` in a caller's `error:` out, under the driver's own domain. */
void sweetmtl_set_message(NSError *_Nullable *_Nullable error,
                          NSString *what,
                          NSString *message);

/* Logs a selector the driver does not implement, once per class and selector, as
 * "unimplemented -[Class selector]". */
void sweetmtl_note_unimplemented(Class cls, SEL sel, BOOL class_method);

/*
 * Answers a selector the driver does not implement instead of leaving the runtime to trap on it.
 *
 * The driver claims the whole of MTLDevice's protocols but implements the corner the bridge
 * covers; every other selector is answered by one stub IMP, installed here from
 * `+resolveInstanceMethod:` the first time the runtime asks. There is no `-forwardingTargetFor-
 * Selector:` that could do this: it may only hand the message to an object that *implements* the
 * selector, and `-methodSignatureForSelector:` would have to invent a signature for one the
 * protocol does not declare either way the runtime ends in `-doesNotRecognizeSelector:`.
 *
 * The stub clears the return registers (nil, 0, NO, a zeroed two-word struct); a *wider* struct
 * return and a float return are left as the caller's stack held them, so the handful of those in
 * the protocols a client of this driver touches are implemented explicitly (they note the same
 * "unimplemented" line and answer zeros).
 *
 * Returns YES so the runtime sends the message to the stub.
 */
BOOL sweetmtl_stub_selector(Class cls, SEL sel, BOOL class_method);

/*
 * Every object the driver hands out over a bridge handle. Metal keeps a label per object for
 * debuggers; everything else the protocols ask for is either implemented by the subclass or
 * answered by the stub above.
 */
@interface SweetMTLObject : NSObject
@property (nullable, copy) NSString *label;
/* The MTLDevice protocols are wide and the bridge answers a corner of them: a selector this class
 * does not implement is answered by sweetmtl_stub_selector(). */
+ (BOOL)resolveInstanceMethod:(SEL)sel;
+ (BOOL)resolveClassMethod:(SEL)sel;
@end

@class SweetMTLDevice, SweetMTLCommandBuffer;

/* MTLBuffer in shared storage: the bridge's `sweetmtl_device_new_buffer[ _with_bytes]`. */
@interface SweetMTLBuffer : SweetMTLObject <MTLBuffer>
- (instancetype)initWithDevice:(SweetMTLDevice *)device handle:(SweetMTLBufferHandle *)handle;
/* The bridge handle. The driver's own classes use it to pass the buffer on; nothing else should. */
@property (nonatomic, readonly) SweetMTLBufferHandle *handle;
@end

/* MTLLibrary: over the bridge's metallib door (`newLibraryWithData:error:`) or its AIR door. */
@interface SweetMTLLibrary : SweetMTLObject <MTLLibrary>
- (instancetype)initWithDevice:(SweetMTLDevice *)device handle:(SweetMTLLibraryHandle *)handle;
@property (nonatomic, readonly) SweetMTLLibraryHandle *handle;
@end

/* MTLFunction: a named function of a library (`newFunctionWithName:`). */
@interface SweetMTLFunction : SweetMTLObject <MTLFunction>
- (instancetype)initWithDevice:(SweetMTLDevice *)device
                        handle:(SweetMTLFunctionHandle *)handle
                          name:(NSString *)name;
@property (nonatomic, readonly) SweetMTLFunctionHandle *handle;
@end

/* MTLComputePipelineState: the compiled `scale_and_offset`-shaped kernel, and its limits. */
@interface SweetMTLComputePipelineState : SweetMTLObject <MTLComputePipelineState>
- (instancetype)initWithDevice:(SweetMTLDevice *)device
                        handle:(SweetMTLComputePipelineHandle *)handle;
@property (nonatomic, readonly) SweetMTLComputePipelineHandle *handle;
@end

/* MTLTexture: a 2D texture of one level (`newTextureWithDescriptor:`), in the storage mode and
 * usage its descriptor asked for. */
@interface SweetMTLTexture : SweetMTLObject <MTLTexture>
- (instancetype)initWithDevice:(SweetMTLDevice *)device
                        handle:(SweetMTLTextureHandle *)handle
                         usage:(MTLTextureUsage)usage
                   storageMode:(MTLStorageMode)storageMode;
@property (nonatomic, readonly) SweetMTLTextureHandle *handle;
@end

/* MTLRenderPipelineState: a vertex function, an optional fragment function, one color attachment,
 * no blending. */
@interface SweetMTLRenderPipelineState : SweetMTLObject <MTLRenderPipelineState>
- (instancetype)initWithDevice:(SweetMTLDevice *)device
                        handle:(SweetMTLRenderPipelineHandle *)handle
                         label:(nullable NSString *)label;
@property (nonatomic, readonly) SweetMTLRenderPipelineHandle *handle;
@end

/* MTLCommandQueue, and the command buffers it makes. */
@interface SweetMTLCommandQueue : SweetMTLObject <MTLCommandQueue>
- (instancetype)initWithDevice:(SweetMTLDevice *)device
                        handle:(SweetMTLCommandQueueHandle *)handle;
@property (nonatomic, readonly) SweetMTLCommandQueueHandle *handle;
@end

/* MTLCommandBuffer: `computeCommandEncoder`, `renderCommandEncoderWithDescriptor:`,
 * `blitCommandEncoder`, `commit`, `waitUntilCompleted`, `status`. */
@interface SweetMTLCommandBuffer : SweetMTLObject <MTLCommandBuffer>
- (instancetype)initWithDevice:(SweetMTLDevice *)device
                        handle:(SweetMTLCommandBufferHandle *)handle;
@property (nonatomic, readonly) SweetMTLCommandBufferHandle *handle;
/* Records a failed call against the command buffer: Metal's error channel is
 * `-[MTLCommandBuffer error]`, so a call that fails (in the encoder, or the commit itself) leaves
 * `message` there. */
- (void)recordFailure:(NSString *)what message:(NSString *)message;
@end

/* MTLComputeCommandEncoder: one at a time per command buffer, ended before the commit. */
@interface SweetMTLComputeCommandEncoder : SweetMTLObject <MTLComputeCommandEncoder>
- (instancetype)initWithCommandBuffer:(SweetMTLCommandBuffer *)command_buffer
                               handle:(SweetMTLComputeEncoderHandle *)handle;
@property (nonatomic, readonly) SweetMTLComputeEncoderHandle *handle;
@end

/* MTLRenderCommandEncoder of a pass with one color attachment: pipeline, vertex and fragment
 * buffers and bytes, viewport, drawPrimitives, end. */
@interface SweetMTLRenderCommandEncoder : SweetMTLObject <MTLRenderCommandEncoder>
- (instancetype)initWithCommandBuffer:(SweetMTLCommandBuffer *)command_buffer
                               handle:(SweetMTLRenderEncoderHandle *)handle;
@property (nonatomic, readonly) SweetMTLRenderEncoderHandle *handle;
@end

/* MTLBlitCommandEncoder: the copy of a texture into a buffer, which is how a client reads back a
 * private render target. */
@interface SweetMTLBlitCommandEncoder : SweetMTLObject <MTLBlitCommandEncoder>
- (instancetype)initWithCommandBuffer:(SweetMTLCommandBuffer *)command_buffer
                               handle:(SweetMTLBlitEncoderHandle *)handle;
@property (nonatomic, readonly) SweetMTLBlitEncoderHandle *handle;
@end

/*
 * The class Metal instantiates (`MetalPluginClassName=SweetMTLDevice`), and the driver's own door
 * for AIR bitcode: Metal has no selector for it (`newLibraryWithData:` takes a metallib), and the
 * test tool needs one to run the translator's fixture through the driver.
 */
@protocol SweetMTLAirLibrary <NSObject>
- (nullable id<MTLLibrary>)newLibraryWithAIR:(NSData *)air
                                       error:(NSError *_Nullable *_Nullable)error;
@end

@interface SweetMTLDevice : MTLIOAccelDevice <MTLDevice, SweetMTLAirLibrary>
@end

NS_ASSUME_NONNULL_END

#endif /* SWEETMTL_DRIVER_H */
