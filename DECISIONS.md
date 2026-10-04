# Core decisions

The locked choices behind this repository, with their rationale.
[AGENTS.md](AGENTS.md) is the working guide; [README.md](README.md) describes
what is implemented.

## Locked decisions

### One core for the family

`calib`, `recon`, `gfx` and `ios` depend on this repository instead of keeping
their own bottom layer. On 2026-10-03 the copies had already drifted:

- `recon` and `gfx` each carried a Vulkan core of about 8k lines with the same
  eleven headers (allocator, buffer, check, descriptor, device, instance, log,
  result, shader, unique_handle, vulkan), but the implementations differed
  throughout: the allocators almost line for line, `device` by about 720
  lines.
- A measured fix stayed in one copy. After an RTX 5090 measurement (one kernel
  went from 14.6 ms to 0.067 ms once its buffers sat in VRAM), `recon` made
  `DeviceLocal` *require* device-local memory; `gfx` still only prefers it.
- The shared-device bootstrap existed twice: `recon`'s
  `examples/viewer/shared_device.hpp` (720 lines) and `ios`'s
  `Bridge/SharedDevice` (756 lines).
- `calib`, `recon` and `gfx` each defined their own `Status`/`Result`.

One copy means a fix lands once, and the zero-copy handoff between libraries
on one `VkDevice` passes one shared buffer type rather than raw handles
described by hand.

### Tiers

One repository, one CMake target per tier, so a consumer links only what it
uses (`gfx` never pulls in a camera model or a vendor SDK):

| Tier | Depends on | Holds | Lands |
| --- | --- | --- | --- |
| `base` | — | `Status`/`Result`, `VKC_CHECK`, logging, version | now |
| `vulkan` | `base` | instance, device create/adopt, allocator, buffers, images, descriptors, shaders, compute pipelines, command batches, external memory, the shared-device bootstrap | in stages from 2026-10-03 (below), before `calib` writes GPU code |
| `camera` | `base` | camera models, the rig calibration file | with `calib`'s rational model |
| `sensor` | `camera`, `vulkan` | frame types, the capture interface, vendor drivers (Orbbec, behind an option) | after `camera` |

- `vulkan` is seeded from `recon`'s core, which carries the measured memory
  rules and the compute pieces `calib` also needs, then gains what `gfx`
  needs. `gfx`'s graphics-only parts (swapchain, render targets, graphics
  pipelines) stay in `gfx`; consumers migrate in the order `recon`, `gfx`
  (re-measuring its frame times, since its allocator semantics change), `ios`
  (deleting its `SharedDevice` copy).
- `camera` starts with the rational model (k1–k6, p1, p2), which holds the
  Orbbec Femto Mega's factory lens exactly and is what `recon`'s calibration
  file stores. Fisheye (Kannala-Brandt) is planned, not built: the interface
  returns unit 3D rays so it can be added without a break.
- `sensor` moves `recon`'s sensor tier here, so `calib` and `recon` share one
  driver per device.

### The vulkan tier

It lands in five stages, each its own PR, merged before any sibling migrates:

| Stage | Holds |
| --- | --- |
| V1 (landed) | `vulkan.hpp`, the `VkResult` helpers, `UniqueHandle`, `PhysicalDeviceInfo`, `DeviceRequirements` with `merge` and `check_device_support`, `Instance`, `Device` |
| V2 (landed) | `Allocator`, `Buffer`, `Image`, descriptor layouts, pools and sets, `ShaderModule`, fences and semaphores, command pools and buffers |
| V3 (landed) | `ComputePipeline`, `ComputeKernel`, `KernelSetBuilder`, `KernelSets`, `dispatch`, `CommandBatch`, the compute helpers, and the shader build functions |
| V4 | query pools, `GpuTimer`, `StageMetrics` (to `base`), external memory |
| V5 | the shared-device bootstrap, replacing recon's example copy and ios's `SharedDevice` |

V1's choices, from comparing the two cores on 2026-10-03:

- **One `DeviceRequirements` replaces both `DeviceConfig`s.** Each library
  states what it needs -- recon a compute queue, timeline semaphores and
  scalar block layout; gfx a graphics queue, Vulkan 1.3, dynamic rendering and
  presentation -- and the same struct drives selection, `Device::create` and
  `Device::adopt`. `merge` combines two libraries' into the one a shared
  device must meet. Names are owned strings (gfx's borrowed pointers could
  dangle); `optional_extensions` replaces recon's `external_memory` /
  `metal_objects` flags and gfx's `needs_external_memory`, and
  `Device::extension_enabled` reports what was enabled.
- **One check.** `check_device_support` -- API version, queue family, present
  family, extensions, core features, the timeline / scalar / dynamic-rendering
  features -- is what selection and create run, and adopt runs its
  device-level half, so the three cannot drift as the copies did.
  `select_physical_device` returns the `PhysicalDeviceInfo` the check read,
  and `Device::create` takes it, so a device is queried once.
- **Generic queue accessors.** `queue()`, `queue_family()`, `queue_flags()`,
  `timestamp_valid_bits()`, plus an optional present queue, replace recon's
  `compute_*` and gfx's `graphics_*`. The present queue is the primary queue
  when its family can present.
- **recon's submit model.** Each submit records on a command pool of its own,
  so submission is thread-safe, and keeps its fence, as creating one cost an
  RTX 5090 about 0.3 ms. gfx's single shared `command_pool()` goes; its
  `submit_and_wait`, `queue_present` and `wait_idle` stay, on the same kept
  fences. `submit_mutex()` is never null (recon's): a device's own mutex
  guards an unshared queue, so several threads may submit through one
  `Device`. A moved-to device locks its own mutex, so gfx's cached
  `submit_mutex()` pointer must be re-read after a move. `submit_and_wait`
  records nothing, so it takes a kept fence without making a command
  buffer. A submit whose wait failed and is still running when the device is
  destroyed leaks an owned `VkDevice`: destroying a device under running
  work, with that work's fence and pool alive, is undefined.
- **gfx's adopt check.** A requirement must be supported by the physical
  device *and* declared enabled by the creator; recon trusted the
  declaration alone. A distinct present queue gets its own mutex
  (`AdoptedDevice::present_mutex`), as ios's bootstrap hands out one per
  queue.
- **The instance asks for 1.3, or the loader's lower version** (gfx's), never
  below 1.1. MoltenVK caps every device's reported version at the instance's
  request, so recon's 1.2 request would hide a 1.3 device from gfx.
- **A device's usable version is the lower of its own and its
  instance's** -- the spec's rule: a 1.3 device on a 1.2 instance may use
  only 1.2. `PhysicalDeviceInfo::api_version()` reports that version and its
  feature queries stop there, so selection, create and adopt all hold
  requirements to it, and dynamic rendering, 1.3 core, cannot be enabled on
  1.2. Vulkan cannot be asked an instance's version, so adopt's caller
  declares it (`AdoptedDevice::instance_api_version`); a 1.0 instance is
  queried through 1.0 calls only.
- **Validation messages reach the log sink with source `"vulkan"`.** A layer
  that is missing, or found but fails to load, is a warning, not a failure,
  and `Instance::validation_logged()` says whether its messages reach the
  sink.
- **`UniqueHandle` binds its deleter by reference** (`auto& Destroy`). The
  same `UniqueHandle<VkFence, vkDestroyFence>` names the link-time loader's
  function today, and the global variable volk loads the pointer into
  later, so the planned switch to volk for iOS and Android stays inside
  `vulkan.hpp`. A runtime deleter would add storage to every handle, and
  traits keyed on the handle type cannot tell `VkFence` from `VkSemaphore` on
  32-bit targets, where both are `uint64_t`.
- **Vulkan from the system** (`find_package(Vulkan)`, `Vulkan::Vulkan`
  PUBLIC), as recon and ios use it; gfx compiles against pinned
  Vulkan-Headers instead (an open decision below).
- **`VKC_WITH_VULKAN` is opt-in for a subproject.** ON at the top level, OFF
  when fetched: recon and gfx set it, and calib, which fetches the core for
  its error types, then needs no Vulkan installed. An installed core ships the
  tier's headers only with its library, and re-finds Vulkan only then.
- **CI without GPUs, but not without devices.** The hosted Linux legs run the
  device tests on lavapipe with `VKC_REQUIRE_VULKAN_DEVICE=1`, so a missing
  device fails instead of skipping; the sanitizer job adds the validation
  layer (`VKC_TEST_VALIDATION=1`), so ASan, UBSan, LSan and validation check
  the same run. Under it, a test fails if validation is off or does not reach
  the log sink, and counts errors the layer reports at `vkDestroyInstance`.

V2's choices, from the same comparison:

- **recon's memory rules.** `MemoryUsage::DeviceLocal` *requires*
  `DEVICE_LOCAL`, so an allocation fails rather than spill bulk kernel data
  into host memory across PCIe (recon measured a TSDF kernel at 14.6 ms there
  against 0.067 ms in VRAM); gfx only preferred it. A host-visible buffer is
  mapped persistently and coherent, so `mapped()` is a plain pointer. A
  device-address usage is refused, as recon's `MarchingCubes` refused it:
  VMA aborts on one unless its allocator enables buffer device addresses,
  which needs a feature `DeviceRequirements` cannot ask for yet.
- **Sharing from the queue families a resource names.** Two or more distinct
  families give `CONCURRENT`, one or none `EXCLUSIVE`; duplicates count once,
  and a family the device lacks is refused. recon's rule, now for images too:
  gfx's were always exclusive, which is undefined when another family -- on
  Apple, a compute library's -- reads them. `BufferDesc::kMaxQueueFamilies`
  and `check_queue_family_count` stay for recon's configs.
- **Resources outlive their allocator safely.** Each buffer and image holds a
  reference to the VMA state, freed with the last of them (recon's); a
  documented destruction order cannot express `a = std::move(b)`.
- **One allocator per library per device**, separate from `Device`: VMA
  allocators are independent bookkeeping over one `VkDevice`, so a library
  sharing an adopted device still reports only its own memory.
- **One `Image` type** replaces gfx's `Texture` and recon's adopted `Image`:
  made by `Allocator::create_image` with gfx's validation (3D, arrays, cubes,
  mips, multisampling, a default view whose type and aspect follow the image),
  or adopted from an `ImageInfo` and a deleter. `handle()`, not gfx's
  `image()`, as every other wrapper names it. Images are device-local only,
  `MemoryUsage::Auto` included: they have no host accessor, so a host-visible
  one would be memory the host cannot use. recon's `BufferMemoryInfo` becomes
  `MemoryInfo`, shared by both. `ImageInfo` records what Vulkan cannot be
  asked afterwards -- type, samples, create flags and tiling with the rest --
  so a library handed a borrowed image can tell a cube from a six-layer array
  or a multisampled image from a single-sample one. It also records the
  layout the contents are in, which the owner updates with `set_layout` after
  each transition it submits: Vulkan cannot be asked that either, and a copy
  recorded against a stale layout is invalid. The default view is refused
  only where it cannot be right: a transfer-only usage, or a multi-planar or
  4:2:2 format, whose view needs a sampler Y'CbCr conversion.
- **VMA v3.4.0, private**, under the FetchContent name recon and gfx use, so
  one build resolves one copy. Static Vulkan functions against the linked
  loader, and Vulkan 1.1 as VMA's ceiling, as both did. It never reaches a
  public header. Its implementation is compiled into the allocator's own
  object, so a static link that also pulls in another VMA implementation --
  recon's or gfx's own, until each moves to this allocator and drops it --
  fails on duplicate symbols, instead of running the allocator on a copy
  built against other Vulkan headers. A shared core hides VMA's symbols.
- **Descriptor sets write all four kinds** the family uses: storage buffers
  (recon), uniform buffers and combined image samplers (gfx), and storage
  images, new, for compute kernels that write images. Copies share a write
  count (recon's), so a recorded batch can refuse a set rewritten through an
  alias.
- **`ShaderModule` without reflection** (recon's): pipelines declare their
  layouts, so the core needs no SPIR-V parser. gfx keeps its spirv-cross
  reflection over this module.
- **gfx's fences, semaphores, command pools and command buffers**, with one
  change: an empty fence or timeline semaphore returns `InvalidArgument`
  instead of passing a null device to Vulkan.

V3's choices, from the same comparison (gfx has no compute):

- **recon's compute, under its own signatures.** `ComputePipeline`,
  `ComputeKernel`, `KernelSetBuilder`, `KernelSets`, `dispatch`,
  `CommandBatch` and the `compute_util` helpers keep recon's names and
  parameters -- the explicit `max_groups` included, which recon's callers also
  use to split oversized grids -- so recon migrates by namespace. `dispatch`
  takes a `const Device&`, and a builder of binding-less kernels sizes a pool
  of one descriptor instead of the invalid zero.
- **One path between host and kernel memory.** `CommandBatch` is it: uploads
  inline up to 64 KiB and staged beyond, readbacks through one host buffer,
  barriers only where a command could see another's writes, a refusal that
  poisons the batch, and a descriptor set rewritten after its dispatch
  refused at submit. Kernel memory is never mapped, so unified memory runs the
  path a discrete GPU does (the open "Unified memory" question below).
- **Image copies take the common uncompressed color formats**, 8- to 128-bit
  texels, where recon's took `R8` and `R8G8`. They read the layout
  `Image::layout` records (V2's `set_layout` keeps it current) and refuse a
  multisampled image by `Image::samples`.
- **`submit_single_time` reports work left in flight**, so a batch whose wait
  failed leaks its staging rather than free memory the GPU may still read.
- **Timer spans come with V4.** recon's optional `GpuStageScope*` parameters
  return then as trailing defaults, beside `GpuTimer`.
- **One shader toolchain.** `vkc_compile_shaders` and `vkc_embed_shaders`
  replace recon's `vr_*`, gfx's `vg_*`, and the copy of recon's ios borrows.
  `TARGET_ENV` (default Vulkan 1.2; gfx's renderer passes 1.3),
  `INCLUDE_DIRS` (recon's include root) and `SPIRV_VAL_ARGS` (recon's
  `--scalar-block-layout`) cover the differences. `SYMBOL_PREFIX` is
  required: the arrays are inline variables, which the linker merges by name,
  so two libraries' `fill.comp` would otherwise share one shader. The
  functions are defined when the core is added, and installed beside the
  package config, which includes them.

### Naming

- Repository and package `volumetric_kit_core`; namespace
  `volumetric_kit::core`. Write `namespace vkc = volumetric_kit::core;` for a
  short alias -- never `vk`, which Vulkan's C++ bindings own.
- Macros use the `VKC_` prefix (`VKC_TRY`, `VKC_ASSIGN`, `VKC_CHECK`,
  `VKC_BASE_API`); Vulkan owns `VK_`.
- Targets are `volumetric_kit::core_<tier>`, plus the umbrella
  `volumetric_kit::core`. Library files are prefixed
  (`libvolumetric_kit_core_base`), since they land in shared lib directories.

### Merging the three `Status`/`Result` types

The base tier is the union of `calib`'s, `recon`'s and `gfx`'s:

- **Codes:** `Ok`, `InvalidArgument`, `NotFound`, `Unsupported`, `OutOfMemory`,
  `IoError` (all three), `Numerical` (`calib`'s solver failures), and `Backend`
  (`recon`'s).
- **Backend detail is a neutral `int64_t`** (`recon`'s design). `gfx`'s
  `Vulkan` domain with a `VkResult`-typed `code()` becomes `Backend` with the
  `VkResult` in `detail()`; the vulkan tier supplies `vk_error`, `VKC_VK_TRY`
  and the `VkResult` name lookup. The base tier includes no GPU API.
  `backend_error` keeps `gfx`'s guard against a success code: a detail of `0`
  (`VK_SUCCESS`, `cudaSuccess`) aborts via `VKC_CHECK`.
- **`with_context` adds context without losing the domain** (new). `recon`
  rebuilt a `Status` with a switch over every code to prefix a kernel's name;
  each such switch breaks when a code is added, as `Numerical` now is.
- **`[[nodiscard]]` on both types** (`calib`'s). With no exceptions, a dropped
  `Status` is a silently lost failure. `recon` and `gfx` lacked it, so their
  migrations will surface call sites that drop one; each is a lost failure to
  handle or to discard explicitly with `(void)`.
- **The converting constructor** (`calib`'s `Result(U&&)`), so
  `return "name";` builds a `Result<std::string>` and `return std::nullopt;` a
  `Result<std::optional<U>>`. `recon`'s `static_assert` keeps
  `Result<Status>` ill-formed. It refuses two conversions that compile but are
  almost always bugs: a pointer to `bool` (`return "missing";` from a
  `Result<bool>` function would succeed with `true`) and a null pointer to a
  string (`std::string(nullptr)` is undefined behaviour).
- **`operator*`** (`calib` and `gfx`; `recon` had omitted it).
- **The rvalue accessors return `T` by value**, unlike `std::optional` and
  all three copies, which returned `T&&`. A reference into a temporary
  `Result` dangles once the full expression ends (`for (auto& p :
  load().value())` before C++23); the price is one move.
- **One discriminator.** A `Result` is OK exactly when it holds a value, and
  `ok()` reads nothing else; its `Status` is set only by the failure
  constructor, which checks it is non-OK. It is not a
  `std::variant<T, Status>`: GCC 13 at `-O2` reports a false
  `-Wmaybe-uninitialized` in that variant's destructor, which a consumer
  building with `-Werror` would inherit.
- **`VKC_CHECK` logs through the sink, then aborts** (`recon` and `gfx`), so an
  application that routes logs to a crash reporter sees why it stopped.
  `calib`'s check printed to stderr directly. A check that fails inside the
  handler (or while the handler reports an earlier failure) writes to stderr
  instead, so a broken handler cannot recurse until the stack overflows.
  Misusing a `Result` names the held error, and `value()` its caller's line.
- **One log handler, with a source.** `recon` and `gfx` each had a handler,
  so an application knew which library a message came from, and their default
  sinks printed `[vr <level>]` and `[vg <level>]`. The one handler now takes
  the emitting library as `source`, and the default sink prints
  `[<source> <level>]`, so passing `vr` or `vg` keeps the old prefixes.
- **`set_log_handler` waits for the previous handler.** It returns only once
  no other thread is still running the old one, so an application may destroy
  what the handler referenced -- a logger, say -- right after replacing it.
  Every call shares one handler object (a stateful handler keeps its state),
  and the old handler is destroyed without the lock held (its destructor may
  log).

### One instance per process

The log handler is process-global state, so a process must hold one copy of
`core_base`. Every binary -- executable, shared library or dynamic framework
-- that links a static `core_base` carries its own copy, with its own
handler, so a static core is safe only when a single binary in the process
links it. Once a shared library or framework links the core (a sibling's
`.so`, or `ios` linking `recon` and `gfx` as frameworks), build the core
shared as well (`BUILD_SHARED_LIBS=ON`), so every binary resolves the one
`libvolumetric_kit_core_base`.

Configuring a build in which a shared or module library links a static
`core_base` prints a warning (`cmake/vkc_single_instance.cmake`). A binary
built outside CMake, such as an Xcode framework target, is beyond that check.

### Consumers pin, and an application declares the core first

Siblings pin a tag or commit SHA of this repository, never `main`. An
application that fetches several siblings (`ios`) declares
`volumetric_kit_core` first, so FetchContent resolves one copy for all of
them. A public API change here lands with a CHANGELOG entry that says how to
migrate.

Tests and `-Werror` default ON only at the top level, but install rules
(`VKC_INSTALL`) default ON everywhere: a sibling that installs and exports its
own targets links the core PUBLIC, so CMake refuses to generate its export
unless the core's targets are in an export set too, and its installed package
needs the core installed beside it. The shared library's soname carries
`MAJOR.MINOR` until 1.0, matching the package's same-minor compatibility.

### Public, with hosted CI first

The repository is public because `recon`, `gfx` and `ios` are public and will
fetch it at configure time. The base tier is pure C++17 with no
dependencies, so GitHub-hosted Linux and macOS runners cover it, including a
`-fno-exceptions` leg, a shared-library leg, sanitizers, and both ways of
consuming the package.

## Open decisions

- **GPU CI for the vulkan tier.** lavapipe covers correctness on hosted
  runners, but no real GPU runs here yet. `recon`'s GPU runners
  (`vk-linux-gpu`, `mac`) are registered per repository, so they must be
  registered here too. They were to come before the allocator (V2), whose
  memory placement differs on a discrete GPU; V2 landed without them, so the
  discrete-GPU path -- `DeviceLocal` in VRAM, apart from host-visible memory
  -- is still untested here. On a public repository those legs must run only
  same-repository code, as `recon`'s guard does.
- **Vulkan headers for gfx.** The tier uses the system's headers; gfx pins
  Vulkan-Headers 1.4.357 and links the loader privately. Both in one build
  would mix two header versions. Decide at gfx's migration: gfx adopts the
  system headers, or the core vendors the same pin.
- **Unified memory.** `recon` deliberately runs the staged path on Apple too,
  pending a staging measurement on the iPad. The vulkan tier inherits that
  rule until the measurement says otherwise.
