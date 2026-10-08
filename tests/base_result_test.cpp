// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/base/result.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace volumetric_kit::core {
namespace {

// --- Status -----------------------------------------------------------------

TEST(Status, DefaultIsOk) {
  const Status s;
  EXPECT_TRUE(s.ok());
  EXPECT_TRUE(static_cast<bool>(s));
  EXPECT_EQ(s.domain(), Status::Code::Ok);
  EXPECT_EQ(s.backend(), std::nullopt);
  EXPECT_EQ(s.detail(), 0);
  EXPECT_TRUE(s.message().empty());
}

TEST(Status, DomainFactoriesSetDomainAndMessageWithNoDetail) {
  struct Case {
    Status status;
    Status::Code domain;
  };
  const Case cases[] = {
      {Status::invalid_argument("m"), Status::Code::InvalidArgument},
      {Status::not_found("m"), Status::Code::NotFound},
      {Status::unsupported("m"), Status::Code::Unsupported},
      {Status::out_of_memory("m"), Status::Code::OutOfMemory},
      {Status::io_error("m"), Status::Code::IoError},
      {Status::numerical("m"), Status::Code::Numerical},
  };
  for (const Case& c : cases) {
    EXPECT_FALSE(c.status.ok());
    EXPECT_EQ(c.status.domain(), c.domain);
    EXPECT_EQ(c.status.backend(), std::nullopt) << to_string(c.domain);
    EXPECT_EQ(c.status.detail(), 0) << to_string(c.domain);
    EXPECT_EQ(c.status.message(), "m");
  }
}

TEST(Status, BackendErrorCarriesItsBackendAndDetail) {
  // -4 is VK_ERROR_DEVICE_LOST; the base tier only carries the number.
  const Status s =
      Status::backend_error(Status::Backend::Vulkan, -4, "vkQueueSubmit");
  EXPECT_FALSE(s.ok());
  EXPECT_EQ(s.domain(), Status::Code::Backend);
  EXPECT_EQ(s.backend(), Status::Backend::Vulkan);
  EXPECT_EQ(s.detail(), -4);
  EXPECT_EQ(s.message(), "vkQueueSubmit");
}

// One code from two backends means two failures (2 is VK_TIMEOUT and
// CUDA_ERROR_OUT_OF_MEMORY); the backend tells them apart.
TEST(Status, TheBackendTellsOneCodeFromTwoBackendsApart) {
  const Status vulkan =
      Status::backend_error(Status::Backend::Vulkan, 2, "vkWaitForFences");
  const Status cuda =
      Status::backend_error(Status::Backend::Cuda, 2, "cuMemAlloc");
  EXPECT_EQ(vulkan.detail(), cuda.detail());
  EXPECT_EQ(vulkan.backend(), Status::Backend::Vulkan);
  EXPECT_EQ(cuda.backend(), Status::Backend::Cuda);
}

TEST(StatusDeathTest, BackendErrorRefusesTheSuccessCode) {
  // 0 is success in every backend (VK_SUCCESS, CUDA_SUCCESS, noErr): a
  // failure built from it is a caller that forgot to test the call's result.
  EXPECT_DEATH(
      (void)Status::backend_error(Status::Backend::Vulkan, 0, "vkQueueSubmit"),
      "backend_error needs a failing backend code");
  EXPECT_DEATH(
      (void)Status::backend_error(Status::Backend::Other, 0, "anything"),
      "backend_error needs a failing backend code");
}

TEST(Status, WithContextKeepsDomainBackendAndDetail) {
  const Status s =
      Status::backend_error(Status::Backend::NvJpeg, 7, "nvjpegDecode");
  const Status named = s.with_context("integrate");
  EXPECT_EQ(named.domain(), Status::Code::Backend);
  EXPECT_EQ(named.backend(), Status::Backend::NvJpeg);
  EXPECT_EQ(named.detail(), 7);
  EXPECT_EQ(named.message(), "integrate: nvjpegDecode");
  EXPECT_EQ(s.message(), "nvjpegDecode");  // the const& overload copies

  const Status moved = Status::numerical("singular").with_context("solve");
  EXPECT_EQ(moved.domain(), Status::Code::Numerical);
  EXPECT_EQ(moved.detail(), 0);
  EXPECT_EQ(moved.message(), "solve: singular");
}

TEST(Status, WithContextLeavesSuccessAlone) {
  const Status s = Status{}.with_context("anything");
  EXPECT_TRUE(s.ok());
  EXPECT_TRUE(s.message().empty());
}

TEST(Status, ToStringNamesEveryDomain) {
  EXPECT_EQ(to_string(Status::Code::Ok), "Ok");
  EXPECT_EQ(to_string(Status::Code::InvalidArgument), "InvalidArgument");
  EXPECT_EQ(to_string(Status::Code::NotFound), "NotFound");
  EXPECT_EQ(to_string(Status::Code::Unsupported), "Unsupported");
  EXPECT_EQ(to_string(Status::Code::OutOfMemory), "OutOfMemory");
  EXPECT_EQ(to_string(Status::Code::IoError), "IoError");
  EXPECT_EQ(to_string(Status::Code::Numerical), "Numerical");
  EXPECT_EQ(to_string(Status::Code::Backend), "Backend");
}

TEST(Status, ToStringNamesEveryBackend) {
  EXPECT_EQ(to_string(Status::Backend::Vulkan), "Vulkan");
  EXPECT_EQ(to_string(Status::Backend::Cuda), "Cuda");
  EXPECT_EQ(to_string(Status::Backend::NvJpeg), "NvJpeg");
  EXPECT_EQ(to_string(Status::Backend::Ffmpeg), "Ffmpeg");
  EXPECT_EQ(to_string(Status::Backend::VideoToolbox), "VideoToolbox");
  EXPECT_EQ(to_string(Status::Backend::Other), "Other");
}

// --- Result -----------------------------------------------------------------

TEST(Result, HoldsValueOnSuccess) {
  Result<int> r = 42;
  ASSERT_TRUE(r.ok());
  EXPECT_TRUE(r.status().ok());
  EXPECT_EQ(r.value(), 42);
  EXPECT_EQ(*r, 42);
}

TEST(Result, HoldsStatusOnFailure) {
  const Result<int> r = Status::numerical("did not converge");
  EXPECT_FALSE(r.ok());
  EXPECT_FALSE(static_cast<bool>(r));
  EXPECT_EQ(r.status().domain(), Status::Code::Numerical);
  EXPECT_EQ(r.status().message(), "did not converge");
}

Result<std::string> camera_name(bool known) {
  if (!known) return Status::not_found("no such camera");
  return "bm-1";  // a const char[5], converted to std::string
}

TEST(Result, ConvertsFromValuesConvertibleToT) {
  const Result<std::string> name = camera_name(true);
  ASSERT_TRUE(name.ok());
  EXPECT_EQ(*name, "bm-1");
  EXPECT_EQ(camera_name(false).status().domain(), Status::Code::NotFound);

  const Result<double> widened = 3;  // int -> double
  ASSERT_TRUE(widened.ok());
  EXPECT_EQ(*widened, 3.0);
}

Result<std::optional<int>> maybe(bool some) {
  if (some) return 7;
  return std::nullopt;  // "nothing", which is success -- not an error
}

TEST(Result, ReturnsNulloptForAnOptionalValue) {
  const Result<std::optional<int>> none = maybe(false);
  ASSERT_TRUE(none.ok());
  EXPECT_FALSE(none->has_value());
  const Result<std::optional<int>> some = maybe(true);
  ASSERT_TRUE(some.ok());
  ASSERT_TRUE(some->has_value());
  EXPECT_EQ(some->value_or(-1), 7);
}

// A pointer is not a bool success value, and a null pointer is not a string:
// both conversions compile, so the converting constructor refuses them.
static_assert(!std::is_convertible_v<const char (&)[15], Result<bool>>);
static_assert(!std::is_convertible_v<const char*, Result<bool>>);
static_assert(!std::is_convertible_v<std::nullptr_t, Result<std::string>>);
static_assert(
    !std::is_convertible_v<std::nullptr_t, Result<std::optional<std::string>>>);
// The intended conversions still work.
static_assert(std::is_convertible_v<bool, Result<bool>>);
static_assert(std::is_convertible_v<std::true_type, Result<bool>>);
static_assert(std::is_convertible_v<const char (&)[3], Result<std::string>>);
static_assert(std::is_convertible_v<std::nullptr_t, Result<int*>>);
static_assert(
    std::is_convertible_v<std::nullptr_t, Result<std::unique_ptr<int>>>);

// The rvalue accessors return the value itself, so a temporary Result never
// leaves a dangling reference behind.
static_assert(
    std::is_same_v<decltype(std::declval<Result<int>>().value()), int>);
static_assert(std::is_same_v<decltype(*std::declval<Result<int>>()), int>);
static_assert(
    std::is_same_v<decltype(std::declval<Result<int>>().status()), Status>);
static_assert(std::is_same_v<decltype(std::declval<Result<int>&>().status()),
                             const Status&>);

Result<std::vector<int>> load_list() { return std::vector<int>{1, 2, 3}; }

TEST(Result, RangeForOverATemporarysValueIsSafe) {
  // Before C++23 a range-for does not extend a temporary Result's lifetime,
  // so this would read a destroyed vector if value() && returned a reference.
  int sum = 0;
  for (const int v : load_list().value()) sum += v;
  EXPECT_EQ(sum, 6);
}

// A type whose unary operator& does not return its address, as some handle
// wrappers' does.
struct AddressOfOverloaded {
  int v = 4;
  const AddressOfOverloaded* operator&() const { return nullptr; }
};

TEST(Result, ArrowReachesATypeThatOverloadsAddressOf) {
  const Result<AddressOfOverloaded> r = AddressOfOverloaded{};
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r->v, 4);
}

TEST(Result, StatusOfAnOkResultIsOk) {
  const Result<int> r = 1;
  EXPECT_TRUE(r.status().ok());
  EXPECT_TRUE(Result<int>(1).status().ok());
}

TEST(Result, StatusMovesOutOfAnExpiringResult) {
  Result<int> r = Status::io_error("short read");
  const Status s = std::move(r).status();
  EXPECT_EQ(s.domain(), Status::Code::IoError);
  EXPECT_EQ(s.message(), "short read");
}

TEST(Result, ArrowAndConstAccessReachTheValue) {
  const Result<std::string> r = std::string("abc");
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r->size(), 3u);
  EXPECT_EQ(r.value(), "abc");
}

TEST(Result, HoldsAndMovesOutAMoveOnlyValue) {
  Result<std::unique_ptr<int>> r = std::make_unique<int>(5);
  ASSERT_TRUE(r.ok());
  const std::unique_ptr<int> taken = *std::move(r);
  ASSERT_NE(taken, nullptr);
  EXPECT_EQ(*taken, 5);
}

// --- VKC_TRY / VKC_ASSIGN ---------------------------------------------------

Status fail_if_negative(int x) {
  if (x < 0) return Status::invalid_argument("negative");
  return {};
}

Status run_try(int x) {
  VKC_TRY(fail_if_negative(x));
  return {};
}

TEST(Macros, TryPassesSuccessAndPropagatesFailure) {
  EXPECT_TRUE(run_try(3).ok());
  const Status s = run_try(-1);
  EXPECT_EQ(s.domain(), Status::Code::InvalidArgument);
  EXPECT_EQ(s.message(), "negative");
}

Result<int> doubled_if_nonneg(int x) {
  if (x < 0) return Status::invalid_argument("negative");
  return x * 2;
}

Result<int> run_assign(int x) {
  VKC_ASSIGN(const int doubled, doubled_if_nonneg(x));
  return doubled + 1;
}

TEST(Macros, AssignUnwrapsOrPropagates) {
  const Result<int> ok = run_assign(10);
  ASSERT_TRUE(ok.ok());
  EXPECT_EQ(*ok, 21);
  EXPECT_EQ(run_assign(-1).status().domain(), Status::Code::InvalidArgument);
}

Result<int> two_assigns_on_one_line() {
  // clang-format off
  VKC_ASSIGN(const int a, doubled_if_nonneg(1)); VKC_ASSIGN(const int b, doubled_if_nonneg(2));
  // clang-format on
  return a + b;
}

TEST(Macros, AssignsOnOneLineDoNotCollide) {
  const Result<int> r = two_assigns_on_one_line();
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(*r, 6);
}

Result<std::unique_ptr<int>> make_box(int v) {
  return std::make_unique<int>(v);
}

Status take_box() {
  VKC_ASSIGN(const std::unique_ptr<int> box, make_box(9));
  if (*box != 9) return Status::invalid_argument("wrong value");
  return {};
}

TEST(Macros, AssignMovesAMoveOnlyValue) { EXPECT_TRUE(take_box().ok()); }

// --- Misuse aborts -----------------------------------------------------------

// Reading an error Result, or building an error Result from success, is a
// programmer error: it aborts with a diagnostic instead of throwing.
TEST(ResultDeathTest, ValueOfAnErrorResultAborts) {
  Result<int> r = Status::numerical("singular");
  EXPECT_DEATH((void)r.value(), "Result::value\\(\\) on an error Result");
  EXPECT_DEATH((void)*r, "Result::operator\\* on an error Result");
  EXPECT_DEATH((void)r.operator->(), "Result::operator-> on an error Result");
  EXPECT_DEATH((void)std::move(r).value(),
               "Result::value\\(\\) on an error Result");
}

// The abort names the held error, and value() names its caller's line rather
// than the header's, so two misuses do not look alike in a crash report.
TEST(ResultDeathTest, MisuseNamesTheErrorAndTheCaller) {
  const Result<int> r =
      Status::backend_error(Status::Backend::Vulkan, -4, "vkQueueSubmit");
  EXPECT_DEATH((void)r.value(),
               "Result::value\\(\\) on an error Result \\(Backend Vulkan -4: "
               "vkQueueSubmit\\) \\[ok\\(\\)\\] at "
               ".*base_result_test.cpp:[0-9]+");
  // The backend says how to read the code: 2 here is no VK_TIMEOUT.
  const Result<int> cuda =
      Status::backend_error(Status::Backend::Cuda, 2, "cuMemAlloc");
  EXPECT_DEATH((void)*cuda,
               "Result::operator\\* on an error Result \\(Backend Cuda 2: "
               "cuMemAlloc\\)");
}

TEST(ResultDeathTest, FailureFromAnOkStatusAborts) {
  EXPECT_DEATH(
      {
        const Result<int> r{Status{}};
        (void)r;
      },
      "requires a non-OK status");
}

}  // namespace
}  // namespace volumetric_kit::core
