# Changelog

All notable changes to `volumetric_kit_core` are documented here. The format
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html). Before
1.0, a minor release may break the API; each such entry says how to migrate.

## [Unreleased]

### Fixed

- `vk_result` returns empty for a backend status whose detail is outside
  `int32_t`, `VkResult`'s range, instead of converting it to `VkResult`. That
  conversion was undefined and in practice truncated the detail, so a failure
  whose detail was 2^40 read as `VK_SUCCESS`. A backend status therefore no
  longer always yields a `VkResult`: check the optional before dereferencing
  it. A 32-bit code stored without sign extension (from a `uint32_t`) is
  outside the range too; store a `VkResult` with `vk_error`. `vk_result` still
  cannot tell a CUDA status from a Vulkan one: ask it only of a status from a
  Vulkan call.
- At a heap's budget, buffer and image allocations still take free space in
  the allocator's existing VMA blocks, so a falling budget no longer rejects
  a suballocation that needs no additional device memory. With budget room,
  VMA allocates as before -- honoring the driver's preference for dedicated
  memory and growing the first candidate type before another. Resources that
  require dedicated memory are never suballocated. A budget refusal's message
  now says so instead of naming a VMA call.
- `CommandBatch` makes completed writes visible to graphics shader uniform
  and storage-buffer accesses, as well as vertex/index input. Barrier scopes
  follow the queue's capabilities, and compute dispatches on a queue without
  compute support are refused.
- Replacing the log handler from inside a callback no longer deadlocks when
  another thread finishes a callback on the old handler. Nested callbacks keep
  the same guarantee.
- `Allocator::memory_stats()` documents `usage_bytes` as what it always
  was, the heap's usage paired with `budget_bytes` -- process-wide where
  `VK_EXT_memory_budget` is enabled, not this allocator's share -- and reports
  that share in new fields (below).
- `create_exported_buffer` refuses (`InvalidArgument`) an allocator made for
  another device, before creating anything, instead of binding that device's
  memory to its buffer.
- `find_memory_type` places device-local memory asked for without
  `HOST_VISIBLE` as `MemoryUsage::DeviceOnly` does: a resource limited to a
  discrete GPU's BAR window now gets no type rather than the window. Ask for
  `DEVICE_LOCAL | HOST_VISIBLE` to accept it.
- Hardware examples and synthetic memory fixtures describe capabilities and
  layouts without specific device models or vendor examples.
- Both Linux and macOS Vulkan CI jobs require a device and a loaded validation
  layer, with synchronization checks enabled -- and a test that fails unless
  they report a deliberate hazard (`VKC_TEST_SYNC_VALIDATION`) -- and
  shader-access checks requested where the layer supports them.

### Added

- `CommandBatch::submit_async(wait, signal)`: submit a batch without waiting
  for it. Nothing it records starts before every `TimelinePoint` in `wait`
  (a `TimelineSemaphore` and a value) is reached, and it sets those in
  `signal` once it completes, so a pipeline's stages chain on the device
  rather than each waiting on the host. It returns a `PendingBatch`: `ready`
  polls, and `wait` writes the readbacks and frees the staging; destroying an
  unfinished one waits for it. Its commands run untimed. Everything the batch
  recorded must stay alive and unchanged until the work completes.
  `Device::submit_pending` does the same for a command buffer you record,
  returning a `Device::PendingSubmit`, and `TimelineSemaphore::device()` names
  the `VkDevice` a semaphore was made on. Existing callers need no change:
  `submit` and `submit_single_time` behave as before.
- `Device::check_enabled(reqs)`: whether a device enabled everything `reqs`
  requires -- the queue's capabilities and presentation; what `create` and
  `adopt` check, the usable version and support for each required extension
  and feature within the version that makes it core; and the device's record
  of the extensions and the core, timeline, scalar and dynamic-rendering
  features it enabled. A device records those features when created,
  including the three flags its feature chain sets, or as its creator
  declares them in the `AdoptedDevice` passed to `adopt`. A library handed a
  device it did not make calls this before building kernels that need a
  feature. Requirements that carry a `feature_chain` are refused
  (`InvalidArgument`), as no device records one: check those features by
  other means and pass the requirements without the chain.
- `EnabledFeatures`: the core features and the timeline, scalar and
  dynamic-rendering flags a device enabled, as a device records them and
  `AdoptedDevice::enabled_features` declares them.
- `vulkan`: `format.hpp`, what a `VkFormat` implies for a view and a copy:
  `format_has_depth`, `format_has_stencil`, `view_aspect` (the aspect a
  default view covers), `format_needs_ycbcr_conversion` (a view needs a
  sampler Y'CbCr conversion) and `texel_bytes` (one texel of an uncompressed,
  single-plane color format; 0 otherwise), for core and KHR formats whatever
  headers the core was built with. gfx can replace Vulkan-Utility-Libraries'
  `vkuFormat*` with them ("Migrating from a sibling's own copy", below).
- `vulkan` tier foundation (`volumetric_kit::core_vulkan`, built with
  `VKC_WITH_VULKAN`), merged from recon's and gfx's cores (DECISIONS.md, "The
  vulkan tier"):
  - `vulkan.hpp`, the one header first-party code includes Vulkan through
    (the `vulkan-include-umbrella` hook enforces it); `vk_error`,
    `VKC_VK_TRY`, `vk_result` and `to_string(VkResult)`; `UniqueHandle`,
    whose deleter may also be a loaded entry point, so a switch to volk
    changes no spelling.
  - `PhysicalDeviceInfo`: a device's properties, memory heaps and types,
    queue families, extensions and features, captured once; the version
    usable on it -- the lower of the device's and its instance's;
    `unified_memory()`, which tells unified memory (every heap device-local)
    from a discrete GPU; and `device_mapped_memory()`, whether `DeviceMapped`
    memory exists.
  - `DeviceRequirements`, which replaces recon's and gfx's `DeviceConfig`;
    `merge` for a device two libraries share; and `check_device_support`, the
    one check selection, create and adopt share.
  - `Instance`: Vulkan 1.3 or the loader's lower version, validation routed
    to the log sink (source `"vulkan"`) -- or dropped with a warning when the
    layer is missing or fails to load, as `validation_logged()` tells --
    debug utils, portability. `select_physical_device(requirements, surface)`
    returns the chosen device's `PhysicalDeviceInfo`, and says why each
    device was refused.
  - `Device`: create from that `PhysicalDeviceInfo`, or adopt, which takes
    the instance's version too; one queue plus an optional present queue;
    thread-safe `submit_single_time`, `submit_and_wait`, `queue_present` and
    `wait_idle`; a never-null `submit_mutex`; debug labels; external-memory
    and Metal-object capabilities. `create` enables `VK_EXT_memory_budget`
    where offered.
- `vulkan` tier memory and resources (DECISIONS.md, "The vulkan tier", V2):
  - `Allocator` over VMA v3.4.0 (private): `create_buffer`, `create_image`,
    per-heap `memory_stats`. Three placements, each a memory-type mask cut to
    the resource's own requirements, and a full heap -- or one past its
    budget, the driver's where `VK_EXT_memory_budget` is enabled -- fails
    rather than spill: `DeviceOnly` -- the default, and the only one for
    images -- device memory the host cannot map (VRAM, never the BAR window;
    private storage on unified memory); `DeviceMapped`, device-local memory
    the host writes (the BAR window; unified memory's pool); `Staging`, host memory
    with copy usage only, so no shader reads host memory. The placement
    decides whether a buffer is mapped; `HostAccess` (`SequentialWrite` by
    default) narrows a mapped one's type. Resources may outlive the
    allocator. A device-address buffer usage is refused for now.
  - `Buffer` and `Image` (with `MemoryInfo`), made by the allocator or
    adopted with a deleter. `Image` covers 2D, 3D, array, cube, mipmapped and
    multisampled images with a default view, and replaces gfx's `Texture`.
    `ImageInfo` records the type, samples, create flags and tiling, and the
    layout the owner last recorded with `Image::set_layout`.
  - Queue-family sharing for buffers and images: two or more distinct
    families give `VK_SHARING_MODE_CONCURRENT`; `check_queue_family_count`.
  - `DescriptorSetLayout`, `DescriptorPool`, and `DescriptorSet` with storage
    buffer, uniform buffer, combined image sampler and storage image writes;
    a write naming a null buffer or view aborts via `VKC_CHECK`. A set from
    `DescriptorPool::allocate`, and every copy, reads as empty once the pool
    is destroyed.
  - `ShaderModule`; `Fence`, `Semaphore`, `TimelineSemaphore`; `CommandPool`
    and `CommandBuffer`.
- `vulkan` tier compute (DECISIONS.md, "The vulkan tier", V3), from recon:
  - `ComputePipeline`; `ComputeKernel`, `KernelSetBuilder` (kernels sharing
    one descriptor pool; a kernel registered again is replaced whole, or left
    as it was on failure), `KernelSets` (extra sets of a kernel's layout), and
    the one-shot `dispatch`.
  - `CommandBatch`: one call's uploads, fills, zeroes, copies (buffer and
    image), queue-family acquires, dispatches (direct and indirect) and
    readbacks, recorded into one command buffer, submitted and waited on once.
    It records handles and set copies, so a kernel or set may move before the
    submit, which refuses a dispatch whose set was rewritten or freed;
    `retain` keeps a buffer its commands use that the caller replaces.
  - `compute_util`: `group_count`, `max_storage_buffer_range` (from the
    `Device`'s caps), `check_storage_buffer_range`, `mapped_storage_buffer`,
    `upload_storage_buffer`, `device_storage_buffer`, `ensure_device_scratch`
    and `StorageInput`, whose outgrown buffers go to the batch that used them.
  - `vkc_compile_shaders` and `vkc_embed_shaders`, for every sibling's
    shaders, installed with the package. Headers and symbols are named for
    the file name made a C identifier; an INTERFACE library shares one
    compile among the targets that link it.
  - `Device::submit_single_time` takes what the work uses (`keep_alive`),
    which the device keeps past a failed wait, with the command buffer, until
    it has waited for the work.
- `vulkan` tier timing (DECISIONS.md, "The vulkan tier", V4):
  - `QueryPool` (timestamp and occlusion queries), from gfx; `GpuTimer`,
    `GpuStageScope` and `GpuSpanTag`, from recon, with `timestamp_delta` and
    `ticks_to_ms`. A span is read only after its own submit
    (`GpuTimer::settle`) and published by the scope that opened it; a stage
    that loses a span to a full window reports no device time.
  - `CommandBatch`'s uploads, copies and dispatches, and `dispatch`, take an
    optional `GpuStageScope*`, keeping its tag rather than the scope;
    `Device::submit_single_time` has an overload taking a `GpuStageScope&`.
    Spans resolve once the fence has signalled. Work that never reached the
    device drops its spans; work that may still run retires its timers, whose
    query pools the device keeps until it has waited.
  - `Device::submit_single_time` takes an optional `bool* in_flight`: whether
    a failed call left work the device may still run.
  - Migrating from recon's `GpuTimer`: `resolve()` and `discard(span)` take
    the spans one submit carried (`resolve(first, count)`,
    `discard(first, count)`; `settle` does both), `report_into` is for a
    timer used without scopes, and a null span name aborts (`VKC_CHECK`)
    rather than reading as `"gpu"`.
- `vulkan` tier external memory (DECISIONS.md, "Where memory lives", V4):
  - `create_exported_buffer`, from recon: a device-only storage buffer on
    memory dedicated to it, from an `Allocator` and within its heap's budget,
    exported as an opaque file descriptor for CUDA to import (as dedicated,
    `cudaExternalMemoryDedicated`). `UniqueFd` owns the descriptor until an
    import takes it. `find_memory_type`, for a resource bound outside the
    allocator, never chooses a protected, lazily allocated or feature-gated
    device-coherent or device-uncached type.
  - `CommandBatch::release`, the releasing half of a queue-family ownership
    transfer, recorded after every command of the batch: an exported buffer
    goes back to `VK_QUEUE_FAMILY_EXTERNAL` once the kernels have read it.
  - Migrating from recon's `create_exported_buffer(device, bytes)`: pass the
    allocator, `create_exported_buffer(device, allocator, bytes)`, whose
    budget now counts the memory. `fd` is a `UniqueFd`: give CUDA
    `fd.get()`, set `cudaExternalMemoryDedicated`, and call `fd.release()`
    once the import succeeds rather than `close` after one fails. Each frame,
    synchronize the CUDA stream that wrote before submitting the batch that
    acquires the buffer, and release it back in that batch, so CUDA writes
    the next frame into a buffer Vulkan has handed over.
- `vulkan` tier shared device (DECISIONS.md, "The vulkan tier", V5):
  `SharedDevice` and `SharedDeviceConfig` build one instance, device and
  optional surface satisfying a compute library's and a renderer's
  requirements, on the best device that meets them and has a `QueuePlan`,
  carve their queues by the best plan it allows -- the renderer's always does
  graphics and presents itself -- and hand each a core `AdoptedDevice`, with a
  mutex for every queue. Replaces recon's `examples/viewer/shared_device.hpp`
  and ios's `SharedDevice`; "Migrating from a sibling's own copy" says how.
- CI runs the vulkan tier's device tests on lavapipe, with a device required,
  and in the sanitizer job under the Khronos validation layer, which must be
  on and reach the log sink; a leg builds with no Vulkan installed.

- Build system: CMake ≥ 3.21, C++17, per-tier targets with an umbrella
  `volumetric_kit::core`, install/export for `find_package(volumetric_kit_core)`
  and FetchContent. Tests and `-Werror` default ON only at the top level;
  install rules default ON everywhere, so a sibling can export targets that
  link the core. The shared library carries a `MAJOR.MINOR` soname, and a
  configure that links a static core into a shared library warns.
- `base` tier (`volumetric_kit::core_base`), merged from `calib`'s, `recon`'s
  and `gfx`'s copies (DECISIONS.md, "Merging the three `Status`/`Result`
  types"):
  - `Status` with the codes of all three (`Numerical` from `calib`, `Backend`
    with a neutral `int64_t` detail from `recon`), `Status::with_context`, and
    `Result<T>`, both `[[nodiscard]]`, plus `VKC_TRY` / `VKC_ASSIGN`.
  - `VKC_CHECK`, which logs through the sink and aborts.
  - The pluggable log sink (`set_log_handler`, `log_message`), one per process
    for the whole family, with each message's source library.
  - The version API (`version_string`, `VKC_VERSION_*`).
  - `StageMetrics`, `StageRow` and `StageScope` (from recon, replacing gfx's
    `FrameMetrics::Section`): named host and device spans every library
    reports in. A copy or an assignment carries the rows, not the open
    scopes; a null name aborts (`VKC_CHECK`).
- Tests (GoogleTest) and a package-consumer project, which installs and
  exports a library of its own, that CI builds against an installed copy and
  against the source tree (also sanitized).
- CI on GitHub-hosted runners: Linux and macOS Debug/Release, Linux
  `-fno-exceptions` and shared-library legs, ASan/UBSan/LSan, and the lint
  gate, behind one required check. A push to `main` never cancels another
  commit's run.
- Formatting and lint: pinned clang-format and cmake-format hooks that fix
  files in place, hygiene checks (YAML, merge-conflict markers, large files,
  line endings), `.editorconfig`, and clang-tidy (`.clang-tidy`, pinned
  22.1.8) run through the build with `-DVKC_CLANG_TIDY=ON`, locally and in CI.

### Changed

- `AdoptedDevice` declares its features in one `EnabledFeatures`:
  `handoff.enabled_features = features` → `handoff.enabled_features.core =
  features`, and `handoff.enabled_timeline_semaphore`,
  `enabled_scalar_block_layout` and `enabled_dynamic_rendering` →
  `handoff.enabled_features.timeline_semaphore`, `.scalar_block_layout` and
  `.dynamic_rendering`. `adopt`'s refusals name those fields.
  `SharedDevice`'s payloads declare the features its device was created
  with, including any its feature chain enabled.
- `TimelineSemaphore::create` takes the `Device`, not its handle, and refuses
  (`Unsupported`) a device that did not enable `timelineSemaphore`:
  `TimelineSemaphore::create(device.handle(), value)` →
  `TimelineSemaphore::create(device, value)`.
- A missing core feature is named: selection, `create`, `adopt` and
  `check_enabled` say which `VkPhysicalDeviceFeatures` member, where they
  said "a required core feature".
- `Device` holds the record of what it enabled. Rebuild consumers after
  bumping their pin: `Device` changes size.
- `vulkan`: **the oldest supported Vulkan headers are 1.3.204, and 1.3.208 on
  Apple** (DECISIONS.md, "Vulkan headers come from the system"); on Android,
  take NDK r25 or newer, whose headers are Vulkan 1.3's. Older headers fail at
  configure where FindVulkan reports a version (CMake 3.23 and newer) -- in
  the build, and in the installed package's `find_dependency` -- and otherwise
  in `vulkan.hpp`, with a message naming the version needed. On Apple, 1.3.204
  through 1.3.207 used to build without portability enumeration and then find
  no MoltenVK device. CI builds and tests on Ubuntu 22.04's headers and, on
  macOS, on 1.3.208's.
- `HeapStats` gains this allocator's own share beside the heap's figures:
  `reserved_bytes`, its blocks and dedicated memory with their free space,
  and `allocation_bytes`, its live allocations. `usage_bytes` and
  `budget_bytes` keep their values and meaning -- the heap's usage and
  budget, as recon's and gfx's `HeapStats` had them -- so headroom computed
  as `budget_bytes - usage_bytes` needs no change. Code that read
  `usage_bytes` as this allocator's own memory, or summed it across
  allocators, switches to `reserved_bytes`. Rebuild consumers after bumping
  their pin: `HeapStats` changes size.

For a consumer pinned at a commit of V2's or V3's first API, which this
section's entries replace (DECISIONS.md, "Where memory lives"):

- `MemoryUsage::DeviceLocal` and `MemoryUsage::Auto` → `DeviceOnly`.
  `MemoryUsage::HostVisible` → `Staging` for a buffer with copy usage only;
  `DeviceMapped` for one a shader reads (uniforms, parameters), or
  `DeviceOnly` written by a `CommandBatch` where
  `PhysicalDeviceInfo::device_mapped_memory()` is false.
- `BufferDesc::mapped` is removed: delete the assignment. `DeviceMapped` and
  `Staging` buffers are always mapped, `DeviceOnly` ones never.
  `ImageDesc::memory` is removed too: every image is device-only.
- `BufferDesc::host_access` defaults to `SequentialWrite` (was `Random`): set
  `HostAccess::Random` on a buffer the host reads, such as a staging
  readback. `Random` on a `DeviceMapped` buffer needs cached device-local
  memory, which unified memory has and a discrete GPU does not
  (`Unsupported`).
- `storage_buffer` → `mapped_storage_buffer`, now device-mapped (was
  host-visible) and `HostAccess::SequentialWrite` by default (was `Random`).
  A caller that reads it through `mapped()` passes `HostAccess::Random` on
  unified memory, or reads the results back with `CommandBatch::readback`; a
  caller that only copied from it (`TRANSFER_SRC`, never bound) makes a
  `MemoryUsage::Staging` buffer instead; a bulk input goes through
  `StorageInput`, staged into device-only memory. `upload_storage_buffer`
  is device-mapped the same way.
- `create_buffer` and `create_image` return `Unsupported` where no type of
  the placement suits the resource (was a `VK_ERROR_FEATURE_NOT_PRESENT`
  backend error, or for an image another device-local type), and
  `VK_ERROR_OUT_OF_DEVICE_MEMORY` past the heap's budget.

### Removed

- Windows and MSVC support, which no sibling or CI leg used (DECISIONS.md,
  "Platforms"): the MSVC warning flags (`/W4`, `/WX`) and `VKC_SANITIZE`'s
  MSVC check; `UniqueFd`'s CRT close; and the `_MSC_VER` path, with the
  `"unknown"` fallback for other compilers, of the call-site capture
  `Result::value()` uses -- `result.hpp` now needs GCC or Clang. Nothing to
  migrate on Linux, macOS, iOS or Android.

### Migrating from a sibling's own copy

- `volumetric_kit::{calib,recon,gfx}::Status` / `Result` →
  `volumetric_kit::core::Status` / `Result`; `VC_*` / `VR_*` / `VG_*` macros →
  `VKC_TRY`, `VKC_ASSIGN`, `VKC_CHECK`.
- `gfx`: `Status::Code::Vulkan` and `Status::code()` → `Status::Code::Backend`
  and `Status::detail()` (the `VkResult` as `int64_t`). The
  `Status::error(VkResult, message)` factory is gone: use
  `Status::backend_error(static_cast<std::int64_t>(result), message)` until
  the vulkan tier lands, then `vk_error`. `vk_error`, `VG_VK_TRY` and
  `to_string(VkResult)` move to the vulkan tier.
- `recon` / `gfx` / `ios`: the new `Status::Code::Numerical` breaks every
  exhaustive `switch` over the codes (an error under `-Werror=switch`). Add a
  case to `ios`'s `error_code()` in `apps/scanner/Bridge/RendererErrors.mm`.
  `recon`'s `named_failure()` in `core/compute_kernel.cpp` becomes
  `why.with_context(name)`, which keeps the domain and detail with no switch;
  `recon`'s own `to_string` in `core/result.cpp` goes with its `Status`.
- `gfx`: `impl/vk_format.hpp` → `volumetric_kit/core/vulkan/format.hpp`:
  `aspect_mask_for` → `view_aspect`, `texel_size` → `texel_bytes`, and
  `format_has_depth` / `format_has_stencil` → the core's of the same name.
  They answer as before for core and KHR formats, but a vendor or EXT
  extension's format now reads as nothing -- `texel_bytes` returns 0 where
  `texel_size` gave its size -- so an offscreen readback or texture upload of
  one is refused. With no `vkuFormat*` left, gfx needs neither
  Vulkan-Utility-Libraries nor its pinned headers.
- `recon` / `gfx`: `Status` and `Result` are now `[[nodiscard]]`; a call site
  that drops one now warns.
- `recon` / `gfx`: `Status::backend_error(0, …)` (a success code) now aborts,
  as `gfx`'s `Status::error(VK_SUCCESS, …)` did; test the call's result first.
- All three: `Result::value() &&` and `operator*() &&` return `T` by value, not
  `T&&`. `auto&& x = std::move(r).value();` still works (the temporary's life
  is extended); code that relied on moving out of the `Result` in place, then
  reading it again, must keep the returned value instead.
- All three: `Result<bool>` no longer converts from a pointer (a string
  literal included), and a string-like `Result` no longer converts from
  `nullptr`; return a `Status` for the error those meant.
- Logging: `LogHandler` and `log_message` take a `source` between the level
  and the message. The default sink prints `[<source> <level>]`, so passing
  `"vr"` (recon) or `"vg"` (gfx) keeps the old prefixes, and log greps keyed
  on `[vr ` / `[vg `, working; the library's name reads better. Every call now
  shares one handler object, concurrently: state it keeps persists between
  calls, and it must synchronize that state. `set_log_handler` waits for other
  threads' calls to the previous handler to return.
- `calib`: `VC_CHECK` now reports through the log sink instead of printing to
  stderr; the default sink still prints it to stderr.
- `recon`: the compute pieces move by namespace, except
  `max_storage_buffer_range(physical)`, which becomes
  `max_storage_buffer_range(device)` (`volume/voxel_block_grid.cpp`). A
  `CommandBatch` now binds the set a dispatch recorded, so reassigning the
  caller's `DescriptorSet` object after it no longer refuses the submit, and
  a temporary set is accepted.
- `recon` / `gfx` memory, as "Changed" above: `DeviceLocal` → `DeviceOnly`;
  drop `desc.mapped`. `recon`: `CommandBatch`'s staging and
  `gpu_frame_prep`'s frame staging (`storage_buffer` with `TRANSFER_SRC`)
  become `MemoryUsage::Staging` buffers; the marching-cubes block spans,
  which the host walks, become a `device_storage_buffer` read back by
  `CommandBatch::readback`, or `mapped_storage_buffer(..., HostAccess::Random)`
  where `unified_memory()` holds. `gfx`: buffers left at `Auto` become
  `DeviceOnly`; the texture-upload and offscreen-readback staging
  (`HostVisible`) becomes `Staging`, the readback with `HostAccess::Random`;
  the per-frame uniform buffers (`OwnedDescriptorSet`) become `DeviceMapped`,
  or `DeviceOnly` written by a `CommandBatch` where `device_mapped_memory()`
  is false; and image descriptors drop `memory`, as `ImageDesc` has none.
- `recon`'s `examples/viewer/shared_device.hpp` and `ios`'s
  `Bridge/SharedDevice` → `SharedDevice`, made only by
  `SharedDevice::create(config)` as a `std::unique_ptr`, after recon and gfx
  adopt through the core's `Device` (the payloads are core `AdoptedDevice`s):
  - The config: each library's requirements go to `config.compute` (recon's)
    and `config.graphics` (gfx's, with `needs_present` for a window); the old
    config's `app_name` and `enable_validation` move to `config.instance`.
    The surface is the app's: put its instance extensions in
    `config.instance.extensions` -- `glfwGetRequiredInstanceExtensions`'s, or
    `VK_KHR_surface` and `VK_EXT_metal_surface` on iOS -- and create it in
    `config.make_surface`, which `glfwCreateWindowSurface` (recon) or
    `vkCreateMetalSurfaceEXT` on the `CAMetalLayer` (ios) becomes.
  - recon: `build_shared_device(window, config, out)`, which returned `bool`
    and printed why, → `create`, which returns why. `out.queue_plan`,
    `graphics_family`, `compute_family`, `instance`, `device` and `surface`
    become accessors (`plan()`, ...), and `instance()` returns the `Instance`
    (`instance().handle()` for the `VkInstance`). The `submit_mutex`, set only
    under the shared-queue plan, becomes one in every payload.
  - ios: `build(metal_layer, app_name)` on a default-constructed member →
    `create`; `instance()` returns the `Instance`, as above.
  - Both: `recon_adopt_payload` / `recon_payload()` → `compute_payload()`;
    `gfx_adopt_payload` / `gfx_payload()` → `graphics_payload()`;
    `QueuePlan::kTwoQueuesOneFamily`, `kTwoFamilies`, `kSharedQueue` →
    `TwoQueuesOneFamily`, `TwoFamilies`, `SharedQueue`. `release_surface()`,
    `wait_idle()` and `summary()` keep their names (recon gains the last two).
