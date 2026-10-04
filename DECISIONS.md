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
| `vulkan` | `base` | instance, device create/adopt, allocator, buffers, images, descriptors, shaders, compute pipelines, command batches, external memory, the shared-device bootstrap | before `calib` writes GPU code |
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
- **One discriminator.** `Result` holds a `std::variant<T, Status>`, so
  whether it is OK and which alternative it holds cannot disagree.
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

- **GPU CI for the vulkan tier.** `recon`'s GPU runners (`vk-linux-gpu`,
  `mac`) are registered per repository, so they must be registered here before
  the vulkan tier lands. On a public repository those legs must run only
  same-repository code, as `recon`'s guard does.
- **Unified memory.** `recon` deliberately runs the staged path on Apple too,
  pending a staging measurement on the iPad. The vulkan tier inherits that
  rule until the measurement says otherwise.
