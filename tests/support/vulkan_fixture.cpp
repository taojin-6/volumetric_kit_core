// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/testing/vulkan_fixture.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// NOLINTNEXTLINE(modernize-deprecated-headers): setenv and unsetenv are POSIX's
#include <stdlib.h>

#include <gtest/gtest.h>

#include "volumetric_kit/core/base/check.hpp"
#include "volumetric_kit/core/base/log.hpp"
#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/allocator.hpp"
#include "volumetric_kit/core/vulkan/device.hpp"
#include "volumetric_kit/core/vulkan/device_requirements.hpp"
#include "volumetric_kit/core/vulkan/instance.hpp"
#include "volumetric_kit/core/vulkan/physical_device_info.hpp"
#include "volumetric_kit/core/vulkan/vk_result.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core::test {

namespace detail {

// One shared device: made for a validation level and requirements, from the
// instance for that level.
struct DeviceSlot {
  Validation validation = Validation::Off;
  DeviceRequirements requirements;
  std::optional<Device> device;
  std::string error;  // why there is no device
  bool lost = false;  // replaced when next borrowed
};

}  // namespace detail

namespace {

// Where the core's instance sends the validation layer's messages.
constexpr std::string_view kVulkanSource = "vulkan";

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

// The layer's settings for `validation`, as the environment variables it reads
// when an instance is created. Synchronization validation has two spellings:
// the settings variable, and VK_LAYER_ENABLES, which older layers read; the
// latter is left alone when the caller set it.
std::vector<std::unique_ptr<ScopedEnv>> layer_settings(Validation validation) {
  std::vector<std::unique_ptr<ScopedEnv>> settings;
  if (validation >= Validation::Sync) {
    settings.push_back(std::make_unique<ScopedEnv>(
        "VK_KHRONOS_VALIDATION_VALIDATE_SYNC", "true"));
    if (env("VK_LAYER_ENABLES") == nullptr) {
      settings.push_back(std::make_unique<ScopedEnv>(
          "VK_LAYER_ENABLES",
          "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT"));
    }
  }
  if (validation >= Validation::ShaderAccesses) {
    settings.push_back(std::make_unique<ScopedEnv>(
        "VK_KHRONOS_VALIDATION_SYNCVAL_SHADER_ACCESSES_HEURISTIC", "true"));
  }
  return settings;
}

// Whether two requirements make the same device. The members are bound one
// by one so that a member added to DeviceRequirements stops this compiling
// until it is compared too.
bool same_requirements(const DeviceRequirements& a,
                       const DeviceRequirements& b) {
  const auto& [a_api, a_queues, a_present, a_extensions, a_optional, a_features,
               a_timeline, a_scalar, a_dynamic, a_chain] = a;
  const auto& [b_api, b_queues, b_present, b_extensions, b_optional, b_features,
               b_timeline, b_scalar, b_dynamic, b_chain] = b;
  return a_api == b_api && a_queues == b_queues && a_present == b_present &&
         a_extensions == b_extensions && a_optional == b_optional &&
         std::memcmp(&a_features, &b_features, sizeof(a_features)) == 0 &&
         a_timeline == b_timeline && a_scalar == b_scalar &&
         a_dynamic == b_dynamic && a_chain == b_chain;
}

// An instance for one validation level, or why there is none.
struct InstanceSlot {
  std::optional<Instance> instance;
  std::string error;
};

// A message logged while an instance was made.
struct Logged {
  LogLevel level = LogLevel::Info;
  std::string source;
  std::string message;
};

// The process's shared instances and devices, made on first use. One that
// could not be made is not retried: every later test reports the same.
class Registry {
 public:
  static Registry& get() {
    static Registry registry;
    return registry;
  }

  // The instance for `validation`. When this call makes it, what was logged
  // meanwhile is added to `logged`, for the caller to judge: the loader's
  // errors about a layer that failed to load are no failure when the layer
  // was only a fixture's wish.
  InstanceSlot& instance(Validation validation, std::vector<Logged>& logged) {
    auto [it, made] = instances_.try_emplace(validation);
    if (made) {
      InstanceConfig config = instance_config();
      config.enable_validation = validation != Validation::Off;
      std::mutex mutex;
      set_log_handler([&](LogLevel level, std::string_view source,
                          std::string_view message) {
        const std::scoped_lock lock(mutex);
        logged.push_back({level, std::string(source), std::string(message)});
      });
      {
        // Read as the instance is created, so they need not outlive it.
        const auto settings = layer_settings(validation);
        Result<Instance> instance = Instance::create(config);
        if (instance) {
          it->second.instance.emplace(*std::move(instance));
        } else {
          it->second.error =
              "no Vulkan instance: " + instance.status().message();
        }
      }
      set_log_handler({});
    }
    return it->second;
  }

  detail::DeviceSlot& device(Validation validation, const Instance& instance,
                             const PhysicalDeviceInfo& physical,
                             const DeviceRequirements& requirements) {
    auto found =
        std::find_if(devices_.begin(), devices_.end(), [&](const auto& slot) {
          return slot->validation == validation &&
                 same_requirements(slot->requirements, requirements);
        });
    if (found == devices_.end()) {
      auto slot = std::make_unique<detail::DeviceSlot>();
      slot->validation = validation;
      slot->requirements = requirements;
      make(*slot, instance, physical);
      devices_.push_back(std::move(slot));
      return *devices_.back();
    }
    detail::DeviceSlot& slot = **found;
    if (slot.lost) {
      // The test that lost it is gone, and with it whatever it made there.
      slot.device.reset();
      slot.lost = false;
      make(slot, instance, physical);
    }
    return slot;
  }

  // An instance held while the tests run, so the loader never unloads the
  // driver between tests that make their own: a driver can take resources at
  // each load that its unload does not give back.
  void hold_anchor() {
    Result<Instance> anchor = Instance::create(InstanceConfig{});
    if (anchor) anchor_.emplace(*std::move(anchor));
  }

  // Destroys every shared device, then the instances, and returns the errors
  // the layer reported meanwhile: chiefly an object a test made and never
  // destroyed, which it finds only as its device goes.
  std::vector<std::string> release() {
    std::mutex mutex;
    std::vector<std::string> errors;
    set_log_handler(
        [&](LogLevel level, std::string_view source, std::string_view message) {
          if (source == kVulkanSource && level == LogLevel::Error) {
            const std::scoped_lock lock(mutex);
            errors.emplace_back(message);
          } else if (level >= LogLevel::Warning) {
            print(level, source, message);
          }
        });
    devices_.clear();
    instances_.clear();
    anchor_.reset();
    set_log_handler({});
    return errors;
  }

 private:
  Registry() = default;

  static void make(detail::DeviceSlot& slot, const Instance& instance,
                   const PhysicalDeviceInfo& physical) {
    Result<Device> device =
        Device::create(instance, physical, slot.requirements);
    if (device) {
      slot.device.emplace(*std::move(device));
      slot.error.clear();
    } else {
      slot.error = "Device::create: " + device.status().message();
    }
  }

  // Destroyed in reverse: the devices, each before the instance it was made
  // from, then the anchor.
  std::optional<Instance> anchor_;
  std::map<Validation, InstanceSlot> instances_;
  std::vector<std::unique_ptr<detail::DeviceSlot>> devices_;
};

// No instance or device: the test skips, or fails where one is required.
void no_device(const std::string& why) {
  if (device_required()) {
    FAIL() << "VKC_REQUIRE_VULKAN_DEVICE is set, and " << why;
  }
  GTEST_SKIP() << why;
}

class VulkanEnvironment final : public ::testing::Environment {
 public:
  void SetUp() override {
    // Instances a test makes itself run the environment's checks too.
    settings_ = layer_settings(requested_validation());
    if (::testing::UnitTest::GetInstance()->test_to_run_count() > 1) {
      Registry::get().hold_anchor();
    }
  }

  void TearDown() override {
    for (const std::string& error : Registry::get().release()) {
      ADD_FAILURE() << "[vulkan error] as the shared devices were destroyed "
                       "(an object a test never destroyed?): "
                    << error;
    }
    settings_.clear();
  }

 private:
  std::vector<std::unique_ptr<ScopedEnv>> settings_;
};

}  // namespace

Validation requested_validation() {
  if (env_set("VKC_TEST_SYNC_VALIDATION")) return Validation::ShaderAccesses;
  if (env_set("VKC_TEST_VALIDATION")) return Validation::On;
  return Validation::Off;
}

bool device_required() { return env_set("VKC_REQUIRE_VULKAN_DEVICE"); }

InstanceConfig instance_config() {
  InstanceConfig config;
  config.app_name = "volumetric_kit tests";
  config.enable_validation = requested_validation() != Validation::Off;
  return config;
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

// What the fixture's log handler gathers, behind a pointer so it stays put
// for the handler.
struct VulkanTest::Capture {
  std::atomic<bool> allowing{false};
  std::atomic<int> allowed{0};
  mutable std::mutex mutex;
  std::vector<std::string> warnings;

  void on_message(LogLevel level, std::string_view source,
                  std::string_view message) {
    if (source == kVulkanSource && level == LogLevel::Error) {
      if (allowing) {
        ++allowed;
      } else {
        ADD_FAILURE() << "[vulkan error] " << message;
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

VulkanTest::VulkanTest() : capture_(std::make_unique<Capture>()) {}

// The handler stays until here, so what a derived fixture's members report as
// they are destroyed, after TearDown, still fails the test.
VulkanTest::~VulkanTest() { set_log_handler({}); }

void VulkanTest::SetUp() {
  const Validation requested = requested_validation();
  validation_ = std::max(requested, validation());
  std::vector<Logged> logged;
  InstanceSlot& slot = Registry::get().instance(validation_, logged);
  set_log_handler([capture = capture_.get()](LogLevel level,
                                             std::string_view source,
                                             std::string_view message) {
    capture->on_message(level, source, message);
  });
  // A layer only the fixture asked for, which did not load: the loader's
  // errors say why, and fail nothing.
  const bool layer_missed =
      requested == Validation::Off && validation_ != Validation::Off &&
      !(slot.instance && slot.instance->validation_logged());
  for (const Logged& message : logged) {
    if (layer_missed && message.level == LogLevel::Error) {
      print(LogLevel::Warning, message.source, message.message);
    } else {
      capture_->on_message(message.level, message.source, message.message);
    }
  }
  if (!slot.instance) {
    no_device(slot.error);
    return;
  }
  instance_ = &*slot.instance;
  if (requested != Validation::Off && !instance_->validation_logged()) {
    FAIL() << "VKC_TEST_VALIDATION or VKC_TEST_SYNC_VALIDATION is set, but "
              "validation is off or its messages do not reach the log sink; "
              "the instance's warning says why";
  }
  Result<PhysicalDeviceInfo> physical =
      instance_->select_physical_device(requirements());
  if (!physical) {
    no_device(physical.status().message());
    return;
  }
  physical_ = *std::move(physical);
  ready_ = true;
}

const Instance& VulkanTest::instance() const {
  VKC_CHECK(instance_ != nullptr,
            "VulkanTest::instance() before SetUp made it");
  return *instance_;
}

void VulkanTest::allow_validation_errors() { capture_->allowing = true; }

int VulkanTest::allowed_validation_errors() const {
  return capture_->allowed.load();
}

std::vector<std::string> VulkanTest::warnings() const {
  const std::scoped_lock lock(capture_->mutex);
  return capture_->warnings;
}

VulkanDeviceTest::VulkanDeviceTest() = default;

VulkanDeviceTest::~VulkanDeviceTest() = default;

void VulkanDeviceTest::SetUp() {
  VulkanTest::SetUp();
  if (base_setup_incomplete()) return;
  ready_ = false;
  const DeviceRequirements reqs = requirements();
  if (reqs.feature_chain != nullptr) {
    FAIL() << "VulkanDeviceTest shares its device among tests with equal "
              "requirements, and cannot compare a feature_chain; make this "
              "device in the test, on a VulkanTest";
  }
  detail::DeviceSlot& slot =
      Registry::get().device(active_validation(), instance(), physical(), reqs);
  if (!slot.device) {
    FAIL() << slot.error;
  }
  slot_ = &slot;
  device_ = &*slot.device;
  Result<Allocator> allocator =
      Allocator::create(instance().handle(), *device_);
  ASSERT_TRUE(allocator.ok()) << allocator.status().message();
  allocator_.emplace(*std::move(allocator));
  ready_ = true;
}

void VulkanDeviceTest::TearDown() {
  if (device_ == nullptr) return;
  // The test's work must be done before its objects go, and the device left
  // idle for the next test.
  const Status idle = device_->wait_idle();
  EXPECT_TRUE(idle.ok()) << "waiting for the device after the test: "
                         << idle.message();
  if (vk_result(idle) == VK_ERROR_DEVICE_LOST) slot_->lost = true;
}

Device& VulkanDeviceTest::device() {
  VKC_CHECK(device_ != nullptr, "VulkanDeviceTest::device() before SetUp");
  return *device_;
}

Allocator& VulkanDeviceTest::allocator() {
  VKC_CHECK(allocator_.has_value(),
            "VulkanDeviceTest::allocator() before SetUp");
  return *allocator_;
}

namespace detail {

::testing::Environment* register_vulkan_environment() noexcept {
  return ::testing::AddGlobalTestEnvironment(
      std::make_unique<VulkanEnvironment>().release());
}

}  // namespace detail

}  // namespace volumetric_kit::core::test
