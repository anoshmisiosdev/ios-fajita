//! The C ABI's opaque handles: one `#[repr(C)]` type per Metal object, each owning a boxed Rust
//! state value that only this crate sees.
//!
//! C never constructs, reads or writes one; the header declares each as
//! `typedef struct { void *_state; } SweetMTL…;`, and every function in [`crate`] takes and
//! returns pointers to them, so no Rust type crosses the boundary.
//!
//! The handle types are written out one by one (rather than generated) so cbindgen, which reads
//! the source and does not expand macros, writes a definition for each. The methods every handle
//! shares come from [`opaque_handle`].

use crate::backend::DeviceState;
use crate::objects::{
    BufferState, CommandBufferState, CommandQueueState, ComputeEncoderState, ComputePipelineState,
    FunctionState, LibraryState,
};
use std::ffi::c_void;

/// Defines the methods every handle has: `make` (a handle owning a state value), `state` (the
/// state behind a live handle) and `free` (release it).
macro_rules! opaque_handle {
    ($name:ident, $state:ty) => {
        impl $name {
            /// A handle owning `state`. The caller owns the handle from here: it is released with
            /// the matching `sweetmtl_*_release`, never by C itself.
            pub fn make(state: $state) -> *mut $name {
                let state: *mut $state = Box::into_raw(Box::new(state));
                Box::into_raw(Box::new($name {
                    _state: state.cast(),
                }))
            }

            /// The state behind a live handle, or `None` where `ptr` is NULL.
            ///
            /// # Safety
            ///
            /// `ptr` is NULL or a handle [`make`](Self::make) returned and [`free`](Self::free)
            /// has not released.
            pub unsafe fn state<'a>(ptr: *mut $name) -> Option<&'a mut $state> {
                if ptr.is_null() {
                    return None;
                }
                // SAFETY: the caller's contract.
                Some(unsafe { &mut *(*ptr)._state.cast::<$state>() })
            }

            /// Frees the handle and its state, once.
            ///
            /// # Safety
            ///
            /// `ptr` is NULL or a handle [`make`](Self::make) returned and this has not released.
            pub unsafe fn free(ptr: *mut $name) {
                if ptr.is_null() {
                    return;
                }
                // SAFETY: the caller's contract.
                unsafe {
                    drop(Box::from_raw((*ptr)._state.cast::<$state>()));
                    drop(Box::from_raw(ptr));
                }
            }
        }
    };
}

/// The Vulkan device the bridge adopted: an instance and a logical device made through an ICD.
///
/// Release it only after every object made from it: the runtime's objects are made from the
/// `VkDevice` this owns.
#[repr(C)]
pub struct SweetMTLDevice {
    /// This bridge's own state (a boxed `DeviceState`); C never reads or writes it.
    _state: *mut c_void,
}

/// `MTLBuffer` in shared storage.
#[repr(C)]
pub struct SweetMTLBuffer {
    /// This bridge's own state; C never reads or writes it.
    _state: *mut c_void,
}

/// `MTLLibrary`.
#[repr(C)]
pub struct SweetMTLLibrary {
    /// This bridge's own state; C never reads or writes it.
    _state: *mut c_void,
}

/// `MTLFunction`.
#[repr(C)]
pub struct SweetMTLFunction {
    /// This bridge's own state; C never reads or writes it.
    _state: *mut c_void,
}

/// `MTLComputePipelineState`.
#[repr(C)]
pub struct SweetMTLComputePipeline {
    /// This bridge's own state; C never reads or writes it.
    _state: *mut c_void,
}

/// `MTLCommandQueue`.
#[repr(C)]
pub struct SweetMTLCommandQueue {
    /// This bridge's own state; C never reads or writes it.
    _state: *mut c_void,
}

/// `MTLCommandBuffer`.
#[repr(C)]
pub struct SweetMTLCommandBuffer {
    /// This bridge's own state; C never reads or writes it.
    _state: *mut c_void,
}

/// `MTLComputeCommandEncoder`. [`sweetmtl_compute_encoder_end`](crate::sweetmtl_compute_encoder_end)
/// consumes the encoder; the handle is freed separately.
#[repr(C)]
pub struct SweetMTLComputeEncoder {
    /// This bridge's own state; C never reads or writes it.
    _state: *mut c_void,
}

opaque_handle!(SweetMTLDevice, DeviceState);
opaque_handle!(SweetMTLBuffer, BufferState);
opaque_handle!(SweetMTLLibrary, LibraryState);
opaque_handle!(SweetMTLFunction, FunctionState);
opaque_handle!(SweetMTLComputePipeline, ComputePipelineState);
opaque_handle!(SweetMTLCommandQueue, CommandQueueState);
opaque_handle!(SweetMTLCommandBuffer, CommandBufferState);
opaque_handle!(SweetMTLComputeEncoder, ComputeEncoderState);
