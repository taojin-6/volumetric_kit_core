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
- A memory-placement fix stayed in one copy: `recon` made `DeviceLocal`
  *require* device-local memory after host-memory accesses across PCIe slowed
  its kernels; `gfx` still only prefers it.
- The shared-device bootstrap existed twice: `recon`'s
  `examples/viewer/shared_device.hpp` (720 lines) and `ios`'s
  `Bridge/SharedDevice` (756 lines).
- `calib`, `recon` and `gfx` each defined their own `Status`/`Result`.

One copy means a fix lands once, and the zero-copy handoff between libraries
on one `VkDevice` passes one shared buffer type rather than raw handles
described by hand.

### Tiers

One repository, one CMake target per tier, so a consumer links only what it
uses (`calib`'s headless solver needs no Vulkan):

| Tier | Depends on | Holds | Lands |
| --- | --- | --- | --- |
| `base` | — | `Status`/`Result`, `VKC_CHECK`, `exception_status`, logging, stage metrics, whole-file I/O, version | now |
| `config` | `base`, private nlohmann/json | family scenario schema, typed settings, defaults and static validation | 0.3.0 |
| `vulkan` | `base` | instance, device create/adopt, allocator, buffers, images, descriptors, shaders, compute pipelines, command batches, external memory, the shared-device bootstrap | in stages from 2026-10-03 (below), before `calib` writes GPU code |

- `vulkan` is seeded from `recon`'s core, which carries the measured memory
  rules and the compute pieces `calib` also needs, then gains what `gfx`
  needs. `gfx`'s graphics-only parts (swapchain, render targets, graphics
  pipelines) stay in `gfx`; consumers migrate in the order `recon`, `gfx`
  (re-measuring its frame times, since its allocator semantics change), `ios`
  (deleting its `SharedDevice` copy).
- **No camera or sensor tier.** Camera models, the rig's calibration file,
  frame types, the capture interface and vendor drivers stay out of the
  core. Core PR #14 proposed a camera tier and was closed; `recon`'s
  DECISIONS.md (2026-10-06, "The family's camera vocabulary is recon's
  `camera` tier", amended the same day) makes `recon`'s `camera` and `sensor`
  tiers the library `calib` and `ios` build on for cameras and sensors, while
  `calib` keeps its own camera model. Moving either here is a new decision,
  in `recon`'s DECISIONS.md as well as this one.

### The vulkan tier

It lands in five stages, each its own PR, merged before any sibling migrates:

| Stage | Holds |
| --- | --- |
| V1 (landed) | `vulkan.hpp`, the `VkResult` helpers, `UniqueHandle`, `PhysicalDeviceInfo`, `DeviceRequirements` with `merge` and `check_device_support`, `Instance`, `Device` |
| V2 (landed) | `Allocator`, `Buffer`, `Image`, descriptor layouts, pools and sets, `ShaderModule`, fences and semaphores, command pools and buffers |
| V3 (landed) | `ComputePipeline`, `ComputeKernel`, `KernelSetBuilder`, `KernelSets`, `dispatch`, `CommandBatch`, the compute helpers, and the shader build functions |
| V4 (landed) | `QueryPool`, `GpuTimer`, `GpuStageScope`, `StageMetrics` (in `base`), timed `CommandBatch` commands and submits; `create_exported_buffer`, `UniqueFd` (now in `base`), `find_memory_type`, `CommandBatch::release` |
| V5 (landed) | `SharedDevice`: one device for a compute library and a renderer, replacing recon's example copy and ios's `SharedDevice` |

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
  features -- is what selection and create run, and adopt and
  `Device::check_enabled` run its device-level half, so the four cannot drift
  as the copies did.
  `select_physical_device` returns the `PhysicalDeviceInfo` the check read,
  and `Device::create` takes it, so a device is queried once.
- **Generic queue accessors.** `queue()`, `queue_family()`, `queue_flags()`,
  `timestamp_valid_bits()`, plus an optional present queue, replace recon's
  `compute_*` and gfx's `graphics_*`. The present queue is the primary queue
  when its family can present.
- **recon's submit model.** Each submit records on a command pool of its own,
  so submission is thread-safe, and keeps its fence to avoid per-submit
  creation overhead. gfx's single shared `command_pool()` goes; its
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
- **A device records what it enabled, and a library checks it.** A library
  handed a `Device` it did not make cannot ask Vulkan what was enabled, and
  using a feature that was not enabled is invalid usage a driver need not
  report: recon's `layout(scalar)` kernels ran without error on a device made
  with the default requirements, which leave `scalarBlockLayout` off. So a
  device keeps an `EnabledFeatures` -- what `create` enabled, its
  requirements' and the three flags its feature chain set, or what `adopt`'s
  creator declared -- and `Device::check_enabled(reqs)` holds a library's
  requirements to it, after the queue, presentation and the device-level
  check. That check comes first because a declaration may claim what adopt's
  own requirements never asked about, which the physical device lacks or its
  usable version does not make core: `check_enabled` is never weaker than
  `create` or `adopt` would be with the same requirements. `adopt` runs the
  record half on the declaration, a refusal naming the `AdoptedDevice` field
  to fix, and `SharedDevice`'s payloads declare what its create actually
  enabled. Requirements that carry a feature chain are refused, not passed:
  no device records a chain, and passing what the check cannot see is the
  silent invalid usage it exists to catch. The core's own feature-dependent
  factory, `TimelineSemaphore::create`, takes the `Device` and runs the
  check itself.
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
  later, so the planned switch to volk for Android stays inside `vulkan.hpp`
  (iOS links MoltenVK statically and needs no loader: the 2026-10-19 entry).
  A runtime deleter would add storage to every handle, and traits keyed on
  the handle type cannot tell `VkFence` from `VkSemaphore` on 32-bit targets,
  where both are `uint64_t`.
- **Vulkan from the system** (`find_package(Vulkan)`, `Vulkan::Vulkan`
  PUBLIC), as recon and ios use it, and gfx too once it builds on this tier
  ("Vulkan headers come from the system", below).
- **`VKC_WITH_VULKAN` is opt-in for a subproject.** ON at the top level, OFF
  when fetched: recon and gfx set it, and calib, which fetches the core for
  its error types, then needs no Vulkan installed. CI builds the tier on
  every leg, as calib will take it too; until then, calib's own CI is the
  build without Vulkan installed.
- **CI without GPUs, but not without devices.** The hosted Linux legs run the
  device tests on lavapipe with `VKC_REQUIRE_VULKAN_DEVICE=1`, so a missing
  device fails instead of skipping; the sanitizer job adds the validation
  layer (`VKC_TEST_VALIDATION=1`), so ASan, UBSan, LSan and validation check
  the same run. Under it, a test fails if validation is off or does not reach
  the log sink, and the run fails on errors the layer reports as the shared
  devices and instances are destroyed ("One Vulkan test fixture for the
  family", below). The 2026-10-23 decision moves the build legs to physical
  GPUs; the hosted sanitizer job keeps this lavapipe coverage.

V2's choices, from the same comparison:

- **recon's memory rules**, tightened since (see "Where memory lives"):
  kernel memory *requires* `DEVICE_LOCAL`, so an allocation fails rather
  than spill bulk kernel data into host memory across PCIe; gfx only preferred
  it. A host-visible buffer is mapped persistently and coherent, so
  `mapped()` is a plain pointer. A device-address usage is refused, as
  recon's `MarchingCubes` refused it:
  VMA aborts on one unless its allocator enables buffer device addresses,
  which needs a feature `DeviceRequirements` cannot ask for yet.
- **Sharing from the queue families a resource names.** Two or more distinct
  families give `CONCURRENT`, one or none `EXCLUSIVE`; duplicates count once,
  and a family the device lacks is refused. recon's rule, now for images too:
  gfx's were always exclusive, which needs an ownership transfer when another
  family reads them. `BufferDesc::kMaxQueueFamilies`
  and `check_queue_family_count` stay for recon's configs.
- **Resources outlive their allocator safely.** Each buffer and image holds a
  reference to the VMA state, freed with the last of them (recon's); a
  documented destruction order cannot express `a = std::move(b)`.
- **One allocator per library per device**, separate from `Device`: VMA
  allocators are independent bookkeeping over one `VkDevice`, so a library
  sharing an adopted device reports its own reserved and live allocation
  bytes separately from process-wide heap-budget usage.
- **One `Image` type** replaces gfx's `Texture` and recon's adopted `Image`:
  made by `Allocator::create_image` with gfx's validation (3D, arrays, cubes,
  mips, multisampling, a default view whose type and aspect follow the image),
  or adopted from an `ImageInfo` and a deleter. `handle()`, not gfx's
  `image()`, as every other wrapper names it. Images are device-only (see
  "Where memory lives"): they have no host accessor, so a host-visible one
  would be memory the host cannot use. recon's `BufferMemoryInfo` becomes
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
  of one descriptor instead of the invalid zero. `max_storage_buffer_range`
  takes the `Device` too, whose caps hold the limit, rather than query a
  `VkPhysicalDevice` again: recon's one caller changes. A kernel registered
  again is built aside and moved in whole, so a failure leaves it as it was
  and a rebuilt kernel holds no set of its old layout.
- **Two kinds of path between host and kernel memory, both through
  `CommandBatch`** (D1 of the 2026-10-22 data-flow design, core 0.21.0). Per
  call: uploads inline up to 64 KiB and staged beyond, readbacks through one
  host buffer. Per frame: a persistent `HostRing` slot the batch binds
  (`input`, `output`), so a frame allocates nothing ("Where memory lives").
  Either way: barriers only where a command could see another's writes, a
  refusal that poisons the batch, and a descriptor set rewritten after its
  dispatch refused at submit. Kernel memory is never mapped.
- **Batch barriers follow queue capabilities.** Transfer-only work can run
  without naming unsupported compute stages; dispatch requires a compute
  queue. The final barrier includes graphics uniform and storage-buffer
  accesses on a graphics queue, alongside vertex/index and indirect input.
  Vertex-input visibility alone does not cover a renderer's descriptor reads.
  Across queues, consumers still provide the semaphore and any ownership
  transfer required by the handoff.
- **A batch records handles, not objects.** A dispatch takes the kernel's
  pipeline and a copy of its set, never a pointer to the caller's object,
  which may move -- a vector of kernels that grows -- or go before the submit.
  What it cannot see stays the caller's to keep alive, as buffers and images
  always were. A set is the exception, as callers rewrite and replace them
  routinely: a `DescriptorSet` sees its pool go, so the submit refuses a set
  rewritten or freed since its dispatch. A buffer the caller replaces
  mid-batch -- an outgrown `StorageInput` upload or scratch -- goes to
  `CommandBatch::retain`, and an unaligned `zero` edge copies from one staged
  word of zeros rather than staging each.
- **Image copies take the common uncompressed color formats**, 8- to 128-bit
  texels, where recon's took `R8` and `R8G8`. They read the layout
  `Image::layout` records (V2's `set_layout` keeps it current) and refuse a
  multisampled image by `Image::samples`.
- **After a failed wait the device keeps what the work uses.**
  `submit_single_time` takes a `keep_alive`, which the device holds with the
  command buffer it may still run and frees once `destroy` has waited for it,
  before the `VkDevice`. A batch's staging, and the allocator behind it, is
  then neither freed under the GPU nor leaked past the device.
- **Timing came with V4** (below): recon's optional `GpuStageScope*`
  parameters are back as trailing defaults.
- **One shader toolchain.** `vkc_compile_shaders` and `vkc_embed_shaders`
  replace recon's `vr_*`, gfx's `vg_*`, and the copy of recon's ios borrows.
  `TARGET_ENV` (default Vulkan 1.2; gfx's renderer passes 1.3),
  `INCLUDE_DIRS` (recon's include root) and `SPIRV_VAL_ARGS` (recon's
  `--scalar-block-layout`) cover the differences. `SYMBOL_PREFIX` is
  required: the arrays are inline variables, which the linker merges by name,
  so two libraries' `fill.comp` would otherwise share one shader. The
  functions are defined when the core is added. Each rule belongs to one
  target -- `vkc_embed_shaders` compiles and embeds under one -- as a rule
  two targets reach races under Make and is refused by Xcode. Headers and
  symbols are named for the file name made a C identifier, and two names that
  make one are refused. An INTERFACE library shares one compile among the
  targets that link it. A shader that fails `spirv-val` fails every build until
  fixed: Make deletes a failed command's output, and Ninja and Xcode rerun it.

V4's choices, timing first (external memory's are with the placement rules,
in "Where memory lives"):

- **One metrics vocabulary, in `base`.** recon's `StageMetrics` and gfx's
  `FrameMetrics::Section` were the same four fields; `StageRow`,
  `StageMetrics` and `StageScope` are now the family's, with no Vulkan, so
  calib and a host-only exporter report in them too and an app shows recon's
  stages and gfx's frame in one table. gfx's `FrameMetrics` keeps its frame
  totals around `StageRow`s at its migration. A set's open scopes are not
  part of its value: a copy or an assignment carries the rows only, so
  `metrics = StageMetrics{}` inside a scope leaves it counted. A null name
  is a programmer error (`VKC_CHECK`), not a crash in `strcmp`.
- **A row says where its time went** (D3 of the 2026-10-22 data-flow
  design, core 0.20.0). Host time splits into busy and blocked (`wait_ms`),
  and the device half says what it holds (`DeviceTime`: none, measured, not
  timed, unavailable), so an unmeasured cell says why; a row is measured
  only when all its device work was. The new fields follow the first four,
  so a row built positionally keeps its meaning; `has_gpu` stays,
  deprecated.
- **Each thread keeps a stack of its open scopes.** This replaces "no
  global sink": every blocking wait in the core -- a blocking submit's fence,
  `PendingSubmit::wait`, `Fence::wait`, `TimelineSemaphore::wait`,
  `wait_idle` -- reports what it blocked (`note_blocked`) to every scope
  open on its thread, whose host span already contains it, so every row
  becomes honest with no change at its call site. A fence or semaphore wait
  that finds the work done reports nothing; a `wait_idle`, which Vulkan
  cannot ask first, always reports. A wait counts in each enclosing row as
  its host time does, and the totals skip breakdowns, so it counts once. A
  scope with null metrics never joins the stack, so nothing is measured when
  unasked. The stack lives in `core_base`'s source, one per thread for the
  whole process, and `WaitScope` covers a wait the core does not see. A
  scope opens and closes on one thread (`VKC_CHECK`): closed on another, it
  would stay on its opener's stack once freed. Serials are drawn for the
  whole process, so a tag recorded or submitted on another thread than its
  scope's matches none of that thread's scopes and keys on its own name.
- **gfx's `QueryPool`, the timer on it.** The pool owns the handle and
  range-checks its commands. It holds timestamp and occlusion queries only,
  one 64-bit result each, which is what it reads. A timer is unavailable
  rather than failing where a family has no timestamps or its pool will not
  allocate. A renderer's frames time on the timer's ranges too
  (`GpuTiles`), so gfx's profiler moves onto it (D9 of the same design).
- **`GpuStageScope` tags the work.** `CommandBatch`'s uploads, copies and
  dispatches and `dispatch` take recon's trailing `GpuStageScope* =
  nullptr`, so recon migrates by namespace. A command keeps the scope's tag
  and the chain of scopes open around it when recorded, never the scope, so
  one that closes before the submit leaves nothing dangling. The `Device`
  overload takes a `GpuStageScope&`: a nullable pointer beside the untimed
  overload's `keep_alive` would let `submit_single_time(record, nullptr)`
  pick the timed one.
- **Device time is tiled, not spanned per command.** A timed command buffer
  writes a `BOTTOM_OF_PIPE` timestamp before its first command, at each
  change of tile and after its last, and a command without a tag joins the
  open tile. So a stage's time counts its fills, copies, barriers and gaps,
  and a batch's tiles add up to its span; per-command spans missed all
  four. A tile counts to its tag's scope and to every scope open around it,
  as a wait does, so a host-only stage around a library's tagged work gets
  that work's device time, a breakdown nests in its stage, and
  `total_gpu_ms` skips breakdowns as `total_cpu_ms` does.
  `GpuTimer::set_max_depth` folds deeper scopes into their ancestor for the
  fewest timestamps, the deeper rows not timed. On a GPU layered on Metal a
  mark may snap to an encoder boundary (unverified): the attribution test
  says so there, and an app there tiles at depth 1 (`set_max_depth(1)`), so
  its sub-stage cells read not timed.
- **A batch holds a range of its timer's queries from recording until it is
  read.** So one timer times any number of pending batches, each read after
  its own work: a nested submit never reads the outer range, and
  `submit_async` is timed (below). The range is reset once, at the batch's
  start: a reset between commands can split MoltenVK's compute encoder. It
  is read once its fence has signalled, never on a timeline value: through
  MoltenVK a value is reached before the timestamps are readable. No free
  range, a failed batch -- before the device or on it -- or a timestamp not
  yet readable reads not timed, never an error: a diagnostic must not fail
  the work. A range the device may still write is never reused, a pending
  batch destroyed after a failed wait included, and the timer keeps timing
  on the rest; its pool rides the submit's `keep_alive`, so the device holds
  it past a failed wait, as it holds a batch's staging. Manually recorded
  tiles dropped while their work may still run also hand the pool to the
  device, without waiting or allocating: losing the timer's last pool owner
  after growth must not destroy queries still in use. With no submission
  fence known, these pools stay until Device destruction waits for its queue
  idle under the shared queue mutex, including on an adopted device. Manual
  tiles submit on the timer's device queue; an indeterminate wait leaks them as
  it does other unfinished work. At most 4096 queries a
  pool: MoltenVK backs one with a 32 KiB Metal counter sample buffer and
  emulates timing past it, so `reserve` clamps there.
- **Calibrated clocks where offered.** A device enables
  `VK_KHR_calibrated_timestamps`, else the EXT, where offered.
  `Device::calibrate` reads the queue's counter with the host domain
  `steady_clock` reads (`CLOCK_MONOTONIC` on Linux) and, where the driver
  has no such domain, brackets the device's reading by two host ones. Its
  deviation adds half the bracket to the driver's uncertainty. A timer uses
  a clock pair for at most one second; a failed refresh leaves spans in ticks
  until a later read succeeds. Gaps between batches on one queue need
  no calibration, being raw ticks of one counter.
- **Counts per device.** `DeviceCounters` -- submits, blocking waits that
  found the work unfinished and their time, staging buffers, and the
  dispatches and pipeline barriers batches record (core 0.21.0) -- are atomic
  adds where they happen, always on: what a test pins per frame. A batch's
  dispatches and barriers are counted once, as its submit records them: a
  GPU layered on Metal showed a step in a batch's GPU time near 80 of each
  (cause unknown), so a library pins its frame's batch at its counts. A
  timeline semaphore shares its device's, so its waits count there; a `Fence`
  and `SharedDevice::wait_idle` know no device, so theirs reach the scopes
  only.

V5's choices, from recon's `examples/viewer/shared_device.hpp` and ios's
`Bridge/SharedDevice`, which did the same job twice and differed only in how
the surface was made:

- **The core builds the shared device; the app makes the surface.**
  `SharedDeviceConfig` takes the two libraries' `DeviceRequirements` and a
  `make_surface(VkInstance)` callback, so the core links neither GLFW nor
  Metal; the app adds its surface's instance extensions to the
  `InstanceConfig`. Windowless (no present) needs no surface at all. So
  `DeviceRequirements` stays device-level: the instance-level needs are the
  app's (its surface's extensions) or the instance's own (debug utils).
- **Each queue does its library's job, whatever the requirements say.** The
  renderer's gets `VK_QUEUE_GRAPHICS_BIT` and the compute library's
  `VK_QUEUE_COMPUTE_BIT`, as both copies forced: `queue_flags` defaults to
  compute alone, and a renderer handed a compute family that presents could
  record no draw. Only the renderer presents; a compute side asking to is
  refused, as nothing would present for it.
- **The union is checked before anything is created, on every device.**
  `merge` combines the two requirements, and selection checks the union --
  version, extensions, features -- so a shortfall names itself instead of
  failing in `vkCreateDevice`. That check finds a family that presents, not
  that it is the renderer's, so each device that passes is asked for a queue
  plan too, and one with none is refused for that and the next tried, as both
  copies did; of the rest, the best by type wins, as
  `select_physical_device` ranks them. Selection, the present probe, the
  feature chain, the enabled extensions and `vkCreateDevice` itself are
  `Device::create`'s own (internal `support.hpp`), so the two ways of making
  a device cannot drift.
- **Three queue plans, best first, all searched:** two queues in one family;
  two families (what MoltenVK, with several one-queue families, gets); one
  queue shared under one mutex. Stopping at the first family that does both
  would take the last plan on MoltenVK. The renderer's family presents
  itself, as in both copies: no driver the siblings target splits graphics
  from present, and a separate present queue would be a third queue, and
  mutex, for every embedder to carry.
- **Every queue has a mutex, always handed out**, as both copies concluded
  after a drain raced a submit: Vulkan requires every host operation on a
  queue be externally synchronized, and `wait_idle` is a third thread on
  both. Under the shared-queue plan the two payloads share one. `create`
  returns a `unique_ptr`, as the libraries keep the mutexes' addresses.
- **The payloads declare what was created, never restate it**: `Device::adopt`
  checks each library's needs against them, as Vulkan cannot be asked what a
  logical device enabled. They are core `AdoptedDevice`s, so recon and gfx
  adopt through the core's `Device` once they migrate.

### Submits that do not wait

recon plans (2026-10-06) to pipeline a live rig's compute stages -- prepare a
set of frames, fuse it, mesh and texture it -- over a ring of sets in flight.
`CommandBatch::submit` and `Device::submit_single_time` each wait on a fence,
so the GPU idles while the host records the next call, and the host while the
GPU runs it. `CommandBatch::submit_async` and `Device::submit_pending` submit
without the wait:

- **A pending handle holds the work until a wait sees it complete.**
  `PendingBatch` and `Device::PendingSubmit` keep the command buffer, the fence
  and the `keep_alive` -- a batch's staging and readback memory, and the ring
  slots it binds -- and give the command back to the device once a wait sees
  the fence signalled. A failed
  wait leaves them to the device, as a failed `submit_single_time` wait does.
  Destroying an unfinished handle waits for it, with no limit: what the work
  uses is never freed under it, and a command is not parked with the device
  until the device is destroyed. Submitted work cannot be withdrawn, so the
  only alternatives -- returning while it may still run, or leaking
  everything it uses, the caller's buffers included -- trade a hang for
  undefined behaviour or a leak the library cannot see. The cost falls on
  work held for a value the host sets: an early return that skips setting it
  hangs the thread destroying the handle, so a caller sets every such value
  on every path (a scope guard declared after the handle). A handle that has
  waited a second for a value not yet reached logs a warning naming it, so
  the hang is not silent. Until a wait sees the work complete, the device
  also counts it as running: a device destroyed under a handle never waited
  on -- a misuse the handle's warning forbids -- waits for the work before
  it frees the command buffer and fence.
- **Used from one thread at a time.** A handle may move to another thread,
  but no call -- a wait, `ready`, `in_flight`, a move -- may overlap another
  on it: making a poll safe beside a blocking wait would take atomics or a
  lock on every call, for a pattern the ring of stages does not use.
- **Timeline values order the work.** A submission waits for
  `TimelinePoint`s (a `TimelineSemaphore` and a value) before anything it
  records starts, and sets others once it completes. The wait's stage mask is
  `ALL_COMMANDS`: it covers every command a batch records, and is valid on a
  queue of any capabilities. The semaphore carries the memory dependency, on
  one queue or across queues; across queue families an `EXCLUSIVE` buffer
  still needs its ownership transfer. An empty batch with values still
  submits, so a stage with no work passes its value on.
- **Submit what sets a value before what waits for it, and set host values
  before waiting on the queue.** The device has one queue, and every
  `CommandBatch` ends in a barrier all later commands on it wait for, so a
  batch waiting for a value only a later batch sets hangs the queue. A wait
  may come first only for a value the host or another queue sets. Until it is
  met, the held work also holds every later fence and queue wait on the
  queue: a later `submit`, `submit_single_time` or `wait_idle` -- and, on a
  shared queue, the other library's frame fences -- waits for it, and
  `wait_idle` holds the queue's mutex meanwhile. Neither can be checked: the
  library cannot know who will set a value, or when. So `submit_pending`
  says both rules, and recon's ring of stages submits each stage after the
  one it waits for.
- **Every refusal comes before a command buffer is taken**: a null or empty
  semaphore, one made on another `VkDevice`, any value on a device that did
  not enable `timelineSemaphore` -- an adopted device may share a `VkDevice`
  on which another library made semaphores -- and a value to set that would
  not advance its counter when the signal runs: one semaphore set twice in a
  submit (its signals run in no set order), a value not above one the submit
  waits for on the same semaphore, or not above both the counter and every
  value an earlier submit sets, as a queue's signals run in submission order.
  Each `TimelineSemaphore` keeps the highest value a submit that reached a
  queue sets. As for `TimelineSemaphore::signal`, these catch a stale value,
  not a race. `submit_async` checks the values before it marks the batch
  submitted, so a refused value can be corrected and the batch submitted
  again; every other refusal leaves it submitted, as `submit`'s do.
- **The checks and the record are public** (`check_timeline_points`,
  `note_timeline_signals`), so a library that submits to the queue itself --
  gfx's frame loop, whose submit carries the present's semaphore -- refuses
  what the core's submits refuse, and its values join the record they check
  against. Presentation requires the entire upstream signal chain to be
  submitted (`VUID-vkQueuePresentKHR-pWaitSemaphores-03268`). A recorded signal
  is insufficient: its producer may still wait for a future host value, and
  even a producer with no explicit waits can sit behind such work on its
  queue. The record keeps values, not a dependency graph, so
  `TimelineWaits::Reached` conservatively requires the current counters to
  have reached every named wait. It does not block; the caller waits for the
  producer or retries later. `Submitted` remains a source-compatible alias
  for this stricter policy. Binary semaphore dependencies and other work on
  the queue remain the submitter's responsibility. The core's own submits
  keep `TimelineWaits::Any`, as the rule above lets them wait for a value the
  host sets later.
- **A readback lands at the wait, once.** The work completing writes nothing
  the host can see, so a destination is never written while the host may be
  reading an earlier one.
- **A dispatched set is checked again at the wait.** `submit` returns only
  once the work completes, so its check covered the set's whole use. A
  pending batch's set is in use until a wait sees the work complete, so that
  wait refuses the batch, writing no readback, if the set was rewritten or
  freed since the submit. It cannot tell a write after the work completed
  from one during it, so the rule is the one it can check: a set stays as it
  was until the wait.
- **A pending batch is timed, and hands its rows back.** It holds its query
  range until its wait reads it, then returns the rows by name
  (`device_rows`) and its span (`device_span`) for the caller to fold in:
  nothing points at a caller's metrics while the batch is pending, so the
  caller may move them.
- **`submit` keeps its path and cost.** It still waits through
  `submit_single_time`; the two submits share only their checks, the
  readback layout and write-out, and the device's recording and submitting
  of a one-time command buffer.

Verified through MoltenVK on a unified-memory GPU, under validation with
synchronization checks: work held by a wait the host has not met does not
run, a chain runs in its timeline's order, and the handles move, self-move
and wait on destruction. A narrower wait mask (`TOP_OF_PIPE`) passed them too,
as MoltenVK held the work anyway: the mask rests on the rule above, not on
those tests.

### Where memory lives

Everything the GPU reads or writes directly is device-local, on both memory
architectures, so a discrete GPU never runs a shader, a
vertex fetch or an indirect read against host memory across PCIe: host memory
reaches the GPU only by a copy. Each placement is a memory-type mask, not a
preference: VMA scores `DEVICE_LOCAL` alone and `DEVICE_LOCAL | HOST_VISIBLE`
the same for memory the host never touches, breaking the tie by the driver's
type order, and on a full heap moves on to the next acceptable type. The mask
is cut to each resource: the allocator creates the buffer or image, reads its
memory requirements, and allocates from the placement's types among those it
allows (`vmaAllocateMemoryForBuffer` / `ForImage`, with no VMA usage to add
preferences of its own), so no resource is placed by a device-wide guess.
Recorded 2026-10-04; the per-resource cut, the budget and the host-access
rules after review, the same day.

| Data | Usage | Discrete GPU (DRAM + VRAM) | Unified memory |
| --- | --- | --- | --- |
| Kernel buffers, images, scratch (`device_storage_buffer`, `create_image`) | `DeviceOnly` | VRAM the host cannot map, never the BAR window | GPU-private storage where the device has it; else the one pool, unmapped |
| Data the host writes and shaders read: uniforms, per-frame parameters, tables (`mapped_storage_buffer`) | `DeviceMapped` | VRAM through the BAR window (all of VRAM under Resizable BAR) | the one pool, mapped |
| Uploads and readbacks (`CommandBatch`) | `Staging`, transfer usage only | system RAM: write-combined uploads, cached readbacks | the one pool |
| Bytes moved every frame: a stage's input, the results it decides on (`HostRing`) | `DeviceMapped` where `DeviceMapped` + `Random` memory is cached; else `Staging` and a `DeviceOnly` mirror; either placed with the ring's host access | system RAM, write-combined for a write-only ring, else cached, and a mirror in VRAM, one GPU copy of each range a batch binds | the one pool, mapped and bound in place; cached, except that a write-only ring takes an uncached type where the pool has one |

- **Three placements, no `Auto`.** `DeviceOnly` replaces `DeviceLocal`, and
  a buffer is device-only unless it says otherwise; `Auto`, which let VMA put
  a buffer in host memory once VRAM filled, is gone.
- **The placement says whether a buffer is mapped.** `DeviceMapped` and
  `Staging` always are, persistently and coherently; `DeviceOnly` never is.
  `BufferDesc::mapped`, which only had to agree with the placement, is gone,
  and so is `ImageDesc::memory`, which had one legal value.
- **`DeviceOnly`** takes the device-local types the host cannot see that the
  resource allows; a device with no such type at all -- every device-local
  type host-visible, as on many unified-memory devices and CPU
  implementations -- has one pool, and takes it. A resource no private type suits takes the
  rest of the pool on unified memory and is
  refused (`Unsupported`) otherwise, as the rest of a discrete GPU's
  device-local memory is the BAR window. Images take only this, so a driver
  may compress and tile them.
- **`DeviceMapped`** takes device-local, host-coherent types only: the BAR
  window on a discrete GPU, the one pool on unified memory. A device without
  one refuses it (`Unsupported`; `PhysicalDeviceInfo::device_mapped_memory`
  says beforehand), and a full BAR window fails the allocation; neither falls
  back to host memory. It is for small data on a discrete GPU: without
  Resizable BAR the window can be small, shared by every library, so a bulk
  input is staged into `DeviceOnly` memory (`StorageInput`), and a buffer the
  host fills for a copy is `Staging`. The host writes it sequentially: its
  reads of the BAR window cross PCIe uncached, so results come back by
  `CommandBatch::readback` or a ring's `output`.
- **`Staging`** takes host memory the device does not hold -- system RAM on a
  discrete GPU, never VRAM or the BAR window -- and copy usage only:
  `TRANSFER_SRC` and `TRANSFER_DST`. A storage, uniform, vertex, index or
  indirect usage is refused, so no shader can bind host memory made here.
- **Host access narrows a mapped placement.** `SequentialWrite`, the
  default, prefers write-combined types. `Random` prefers cached ones for
  `Staging`, and requires them for `DeviceMapped`, so the host never reads a
  discrete GPU's uncached BAR window: there it is refused (`Unsupported`),
  and unified memory, which has cached device-local memory, takes it.
- **New memory within budget; at the budget, only reuse.** Where a candidate
  heap has room for the resource, VMA allocates as it would without a
  budget, the mask cut to the heaps with room: an existing block first, then
  a new block of the first candidate type, or memory of the resource's own
  where the driver prefers that or the resource is large. VMA's
  `WITHIN_BUDGET` bounds its new blocks but not that dedicated path, which
  the explicit admission covers. Where no heap has room, nothing new is
  allocated, but a block's free space already counts toward its heap's usage,
  so a resource that fits one still takes it -- in any candidate type, even
  when the reported budget has fallen below usage. A resource that requires
  memory of its own is refused there. Trying reuse first instead would
  suballocate resources the driver prefers dedicated, and spill into another
  type's blocks before the first type grew. Exhaustion returns
  `VK_ERROR_OUT_OF_DEVICE_MEMORY`, never a spill to another placement, and a
  budget refusal names the budget rather than a VMA call. The driver reports
  current-process usage and a changing budget where `VK_EXT_memory_budget`
  is enabled; otherwise VMA estimates its own usage against 80% of each heap.
  Budgets are admission estimates, not reservations or guarantees of
  residency: the check and the allocation are not atomic, and another
  allocator on the device, or another process, can take the room between
  them.
- **A heap's figures and the allocator's share are separate fields.**
  `HeapStats::usage_bytes` is the heap's usage paired with `budget_bytes` --
  as recon's and gfx's `usage_bytes` was, so their headroom arithmetic
  ports unchanged -- and includes every allocation in the process where the
  memory-budget extension is enabled, so it is never summed across
  allocators. `reserved_bytes` (blocks and dedicated memory, free space
  included) and `allocation_bytes` (live allocations) are this allocator's
  own, and may be summed. Budget admission uses the heap's usage, not
  per-library accounting.
- **Special memory stays out.** Lazily allocated, protected, and feature-gated
  device-coherent and device-uncached memory are never a placement's: each
  needs a use or a feature this tier does not have, and VMA leaves the latter
  kinds out of every allocation, so a mask that counted them would promise
  memory VMA refuses.
- **Migrations.** recon's `DeviceLocal` and gfx's preferred `DeviceLocal`
  become `DeviceOnly`; host-visible buffers with copy usage become
  `Staging`, among them recon's frame staging in `gpu_frame_prep`, made by
  `storage_buffer` with `TRANSFER_SRC`. Two siblings bind host memory
  directly today and must move: gfx's per-frame uniform buffers
  (`OwnedDescriptorSet`) become `DeviceMapped`, or `DeviceOnly` written by a
  `CommandBatch` where `device_mapped_memory()` is false; and recon's
  marching-cubes block spans, which the kernel writes and the host walks,
  become a device-only buffer read back by `CommandBatch` (or device-mapped
  with `HostAccess::Random` where `unified_memory()` holds). `storage_buffer`
  becomes `mapped_storage_buffer`, and its default host access becomes
  `SequentialWrite`: a caller that reads it passes `Random`, which a discrete
  GPU refuses. CHANGELOG.md lists each step.
- **Exported memory follows the same rule** (V4's external memory).
  `create_exported_buffer` -- recon's, the buffer CUDA imports as an opaque
  file descriptor and a decoder writes -- takes its memory from an
  `Allocator`, placed by the `DeviceOnly` mask (`placement_types`) and
  within its heap's budget. recon allocated it outside VMA, as exported
  memory is dedicated to its resource; VMA's dedicated allocation takes the
  export chained (`vmaAllocateDedicatedMemory`), and memory outside the
  allocator was in no budget -- without `VK_EXT_memory_budget`, VMA's
  estimate counts only its own allocations, so the allocator overcommitted
  VRAM the decoder's buffers held. Its own fallback, any device-local type,
  also let a discrete GPU's exported buffer into the BAR window.
- **The descriptor is owned, and the host orders the APIs.** `UniqueFd`
  closes the descriptor on every path until an import takes it
  (`release`): an open one keeps the memory alive after Vulkan frees it, so
  a leak pins a whole dedicated allocation. Each frame a batch takes the
  buffer over from `VK_QUEUE_FAMILY_EXTERNAL` before its kernels read it and
  hands it back after them (`CommandBatch::release`, recorded after every
  command, so none runs on a buffer already handed back); CUDA's stream is
  synchronized before the batch is submitted, and writes again once the
  submit returns. External semaphores would order the two on the GPU; they
  wait for a consumer that needs the overlap. gfx's `ExternalHandleType`, a
  field every value but `None` refused, is not carried over.
  The allocator must be the device's own, checked before the buffer is
  made: memory from another device's allocator could neither be bound to it
  nor free it. `find_memory_type`, recon's search for a resource bound
  outside the allocator, skips the same special types the placement masks
  do, and places device-local memory asked for without `HOST_VISIBLE` by the
  `DeviceOnly` mask, so a resource limited to a discrete GPU's BAR window
  gets no type rather than the window. Opaque descriptors only: the
  family's CUDA interop is Linux.
- **An exported image is one plane, on memory of its own** (core 0.24.0).
  `create_exported_image` makes a 2D, optimal-tiling image of one mip, layer
  and sample from an `ImageDesc` -- format, extent, usage, view and queue
  families as `create_image` takes them -- on memory dedicated to it, placed
  and budgeted as an exported buffer's, so an importer maps the whole
  allocation as the image (CUDA as an array of its format and extent). A
  decoder's picture is an image per plane (`R8` luma, `R8G8` chroma): a
  shader samples each with no sampler Y'CbCr conversion, and the core needs
  no multi-planar format, plane views or `MUTABLE` flag.
  The importer writes in place, so a picture takes no buffer-to-image copy.
- **Its layout crosses with it.** `CommandBatch::acquire` and `release` take
  an image and the layout its contents move to. A release to
  `VK_QUEUE_FAMILY_EXTERNAL` moves them to the layout the importer takes --
  `GENERAL` for CUDA -- which the caller then records with
  `Image::set_layout`; the acquire that takes the image back moves them from
  it to the layout the batch's commands use, which a later copy in the same
  batch, and a release recorded anywhere in it, start from. An `EXCLUSIVE`
  image crosses only to and from outside Vulkan: between two of the device's
  families both halves name the same change of layout, from the one before
  the release, which one `Image::layout` cannot give the acquiring side once
  the release has been recorded; a `CONCURRENT` image needs no transfer
  there. An acquire of contents in `UNDEFINED` is refused, as they would be
  lost: an image the importer writes first is released to it first. A
  release waits for every stage the final barrier lets follow a batch, the
  graphics stages included: its change of layout is a write, which every
  read of the image on the same queue since the acquire, a draw's included,
  must precede, and only the barrier's own scope orders the two.
- **`PhysicalDeviceInfo::unified_memory`** tells the architectures apart:
  every heap is device-local, so every type is (the spec sets
  `DEVICE_LOCAL` on a type exactly when its heap has it). An APU whose driver
  reports a VRAM carve-out beside host memory is not unified: the core
  follows the driver's own accounting, so kernel data fills the carve-out
  and fails when it is full, as VRAM does, rather than land in host memory
  the driver does not call device-local. A library may branch on it.
  `HostRing` branches on memory by `device_mapped_cached` (below);
  `StorageInput` stays staged on every device.
- **A `HostRing` takes its path from the device's memory, with no switch**
  (D1 and D2 of the 2026-10-22 data-flow design, core 0.21.0). In place where
  a `DeviceMapped` + `Random` buffer lands in cached memory
  (`PhysicalDeviceInfo::device_mapped_cached`: unified memory's cached pool,
  a CPU implementation's one type): kernels bind the slot the host maps.
  Copied everywhere else, a discrete GPU with or without Resizable BAR
  included: a `Staging` slot and a `DeviceOnly` mirror, with one GPU
  copy of each range a batch binds -- an input's where `input` is called, an
  output's after every other command. On a discrete GPU that copy is the bus
  crossing, not an extra one, and the host never reads the BAR window. Slots
  are placed at create and replaced only by a claim that finds none big
  enough (at 1.5x), each placement counted in `staging_buffers`, so a frame
  allocates nothing. This closes the open "Unified memory" question: cached
  unified memory runs in place, by the device's facts. recon ran the staged
  path there so that Mac runs would exercise a discrete GPU's; the NVIDIA
  legs exercise it now, the Mac and lavapipe legs run in place, and each leg
  names its path (`VKC_EXPECT_RING_PATH`), which every ring test asserts.
- **A ring the host only fills is write-combined where it is copied** (D8 of
  the 2026-10-22 data-flow design, core 0.22.0). `HostRingDesc::host_access`
  places a ring's mapped buffers within the shape the device gives it and
  never changes the shape. `Random`, the default, stays cached. A
  `SequentialWrite` ring's `Staging` takes write-combined system RAM where
  the host heap has an uncached type; in place, its `DeviceMapped` buffer
  takes an uncached type where the pool has one beside the cached one, and
  the cached one otherwise. Resizable BAR gets no in-place rule. On a
  discrete GPU with all of its VRAM in one uncached `DEVICE_LOCAL |
  HOST_VISIBLE | HOST_COHERENT` type behind a PCIe Gen4 link, writing the
  frame through the BAR and binding it in place beat a cached slot's copy
  back to back, but lost once paced: the host's BAR writes stalled when the
  link dropped to Gen2. Write-combined staging plus one copy was fastest
  both ways (recon's PERF.md has the figures). `CommandBatch::output` refuses
  a write-only slot on every device, so code that passes where its rings run
  in place also passes where they are copied; a ring of outputs, or of input
  the host parses in place, keeps `Random`.
- **A ring slot is a lease.** `claim` marks the slot held and hands out a
  `shared_ptr` whose deleter clears the mark with a release store, which
  `claim` reads with an acquire load. So a slot may be dropped on any thread
  -- a `PendingBatch` waited on another -- and what that thread did with the
  bytes happens before the claim that next returns the slot. Rejected:
  freeing a slot at `use_count() == 1`, which is a relaxed load in both
  standard libraries and orders nothing (P0521R0); a ThreadSanitizer model of
  the ring flagged it on every libc++ run, and on libstdc++ when a claim
  regrew a slot. `HostRingTest.ASlotDroppedOnAnotherThreadIsClaimedWithItsBytes`
  catches that only under ThreadSanitizer, so the Mac leg builds the ring's
  tests with it (Debug) and runs them: Apple ships libc++.
- **A result word a kernel skips shows in tests.** In a test process,
  `output` poisons every whole word of its range before the batch runs -- the
  host writes it in place, a fill of the mirror at the batch's front when
  copied -- so a sibling's tests see a kernel that leaves a result word
  unwritten, rather than an earlier frame's value. Copied, that is one more
  command, and one more barrier in a batch that would otherwise open with a
  dispatch, so a test's counts may exceed an app's. A run-time switch, which
  the test policy's `RingOutputPoison` turns on while it lives: the GoogleTest
  environment holds one, and a sibling's test that is a `main()` of its own
  holds its own, so a sibling needs no reach into the core's internals. It is
  not part of `ValidationSession`, which a measuring tool holds too (the
  player's `vp_probe`). A build option would also poison every app and probe
  built in a tree with tests, and with them the counts and GPU times a
  measurement records.
- **The masks are tested against memory topologies**: separate host/device
  heaps with and without a BAR window, unified heaps with and without private
  types, device-local carve-outs, and feature-gated memory. These cover a
  resource's placement, host-access rules and budget admission without a
  device. VMA allocation tests separately cover buffer and image reuse under
  a reduced budget; hardware coverage remains a separate requirement.

### Naming

- Repository and package `volumetric_kit_core`; namespace
  `volumetric_kit::core`. Write `namespace vkc = volumetric_kit::core;` for a
  short alias -- never `vk`, which Vulkan's C++ bindings own.
- Macros use the `VKC_` prefix (`VKC_TRY`, `VKC_ASSIGN`, `VKC_CHECK`,
  `VKC_BASE_API`); Vulkan owns `VK_`.
- Targets are `volumetric_kit::core_<tier>`, plus the umbrella
  `volumetric_kit::core`. Library files are prefixed
  (`libvolumetric_kit_core_base`), since they land in shared lib directories.
- Hardware examples and test fixtures name capabilities and memory layouts,
  not specific devices or vendors. Actual backend/API identifiers and runtime
  device diagnostics retain their names.

### Merging the three `Status`/`Result` types

The base tier is the union of `calib`'s, `recon`'s and `gfx`'s:

- **Codes:** `Ok`, `InvalidArgument`, `NotFound`, `Unsupported`, `OutOfMemory`,
  `IoError` (all three), `Numerical` (`calib`'s solver failures), and `Backend`
  (`recon`'s).
- **Backend detail is a neutral `int64_t`, tagged with its backend**
  (`recon`'s design, plus the tag). `gfx`'s `Vulkan` domain with a
  `VkResult`-typed `code()` becomes `Backend` with the `VkResult` in
  `detail()`; the vulkan tier supplies `vk_error`, `VKC_VK_TRY`, `vk_result`,
  `is_timeout` and the `VkResult` name lookup. The base tier includes no GPU
  API. `backend_error` requires the backend (`Status::Backend`: `Vulkan`,
  `Cuda`, `NvJpeg`, `Ffmpeg`, `VideoToolbox`, `Other`), because backends
  number their codes independently -- `2` is `VK_TIMEOUT`,
  `CUDA_ERROR_OUT_OF_MEMORY` and `NVJPEG_STATUS_INVALID_PARAMETER` -- and one
  call can return two backends' codes (`recon`'s CUDA interop fails with a
  `VkResult` or a `CUresult`). `vk_result` reads only a `Vulkan` status, so a
  decoder running out of memory never reads as a wait to retry. A tag, not a
  domain per backend: the domain stays what a caller branches on whichever
  library failed, and a new backend breaks no `switch` over the domains.
  `backend_error` keeps `gfx`'s guard against a success code: a detail of `0`
  (success in every listed backend) aborts via `VKC_CHECK`.
- **`with_context` adds context without losing the domain** (new). `recon`
  rebuilt a `Status` with a switch over every code to prefix a kernel's name;
  each such switch breaks when a code is added, as `Numerical` now is.
  If allocating that context fails, it returns `OutOfMemory` and leaves the
  original status unchanged. Vulkan result conversion keeps its backend and
  code even when its diagnostic cannot be allocated: losing `VK_TIMEOUT`
  would change whether the caller must keep pending work alive.
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
  A returning call on a replaced handler wakes waiters even when other calls
  remain: a handler replacing itself waits only for calls on other threads,
  leaving its own active callbacks to return afterwards.

### Whole-file I/O, and an exception as a `Status`

`recon` and `gfx` read whole files with loops of their own, and `calib`'s
capture writes a session that must survive a crash and never overwrite a
capture, so `file_io.hpp` is in `base`.

- **A write goes through a temporary file** beside the destination, flushed
  before it takes the name, so neither a reader nor a crash finds part of it.
  `Replace` renames it over the old file, which POSIX makes atomic.
  `CreateNew` hard-links it to the path, which `link` refuses when anything
  is there; `O_EXCL` on the final name would refuse too, but shows the file
  while it is written.
- **On Apple, `F_FULLFSYNC`, not `F_BARRIERFSYNC`**, for the file and then
  its directory, since Apple's `fsync` stops at the drive's cache. A barrier
  costs less but only orders the writes, so a write that returned could
  still be lost to a power loss.
- **`Replace` follows symbolic links and keeps permission bits**, as an
  editor's save does: a link to a shared file still points at it afterwards.
- **POSIX calls, not `std::filesystem`**, which cannot `fsync`. Their
  descriptors are `UniqueFd`s.
- **`exception_status(api, fallback)` and `check_path(api, path)`** are
  `recon` io's private helpers, the first with the domain a parameter, so
  `calib`'s OpenCV guard and `recon`'s io turn an exception into a `Status`,
  and refuse a path no C API can take, one way.

### One Vulkan test fixture for the family

The test support in `tests/support/` holds the policy that makes a GPU test
mean something, written once for every sibling: a missing device skips or,
under `VKC_REQUIRE_VULKAN_DEVICE`, fails; `VKC_TEST_VALIDATION` and
`VKC_TEST_SYNC_VALIDATION` turn the layer on and fail a test that runs
without it; and a `"vulkan"` error fails the running test.

- **The policy needs no test framework.** `core_test_policy` holds it for a
  test that is a `main()` of its own, as recon's are, and skips by exit code
  (77, CTest's `SKIP_RETURN_CODE`). `core_test_support`'s GoogleTest fixtures
  are built on it, and link the consumer's `GTest::gtest` only where it
  exists, so a sibling without googletest still generates its build.
- **One instance and device per process**, as gfx's fixture already shared
  them (gfx DECISIONS.md, "GPU tests share a device per process", for the
  driver costs and limits behind it): one instance per validation level, one
  device per level and equal requirements. A test makes its own objects on
  them; one it leaks is reported as the shared device is destroyed, failing
  the run rather than the test. Requirements carrying a `feature_chain`
  cannot be compared, so a fixture asking for one fails rather than share.
- **Static**, so the shared objects are the test binary's own.
- **The layer's settings through its environment variables**, which it reads
  as an instance is created: set while a fixture's instance is made, and for
  the whole run under `VKC_TEST_SYNC_VALIDATION`, so an instance a test makes
  itself (through `test::instance_config()`) runs the same checks.
  Synchronization validation goes through `VK_KHRONOS_VALIDATION_VALIDATE_SYNC`,
  or through `VK_LAYER_ENABLES` on a layer older than 1.3.268, which predates
  that setting; never both, since a layer given both honours the deprecated
  one, and newer layers warn of the mix. A synchronization session temporarily
  overrides legacy enables/disables and their environment aliases so they
  cannot suppress the requested checks; it restores the caller's values when
  it ends.
- **A fixture may ask for more** (`validation()`), met wherever the layer is
  installed -- elsewhere the test runs without it -- so barrier-heavy tests
  run under synchronization validation locally as well as in CI.
- **Cross-family tests run on `SharedDevice`'s own carving.** The layer does
  not report an `EXCLUSIVE` resource read from the wrong family, so a test of
  a producer's buffer read in place by another library needs the two
  families a product gets. `TwoFamilyDevice` (in `core_test_policy`, for
  recon's `main()` tests) adopts a headless `SharedDevice` for both libraries'
  requirements; `SharedDeviceTest` shares one per process as above, validated
  wherever the layer is installed. Both cross families exactly where
  `SharedDevice` takes `TwoFamilies`, whatever the family count; where it
  puts both libraries on one family, no product crosses there, so the
  fixture skips. Its writes on the compute queue refuse an `EXCLUSIVE`
  destination for the same reason.

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

Siblings pin a commit SHA of this repository, never `main`. An
application that fetches several siblings (`ios`) declares
`volumetric_kit_core` first, so FetchContent resolves one copy for all of
them. A public API change here lands with a CHANGELOG entry that says how to
migrate.

- **The version moves with the API.** `project(VERSION)` is the core's one
  version: the generated `version.hpp`, the soname and `vkc_require_core` /
  `vkc_core_version` all take it. Before 1.0, a PR that changes the public API
  or ABI bumps the minor version (`0.1.0` → `0.2.0`), and one that changes only
  behaviour a sibling relies on bumps the patch. The PR moves the CHANGELOG's
  `[Unreleased]` entries under the new version, so a version names one API.
  No tag marks it (amended 2026-10-28, below).
- **Each sibling requires the oldest core it builds with**, and the tiers it
  uses, by calling `vkc_require_core(<version> [VULKAN] [CONFIG])` after
  `FetchContent_MakeAvailable`. Its pin yields to an application's, so the
  check reads the version the linked `core_base` carries rather than trusting
  the request. A core too old for the sibling fails the configure instead of
  the compile deep in the sibling's sources -- or of nothing, when what
  changed is behaviour ([README.md](README.md#use-it-in-your-project) says
  what the failure names, and what a core older than 0.1.0 does). It checks
  a minimum only: before 1.0 a newer minor may break the API, which the
  sibling's next re-pin meets.
- **Consumed by commit, never installed** (2026-10-22). The core has no
  install rules or package config, and a sibling that links it PUBLIC
  installs nothing either: every application fetches the family by commit,
  no consumer ever installed a family package, and one install prefix could
  not hold siblings pinned to different minors. Third-party SDKs stay
  prerequisites installed once.

Tests and `-Werror` default ON only at the top level. The shared library's
soname carries `MAJOR.MINOR` until 1.0, as a newer minor may break the ABI.

### Public, with hosted CI first

The repository is public because `recon`, `gfx` and `ios` are public and will
fetch it at configure time. The base tier is pure C++17 with no
dependencies, so GitHub-hosted Linux and macOS runners cover it, including
sanitizers and a sibling's subproject build. Every build leg is the
same build on a different OS: Release and static, as every sibling builds
the core. None builds it shared, so nothing checks that the shared library
exports what consumers call, and none builds with `-fno-exceptions`, which no
sibling uses (`calib`'s DECISIONS.md, 2026-10-10): the core throws nothing,
and catches only to return a `Status`.

### Only the owner triggers CI

`recon`, `gfx`, `calib` and `player` are public and run legs on self-hosted
runners: persistent machines, one a Mac whose jobs run as its user. For a
`pull_request` event GitHub runs the workflow files from the pull request
itself, so a fork could send any job to those runners, and an `if:` in a
workflow stops only a fork that leaves it in.

- **GitHub enforces it, before any job.** Each family repository has an
  Actions policy whose one rule lets only the owner's account trigger a
  workflow. GitHub checks it before it creates a run's jobs, and no pull
  request can change it, so the workflows carry no fork guards and the
  runners no hook. Approval of every outside contributor's run stays on as a
  second gate.
- **Tested** on 2026-10-10 in a throwaway repository: with the owner left out
  of the rule, a push's run ended in `startup_failure` with no jobs; with the
  owner in, the same workflow ran. A real fork's pull request has not been
  tried: the account that triggers one is the fork's, which is not in the
  rule. Nor has the owner's approving or re-running a fork's run, which the
  policy may then let through.
- **The cost:** a collaborator's runs are refused until their account is added
  to every repository's rule, and a bot's are refused too: a pre-commit.ci
  autofix push or a Dependabot pull request would get no CI, so neither is
  installed and the lint job runs the hooks. A fork's change gets CI only
  once a maintainer has reviewed it, above all `.github/` and the build and
  test scripts, and pushed its branch here: from then on it runs on the
  runners as the owner's. Approving or re-running a fork's run is treated the
  same. [tools/runners/README.md](tools/runners/README.md) has the policy and
  the command.
- **The runner tooling has one copy, here,** parametrized by repository, so a
  fix reaches every repository's runners.

### A brew upgrade needs no cache clearing

The Mac runners build on the Mac's Homebrew, which only the owner changes, by
hand, while CI is idle. A build keeps absolute paths into Homebrew's Cellar,
which name each formula's version
(`/opt/homebrew/Cellar/<formula>/<version>/`): pkg-config's results in
`CMakeCache.txt`, and compiled objects' header dependencies. `brew upgrade`
deletes the old version's directory, so a tree cached before an upgrade
failed to configure, build or link until it was cleared by hand.

- **Mac caches key on the installed versions** of the formulae a leg uses and
  of their installed dependencies, recursively, and on Homebrew's prefix:
  the key `tools/runners/homebrew-key.sh` prints, which workflows get from
  `.github/actions/homebrew-key`. After an upgrade the next run misses once
  and builds on the new versions. The key has this one home; the siblings
  use the action by commit, which brings the script at that commit.
- **Versions, not Homebrew itself:** `brew update`, or an upgrade of
  formulae no leg uses, keeps every cache. Keying on every installed formula
  would rebuild everything after any upgrade, and recording only the
  version-free `opt/` links is not in a build's hands: Homebrew's pkg-config
  files name the Cellar.
- **A job on the self-hosted Mac installs nothing:** it fails, naming any
  formula missing. Installing one can upgrade installed dependencies that
  other legs build against (`HOMEBREW_NO_INSTALL_UPGRADE` spares only the
  named formula), and legs installing at once collide on Homebrew's locks;
  calib decided this first (its DECISIONS.md, 2026-10-16). GitHub-hosted
  runners, one job each, install what their image lacks.
- **The cost:** the first run after an upgrade rebuilds what it changed, and
  an upgrade during a job can still break that job, so upgrades wait until
  CI is idle ([tools/runners/README.md](tools/runners/README.md#upgrading-homebrew)).

### Platforms

The core builds for Linux, macOS, iOS and Android, with GCC or Clang
(Apple's included). Windows and MSVC are not supported: no sibling builds
there -- the family's CUDA interop is Linux, its apps are Apple's -- and no
CI leg does, so a Windows branch could only rot untested. The code assumes
POSIX and those compilers outright, with no `_WIN32` or `_MSC_VER` paths and
no MSVC flags: an exported buffer's memory is a file descriptor
(`VK_KHR_external_memory_fd`), and `Result::value()` takes its caller's line
from `__builtin_FILE`/`__builtin_LINE`. Supporting Windows would be a new
decision, landing with its CI leg.

### Vulkan headers come from the system

The vulkan tier compiles against the headers `find_package(Vulkan)` finds --
the system's, a Vulkan SDK's, or on iOS those of the MoltenVK release
`MoltenVK_ROOT` names (the 2026-10-19 entry) -- and the core vendors none.
This settles the question gfx's migration raised (2026-10-04): gfx pinned
Vulkan-Headers 1.4.357, and both in one build would have mixed two header
versions.

- **The application decides the headers, not a library.** VMA, Dear ImGui's
  Vulkan backend and volk build against what their consumer provides. A pin
  vendored here would force one version on every application that links recon
  or gfx, and mix two versions in one that also includes `<vulkan/vulkan.h>`
  from elsewhere (GLFW, ImGui). An application that wants a reproducible pin
  points `Vulkan_INCLUDE_DIR` at it.
- **The oldest supported headers are 1.3.275, Ubuntu 24.04's, on every
  platform** (raised from 1.3.204, Ubuntu 22.04's, on 2026-10-10, when recon
  and gfx dropped their 22.04 legs and nothing built on that floor any more).
  It is past Apple's own need, 1.3.208, the first with
  `VK_KHR_portability_enumeration`, without which the loader hides MoltenVK's
  devices. On Android the headers are the NDK's sysroot's, so the floor is
  NDK r27, whose are 1.3.275; no CI builds Android. `vulkan.hpp` refuses older
  headers as it compiles. At configure, the build refuses them where
  FindVulkan reports a version (CMake 3.23 and newer), from
  `VKC_VULKAN_MIN_VERSION`; a test checks the two floors agree.
  The core's CI builds on Ubuntu 26.04's headers and on macOS on Homebrew's
  current ones, and its Ubuntu 24.04 leg, like recon's and gfx's, on the
  floor itself. A newer symbol is used behind its extension's macro, as
  the portability bits are, or by its registry value, as `texel_bytes` takes
  VK_KHR_maintenance5's formats.
- **Format metadata is the core's** (`format.hpp`): `format_has_depth`,
  `format_has_stencil`, `view_aspect`, `format_needs_ycbcr_conversion` and
  `texel_bytes`, for core and KHR formats whatever headers the core was built
  with. A test checks them against Vulkan-Utility-Libraries' `vkuFormat*` for
  every format the headers name, wherever that library's headers are
  installed (macOS CI). gfx vendored that library -- and with it the newer
  headers it needs -- for five such helpers alone; with these it builds on
  the system's headers too. A new format is a table entry here, not a version
  bump; the test fails on headers that name a core or KHR format the tables
  lack.

## Open decisions

None. "Unified memory" closed with core 0.21.0 ("Where memory lives", the
`HostRing` entry).

## 2026-10-11 — One family scenario format in the core

The applications use separate JSON files for dataset, live, rendering and
mobile scenarios with one format owned here (CONFIG.md). The user chose a
single owner for typed settings, defaults and all static/cross-field checks.
The optional CPU-only `core_config` tier supplies it; `core_base` remains free
of dependencies. nlohmann/json 3.12.0 is a private implementation dependency,
using the same FetchContent name and hash as recon. No camera model, driver,
renderer or solver algorithm moves into core.

Applications select their `Application` profile, load the file, adapt typed
settings, and check files, device modes and build/backend availability at
runtime. Unknown or ignored explicit settings are errors. Every file states
its version, operation and, when present, discriminated source. Paths are
relative to the config file. Shared calibration/session artifact formats
remain with their domain owners.

This replaces the unconsumed core `Cli` proposal in PR #26. No generalized
CLI registration API is released: desktop programs accept only `--config`
and `--help`; iOS loads its bundled scenario. Existing defaults and recon's
stream-only exclusions are preserved. Siblings pin 0.3.0, enable
`VKC_WITH_CONFIG` before the first core declaration, and require its `CONFIG`
tier. A base-only build fetches no JSON dependency.

**Amended 2026-10-12 (core 0.5.0): the codec owns its validity rules.** Core
resolved each quantization table to 512 weights and checked every kept
coefficient's step: a second copy of recon's tables, coefficient order and
step bounds, free to disagree with the codec it configures. The user gave
those rules to the codec. Core keeps the table choice and the generic type
and range checks; recon maps the table to weights and refuses an
out-of-bounds step at runtime, so a scenario with one loads.

**Amended 2026-10-14 (core 0.7.0): the mesh conversion's shell bound is
recon's too.** Core refused a `shell_voxels` outside [√3/2, 4): the
conversion's own rule -- at least half a voxel's diagonal, thinner than the
truncation -- frozen at the 4-voxel band CodecMesh could not change. Once
CodecMesh takes `fusion.truncation_m`, that copy refuses valid shells and
admits invalid ones. The user gave the bound to the mesh conversion's
library in recon, which is to check it before any GPU work (today
`MeshIntegrator::integrate` checks it, after GPU setup); core keeps finite
and positive.

**Amended 2026-10-14 (core 0.7.0): a mesh source states its unit or its
height.** CodecMesh required `height_m`, the height recon scales its input
to: right for an input that declares no unit, wrong for one that does. The
schema serves generic temporal mesh sequences, of which today's is one: a
sequence either declares its unit (`metres_per_unit`) or will have its
height fitted over the whole range by the consumer. Exactly one is required,
so no scenario carries two scales to reconcile; core checks only that the
one given is finite and positive, and reads no mesh.

**Amended 2026-10-14 (core 0.7.0): `codec.entropy` goes.** The key chose
where the codec's rANS coder ran: `host`, `device` or `auto`. Recon's codec
has coded on the device alone since recon #237, so `auto` and `device` meant
the same and recon refused `host`: a choice with one answer, which the codec
owns. Core removes the key and its enum rather than keep a setting no
consumer can act on. The schema stays closed, so a scenario that still sets
it fails to load, naming it, rather than having it silently ignored.

**Amended 2026-10-14 (core 0.8.0): a mesh path is pattern text throughout.**
A CodecMesh consumer reads a printf-style frame-number pattern in
`source.path`, but core joins the scenario's directory onto a relative path
first, so a `%` in that directory was read as pattern: `50%off/` was refused
and `take%d/` loaded `take0/`. The user cannot escape a directory they never
wrote, so core escapes each `%` in it as `%%` before joining. What the user
wrote, including an absolute path, is unchanged; every `%` in the result is
pattern text, which the consumer unescapes. No other path is a pattern, so
no other is escaped.

**Amended 2026-10-16 (core 0.11.0): a profile may narrow a shared key to one
value.** CalibRig accepts `source.color_codec` only as `mjpeg`, as every
Orbbec profile accepts `source.driver` only as `orbbec`. `codec.entropy` went
because it had one answer in every profile. `color_codec` chooses between two
codings for FuseOrbbec and RigViewer, and calib_rig requests it from each
camera along with the sizes and rate, so its scenario states the coding a
capture saves. A key with one answer in every profile still goes.

## 2026-10-16 — calib's runners, and each repository's own share of a host

`calib` runs every job on self-hosted runners, registered with this
repository's runner tooling on the hosts that serve `recon` and `gfx`: its
macOS legs on the Mac (`mac`), its Linux jobs on the Linux hosts
(`vk-linux-gpu`; calib's DECISIONS.md says which job runs where). calib is
private, so a job of its on a GitHub-hosted runner draws on the account's
Actions minutes, and once they are spent, as on 2026-10-17, no such job
runs; the public repositories' hosted jobs cost nothing. A host gets as many
runners of calib as of any repository: 6 on a Linux host, enough for a run's
4 Linux jobs at once on one host while the other is offline, and 2 on a Mac.

- **A runner's parallel level is the host's cores divided by its own
  repository's runners there**: 5 on the 10-core Mac with 2 per repository.
  Setting up one repository no longer rewrites or restarts another's runners.
  This reverses 652eba2 (2026-10-10), which divided by every runner on the
  host, of every repository. A push runs one repository's CI, so its jobs
  rarely overlap another's, yet the whole-host split sized every job for the
  overlap: with three repositories' 6 runners on the Mac each job got 10 / 6
  = 1 thread, and 2 with two repositories, while the other cores idled. The
  cost: when repositories' jobs do overlap, up to 6 × 5 threads share 10
  cores, so those jobs run slower and their builds need more memory at once.
- **A build passes its level.** A bare `--parallel` ignores
  `CMAKE_BUILD_PARALLEL_LEVEL` ([tools/runners/README.md](tools/runners/README.md)),
  so the level never limited a family workflow's build step. Each
  repository's workflows pass it, in their own change; this repository's do
  from this one.
- **calib is private, yet a job of `recon` or `gfx` can read calib's
  checkouts and its runners' credentials on a host they share:** on the Mac
  every runner runs as one user, and on the Linux hosts each job container
  is given the Docker socket, which is root-equivalent. Calib's code on the
  hosts is as safe as those repositories' owner-only policies keep their
  runners ("Only the owner triggers CI").

**Amended 2026-10-17: calib is public, and only its macOS legs use the
runners.** Its hosted jobs cost nothing now, so its Ubuntu builds, lint,
sanitizers and gate run on GitHub-hosted runners, and its macOS legs on the
Mac, where its Homebrew formulae are installed and a job builds with 5
threads rather than a hosted runner's 3 (calib's DECISIONS.md, 2026-10-16).
It has no runners on the Linux hosts, which keep 6 for each of recon and gfx,
whose runs have 2 Linux jobs. The private-checkout bullet above no longer
applies.

**Amended 2026-10-18: `player` has runners on the Linux hosts and the Mac,**
as recon and gfx do, so overlapping jobs share the Mac's 10 cores among up to
8 × 5 threads.

## 2026-10-17 — A Mac's runners are system daemons

Each Mac runner is a LaunchDaemon in `/Library/LaunchDaemons` that runs as
the user who registered it, from the same runner directory, with the same
credentials and logs. It was a per-user LaunchAgent, as the runner's own
`svc.sh` installs one, and an agent lives in launchd's `gui/<uid>` domain,
which exists only while that user is logged in at the console.

- **No login, no runners.** FileVault rules out automatic login, so after a
  restart, a macOS update's included, the runners sat offline at the login
  window until someone logged in. The family's Mac has run its runners as
  daemons, converted by hand, since 2026-09-29. Registering over SSH could not
  start an agent at all: on 2026-10-17 `launchctl load` failed for calib's,
  and `launchctl bootstrap gui/<uid>` found no such domain.
- **A daemon is labelled with the runner's service name**
  (`actions.runner.<owner>-<repo>.<runner>`), the name the hand-made daemons
  carry, so setup keeps those. It is the runner's LaunchAgent template plus
  `KeepAlive`, which restarts a listener that exits, and the `HOME` and
  `USER` a login session gave the agent. An earlier setup's agent is unloaded
  and deleted: at the next console login it would start a second listener on
  the runner's credentials.
- **A restart is in place** (`launchctl kickstart -k`), and a stop waits until
  launchd has let go of the daemon: on 2026-10-17 a `launchctl bootstrap`
  straight after a `bootout` failed ("Bootstrap failed: 5") and left a
  runner down until it was loaded again by hand.
- **The cost:** setting up or tearing down a Mac's runners needs `sudo`, as on
  Linux, and their jobs run outside any login session, with no window server
  or unlocked login keychain. The Mac legs need neither: on daemons, calib's
  rig leg ran its 408 tests, GPU tests on MoltenVK included, with none
  skipped.

## 2026-10-19 — iOS links MoltenVK statically, set up by the core

An iOS build takes MoltenVK's iOS release, a static library, as its Vulkan,
with no loader: iOS has no system loader, and the ios app already links
MoltenVK this way on a device. `cmake/vkc_ios.cmake` sets the build up. The
root CMakeLists includes it in the vulkan tier's block when
`CMAKE_SYSTEM_NAME` is iOS, so in an application that declares the core first
("Consumers pin, and an application declares the core first") it runs before
recon's and gfx's finds of Vulkan and glm.

- **One home for the family's iOS setup.** Each app carried it in a toolchain
  file and its framework lists; the ios repository already holds two copies.
  The core cannot ship a toolchain file instead: CMake reads one at the first
  `project()`, before FetchContent has fetched the core. So an application
  configures with CMake's own iOS support and nothing else
  ([README.md](README.md#prerequisites)). Rejected: each application's preset
  passing `Vulkan_LIBRARY` and `Vulkan_INCLUDE_DIR`, which copies the
  release's layout, the framework list and glm into every application, with
  no floor.
- **One knob, `MoltenVK_ROOT`**: the release's `MoltenVK/` directory, a
  prerequisite installed once outside every repository, read from `-D` or
  else the environment, as recon reads `OrbbecSDK_ROOT`, and else taken from
  the last configure, since Xcode re-runs CMake without the shell's
  environment. The module derives `Vulkan_LIBRARY` and `Vulkan_INCLUDE_DIR`
  from it, FORCEd so that a new root takes effect, and `core_vulkan` links
  PUBLIC the system frameworks the library needs, so an app names only its
  own. It refuses a release older than 1.4.2, which fixes 1x1 drawables after
  a swapchain rebuild; the floor rises in step with Homebrew's molten-vk,
  which backs the macOS legs.
- **glm from the host's headers, header-only.** recon and gfx find
  `glm::glm`, but an iOS build searches packages in the SDK only, and
  Homebrew's `glm::glm` is a macOS dylib, which an iOS link refuses. The
  module answers `find_package(glm)` itself, with an INTERFACE `glm::glm` on a
  directory of the build that holds only a link to the `glm/` found. Not
  Homebrew's shared `include/`, whose macOS Vulkan headers must not reach an
  iOS compile beside MoltenVK's; and not the keg the link resolves to, which
  `brew upgrade` deletes without an Xcode build noticing. A missing glm fails
  the `find_package` that needs it, so a build without glm configures.
  Rejected: fetching glm, a download and a second pin beside gfx's fallback.
- **No validation on the device.** The release has no validation layer, and
  there is no loader to insert one. The macOS legs stand in: they run every
  device test under validation on Homebrew's MoltenVK, kept at the iOS
  release's version.
- **Tested at configure** on macOS, against stubs of the release and of
  Homebrew's glm (`tests/ios_consumer/`): the root, the Vulkan entries, the
  floor, the frameworks, glm and the messages are all decided there. The
  real compile and link remain the siblings' iOS steps on the self-hosted
  Mac; moving core's builds there does not change its configure-only iOS
  coverage. The runners get `MoltenVK_ROOT` from setup
  ([tools/runners/README.md](tools/runners/README.md#macs)).

## 2026-10-23 — The core's build legs use the family's GPUs

Release/static builds run on the self-hosted `vk-linux-gpu` and `mac`
runners, as the owner requested after the generic runner setup landed.
Ubuntu 24.04 and 26.04 run in matching containers with `--gpus all` and
`NVIDIA_DRIVER_CAPABILITIES=all`; the Mac runs natively through MoltenVK.
Why: the allocator and timer need the physical driver's behavior, including
separate device and host memory, which hosted lavapipe does not exercise.

- **Validation stays required:** device presence, the Khronos layer,
  synchronization validation, and shader-access checks where supported.
  The four test shards, the CMake tests on Ubuntu 26.04, the iOS configure
  tests on the Mac, and every leg's base-only dependency check stay intact.
- **A container sets both parallel levels to 4,** because the runner's
  `.env` does not enter it, and keeps the driver's shader cache under the
  persistent runner tool cache. The Mac uses the levels setup writes.
- **The Mac only checks dependencies.** Its Homebrew is shared; installs and
  upgrades happen by hand while the host is idle, as the runner guide says.
  Checkout leaves no token in the shared host's repository configuration.
- **Lint, sanitizers and the required gate stay hosted.** Sanitizers retain
  lavapipe and validation; moving the build legs adds physical GPU coverage
  without removing that independent check.

## 2026-10-27 — The Orbbec profiles default to H.265 colour at 3840x2160 and 25 fps

`Source`'s colour defaults move from 1280x720 at 30 fps to 3840x2160 at
25 fps (core 0.25.0); the codec stays H.265 and depth 640x576. recon's frame
prep keeps each camera's colour at the camera's own size, and the textures
are drawn from it, so the largest colour mode gives the finest texture. The
struct is the one home of every Orbbec profile's modes: FuseOrbbec and
RigViewer take them, and CalibRig's block keeps only its MJPEG colour. The
rig's cameras stream 3840x2160 colour at 25, 15 and 5 fps only, and depth at
the colour's rate, so the whole rig runs at 25 Hz: 40 ms per set. A scenario
that states another mode still gets it.

**The colour codec follows the purpose** (the owner, 2026-10-27): H.265 for
reconstruction, MJPEG for calibration captures.

- **H.265 for FuseOrbbec and RigViewer.** At 3840x2160 and 25 fps a camera
  sends about 21 Mbit/s of H.265 against 185 of MJPEG (recon's 2026-09-28
  H.265 entry), and recon decodes it on NVDEC or VideoToolbox, leaving the
  picture on the device (its 2026-10-06 decoder entry). recon decodes JPEG
  on hardware only, so a GPU without a hardware JPEG decoder cannot
  reconstruct from MJPEG at all. Decoded as BT.601 full range, H.265 gives
  the camera's JPEG colour (per-channel gains of 0.996 to 0.999 on a still
  view at 1280x720), and the two modes' profiles (intrinsics, distortion,
  depth extrinsic) agree byte for byte, so poses calibrated on MJPEG
  captures are taken for H.265 frames; the measurements below check that
  the two modes' images sit on the same pixels.
- **H.265 only for CalibColor** (core 0.26.0). It fits each camera's colour
  response for reconstruction, so it must fit the decode that reconstruction
  sees. It narrows `source.color_codec` to `hevc` (the 2026-10-11 entry's
  0.11.0 amendment), as CalibRig narrows it to `mjpeg`.
- **MJPEG for CalibRig's captures.** A JPEG frame is self-contained, which an
  H.265 frame is not, and a capture saves each camera's own JPEG (the
  2026-10-11 entry's 0.11.0 amendment). At 3840x2160 and 25 fps the JPEG
  spends about 0.9 bit per pixel against H.265's 0.1, which the camera fixes
  (the SDK sets a JPEG quality but no H.265 rate), so a capture keeps the
  least distorted image a board is detected in.
- MJPEG stays selectable on FuseOrbbec and RigViewer for diagnostics, such
  as `diagnostics.dump_sets`, which requires it.

**Measured on the rig at these defaults** (four cameras at 3840x2160 H.265
and 25 fps with depth, fusion and the geometry stream on, 180 s a run;
another process held 7.4 GB of the GPU's memory and stayed idle):

| | exposure automatic | exposure locked |
| --- | --- | --- |
| published sets per second | 24.99 | 24.97 |
| sets missing a camera | 8.6 % | 7.2 % |
| arrival to published, p95 / max | 32.4 / 68.5 ms | 19.9 / 64.1 ms |
| fusion's GPU memory at peak | 2.7 GB | 2.8 GB |
| hardware decoder use, mean | 19 % | 20 % |
| network receive | 707 Mbit/s | 704 Mbit/s |

- The runs are camera-limited: at p95 a set takes 0.42 ms of frame
  preparation, 2.0 ms of integration and 1.8 ms of encoding on the device.
- Lost frames and latency vary from run to run (a camera lost 12 to 54
  frames a run), so a comparison against these numbers needs a noise
  allowance.
- A camera's H.265 and MJPEG images sit on the same pixels: over a 3x3 grid
  of image regions the largest shift is 0.09 px between 60-frame means and
  0.15 px between single frames, against a 0.2 px allowance.
- The camera sends an H.265 key frame every 25 frames (1 s), so a frame lost
  before decoding costs that camera's colour until the next key.

**What 25 Hz changes for the consumers.**

- A scenario that states `intra_period` 30 gets a key every 1.2 s instead of
  every 1 s; one that wants 1 s states 25. A live viewer does not wait for a
  key: FuseOrbbec's serve makes the next set a key when a viewer joins,
  40 ms away instead of 33.
- Dynamic fusion reaches `max_weight` 1.2 times later in seconds. At 1 m
  from one camera, where an observation weighs 1, the default 20 spans
  0.8 s instead of 0.67 s.
- A camera's stored sync settings record the rate of the last stream it ran.
  recon's sync check (`sync_differences`) compares no rate, so a camera last
  run at 30 fps opens at 25 with no refusal.

## 2026-10-28 — Pins are SHAs, floors read `project(VERSION)`, and no tags; one floor check for every sibling

Amends "Consumers pin, and an application declares the core first", which
tagged each version's merge commit `v<version>`. That was never done: the
core went from 0.1.0 to 0.26.0 with no tag and no release, every sibling
pins a SHA, and nothing reads a tag. A floor reads the version the code
carries, which `project(VERSION)` sets.

- **Pins are SHAs.** A pin names the one commit a build takes, and its
  version is whatever that commit's `project(VERSION)` says.
- **Each sibling's floor goes through the core, with
  `vkc_require_sibling(<umbrella> <min-version> [TARGETS <target>...])`**
  (core 0.27.0). recon is splitting into packages that build on each other,
  and an application's first declaration of a package wins over every later
  one, so each needs the floor check the core has. A copy of
  `vkc_require_core` per repository would be one more copy of one rule.
  - **The version rides the umbrella target**, as `VKC_PACKAGE_VERSION`: the
    umbrella exists in every build of a package, whatever its options, and a
    target reaches every directory of the build where a variable does not.
  - **The package's name follows from its umbrella**: the umbrella
    `volumetric_kit::<name>` belongs to `volumetric_kit_<name>` (AGENTS.md,
    naming), so a refusal names the pin to move and its
    `FETCHCONTENT_SOURCE_DIR_<NAME>`, as the core's does.
  - **A listed target that is missing is refused** as a tier the core lacks
    is: options yield to the first declaration as pins do, so a sibling built
    without the target fails at configure, not in the compile.
- **`vkc_require_core` keeps reading `core_base`'s `VKC_CORE_VERSION`** and
  shares the version check and its message with `vkc_require_sibling`.
  recon's and calib's tests stamp an older version on `core_base`, and
  calib's stand-in core has no umbrella.

## 2026-10-28 — Exported GLSL through the core's shader build

A package exports the GLSL other projects' shaders include with
`vkc_export_glsl(<target> PREFIX <include-prefix> FILES <file>...)`, and a
consumer's shaders reach it with `INCLUDE_TARGETS` on `vkc_compile_shaders`
and `vkc_embed_shaders` (core 0.27.0). It generalises the export recon
built for `pinhole.glsl` (its 2026-10-16 entry, "A shared GLSL pinhole"),
which its coming packages need between each other.

- **The root holds the exported files alone**, a copy in the build tree, so
  a consumer's kernel cannot include a package's internal GLSL, such as a
  layout the package keeps private. A file the package stops exporting
  leaves the root at the next configure, once its directory is done, so a
  stale copy cannot keep a kernel compiling until a clean build.
- **The copy is made at configure** (`configure_file`), which CMake redoes
  when a source changes, and the kernels that include it then recompile
  through their depfiles. A build-time copy would need a dependency from each
  consumer's shader rule on another package's target.
- **The root rides the target**, in `VKC_SHADER_INCLUDE_DIRS`, as recon's
  `VR_SHADER_INCLUDE_DIR` did, so it reaches a project that fetches the
  package. `INCLUDE_TARGETS` reads it at the call, so the exporting target
  is defined first, as a dependency already is.
- **An include path has one exporter in a build**: a second is refused, as
  a second symbol is, since the first root on the include path would win.
