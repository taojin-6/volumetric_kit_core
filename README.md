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

`VKC_BUILD_TESTS`, `VKC_INSTALL` and `VKC_WARNINGS_AS_ERRORS` default ON only
when this is the top-level project. `VKC_SANITIZE` is a semicolon list, empty
by default, e.g. `-DVKC_SANITIZE="address;undefined"`.

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
pre-commit install   # clang-format + cmake-format + hygiene hooks
```

## License

Released under the [MIT License](LICENSE). Each source file carries an
`SPDX-License-Identifier: MIT` header.
