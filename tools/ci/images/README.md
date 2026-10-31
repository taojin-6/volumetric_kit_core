# Linux CI images

The family's Linux legs run in job containers. These images replace the bare
`ubuntu@sha256` image and the install step: one per Ubuntu release, each with
the union of what that release's legs install, pushed to the private GHCR
package `ghcr.io/taojin-6/volumetric_kit_ci` (tags `ubuntu-24.04`,
`ubuntu-26.04`), and pinned by digest in each repository. They are rebuilt
and re-pinned by hand only (DECISIONS.md, the 2026-10-31 entry).

| File | Does |
| --- | --- |
| `ubuntu-24.04/Dockerfile`, `ubuntu-26.04/Dockerfile` | the images; each comment says which legs need a group of packages |
| `smoke.sh` | checks a built image inside a GPU container, before it is pushed |
| `check_dockerfiles.py` | the rules below; lint runs its tests |
| `../../../.github/workflows/ci-images.yml` | builds, checks and pushes both, when the owner dispatches it |

## What is in them

- **Both:** git, sudo, curl and gnupg; GCC (`build-essential`), CMake,
  pkg-config and Python 3; the Vulkan loader, headers, tools and validation
  layer; glslc, glslang and SPIRV-Tools; the two libraries the NVIDIA ICD
  needs (libXext, libEGL); GLM, GLFW and Xvfb; Assimp, FFmpeg's libraries and
  PortAudio; and CUDA 13.4's nvcc, runtime, driver API headers and nvJPEG,
  with `CUDAToolkit_ROOT=/usr/local/cuda-13.4`.
- **24.04 adds** TensorRT, at the build for that CUDA.
- **26.04 adds** ssh, Python's venv, the FFmpeg command line, and the
  sanitizer legs' clang, sanitizer runtimes, llvm-symbolizer and lavapipe.
  Mesa's driver and implicit-layer manifests are moved out of the loader's
  search path, so a GPU leg sees only the NVIDIA ICD, as on a bare image; a
  leg that wants lavapipe names `VKC_CI_LAVAPIPE_ICD` in `VK_ICD_FILENAMES`
  (alone) or `VK_ADD_DRIVER_FILES` (beside the GPU).

Not in them: the Orbbec SDK and librealsense, which sensor's actions install
at the pins they keep in lockstep with its CMake floors; a repository's own
Python packages; and the driver's shader-cache directory, which is under the
runner's tool cache, mounted at run time, so a leg still creates it.

## Using one

A job pulls the image by digest, the tag in a comment, with the job's token;
its `permissions` (and, for a reusable workflow, its caller's) include
`packages: read`:

```yaml
container:
  # ghcr.io/taojin-6/volumetric_kit_ci:ubuntu-26.04
  image: ghcr.io/taojin-6/volumetric_kit_ci@sha256:<digest>
  credentials:
    username: ${{ github.actor }}
    password: ${{ secrets.GITHUB_TOKEN }}
  options: --gpus all
```

The leg then drops its install step. A host keeps a pulled image, so a job
pays only the registry's manifest check.

## Rebuilding and re-pinning, by hand

1. Edit the Dockerfile. A new base image is `docker buildx imagetools inspect
   ubuntu:26.04`'s `Digest:`, with the tag in the comment above `FROM`.
   `python3 tools/ci/images/check_dockerfiles.py tools/ci/images` checks the
   rules below.
2. Merge it, then run the workflow:
   `gh workflow run ci-images.yml -R taojin-6/volumetric_kit_core`. It builds
   without cached layers, so a rebuild with no edit takes Ubuntu's and NVIDIA's
   current packages. Its summary prints each image's digest.
3. Move each repository's pin to the new digest, one pull request each, whose
   CI is the check of the new image.

GHCR keeps a replaced image as an untagged version, so a repository still on
the old digest keeps working. Delete a version only once no repository pins
it; the hosts' old images are pruned by hand too.

## The owner's one-time step

The package is private, as it holds NVIDIA's CUDA and TensorRT, which are not
ours to redistribute; keep it so. The core reaches it through the source
label. Every other repository needs read access, given once after the first
run: on the package's settings
(<https://github.com/users/taojin-6/packages/container/volumetric_kit_ci/settings>),
"Manage Actions access" -> "Add repository", each repository with a Linux leg,
role Read.

## The rules

`check_dockerfiles.py` fails a Dockerfile, naming the line, that breaks one:

- `FROM` pins `ubuntu@sha256:<digest>`, with `# ubuntu:<release>` above it,
  the release its directory names;
- every `apt-get install` takes `--no-install-recommends`, as the legs did;
- a `RUN` that runs `apt-get update` ends by removing `/var/lib/apt/lists/*`,
  so the lists stay out of the image;
- `dpkg -i` installs a file only after the same `RUN` has checked its SHA256;
- `org.opencontainers.image.source` names the core.
