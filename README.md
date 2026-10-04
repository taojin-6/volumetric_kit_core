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
> the version API. The `vulkan`, `camera` and `sensor` tiers are planned; see
> [DECISIONS.md](DECISIONS.md#tiers) for what each holds and the order they
> land in. No sibling consumes this repository yet.

[AGENTS.md](AGENTS.md) is the shared working guide for contributors, Codex and
Claude Code; `CLAUDE.md` imports it.

## Tiers

| Tier | Target | Holds | Status |
| --- | --- | --- | --- |
| `base` | `volumetric_kit::core_base` | `Status`/`Result`, `VKC_CHECK`, logging, version | implemented |
| `vulkan` | `volumetric_kit::core_vulkan` | instance, device create/adopt, allocator, buffers, images, compute, external memory, shared-device bootstrap | planned |
| `camera` | `volumetric_kit::core_camera` | camera models (rational first), rig calibration file | planned |
| `sensor` | `volumetric_kit::core_sensor` | frame types, capture interface, vendor drivers (Orbbec) | planned |

`volumetric_kit::core` links every tier that is built. A consumer that needs no
GPU (calib's headless solver) links only the tiers it uses.

## Prerequisites

The `base` tier needs only a C++17 compiler and CMake ≥ 3.21. googletest is
fetched, pinned, when tests are built.

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
