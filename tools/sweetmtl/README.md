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
