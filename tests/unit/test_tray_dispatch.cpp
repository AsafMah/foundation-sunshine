/**
 * @file tests/unit/test_tray_dispatch.cpp
 * @brief Tray callbacks must not block the tray thread on the topology coordinator.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <future>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "src/display_device/topology_coordinator.h"
#include "src/tray/tray_dispatch.h"

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#endif

namespace {
  using namespace std::chrono_literals;
  using namespace display_device::topology;

#ifdef _WIN32
  constexpr UINT menu_click_message = WM_APP + 1;
  /** Stands in for tray_update()'s synchronous WM_INITMENUPOPUP to the tray window. */
  constexpr UINT tray_update_message = WM_APP + 2;

  struct tray_window_state_t {
    coordinator_t *coordinator {};
    std::promise<command_result_t> finished;
    std::atomic<DWORD> tray_thread_id {};
    std::atomic<DWORD> caller_thread_id {};
    std::atomic_bool tray_update_delivered {};
    std::atomic_int tray_updates_handled {};
  };

  LRESULT CALLBACK
  tray_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto *state = reinterpret_cast<tray_window_state_t *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
      case menu_click_message:
        // The production tray callbacks dispatch exactly like this.
        system_tray::run_off_tray_thread("Test tray action", [state, window] {
          state->caller_thread_id = GetCurrentThreadId();
          const auto result = state->coordinator->run(command_kind_e::vdd_toggle, [state, window](const mutation_capability_t &, const command_context_t &) {
            DWORD_PTR reply {};
            // A real worker body calls update_vdd_menu() -> tray_update() -> SendMessage.
            // The timeout turns the old deadlock into a failure instead of a hung test.
            state->tray_update_delivered = SendMessageTimeoutW(window, tray_update_message, 0, 0, SMTO_NORMAL, 2000, &reply) != 0;
          });
          state->finished.set_value(result);
        });
        return 0;
      case tray_update_message:
        ++state->tray_updates_handled;
        return 0;
      case WM_CLOSE:
        DestroyWindow(window);
        return 0;
      case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
      default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
  }

  TEST(TrayDispatch, ACoordinatorCallFromATrayCallbackLetsTheWorkerMessageTheTrayWindow) {
    coordinator_options_t options;
    options.clock = [] {
      return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch());
    };
    options.poll_interval = 1ms;
    coordinator_t coordinator { options };
    ASSERT_TRUE(coordinator.start({}).get().completed());

    tray_window_state_t state;
    state.coordinator = &coordinator;
    auto finished = state.finished.get_future();

    std::promise<HWND> created;
    auto window_future = created.get_future();
    std::thread tray_thread([&state, &created] {
      state.tray_thread_id = GetCurrentThreadId();
      WNDCLASSW window_class {};
      window_class.lpfnWndProc = tray_window_proc;
      window_class.hInstance = GetModuleHandleW(nullptr);
      window_class.lpszClassName = L"SunshineTrayDispatchTest";
      RegisterClassW(&window_class);
      HWND window = CreateWindowExW(0, window_class.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, window_class.hInstance, nullptr);
      if (window) {
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&state));
      }
      created.set_value(window);
      if (!window) {
        return;
      }
      MSG message;
      while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
      }
      UnregisterClassW(window_class.lpszClassName, window_class.hInstance);
    });

    HWND window = window_future.get();
    ASSERT_NE(window, nullptr);
    ASSERT_TRUE(PostMessageW(window, menu_click_message, 0, 0));

    const auto status = finished.wait_for(10s);
    PostMessageW(window, WM_CLOSE, 0, 0);
    tray_thread.join();

    ASSERT_EQ(status, std::future_status::ready) << "the tray action never finished";
    EXPECT_TRUE(finished.get().completed());
    EXPECT_TRUE(state.tray_update_delivered) << "the worker could not message the tray window: the tray thread was blocked on the coordinator";
    EXPECT_EQ(state.tray_updates_handled.load(), 1);
    EXPECT_NE(state.caller_thread_id.load(), state.tray_thread_id.load()) << "the coordinator call ran on the tray thread";
  }
#endif

  /** Replaces comments, string and character literals with spaces, keeping offsets and newlines. */
  std::string
  code_only(const std::string &source) {
    std::string out = source;
    enum class mode_e { code, line_comment, block_comment, string, character } mode = mode_e::code;
    for (std::size_t i = 0; i < source.size(); ++i) {
      const char c = source[i];
      const char next = i + 1 < source.size() ? source[i + 1] : '\0';
      const auto blank = [&](std::size_t at) {
        if (out[at] != '\n') {
          out[at] = ' ';
        }
      };
      switch (mode) {
        case mode_e::code:
          if (c == '/' && next == '/') {
            mode = mode_e::line_comment;
            blank(i);
          }
          else if (c == '/' && next == '*') {
            mode = mode_e::block_comment;
            blank(i);
            blank(++i);
          }
          else if (c == '"') {
            mode = mode_e::string;
          }
          else if (c == '\'') {
            mode = mode_e::character;
          }
          break;
        case mode_e::line_comment:
          if (c == '\n') {
            mode = mode_e::code;
          }
          else {
            blank(i);
          }
          break;
        case mode_e::block_comment:
          blank(i);
          if (c == '*' && next == '/') {
            blank(++i);
            mode = mode_e::code;
          }
          break;
        case mode_e::string:
        case mode_e::character:
          if (c == '\\') {
            blank(i);
            blank(++i);
          }
          else if ((mode == mode_e::string && c == '"') || (mode == mode_e::character && c == '\'')) {
            mode = mode_e::code;
          }
          else {
            blank(i);
          }
          break;
      }
    }
    return out;
  }

  /** Index of the bracket closing the one at `open`, or npos. */
  std::size_t
  matching(const std::string &code, std::size_t open, char open_char, char close_char) {
    int depth = 0;
    for (std::size_t i = open; i < code.size(); ++i) {
      if (code[i] == open_char) {
        ++depth;
      }
      else if (code[i] == close_char && --depth == 0) {
        return i;
      }
    }
    return std::string::npos;
  }

  std::size_t
  line_of(const std::string &code, std::size_t offset) {
    return 1 + static_cast<std::size_t>(std::count(code.begin(), code.begin() + static_cast<std::ptrdiff_t>(offset), '\n'));
  }

  std::string
  read_source(const char *relative) {
    std::ifstream file(std::filesystem::path(SUNSHINE_SOURCE_DIR) / relative, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
  }

  TEST(TrayDispatch, EveryTrayCoordinatorCallIsDispatchedOffTheTrayThread) {
    const auto source = read_source("src/tray/system_tray.cpp");
    ASSERT_FALSE(source.empty());
    const auto code = code_only(source);

    EXPECT_NE(source.find("#include \"tray_dispatch.h\""), std::string::npos) << "system_tray.cpp must use the tested dispatcher";
    EXPECT_FALSE(std::regex_search(code, std::regex(R"(\brun_off_tray_thread\s*\([^()]*&&)"))) << "system_tray.cpp must not define its own dispatcher";

    // Every argument list of run_off_tray_thread(...) is an allowed region; a bare
    // function name passed as the action makes that function's body allowed too.
    std::vector<std::pair<std::size_t, std::size_t>> allowed;
    std::vector<std::string> helpers;
    const std::regex call(R"(\brun_off_tray_thread\s*\()");
    for (std::sregex_iterator it(code.begin(), code.end(), call), end; it != end; ++it) {
      const auto open = static_cast<std::size_t>(it->position() + it->length() - 1);
      const auto close = matching(code, open, '(', ')');
      ASSERT_NE(close, std::string::npos);
      allowed.emplace_back(open, close);
      std::smatch helper;
      const auto args = code.substr(open + 1, close - open - 1);
      if (std::regex_search(args, helper, std::regex(R"(,\s*([A-Za-z_]\w*)\s*$)"))) {
        helpers.push_back(helper[1]);
      }
    }
    ASSERT_GE(allowed.size(), 3u) << "create, close and reset must all be dispatched";

    for (const auto &name : helpers) {
      const std::regex definition("\\b" + name + R"(\s*\(\s*\)\s*\{)");
      std::smatch match;
      ASSERT_TRUE(std::regex_search(code, match, definition)) << name << " is not defined in system_tray.cpp";
      const auto body_open = static_cast<std::size_t>(match.position() + match.length() - 1);
      allowed.emplace_back(body_open, matching(code, body_open, '{', '}'));

      // The helper may only ever run through the dispatcher.
      const std::regex use("\\b" + name + "\\b");
      for (std::sregex_iterator it(code.begin(), code.end(), use), end; it != end; ++it) {
        const auto at = static_cast<std::size_t>(it->position());
        const bool is_definition = at == static_cast<std::size_t>(match.position());
        const bool dispatched = std::any_of(allowed.begin(), allowed.end(), [&](const auto &span) {
          return at > span.first && at < span.second && span.first != body_open;
        });
        EXPECT_TRUE(is_definition || dispatched) << name << " is used outside the dispatcher at line " << line_of(code, at);
      }
    }

    std::size_t calls = 0;
    const std::regex coordinated(R"(\bcoordinated\s*::)");
    for (std::sregex_iterator it(code.begin(), code.end(), coordinated), end; it != end; ++it) {
      ++calls;
      const auto at = static_cast<std::size_t>(it->position());
      const bool dispatched = std::any_of(allowed.begin(), allowed.end(), [&](const auto &span) {
        return at > span.first && at < span.second;
      });
      EXPECT_TRUE(dispatched) << "coordinator call on the tray thread at system_tray.cpp:" << line_of(code, at);
    }
    EXPECT_GE(calls, 3u) << "the scan found too few coordinator calls to be meaningful";
  }
}  // namespace
