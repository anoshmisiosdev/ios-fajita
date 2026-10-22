//! The C ABI's second milestone: 2D textures, render pipelines, render encoders over one color
//! attachment, and the blit that copies a texture into a buffer the CPU reads — the
//! runtime's `render_to_texture` example, function by function.
//!
//! Every value Metal passes as an enum or an option set (`MTLPixelFormat`, `MTLTextureUsage`,
//! `MTLStorageMode`, `MTLLoadAction`, `MTLStoreAction`, `MTLPrimitiveType`) crosses the boundary
//! as Metal's own raw value, so the Objective-C side passes a descriptor's properties through
//! untouched; a raw value Metal does not define is refused with a message, never cast.

use crate::ffi::{bytes_arg, guard, set_last_error};
use crate::handle::{
    SweetMTLBlitEncoder, SweetMTLBuffer, SweetMTLCommandBuffer, SweetMTLDevice, SweetMTLFunction,
    SweetMTLRenderEncoder, SweetMTLRenderPipeline, SweetMTLTexture,
};
use crate::objects::{BlitEncoderState, RenderEncoderState, RenderPipelineStateState, TextureState};
use metal2vulkan::metal::{PixelFormat, PrimitiveType, TextureUsage};
use metal2vulkan::pipeline::{ColorAttachmentDescriptor, RenderPipelineDescriptor, StageSource};
use metal2vulkan_runtime::{
    BlitOption, ClearColor, LoadAction, Origin, PipelineOptions, Region, RenderPassDescriptor,
    ResourceOptions, Size, StoreAction, TextureDescriptor, Viewport,
};
use std::ffi::{c_int, c_void};
use std::ptr;

/// `MTLPixelFormatInvalid`: a render pipeline with no color attachment.
const PIXEL_FORMAT_INVALID: u32 = 0;

fn pixel_format(raw: u32) -> Result<PixelFormat, String> {
    PixelFormat::try_from(raw).map_err(|_| format!("MTLPixelFormat {raw} is not one Metal defines"))
}

/// The resource options of `MTLStorageMode` `storage` (bits 4-7 of `MTLResourceOptions`), the
/// other modes at their defaults.
fn storage_options(storage: u32) -> Result<ResourceOptions, String> {
    ResourceOptions::from_raw(u64::from(storage) << 4)
        .map_err(|error| format!("MTLStorageMode {storage}: {error}"))
}

/// `newTextureWithDescriptor:` for `texture2DDescriptorWithPixelFormat:width:height:mipmapped:NO`:
/// a 2D texture of one level, `usage` and `storage_mode` Metal's raw `MTLTextureUsage` and
/// `MTLStorageMode` (0 shared, 1 managed, 2 private). A render target is usually private and read
/// back with a blit ([`sweetmtl_blit_encoder_copy_texture_to_buffer`]); a shared one can be read
/// with [`sweetmtl_texture_get_bytes`].
///
/// Returns NULL on failure, with [`sweetmtl_last_error`](crate::sweetmtl_last_error) saying why.
///
/// # Safety
///
/// `device` is a live device handle.
#[no_mangle]
pub unsafe extern "C" fn sweetmtl_device_new_texture_2d(
    device: *mut SweetMTLDevice,
    pixel_format_raw: u32,
    width: u64,
    height: u64,
    usage: u32,
    storage_mode: u32,
) -> *mut SweetMTLTexture {
    guard(ptr::null_mut(), || {
        let Some(state) = (unsafe { SweetMTLDevice::state(device) }) else {
            set_last_error("sweetmtl_device_new_texture_2d: device is NULL");
            return ptr::null_mut();
        };
        let made = (|| {
            let format = pixel_format(pixel_format_raw)?;
            let mut descriptor = TextureDescriptor::texture_2d(format, width, height, false);
            descriptor.usage = TextureUsage::try_from(usage)
                .map_err(|_| format!("MTLTextureUsage 0x{usage:x} is not one Metal defines"))?;
            descriptor.resource_options = storage_options(storage_mode)?;
            state.device().new_texture(&descriptor).map_err(|error| error.to_string())
        })();
        match made {
            Ok(texture) => SweetMTLTexture::make(TextureState { texture }),
            Err(message) => {
                set_last_error(format!(
                    "new_texture(2D {width}x{height}, format {pixel_format_raw}) failed: {message}"
                ));
                ptr::null_mut()
            }
        }
    })
}

/// `MTLTexture.width`.
///
/// # Safety
///
/// `texture` is a live texture handle.
#[no_mangle]
pub unsafe extern "C" fn sweetmtl_texture_width(texture: *mut SweetMTLTexture) -> u64 {
    guard(0, || match unsafe { SweetMTLTexture::state(texture) } {
        Some(state) => state.texture.width(),
        None => {
            set_last_error("sweetmtl_texture_width: texture is NULL");
            0
        }
    })
}

/// `MTLTexture.height`.
///
/// # Safety
///
/// `texture` is a live texture handle.
#[no_mangle]
pub unsafe extern "C" fn sweetmtl_texture_height(texture: *mut SweetMTLTexture) -> u64 {
    guard(0, || match unsafe { SweetMTLTexture::state(texture) } {
        Some(state) => state.texture.height(),
        None => {
            set_last_error("sweetmtl_texture_height: texture is NULL");
            0
        }
    })
}

/// `MTLTexture.pixelFormat`, as Metal's raw value.
///
/// # Safety
///
/// `texture` is a live texture handle.
#[no_mangle]
pub unsafe extern "C" fn sweetmtl_texture_pixel_format(texture: *mut SweetMTLTexture) -> u32 {
    guard(PIXEL_FORMAT_INVALID, || match unsafe { SweetMTLTexture::state(texture) } {
        Some(state) => state.texture.pixel_format().as_raw(),
        None => {
            set_last_error("sweetmtl_texture_pixel_format: texture is NULL");
            PIXEL_FORMAT_INVALID
        }
    })
}

/// `getBytes:bytesPerRow:fromRegion:mipmapLevel:`: the texels of a region of level `level` into
/// `bytes` (`length` of them writable), rows `bytes_per_row` apart. Refused for a private texture,
/// as Metal refuses it.
///
/// Returns 0 on success and nonzero on failure, with
/// [`sweetmtl_last_error`](crate::sweetmtl_last_error) saying why.
///
/// # Safety
///
/// `texture` is a live texture handle the GPU is not writing, and `bytes` names `length` writable
/// bytes.
#[no_mangle]
pub unsafe extern "C" fn sweetmtl_texture_get_bytes(
    texture: *mut SweetMTLTexture,
    bytes: *mut c_void,
    length: u64,
    bytes_per_row: u64,
    x: u64,
    y: u64,
    width: u64,
    height: u64,
    level: u64,
) -> c_int {
    guard(-1, || {
        let Some(state) = (unsafe { SweetMTLTexture::state(texture) }) else {
            set_last_error("sweetmtl_texture_get_bytes: texture is NULL");
            return -1;
        };
        if bytes.is_null() {
            set_last_error("sweetmtl_texture_get_bytes: bytes is NULL");
            return -1;
        }
        let Ok(length) = usize::try_from(length) else {
            set_last_error(format!("sweetmtl_texture_get_bytes: length {length} is too large"));
            return -1;
        };
        // SAFETY: the caller's contract.
        let bytes = unsafe { std::slice::from_raw_parts_mut(bytes.cast::<u8>(), length) };
        let region = Region::new_2d(x, y, width, height);
        match state.texture.get_bytes(bytes, bytes_per_row, 0, region, level, 0) {
            Ok(()) => 0,
            Err(error) => {
                set_last_error(format!("get_bytes({width}x{height} at {x},{y}) failed: {error}"));
                -1
            }
        }
    })
}

/// Releases a texture. Does nothing for NULL.
///
/// # Safety
///
/// `texture` is a live texture handle, released once, and no command buffer still uses it.
#[no_mangle]
pub unsafe extern "C" fn sweetmtl_texture_release(texture: *mut SweetMTLTexture) {
    guard((), || {
        // SAFETY: the caller's contract.
        unsafe { SweetMTLTexture::free(texture) }
    })
}

/// `newRenderPipelineStateWithDescriptor:error:` for a descriptor of a vertex function, a
/// fragment function (NULL for none) and color attachment 0's pixel format (Metal's raw value;
/// 0, `MTLPixelFormatInvalid`, for no color attachment). Blending is off, as a fresh
/// `MTLRenderPipelineColorAttachmentDescriptor` has it.
///
/// Returns NULL on failure, with [`sweetmtl_last_error`](crate::sweetmtl_last_error) saying why.
///
/// # Safety
///
/// `device` is a live device handle, `vertex` a live function of it, and `fragment` NULL or one.
#[no_mangle]
pub unsafe extern "C" fn sweetmtl_device_new_render_pipeline(
    device: *mut SweetMTLDevice,
    vertex: *mut SweetMTLFunction,
    fragment: *mut SweetMTLFunction,
    color_pixel_format: u32,
) -> *mut SweetMTLRenderPipeline {
    guard(ptr::null_mut(), || {
        let Some(state) = (unsafe { SweetMTLDevice::state(device) }) else {
            set_last_error("sweetmtl_device_new_render_pipeline: device is NULL");
            return ptr::null_mut();
        };
        let Some(vertex) = (unsafe { SweetMTLFunction::state(vertex) }) else {
            set_last_error("sweetmtl_device_new_render_pipeline: vertex function is NULL");
            return ptr::null_mut();
        };
        let fragment = unsafe { SweetMTLFunction::state(fragment) };
        let made = (|| {
            let mut descriptor = RenderPipelineDescriptor::new(StageSource::Function(
                vertex.function.function_descriptor(),
            ));
            descriptor.fragment =
                fragment.map(|fragment| StageSource::Function(fragment.function.function_descriptor()));
            if color_pixel_format != PIXEL_FORMAT_INVALID {
                descriptor.color_attachments[0] =
                    Some(ColorAttachmentDescriptor::new(pixel_format(color_pixel_format)?));
            }
            state
                .device()
                .new_render_pipeline_state(&descriptor, &PipelineOptions::default())
                .map_err(|error| error.to_string())
        })();
        match made {
            Ok(pipeline) => SweetMTLRenderPipeline::make(RenderPipelineStateState(pipeline)),
            Err(message) => {
                set_last_error(format!("new_render_pipeline_state failed: {message}"));
                ptr::null_mut()
            }
        }
    })
}

/// Releases a render pipeline. Does nothing for NULL.
///
/// # Safety
///
/// `pipeline` is a live render pipeline handle, released once, and no command buffer still uses
/// it.
#[no_mangle]
pub unsafe extern "C" fn sweetmtl_render_pipeline_release(pipeline: *mut SweetMTLRenderPipeline) {
    guard((), || {
        // SAFETY: the caller's contract.
        unsafe { SweetMTLRenderPipeline::free(pipeline) }
    })
}

/// `renderCommandEncoderWithDescriptor:` for a pass of one color attachment: `texture` at
/// attachment 0 with `load_action` and `store_action` (Metal's raw `MTLLoadAction` /
/// `MTLStoreAction`) and, for a clear, the clear color `red`, `green`, `blue`, `alpha`.
///
/// Returns NULL on failure, with [`sweetmtl_last_error`](crate::sweetmtl_last_error) saying why
/// (another encoder open, a texture that is not a render target).
///
/// # Safety
///
/// `command_buffer` is a live command buffer handle and `texture` a live texture of its device.
#[no_mangle]
pub unsafe extern "C" fn sweetmtl_command_buffer_new_render_encoder(
    command_buffer: *mut SweetMTLCommandBuffer,
    texture: *mut SweetMTLTexture,
    load_action: u32,
    store_action: u32,
    red: f64,
    green: f64,
    blue: f64,
    alpha: f64,
) -> *mut SweetMTLRenderEncoder {
    guard(ptr::null_mut(), || {
        let Some(state) = (unsafe { SweetMTLCommandBuffer::state(command_buffer) }) else {
            set_last_error("sweetmtl_command_buffer_new_render_encoder: command buffer is NULL");
            return ptr::null_mut();
        };
        let Some(texture) = (unsafe { SweetMTLTexture::state(texture) }) else {
            set_last_error("sweetmtl_command_buffer_new_render_encoder: texture is NULL");
            return ptr::null_mut();
        };
        let made = (|| {
            let mut pass = RenderPassDescriptor::new();
            let color = &mut pass.color_attachments[0];
            color.texture = Some(texture.texture.clone());
            color.load_action =
                LoadAction::from_raw(u64::from(load_action)).map_err(|error| error.to_string())?;
            color.store_action =
                StoreAction::from_raw(u64::from(store_action)).map_err(|error| error.to_string())?;
            color.clear_color = ClearColor::new(red, green, blue, alpha);
            state
                .command_buffer
                .render_command_encoder(&pass)
                .map_err(|error| error.to_string())
        })();
        match made {
            Ok(encoder) => SweetMTLRenderEncoder::make(RenderEncoderState {
                encoder: Some(encoder),
            }),
            Err(message) => {
                set_last_error(format!("render_command_encoder failed: {message}"));
                ptr::null_mut()
            }
        }
    })
}

/// The open encoder behind a render encoder handle, or the failure `call` reports.
///
/// # Safety
///
/// `encoder` is NULL or a live render encoder handle.
unsafe fn open_render<'a>(
    encoder: *mut SweetMTLRenderEncoder,
    call: &str,
) -> Result<&'a mut metal2vulkan_runtime::RenderCommandEncoder, ()> {
    let Some(state) = (unsafe { SweetMTLRenderEncoder::state(encoder) }) else {
        set_last_error(format!("{call}: encoder is NULL"));
        return Err(());
    };
    match state.encoder.as_mut() {
        Some(encoder) => Ok(encoder),
        None => {
            set_last_error(format!("{call}: the encoder has ended"));
            Err(())
        }
    }
}

/// 0, or -1 with the runtime's error after `what`.
fn status<E: std::fmt::Display>(result: Result<(), E>, what: impl FnOnce() -> String) -> c_int {
    match result {
        Ok(()) => 0,
        Err(error) => {
            set_last_error(format!("{} failed: {error}", what()));
            -1
        }
    }
}

/// `setRenderPipelineState:`.
///
/// Returns 0 on success and nonzero on failure, with
/// [`sweetmtl_last_error`](crate::sweetmtl_last_error) saying why.
///
/// # Safety
///
/// `encoder` is a live, not yet ended render encoder handle and `pipeline` a live render pipeline
/// of its device.
#[no_mangle]
pub unsafe extern "C" fn sweetmtl_render_encoder_set_pipeline(
    encoder: *mut SweetMTLRenderEncoder,
    pipeline: *mut SweetMTLRenderPipeline,
) -> c_int {
    guard(-1, || {
        let Ok(encoder) = (unsafe { open_render(encoder, "sweetmtl_render_encoder_set_pipeline") })
        else {
            return -1;
        };
        let Some(pipeline) = (unsafe { SweetMTLRenderPipeline::state(pipeline) }) else {
            set_last_error("sweetmtl_render_encoder_set_pipeline: pipeline is NULL");
            return -1;
        };
        status(encoder.set_render_pipeline_state(&pipeline.0), || {
            "set_render_pipeline_state".to_owned()
        })
    })
}

/// `setVertexBuffer:offset:atIndex:` (`fragment` 0) or `setFragmentBuffer:offset:atIndex:`
/// (`fragment` nonzero).
///
/// Returns 0 on success and nonzero on failure, with
/// [`sweetmtl_last_error`](crate::sweetmtl_last_error) saying why.
///
/// # Safety
///
/// `encoder` is a live, not yet ended render encoder handle, and `buffer` a live buffer of its
/// device (kept alive until the command buffer completes).
#[no_mangle]
pub unsafe extern "C" fn sweetmtl_render_encoder_set_buffer(
    encoder: *mut SweetMTLRenderEncoder,
    fragment: c_int,
    buffer: *mut SweetMTLBuffer,
    offset: u64,
    index: u32,
) -> c_int {
    guard(-1, || {
        let Ok(encoder) = (unsafe { open_render(encoder, "sweetmtl_render_encoder_set_buffer") })
        else {
            return -1;
        };
        let Some(buffer) = (unsafe { SweetMTLBuffer::state(buffer) }) else {
            set_last_error("sweetmtl_render_encoder_set_buffer: buffer is NULL");
            return -1;
        };
        let stage = if fragment != 0 { "fragment" } else { "vertex" };
        let result = if fragment != 0 {
            encoder.set_fragment_buffer(Some(&buffer.buffer), offset, index)
        } else {
            encoder.set_vertex_buffer(Some(&buffer.buffer), offset, index)
        };
        status(result, || format!("set_{stage}_buffer at index {index}"))
    })
}

/// `setVertexBytes:length:atIndex:` (`fragment` 0) or `setFragmentBytes:length:atIndex:`
/// (`fragment` nonzero): the bytes are copied at the call.
///
/// Returns 0 on success and nonzero on failure, with
/// [`sweetmtl_last_error`](crate::sweetmtl_last_error) saying why.
///
/// # Safety
///
/// `encoder` is a live, not yet ended render encoder handle, and `bytes` names `length` readable
/// bytes.
#[no_mangle]
pub unsafe extern "C" fn sweetmtl_render_encoder_set_bytes(
    encoder: *mut SweetMTLRenderEncoder,
    fragment: c_int,
    bytes: *const u8,
    length: u64,
    index: u32,
) -> c_int {
    guard(-1, || {
        let Ok(encoder) = (unsafe { open_render(encoder, "sweetmtl_render_encoder_set_bytes") })
        else {
            return -1;
        };
        let bytes = match unsafe { bytes_arg(bytes, length, "bytes") } {
            Ok(bytes) => bytes,
            Err(message) => {
                set_last_error(message);
                return -1;
            }
        };
        let stage = if fragment != 0 { "fragment" } else { "vertex" };
        let result = if fragment != 0 {
            encoder.set_fragment_bytes(bytes, index)
        } else {
            encoder.set_vertex_bytes(bytes, index)
        };
        status(result, || format!("set_{stage}_bytes({length}) at index {index}"))
    })
}

/// `setViewport:`. A pass starts with the viewport of its attachment's whole size, as Metal's
/// does, so a client that draws to all of it need not call this.
///
/// Returns 0 on success and nonzero on failure, with
/// [`sweetmtl_last_error`](crate::sweetmtl_last_error) saying why.
///
/// # Safety
///
/// `encoder` is a live, not yet ended render encoder handle.
#[no_mangle]
pub unsafe extern "C" fn sweetmtl_render_encoder_set_viewport(
    encoder: *mut SweetMTLRenderEncoder,
    origin_x: f64,
    origin_y: f64,
    width: f64,
    height: f64,
    znear: f64,
    zfar: f64,
) -> c_int {
    guard(-1, || {
        let Ok(encoder) = (unsafe { open_render(encoder, "sweetmtl_render_encoder_set_viewport") })
        else {
            return -1;
        };
        let viewport = Viewport::new(origin_x, origin_y, width, height, znear, zfar);
        status(encoder.set_viewport(viewport), || "set_viewport".to_owned())
    })
}

/// `drawPrimitives:vertexStart:vertexCount:instanceCount:baseInstance:`, `primitive_type` Metal's
/// raw `MTLPrimitiveType` (3 triangle, 4 triangle strip).
///
/// Returns 0 on success and nonzero on failure, with
/// [`sweetmtl_last_error`](crate::sweetmtl_last_error) saying why.
///
/// # Safety
///
/// `encoder` is a live, not yet ended render encoder handle with a pipeline set.
#[no_mangle]
pub unsafe extern "C" fn sweetmtl_render_encoder_draw_primitives(
    encoder: *mut SweetMTLRenderEncoder,
    primitive_type: u32,
    vertex_start: u32,
    vertex_count: u32,
    instance_count: u32,
    base_instance: u32,
) -> c_int {
    guard(-1, || {
        let Ok(encoder) =
            (unsafe { open_render(encoder, "sweetmtl_render_encoder_draw_primitives") })
        else {
            return -1;
        };
        let Ok(primitive) = PrimitiveType::try_from(primitive_type) else {
            set_last_error(format!("MTLPrimitiveType {primitive_type} is not one Metal defines"));
            return -1;
        };
        status(
            encoder.draw_primitives(
                primitive,
                vertex_start,
                vertex_count,
                instance_count,
                base_instance,
            ),
            || format!("draw_primitives({primitive_type}, {vertex_start}, {vertex_count})"),
        )
    })
}

/// `endEncoding` of a render encoder; the handle then only has to be freed.
///
/// Returns 0 on success and nonzero on failure, with
/// [`sweetmtl_last_error`](crate::sweetmtl_last_error) saying why.
///
/// # Safety
///
/// `encoder` is a live render encoder handle not yet ended.
#[no_mangle]
pub unsafe extern "C" fn sweetmtl_render_encoder_end(encoder: *mut SweetMTLRenderEncoder) -> c_int {
    guard(-1, || {
        let Some(state) = (unsafe { SweetMTLRenderEncoder::state(encoder) }) else {
            set_last_error("sweetmtl_render_encoder_end: encoder is NULL");
            return -1;
        };
        let Some(encoder) = state.encoder.take() else {
            set_last_error("sweetmtl_render_encoder_end: the encoder has already ended");
            return -1;
        };
        status(encoder.end_encoding(), || "end_encoding".to_owned())
    })
}

/// Frees a render encoder handle. Does nothing for NULL. Only after
/// [`sweetmtl_render_encoder_end`].
///
/// # Safety
///
/// `encoder` is a live render encoder handle, released once.
#[no_mangle]
pub unsafe extern "C" fn sweetmtl_render_encoder_release(encoder: *mut SweetMTLRenderEncoder) {
    guard((), || {
        // SAFETY: the caller's contract.
        unsafe { SweetMTLRenderEncoder::free(encoder) }
    })
}

/// `blitCommandEncoder`: the encoder of a command buffer's copies.
///
/// Returns NULL on failure, with [`sweetmtl_last_error`](crate::sweetmtl_last_error) saying why.
///
/// # Safety
///
/// `command_buffer` is a live command buffer handle.
#[no_mangle]
pub unsafe extern "C" fn sweetmtl_command_buffer_new_blit_encoder(
    command_buffer: *mut SweetMTLCommandBuffer,
) -> *mut SweetMTLBlitEncoder {
    guard(ptr::null_mut(), || {
        let Some(state) = (unsafe { SweetMTLCommandBuffer::state(command_buffer) }) else {
            set_last_error("sweetmtl_command_buffer_new_blit_encoder: command buffer is NULL");
            return ptr::null_mut();
        };
        match state.command_buffer.blit_command_encoder() {
            Ok(encoder) => SweetMTLBlitEncoder::make(BlitEncoderState {
                encoder: Some(encoder),
            }),
            Err(error) => {
                set_last_error(format!("blit_command_encoder failed: {error}"));
                ptr::null_mut()
            }
        }
    })
}

/// `copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toBuffer:destinationOffset:
/// destinationBytesPerRow:destinationBytesPerImage:`: a 2D region of `texture` into `buffer`,
/// ordered after the command buffer's earlier passes.
///
/// Returns 0 on success and nonzero on failure, with
/// [`sweetmtl_last_error`](crate::sweetmtl_last_error) saying why.
///
/// # Safety
///
/// `encoder` is a live, not yet ended blit encoder handle, and `texture` and `buffer` live objects
/// of its device.
#[no_mangle]
pub unsafe extern "C" fn sweetmtl_blit_encoder_copy_texture_to_buffer(
    encoder: *mut SweetMTLBlitEncoder,
    texture: *mut SweetMTLTexture,
    slice: u64,
    level: u64,
    x: u64,
    y: u64,
    width: u64,
    height: u64,
    buffer: *mut SweetMTLBuffer,
    offset: u64,
    bytes_per_row: u64,
    bytes_per_image: u64,
) -> c_int {
    guard(-1, || {
        const CALL: &str = "sweetmtl_blit_encoder_copy_texture_to_buffer";
        let Some(state) = (unsafe { SweetMTLBlitEncoder::state(encoder) }) else {
            set_last_error(format!("{CALL}: encoder is NULL"));
            return -1;
        };
        let Some(encoder) = state.encoder.as_mut() else {
            set_last_error(format!("{CALL}: the encoder has ended"));
            return -1;
        };
        let Some(texture) = (unsafe { SweetMTLTexture::state(texture) }) else {
            set_last_error(format!("{CALL}: texture is NULL"));
            return -1;
        };
        let Some(buffer) = (unsafe { SweetMTLBuffer::state(buffer) }) else {
            set_last_error(format!("{CALL}: buffer is NULL"));
            return -1;
        };
        status(
            encoder.copy_from_texture_to_buffer(
                &texture.texture,
                slice,
                level,
                Origin::new(x, y, 0),
                Size::new(width, height, 1),
                &buffer.buffer,
                offset,
                bytes_per_row,
                bytes_per_image,
                BlitOption::None,
            ),
            || format!("copy_from_texture_to_buffer({width}x{height} at {x},{y})"),
        )
    })
}

/// `endEncoding` of a blit encoder; the handle then only has to be freed.
///
/// Returns 0 on success and nonzero on failure, with
/// [`sweetmtl_last_error`](crate::sweetmtl_last_error) saying why.
///
/// # Safety
///
/// `encoder` is a live blit encoder handle not yet ended.
#[no_mangle]
pub unsafe extern "C" fn sweetmtl_blit_encoder_end(encoder: *mut SweetMTLBlitEncoder) -> c_int {
    guard(-1, || {
        let Some(state) = (unsafe { SweetMTLBlitEncoder::state(encoder) }) else {
            set_last_error("sweetmtl_blit_encoder_end: encoder is NULL");
            return -1;
        };
        let Some(encoder) = state.encoder.take() else {
            set_last_error("sweetmtl_blit_encoder_end: the encoder has already ended");
            return -1;
        };
        status(encoder.end_encoding(), || "end_encoding".to_owned())
    })
}

/// Frees a blit encoder handle. Does nothing for NULL. Only after [`sweetmtl_blit_encoder_end`].
///
/// # Safety
///
/// `encoder` is a live blit encoder handle, released once.
#[no_mangle]
pub unsafe extern "C" fn sweetmtl_blit_encoder_release(encoder: *mut SweetMTLBlitEncoder) {
    guard((), || {
        // SAFETY: the caller's contract.
        unsafe { SweetMTLBlitEncoder::free(encoder) }
    })
}
