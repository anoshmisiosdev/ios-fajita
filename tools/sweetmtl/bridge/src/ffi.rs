//! The C ABI's shared plumbing: the last-error slot, the panic guard every entry point runs in,
//! and readers for C string arguments.

use std::cell::RefCell;
use std::ffi::{c_char, CStr, CString};

thread_local! {
    /// The last failure of a call on this thread. A `c_char` pointer handed out by
    /// [`last_error_ptr`] stays valid until this thread's next failing call.
    static LAST_ERROR: RefCell<CString> = RefCell::new(CString::default());
}

/// Records `message` as this thread's last error.
pub fn set_last_error(message: impl AsRef<str>) {
    // A NUL cannot travel in a C string; any in the message would otherwise be a leak of the
    // rest of the text.
    let text = CString::new(message.as_ref().replace('\0', " ")).unwrap_or_default();
    LAST_ERROR.with(|slot| *slot.borrow_mut() = text);
}

/// Clears this thread's last error (a call that returns a usable value did not fail).
pub fn clear_last_error() {
    set_last_error("");
}

/// The pointer [`sweetmtl_last_error`](crate::sweetmtl_last_error) hands back.
pub fn last_error_ptr() -> *const c_char {
    LAST_ERROR.with(|slot| slot.borrow().as_ptr())
}

/// Runs `body`, catching a panic so it cannot unwind into Metal.framework or into a bare ICD.
///
/// A panic is recorded in the last-error slot and `on_panic` is returned, so every entry point
/// answers with a value that means failure rather than by unwinding.
pub fn guard<T>(on_panic: T, body: impl FnOnce() -> T) -> T {
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(body)) {
        Ok(value) => value,
        Err(_) => {
            set_last_error("the SweetMTL bridge panicked (see stderr)");
            on_panic
        }
    }
}

/// The argument `ptr` names, as a Rust string.
///
/// # Safety
///
/// `ptr` is NULL or a NUL-terminated C string valid for the call.
pub unsafe fn str_arg(ptr: *const c_char, what: &str) -> Result<String, String> {
    match unsafe { opt_str_arg(ptr, what) }? {
        Some(text) => Ok(text),
        None => Err(format!("{what} is NULL")),
    }
}

/// The argument `ptr` names, or `None` where it is NULL.
///
/// # Safety
///
/// `ptr` is NULL or a NUL-terminated C string valid for the call.
pub unsafe fn opt_str_arg(ptr: *const c_char, what: &str) -> Result<Option<String>, String> {
    if ptr.is_null() {
        return Ok(None);
    }
    // SAFETY: the caller's contract.
    let text = unsafe { CStr::from_ptr(ptr) };
    match text.to_str() {
        Ok(text) => Ok(Some(text.to_owned())),
        Err(_) => Err(format!("{what} is not UTF-8")),
    }
}

/// The bytes of `ptr` and `length`, or an error naming `what` for a NULL pointer with a length.
///
/// # Safety
///
/// `ptr` is NULL or names `length` readable bytes.
pub unsafe fn bytes_arg<'a>(ptr: *const u8, length: u64, what: &str) -> Result<&'a [u8], String> {
    if ptr.is_null() {
        if length == 0 {
            return Ok(&[]);
        }
        return Err(format!("{what} is NULL with a length of {length}"));
    }
    let length = usize::try_from(length).map_err(|_| format!("{what} length {length} is too large"))?;
    // SAFETY: the caller's contract.
    Ok(unsafe { std::slice::from_raw_parts(ptr, length) })
}
