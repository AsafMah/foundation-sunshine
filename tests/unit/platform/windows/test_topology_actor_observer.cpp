/**
 * @file tests/unit/platform/windows/test_topology_actor_observer.cpp
 * @brief Recovery observation of a recorded external actor by PID and creation time.
 *
 * Kernel branches use a fake API. Real-kernel checks only look at processes that
 * already exist (this test and the System process); nothing is launched.
 */
#include <gtest/gtest.h>

#include <vector>

#include "src/platform/windows/topology_actor_observer.h"

using display_device::topology::actor_receipt_t;
using display_device::topology::process_observation_e;

namespace {
  char opened_tag;
  const HANDLE opened = &opened_tag;

  struct kernel_t {
    HANDLE open_result { opened };
    DWORD open_error { ERROR_SUCCESS };
    bool times_succeed { true };
    std::uint64_t creation { 0x01DB'0000'1234'5678ULL };
    DWORD wait_result { WAIT_TIMEOUT };
    DWORD error { ERROR_SUCCESS };
    unsigned opens {}, times {}, waits {};
    DWORD access {}, pid {}, timeout { 12345 };
    BOOL inherit { TRUE };
    std::vector<HANDLE> timed, waited, closed;
  } kernel;

  HANDLE WINAPI fake_open(DWORD access, BOOL inherit, DWORD pid) {
    ++kernel.opens;
    kernel.access = access;
    kernel.inherit = inherit;
    kernel.pid = pid;
    kernel.error = kernel.open_error;
    return kernel.open_result;
  }

  BOOL WINAPI fake_times(HANDLE handle, LPFILETIME creation, LPFILETIME exit, LPFILETIME kernel_time, LPFILETIME user) {
    ++kernel.times;
    kernel.timed.push_back(handle);
    EXPECT_NE(exit, nullptr);
    EXPECT_NE(kernel_time, nullptr);
    EXPECT_NE(user, nullptr);
    // A failed call's outputs are unspecified; make ignoring the failure observable.
    creation->dwLowDateTime = static_cast<DWORD>(kernel.creation);
    creation->dwHighDateTime = static_cast<DWORD>(kernel.creation >> 32);
    if (!kernel.times_succeed) {
      kernel.error = ERROR_ACCESS_DENIED;
      return FALSE;
    }
    return TRUE;
  }

  DWORD WINAPI fake_wait(HANDLE handle, DWORD timeout) {
    ++kernel.waits;
    kernel.waited.push_back(handle);
    kernel.timeout = timeout;
    if (kernel.wait_result == WAIT_FAILED) kernel.error = ERROR_ACCESS_DENIED;
    return kernel.wait_result;
  }

  DWORD WINAPI fake_last_error() {
    return kernel.error;
  }

  BOOL WINAPI fake_close(HANDLE handle) {
    kernel.closed.push_back(handle);
    return TRUE;
  }

  const platf::process_query_api_t fake_api { fake_open, fake_times, fake_wait, fake_last_error, fake_close };

  constexpr actor_receipt_t recorded { 4242, 0x01DB'0000'1234'5678ULL };

  class ActorObserver: public ::testing::Test {
  protected:
    void
    SetUp() override {
      kernel = {};
    }

    void
    expect_single_handle_used_and_closed() {
      for (const auto handle : kernel.timed) EXPECT_EQ(handle, opened);
      for (const auto handle : kernel.waited) EXPECT_EQ(handle, opened);
      ASSERT_EQ(kernel.closed.size(), 1u);
      EXPECT_EQ(kernel.closed.front(), opened);
    }
  };

  std::uint64_t
  own_creation_time() {
    FILETIME creation {}, exit {}, kernel_time {}, user {};
    EXPECT_TRUE(GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel_time, &user));
    return (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
  }
}  // namespace

TEST_F(ActorObserver, ItOpensThePidOnceForQueryAndSynchronizeWithoutInheritance) {
  EXPECT_EQ(platf::observe_actor_process(recorded, fake_api), process_observation_e::running);
  EXPECT_EQ(kernel.opens, 1u);
  EXPECT_EQ(kernel.pid, recorded.pid);
  EXPECT_EQ(kernel.access, static_cast<DWORD>(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE));
  EXPECT_EQ(kernel.inherit, FALSE);
  EXPECT_EQ(kernel.times, 1u);
  EXPECT_EQ(kernel.waits, 1u);
  EXPECT_EQ(kernel.timeout, 0u);
  expect_single_handle_used_and_closed();
}

TEST_F(ActorObserver, OnlyAnInvalidParameterOpenFailureMeansAbsent) {
  for (const DWORD error : std::initializer_list<DWORD> { ERROR_INVALID_PARAMETER, ERROR_ACCESS_DENIED, ERROR_NOT_ENOUGH_MEMORY, ERROR_SUCCESS }) {
    SCOPED_TRACE(error);
    kernel = {};
    kernel.open_result = nullptr;
    kernel.open_error = error;
    EXPECT_EQ(platf::observe_actor_process(recorded, fake_api),
      error == ERROR_INVALID_PARAMETER ? process_observation_e::absent : process_observation_e::inaccessible);
    EXPECT_EQ(kernel.times, 0u);
    EXPECT_EQ(kernel.waits, 0u);
    EXPECT_TRUE(kernel.closed.empty());
  }
}

TEST_F(ActorObserver, AnUnreadableCreationTimeIsInaccessibleAndNeverObserved) {
  kernel.times_succeed = false;
  EXPECT_EQ(platf::observe_actor_process(recorded, fake_api), process_observation_e::inaccessible);
  EXPECT_EQ(kernel.waits, 0u);
  expect_single_handle_used_and_closed();
}

TEST_F(ActorObserver, AReusedPidIsAnIdentityMismatchBeforeAnyLivenessCheck) {
  for (const auto wait : std::initializer_list<DWORD> { WAIT_TIMEOUT, WAIT_OBJECT_0 }) {
    kernel = {};
    kernel.creation = recorded.creation_time + 1;
    kernel.wait_result = wait;
    EXPECT_EQ(platf::observe_actor_process(recorded, fake_api), process_observation_e::identity_mismatch);
    EXPECT_EQ(kernel.waits, 0u);
    expect_single_handle_used_and_closed();
  }
}

TEST_F(ActorObserver, TheSameHandleDecidesRunningOrExitedAndAnyOtherWaitIsInaccessible) {
  const std::pair<DWORD, process_observation_e> cases[] {
    { WAIT_TIMEOUT, process_observation_e::running },
    { WAIT_OBJECT_0, process_observation_e::exited },
    { WAIT_FAILED, process_observation_e::inaccessible },
    { WAIT_ABANDONED, process_observation_e::inaccessible },
  };
  for (const auto &[wait, expected] : cases) {
    SCOPED_TRACE(wait);
    kernel = {};
    kernel.wait_result = wait;
    EXPECT_EQ(platf::observe_actor_process(recorded, fake_api), expected);
    expect_single_handle_used_and_closed();
  }
}

TEST_F(ActorObserver, AnIncompleteReceiptIsInaccessibleWithoutOpeningAnything) {
  for (const auto receipt : { actor_receipt_t { 0, recorded.creation_time }, actor_receipt_t { recorded.pid, 0 } }) {
    kernel = {};
    EXPECT_EQ(platf::observe_actor_process(receipt, fake_api), process_observation_e::inaccessible);
    EXPECT_EQ(kernel.opens, 0u);
  }
}

TEST(ActorObserverSystem, ThisRunningProcessIsRecognizedByItsOwnCreationTime) {
  const actor_receipt_t self { GetCurrentProcessId(), own_creation_time() };
  EXPECT_EQ(platf::observe_actor_process(self), process_observation_e::running);
}

TEST(ActorObserverSystem, ThisProcessWithADifferentCreationTimeIsAnotherProcess) {
  const actor_receipt_t reused { GetCurrentProcessId(), own_creation_time() - 1 };
  EXPECT_EQ(platf::observe_actor_process(reused), process_observation_e::identity_mismatch);
}

TEST(ActorObserverSystem, ANonexistentPidIsAbsent) {
  // Windows PIDs are multiples of four; this one is never assigned.
  const actor_receipt_t gone { 0xFFFF'FFFEu, 1 };
  EXPECT_EQ(platf::observe_actor_process(gone), process_observation_e::absent);
}
