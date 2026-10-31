// A fake Core: no service, session, display, device, or network operations.
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
      DWORD flags = 0;
      return GetHandleInformation(reinterpret_cast<HANDLE>(value), &flags) ||
                 GetLastError() != ERROR_INVALID_HANDLE ?
               5 :
               0;
    }
  }
  return 6;
}
