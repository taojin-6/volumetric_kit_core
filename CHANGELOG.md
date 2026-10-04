# Changelog

All notable changes to `volumetric_kit_core` are documented here. The format
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html). Before
1.0, a minor release may break the API; each such entry says how to migrate.

## [Unreleased]

### Added

- Build system: CMake ≥ 3.21, C++17, per-tier targets with an umbrella
  `volumetric_kit::core`, install/export for `find_package(volumetric_kit_core)`
  and FetchContent, and options that default ON only at the top level.
- `base` tier (`volumetric_kit::core_base`), merged from `calib`'s, `recon`'s
  and `gfx`'s copies (DECISIONS.md, "Merging the three `Status`/`Result`
  types"):
  - `Status` with the codes of all three (`Numerical` from `calib`, `Backend`
    with a neutral `int64_t` detail from `recon`) and `Result<T>`, both
    `[[nodiscard]]`, plus `VKC_TRY` / `VKC_ASSIGN`.
  - `VKC_CHECK`, which logs through the sink and aborts.
  - The pluggable log sink (`set_log_handler`, `log_message`), one per process
    for the whole family.
  - The version API (`version_string`, `VKC_VERSION_*`).
- Tests (GoogleTest) and a package-consumer project CI builds against an
  installed copy and against the source tree.
- CI on GitHub-hosted runners: Linux and macOS Debug/Release, Linux
  `-fno-exceptions` and shared-library legs, ASan/UBSan/LSan, and the lint
  gate, behind one required check.
- Formatting and lint: pinned clang-format and cmake-format hooks that fix
  files in place, hygiene checks (YAML, merge-conflict markers, large files,
  line endings), `.editorconfig`, and clang-tidy (`.clang-tidy`, pinned
  22.1.8) run through the build with `-DVKC_CLANG_TIDY=ON`, locally and in CI.

### Migrating from a sibling's own copy

- `volumetric_kit::{calib,recon,gfx}::Status` / `Result` →
  `volumetric_kit::core::Status` / `Result`; `VC_*` / `VR_*` / `VG_*` macros →
  `VKC_TRY`, `VKC_ASSIGN`, `VKC_CHECK`.
- `gfx`: `Status::Code::Vulkan` and `Status::code()` → `Status::Code::Backend`
  and `Status::detail()` (the `VkResult` as `int64_t`). `vk_error`,
  `VG_VK_TRY` and `to_string(VkResult)` move to the vulkan tier.
- `recon` / `gfx`: `Status` and `Result` are now `[[nodiscard]]`; a call site
  that drops one now warns.
- `calib`: `VC_CHECK` now reports through the log sink instead of printing to
  stderr; the default sink still prints it to stderr.
