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

If iOS's `Metal.framework` does not export `MTLIOAccelDevice`, the class will
not instantiate; that is the first thing to check in the boot log.

## Build, install, boot

```sh
./build.sh                       # -> out/SweetMTLDriver.bundle (ad-hoc signed)
ADB=1 ./push.sh                  # or: ./push.sh /path/to/mounted/ios16-root
```

Boot with:

```
sweetaccel=1
sweetmtl=../../../usr/lib/sweetmtl/SweetMTLDriver
sweetmtlclass=SweetMTLDevice
```

Expect `SWEET GPU: IOAcceleratorES Metal plugin ...` from the kernel and
`sweetmtl: initWithAcceleratorPort: ...` / `Metal device live` on stderr once
Metal loads the bundle.

## What is still stubbed (the driver body)

`SweetMTLDevice.m` is the **load milestone**: it proves Metal instantiates the
class and connects the Vulkan backend. The actual `MTLDevice` implementation is
the metal2vulkan translation layer plus Turnip:

- **Translator** — [steelbrain/metal2vulkan](https://github.com/steelbrain/metal2vulkan)
  ("Translate Metal shaders to Vulkan, also a Metal Device API backed by Vulkan")
  and [UniXNU/NativeMetal](https://github.com/UniXNU/NativeMetal). Apple AIR
  bitcode → SPIR-V, and the Metal object model → Vulkan. NullMoth's
  `nvidia-macos-driver` is a working, complete example of exactly this design
  (`plugin/NVMTLDevice.m` + `translator/`, LGPL-3.0 based on metal2vulkan).
- **Vulkan backend** — Mesa Turnip for freedreno (a6xx). Build it like
  `tools/sweetgl/mesa-setup.sh` but with `-Dvulkan-drivers=freedreno`; the result
  is `libvulkan_freedreno.dylib`, staged next to the bundle by `build.sh`. It
  talks to the GPU through the SweetGPU user client (the msm-style BO/submit
  interface the kernel already exposes).
- The CA display path (IOSurface ↔ Turnip image, `AppleCLCD` present) is the
  same work `tools/sweetgl`'s `sweet_drm.c` already does for GLES.

Nothing here is hardware-tested yet: the `fajita-gpu` branch has not booted with
`sweetgpu`/`sweetaccel` enabled.

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
                             builds for aarch64-apple-ios, checks the header, uploads
                             `libsweetmtl_bridge.a`, `.dylib` and the header
```

The ABI (33 functions, 8 handle types) is one milestone of `MTLDevice`: open a
device on a named ICD, read its name, allocate a shared-storage buffer, make a
library from AIR, get a function, make a compute pipeline, make a command
queue/command buffer/compute encoder, set the pipeline, set a buffer at an
index, dispatch a grid of threads in threadgroups, end, commit, wait for
completion, query the status, release everything. Failures come back as NULL/0
with `sweetmtl_last_error()` set; a Rust panic never crosses the boundary
(`catch_unwind`).

### Two decisions worth knowing

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

### What works, and where it was tested

- `cargo check --target aarch64-apple-ios` and
  `cargo build --release --target aarch64-apple-ios` pass (rustc 1.99.1); CI
  builds both artifacts.
- Against bare Lavapipe (macOS, Homebrew Mesa 26.0.2, LLVM 22.1, no loader):
  device creation and its name, the library, the function, the compute pipeline
  (and its "1024 threads per threadgroup at most, SIMD width 32"),
  the encoder with its buffers, and `commit` all work — the whole C ABI up to
  submission.
- **Then the platform's CPU driver dies, not the bridge.**
  `sweetmtl_command_buffer_wait_until_completed` takes SIGSEGV inside
  `libLLVM.dylib`'s `DAGTypeLegalizer::PromoteIntRes_Select` — Lavapipe's own
  shader compiler, reached on one of its worker threads while the dispatch runs.
  No bridge or runtime frame is in that stack. Controls: a trivial hand-written
  SPIR-V kernel (`void main() {}`, no metal2vulkan) dispatches cleanly on the
  same ICD, with no features and with every reported one; and the translated
  module metal2vulkan caches also compiles there (it then faults on the null
  descriptor that test never binds). So this is llvmpipe's LLVM JIT failing on
  the shader variant the runtime dispatches on this macOS/arm64/LLVM 22.1 build,
  not an ICD that cannot dispatch at all. `tests/run.sh` defaults to the Linux
  ICD path, where the test is expected to pass; on this Mac it stops at that one
  step.
- Never tested: anything on hardware, and any Turnip build.

Run it with `sh tests/run.sh [<icd-path>]`; `SWEETMTL_VK_ICD`,
`METAL2VULKAN_LLVM_AS` (LLVM 21's `llvm-as`) and `METAL2VULKAN_DIR` override what
it finds.
