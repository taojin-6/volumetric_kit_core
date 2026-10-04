// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/vulkan/instance.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "support.hpp"
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

// The instance extensions the loader and drivers offer; empty when the
// enumeration fails, so each reads as unavailable.
std::vector<VkExtensionProperties> instance_extensions() {
  std::uint32_t count = 0;
  if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) !=
      VK_SUCCESS) {
    return {};
  }
  std::vector<VkExtensionProperties> extensions(count);
  if (vkEnumerateInstanceExtensionProperties(nullptr, &count,
                                             extensions.data()) != VK_SUCCESS) {
    return {};
  }
  extensions.resize(count);
  return extensions;
}

bool offered(const std::vector<VkExtensionProperties>& extensions,
             const char* name) {
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
  // a 1.3 device. vkEnumerateInstanceVersion is 1.1, so it is resolved, not
  // linked: a 1.0 loader does not export it.
  std::uint32_t loader_version = VK_API_VERSION_1_0;
  const auto enumerate_version =
      reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
          vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
  if (enumerate_version == nullptr ||
      enumerate_version(&loader_version) != VK_SUCCESS) {
    loader_version = VK_API_VERSION_1_0;
  }
  const std::uint32_t api_version =
      std::min<std::uint32_t>(VK_API_VERSION_1_3, loader_version);
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

  const std::vector<VkExtensionProperties> extensions_offered =
      instance_extensions();
  const bool debug_utils_offered =
      offered(extensions_offered, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
  // Portability enumeration is checked, not tried: a loader that lacks it
  // would fail a create, and report that through the chained messenger as an
  // error. Guarded: headers older than 1.3.216 (Ubuntu 22.04's) do not
  // define it.
#ifdef VK_KHR_portability_enumeration
  const bool portability = offered(
      extensions_offered, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
#endif

  // What the instance enables, given whether the validation layer is on.
  // VK_EXT_debug_utils backs two separate things: the validation messenger
  // and a profiler's labels. Only the first needs validation, so the request
  // stands on its own -- a Release build, the one worth profiling, keeps its
  // labels.
  struct Plan {
    bool validation = false;
    bool messenger = false;
    bool debug_utils = false;
  };
  auto plan_for = [&](bool validation) {
    Plan plan;
    plan.validation = validation;
    plan.messenger = validation && debug_utils_offered;
    plan.debug_utils =
        (config.request_debug_utils && debug_utils_offered) || plan.messenger;
    return plan;
  };

  auto make = [&](const Plan& plan, VkInstance* out) {
    std::vector<const char*> extensions = config.extensions;
    auto add = [&extensions](const char* name) {
      if (std::none_of(
              extensions.begin(), extensions.end(),
              [&](const char* e) { return std::strcmp(e, name) == 0; })) {
        extensions.push_back(name);
      }
    };
    if (plan.debug_utils) add(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
#ifdef VK_KHR_portability_enumeration
    if (portability) add(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
#endif
    std::vector<const char*> layers;
    if (plan.validation) layers.push_back(kValidationLayer);

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
    if (plan.messenger) info.pNext = &chained;
    return vkCreateInstance(&info, nullptr, out);
  };

  Plan plan =
      plan_for(config.enable_validation && layer_available(kValidationLayer));
  if (config.enable_validation && !plan.validation) {
    log_message(LogLevel::Warning, kLogSource,
                "validation requested, but VK_LAYER_KHRONOS_validation is not "
                "installed; continuing without it");
  }
  // Created into a local, so a failed create leaves nothing to destroy.
  VkInstance handle = VK_NULL_HANDLE;
  VkResult created = make(plan, &handle);
  if (created == VK_ERROR_LAYER_NOT_PRESENT && plan.validation) {
    // The layer's manifest was found but its library did not load (e.g.
    // Homebrew's on macOS, outside the loader's search path): as when it is
    // not installed, continue without it.
    log_message(LogLevel::Warning, kLogSource,
                "validation requested, but VK_LAYER_KHRONOS_validation failed "
                "to load; continuing without it");
    plan = plan_for(false);
    handle = VK_NULL_HANDLE;
    created = make(plan, &handle);
  }
  if (created != VK_SUCCESS) return vk_error(created, "vkCreateInstance");

  if (plan.validation && !plan.messenger) {
    log_message(LogLevel::Warning, kLogSource,
                "validation is on, but VK_EXT_debug_utils is unavailable; the "
                "layer's messages go to its own output, not the log sink");
  }
  if (!config.request_debug_utils && plan.messenger) {
    log_message(LogLevel::Info, kLogSource,
                "request_debug_utils is false, but the validation messenger "
                "needs VK_EXT_debug_utils, so it stays enabled");
  }

  Instance instance;
  instance.instance_ = handle;
  instance.api_version_ = api_version;
  instance.validation_enabled_ = plan.validation;
  instance.debug_utils_enabled_ = plan.debug_utils;

  if (plan.messenger) {
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

Result<PhysicalDeviceInfo> Instance::select_physical_device(
    const DeviceRequirements& reqs, VkSurfaceKHR surface) const {
  return detail::select_physical_device(*this, reqs, surface, nullptr);
}

Result<PhysicalDeviceInfo> detail::select_physical_device(
    const Instance& instance, const DeviceRequirements& reqs,
    VkSurfaceKHR surface,
    const std::function<Status(const PhysicalDeviceInfo&)>& accept) {
  if (reqs.needs_present && surface == VK_NULL_HANDLE) {
    return Status::invalid_argument(
        "select_physical_device: needs_present requires a surface");
  }
  std::uint32_t count = 0;
  VKC_VK_TRY(vkEnumeratePhysicalDevices(instance.handle(), &count, nullptr));
  std::vector<VkPhysicalDevice> devices(count);
  const VkResult listed =
      vkEnumeratePhysicalDevices(instance.handle(), &count, devices.data());
  if (listed != VK_SUCCESS && listed != VK_INCOMPLETE) {
    return vk_error(listed, "vkEnumeratePhysicalDevices");
  }
  devices.resize(count);
  if (devices.empty()) {
    return Status::unsupported("no Vulkan physical device is available");
  }

  std::optional<PhysicalDeviceInfo> best;
  int best_score = -1;
  std::string refusals;
  const auto refuse = [&refusals](const Status& why) {
    refusals += refusals.empty() ? "" : "; ";
    refusals += why.message();
  };
  for (VkPhysicalDevice device : devices) {
    PhysicalDeviceInfo caps =
        PhysicalDeviceInfo::query(device, instance.api_version());
    const Result<DeviceSupport> support =
        check_device_support(caps, reqs, surface);
    if (!support) {
      refuse(support.status());
      continue;
    }
    if (accept) {
      const Status accepted = accept(caps);
      if (!accepted) {
        refuse(accepted);
        continue;
      }
    }
    const int score = device_type_score(caps.properties().deviceType);
    if (score > best_score) {
      best_score = score;
      best = std::move(caps);
    }
  }
  if (!best) {
    return Status::unsupported("no physical device meets the requirements: " +
                               refusals);
  }
  return *std::move(best);
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
