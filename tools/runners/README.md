# Self-hosted CI runners

`recon`, `gfx` and `player` run their GPU legs on self-hosted runners: Linux
GPU hosts (label `vk-linux-gpu`), where each leg runs in an OS-matched
container with the GPU passed through (`--gpus all`), and a Mac (label
`mac`), where jobs run natively for the Apple toolchain and MoltenVK on its
GPU. `calib` runs its macOS legs on the Mac too, on its installed Homebrew.
Every other job stays on GitHub-hosted runners, free for public repositories.
Each repository's `ci.yml` says which legs go where; this directory is the
host side, for all four.

| File | Does |
| --- | --- |
| `setup-runners.sh <recon\|gfx\|calib\|player>` | registers a repository's runners on this host |
| `teardown-runners.sh <recon\|gfx\|calib\|player>` | stops, deregisters and deletes them |
| `common.sh` | what the two scripts share |
| `homebrew-key.sh <formula>...` | prints a cache key of their installed Homebrew versions ([Upgrading Homebrew](#upgrading-homebrew)) |

Runners on a personal account belong to one repository, so a host serving
several carries a set for each.

## Who can trigger CI

Only the owner. Each family repository has an Actions policy (Settings ->
Actions -> Rules) that lets no other account trigger a workflow; the core's
[DECISIONS.md](../../DECISIONS.md#only-the-owner-triggers-ci) says why.
GitHub checks it before it creates any job: a push by an account outside the
rule was tested, and its run ended in `startup_failure` with no jobs, which
GitHub's page blames on "a workflow file issue". A fork's pull request, which
runs the workflow files in the fork and is triggered by the fork's account,
should end the same way, but that has not been tried. Bots are refused too,
so pre-commit.ci and Dependabot are not installed.

Outside code therefore reaches the runners only through the owner, and the
owner reviews it first. The runners are persistent, the Mac's jobs run as its
user, and the Linux hosts' `docker` group is root-equivalent. To give a
fork's change CI, a maintainer first reads its whole diff, above all
`.github/` and every build and test script, then pushes its branch to the
repository and opens a pull request from there. Approving or re-running a
fork's run counts the same: once the owner triggers it, it may run.

The policy, as created on each repository. Add a new collaborator's user ID
to `allowed_actors` on every repository, or their runs are refused too:

```sh
gh api -X POST repos/taojin-6/<repo>/actions/policies --input - <<'EOF'
{"name": "Only the owner can trigger workflows", "enforcement": "active",
 "rules": [{"type": "restrict_actions_actors",
            "parameters": {"allowed_actors": [{"id": 22780239, "type": "User"}]}}]}
EOF
```

## Linux hosts

1. **Docker Engine**, by the apt-repository method
   (<https://docs.docker.com/engine/install/ubuntu/>).
2. **NVIDIA Container Toolkit**, so containers can use the GPU:

   ```sh
   # add the toolkit's apt repository first (see NVIDIA's docs), then:
   sudo apt-get install -y nvidia-container-toolkit
   sudo nvidia-ctk runtime configure --runtime=docker
   sudo systemctl restart docker
   # The toolkit bind-mounts nvidia-persistenced's socket into every --gpus
   # container, so without the daemon no container starts ("failed to fulfil
   # mount request: open /run/nvidia-persistenced/socket"):
   sudo systemctl enable --now nvidia-persistenced
   # The GPU is visible in a container:
   docker run --rm --gpus all -e NVIDIA_DRIVER_CAPABILITIES=all ubuntu:24.04 nvidia-smi
   # and Vulkan reaches it from a bare image -- the ICD the toolkit mounts
   # needs the image's libXext and libEGL, which the workflows install. A
   # device must be listed, or the legs fail at their GPU diagnostic:
   docker run --rm --gpus all -e NVIDIA_DRIVER_CAPABILITIES=all ubuntu:24.04 sh -c \
     'apt-get update -qq && apt-get install -y -qq libxext6 libegl1 vulkan-tools >/dev/null && vulkaninfo --summary | grep deviceName'
   ```

3. As the login user (not root):
   `bash tools/runners/setup-runners.sh recon`, and again with `gfx` and
   `player`. Each registers 6 runners labelled `vk-linux-gpu` -- a run has at
   most 2 Linux jobs, so 6 fit three runs on one host while the other is
   offline -- each a systemd service started at boot; it calls `sudo` for the
   services. Log `gh` out afterwards, as on a Mac (below).

Until a `vk-linux-gpu` runner is online, the Linux legs stay pending and
`ci / required` waits for them. Membership in the `docker` group is
root-equivalent: fine for a personal machine, not for a shared one.

## Macs

1. **Full Xcode**, not only the Command Line Tools: a leg that generates with
   `-G Xcode` drives `xcodebuild`, which they do not ship.

   ```sh
   sudo xcode-select -s /Applications/Xcode.app
   sudo xcodebuild -license accept
   ```

   The runners take the `xcode-select` choice: they run outside any login
   session, so a `DEVELOPER_DIR` a shell exports does not reach them.
2. **Homebrew** (<https://brew.sh>) and the formulae the legs use
   ([Upgrading Homebrew](#upgrading-homebrew)).
3. As the user the jobs run as, in Terminal or over SSH, with conda or any
   other environment deactivated: `bash tools/runners/setup-runners.sh recon`,
   and again with `gfx`, `calib` and `player`. Each registers 2 runners
   labelled `mac` (Debug and Release in parallel), each a system LaunchDaemon
   that runs as that user; it calls `sudo` for them. A runner keeps the
   `PATH` it was registered with, in its `.path`, and CMake searches that
   `PATH` for packages, so an environment active then would shadow
   Homebrew's. The jobs can read the user's `gh` login, which acts on every
   repository of the account: run `gh auth logout` once they are registered,
   and log in again only to register or tear down.
4. Keep the Mac awake: `sudo pmset -a sleep 0 disablesleep 1`. The daemons
   start at boot, before anyone logs in, so the Mac needs no automatic login,
   which FileVault rules out ([DECISIONS.md](../../DECISIONS.md), the
   2026-10-17 entry). With FileVault on, a cold start still waits for its
   password at the unlock screen.
5. **MoltenVK's iOS release**, for the legs that build for iOS, installed once
   as the core's [README.md](../../README.md#prerequisites) says. Setup writes
   a `MoltenVK_ROOT` the shell sets into each runner's `.env`; unset, it keeps
   the `.env` as it is, and naming no absolute path to a directory, it stops
   before any change:

   ```sh
   export MoltenVK_ROOT=$HOME/ws/volumetric_kit/third_party/MoltenVK_ios_v1.4.2/MoltenVK
   bash tools/runners/setup-runners.sh player
   bash tools/runners/setup-runners.sh gfx
   ```

## What setup-runners.sh does

- Runners live in `~/ci-runners/<repo>/runner-<i>`, so several repositories'
  runners share a host. Hosts set up before that layout have them in
  `~/actions-runner-*`; both scripts act on those whose `.runner` names the
  repository, and leave the rest.
- A new runner is named `<host>-<repo>-<i>`, after the lowest number no
  runner of the repository on the host holds. `recon`'s pre-layout runners,
  `~/actions-runner-recon-<i>`, carry such names, so their numbers are taken
  too: registering takes over any runner of the same name, which is how it
  reclaims one whose directory was lost. A runner already registered keeps
  its name, so `gfx`'s older runners keep theirs (`<host>-<i>`).
- A re-run keeps registered runners, in either layout, and registers new
  ones only until the repository has 6 (Linux) or 2 (Mac) on the host, so it
  needs no token. For a new runner it mints the token with `gh` when that is
  authed, and otherwise asks for one from the repository's Settings ->
  Actions -> Runners -> New self-hosted runner. It installs the runner's
  latest release, reusing a cached tarball only if the archive is whole.
- Each runner is a service, installed if it is not and started if it is not
  running: a systemd unit on Linux, through the runner's `svc.sh`, and on a
  Mac a LaunchDaemon in `/Library/LaunchDaemons` that `KeepAlive` restarts if
  it exits, labelled with the runner's own service name
  (`actions.runner.<owner>-<repo>.<runner>`), so one already installed under
  it is kept. A LaunchAgent an earlier setup installed for a runner is
  unloaded and deleted ([DECISIONS.md](../../DECISIONS.md), the 2026-10-17
  entry). A service that does not install or start fails the run, after the
  other runners are set up.
- Each of the repository's runners gets `CMAKE_BUILD_PARALLEL_LEVEL` and
  `CTEST_PARALLEL_LEVEL` in its `.env`: the host's cores divided by the
  repository's runners on it, 5 on a 10-core Mac with 2
  ([DECISIONS.md](../../DECISIONS.md), the 2026-10-16 entry). Other
  repositories' runners keep theirs, so jobs of several repositories at once
  oversubscribe the cores.
- The levels reach jobs that run on the host (the Mac's), not job
  containers, whose workflows set their own. A step honours them only when
  it passes them or gives no `--parallel`: a bare `cmake --build --parallel`
  runs the build tool's default, with Makefiles an unbounded `make -j`, and a
  bare `ctest --parallel` one test per core. A build step therefore passes
  `--parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-$(getconf _NPROCESSORS_ONLN)}"`.
  `ctest` honours `CTEST_PARALLEL_LEVEL` with no `--parallel`, or with the
  same form; only the form runs tests at once where the level is unset, as on
  GitHub-hosted runners.
- The runner reads `.env` only when it starts, so a running runner of the
  repository whose `.env` changed is restarted, which cancels a job running
  on it: run the script while CI is idle. A Mac's daemon restarts in place
  (`launchctl kickstart -k`).

## Upgrading Homebrew

Run `brew update && brew upgrade` on a Mac freely while CI is idle: caches
key on the installed versions, so the next run rebuilds what changed. An
upgrade while a job runs can still break that job, whose formulae change
under it. Why the caches need it: [DECISIONS.md](../../DECISIONS.md#a-brew-upgrade-needs-no-cache-clearing).
When an upgrade moves `molten-vk`, install MoltenVK's iOS release of the
same version and re-run step 5 of [Macs](#macs) with its path
([DECISIONS.md](../../DECISIONS.md), the 2026-10-19 entry).

A leg's key is what `homebrew-key.sh` prints for the formulae it uses: 16
hex digits that change whenever the installed version of one of them, or of
anything they depend on, does, and `none` off macOS or without `brew`. It
only reads, with Homebrew's auto-update off, and prints no key when brew
fails. A workflow gets it from this repository's action, at the full SHA of
the core commit the repository already pins, so it keeps one core pin:

```yaml
- name: Install or check the Homebrew formulae
  id: formulae
  run: |
    pkgs="cmake glm ffmpeg pkgconf"
    echo "formulae=$pkgs" >>"$GITHUB_OUTPUT"
    # ... installs or checks $pkgs, as the last rule below says
- name: Homebrew key
  id: brew
  uses: taojin-6/volumetric_kit_core/.github/actions/homebrew-key@<full SHA of the core pin>
  with:
    formulae: ${{ steps.formulae.outputs.formulae }}
- name: Cache third-party deps
  uses: actions/cache@v6
  with:
    path: build/_deps
    key: deps-<leg>-brew${{ steps.brew.outputs.key }}-${{ hashFiles('third_party/CMakeLists.txt') }}
    restore-keys: deps-<leg>-brew${{ steps.brew.outputs.key }}-
```

- Name the leg's formulae once, in the step that installs or checks them,
  and pass that list to the action. List every formula the build finds: one
  left out is covered only as a listed formula's dependency.
- Keep the key inside every `restore-keys` prefix, so a fallback never
  restores a tree built on other versions.
- On the self-hosted Mac a job installs nothing: it fails, naming any
  formula missing, and the owner installs that by hand while CI is idle
  ([why](../../DECISIONS.md#a-brew-upgrade-needs-no-cache-clearing)).
  GitHub-hosted runners install what their image lacks, with
  `HOMEBREW_NO_AUTO_UPDATE` set.

## Pausing, moving and removing a host

Runners on a host share one label, and GitHub routes jobs to whichever host
is online, so moving to a new host needs no workflow change: bring the new one
up, then tear the old one down.

- **Pause** (offline, still registered), per repository, in either layout:

  ```sh
  bash -c '. tools/runners/common.sh; select_repo recon; find_runner_dirs
    for d in "${RUNNER_DIRS[@]}"; do stop_service "$d"; done'
  ```

  The runners stay offline until the host restarts. Resume with
  `bash tools/runners/setup-runners.sh recon`, which starts every runner of
  the repository that is not running.
- **Remove**, on the old host: `bash tools/runners/teardown-runners.sh recon`,
  and again with each other repository there. It uninstalls each runner's
  service (a Mac's daemon only once launchd has let go of it),
  deregisters the runner and deletes its directory, and deletes the
  repository's leftovers that never finished registering. It checks `gh` auth
  and mints one remove token before it touches any runner, so if either fails
  every runner keeps running. A runner GitHub will not deregister is kept,
  offline, with the credentials a retry needs, and the script exits non-zero.
  Deregistering matters: a registration left on a machine given away is a way
  into the repository's CI.
