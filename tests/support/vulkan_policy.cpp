// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/testing/vulkan_policy.hpp"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// NOLINTNEXTLINE(modernize-deprecated-headers): setenv and unsetenv are POSIX's
#include <stdlib.h>

#include "volumetric_kit/core/base/log.hpp"
#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/instance.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core::test {

namespace {

// Where the core's instance sends the validation layer's messages.
constexpr std::string_view kVulkanSource = "vulkan";

// The first layer that reads VK_KHRONOS_VALIDATION_VALIDATE_SYNC; an older
// one takes synchronization validation from VK_LAYER_ENABLES alone.
constexpr std::uint32_t kSyncSettingLayerVersion =
    VK_MAKE_API_VERSION(0, 1, 3, 268);

// The environment is read and set from the test thread, outside the threads a
// test starts.
const char* env(const char* name) {
  return std::getenv(name);  // NOLINT(concurrency-mt-unsafe)
}

// Sets `name` to `value`, or unsets it for null. include-cleaner: the macOS
// SDK declares setenv and unsetenv in a header of its own, not <stdlib.h>.
void set_env(const char* name, const char* value) {
  if (value != nullptr) {
    // NOLINTNEXTLINE(concurrency-mt-unsafe,misc-include-cleaner)
    ::setenv(name, value, 1);
  } else {
    // NOLINTNEXTLINE(concurrency-mt-unsafe,misc-include-cleaner)
    ::unsetenv(name);
  }
}

bool env_set(const char* name) {
  const char* value = env(name);
  return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

void print(LogLevel level, std::string_view source, std::string_view message) {
  std::cerr << '[' << source
            << (level == LogLevel::Error ? " error] " : " warning] ") << message
            << '\n';
}

}  // namespace

Validation requested_validation() {
  if (env_set("VKC_TEST_SYNC_VALIDATION")) return Validation::ShaderAccesses;
  if (env_set("VKC_TEST_VALIDATION")) return Validation::On;
  return Validation::Off;
}

bool device_required() { return env_set("VKC_REQUIRE_VULKAN_DEVICE"); }

InstanceConfig instance_config(Validation validation) {
  InstanceConfig config;
  config.app_name = "volumetric_kit tests";
  config.enable_validation = validation != Validation::Off;
  return config;
}

std::uint32_t validation_layer_version() {
  static const std::uint32_t version = [] {
    std::uint32_t count = 0;
    if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS) {
      return 0U;
    }
    std::vector<VkLayerProperties> layers(count);
    if (vkEnumerateInstanceLayerProperties(&count, layers.data()) !=
        VK_SUCCESS) {
      return 0U;
    }
    for (const VkLayerProperties& layer : layers) {
      if (std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
        return layer.specVersion;
      }
    }
    return 0U;
  }();
  return version;
}

void host_read_barrier(VkCommandBuffer cmd) {
  VkMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr,
                       0, nullptr);
}

ScopedEnv::ScopedEnv(const char* name, const char* value) : name_(name) {
  if (const char* old = env(name)) old_ = old;
  set_env(name, value);
}

ScopedEnv::~ScopedEnv() {
  set_env(name_.c_str(), old_ ? old_->c_str() : nullptr);
}

// Synchronization validation through its setting, or through
// VK_LAYER_ENABLES on a layer too old for the setting. Never both: a layer
// given both honours the deprecated one, and newer layers warn of the mix. A
// VK_LAYER_ENABLES the caller set is left alone.
ValidationSession::ValidationSession(Validation validation) {
  const auto set = [this](const char* name, const char* value) {
    settings_.push_back(std::make_unique<ScopedEnv>(name, value));
  };
  if (validation >= Validation::Sync) {
    const std::uint32_t layer = validation_layer_version();
    if (layer != 0 && layer < kSyncSettingLayerVersion) {
      if (env("VK_LAYER_ENABLES") == nullptr) {
        set("VK_LAYER_ENABLES",
            "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT");
      }
    } else {
      set("VK_KHRONOS_VALIDATION_VALIDATE_SYNC", "true");
    }
  }
  if (validation >= Validation::ShaderAccesses) {
    set("VK_KHRONOS_VALIDATION_SYNCVAL_SHADER_ACCESSES_HEURISTIC", "true");
  }
}

ValidationSession::~ValidationSession() = default;

// What the handler gathers, behind a pointer so it stays put for the handler.
struct LogCapture::State {
  ErrorHandler on_error;
  std::atomic<int> errors{0};
  mutable std::mutex mutex;
  std::vector<std::string> warnings;

  void on_message(LogLevel level, std::string_view source,
                  std::string_view message) {
    if (source == kVulkanSource && level == LogLevel::Error) {
      ++errors;
      if (on_error) {
        on_error(message);
      } else {
        print(level, source, message);
      }
      return;
    }
    if (level < LogLevel::Warning) return;
    // As the default sink would: why a test fails may be in one.
    print(level, source, message);
    if (level == LogLevel::Warning) {
      const std::scoped_lock lock(mutex);
      warnings.emplace_back(message);
    }
  }
};

LogCapture::LogCapture(ErrorHandler on_error)
    : state_(std::make_unique<State>()) {
  state_->on_error = std::move(on_error);
  set_log_handler([state = state_.get()](LogLevel level,
                                         std::string_view source,
                                         std::string_view message) {
    state->on_message(level, source, message);
  });
}

LogCapture::~LogCapture() { set_log_handler({}); }

int LogCapture::errors() const { return state_->errors.load(); }

std::vector<std::string> LogCapture::warnings() const {
  const std::scoped_lock lock(state_->mutex);
  return state_->warnings;
}

int LogCapture::exit_code(int code) const {
  if (errors() == 0) return code;
  return code == 0 || code == kSkipExitCode ? 1 : code;
}

Status check_layer_loaded(const Instance& instance) {
  if (requested_validation() == Validation::Off ||
      instance.validation_logged()) {
    return {};
  }
  return Status::unsupported(
      "VKC_TEST_VALIDATION or VKC_TEST_SYNC_VALIDATION is set, but validation "
      "is off or its messages do not reach the log sink; the instance's "
      "warning says why");
}

int no_device_exit_code(std::string_view why) {
  if (device_required()) {
    std::cerr << "VKC_REQUIRE_VULKAN_DEVICE is set, and " << why
              << "; failing\n";
    return 1;
  }
  std::cerr << why << "; skipping\n";
  return kSkipExitCode;
}

}  // namespace volumetric_kit::core::test
