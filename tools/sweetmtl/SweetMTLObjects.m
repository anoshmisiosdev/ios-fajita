/*
 * SweetMTLObjects.m - the objects SweetMTLDevice hands out, each a thin Objective-C face over one
 * hand of the bridge in bridge/include/sweetmtl_bridge.h.
 *
 * The shape is the same for all of them: hold the device that made the object (Metal's objects
 * keep their device alive, and so does the bridge device behind this one), own the bridge handle,
 * release it in -dealloc, and pass on the selectors the bridge answers. Every other selector of
 * the protocol is answered by the stub in SweetMTLDevice.m (sweetmtl_stub_selector).
 *
 * What each class is for:
 *
 *   SweetMTLBuffer                 shared-storage memory the CPU writes through -contents
 *   SweetMTLLibrary                a metallib or AIR bitcode, translated once
 *   SweetMTLFunction               a named function of a library
 *   SweetMTLComputePipelineState   the compiled kernel and its limits
 *   SweetMTLCommandQueue           makes command buffers
 *   SweetMTLCommandBuffer          encoders, commit, wait, status, error
 *   SweetMTLComputeCommandEncoder  one at a time: pipeline, buffers, dispatch, end
 */

#import "SweetMTLDriver.h"

@implementation SweetMTLBuffer {
	SweetMTLDevice *_device;
	SweetMTLBufferHandle *_handle;
}

- (instancetype)initWithDevice:(SweetMTLDevice *)device handle:(SweetMTLBufferHandle *)handle
{
	self = [super init];
	if (self == nil) {
		return nil;
	}
	_device = device;
	_handle = handle;
	return self;
}

- (void)dealloc
{
	/* The bridge's own rule: every object goes before the device it came from, and the device is
	 * held here, so it does. */
	if (_handle != NULL) {
		sweetmtl_buffer_release(_handle);
	}
}

- (id<MTLDevice>)device
{
	return _device;
}

- (void *)contents
{
	return sweetmtl_buffer_contents(_handle);
}

- (NSUInteger)length
{
	return (NSUInteger)sweetmtl_buffer_length(_handle);
}

@end

@implementation SweetMTLLibrary {
	SweetMTLDevice *_device;
	SweetMTLLibraryHandle *_handle;
}

- (instancetype)initWithDevice:(SweetMTLDevice *)device handle:(SweetMTLLibraryHandle *)handle
{
	self = [super init];
	if (self == nil) {
		return nil;
	}
	_device = device;
	_handle = handle;
	return self;
}

- (void)dealloc
{
	if (_handle != NULL) {
		sweetmtl_library_release(_handle);
	}
}

- (id<MTLDevice>)device
{
	return _device;
}

- (nullable id<MTLFunction>)newFunctionWithName:(NSString *)functionName
{
	SweetMTLFunctionHandle *handle =
	    sweetmtl_library_new_function(_handle, functionName.UTF8String);
	if (handle == NULL) {
		sweetmtl_log_failure(@"newFunctionWithName:");
		return nil;
	}
	return [[SweetMTLFunction alloc] initWithDevice:_device handle:handle name:functionName];
}

@end

@implementation SweetMTLFunction {
	SweetMTLDevice *_device;
	SweetMTLFunctionHandle *_handle;
	NSString *_name;
}

- (instancetype)initWithDevice:(SweetMTLDevice *)device
                        handle:(SweetMTLFunctionHandle *)handle
                          name:(NSString *)name
{
	self = [super init];
	if (self == nil) {
		return nil;
	}
	_device = device;
	_handle = handle;
	_name = [name copy];
	return self;
}

- (void)dealloc
{
	if (_handle != NULL) {
		sweetmtl_function_release(_handle);
	}
}

- (id<MTLDevice>)device
{
	return _device;
}

- (NSString *)name
{
	return _name;
}

- (MTLFunctionType)functionType
{
	/* The bridge makes compute pipelines only, so every function it hands out is a kernel: there
	 * is no render path that could ask for a vertex or fragment one. */
	return MTLFunctionTypeKernel;
}

@end

@implementation SweetMTLComputePipelineState {
	SweetMTLDevice *_device;
	SweetMTLComputePipelineHandle *_handle;
}

- (instancetype)initWithDevice:(SweetMTLDevice *)device
                        handle:(SweetMTLComputePipelineHandle *)handle
{
	self = [super init];
	if (self == nil) {
		return nil;
	}
	_device = device;
	_handle = handle;
	return self;
}

- (void)dealloc
{
	if (_handle != NULL) {
		sweetmtl_compute_pipeline_release(_handle);
	}
}

- (id<MTLDevice>)device
{
	return _device;
}

- (NSUInteger)maxTotalThreadsPerThreadgroup
{
	return (NSUInteger)sweetmtl_compute_pipeline_max_total_threads_per_threadgroup(_handle);
}

- (NSUInteger)threadExecutionWidth
{
	return (NSUInteger)sweetmtl_compute_pipeline_thread_execution_width(_handle);
}

- (MTLSize)requiredThreadsPerThreadgroup
{
	/* The shape a kernel demands of its threadgroup (`[[max_total_threads_per_threadgroup]]`); the
	 * bridge does not report it, and a client must not read a stack variable instead. Zeroes, and
	 * the line the stub would have written. */
	sweetmtl_note_unimplemented(self.class, _cmd, NO);
	return MTLSizeMake(0, 0, 0);
}

@end

@implementation SweetMTLCommandQueue {
	SweetMTLDevice *_device;
	SweetMTLCommandQueueHandle *_handle;
}

- (instancetype)initWithDevice:(SweetMTLDevice *)device
                        handle:(SweetMTLCommandQueueHandle *)handle
{
	self = [super init];
	if (self == nil) {
		return nil;
	}
	_device = device;
	_handle = handle;
	return self;
}

- (void)dealloc
{
	/* The bridge releases a queue after its command buffers, and a command buffer holds this
	 * object, so none is left. */
	if (_handle != NULL) {
		sweetmtl_command_queue_release(_handle);
	}
}

- (id<MTLDevice>)device
{
	return _device;
}

- (nullable id<MTLCommandBuffer>)commandBuffer
{
	SweetMTLCommandBufferHandle *handle =
	    sweetmtl_command_queue_new_command_buffer(_handle);
	if (handle == NULL) {
		sweetmtl_log_failure(@"commandBuffer");
		return nil;
	}
	return [[SweetMTLCommandBuffer alloc] initWithDevice:_device handle:handle];
}

@end

@implementation SweetMTLCommandBuffer {
	SweetMTLDevice *_device;
	SweetMTLCommandBufferHandle *_handle;
	NSError *_error;
}

- (instancetype)initWithDevice:(SweetMTLDevice *)device
                        handle:(SweetMTLCommandBufferHandle *)handle
{
	self = [super init];
	if (self == nil) {
		return nil;
	}
	_device = device;
	_handle = handle;
	return self;
}

- (void)dealloc
{
	if (_handle != NULL) {
		sweetmtl_command_buffer_release(_handle);
	}
}

- (id<MTLDevice>)device
{
	return _device;
}

- (void)recordFailure:(NSString *)what message:(NSString *)message
{
	sweetmtl_log("%s: %s", what.UTF8String, message.UTF8String);
	if (_error != nil) {
		/* Metal reports the first failure; what follows is usually its consequence. */
		return;
	}
	NSError *error = nil;
	sweetmtl_set_message(&error, what, message);
	_error = error;
}

- (nullable id<MTLComputeCommandEncoder>)computeCommandEncoder
{
	SweetMTLComputeEncoderHandle *handle =
	    sweetmtl_command_buffer_new_compute_encoder(_handle);
	if (handle == NULL) {
		/* The bridge refuses a second encoder while one is open, and says so. */
		[self recordFailure:@"computeCommandEncoder" message:@(sweetmtl_last_error())];
		return nil;
	}
	return [[SweetMTLComputeCommandEncoder alloc] initWithCommandBuffer:self handle:handle];
}

- (void)commit
{
	if (sweetmtl_command_buffer_commit(_handle) != 0) {
		[self recordFailure:@"commit" message:@(sweetmtl_last_error())];
	}
}

- (void)waitUntilCompleted
{
	/* 0 waits indefinitely, as Metal's own -waitUntilCompleted does; a command buffer that failed
	 * on the GPU comes back as a failure here. */
	if (sweetmtl_command_buffer_wait_until_completed(_handle, 0) != 0) {
		[self recordFailure:@"waitUntilCompleted" message:@(sweetmtl_last_error())];
	}
}

- (MTLCommandBufferStatus)status
{
	/* The bridge reports Metal's own raw values (0 NotEnqueued ... 4 Completed, 5 Error). */
	return (MTLCommandBufferStatus)sweetmtl_command_buffer_status(_handle);
}

- (nullable NSError *)error
{
	return _error;
}

- (CFTimeInterval)kernelStartTime
{
	return [self unimplementedGPUTimeForSelector:_cmd];
}

- (CFTimeInterval)kernelEndTime
{
	return [self unimplementedGPUTimeForSelector:_cmd];
}

- (CFTimeInterval)GPUStartTime
{
	return [self unimplementedGPUTimeForSelector:_cmd];
}

- (CFTimeInterval)GPUEndTime
{
	return [self unimplementedGPUTimeForSelector:_cmd];
}

- (CFTimeInterval)unimplementedGPUTimeForSelector:(SEL)selector
{
	/* The bridge does not report timings; a timing a client reads must not be a stack variable. */
	sweetmtl_note_unimplemented(self.class, selector, NO);
	return 0.0;
}

@end

@implementation SweetMTLComputeCommandEncoder {
	SweetMTLCommandBuffer *_commandBuffer;
	SweetMTLComputeEncoderHandle *_handle;
}

- (instancetype)initWithCommandBuffer:(SweetMTLCommandBuffer *)command_buffer
                               handle:(SweetMTLComputeEncoderHandle *)handle
{
	self = [super init];
	if (self == nil) {
		return nil;
	}
	_commandBuffer = command_buffer;
	_handle = handle;
	return self;
}

- (void)dealloc
{
	if (_handle == NULL) {
		return;
	}
	/* A client that never called -endEncoding: end it here, or the command buffer would be left
	 * with an encoder open and could not be committed (the runtime answers an error where it
	 * cannot end). */
	sweetmtl_log("released a compute encoder without -endEncoding: ending it");
	(void)sweetmtl_compute_encoder_end(_handle);
	sweetmtl_compute_encoder_release(_handle);
}

- (id<MTLDevice>)device
{
	return _commandBuffer.device;
}

/* A bridge call in this class failed: Metal reports a failed encoding on the command buffer. */
- (void)failedCall:(NSString *)what
{
	[_commandBuffer recordFailure:what message:@(sweetmtl_last_error())];
}

- (void)setComputePipelineState:(id<MTLComputePipelineState>)computePipelineState
{
	if (![computePipelineState isKindOfClass:[SweetMTLComputePipelineState class]]) {
		/* The bridge binds a pipeline of the encoder's own device; a foreign object is refused
		 * with a message rather than by a trap. */
		[_commandBuffer recordFailure:@"setComputePipelineState:"
		                      message:@"the pipeline state is not this device's"];
		return;
	}
	if (sweetmtl_compute_encoder_set_pipeline(
	        _handle, [(SweetMTLComputePipelineState *)computePipelineState handle]) != 0) {
		[self failedCall:@"setComputePipelineState:"];
	}
}

- (void)setBuffer:(nullable id<MTLBuffer>)buffer
           offset:(NSUInteger)offset
          atIndex:(NSUInteger)index
{
	if (buffer == nil) {
		/* Metal lets a client unbind an argument with nil; the bridge has no way to bind "no
		 * buffer" yet, so the argument keeps whatever it had. */
		[_commandBuffer recordFailure:@"setBuffer:offset:atIndex:"
		                      message:@"the bridge cannot unbind a buffer argument"];
		return;
	}
	if (![buffer isKindOfClass:[SweetMTLBuffer class]]) {
		[_commandBuffer recordFailure:@"setBuffer:offset:atIndex:"
		                      message:@"the buffer is not this device's"];
		return;
	}
	if (sweetmtl_compute_encoder_set_buffer(_handle, [(SweetMTLBuffer *)buffer handle],
	        (uint64_t)offset, (uint32_t)index) != 0) {
		[self failedCall:@"setBuffer:offset:atIndex:"];
	}
}

- (void)dispatchThreads:(MTLSize)threadsPerGrid
    threadsPerThreadgroup:(MTLSize)threadsPerThreadgroup
{
	if (sweetmtl_compute_encoder_dispatch_threads(_handle, threadsPerGrid.width,
	        threadsPerGrid.height, threadsPerGrid.depth, threadsPerThreadgroup.width,
	        threadsPerThreadgroup.height, threadsPerThreadgroup.depth) != 0) {
		[self failedCall:@"dispatchThreads:threadsPerThreadgroup:"];
	}
}

- (void)dispatchThreadgroups:(MTLSize)threadgroupsPerGrid
      threadsPerThreadgroup:(MTLSize)threadsPerThreadgroup
{
	/* The bridge dispatches a flat grid: the thread count is threads-per-group times groups, each
	 * threadgroup that size - Metal's own arithmetic for the call. */
	if (sweetmtl_compute_encoder_dispatch_threads(_handle,
	        threadgroupsPerGrid.width * threadsPerThreadgroup.width,
	        threadgroupsPerGrid.height * threadsPerThreadgroup.height,
	        threadgroupsPerGrid.depth * threadsPerThreadgroup.depth,
	        threadsPerThreadgroup.width, threadsPerThreadgroup.height,
	        threadsPerThreadgroup.depth) != 0) {
		[self failedCall:@"dispatchThreadgroups:threadsPerThreadgroup:"];
	}
}

- (void)endEncoding
{
	if (_handle == NULL) {
		/* Ending twice, or after a failure: nothing left to end. */
		return;
	}
	if (sweetmtl_compute_encoder_end(_handle) != 0) {
		[self failedCall:@"endEncoding"];
	}
	/* end consumes the encoder, ended or refused; the handle is this side's to free. */
	sweetmtl_compute_encoder_release(_handle);
	_handle = NULL;
}

@end
