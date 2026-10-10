# Self-hosted CI runners

`recon` and `gfx` run their GPU legs on self-hosted runners: Linux GPU hosts
(label `vk-linux-gpu`), where each leg runs in an OS-matched container with
the GPU passed through (`--gpus all`), and a Mac (label `mac`), where jobs run
natively for the Apple toolchain and MoltenVK on its GPU. Lint, sanitizers
and the `required` gate stay on GitHub-hosted runners. Each repository's
`ci.yml` says which legs go where; this directory is the host side, for both.

| File | Does |
| --- | --- |
| `setup-runners.sh <recon\|gfx>` | registers a repository's runners on this host |
| `teardown-runners.sh <recon\|gfx>` | stops, deregisters and deletes them |
| `common.sh` | what the two scripts share |

Runners on a personal account belong to one repository, so a host serving
both carries a set for each.

## Who can trigger CI

Only the owner. Each family repository has an Actions policy (Settings ->
Actions -> Rules) that lets no other account trigger a workflow; the core's
[DECISIONS.md](../../DECISIONS.md#only-the-owner-triggers-ci) says why.
GitHub checks it before it creates any job, so a fork's pull request, which
runs the workflow files in the fork, never reaches a runner. Its run ends in
`startup_failure` with no jobs, and GitHub's page blames "a workflow file
issue". To test a fork's change, a maintainer pushes its branch to the
repository and opens a pull request from there.

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
   `bash tools/runners/setup-runners.sh recon`, and again with `gfx`. Each
   registers 6 runners labelled `vk-linux-gpu` -- a run has 3 Linux legs, so
   6 let two runs, or both hosts' legs while one is offline, proceed -- each a
   systemd service started at boot; it calls `sudo` for the services.

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

   `DEVELOPER_DIR` overrides `xcode-select`, and the runner, a LaunchAgent,
   inherits the login session's: make sure the session exports no stale one.
2. **Homebrew** (<https://brew.sh>).
3. In Terminal, as the login user: `bash tools/runners/setup-runners.sh recon`,
   and again with `gfx`. Each registers 2 runners labelled `mac` (Debug and
   Release in parallel), each a launchd LaunchAgent.
4. Keep the Mac awake and logged in, since a LaunchAgent runs only in the
   user's session: `sudo pmset -a sleep 0 disablesleep 1`, and enable
   automatic login in System Settings.

## What setup-runners.sh does

- Runners live in `~/ci-runners/<repo>/runner-<i>`, so several repositories'
  runners share a host. Hosts set up before that layout have them in
  `~/actions-runner-*`; both scripts act on those whose `.runner` names the
  repository, and leave the rest.
- A new runner is named `<host>-<repo>-<i>`, e.g. `taojin-desktop-recon-1`.
  A runner already registered keeps its name, so `gfx`'s older runners keep
  theirs (`<host>-<i>`).
- A re-run keeps registered runners, in either layout, and registers new
  ones only until the repository has 6 (Linux) or 2 (Mac) on the host, so it
  needs no token. For a new runner it mints the token with `gh` when that is
  authed, and otherwise asks for one from the repository's Settings ->
  Actions -> Runners -> New self-hosted runner. It reuses a cached runner
  tarball only if the archive is whole.
- Each runner's `.env` gets `CMAKE_BUILD_PARALLEL_LEVEL` and
  `CTEST_PARALLEL_LEVEL` set to the host's cores divided by every runner
  registered on it, of every repository. They reach jobs that run on the host
  (the Mac's), not job containers, whose workflows set their own. After
  adding another repository's runners to a host, re-run the script for each
  repository so the share is recomputed.
- The runner reads `.env` only when it starts, so a runner whose `.env`
  changed is restarted, which cancels a job running on it: run the script
  while CI is idle.

On `Taos-Mac-mini`, where jobs run on the host, run the script once for
`recon` and once for `gfx` to write that share. The Linux hosts need nothing,
since their jobs run in containers.

## Pausing, moving and removing a host

Runners on a host share one label, and GitHub routes jobs to whichever host
is online, so moving to a new host needs no workflow change: bring the new one
up, then tear the old one down.

- **Pause** (offline, still registered), per repository; drop `sudo` on
  macOS:

  ```sh
  for d in ~/ci-runners/volumetric_kit_recon/runner-*/; do (cd "$d" && sudo ./svc.sh stop); done
  ```

  Resume with `./svc.sh start` the same way.
- **Remove**, on the old host: `bash tools/runners/teardown-runners.sh recon`,
  and again with `gfx`. It stops and uninstalls each runner's service,
  deregisters it and deletes its directory. A runner it cannot deregister (no
  `gh` auth, or GitHub refused) is kept, with the credentials a retry needs,
  and the script exits non-zero. Deregistering matters: a registration left
  on a machine given away is a way into the repository's CI.
