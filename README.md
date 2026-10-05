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

> **Status:** the `base` tier is implemented and tested: exception-free
> `Status`/`Result`, the `VKC_CHECK` contract check, the pluggable log sink, and
> the version API. calib and recon build on it. The `vulkan` tier is
> implemented -- the instance, device selection against a library's
> requirements, the logical device every sibling creates or adopts, the VMA
> allocator with its buffers and images, descriptors, synchronization, compute
> kernels, `CommandBatch`, the shader build functions, GPU timers, buffers
> exported for CUDA to write, and the shared device a compute library and a
> renderer both adopt. The `camera` and `sensor` tiers are planned; see
> [DECISIONS.md](DECISIONS.md#tiers) for what each holds and the order they land
> in.

[AGENTS.md](AGENTS.md) is the shared working guide for contributors, Codex and
Claude Code; `CLAUDE.md` imports it.

## Tiers

| Tier | Target | Holds | Status |
| --- | --- | --- | --- |
| `base` | `volumetric_kit::core_base` | `Status`/`Result`, `VKC_CHECK`, logging, stage metrics, version | implemented |
| `vulkan` | `volumetric_kit::core_vulkan` | instance, device selection, device create/adopt and submission, allocator, buffers, images, descriptors, shaders, sync, command buffers, compute kernels, `CommandBatch`, shader build functions, GPU timers, external memory, shared-device bootstrap | implemented |
| `camera` | `volumetric_kit::core_camera` | camera models (rational first), rig calibration file | planned |
| `sensor` | `volumetric_kit::core_sensor` | frame types, capture interface, vendor drivers (Orbbec) | planned |

`volumetric_kit::core` links every tier that is built. A consumer that needs no
GPU (calib's headless solver) links only the tiers it uses.

## Prerequisites

The `base` tier needs only a C++17 compiler -- GCC or Clang; Windows and MSVC
are not supported ([DECISIONS.md](DECISIONS.md#platforms)) -- and CMake ≥
3.21. googletest is fetched, pinned, when tests are built. The `vulkan` tier
also needs a Vulkan SDK's headers and loader (MoltenVK on Apple) -- headers
1.3.204 or newer, 1.3.208 on Apple
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

## Build and test

```sh
core_root="$(git rev-parse --show-toplevel)"
cmake -S "$core_root" -B "$core_root/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$core_root/build" --parallel
ctest --test-dir "$core_root/build" --output-on-failure
```

`VKC_BUILD_TESTS` and `VKC_WARNINGS_AS_ERRORS` default ON only when this is
the top-level project; `VKC_INSTALL` defaults ON everywhere (see below).
`VKC_SANITIZE` is a semicolon list, empty by default, e.g.
`-DVKC_SANITIZE="address;undefined"`; in a subproject build it also reaches
the consumer's targets that link the core.

The vulkan tier's device tests run on the best device present and skip when
there is none. Two environment variables tighten them, and CI sets both:

- `VKC_REQUIRE_VULKAN_DEVICE=1` fails a device test that has no device, so a
  runner cannot pass by skipping;
- `VKC_TEST_VALIDATION=1` enables the Khronos validation layer and fails any
  test that triggers a validation error, or that runs without validation.

CI also enables synchronization validation, checked by a test that commits a
deliberate hazard (`VKC_TEST_SYNC_VALIDATION=1`), and requests shader-access
checks where the layer supports them. Linux uses a software Vulkan driver;
macOS requires an available device. Discrete DRAM/VRAM hardware coverage
remains planned (DECISIONS.md, "Open decisions").

`Allocator::memory_stats()` reports each heap's usage against its budget
(`usage_bytes`, `budget_bytes`) beside this allocator's own share: its
reserved blocks (`reserved_bytes`) and live allocations (`allocation_bytes`).
Only the allocator's own figures may be summed across sibling allocators;
with `VK_EXT_memory_budget`, a heap's usage already includes every allocation
in the process.

## Use it in your project

Pin a release tag or a commit SHA -- never `main` -- so every consumer builds
the same core:

```cmake
include(FetchContent)
FetchContent_Declare(
  volumetric_kit_core
  GIT_REPOSITORY https://github.com/taojin-6/volumetric_kit_core.git
  GIT_TAG <tag-or-sha>)
FetchContent_MakeAvailable(volumetric_kit_core)

target_link_libraries(your_target PRIVATE volumetric_kit::core_base)
```

or, against an installed copy, `find_package(volumetric_kit_core CONFIG)`.
For the vulkan tier, set `VKC_WITH_VULKAN` ON before
`FetchContent_MakeAvailable` and link `volumetric_kit::core_vulkan`. Either
way, `vkc_embed_shaders` then compiles a target's GLSL and embeds the SPIR-V
as headers (`cmake/vkc_shaders.cmake` documents its options):

```cmake
vkc_embed_shaders(your_target SYMBOL_PREFIX your_ SHADERS shaders/integrate.comp)
# your sources: #include "integrate_comp.spv.hpp" -> your_integrate_comp_spv[]
```

A consumer that installs and exports its own targets installs the core beside
them (the core's install rules stay on in a subproject), and its package config
must `find_dependency(volumetric_kit_core)`. One that links the core into a
shared library or framework builds the core shared too
(`BUILD_SHARED_LIBS=ON`): the log handler is process-global, and each binary
linking a static core gets its own (DECISIONS.md, "One instance per process").

An application that fetches several siblings (as `ios` fetches `recon` and
`gfx`) declares `volumetric_kit_core` **first**. FetchContent keeps the first
declaration of a name, so every sibling then builds against that one copy
rather than each pinning its own.

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
