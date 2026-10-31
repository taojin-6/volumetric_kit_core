#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin
#
# smoke.sh: checks run inside a built CI image, on a GPU host with
# `docker run --gpus all -e NVIDIA_DRIVER_CAPABILITIES=all`, before the image
# is pushed (README.md). It fails on the first check that does not hold.
set -euo pipefail

fail() {
  echo "smoke: $*" >&2
  exit 1
}

# The compilers a leg's build finds by default stay GCC's, as on a bare image
# with build-essential, even where clang is installed too.
gcc_only() {
  local macros
  macros=$("$1" -dM -E -x "$2" /dev/null)
  [[ $macros == *__GNUC__* && $macros != *__clang__* ]] || fail "$1 is not GCC"
}
gcc_only cc c
gcc_only c++ c++

# CUDA: FindCUDAToolkit finds the toolkit through nvcc under CUDAToolkit_ROOT,
# and the decoders include cuda.h and nvjpeg.h.
[ -n "${CUDAToolkit_ROOT:-}" ] || fail "CUDAToolkit_ROOT is not set"
cuda=${CUDAToolkit_ROOT##*-}
[[ $("$CUDAToolkit_ROOT/bin/nvcc" --version) == *"release $cuda,"* ]] ||
  fail "nvcc under $CUDAToolkit_ROOT is not release $cuda"
for header in cuda.h nvjpeg.h; do
  [ -f "$CUDAToolkit_ROOT/include/$header" ] || fail "no $header under $CUDAToolkit_ROOT"
done

# No installed package puts a driver or an implicit layer where the loader
# looks: the GPU legs see the NVIDIA ICD the toolkit mounts, and nothing else.
for dir in /usr/local/share/vulkan /usr/share/vulkan /etc/vulkan /etc/xdg/vulkan; do
  for kind in icd.d implicit_layer.d; do
    for manifest in "$dir/$kind"/*.json; do
      [ -e "$manifest" ] || continue
      if owner=$(dpkg -S "$manifest" 2>/dev/null); then
        fail "the loader would find ${owner}"
      fi
    done
  done
done

summary=$(vulkaninfo --summary 2>/dev/null) || fail "vulkaninfo found no device"
grep -q 'PHYSICAL_DEVICE_TYPE_DISCRETE_GPU' <<<"$summary" ||
  fail "no discrete GPU (run with --gpus all):"$'\n'"$summary"
if grep -q 'PHYSICAL_DEVICE_TYPE_CPU' <<<"$summary"; then
  fail "a software device is visible beside the GPU:"$'\n'"$summary"
fi

# lavapipe, where the image has it, loads when named and alone.
if [ -n "${VKC_CI_LAVAPIPE_ICD:-}" ]; then
  [ -f "$VKC_CI_LAVAPIPE_ICD" ] || fail "no lavapipe manifest at $VKC_CI_LAVAPIPE_ICD"
  if ldd /usr/lib/x86_64-linux-gnu/libvulkan_lvp.so* | grep 'not found'; then
    fail "lavapipe has missing libraries"
  fi
  summary=$(VK_ICD_FILENAMES="$VKC_CI_LAVAPIPE_ICD" vulkaninfo --summary 2>/dev/null) ||
    fail "lavapipe produced no device"
  grep -q 'PHYSICAL_DEVICE_TYPE_CPU' <<<"$summary" ||
    fail "VK_ICD_FILENAMES=$VKC_CI_LAVAPIPE_ICD gave no software device"
fi

# shellcheck source=/dev/null
. /etc/os-release
echo "smoke: ok ($PRETTY_NAME, CUDA $cuda)"
