/**
 * @file tests/unit/test_lifetime.cpp
 * @brief Real process-termination behavior in isolated test children, not Sunshine.
 */
#include <cstdio>
#include <cstdlib>
#include <gtest/gtest.h>

#ifdef _WIN32
  #include <Windows.h>
#endif

#include "src/entry_handler.h"

#ifdef _WIN32
namespace lifetime::testing {
  // Only test_sunshine defines this native-call seam; production calls Win32 directly.
  extern decltype(&TerminateProcess) terminate_process;
}  // namespace lifetime::testing
#endif

namespace {
  TEST(LifetimeDeathTest, LegacyTerminationPreservesTheSelectedExitCode) {
    for (const int code : { 0, 53 }) {
      EXPECT_EXIT(
        {
          lifetime::desired_exit_code = code;
          lifetime::force_terminate();
          std::_Exit(90);
        },
        ::testing::ExitedWithCode(code), "");
    }
  }

  TEST(LifetimeDeathTest, ExplicitTerminationIgnoresTheGracefulExitCode) {
    for (const int graceful : { 0, 53 }) {
      EXPECT_EXIT(
        {
          lifetime::desired_exit_code = graceful;
          lifetime::force_terminate(37);
          std::_Exit(90);
        },
        ::testing::ExitedWithCode(37), "");
    }
  }

#ifdef _WIN32
  BOOL WINAPI
  reject_native_termination(HANDLE process, UINT code) {
    if (process != GetCurrentProcess() || code != 37) std::_Exit(91);
    std::fputs("native-termination-attempt\n", stderr);
    std::fflush(stderr);
    return FALSE;
  }

  TEST(LifetimeDeathTest, NativeFailureStillTerminatesWithTheSelectedCode) {
    for (const bool explicit_code : { false, true }) {
      EXPECT_EXIT(
        {
          lifetime::desired_exit_code = explicit_code ? 0 : 37;
          lifetime::testing::terminate_process = reject_native_termination;
          if (explicit_code)
            lifetime::force_terminate(37);
          else
            lifetime::force_terminate();
          std::_Exit(90);
        },
        ::testing::ExitedWithCode(37), "native-termination-attempt");
    }
  }
#endif

  struct unwind_sentinel_t {
    ~unwind_sentinel_t() {
      std::_Exit(88);
    }
  };

  TEST(LifetimeDeathTest, TerminationBypassesCleanupAndUnwinding) {
    EXPECT_EXIT(
      {
        lifetime::desired_exit_code = 37;
#ifdef _WIN32
        lifetime::testing::terminate_process = reject_native_termination;
#endif
        if (std::atexit([]() { std::_Exit(89); }) != 0) std::_Exit(92);
        try {
          unwind_sentinel_t sentinel;
          lifetime::force_terminate();
        }
        catch (...) {
          std::_Exit(93);
        }
        std::_Exit(90);
      },
      ::testing::ExitedWithCode(37), "");
  }
}  // namespace
