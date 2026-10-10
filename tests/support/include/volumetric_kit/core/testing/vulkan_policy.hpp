// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#pragma once

/// @file vulkan_policy.hpp
/// @brief The policy the environment sets for the family's Vulkan tests, with
///        no test framework (`volumetric_kit::core_test_policy`).
///
/// Three variables set one policy for every test in a process. A variable
/// counts as set unless it is unset, empty or `0`.
///
/// - `VKC_REQUIRE_VULKAN_DEVICE=1`: a test that finds no instance or device
///   fails instead of skipping, so a runner cannot pass by skipping.
/// - `VKC_TEST_VALIDATION=1`: every instance enables the Khronos validation
///   layer, and a test fails if the layer is missing, fails to load, or its
///   messages do not reach the log sink.
/// - `VKC_TEST_SYNC_VALIDATION=1`: the above, plus synchronization validation
///   and, on layers that have it, its tracking of what shaders access.
///
/// Every error the layer reports -- any `LogLevel::Error` from source
/// `"vulkan"` -- fails the test. The GoogleTest fixtures
/// (`vulkan_fixture.hpp`) apply all of this themselves. A test that is a
/// `main()` of its own applies it with the pieces below, and returns
/// @ref volumetric_kit::core::test::kSkipExitCode to skip, which CTest's
/// `SKIP_RETURN_CODE` reports as a skip.
///
/// @code
/// namespace test = volumetric_kit::core::test;
///
/// // Everything made on the device is destroyed before this returns, while
/// // the capture still counts what the layer reports.
/// int run() {
///   Result<Instance> instance = Instance::create(test::instance_config());
///   if (!instance) {
///     return test::no_device_exit_code(instance.status().message());
///   }
///   if (Status loaded = test::check_layer_loaded(*instance); !loaded.ok()) {
///     std::fprintf(stderr, "%s\n", loaded.message().c_str());
///     return 1;
///   }
///   // ... select a physical device (no_device_exit_code without one), make
///   // the device, run the cases.
///   return 0;
/// }
///
/// int main() {
///   const test::ValidationSession validation;
///   const test::LogCapture log;
///   return log.exit_code(run());
/// }
/// @endcode

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "volumetric_kit/core/base/result.hpp"
#include "volumetric_kit/core/vulkan/instance.hpp"
#include "volumetric_kit/core/vulkan/vulkan.hpp"

namespace volumetric_kit::core::test {

/// @brief What the validation layer checks; each level adds to the one before.
enum class Validation : std::uint8_t {
  Off,  ///< No validation layer.
  On,   ///< The Khronos validation layer.
  /// Plus synchronization validation, which reports a missing barrier.
  Sync,
  /// Plus synchronization validation's tracking of what shaders access
  /// through their descriptors, on layers that have it. Without it, the layer
  /// reports no hazard against a compute shader's storage-buffer write.
  ShaderAccesses,
};

/// @brief The validation the environment asks of every test.
/// @return @ref Validation::ShaderAccesses under `VKC_TEST_SYNC_VALIDATION`;
///         otherwise @ref Validation::On under `VKC_TEST_VALIDATION`;
///         otherwise @ref Validation::Off.
Validation requested_validation();

/// @return Whether `VKC_REQUIRE_VULKAN_DEVICE` is set: a test that finds no
///         instance or device fails rather than skips.
bool device_required();

/// @brief The configuration for an instance a test makes itself.
///
/// The layer's settings beyond loading it come from a
/// @ref ValidationSession alive as the instance is created; the fixtures hold
/// one for the environment's level while their tests run.
/// @param validation  The level; the environment's by default.
/// @return An `InstanceConfig` named for the tests, enabling the validation
///         layer unless @p validation is @ref Validation::Off.
InstanceConfig instance_config(Validation validation = requested_validation());

/// @return The installed Khronos validation layer's `specVersion`, as the
///         loader enumerates it, or 0 without one. Queried once per process.
std::uint32_t validation_layer_version();

/// @brief Make the transfers recorded before it visible to the host, for a
///        readback through a mapped buffer.
///
/// A fence wait orders the host after the work but does not make its writes
/// visible; coherent memory needs no invalidate on top of this.
/// @param cmd  The command buffer to record the barrier into.
void host_read_barrier(VkCommandBuffer cmd);

/// @brief Set an environment variable for this object's lifetime, then
///        restore the value it had, or unset it.
///
/// @code
/// {
///   const test::ScopedEnv sync("VKC_TEST_SYNC_VALIDATION", "1");
///   EXPECT_EQ(test::requested_validation(),
///             test::Validation::ShaderAccesses);
/// }
/// @endcode
class ScopedEnv {
 public:
  /// @param name   The variable.
  /// @param value  Its value while this lives, or null to unset it.
  ScopedEnv(const char* name, const char* value);
  ~ScopedEnv();
  ScopedEnv(const ScopedEnv&) = delete;
  ScopedEnv& operator=(const ScopedEnv&) = delete;
  ScopedEnv(ScopedEnv&&) = delete;
  ScopedEnv& operator=(ScopedEnv&&) = delete;

 private:
  std::string name_;
  std::optional<std::string> old_;
};

/// @brief The validation layer's settings for a level, in the process
///        environment while this lives: the layer reads them as each
///        instance is created.
///
/// It loads no layer; an instance from @ref instance_config does that.
/// At @ref Validation::Sync and above, it overrides legacy layer enables and
/// disables (including their environment aliases), which otherwise suppress
/// the synchronization setting. The caller's values return on destruction.
///
/// @code
/// const test::ValidationSession validation(test::Validation::Sync);
/// Result<Instance> instance =
///     Instance::create(test::instance_config(test::Validation::Sync));
/// @endcode
class ValidationSession {
 public:
  /// @param validation  The level; the environment's by default.
  explicit ValidationSession(Validation validation = requested_validation());
  ~ValidationSession();
  ValidationSession(const ValidationSession&) = delete;
  ValidationSession& operator=(const ValidationSession&) = delete;
  ValidationSession(ValidationSession&&) = delete;
  ValidationSession& operator=(ValidationSession&&) = delete;

 private:
  std::vector<std::unique_ptr<ScopedEnv>> settings_;
};

/// @brief The log handler while this lives, counting the validation layer's
///        errors; the default sink returns when it is destroyed.
///
/// A `"vulkan"` error goes to the given handler, or to stderr without one.
/// Other warnings and errors go to stderr, as the default sink sends them.
/// Installing another handler meanwhile ends the capture.
///
/// @code
/// const test::LogCapture log;
/// const int code = run();  // its objects destroyed inside
/// return log.exit_code(code);
/// @endcode
class LogCapture {
 public:
  /// @brief Takes a `"vulkan"` error's message, on the thread that logged it.
  using ErrorHandler = std::function<void(std::string_view message)>;

  /// @param on_error  What a `"vulkan"` error does besides being counted.
  explicit LogCapture(ErrorHandler on_error = {});
  ~LogCapture();
  LogCapture(const LogCapture&) = delete;
  LogCapture& operator=(const LogCapture&) = delete;
  LogCapture(LogCapture&&) = delete;
  LogCapture& operator=(LogCapture&&) = delete;

  /// @return The `"vulkan"` errors logged so far.
  int errors() const;
  /// @return The warnings logged so far.
  std::vector<std::string> warnings() const;
  /// @param code  The test's own exit code.
  /// @return @p code; or 1, for a test that passed or skipped, when the layer
  ///         reported an error.
  int exit_code(int code) const;

 private:
  struct State;
  std::unique_ptr<State> state_;
};

/// @brief Whether an instance meets the environment's request for
///        validation.
/// @param instance  The instance a test made.
/// @return OK, unless the environment asks for validation and the layer is
///         off on @p instance or its messages do not reach the log sink; then
///         @ref Status::Code::Unsupported, saying so.
Status check_layer_loaded(const Instance& instance);

/// The exit code with which a test that is its own `main()` skips; register
/// it as the test's `SKIP_RETURN_CODE`.
inline constexpr int kSkipExitCode = 77;

/// @brief Report why a test has no instance or device, and end it as the
///        environment asks.
/// @param why  Why; printed to stderr.
/// @return @ref kSkipExitCode, or 1 under `VKC_REQUIRE_VULKAN_DEVICE`.
int no_device_exit_code(std::string_view why);

}  // namespace volumetric_kit::core::test
