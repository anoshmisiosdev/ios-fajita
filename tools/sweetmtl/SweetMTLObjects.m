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
 *   SweetMTLTexture                a 2D texture of one level: a render target, read back
 *   SweetMTLRenderPipelineState    a compiled vertex + fragment pair for one color attachment
 *   SweetMTLCommandQueue           makes command buffers
 *   SweetMTLCommandBuffer          encoders, commit, wait, status, error
 *   SweetMTLComputeCommandEncoder  one at a time: pipeline, buffers, dispatch, end
 *   SweetMTLRenderCommandEncoder   one pass, one color attachment: pipeline, buffers, draw, end
 *   SweetMTLBlitCommandEncoder     texture-to-buffer copies
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
	/* The bridge reads it from the translated library, as Metal's raw value; 0 is its failure,
	 * which no MTLFunctionType is, so a kernel is the answer that keeps a client's switch sane. */
	uint32_t type = sweetmtl_function_type(_handle);
	if (type == 0) {
		sweetmtl_log_failure(@"functionType");
		return MTLFunctionTypeKernel;
	}
	return (MTLFunctionType)type;
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

@implementation SweetMTLTexture {
	SweetMTLDevice *_device;
	SweetMTLTextureHandle *_handle;
	MTLTextureUsage _usage;
	MTLStorageMode _storageMode;
}

- (instancetype)initWithDevice:(SweetMTLDevice *)device
                        handle:(SweetMTLTextureHandle *)handle
                         usage:(MTLTextureUsage)usage
                   storageMode:(MTLStorageMode)storageMode
{
	self = [super init];
	if (self == nil) {
		return nil;
	}
	_device = device;
	_handle = handle;
	_usage = usage;
	_storageMode = storageMode;
	return self;
}

- (void)dealloc
{
	if (_handle != NULL) {
		sweetmtl_texture_release(_handle);
	}
}

- (id<MTLDevice>)device
{
	return _device;
}

- (MTLTextureType)textureType
{
	return MTLTextureType2D;
}

- (MTLPixelFormat)pixelFormat
{
	return (MTLPixelFormat)sweetmtl_texture_pixel_format(_handle);
}

- (NSUInteger)width
{
	return (NSUInteger)sweetmtl_texture_width(_handle);
}

- (NSUInteger)height
{
	return (NSUInteger)sweetmtl_texture_height(_handle);
}

/* The bridge makes 2D textures of one level, one slice, one sample: these are the answers, not
 * stubs. */
- (NSUInteger)depth
{
	return 1;
}

- (NSUInteger)mipmapLevelCount
{
	return 1;
}

- (NSUInteger)arrayLength
{
	return 1;
}

- (NSUInteger)sampleCount
{
	return 1;
}

- (MTLTextureUsage)usage
{
	return _usage;
}

- (MTLStorageMode)storageMode
{
	return _storageMode;
}

- (void)getBytes:(void *)pixelBytes
     bytesPerRow:(NSUInteger)bytesPerRow
      fromRegion:(MTLRegion)region
     mipmapLevel:(NSUInteger)level
{
	/* Metal's own rule: a private texture's texels are the GPU's; the bridge refuses the read, and
	 * the caller's bytes are left as they were. The destination is bytesPerRow x height bytes,
	 * the size Metal's documentation asks a caller to provide. */
	uint64_t length = (uint64_t)bytesPerRow * region.size.height;
	if (sweetmtl_texture_get_bytes(_handle, pixelBytes, length, (uint64_t)bytesPerRow,
	        region.origin.x, region.origin.y, region.size.width, region.size.height,
	        (uint64_t)level) != 0) {
		sweetmtl_log_failure(@"getBytes:bytesPerRow:fromRegion:mipmapLevel:");
	}
}

@end

@implementation SweetMTLRenderPipelineState {
	SweetMTLDevice *_device;
	SweetMTLRenderPipelineHandle *_handle;
	NSString *_pipelineLabel;
}

- (instancetype)initWithDevice:(SweetMTLDevice *)device
                        handle:(SweetMTLRenderPipelineHandle *)handle
                         label:(nullable NSString *)label
{
	self = [super init];
	if (self == nil) {
		return nil;
	}
	_device = device;
	_handle = handle;
	_pipelineLabel = [label copy];
	return self;
}

- (void)dealloc
{
	if (_handle != NULL) {
		sweetmtl_render_pipeline_release(_handle);
	}
}

- (id<MTLDevice>)device
{
	return _device;
}

- (nullable NSString *)label
{
	/* A pipeline state's label is its descriptor's, read-only, as Metal's is. */
	return _pipelineLabel;
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

- (nullable id<MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:
    (MTLRenderPassDescriptor *)renderPassDescriptor
{
	/* The bridge takes a pass of one color attachment at level 0, slice 0: anything else in the
	 * descriptor would be dropped without a word, so it is refused with one instead. */
	NSString *const what = @"renderCommandEncoderWithDescriptor:";
	MTLRenderPassColorAttachmentDescriptor *color = renderPassDescriptor.colorAttachments[0];
	if (![color.texture isKindOfClass:[SweetMTLTexture class]]) {
		[self recordFailure:what
		            message:color.texture == nil ? @"color attachment 0 has no texture"
		                                         : @"color attachment 0's texture is not this device's"];
		return nil;
	}
	for (NSUInteger i = 1; i < 8; i++) {
		if (renderPassDescriptor.colorAttachments[i].texture != nil) {
			[self recordFailure:what
			            message:[NSString stringWithFormat:@"color attachment %lu: the bridge draws "
			                                               @"to color attachment 0 only",
			                                               (unsigned long)i]];
			return nil;
		}
	}
	if (renderPassDescriptor.depthAttachment.texture != nil ||
	    renderPassDescriptor.stencilAttachment.texture != nil) {
		[self recordFailure:what message:@"the bridge has no depth or stencil attachments yet"];
		return nil;
	}
	if (color.level != 0 || color.slice != 0 || color.depthPlane != 0 ||
	    color.resolveTexture != nil) {
		[self recordFailure:what
		            message:@"the bridge draws to level 0, slice 0 of the texture, unresolved"];
		return nil;
	}
	MTLClearColor clear = color.clearColor;
	SweetMTLRenderEncoderHandle *handle = sweetmtl_command_buffer_new_render_encoder(_handle,
	    [(SweetMTLTexture *)color.texture handle], (uint32_t)color.loadAction,
	    (uint32_t)color.storeAction, clear.red, clear.green, clear.blue, clear.alpha);
	if (handle == NULL) {
		[self recordFailure:what message:@(sweetmtl_last_error())];
		return nil;
	}
	return [[SweetMTLRenderCommandEncoder alloc] initWithCommandBuffer:self handle:handle];
}

- (nullable id<MTLBlitCommandEncoder>)blitCommandEncoder
{
	SweetMTLBlitEncoderHandle *handle = sweetmtl_command_buffer_new_blit_encoder(_handle);
	if (handle == NULL) {
		[self recordFailure:@"blitCommandEncoder" message:@(sweetmtl_last_error())];
		return nil;
	}
	return [[SweetMTLBlitCommandEncoder alloc] initWithCommandBuffer:self handle:handle];
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

@implementation SweetMTLRenderCommandEncoder {
	SweetMTLCommandBuffer *_commandBuffer;
	SweetMTLRenderEncoderHandle *_handle;
}

- (instancetype)initWithCommandBuffer:(SweetMTLCommandBuffer *)command_buffer
                               handle:(SweetMTLRenderEncoderHandle *)handle
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
	/* As the compute encoder: an encoder left open would keep the command buffer from committing. */
	sweetmtl_log("released a render encoder without -endEncoding: ending it");
	(void)sweetmtl_render_encoder_end(_handle);
	sweetmtl_render_encoder_release(_handle);
}

- (id<MTLDevice>)device
{
	return _commandBuffer.device;
}

- (void)failedCall:(NSString *)what
{
	[_commandBuffer recordFailure:what message:@(sweetmtl_last_error())];
}

- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)pipelineState
{
	if (![pipelineState isKindOfClass:[SweetMTLRenderPipelineState class]]) {
		[_commandBuffer recordFailure:@"setRenderPipelineState:"
		                      message:@"the pipeline state is not this device's"];
		return;
	}
	if (sweetmtl_render_encoder_set_pipeline(
	        _handle, [(SweetMTLRenderPipelineState *)pipelineState handle]) != 0) {
		[self failedCall:@"setRenderPipelineState:"];
	}
}

/* setVertexBuffer: and setFragmentBuffer: are one bridge call with the stage as its flag. */
- (void)setBuffer:(nullable id<MTLBuffer>)buffer
           offset:(NSUInteger)offset
          atIndex:(NSUInteger)index
         fragment:(BOOL)fragment
             what:(NSString *)what
{
	if (buffer == nil) {
		[_commandBuffer recordFailure:what message:@"the bridge cannot unbind a buffer argument"];
		return;
	}
	if (![buffer isKindOfClass:[SweetMTLBuffer class]]) {
		[_commandBuffer recordFailure:what message:@"the buffer is not this device's"];
		return;
	}
	if (sweetmtl_render_encoder_set_buffer(_handle, fragment ? 1 : 0,
	        [(SweetMTLBuffer *)buffer handle], (uint64_t)offset, (uint32_t)index) != 0) {
		[self failedCall:what];
	}
}

- (void)setVertexBuffer:(nullable id<MTLBuffer>)buffer
                 offset:(NSUInteger)offset
                atIndex:(NSUInteger)index
{
	[self setBuffer:buffer
	         offset:offset
	        atIndex:index
	       fragment:NO
	           what:@"setVertexBuffer:offset:atIndex:"];
}

- (void)setFragmentBuffer:(nullable id<MTLBuffer>)buffer
                   offset:(NSUInteger)offset
                  atIndex:(NSUInteger)index
{
	[self setBuffer:buffer
	         offset:offset
	        atIndex:index
	       fragment:YES
	           what:@"setFragmentBuffer:offset:atIndex:"];
}

- (void)setVertexBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
	if (sweetmtl_render_encoder_set_bytes(_handle, 0, bytes, (uint64_t)length,
	        (uint32_t)index) != 0) {
		[self failedCall:@"setVertexBytes:length:atIndex:"];
	}
}

- (void)setFragmentBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
	if (sweetmtl_render_encoder_set_bytes(_handle, 1, bytes, (uint64_t)length,
	        (uint32_t)index) != 0) {
		[self failedCall:@"setFragmentBytes:length:atIndex:"];
	}
}

- (void)setViewport:(MTLViewport)viewport
{
	if (sweetmtl_render_encoder_set_viewport(_handle, viewport.originX, viewport.originY,
	        viewport.width, viewport.height, viewport.znear, viewport.zfar) != 0) {
		[self failedCall:@"setViewport:"];
	}
}

- (void)drawPrimitives:(MTLPrimitiveType)primitiveType
           vertexStart:(NSUInteger)vertexStart
           vertexCount:(NSUInteger)vertexCount
         instanceCount:(NSUInteger)instanceCount
          baseInstance:(NSUInteger)baseInstance
{
	if (sweetmtl_render_encoder_draw_primitives(_handle, (uint32_t)primitiveType,
	        (uint32_t)vertexStart, (uint32_t)vertexCount, (uint32_t)instanceCount,
	        (uint32_t)baseInstance) != 0) {
		[self failedCall:@"drawPrimitives:vertexStart:vertexCount:..."];
	}
}

/* Metal's shorter forms: one instance, base instance 0. */
- (void)drawPrimitives:(MTLPrimitiveType)primitiveType
           vertexStart:(NSUInteger)vertexStart
           vertexCount:(NSUInteger)vertexCount
         instanceCount:(NSUInteger)instanceCount
{
	[self drawPrimitives:primitiveType
	         vertexStart:vertexStart
	         vertexCount:vertexCount
	       instanceCount:instanceCount
	        baseInstance:0];
}

- (void)drawPrimitives:(MTLPrimitiveType)primitiveType
           vertexStart:(NSUInteger)vertexStart
           vertexCount:(NSUInteger)vertexCount
{
	[self drawPrimitives:primitiveType
	         vertexStart:vertexStart
	         vertexCount:vertexCount
	       instanceCount:1
	        baseInstance:0];
}

- (void)endEncoding
{
	if (_handle == NULL) {
		return;
	}
	if (sweetmtl_render_encoder_end(_handle) != 0) {
		[self failedCall:@"endEncoding"];
	}
	sweetmtl_render_encoder_release(_handle);
	_handle = NULL;
}

@end

@implementation SweetMTLBlitCommandEncoder {
	SweetMTLCommandBuffer *_commandBuffer;
	SweetMTLBlitEncoderHandle *_handle;
}

- (instancetype)initWithCommandBuffer:(SweetMTLCommandBuffer *)command_buffer
                               handle:(SweetMTLBlitEncoderHandle *)handle
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
	sweetmtl_log("released a blit encoder without -endEncoding: ending it");
	(void)sweetmtl_blit_encoder_end(_handle);
	sweetmtl_blit_encoder_release(_handle);
}

- (id<MTLDevice>)device
{
	return _commandBuffer.device;
}

- (void)copyFromTexture:(id<MTLTexture>)sourceTexture
                 sourceSlice:(NSUInteger)sourceSlice
                 sourceLevel:(NSUInteger)sourceLevel
                sourceOrigin:(MTLOrigin)sourceOrigin
                  sourceSize:(MTLSize)sourceSize
                    toBuffer:(id<MTLBuffer>)destinationBuffer
           destinationOffset:(NSUInteger)destinationOffset
      destinationBytesPerRow:(NSUInteger)destinationBytesPerRow
    destinationBytesPerImage:(NSUInteger)destinationBytesPerImage
{
	NSString *const what = @"copyFromTexture:...toBuffer:...";
	if (![sourceTexture isKindOfClass:[SweetMTLTexture class]] ||
	    ![destinationBuffer isKindOfClass:[SweetMTLBuffer class]]) {
		[_commandBuffer recordFailure:what message:@"the texture or buffer is not this device's"];
		return;
	}
	if (sourceOrigin.z != 0 || sourceSize.depth != 1) {
		[_commandBuffer recordFailure:what message:@"the bridge copies one 2D image"];
		return;
	}
	if (sweetmtl_blit_encoder_copy_texture_to_buffer(_handle,
	        [(SweetMTLTexture *)sourceTexture handle], (uint64_t)sourceSlice,
	        (uint64_t)sourceLevel, sourceOrigin.x, sourceOrigin.y, sourceSize.width,
	        sourceSize.height, [(SweetMTLBuffer *)destinationBuffer handle],
	        (uint64_t)destinationOffset, (uint64_t)destinationBytesPerRow,
	        (uint64_t)destinationBytesPerImage) != 0) {
		[_commandBuffer recordFailure:what message:@(sweetmtl_last_error())];
	}
}

- (void)endEncoding
{
	if (_handle == NULL) {
		return;
	}
	if (sweetmtl_blit_encoder_end(_handle) != 0) {
		[_commandBuffer recordFailure:@"endEncoding" message:@(sweetmtl_last_error())];
	}
	sweetmtl_blit_encoder_release(_handle);
	_handle = NULL;
}

@end
