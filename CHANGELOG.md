# Changelog

All notable changes to `volumetric_kit_core` are documented here. The format
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html). Before
1.0, a minor release may break the API; each such entry says how to migrate.

## [Unreleased]

## [0.23.0] - 2026-10-26

### Fixed

- Allocator buffer, image and exported-buffer creation retains every partial
  Vulkan/VMA resource until its returned owner's deleter is constructed.
  Host allocation failure returns `OutOfMemory`, frees partial resources and
  leaves the allocator usable for retry.
- Synchronization and device submission APIs contain host allocation failures,
  including validation diagnostics and user recording callbacks. A failed
  recording returns its command for reuse without submitting partial work.
- `Status::with_context` returns `OutOfMemory` if copying or prefixing its
  diagnostic fails, leaving the original status unchanged. `vk_error` keeps
  the Vulkan backend and result code with an empty message if only its
  diagnostic cannot be allocated, preserving timeout and device-loss handling.
- Timing-result allocation failure does not fail work that already completed.
  Pending-submit destruction still waits if a warning cannot be allocated.
- Device command storage preallocates an emergency resource holder, so an
  unconfirmed completion at destruction can retain its owners even under
  persistent host allocation failure.

### Migration

- Bump the pin and rebuild consumers: the private `Device` layout changes to
  hold preallocated resource owners. Code that caught recording-callback
  exceptions now checks the returned status (`OutOfMemory` for
  `std::bad_alloc`, `InvalidArgument` for other exceptions).

## [0.22.0] - 2026-10-25

### Added

- `HostRingDesc::host_access`: how the host touches a ring's slots,
  `HostAccess::Random` by default. A `HostAccess::SequentialWrite` ring's
  slots, which the host fills and never reads, take write-combined memory
  where their shape's mapped memory has an uncached type -- system RAM
  staging where the ring is copied. Whether a ring runs in place stays the
  device's fact (`HostRing::in_place`).
- `HostSlot::memory_info`: the memory type a slot's mapping took.

### Changed

- `CommandBatch::output` refuses a slot of a `HostAccess::SequentialWrite`
  ring (`InvalidArgument`) on every device, before the batch holds the slot
  or poisons its range.
- iOS builds target iOS 26.0: the configure lines in README.md and
  `cmake/vkc_ios.cmake`, and the iOS configure tests, pass
  `-DCMAKE_OSX_DEPLOYMENT_TARGET=26.0`.

### Migration

- None: bump the pin. A ring that does not set `host_access` keeps
  `HostAccess::Random`, as before. A ring of inputs the host only writes may
  set `HostAccess::SequentialWrite`; binding one of its slots as an output is
  then refused.

## [0.21.0] - 2026-10-24

### Added

- `HostRing`, `HostSlot` and `HostRingDesc` (`vulkan/host_ring.hpp`):
  persistent slots for the bytes a stage moves between host and device every
  frame, placed once and grown only by a claim too big for every free slot.
  In place where device-mapped memory is cached (`HostRing::in_place`),
  copied through a device-only mirror elsewhere: the device's memory decides.
  A slot is a lease, which may be dropped on any thread.
- `CommandBatch::input` and `CommandBatch::output`: bind a slot's range and
  return the `VkBuffer` the kernels bind. Copied, an input records one copy
  where it is called, and an output's copy is recorded after every other
  command. While a test holds a `test::RingOutputPoison`, as the core's
  GoogleTest fixtures do, an output's range holds `test::kRingOutputPoison`
  before the batch runs.
- `PhysicalDeviceInfo::device_mapped_cached`: whether a `DeviceMapped`
  buffer may take `HostAccess::Random`.
- `DeviceCounters::dispatches` and `DeviceCounters::barriers`: the dispatches
  and pipeline barriers the device's batches record.
- `tools/runners/`: `--user <account>` sets up or tears down runners that run
  as another account, in its home, its files written as it through `sudo -u`
  ([tools/runners/README.md](tools/runners/README.md#a-dedicated-account)).

### Changed

- `DeviceCounters::staging_buffers` also counts each ring slot placed, at
  create and when a claim grows one.
- `tools/runners/`: `setup-runners.sh` and `teardown-runners.sh` take any
  repository in full, as `owner/repo`, as well as the family's short names.
  Its runners go in `~/ci-runners/<repo>`, named `<host>-<repo>-<i>`, with the
  same labels and services ([tools/runners/README.md](tools/runners/README.md)).
- CI: the Mac leg runs the `HostRing` tests built with ThreadSanitizer, and
  each leg names the ring path its device must run (`VKC_EXPECT_RING_PATH`).

### Fixed

- Overlapping `CommandBatch::output` ranges synchronize their copies back to
  the same host slot. Disjoint output ranges still share one barrier.
- Moved-from `HostSlot` handles return a null mapping and zero capacity.
- `HostRing::create`, `HostRing::claim`, `CommandBatch::input` and
  `CommandBatch::output` return allocation failures as `OutOfMemory`; a failed
  recording poisons the batch so it cannot submit partial work.

### Migration

- Nothing breaks: bump the pin. Bytes a call moves every frame -- a frame's
  input, the results the host decides on -- move from `upload`/`readback` to
  a `HostRing` slot bound by `input`/`output`; a kernel that writes an
  output must write every word the host reads, which a test shows by holding
  a `test::RingOutputPoison`. A test that pins `staging_buffers` counts its
  rings' slots at their create; one that pins `barriers` counts the poison's
  (`CommandBatch::output`).

## [0.20.0] - 2026-10-22

### Added

- Busy, blocked and device time per row: `StageRow::wait_ms`, the part of
  `cpu_ms` spent blocked, and `StageRow::device` (`DeviceTime`: `None`,
  `Measured`, `NotTimed`, `Unavailable`), after the first four fields. Every
  blocking wait in the core reports to the scopes open on its thread
  (`note_blocked`); `WaitScope` times a wait as a row of its own;
  `StageMetrics` gains `add_wait`, `add_device`, `merge_device` and
  `total_wait_ms`.
- Tiled device time: a timed batch writes a timestamp before its first
  command, at each change of tag and after its last, and `GpuTiles` does the
  same for a command buffer the caller submits itself. `CommandBatch::time`
  times a batch in a given timer; `GpuTimer::set_max_depth` folds deeper
  scopes into their ancestor.
- `CommandBatch::submit_async` is timed: `PendingBatch::device_rows` and
  `PendingBatch::device_span` (`DeviceSpan`) after the wait.
- `Device::counters` (`DeviceCounters`: submits, host waits, blocked time,
  staging buffers) and `Device::calibrate` (`ClockPair`); a device enables
  `VK_KHR_calibrated_timestamps`, else `VK_EXT_calibrated_timestamps`, where
  offered.

### Changed

- A tile counts to its tag's scope and to every scope open around it when
  the command was recorded, so a stage's device time holds its fills,
  copies, barriers and breakdowns, and `total_gpu_ms` skips breakdown rows.
- A timer's queries go per batch, so it times any number of pending batches.
  A batch that finds no free range, or fails before the device or on it,
  reads `NotTimed`; one the device may still run keeps its range, and the
  timer times on. `GpuTiles::drop(true)` also retains the pool across timer
  growth or destruction until device teardown drains its assigned queue.
- `GpuSpanTag::scope` is the scope's serial (`StageScope::serial`), unique
  in the process.
- A `StageScope` or `WaitScope` opens and closes on one thread: closing one
  on another is a programmer error (`VKC_CHECK`).

### Removed

- `GpuTimer`'s per-span API: `begin`, `end`, `cmd_reset_ahead`, `settle`,
  `resolve`, `discard`, `abandon`, `report_into`, `reset`, `count` and
  `kNoSpan`.

### Migration

- Code reading `has_gpu` builds and behaves as before; read `device` to tell
  n/t from n/a, and show busy time as `cpu_ms - wait_ms`. A row built
  positionally keeps its meaning; set `device` beside `has_gpu`.
- Drop any subtraction of breakdown rows from `total_gpu_ms`, and expect a
  stage's device time to include its nested scopes'.
- A scope on any set, open around tagged work, now gets that work's device
  time, as it gets the waits inside it. Drop any loop that copies a
  library's device rows into the row around its call (as the player's
  `StreamPlayer::decode` adds recon's `gpu_ms` to "decode"): that row holds
  them already, and the loop counts them twice.
- Fold an async batch's rows in after its wait, into one set, once:
  `metrics.merge_device(pending.device_rows())`. They name every scope its
  tiles count to, whichever set opened it.
- Close each scope on the thread that opened it.
- A timer used by hand moves to `GpuTiles`: `begin(timer, cmd, tiles)`,
  `mark(cmd, name)` at each tile, `end(cmd)`, `resolve()` once the command
  buffer's fence has signalled (a timeline value is not enough through
  MoltenVK), then `rows()` and `span()`. Submit these command buffers on the
  queue of the device that created the timer.

## [0.19.0] - 2026-10-22

### Added

- `volumetric_kit::core_test_main` (test support): GoogleTest's main, for a
  suite to link in place of `GTest::gtest_main`. On Linux a death test's child
  dumps no core, so a host that pipes dumps to a crash reporter (apport) no
  longer holds each death test about a second (README.md, "Use it in your
  project").

### Removed

- The install rules and the package config: `find_package(volumetric_kit_core)`,
  `VKC_INSTALL` and `vkc_require_core`'s `PACKAGE` mode. The family consumes
  the core by commit only (DECISIONS.md, "Consumers pin, and an application
  declares the core first").

### Migration

- Add the core with FetchContent (or `add_subdirectory`) at a pinned commit.
  A sibling that installs targets linking the core PUBLIC drops its install
  rules and package config with its next pin bump: CMake can no longer
  generate their export, and `vkc_require_core(... PACKAGE ...)` fails as a
  usage error. `set(VKC_INSTALL ...)` is ignored.

## [0.18.0] - 2026-10-22

### Added

- `Device::pipeline_cache()`: every device, created or adopted, makes one
  `VkPipelineCache` and destroys it with itself, in memory only.
  `KernelSetBuilder` builds every kernel through it, so a kernel that several
  objects build on one device is compiled once; `ComputePipelineDesc::cache`
  (default none) passes a cache to `ComputePipeline::create`.

### Migration

- Nothing to change beyond the pin; a pipeline made another way may pass
  `device.pipeline_cache()` to `vkCreate*Pipelines`.

## [0.17.0] - 2026-10-21

### Added

- `config::Application::Player`, `config::SourceKind::Stream` (`stream`) and
  `config::Source::host` and `port`: volumetric_kit_player's view of a
  stream file or of a live sender (CONFIG.md). Its `view` scenario requires
  a `stream` source with exactly one of `path` and `host`, and takes `port`
  beside `host`, `window` `width`, `height` and `validation`, and
  `run.frames`. It refuses every other field and section. Other profiles
  refuse a `stream` source, `host` and `port`.

### Migration

- volumetric_kit_player requires core 0.17.0 and loads its scenario with
  `Application::Player`. A `switch` that names every `Application` or
  `SourceKind` gains a case for the new value; existing scenarios load
  unchanged.

## [0.16.0] - 2026-10-21

### Added

- `config::Output::serve` (`output.serve`, a port, default 0) for
  `Application::FuseOrbbec`, so that fuse_orbbec can serve its coded frames
  live on that port, on every interface (CONFIG.md); 0 serves nothing. A
  port turns coding on, as `csv` or `stream` does, so a `codec` section with
  only `output.serve` loads. Other profiles refuse it.

### Migration

- A FuseOrbbec consumer (recon: fuse_orbbec) requires core 0.16.0 and
  honours `output.serve`, or refuses a nonzero one. Existing scenarios load
  unchanged.

## [0.15.0] - 2026-10-20

### Changed

- `Application::FuseOrbbec` resolves `fusion.dynamic` true, as `RigViewer`
  does; `config::Fusion::dynamic` and every other profile stay false.

### Migration

- A FuseOrbbec scenario that wants Classic integration, such as a static
  scan, sets `fusion.dynamic` false; a consumer that reads the resolved
  setting (recon: fuse_orbbec) otherwise fuses Dynamic.

## [0.14.0] - 2026-10-20

### Added

- `Application::FuseOrbbec` takes RigViewer's `fusion.free_after`,
  nonnegative with default 30 (CONFIG.md), so that fuse_orbbec can fuse a rig
  with Dynamic integration as rig_viewer does, freeing the blocks a surface
  has left.

### Removed

- `config::Fusion::accumulate` (`fusion.accumulate`), added in 0.13.0: every
  set fuses into the grid so far. A scenario that still sets it, to any
  value, now fails to load: `fusion.accumulate: unknown field`.

### Migration

- A FuseOrbbec consumer (recon: fuse_orbbec) requires core 0.14.0 and
  honours `fusion.free_after`, or refuses it. A consumer that honours it
  frees stale blocks every 30 sets by default; 0 frees none, as before.
- Delete `fusion.accumulate` from every scenario, and code that reads
  `config::Fusion::accumulate`. A rig scenario that set it false, to follow a
  changing scene, sets `fusion.dynamic` true instead, as rig_viewer fuses.
  Other scenarios load unchanged.

## [0.13.0] - 2026-10-20

### Added

- `Application::FuseOrbbec` takes the `codec` section, `output.csv` and
  `output.stream` in operation `fuse`, so that fuse_orbbec can code each
  fused set as one frame (CONFIG.md). Its codec fields are those the codec
  profiles share, with their defaults and checks. Refused: a `codec` section
  without `csv` or `stream`, and a `csv` or `stream` that resolves to
  `output.path`, the PLY.
- `config::Fusion::accumulate` (`fusion.accumulate`, default true) for
  FuseOrbbec: false clears the grid before each set, so that each set is
  reconstructed on its own. Other profiles refuse it.

### Changed

- An `output.path`, `csv` or `stream` that resolves to `source.sync_config`
  or `source.calibration` is refused, so that a run cannot overwrite the rig
  files it reads.

### Migration

- A FuseOrbbec consumer (recon: fuse_orbbec) requires core 0.13.0 and
  honours `fusion.accumulate`, the `codec` section and both outputs, or
  refuses them. Existing scenarios load unchanged, unless an output names a
  rig file.

## [0.12.0] - 2026-10-19

### Added

- iOS builds link MoltenVK's iOS release statically, set up by the core
  (`cmake/vkc_ios.cmake`; DECISIONS.md, the 2026-10-19 entry). From
  `MoltenVK_ROOT` it sets (FORCEd) `Vulkan_LIBRARY` and `Vulkan_INCLUDE_DIR`,
  refuses a release older than 1.4.2, and links MoltenVK's system frameworks
  into `core_vulkan`; it answers `find_package(glm)` with the host's glm
  headers, header-only. The configure line is in README.md, "Prerequisites".
- `tools/runners/setup-runners.sh` writes the shell's `MoltenVK_ROOT` into
  each runner's `.env` ([tools/runners/README.md](tools/runners/README.md#macs)).

### Changed

- `tools/runners/`: `setup-runners.sh` and `teardown-runners.sh` take
  `player`, like `recon` and `gfx`, on the Linux hosts and the Mac.

### Migration

- An iOS application re-pinned to 0.12.0 declares the core first and
  configures with CMake's iOS variables and `MoltenVK_ROOT`, with no
  toolchain file. It drops its toolchain's Vulkan seeds, its list of
  MoltenVK's frameworks, and any glm declaration or override for iOS. A
  sibling configured for iOS at its own top level adds the core before it
  finds Vulkan or glm. Desktop builds are unchanged.

## [0.11.0] - 2026-10-16

### Added

- `config::Application::CalibRig` and `config::Operation::Calibrate`
  (`calibrate`): calib_rig's live view and capture (CONFIG.md). It requires
  `source.sync_config` and `output.path`, its session root, with no default,
  and takes `source.driver`, `color_codec`, the colour and depth sizes and
  `fps` (default MJPEG 3840x2160, 640x576, 25 fps) and `window.width`,
  `height` and `validation`. Refused: `color_codec` `hevc`,
  `window.overlay`, and every other field and section. Other profiles refuse
  `calibrate`.
- `.github/actions/homebrew-key` and `tools/runners/homebrew-key.sh`: a cache
  key of the installed Homebrew versions a build uses, so a `brew upgrade`
  on the Mac starts the affected dependency builds fresh (DECISIONS.md, "A
  brew upgrade needs no cache clearing"). Migrating, in `recon`, `gfx` and
  `calib`: add the key to the Mac legs' `build/_deps` cache keys, and
  install nothing on the self-hosted Mac
  ([tools/runners/README.md](tools/runners/README.md#upgrading-homebrew)).

### Changed

- `tools/runners/`: a Mac's runners are system LaunchDaemons, which start at
  boot with nobody logged in, rather than LaunchAgents, so `setup-runners.sh`
  and `teardown-runners.sh` call `sudo` on a Mac too, and work over SSH
  (DECISIONS.md, the 2026-10-17 entry). The re-run below moves a Mac's
  LaunchAgent runners to daemons, and keeps daemons already installed under
  the runners' service names.
- `tools/runners/`: `setup-runners.sh` and `teardown-runners.sh` take
  `calib`, like `recon` and `gfx`. A repository's runners split the host's
  cores among themselves alone, and setting one repository up leaves the
  others' runners as they are (DECISIONS.md, the 2026-10-16 entry). On a
  host set up before, run `setup-runners.sh` once for each of its
  repositories, while CI is idle.
- CI's build steps pass the parallel level rather than a bare `--parallel`,
  which ignores it ([tools/runners/README.md](tools/runners/README.md)).

### Migration

- calib_rig requires core 0.11.0 and loads its scenario with
  `Application::CalibRig`. A `switch` that names every `Application` or
  `Operation` gains a case for the new value; existing scenarios load
  unchanged.

## [0.10.0] - 2026-10-16

### Added

- `test::TwoFamilyDevice` (`core_test_policy`,
  `volumetric_kit/core/testing/two_family_device.hpp`): a headless
  `SharedDevice` adopted by a compute library and a renderer with their own
  requirements, an allocator each, `families()` for a `CONCURRENT` resource,
  and `write_buffer` / `write_image` on the compute queue, fence-waited.
  `crosses_families()` is true exactly under `QueuePlan::TwoFamilies`.
- `test::write_image` (`core_test_policy`,
  `volumetric_kit/core/testing/write_image.hpp`): fills mip 0, layer 0 of a
  color image from host texels on any device's queue, leaves the whole image
  in a layout its usage allows, and records it.
- `test::SharedDeviceTest` (`core_test_support`, `vulkan_fixture.hpp`): the
  GoogleTest fixture on it, one per process for each validation level and pair
  of requirements, validated wherever the layer is installed; it skips where
  the shared device has one family (DECISIONS.md, "One Vulkan test fixture for
  the family"). It hides `VulkanTest`'s instance and physical device, which
  are not the two-family device's.

### Changed

- The fixtures' skip without a device opens with "no Vulkan device for this
  test: ", which a CTest `SKIP_REGULAR_EXPRESSION` can match.

### Migration

- Nothing changes for existing tests. A sibling's cross-family test requires
  core 0.10.0 and moves its own `SharedDevice` setup onto these: recon's
  `sensor_apple_pixel_buffer_importer_test` and `sensor_picture_readback_test`
  onto `TwoFamilyDevice`, and the fill in `test_image::make` onto
  `test::write_image`.

## [0.9.0] - 2026-10-16

### Added

- `config::Codec::update_size` (`codec.update_size`, default 0) for
  `Application::CodecReplica` and `CodecMesh`: the P-frame update-segment
  size in entries, 0 meaning the codec's own default. Refused: a value that
  is not an integer in [0, 2^32 - 1], naming `codec.update_size`, and any
  size under `codec.inspect_only` or with `codec.intra_period` 1, which codes
  no P-frame; the codec checks its range.

### Migration

- Consumers of the setting require core 0.9.0 and map a nonzero size to their
  encoder (recon: `EncoderConfig::update_size`), or refuse one; existing
  scenarios load unchanged.

## [0.8.0] - 2026-10-14

### Added

- `config::Codec::intra_period` (`codec.intra_period`, default 1) for
  `Application::CodecReplica` and `CodecMesh`: an I-frame every
  `intra_period` frames, zero-motion P-frames between them. Refused: a value
  that is not an integer in [1, 2^32 - 1], naming `codec.intra_period`, and
  any period under `codec.inspect_only`.

### Changed

- A relative CodecMesh `source.path` is joined to the scenario's directory
  with each `%` in that directory escaped as `%%`, so a `%` there is no
  longer read as pattern (DECISIONS.md, the 2026-10-11 entry's amendments).
  Absolute paths and every other path are unchanged.

### Migration

- Consumers of the setting require core 0.8.0 and map it to their encoder
  (recon: `EncoderConfig::intra_period`), or refuse a period other than 1;
  existing scenarios load unchanged.
- A CodecMesh consumer reads each `%%` in `source.path` as a literal `%`.

## [0.7.0] - 2026-10-14

### Added

- `config::Run::first` (`run.first`, default 0) for `Application::CodecMesh`,
  which also accepts `run.frames` now: frames `first` to
  `first + frames - 1` of a numbered mesh sequence. CodecMesh's `frames` is
  an exact count, default 1, where a dataset's is a limit. Refused: a
  negative `first`, `frames` below 1, and a last number past INT32_MAX,
  naming `run.frames`; other profiles refuse `run.first`. A frame-number
  pattern in `source.path`, such as `%05d`, is the consumer's to read: core
  resolves the path and leaves `%` alone.
- `config::Source::metres_per_unit` (`source.metres_per_unit`, CodecMesh):
  the input's declared unit, finite and positive. It is the alternative to
  `source.height_m`: exactly one of the two is required.
- `config::Source::allow_holes` (`source.allow_holes`, default false,
  CodecMesh): signed mode admits a mesh whose only defect is boundary edges.
  Refused with `sdf_mode` `shell`, even when false.
- CodecMesh accepts `fusion.truncation_m`, with the shared sentinel (absent
  or nonpositive is four voxels) and 127-block bound.
- Mesh inspection (`codec.inspect_only`) accepts `run` and either scale, and
  refuses `allow_holes`, as it refuses the other SDF settings.

### Changed

- CodecMesh's `source.shell_voxels` only needs to be finite and positive,
  where core refused one outside [0.8660254, 4); the bound is the consumer's
  (DECISIONS.md, the 2026-10-11 entry's amendments).
- A CodecMesh scenario with neither `height_m` nor `metres_per_unit` is
  refused naming `source`, where 0.6 named `source.height_m`.

### Removed

- `config::Entropy` and `config::Codec::entropy` (`codec.entropy`;
  DECISIONS.md, the 2026-10-11 entry's amendments). A scenario that still
  sets `codec.entropy`, to any value, now fails to load:
  `codec.entropy: unknown field`.

### Migration

- Consumers of the new settings require core 0.7.0. A default-constructed
  `Run` has `first` 0; `Source` has `metres_per_unit` 0 and `allow_holes`
  false. A CodecMesh scenario without `codec.entropy` loads unchanged and
  resolves one frame from number 0.
- A CodecMesh consumer reads `metres_per_unit` as well as `height_m`: a
  scenario that gives the unit leaves `height_m` 0, where every CodecMesh
  config 0.6 loaded had a positive `height_m`.
- A CodecMesh consumer must refuse at runtime what core now loads: a `run`
  range with no pattern in `source.path` to number, and a `shell_voxels`
  outside its conversion's bound, since core loads any finite, positive one.
- Delete `codec.entropy` from every scenario, and code that reads
  `config::Codec::entropy` or names `config::Entropy`; the codec behaves as
  `auto` and `device` did.

## [0.6.1] - 2026-10-12

### Fixed

- `vkc_require_core` no longer recurses until CMake stops it when one
  configure reads the core's package more than once: a project that finds the
  core, or adds it from source, and then finds a sibling whose package config
  re-finds it (#31). `cmake/vkc_require_core.cmake` now defines its commands
  once per configure, and a copy older than 0.6.1 read after it no longer
  breaks them. Such a copy read twice still recurses. A package config that
  reached the core only through another sibling's can now find it directly
  with `find_dependency(volumetric_kit_core 0.6.1 CONFIG)`: the full version
  refuses a 0.6.0 install before its config is read, where `0.6` would accept
  it.

## [0.6.0] - 2026-10-12

### Added

- `config::Output::csv` and `config::Output::stream`, exposed as `output.csv`
  and `output.stream` for `Application::CodecReplica` and `CodecMesh`: the
  codec's per-frame rows and its coded frames, in one file each. Paths are
  relative to the config file, an empty string disables each, and equal
  resolved paths are refused, naming `output.stream`. Mesh inspection and
  other profiles reject both. Core checks neither the files nor the stream's
  format.

### Changed

- `fusion.dynamic` is accepted by every dataset and Orbbec profile
  (`FuseReplica`, `CodecReplica`, `FuseRender`, `FuseViewer`, `FuseOrbbec`
  and `RigViewer`), not by `RigViewer` alone. `config::Fusion::dynamic` now
  defaults to false (Classic); `RigViewer` still resolves true. Existing
  scenario files need no changes.

### Removed

- `config::Codec::quantization_weights`: core no longer resolves the
  quantization table to weights or checks each kept coefficient's step
  (DECISIONS.md, the 2026-10-11 entry's amendment).

### Migration

- Consumers of the new settings require core 0.6.0. Code that
  default-constructs `Config` or `Fusion` and relies on `dynamic == true`
  sets it.
- Code that read `codec.quantization_weights` derives the weights from
  `codec.quantization_table` and checks the steps itself (recon:
  `CodecParams::validate()`): a scenario whose steps are out of bounds now
  loads.

## [0.5.0] - 2026-10-12

### Added

- `config::Diagnostics::dump_sets` (default 0, off) and `dump_directory`, as
  `diagnostics.dump_sets` / `dump_directory` for `Application::FuseOrbbec`
  stream and fuse runs; rules in CONFIG.md. Consumers require core 0.5.0;
  existing scenario files need no changes.

## [0.4.0] - 2026-10-11

### Added

- `config::Diagnostics::readback`, exposed as `diagnostics.readback` in
  version-1 scenarios for `Application::FuseOrbbec`. The boolean defaults to
  false and is accepted in both stream and fuse operations. Other application
  profiles reject it, including an explicitly supplied false value.
  Consumers of the new setting require core 0.4.0 and read
  `settings.diagnostics.readback`; existing scenario files need no changes.

## [0.3.0] - 2026-10-11

### Added

- Optional CPU-only `core_config`: the family's version-1 scenario JSON,
  public typed settings, application defaults and static validation. Includes
  bounded reads, strict fields/discriminators, duplicate/numeric overflow
  checks, config-relative paths and presence-sensitive operation exclusions.
- `vkc_require_core(... CONFIG)` checks the optional target. Base-only builds
  remain independent of JSON; installed config headers expose no JSON types.

### Changed

- Replaces PR #26's unconsumed `Cli`/`parse_number` proposal. Consumers enable
  `VKC_WITH_CONFIG`, require core 0.3.0 and link `core_config`; load the scenario
  for their `Application` profile instead of sharing option registration.
  Desktop bootstrap accepts `--config PATH` / `--help`. Parsing uses
  nlohmann/json with scoped nearest rounding; defaults, field presence and
  validation stay private to decoding. See CONFIG.md.

## [0.2.0] - 2026-10-11

### Added

- `base/file_io.hpp`: `read_file(path)`, `write_file(path, bytes, WriteMode)`
  and `create_new_directory(path)`, which fail with a `Status`, and
  `check_path(api, path)`, which refuses an empty path or an embedded NUL. A
  write goes through a temporary file flushed to the storage device, so
  neither a reader nor a crash finds part of it: `WriteMode::Replace` renames
  it over the file, keeping its permission bits and writing through a
  symbolic link, and `WriteMode::CreateNew` refuses anything already at the
  path. When recon and gfx pin 0.2.0, their whole-file reads move onto
  `read_file`, and recon's `io::detail::check_path(api, path)` becomes
  `check_path(api, path)`.
- `base/exception_status.hpp`: `exception_status(api, fallback)`, the
  exception being handled as a `Status`: `OutOfMemory` for a
  `std::bad_alloc`, `fallback` for anything else. recon's
  `io::detail::exception_status(api)` becomes
  `exception_status(api, Status::Code::IoError)`.

### Changed

- `UniqueFd` moves from `vulkan/external_memory.hpp` and `core_vulkan` to
  `base/unique_fd.hpp` and `core_base`, which owns descriptors too now.
  `external_memory.hpp` still includes it, so no consumer changes.
- `core_base` catches exceptions, to return them as a `Status`, so it no
  longer compiles with `-fno-exceptions`, which no sibling uses.

## [0.1.0] - 2026-10-08

### Fixed

- `vkc_embed_shaders` refuses a symbol another call in the build already
  emits. Two targets embedding shaders of one name under one `SYMBOL_PREFIX`
  used to configure and link: the linker merged their arrays into one, while
  each target kept its own size, so one dispatched the other's SPIR-V. Give
  each library a prefix of its own; no sibling's shaders clash today.
- `vk_result` returns empty for a backend status whose detail is outside
  `int32_t`, `VkResult`'s range, instead of converting it to `VkResult`. That
  conversion was undefined and in practice truncated the detail, so a failure
  whose detail was 2^40 read as `VK_SUCCESS`. A backend status therefore no
  longer always yields a `VkResult`: check the optional before dereferencing
  it. A 32-bit code stored without sign extension (from a `uint32_t`) is
  outside the range too; store a `VkResult` with `vk_error`.
- A log handler that throws out of a failed `VKC_CHECK`, in a build with
  exceptions, no longer leaves the thread's next failed check reading the
  first one's destroyed message and bypassing the handler: every failed check
  reports through the handler.
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

- `check_timeline_points(device, wait, signal, call, waits)` and
  `note_timeline_signals(signal)` (`sync.hpp`): the timeline-value refusals
  `Device::submit_pending` and `CommandBatch::submit_async` make, and their
  record of the values a submit that reached a queue sets, for a library that
  submits to the queue itself. `TimelineWaits::Reached` conservatively
  refuses unfinished timeline waits feeding presentation: a submitted signal
  may still depend on an unsubmitted signal, including through earlier queue
  work. The initial `Submitted` spelling remains an alias, with this stricter
  behavior. Callers using that policy must wait for the producer's value or
  retry after it is reached; prefer the `Reached` spelling. The check covers
  the named timeline points, not binary waits or other queue dependencies.
  The core's own submits keep `Any` and their asynchronous behavior.
- `tools/runners/`: the self-hosted runner scripts `recon` and `gfx` each kept
  a copy of, in one copy for both: `setup-runners.sh <recon|gfx>` for Linux
  hosts and Macs, and `teardown-runners.sh`
  ([tools/runners/README.md](tools/runners/README.md)). Migrating, in `recon`
  and `gfx`: delete `.github/setup-*-runner.sh`, `.github/teardown-runners.sh`
  and `.github/self-hosted-runners.md` in favour of these, the fork guards in
  favour of the repository's Actions policy (DECISIONS.md, "Only the owner
  triggers CI"), and the `ci:` block of `.pre-commit-config.yaml`, since that
  policy refuses pre-commit.ci's pushes. On a host set up before these
  scripts, run `setup-runners.sh` once, for either repository, so every
  runner on it gets its share of the cores.
- `vkc_require_core(<version> [VULKAN] [PACKAGE <package>])`, which fails the
  configure when the core a project got is older than `<version>`, or with
  `VULKAN` lacks the vulkan tier, naming what it found and how to fix the pin
  (DECISIONS.md, "Consumers pin, and an application declares the core
  first") -- or, with `PACKAGE` in that package's config, leaves the package
  not found, as `find_dependency` does -- and
  `vkc_core_version(<var> [MAJOR_MINOR])`, which reads the core's version.
  Both are defined once the core is added or found, and read the
  `VKC_CORE_VERSION` that `volumetric_kit::core_base` now carries, built here
  or installed. In the re-pins, recon, gfx and calib call `vkc_require_core`
  after `FetchContent_MakeAvailable(volumetric_kit_core)` with the oldest
  version they build with, recon and gfx adding `VULKAN`: it replaces recon's
  `core_vulkan` check in `third_party/CMakeLists.txt`, and gfx's
  `cmake/vg_core.cmake` and the `format.hpp` probe in its package config.
  Each package config calls it after `find_dependency(volumetric_kit_core)`,
  with `PACKAGE` and its own package name. Each replaces the block in its
  `src/CMakeLists.txt` that derives the MAJOR.MINOR its package config
  re-finds the core at with `vkc_core_version(<var> MAJOR_MINOR)`.
- `vulkan`: `is_timeout(status)`, whether a wait ran out of time with the work
  still pending -- a `Vulkan` status carrying `VK_TIMEOUT`, as the waits of
  `Fence`, `TimelineSemaphore`, `Device::PendingSubmit` and `PendingBatch`
  return. Use it in place of `vk_result(status) == VK_TIMEOUT`.
- `volumetric_kit::core_test_policy` and `volumetric_kit::core_test_support`,
  the test support for the family's Vulkan tests: the policy
  `VKC_REQUIRE_VULKAN_DEVICE`, `VKC_TEST_VALIDATION` and
  `VKC_TEST_SYNC_VALIDATION` set, for a test that is a `main()` of its own,
  and GoogleTest fixtures on it. README.md ("Use it in your project") says how
  a sibling adopts them. `VKC_TEST_SYNC_VALIDATION` now sets the layer's
  settings itself, without `VK_LAYER_ENABLES` or
  `VK_VALIDATION_SYNCVAL_SHADER_ACCESSES_HEURISTIC`.
- `CommandBatch::submit_async(wait, signal)`: submit a batch without waiting
  for it. Nothing it records starts before every `TimelinePoint` in `wait`
  (a `TimelineSemaphore` and a value) is reached, and it sets those in
  `signal` once it completes, so a pipeline's stages chain on the device
  rather than each waiting on the host. It returns a `PendingBatch`: `ready`
  polls, `in_flight` says whether the device may still run the work, and
  `wait` writes the readbacks and frees the staging -- refusing the batch if
  a dispatched set was rewritten while it was pending. Destroying an
  unfinished one waits for it with no limit, warning after a second of a
  value it still waits for, so set every value the host owns on every path.
  Submit a batch that sets a value before any that waits for it: on the
  device's one queue, a batch waiting for a value only a later one sets
  hangs the queue. Its commands run untimed. Everything the batch recorded
  must stay alive and unchanged until the work completes. A refused value
  leaves the batch unsubmitted, to submit again with a corrected one.
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
  on and reach the log sink.

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
- CI on GitHub-hosted runners: one static Release build each on Ubuntu 24.04
  and 26.04 and macOS, ASan/UBSan/LSan in Debug, and the lint gate, behind
  one required check. A push to `main` never cancels another commit's run.
- Formatting and lint: pinned clang-format and cmake-format hooks that fix
  files in place, hygiene checks (YAML, merge-conflict markers, large files,
  line endings), `.editorconfig`, and clang-tidy (`.clang-tidy`, pinned
  22.1.8) run through the build with `-DVKC_CLANG_TIDY=ON`, locally and in CI.

### Changed

- The version is 0.1.0 (was 0.0.1), and from here on moves with the API
  (DECISIONS.md, "Consumers pin, and an application declares the core
  first"). The soname becomes `0.1`: rebuild consumers after bumping their
  pin. An installed sibling's `find_dependency(volumetric_kit_core 0.0)` no
  longer accepts it; the re-pinned sibling's own config asks for `0.1`.
- **`Status::backend_error` takes the backend whose call failed**
  (DECISIONS.md, "Merging the three `Status`/`Result` types"):
  `backend_error(detail, message)` → `backend_error(Status::Backend::<b>,
  detail, message)`, with `<b>` one of `Vulkan`, `Cuda`, `NvJpeg`, `Ffmpeg`,
  `VideoToolbox` and `Other`. `Status::backend()` returns it (empty for any
  other domain), `to_string(Status::Backend)` names it, and misusing an error
  `Result` prints it (`Backend Cuda 2: …`). `vk_result` returns empty for a
  status from any backend but `Vulkan`. `vk_error` sets `Vulkan`, so Vulkan
  failures built with it, and callers of `vk_result` on them -- `gfx`'s
  `swapchain_stale`, `ios`'s `RendererErrors.mm` -- need no change. The calls
  to migrate, in `recon`'s
  `src/volumetric_kit/recon/sensor/video/` (none in `gfx`, `calib` or `ios`):
  - `cuda_pictures.cpp`, `cuda_error` (a `CUresult`): `Cuda`.
  - `jpeg_decoder.cpp`, `nvjpeg_error` (an `nvjpegStatus_t`): `NvJpeg`.
  - `ffmpeg.cpp`, `ffmpeg_error` (`AVERROR_EXTERNAL`): `Ffmpeg`.
  - `vt_jpeg.cpp`, its fallback for an unclassified `OSStatus`:
    `VideoToolbox`.
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
- `vulkan`: **the oldest supported Vulkan headers are 1.3.275**, Ubuntu
  24.04's, on every platform (DECISIONS.md, "Vulkan headers come from the
  system"); on Android, take NDK r27 or newer. Older headers fail at configure
  where FindVulkan reports a version (CMake 3.23 and newer) -- in the build,
  and in the installed package's `find_dependency` -- and otherwise in
  `vulkan.hpp`, with a message naming the version needed. On Ubuntu 22.04,
  point `Vulkan_INCLUDE_DIR` at a Vulkan SDK's headers.
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
  `vk_error(result, message)`. `vk_error`, `VG_VK_TRY` and
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
- `recon` / `gfx`: `Status::backend_error(backend, 0, …)` (a success code)
  now aborts, as `gfx`'s `Status::error(VK_SUCCESS, …)` did; test the call's
  result first.
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

[Unreleased]: https://github.com/taojin-6/volumetric_kit_core/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/taojin-6/volumetric_kit_core/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/taojin-6/volumetric_kit_core/releases/tag/v0.1.0
