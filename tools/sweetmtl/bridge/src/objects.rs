//! The Metal objects the C ABI hands out, each a thin state value over its `metal2vulkan`
//! counterpart.
//!
//! The runtime's objects are cheap handles into a device that track their own lifetime (a
//! `Buffer` is a clone-able reference to a `VkBuffer` and its memory), so the bridge holds one of
//! each and releases it when C says to, exactly as Metal's own `release` does.

use metal2vulkan_runtime::ComputePipelineState as RuntimePipeline;
use metal2vulkan_runtime::{
    Buffer, CommandBuffer, CommandQueue, ComputeCommandEncoder, Function, Library, Size,
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

/// `MTLSize` for the bridge's `u64` triples.
pub fn size(x: u64, y: u64, z: u64) -> Size {
    Size::new(x, y, z)
}
