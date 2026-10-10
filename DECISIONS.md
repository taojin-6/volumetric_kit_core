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
- `camera` starts with the rational model (k1–k6, p1, p2), matching the
  factory calibration format that `recon`'s calibration file stores.
  Fisheye (Kannala-Brandt) is planned, not built: the interface
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
| V4 (landed) | `QueryPool`, `GpuTimer`, `GpuStageScope`, `StageMetrics` (in `base`), timed `CommandBatch` commands and submits; `create_exported_buffer`, `UniqueFd`, `find_memory_type`, `CommandBatch::release` |
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
  later, so the planned switch to volk for iOS and Android stays inside
  `vulkan.hpp`. A runtime deleter would add storage to every handle, and
  traits keyed on the handle type cannot tell `VkFence` from `VkSemaphore` on
  32-bit targets, where both are `uint64_t`.
- **Vulkan from the system** (`find_package(Vulkan)`, `Vulkan::Vulkan`
  PUBLIC), as recon and ios use it, and gfx too once it builds on this tier
  ("Vulkan headers come from the system", below).
- **`VKC_WITH_VULKAN` is opt-in for a subproject.** ON at the top level, OFF
  when fetched: recon and gfx set it, and calib, which fetches the core for
  its error types, then needs no Vulkan installed. An installed core ships the
  tier's headers only with its library, and re-finds Vulkan only then.
- **CI without GPUs, but not without devices.** The hosted Linux legs run the
  device tests on lavapipe with `VKC_REQUIRE_VULKAN_DEVICE=1`, so a missing
  device fails instead of skipping; the sanitizer job adds the validation
  layer (`VKC_TEST_VALIDATION=1`), so ASan, UBSan, LSan and validation check
  the same run. Under it, a test fails if validation is off or does not reach
  the log sink, and the run fails on errors the layer reports as the shared
  devices and instances are destroyed ("One Vulkan test fixture for the
  family", below).

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
- **One path between host and kernel memory.** `CommandBatch` is it: uploads
  inline up to 64 KiB and staged beyond, readbacks through one host buffer,
  barriers only where a command could see another's writes, a refusal that
  poisons the batch, and a descriptor set rewritten after its dispatch
  refused at submit. Kernel memory is never mapped, so unified memory runs the
  path a discrete GPU does (the open "Unified memory" question below).
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
- **Timer spans came with V4** (below): recon's optional `GpuStageScope*`
  parameters are back as trailing defaults.
- **One shader toolchain.** `vkc_compile_shaders` and `vkc_embed_shaders`
  replace recon's `vr_*`, gfx's `vg_*`, and the copy of recon's ios borrows.
  `TARGET_ENV` (default Vulkan 1.2; gfx's renderer passes 1.3),
  `INCLUDE_DIRS` (recon's include root) and `SPIRV_VAL_ARGS` (recon's
  `--scalar-block-layout`) cover the differences. `SYMBOL_PREFIX` is
  required: the arrays are inline variables, which the linker merges by name,
  so two libraries' `fill.comp` would otherwise share one shader. The
  functions are defined when the core is added, and installed beside the
  package config, which includes them. Each rule belongs to one target --
  `vkc_embed_shaders` compiles and embeds under one -- as a rule two targets
  reach races under Make and is refused by Xcode. Headers and symbols are
  named for the file name made a C identifier, and two names that make one
  are refused. An INTERFACE library shares one compile among the targets
  that link it. A shader that fails `spirv-val` fails every build until
  fixed: Make deletes a failed command's output, and Ninja and Xcode rerun
  it.

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
- **gfx's `QueryPool`, recon's `GpuTimer` on it.** The pool owns the handle
  and range-checks its commands. It holds timestamp and occlusion queries
  only, one 64-bit result each, which is what it reads. The timer is recon's
  -- fence-blocked spans read the moment a submit returns, unavailable rather
  than failing where a family has no timestamps. gfx's frames-in-flight
  profiler stays in gfx, rebuilt on these.
- **`GpuStageScope` is how work is timed.** `CommandBatch`'s uploads,
  copies and dispatches and `dispatch` take recon's trailing
  `GpuStageScope* = nullptr`, so recon migrates by namespace. A command keeps
  the scope's `GpuSpanTag` (timer, label, scope id), not the scope, so a
  scope that closes before the submit leaves the command untimed rather than
  dangling. The `Device` overload takes a `GpuStageScope&`: a nullable
  pointer beside the untimed overload's `keep_alive` would let
  `submit_single_time(record, nullptr)` pick the timed one.
- **A span is read after its own submit, and published by its own scope.**
  recon's `resolve` read every unresolved span, so a submit nested in
  another's recording read the outer span before its command buffer ran --
  unreset queries in a first window, the last frame's value after -- and a
  closing scope published every span on the timer, a helper's scope taking
  its stage's. `GpuTimer::settle` resolves exactly the spans one submit
  carried, and a scope publishes the spans opened under it; the window ends
  when the last open scope closes.
- **A failed submit drops its spans, or retires its timers, as recon did.**
  `Device::submit_single_time` reports `in_flight`, whether a failed call
  left work the device may still run. Spans whose work never reached the
  device are dropped, a `record` that throws included, and the timer keeps
  timing; work in flight retires its timers (`abandon`), which still publish
  what they resolved. A timer's query pool rides the submit's `keep_alive`,
  so the device holds it past a failed wait, as it holds a batch's staging.
- **One query reset a timer a batch.** A batch resets each timer's queries
  once, ahead of its spans, not between every two commands, where a reset
  can split MoltenVK's compute encoder and perturb what is measured.
- **At most 2048 spans a window.** MoltenVK backs a timestamp pool with a
  32 KiB Metal counter sample buffer -- 4096 timestamps -- and emulates
  timing past it, so recon's 4096-span ceiling became 2048. A `reserve` past
  it is clamped, logged, and a stage that loses a span to a full window
  publishes no device time: a partial sum would read as the whole stage's.

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
  and the `keep_alive` -- a batch's staging and readback memory -- and give the
  command back to the device once a wait sees the fence signalled. A failed
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
- **A pending batch's commands are untimed.** A span is read after its own
  submit, and while a batch is pending, a later window on its timer may reset
  the queries the batch will write. The stage's device row is missing, not
  wrong, until spans get queries a later window cannot reset.
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
  `CommandBatch::readback`.
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
- **`PhysicalDeviceInfo::unified_memory`** tells the architectures apart:
  every heap is device-local, so every type is (the spec sets
  `DEVICE_LOCAL` on a type exactly when its heap has it). An APU whose driver
  reports a VRAM carve-out beside host memory is not unified: the core
  follows the driver's own accounting, so kernel data fills the carve-out
  and fails when it is full, as VRAM does, rather than land in host memory
  the driver does not call device-local. A library may branch on it; the
  core's own paths do not yet (the open "Unified memory" question; a
  `TODO:` in `StorageInput::buffer` marks where).
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
- **Backend detail is a neutral `int64_t`** (`recon`'s design). `gfx`'s
  `Vulkan` domain with a `VkResult`-typed `code()` becomes `Backend` with the
  `VkResult` in `detail()`; the vulkan tier supplies `vk_error`, `VKC_VK_TRY`
  and the `VkResult` name lookup. The base tier includes no GPU API. Which
  backend set the detail is not recorded (an open decision below).
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
  A returning call on a replaced handler wakes waiters even when other calls
  remain: a handler replacing itself waits only for calls on other threads,
  leaving its own active callbacks to return afterwards.

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
- **From source only.** Neither target is installed or exported, so an
  installed core carries no googletest. Static, so the shared objects are the
  test binary's own.
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
shared-library build with `-fno-exceptions`, sanitizers, and both ways of
consuming the package.

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
the system's, or a Vulkan SDK's -- and the core vendors none. This settles
the question gfx's migration raised (2026-10-04): gfx pinned Vulkan-Headers
1.4.357, and both in one build would have mixed two header versions.

- **The application decides the headers, not a library.** VMA, Dear ImGui's
  Vulkan backend and volk build against what their consumer provides. A pin
  vendored here would force one version on every application that links recon
  or gfx, and mix two versions in one that also includes `<vulkan/vulkan.h>`
  from elsewhere (GLFW, ImGui). An application that wants a reproducible pin
  points `Vulkan_INCLUDE_DIR` at it.
- **The oldest supported headers are 1.3.204** -- Ubuntu 22.04's -- **and
  1.3.208 on Apple**, the first with `VK_KHR_portability_enumeration`, without
  which the loader hides MoltenVK's devices. On Android the headers are the
  NDK's, so the floor is NDK r25, the first to ship Vulkan 1.3's; older NDKs
  ship 1.2's. `vulkan.hpp` refuses older headers as it compiles. At configure,
  the build and the installed package's `find_dependency` refuse them where
  FindVulkan reports a version (CMake 3.23 and newer), from one
  `VKC_VULKAN_MIN_VERSION`. The core's CI builds on neither floor: on Ubuntu
  24.04's and 26.04's headers, and on macOS on Homebrew's current ones.
  recon's and gfx's Ubuntu 22.04 legs build the vulkan tier on 1.3.204, so
  code that needs newer headers fails there when they bump their pin, not in
  the core's own CI. A newer symbol is used behind its extension's macro, as
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

- **Discrete GPU CI for the vulkan tier.** Hosted Linux uses lavapipe;
  macOS requires a Vulkan device. Both require validation, with synchronization
  checks enabled and shader-access checks requested where supported by the
  layer. No discrete-memory runner is registered
  here yet. `recon`'s GPU runners
  (`vk-linux-gpu`, `mac`) are registered per repository, so they must be
  registered here too. They were to come before the allocator (V2), whose
  memory placement differs on a discrete GPU; V2 landed without them, so the
  discrete-GPU path -- `DeviceOnly` in VRAM, staging in system RAM -- is
  tested only through the memory-type masks, against recorded layouts. On a
  public repository those legs must run only same-repository code, as
  `recon`'s guard does.
- **Unified memory.** `recon` deliberately runs the staged path on unified
  memory too, pending measurements of staging cost. The vulkan tier inherits that
  rule until the measurement says otherwise. On unified memory a staged
  upload costs one GPU copy within the same DRAM; the alternative,
  `DeviceMapped` memory the host writes in place and a kernel reads as a
  `StorageInput` device buffer, now exists. Which inputs take it, and on
  which architecture (`unified_memory()`), is the open part.
- **Which backend a `Backend` status came from.** `Status` records the domain
  and an `int64_t` detail, not the backend that set it, so `vk_result` reads a
  CUDA status's `cudaError_t` as an unrelated `VkResult`:
  `cudaErrorMemoryAllocation` (2) as `VK_TIMEOUT`, which a caller that retries
  on `VK_TIMEOUT` would retry. Until then, `vk_result` is asked only of a
  status from a Vulkan call. Recording it changes the base tier's API: a
  backend tag set by `backend_error`, or a domain per backend (`Vulkan`,
  `Cuda`); either lets `vk_result` return empty for a CUDA status. Decide at
  `recon`'s migration, whose CUDA interop returns both kinds.
