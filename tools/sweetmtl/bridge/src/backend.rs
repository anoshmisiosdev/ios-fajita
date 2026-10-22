//! The Vulkan side of the bridge: opening a bare Mesa ICD by path, making an instance and a
//! logical device through it, and adopting that device with `metal2vulkan`.
//!
//! # Why the bridge makes the device itself
//!
//! `metal2vulkan`'s own device creation opens a **Vulkan loader** (`ash::Entry::load_from`, then
//! `vkGetInstanceProcAddr`) and reads the ICDs the loader enumerates from the registry. Neither
//! holds here: a Mesa ICD exports `vk_icdGetInstanceProcAddr` and its loader interface, not
//! `vkGetInstanceProcAddr`, and on iOS there is no loader library at all — the driver ships
//! Turnip (`libvulkan_freedreno.dylib`) and calls it directly, which is what the kernel's
//! SweetGPU client is for.
//!
//! So the bridge takes the other path the runtime documents: it opens the ICD itself, makes the
//! instance and the `VkDevice` through the ICD's `vk_icdGetInstanceProcAddr`, and hands both to
//! [`Device::from_raw`](metal2vulkan_runtime::Device::from_raw) (a
//! [`RawDevice`](metal2vulkan_runtime::RawDevice)), which adopts them. The instance and the
//! `VkDevice` stay the bridge's, which destroys them when the device handle is released.
//!
//! # What the bridge enables
//!
//! A description read from a physical device states what it **supports**; the consumer enables at
//! least `recommended_enables` and reports what it enabled. This bridge asks the device itself —
//! one `vkGetPhysicalDeviceFeatures2` chaining `VkPhysicalDeviceVulkan11Features` and
//! `VkPhysicalDeviceVulkan12Features` — and enables **exactly the members that query reports as
//! supported**. That is the most a create info can ask for, and it is not optional: Mesa checks a
//! create info's features against the device's own (its check, in `vkCreateDevice`, not the
//! loader's) and refuses an unsupported member with `VK_ERROR_FEATURE_NOT_PRESENT`.
//!
//! The bridge reports to the runtime exactly what the runtime recommends of those: the core
//! features of `recommended_enables` and whatever the policy keeps of them (`[`core_enables`]`).
//! The two differ only where a recommended feature is a *removal* rather than a requirement — a
//! policy that turns `robustBufferAccess` off as containment — or where a feature is an extension
//! feature, and this milestone enables no extension feature (see the crate README). Enabling a
//! member the runtime was not told about is inert: it uses no capability the reported set lacks.
//!
//! Asking the device is also what keeps the bridge from carrying a second copy of `metal2vulkan`'s
//! own feature-to-structure-member table (`crates/metal2vulkan-runtime/src/enables.rs`), which a
//! later milestone needs to enable extension features too.
//!
//! # What is not checked here
//!
//! A create info this bridge makes goes straight to the ICD: no Vulkan loader sees it and no
//! validation layer can be enabled (there is no loader to load one), so the cross-feature rules a
//! layer enforces (`VUID-VkDeviceCreateInfo-robustBufferAccess-10247` and its like) are not applied
//! and nothing here depends on them. Mesa's own check is about support alone, and it is.

use ash::vk;
use metal2vulkan::device::{
    ApiVersion, DeviceDescription, DevicePolicy, EnableSet, Exposure, VkExtension,
};
use metal2vulkan_runtime::{Device, MetalLevel, RawDevice, ShaderCacheOptions};
use std::ffi::{c_char, c_void, CString};
use std::mem::ManuallyDrop;

/// The Vulkan API version the bridge asks the ICD for.
///
/// Vulkan 1.2 is `metal2vulkan`'s floor (`Device::new` refuses a device below it), and the
/// runtime needs `timelineSemaphore` and `hostQueryReset`, both core in 1.2. An ICD that cannot
/// give a 1.2 instance is refused with the ICD's own error.
const API_VERSION: ApiVersion = ApiVersion::V1_2;

/// An open ICD: the `dlopen` handle and the instance entry point a bare Mesa ICD exports.
struct Icd {
    handle: *mut c_void,
    get_instance_proc_addr: vk::PFN_vkGetInstanceProcAddr,
}

impl Icd {
    /// Opens the library at `path` and resolves its instance entry point.
    ///
    /// `vk_icdGetInstanceProcAddr` is what a bare Mesa ICD exports (the loader-ICD interface);
    /// `vkGetInstanceProcAddr` is what a real Vulkan loader exports. Both answer the same
    /// questions about an instance, so either serves.
    fn open(path: &str) -> Result<Icd, String> {
        let c_path = CString::new(path).map_err(|_| format!("the ICD path {path:?} holds a NUL"))?;
        // SAFETY: `c_path` is a valid NUL-terminated path; `dlopen` runs the library's
        // initializers, which is what opening a driver does.
        let handle = unsafe { libc::dlopen(c_path.as_ptr(), libc::RTLD_NOW | libc::RTLD_LOCAL) };
        if handle.is_null() {
            return Err(format!("cannot open the ICD {path}: {}", dlerror_text()));
        }
        // SAFETY: `handle` is a live handle `dlopen` returned.
        let mut entry = unsafe { libc::dlsym(handle, c"vk_icdGetInstanceProcAddr".as_ptr()) };
        if entry.is_null() {
            // SAFETY: as above.
            entry = unsafe { libc::dlsym(handle, c"vkGetInstanceProcAddr".as_ptr()) };
        }
        if entry.is_null() {
            // SAFETY: the handle was opened here and is not used again.
            unsafe { libc::dlclose(handle) };
            return Err(format!(
                "the library {path} exports neither vk_icdGetInstanceProcAddr (a bare Mesa ICD) \
                 nor vkGetInstanceProcAddr (a Vulkan loader)"
            ));
        }
        // SAFETY: the symbol is one of the two function pointers named above, which have the
        // same signature.
        let get_instance_proc_addr: vk::PFN_vkGetInstanceProcAddr =
            unsafe { std::mem::transmute(entry) };
        Ok(Icd {
            handle,
            get_instance_proc_addr,
        })
    }
}

impl Drop for Icd {
    fn drop(&mut self) {
        // SAFETY: the handle was opened by `Icd::open` and is closed once. Everything made
        // through it (the instance, the device, their dispatch tables) is gone by now: the
        // fields that own them drop first.
        unsafe { libc::dlclose(self.handle) };
    }
}

/// The Vulkan objects the bridge made and must destroy, in the order Vulkan wants. Its `device`
/// is `None` until the logical device exists, so the instance is destroyed whatever fails in
/// between.
struct Vulkan {
    instance: ash::Instance,
    device: Option<ash::Device>,
}

impl Vulkan {
    /// The `VkInstance` it holds.
    fn raw_instance(&self) -> vk::Instance {
        self.instance.handle()
    }

    /// The `VkDevice` it holds.
    fn raw_device(&self) -> vk::Device {
        match &self.device {
            Some(device) => device.handle(),
            None => vk::Device::null(),
        }
    }
}

impl Drop for Vulkan {
    fn drop(&mut self) {
        // SAFETY: both handles are this struct's, were made here, and are destroyed once.
        unsafe {
            if let Some(device) = &self.device {
                device.destroy_device(None);
            }
            self.instance.destroy_instance(None);
        }
    }
}

/// The device [`sweetmtl_device_new`](crate::sweetmtl_device_new) made: the adopted runtime
/// device, the Vulkan objects it runs on, and the ICD that holds both.
pub struct DeviceState {
    /// The device's name, as C sees it (`MTLDevice.name`).
    name: CString,
    /// The runtime's device. Dropped by hand, before `vulkan`: every object the runtime owns is
    /// made from that `VkDevice`.
    runtime: ManuallyDrop<Device>,
    /// The instance and device that outlive the runtime's. Never read, only dropped (after
    /// `runtime` and before `icd`).
    #[allow(dead_code)]
    vulkan: Vulkan,
    /// Holding the ICD open: the `VkDevice`'s dispatch table lives in it. Never read, only
    /// dropped (after `vulkan`).
    #[allow(dead_code)]
    icd: Icd,
}

impl DeviceState {
    /// The adopted runtime device.
    pub fn device(&self) -> &Device {
        &self.runtime
    }

    /// The device's name for C.
    pub fn name_ptr(&self) -> *const c_char {
        self.name.as_ptr()
    }
}

impl Drop for DeviceState {
    fn drop(&mut self) {
        // SAFETY: `runtime` is a `ManuallyDrop` no one else has dropped. Its objects (buffers,
        // libraries, pipelines, queues and command buffers) hold clones of it, so the caller
        // releases them first, as it would with Metal.
        unsafe { ManuallyDrop::drop(&mut self.runtime) };
    }
}

/// Opens `icd_path`, makes a Vulkan 1.2 instance and a logical device with every core feature the
/// device reports, and adopts both with `metal2vulkan`.
///
/// `device_wanted`, where it names something, picks the physical device (an index, or a substring
/// of its name; the first device with a graphics and compute queue family otherwise).
pub fn open(
    icd_path: &str,
    cache_directory: Option<&str>,
    device_wanted: Option<&str>,
) -> Result<DeviceState, String> {
    let icd = Icd::open(icd_path)?;
    // SAFETY: `icd.get_instance_proc_addr` is the ICD's own entry point, alive with `icd`.
    let entry = unsafe {
        ash::Entry::from_static_fn(ash::StaticFn {
            get_instance_proc_addr: icd.get_instance_proc_addr,
        })
    };
    let instance = create_instance(&entry)?;
    let mut vulkan = Vulkan {
        instance,
        device: None,
    };
    let (physical, family) = choose_physical(&vulkan.instance, device_wanted)?;
    // SAFETY: `physical` was enumerated from this instance, which was made at `API_VERSION`.
    let description = unsafe { DeviceDescription::query(&vulkan.instance, physical, API_VERSION) }
        .map_err(|error| format!("cannot read the physical device's description: {error}"))?;
    let policy = DevicePolicy::default();
    let enabled = core_enables(&description, &description.recommended_enables(&policy));
    let device = create_device(&vulkan.instance, physical, family, &enabled)?;
    vulkan.device = Some(device);

    // What the runtime is told the consumer enabled. The extensions of `enabled` are the
    // vocabulary's names for them: the portability subset is the only one this milestone can
    // enable.
    let device_extensions: Vec<CString> = enabled
        .extensions
        .iter()
        .map(|&extension| CString::new(extension_name(extension)).unwrap_or_default())
        .filter(|name| !name.as_bytes().is_empty())
        .collect();
    let mut raw = RawDevice::new(
        icd.get_instance_proc_addr,
        vulkan.raw_instance(),
        API_VERSION,
        physical,
        vulkan.raw_device(),
        family,
        enabled,
    );
    raw.queue_index = 0;
    raw.device_extensions = device_extensions;
    raw.shader_cache = match cache_directory {
        Some(directory) => ShaderCacheOptions::in_directory(directory),
        None => ShaderCacheOptions::default(),
    };
    // Answer as the newest Metal on every host. Without this the runtime reads the *host's*
    // macOS release, which an iOS host is not a release of: the adoption would be refused
    // (`Unproven`, `HostReleaseUnmeasured`).
    raw.metal_level = Some(MetalLevel::NEWEST);

    // SAFETY: the instance was made through `icd.get_instance_proc_addr` at `API_VERSION`,
    // `physical` was enumerated from it, and the device was made on `physical` with exactly
    // `enabled` enabled (the same set is handed to the runtime), its queue 0 of family `family`,
    // and `timelineSemaphore` and `hostQueryReset` beside it (the create info asks for every core
    // feature the device reports, and `create_device` refuses a device without those two). All of
    // them outlive the runtime device: they are moved into the `DeviceState` this returns, whose
    // `Drop` runs the runtime's first.
    let runtime = unsafe { Device::from_raw(raw, policy) }
        .map_err(|error| format!("the runtime refused the adopted device: {error}"))?;
    let name = CString::new(runtime.name()).map_err(|_| "the device name holds a NUL".to_owned())?;
    Ok(DeviceState {
        name,
        runtime: ManuallyDrop::new(runtime),
        vulkan,
        icd,
    })
}

/// The runtime's enables restricted to what this bridge enables: the core features the device
/// reports and the policy recommends, plus `VK_KHR_portability_subset` on a device that lists it
/// (Vulkan requires a device that lists it to have it enabled).
///
/// A subset of the recommended set is always a valid one: dropping extension-exposed features
/// breaks none of Vulkan's co-enable rules (`metal2vulkan`'s own recommended set keeps them, and
/// what this drops is the extension features, never a companion another feature needs).
fn core_enables(description: &DeviceDescription, recommended: &EnableSet) -> EnableSet {
    let mut enabled = EnableSet::new();
    for &feature in &recommended.features {
        if matches!(feature.exposure(), Exposure::Core(_)) {
            enabled.features.insert(feature);
        }
    }
    if description
        .extensions
        .contains(&VkExtension::KhrPortabilitySubset)
    {
        enabled.extensions.insert(VkExtension::KhrPortabilitySubset);
    }
    enabled
}

/// The registry name of the one extension this milestone can enable.
fn extension_name(extension: VkExtension) -> &'static str {
    match extension {
        VkExtension::KhrPortabilitySubset => "VK_KHR_portability_subset",
        _ => "",
    }
}

/// Makes the instance through the ICD's entry point.
fn create_instance(entry: &ash::Entry) -> Result<ash::Instance, String> {
    let app_info = vk::ApplicationInfo::default()
        .application_name(c"sweetmtl-bridge")
        .application_version(vk::make_api_version(0, 0, 1, 0))
        .engine_name(c"metal2vulkan")
        .engine_version(vk::make_api_version(0, 0, 1, 0))
        .api_version(API_VERSION.packed());
    let create_info = vk::InstanceCreateInfo::default().application_info(&app_info);
    // SAFETY: the create info and everything it points at live for the call. Ash's `loaded`
    // `Entry` answers with the loaded instance already (`Entry::create_instance`).
    let instance = match unsafe { entry.create_instance(&create_info, None) } {
        Ok(instance) => instance,
        Err(vk::Result::ERROR_INCOMPATIBLE_DRIVER) => {
            return Err(
                "the ICD cannot make a Vulkan 1.2 instance (vkCreateInstance said \
                 VK_ERROR_INCOMPATIBLE_DRIVER); metal2vulkan needs Vulkan 1.2"
                    .to_owned(),
            );
        }
        Err(error) => return Err(format!("vkCreateInstance through the ICD failed: {error}")),
    };
    Ok(instance)
}

/// The physical device to use and the queue family of its the runtime submits to.
fn choose_physical(
    instance: &ash::Instance,
    wanted: Option<&str>,
) -> Result<(vk::PhysicalDevice, u32), String> {
    // SAFETY: the instance is live.
    let devices = unsafe { instance.enumerate_physical_devices() }
        .map_err(|error| format!("vkEnumeratePhysicalDevices failed: {error}"))?;
    if devices.is_empty() {
        return Err("the ICD enumerates no physical device".to_owned());
    }
    let needed = vk::QueueFlags::GRAPHICS | vk::QueueFlags::COMPUTE;
    let mut notes: Vec<String> = Vec::new();
    for (index, &device) in devices.iter().enumerate() {
        // SAFETY: the device was enumerated from this instance, and its name is a NUL-terminated
        // array inside its properties.
        let name = unsafe {
            let properties = instance.get_physical_device_properties(device);
            std::ffi::CStr::from_ptr(properties.device_name.as_ptr())
        }
        .to_string_lossy()
        .into_owned();
        if let Some(wanted) = wanted {
            let chosen =
                wanted.parse::<usize>().is_ok_and(|wanted| wanted == index) || name.contains(wanted);
            if !chosen {
                continue;
            }
        }
        // SAFETY: the device was enumerated from this instance.
        let families = unsafe { instance.get_physical_device_queue_family_properties(device) };
        match families
            .iter()
            .position(|family| family.queue_flags.contains(needed) && family.queue_count > 0)
        {
            Some(family) => return Ok((device, family as u32)),
            None => notes.push(format!("#{index} {name}: no graphics and compute queue family")),
        }
    }
    let devices = devices.len();
    Err(match wanted {
        Some(wanted) => format!(
            "no physical device of {devices} matches {wanted:?} with a graphics and compute \
             queue family ({})",
            notes.join("; ")
        ),
        None => format!(
            "none of the {devices} physical devices has a graphics and compute queue family ({})",
            notes.join("; ")
        ),
    })
}

/// Makes the logical device with one queue of `family`, the extensions of `enabled`, and every
/// core feature the device reports (see the module docs).
fn create_device(
    instance: &ash::Instance,
    physical: vk::PhysicalDevice,
    family: u32,
    enabled: &EnableSet,
) -> Result<ash::Device, String> {
    let priorities = [1.0f32];
    let queues = [vk::DeviceQueueCreateInfo::default()
        .queue_family_index(family)
        .queue_priorities(&priorities)];
    let extension_names: Vec<CString> = enabled
        .extensions
        .iter()
        .map(|&extension| CString::new(extension_name(extension)).unwrap_or_default())
        .filter(|name| !name.as_bytes().is_empty())
        .collect();
    let extension_ptrs: Vec<*const c_char> =
        extension_names.iter().map(|name| name.as_ptr()).collect();

    // What the device reports it supports, in the three core structures of `API_VERSION`, which
    // are what the create info enables: the 1.0 members as the legacy `pEnabledFeatures`, the 1.1
    // and 1.2 members as their core structures chained beside it.
    let mut core = vk::PhysicalDeviceFeatures::default();
    let mut v11 = vk::PhysicalDeviceVulkan11Features::default();
    let mut v12 = vk::PhysicalDeviceVulkan12Features::default();
    query_features(instance, physical, &mut core, &mut v11, &mut v12);
    // `RawDevice`'s contract asks for both, and metal2vulkan needs both (core in Vulkan 1.2, which
    // `Device::new` would refuse a device below anyway). Saying so here names the device rather
    // than leaving the adoption to fail on a lost feature later.
    if v12.timeline_semaphore == vk::FALSE || v12.host_query_reset == vk::FALSE {
        return Err(
            "the device supports neither timelineSemaphore nor hostQueryReset, which \
             metal2vulkan's device needs (both core in Vulkan 1.2)"
                .to_owned(),
        );
    }

    // The query chained `v12` and `v11` together and left that link on `v12` (ash's `push_next`
    // walks the pushed structure's *own* chain and rewires its tail). Chaining them again as they
    // are would make the create-info chain circular - `v11.p_next` would be rewired to `v11`
    // itself - and a driver walking the chain to its end would never find it. Break the query's
    // link before reusing the structures, then chain them: the create info holds `v12` and `v11`
    // in turn, each ending in NULL.
    v11.p_next = std::ptr::null_mut();
    v12.p_next = std::ptr::null_mut();

    let create_info = vk::DeviceCreateInfo::default()
        .queue_create_infos(&queues)
        .enabled_extension_names(&extension_ptrs)
        .enabled_features(&core)
        .push_next(&mut v11)
        .push_next(&mut v12);
    // SAFETY: the create info and everything it points at live for the call; `physical` was
    // enumerated from this instance. Ash's `loaded` `Instance` answers with the loaded device
    // already (`Instance::create_device`).
    let device = unsafe { instance.create_device(physical, &create_info, None) }
        .map_err(|error| format!("vkCreateDevice failed: {error}"))?;
    Ok(device)
}

/// Fills `core`, `v11` and `v12` with what the device reports it supports: one
/// `vkGetPhysicalDeviceFeatures2` with the two later core feature structures chained into it.
///
/// The query is Vulkan 1.1's, which the instance of `API_VERSION` has.
fn query_features(
    instance: &ash::Instance,
    physical: vk::PhysicalDevice,
    core: &mut vk::PhysicalDeviceFeatures,
    v11: &mut vk::PhysicalDeviceVulkan11Features<'_>,
    v12: &mut vk::PhysicalDeviceVulkan12Features<'_>,
) {
    let mut features2 = vk::PhysicalDeviceFeatures2::default()
        .push_next(v11)
        .push_next(v12);
    // SAFETY: `physical` was enumerated from `instance`, and the chain lives for the call (the
    // query writes through it and keeps no pointer).
    unsafe { instance.get_physical_device_features2(physical, &mut features2) };
    *core = features2.features;
}

/// `dlerror`'s current message, for an error naming why a library would not open.
fn dlerror_text() -> String {
    // SAFETY: `dlerror` returns NULL or a NUL-terminated message owned by the loader until the
    // next call on this thread.
    let message = unsafe { libc::dlerror() };
    if message.is_null() {
        return "no detail".to_owned();
    }
    // SAFETY: as above.
    unsafe { std::ffi::CStr::from_ptr(message) }
        .to_string_lossy()
        .into_owned()
}
