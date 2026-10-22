# Contributing

[AGENTS.md](AGENTS.md) is the canonical shared guide for contributors, Codex,
and Claude Code. It owns the working rules, validation commands, and task map;
`CLAUDE.md` imports it. [DECISIONS.md](DECISIONS.md) keeps the recorded choices.

## One-time setup

Install [`pre-commit`](https://pre-commit.com) (`pipx install pre-commit`, or
`pip install --user pre-commit`), then, from a fresh clone, install the git hook:

```sh
pre-commit install
```

This wires the hooks in [`.pre-commit-config.yaml`](.pre-commit-config.yaml)
into `.git/hooks/`. They run on staged files at `git commit`; the formatters
(clang-format, cmake-format, whitespace, line endings) fix files in place and
abort the commit so you can stage the result, and the checks (YAML syntax,
merge-conflict markers, large files) report what to fix. The hook lives in
`.git/` and is **not** tracked, so each clone must run this once.
[`.editorconfig`](.editorconfig) gives editors the same whitespace rules.

## Format and lint

Format everything, the way CI checks it:

```sh
pre-commit run --all-files
```

Lint with clang-tidy, pinned to the same release as clang-format. It needs a
compile database, so it runs through the build rather than as a hook: every
first-party file is checked against [`.clang-tidy`](.clang-tidy) as it
compiles, and any finding fails the build. The test files skip the
path-sensitive analyzer (`clang-analyzer-*`), which is most of the time it
takes ([`tests/.clang-tidy`](tests/.clang-tidy)); the library, test support
included, keeps it.

```sh
pipx install clang-tidy==22.1.8   # or: pip install --user clang-tidy==22.1.8
core_root="$(git rev-parse --show-toplevel)"
cmake -S "$core_root" -B "$core_root/build-tidy" \
  -DCMAKE_BUILD_TYPE=Debug -DVKC_CLANG_TIDY=ON
cmake --build "$core_root/build-tidy" --parallel
```

Fix a finding rather than silencing it. When a check is wrong for a line, use a
targeted `// NOLINT(check-name)` with the reason; when it is wrong for the
codebase, turn it off in `.clang-tidy` with a comment saying why.

## Build and test

```sh
core_root="$(git rev-parse --show-toplevel)"
cmake -S "$core_root" -B "$core_root/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$core_root/build" --parallel
ctest --test-dir "$core_root/build" --output-on-failure
```

### GPU tests

The vulkan tier's device tests skip without a device. Run them as CI does,
with the variables [README.md](README.md#build-and-test) describes:

```sh
VKC_REQUIRE_VULKAN_DEVICE=1 VKC_TEST_VALIDATION=1 VKC_TEST_SYNC_VALIDATION=1 \
  ctest --test-dir "$core_root/build" --output-on-failure
```

On macOS with Homebrew's `vulkan-validationlayers`, the loader finds the
layer's manifest but not its library; add `DYLD_LIBRARY_PATH=/opt/homebrew/lib`,
or every device test fails with "validation is off". Capability skips, such
as unsupported external-memory handles, remain distinct from tests that
executed successfully.

CI configures `-DVKC_TEST_SHARDS=4`: each ordinary device-test binary runs in
four processes, reusing its shared Vulkan fixtures within each shard. Policy
and environment probes still run separately. Shards fail on their exit code,
including shared-device teardown errors; a capability skip does not mask a
failure in another case. GoogleTest reports individual skips in the output.
Leave the default `VKC_TEST_SHARDS=0` for one CTest entry per case and
`ctest -R <case>`, or run a binary with `--gtest_filter=<case>` in either mode.

## Changing a public API

Four repositories build on this one. A change to a public header or to the
CMake a consumer sees needs:

- a CHANGELOG entry that says how a consumer migrates, and the version bump
  that releases it (DECISIONS.md, "Consumers pin, and an application declares
  the core first");
- the consumer check CI runs (`tests/consumer/`);
- the consumers' migration as tasks in their own repositories, each bumping its
  pin.

## Formatting and CI

- clang-format is pinned (see `.pre-commit-config.yaml`) to the release recon,
  gfx and ios pin, so their formatting is byte-identical; clang-tidy is pinned
  to the same release.
- CI's single required check is `ci / required`; it passes only when every
  build leg, lint (the pre-commit hooks, clang-tidy and the tests of
  `tools/runners/`) and sanitizers pass.
- Mark deferred work inline with a greppable `// TODO:` (or `# TODO:` in CMake
  and YAML), rather than tracking it only in prose or commits.

## Commits

Follow Conventional Commits, e.g. `feat(base): …`, `build: …`,
`refactor(vulkan): …`.
