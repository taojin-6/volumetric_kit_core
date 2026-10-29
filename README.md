# volumetric_kit_core

The shared foundation of the `volumetric_kit` family. `calib` (calibration),
`recon` (reconstruction), `gfx` (rendering) and `ios` (the iOS app) each kept
their own copy of the same bottom layer -- error handling, then a Vulkan core
-- and the copies drifted. This repository holds one copy that every sibling
depends on, so a fix lands once and the libraries can hand each other GPU
buffers zero-copy on one shared device.

```
               volumetric_kit_core
              /        |         \
          calib      recon       gfx
              \        |         /
                      ios
```

> **Status:** the `base` tier is implemented and tested, and calib and recon
> build on it. The `vulkan` tier is implemented -- the instance, device
> selection against a library's requirements, the logical device every sibling
> creates or adopts, the VMA allocator with its buffers and images,
> descriptors, synchronization, compute kernels, `CommandBatch` (waited on, or
> ordered by timeline semaphores and waited on later), the shader build
> functions, GPU timers, buffers and images exported for CUDA to write, and
> the shared device a compute library and a renderer both adopt. Camera
> models and sensors are not core tiers: they stay in recon and calib
> ([DECISIONS.md](DECISIONS.md#tiers)).

[AGENTS.md](AGENTS.md) is the shared working guide for contributors, Codex and
Claude Code; `CLAUDE.md` imports it.

## Tiers

| Tier | Target | Holds |
| --- | --- | --- |
| `base` | `volumetric_kit::core_base` | `Status`/`Result`, `VKC_CHECK`, `exception_status`, logging, stage metrics (busy, blocked and device time), whole-file I/O, version |
| `config` | `volumetric_kit::core_config` | Optional CPU-only family scenario format, typed settings, defaults and static validation; private nlohmann/json dependency |
| `vulkan` | `volumetric_kit::core_vulkan` | instance, device selection, device create/adopt and submission, allocator, buffers, images, descriptors, shaders, sync, command buffers, compute kernels, `CommandBatch`, persistent host rings (`HostRing`), shader build functions, tiled GPU timers and calibrated clocks, external memory, shared-device bootstrap |

`volumetric_kit::core` links every tier that is built. A consumer that needs no
GPU (calib's headless solver) links only the tiers it uses.

## Scenario configuration

[CONFIG.md](CONFIG.md) defines the shared JSON format. Enable `VKC_WITH_CONFIG`
(default ON at the top level, OFF as a dependency), link `core_config`, and
load a scenario for the consuming application. `core_base` stays independent
of JSON, Vulkan and the config tier.

## Prerequisites

The `base` tier needs only a C++17 compiler -- GCC or Clang; Windows and MSVC
are not supported ([DECISIONS.md](DECISIONS.md#platforms)) -- and CMake ≥
3.21. googletest is fetched, pinned, when tests are built. The `vulkan` tier
also needs a Vulkan SDK's headers and loader (MoltenVK on macOS; iOS below)
-- headers 1.3.275 or newer
([DECISIONS.md](DECISIONS.md#vulkan-headers-come-from-the-system)) -- and its
tests a GLSL compiler (`glslc`, or `glslangValidator`); `spirv-val`
validates the shaders when found:

```sh
brew install vulkan-headers vulkan-loader molten-vk shaderc spirv-tools  # macOS
sudo apt-get install libvulkan-dev mesa-vulkan-drivers glslc spirv-tools # Ubuntu
```

`VKC_WITH_VULKAN` builds it. It defaults ON at the top level and OFF in a
subproject: a sibling that uses the tier sets it before fetching the core, so
one that does not (calib's headless solver) needs no Vulkan installed.

On iOS the tier links MoltenVK statically, with no loader, from MoltenVK's
iOS release (`MoltenVK-ios.tar`, 1.4.2 or newer, from
<https://github.com/KhronosGroup/MoltenVK/releases>), installed once outside
every repository, by convention at
`<workspace>/third_party/MoltenVK_ios_v<version>/MoltenVK`; glm comes from
the host's headers (`brew install glm`). An application that adds the core
first configures with CMake 3.24 or newer and its own iOS support, with no
toolchain file, and names that directory with `-DMoltenVK_ROOT`, which the
build remembers and prefers, or with the environment, read at each
configure:

```sh
cmake -S . -B build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0 \
  -DMoltenVK_ROOT=<workspace>/third_party/MoltenVK_ios_v1.4.2/MoltenVK
```

`cmake/vkc_ios.cmake` does the rest (DECISIONS.md, the 2026-10-19 entry).
On macOS, the tier's tests also need full Xcode with its iOS platform, since
some configure for iOS.

## Build and test

```sh
core_root="$(git rev-parse --show-toplevel)"
cmake -S "$core_root" -B "$core_root/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$core_root/build" --parallel
ctest --test-dir "$core_root/build" --output-on-failure
```

`VKC_BUILD_TESTS` and `VKC_WARNINGS_AS_ERRORS` default ON only when this is
the top-level project. `VKC_SANITIZE` is a semicolon list, empty by default,
e.g. `-DVKC_SANITIZE="address;undefined"`; in a subproject build it also
reaches the consumer's targets that link the core.

The vulkan tier's device tests run on the best device present and skip when
there is none. Three environment variables tighten them, and CI sets all
three:

- `VKC_REQUIRE_VULKAN_DEVICE=1` fails a device test that has no device, so a
  runner cannot pass by skipping;
- `VKC_TEST_VALIDATION=1` enables the Khronos validation layer and fails any
  test that triggers a validation error, or that runs without validation;
- `VKC_TEST_SYNC_VALIDATION=1` adds synchronization validation and, where
  the layer supports them, shader-access checks;
  `tests/testing_vulkan_fixture_test.cpp` checks that each reports a hazard.

Synchronization sessions temporarily override legacy layer enable/disable
environment variables and restore them on exit.

Release CI runs on the family's physical GPUs: Ubuntu 24.04 and 26.04 in
containers on the Linux GPU hosts, and MoltenVK on the Mac. The Linux legs
exercise discrete DRAM/VRAM memory placement. Sanitizers still run on hosted
Linux with lavapipe; lint and the required gate also stay hosted
([tools/runners/README.md](tools/runners/README.md)). The Mac leg also runs
the `HostRing` tests built with ThreadSanitizer, the one check of a ring
slot's threading rule (DECISIONS.md, "Where memory lives").

`Allocator::memory_stats()` reports each heap's usage against its budget
(`usage_bytes`, `budget_bytes`) beside this allocator's own share: its
reserved blocks (`reserved_bytes`) and live allocations (`allocation_bytes`).
Only the allocator's own figures may be summed across sibling allocators;
with `VK_EXT_memory_budget`, a heap's usage already includes every allocation
in the process.

## Use it in your project

Pin a commit SHA -- never `main` -- so every consumer builds the same core,
and require the oldest version your project builds with. The core installs
nothing: a project builds it from the commit it pins (DECISIONS.md, "Consumers
pin, and an application declares the core first").

```cmake
include(FetchContent)
FetchContent_Declare(
  volumetric_kit_core
  GIT_REPOSITORY https://github.com/taojin-6/volumetric_kit_core.git
  GIT_TAG <sha>)
FetchContent_MakeAvailable(volumetric_kit_core)
vkc_require_core(0.1.0)

target_link_libraries(your_target PRIVATE volumetric_kit::core_base)
```

`vkc_require_core` fails the configure, naming the version found, the version
required and how to move the pin, when the core is older, as one an
application declared first (below) may be. A core older than 0.1.0 has no
`vkc_require_core`, so the call stops as an unknown command: move the
first-declared pin to 0.1.0 or newer. For the vulkan tier, set
`VKC_WITH_VULKAN` ON before `FetchContent_MakeAvailable`, require it with
`vkc_require_core(0.1.0 VULKAN)` and link `volumetric_kit::core_vulkan`.
`vkc_embed_shaders` then compiles a target's GLSL and embeds the SPIR-V as
headers (`cmake/vkc_shaders.cmake` documents its options):

```cmake
vkc_embed_shaders(your_target SYMBOL_PREFIX your_ SHADERS shaders/integrate.comp)
# your sources: #include "integrate_comp.spv.hpp" -> your_integrate_comp_spv[]
```

A package whose GLSL other projects' shaders include exports it with
`vkc_export_glsl`, and their shaders reach it through `INCLUDE_TARGETS`:

```cmake
vkc_export_glsl(your_library PREFIX your/shaders FILES shaders/common.glsl)
# a consumer, whose kernel.comp has #include "your/shaders/common.glsl":
vkc_embed_shaders(their_target SYMBOL_PREFIX their_
                  INCLUDE_TARGETS your::library SHADERS shaders/kernel.comp)
```

A sibling's own floor works as the core's: the sibling sets
`VKC_PACKAGE_VERSION` on its umbrella target (`volumetric_kit::<name>`) to its
`project(VERSION)`, and a project that adds it calls
`vkc_require_sibling(volumetric_kit::<name> <min-version> [TARGETS <target>...])`
after `FetchContent_MakeAvailable`, which refuses an older sibling, or one
without the umbrella or a listed target
(`cmake/vkc_require_sibling.cmake`).

A consumer that links the core into a shared library or framework builds the
core shared too (`BUILD_SHARED_LIBS=ON`): the log handler is process-global,
and each binary linking a static core gets its own (DECISIONS.md, "One
instance per process").

Libraries submitting directly use `check_timeline_points` before submission
and `note_timeline_signals` after success (`vulkan/sync.hpp`). For timeline
waits feeding presentation, `TimelineWaits::Reached` requires each value to
be reached already: a recorded pending signal does not prove its upstream
dependencies are submitted. Wait for the producer or retry later. The
initial `Submitted` spelling is an alias with the same stricter behavior;
ordinary core submissions keep `Any` and may wait for future values. Binary
waits and other queue dependencies remain the submitter's responsibility.

A sibling's tests apply the variables above through the core's test
support, which a sibling gets by setting `VKC_BUILD_TEST_SUPPORT` ON before
`FetchContent_MakeAvailable`.

- A GoogleTest suite provides googletest (`GTest::gtest`), links
  `volumetric_kit::core_test_support`, and derives from `test::VulkanTest`
  (an instance and a physical device), `test::VulkanDeviceTest` (plus a
  device and an allocator) or, for a cross-family test, `test::SharedDeviceTest`
  (a compute library's and a renderer's devices on two queue families). The
  tests in a process share the instances and devices; a two-family device
  brings an instance of its own
  (`volumetric_kit/core/testing/vulkan_fixture.hpp`).
- A GoogleTest suite links `volumetric_kit::core_test_main` in place of
  `GTest::gtest_main`, so that on Linux a death test's child dumps no core: a
  host that pipes core dumps to a crash reporter (Ubuntu's apport) otherwise
  holds each death test about a second.
- A test that is a `main()` of its own links
  `volumetric_kit::core_test_policy`, which needs no googletest, and returns
  `test::kSkipExitCode` (77) to skip: register that as its `SKIP_RETURN_CODE`
  (`volumetric_kit/core/testing/vulkan_policy.hpp`). `test::run_on_gpu`
  runs it on a device that meets its requirements
  (`volumetric_kit/core/testing/gpu_test.hpp`), and `test::read_back` reads
  its buffers (`volumetric_kit/core/testing/buffer_readback.hpp`). Its
  cross-family cases make a `test::TwoFamilyDevice`
  (`volumetric_kit/core/testing/two_family_device.hpp`).
- A test that is a `main()` of its own, with or without the vulkan tier,
  links `volumetric_kit::core_test_base` for `CHECK`
  (`volumetric_kit/core/testing/test_check.hpp`) and a scratch directory
  (`volumetric_kit/core/testing/scratch_dir.hpp`), and one that fails an
  allocation on purpose links `volumetric_kit::core_test_allocation_failure`
  (`volumetric_kit/core/testing/test_allocation_failure.hpp`). Without the
  vulkan tier, `VKC_BUILD_TEST_SUPPORT` builds only these and
  `core_test_main`.
- Either kind fills an image from host texels with `test::write_image`
  (`volumetric_kit/core/testing/write_image.hpp`).

An application that fetches several siblings (as `ios` fetches `recon` and
`gfx`) declares `volumetric_kit_core` **first**, at least as new as every
sibling requires. FetchContent keeps the first declaration of a name, so every
sibling then builds against that one copy rather than each pinning its own.

```cpp
#include "volumetric_kit/core/base/result.hpp"

namespace vkc = volumetric_kit::core;

vkc::Result<int> parse_positive(int x) {
  if (x <= 0) return vkc::Status::invalid_argument("not positive");
  return x;
}

vkc::Status run() {
  VKC_ASSIGN(const int n, parse_positive(3));
  VKC_CHECK(n == 3, "parsed what it was given");
  return {};
}
```

## Development

```sh
core_root="$(git rev-parse --show-toplevel)"
pre-commit install          # format + hygiene hooks on every commit
pre-commit run --all-files  # format everything, as CI checks it
cmake -S "$core_root" -B "$core_root/build-tidy" \
  -DCMAKE_BUILD_TYPE=Debug -DVKC_CLANG_TIDY=ON
cmake --build "$core_root/build-tidy"  # lint: clang-tidy on first-party code
```

The formatters are pinned in `.pre-commit-config.yaml` (clang-format 22.1.8,
as in recon, gfx and ios) and clang-tidy to the same release; see
[CONTRIBUTING.md](CONTRIBUTING.md#format-and-lint).

## License

Released under the [MIT License](LICENSE). Each source file carries an
`SPDX-License-Identifier: MIT` header.
