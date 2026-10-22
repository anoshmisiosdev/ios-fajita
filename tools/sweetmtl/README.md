# SweetMTL — the Metal driver Metal.framework loads on the 6T

This is the userspace half of the GPU work: a Metal driver bundle that
CoreAnimation (inside `backboardd`) uses instead of Apple's GPU stack, running
on Mesa's Vulkan freedreno driver (Turnip) over the SweetGPU user client that
patch `0085`/`0086` provide. It is the Metal counterpart of ios-sailfish's
`tools/sweetgl` (OpenGL ES).

## How Metal is told to load it

ios-sailfish gets its GLES driver into `backboardd` with
`DYLD_INSERT_LIBRARIES` and an `EAGLContext` swizzle. Metal has a different —
official — hook, and it is the one this directory uses:

1. CoreAnimation polls the I/O registry for `IOServiceMatching("IOAcceleratorES")`
   in every process that sets up CA rendering.
2. Metal reads two properties off that service:
   - **`MetalPluginName`** — the driver bundle's path **without** the `.bundle`
     suffix, resolved like the macOS accelerator property (relative to
     `/System/Library/Extensions`).
   - **`MetalPluginClassName`** — the class inside the bundle to instantiate.
3. Metal loads the bundle and instantiates the class, then calls
   `-initWithAcceleratorPort:`.

The kernel side is `iokit/Kernel/arm/SweetGPU.cpp` (patch
`0087-fajita-ioacceleratores-metal.patch`): `SweetGPU::start()` publishes an
`IOAcceleratorES` when boot-arg `sweetaccel=1`, and sets `MetalPluginName` /
`MetalPluginClassName` from `sweetmtl=` / `sweetmtlclass=`. With no
`MetalPluginName` Metal passes over the service and CA keeps its GLES renderer,
which is exactly what ios-sailfish's `0166` does deliberately.

This is the same scheme `NVAccel.kext` uses on macOS to load the unsigned
`NVMTLDriver.bundle`:

```xml
<key>MetalPluginName</key><string>../../../Library/GPUBundles/NVMTLDriver</string>
<key>MetalPluginClassName</key><string>NVMTLDevice</string>
```

The only reason the bundle can be loaded at all is the code-signing relaxation
already carried in patch `0085`: `cs_enforcement_disable=1` stops signing
enforcement at the mapping layer, and the AMFI policy syscall (op `0x5a` →
`0x7b`, policy renamed `"AMFI"`) lets dyld/mmap map an ad-hoc-signed bundle into
`backboardd`. See the "Loading the userland driver" section of `FAJITA.md`.

## The class contract

`SweetMTLDevice` subclasses Metal's private `MTLIOAccelDevice` and overrides
`-initWithAcceleratorPort:` — the entry point Metal calls (the macOS driver's
`NVMTLDevice` is identical in shape):

```objc
@interface MTLIOAccelDevice : NSObject
- (instancetype)initWithAcceleratorPort:(mach_port_t)port;
@end
@interface SweetMTLDevice : MTLIOAccelDevice <MTLDevice>
@end
```

iPhoneOS 27.0's `Metal.tbd` lists `MTLIOAccelDevice` — with `MTLIOAccelBuffer`,
`MTLIOAccelCommandBuffer`, `MTLIOAccelComputeCommandEncoder` and the rest —
among its ObjC classes, so the superclass resolves on the phone; if a boot log
ever shows the class failing to instantiate, that listing is the first thing to
check. (The `.tbd` lists classes without the `_OBJC_CLASS_$_` prefix, which is
why a `nm`-style grep for the name finds nothing.)

## Build, install, boot

```sh
./build.sh                       # -> out/SweetMTLDriver.bundle, out/metal_compute,
                                 #    out/scale_and_offset.metallib (ad-hoc signed)
./build.sh --host                # macOS, for the local self-check (see below)
ADB=1 ./push.sh                  # or: ./push.sh /path/to/mounted/ios16-root
```

`build.sh` links the bridge's iOS staticlib into the bundle — from
`$SWEETMTL_BRIDGE_ARTIFACT` (an unpacked `sweetmtl-bridge-ios` CI artifact),
else `bridge/out/`, else a local
`bridge/target/aarch64-apple-ios/release/libsweetmtl_bridge.a`. Nothing is
staged next to the bundle: the driver needs no second dylib, and it opens the
ICD by absolute path — `/usr/lib/sweetmtl/libvulkan_freedreno.dylib`, then
`/usr/lib/sweetmtl/libvulkan_lvp.dylib`, unless `SWEETMTL_ICD` says otherwise.
Build Turnip to the first of those; `push.sh` stages an
`out/libvulkan_freedreno.dylib` if there is one, and leaves the root's own file
alone if not.

Boot with:

```
sweetaccel=1
sweetmtl=../../../usr/lib/sweetmtl/SweetMTLDriver
sweetmtlclass=SweetMTLDevice
```

Expect `SWEET GPU: IOAcceleratorES Metal plugin ...` from the kernel, then on
stderr `sweetmtl: initWithAcceleratorPort:<port> pid <pid> (<process>)`,
`sweetmtl: ICD ...` and `sweetmtl: device: <name>` once Metal loads the bundle.
`/usr/lib/sweetmtl/metal_compute /usr/lib/sweetmtl/scale_and_offset.metallib`
then runs one dispatch through it (below).

## The driver

```
tools/sweetmtl/
  SweetMTLDriver.h             private header: the handle rename, logging, the stub rule,
                               SweetMTLObject and the seven object classes, SweetMTLDevice,
                               SweetMTLAirLibrary
  SweetMTLDevice.m             the class Metal instantiates; the ICD choice; the stub machinery
  SweetMTLObjects.m            buffer, library, function, pipeline, queue, command buffer, encoder
  test/scale_and_offset.metal  the kernel the test tool dispatches
  test/metal_compute.m         the tool: MTLCreateSystemDefaultDevice() + one dispatch, checked
  Info.plist                   bundle metadata
```

The MTLDevice a client sees:

| works | over |
| --- | --- |
| `-name` | `sweetmtl_device_name` |
| `newBufferWithLength:options:`, `newBufferWithBytes:length:options:` | `sweetmtl_device_new_buffer[_with_bytes]` |
| `newLibraryWithData:error:` (a metallib — the normal client door) | `sweetmtl_device_new_library_with_data` |
| `newLibraryWithAIR:error:` (the driver's own door, `llvm-as` output) | `sweetmtl_device_new_library_with_air` |
| `newFunctionWithName:` | `sweetmtl_library_new_function` |
| `newComputePipelineStateWithFunction:error:` | `sweetmtl_device_new_compute_pipeline` |
| `newCommandQueue`, `-commandBuffer`, `-computeCommandEncoder` | the queue/buffer/encoder constructors |
| `setComputePipelineState:`, `setBuffer:offset:atIndex:`, `dispatchThreads:threadsPerThreadgroup:`, `dispatchThreadgroups:threadsPerThreadgroup:` (threads-per-group × groups), `endEncoding` | `sweetmtl_compute_encoder_*` |
| `commit`, `waitUntilCompleted` (0 waits indefinitely), `-status`, `-error` | `sweetmtl_command_buffer_*` |
| `-contents`, `-length`, `-maxTotalThreadsPerThreadgroup`, `-threadExecutionWidth` | the bridge's getters |
| `-functionType` (always Kernel: the bridge makes compute pipelines only) | this side |
| `-device`, `-name`/`-label` on everything | this side |

The bridge makes **shared-storage** buffers only; a caller asking for another
storage mode gets a shared buffer and one line saying so. A failed bridge call is
one line (`newBufferWithLength:options:: <the bridge's message>`) and nil/NULL to
the caller; inside an encoder it lands in `-[MTLCommandBuffer error]` too, which
is Metal's own channel for it. A buffer, pipeline or function that came from
somewhere else is refused with a message rather than by a trap.

## What is stubbed, and how

Metal's protocols are roughly 150 selectors wide and the bridge answers 33
functions of them. Everything else is answered the same way: the runtime asks
the class for a missing selector (`+resolveInstanceMethod:`), the driver logs

```
sweetmtl: unimplemented -[SweetMTLBuffer setPurgeableState:]
```

**once per class and selector**, installs a single stub IMP for it, and answers
nil/0/NO. Nothing traps, and a client may take the selector again as often as it
likes. The stub clears the return registers, which covers pointer, integer and
two-word-struct returns; the *wider* struct returns and the float returns in the
protocols a client of this driver touches are implemented explicitly instead —
device `maxThreadsPerThreadgroup`, pipeline `requiredThreadsPerThreadgroup`,
command-buffer `kernelStartTime`/`kernelEndTime`/`GPUStartTime`/`GPUEndTime`:
each writes the same "unimplemented" line and answers zeros, so no caller ever
reads a stack variable as a struct or a time.

`-forwardingTargetForSelector:` cannot do this: it may only hand a message to an
object that already implements the selector, and `-methodSignatureForSelector:`
would have to invent a signature for one the protocol does not declare — the
runtime ends in `-doesNotRecognizeSelector:` either way.

Two consequences worth knowing. `-respondsToSelector:` says no until the
selector has been sent once, because the stub is installed by the first message;
a client that probes before using a door sees the conservative answer. And a
capability query — `supportsFamily:`, `registryID`, `hasUnifiedMemory` — is one
of those stubs: it answers no/0, not because the GPU said so but because the
bridge does not report capabilities yet. A client that needs them needs the
bridge to grow the call.

## The test tool

`test/metal_compute.m` runs the dispatch the bridge's C test
(`bridge/tests/compute.c`) runs, but from a normal Metal client instead of the C
ABI: `MTLCreateSystemDefaultDevice()`, `newLibraryWithData:error:`,
`newFunctionWithName:`, pipeline state, buffers, encoder, `dispatchThreads:`,
`commit`, `waitUntilCompleted`, and the result read back through `-contents` and
checked — 1000 floats in 64-wide threadgroups, `output[i] == 3i`. Exit 0 and
`sweetmtl: metal_compute through Metal.framework: OK`.

```
metal_compute <library>                 a .metallib (the door a client uses)
SWEETMTL_AIR=1 metal_compute <library>  AIR bitcode, through the driver's own door
SWEETMTL_DIRECT=<driver bundle> ...     instantiate SweetMTLDevice from the bundle
                                        directly, with Metal not in the picture (a host)
```

`./build.sh` builds it for the target SDK — the CI `driver` job builds it, and
the bundle, from the `ios` job's bridge artifact — and `--host` builds both for
this machine. `xcrun -sdk <sdk> metal`/`metallib` build the kernel next to it
(`out/scale_and_offset.metallib`); with Xcode 16.3+ they need
`xcodebuild -downloadComponent MetalToolchain` first, and `build.sh` says so and
carries on without the metallib when they are missing (the AIR mode then still
runs, against `llvm-as` output).

## Where it was tested

- **On this Mac, over MoltenVK, it passes end to end** (2026-10-22: Mac16,8,
  Apple M4 Pro, macOS 27.0 26A428, Homebrew MoltenVK 1.4.2, no loader).
  `./build.sh --host`, then:

  ```sh
  SWEETMTL_DIRECT=out/SweetMTLDriver.bundle \
  SWEETMTL_ICD=/opt/homebrew/Cellar/molten-vk/1.4.2/lib/libMoltenVK.dylib \
  ./out/metal_compute out/scale_and_offset.metallib
  ```

  prints `sweetmtl: device: Apple M4 Pro`, `device: Apple M4 Pro
  (SweetMTLDevice)`, `library: metallib: out/scale_and_offset.metallib`,
  `pipeline: 1024 threads per threadgroup at most, SIMD width 32`,
  `committed; waiting`, `status: Completed`,
  `output[0..4] = 0 3 6 9, ..., output[999] = 2997: as expected` and
  `sweetmtl: metal_compute through Metal.framework: OK`, exit 0 — the read-back
  Lavapipe never let this machine see. `SWEETMTL_AIR=1` with
  `out/scale_and_offset.air` (the driver's own AIR door) passes the same way.
  The bridge needed no change: it already enables `VK_KHR_portability_subset`
  on a device that lists it, and an instance made straight from the ICD has no
  loader to hide a portability driver, so `VK_KHR_portability_enumeration` does
  not come into it. metal2vulkan accepts the device, saying so in four
  diagnostics — `[metal2vulkan-unmeasured-driver]` (MoltenVK driverVersion
  10402 is newer than the 10401 its tables measured),
  `[metal2vulkan-degrade-moltenvk-argument-buffers-unread]`,
  `[metal2vulkan-degrade-beyond-measured-deviation]` and
  `[metal2vulkan-degrade-split-dispatch]` (1000 threads are not a whole number
  of 64-wide groups) — none of them a refusal. `MVK_CONFIG_LOG_LEVEL=2` printed
  no MoltenVK errors.
- **With Homebrew Lavapipe instead, the Objective-C layer runs to the same
  platform limit the bridge's C test has.** `./build.sh --host`, then with
  Homebrew Mesa 26.0.2's Lavapipe and no loader:

  ```sh
  SWEETMTL_DIRECT=out/SweetMTLDriver.bundle \
  SWEETMTL_ICD=/opt/homebrew/Cellar/mesa/26.0.2/lib/libvulkan_lvp.dylib \
  ./out/metal_compute out/scale_and_offset.metallib
  ```

  prints `device: llvmpipe (LLVM 22.1.0, 128 bits) (SweetMTLDevice)` — our class,
  instantiated by the tool's `dlopen` as Metal would — then the **metallib**
  loaded through `-newLibraryWithData:error:` (a door the bridge's C test never
  exercised), the function, `pipeline: 1024 threads per threadgroup at most, SIMD
  width 32`, the buffers and their `-contents` check, `setBuffer`,
  `dispatchThreads:`, `endEncoding`, `commit`, and dies with **SIGSEGV (exit
  139)** inside Lavapipe's *own* `libLLVM.dylib`
  (`DAGTypeLegalizer::PromoteIntRes_Select`) while it compiles the translated
  shader — the identical failure the bridge's README documents, with no bridge,
  runtime or driver frame in the stack. `SWEETMTL_AIR=1` with
  `out/scale_and_offset.air` (the same source through `xcrun metal -c`) takes the
  driver's other library door and stops at the same point. So the last step —
  `status: Completed` and the result check — cannot be seen on this machine. The
  stub path was seen: `sweetmtl: unimplemented -[SweetMTLLibrary functionNames]`,
  once, from the tool's own probe.
- **The C ABI under it passes on Linux** — the same dispatch with the same
  expected results, from the ICD's `vk_icdGetInstanceProcAddr` to the read-back
  (see `bridge/` below). That is what the CI `lavapipe` job runs.
- **The same build runs in CI**, in the workflow's `driver` job (runs
  39863197926 on `b4ddeb3` and 39864198007 on `10f3a4a`): the `ios` job's `libsweetmtl_bridge.a` comes back as
  an artifact, `sh build.sh` runs against it, and the bundle, the tool and the
  metallib are uploaded as `sweetmtl-driver-ios`. That is compiling and linking
  for iphoneos only — nothing on a runner executes the driver, because nothing
  there has an accelerator.
- **Never tested: on the phone.** No `fajita-gpu` boot has had `sweetaccel=1`,
  the bundle and a Turnip build together yet. The first thing to read in that log
  is the `sweetmtl:` lines — the port, the ICD it chose, the device name — and
  then what `metal_compute` prints.

## Under the driver

The translation and the Vulkan work are not written here:

- **Translator** — [steelbrain/metal2vulkan](https://github.com/steelbrain/metal2vulkan)
  ("Translate Metal shaders to Vulkan, also a Metal Device API backed by Vulkan")
  and [UniXNU/NativeMetal](https://github.com/UniXNU/NativeMetal). Apple AIR
  bitcode → SPIR-V, and the Metal object model → Vulkan, both behind the
  bridge's C ABI. NullMoth's `nvidia-macos-driver` is a working, complete example
  of exactly this design (`plugin/NVMTLDevice.m` + `translator/`, LGPL-3.0 based on
  metal2vulkan).
- **Vulkan backend** — Mesa Turnip for freedreno (a6xx). Build it like
  `tools/sweetgl/mesa-setup.sh` but with `-Dvulkan-drivers=freedreno`; the result
  is `libvulkan_freedreno.dylib`, which belongs at `/usr/lib/sweetmtl/` next to
  the bundle. It talks to the GPU through the SweetGPU user client (the msm-style
  BO/submit interface the kernel already exposes).
- **The CA display path** (IOSurface ↔ Turnip image, `AppleCLCD` present) is the
  same work `tools/sweetgl`'s `sweet_drm.c` already does for GLES, and the driver
  does not do it yet: this milestone is a compute dispatch a client drives, not a
  layer CoreAnimation can present with.

## The Vulkan bridge — `bridge/`

`bridge/` is `sweetmtl-bridge`, a Rust crate (staticlib + cdylib) that puts a
small, stable C ABI over metal2vulkan's *runtime* — the crate that owns the
Metal object model on Vulkan. `SweetMTLDevice` calls this ABI; no Rust type
crosses the boundary, only opaque handles.

```
tools/sweetmtl/bridge/
  Cargo.toml                 ONE metal2vulkan revision, twice: translator + runtime
  src/{lib,ffi,handle,objects,backend}.rs
  include/sweetmtl_bridge.h  generated from src/ by cbindgen (`generate-header.sh`)
  tests/compute.c            the C test: the runtime's compute_dispatch example over the ABI
  tests/run.sh               assembles the fixture and runs it against a bare Mesa ICD
.github/workflows/sweetmtl-bridge.yml
                             `ios` job: builds for aarch64-apple-ios, checks the header,
                             uploads `libsweetmtl_bridge.a`, `.dylib` and the header
                             `driver` job: downloads that artifact and builds
                             SweetMTLDriver.bundle + metal_compute from it (build.sh),
                             uploading all three
                             `lavapipe` job: runs the C test on Linux
```

The ABI (33 functions, 8 handle types) is one milestone of `MTLDevice`: open a
device on a named ICD, read its name, allocate a shared-storage buffer, make a
library from AIR, get a function, make a compute pipeline, make a command
queue/command buffer/compute encoder, set the pipeline, set a buffer at an
index, dispatch a grid of threads in threadgroups, end, commit, wait for
completion, query the status, release everything. Failures come back as NULL/0
with `sweetmtl_last_error()` set; a Rust panic never crosses the boundary
(`catch_unwind`).

### Decisions worth knowing

**The ICD is opened directly; there is no Vulkan loader.** iOS has none, and
Turnip and Lavapipe both export `vk_icdGetInstanceProcAddr`, so the bridge opens
that one symbol, makes the `VkInstance` and `VkDevice` itself and adopts them
with `Device::from_raw` (`RawDevice`). The alternative, `DeviceOptions.loader`,
takes a *loader* library path that `open_loader` dlopens and asks for
`vkGetInstanceProcAddr`/`vkEnumerateInstanceVersion` — an ICD-only driver has
neither. The instance is made at Vulkan 1.2 only, and
`VK_ERROR_INCOMPATIBLE_DRIVER` is reported as "the ICD cannot make a Vulkan 1.2
instance".

**The bridge enables exactly what the physical device reports.** One
`vkGetPhysicalDeviceFeatures2` chaining `VkPhysicalDeviceVulkan11Features` and
`VkPhysicalDeviceVulkan12Features` gives the three sets; the bridge then chains
those same filled structures into `VkDeviceCreateInfo`, with
`features2.features` as `pEnabledFeatures`. That is table-free — it does not
duplicate the runtime's private ~1350-line member table — and it cannot be
refused, because it never asks for more than the device reports. The one
consequence: the bridge enables a *superset* of what the runtime declares to us
of its own policy — for example `robustBufferAccess` — which only makes the
driver more conservative than the runtime assumes. A device that reports neither
`timelineSemaphore` nor `hostQueryReset` is refused up front with a named error,
since the runtime needs both.

**ash's `push_next` hazard** (its only symptom was `vkCreateDevice` never
returning). `push_next` does not just prepend a structure: it walks the *pushed
structure's own* chain to its last link and rewires that link to the old head.
A structure that already took part in an earlier chain therefore still carries
it. Chaining the query's `v11`/`v12` into the create info a second time made
`v11.p_next` point at `v11` — a circular `pNext` chain — and the driver walked
it forever. The bridge clears `p_next` on both structures before reusing them.

**The driver renames the bridge's handles, once, in `SweetMTLDriver.h`.** The
header names each handle after the Metal class it stands for (`SweetMTLBuffer`,
`SweetMTLCommandBuffer`, …), which is what makes the C test read like Metal —
and in Objective-C a class shares the C namespace, so
`@interface SweetMTLBuffer` cannot be declared next to a C `SweetMTLBuffer`
("redefinition … as different kind of symbol"). The driver therefore `#define`s
each handle to `…Handle` around its one `#include "sweetmtl_bridge.h"` and
`#undef`s them again. The ABI is untouched: no `sweetmtl_*` function changed
name, `bridge/tests/compute.c` and cbindgen's header are exactly as they were.

### What works, and where it was tested

- `cargo check --target aarch64-apple-ios` and
  `cargo build --release --target aarch64-apple-ios` pass (rustc 1.99.1); CI
  builds both artifacts.
- Against bare Lavapipe (macOS, Homebrew Mesa 26.0.2, LLVM 22.1, no loader):
  device creation and its name, the library, the function, the compute pipeline
  (and its "1024 threads per threadgroup at most, SIMD width 32"),
  the encoder with its buffers, and `commit` all work — the whole C ABI up to
  submission.
- **It passes on Linux**, which is what the test is written for: in a Debian 13
  (trixie) arm64 container, against the system Lavapipe at
  `/usr/lib/aarch64-linux-gnu/libvulkan_lvp.so` (Mesa 25.0.7, LLVM 19.1.7) with
  no loader, `tests/run.sh` ends in
  `device: llvmpipe (LLVM 19.1.7, 128 bits)` … `status: Completed` …
  `output[0..4] = 0 3 6 9, ..., output[999] = 2997: as expected` …
  `sweetmtl-bridge: compute_dispatch through the C ABI: OK`, exit 0 — the whole
  ABI, from the ICD's `vk_icdGetInstanceProcAddr` to a dispatched grid whose
  results are read back and checked. The CI `lavapipe` job runs that same test
  on `ubuntu-24.04` with apt.llvm.org's LLVM 21 and noble's Lavapipe.
- **It passes on this Mac over MoltenVK** (1.4.2, Apple M4 Pro, macOS 27.0, no
  loader): `METAL2VULKAN_LLVM_AS=/opt/homebrew/opt/llvm@21/bin/llvm-as sh
  tests/run.sh /opt/homebrew/Cellar/molten-vk/1.4.2/lib/libMoltenVK.dylib` ends in
  `device: Apple M4 Pro` … `status: Completed` …
  `output[0..4] = 0 3 6 9, ..., output[999] = 2997: as expected` …
  `sweetmtl-bridge: compute_dispatch through the C ABI: OK`, exit 0, with no
  bridge change.
- **Over Homebrew Lavapipe on this Mac it stops at the last step**, and the
  reason is the platform, not the bridge: `sweetmtl_command_buffer_wait_until_completed` takes SIGSEGV
  inside `libLLVM.dylib`'s `DAGTypeLegalizer::PromoteIntRes_Select` — Lavapipe's
  own shader compiler, reached on one of its worker threads while the dispatch
  runs. No bridge or runtime frame is in that stack. Controls: a trivial
  hand-written SPIR-V kernel (`void main() {}`, no metal2vulkan) dispatches
  cleanly on the same ICD, with no features and with every reported one; the
  translated module metal2vulkan caches also compiles there (it then faults on
  the null descriptor that test never binds); and the same test passes on Linux
  with the same Mesa. So this is llvmpipe's LLVM *22.1* JIT — the one build that
  differs — failing on the shader variant the runtime dispatches, not an ICD that
  cannot dispatch at all.
- Never tested: anything on hardware, and any Turnip build.

Run it with `sh tests/run.sh [<icd-path>]`; `SWEETMTL_VK_ICD`,
`METAL2VULKAN_LLVM_AS` (LLVM 21's `llvm-as`) and `METAL2VULKAN_DIR` override what
it finds. The script fetches the pinned metal2vulkan before looking for the
fixture (a fresh machine, or a container, has no checkout yet), finds the
checkout under `$CARGO_HOME` rather than `$HOME/.cargo` (a container toolchain
sets it elsewhere), and picks the ICD by architecture on Linux.
