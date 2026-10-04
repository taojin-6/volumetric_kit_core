# Changelog

All notable changes to `volumetric_kit_core` are documented here. The format
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html). Before
1.0, a minor release may break the API; each such entry says how to migrate.

## [Unreleased]

### Added

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
    Apple's private storage); `DeviceMapped`, device-local memory the host
    writes (the BAR window; unified memory's pool); `Staging`, host memory
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
