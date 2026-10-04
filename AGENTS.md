# Shared repository instructions

This is the canonical working guide for Codex, Claude Code, and other agents.
Keep shared instructions here. `CLAUDE.md` imports this file with `@AGENTS.md`;
do not duplicate rules there or create a circular reference.

## Project and boundaries

`volumetric_kit_core` is the shared foundation of the `volumetric_kit` family:
`calib`, `recon`, `gfx` and `ios` all depend on it. Implemented and planned
tiers are listed in [README.md](README.md#tiers); why each exists and the order
they land in are in [DECISIONS.md](DECISIONS.md#tiers).

- Everything here is consumed by four repositories. A public API change needs a
  CHANGELOG entry that says how to migrate, and the consumers move by bumping
  their pin -- never by tracking `main`.
- Code belongs here only when two or more siblings need it. A renderer-only
  piece stays in `gfx`; a reconstruction kernel stays in `recon`.
- The `base` tier has no dependencies and includes no GPU API. GPU API types
  appear only in the `vulkan` tier and above.
- The siblings sit beside this checkout (`../volumetric_kit_recon/` and so on
  from the primary checkout). Keep this task's changes in this repository; a
  sibling's migration is its own task in its own repository.

## Read what the task needs

| Task | Read |
| --- | --- |
| Scope, tiers, consuming the package | [README.md](README.md) and the affected public headers |
| Why a tier or rule exists | [DECISIONS.md](DECISIONS.md) |
| Formatting and CI | [CONTRIBUTING.md](CONTRIBUTING.md), `.pre-commit-config.yaml`, `.github/workflows/` |
| Seeding the vulkan tier | `recon`'s and `gfx`'s `include/volumetric_kit/*/core/` and their DECISIONS.md |

## Naming conventions

- Package/repo: `volumetric_kit_core`
- Namespace: `volumetric_kit::core`. Alias it as `vkc`, never `vk` (Vulkan's
  C++ bindings own `namespace vk`).
- Headers: `include/volumetric_kit/core/<tier>/…`, e.g.
  `#include "volumetric_kit/core/base/result.hpp"`
- CMake: `find_package(volumetric_kit_core)`; targets
  `volumetric_kit::core_base`, `…_vulkan`, `…_camera`, `…_sensor`; umbrella
  `volumetric_kit::core`.
- Macros/export: `VKC_*` (e.g. `VKC_TRY`, `VKC_CHECK`, `VKC_BASE_API`). Never
  `VK_*`, which Vulkan owns.

## Architecture

`base` → {`vulkan`, `camera`} → `sensor`. A tier depends only on the tiers its
row in DECISIONS.md names. `base` is pure CPU, so its tests run on every
machine.

## Error handling

Mobile consumers build with `-fno-exceptions`. Fallible calls return `Status`
(success, or an error domain plus a message, and a backend code for `Backend`)
or `Result<T>` (a value or a `Status`). Propagate with `VKC_TRY` /
`VKC_ASSIGN`. Programmer errors (reading an error `Result`'s value) fail fast
via `VKC_CHECK`, which logs through the sink and aborts; never throw.

## Conventions

C++17, with no compiler extensions.

- **Doxygen on every public class/function**, matching
  `include/volumetric_kit/core/base/result.hpp`: `@file`/`@brief` on the header,
  `@brief` + a `@code … @endcode` example per class, and `@brief`/`@param`/
  `@return` (`@pre` where relevant) on functions; accessors may be a single
  `/// @return`.
- **Mark deferred work inline with a greppable `// TODO:`** (or `# TODO:` in
  CMake and YAML) rather than tracking it only in prose or commits.
- Prefer plain, behavior-level tests over friend-class backdoors into private
  state.

## Working with Git

- Implement each task in a dedicated branch and git worktree under
  `.worktrees/`; never create a sibling worktree in the parent folder.
  Concurrent Claude/Codex tasks use separate worktrees.
- Preserve unrelated local changes and other worktrees. Remove your worktree
  with `git worktree remove` after its PR merges.
- Use absolute paths for `git -C`, `cmake -S/-B`, and file operations so work
  cannot spill into a sibling checkout.
- Use Conventional Commits, e.g. `feat(base): …`, `fix(vulkan): …`, `docs: …`.
- Assign PRs to the authenticated user (`gh pr create --assignee @me`).
- Commit shared decisions and handoff context with the project so either agent
  can continue the task.

## Build and validation

Run from the task's worktree. The build requires CMake ≥ 3.21 and a C++17
compiler.

```sh
core_root="$(git rev-parse --show-toplevel)"
cmake -S "$core_root" -B "$core_root/build" \
  -DCMAKE_BUILD_TYPE=Release -DVKC_BUILD_TESTS=ON
cmake --build "$core_root/build" --parallel
ctest --test-dir "$core_root/build" --output-on-failure
pre-commit run --all-files --show-diff-on-failure
git -C "$core_root" diff --check
```

- Use the pinned formatting tools in `.pre-commit-config.yaml`; CI runs the
  same hooks. Set the build type explicitly and report it with measurements.
- For code changes, build affected targets and run relevant tests, with
  regression coverage for changed behavior. A change to an installed header or
  to the CMake package also needs the package-consumer check CI runs
  (`tests/package_consumer/`, both `find_package` and `add_subdirectory`).
- Documentation-only changes need formatting, link, and consistency checks;
  no build is needed. Hooks can be scoped with `pre-commit run --files`.

## Keeping guidance current

- Keep this guide concise (roughly 100–200 lines). Put detailed contracts and
  rationale in DECISIONS.md.
- Update a changed decision and its rationale in the same commit as the code,
  then update any essential rule here and the affected README guidance.
  Preserve the distinction between locked choices, open questions, and plans.
- `CLAUDE.md` stays an import; add only Claude-specific instructions below it
  if a concrete need arises.
