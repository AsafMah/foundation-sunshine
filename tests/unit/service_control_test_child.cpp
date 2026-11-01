// A fake Core: no service, session, display, device, or network operations.
#include "service_control_test_identity.h"
#include "tools/service_control.h"

int
main(int argc, char **argv) {
  namespace sc = sunshinesvc::service_control;
  const auto args = sc::parse_arguments(argc, argv);
  if (!args.present || !sc::validate(args, true).empty()) {
    return 1;
  }
  if (WaitForSingleObject(args.restart, 0) != WAIT_OBJECT_0 ||
      WaitForSingleObject(args.stop, 0) != WAIT_OBJECT_0) {
    return 2;
  }
  if (SetEvent(args.restart) || GetLastError() != ERROR_ACCESS_DENIED ||
      SetEvent(args.stop) || GetLastError() != ERROR_ACCESS_DENIED) {
    return 3;
  }
  std::string sentinel_name;
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument(argv[i]);
    constexpr std::string_view prefix = "--excluded-event-name=";
    if (argument.starts_with(prefix)) {
      sentinel_name = argument.substr(prefix.size());
    }
  }
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument(argv[i]);
    constexpr std::string_view prefix = "--excluded-handle=";
    if (argument.starts_with(prefix)) {
      const auto text = argument.substr(prefix.size());
      std::uintptr_t value = 0;
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
      if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size()) {
        return 4;
      }
      // Pin any existing candidate before opening the reference: OpenEvent
      // itself could otherwise reuse the previously empty numeric slot.
      HANDLE pinned = nullptr;
      if (!DuplicateHandle(GetCurrentProcess(), reinterpret_cast<HANDLE>(value),
            GetCurrentProcess(), &pinned, 0, FALSE, DUPLICATE_SAME_ACCESS) &&
          GetLastError() != ERROR_INVALID_HANDLE) {
        return 8;
      }
      sc::handle candidate(pinned);
      sc::handle reference(OpenEventA(SYNCHRONIZE | 1, FALSE, sentinel_name.c_str()));
      if (!reference.get()) {
        return 7;
      }
      return candidate.get() && excluded_object_is_present(candidate.get(), reference.get()) ? 5 : 0;
    }
  }
  return 6;
}
