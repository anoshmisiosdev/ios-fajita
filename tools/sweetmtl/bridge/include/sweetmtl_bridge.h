#ifndef SWEETMTL_BRIDGE_H
#define SWEETMTL_BRIDGE_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

// The Vulkan device the bridge adopted: an instance and a logical device made through an ICD.
//
// Release it only after every object made from it: the runtime's objects are made from the
// `VkDevice` this owns.
typedef struct {
    // This bridge's own state (a boxed `DeviceState`); C never reads or writes it.
    void *_state;
} SweetMTLDevice;

// `MTLBuffer` in shared storage.
typedef struct {
    // This bridge's own state; C never reads or writes it.
    void *_state;
} SweetMTLBuffer;

// `MTLLibrary`.
typedef struct {
    // This bridge's own state; C never reads or writes it.
    void *_state;
} SweetMTLLibrary;

// `MTLFunction`.
typedef struct {
    // This bridge's own state; C never reads or writes it.
    void *_state;
} SweetMTLFunction;

// `MTLComputePipelineState`.
typedef struct {
    // This bridge's own state; C never reads or writes it.
    void *_state;
} SweetMTLComputePipeline;

// `MTLCommandQueue`.
typedef struct {
    // This bridge's own state; C never reads or writes it.
    void *_state;
} SweetMTLCommandQueue;

// `MTLCommandBuffer`.
typedef struct {
    // This bridge's own state; C never reads or writes it.
    void *_state;
} SweetMTLCommandBuffer;

// `MTLComputeCommandEncoder`. [`sweetmtl_compute_encoder_end`](crate::sweetmtl_compute_encoder_end)
// consumes the encoder; the handle is freed separately.
typedef struct {
    // This bridge's own state; C never reads or writes it.
    void *_state;
} SweetMTLComputeEncoder;

// `MTLTexture`, 2D.
typedef struct {
    // This bridge's own state; C never reads or writes it.
    void *_state;
} SweetMTLTexture;

// `MTLRenderPipelineState`: a vertex function, an optional fragment function and one color
// attachment's pixel format.
typedef struct {
    // This bridge's own state; C never reads or writes it.
    void *_state;
} SweetMTLRenderPipeline;

// `MTLRenderCommandEncoder` of a pass with one color attachment.
// [`sweetmtl_render_encoder_end`](crate::sweetmtl_render_encoder_end) consumes the encoder; the
// handle is freed separately.
typedef struct {
    // This bridge's own state; C never reads or writes it.
    void *_state;
} SweetMTLRenderEncoder;

// `MTLBlitCommandEncoder`. [`sweetmtl_blit_encoder_end`](crate::sweetmtl_blit_encoder_end)
// consumes the encoder; the handle is freed separately.
typedef struct {
    // This bridge's own state; C never reads or writes it.
    void *_state;
} SweetMTLBlitEncoder;

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

// The message of this thread's last failing bridge call, or `""` where none failed.
//
// The pointer is valid until the next failing call on the same thread (which replaces the
// message); copy it if it has to outlive that.
const char *sweetmtl_last_error(void);

// Opens the Vulkan ICD at `icd_path` and adopts its first suitable device: a Vulkan 1.2
// instance and a logical device made through the ICD itself, handed to `metal2vulkan`
// (`Device::from_raw`), which is what makes the runtime's Metal objects run on Turnip.
//
// `icd_path` is NULL to take the `SWEETMTL_VK_ICD` environment variable instead; the two are the
// same argument. On iOS it names the shipped Turnip (`…/libvulkan_freedreno.dylib`), on a Linux
// test machine the Mesa Lavapipe ICD
// (`/usr/lib/x86_64-linux-gnu/libvulkan_lvp.so`), and on a macOS test machine Homebrew's
// (`/opt/homebrew/Cellar/mesa/…/lib/libvulkan_lvp.dylib`). A bare Mesa ICD (its
// `vk_icdGetInstanceProcAddr`) and a real Vulkan loader (its `vkGetInstanceProcAddr`) both work.
//
// `cache_directory` is where `metal2vulkan` keeps its translated-shader cache, which it must be
// able to write to (a directory it cannot use fails this call). NULL takes the platform default
// (`~/Library/Caches/metal2vulkan` on Apple platforms, `$XDG_CACHE_HOME/metal2vulkan` elsewhere).
// On iOS pass somewhere writable by the process that loads the driver.
//
// The `SWEETMTL_VK_DEVICE` environment variable picks the physical device where it names one: an
// index, or a substring of its name (a machine with more than one device). Without it the first
// device with a graphics-and-compute queue family is taken.
//
// Returns NULL on failure, with [`sweetmtl_last_error`] saying why.
//
// # Safety
//
// `icd_path` and `cache_directory` are NULL or NUL-terminated C strings valid for the call.
SweetMTLDevice *sweetmtl_device_new(const char *icd_path, const char *cache_directory);

// `MTLDevice.name`: the ICD's name for the physical device the bridge adopted (for example
// `"V3D 4.2"`, `"llvmpipe (LLVM …)"`, `"Turnip Adreno (TM) 630"`).
//
// The pointer is the device's own and stays valid until the device is released.
//
// # Safety
//
// `device` is a device [`sweetmtl_device_new`] returned and not yet released.
const char *sweetmtl_device_name(SweetMTLDevice *device);

// Releases the device: the runtime's device first (its objects must already be released), then
// the Vulkan device, the instance and the ICD. Does nothing for a NULL device.
//
// # Safety
//
// `device` is a device [`sweetmtl_device_new`] returned and not yet released, and every object
// made from it has been released.
void sweetmtl_device_release(SweetMTLDevice *device);

// `newBufferWithLength:options:` in shared storage: the buffer's bytes are in CPU-visible memory,
// so [`sweetmtl_buffer_contents`] is its pointer (the bridge never makes a private buffer: it has
// no way to copy to one yet).
//
// Returns NULL on failure, with [`sweetmtl_last_error`] saying why.
//
// # Safety
//
// `device` is a live device handle.
SweetMTLBuffer *sweetmtl_device_new_buffer(SweetMTLDevice *device, uint64_t length);

// `newBufferWithBytes:length:options:` in shared storage: the bytes are copied into the buffer.
//
// Returns NULL on failure, with [`sweetmtl_last_error`] saying why.
//
// # Safety
//
// `device` is a live device handle, and `bytes` names `length` readable bytes.
SweetMTLBuffer *sweetmtl_device_new_buffer_with_bytes(SweetMTLDevice *device,
                                                      const uint8_t *bytes,
                                                      uint64_t length);

// `MTLBuffer.contents`: the CPU address of the buffer's bytes, or NULL for private storage (the
// bridge makes none yet). Reading or writing through it while the GPU writes those bytes is a
// data race, as in Metal.
//
// # Safety
//
// `buffer` is a live buffer handle, and the caller respects the race above.
void *sweetmtl_buffer_contents(SweetMTLBuffer *buffer);

// `MTLBuffer.length`: the buffer's length in bytes.
//
// # Safety
//
// `buffer` is a live buffer handle.
uint64_t sweetmtl_buffer_length(SweetMTLBuffer *buffer);

// Releases a buffer. Does nothing for NULL.
//
// # Safety
//
// `buffer` is a live buffer handle, released once, and no command buffer still uses it.
void sweetmtl_buffer_release(SweetMTLBuffer *buffer);

// `newLibraryWithData:error:`: a library from the bytes of a **metallib** (what Apple's `metal`
// tool writes, `xcrun -sdk iphoneos metal`). Bytes that are not a metallib are refused, as Metal
// refuses them; AIR bitcode goes in through [`sweetmtl_device_new_library_with_air`].
//
// Returns NULL on failure, with [`sweetmtl_last_error`] saying why.
//
// # Safety
//
// `device` is a live device handle, and `bytes` names `length` readable bytes.
SweetMTLLibrary *sweetmtl_device_new_library_with_data(SweetMTLDevice *device,
                                                       const uint8_t *bytes,
                                                       uint64_t length);

// A library from **AIR bitcode** — what `llvm-as` writes from a `.ll` (a raw module or a bitcode
// wrapper), translated by `metal2vulkan` rather than read as a metallib.
//
// Metal has no counterpart (`newLibraryWithData:` takes metallibs alone): this is the bridge's
// door for the translator's own fixtures, and what the C test drives
// (`llvm-as tests/…/runtime_examples.ll`).
//
// Returns NULL on failure, with [`sweetmtl_last_error`] saying why.
//
// # Safety
//
// `device` is a live device handle, and `bytes` names `length` readable bytes.
SweetMTLLibrary *sweetmtl_device_new_library_with_air(SweetMTLDevice *device,
                                                      const uint8_t *bytes,
                                                      uint64_t length);

// Releases a library. Does nothing for NULL.
//
// # Safety
//
// `library` is a live library handle, released once, and the functions made from it are released
// or no longer used.
void sweetmtl_library_release(SweetMTLLibrary *library);

// `newFunctionWithName:`: the function `name` of a library.
//
// Returns NULL on failure, with [`sweetmtl_last_error`] saying why.
//
// # Safety
//
// `library` is a live library handle, and `name` is a NUL-terminated C string.
SweetMTLFunction *sweetmtl_library_new_function(SweetMTLLibrary *library, const char *name);

// `MTLFunction.functionType`, as Metal's raw value: 1 vertex, 2 fragment, 3 kernel. Returns 0
// on failure (a NULL handle, a function whose type the translator cannot read), with
// [`sweetmtl_last_error`] saying why.
//
// # Safety
//
// `function` is a live function handle.
uint32_t sweetmtl_function_type(SweetMTLFunction *function);

// Releases a function. Does nothing for NULL.
//
// # Safety
//
// `function` is a live function handle, released once, and no pipeline made from it still
// compiles.
void sweetmtl_function_release(SweetMTLFunction *function);

// `newComputePipelineStateWithFunction:error:`: compiles `function` into a compute pipeline
// state, translating its AIR and compiling the SPIR-V for the adopted device.
//
// Returns NULL on failure, with [`sweetmtl_last_error`] saying why (an unsupported capability, a
// translation refusal, a compile failure).
//
// # Safety
//
// `device` is a live device handle and `function` is a live function of it.
SweetMTLComputePipeline *sweetmtl_device_new_compute_pipeline(SweetMTLDevice *device,
                                                              SweetMTLFunction *function);

// `MTLComputePipelineState.maxTotalThreadsPerThreadgroup`.
//
// # Safety
//
// `pipeline` is a live compute pipeline handle.
uint64_t sweetmtl_compute_pipeline_max_total_threads_per_threadgroup(SweetMTLComputePipeline *pipeline);

// `MTLComputePipelineState.threadExecutionWidth`.
//
// # Safety
//
// `pipeline` is a live compute pipeline handle.
uint64_t sweetmtl_compute_pipeline_thread_execution_width(SweetMTLComputePipeline *pipeline);

// Releases a compute pipeline. Does nothing for NULL.
//
// # Safety
//
// `pipeline` is a live compute pipeline handle, released once, and no command buffer still uses
// it.
void sweetmtl_compute_pipeline_release(SweetMTLComputePipeline *pipeline);

// `newCommandQueue`: a queue the device's commands are submitted through.
//
// Returns NULL on failure, with [`sweetmtl_last_error`] saying why.
//
// # Safety
//
// `device` is a live device handle.
SweetMTLCommandQueue *sweetmtl_device_new_command_queue(SweetMTLDevice *device);

// Releases a command queue. Does nothing for NULL.
//
// # Safety
//
// `queue` is a live command queue handle, released once, and every command buffer made from it is
// released first.
void sweetmtl_command_queue_release(SweetMTLCommandQueue *queue);

// `commandBuffer`: a new command buffer from a queue.
//
// Returns NULL on failure, with [`sweetmtl_last_error`] saying why.
//
// # Safety
//
// `queue` is a live command queue handle.
SweetMTLCommandBuffer *sweetmtl_command_queue_new_command_buffer(SweetMTLCommandQueue *queue);

// Releases a command buffer. Does nothing for NULL. A committed command buffer must be waited
// for (or its work is abandoned when the buffer is released).
//
// # Safety
//
// `command_buffer` is a live command buffer handle, released once, and its encoders are ended.
void sweetmtl_command_buffer_release(SweetMTLCommandBuffer *command_buffer);

// `computeCommandEncoder`: the encoder of a command buffer's compute commands, or NULL with
// [`sweetmtl_last_error`] set (a command buffer that already has an encoder open, a lost device).
//
// The encoder must be ended with [`sweetmtl_compute_encoder_end`] before the command buffer is
// committed, and its handle freed after that.
//
// # Safety
//
// `command_buffer` is a live command buffer handle.
SweetMTLComputeEncoder *sweetmtl_command_buffer_new_compute_encoder(SweetMTLCommandBuffer *command_buffer);

// Frees an encoder handle. Does nothing for NULL. Only after
// [`sweetmtl_compute_encoder_end`] has ended the encoding.
//
// # Safety
//
// `encoder` is a live encoder handle, released once.
void sweetmtl_compute_encoder_release(SweetMTLComputeEncoder *encoder);

// `setComputePipelineState:`: the pipeline the following dispatches run.
//
// Returns 0 on success and nonzero on failure, with [`sweetmtl_last_error`] saying why.
//
// # Safety
//
// `encoder` is a live, not yet ended encoder handle, and `pipeline` is a live compute pipeline of
// the encoder's device.
int sweetmtl_compute_encoder_set_pipeline(SweetMTLComputeEncoder *encoder,
                                          SweetMTLComputePipeline *pipeline);

// `setBuffer:offset:atIndex:`: binds a buffer at a buffer argument table index.
//
// Returns 0 on success and nonzero on failure, with [`sweetmtl_last_error`] saying why.
//
// # Safety
//
// `encoder` is a live, not yet ended encoder handle, and `buffer` is a live buffer of the
// encoder's device (kept alive until the command buffer completes).
int sweetmtl_compute_encoder_set_buffer(SweetMTLComputeEncoder *encoder,
                                        SweetMTLBuffer *buffer,
                                        uint64_t offset,
                                        uint32_t index);

// `dispatchThreads:threadsPerThreadgroup:`: `threads` threads in a `groups`-sized threadgroup.
//
// Returns 0 on success and nonzero on failure, with [`sweetmtl_last_error`] saying why (a
// threadgroup larger than the pipeline's maximum).
//
// # Safety
//
// `encoder` is a live, not yet ended encoder handle with a pipeline set.
int sweetmtl_compute_encoder_dispatch_threads(SweetMTLComputeEncoder *encoder,
                                              uint64_t width,
                                              uint64_t height,
                                              uint64_t depth,
                                              uint64_t group_width,
                                              uint64_t group_height,
                                              uint64_t group_depth);

// `endEncoding`: ends the encoding, so the command buffer can be committed. The encoder handle
// then only has to be freed.
//
// Returns 0 on success and nonzero on failure, with [`sweetmtl_last_error`] saying why.
//
// # Safety
//
// `encoder` is a live encoder handle not yet ended.
int sweetmtl_compute_encoder_end(SweetMTLComputeEncoder *encoder);

// `commit`: submits the command buffer's commands to its queue.
//
// Returns 0 on success and nonzero on failure, with [`sweetmtl_last_error`] saying why.
//
// # Safety
//
// `command_buffer` is a live command buffer handle whose encoders have all ended.
int sweetmtl_command_buffer_commit(SweetMTLCommandBuffer *command_buffer);

// `waitUntilCompleted`: waits for the committed command buffer's work to complete.
//
// `timeout_ms` is 0 to wait indefinitely, or a number of milliseconds after which the call gives
// up (nonzero return with `"timed out"` in [`sweetmtl_last_error`]). A command buffer that failed
// on the GPU returns nonzero, with what the runtime knows of the failure in the error.
//
// Returns 0 on success and nonzero on failure.
//
// # Safety
//
// `command_buffer` is a live, committed command buffer handle.
int sweetmtl_command_buffer_wait_until_completed(SweetMTLCommandBuffer *command_buffer,
                                                 uint64_t timeout_ms);

// `MTLCommandBuffer.status`, as Metal's own raw values: 0 not enqueued, 1 enqueued, 2 committed,
// 3 scheduled, 4 completed, 5 error. Returns -1 for a NULL handle.
//
// # Safety
//
// `command_buffer` is a live command buffer handle.
int sweetmtl_command_buffer_status(SweetMTLCommandBuffer *command_buffer);

// The `MTLCommandBufferStatus` a value of [`sweetmtl_command_buffer_status`] stands for, for a C
// switch that would rather name them.
const char *sweetmtl_command_buffer_status_name(int status);

// `CommandBuffer::status` is also the bridge's way to tell a completed command buffer from a
// failed one: nonzero where the status is [`CommandBufferStatus::Completed`]. For the C test.
int sweetmtl_command_buffer_completed(SweetMTLCommandBuffer *command_buffer);

// `newTextureWithDescriptor:` for `texture2DDescriptorWithPixelFormat:width:height:mipmapped:NO`:
// a 2D texture of one level, `usage` and `storage_mode` Metal's raw `MTLTextureUsage` and
// `MTLStorageMode` (0 shared, 1 managed, 2 private). A render target is usually private and read
// back with a blit ([`sweetmtl_blit_encoder_copy_texture_to_buffer`]); a shared one can be read
// with [`sweetmtl_texture_get_bytes`].
//
// Returns NULL on failure, with [`sweetmtl_last_error`](crate::sweetmtl_last_error) saying why.
//
// # Safety
//
// `device` is a live device handle.
SweetMTLTexture *sweetmtl_device_new_texture_2d(SweetMTLDevice *device,
                                                uint32_t pixel_format_raw,
                                                uint64_t width,
                                                uint64_t height,
                                                uint32_t usage,
                                                uint32_t storage_mode);

// `MTLTexture.width`.
//
// # Safety
//
// `texture` is a live texture handle.
uint64_t sweetmtl_texture_width(SweetMTLTexture *texture);

// `MTLTexture.height`.
//
// # Safety
//
// `texture` is a live texture handle.
uint64_t sweetmtl_texture_height(SweetMTLTexture *texture);

// `MTLTexture.pixelFormat`, as Metal's raw value.
//
// # Safety
//
// `texture` is a live texture handle.
uint32_t sweetmtl_texture_pixel_format(SweetMTLTexture *texture);

// `getBytes:bytesPerRow:fromRegion:mipmapLevel:`: the texels of a region of level `level` into
// `bytes` (`length` of them writable), rows `bytes_per_row` apart. Refused for a private texture,
// as Metal refuses it.
//
// Returns 0 on success and nonzero on failure, with
// [`sweetmtl_last_error`](crate::sweetmtl_last_error) saying why.
//
// # Safety
//
// `texture` is a live texture handle the GPU is not writing, and `bytes` names `length` writable
// bytes.
int sweetmtl_texture_get_bytes(SweetMTLTexture *texture,
                               void *bytes,
                               uint64_t length,
                               uint64_t bytes_per_row,
                               uint64_t x,
                               uint64_t y,
                               uint64_t width,
                               uint64_t height,
                               uint64_t level);

// Releases a texture. Does nothing for NULL.
//
// # Safety
//
// `texture` is a live texture handle, released once, and no command buffer still uses it.
void sweetmtl_texture_release(SweetMTLTexture *texture);

// `newRenderPipelineStateWithDescriptor:error:` for a descriptor of a vertex function, a
// fragment function (NULL for none) and color attachment 0's pixel format (Metal's raw value;
// 0, `MTLPixelFormatInvalid`, for no color attachment). Blending is off, as a fresh
// `MTLRenderPipelineColorAttachmentDescriptor` has it.
//
// Returns NULL on failure, with [`sweetmtl_last_error`](crate::sweetmtl_last_error) saying why.
//
// # Safety
//
// `device` is a live device handle, `vertex` a live function of it, and `fragment` NULL or one.
SweetMTLRenderPipeline *sweetmtl_device_new_render_pipeline(SweetMTLDevice *device,
                                                            SweetMTLFunction *vertex,
                                                            SweetMTLFunction *fragment,
                                                            uint32_t color_pixel_format);

// Releases a render pipeline. Does nothing for NULL.
//
// # Safety
//
// `pipeline` is a live render pipeline handle, released once, and no command buffer still uses
// it.
void sweetmtl_render_pipeline_release(SweetMTLRenderPipeline *pipeline);

// `renderCommandEncoderWithDescriptor:` for a pass of one color attachment: `texture` at
// attachment 0 with `load_action` and `store_action` (Metal's raw `MTLLoadAction` /
// `MTLStoreAction`) and, for a clear, the clear color `red`, `green`, `blue`, `alpha`.
//
// Returns NULL on failure, with [`sweetmtl_last_error`](crate::sweetmtl_last_error) saying why
// (another encoder open, a texture that is not a render target).
//
// # Safety
//
// `command_buffer` is a live command buffer handle and `texture` a live texture of its device.
SweetMTLRenderEncoder *sweetmtl_command_buffer_new_render_encoder(SweetMTLCommandBuffer *command_buffer,
                                                                  SweetMTLTexture *texture,
                                                                  uint32_t load_action,
                                                                  uint32_t store_action,
                                                                  double red,
                                                                  double green,
                                                                  double blue,
                                                                  double alpha);

// `setRenderPipelineState:`.
//
// Returns 0 on success and nonzero on failure, with
// [`sweetmtl_last_error`](crate::sweetmtl_last_error) saying why.
//
// # Safety
//
// `encoder` is a live, not yet ended render encoder handle and `pipeline` a live render pipeline
// of its device.
int sweetmtl_render_encoder_set_pipeline(SweetMTLRenderEncoder *encoder,
                                         SweetMTLRenderPipeline *pipeline);

// `setVertexBuffer:offset:atIndex:` (`fragment` 0) or `setFragmentBuffer:offset:atIndex:`
// (`fragment` nonzero).
//
// Returns 0 on success and nonzero on failure, with
// [`sweetmtl_last_error`](crate::sweetmtl_last_error) saying why.
//
// # Safety
//
// `encoder` is a live, not yet ended render encoder handle, and `buffer` a live buffer of its
// device (kept alive until the command buffer completes).
int sweetmtl_render_encoder_set_buffer(SweetMTLRenderEncoder *encoder,
                                       int fragment,
                                       SweetMTLBuffer *buffer,
                                       uint64_t offset,
                                       uint32_t index);

// `setVertexBytes:length:atIndex:` (`fragment` 0) or `setFragmentBytes:length:atIndex:`
// (`fragment` nonzero): the bytes are copied at the call.
//
// Returns 0 on success and nonzero on failure, with
// [`sweetmtl_last_error`](crate::sweetmtl_last_error) saying why.
//
// # Safety
//
// `encoder` is a live, not yet ended render encoder handle, and `bytes` names `length` readable
// bytes.
int sweetmtl_render_encoder_set_bytes(SweetMTLRenderEncoder *encoder,
                                      int fragment,
                                      const uint8_t *bytes,
                                      uint64_t length,
                                      uint32_t index);

// `setViewport:`. A pass starts with the viewport of its attachment's whole size, as Metal's
// does, so a client that draws to all of it need not call this.
//
// Returns 0 on success and nonzero on failure, with
// [`sweetmtl_last_error`](crate::sweetmtl_last_error) saying why.
//
// # Safety
//
// `encoder` is a live, not yet ended render encoder handle.
int sweetmtl_render_encoder_set_viewport(SweetMTLRenderEncoder *encoder,
                                         double origin_x,
                                         double origin_y,
                                         double width,
                                         double height,
                                         double znear,
                                         double zfar);

// `drawPrimitives:vertexStart:vertexCount:instanceCount:baseInstance:`, `primitive_type` Metal's
// raw `MTLPrimitiveType` (3 triangle, 4 triangle strip).
//
// Returns 0 on success and nonzero on failure, with
// [`sweetmtl_last_error`](crate::sweetmtl_last_error) saying why.
//
// # Safety
//
// `encoder` is a live, not yet ended render encoder handle with a pipeline set.
int sweetmtl_render_encoder_draw_primitives(SweetMTLRenderEncoder *encoder,
                                            uint32_t primitive_type,
                                            uint32_t vertex_start,
                                            uint32_t vertex_count,
                                            uint32_t instance_count,
                                            uint32_t base_instance);

// `endEncoding` of a render encoder; the handle then only has to be freed.
//
// Returns 0 on success and nonzero on failure, with
// [`sweetmtl_last_error`](crate::sweetmtl_last_error) saying why.
//
// # Safety
//
// `encoder` is a live render encoder handle not yet ended.
int sweetmtl_render_encoder_end(SweetMTLRenderEncoder *encoder);

// Frees a render encoder handle. Does nothing for NULL. Only after
// [`sweetmtl_render_encoder_end`].
//
// # Safety
//
// `encoder` is a live render encoder handle, released once.
void sweetmtl_render_encoder_release(SweetMTLRenderEncoder *encoder);

// `blitCommandEncoder`: the encoder of a command buffer's copies.
//
// Returns NULL on failure, with [`sweetmtl_last_error`](crate::sweetmtl_last_error) saying why.
//
// # Safety
//
// `command_buffer` is a live command buffer handle.
SweetMTLBlitEncoder *sweetmtl_command_buffer_new_blit_encoder(SweetMTLCommandBuffer *command_buffer);

// `copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toBuffer:destinationOffset:
// destinationBytesPerRow:destinationBytesPerImage:`: a 2D region of `texture` into `buffer`,
// ordered after the command buffer's earlier passes.
//
// Returns 0 on success and nonzero on failure, with
// [`sweetmtl_last_error`](crate::sweetmtl_last_error) saying why.
//
// # Safety
//
// `encoder` is a live, not yet ended blit encoder handle, and `texture` and `buffer` live objects
// of its device.
int sweetmtl_blit_encoder_copy_texture_to_buffer(SweetMTLBlitEncoder *encoder,
                                                 SweetMTLTexture *texture,
                                                 uint64_t slice,
                                                 uint64_t level,
                                                 uint64_t x,
                                                 uint64_t y,
                                                 uint64_t width,
                                                 uint64_t height,
                                                 SweetMTLBuffer *buffer,
                                                 uint64_t offset,
                                                 uint64_t bytes_per_row,
                                                 uint64_t bytes_per_image);

// `endEncoding` of a blit encoder; the handle then only has to be freed.
//
// Returns 0 on success and nonzero on failure, with
// [`sweetmtl_last_error`](crate::sweetmtl_last_error) saying why.
//
// # Safety
//
// `encoder` is a live blit encoder handle not yet ended.
int sweetmtl_blit_encoder_end(SweetMTLBlitEncoder *encoder);

// Frees a blit encoder handle. Does nothing for NULL. Only after [`sweetmtl_blit_encoder_end`].
//
// # Safety
//
// `encoder` is a live blit encoder handle, released once.
void sweetmtl_blit_encoder_release(SweetMTLBlitEncoder *encoder);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  /* SWEETMTL_BRIDGE_H */
