# The family's Ubuntu 24.04 CI image: what every 24.04 leg installed on a bare
# image, so a leg starts with it installed (../README.md).
# ubuntu:24.04
FROM ubuntu@sha256:fe81560990fae86dc4743202b2cb4d3df98d0bcf949173cecef15ce1127ef852

LABEL org.opencontainers.image.source="https://github.com/taojin-6/volumetric_kit_core" \
      org.opencontainers.image.description="volumetric_kit CI image, Ubuntu 24.04 (tools/ci/images)" \
      org.opencontainers.image.base.name="docker.io/library/ubuntu:24.04"

SHELL ["/bin/bash", "-euo", "pipefail", "-c"]
ARG DEBIAN_FRONTEND=noninteractive

# git and sudo before checkout, as the legs ran as root; curl and gnupg for the
# SDKs sensor's actions install per job; the toolchain and the Vulkan SDK; the
# two libraries the NVIDIA ICD needs before it creates an instance (libXext,
# libEGL); GLFW and the X server a smoke run draws to; and the optional
# drivers' prerequisites: Assimp (its CMake export links libz), FFmpeg,
# PortAudio.
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates git sudo curl gnupg \
      build-essential cmake pkg-config python3 \
      libvulkan-dev vulkan-tools vulkan-validationlayers \
      glslc glslang-tools spirv-tools \
      libxext6 libegl1 \
      libglm-dev libglfw3-dev xvfb \
      libassimp-dev zlib1g-dev \
      libavcodec-dev libavutil-dev portaudio19-dev \
 && rm -rf /var/lib/apt/lists/*

# CUDA 13.4 from NVIDIA's repository: nvcc, which FindCUDAToolkit locates the
# toolkit by, the runtime, the driver API's headers and nvJPEG, for the NVDEC
# and nvJPEG builds; libcuda comes from the container toolkit at run time.
# The repository's key comes as a .deb that dpkg installs, so only the file
# with the SHA256 the repository's signed index lists is installed; purging it
# once the packages are in drops NVIDIA's apt source, so a leg's apt-get
# update reads Ubuntu's archive alone. TensorRT for the FFS builds, every
# package at the build for this CUDA.
RUN keyring_sha256=d2a6b11c096396d868758b86dab1823b25e14d70333f1dfa74da5ddaf6a06dba \
 && trt=11.4.0.106-1+cuda13.4 \
 && curl -fsSL -o /tmp/cuda-keyring.deb \
      https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/x86_64/cuda-keyring_1.1-1_all.deb \
 && echo "${keyring_sha256}  /tmp/cuda-keyring.deb" | sha256sum -c - \
 && dpkg -i /tmp/cuda-keyring.deb \
 && rm /tmp/cuda-keyring.deb \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
      cuda-nvcc-13-4 cuda-cudart-dev-13-4 cuda-driver-dev-13-4 \
      libnvjpeg-dev-13-4 \
      "libnvinfer11=${trt}" "libnvinfer-headers-dev=${trt}" \
      "libnvinfer-safe-headers-dev=${trt}" "libnvinfer-dev=${trt}" \
      "libnvonnxparsers11=${trt}" "libnvonnxparsers-dev=${trt}" \
 && dpkg --purge cuda-keyring \
 && rm -rf /var/lib/apt/lists/*

ENV CUDAToolkit_ROOT=/usr/local/cuda-13.4
