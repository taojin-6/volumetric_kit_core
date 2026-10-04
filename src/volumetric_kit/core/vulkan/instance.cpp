// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/vulkan/instance.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "volumetric_kit/core/base/log.hpp"
#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/device_requirements.hpp"
#include "volumetric_kit/core/vulkan/physical_device_info.hpp"
#include "volumetric_kit/core/vulkan/vk_result.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core {
namespace {

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
constexpr const char* kLogSource = "vulkan";

bool layer_available(const char* name) {
  std::uint32_t count = 0;
  if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS) {
    return false;
  }
  std::vector<VkLayerProperties> layers(count);
  if (vkEnumerateInstanceLayerProperties(&count, layers.data()) != VK_SUCCESS) {
    return false;
  }
  return std::any_of(layers.begin(), layers.end(), [&](const auto& l) {
    return std::strcmp(l.layerName, name) == 0;
  });
}

bool instance_extension_available(const char* name) {
  std::uint32_t count = 0;
  if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) !=
      VK_SUCCESS) {
    return false;
  }
  std::vector<VkExtensionProperties> extensions(count);
  if (vkEnumerateInstanceExtensionProperties(nullptr, &count,
                                             extensions.data()) != VK_SUCCESS) {
    return false;
  }
  return std::any_of(extensions.begin(), extensions.end(), [&](const auto& e) {
    return std::strcmp(e.extensionName, name) == 0;
  });
}

// Routes the validation layer's messages to the log sink, so they reach an
// application's handler rather than the layer's own stderr. VK_FALSE: a
// messenger that is not a debugger must not abort the offending call.
VKAPI_ATTR VkBool32 VKAPI_CALL on_validation_message(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*types*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void* /*user*/) {
  LogLevel level = LogLevel::Info;
  if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
    level = LogLevel::Error;
  } else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) !=
             0) {
    level = LogLevel::Warning;
  }
  const char* message = data != nullptr && data->pMessage != nullptr
                            ? data->pMessage
                            : "(no message)";
  log_message(level, kLogSource, message);
  return VK_FALSE;
}

VkDebugUtilsMessengerCreateInfoEXT messenger_info() {
  VkDebugUtilsMessengerCreateInfoEXT info{};
  info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
  info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
  info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
  info.pfnUserCallback = on_validation_message;
  return info;
}

int device_type_score(VkPhysicalDeviceType type) {
  switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
      return 4;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
      return 3;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
      return 2;
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
      return 1;
    default:
      return 0;
  }
}

}  // namespace

Result<Instance> Instance::create(const InstanceConfig& config) {
  // The highest version the loader offers, up to 1.3: MoltenVK caps each
  // device's apiVersion at the instance's request, so asking low would hide
  // a 1.3 device. vkEnumerateInstanceVersion is 1.1; a 1.0 loader lacks it.
  std::uint32_t api_version = VK_API_VERSION_1_3;
  std::uint32_t loader_version = VK_API_VERSION_1_0;
  if (vkEnumerateInstanceVersion(&loader_version) != VK_SUCCESS) {
    loader_version = VK_API_VERSION_1_0;
  }
  api_version = std::min(api_version, loader_version);
  if (api_version < VK_API_VERSION_1_1) {
    return Status::unsupported(
        "a Vulkan 1.1 loader is required (vkGetPhysicalDeviceFeatures2 is "
        "1.1 core)");
  }

  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = config.app_name.c_str();
  app.pEngineName = "volumetric_kit_core";
  app.apiVersion = api_version;

  const bool validation =
      config.enable_validation && layer_available(kValidationLayer);
  if (config.enable_validation && !validation) {
    log_message(LogLevel::Warning, kLogSource,
                "validation requested, but VK_LAYER_KHRONOS_validation is not "
                "installed; continuing without it");
  }
  // VK_EXT_debug_utils backs two separate things: the validation messenger
  // and a profiler's labels. Only the first needs validation, so the request
  // stands on its own -- a Release build, the one worth profiling, keeps its
  // labels.
  const bool debug_utils_offered =
      instance_extension_available(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
  const bool messenger = validation && debug_utils_offered;
  if (validation && !debug_utils_offered) {
    log_message(LogLevel::Warning, kLogSource,
                "validation is on, but VK_EXT_debug_utils is unavailable; the "
                "layer's messages go to its own output, not the log sink");
  }
  const bool debug_utils =
      (config.request_debug_utils && debug_utils_offered) || messenger;
  if (!config.request_debug_utils && messenger) {
    log_message(LogLevel::Info, kLogSource,
                "request_debug_utils is false, but the validation messenger "
                "needs VK_EXT_debug_utils, so it stays enabled");
  }

  std::vector<const char*> layers;
  if (validation) layers.push_back(kValidationLayer);

  auto make = [&](bool portability, VkInstance* out) {
    std::vector<const char*> extensions = config.extensions;
    auto add = [&extensions](const char* name) {
      if (std::none_of(
              extensions.begin(), extensions.end(),
              [&](const char* e) { return std::strcmp(e, name) == 0; })) {
        extensions.push_back(name);
      }
    };
    if (debug_utils) add(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    // Guarded: headers older than 1.3.216 (Ubuntu 22.04's) do not define it.
#ifdef VK_KHR_portability_enumeration
    if (portability) add(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
#endif

    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app;
    info.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    info.ppEnabledLayerNames = layers.empty() ? nullptr : layers.data();
    info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    info.ppEnabledExtensionNames =
        extensions.empty() ? nullptr : extensions.data();
#ifdef VK_KHR_portability_enumeration
    if (portability) {
      info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
#endif
    // Chained so the layer also validates instance creation and destruction.
    VkDebugUtilsMessengerCreateInfoEXT chained = messenger_info();
    if (messenger) info.pNext = &chained;
    return vkCreateInstance(&info, nullptr, out);
  };

  Instance instance;
  VkResult created = make(/*portability=*/true, &instance.instance_);
  // A loader without portability enumeration refuses the extension; retry
  // without it.
  if (created == VK_ERROR_EXTENSION_NOT_PRESENT ||
      created == VK_ERROR_INCOMPATIBLE_DRIVER) {
    created = make(/*portability=*/false, &instance.instance_);
  }
  if (created != VK_SUCCESS) {
    instance.instance_ = VK_NULL_HANDLE;
    return vk_error(created, "vkCreateInstance");
  }
  instance.api_version_ = api_version;
  instance.validation_enabled_ = validation;
  instance.debug_utils_enabled_ = debug_utils;

  if (messenger) {
    auto create_messenger =
        reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance.instance_,
                                  "vkCreateDebugUtilsMessengerEXT"));
    const VkDebugUtilsMessengerCreateInfoEXT info = messenger_info();
    if (create_messenger == nullptr ||
        create_messenger(instance.instance_, &info, nullptr,
                         &instance.messenger_) != VK_SUCCESS) {
      instance.messenger_ = VK_NULL_HANDLE;
      log_message(LogLevel::Warning, kLogSource,
                  "the validation messenger could not be created; the layer's "
                  "messages go to its own output, not the log sink");
    }
  }
  return instance;
}

Result<VkPhysicalDevice> Instance::select_physical_device(
    const DeviceRequirements& reqs, VkSurfaceKHR surface) const {
  if (reqs.needs_present && surface == VK_NULL_HANDLE) {
    return Status::invalid_argument(
        "select_physical_device: needs_present requires a surface");
  }
  std::uint32_t count = 0;
  VKC_VK_TRY(vkEnumeratePhysicalDevices(instance_, &count, nullptr));
  std::vector<VkPhysicalDevice> devices(count);
  const VkResult listed =
      vkEnumeratePhysicalDevices(instance_, &count, devices.data());
  if (listed != VK_SUCCESS && listed != VK_INCOMPLETE) {
    return vk_error(listed, "vkEnumeratePhysicalDevices");
  }
  devices.resize(count);
  if (devices.empty()) {
    return Status::unsupported("no Vulkan physical device is available");
  }

  VkPhysicalDevice best = VK_NULL_HANDLE;
  int best_score = -1;
  std::string refusals;
  for (VkPhysicalDevice device : devices) {
    const PhysicalDeviceInfo caps = PhysicalDeviceInfo::query(device);
    const Result<DeviceSupport> support =
        check_device_support(caps, reqs, surface);
    if (!support) {
      refusals += refusals.empty() ? "" : "; ";
      refusals += support.status().message();
      continue;
    }
    const int score = device_type_score(caps.properties().deviceType);
    if (score > best_score) {
      best_score = score;
      best = device;
    }
  }
  if (best == VK_NULL_HANDLE) {
    return Status::unsupported("no physical device meets the requirements: " +
                               refusals);
  }
  return best;
}

Instance::Instance(Instance&& other) noexcept
    : instance_(std::exchange(other.instance_, VK_NULL_HANDLE)),
      messenger_(std::exchange(other.messenger_, VK_NULL_HANDLE)),
      api_version_(std::exchange(other.api_version_, 0)),
      validation_enabled_(std::exchange(other.validation_enabled_, false)),
      debug_utils_enabled_(std::exchange(other.debug_utils_enabled_, false)) {}

Instance& Instance::operator=(Instance&& other) noexcept {
  if (this != &other) {
    destroy();
    instance_ = std::exchange(other.instance_, VK_NULL_HANDLE);
    messenger_ = std::exchange(other.messenger_, VK_NULL_HANDLE);
    api_version_ = std::exchange(other.api_version_, 0);
    validation_enabled_ = std::exchange(other.validation_enabled_, false);
    debug_utils_enabled_ = std::exchange(other.debug_utils_enabled_, false);
  }
  return *this;
}

Instance::~Instance() { destroy(); }

void Instance::destroy() noexcept {
  if (messenger_ != VK_NULL_HANDLE) {
    auto destroy_messenger =
        reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_,
                                  "vkDestroyDebugUtilsMessengerEXT"));
    if (destroy_messenger != nullptr) {
      destroy_messenger(instance_, messenger_, nullptr);
    }
    messenger_ = VK_NULL_HANDLE;
  }
  if (instance_ != VK_NULL_HANDLE) {
    vkDestroyInstance(instance_, nullptr);
    instance_ = VK_NULL_HANDLE;
  }
  api_version_ = 0;
  validation_enabled_ = false;
  debug_utils_enabled_ = false;
}

}  // namespace volumetric_kit::core
