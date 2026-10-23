# sweetvk — Mesa Turnip for iOS (Adreno 630 through SweetGPU)

Turnip, Mesa's Vulkan driver for the freedreno/msm kernel interface, built for
iOS 16 arm64 and pointed at the Adreno 630 in this phone. There is no DRM on
iOS, so the msm ioctls Turnip sends are answered in user space on top of the
SweetGPU IOKit user client (`xnu iokit/Kernel/arm/SweetGPU.cpp`, the same user
client `tools/sweetgl` drives for gallium Freedreno).

Ported from **joranv1/ios-sailfish**'s `tools/sweetgl` (`mesa-setup.sh`,
`ios-cross.txt`, `sweet_drm.c` — the msm DRM emulation over SweetGPU), whose
Mesa is gallium-only. This one is Vulkan-only and needed more of the msm UAPI
(see "What the emulation cannot honour" below). Credit for the approach and the
emulation's shape is theirs.

## What it builds

| Artifact | What it is |
| --- | --- |
| `out/libvulkan_freedreno.dylib` | Mesa 23.1.9 Turnip ICD for iOS 16 arm64, ad-hoc signed, install name `/usr/lib/sweetmtl/libvulkan_freedreno.dylib`. Exports exactly `vk_icdGetInstanceProcAddr`, `vk_icdGetPhysicalDeviceProcAddr`, `vk_icdNegotiateLoaderICDInterfaceVersion` |
| `out/vk_compute` | CLI Vulkan-compute test, ad-hoc signed. **Only passes on the phone** |
| `out/libsweetvk.a` | the msm emulation (`sweet_drm.c` + `compat.c`), compiled for iphoneos; force-loaded into the ICD link |

Both binaries are copied to the phone by hand — nothing here touches the
kernel, the boot arguments or the code-signing relaxation.

## Building

Needs Xcode (its iPhoneOS SDK), `meson` and `ninja` on `PATH`. `build.sh`
creates its own python venv in `build/venv` for Mesa's generators.

```sh
sh build.sh                 # everything (~10 min cold, seconds warm)
sh build.sh --cross-only    # just build/ios-cross.txt and out/libsweetvk.a
sh setup-mesa.sh --clean    # throw Mesa's source and build directories away
sh setup-mesa.sh --reconfigure
```

`build.sh` generates `build/ios-cross.txt` from `ios-cross.txt.in` (the SDK
path differs per machine), compiles the emulation into `out/libsweetvk.a`, has
`setup-mesa.sh` fetch/verify/patch/configure Mesa 23.1.9, ninja-builds the ICD,
names and signs it, checks its exports and its unresolved symbols, and then
builds the test. See the file for the step-by-step comments.

`.github/workflows/sweetvk.yml` does the same on `macos-14` and uploads
`out/`, so a phone-side run starts from a checked artifact rather than a build
tree.

## Layout

| File | |
| --- | --- |
| `build.sh` | the whole build; the only file that knows where the SDK is |
| `setup-mesa.sh` | fetch Mesa 23.1.9, apply `mesa.patch`, meson configure |
| `ios-cross.txt.in` | cross-file template (`build.sh` substitutes `@SDK@`, `@CC@`, …) |
| `mesa.patch` | the three things Mesa itself has to say differently on iOS |
| `include/xf86drm.h` | the libdrm surface Turnip uses, answered by `sweet_drm.c` |
| `include/sweet_compat.h` | forced into every Mesa TU: calls the macOS SDK lacks |
| `include/sys/sysinfo.h` | shadow of the Linux header `tu_device.cc` reads |
| `include/sweet_drm.h`, `sweet_drm.c`, `compat.c` | the emulation and the shims |
| `test/vk_compute.c`, `.comp` | the phone test and its GLSL |

`mesa.patch` has three hunks, each with the reasoning in a comment:

1. `src/freedreno/drm/freedreno_bo.c` — `os_mmap`/`os_munmap` on `__APPLE__`
   become `sweet_drm_bo_map()`: there is no DRM to mmap, the kernel already
   mapped the bo, and the "mmap offset" Turnip passes around is its GEM handle.
2. `src/freedreno/vulkan/meson.build` — `-Wl,-undefined,dynamic_lookup` on the
   ICD target only (see below).
3. `src/vulkan/runtime/meson.build` — compile `vk_drm_syncobj.c` for the msm
   KMD even though `dep_libdrm` is not found. The emulation *is* the DRM here,
   and `tu_knl_drm_msm_load()` calls `vk_drm_syncobj_get_type()` while creating
   the device, so this file is not optional.

Everything else was solved with shims rather than more Mesa hunks.

## How the ICD is consumed

`tools/sweetmtl`'s bridge (`bridge/src/backend.rs`) `dlopen()`s a bare Mesa ICD
by path and calls `vk_icdGetInstanceProcAddr` itself — no Vulkan loader
involved, which is what iOS allows. `SweetMTLDevice.m` looks for
`/usr/lib/sweetmtl/libvulkan_freedreno.dylib` first and `SWEETMTL_ICD`
overrides it. `out/vk_compute` does the same dance by hand, so an ICD that
loads for the test loads for the bridge.

## The `-undefined dynamic_lookup` caveat

Mesa's generated entrypoint tables hold the addresses of functions declared
`__attribute__((weak))` and *meant* to be absent in some builds:

* `wsi_*` — this ICD has no window system;
* `vk_cmd_enqueue_*` — no layers are linked in;
* `vk_common_*` — the Vulkan runtime's own fallbacks, invoked only when the
  driver does not provide that entry point;
* `tu_*<A6XX>` / `tu_*<(chip)6|7>` — 23.1 declares the per-chip
  specialisations but never instantiates them (no `TU_GENX` call sites in
  23.1.9);
* the `tu_*` entry points 23.1.9 simply does not implement (`tu_QueueSubmit`,
  `tu_CreateFence`, `tu_ResetFences`, `tu_CmdDispatch`, …). On Linux those
  entries are NULL too: the runtime's `vk_common_*` implementation handles the
  entry point, and `tu_device.cc` wires the driver in through
  `queue->vk.driver_submit` (`vk_common_QueueSubmit` in
  `vk_synchronization2.c` does the submit work).

Linux resolves all of those to NULL through the ELF weak-symbol rules. ld64
refuses to write a dylib that has weak references to symbols no library
provides unless `-undefined dynamic_lookup` allows it to leave them to dyld,
which then binds them to 0 — the same result. That flag lives in `mesa.patch`
on the ICD's `link_args` and **must not** move into the cross file: meson hands
cross-file link args to its `has_function()`/`links()` feature probes too, so
with the flag every "is this function there?" probe links and answers *yes*
(that is how `-DHAVE_MEMFD_CREATE` appeared and `anon_file.c` stopped
compiling).

The trade-off is that the flag would equally hide a symbol *we* forgot to
compile in — it would only fail when dyld loads the ICD on the phone. So
`build.sh`'s `check_unresolved()` relinks the same command without the flag and
requires that every reference ld then refuses comes from a generated entrypoint
table (`.._tu_entrypoints.cc.o`, `.._wsi_common_entrypoints.c.o`,
`.._vk_cmd_enqueue_entrypoints.c.o`, `.._vk_common_entrypoints.c.o`). Anything
else fails the build and points at ld's list in
`build/mesa-ios/strict-check.log`. That check is what caught the two real bugs
in this port: a mangled reference to the msm emulation (our shim's declarations
needed `extern "C"` for C++ TUs) and the missing `vk_drm_syncobj.c`.

## How Turnip reaches the A630

Turnip's msm backend (`tu_knl_drm_msm.cc`) only ever calls the libdrm functions
declared in `include/xf86drm.h` plus one mmap hook. `sweet_drm.c` answers them
on the SweetGPU user client (selectors from `SweetGPU.cpp`):

| DRM call | SweetGPU |
| --- | --- |
| `GEM_NEW`/`GEM_CLOSE` | sel 10/11: wired buffer, kernel-chosen GPU VA, the kernel's CPU mapping |
| `MSM_INFO_GET_OFFSET` | `handle << 32`; `sweet_drm_bo_map()` turns it back into the kernel's mapping |
| `MSM_INFO_GET_IOVA` | the kernel's GPU VA (23.1 softpins: `has_set_iova` is hardcoded false) |
| `MSM_GEM_SUBMIT` | sel 13: `MSM_SUBMIT_CMD_BUF` commands become IBs at `bo VA + offset`; the kernel runs them in order and flushes the dcache over every listed bo |
| `WAIT_FENCE`, `CPU_PREP` | sel 14 (the kernel's fence page), then sel 16, which drops the CPU's cached lines |
| `GET_PARAM` | sel 15 (chip id, GMEM size/base, max frequency) and constants |
| syncobjs | emulated here; see below |

Turnip's memory types come back as `DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT`
(`MSM_BO_WC`) and `HOST_CACHED`, and `vk_compute` invalidates after the fence
as well, so it stays correct whether or not the kernel's flush already covered
the CPU.

## What the emulation cannot honour

These are the places where an emulated kernel is not the real one. None of them
is on the path Turnip 23.1 takes for the test (bold items are ones it *would*
take on the phone if a later change starts to use them):

* **`MSM_PARAM_FAULTS` / `MSM_PARAM_SUSPENDS` return 0** — no fault or suspend
  counters; dmesg-style GPU diagnostics stay empty.
* **`MSM_PARAM_TIMESTAMP`** comes from `mach_absolute_time()`. The conversion
  is a guess, so anything timing-sensitive (autotuning) is not trustworthy.
* `MSM_PARAM_PRIORITIES` returns 1: no submitqueue priorities.
* `MSM_BO_CACHED_COHERENT` is refused (`-EINVAL`, logged): it arrived in UAPI
  minor 8, and this emulates minor 7. 23.1 does not ask.
* `MSM_INFO_SET_IOVA` is refused (`-EINVAL`, logged): the kernel picks IOVAs.
* `MSM_INFO_SET_NAME` is ignored (nowhere to keep it); debug output only.
* **Fence fds** (`MSM_SUBMIT_FENCE_FD_IN/OUT`) are refused, so no dma-buf or
  sync-file sharing: `drmPrimeHandleToFD`/`drmPrimeFDToHandle` return -1 and
  `drmSyncobjHandleToFD`/`FDToHandle`/`ImportSyncFile`/`ExportSyncFile`/
  `TimelineSignal`/`TimelineWait` return `-ENOSYS`. `DRM_CAP_SYNCOBJ_TIMELINE`
  answers `-EINVAL` so Turnip takes the *binary* syncobj path — which this does
  support: a submitting call that signals a syncobj remembers its fence seqno,
  and in-syncobjs are waited on (one cap, 10 s) before the submit.
* **Submits are chunked** at `SWEET_GPU_MAX_IBS` (256) IBs and 16384 bos; a big
  submit becomes several kernel submits in order.
* Only `MSM_SUBMIT_CMD_BUF` has real semantics. `IB_TARGET_BUF` is treated as
  an IB and `CTX_RESTORE_BUF` is skipped with a log.
* One device per process; the "fd" is a `dup()` of `/dev/null` (Mesa dup()s and
  closes it), and `drmOpenWithType()` returns one. No DRM master, no
  authentication, no PRIME.
* The VA window is the kernel client's (`0x200000000`, 8 GiB). Turnip 23.1 does
  not ask for a window and does not set IOVAs.

The emulation logs what it refuses to stderr with a `sweetvk: ` prefix — on the
phone that is the console, and it is the first place to look when something the
real kernel answers shows up as a failure.

`NOTES.md` lists the places where the kernel's `sweet_gpu.h` gives less than a
real msm DRM would (fault counts, cache-coherent bos, fence fds, the extra
`MSM_PARAM_*` values, …), i.e. what to add to the user client if a later
Turnip needs it.

## Testing on the phone

```sh
# stage the driver where SweetMTLDevice.m looks for it, then:
/usr/lib/sweetmtl/vk_compute                          # default path
/usr/lib/sweetmtl/vk_compute /path/to/libvulkan_freedreno.dylib
```

It opens the dylib with `dlopen()` (no loader), gets
`vk_icdGetInstanceProcAddr`, makes an instance and a device, refuses anything
that does not name itself `Turnip Adreno (TM) 630`, runs one 64-thread
storage-buffer dispatch (`out.x[i] = in.x[i] * 2 + 1`) and checks the result
after a fence wait and a `vkInvalidateMappedMemoryRanges`. Every step prints a
line, so the console says where it stopped.

On the Mac only the code path *building* can be checked — the ICD's only
transport is the SweetGPU kernel, and a macOS `dlopen()` of this dylib fails on
its `platform IOS` build version, which is itself a useful signal that the
artifact is an iOS one.
