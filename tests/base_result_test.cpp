// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tao Jin

#include "volumetric_kit/core/base/result.hpp"

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <gtest/gtest.h>

namespace volumetric_kit::core {
namespace {

// --- Status -----------------------------------------------------------------

TEST(Status, DefaultIsOk) {
  const Status s;
  EXPECT_TRUE(s.ok());
  EXPECT_TRUE(static_cast<bool>(s));
  EXPECT_EQ(s.domain(), Status::Code::Ok);
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
    EXPECT_EQ(c.status.detail(), 0) << to_string(c.domain);
    EXPECT_EQ(c.status.message(), "m");
  }
}

TEST(Status, BackendErrorCarriesItsDetail) {
  // -4 is VK_ERROR_DEVICE_LOST; the base tier only carries the number.
  const Status s = Status::backend_error(-4, "vkQueueSubmit");
  EXPECT_FALSE(s.ok());
  EXPECT_EQ(s.domain(), Status::Code::Backend);
  EXPECT_EQ(s.detail(), -4);
  EXPECT_EQ(s.message(), "vkQueueSubmit");
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
  EXPECT_DEATH((void)*r, "contract check failed");
  EXPECT_DEATH((void)r.operator->(), "Result::operator-> on an error Result");
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
