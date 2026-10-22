#!/bin/sh
# The bridge's C test end to end, with no Vulkan loader anywhere in the picture:
#
#   1. `llvm-as` assembles the translator's owned fixture
#      (`crates/metal2vulkan-validation/fixtures/public/examples/runtime_examples.ll`) into AIR
#      bitcode, as `examples/common/mod.rs` does for the Rust example;
#   2. `cc` links tests/compute.c and tests/render.c against the bridge (the cdylib in
#      target/debug);
#   3. compute opens the bare Mesa ICD by path and runs the same `scale_and_offset` dispatch the
#      runtime's own `compute_dispatch` example runs, checking output[i] == 3i;
#   4. render does what the runtime's `render_to_texture` example does - `colored_vertex` and
#      `colored_fragment` drawing two triangles into a cleared 8x8 texture, blitted to a buffer -
#      and checks every texel.
#
#     tests/run.sh [<icd-path>]
#
# Nothing is taken from `PATH` that the environment cannot override:
#
#   SWEETMTL_VK_ICD        the ICD, where the argument is not given
#   METAL2VULKAN_LLVM_AS   `llvm-as` (LLVM 21 is what the fixture is written for)
#   METAL2VULKAN_DIR       a metal2vulkan checkout to take the fixture from
#   CC                     the C compiler
#
# Defaults: Mesa's Lavapipe ICD (Linux: /usr/lib/<machine>-linux-gnu/libvulkan_lvp.so; macOS:
# Homebrew's /opt/homebrew/Cellar/mesa/…/lib/libvulkan_lvp.dylib), `llvm-as` from PATH then
# Homebrew's LLVM 21, and the fixture from cargo's checkout of the revision Cargo.toml pins.
#
# On macOS (2026-10-22, Homebrew Mesa 26.0.2 / LLVM 22.1) the test gets through the whole ABI --
# device, library, pipeline, encoder, commit -- and then dies in Lavapipe's *own* shader compiler
# (SIGSEGV in libLLVM.dylib's DAGTypeLegalizer while its worker JITs the shader the runtime
# dispatched); a trivial hand-written SPIR-V kernel dispatches cleanly on the same ICD, so that is
# the platform's CPU driver, not the bridge. See README.md. For a pass on macOS, pass MoltenVK's
# libMoltenVK.dylib as the ICD (2026-10-22, MoltenVK 1.4.2 on an M4 Pro: OK); on Linux, Lavapipe.
set -eu
H=$(cd "$(dirname "$0")/.." && pwd)
REL=crates/metal2vulkan-validation/fixtures/public/examples/runtime_examples.ll

find_llvm_as() {
    if [ -n "${METAL2VULKAN_LLVM_AS:-}" ]; then
        echo "$METAL2VULKAN_LLVM_AS"
        return 0
    fi
    for candidate in llvm-as /opt/homebrew/opt/llvm@21/bin/llvm-as /usr/lib/llvm-21/bin/llvm-as; do
        if command -v "$candidate" >/dev/null 2>&1; then
            echo "$candidate"
            return 0
        fi
    done
    return 1
}

find_fixture() {
    rev=$(sed -n 's/.*rev = "\([0-9a-f]\{40\}\)".*/\1/p' "$H/Cargo.toml" | head -1)
    # `$CARGO_HOME`, not `$HOME/.cargo`: a container's toolchain sets it elsewhere (the Rust image
    # uses /usr/local/cargo), and that is where the `cargo fetch` below puts the checkout.
    checkouts=${CARGO_HOME:-$HOME/.cargo}/git/checkouts
    for dir in "$checkouts"/metal2vulkan-*/*; do
        [ -d "$dir" ] || continue
        if [ -n "$rev" ] && [ "$(basename "$dir")" != "$rev" ]; then
            continue
        fi
        if [ -f "$dir/$REL" ]; then
            echo "$dir/$REL"
            return 0
        fi
    done
    for dir in "$checkouts"/metal2vulkan-*/*; do
        [ -f "$dir/$REL" ] || continue
        echo "$dir/$REL"
        return 0
    done
    if [ -n "${METAL2VULKAN_DIR:-}" ] && [ -f "$METAL2VULKAN_DIR/$REL" ]; then
        echo "$METAL2VULKAN_DIR/$REL"
        return 0
    fi
    return 1
}

find_icd() {
    if [ $# -gt 0 ] && [ -n "$1" ]; then
        echo "$1"
        return 0
    fi
    if [ -n "${SWEETMTL_VK_ICD:-}" ]; then
        echo "$SWEETMTL_VK_ICD"
        return 0
    fi
    if [ "$(uname -s)" = "Darwin" ]; then
        ls /opt/homebrew/Cellar/mesa/*/lib/libvulkan_lvp.dylib 2>/dev/null | tail -1
    else
        # Debian/Ubuntu name the directory after the machine, so this covers x86_64 and aarch64.
        for candidate in "/usr/lib/$(uname -m)-linux-gnu/libvulkan_lvp.so" \
                         /usr/lib/x86_64-linux-gnu/libvulkan_lvp.so \
                         /usr/lib/aarch64-linux-gnu/libvulkan_lvp.so \
                         /usr/lib/libvulkan_lvp.so; do
            if [ -f "$candidate" ]; then
                echo "$candidate"
                return 0
            fi
        done
        return 1
    fi
}

ICD=$(find_icd "${1:-}") || true
if [ -z "${ICD:-}" ] || [ ! -f "$ICD" ]; then
    echo "run.sh: no Vulkan ICD found: pass one as the first argument or set SWEETMTL_VK_ICD" >&2
    exit 2
fi
LLVM_AS=$(find_llvm_as) || {
    echo "run.sh: no llvm-as: install LLVM 21 or set METAL2VULKAN_LLVM_AS" >&2
    exit 2
}
# The fixture lives in cargo's checkout of the revision Cargo.toml pins - which a machine (or a
# container) that has never built this crate does not have yet, so fetch it before looking.
(cd "$H" && cargo fetch --quiet)
FIXTURE=$(find_fixture) || {
    echo "run.sh: cannot find $REL: set METAL2VULKAN_DIR to a metal2vulkan checkout" >&2
    exit 2
}

echo "icd:     $ICD"
echo "llvm-as: $LLVM_AS"
echo "fixture: $FIXTURE"

# The bridge, as a cdylib the test links against (the same crate builds the staticlib and the
# aarch64-apple-ios artifacts; see README.md).
(cd "$H" && cargo build --quiet)
LIBDIR=${CARGO_TARGET_DIR:-$H/target}/debug
CC=${CC:-cc}
for test in compute render; do
    $CC -std=c11 -O2 -Wall -Wextra -I "$H/include" "$H/tests/$test.c" \
        -L "$LIBDIR" -lsweetmtl_bridge -Wl,-rpath,"$LIBDIR" -o "$H/tests/$test"
done

TMP=${TMPDIR:-/tmp}/sweetmtl-bridge-$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT INT TERM
"$LLVM_AS" "$FIXTURE" -o "$TMP/runtime_examples.air"
"$H/tests/compute" "$TMP/runtime_examples.air" "$ICD"
"$H/tests/render" "$TMP/runtime_examples.air" "$ICD"
