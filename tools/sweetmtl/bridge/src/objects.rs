//! The Metal objects the C ABI hands out, each a thin state value over its `metal2vulkan`
//! counterpart.
//!
//! The runtime's objects are cheap handles into a device that track their own lifetime (a
//! `Buffer` is a clone-able reference to a `VkBuffer` and its memory), so the bridge holds one of
//! each and releases it when C says to, exactly as Metal's own `release` does.

use metal2vulkan_runtime::ComputePipelineState as RuntimePipeline;
use metal2vulkan_runtime::{
    BlitCommandEncoder, Buffer, CommandBuffer, CommandQueue, ComputeCommandEncoder, Function,
    Library, RenderCommandEncoder, RenderPipelineState, Size, Texture,
};

/// `MTLBuffer` in shared storage, and the pointer it was made with.
pub struct BufferState {
    pub buffer: Buffer,
}

/// `MTLLibrary`.
pub struct LibraryState {
    pub library: Library,
}

/// `MTLFunction`.
pub struct FunctionState {
    pub function: Function,
}

/// `MTLComputePipelineState`.
pub struct ComputePipelineState(pub RuntimePipeline);

/// `MTLCommandQueue`.
pub struct CommandQueueState {
    pub queue: CommandQueue,
}

/// `MTLCommandBuffer`.
pub struct CommandBufferState {
    pub command_buffer: CommandBuffer,
}

/// `MTLComputeCommandEncoder` and the command buffer it encodes into.
///
/// `encoder` is `Some` until `sweetmtl_compute_encoder_end` takes it: `end_encoding` consumes the
/// encoder, as Metal's `endEncoding` ends it, and the handle then only has to be freed.
pub struct ComputeEncoderState {
    pub encoder: Option<ComputeCommandEncoder>,
}

/// `MTLTexture`: a 2D texture, its storage mode the caller's.
pub struct TextureState {
    pub texture: Texture,
}

/// `MTLRenderPipelineState`.
pub struct RenderPipelineStateState(pub RenderPipelineState);

/// `MTLRenderCommandEncoder`, `Some` until `sweetmtl_render_encoder_end` takes it, as
/// [`ComputeEncoderState`] is.
pub struct RenderEncoderState {
    pub encoder: Option<RenderCommandEncoder>,
}

/// `MTLBlitCommandEncoder`, `Some` until `sweetmtl_blit_encoder_end` takes it.
pub struct BlitEncoderState {
    pub encoder: Option<BlitCommandEncoder>,
}

/// `MTLSize` for the bridge's `u64` triples.
pub fn size(x: u64, y: u64, z: u64) -> Size {
    Size::new(x, y, z)
}
