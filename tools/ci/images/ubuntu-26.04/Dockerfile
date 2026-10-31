# The family's Ubuntu 26.04 CI image: what every 26.04 leg installed on a bare
# image, so a leg starts with it installed (../README.md).
# ubuntu:26.04
FROM ubuntu@sha256:6a06111c3b0a8600feadb270aec771dc239bf407b25625f93f44ab00e11f4d3c

LABEL org.opencontainers.image.source="https://github.com/taojin-6/volumetric_kit_core" \
      org.opencontainers.image.description="volumetric_kit CI image, Ubuntu 26.04 (tools/ci/images)" \
      org.opencontainers.image.base.name="docker.io/library/ubuntu:26.04"

SHELL ["/bin/bash", "-euo", "pipefail", "-c"]
ARG DEBIAN_FRONTEND=noninteractive

# As the 24.04 image, and: ssh for a sibling fetched over it; Python's venv and
# the FFmpeg command line for Python tests; and the sanitizer legs' clang, its
# sanitizer runtimes (libclang-rt-dev), llvm-symbolizer (llvm) and lavapipe
# (mesa-vulkan-drivers).
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates git sudo curl gnupg openssh-client \
      build-essential cmake pkg-config python3 python3-venv \
      libvulkan-dev vulkan-tools vulkan-validationlayers \
      glslc glslang-tools spirv-tools \
      libxext6 libegl1 \
      libglm-dev libglfw3-dev xvfb \
      libassimp-dev zlib1g-dev \
      libavcodec-dev libavutil-dev portaudio19-dev ffmpeg \
      clang libclang-rt-dev llvm mesa-vulkan-drivers \
 && rm -rf /var/lib/apt/lists/*

# Mesa's driver and implicit layer manifests move out of the loader's search
# path, so a GPU leg's loader finds only the ICD the NVIDIA container toolkit
# mounts, as on a bare image. A leg that wants lavapipe names it
# (../README.md).
RUN mkdir -p /opt/vkc-ci/icd.d /opt/vkc-ci/implicit_layer.d \
 && dpkg -L mesa-vulkan-drivers \
    | grep -E '^/usr/share/vulkan/(icd|implicit_layer)\.d/[^/]+\.json$' \
    | while read -r f; do mv "$f" "/opt/vkc-ci/$(basename "$(dirname "$f")")/"; done \
 && test -f /opt/vkc-ci/icd.d/lvp_icd.json
ENV VKC_CI_LAVAPIPE_ICD=/opt/vkc-ci/icd.d/lvp_icd.json

# CUDA 13.4, as in the 24.04 image; no leg builds FFS on 26.04, so no TensorRT.
RUN keyring_sha256=f7f474b5f6a4adf987aa587920df00e713285958ef6a913dda1945a544a3099e \
 && curl -fsSL -o /tmp/cuda-keyring.deb \
      https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2604/x86_64/cuda-keyring_1.1-1_all.deb \
 && echo "${keyring_sha256}  /tmp/cuda-keyring.deb" | sha256sum -c - \
 && dpkg -i /tmp/cuda-keyring.deb \
 && rm /tmp/cuda-keyring.deb \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
      cuda-nvcc-13-4 cuda-cudart-dev-13-4 cuda-driver-dev-13-4 \
      libnvjpeg-dev-13-4 \
 && rm -rf /var/lib/apt/lists/*

ENV CUDAToolkit_ROOT=/usr/local/cuda-13.4
