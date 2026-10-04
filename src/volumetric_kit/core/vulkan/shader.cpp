// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/vulkan/shader.hpp"

#include <cstddef>
#include <cstdint>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/unique_handle.hpp"
#include "volumetric_kit/core/vulkan/vk_result.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {

Result<ShaderModule> ShaderModule::create(VkDevice device,
                                          const std::uint32_t* code,
                                          std::size_t size_bytes) {
  // SPIR-V is a stream of 32-bit words, so codeSize is a non-zero multiple of
  // 4. Checked before the device, so a device-less test reaches it.
  if (code == nullptr || size_bytes == 0 || size_bytes % 4 != 0) {
    return Status::invalid_argument(
        "ShaderModule::create: SPIR-V must be non-null and a non-zero "
        "multiple of 4 bytes");
  }
  if (device == VK_NULL_HANDLE) {
    return Status::invalid_argument("ShaderModule::create: device is null");
  }
  VkShaderModuleCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  info.codeSize = size_bytes;
  info.pCode = code;
  VkShaderModule handle = VK_NULL_HANDLE;
  VKC_VK_TRY(vkCreateShaderModule(device, &info, nullptr, &handle));
  ShaderModule shader;
  shader.module_ =
      UniqueHandle<VkShaderModule, vkDestroyShaderModule>(device, handle);
  return shader;
}

}  // namespace volumetric_kit::core
