#include "service_control_test_identity.h"
#include "src/streaming/restart_intent.h"
#include "tools/service_control.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <future>

namespace sc = sunshinesvc::service_control;
namespace ri = streaming::restart_intent;

TEST(ServiceControl, MigrationExitUsesExistingBoundsAndDoesNotForceAnOrderlyExit) {
  std::vector<DWORD> waits;
  int forced = 0;
  EXPECT_EQ(sc::finish_migration([] { return true; },
              [&](DWORD wait) { waits.push_back(wait); return true; },
              [&] { ++forced; return true; }),
    sc::migration_exit::orderly);
  EXPECT_EQ(waits, (std::vector<DWORD> { 20000 }));
  EXPECT_EQ(forced, 0);
}

TEST(ServiceControl, MigrationFallbackHandlesLostSignalHungCoreAndFailedKill) {
  std::vector<DWORD> waits;
  EXPECT_EQ(sc::finish_migration([] { return true; },
              [&](DWORD wait) { waits.push_back(wait); return wait == 5000; },
              [] { return true; }),
    sc::migration_exit::forced);
  EXPECT_EQ(waits, (std::vector<DWORD> { 20000, 5000 }));
  waits.clear();
  EXPECT_EQ(sc::finish_migration([] { return false; },
              [&](DWORD wait) { waits.push_back(wait); return false; },
              [] { return true; }),
    sc::migration_exit::incomplete);
  EXPECT_EQ(waits, (std::vector<DWORD> { 5000 }));
  EXPECT_EQ(sc::finish_migration([] { return false; }, [](DWORD) { return true; },
              [] { return false; }),
    sc::migration_exit::force_failed);
}

static sc::arguments
parse(std::vector<std::string> &values) {
  std::vector<char *> argv;
  for (auto &value : values) {
    argv.push_back(value.data());
  }
  return sc::parse_arguments(static_cast<int>(argv.size()), argv.data());
}

TEST(ServiceControl, FiltersPrivateArgumentsWithoutChangingConfigOrCommandTokens) {
  std::vector<std::string> values { "Sunshine.exe", "--service-control-version=1",
    "--service-restart-event=12", "config.conf", "--service-stop-event=16", "-p", "--creds", "user", "password" };
  const auto args = parse(values);
  EXPECT_TRUE(args.present);
  EXPECT_TRUE(args.error.empty());
  ASSERT_EQ(args.filtered.size(), 7u);
  EXPECT_STREQ(args.filtered[1], "config.conf");
  EXPECT_STREQ(args.filtered[2], "-p");
  EXPECT_STREQ(args.filtered[3], "--creds");
  EXPECT_STREQ(args.filtered[4], "user");
  EXPECT_STREQ(args.filtered[5], "password");
  EXPECT_EQ(args.filtered.back(), nullptr);
}

TEST(ServiceControl, RejectsInvalidDuplicateIncompleteAndUnknownPrivateArgs) {
  for (auto values : std::vector<std::vector<std::string>> {
         { "core", "--service-control-version=2", "--service-restart-event=12", "--service-stop-event=16" },
         { "core", "--service-control-version=1" },
         { "core", "--service-control-version=1", "--service-restart-event=12", "--service-stop-event=12" },
         { "core", "--service-control-version=1", "--service-restart-event=12x", "--service-stop-event=16" },
         { "core", "--service-control-version=1", "--service-restart-event=0", "--service-stop-event=16" },
         { "core", "--service-control-version=1", "--service-restart-event=12", "--service-stop-event=16", "--service-stop-event=20" },
         { "core", "--service-unknown=1" } }) {
    EXPECT_FALSE(parse(values).error.empty());
  }
}

TEST(ServiceControl, PortableStartupHasNoBridge) {
  std::vector<std::string> values { "core", "--version" };
  const auto args = parse(values);
  EXPECT_FALSE(args.present);
  EXPECT_TRUE(sc::validate(args, false).empty());
  EXPECT_STREQ(args.filtered[1], "--version");
}

TEST(ServiceControl, ValidatesEventTypeResetModeAndIdentity) {
  sc::handle manual(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  sc::handle automatic(CreateEventW(nullptr, FALSE, FALSE, nullptr));
  sc::handle mutex(CreateMutexW(nullptr, FALSE, nullptr));
  sc::arguments args;
  args.present = true;
  args.restart = manual.get();
  args.stop = automatic.get();
  EXPECT_FALSE(sc::validate(args, true).empty());
  args.stop = mutex.get();
  EXPECT_FALSE(sc::validate(args, true).empty());
  sc::handle stop(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  args.stop = stop.get();
  EXPECT_TRUE(sc::validate(args, true).empty());
  EXPECT_FALSE(sc::validate(args, false).empty());
}

TEST(ServiceControl, ChildHandlesAreInheritableQueryOnlyAndGenerationScoped) {
  sc::handle stop(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  sc::generation_events generation;
  ASSERT_EQ(generation.create(stop.get()), ERROR_SUCCESS);
  DWORD flags = 0;
  ASSERT_TRUE(GetHandleInformation(generation.child_restart(), &flags));
  EXPECT_NE(flags & HANDLE_FLAG_INHERIT, 0u);
  EXPECT_TRUE(sc::is_manual_reset_event(generation.child_restart()));
  EXPECT_FALSE(SetEvent(generation.child_restart()));
  EXPECT_EQ(GetLastError(), ERROR_ACCESS_DENIED);
  EXPECT_FALSE(SetEvent(generation.child_stop()));
  EXPECT_EQ(GetLastError(), ERROR_ACCESS_DENIED);
  ASSERT_TRUE(SetEvent(generation.restart()));
  EXPECT_EQ(WaitForSingleObject(generation.child_restart(), 0), WAIT_OBJECT_0);
  EXPECT_EQ(WaitForSingleObject(generation.child_stop(), 0), WAIT_TIMEOUT);
}

TEST(ServiceControl, RejectsQueryOnlyHandlesThatCannotBeWaitedOn) {
  sc::handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  HANDLE query = nullptr;
  ASSERT_TRUE(DuplicateHandle(GetCurrentProcess(), event.get(), GetCurrentProcess(),
    &query, 1, FALSE, 0));
  sc::handle query_handle(query);
  sc::arguments args;
  args.present = true;
  args.restart = query;
  args.stop = event.get();
  EXPECT_FALSE(sc::validate(args, true).empty());
}

TEST(ServiceControl, ListenerPublishesRestartThenTerminalFromFakeServiceEvents) {
  sc::handle restart(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  sc::handle stop(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  sc::arguments args;
  args.present = true;
  ASSERT_TRUE(DuplicateHandle(GetCurrentProcess(), restart.get(), GetCurrentProcess(),
    &args.restart, SYNCHRONIZE | 1, FALSE, 0));
  ASSERT_TRUE(DuplicateHandle(GetCurrentProcess(), stop.get(), GetCurrentProcess(),
    &args.stop, SYNCHRONIZE | 1, FALSE, 0));
  ri::shutdown_state state;
  std::promise<void> restarted, stopped;
  sc::listener listener;
  ASSERT_EQ(listener.start(args, [&] { state.request(ri::shutdown_state::intent::migration, [&] { restarted.set_value(); }); }, [&] { state.request(ri::shutdown_state::intent::terminal, [&] { stopped.set_value(); }); }, [](DWORD error) { ADD_FAILURE() << error; }), ERROR_SUCCESS);
  ASSERT_TRUE(SetEvent(restart.get()));
  ASSERT_EQ(restarted.get_future().wait_for(std::chrono::seconds(2)), std::future_status::ready);
  EXPECT_EQ(state.current(), ri::shutdown_state::intent::migration);
  ASSERT_TRUE(SetEvent(stop.get()));
  ASSERT_EQ(stopped.get_future().wait_for(std::chrono::seconds(2)), std::future_status::ready);
  EXPECT_EQ(state.current(), ri::shutdown_state::intent::terminal);
}

TEST(ServiceControl, AlreadySignaledStopWinsOverRestart) {
  sc::arguments args;
  args.present = true;
  args.restart = CreateEventW(nullptr, TRUE, TRUE, nullptr);
  args.stop = CreateEventW(nullptr, TRUE, TRUE, nullptr);
  std::promise<void> stopped;
  sc::listener listener;
  ASSERT_EQ(listener.start(args, [] { ADD_FAILURE() << "Restart must not win"; }, [&] { stopped.set_value(); }, [](DWORD error) { ADD_FAILURE() << error; }), ERROR_SUCCESS);
  EXPECT_EQ(stopped.get_future().wait_for(std::chrono::seconds(2)), std::future_status::ready);
}

TEST(ServiceControl, RealFakeCoreInheritsOnlyTheWhitelistedGenerationEvents) {
  for (const bool deliberately_leak : { false, true }) {
    SCOPED_TRACE(deliberately_leak ? "Deliberately leaked sentinel" : "Excluded sentinel");
    sc::handle stop(CreateEventW(nullptr, TRUE, TRUE, nullptr));
    sc::generation_events generation;
    ASSERT_EQ(generation.create(stop.get()), ERROR_SUCCESS);
    ASSERT_TRUE(SetEvent(generation.restart()));
    SECURITY_ATTRIBUTES security { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
    const auto sentinel_name = excluded_event_name();
    sc::handle excluded(CreateEventA(&security, TRUE, FALSE, sentinel_name.c_str()));
    ASSERT_NE(excluded.get(), nullptr);
    wchar_t module[MAX_PATH] {};
    ASSERT_NE(GetModuleFileNameW(nullptr, module, MAX_PATH), 0u);
    const auto child = std::filesystem::path(module).parent_path() / L"service_control_test_child.exe";
    auto command = L"\"" + child.wstring() + L"\"" + generation.command().substr(std::wstring_view(L"Sunshine.exe").size()) +
                   L" --excluded-handle=" + std::to_wstring(reinterpret_cast<std::uintptr_t>(excluded.get())) +
                   L" --excluded-event-name=" + std::wstring(sentinel_name.begin(), sentinel_name.end());
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<std::byte> storage(size);
    STARTUPINFOEXW startup {};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    ASSERT_TRUE(InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &size));
    const HANDLE whitelist[] = { generation.child_restart(), generation.child_stop(), excluded.get() };
    ASSERT_EQ(sc::set_handle_list(startup, std::span(whitelist).first(deliberately_leak ? 3 : 2)), ERROR_SUCCESS);
    PROCESS_INFORMATION process {};
    const bool created = CreateProcessW(child.c_str(), command.data(), nullptr, nullptr, TRUE,
      EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, nullptr,
      &startup.StartupInfo, &process);
    const auto create_error = GetLastError();
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    ASSERT_TRUE(created) << create_error;
    sc::handle thread(process.hThread), child_process(process.hProcess);
    const auto waited = WaitForSingleObject(child_process.get(), 5000);
    if (waited != WAIT_OBJECT_0) {
      // Only the fake child created by this test may be terminated.
      TerminateProcess(child_process.get(), 100);
      WaitForSingleObject(child_process.get(), 1000);
    }

    ASSERT_EQ(waited, WAIT_OBJECT_0);
    DWORD exit_code = 0;
    ASSERT_TRUE(GetExitCodeProcess(child_process.get(), &exit_code));
    EXPECT_EQ(exit_code, deliberately_leak ? 5u : 0u);
  }
}

TEST(ServiceControl, ThreeCoreGenerationsRefreshBothProcessAttributes) {
  sc::process_attributes attributes;
  for (const bool reuse_helper : { true, false }) {
    SCOPED_TRACE(reuse_helper ? "Reuse setup helper" : "Fresh per-generation helper");
    for (int number = 1; number <= 3; ++number) {
      sc::process_attributes fresh_attributes;
      SCOPED_TRACE("Core generation " + std::to_string(number));
      sc::handle stop(CreateEventW(nullptr, TRUE, TRUE, nullptr));
      sc::generation_events generation;
      ASSERT_EQ(generation.create(stop.get()), ERROR_SUCCESS);
      ASSERT_TRUE(SetEvent(generation.restart()));
      sc::handle job(CreateJobObjectW(nullptr, nullptr));
      ASSERT_NE(job.get(), nullptr);
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
      limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
      ASSERT_TRUE(SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)));
      SECURITY_ATTRIBUTES security { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
      const auto sentinel_name = excluded_event_name();
      sc::handle excluded(CreateEventA(&security, TRUE, FALSE, sentinel_name.c_str()));
      ASSERT_NE(excluded.get(), nullptr);
      wchar_t module[MAX_PATH] {};
      ASSERT_NE(GetModuleFileNameW(nullptr, module, MAX_PATH), 0u);
      const auto child = std::filesystem::path(module).parent_path() / L"service_control_test_child.exe";
      auto command = L"\"" + child.wstring() + L"\"" + generation.command().substr(std::wstring_view(L"Sunshine.exe").size()) +
                     L" --excluded-handle=" + std::to_wstring(reinterpret_cast<std::uintptr_t>(excluded.get())) +
                     L" --excluded-event-name=" + std::wstring(sentinel_name.begin(), sentinel_name.end());
      STARTUPINFOEXW startup {};
      startup.StartupInfo.cb = sizeof(startup);
      const HANDLE whitelist[] = { generation.child_restart(), generation.child_stop() };
      const auto error = (reuse_helper ? attributes : fresh_attributes).prepare(startup, whitelist, job.get());
      ASSERT_EQ(error, ERROR_SUCCESS) << "process attribute setup failed with Win32 error " << error;
      PROCESS_INFORMATION process {};
      ASSERT_TRUE(CreateProcessW(child.c_str(), command.data(), nullptr, nullptr, TRUE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | CREATE_SUSPENDED,
        nullptr, nullptr, &startup.StartupInfo, &process))
        << GetLastError();
      sc::handle thread(process.hThread), child_process(process.hProcess);
      BOOL in_job = FALSE;
      EXPECT_TRUE(IsProcessInJob(child_process.get(), job.get(), &in_job));
      EXPECT_TRUE(in_job);
      ASSERT_NE(ResumeThread(thread.get()), static_cast<DWORD>(-1));
      const auto waited = WaitForSingleObject(child_process.get(), 5000);
      if (waited != WAIT_OBJECT_0) {
        TerminateProcess(child_process.get(), 100);
        WaitForSingleObject(child_process.get(), 1000);
      }
      ASSERT_EQ(waited, WAIT_OBJECT_0);
      DWORD exit_code = 0;
      ASSERT_TRUE(GetExitCodeProcess(child_process.get(), &exit_code));
      EXPECT_EQ(exit_code, 0u);
    }
  }
}
TEST(ServiceControl, ExcludedObjectCheckDistinguishesValidUnrelatedHandles) {
  sc::handle excluded(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  sc::handle unrelated(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  ASSERT_NE(excluded.get(), nullptr);
  ASSERT_NE(unrelated.get(), nullptr);
  EXPECT_FALSE(excluded_object_is_present(unrelated.get(), excluded.get()));
  EXPECT_TRUE(excluded_object_is_present(excluded.get(), excluded.get()));
  HANDLE alias = nullptr;
  ASSERT_TRUE(DuplicateHandle(GetCurrentProcess(), excluded.get(), GetCurrentProcess(), &alias, 0, FALSE, DUPLICATE_SAME_ACCESS));
  sc::handle duplicate(alias);
  EXPECT_TRUE(excluded_object_is_present(duplicate.get(), excluded.get()));
}
