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
(clang-format, cmake-format, trailing-whitespace, end-of-file-fixer) into
`.git/hooks/`. They run on staged files at `git commit` and abort the commit if
anything is reformatted. The hook lives in `.git/` and is **not** tracked, so
each clone must run this once.

## Build and test

```sh
core_root="$(git rev-parse --show-toplevel)"
cmake -S "$core_root" -B "$core_root/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$core_root/build" --parallel
ctest --test-dir "$core_root/build" --output-on-failure
```

## Changing a public API

Four repositories build on this one. A change to an installed header or to the
CMake package needs:

- a CHANGELOG entry under `[Unreleased]` that says how a consumer migrates;
- the package-consumer check CI runs (`tests/package_consumer/`);
- the consumers' migration as tasks in their own repositories, each bumping its
  pin.

## Formatting and CI

- clang-format is pinned (see `.pre-commit-config.yaml`) to the same version as
  the sibling repos, so formatting is byte-identical across the family.
- CI's single required check is `ci / required`; it passes only when every
  build leg, lint and sanitizers pass.
- Mark deferred work inline with a greppable `// TODO:` (or `# TODO:` in CMake
  and YAML), rather than tracking it only in prose or commits.

## Commits

Follow Conventional Commits, e.g. `feat(base): …`, `build: …`,
`refactor(vulkan): …`.
